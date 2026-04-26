#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <sched.h>
#include <string>
#include <thread>
#include <vector>

namespace dpa {

struct Monitor {
  void start(uint16_t port_id_) {
    if (auto *v = std::getenv("DPA_DPDK_MONITOR")) enabled = std::string(v) == "1";
    if (auto *v = std::getenv("DPA_DPDK_MONITOR_INTERVAL_US")) interval_us = std::stoull(v);
    if (auto *v = std::getenv("DPA_DPDK_MONITOR_OUTPUT")) output = v;

    if (!enabled) return;

    port_id = port_id_;
    thread = std::thread(&Monitor::loop, this);
  }

  void stop() {
    if (!enabled) return;
    done.store(true, std::memory_order_release);
    if (thread.joinable()) thread.join();
  }

private:
  bool enabled = false;
  uint64_t interval_us = 100;
  std::string output = "/tmp/nic_throughput.csv";
  uint16_t port_id = 0;
  std::atomic<bool> done{false};
  std::thread thread;

  void loop() {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    constexpr int MAX_SAMPLES = 16384;
    struct Sample {
      uint64_t tsc, rx, tx;
    };
    std::vector<Sample> samples;
    samples.reserve(MAX_SAMPLES);

    struct rte_eth_stats stats;
    while (!done.load(std::memory_order_acquire) && samples.size() < MAX_SAMPLES) {
      rte_eth_stats_get(port_id, &stats);
      samples.push_back({rte_rdtsc(), stats.ibytes, stats.obytes});
      rte_delay_us_block(interval_us);
    }

    if (samples.empty()) return;
    uint64_t hz = rte_get_tsc_hz();
    FILE *f = fopen(output.c_str(), "w");
    if (!f) return;
    fprintf(f, "time_us,rx_bytes,tx_bytes\n");
    uint64_t t0 = samples[0].tsc;
    for (auto &s : samples) fprintf(f, "%.1f,%lu,%lu\n", (double)(s.tsc - t0) * 1e6 / hz, s.rx, s.tx);
    fclose(f);
    fprintf(stderr, "[DPA Monitor] %zu samples written to %s\n", samples.size(), output.c_str());
  }
};

} // namespace dpa