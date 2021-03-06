#include <stdint.h>
#include <inttypes.h>
#include <signal.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
/* get cores */
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
/* dump packet contents */
#include <rte_hexdump.h>
#include <rte_malloc.h>
/* logging */
#include <rte_log.h>
/* timer event */
#include <rte_timer.h>
/* get clock cycles */
#include <rte_cycles.h>

/* libpaxos learner */
#include "learner.h"
/* paxos header definition */
#include "rte_paxos.h"
#include "const.h"
#include "utils.h"
#include "args.h"

#define BURST_TX_DRAIN_NS 100
#define FAKE_ADDR IPv4(192,168,4,198)

#define ETHER_ADDR_FOR_IPV4_MCAST(x)    \
        (rte_cpu_to_be_64(0x01005e000000ULL | ((x) & 0x7fffff)) >> 16)


struct dp_learner {
	int num_acceptors;
	int nb_learners;
	int learner_id;
    int nb_pkt_buf;
    int vcount;
    unsigned latest_accepted_iid;
    unsigned latest_prepare_iid;
	struct learner *paxos_learner;
    struct rte_mempool *mbuf_pool;
	struct rte_mempool *mbuf_tx;
    struct rte_eth_dev_tx_buffer *tx_buffer;
    struct rte_mbuf *tx_mbufs[BURST_SIZE];
    struct paxos_value *values[BURST_SIZE];
};

static const struct ether_addr mac95 = {
	.addr_bytes= { 0x0c, 0xc4, 0x7a, 0xa3, 0x25, 0xd0 }
};

static const struct ether_addr mac96 = {
	.addr_bytes= { 0x0c, 0xc4, 0x7a, 0xa3, 0x25, 0xc8 }
};

static const struct ether_addr mac97 = {
	.addr_bytes= { 0x0c, 0xc4, 0x7a, 0xa3, 0x25, 0x38 }
};

static const struct ether_addr mac98 = {
	.addr_bytes= { 0x0c, 0xc4, 0x7a, 0xa3, 0x25, 0x35 }
};

static rte_atomic32_t tx_counter = RTE_ATOMIC32_INIT(0);
static rte_atomic32_t rx_counter = RTE_ATOMIC32_INIT(0);
static uint32_t at_second;
static uint32_t dropped;
static bool primary_alive;

static struct rte_timer coord_timer;
static struct rte_timer timer;

static const struct rte_eth_conf port_conf_default = {
	.rxmode = { .max_rx_pkt_len = ETHER_MAX_LEN, },
};


enum Operation {
    GET,
    SET
};

struct command {
    struct timespec ts;
    uint16_t command_id;
    enum Operation op;
    char content[32];
};


struct __attribute__((__packed__)) client_request {
    uint16_t length;
    struct sockaddr_in cliaddr;
    char content[1];
};

static void __rte_unused
reset_tx_mbufs(struct dp_learner* dl, unsigned nb_tx)
{
    rte_atomic32_add(&tx_counter, nb_tx);
    rte_pktmbuf_alloc_bulk(dl->mbuf_tx, dl->tx_mbufs, nb_tx);
    dl->nb_pkt_buf = 0;
}


