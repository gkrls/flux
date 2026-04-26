#include "dpa/backend_dpdk/backend_dpdk.h"
#include "dpa/backend_dpdk/backend_dpdk_utils.h"
#include "dpa/util/config.h"
#include "dpa/util/error.h"
#include "dpa/util/log.h"
#include "dpa/util/pp.h"
#include "fmt/core.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <generic/rte_byteorder.h>
#include <memory>
#include <rte_byteorder.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <string>

using namespace dpa;
using namespace dpa::dpdk;
using namespace std;
using namespace nlohmann;

std::string DpdkBackendOptions::str() const { return fmt::format("DpdkOptions: addr={}, port={}, threads={}", addr, port, threads); }

DpdkBackendOptions DpdkBackendOptions::fromConfig(const std::string &path) {
  Info("reading DPDK backend options from config.json: {}", path);

  DpdkBackendOptions opt;

  std::ifstream f(path);
  if (!f.is_open()) DPA_THROW("failed to open config json: {}", path)

  try {
    json data = json::parse(f);
    if (!data.contains("dpdk")) DPA_THROW("config '{}' does not contain dpa options", path);

    conf::read_if_present<std::string>(opt.iface, data, "/dpdk/iface");
    conf::read_if_present<uint16_t>(opt.port, data, "/dpdk/port");
    conf::read_if_present<uint16_t>(opt.threads, data, "/dpdk/threads");
    // conf::read_if_present<bool>(opt.pinned, data, "/dpdk/pinned");
    conf::read_if_present<uint16_t>(opt.window, data, "/dpdk/window");
    conf::read_if_present<bool>(opt.async, data, "/dpdk/async");

    uint64_t timeout_us = 0;
    uint64_t tx_interval_us = 0;
    uint64_t rx_interval_us = 0;
    if (conf::read_if_present<uint64_t>(timeout_us, data, "/dpdk/timeout_us")) opt.timeout = std::chrono::microseconds(timeout_us);
    if (conf::read_if_present<uint64_t>(tx_interval_us, data, "/dpdk/tx_interval_us"))
      opt.tx_interval = std::chrono::microseconds(tx_interval_us);
    if (conf::read_if_present<uint64_t>(rx_interval_us, data, "/dpdk/rx_interval_us"))
      opt.rx_interval = std::chrono::microseconds(rx_interval_us);

    conf::read_if_present<float>(opt.timeout_init_scaling, data, "/dpdk/timeout_init_scaling");

    conf::read_if_present<bool>(opt.drain_queues, data, "/dpdk/drain_queues");

    conf::read_if_present<uint32_t>(opt.tx_ring_size, data, "/dpdk/tx_ring_size");
    conf::read_if_present<uint32_t>(opt.rx_ring_size, data, "/dpdk/rx_ring_size");

    conf::read_if_present<uint16_t>(opt.tx_burst, data, "/dpdk/tx_burst");
    conf::read_if_present<uint16_t>(opt.rx_burst, data, "/dpdk/rx_burst");

    conf::read_if_present<uint32_t>(opt.tx_pool_size, data, "/dpdk/tx_pool_size");
    conf::read_if_present<uint32_t>(opt.tx_pool_cache, data, "/dpdk/tx_pool_cache");
    conf::read_if_present<uint32_t>(opt.rx_pool_size, data, "/dpdk/rx_pool_size");
    conf::read_if_present<uint32_t>(opt.rx_pool_cache, data, "/dpdk/rx_pool_cache");

    conf::read_if_present<std::string>(opt.eal_port, data, "/dpdk/eal_port");
    conf::read_if_present<std::string>(opt.eal_lcores, data, "/dpdk/eal_lcores");
    conf::read_if_present<std::string>(opt.eal_iface, data, "/dpdk/eal_iface");

    conf::read_if_present<std::vector<std::string>>(opt.eal_extra_args, data, "/dpdk/eal_extra_args");

    conf::read_if_present<bool>(opt.debug_trace_packet, data, "/dpdk/debug_trace_packet");
    conf::read_if_present<bool>(opt.debug_trace_packet_rtx, data, "/dpdk/debug_trace_packet_rtx");

    conf::read_if_present<uint32_t>(opt.profile_skip, data, "/dpdk/profile_skip");

  } catch (const json::parse_error &e) { DPA_THROW("failed to parse config json {}: {}", path, std::string(e.what())); }

  return opt;
}

