#include "dpa/allreduce.h"
#include "dpa/backend_dpdk/backend_dpdk.h"
#include "dpa/backend.h"
#include "dpa/context.h"
#include "dpa/util/error.h"
#include "dpa/util/log.h"
#include "dpa/util/pp.h"
#include "dpa/util/prof.h"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <generic/rte_cycles.h>
#include <memory>
#include <mutex>
#include <rte_common.h>
#include <rte_errno.h>
#include <rte_ether.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_mempool.h>
#include <rte_timer.h>
#include <rte_udp.h>

using namespace dpa;

DpdkWorker::DpdkWorker(uint16_t tid, uint16_t lcore, rte_mempool *rx_pool, DpdkBackend &backend)
    : BackendWorker(tid, backend), tid(tid), lcore(lcore), backend(backend), opt(backend.opt), rx_pool(rx_pool),
      global_pool_base(backend.context.device.session.pool.base), global_pool_size(backend.context.device.session.pool.size),
      subpool_size(backend.opt.window * 2), subpool_base_local(tid * backend.opt.window * 2),
      subpool_base_global(backend.context.device.session.pool.base + tid * backend.opt.window * 2), prof(tid, backend.opt.profile_skip) {}

static thread_local struct {
  uint64_t unsent;
} dbg = {};

static void tx_buf_err_callback(struct rte_mbuf **pkts, uint16_t unsent, void *arg) {
  (void)arg;
  // dbg.unsent += unsent;
  // dump the first one to see why it might be rejected
  // if (unsent) {
  //   struct rte_mbuf *m = pkts[0];
  //   fprintf(stderr, "[txbuf] unsent=%u pkt_len=%u ol_flags=0x%" PRIx64 " l2=%u l3=%u l4=%u\n", unsent, m->pkt_len, m->ol_flags,
  //   m->l2_len,
  //           m->l3_len, m->l4_len);
  //   DPA_THROW("WTTTTTTTTF\n");
  // }
  rte_pktmbuf_free_bulk(pkts, unsent);
}

