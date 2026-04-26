#ifndef DPA_DEVICE_H
#define DPA_DEVICE_H

#include <stdint.h>
#include <string>

#include "dpa/util.h"
#include "fmt/core.h"

namespace dpa {

struct DeviceOptions final {
  std::string name;
  /// @brief "Fake" mac address of the switch endpoint
  net::MacAddress mac = "00:00:00:00:00:00";
  /// @brief "Fake" ip address of the switch endpoint
  net::IPAddress ip = "0.0.0.0";
  /// @brief "Fake" udp port of the switch endpoint
  uint16_t port = 4242;
  /// @brief Maximum number of switch pipes available
  uint16_t pipes = 4;
  /// @brief Maximum number of exponent reducers available
  uint16_t exponents = 2;
  /// @brief Maximum number of values reducers available
  uint16_t reducers = 32;
  /// @brief Reduce mode. In dual mode each reducer works on 2 valus
  /// effectively doubling the number of values per pipes
  uint16_t reducer_mode = 1;
  /// @brief Maximum number of slots available
  uint16_t slots = 32768;
  /// @brief Whether or not the switch is running the straggle-aware program
  /// If false all straggle aware options are ignored by backends
  bool straggle_aware = true;
  /// @brief This struct contains information about a session with the device
  /// Ideally, there should be some mechanism in place to handle sessions.
  /// For instance in switchml, a master worker sets things like packet sizes etc
  /// and then it communicates with the others.
  /// In our case sessions are a bit more important, as they contain things like
  /// which slots, what are their sequence numbers, etc. So ideally, a master worker
  /// would contact some runtime system to "ask" for a session, and then sync with
  /// the other workers, etc.
  /// For now, we manually fill this struct. This is fine as we will only have 1 session
  /// for the entire switch.
  /// But if we want something more complex we have to realize session management somehow.
  struct Session {
    uint32_t id = 1;
    /// Amount of nanoseconds after which the switch will force-complete a slot if
    /// at least K results have been aggregated.
    /// K is controlled per task (see @AllReduceOptions)
    /// The straggleTimeout should be >= rtt timeout (see @SocketBackendOptions.timeout)
    float straggleTimeout = 0; // ms
    float dropsimIngress = 0;  // %
    float dropsimEgress = 0;   // %
    struct Pool {
      uint32_t base = 0;
      uint32_t size = 2;
      std::vector<uint32_t> seqnums;
    } pool;
  } session;

  inline uint32_t valuesPerPipe() const { return this->reducers * this->reducer_mode; }
  inline uint32_t minValues() const { return this->valuesPerPipe(); }
  inline uint32_t maxValues() const { return this->pipes * this->valuesPerPipe(); }

  void print(bool detailed = false) const;
  // inline void print(bool detailed = false) const {
  //   Info("device : {} {}:{} pipes={} quants={} reducers={} mode={} slots={} straggle_aware={}", name, ip.str(), port, pipes, exponents, reducers,
  //        reducer_mode, slots, straggle_aware);
  //   if (!detailed) return;
  //   Info("         session.{}", session.id);
  //   Info("          slots_pool_alloc: {}-{}", session.pool.base, session.pool.base + session.pool.size);
  //   Info("          starting_seqnums: {}", pp::headtail(session.pool.seqnums, 4));
  //   Info("          straggle_timeout: {}", straggle_aware ? fmt::format("{:.2f} usec", float(session.straggleTimeout) / 1000) : "n/a");
  //   Info("          gress_drops_prob: {:.2f}/{:.2f} %", session.dropsimIngress, session.dropsimIngress);
  // }

