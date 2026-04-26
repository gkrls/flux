#include "dpa/allreduce.h"
#include "dpa/backend_dpdk/backend_dpdk.h"
#include "dpa/backend_dpdk/backend_dpdk_utils.h"
#include "dpa/context.h"
#include "dpa/util/config.h"
#include "dpa/util/error.h"
#include "dpa/util/input.h"
#include "dpa/util/log.h"
#include "dpa/util/prof.h"
#include "dpa/util/quant.h"
#include "fmt/core.h"
#include "fmt/ostream.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <rte_build_config.h>
#include <rte_byteorder.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>

using namespace dpa;
using namespace dpa::quant;
using namespace dpa::serdes;
using namespace dpa::dpdk;

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#if DPA_TRACE
#define TRACE_PKT(packet, tx, suffix)                                                                                                      \
  if (backend.opt.debug_trace_packet) TRACE("t{}: {}", tid, pktString(packet, tx, 0, suffix));
#define TRACE_RTX(packet, tx, suffix)                                                                                                      \
  if (backend.opt.debug_trace_packet_rtx) TRACE_PKT(packet, tx, suffix);
#else
#define TRACE_PKT(packet, tx, suffix)
#define TRACE_RTX(packet, tx, suffix)
#endif

static const bool DPA_PREEMPTIVE = [](auto e) {
  if (!e) return false;
  auto v = std::string_view(e);
  auto b = v == "1" || v == "ON";
  if (b) printf("dpa: DPA_PREEMPTIVE is ON\n");
  else printf("dpa: DPA_PREEMPTIVE is OFF\n");
  // if (b) dpa::Info("DPA_PREEMPTIVE is ON");
  return b;
}(std::getenv("DPA_PREEMPTIVE"));

static const bool DPA_SYN_DISABLE = [](auto e) {
  if (!e) return false;
  auto v = std::string_view(e);
  auto b = v == "1" || v == "ON";
  if (b) printf("dpa: DPA_SYN_DISABLE is ON\n");
  else printf("dpa: DPA_SYN_DISABLE is OFF\n");
  // if (b) dpa::Info("DPA_SYN_DISABLE is ON");
  return b;
}(std::getenv("DPA_SYN_DISABLE"));

__rte_always_inline static void buildHeader(AllReducePacket &p, uint32_t sessid, uint32_t operid, uint32_t seqnum, uint32_t bitmap,
                                            uint32_t offset, uint16_t slotid, uint8_t n, uint8_t k, uint8_t qcount, uint16_t vcount,
                                            uint8_t flags, uint32_t quants) {
  // Only bswap the fields that will be used by the switch
  p.sessid = rte_cpu_to_be_32(sessid);
  p.operid = operid;
  p.seqnum = rte_cpu_to_be_32(seqnum);
  p.bitmap = rte_cpu_to_be_32(bitmap);
  p.offset = offset;
  p.slotid = rte_cpu_to_be_16(slotid);
  p.n = n;
  p.k = k;
  p.counts = rte_cpu_to_be_16(AllReducePacket::getqvcount(qcount, vcount));
  p.flags = flags;
  p.quants = rte_cpu_to_be_32(quants);
}

__rte_always_inline static void buildSyn(AllReducePacket &p, uint32_t seqnum, uint32_t offset, uint32_t slotid, uint16_t qcount,
                                         flags_t flags) {
  p.seqnum = rte_cpu_to_be_32(seqnum);
  p.offset = offset;
  p.slotid = rte_cpu_to_be_16(slotid);
  p.counts = rte_cpu_to_be_16(AllReducePacket::getqvcount(qcount, 0));
  p.flags = flags;
  p.quants = 0;
}

__rte_always_inline __attribute__((hot)) static void rebuildHeader(AllReducePacket &p, uint32_t seqnum, uint32_t bitmap, uint32_t offset,
                                                                   uint32_t slotid, uint8_t k, uint8_t qcount, uint16_t vcount,
                                                                   flags_t flags) {
  p.seqnum = rte_cpu_to_be_32(seqnum);
  p.bitmap = rte_cpu_to_be_32(bitmap);
  p.offset = offset;
  p.slotid = rte_cpu_to_be_16(slotid);
  p.k = k;
  p.counts = rte_cpu_to_be_16(AllReducePacket::getqvcount(qcount, vcount));
  p.flags = flags;
}