void DpdkWorker::main() {
  DEBUG("DpdkWorker {} running at lcore {}", tid, lcore);
  auto const &ctx = backend.context;
  auto const &dev = ctx.getDevice();
  auto const rte_soc = rte_socket_id();

  // Create a mempool for TX
  std::string tx_pool_name = "dpa_worker_" + std::to_string(tid) + "_tx";
  tx_pool = rte_pktmbuf_pool_create(tx_pool_name.c_str(), opt.tx_pool_size, opt.tx_pool_cache, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_soc);
  DPA_THROW_IF(tx_pool == NULL, "Failed to create tx_mpool for worker {}: {}", tid, rte_strerror(rte_errno));
  DPA_THROW_IF(frame_size + RTE_PKTMBUF_HEADROOM > rte_pktmbuf_data_room_size(tx_pool), "max frame_size ({}) exceeds TX data room ({})",
               frame_size, rte_pktmbuf_data_room_size(tx_pool));
  DPA_THROW_IF(frame_size + RTE_PKTMBUF_HEADROOM > rte_pktmbuf_data_room_size(rx_pool), "max frame_size ({}) exceeds RX data room ({})",
               frame_size, rte_pktmbuf_data_room_size(rx_pool));

  // Allocate buffer to be used to bulk TX
  tx_buffer = (rte_eth_dev_tx_buffer *)rte_zmalloc_socket("tx_buffer", RTE_ETH_TX_BUFFER_SIZE(opt.tx_burst), RTE_CACHE_LINE_SIZE, rte_soc);
  DPA_THROW_IF(tx_buffer == NULL, "rte_zmalloc_socket failed");
  DPA_THROW_IF(rte_eth_tx_buffer_init(tx_buffer, opt.tx_burst) < 0, "rte_eth_tx_buffer_init failed");
  DPA_THROW_IF(rte_eth_tx_buffer_set_err_callback(tx_buffer, tx_buf_err_callback, this) < 0, "rte_eth_tx_buffer_set_err_callback failed");

  // Create rx mbuf pointers
  rx_mbufs = (rte_mbuf **)rte_malloc_socket("rx_mbufs", opt.rx_burst * sizeof(struct rte_mbuf *), RTE_CACHE_LINE_SIZE, rte_soc);
  DPA_THROW_IF(rx_mbufs == NULL, "Failed to create rx_mbufs for worker {}: {}", tid, rte_strerror(rte_errno));

  // Fill mac/ip/port copies
  DPA_THROW_IF(rte_eth_macaddr_get(opt.eal_port_id, &smac) < 0, "Worker {} failed to retrieve MAC for port_id {}", tid, opt.eal_port_id);
  memcpy(&dmac, backend.context.device.mac.bytes().data(), RTE_ETHER_ADDR_LEN);
  in_addr addr1{}, addr2{};
  DPA_THROW_IF(inet_pton(AF_INET, opt.addr.c_str(), &addr1) != 1, "Failed to parse host ip address");
  DPA_THROW_IF(inet_pton(AF_INET, backend.context.device.ip.str().c_str(), &addr2) != 1, "Failed to parse device ip address");
  saddr = addr1.s_addr;
  daddr = addr2.s_addr;
  sport = rte_cpu_to_be_16(opt.port + tid);
  dport = rte_cpu_to_be_16(backend.context.device.port);

  // ceil(hz / 1e6)  => cycles per microsecond (rounded up)
  re_timeout = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * opt.timeout.count();
  re_timeout_scaled = (opt.timeout_init_scaling * re_timeout + 0.5);

  // std::cout << "STRAGGLE_AWARE: " << (int) backend.context.device.straggle_aware << '\n';
  header_size = (backend.context.device.straggle_aware ? sizeof(AllReduceHeader) : sizeof(AllReduceHeaderNS));
  // std::cout << "HEADER_SIZE" << (int) header_size << '\n';
  header_offset = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr);
  payload_offset = header_offset + header_size;

  payload_len = backend.context.device.valuesPerPipe() * backend.context.device.pipes;
  payload_size = payload_len * 4;
  packet_size = header_size + payload_size;
  frame_size = sizeof(rte_ether_hdr) + sizeof(rte_ipv4_hdr) + sizeof(rte_udp_hdr) + packet_size;

  // Create window entries
  win_capacity = backend.opt.window;
  win = (DpdkWorker::Entry *)rte_malloc_socket("win", win_capacity * sizeof(Entry), RTE_CACHE_LINE_SIZE, rte_soc);

  for (auto i = 0; i < win_capacity; ++i) {
    Entry &e = win[i];
    e.slot[0] = Slot::get_for_entry(win_capacity, i, 0, subpool_base_global); // subpool_base_global + i * 2;
    e.slot[1] = Slot::get_for_entry(win_capacity, i, 1, subpool_base_global); // subpool_base_global + i * 2 + 1;
    e.seq[0] = backend.context.device.session.pool.seqnums[Slot::get_for_entry(win_capacity, i, 0, subpool_base_local)]; // 1 
    e.seq[1] = backend.context.device.session.pool.seqnums[Slot::get_for_entry(win_capacity, i, 1, subpool_base_local)]; // 1
    e.seq_last[0] = e.seq[0];
    e.seq_last[1] = e.seq[1];
    e.ver = 0; // we always start at 0
    e.idx = i;

    // Create the entry's mbuf
    // e.mbuf_rtx = nullptr;
    e.mbuf0 = rte_pktmbuf_alloc(tx_pool);
    DPA_THROW_IF(e.mbuf0 == NULL, "failed to create mbuf0 for worker {} entry {}", tid, i);

    // Prebuild L2/L3/L4 once into mbuf[0]
    rte_pktmbuf_reset(e.mbuf0);
    rte_pktmbuf_append(e.mbuf0, frame_size);
    e.mbuf0->nb_segs = 1;
    e.mbuf0->next = nullptr;
    e.mbuf0->l2_len = sizeof(struct rte_ether_hdr);
    e.mbuf0->l3_len = sizeof(struct rte_ipv4_hdr);
    e.mbuf0->l4_len = sizeof(struct rte_udp_hdr);
    e.mbuf0->ol_flags = opt.hw_csum ? (RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_UDP_CKSUM) : 0;

    auto *eth = rte_pktmbuf_mtod(e.mbuf0, rte_ether_hdr *);
    eth->src_addr = smac;
    eth->dst_addr = dmac;
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    auto *ip = (rte_ipv4_hdr *)(eth + 1);
    ip->version_ihl = 0x45;
    ip->time_to_live = 128;
    ip->next_proto_id = IPPROTO_UDP;
    ip->src_addr = saddr;
    ip->dst_addr = daddr;
    ip->total_length = rte_cpu_to_be_16(frame_size - sizeof(rte_ether_hdr)); // bytes after L2

    auto *udp = (rte_udp_hdr *)(ip + 1);
    udp->src_port = sport;
    udp->dst_port = dport;
    udp->dgram_len = rte_cpu_to_be_16(frame_size - sizeof(rte_ether_hdr) - sizeof(rte_ipv4_hdr));

    // ip->hdr_checksum = opt.hw_csum ? 0 : rte_ipv4_cksum(ip);
    // udp->dgram_cksum = opt.hw_csum ? rte_ipv4_phdr_cksum(ip, e.mbuf0->ol_flags) : rte_ipv4_udptcp_cksum(ip, udp);
  }

  notifyReady();
  taskLoop();
  shutdown();
}