void DpdkBackend::print(bool details) const {
  auto threadsstr = fmt::format("{}{}", opt.threads, opt.async ? ",async" : ",sync");
  WriteLn("Backend: ({}) addr: {}:{} iface: {} threads: {} window: {} subpool: [{},{}){} loop: {}", name(), opt.addr, opt.port, opt.iface,
          threadsstr, opt.window, context.device.session.pool.base, context.device.session.pool.base + opt.requiredSlots(), "",
          //  (context.device.session.pool.size - opt.requiredSlots())
          //      ? fmt::format(",{} unused", context.device.session.pool.size - opt.requiredSlots())
          //      : "",
          context.device.straggle_aware ? "sa" : "su");

  if (details) {
    WriteLn("         Outstanding: {} packets", opt.maxOutstandingPackets());
    WriteLn("         RTT timeout: {} usec, init_scaling: {:.2f}", opt.timeout.count(), opt.timeout_init_scaling);
    WriteLn("         TX/RX intrv: {}/{} usec", opt.tx_interval.count(), opt.rx_interval.count());
    WriteLn("         TX/RX burst: {}/{}", opt.tx_burst, opt.rx_burst);
    WriteLn("         TX/RX rings: {}/{}", opt.tx_ring_size, opt.rx_ring_size);
    WriteLn("         TX/RX mpool: {}/{}", opt.tx_pool_size, opt.rx_pool_size);
    WriteLn("         TX/RX cache: {}/{}", opt.tx_pool_cache, opt.rx_pool_cache);
  }

  WriteLn("            eal_port: {} (virtual={})", opt.eal_port, opt.eal_virtual ? "yes" : "no");
  WriteLn("          eal_lcores: {}", opt.eal_lcores);
  // Info("     eal_file_prefix: {}", opt.eal_file_prefix);
  WriteLn("      eal_extra_args: {}", opt.eal_extra_args.size() ? pp::join(opt.eal_extra_args) : "n/a");
}

DpdkBackend::~DpdkBackend() { stop(); }