__attribute__((hot)) __rte_always_inline static uint32_t nextOffsetEntry(uint32_t curr_offset, uint32_t input_len, uint16_t window_len,
                                                                         uint16_t payload_len, uint32_t multi, bool first_quant_pending) {
  multi = std::max<uint32_t>(multi, 1);
  uint64_t step =
      (static_cast<uint64_t>(multi) - first_quant_pending) * static_cast<uint64_t>(window_len) * static_cast<uint64_t>(payload_len);
  uint64_t next = static_cast<uint64_t>(curr_offset) + step;
  return static_cast<uint32_t>(std::min<uint64_t>(input_len, next));
}

__attribute__((hot)) __rte_always_inline static uint32_t nextOffsetSlot(uint32_t curr_offset, uint32_t input_len, uint16_t window_len,
                                                                        uint16_t payload_len, uint32_t multi, bool first_quant_pending) {
  multi = std::max<uint32_t>(multi, 1);
  uint64_t step =
      (static_cast<uint64_t>(multi) * 2 - first_quant_pending) * static_cast<uint64_t>(window_len) * static_cast<uint64_t>(payload_len);
  uint64_t next = static_cast<uint64_t>(curr_offset) + step;
  return static_cast<uint32_t>(std::min<uint64_t>(input_len, next));
}

// __attribute__((hot)) __rte_always_inline static uint32_t nextOffsetEntry(uint32_t curr_offset, uint32_t input_len, uint16_t window_len,
//                                                                          uint16_t payload_len, uint32_t multi, bool first_quant_pending)
//                                                                          {
//   return std::min<uint32_t>(input_len, curr_offset + (multi - first_quant_pending) * window_len * payload_len);
// }

// __attribute__((hot)) __rte_always_inline static uint32_t nextOffsetSlot(uint32_t curr_offset, uint32_t input_len, uint16_t window_len,
//                                                                         uint16_t payload_len, uint32_t multi, bool first_quant_pending) {
//   return std::min<uint32_t>(input_len, curr_offset + (multi * 2 - first_quant_pending) * window_len * payload_len);
// }

__attribute__((hot)) __rte_always_inline static uint32_t getOffsetForSlot(uint32_t seqnum, uint32_t starting_seqnum,
                                                                          uint32_t starting_offset, uint32_t input_len, uint16_t window_len,
                                                                          uint16_t payload_len, bool quantization) {
  auto diff = starting_seqnum - seqnum;
  auto stride = window_len * payload_len;
  auto offset = starting_offset + (2 * diff - quantization) + stride;
  return std::min<uint32_t>(input_len, offset);
}

int DpdkWorker::checkTimeouts(uint64_t now) {
  int rtx = 0;
  for (auto i = 0; i < win_size; ++i) {
    auto &e = win[i];
    if (unlikely(e.fin)) continue;
    if (likely((now - e.tsc) < e.timeout)) continue;

    auto *rtx_mbuf = rte_pktmbuf_copy(e.mbuf0, tx_pool, 0, UINT32_MAX);
    auto *p = rte_pktmbuf_mtod_offset(rtx_mbuf, AllReducePacket *, header_offset);
    p->slotid = rte_cpu_to_be_16(e.slot[e.ver]);
    p->seqnum = rte_cpu_to_be_32(e.seq[e.ver]);
    p->offset = e.offset;
    p->flags = e.flags;
    p->quants = rte_cpu_to_be_32(e.quants);

    if (!f_check(e.flags, F_SY)) {
      if (task->opt.quantization) {
        if (e.firstquant) p->counts = rte_cpu_to_be_16(AllReducePacket::getqvcount(task->opt.quantization, 0));
        else {
          p->counts = rte_cpu_to_be_16(AllReducePacket::getqvcount(task->opt.quantization, e.vcount));
          quantize(p->payload(), (float *)task->in + e.offset, e.vcount, e.exponents, task->opt.quantization, p->n, true);
        }
      } else {
        p->counts = rte_cpu_to_be_16(AllReducePacket::getqvcount(0, e.vcount));
        htonlv(p->payload(), (uint32_t *)task->in + e.offset, e.vcount);
        if constexpr (DPA_DEBUG) memset(p->payload() + e.vcount, 0, (payload_len - e.vcount) * 4);
      }
    }
    checksum(rtx_mbuf);
    TRACE_RTX(*rte_pktmbuf_mtod_offset(rtx_mbuf, AllReducePacket *, header_offset), 1, "(rtx)");
    rte_eth_tx_buffer(opt.eal_port_id, tid, tx_buffer, rtx_mbuf);
    e.tsc = rte_get_tsc_cycles(); // now + 30;
    e.timeout = re_timeout;
    ++rtx;
  }
  return rtx;
}

