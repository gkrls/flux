#ifndef DPA_UTIL_PERF_H
#define DPA_UTIL_PERF_H

#include "dpa/backend.h"
#include "dpa/util/config.h"
#include "dpa/util/log.h"
#include "dpa/util/pp.h"
#include "fmt/ostream.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

#include <rte_cycles.h>

#ifdef DPA_PROFILE
#define DPA_PROFILE_DO(code) code;
#else
#define DPA_PROFILE_DO(code) ;
#endif

namespace dpa {
namespace prof {

struct Prof {
  static const uint32_t SAMPLE_STEP = 100;

private:
  struct TimeStats {
    std::size_t n;
    uint64_t min, p50, p95, p99, max;
    double mean, sd;
  };
  struct time_t {
    std::array<uint64_t, 1024> drain = {0};
    uint32_t drain_cnt = 0;

    std::array<uint64_t, 1024> burst = {0};
    uint32_t burst_cnt = 0;

    std::array<uint64_t, 32768> iter = {0};
    uint32_t iter_cnt = 0;

    std::array<uint64_t, 32768> rtt = {0};
    uint32_t rtt_cnt = 0;

    std::array<uint64_t, 1024> rtt_init = {0};
    std::array<bool, 512> rtt_init_ok = {0};
    uint32_t rtt_init_cnt = 0;

    std::array<uint64_t, 1024> rtt0 = {0};
    uint32_t rtt0_cnt = 0;
    bool rtt0_ok = false;
  } time;
  struct pkt_t {
    uint64_t rx_res = 0;   // all result packets
    uint64_t rx_res_k = 0; // result packets with k<n
    uint64_t rx_all = 0;
  } pkt;

  struct data_t {
    time_t time;
    pkt_t pkt;
  };

private:
  bool done = false;
  uint16_t tid = 0;
  uint16_t ops = 0;
  uint32_t ops_skipped = 0;
  uint32_t ops_skip = 0;

public:
  Prof(uint16_t tid, uint32_t skip = 0) : tid(tid), ops_skip(skip) {
    if constexpr (DPA_PROFILE) fmt::println("tid-{} profiler skipping {} ops", tid, skip);
  }

  void start(uint16_t opid) {
    if constexpr (not DPA_PROFILE) return;
    if (done) return;
    // #ifdef DPA_PROFILE_SKIP
    //     if (ops_skipped < DPA_PROFILE_SKIP) {
    //       ops_skipped++;
    //       return;
    //     }
    // #endif
    if (ops_skipped < ops_skip) {
      ops_skipped++;
    } else {
      ops++;
      done = false;
      time.rtt_init_ok = {0};
      time.rtt0_ok = false;
      // pkt.rx_all = 0;
      // pkt.rx_res_k = 0;
      // pkt.rx_res = 0;
    }
  }

  void stop() {
    if constexpr (not DPA_PROFILE) return;
    done = true;
  }
  bool stopped() { return done; }

  void rec_drain(uint64_t tsc) {
    if constexpr (not DPA_PROFILE) return;
    if (done || (time.drain_cnt >= time.drain.size())) return;
    time.drain[time.drain_cnt++] = tsc;
  }
  void rec_burst(uint64_t tsc) {
    if constexpr (not DPA_PROFILE) return;
    if (done || (time.burst_cnt >= time.burst.size())) return;
    time.burst[time.burst_cnt++] = tsc;
  }

  void rec_iter(uint64_t tsc) {
    if constexpr (not DPA_PROFILE) return;
    if (done || (time.iter_cnt >= time.iter.size())) return;
    time.iter[time.iter_cnt++] = tsc;
  }

