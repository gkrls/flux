#ifndef DPA_BACKEND_DPDK_UTILS_H
#define DPA_BACKEND_DPDK_UTILS_H

#include "dpa/allreduce.h"
#include "dpa/util/config.h"
#include "dpa/util/error.h"
#include <cstdint>
#include <rte_flow.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_mempool.h>

namespace dpa {
namespace dpdk {

inline std::string dpdkInferPortFromArgs(std::vector<std::string> const &argv) {
  std::string port_name;
  for (int i = 1; i < argv.size(); i++) {
    std::string const &s = argv[i];
    if (s.rfind("--vdev=", 0) == 0) {
      auto v = s.substr(7);
      port_name = v.substr(0, v.find(','));
      break;
    }
    if ((s == "-a" || s == "-w") && i + 1 < argv.size()) {
      port_name = argv[++i];
      break;
    }
  }
  return port_name.size() ? port_name : "";
}

inline bool filterPacket(rte_mbuf &mbuf, rte_ether_addr &mac_addr, rte_be32_t ip_addr, rte_be16_t udp_port) {
  const uint32_t avail = rte_pktmbuf_data_len(&mbuf);
  const auto *eth = rte_pktmbuf_mtod(&mbuf, const rte_ether_hdr *);
  if (!rte_is_same_ether_addr(&eth->dst_addr, &mac_addr)) return false;
  uint16_t ether_type = rte_be_to_cpu_16(eth->ether_type);
  uint32_t off = sizeof(*eth);
  // Here we should check for VLAN but we skip this for now
  if (ether_type != RTE_ETHER_TYPE_IPV4) return false;
  if (avail < off + sizeof(rte_ipv4_hdr)) return false;
  const auto *ip = rte_pktmbuf_mtod_offset(&mbuf, const rte_ipv4_hdr *, off);
  if (ip->next_proto_id != IPPROTO_UDP) return false;
  if (ip->dst_addr != ip_addr) return false; // compare in network order
  const uint32_t ihl = (ip->version_ihl & 0x0Fu) * 4u;
  if (ihl < sizeof(rte_ipv4_hdr)) return false;
  if (avail < off + ihl + sizeof(rte_udp_hdr)) return false;
  const auto *udph = rte_pktmbuf_mtod_offset(&mbuf, const rte_udp_hdr *, off + ihl);
  if (udph->dst_port != udp_port) return false; // compare in network order
  return true;
}

inline void printDevInfo(struct rte_eth_dev_info &dev_info) {
  std::cout << "dpa: DPDK device info:" 
            << "\n    RX buffer min size: " << dev_info.min_rx_bufsize
            << "\n    RX/TX queues max number: " << dev_info.max_rx_queues << '/' << dev_info.max_tx_queues
            << "\n    RX/TX per-port offload capabilities: 0x" << dev_info.rx_offload_capa << '/' << dev_info.tx_offload_capa
            << "\n    RX/TX per-queue offload capabilities: 0x" << dev_info.rx_queue_offload_capa << '/' << dev_info.tx_queue_offload_capa
            << "\n    RX descriptors limits: ["<< dev_info.rx_desc_lim.nb_min << "," << dev_info.rx_desc_lim.nb_max << "] aligned: " << dev_info.rx_desc_lim.nb_align
            << "\n    TX descriptors limits: [" << dev_info.tx_desc_lim.nb_min << "," << dev_info.tx_desc_lim.nb_max << "] aligned: " << dev_info.tx_desc_lim.nb_align
            << "\n    RX queue share: " << ((dev_info.dev_capa & RTE_ETH_DEV_CAPA_RXQ_SHARE) ? "Supported" : "Unsupported")
            << "\n";
}

inline void checkPortLinkStatus(uint16_t portid) {
  struct rte_eth_link link;
  for (uint8_t count = 0; count <= 90; count++) {
    memset(&link, 0, sizeof(link));
    rte_eth_link_get_nowait(portid, &link);
    if (link.link_status) {
      dpa::Info("DPDK eth link up. Speed {} Mbps {}", link.link_speed,
                (link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX) ? ("Full-duplex") : ("Half-duplex"));
      return;
    } else {
      rte_delay_ms(100);
    }
  }
  DPA_THROW("Link Down!")
}

// eth / ipv4 (proto=UDP) / udp dst=<port> -> QUEUE <rx_q>
inline void createWorkerFlow(uint16_t port_id, uint16_t rx_q, uint16_t udp_dst_port, bool use_priorities) {
  struct rte_flow_attr attr = {};
  memset(&attr, 0, sizeof(struct rte_flow_attr));
  attr.ingress = 1;
  if (use_priorities) attr.priority = rx_q; // 10 + rx_q; // TAP only

  struct rte_flow_item pattern[4] = {};
  struct rte_flow_item_eth eth = {};
  struct rte_flow_item_ipv4 ip_spec = {}, ip_mask = {};
  struct rte_flow_item_udp udp_spec = {}, udp_mask = {};
  struct rte_flow_action actions[2] = {};
  struct rte_flow_action_queue act_q = {.index = rx_q};
  struct rte_flow_error err;
  memset(pattern, 0, sizeof(pattern));
  memset(actions, 0, sizeof(actions));

  ip_spec.hdr.next_proto_id = IPPROTO_UDP;
  ip_mask.hdr.next_proto_id = 0xFF;
  udp_spec.hdr.dst_port = rte_cpu_to_be_16(udp_dst_port);
  udp_mask.hdr.dst_port = 0xFFFF;

  pattern[0] = (struct rte_flow_item){.type = RTE_FLOW_ITEM_TYPE_ETH, .spec = 0, .mask = 0};
  pattern[1] = (struct rte_flow_item){.type = RTE_FLOW_ITEM_TYPE_IPV4, .spec = &ip_spec, .mask = &ip_mask};
  pattern[2] = (struct rte_flow_item){.type = RTE_FLOW_ITEM_TYPE_UDP, .spec = &udp_spec, .mask = &udp_mask};
  pattern[3] = (struct rte_flow_item){.type = RTE_FLOW_ITEM_TYPE_END};

  actions[0] = (struct rte_flow_action){.type = RTE_FLOW_ACTION_TYPE_QUEUE, .conf = &act_q};
  actions[1] = (struct rte_flow_action){.type = RTE_FLOW_ACTION_TYPE_END};

  int ret = rte_flow_validate(port_id, &attr, pattern, actions, &err);
  DPA_THROW_IF(ret < 0, "rte_flow_validate: {}", err.message ? err.message : "n/a");

  struct rte_flow *f = rte_flow_create(port_id, &attr, pattern, actions, &err);
  Info("Flow created: UDP dst={} -> queue {}", udp_dst_port, rx_q);

  if (!f && rte_errno == EEXIST) {
    dpa::Warn("Flow already exists: dport={} -> q{}; continuing", udp_dst_port, rx_q);
  } else {
    DPA_THROW_IF(!f, "rte_flow_create: {}", err.message ? err.message : "n/a");
  }
}

inline struct rte_flow *createWorkerFlow2(uint16_t port_id, uint16_t rx_q, uint16_t udp_dst_port, bool use_priorities) {
  struct rte_flow_attr attr = {};
  attr.ingress = 1;
  if (use_priorities) attr.priority = rx_q;

  struct rte_flow_item pattern[4] = {};
  struct rte_flow_item_eth eth = {};
  struct rte_flow_item_ipv4 ip_spec = {}, ip_mask = {};
  struct rte_flow_item_udp udp_spec = {}, udp_mask = {};
  struct rte_flow_action actions[3] = {}; // Changed to 3
  struct rte_flow_action_queue act_q = {.index = rx_q};
  struct rte_flow_error err;

  ip_spec.hdr.next_proto_id = IPPROTO_UDP;
  ip_mask.hdr.next_proto_id = 0xFF;
  udp_spec.hdr.dst_port = rte_cpu_to_be_16(udp_dst_port);
  udp_mask.hdr.dst_port = 0xFFFF;

  pattern[0] = (struct rte_flow_item){.type = RTE_FLOW_ITEM_TYPE_ETH};
  pattern[1] = (struct rte_flow_item){.type = RTE_FLOW_ITEM_TYPE_IPV4, .spec = &ip_spec, .mask = &ip_mask};
  pattern[2] = (struct rte_flow_item){.type = RTE_FLOW_ITEM_TYPE_UDP, .spec = &udp_spec, .mask = &udp_mask};
  pattern[3] = (struct rte_flow_item){.type = RTE_FLOW_ITEM_TYPE_END};

  actions[0] = (struct rte_flow_action){.type = RTE_FLOW_ACTION_TYPE_COUNT}; // Add counter
  actions[1] = (struct rte_flow_action){.type = RTE_FLOW_ACTION_TYPE_QUEUE, .conf = &act_q};
  actions[2] = (struct rte_flow_action){.type = RTE_FLOW_ACTION_TYPE_END};

  int ret = rte_flow_validate(port_id, &attr, pattern, actions, &err);
  DPA_THROW_IF(ret < 0, "rte_flow_validate: {}", err.message ? err.message : "n/a");

  struct rte_flow *f = rte_flow_create(port_id, &attr, pattern, actions, &err);
  Info("Flow created: UDP dst={} -> queue {}", udp_dst_port, rx_q);

  if (!f && rte_errno == EEXIST) {
    dpa::Warn("Flow already exists: dport={} -> q{}; continuing", udp_dst_port, rx_q);
  } else {
    DPA_THROW_IF(!f, "rte_flow_create: {}", err.message ? err.message : "n/a");
  }

  return f; // Return the flow handle
}

inline struct rte_flow *createWorkerFlow3(uint16_t port_id, uint16_t rx_q, uint16_t udp_dst_port, bool use_priorities) {
  struct rte_flow_attr attr = {0};
  attr.ingress = 1;

  struct rte_flow_item pattern[] = {{.type = RTE_FLOW_ITEM_TYPE_ETH}, {.type = RTE_FLOW_ITEM_TYPE_END}};

  struct rte_flow_action_queue queue_action = {.index = 0};
  struct rte_flow_action action[] = {{.type = RTE_FLOW_ACTION_TYPE_QUEUE, .conf = &queue_action}, {.type = RTE_FLOW_ACTION_TYPE_END}};

  struct rte_flow_error error;
  struct rte_flow *flow = rte_flow_create(0, &attr, pattern, action, &error);
  if (flow) {
    printf("Created catch-all flow\n");
  } else {
    printf("Catch-all flow failed: %s\n", error.message);
  }

  if (flow) {
    printf("Flow created: handle=%p\n", flow);

    // Verify it was installed
    struct rte_flow_error verr;
    if (rte_flow_validate(0, &attr, pattern, action, &verr) != 0) { printf("Flow validation FAILED after creation: %s\n", verr.message); }
  } else {
    printf("Flow creation FAILED: %s\n", error.message ? error.message : "null");
  }
  return flow;
}

// TAP only: let non-matching traffic continue to the kernel
inline void createTAPPassthroughFlow(uint16_t port_id) {
  struct rte_flow_attr attr = {};
  attr.ingress = 1;
  attr.priority = 100; // TAP only
  struct rte_flow_item patt[] = {{.type = RTE_FLOW_ITEM_TYPE_ETH}, {.type = RTE_FLOW_ITEM_TYPE_END}};
  struct rte_flow_action acts[] = {{.type = RTE_FLOW_ACTION_TYPE_PASSTHRU}, {.type = RTE_FLOW_ACTION_TYPE_END}};
  struct rte_flow_error err;
  DPA_THROW_IF(!rte_flow_create(port_id, &attr, patt, acts, &err), "tap PASSTHRU create: %s", err.message ? err.message : "n/a");
}
// inline void resetFlows(uint16_t port_id) {
//   struct rte_flow_error err;
//   int rc = rte_flow_flush(port_id, &err);
//   if (rc < 0) {
//     // ignore “no flows” etc.; just log
//     dpa::Warn("rte_flow_flush: %s", err.message ? err.message : "n/a");
//   }
// }

inline void createARPDropFlow(uint16_t port_id) {
  struct rte_flow_attr attr = {};
  attr.ingress = 1;
  attr.priority = 5; // Higher priority (lower number) than worker flows

  struct rte_flow_item_eth eth_spec = {}, eth_mask = {};
  eth_spec.type = rte_cpu_to_be_16(0x0806); // ARP
  eth_mask.type = 0xFFFF;

  struct rte_flow_item pattern[] = {{.type = RTE_FLOW_ITEM_TYPE_ETH, .spec = &eth_spec, .mask = &eth_mask},
                                    {.type = RTE_FLOW_ITEM_TYPE_END}};

  struct rte_flow_action actions[] = {{.type = RTE_FLOW_ACTION_TYPE_DROP}, {.type = RTE_FLOW_ACTION_TYPE_END}};

  struct rte_flow_error err;
  struct rte_flow *f = rte_flow_create(port_id, &attr, pattern, actions, &err);
  DPA_THROW_IF(!f, "ARP drop flow create: %s", err.message ? err.message : "n/a");
}

inline void forceClearTAPFlows(const char *ifname) {
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s clsact 2>/dev/null || true", ifname);
  system(cmd);
  snprintf(cmd, sizeof(cmd), "tc qdisc add dev %s clsact 2>/dev/null || true", ifname);
  system(cmd);
}

inline bool match_udp_dport_range(struct rte_mbuf *m, uint16_t base, uint16_t n) {
  // Ensure headers are contiguous (DPDK mbufs from af_packet usually are)
  const uint8_t *p = rte_pktmbuf_mtod(m, const uint8_t *);
  const size_t len = rte_pktmbuf_data_len(m);
  if (len < sizeof(rte_ether_hdr) + sizeof(rte_ipv4_hdr) + sizeof(rte_udp_hdr)) return false;

  const rte_ether_hdr *eth = (const rte_ether_hdr *)p;
  if (rte_be_to_cpu_16(eth->ether_type) != RTE_ETHER_TYPE_IPV4) return false;

  const rte_ipv4_hdr *ip4 = (const rte_ipv4_hdr *)(p + sizeof(rte_ether_hdr));
  if (ip4->next_proto_id != IPPROTO_UDP) return false;

  const uint8_t ihl = (ip4->version_ihl & 0x0F) * 4;
  if (sizeof(rte_ether_hdr) + ihl + sizeof(rte_udp_hdr) > len) return false;

  const rte_udp_hdr *udp = (const rte_udp_hdr *)(p + sizeof(rte_ether_hdr) + ihl);
  uint16_t dport = rte_be_to_cpu_16(udp->dst_port);
  return (dport >= base) && (dport < (uint16_t)(base + n));
}

__rte_always_inline __attribute__((hot)) void sendCloned(uint16_t port_id, uint16_t qid, struct rte_eth_dev_tx_buffer *tx_buffer,
                                                         struct rte_mempool *mpool, struct rte_mbuf *mbuf) {
  if (auto *c = rte_pktmbuf_clone(mbuf, mpool)) { rte_eth_tx_buffer(port_id, qid, tx_buffer, c); }
}

__rte_always_inline __attribute__((hot)) void sendRefcounted(uint16_t port_id, uint16_t qid, struct rte_eth_dev_tx_buffer *tx_buffer,
                                                             struct rte_mbuf *mbuf) {
  rte_mbuf_refcnt_update(mbuf, 1);
  rte_eth_tx_buffer(port_id, qid, tx_buffer, mbuf);
}

__rte_always_inline __attribute__((hot)) void dumpMbuf(const char *tag, struct rte_mbuf *m, uint16_t header_offset) {
  struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
  struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
  struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
  fprintf(stderr, "%s m=%p pool=%p ref=%u pkt_len=%u data_len=%u nb_segs=%u l2=%u l3=%u l4=%u ol=0x%016" PRIx64 " ip=%04x udp=%04x\n", tag,
          (void *)m, (void *)m->pool, rte_mbuf_refcnt_read(m), m->pkt_len, m->data_len, m->nb_segs, m->l2_len, m->l3_len, m->l4_len,
          m->ol_flags, ip->hdr_checksum, udp->dgram_cksum);
}

// Reclaim all completed TX descriptors and free their mbufs.
static inline uint32_t txq_reclaim_done(uint16_t port, uint16_t qid) {
  uint32_t total = 0;
  for (;;) {
    // nb_pkts is a hint/limit; many PMDs will free "as many as possible".
    uint32_t n = rte_eth_tx_done_cleanup(port, qid, UINT16_MAX);
    if (n == 0) break;
    total += n;
  }
  return total;
}

inline int tx_drop_all(uint16_t port, uint16_t qid, struct rte_eth_txconf const *tx_conf, uint16_t nb_desc, int socket_id,
                       struct rte_eth_dev_tx_buffer *txbuf /* can be NULL */) {
  // 1) Block new enqueues on this queue
  int rc = rte_eth_dev_tx_queue_stop(port, qid);
  if (rc < 0) return rc;