int DpdkWorker::sendInitialBurst() {
  DPA_PROFILE_DO(uint64_t const burst_start = rte_get_tsc_cycles());

  const auto sessid = backend.context.device.session.id;
  const auto operid = task->id;
  const auto bitmap = 1u << backend.context.rank;
  // const auto flags = task->opt.pipes - 1;
  // task->opt.sa_preemptive ?
  const auto flags = DPA_PREEMPTIVE ? f_set(task->opt.pipes - 1, F_CK) : task->opt.pipes - 1;

  rte_mbuf *burst[DpdkBackendOptions::MAX_WINDOW];
  uint16_t num = 0;
  uint32_t offset = chunk.lo;

  for (uint16_t i = 0; i < win_size; ++i, offset += payload_len) {
    auto &e = win[i];
    auto *p = rte_pktmbuf_mtod_offset(e.mbuf0, AllReducePacket *, header_offset);

    DPA_ASSERT(rte_pktmbuf_pkt_len(e.mbuf0) == frame_size, "mbuf pkt_len mismatch");
    DPA_ASSERT(rte_pktmbuf_data_len(e.mbuf0) == frame_size, "mbuf data_len mismatch");
    if constexpr (DPA_DEBUG) memset(p->payload(), 0, payload_len * sizeof(uint32_t));

    const auto nxt_cnt = (uint16_t)std::min<uint32_t>(payload_len, chunk.hi - offset);
    // const uint16_t nxt_cnt = RTE_MIN(payload_len, chunk.hi - offset);

    e.flags = flags;
    e.offset = offset;
    e.firstquant = task->opt.quantization > 0;
    e.vcount = task->opt.quantization ? 0 : nxt_cnt; // std::min<uint16_t>(payload_len, chunk.hi - offset);
    e.quants = task->opt.quantization ? exponents((float *)task->in + offset, nxt_cnt, task->opt.quantization) : 0u;

    buildHeader(*p, sessid, operid, e.slot_seq(), bitmap, e.offset, e.slot_idx(), backend.context.world, task->opt.sa_world,
                task->opt.quantization, e.vcount, e.flags, e.quants);
    htonlv(p->payload(), (uint32_t *)task->in + offset, e.vcount);

    burst[num++] = checksum(e.mbuf0);
    rte_mbuf_refcnt_update(e.mbuf0, 1); // make sure mbuf0 is kept alive
    TRACE_PKT(*p, 1, "(initial)");
  }

  if (opt.drain_queues) {
    DPA_PROFILE_DO(uint64_t const drain_start = rte_get_tsc_cycles());

    tx_buffer_discard(tx_buffer);
    rx_drain_fast(opt.eal_port_id, tid);

    DPA_PROFILE_DO(prof.rec_drain(rte_get_tsc_cycles() - drain_start));
  }

  auto sent = 0;
  while (sent < num) sent += rte_eth_tx_burst(opt.eal_port_id, tid, &burst[sent], num - sent);
  auto tsc = rte_get_tsc_cycles();
  for (auto i = 0; i < num; ++i) {
    win[i].tsc = tsc;
    win[i].timeout = re_timeout_scaled; // 2 * re_timeout; // + (num - i - 1) * 20;
  }

  DPA_PROFILE_DO(prof.rec_burst(rte_get_tsc_cycles() - burst_start));
  return sent;
}

__rte_always_inline void DpdkWorker::fastestk(const AllReducePacket &q, uint32_t bitmap, uint32_t world, DpdkWorker::Entry &e) {
  // If we do not use fastest-k then we wait STO for each slot use. This
  // if constexpr (not DPA_FASTESTK) return;

  // If we have preemptive fastest-k we don't toggle F_CK. It is always on.
  // if (task->opt.sa_preemptive) return;
  if (DPA_PREEMPTIVE) return;

  if (unlikely(q.k < q.n) and (rte_be_to_cpu_32(q.bitmap) & bitmap)) {
    // #if DPA_FASTESTK_BULK
    // #pragma message("DPA_FASTESTK_BULK Enabled")
    //     TRACE_IF(not f_check(e.flags, F_CK), "t{}: exiting fastestk bulk", tid);
    //     for (auto i = 0; i < win_size; ++i) win[i].flags = f_set(win[i].flags, F_CK);
    // #else
    // #pragma message("DPA_FASTESTK_BULK Disabled")
    e.flags = f_set(e.flags, F_CK);
    TRACE_IF(not f_check(e.flags, F_CK), "t{}: entering fastestk.{}", tid, e.idx);
    // #endif
  } else {
#if DPA_FASTESTK_EXIT
#pragma message("DPA_FASTESTK_EXIT Enabled")
    if (f_check(e.flags, F_CK) and (q.k == world)) {
      // Info("EXITING FASTESTK: {}", pktString(q, 0, 0));
      TRACE_IF(f_check(e.flags, F_CK), "t{}: exiting fastestk.{}", tid, e_idx);
      e.flags = f_clear(e.flags, F_CK); // Disable
    }
#else
#pragma message("DPA_FASTESTK_EXIT Disabled")
#endif
  }
}