static int __rte_unused
dp_learner_send(struct dp_learner* dl,
					char* data, size_t size, struct sockaddr_in* dest) {

     struct rte_mbuf *pkt = dl->tx_mbufs[dl->nb_pkt_buf++];
	struct ipv4_hdr *iph;
	struct udp_hdr *udp_hdr;
	struct ether_hdr *phdr = rte_pktmbuf_mtod(pkt, struct ether_hdr *);

    int l2_len = sizeof(struct ether_hdr);
    int l3_len = sizeof(struct ipv4_hdr);
    int l4_len = sizeof(struct udp_hdr);

    iph = (struct ipv4_hdr *) ((char *)phdr + l2_len);
    udp_hdr = (struct udp_hdr *)((char *)iph + l3_len);

    char *datagram = rte_pktmbuf_mtod_offset(pkt,
                                char *, l2_len + l3_len + l4_len);
    rte_memcpy(datagram, data, size);

    phdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);
    // set src MAC address
	rte_eth_macaddr_get(0, &phdr->s_addr);
	switch (dest->sin_addr.s_addr) {
		case 0x5f04a8c0:
			ether_addr_copy(&mac95, &phdr->d_addr);
			break;
		case 0x6004a8c0:
			ether_addr_copy(&mac96, &phdr->d_addr);
			break;
		case 0x6104a8c0:
			ether_addr_copy(&mac97, &phdr->d_addr);
			break;
		case 0x6204a8c0:
			ether_addr_copy(&mac98, &phdr->d_addr);
			break;
		default:
			PRINT_INFO("Unknown host %x", dest->sin_addr.s_addr);
            print_addr(dest);
	}

    iph->total_length = rte_cpu_to_be_16(l3_len + l4_len + size);
    iph->version_ihl = 0x45;
    iph->time_to_live = 64;
    iph->packet_id = rte_cpu_to_be_16(rte_rdtsc());
    iph->fragment_offset = rte_cpu_to_be_16(IPV4_HDR_DF_FLAG);
    iph->next_proto_id = IPPROTO_UDP;
    iph->hdr_checksum = 0;
    iph->src_addr = rte_cpu_to_be_32(FAKE_ADDR);
    iph->dst_addr = dest->sin_addr.s_addr;  // Already in network byte order
	udp_hdr->dst_port = dest->sin_port; // Already in network byte order
    udp_hdr->src_port = rte_cpu_to_be_16(LEARNER_PORT);
	udp_hdr->dgram_len = rte_cpu_to_be_16(l4_len + size);
	pkt->l2_len = l2_len;
	pkt->l3_len = l3_len;
	pkt->l4_len = l4_len + size;
    pkt->data_len = l2_len + l3_len + l4_len + size;
    pkt->pkt_len = l2_len + l3_len + l4_len + size;
	pkt->ol_flags = PKT_TX_IPV4 | PKT_TX_IP_CKSUM | PKT_TX_UDP_CKSUM;
	udp_hdr->dgram_cksum = get_psd_sum(iph, ETHER_TYPE_IPv4, pkt->ol_flags);
    // pkt->udata64 = rte_rdtsc();
    int nb_tx = rte_eth_tx_buffer(0, 0, dl->tx_buffer, pkt);
	if (nb_tx)
        reset_tx_mbufs(dl, nb_tx);
	return 0;
}

static uint16_t
content_length(struct client_request *request)
{
    return request->length - (sizeof(struct client_request) - 1);
}


static int __rte_unused
deliver(unsigned int __rte_unused inst, __rte_unused char* val,
			__rte_unused size_t size, void* arg) {

	struct dp_learner* dl = (struct dp_learner*) arg;
    struct client_request *req = (struct client_request*)val;
    /* Skip command ID and client address */
    char *retval = (val + sizeof(uint16_t) + sizeof(struct sockaddr_in));
    struct command *cmd = (struct command*)(val + sizeof(struct client_request) - 1);

    if (cmd->command_id % dl->nb_learners == dl->learner_id) {
        // print_addr(&req->cliaddr);
        return dp_learner_send(dl, retval, content_length(req), &req->cliaddr);
    }
    return -1;
}


