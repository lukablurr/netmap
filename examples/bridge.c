/*
 * (C) 2011-2014 Luigi Rizzo, Matteo Landi
 *
 * BSD license
 *
 * A netmap client to bridge two network interfaces
 * (or one interface and the host stack).
 *
 * $FreeBSD: head/tools/tools/netmap/bridge.c 228975 2011-12-30 00:04:11Z uqs $
 */

#include <stdio.h>
#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>
#include <sys/poll.h>
#include <pcap.h>

int verbose = 0;

static int do_abort = 0;
static int zerocopy = 1; /* enable zerocopy if possible */

static void
sigint_h(int sig)
{
	(void)sig;	/* UNUSED */
	do_abort = 1;
	signal(SIGINT, SIG_DFL);
}


/*
 * how many packets on this set of queues ?
 */
int
pkt_queued(struct nm_desc *d, int tx)
{
        u_int i, tot = 0;

        if (tx) {
                for (i = d->first_tx_ring; i <= d->last_tx_ring; i++) {
                        tot += nm_ring_space(NETMAP_TXRING(d->nifp, i));
                }
        } else {
                for (i = d->first_rx_ring; i <= d->last_rx_ring; i++) {
                        tot += nm_ring_space(NETMAP_RXRING(d->nifp, i));
                }
        }
        return tot;
}

/*
 * move up to 'limit' pkts from rxring to txring swapping buffers.
 */
static int
process_rings(struct netmap_ring *rxring, struct netmap_ring *txring,
	      u_int limit, const char *msg)
{
	u_int j, k, m = 0;

	/* print a warning if any of the ring flags is set (e.g. NM_REINIT) */
	if (rxring->flags || txring->flags)
		D("%s rxflags %x txflags %x",
			msg, rxring->flags, txring->flags);
	j = rxring->cur; /* RX */
	k = txring->cur; /* TX */
	m = nm_ring_space(rxring);
	if (m < limit)
		limit = m;
	m = nm_ring_space(txring);
	if (m < limit)
		limit = m;
	m = limit;
	while (limit-- > 0) {
		struct netmap_slot *rs = &rxring->slot[j];
		struct netmap_slot *ts = &txring->slot[k];

		/* swap packets */
		if (ts->buf_idx < 2 || rs->buf_idx < 2) {
			D("wrong index rx[%d] = %d  -> tx[%d] = %d",
				j, rs->buf_idx, k, ts->buf_idx);
			sleep(2);
		}
		/* copy the packet length. */
		if (rs->len > 2048) {
			D("wrong len %d rx[%d] -> tx[%d]", rs->len, j, k);
			rs->len = 0;
		} else if (verbose > 1) {
			D("%s send len %d rx[%d] -> tx[%d]", msg, rs->len, j, k);
		}
		ts->len = rs->len;
		if (zerocopy) {
			uint32_t pkt = ts->buf_idx;
			ts->buf_idx = rs->buf_idx;
			rs->buf_idx = pkt;
			/* report the buffer change. */
			ts->flags |= NS_BUF_CHANGED;
			rs->flags |= NS_BUF_CHANGED;
		} else {
			char *rxbuf = NETMAP_BUF(rxring, rs->buf_idx);
			char *txbuf = NETMAP_BUF(txring, ts->buf_idx);
			nm_pkt_copy(rxbuf, txbuf, ts->len);
		}
		j = nm_ring_next(rxring, j);
		k = nm_ring_next(txring, k);
	}
	rxring->head = rxring->cur = j;
	txring->head = txring->cur = k;
	if (verbose && m > 0)
		D("%s sent %d packets to %p", msg, m, txring);

	return (m);
}

/* move packts from src to destination */
static int
move(struct nm_desc *src, struct nm_desc *dst, u_int limit)
{
	struct netmap_ring *txring, *rxring;
	u_int m = 0, si = src->first_rx_ring, di = dst->first_tx_ring;
	const char *msg = (src->req.nr_ringid & NETMAP_SW_RING) ?
		"host->net" : "net->host";

	while (si <= src->last_rx_ring && di <= dst->last_tx_ring) {
		rxring = NETMAP_RXRING(src->nifp, si);
		txring = NETMAP_TXRING(dst->nifp, di);
		ND("txring %p rxring %p", txring, rxring);
		if (nm_ring_empty(rxring)) {
			si++;
			continue;
		}
		if (nm_ring_empty(txring)) {
			di++;
			continue;
		}
		m += process_rings(rxring, txring, limit, msg);
	}

	return (m);
}

#ifndef NO_PCAP
struct filter_counter {
	char *ifname;
	uint64_t matched;
	uint64_t total;
	struct bpf_program fp;
};