__rte_always_inline void DpdkWorker::enterSynBulk(const AllReducePacket &q, DpdkWorker::Entry &e) {
  if (f_check(e.flags, F_SY)) return;

  rte_mbuf *mbufs[DpdkBackendOptions::MAX_WINDOW];
  auto cnt = 0;
  for (auto i = 0; i < win_size; ++i) {
    if (auto &e = win[i]; !e.fin) {
      
      
      // We only clear the F_SY if we are in non-preemptive mode
      auto base = f_set(f_pipes(e.flags), F_SY);
      e.flags = DPA_PREEMPTIVE ? f_set(base, F_CK) : base;
      // e.flags = f_set(f_pipes(e.flags), F_SY);

      auto *mbuf = rte_pktmbuf_copy(e.mbuf0, tx_pool, 0, UINT32_MAX);
      auto *p = rte_pktmbuf_mtod_offset(mbuf, AllReducePacket *, header_offset);
      buildSyn(*p, e.seq[e.ver], e.offset, e.slot[e.ver], e.firstquant ? 0 : task->opt.quantization, e.flags);
      if constexpr (DPA_DEBUG) memset(p->payload(), 0, payload_len * sizeof(uint32_t));
      mbufs[cnt++] = checksum(mbuf);
    }
  }

  TRACE_PKT(q, 0, cnt ? fmt::format("(straggler, entering SYN bulk.{} ({} skipped))", cnt, win_size - cnt) : "(straggler, SYN skipped)");

  if (opt.drain_queues) {
    tx_buffer_discard(tx_buffer);
    rx_drain_fast(opt.eal_port_id, tid);
  }

  auto now = rte_get_tsc_cycles();
  for (auto i = 0, j = 0; i < win_size and j < cnt; ++i) {
    if (auto &e = win[i]; !e.fin) {
      e.tsc = rte_get_tsc_cycles(); // now + (i * 20);
      e.timeout = re_timeout_scaled;
      auto *p = rte_pktmbuf_mtod_offset(mbufs[j], AllReducePacket *, header_offset);
      TRACE_PKT(*p, 1, "(syn)")
      ++j;
    }
  }

  size_t sent = 0;
  while (sent < cnt) sent += rte_eth_tx_burst(opt.eal_port_id, tid, &mbufs[sent], cnt - sent);
}

__rte_always_inline void DpdkWorker::enterSyn(const AllReducePacket &q, DpdkWorker::Entry &e) {
  // if (e.fin) return;
  // if (e.fin or f_check(e.flags, F_SY)) return;
  if (f_check(e.flags, F_SY)) return;

  // We only clear the F_SY if we are in non-preemptive mode
  auto base = f_set(f_pipes(e.flags), F_SY);
  e.flags = DPA_PREEMPTIVE ? f_set(base, F_CK) : base;
  // e.flags = f_set(f_pipes(e.flags), F_SY);

  auto *mbuf = rte_pktmbuf_copy(e.mbuf0, tx_pool, 0, UINT32_MAX);
  auto *p = rte_pktmbuf_mtod_offset(mbuf, AllReducePacket *, header_offset);

  buildSyn(*p, e.seq[e.ver], e.offset, e.slot[e.ver], e.firstquant ? 0 : task->opt.quantization, e.flags);

  if constexpr (DPA_DEBUG) memset(p->payload(), 0, payload_len * sizeof(uint32_t));
  TRACE_PKT(*p, 1, "(syn)");
  e.tsc = rte_get_tsc_cycles();
  e.timeout = re_timeout; // re_timeout_scaled;
  rte_eth_tx_buffer(opt.eal_port_id, tid, tx_buffer, checksum(mbuf));
}

__rte_always_inline void DpdkWorker::skipForward(int64_t diff, Entry &e) {
  e.offset = nextOffsetSlot(e.offset, chunk.hi, win_capacity, payload_len, diff, e.firstquant);

  if (e.offset >= chunk.hi) {
    e.seq[0] = e.seq_last[0];
    e.seq[1] = e.seq_last[1];
    e.ver = e.ver_last;
  } else {
    uint32_t target = e.seq[e.ver] + diff;
    e.seq[0] = target + 1;
    e.seq[1] = target + 1 - (1 - e.ver);
    e.ver = !e.ver;
  }

  e.flags = f_clear(e.flags, F_SY);
  e.firstquant = false;
  e.mbuf_rebuild = true;
}