  // static DeviceOptions fromJson(std::string const &path);
  static DeviceOptions fromConfig(const std::string &path);
};

struct SlotBase {
  uint16_t g;    // global slot id
  uint16_t l;    // position relative to local slot pool
  uint16_t t;     // position relative to thread's pool (2 * window)
  uint16_t w;     // position within a window (the window entry)
  uint8_t w_ver; // version of the window entry
protected:
  SlotBase() = default;
  SlotBase(uint16_t global_slot_idx, uint16_t global_pool_base, uint16_t tid, uint16_t window);
  SlotBase(uint16_t global_slot_idx, uint16_t global_pool_base, uint16_t local_pool_base, uint16_t tid, uint16_t window);
public:
  uint16_t get_for_entry(uint16_t window, uint16_t entry, bool version, uint16_t base);
};

struct SlotStrided : public SlotBase {
  uint16_t g;    // global slot id
  uint16_t l;    // position relative to local slot pool
  uint16_t t;     // position relative to thread's pool (2 * window)
  uint16_t w;     // position within a window (the window entry)
  uint8_t w_ver; // version of the window entry
public:
  SlotStrided() : g(0), l(0), t(0), w(0) {}
  SlotStrided(uint16_t global_slot_idx, uint16_t global_pool_base, uint16_t tid, uint16_t window) {
    DPA_ASSERT(global_slot_idx >= global_pool_base, "slot must be greater than poolBase");
    g = global_slot_idx;
    l = global_slot_idx - global_pool_base;
    t = l - (tid * window * 2);
    w = t < window ? t : t - window;
    // w = t / 2;
    w_ver = t % 2; //w != t;
  }
  operator uint16_t() const { return g; }
  bool operator==(const SlotStrided &other) const { return g == other.g && l == other.l && t == other.t && w == other.w; }
  bool operator!=(const SlotStrided &other) const { return !(*this == other); }
  SlotStrided &operator=(const SlotStrided &other) = default;
  std::string str() { return fmt::format("g={} l={} t={} w={} w_ver={}", g, l, t, w, w_ver); }

  static uint16_t get_for_entry(uint16_t window, uint16_t entry, bool version, uint16_t base) {
    return base + entry + (version * window);
  }
};

struct SlotAlt : public SlotBase {
  uint16_t g;    // global slot id
  uint16_t l;    // position relative to local slot pool
  uint16_t t;     // position relative to thread's pool (2 * window)
  uint16_t w;     // position within a window (the window entry)
  uint8_t w_ver; // version of the window entry
public:
  SlotAlt() : g(0), l(0), t(0), w(0) {}
  SlotAlt(uint16_t global_slot_idx, uint16_t global_pool_base, uint16_t tid, uint16_t window) {
    DPA_ASSERT(global_slot_idx >= global_pool_base, "slot must be greater than poolBase");
    g = global_slot_idx;
    l = global_slot_idx - global_pool_base;
    t = l - (tid * window * 2);
    DPA_ASSERT(t < 2u * window, "slot not in thread's slot subpool");
    // w = t < window ? t : t - window;
    w = t / 2;
    w_ver = t % 2; //w != t;
  }
  SlotAlt(uint16_t global_slot_idx, uint16_t global_pool_base, uint16_t local_pool_base) {
    DPA_ASSERT(global_slot_idx >= global_pool_base, "slot must be greater than poolBase");
    g = global_slot_idx;
    l = global_slot_idx - global_pool_base;
    t = l - local_pool_base; //(tid * window * 2);
    // w = t < window ? t : t - window;
    w = t / 2;
    w_ver = t % 2; //w != t;
  }
  operator uint16_t() const { return g; }
  bool operator==(const SlotAlt &other) const { return g == other.g && l == other.l && t == other.t && w == other.w; }
  bool operator!=(const SlotAlt &other) const { return !(*this == other); }
  SlotAlt &operator=(const SlotAlt &other) = default;
  std::string str() { return fmt::format("g={} l={} t={} w={} w_ver={}", g, l, t, w, w_ver); }

  static uint16_t get_for_entry(uint16_t window, uint16_t entry, bool version, uint16_t base) {
    return base + entry * 2 + version;
  }
};

} // namespace dpa

#endif // DPA_DEVICE_H