DpdkBackend::DpdkBackend(Context &ctx, DpdkBackendOptions opts) : Backend(ctx), opt(std::move(opts)), context(ctx) {
  if (!opt.addr.empty() and !opt.iface.empty()) {
    auto iface = net::iface_for_ipv4(opt.addr);
    auto addr = net::ipv4_for_iface(opt.iface);
    // if (iface != opt.iface || addr != opt.addr)
    //   DPA_THROW_CONTEXT(&context, "iface/addr combination '{}'/'{}' not found", opt.iface, opt.addr)
  } else if (!opt.addr.empty()) {
    opt.iface = net::iface_for_ipv4(opt.addr);
    DPA_THROW_CONTEXT_IF(opt.iface.empty(), &context, "address '{}' not found at an interface", opt.addr)
    dpa::Info("Using interface '{}', inferred from address '{}'", opt.iface, opt.addr);
  } else if (!opt.iface.empty()) {
    DPA_THROW_CONTEXT_IF(!net::iface_exists(opt.iface), &context, "interface '{}' not found", opt.iface)
    opt.addr = net::ipv4_for_iface(opt.iface);
    DPA_THROW_CONTEXT_IF(opt.addr.empty(), &context, "iface '{}' has no address", opt.iface)
    dpa::Info("Using address '{}', inferred from iface '{}'", opt.addr, opt.iface);
  } else {
    DPA_THROW_CONTEXT(&context, "missing both 'iface' and 'addr' options, at least one must be supplied")
  }

  // Check if we have enough slots allocated
  auto required = opt.requiredSlots();
  auto available = ctx.device.session.pool.size;
  DPA_THROW_CONTEXT_IF(required > available, &context, "{} slots required by backend but only {} allocated for session", required,
                       available);

  if (opt.threads == 0) opt.threads = 1;
  if (!opt.rx_ring_size) opt.rx_ring_size = opt.DEFAULT_RX_RING_SIZE;
  if (!opt.tx_ring_size) opt.tx_ring_size = opt.DEFAULT_TX_RING_SIZE;
  if (!opt.rx_burst) opt.rx_burst = opt.DEFAULT_RX_BURST;
  if (!opt.tx_burst) opt.tx_burst = opt.DEFAULT_TX_BURST;
  // Auto-compute mempool sizes using actual ring sizes
  if (opt.rx_pool_size == 0) {
    uint32_t min_rx_pool = opt.rx_ring_size + opt.window + (2 * opt.rx_burst) + 256;
    opt.rx_pool_size = RTE_ALIGN_CEIL(min_rx_pool, 64);
    Info("Auto-computed rx_pool_size={} (ring={}, window={}, burst={})", opt.rx_pool_size, opt.rx_ring_size, opt.window, opt.rx_burst);
  }
  if (opt.tx_pool_size == 0) {
    uint32_t min_tx_pool = (opt.window * 2) + (2 * opt.tx_burst) + 256;
    opt.tx_pool_size = RTE_ALIGN_CEIL(min_tx_pool, 64);
    Info("Auto-computed tx_pool_size={} (window={}, burst={})", opt.tx_pool_size, opt.window, opt.tx_burst);
  }

  if (!opt.timeout.count()) opt.timeout = chrono::microseconds(DpdkBackendOptions::DEFAULT_TIMEOUT_US);

  if (!opt.tx_interval.count()) opt.tx_interval = chrono::microseconds(DpdkBackendOptions::DEFAULT_TX_INTERVAL_US);
  // if (!opt.rx_interval.count()) opt.rx_interval = chrono::microseconds(DpdkBackendOptions::DEFAULT_RX_INTERVAL_US);

  DPA_THROW_IF(opt.timeout < (opt.tx_interval * 2), "timeout ({}) must be >= 2x tx_interval ({}) to avoid retransmit-before-flush",
               opt.timeout.count(), opt.tx_interval.count());
  DPA_THROW_IF(opt.window > DpdkBackendOptions::MAX_WINDOW, "window cannot be larger than {}", DpdkBackendOptions::MAX_WINDOW);

  /// SETUP DPDK EAL

  // See if we should infere port from eal_extra_args
  if (opt.eal_port.size() == 0) {
    auto inferred_port = dpdkInferPortFromArgs(opt.eal_extra_args);
    DPA_THROW_IF(inferred_port.empty(), "eal_port not provided and not inferred from eal_extra_args");
    opt.eal_port = inferred_port;
  }

  opt.eal_isafp = opt.eal_port.rfind("net_af_packet", 0) == 0;
  opt.eal_istap = opt.eal_port.rfind("net_tap0", 0) == 0;
  opt.eal_virtual = opt.eal_istap || opt.eal_isafp;

  DPA_THROW_IF(opt.eal_isafp && opt.threads > 1, "DpdkBackend with net_af_packet0 supports only 1 worker (no steering)")

  // af_packet: iface = existing kernel netdev (e.g. eth0)
  // TAP: iface = tap name to create; remote = kernel dev to mirror
  if (opt.eal_istap && opt.eal_iface.empty()) {
    opt.eal_iface = "tap0";
    dpa::Warn("No eal_iface provided for '{}', assuming '{}'", opt.eal_port, "tap0");
  }

  // We ignore anything passed by the user here. We must match the number of backend threads + 1
  // opt.eal_lcores = "0-" + std::to_string(opt.threads);
  // --- lcore selection ---
  // Check if eal_extra_args already contains -l/--lcores
  bool lcores_in_extra =
      std::any_of(opt.eal_extra_args.begin(), opt.eal_extra_args.end(), [](const std::string &a) { return a == "-l" || a == "--lcores"; });

  if (lcores_in_extra) {
    dpa::Warn("eal_extra_args contains lcore flag — skipping validation. Ensure >= {} ({} threads + 1 main)",opt.threads + 1, opt.threads);
  } else {
    int lcore_start = -1, lcore_end = -1;
    bool valid = !opt.eal_lcores.empty() && sscanf(opt.eal_lcores.c_str(), "%d-%d", &lcore_start, &lcore_end) == 2;

    if (valid) {
      int available = lcore_end - lcore_start + 1;
      DPA_THROW_IF(available < opt.threads + 1, "eal_lcores '{}' provides {} cores but {} needed ({} threads + 1 main)", opt.eal_lcores,
                   available, opt.threads + 1, opt.threads);
      if (lcore_start == 0) dpa::Warn("eal_lcores '{}' starts at core 0 — may conflict with user process", opt.eal_lcores);
      WriteLn("Using user-specified eal_lcores '{}'", opt.eal_lcores);
    } else {
      if (!opt.eal_lcores.empty()) dpa::Warn("Could not parse eal_lcores '{}' — falling back to auto-select", opt.eal_lcores);
      int total = sysconf(_SC_NPROCESSORS_ONLN);
      int needed = opt.threads + 1;
      DPA_THROW_IF(needed > total, "Need {} cores but system only has {}", needed, total);
      int start = total - needed;
      opt.eal_lcores = std::to_string(start) + "-" + std::to_string(total - 1);
      WriteLn("Auto-selected eal_lcores '{}'", opt.eal_lcores);
    }
  }

  // Prepare the EAL command line
  int main_lcore = -1;
  sscanf(opt.eal_lcores.c_str(), "%d", &main_lcore);
  opt.eal_args = {"dpa-dpdk", "-l", opt.eal_lcores, "--main-lcore", std::to_string(main_lcore), "--file-prefix", opt.eal_file_prefix};


  WriteLn("Main lcore: {}", main_lcore);

  if (opt.eal_virtual) {
    opt.eal_args.push_back("--no-pci");
    opt.eal_args.push_back("--in-memory");
    // opt.eal_args.push_back("--no-huge");
    opt.eal_args.push_back("-n");
    opt.eal_args.push_back("1");
    if (opt.eal_isafp) {
      opt.eal_args.push_back(fmt::format("--vdev={},iface={}", opt.eal_port, opt.iface));
    } else {
      opt.eal_args.push_back(fmt::format("--vdev={},iface={},remote={}", opt.eal_port, opt.eal_iface, opt.iface));
    }

    // Make sure pool sizes are reasonable when not using hugepages
    // opt.rx_pool_size = std::max<uint32_t>(opt.rx_pool_size, 2048);
    // opt.rx_pool_cache = std::max<uint32_t>(opt.rx_pool_cache, 512);
    // opt.tx_pool_size = std::max<uint32_t>(opt.rx_pool_size / 2, 1024);
    // opt.tx_pool_cache = std::max<uint32_t>(opt.tx_pool_cache, 512);

  } else {
    opt.eal_args.push_back("-a");
    opt.eal_args.push_back(opt.eal_port);
    // opt.eal_args.push_back("--allow");
    // opt.eal_args.push_back(fmt::format("{},mprq_en=0", opt.eal_port));
    // TODO: What else is needed here?
  }

  for (auto &extra : opt.eal_extra_args) opt.eal_args.push_back(extra);
  for (auto &arg : opt.eal_args) opt.eal_argv.push_back(&arg[0]);

  state = Backend::Created;
}