__rte_always_inline bool DpdkWorker::exitSyn(const AllReducePacket &q, uint32_t q_seqnum, uint32_t q_offset, DpdkWorker::Entry &e) {
  if (q_seqnum != e.seq[e.ver]) {
    DPA_THROW("WTF: seqnum missmatch in exitSyn\n----{}\n----q_seqnum+q_offset: {} \nentry: ver: {}, slot: {}, seq: {}/{}, syn: {}",
              pktString(q, 0, 0), q_seqnum + q_offset, e.ver, e.slot[e.ver], e.seq[0], e.seq[1], (bool)f_check(e.flags, F_SY))
  }

  int64_t diff = static_cast<int64_t>(q_offset) - static_cast<int64_t>(q_seqnum);
  if (diff < 0) DPA_THROW("WTF: received negative diff in SYN: {}", pktString(q, 0, 0));
  if (diff == 0) DPA_THROW("WTF: received no diff in SYN: {}", pktString(q, 0, 0));
  if (diff >= (int64_t)10000000) DPA_THROW("WTF: received super high diff {}: ", diff, pktString(q, 0, 0));

  skipForward(diff, e);
  
  // RESTORE IF NEEDED
  // e.offset = nextOffsetSlot(e.offset, chunk.hi, win_capacity, payload_len, diff, e.firstquant);

  // // TODO: If we do NOT use quantization, or if we use a fixed scaling factor, we CAN use the payload

  // // Set e.seq to the _next_ seqnum.
  // if (e.offset >= chunk.hi) {
  //   // Info("Exiting syn last {} : {}", e.idx, q_offset);
  //   // Info("Exiting syn last {} : {}", e.idx, pktString(q, 0, 0));
  //   e.seq[0] = e.seq_last[0];
  //   e.seq[1] = e.seq_last[1];
  //   e.ver = e.ver_last;
  // } else {
  //   // Info("Exiting syn norm {} : {}", e.idx, q_offset);
  //   // Info("Exiting syn norm {} : {}", e.idx, pktString(q, 0, 0));
  //   e.seq[0] = /* q_seqnum  + */ q_offset + 1;
  //   e.seq[1] = /* q_seqnum  + */ q_offset + 1 - (1 - e.ver);
  //   e.ver = !e.ver;
  // }

  // e.flags = f_clear(e.flags, F_SY);
  // e.firstquant = false;
  // e.mbuf_rebuild = true; // next RTX must rebuild a *normal* packet
  return false;
}

enum PacketKind { RESULT /* advance */, IGNORE /* old etc */, STRAGGLE /* straggle in-task */, STRAGGLE_OP /* straggle cross-task */, SYN };