static int
filter_init(struct filter_counter *fc, char *ifname, const char *filter_exp)
{
	pcap_t *handle;
	bpf_u_int32 net;
	bpf_u_int32 mask;
	char errbuf[PCAP_ERRBUF_SIZE];

	fc->ifname = ifname;
	fc->matched = fc->total = 0;

	/* Find the properties for the device */
	if (pcap_lookupnet(ifname, &net, &mask, errbuf) == -1) {
		D("Couldn't get netmask for device %s: %s\n", ifname, errbuf);
		mask = PCAP_NETMASK_UNKNOWN;
	}

	/* Open the session in promiscuous mode */
	handle = pcap_open_live(ifname, BUFSIZ, 1, 0, errbuf);
	if (handle == NULL) {
		D("Couldn't open device %s: %s\n", ifname, errbuf);
		return (-1);
	}
	/* Compile the filter */
	if (pcap_compile(handle, &fc->fp, filter_exp, 1, mask) == -1) {
		D("Couldn't parse filter: %s\n", filter_exp);
		return (-2);
	}

	/* And close the session */
	pcap_close(handle);

	return 0;
}

static void
filter_do(struct filter_counter *fc, struct nm_desc *nm, u_int limit)
{
	struct netmap_ring *rxring;
	u_int si = nm->first_rx_ring;

	while (si <= nm->last_rx_ring) {
		u_int j, m = 0;

		rxring = NETMAP_RXRING(nm->nifp, si);
		if (nm_ring_empty(rxring)) {
			si++;
			continue;
		}

		j = rxring->cur; /* RX */
		m = nm_ring_space(rxring);
		if (m < limit)
			limit = m;
		while (limit-- > 0) {
			struct netmap_slot *rs = &rxring->slot[j];
			char *rxbuf = NETMAP_BUF(rxring, rs->buf_idx);

			if (bpf_filter(fc->fp.bf_insns, (const u_char* ) rxbuf, rs->len, rs->len)) {
				fc->matched++;
			}
			fc->total++;

			j = nm_ring_next(rxring, j);
		}

		if (verbose && fc->total % 1000 == 0)
			D("Matched %lu/%lu", fc->matched, fc->total);

		si++;
	}
}
#endif


static void
usage(void)
{
	fprintf(stderr,
	    "usage: bridge [-v] [-i ifa] [-i ifb] [-b burst] [-w wait_time] [iface]\n");
	exit(1);
}

/*
 * bridge [-v] if1 [if2]
 *
 * If only one name, or the two interfaces are the same,
 * bridges userland and the adapter. Otherwise bridge
 * two intefaces.
 */