static int
paxos_rx_process(struct rte_mbuf *pkt, struct dp_learner* dl)
{
	int ret = -1;
	struct udp_hdr *udp_hdr;
	struct paxos_hdr *paxos_hdr;
	struct ether_hdr *phdr = rte_pktmbuf_mtod(pkt, struct ether_hdr *);

    int l2_len = sizeof(struct ether_hdr);
    int l3_len = sizeof(struct ipv4_hdr);
    // int l4_len = sizeof(struct udp_hdr);

    struct ipv4_hdr *iph = (struct ipv4_hdr *) ((char *)phdr + l2_len);
    if (rte_get_log_level() == RTE_LOG_DEBUG)
        rte_hexdump(stdout, "ip", iph, sizeof(struct ipv4_hdr));

    if (iph->next_proto_id != IPPROTO_UDP)
        return -1;

    udp_hdr = (struct udp_hdr *)((char *)iph + l3_len);
    /* SUME BAD: COORDINATOR and ACCEPTOR use the same port */
	if (!(udp_hdr->dst_port == rte_cpu_to_be_16(LEARNER_PORT) ||
          udp_hdr->dst_port == rte_cpu_to_be_16(ACCEPTOR_PORT)) &&
			(pkt->packet_type & RTE_PTYPE_TUNNEL_MASK) == 0)
		return -1;

	paxos_hdr = (struct paxos_hdr *)((char *)udp_hdr + sizeof(struct udp_hdr));

	if (rte_get_log_level() == RTE_LOG_DEBUG) {
		rte_hexdump(stdout, "udp", udp_hdr, sizeof(struct udp_hdr));
		rte_hexdump(stdout, "paxos", paxos_hdr, sizeof(struct paxos_hdr));
		print_paxos_hdr(paxos_hdr);
	}

	uint16_t msgtype = rte_be_to_cpu_16(paxos_hdr->msgtype);
	switch(msgtype) {
		case PAXOS_PROMISE: {
            int vsize = rte_be_to_cpu_32(paxos_hdr->value_len);
            struct paxos_value *v = paxos_value_new((char *)paxos_hdr->paxosval, vsize);
			struct paxos_promise promise = {
				.iid = rte_be_to_cpu_32(paxos_hdr->inst),
				.ballot = rte_be_to_cpu_16(paxos_hdr->rnd),
				.value_ballot = rte_be_to_cpu_16(paxos_hdr->vrnd),
				.aid = rte_be_to_cpu_16(paxos_hdr->acptid),
				.value = *v,
			};
			paxos_message pa;
			ret = learner_receive_promise(dl->paxos_learner, &promise, &pa.u.accept);
			if (ret){
                if (pa.u.accept.value.paxos_value_len == 0) {
                    int vid = dl->vcount - 1;
                    rte_memcpy(&pa.u.accept.value.paxos_value_val,
                                dl->values[vid]->paxos_value_val,
                                dl->values[vid]->paxos_value_len);
                    paxos_value_free(dl->values[vid]);
                    dl->vcount--;
                }
				add_paxos_message(&pa, pkt, LEARNER_PORT, ACCEPTOR_PORT, ACCEPTOR_ADDR);
            }
			break;
		}
        case PAXOS_ACCEPT: {
            union {
                uint64_t as_int;
                struct ether_addr as_addr;
            } dst_eth_addr;

            dst_eth_addr.as_int = ETHER_ADDR_FOR_IPV4_MCAST(ACCEPTOR_ADDR);
            ether_addr_copy(&dst_eth_addr.as_addr, &phdr->d_addr);
            iph->dst_addr = rte_cpu_to_be_32(ACCEPTOR_ADDR);
            iph->hdr_checksum = 0;
            paxos_hdr->inst = rte_cpu_to_be_32(dl->latest_accepted_iid++);
            pkt->l2_len = l2_len;
            pkt->l3_len = l3_len;
            pkt->l4_len = rte_be_to_cpu_16(udp_hdr->dgram_len);
            pkt->ol_flags = PKT_TX_IPV4 | PKT_TX_IP_CKSUM | PKT_TX_UDP_CKSUM;
            udp_hdr->dst_port = rte_cpu_to_be_16(ACCEPTOR_PORT);
            udp_hdr->dgram_cksum = get_psd_sum(iph, ETHER_TYPE_IPv4, pkt->ol_flags);
            struct rte_mbuf *cloned_pkt = rte_pktmbuf_clone(pkt, dl->mbuf_tx);
            dl->nb_pkt_buf++;
            rte_eth_tx_buffer(0, 0, dl->tx_buffer, cloned_pkt);
            /*
            int vsize = rte_be_to_cpu_32(paxos_hdr->value_len);
            dl->values[dl->vcount++] = paxos_value_new((char *)paxos_hdr->paxosval, vsize);
            */
            break;

        }
		case PAXOS_ACCEPTED: {
            int vsize = rte_be_to_cpu_32(paxos_hdr->value_len);
			struct paxos_value *v = paxos_value_new((char *)paxos_hdr->paxosval, vsize);
			struct paxos_accepted ack = {
				.iid = rte_be_to_cpu_32(paxos_hdr->inst),
				.ballot = rte_be_to_cpu_16(paxos_hdr->rnd),
				.value_ballot = rte_be_to_cpu_16(paxos_hdr->vrnd),
				.aid = rte_be_to_cpu_16(paxos_hdr->acptid),
				.value = *v,
			};
			ret = learner_receive_accepted(dl->paxos_learner, &ack);
			if (ret) {
                if (dl->latest_accepted_iid < ack.iid)
                    dl->latest_accepted_iid = ack.iid;
                /*
                paxos_message out;
                out.type = PAXOS_PREPARE;
                learner_prepare(dl->paxos_learner, &out.u.prepare, dl->latest_prepare_iid + 1);
                add_paxos_message(&out, pkt, LEARNER_PORT, ACCEPTOR_PORT, ACCEPTOR_ADDR);
                */
            }
		}
		default:
			PRINT_DEBUG("No handler for %u", msgtype);
	}
	return ret;
}

