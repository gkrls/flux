#include "dpa/allreduce.h"
#include "dpa/backend_dpdk/backend_dpdk.h"
#include "dpa/backend_dpdk/backend_dpdk_utils.h"
#include "dpa/util/error.h"
#include <rte_byteorder.h>
// #include "dpa/util/config.h"

using namespace dpa;
using namespace dpa::quant;
using namespace dpa::serdes;
using namespace dpa::dpdk;

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#if DPA_DEBUG
#define TRACE_PKT(packet, tx, suffix)                                                                                                      \
  if (backend.opt.debug_trace_packet) TRACE("t{}: {}", tid, pktString(packet, tx, 0, suffix));
#define TRACE_RTX(packet, tx, suffix)                                                                                                      \
  if (backend.opt.debug_trace_packet_rtx) TRACE_PKT(packet, tx, suffix);
#else
#define TRACE_PKT(packet, tx, suffix)
#define TRACE_RTX(packet, tx, suffix)
#endif

// bool DpdkWorker::checkTimeoutsNS(uint64_t now) {
//   bool queued = false;
//   for (uint16_t i = 0; i < win_size; ++i) {
//     auto &e = win[i];
//     if (unlikely(e.fin)) continue;
//     if (likely((now - e.tsc) < e.timeout)) continue;

//     // Rebuild the retransmit template only if content changed (advance or SYN)
//     if (e.mbuf_rebuild || !e.mbuf_rtx) {
//       if (e.mbuf_rtx != e.mbuf0) { rte_pktmbuf_free(e.mbuf_rtx); }
//       e.mbuf_rtx = rte_pktmbuf_copy(e.mbuf0, tx_pool, 0, UINT32_MAX);
//       DPA_THROW_IF(!e.mbuf_rtx, "rtx: copy mbuf0 failed");

//       // Patch ALR header (payload only if non-SYN and needed)
//       auto *p = rte_pktmbuf_mtod_offset(e.mbuf_rtx, AllReducePacketNS *, header_offset);
//       p->slotid = rte_cpu_to_be_16(e.slot[e.ver]);
//       p->offset = e.offset;
//       p->flags = e.flags;
//       p->quants = rte_cpu_to_be_32(e.quants);

//       // if (!f_check(e.flags, F_SY)) {
//       if (task->opt.quantization) {
//         if (e.firstquant) {
//           p->counts = rte_cpu_to_be_16(AllReducePacketNS::getqvcount(task->opt.quantization, 0));
//         } else {
//           p->counts = rte_cpu_to_be_16(AllReducePacketNS::getqvcount(task->opt.quantization, e.vcount));
//           quantize(p->payload(), (float *)task->in + e.offset, e.vcount, e.exponents, task->opt.quantization, p->n, true);
//         }
//       } else {
//         p->counts = rte_cpu_to_be_16(AllReducePacketNS::getqvcount(0, e.vcount));
//         htonlv(p->payload(), (uint32_t *)task->in + e.offset, e.vcount);
//         if constexpr (DPA_DEBUG)
//           if (e.vcount < payload_len) memset(p->payload() + e.vcount, 0, (payload_len - e.vcount) * 4);
//       }

//       checksum(e.mbuf_rtx);
//       e.mbuf_rebuild = false;
//     }

//     // Clone the current retransmit template and enqueue
//     rte_mbuf *clone = rte_pktmbuf_clone(e.mbuf_rtx, tx_pool);
//     DPA_THROW_IF(!clone, "rtx: clone failed");
//     rte_eth_tx_buffer(opt.eal_port_id, tid, tx_buffer, clone);
//     if constexpr (DPA_DEBUG) TRACE_RTX(*rte_pktmbuf_mtod_offset(e.mbuf_rtx, AllReducePacketNS *, header_offset), 1, "(rtx)");
//     e.tsc = now + 30;
//     e.timeout = re_timeout;
//     queued = true;
//   }
//   return queued;
// }

__rte_always_inline static void buildHeader(AllReducePacketNS &p, uint32_t sessid, uint32_t operid, uint32_t bitmap, uint32_t offset,
                                            uint16_t slotid, uint8_t n, uint8_t qcount, uint16_t vcount, uint8_t flags, uint32_t quants) {
  // Only bswap the fields that will be used by the switch
  p.sessid = rte_cpu_to_be_32(sessid);
  p.operid = operid;
  p.bitmap = rte_cpu_to_be_32(bitmap);
  p.offset = offset;
  p.slotid = rte_cpu_to_be_16(slotid);
  p.n = n;
  p.counts = rte_cpu_to_be_16(AllReducePacketNS::getqvcount(qcount, vcount));
  p.flags = flags;
  p.quants = rte_cpu_to_be_32(quants);
}