bool DpdkWorker::taskStart(std::shared_ptr<Task> task) {
  this->task = task;
  payload_len = task->opt.pipes * backend.context.device.valuesPerPipe();

  // Compute how much of the input vector we will process, and exit if 0
  chunk = InputChunk::get(tid, backend.opt.threads, task->len, payload_len);
  if (!chunk.packets) return false;

  // Compute packet sizes
  payload_size = payload_len * 4;
  packet_size = header_size + payload_size;
  frame_size = sizeof(rte_ether_hdr) + sizeof(rte_ipv4_hdr) + sizeof(rte_udp_hdr) + packet_size;
  win_size = (chunk.packets < win_capacity) ? chunk.packets : win_capacity;

  active_entries.clear();

  // Pre-compute exit state for each entry we will use
  const auto total_packets = chunk.packets + (task->opt.quantization ? win_size : 0);
  const auto entry_packets = total_packets / win_size;     // win_capacity;
  const auto entry_packets_rem = total_packets % win_size; // win_capacity


  for (auto i = 0; i < win_size; ++i) {

    active_entries.set(i);

    auto &e = win[i];

    auto packets = entry_packets + (i < entry_packets_rem);

    e.ver_first = e.ver;
    e.ver_last = (packets % 2) == 0 ? e.ver : !e.ver;
    e.seq_last[e.ver] = e.seq[e.ver] + ((packets + 1) / 2);
    e.seq_last[!e.ver] = e.seq[!e.ver] + (packets / 2);
    e.firstquant = task->opt.quantization;
    e.fin = false;

    // This is like switchml, we start with a scaled *first* timeout to make sure all packets in the initial burst
    // had a chance to be received. This is changed back to re_timeout the next time we TX this entry: either on a
    // send or at the first timeout. See the two functions below
    e.timeout = re_timeout_scaled;
    e.tsc = rte_get_tsc_cycles();

    // resize all templates to this task's frame_size and patch L3/L4 lengths once (if needed)
    if (e.mbuf0->pkt_len != frame_size) {
      if (e.mbuf0->pkt_len < frame_size) {
        DPA_THROW_IF(!rte_pktmbuf_append(e.mbuf0, frame_size - e.mbuf0->pkt_len), "mbuf0 append failed");
      } else {
        DPA_THROW_IF(rte_pktmbuf_trim(e.mbuf0, e.mbuf0->pkt_len - frame_size) != 0, "mbuf0 trim failed");
      }

      if (e.mbuf0->nb_segs != 1) { printf("t%d: ERROR - mbuf0 has %u segments after resize!\n", tid, e.mbuf0->nb_segs); }
      if (rte_pktmbuf_headroom(e.mbuf0) == 0) { printf("t%d: WARNING - mbuf0 has no headroom left\n", tid); }

      auto *eth = rte_pktmbuf_mtod(e.mbuf0, rte_ether_hdr *);
      auto *ip = (rte_ipv4_hdr *)(eth + 1);
      auto *udp = (rte_udp_hdr *)(ip + 1);
      ip->total_length = rte_cpu_to_be_16(frame_size - sizeof(rte_ether_hdr));
      udp->dgram_len = rte_cpu_to_be_16(frame_size - sizeof(rte_ether_hdr) - sizeof(rte_ipv4_hdr));
      ip->hdr_checksum = opt.hw_csum ? 0 : rte_ipv4_cksum(ip);
      udp->dgram_cksum = opt.hw_csum ? rte_ipv4_phdr_cksum(ip, e.mbuf0->ol_flags) : rte_ipv4_udptcp_cksum(ip, udp);
    }
  }

  return true;
}