void DpdkBackend::configureDPDKPort() {
  // Check if out port id is present
  uint16_t tmp_port_id;
  bool port_id_found = false;
  RTE_ETH_FOREACH_DEV(tmp_port_id) {
    if (opt.eal_port_id == tmp_port_id) {
      port_id_found = true;
      break;
    }
  }
  DPA_THROW_IF(!port_id_found, "Port id {} not found. Number of enabled ports is {}", opt.eal_port_id, rte_eth_dev_count_avail());

  struct rte_eth_dev_info dev_info;
  rte_eth_dev_info_get(opt.eal_port_id, &dev_info);
  // printDevInfo(dev_info);
  // printf("RX queues: %u, TX queues: %u\n", dev_info.nb_rx_queues, dev_info.nb_tx_queues);
  // printf("Device capabilities: %lx\n", dev_info.dev_capa);
  // Check if hairpin is configured
  // struct rte_eth_hairpin_cap hairpin_cap;
  // if (rte_eth_dev_hairpin_capability_get(opt.eal_port_id, &hairpin_cap) == 0) {
  //   printf("Hairpin: max_nb_queues=%u, max_rx_2_tx=%u, max_tx_2_rx=%u\n", hairpin_cap.max_nb_queues, hairpin_cap.max_rx_2_tx,
  //          hairpin_cap.max_tx_2_rx);
  // }

  // Check if mlx5 registered any dynamic fields:
  // int mlx5_timestamp_dynflag = rte_mbuf_dynflag_lookup("RTE_MBUF_DYNFLAG_RX_TIMESTAMP", NULL);
  // int mlx5_flow_dynfield = rte_mbuf_dynfield_lookup("rte_flow_dynfield", NULL);
  // printf("mlx5 dynflags: timestamp=%d, flow=%d\n", mlx5_timestamp_dynflag, mlx5_flow_dynfield);
  struct rte_eth_conf port_conf {};
  port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS; // RTE_ETH_MQ_RX_NONE;
  port_conf.rxmode.offloads |= (dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_IPV4_CKSUM) ? RTE_ETH_RX_OFFLOAD_IPV4_CKSUM : 0;
  port_conf.rxmode.offloads |= (dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_UDP_CKSUM) ? RTE_ETH_RX_OFFLOAD_UDP_CKSUM : 0;
  port_conf.txmode.offloads |= (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM) ? RTE_ETH_TX_OFFLOAD_IPV4_CKSUM : 0;
  port_conf.txmode.offloads |= (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM) ? RTE_ETH_TX_OFFLOAD_UDP_CKSUM : 0;

  // #if !DPA_DPDK_RX_REUSE
  //   // If we reuse RX mbufs we have to disable this because with it the driver assume/requires TX/RX mbufs to come from diff pools
  //   port_conf.txmode.offloads |= (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) ? RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE : 0;
  // #endif
  port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;
  port_conf.lpbk_mode = 0;

  // RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE seems to cause problems in both cases, so keep it disabled for now...
  port_conf.txmode.offloads &= ~RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
  // #if DPA_DPDK_RX_REUSE
  //   #warning "DISABLING RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE"
  //   // When reusing RX mbufs for TX, must disable fast free
  //   port_conf.txmode.offloads &= ~RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
  // #else
  //   #warning "ENABLING RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE (if available)"
  //   // Only enable if supported
  //   port_conf.txmode.offloads |= (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) ? RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE : 0;
  // #endif

  // remember HW csum capability for later
  opt.hw_csum = ((dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM) && (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM));

  // Flow director cfg
  port_conf.rx_adv_conf.rss_conf.rss_key = NULL;
  port_conf.rx_adv_conf.rss_conf.rss_hf = 0;
  // port_conf.rx_adv_conf.rss_conf.rss_hf = RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP;
  // port_conf.lpbk_mode = RTE_ETH_LINK_LOOPBACK_MODE_NONE;  // Explicitly disable loopback

  // Isolate flow so that we only receive packets according to our flow rules
  struct rte_flow_error error;
  DPA_THROW_IF(rte_flow_isolate(opt.eal_port_id, 1, &error) < 0, "Flow isolated mode failed: {}: {}", (int)error.type,
               error.message ? error.message : "no reason");

  int ret = rte_eth_dev_configure(opt.eal_port_id, opt.threads, opt.threads, &port_conf);
  DPA_THROW_IF(ret < 0, "Config dev port: {}", rte_strerror(ret));

  // Check that numbers of Rx/Tx descriptors satisfy descriptors limits from dev info, otherwise adjust to boundaries.
  uint16_t port_rx_ring_size = std::min<uint16_t>(dev_info.rx_desc_lim.nb_max, opt.rx_ring_size);
  uint16_t port_tx_ring_size = std::min<uint16_t>(dev_info.tx_desc_lim.nb_max, opt.tx_ring_size);

  ret = rte_eth_dev_adjust_nb_rx_tx_desc(opt.eal_port_id, &port_rx_ring_size, &port_tx_ring_size);
  DPA_THROW_IF(ret < 0, "Cannot adjust number of descriptors: {}", rte_strerror(ret))

  Info("DPDK runtime ring size: RX={} TX={}", port_rx_ring_size, port_tx_ring_size);

  struct rte_eth_rxconf rx_conf = {};
  struct rte_eth_txconf tx_conf = {};
  rx_conf = dev_info.default_rxconf;
  rx_conf.offloads = port_conf.rxmode.offloads;
  tx_conf = dev_info.default_txconf;
  tx_conf.offloads = port_conf.txmode.offloads;

  // How many mbufs do we need?
  // size_t rx_pool_size = 32768; // opt.window * 2; // SwitchML uses 131072 ????
  // size_t rx_pool_cache = std::min<unsigned>(512, (unsigned)rx_pool_size / 2);

  auto dev_sock = rte_eth_dev_socket_id(opt.eal_port_id);

  // For each worker do all the setup that's needed before dev_start
  for (auto i = 0; i < opt.threads; ++i) {
    DEBUG("Creating RX and TX queues for worker thread {}", i);

    // Create RX mempool
    auto rx_mpool_name = fmt::format("dpa_worker_{}_rx", i); // + std::to_string(i) + "_rx";
    auto *rx_mpool =
        rte_pktmbuf_pool_create(rx_mpool_name.c_str(), opt.rx_pool_size, opt.rx_pool_cache, 0, RTE_MBUF_DEFAULT_BUF_SIZE, dev_sock);
    DPA_THROW_IF(rx_mpool == NULL, "RX mempool '{}', size {}, cache {} failed", rx_mpool_name, opt.rx_pool_size, opt.rx_pool_cache);
    rx_mpools.push_back(rx_mpool);

    // Create RX queue
    ret = rte_eth_rx_queue_setup(opt.eal_port_id, i, port_rx_ring_size, dev_sock, &rx_conf, rx_mpool);
    DPA_THROW_IF(ret < 0, "RX queue setup failed: {}", rte_strerror(ret));

    // Create TX queue -- TX mbufs will be created later by the worker
    ret = rte_eth_tx_queue_setup(opt.eal_port_id, i, port_tx_ring_size, dev_sock, &tx_conf);
    DPA_THROW_IF(ret < 0, "TX queue setup failed: {}", rte_strerror(ret));
  }

  if (rte_eth_dev_start(opt.eal_port_id) < 0) DPA_THROW("Error starting dev port: {}", rte_strerror(ret));

  ret = rte_eth_promiscuous_disable(opt.eal_port_id);
  Info("Promiscuous mode disabled: {}", ret == 0 ? "success" : "failed");

  struct rte_flow_error ferr;
  rte_flow_flush(opt.eal_port_id, &ferr);

  if (!opt.eal_isafp) { // mlx5 or TAP
    for (auto i = 0; i < opt.threads; ++i) {
      createWorkerFlow(opt.eal_port_id, /*rx_q=*/i, /*udp_dst_port=*/opt.port + i, /*use_priorities*/ opt.eal_istap); // opt.eal_istap
      // flows.push_back(flow);
    }
    if (opt.eal_istap) {
      // createARPDropFlow(opt.eal_port_id);
      // createTAPPassthroughFlow(opt.eal_port_id);
    }
  } else {
    Info("af_packet: no rte_flow; single-queue mode, software filter if needed.");
  }

  rte_eth_dev_info_get(opt.eal_port_id, &dev_info);
  printf("Port configured with %u RX queues and %u TX queues\n", dev_info.nb_rx_queues, dev_info.nb_tx_queues);

  // struct rte_flow *f;
  // int count = 0;
  // printf("Listing all flows:\n");
  // while ((f = rte_flow_iterate(0, &f)) != NULL) {
  //     count++;
  //     printf("  Flow %d: %p\n", count, f);
  // }
  // printf("Total flows: %d\n", count);

  rte_ether_addr mac;
  ret = rte_eth_macaddr_get(opt.eal_port_id, &mac);
  DPA_THROW_IF(ret < 0, "Backend failed to retrieve MAC for port_id {}", opt.eal_port_id);
  std::array<char, RTE_ETHER_ADDR_FMT_SIZE> buf{};
  rte_ether_format_addr(buf.data(), buf.size(), &mac);
  WriteLn("DPDK device configured succefully. Port: {}, address {}", opt.eal_port_id, buf.data());
}

