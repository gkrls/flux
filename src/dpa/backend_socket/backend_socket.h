
#ifndef DPA_BACKEND_SOCKET_H
#define DPA_BACKEND_SOCKET_H

#include <chrono>
#include <cstdint>
#include <netinet/in.h>
#include <sys/socket.h>

#include "dpa/backend.h"
#include "dpa/util/error.h"

namespace dpa {
struct SocketBackendOptions : public BackendOptions {
public:
  // static constexpr uint32_t MINIMUM_RX_POLLING_INTERVAL_US = 2; // 2us
  // static constexpr uint32_t MINIMUM_TX_FLUSH_INTERVAL_US = 0;   // 1us

  static constexpr uint32_t DEFAULT_TIMEOUT_US = 2000; // 1ms
  static constexpr uint32_t DEFAULT_RX_INTERVAL_US = 10;
  static constexpr uint32_t DEFAULT_TX_INTERVAL_US = 50;
  static constexpr uint32_t DEFAULT_RX_BURST = 1;
  static constexpr uint32_t DEFAULT_TX_BURST = 1;

  static constexpr uint8_t MIN_WINDOW = 1;
  static constexpr uint8_t MAX_WINDOW = 64;
  static constexpr uint8_t DEFAULT_WINDOW = 1;

public:
  // /// Interface to use for this backend
  // /// If empty the it will be inferred from the addr field
  // /// If both addr+iface are supplied, addr must be bound to the iface
  // /// If none is supplied an error is thrown
  // std::string iface = "";
  // /// IP address for this backend all threads will use this address
  // /// If empty, the first IP from the provided interface will be used
  // /// If both addr+iface are supplied, addr must be bound to the iface
  // /// If none is supplied an error is thrown
  // std::string addr = "";
  // /// UDP base port. Each thread will use port + tid as its port
  // uint16_t port = 4242;
  /// Interface to use for this backend
  /// If empty the it will be inferred from the addr field
  /// If both addr+iface are supplied, addr must be bound to the iface
  /// If none is supplied an error is thrown
  std::string iface = "eth0";

  /// IP address for this backend all threads will use this address
  /// If empty, the first IP from the provided interface will be used
  /// If both addr+iface are supplied, addr must be bound to the iface
  /// If none is supplied an error is thrown
  std::string addr = ""; // 42.0.0.1

  /// Port number for this backend. When using multiple threads this
  /// is a base port and each thread i gets is assigned port + i
  uint16_t port = 4242;

  /// Number of threads to use for this backend
  uint16_t threads = 1;
  /// If true, the backend will pin each thread to a core
  bool pinned = false;
  // In async mode, when a worker finishes a task it continue to the next task
  // immediately, otherwise it blocks waiting for all threads to finish
  bool async = false;
  /// Each thread operates on a sliding window of slots
  /// Each slot in the window requires 2 slots from the pool
  /// This window is the maximum number of outstanding packets/per thread
  /// and window * treads equals the maximum number of outstading packets.
  /// Window should be carefully set so as to maximize throughput.
  /// If the window is set to 0, the backend will automatically set to the
  /// maximum possible given the session pool size (dev.session.pool.size)
  uint8_t window = 0;
  /// Timeout (in microseconds) after which we consider that a packet is lost
  /// This should be set proportionally to the RTT
  /// uint64_t timeout = 2000000; // 2 ms
  std::chrono::microseconds timeout{DEFAULT_TIMEOUT_US}; // 2 us
  std::chrono::microseconds rx_interval{DEFAULT_RX_INTERVAL_US};
  std::chrono::microseconds tx_interval{DEFAULT_TX_INTERVAL_US};

  uint8_t rx_burst = 1;
  uint8_t tx_burst = 1;
  uint8_t tx_attempts = 5;

  float rx_dropsim = 0.0f;
  float tx_dropsim = 0.0f;

  // bool use_nonstraggle_loop = false;
  // /// RX configuration
  // struct rxopt {
  //   /// Maximum number of packets to receive in a burst
  //   uint8_t burst = 1;
  //   /// How often to poll the RX buffer (in microseconds)
  //   /// This should be set to a fraction of the tx.interval
  //   std::chrono::microseconds interval{10};
  //   float dropsim = 0.0;
  //   bool discard_on_syn = 0;
  // } rx;

  // /// TX configuration
  // struct txopt {
  //   /// Maximum number of packets to send in a burst
  //   uint8_t burst = 1;
  //   /// How often to flush the TX buffer (in microseconds)
  //   /// This should be set to a fraction of the rtt timeout
  //   std::chrono::microseconds interval{50};
  //   float dropsim = 0.0;
  //   const uint8_t attempts = 5;
  // } tx;

  // bool adaptive_timeout = false;        // not currently used
  // bool exponentialBackoff = false;      // not currently used
  // bool exponentialBackoffAttempts = 10; // not currently used

  bool debug_trace_packet = false;
  bool debug_trace_packet_rtx = false;
  // bool debug_start_with_zero_ver = false;
  // struct Debug {
  //   bool log_retransmissions = 0;
  //   bool win_start_with_zero_ver = 0;
  // } debug;
  // bool startVersionZeroAlways = false;
  // bool fusedloop = true;

  SocketBackendOptions() : BackendOptions(dpa::SOCKET) {}
  virtual std::string str() const override;
  uint32_t requiredSlots() const { return window * threads * 2; }
  uint32_t maxOutstandingPackets() const { return threads * window; }

  static SocketBackendOptions fromConfig(const std::string &path);
};

class SocketBackend : public Backend {
public:
  /// Helper class for net operations. Implementing burst tx and other things
  inline static const std::string Name = "dpa-sock";
  SocketBackend() = delete;
  SocketBackend(Context &ctx, SocketBackendOptions opt);

private:
  SocketBackendOptions opt;
};

} // namespace dpa

#endif