  void rec_rtt(uint64_t rtt, uint8_t entry, bool straggle = false) {
    if constexpr (not DPA_PROFILE) return;
    if (done) return;

    ++pkt.rx_res;
    if (straggle) ++pkt.rx_res_k;

    if (entry == 0 && !time.rtt0_ok && time.rtt0_cnt < time.rtt0.size()) {
      time.rtt0[time.rtt0_cnt++] = rtt;
      time.rtt0_ok = true;
    }
    if (!time.rtt_init_ok[entry] && time.rtt_init_cnt < time.rtt0.size()) {
      time.rtt_init[time.rtt_init_cnt++] = rtt;
      time.rtt_init_ok[entry] = true;
    } else {
      if (time.rtt_cnt < time.rtt.size() && pkt.rx_all % SAMPLE_STEP == 0) time.rtt[time.rtt_cnt++] = rtt;
    }
  }

  void rec_rx() {
    if constexpr (not DPA_PROFILE) return;
    if (done) return;
    ++pkt.rx_all;
  }

  void summary(bool json = false) {
    const uint64_t hz = rte_get_tsc_hz();
    auto const to_us = [&](uint64_t cyc) -> uint64_t { return (uint64_t)(((__uint128_t)cyc * 1000000ull + hz / 2) / hz); };
    auto stats = [&](uint64_t *a, std::size_t n) -> TimeStats {
      for (size_t i = 0; i < n; ++i) a[i] = to_us(a[i]);
      TimeStats s{n, 0, 0, 0, 0, 0, 0.0, 0.0};
      if (!n) return s;
      auto [mi, ma] = std::minmax_element(a, a + n);
      s.min = *mi;
      s.max = *ma;
      long double m = std::accumulate(a, a + n, (long double)0) / n, ss = 0;
      for (std::size_t i = 0; i < n; ++i) {
        long double d = (long double)a[i] - m;
        ss += d * d;
      }
      std::vector<uint64_t> v(a, a + n);
      std::sort(v.begin(), v.end());
      auto q = [&](double qq) {
        double p = qq * (n - 1);
        std::size_t i = (std::size_t)p;
        double f = p - i;
        return (i + 1 < n) ? (uint64_t)std::llround((1 - f) * v[i] + f * v[i + 1]) : v[i];
      };
      s.p50 = q(0.50);
      s.p95 = q(0.95);
      s.p99 = q(0.99);
      s.mean = (double)m;
      s.sd = std::sqrt((double)(ss / (n > 1 ? (n - 1) : 1)));
      return s;
    };

    auto t_burst = stats(time.burst.data(), time.burst_cnt);
    auto t_drain = stats(time.drain.data(), time.drain_cnt);
    auto t_rtt0 = stats(time.rtt0.data(), time.rtt0_cnt);
    auto t_rtt_init = stats(time.rtt_init.data(), time.rtt_init_cnt);
    auto t_rtt = stats(time.rtt.data(), time.rtt_cnt);

    fmt::println("'{}': {{'ops': {}, 'skipped': {}, 'rx': {}, 'rx_k': {},  'drain': {:.2f}, 'burst': {:.2f}, "
                 "'rtt0': {{'n': {}, 'min': {}, 'max': {}, 'mean': {:.2f}, 'std': {:.2f}, 'p': [{},{},{}] }}, "
                 "'rtt_init': {{'n': {}, 'min': {}, 'max': {}, 'mean': {:.2f}, 'std': {:.2f}, 'p': [{},{},{}] }}, "
                 "'rtt': {{'n': {}, 'min': {}, 'max': {}, 'mean': {:.2f}, 'std': {:.2f}, 'p': [{},{},{}] }}, ",
                 tid, ops, ops_skipped, pkt.rx_res, pkt.rx_res_k, t_drain.mean, t_burst.mean, t_rtt0.n, t_rtt0.min, t_rtt0.max, t_rtt0.mean,
                 t_rtt0.sd, t_rtt0.p50, t_rtt0.p95, t_rtt0.p99, t_rtt_init.n, t_rtt_init.min, t_rtt_init.max, t_rtt_init.mean,
                 t_rtt_init.sd, t_rtt_init.p50, t_rtt_init.p95, t_rtt_init.p99, t_rtt.n, t_rtt.min, t_rtt.max, t_rtt.mean, t_rtt.sd,
                 t_rtt.p50, t_rtt.p95, t_rtt.p99);
  }
};

} // namespace prof
} // namespace dpa

#endif