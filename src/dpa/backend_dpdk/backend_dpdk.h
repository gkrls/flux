#ifndef DPA_BACKEND_DPDK_H
#define DPA_BACKEND_DPDK_H

#include "dpa/allreduce.h"
#include "dpa/backend.h"
#include "dpa/context.h"
#include "dpa/task.h"
#include "dpa/util/prof.h"
#include "backend_dpdk_monitor.h"
// #include "dpa/util/net.h"

#include <chrono>
#include <condition_variable>
#include <generic/rte_byteorder.h>
#include <rte_atomic.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_launch.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_mempool.h>
#include <rte_timer.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace dpa {
// forward declarations to avoid heavy includes in header
class Task;
class Context;
class DpdkBackend;
class DpdkWorker;

struct DpdkBackendOptions : public BackendOptions {
public:
  friend class DpdkBackend;
  friend class DpdkWorker;
  static constexpr uint64_t DEFAULT_TIMEOUT_US = 200;
  static constexpr float DEFAULT_TIMEOUT_INIT_SCALING = 2.0;
  static constexpr uint64_t DEFAULT_RX_INTERVAL_US = 10;
  static constexpr uint64_t DEFAULT_TX_INTERVAL_US = 50;
  static constexpr uint32_t DEFAULT_RX_RING_SIZE = 1024;
  static constexpr uint32_t DEFAULT_TX_RING_SIZE = 512;
  static constexpr uint32_t DEFAULT_RX_BURST = 32;
  static constexpr uint32_t DEFAULT_TX_BURST = 32;
  static constexpr uint16_t DEFAULT_WINDOW = 32;

#if DPA_DPDK_WIN_HUGE
  #pragma message("DPA_DPDK_WIN_HUGE Enabled")
  static constexpr uint16_t MAX_WINDOW = 2048;
#elif DPA_DPDK_WIN_LARGE
  static constexpr uint16_t MAX_WINDOW = 256;
#else
  static constexpr uint16_t MAX_WINDOW = 64;
#endif

public:
  /// Interface to use for this backend
  /// If empty the it will be inferred from the addr field
  /// If both addr+iface are supplied, addr must be bound to the iface
  /// If none is supplied an error is thrown
  std::string iface = "eth0";

  /// IP address for this backend all threads will use this address
  /// If empty, the first IP from the provided interface will be used
  /// If both addr+iface are supplied, addr must be bound to the iface
  /// If none is supplied an error is thrown
  std::string addr = "";

  /// Port number for this backend. When using multiple threads this
  /// is a base port and each thread i gets is assigned port + i
  uint16_t port = 4242;

  /// Number of threads to use for this backend
  uint16_t threads = 1;
  // bool pinned = false;
  bool async = false;
  bool drain_queues = false;
  /// Each thread operates on a sliding window of slots
  /// Each slot in the window requires 2 slots from the pool
  /// This window is the maximum number of outstanding packets/per thread
  /// and window * treads equals the total maximum of outstading packets.
  /// Window should be carefully set so as to maximize throughput.
  /// If the window is set to 0, the backend will automatically set to the
  /// maximum possible given the session pool size (dev.session.pool.size)
  uint16_t window = 0;

  bool debug_trace_packet = false;
  bool debug_trace_packet_rtx = false;

  std::chrono::microseconds timeout{DEFAULT_TIMEOUT_US};
  float timeout_init_scaling = DEFAULT_TIMEOUT_INIT_SCALING;
  std::chrono::microseconds rx_interval{DEFAULT_RX_INTERVAL_US};
  std::chrono::microseconds tx_interval{DEFAULT_TX_INTERVAL_US};

  uint16_t rx_burst = 1;
  uint16_t tx_burst = 1;
  uint16_t tx_attempts = 5;
  uint32_t rx_ring_size = 0;
  uint32_t rx_pool_size = 0;
  uint32_t rx_pool_cache = 0;
  uint32_t tx_ring_size = 0;
  uint32_t tx_pool_size = 0;
  uint32_t tx_pool_cache = 0;