int
main(int argc, char **argv)
{
	struct pollfd pollfd[2];
	int ch;
	u_int burst = 1024, wait_link = 4;
	struct nm_desc *pa = NULL, *pb = NULL;
	char *ifa = NULL, *ifb = NULL;
	char ifabuf[64] = { 0 };
#ifndef NO_PCAP
	char *filter_exp = NULL;
	struct filter_counter fc[2];
#endif

	fprintf(stderr, "%s built %s %s\n",
		argv[0], __DATE__, __TIME__);

	while ( (ch = getopt(argc, argv, "b:cf:i:vw:")) != -1) {
		switch (ch) {
		default:
			D("bad option %c %s", ch, optarg);
			usage();
			break;
		case 'b':	/* burst */
			burst = atoi(optarg);
			break;
		case 'i':	/* interface */
			if (ifa == NULL)
				ifa = optarg;
			else if (ifb == NULL)
				ifb = optarg;
			else
				D("%s ignored, already have 2 interfaces",
					optarg);
			break;
		case 'c':
			zerocopy = 0; /* do not zerocopy */
			break;
#ifndef NO_PCAP
		case 'f':
			filter_exp = optarg;
			break;
#endif
		case 'v':
			verbose++;
			break;
		case 'w':
			wait_link = atoi(optarg);
			break;
		}

	}

	argc -= optind;
	argv += optind;

	if (argc > 1)
		ifa = argv[1];
	if (argc > 2)
		ifb = argv[2];
	if (argc > 3)
		burst = atoi(argv[3]);
	if (!ifb)
		ifb = ifa;
	if (!ifa) {
		D("missing interface");
		usage();
	}
	if (burst < 1 || burst > 8192) {
		D("invalid burst %d, set to 1024", burst);
		burst = 1024;
	}
	if (wait_link > 100) {
		D("invalid wait_link %d, set to 4", wait_link);
		wait_link = 4;
	}
	if (!strcmp(ifa, ifb)) {
		D("same interface, endpoint 0 goes to host");
		snprintf(ifabuf, sizeof(ifabuf) - 1, "%s^", ifa);
		ifa = ifabuf;
	} else {
		/* two different interfaces. Take all rings on if1 */
	}
	pa = nm_open(ifa, NULL, 0, NULL);
	if (pa == NULL) {
		D("cannot open %s", ifa);
		return (1);
	}
	// XXX use a single mmap ?
	pb = nm_open(ifb, NULL, NM_OPEN_NO_MMAP, pa);
	if (pb == NULL) {
		D("cannot open %s", ifb);
		nm_close(pa);
		return (1);
	}
	zerocopy = zerocopy && (pa->mem == pb->mem);
	D("------- zerocopy %ssupported", zerocopy ? "" : "NOT ");
#ifndef NO_PCAP
	if (filter_exp) {
		if (filter_init(&fc[0], ifa, filter_exp)) {
			D("Error setting filter '%s' on if %s", filter_exp, ifa);
			return (1);
		}
		if (filter_init(&fc[1], ifb, filter_exp)) {
			D("Error setting filter '%s' on if %s", filter_exp, ifb);
			return (1);
		}
	}
#endif

	/* setup poll(2) variables. */
	memset(pollfd, 0, sizeof(pollfd));
	pollfd[0].fd = pa->fd;
	pollfd[1].fd = pb->fd;

	D("Wait %d secs for link to come up...", wait_link);
	sleep(wait_link);
	D("Ready to go, %s 0x%x/%d <-> %s 0x%x/%d.",
		pa->req.nr_name, pa->first_rx_ring, pa->req.nr_rx_rings,
		pb->req.nr_name, pb->first_rx_ring, pb->req.nr_rx_rings);

	/* main loop */
	signal(SIGINT, sigint_h);
	while (!do_abort) {
		int n0, n1, ret;
		pollfd[0].events = pollfd[1].events = 0;
		pollfd[0].revents = pollfd[1].revents = 0;
		n0 = pkt_queued(pa, 0);
		n1 = pkt_queued(pb, 0);
#if defined(_WIN32) || defined(BUSYWAIT)
		if (n0){
			ioctl(pollfd[1].fd, NIOCTXSYNC, NULL);
			pollfd[1].revents = POLLOUT;
		}
		else {
			ioctl(pollfd[0].fd, NIOCRXSYNC, NULL);
		}
		if (n1){
			ioctl(pollfd[0].fd, NIOCTXSYNC, NULL);
			pollfd[0].revents = POLLOUT;
		}
		else {
			ioctl(pollfd[1].fd, NIOCRXSYNC, NULL);
		}
		ret = 1;
#else
		if (n0)
			pollfd[1].events |= POLLOUT;
		else
			pollfd[0].events |= POLLIN;
		if (n1)
			pollfd[0].events |= POLLOUT;
		else
			pollfd[1].events |= POLLIN;
		ret = poll(pollfd, 2, 2500);
#endif //defined(_WIN32) || defined(BUSYWAIT)
		if (ret <= 0 || verbose)
		    D("poll %s [0] ev %x %x rx %d@%d tx %d,"
			     " [1] ev %x %x rx %d@%d tx %d",
				ret <= 0 ? "timeout" : "ok",
				pollfd[0].events,
				pollfd[0].revents,
				pkt_queued(pa, 0),
				NETMAP_RXRING(pa->nifp, pa->cur_rx_ring)->cur,
				pkt_queued(pa, 1),
				pollfd[1].events,
				pollfd[1].revents,
				pkt_queued(pb, 0),
				NETMAP_RXRING(pb->nifp, pb->cur_rx_ring)->cur,
				pkt_queued(pb, 1)
			);
		if (ret < 0)
			continue;
		if (pollfd[0].revents & POLLERR) {
			struct netmap_ring *rx = NETMAP_RXRING(pa->nifp, pa->cur_rx_ring);
			D("error on fd0, rx [%d,%d,%d)",
				rx->head, rx->cur, rx->tail);
		}
		if (pollfd[1].revents & POLLERR) {
			struct netmap_ring *rx = NETMAP_RXRING(pb->nifp, pb->cur_rx_ring);
			D("error on fd1, rx [%d,%d,%d)",
				rx->head, rx->cur, rx->tail);
		}
#ifndef NO_PCAP
		if (filter_exp) {
			if (pollfd[0].revents & POLLIN) {
				filter_do(&fc[0], pa, burst);
			}
			if (pollfd[1].revents & POLLIN) {
				filter_do(&fc[1], pb, burst);
			}
		}
#endif
		if (pollfd[0].revents & POLLOUT) {
			move(pb, pa, burst);
			// XXX we don't need the ioctl */
			// ioctl(me[0].fd, NIOCTXSYNC, NULL);
		}
		if (pollfd[1].revents & POLLOUT) {
			move(pa, pb, burst);
			// XXX we don't need the ioctl */
			// ioctl(me[1].fd, NIOCTXSYNC, NULL);
		}
	}
#ifndef NO_PCAP
	if (filter_exp) {
		D("if %s matched %lu/%lu", fc[0].ifname, fc[0].matched, fc[0].total);
		D("if %s matched %lu/%lu", fc[1].ifname, fc[1].matched, fc[1].total);
	}
#endif
	D("exiting");
	nm_close(pb);
	nm_close(pa);

	return (0);
}