bool DpdkWorker::taskFinish() {
  // Info("t{}: taskFinish {} enter", tid, task->name);
  DPA_THROW_IF(!task, "called task_finish without task");
  // if (active_entries.count()) {

  for (auto i = 0; i < win_size; ++i) {
    auto &e = win[i];
    // I think if we exit from a SYN response it is possible that the following check is true
    // e,g on straggle_op. TODO: investigate
    //
    // if (e.seq[e.ver] != e.seq_last[e.ver]) {
    //   DPA_THROW("t{}: Entry {} finishing task {} early: seq[{}]={} but expected seq_last[{}]={}, active_at_exit: {}", tid, i, task->name,
    //             e.ver, e.seq[e.ver], e.ver, e.seq_last[e.ver], (int)active_entries.check(i));
    // }
    win[i].fin = true;
    win[i].firstquant = false;
    win[i].seq[0] = win[i].seq_last[0];
    win[i].seq[1] = win[i].seq_last[1];
    win[i].ver = win[i].ver_last;
    win[i].mbuf_rebuild = false;
    // if (win[i].mbuf_rtx && (win[i].mbuf_rtx != win[i].mbuf0)) { rte_pktmbuf_free(win[i].mbuf_rtx); }
    // win[i].mbuf_rtx = nullptr;
    // rte_pktmbuf_free(e.mbuf_rtx); // null-safe
    // e.mbuf_rtx = nullptr;
  }
#if DPA_DEBUG
  // std::vector<uint32_t> exit_seqnums;
  // std::vector<uint32_t> exit_ver;
  // for (auto i = 0; i < win_capacity; ++i) {
  //   exit_seqnums.push_back(win[i].seq[0]);
  //   exit_seqnums.push_back(win[i].seq[1]);
  // }
  // for (auto i = 0; i < win_capacity; ++i) exit_ver.push_back(win[i].ver);
  // DEBUG("t{}: finished task '{}' with subpool [{}:{}] seq: {}, entry versions: {}", tid, task->name, subpool_base_global,
  //       subpool_base_global + subpool_size, pp::head(exit_seqnums, exit_seqnums.size()), pp::head(exit_ver, exit_ver.size()));
#endif
  // Info("t{}: taskFinish {} exit", tid, task->name);
  active_entries.clear();
  return false;
}

bool DpdkWorker::taskFinish(uint16_t entry) {
  // printf("t%d: Finishing entry %u (offset was %u, fin was %d)\n", tid, entry, win[entry].offset, win[entry].fin);
  // if (!win[entry].fin) {
  if (active_entries.check(entry)) {
    active_entries.clear(entry);

    auto &e = win[entry];

    DPA_THROW_IF(e.seq[0] != e.seq_last[0] or e.seq[1] != e.seq_last[1], "taskFinish.{} seq,seq_last[0]={},{} seq,seq_last[1]={},{}", entry,
                 e.seq[0], e.seq_last[0], e.seq[1], e.seq_last[1])
    e.ver = e.ver_last;
    e.fin = true;
    e.firstquant = false;
    e.mbuf_rebuild = false;
    // if (e.mbuf_rtx && (e.mbuf_rtx != e.mbuf0)) { rte_pktmbuf_free(win[entry].mbuf_rtx); }
    // e.mbuf_rtx = nullptr;
    return true;
  }
  return false;
}