  // DPDK EAL arguments
  std::string eal_port = "";
  std::string eal_iface = "";
  std::vector<std::string> eal_extra_args;

  uint32_t profile_skip = 0;

private:
  std::string eal_lcores = ""; // e.g., "0-3" or "0,2,4,6"
  std::string eal_file_prefix = "dpa-dpdk";

  bool eal_virtual = false;
  bool eal_istap = false;
  bool eal_isafp = false;
  uint16_t eal_port_id = 0;
  std::vector<std::string> eal_args;
  std::vector<char *> eal_argv;

  bool hw_csum = false;

public:
  DpdkBackendOptions() : BackendOptions(dpa::DPDK) {}
  virtual std::string str() const override;
  uint32_t requiredSlots() const { return window * threads * 2; }
  uint32_t maxOutstandingPackets() const { return threads * window; }

  static DpdkBackendOptions fromConfig(const std::string &path);
};

class DpdkWorker;

class DpdkBackend : public Backend {
public:
  inline static const std::string Name = "dpa-dpdk";

  friend class DpdkWorker;
  using Slot = SlotAlt;
  using Options = DpdkBackendOptions;
  DpdkBackend(Context &ctx, DpdkBackendOptions opt);
  ~DpdkBackend();
  virtual DpdkBackendOptions const &options() const override { return opt; }
  virtual void print(bool details) const override;
  virtual std::string name() const override { return Name; }

protected:
  virtual bool push(std::shared_ptr<Task> task) override;
  virtual void start() override;
  virtual void stop() override;
  void notify(uint16_t tid, std::shared_ptr<Task> task, Task::Status status);

  void configureDPDKPort();

private:
  Monitor monitor;

  State state;
  DpdkBackendOptions opt;

  std::string eal_args;

  // Hold pointers to each worker's mpool
  std::vector<rte_mempool *> rx_mpools;
  std::vector<rte_flow *> flows;

  Context &context;
  std::once_flag init_flag;
  std::once_flag fini_flag;
  std::condition_variable cv;

  std::vector<std::unique_ptr<DpdkWorker>> workers;

  struct RunningTaskInfo {
    std::shared_ptr<Task> task;
    std::atomic<uint16_t> threads;
  };

  std::mutex taskMutex;
  std::unordered_map<Task::id_t, RunningTaskInfo> tasks;
};

/// One thread in the DPDK backend
class DpdkWorker : public BackendWorker {
public:
  using Slot = DpdkBackend::Slot;
  friend class DpdkBackend;

  /// A worker's window entry. Fields ordered for some cache friendliness
  struct alignas(64) Entry {
    // first cache line
    uint64_t timeout = 0;
    uint64_t tsc = 0;
    rte_mbuf *mbuf0 = nullptr;
    // rte_mbuf *mbuf_rtx = nullptr;

    uint32_t seq[2] = {0};  // sequence number tracking
    uint16_t slot[2] = {0}; // device slot tracking

    // minimum info needed to rebuild packet for retransmission
    uint32_t offset = 0;
    uint32_t quants = 0;
    uint32_t exponents = 0; // stored exponents
    uint16_t vcount = 0;

    flags_t flags = 0;
    bool mbuf_rebuild = false;
    bool fin = 1;
    bool ver = 0; // version
    bool firstquant = false;

    // Second cache line
    bool ver_first = 0;
    bool ver_last = 0;
    uint32_t seq_last[2] = {0};
    uint16_t idx = 0;

    inline uint16_t slot_idx() const { return slot[ver]; }
    inline uint32_t slot_seq() const { return seq[ver]; }
  };
  std::string pktString(AllReducePacketNS const &pkt, bool tx, bool hostorder, std::string const &suffix = "",
                        std::chrono::steady_clock::time_point const &ts = std::chrono::steady_clock::now()) const;

  std::string pktString(AllReducePacket const &pkt, bool tx, bool hostorder, std::string const &suffix = "",
                        std::chrono::steady_clock::time_point const &ts = std::chrono::steady_clock::now()) const;