  // 2) Dump any software-staged packets without sending.
  if (txbuf) {
    // With the queue stopped, flush will fail to enqueue → drop callback frees mbufs.
    rte_eth_tx_buffer_flush(port, qid, txbuf);
  }

  // 3) Hard reset the HW ring. This reinitializes descriptors and releases any mbufs
  //    the PMD was still holding, without transmitting them.
  rc = rte_eth_tx_queue_setup(port, qid, nb_desc, socket_id, tx_conf);
  if (rc < 0) return rc;

  // 4) Bring the queue back up.
  rc = rte_eth_dev_tx_queue_start(port, qid);
  return rc;
}

__rte_always_inline void tx_buffer_discard(struct rte_eth_dev_tx_buffer *buf) {
  // Drop everything currently buffered without sending
  const uint16_t n = buf->length;
  rte_pktmbuf_free_bulk(buf->pkts, buf->length);
  // for (uint16_t i = 0; i < n; ++i) { rte_pktmbuf_free(buf->pkts[i]); }
  buf->length = 0;
}

__rte_always_inline void rx_drain_fast(uint16_t port, uint16_t queue) {
  // Bigger bursts drain faster (fewer PMD calls).
  static thread_local const uint16_t BURST = 128;
  int drained = 0;
  rte_mbuf *pkts[BURST];
  // for (;;) {
  //   const uint16_t n = rte_eth_rx_burst(port, queue, pkts, BURST);
  //   if (n == 0) break;
  //   drained += n;
  //   rte_pktmbuf_free_bulk(pkts, n);
  //   if (n < BURST) break; // If we didn't fill the whole burst, the queue is likely empty.
  // }
  // if (drained) DEBUG("Drained {} old packets at task start", drained);
  const uint32_t budget = 4096;
  uint32_t done = 0;
  while (done < budget) {
    uint16_t n = rte_eth_rx_burst(port, queue, pkts, BURST);
    if (!n) break;
    rte_pktmbuf_free_bulk(pkts, n);
    done += n;
    if (n < BURST) break;
  }
}

__rte_always_inline __attribute__((hot)) void *reuse(rte_mbuf *m, uint16_t frame_size, uint64_t hw_csum) {

  // m->port = 0;
  m->packet_type = 0;
  m->ol_flags &=
      ~(RTE_MBUF_F_RX_RSS_HASH | RTE_MBUF_F_RX_FDIR | RTE_MBUF_F_RX_VLAN | RTE_MBUF_F_RX_IP_CKSUM_MASK | RTE_MBUF_F_RX_L4_CKSUM_MASK);
  m->hash.rss = 0;

  struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
  struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
  struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);

