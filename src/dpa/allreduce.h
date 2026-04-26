#ifndef DPA_ALLREDUCE_H
#define DPA_ALLREDUCE_H

#include <cassert>
#include <stdint.h>

#include "dpa/device.h"

namespace dpa {

// Supported AllReduce datatypes
enum DataType { INT_32 = 1, UINT_32 = 2, FLOAT_32 = 3 };

using AllReduceDType = DataType;

inline static const std::string datatypeString(DataType dt) {
  switch (dt) {
  default: break;
  case UINT_32: return "u32";
  case FLOAT_32: return "f32";
  }
  return "i32";
};

inline static const uint8_t datatypeWidth(DataType dt) { return 4; }

struct AllReduceContributions {
  /// The backend will allocate this array to store contributions
  /// for each block. Clients are responsible for managing this
  /// allocation.
  uint8_t *ptr = nullptr;
  /// Each contributions corresponds to a block of block_size elements
  uint16_t block_size = 0;
  /// The number of blocks in the input data
  uint32_t blocks = 1;
  /// By default, each contribution is a counter
  /// If bitmaps is true, we will report contributions as bitmaps,
  /// where the j-th LSB (0-indexed) of the i-th bitmap, if set,
  /// denotes that that rank j has contributed to the result of block i
  /// This is useful for implementing mechanisms that attempt to
  /// retrieve gradients from non-straggling workers
  bool bitmaps = false;
};

struct AllReduceStats {
  std::chrono::steady_clock::time_point t_start;
  std::chrono::steady_clock::time_point t_end;
  std::atomic<uint32_t> sent_packets;
  std::atomic<uint32_t> rcvd_packets;
  std::atomic<uint32_t> sent_retransmissions;
};

// using AllReduceContributions = std::unordered_map<uint32_t, uint32_t>;

/// An AllReduceOptions struct can be passed to the AllReduce calls
/// for fine grained control of the operation
struct AllReduceOptions {
  /// Number of pipes to use for the allreduce task
  /// This is only meant for debugging purposes.
  /// For normal operation you should leave this at 0, which
  /// will default to the maximum pipes available in the device
  /// This is handled at the Task constructor.
  uint8_t pipes = 0;

  /// Quantization amount for floating point inputs
  /// Currently we only support block quantization, where a
  /// single exponent is used to scale a block of input values.
  /// This option selects the number of blocks to use (up to 4)
  /// 0 means no quantization
  /// Currently we do not support unquantized floating point
  /// allreduce, so leaving this at 0 will throw an error when
  /// the DataType is is FLOAT_32
  uint8_t quantization = 0;

  /// Enable averaging of the allreduce result.
  /// On straggle-anaware allreduce we average by the world size.
  /// On straggle-aware allreduce (see below), we average by the
  /// number of workers that contributed (per packet), which may
  /// be less than the world size
  /// If clients want to handle the averaging themselves can set
  /// this to false and use the straggle.contrib struct below to
  /// retrieve the contributions per input block (packet).
  /// TODO: This is currently not implemented
  bool averaging = false;

  /// If true it means that the input is pre-scaled, that is,
  /// each value is multiplied by 1/world_size in advance.
  /// This is the default behavior in PyTorch DDP
  /// If the input is pre-scaled, and averaging is enabled the
  /// behavior is as follows:
  ///   All N workers contributed   -> Nothing to do
  ///   C < N workers conttributed  -> Multiply each value by N/C
  /// Notice that the latter case requires straggle-awareness to
  /// be enabled (i.e. straggle.k > 0)
  bool prescaled = false;

  /// This is the K from fastest-K
  uint8_t sa_world      = 0;
  bool    sa_preemptive = false;

  // bool world_k_preemptive = false;

  // bool force_world_k = false;
  // uint8_t straggle_theta = 0;

  // bool isStraggleAware() { return this->world_k > 0; }