__attribute__((hot)) __rte_always_inline static PacketKind classify(const AllReducePacket &q, uint32_t q_seqnum, uint32_t q_offset,
                                                                    uint32_t q_slotid, const DpdkWorker::Entry &e, uint32_t sessid,
                                                                    uint32_t operid, DpdkWorker *worker) {
  // Info("    -----classify-----");
  if (unlikely(f_check(q.flags, F_BA))) {
    DPA_THROW("\n-----------------------------------------------------"
              "\nF_BAD received, need re-sync!"
              "\n-----------------------------------------------------"
              "\n  packet: {}"
              "\n   entry: {}",
              worker->pktString(q, 0, 0), // fmt::format("{:b}", q.flags)
              fmt::format("e.seq[0]={}, e.seq[1]={}, e.ver={} e.offset={}, e.flags={:b}", e.seq[0], e.seq[1], e.ver, e.offset, e.flags));
  }

  if (unlikely(rte_be_to_cpu_32(q.sessid) != sessid)) return IGNORE;

  if (unlikely(e.fin)) return IGNORE;
  if (unlikely(q.operid < operid)) return IGNORE;
  if (unlikely(q.operid > operid)) return STRAGGLE_OP;

  const uint16_t e_slotid = e.slot[e.ver];
  const uint32_t e_seqnum = e.seq[e.ver];
  const uint32_t e_offset = e.offset;

  if (likely(q_slotid == e_slotid)) {
    // SYN comparisons use q_offset (seq delta) that is in network order
    // if (unlikely(q_syn)) return ((q_seqnum + q_offset) <= e_seqnum) ? IGNORE : SYN;
    if (unlikely(q_seqnum < e_seqnum)) return IGNORE;
    if (unlikely(q_seqnum > e_seqnum)) return STRAGGLE; // if (unlikely(q_seqnum > e_seqnum) || f_check(q.flags, F_OL)) return STRAGGLE;

    const bool q_syn = f_check(q.flags, F_SY);
    const bool q_old = f_check(q.flags, F_OL);
    const bool e_syn = f_check(e.flags, F_SY);

    // TODO: If q_old and seqnum >= large e.seq_last do not enter SYN just exit the task
    if (unlikely(q_old)) { // return STRAGGLE;
      // q_offset holds the current (at the time the switch responded) seqnum for e_slotid
      if (q_offset == e_seqnum) DPA_THROW("WTF: received F_OL q_seqnum:{} q_offset:{} e_seqnum:{}", q_seqnum, q_offset, e_seqnum);
      if (q_offset < e_seqnum) return IGNORE; // this is an old F_OL, does not help us

      // if (q_offset <= e_seqnum) return IGNORE;
      // Explanation:
      //   e.seq_last[e.ver] is the next usable seq for e.slot[e.ver]
      //   if others are already there, then they cannot be at e.seq_last[!e.ver] - 1. They are 100% at the next op
      if (q_offset >= e.seq_last[e.ver]) return STRAGGLE_OP;

        // TODO: If DPA_IMPLICIT_SYN we could use the following here:
        //  if (q_syn and q_offset > e.seq_last[e.ver] - 1) return STRAGGLE_OP;
        // if (q_syn and q_offset > e.seq[e.ver)
#if DPA_IMPLICIT_SYN
      return q_syn ? SYN : STRAGGLE; // F_OL + F_SY
#else
      return STRAGGLE;
#endif
    }
    if (unlikely(q_syn != e_syn)) { return IGNORE; }
    if (unlikely(q_syn)) { // F_SY only
      if (q_seqnum == q_offset) {
        DPA_THROW("RECEIVED NO DIFF SYN: {}", worker->pktString(q, 0, 0));
        return IGNORE;
      }
      return SYN;
      // return ((q_seqnum + q_offset) < e_seqnum) ? IGNORE : SYN;
    }

    // if (unlikely(q_syn)) return ((q_seqnum + q_offset) <= e_seqnum) ? IGNORE : SYN;
    if (unlikely(q.offset != e_offset)) {
      DPA_THROW("\n------------------------------------------------------------"
                "\noffset mismatch: q.offset={} q_offset={} e.offset={}, e.slot={} e.seq={}"
                "\n------------------------------------------------------------\n  t{}: {}\n ",
                (uint32_t)q.offset, q_offset, e.offset, e_slotid, e_seqnum, worker->getid(), worker->pktString(q, 0, 0, ""));
    }

  } else {
    return IGNORE;
    // if (unlikely(f_check(q.flags, F_SY))) return IGNORE; // ignore SYN for the other slot of the entry
    // // non-SYN comparisons use q.offset that is in host order since it was not touched by the switch
    // if (e.firstquant || (q_seqnum > e.seq[!e.ver]))
    //   return STRAGGLE;
    // return IGNORE;
    // if (unlikely(q.offset < e_offset)) return IGNORE;
    // if (unlikely(q.offset > e_offset)) return STRAGGLE;
    // return e.firstquant ? STRAGGLE : IGNORE;
  }
  return RESULT;
}