static uint16_t
add_timestamps(uint8_t port __rte_unused, uint16_t qidx __rte_unused,
        struct rte_mbuf **pkts, uint16_t nb_pkts,
        uint16_t max_pkts __rte_unused, void *user_param)
{
    struct dp_learner* dl = (struct dp_learner *)user_param;
    unsigned i;
    for (i = 0; i < nb_pkts; i++) {
        paxos_rx_process(pkts[i], dl);
    }
    return nb_pkts;
}

static inline int
port_init(uint8_t port, void* user_param)
{
    struct dp_learner *dl = (struct dp_learner *) user_param;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf *txconf;
	struct rte_eth_rxconf *rxconf;
	struct rte_eth_conf port_conf = port_conf_default;
	const uint16_t rx_rings = 1, tx_rings = 1;
	int retval;
	uint16_t q;

	rte_eth_dev_info_get(port, &dev_info);

	rxconf = &dev_info.default_rxconf;
	txconf = &dev_info.default_txconf;

	txconf->txq_flags &= PKT_TX_IPV4;
	txconf->txq_flags &= PKT_TX_UDP_CKSUM;
	if (port >= rte_eth_dev_count())
		return -1;

	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, RX_RING_SIZE,
				rte_eth_dev_socket_id(port), rxconf, dl->mbuf_pool);
		if (retval < 0)
			return retval;
	}

	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, TX_RING_SIZE,
				rte_eth_dev_socket_id(port), txconf);
		if (retval < 0)
			return retval;
	}

	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	rte_eth_promiscuous_enable(port);

	rte_eth_add_rx_callback(port, 0, add_timestamps, user_param);
	// rte_eth_add_tx_callback(port, 0, calc_latency, user_param);
	return 0;
}

static void
lcore_main(uint8_t port, struct dp_learner* dl)
{
    struct rte_mbuf *pkts_burst[BURST_SIZE];
    uint64_t prev_tsc, diff_tsc, cur_tsc;
    const uint64_t drain_tsc = (rte_get_tsc_hz() + NS_PER_S - 1) / NS_PER_S *
            BURST_TX_DRAIN_NS;

    prev_tsc = 0;

    rte_pktmbuf_alloc_bulk(dl->mbuf_tx, dl->tx_mbufs, BURST_SIZE);

	while (!force_quit) {
        cur_tsc = rte_rdtsc();
        /* TX burst queue drain */
        diff_tsc = cur_tsc - prev_tsc;
        if (unlikely(diff_tsc > drain_tsc)) {
            unsigned nb_tx = rte_eth_tx_buffer_flush(port, 0, dl->tx_buffer);
            if (nb_tx)
                PRINT_DEBUG("Sent %u", nb_tx);
                // reset_tx_mbufs(dl, nb_tx);
        }
        prev_tsc = cur_tsc;

		const uint16_t nb_rx = rte_eth_rx_burst(port, 0, pkts_burst, BURST_SIZE);
		if (unlikely(nb_rx == 0))
			continue;
        primary_alive = true;
        rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_TIMER, "Received %8d packets\n", nb_rx);
	    rte_atomic32_add(&rx_counter, nb_rx);
        uint16_t idx = 0;
        for (; idx < nb_rx; idx++)
            rte_pktmbuf_free(pkts_burst[idx]);
	}
}

static void
report_stat(struct rte_timer *tim, __attribute((unused)) void *arg)
{
    int nb_tx = rte_atomic32_read(&tx_counter);
    // int nb_rx = rte_atomic32_read(&rx_counter);
    // PRINT_INFO("Throughput: tx %8d, rx %8d, drop %8d", nb_tx, nb_rx, dropped);
    printf("%2d %8d\n", at_second++, nb_tx);
    rte_atomic32_set(&tx_counter, 0);
    rte_atomic32_set(&rx_counter, 0);
    dropped = 0;
    primary_alive = false;
    if (force_quit)
        rte_timer_stop(tim);
}