//
//
// Public API impl below
//
//

void DpdkBackend::start() {
  std::call_once(init_flag, [this] {
    dpa::Info("Using EAL args: {}", pp::join(opt.eal_args));

    // Initialize DPDK EAL
    DPA_THROW_IF(rte_eal_init(opt.eal_argv.size(), opt.eal_argv.data()) < 0, "Failed to initialize DPDK EAL");

    // struct rte_eth_dev_info dev_info;
    // rte_eth_dev_info_get(0, &dev_info);

    // Check port
    DPA_THROW_IF(rte_eth_dev_get_port_by_name(opt.eal_port.c_str(), &opt.eal_port_id) < 0, "No ethdev: {}", opt.eal_port.c_str());
    struct rte_eth_link link;
    memset(&link, 0, sizeof(link));
    rte_eth_link_get(0, &link);
    Info("DPDK link status: {}, speed: {}", link.link_status ? "UP" : "DOWN", link.link_speed);
    // printf("dpa: DPDK link status: %s, speed: %d\n", link.link_status ? "UP" : "DOWN", link.link_speed);

    // Configure port
    configureDPDKPort();
    checkPortLinkStatus(opt.eal_port_id);

    // Create workers
    std::vector<uint16_t> worker_lcores;
    uint16_t lc;
    RTE_LCORE_FOREACH_WORKER(lc) { worker_lcores.push_back(lc); }
    DPA_THROW_IF(worker_lcores.size() < opt.threads, "Not enough worker lcores");
    for (auto i = 0; i < opt.threads; ++i) workers.push_back(std::unique_ptr<DpdkWorker>(new DpdkWorker(i, worker_lcores[i], rx_mpools[i], *this)));

    // Start workers
    state = Backend::Initialized;
    for (auto &w : workers) w->start();
    for (auto &w : workers) w->waitReady();
    DEBUG("DPDK workers ready...")

    monitor.start(opt.eal_port_id);
  });
}