__rte_always_inline __attribute__((hot)) static void reuseHeader(AllReducePacketNS &p, uint32_t bitmap, uint32_t offset, uint16_t slotid,
                                                                 uint8_t qcount, uint16_t vcount, flags_t flags) {
  p.bitmap = rte_cpu_to_be_32(bitmap);
  p.offset = offset;
  p.slotid = rte_cpu_to_be_16(slotid);
  p.counts = rte_cpu_to_be_16(AllReducePacketNS::getqvcount(qcount, vcount));
  p.flags = flags;
}

enum PacketKindNS { RESULT /* advance */, IGNORE };

__rte_always_inline __attribute__((hot)) static PacketKindNS classifyPacket(AllReducePacketNS const &q, uint32_t q_slotid,
                                                                            DpdkWorker::Entry const &e, uint32_t sessid, uint32_t operid) {
  DPA_THROW_IF(f_check(q.flags, F_BA), "Received F_BAD, need to re-sync with the switch !!!");
  if (unlikely(e.fin)) {
    std::cout << "fin ignore\n";
    return PacketKindNS::IGNORE;
  }
  if (unlikely(rte_be_to_cpu_32(q.sessid) != sessid)) return IGNORE;
  if (unlikely(q.operid != operid)) {
    std::cout << "operid ignore\n";
    return PacketKindNS::IGNORE;
  }
  if (unlikely(q_slotid != e.slot_idx())) {
    std::cout << "slot ignore\n";
    return PacketKindNS::IGNORE;
  }
  if (unlikely(q.offset != e.offset)) {
    std::cout << "offset ignore\n";
    return PacketKindNS::IGNORE;
  }
  return PacketKindNS::RESULT;
}

__rte_always_inline __attribute__((hot)) static uint32_t nextOffsetForEntry(uint32_t curr_offset, uint32_t input_len, uint16_t wnd_len,
                                                                            uint16_t payload_len, uint32_t multiplier,
                                                                            bool first_quant_pending) {
  return std::min<uint32_t>(input_len, curr_offset + (multiplier - first_quant_pending) * wnd_len * payload_len);
}

bool DpdkWorker::checkTimeoutsNS(uint64_t now) {
  int rtx = 0;
  for ( auto i = 0; i < win_size; ++i) {
    auto &e = win[i];
    if (unlikely(e.fin)) continue;
    if (likely((now - e.tsc) < e.timeout)) continue;

    auto *rtx_mbuf = rte_pktmbuf_copy(e.mbuf0, tx_pool, 0, UINT32_MAX);
    auto *p = rte_pktmbuf_mtod_offset(rtx_mbuf, AllReducePacketNS *, header_offset);
    p->slotid = rte_cpu_to_be_16(e.slot[e.ver]);
    p->offset = e.offset;
    p->flags = e.flags;
    p->quants = rte_cpu_to_be_32(e.quants);

    if (!f_check(e.flags, F_SY)) {
      if (task->opt.quantization) {
        if (e.firstquant)
          p->counts = rte_cpu_to_be_16(AllReducePacketNS::getqvcount(task->opt.quantization, 0));
        else {
          p->counts = rte_cpu_to_be_16(AllReducePacketNS::getqvcount(task->opt.quantization, e.vcount));
          quantize(p->payload(), (float *)task->in + e.offset, e.vcount, e.exponents, task->opt.quantization, p->n, true);
        }
      } else {
        p->counts = rte_cpu_to_be_16(AllReducePacketNS::getqvcount(0, e.vcount));
        htonlv(p->payload(), (uint32_t *)task->in + e.offset, e.vcount);
        if constexpr (DPA_DEBUG) memset(p->payload() + e.vcount, 0, (payload_len - e.vcount) * 4);
      }
    }
    checksum(rtx_mbuf);
    TRACE_RTX(*rte_pktmbuf_mtod_offset(rtx_mbuf, AllReducePacketNS *, header_offset), 1, "(rtx)");
    rte_eth_tx_buffer(opt.eal_port_id, tid, tx_buffer, rtx_mbuf);
    e.tsc = rte_get_tsc_cycles(); // now + 30;
    e.timeout = re_timeout;
    ++rtx;
  }
  return rtx;
}