// const auto default_avg_factor = averaging ? averaging_factor(task->ctx.world, prescaling) : 1.0f;
int DpdkWorker::loop(std::shared_ptr<Task> task) {
  if (!taskStart(task)) {
    TRACE("Worker {} running 'loop' on task {}, nothing to do...", tid, task->name);
    return 0;
  } else {
    TRACE("Worker {} running 'loop' on task {}", tid, task->name);
    DPA_PROFILE_DO(prof.start(task->id));
  }

  // if constexpr (DPA_PROFILE) prof.rec_drain_start(rte_get_tsc_cycles());
  // if (opt.drain_queues) {
  //   DPA_PROFILE_DO(prof.rec_drain_start(rte_get_tsc_cycles()));
  //   tx_buffer_discard(tx_buffer);
  //   rx_drain_fast(opt.eal_port_id, tid);
  //   DPA_PROFILE_DO(prof.rec_drain_end(rte_get_tsc_cycles()));
  // }
  // if constexpr (DPA_PROFILE) prof.rec_drain_end(rte_get_tsc_cycles());

  const auto prescaling = task->opt.prescaled ? backend.context.world : 1;
  const auto averaging = task->opt.averaging;
  const auto quantization = task->opt.quantization;

  const uint64_t rx_interval_us = opt.rx_interval.count();
  const uint64_t tx_interval = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * opt.tx_interval.count();
  const uint32_t sessid = backend.context.device.session.id;
  const uint32_t operid = task->id;
  const uint32_t bitmap = 1u << backend.context.rank;
  const uint16_t world = backend.context.world;
  const uint16_t world_k = task->opt.sa_world;

  sendInitialBurst();

  uint64_t tx_tsc = rte_get_tsc_cycles(); // last time we flushed the tx_buffer
  uint64_t re_tsc = tx_tsc;               // last time we checked retransmissions -- give the initial burst extra time

  while (likely(!taskFinished())) {
    const uint64_t now = rte_get_tsc_cycles();
    const uint16_t received = rte_eth_rx_burst(opt.eal_port_id, tid, rx_mbufs, opt.rx_burst);

    if (likely(received > 0)) { // hot path
      auto *nxt_q = rte_pktmbuf_mtod_offset(rx_mbufs[0], AllReducePacket *, header_offset);
      rte_prefetch0(nxt_q); // is this prefetch needed?
      Slot nxt_slot(rte_be_to_cpu_16(nxt_q->slotid), global_pool_base, subpool_base_local);
      rte_prefetch0(&win[nxt_slot.w]);

      for (uint16_t i = 0; i < received; ++i) {
        DPA_PROFILE_DO(auto const iter_enter = rte_get_tsc_cycles(); prof.rec_rx());

        const Slot slot = nxt_slot;
        auto &e = win[slot.w];
        auto &q = *nxt_q;
        if (likely((i + 1) < received)) {
          nxt_q = rte_pktmbuf_mtod_offset(rx_mbufs[i + 1], AllReducePacket *, header_offset);
          rte_prefetch2(nxt_q);
          nxt_slot = Slot(rte_be_to_cpu_16(nxt_q->slotid), global_pool_base, subpool_base_local);
          rte_prefetch2(&win[nxt_slot.w]);
        }

        const uint32_t q_seqnum = rte_be_to_cpu_32(q.seqnum);
        const uint32_t q_offset = rte_be_to_cpu_32(q.offset);
        const uint16_t q_counts = rte_be_to_cpu_16(q.counts);

        if (const auto kind = classify(q, q_seqnum, q_offset, slot.g, e, sessid, operid, this); unlikely(kind != RESULT)) {
          switch (kind) {
          default: DPA_THROW("UNREACHABLE - switch packet kind default");
          case SYN: exitSyn(q, q_seqnum, q_offset, e); break;
          case IGNORE: rte_pktmbuf_free(rx_mbufs[i]); continue;
          case STRAGGLE_OP: rte_pktmbuf_free_bulk(&rx_mbufs[i], received - i); goto EXIT;
          case STRAGGLE:
            if (DPA_SYN_DISABLE) {
              if (f_check(q.flags, F_OL) && f_check(q.flags, F_SY)) {
                skipForward((int64_t)q_offset - (int64_t)q_seqnum, e);
                break;
              } else {
                rte_pktmbuf_free(rx_mbufs[i]);
                continue;
              }
              // else
              //   skipForward((int64_t)q_seqnum - (int64_t)e.seq[e.ver], e);
              // rte_pktmbuf_free(rx_mbufs[i]);
              // break;
            } else {
              enterSyn(q, e);
              rte_pktmbuf_free(rx_mbufs[i]);
              // #endif
              continue; // wait for SYN acks or timeouts
            }
          }
        } else { // normal result packet (not SYN) -- hot path

          TRACE_PKT(q, 0, "(expected result)");

          DPA_PROFILE_DO(prof.rec_rtt(rte_get_tsc_cycles() - e.tsc, e.idx, q.n != q.k))

          fastestk(q, bitmap, world, e);

          if (quantization) {
            if (likely(!e.firstquant)) {
              auto af = averaging ? averaging_factor(q.k, prescaling) : 1.0f;
              dequantize((float *)task->out + q.offset, q.payload(), q.getvcount(q_counts), e.exponents, quantization, q.n, af);
            }
          } else {
            ntohlv((uint32_t *)task->out + q.offset, q.payload(), AllReducePacket::getvcount(q_counts));
          }

          // increment seqnum and swap version in preperation for next use (advance or next task)
          e.seq[e.ver]++;
          e.ver = !e.ver;
        }

        if (auto nxt_off = nextOffsetEntry(e.offset, chunk.hi, win_capacity, payload_len, 1, e.firstquant); likely(nxt_off < chunk.hi)) {
          auto nxt_cnt = (uint16_t)std::min<uint32_t>(payload_len, chunk.hi - nxt_off);

          e.timeout = re_timeout;
          e.offset = nxt_off;
          e.vcount = nxt_cnt;
          e.mbuf_rebuild = true;

#if DPA_DPDK_RX_REUSE
          auto *tx_mbuf = rx_mbufs[i];
          auto *p = static_cast<AllReducePacket *>(reuse(tx_mbuf, frame_size, opt.hw_csum));
#else
          rte_pktmbuf_free(rx_mbufs[i]);
          auto *tx_mbuf = rte_pktmbuf_copy(e.mbuf0, tx_pool, 0, UINT32_MAX); // rte_pktmbuf_clone(rx_mbufs[i], tx_pool);
          auto *p = rte_pktmbuf_mtod_offset(tx_mbuf, AllReducePacket *, header_offset);
#endif

          // Build packet header based on quantization
          if (quantization) {
            const auto nxt_nxt_off = nextOffsetEntry(nxt_off, chunk.hi, win_capacity, payload_len, 1, false);
            const auto nxt_nxt_cnt = (uint16_t)std::min<uint32_t>(payload_len, chunk.hi - nxt_nxt_off); // <- this was nxt_off !!

            e.firstquant = false;
            e.exponents = rte_be_to_cpu_32(q.quants);
            e.quants = exponents((float *)task->in + nxt_nxt_off, nxt_nxt_cnt, task->opt.quantization);

            buildHeader(*p, sessid, operid, e.slot_seq(), bitmap, nxt_off, e.slot_idx(), world, world_k, quantization, e.vcount, e.flags,
                        e.quants);
            quantize(p->payload(), (float *)task->in + nxt_off, nxt_cnt, e.exponents, task->opt.quantization, q.n, true);
          } else {
            buildHeader(*p, sessid, operid, e.slot_seq(), bitmap, nxt_off, e.slot_idx(), world, world_k, 0, e.vcount, e.flags, e.quants);
            htonlv(p->payload(), (uint32_t *)task->in + nxt_off, nxt_cnt);
          }

          DPA_DEBUG_DO(memset(p->payload() + nxt_cnt, 0, (payload_len - nxt_cnt) * 4));

          if constexpr (DPA_DEBUG) memset(p->payload() + nxt_cnt, 0, (payload_len - nxt_cnt) * 4);
          TRACE_PKT(*p, 1, "(advance)" /* fmt::format("(advance) e.offset={}", e.offset) */);

          e.tsc = rte_get_tsc_cycles(); //+ 200;
          rte_eth_tx_buffer(opt.eal_port_id, tid, tx_buffer, checksum(tx_mbuf));

          DPA_PROFILE_DO(prof.rec_iter(rte_get_tsc_cycles() - iter_enter))
        } else {

          rte_pktmbuf_free(rx_mbufs[i]);
          taskFinish(slot.w);
        }
      }
      continue;
    }

    // ---- cold path ----
    if constexpr (DPA_DPDK_RE_FIRST) {
      if constexpr (not DPA_DPDK_RE_DISABLE) {
        if (unlikely(now - re_tsc > re_timeout)) {
          checkTimeouts(now);
          re_tsc = now;
        }
      }
      if (unlikely(now - tx_tsc >= tx_interval)) {
        rte_eth_tx_buffer_flush(opt.eal_port_id, tid, tx_buffer);
        tx_tsc = now;
        continue; // skip polling
      }
    } else { // This seems to be faster (and also what switchml does)
      if (unlikely(now - tx_tsc >= tx_interval)) {
        rte_eth_tx_buffer_flush(opt.eal_port_id, tid, tx_buffer);
        tx_tsc = now;
      }
      if constexpr (not DPA_DPDK_RE_DISABLE) {
        if (unlikely(now - re_tsc > re_timeout)) {
          checkTimeouts(now);
          re_tsc = now;
          continue;
        }
      }
    }

    // #if DPA_DPDK_RE_FIRST
    // #if !DPA_DPDK_RE_DISABLE
    //     if (unlikely(now - re_tsc > re_timeout)) {
    //       checkTimeouts(now);
    //       re_tsc = now;
    //     }
    // #endif
    //     if (unlikely(now - tx_tsc >= tx_interval)) {
    //       rte_eth_tx_buffer_flush(opt.eal_port_id, tid, tx_buffer);
    //       tx_tsc = now;
    //       continue;
    //     }
    // #else // This seems to be faster (and also what switchml does)
    //     if (unlikely(now - tx_tsc >= tx_interval)) {
    //       rte_eth_tx_buffer_flush(opt.eal_port_id, tid, tx_buffer);
    //       tx_tsc = now;
    //     }
    // #if !DPA_DPDK_RE_DISABLE
    //     if (unlikely(now - re_tsc > re_timeout)) {
    //       checkTimeouts(now);
    //       re_tsc = now;
    //       continue;
    //     }
    // #endif
    // #endif

    // predictable branch
    if (rx_interval_us > 0) rte_delay_us_block(rx_interval_us);
  }

EXIT:
  taskFinish();
  return chunk.hi - chunk.lo;
  ;
}