void DpdkBackend::stop() {
  std::call_once(fini_flag, [this] {
    monitor.stop();
    DEBUG("Backend DPDK stopping...");

    {
      std::unique_lock<std::mutex> lock(mutex);
      state = State::Finalizing;
      cv.notify_all();
    }

    // wait for all workers to cleanly exit
    for (auto &worker : workers) worker->stop();
    for (auto &worker : workers) worker->join();

    // now cleanup
    {
      std::lock_guard<std::mutex> lock(taskMutex);
      for (auto &[taskId, taskInfo] : tasks) taskInfo.task->setStatus(Task::Aborted);
      tasks.clear();
    }

    struct rte_flow_error err;
    if (opt.eal_istap) rte_flow_flush(opt.eal_port_id, &err);
    rte_eth_dev_stop(opt.eal_port_id);
    rte_eth_dev_close(opt.eal_port_id);

    for (auto *mp : rx_mpools)
      if (mp) rte_mempool_free(mp);
    rx_mpools.clear();

    // rte_malloc_dump_stats(stdout, NULL);
    rte_eal_cleanup();

    {
      std::unique_lock<std::mutex> lock(mutex);
      state = State::Finalized;
      cv.notify_all();
    }

    auto slotlo = context.device.session.pool.base;
    auto slothi = slotlo + opt.requiredSlots();
    dpa::Warn("dpa_dpdk finished with pool[{}:{}] seqnums: {}", slotlo, slothi,
              pp::head(&context.device.session.pool.seqnums[slotlo], context.device.session.pool.seqnums.size(), 32));

    if constexpr (DPA_PROFILE) {
      WriteLn("Profile output:");
      for (auto &w : workers) w->prof.summary(true);
    }
  });
}