  uint16_t getid() const { return tid; }

protected:
  DpdkWorker(uint16_t tid, uint16_t lcore, rte_mempool *rx_mempool, DpdkBackend &backend);
  virtual void join() override;
  virtual void start() override;
  virtual void stop() override;
  virtual bool push(std::shared_ptr<Task> task) override;
  void waitReady();
  void main();

private:
  static int trampoline(void *worker_ptr);
  void shutdown();
  void notifyReady();
  void taskLoop();
  bool taskFinished() { return active_entries.empty(); }
  bool taskStart(std::shared_ptr<Task> task);
  bool taskFinish();
  bool taskFinish(uint16_t entry);

private:
  int loop_ns(std::shared_ptr<Task> task); // straggle-unaware loop
  int sendInitialBurstNS();
  bool checkTimeoutsNS(uint64_t now);

  // Send the initial burst of packets

private:
  int loop(std::shared_ptr<Task> task); // straggle-aware loop
  int sendInitialBurst();
  int checkTimeouts(uint64_t now);
  void fastestk(const AllReducePacket &q, uint32_t bitmap, uint32_t world, DpdkWorker::Entry &e);
  void enterSynBulk(const AllReducePacket &q, DpdkWorker::Entry &e);
  void enterSyn(const AllReducePacket &q, DpdkWorker::Entry &e);
  /// Exit SYN-mode for an entry e.
  /// If received seqnum is outside the range of the current task, the entry is set to its exit state and marked finished for the task
  /// Otherwise, the entry state is ready to advance
  /// @return `true` if the entry is finished, otherwise `false`
  bool exitSyn(const AllReducePacket &q, uint32_t q_seqnum, uint32_t q_offset, DpdkWorker::Entry &e);
  void skipForward(int64_t diff, Entry &e);

private:
  std::once_flag init_flag;
  std::once_flag fini_flag;

  /// Mutex for worker-wide operations
  /// e.g. state transitions, task pushes (from the backend) etc
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic<bool> running{false};

  std::mutex mutexReady;
  std::condition_variable cvReady;
  std::atomic<bool> ready{false};

  /// Tasks to be executed by this worker
  std::queue<std::shared_ptr<Task>> queue;

  /// Current task being executed
  std::shared_ptr<Task> task;

  DpdkBackend &backend;
  DpdkBackendOptions &opt;
  uint16_t tid;
  uint16_t lcore;

  rte_ether_addr smac;
  rte_ether_addr dmac;
  // Keep these in network order
  rte_be32_t saddr;
  rte_be32_t daddr;
  rte_be16_t sport;
  rte_be16_t dport;

  InputChunk chunk;
  uint16_t win_capacity;
  uint16_t win_size = 0;

  Bitset<DpdkBackendOptions::MAX_WINDOW> active_entries;

  Entry *win = nullptr;

  /// Pool info
  const uint16_t global_pool_base;
  const uint16_t global_pool_size;
  const uint16_t subpool_base_local;
  const uint16_t subpool_base_global;
  const uint16_t subpool_size;

  /// Packet size info
  uint16_t header_size = 0;
  uint16_t header_offset = 0;
  uint16_t payload_offset = 0;
  uint16_t payload_len = 0;
  uint16_t payload_size = 0;
  uint16_t packet_size = 0;
  uint16_t frame_size = 0;

  /// DPDK buffer management
  struct rte_mempool *rx_pool = nullptr;
  struct rte_mempool *tx_pool = nullptr;
  struct rte_mbuf **rx_mbufs = nullptr;
  struct rte_mbuf **tx_mbufs = nullptr;
  struct rte_eth_dev_tx_buffer *tx_buffer = nullptr;
  struct rte_flow *flow = nullptr;

  /// Timeouts
  uint64_t re_timeout = 0;
  uint64_t re_timeout_scaled = 0;

  /// Profiling
  prof::Prof prof;
};

namespace dpdk {
using Options = DpdkBackendOptions;
using Backend = DpdkBackend;
} // namespace dpdk
} // namespace dpa

#endif