void DpdkWorker::shutdown() {
  DEBUG("t{} shutdown", tid);

  if (tx_buffer) { rte_eth_tx_buffer_flush(opt.eal_port_id, tid, tx_buffer); }

  for (auto w = 0; w < win_capacity; ++w) {
    // backend.context.device.session.pool.seqnums[subpool_base_local + w] = win[w].seq[0];
    // backend.context.device.session.pool.seqnums[subpool_base_local + w + win_capacity] = win[w].seq[1];
    auto idx0 = Slot::get_for_entry(win_capacity, w, 0, subpool_base_local);
    auto idx1 = Slot::get_for_entry(win_capacity, w, 1, subpool_base_local);
    if (idx0 >= backend.context.device.session.pool.seqnums.size() || idx1 >= backend.context.device.session.pool.seqnums.size()) {
      printf("ERROR: Worker %d writing out of bounds! idx0=%u, idx1=%u, size=%zu\n", tid, idx0, idx1,
             backend.context.device.session.pool.seqnums.size());
      abort();
    }
    backend.context.device.session.pool.seqnums[Slot::get_for_entry(win_capacity, w, 0, subpool_base_local)] = win[w].seq[0];
    backend.context.device.session.pool.seqnums[Slot::get_for_entry(win_capacity, w, 1, subpool_base_local)] = win[w].seq[1];
    // if (win[w].mbuf_rtx) {
    //   rte_pktmbuf_free(win[w].mbuf_rtx);
    //   win[w].mbuf_rtx = nullptr;
    // }
    rte_pktmbuf_free(win[w].mbuf0);
    win[w].mbuf0 = nullptr;
  }

  if (rx_mbufs) rte_free(rx_mbufs);
  if (tx_buffer) rte_free(tx_buffer);
  if (win) rte_free(win);
  // if (tx_pool) rte_mempool_free(tx_pool);
  rx_mbufs = nullptr;
  tx_buffer = nullptr;
  tx_pool = nullptr;
  win = nullptr;
}

void DpdkWorker::taskLoop() {
  std::unique_lock<std::mutex> lock(mutex);
  // main loop: wait task > pop > execute > notify backend for completion/failure
  while (true) {
    // DEBUG("worker {} waiting task ...", tid);
    cv.wait(lock, [&] { return !queue.empty() || !running; });
    if (!running) break;
    auto task = queue.front();
    queue.pop();
    lock.unlock();
    // Notify backend we picked up the task
    backend.notify(tid, task, Task::Running);
    // Execute the task
    int ret = backend.context.device.straggle_aware ? loop(task) : loop_ns(task);
    auto status = ret < 0 ? Task::Failed : ret ? Task::Completed : Task::DidNotRun;
    // Notify backend we finished with the task
    backend.notify(tid, task, status);
    if (!opt.async) task->wait();
    lock.lock(); // for the next iteration
  }
}

void DpdkWorker::notifyReady() {
  std::lock_guard<std::mutex> lock(mutexReady);
  ready.store(true, std::memory_order_release);
  cvReady.notify_one();
}

int DpdkWorker::trampoline(void *worker_ptr) {
  static_cast<DpdkWorker *>(worker_ptr)->main();
  return 0;
}

//
//
// Public API below
//
//

void DpdkWorker::stop() {
  std::call_once(fini_flag, [this] {
    running.store(false, std::memory_order_release);
    cv.notify_one();
  });
}

void DpdkWorker::start() {
  std::call_once(init_flag, [this] {
    running.store(true, std::memory_order_release);
    DPA_THROW_IF(rte_eal_remote_launch(&DpdkWorker::trampoline, this, lcore) != 0, "rte_eal_remote_launch failed");
    cv.notify_one();
  });
}

void DpdkWorker::waitReady() {
  std::unique_lock<std::mutex> lock(mutexReady);
  cvReady.wait(lock, [&] { return ready.load(std::memory_order_acquire); });
}

bool DpdkWorker::push(std::shared_ptr<Task> task) {
  std::lock_guard<std::mutex> lock(mutex);
  if (running) {
    queue.push(task);
    cv.notify_one();
    return true;
  }
  return false;
}

void DpdkWorker::join() { rte_eal_wait_lcore(lcore); }