static void __rte_unused
check_primary_alive(struct rte_timer *tim, void *arg)
{
    if (primary_alive)
        return;

	struct dp_learner *dl = (struct dp_learner *) arg;
    if (dl->latest_accepted_iid <= 0)
        return;

    PRINT_INFO("PRIMARY FAILED at iid %u", dl->latest_accepted_iid);
    /*
    unsigned i;
    for (i = 0; i < BURST_SIZE; i++) {
         struct rte_mbuf *pkt = dl->tx_mbufs[dl->nb_pkt_buf++];
        paxos_message out;
        out.type = PAXOS_PREPARE;
        learner_prepare(dl->paxos_learner, &out.u.prepare, dl->latest_accepted_iid + i);
        add_paxos_message(&out, pkt, LEARNER_PORT, ACCEPTOR_PORT, ACCEPTOR_ADDR);
        int nb_tx = rte_eth_tx_buffer(0, 0, dl->tx_buffer, pkt);
        if (nb_tx)
            reset_tx_mbufs(dl, nb_tx);
    }
    */    
    dl->latest_prepare_iid += BURST_SIZE;
	rte_timer_stop(tim);
}

int
main(int argc, char *argv[])
{
	uint8_t portid = 0;
	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);
	force_quit = false;
    primary_alive = true;
	/* init EAL */
	int ret = rte_eal_init(argc, argv);

	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	if (rte_get_log_level() == RTE_LOG_DEBUG) {
		paxos_config.verbosity = PAXOS_LOG_DEBUG;
	}

    argc -= ret;
    argv += ret;

    parse_args(argc, argv);
	//initialize learner
	struct dp_learner dp_learner = {
		.num_acceptors = learner_config.nb_acceptors,
		.nb_learners = learner_config.nb_learners,
		.learner_id = learner_config.learner_id,
        .nb_pkt_buf = 0,
        .vcount = 0,
	};
    at_second = 0;
    dp_learner.mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL",
            NUM_MBUFS, MBUF_CACHE_SIZE, 0,
            RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

    if (dp_learner.mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create MBUF_POOL\n");

    dp_learner.mbuf_tx = rte_pktmbuf_pool_create("MBUF_TX",
            NUM_MBUFS, MBUF_CACHE_SIZE, 0,
            RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (dp_learner.mbuf_tx == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create MBUF_TX\n");


	dp_learner.paxos_learner = learner_new(dp_learner.num_acceptors);
	learner_set_instance_id(dp_learner.paxos_learner, 0);

	/* load deliver_timer, every second, on a slave lcore, reloaded automatically */
	uint64_t hz = rte_get_timer_hz();

	/* Call rte_timer_manage every 10ms */
	TIMER_RESOLUTION_CYCLES = hz / 100;

	unsigned lcore_id;
	/* init RTE timer library */
	rte_timer_subsystem_init();
	/* init timer structure */
	rte_timer_init(&coord_timer);

	/* slave core */
	lcore_id = rte_get_next_lcore(rte_lcore_id(), 0, 1);
	rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_USER1, "lcore_id: %d\n", lcore_id);
	rte_timer_reset(&coord_timer, hz, PERIODICAL, lcore_id, check_primary_alive, &dp_learner);
	rte_eal_remote_launch(check_timer_expiration, NULL, lcore_id);

    dp_learner.tx_buffer = rte_zmalloc_socket("tx_buffer",
                RTE_ETH_TX_BUFFER_SIZE(BURST_SIZE), 0,
                rte_eth_dev_socket_id(portid));
    if (dp_learner.tx_buffer == NULL)
        rte_exit(EXIT_FAILURE, "Cannot allocate buffer for tx on port %u\n",
                (unsigned) portid);

    rte_eth_tx_buffer_init(dp_learner.tx_buffer, BURST_SIZE);
    // rte_eth_tx_buffer_set_err_callback(tx_buffer, on_sending_error, NULL);
    ret = rte_eth_tx_buffer_set_err_callback(dp_learner.tx_buffer,
                rte_eth_tx_buffer_count_callback,
                &dropped);
    if (ret < 0)
            rte_exit(EXIT_FAILURE, "Cannot set error callback for "
                    "tx buffer on port %u\n", (unsigned) portid);

    /* display stats every period seconds */
    lcore_id = rte_get_next_lcore(rte_lcore_id(), 0, 1);
    rte_timer_reset(&timer, hz, PERIODICAL, lcore_id, report_stat, NULL);
    rte_eal_remote_launch(check_timer_expiration, NULL, lcore_id);

    rte_timer_subsystem_init();


    if (port_init(portid, &dp_learner) != 0)
            rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu8"\n", portid);

	lcore_main(portid, &dp_learner);

	rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_USER1, "free learner\n");
	learner_free(dp_learner.paxos_learner);
	return 0;
}