  RTE_SWAP(eth->src_addr, eth->dst_addr);
  RTE_SWAP(ip->src_addr, ip->dst_addr);
  RTE_SWAP(udp->src_port, udp->dst_port);

  uint32_t pkt_len = m->pkt_len;
  uint32_t data_off = m->data_off;
  void *buf_addr = m->buf_addr;
  // struct rte_mempool *pool = m->pool;

  // Reset the entire mbuf structure
  // rte_pktmbuf_reset(m);

  // Restore necessary fields
  // m->buf_addr = buf_addr;
  // m->pool = pool;
  // m->data_off = data_off;
  // m->pkt_len = pkt_len;
  // m->data_len = pkt_len;
  // m->nb_segs = 1;

  ip->total_length = rte_cpu_to_be_16(frame_size - sizeof(*eth));
  udp->dgram_len = rte_cpu_to_be_16(frame_size - sizeof(*eth) - sizeof(*ip));

  // if (ip->time_to_live == 0) { DPA_THROW("ip->ttl is 0!!!!!"); }

  // CRITICAL: Clear RX-specific mbuf metadata before reusing for TX
  m->packet_type = 0;
  m->hash.rss = 0;
  m->vlan_tci = 0;
  m->vlan_tci_outer = 0;
  // m->timestamp = 0;