std::string DpdkWorker::pktString(AllReducePacket const &pkt, bool tx, bool hostorder, std::string const &suffix,
                                  std::chrono::steady_clock::time_point const &ts) const {
#define ORDL(x) (hostorder ? x : ntohl(x))
#define ORDS(x) (hostorder ? x : ntohs(x))
  auto slot = Slot(ORDS(pkt.slotid), global_pool_base, tid, win_capacity);
  auto quants = ORDL(pkt.quants);
  auto counts = ORDS(pkt.counts);
  auto flags = fmt::format("{}{}{}{}{}{}.{}", pkt.bad() ? 'b' : '-', pkt.old() ? 'o' : '-', pkt.re() ? 'r' : '-', '-',
                           pkt.syn() ? 's' : '-', pkt.cntk() ? 'k' : '-', pkt.pipes());
  auto offset = pkt.offset;
  if (!hostorder && !tx && f_check(pkt.flags, F_SY)) { offset = ntohl(offset); }

  std::string values;
  auto valuesToShow = std::min<uint16_t>(pkt.payloadLen(backend.context.device), 6);
  for (auto i = 0; i < valuesToShow; ++i) {
    values.append(std::to_string(ORDL(pkt.payload()[i])));
    if (valuesToShow && i < valuesToShow - 1) { values.append(","); }
  }
  if (valuesToShow < pkt.payloadLen(backend.context.device)) values.append("...");

  return fmt::format("{} {} ({},{}) {} b={} w={}/{} sn={} sl={},{} :{},{} o={} q[{}/4]={},{},{},{} v[{}/{}]={} {}", pp::formatTime(ts),
                     tx ? 'T' : 'R', ORDL(pkt.sessid), pkt.operid, flags, fmt::format("{:0{}b}", ORDL(pkt.bitmap), pkt.n), pkt.n, pkt.k,
                     ORDL(pkt.seqnum), slot.g, slot.l, slot.w, slot.w_ver, offset, pkt.getqcount(counts), quant::extract(quants, 3),
                     quant::extract(quants, 2), quant::extract(quants, 1), quant::extract(quants, 0), pkt.getvcount(counts),
                     pkt.payloadLen(backend.context.device), values, suffix);

#undef ORDL
#undef ORDS
  // return "wtf";
}

std::string DpdkWorker::pktString(AllReducePacketNS const &pkt, bool tx, bool hostorder, std::string const &suffix,
                                  std::chrono::steady_clock::time_point const &ts) const {
#define ORDL(x) (hostorder ? x : ntohl(x))
#define ORDS(x) (hostorder ? x : ntohs(x))
  auto slot = Slot(ORDS(pkt.slotid), global_pool_base, tid, win_capacity);
  auto quants = ORDL(pkt.quants);
  auto counts = ORDS(pkt.counts);
  auto flags = fmt::format("{}{}{}{}{}{}.{}", pkt.bad() ? 'b' : '-', pkt.old() ? 'o' : '-', pkt.re() ? 'r' : '-', pkt.ver() ? '1' : '0',
                           pkt.syn() ? 's' : '-', pkt.cntk() ? 'k' : '-', pkt.pipes());
  auto offset = pkt.offset;
  if (!hostorder && !tx && f_check(pkt.flags, F_SY)) { offset = ntohl(offset); }

  std::string values;
  auto valuesToShow = std::min<uint16_t>(pkt.payloadLen(backend.context.device), 6);
  for (auto i = 0; i < valuesToShow; ++i) {
    values.append(std::to_string(ORDL(pkt.payload()[i])));
    if (valuesToShow && i < valuesToShow - 1) { values.append(","); }
  }
  if (valuesToShow < pkt.payloadLen(backend.context.device)) values.append("...");

  return fmt::format("{} {} ({},{}) {} b={} w={} sl={},{} :{},{} o={} q[{}/4]={},{},{},{} v[{}/{}]={} {}", pp::formatTime(ts),
                     tx ? 'T' : 'R', ORDL(pkt.sessid), pkt.operid, flags, fmt::format("{:0{}b}", ORDL(pkt.bitmap), pkt.n), pkt.n, slot.g,
                     slot.l, slot.w, slot.w_ver, offset, pkt.getqcount(counts), quant::extract(quants, 3), quant::extract(quants, 2),
                     quant::extract(quants, 1), quant::extract(quants, 0), pkt.getvcount(counts), pkt.payloadLen(backend.context.device),
                     values, suffix);

#undef ORDL
#undef ORDS
}