int DpdkWorker::sendInitialBurstNS() {
  DPA_PROFILE_DO( uint64_t const burst_start = rte_get_tsc_cycles());

  const auto sessid = backend.context.device.session.id;
  const auto operid = task->id;
  const auto bitmap = 1u << backend.context.rank;
  const auto flags = task->opt.pipes - 1;

  rte_mbuf *burst[DpdkBackendOptions::MAX_WINDOW];
  uint16_t num = 0;
  uint32_t offset = chunk.lo;

  for (uint16_t i = 0; i < win_size; ++i, offset += payload_len) {
    auto &e = win[i];
    auto *p = rte_pktmbuf_mtod_offset(e.mbuf0, AllReducePacketNS *, header_offset);

    DPA_ASSERT(rte_pktmbuf_pkt_len(e.mbuf0) == frame_size, "mbuf pkt_len mismatch");
    DPA_ASSERT(rte_pktmbuf_data_len(e.mbuf0) == frame_size, "mbuf data_len mismatch");
    if constexpr (DPA_DEBUG) memset(p->payload(), 0, payload_len * sizeof(uint32_t));

    const auto nxt_cnt = (uint16_t)std::min<uint32_t>(payload_len, chunk.hi - offset);
    // const uint16_t nxt_cnt = RTE_MIN(payload_len, chunk.hi - offset);

    e.flags = flags | (e.ver ? F_VER : 0);
    e.offset = offset;
    e.firstquant = task->opt.quantization > 0;
    e.vcount = task->opt.quantization ? 0 : nxt_cnt; // std::min<uint16_t>(payload_len, chunk.hi - offset);
    e.quants = task->opt.quantization ? exponents((float *)task->in + offset, nxt_cnt, task->opt.quantization) : 0u;

    buildHeader(*p, sessid, operid, bitmap, e.offset, e.slot_idx(), backend.context.world, task->opt.quantization, e.vcount, e.flags,
                e.quants);
    htonlv(p->payload(), (uint32_t *)task->in + offset, e.vcount);

    burst[num++] = checksum(e.mbuf0);
    rte_mbuf_refcnt_update(e.mbuf0, 1); // make sure mbuf0 is kept alive
    // e.mbuf_rtx = e.mbuf0;
    // e.mbuf_rebuild = false;
    // e.mbuf_rtx = nullptr;
    // e.mbuf_rebuild = true;
    TRACE_PKT(*p, 1, "(initial)");
  }

  if (opt.drain_queues) {
    DPA_PROFILE_DO(uint64_t const drain_start = rte_get_tsc_cycles());
    tx_buffer_discard(tx_buffer);
    rx_drain_fast(opt.eal_port_id, tid);
    DPA_PROFILE_DO(prof.rec_drain(rte_get_tsc_cycles() - drain_start));
  }

  auto sent = 0;
  for (auto i = 0; i < num; ++i) {
    win[i].tsc = rte_get_tsc_cycles();
    win[i].timeout = 2 * re_timeout; // + (num - i - 1) * 20;
  }
  while (sent < num) sent += rte_eth_tx_burst(opt.eal_port_id, tid, &burst[sent], num - sent);

  DPA_PROFILE_DO(prof.rec_burst(rte_get_tsc_cycles() - burst_start));
  return sent;
}