bool DpdkBackend::push(std::shared_ptr<Task> task) {
  start();

  if (state != State::Initialized) return false;

  if (opt.threads == 0) return false;

  DPA_THROW_CONTEXT_IF(context.isFinalized(), &context, "Context is finalized");
  DPA_THROW_CONTEXT_IF(tasks.find(task->id) != tasks.end(), &context, "Task is already pushed");
  {
    std::lock_guard<std::mutex> lock(taskMutex);
    tasks[task->id].task = task;
    tasks[task->id].threads = opt.threads;
  }

  task->setStatus(Task::Submitted);

  for (auto &worker : workers)
    if (!worker->push(task)) {
      task->setStatus(Task::Aborted);
      return false;
    }

  return true;
}

void DpdkBackend::notify(uint16_t tid, std::shared_ptr<Task> task, Task::Status status) {
  std::lock_guard<std::mutex> lock(taskMutex);
  auto it = tasks.find(task->id);
  DPA_THROW_IF(it == tasks.end(), "DpdkWorker {} notification for unknown task '{}'", tid, task->name);

  Task::Status old_status = task->getStatus();
  int old_thread_count = 0;

  if (status == Task::Running) {
    if (task->getStatus() == Task::Submitted) task->setStatus(Task::Status::Running);
  } else {
    if (status == Task::Failed) {
      if (task->isRunning()) task->setStatus(Task::Failed);
      task->stats.perf.threads.fetch_add(1); // although it doesn't matter if the task failed
    }

    if (status == Task::Completed) { task->stats.perf.threads.fetch_add(1); }

    if (it->second.threads.fetch_sub(1) == 1) {
      // auto slotlo = context.device.session.pool.base;
      // auto slothi = slotlo + opt.requiredSlots();
      // dpa::Info("{} finishing with pool[{}:{}] seqnums: {}", task->name, slotlo, slothi,
      //           pp::head(&context.device.session.pool.seqnums[slotlo], context.device.session.pool.seqnums.size(), 32));
      if (!task->isFinished()) task->setStatus(Task::Completed);
      { tasks.erase(task->id); }
    }
  }
}