  // AllReduceOptions() : pipes(0), quantization(0), averaging(false), prescaled(false) {}
};

using bitmap_t = uint32_t;
using flags_t = uint8_t;
using quant_t = uint32_t;
using value_t = uint32_t;

enum Flag : flags_t {
  F_BA = 0b10000000,
  F_OL = 0b01000000,
  F_U1 = 0b00100000,
  F_U2 = 0b00010000,
  F_SY = 0b00001000,
  F_CK = 0b00000100,
  F_PIPES_MASK = 0x00000011,
  F_RE = F_U1,
  F_VER = F_U2
};

inline static flags_t f_check(flags_t flags, flags_t flag) { return flags & flag; }
inline static flags_t f_set(flags_t flags, flags_t flag) { return flags | flag; }
inline static flags_t f_clear(flags_t flags, flags_t flag) { return flags & ~flag; }
inline static flags_t f_toggle(flags_t flags, flags_t flag) { return flags ^ flag; }

inline static bool f_missmatch(flags_t a, flags_t b, flags_t flag) { return f_check(a, flag) != f_check(b, flag); }

inline static uint8_t f_pipes(flags_t flags) { return (flags & 0b00000011); }
inline static uint8_t f_getpipes(flags_t flags) { return f_pipes(flags) + 1; }
inline static uint8_t f_setpipes(uint8_t num_pipes) { return num_pipes - 1; }

// Maximum number of quantizers (exponents) we can handle
static constexpr uint32_t ALLREDUCE_QUANTS = 4;
static constexpr uint32_t ALLREDUCE_QUANTS_WIDTH = 1; // bytes

// Maximum values we can (safely) fit in a standard Ethernet frame
// In most cases we do not use more than 256 which is the max for Tofino1
static constexpr uint32_t ALLREDUCE_MAX_VALUES = 256; // + 32; // 1152 bytes

static constexpr uint32_t ALLREDUCE_MAX_VALUE_WIDTH = 4; // bytes
static constexpr uint32_t ALLREDUCE_MAX_PAYLOAD_SIZE = ALLREDUCE_MAX_VALUES * ALLREDUCE_MAX_VALUE_WIDTH;

struct world_t {
  uint8_t n = 0; // 1 byte
  uint8_t k = 0; // 1 byte
} __attribute__((packed));

struct qvcnt_t {
  uint8_t q = 0; // 1 byte
  uint8_t v = 0; // 1 byte
} __attribute__((packed));

struct AllReduceHeader {
  uint32_t sessid;    // 4 bytes
  uint32_t operid;    // 4 bytes - unused by device
  uint32_t seqnum;    // 4 bytes
  uint32_t bitmap;    // 4 bytes
  uint32_t offset;    // 4 bytes - unused by device
  uint16_t slotid;    // 2 bytes
  uint8_t n;          // 1 byte
  uint8_t k;          // 1 byte
  flags_t flags;      // 1 byte
  uint8_t unused : 8; // 1 byte
  uint16_t counts;    // 2 bytes(4+12) - unused by device
  uint32_t quants;    // 4 bytes

  inline void qvcount(uint16_t qvcount) { counts = qvcount; }
  inline void qvcount(uint8_t q, uint16_t v) { counts = AllReduceHeader::getqvcount(q, v); }
  inline uint8_t qcount() const { return AllReduceHeader::getqcount(counts); }
  inline int16_t vcount() const { return AllReduceHeader::getvcount(counts); }

  inline uint8_t pipes() const { return f_getpipes(flags); }
  inline bool bad() const { return f_check(flags, F_BA); }
  inline bool old() const { return f_check(flags, F_OL); }
  inline bool syn() const { return f_check(flags, F_SY); }
  inline bool cntk() const { return f_check(flags, F_CK); }
  inline bool re() const { return f_check(flags, F_RE); }

  static uint8_t getqcount(uint16_t qvcount) { return (qvcount >> 12); }
  static uint16_t getvcount(uint16_t qvcount) { return qvcount & 0xfff; }
  static uint16_t getqvcount(uint8_t q, uint16_t v) { return (v & 0xFFF) | ((uint16_t)(q & 0x0F) << 12); }
} __attribute__((packed)); // 32 bytes;

struct AllReducePacket : public AllReduceHeader {
public:
  using header_t = AllReduceHeader;
  AllReduceHeader *header() { return this; }
  inline uint32_t *payload() {
    return reinterpret_cast<uint32_t *>(reinterpret_cast<char *>(this) + sizeof(AllReduceHeader));
  }
  inline uint32_t const *payload() const {
    return reinterpret_cast<uint32_t const *>(reinterpret_cast<char const *>(this) + sizeof(AllReduceHeader));
  }
  inline uint32_t payloadLen(DeviceOptions const &dev) const { return pipes() * dev.valuesPerPipe(); }
  inline uint32_t payloadSize(DeviceOptions const &dev) const { return pipes() * dev.valuesPerPipe() * 4; }
  inline uint32_t size(DeviceOptions const &dev) const { return sizeof(AllReduceHeader) + payloadSize(dev); }