int DpdkWorker::loop_ns(std::shared_ptr<Task> task) {
  if (!taskStart(task)) {
    DEBUG("Worker {} running 'loop_ns' on task {}, nothing to do...", tid, task->name);
    return 0;
  } else {
    TRACE("Worker {} running 'loop' on task {}", tid, task->name);
    DPA_PROFILE_DO( prof.start(task->id));
  }

  const auto prescaling = task->opt.prescaled ? backend.context.world : 1;
  const auto averaging = task->opt.averaging;
  const auto quantization = task->opt.quantization;
  const auto avg_factor = averaging ? averaging_factor(task->ctx.world, prescaling) : 1.0f;
  const uint64_t rx_interval_us = opt.rx_interval.count();
  const uint64_t tx_interval = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * opt.tx_interval.count();
  const uint32_t sessid = backend.context.device.session.id;
  const uint32_t operid = task->id;
  const uint32_t bitmap = 1u << backend.context.rank;
  const uint16_t world = backend.context.world;

  sendInitialBurstNS();

  uint64_t tx_tsc = rte_get_tsc_cycles(); // last time we flushed the tx_buffer
  uint64_t re_tsc = tx_tsc;               // last time we checked retransmissions -- give the initial burst extra time

  while (likely(!taskFinished())) {
    const uint64_t now = rte_get_tsc_cycles();
    const uint16_t received = rte_eth_rx_burst(opt.eal_port_id, tid, rx_mbufs, opt.rx_burst);

    if (likely(received > 0)) {
      auto *nxt_q = rte_pktmbuf_mtod_offset(rx_mbufs[0], AllReducePacketNS *, header_offset);
      rte_prefetch0(nxt_q);
      Slot nxt_slot(rte_be_to_cpu_16(nxt_q->slotid), global_pool_base, subpool_base_local);
      rte_prefetch0(&win[nxt_slot.w]);

      for (uint16_t i = 0; i < received; ++i) {
        DPA_PROFILE_DO(auto const iter_enter = rte_get_tsc_cycles(); prof.rec_rx());

        const Slot slot = nxt_slot;
        auto &e = win[slot.w];
        auto &q = *nxt_q;
        if (likely(i + 1 < received)) { // prefetch next packet+entry if we received more than 1
          nxt_q = rte_pktmbuf_mtod_offset(rx_mbufs[i + 1], AllReducePacketNS *, header_offset);
          rte_prefetch0(nxt_q);
          nxt_slot = Slot(rte_be_to_cpu_16(nxt_q->slotid), global_pool_base, subpool_base_local);
          rte_prefetch0(&win[nxt_slot.w]);
        }

        const uint16_t q_counts = rte_be_to_cpu_16(q.counts);

        if (const auto kind = classifyPacket(q, slot.g, e, sessid, operid); unlikely(kind != RESULT)) {
          TRACE_PKT(q, 0, "(ignored)");
          rte_pktmbuf_free(rx_mbufs[i]);
          continue;
        }

        TRACE_PKT(q, 0, "(expected result)");

        DPA_PROFILE_DO(prof.rec_rtt(now - e.tsc, e.idx))

        e.seq[e.ver]++;

        if (quantization) {
          if (likely(!e.firstquant))
            dequantize((float *)task->out + q.offset, q.payload(), q.getvcount(q_counts), e.exponents, quantization, q.n, avg_factor);
        } else {
          ntohlv((uint32_t *)task->out + q.offset, q.payload(), AllReducePacketNS::getvcount(q_counts));

          auto n = q.getvcount(q_counts);
          for (auto i = 0; i < n; ++i) {
            if (*(q.payload() + i) != rte_cpu_to_be_32(6))
              DPA_THROW("Expected 6 but found {} in payload. Window {}:{} Slot: {} Offset: {}", rte_be_to_cpu_32(*(q.payload() + i)), e.idx,
                        (int)e.ver, e.slot_idx(), e.offset + i);
          }
          for (auto i = 0; i < n; ++i) {
            if (*((uint32_t *)task->out + q.offset + i) != 6) {
              DPA_THROW("Expected 6 at offset {} but found {}. Window {}:{} Slot: {}", (uint32_t)q.offset,
                        *((uint32_t *)task->out + q.offset + i), e.idx, (int)e.ver, e.slot_idx());
            }
          }
        }

        // swap version in preperation for next use (advance of next task)
        e.ver = !e.ver;
        e.flags = f_toggle(e.flags, F_VER);

        const uint32_t nxt_off = nextOffsetForEntry(e.offset, chunk.hi, win_capacity, payload_len, 1, e.firstquant);

        if (auto nxt_off = nextOffsetForEntry(e.offset, chunk.hi, win_capacity, payload_len, 1, e.firstquant); likely(nxt_off < chunk.hi)) {
          auto nxt_cnt = (uint16_t)std::min<uint32_t>(payload_len, chunk.hi - nxt_off);

          e.timeout = re_timeout;
          e.offset = nxt_off;
          e.vcount = nxt_cnt;
          e.mbuf_rebuild = true; // stored mbuf is stale and should not be RTXed, rebuild it the next time we rtx

#if DPA_DPDK_RX_REUSE
          auto *tx_mbuf = rx_mbufs[i];
          auto *p = reinterpret_cast<AllReducePacketNS *>(reuse(tx_mbuf, frame_size, opt.hw_csum));
#else
          rte_pktmbuf_free(rx_mbufs[i]);
          auto *tx_mbuf = rte_pktmbuf_copy(e.mbuf0, tx_pool, 0, UINT32_MAX); // rte_pktmbuf_clone(rx_mbufs[i], tx_pool);
          auto *p = rte_pktmbuf_mtod_offset(tx_mbuf, AllReducePacketNS *, header_offset);
#endif

          // #if !defined(DPA_DPDK_RX_REUSE) || !DPA_DPDK_RX_REUSE
          //           auto *tx_mbuf = rte_pktmbuf_copy(rx_mbufs[i], tx_pool, 0, UINT32_MAX);
          //           rte_pktmbuf_free(rx_mbufs[i]);
          // #else
          //           auto *tx_mbuf = rx_mbufs[i];
          // #endif
          if (quantization) {
            const auto nxt_nxt_off = nextOffsetForEntry(nxt_off, chunk.hi, win_capacity, payload_len, 1, false); // win_size
            const auto nxt_nxt_cnt = (uint16_t)std::min<uint32_t>(payload_len, chunk.hi - nxt_off);
            //
            e.firstquant = false;
            e.exponents = rte_be_to_cpu_32(q.quants); // stored exponents
            e.quants = exponents((float *)task->in + nxt_nxt_off, nxt_nxt_cnt, task->opt.quantization);

            buildHeader(*p, sessid, operid, bitmap, nxt_off, e.slot_idx(), world, quantization, e.vcount, e.flags, e.quants);
            // rebuildHeader(*p, bitmap, nxt_off, e.slot[e.ver], task->opt.quantization, nxt_cnt, e.flags);
            // p->quants = rte_cpu_to_be_32(e.quants);
            quantize(p->payload(), (float *)task->in + nxt_off, nxt_cnt, e.exponents, task->opt.quantization, q.n, true);
          } else {
            // rebuildHeader(*p, bitmap, nxt_off, e.slot[e.ver], 0, nxt_cnt, e.flags);
            buildHeader(*p, sessid, operid, bitmap, nxt_off, e.slot_idx(), world, 0, e.vcount, e.flags, e.quants);
            htonlv(p->payload(), (uint32_t *)task->in + nxt_off, nxt_cnt);
            // htonlv(np->payload(), (uint32_t *)task->in + nxt_off, nxt_cnt);
          }

          if constexpr (DPA_DEBUG) memset(p->payload() + nxt_cnt, 0, (payload_len - nxt_cnt) * 4);
          TRACE_PKT(*p, 1, "(advance)");

          e.tsc = rte_get_tsc_cycles(); //+ 200;
          rte_eth_tx_buffer(opt.eal_port_id, tid, tx_buffer, checksum(tx_mbuf));

        } else {

          rte_pktmbuf_free(rx_mbufs[i]);
          taskFinish(slot.w);
        }
      }
      continue; // if we received something, don't try to retransmit or flush tx
    }

    // ---- cold path ----
    if constexpr (DPA_DPDK_RE_FIRST) {
      if constexpr (not DPA_DPDK_RE_DISABLE) {
        if (unlikely(now - re_tsc > re_timeout)) {
          checkTimeoutsNS(now);
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
          checkTimeoutsNS(now);
          re_tsc = now;
          continue;
        }
      }
    }

    // #if DPA_DPDK_RE_FIRST
    //     if (unlikely(now - re_tsc > re_timeout)) {
    //       checkTimeoutsNS(now); // tx_tsc = checkTimeoutsNS(now) ? now : tx_tsc;
    //       re_tsc = now;
    //     }
    //     // 1) flush TX occasionally
    //     if (unlikely(now - tx_tsc >= tx_interval)) {
    //       rte_eth_tx_buffer_flush(opt.eal_port_id, tid, tx_buffer);
    //       tx_tsc = now;
    //       continue; // in case we have to poll, don't poll if we just flushed
    //     }
    // #else
    //     if (unlikely(now - tx_tsc >= tx_interval)) {
    //       rte_eth_tx_buffer_flush(opt.eal_port_id, tid, tx_buffer);
    //       tx_tsc = now;
    //     }
    //     if (unlikely(now - re_tsc > re_timeout)) {
    //       checkTimeoutsNS(now);
    //       re_tsc = now;
    //       continue;
    //     }
    // #endif
    // 3) If polling is requested sleep here to avoid sleeping if we have received data
    if (rx_interval_us > 0) rte_delay_us_block(rx_interval_us);
  }

  taskFinish();
  return chunk.hi - chunk.lo;
}