  // Clear all flags first, then set TX flags
  m->ol_flags = 0;

  // Set TX header lengths
  m->l2_len = sizeof(*eth);
  m->l3_len = sizeof(*ip);
  m->l4_len = sizeof(*udp);

  // Set TX offload flags if hardware checksum is enabled
  if (hw_csum) { m->ol_flags = RTE_MBUF_F_TX_UDP_CKSUM | RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_IPV4; }

  return udp + 1;
}


__rte_always_inline __attribute__((hot)) static rte_mbuf *checksum(struct rte_mbuf *m) {
  auto *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
  auto *ip = (struct rte_ipv4_hdr *)(eth + 1);
  auto *udp = (struct rte_udp_hdr *)(ip + 1);
  if (m->ol_flags != 0) {
    // HW offload path: prep pseudo header now; full UDP csum will be done by NIC
    ip->hdr_checksum = 0;
    udp->dgram_cksum = rte_ipv4_phdr_cksum(ip, m->ol_flags);
  } else {
    // DPA_THROW("COMPUTING SOFTWARE CHECKSUMS!!!");
    // SW path: compute the checksums
    ip->hdr_checksum = rte_ipv4_cksum(ip);
    udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, udp);
  }
  return m;
}

__rte_always_inline __attribute__((hot)) static void verifyRx(struct rte_mbuf *m) {
  auto *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
  uint16_t ether_type = ntohs(eth->ether_type);
  if (ether_type == 0x0806) { DPA_THROW("ARP packet received in worker queue! This shouldn't happen with proper flow rules."); }
  if (ether_type != RTE_ETHER_TYPE_IPV4) { DPA_THROW("Non-IPv4 packet received: ether_type=0x{:04x}", ether_type); }
  auto *ip = (struct rte_ipv4_hdr *)(eth + 1);
  if (ip->next_proto_id != IPPROTO_UDP) { DPA_THROW("Non-UDP IP packet received: proto={}", ip->next_proto_id); }
}

} // namespace dpdk
} // namespace dpa

#endif // DPA_BACKEND_DPDK_UTILS_H