  inline void htonHeader() {
    sessid = htonl(sessid);
    seqnum = htonl(seqnum);
    bitmap = htonl(bitmap);
    offset = htonl(offset);
    counts = htons(counts);
    quants = htonl(quants);
  }
  inline void ntohHeader() {
    sessid = ntohl(sessid);
    seqnum = ntohl(seqnum);
    bitmap = ntohl(bitmap);
    offset = ntohl(offset);
    counts = ntohs(counts);
    quants = ntohl(quants);
  }
  static inline uint32_t maxPayloadLen(DeviceOptions const &dev) { return dev.pipes * dev.reducers * dev.reducer_mode; }
  static inline uint32_t maxPayloadSize(DeviceOptions const &dev) { return maxPayloadLen(dev) * 4; }
  static inline uint32_t maxSize(DeviceOptions const &dev) { return sizeof(AllReduceHeader) + maxPayloadSize(dev); }
} __attribute__((packed));

struct AllReducePacketRdma {
  // TODO;
};

struct AllReduceHeaderNS {
  uint32_t sessid;    // 4 bytes
  uint32_t operid;    // 4 bytes
  uint32_t bitmap;    // 4 bytes
  uint32_t offset;    // 4 bytes - unused by device
  uint16_t slotid;    // 2 bytes
  uint8_t n;          // 1 byte
  flags_t flags;      // 1 byte
  uint8_t unused;     // 1 byte
  uint16_t counts;    // 2 bytes(4+12) - unused by device
  uint32_t quants;    // 4 bytes

  // inline void setqcount(uint8_t q) { setqvcount(q, vcount()); }
  // inline void setvcount(uint16_t v) { setqvcount(qcount(), v); }
  inline void qvcount(uint16_t qvcount) { counts = qvcount; }
  inline void qvcount(uint8_t q, uint16_t v) { counts = AllReduceHeader::getqvcount(q, v); }
  inline uint8_t qcount() const { return AllReduceHeader::getqcount(counts); }
  inline int16_t vcount() const { return AllReduceHeader::getvcount(counts); }

  inline uint8_t pipes() const { return f_getpipes(flags); }
  inline bool bad() const { return f_check(flags, F_BA); }
  inline bool old() const { return f_check(flags, F_OL); }
  inline bool syn() const { return f_check(flags, F_SY); }
  inline bool cntk() const { return f_check(flags, F_CK); }
  inline bool ver() const { return f_check(flags, F_VER); }
  inline bool re() const { return f_check(flags, F_RE); }

  static uint8_t getqcount(uint16_t qvcount) { return (qvcount >> 12); }
  static uint16_t getvcount(uint16_t qvcount) { return qvcount & 0xfff; }
  static uint16_t getqvcount(uint8_t q, uint16_t v) { return (v & 0xFFF) | ((uint16_t)(q & 0x0F) << 12); }
} __attribute__((packed)); // 27 bytes;

struct AllReducePacketNS : public AllReduceHeaderNS {
public:
  using header_t = AllReduceHeaderNS;


  AllReduceHeaderNS *header() { return this; }
  inline uint32_t *payload() {
    return reinterpret_cast<uint32_t *>(reinterpret_cast<char *>(this) + sizeof(AllReduceHeaderNS));
  }
  inline uint32_t const *payload() const {
    return reinterpret_cast<uint32_t const *>(reinterpret_cast<char const *>(this) + sizeof(AllReduceHeaderNS));
  }

  inline void htonHeader() {
    sessid = htonl(sessid);
    bitmap = htonl(bitmap);
    offset = htonl(offset);
    counts = htons(counts);
    quants = htonl(quants);
  }
  inline void ntohHeader() {
    sessid = ntohl(sessid);
    bitmap = ntohl(bitmap);
    offset = ntohl(offset);
    counts = ntohs(counts);
    quants = ntohl(quants);
  }
  inline uint32_t payloadLen(DeviceOptions const &dev) const { return pipes() * dev.valuesPerPipe(); }
  inline uint32_t payloadSize(DeviceOptions const &dev) const { return pipes() * dev.valuesPerPipe() * 4; }
  inline uint32_t size(DeviceOptions const &dev) const { return sizeof(AllReduceHeader) + payloadSize(dev); }

  static inline uint32_t maxPayloadLen(DeviceOptions const &dev) { return dev.pipes * dev.reducers * dev.reducer_mode; }
  static inline uint32_t maxPayloadSize(DeviceOptions const &dev) { return maxPayloadLen(dev) * 4; }
  static inline uint32_t maxSize(DeviceOptions const &dev) { return sizeof(AllReduceHeaderNS) + maxPayloadSize(dev); }
} __attribute__((packed));

} // namespace dpa

#endif // DPA_ALLREDUCE_H
