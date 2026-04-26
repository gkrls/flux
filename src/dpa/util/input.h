#ifndef DPA_UTIL_INPUT_H
#define DPA_UTIL_INPUT_H

#include <cstdint>
#include <vector>

namespace dpa {
struct InputChunk {
  uint32_t lo;
  uint32_t hi;
  uint32_t packets;
  uint32_t payload;

  // static inline InputChunk get(uint32_t bucket, uint32_t buckets, uint32_t lo, uint32_t hi, uint32_t unit_size) {
  //   uint32_t sLo = hi, sHi = hi;

  //   if (bucket >= buckets or lo >= hi)
  //     return {hi, hi, 0, 0};

  //   // if (bucket < buckets) {
  //   uint32_t size = hi - lo;
  //   uint32_t unitsTot = (size + unit_size - 1) / unit_size;
  //   uint32_t unitsPer = unitsTot / buckets;
  //   uint32_t unitsRem = unitsTot % buckets;

  //   // number of units for this bucket
  //   uint32_t units = unitsPer + (bucket < unitsRem);
  //   uint32_t startUnit = bucket * unitsPer + std::min(bucket, unitsRem);
  //   // uint32_t startUnit =
  //   //     (bucket < unitsRem) ? bucket * (unitsPer + 1) : (bucket - unitsRem) * unitsPer + unitsRem * (unitsPer + 1);

  //   sLo = std::min(lo + startUnit * unit_size, hi);
  //   sHi = std::min(lo + (startUnit + units) * unit_size, hi);
  //   return {sLo, sHi, units, unit_size};
  // }

public:
  /// @brief Distribute the range [lo, hi) into buckets where each bucket
  /// contains unit-size chunks of the range
  /// The last bucket's assigned the remainder and thus its subrange may not be
  /// a multiple of the unit size.
  /// The requested bucket _can_ be empty, in which case {hi,hi,_,0} is returned
  ///
  /// Examples:
  ///   getSubrange(0, 3, 0, 10, 1) -> {0, 4, 4}   (4 elements total)
  ///   getSubrange(1, 3, 0, 10, 1) -> {4, 7, 3}   (3 elements total)
  ///   getSubrange(2, 3, 0, 10, 1) -> {7, 10, 3}  (3 elements total)
  ///
  ///   getSubrange(0, 3, 0, 100, 3) -> {0, 36, 12}   (36 elements total)
  ///   getSubrange(1, 3, 0, 100, 3) -> {36, 69, 11}  (33 elements total)
  ///   getSubrange(2, 3, 0, 100, 3) -> {69, 100, 11} (31 elements total)
  static inline InputChunk get(uint32_t bucket, uint32_t buckets, uint32_t lo, uint32_t hi, uint32_t unit_size) {
    // uint32_t sLo = hi, sHi = hi;

    if (bucket >= buckets or lo >= hi)
      return {hi, hi, 0, 0};

    // if (bucket < buckets) {
    uint32_t size = hi - lo;
    uint32_t unitsTot = (size + unit_size - 1) / unit_size;
    uint32_t unitsPer = unitsTot / buckets;
    uint32_t unitsRem = unitsTot % buckets;

    // number of units for this bucket
    uint32_t units = unitsPer + (bucket < unitsRem);
    uint32_t startUnit = bucket * unitsPer + std::min(bucket, unitsRem);
    // uint32_t startUnit =
    //     (bucket < unitsRem) ? bucket * (unitsPer + 1) : (bucket - unitsRem) * unitsPer + unitsRem * (unitsPer + 1);

    // sLo = std::min(lo + startUnit * unit_size, hi);
    // sHi = std::min(lo + (startUnit + units) * unit_size, hi);
    // return {sLo, sHi, units, unit_size};
    // sLo = std::min(lo + startUnit * unit_size, hi);
    // sHi = std::min(lo + (startUnit + units) * unit_size, hi);
    uint64_t start = lo + (uint64_t)startUnit * unit_size;
    uint64_t end   = lo + (uint64_t)(startUnit + units) * unit_size;
    lo = std::min<uint64_t>(start, hi);
    hi = std::min<uint64_t>(end, hi);
    // }
    return {lo, hi, units, unit_size};
  }
  static inline InputChunk get(uint32_t bucket, uint32_t buckets, uint32_t n, uint32_t unit) {
    return InputChunk::get(bucket, buckets, 0, n, unit);
  }
  static inline std::vector<uint32_t> offsets(uint32_t bucket, uint32_t buckets, uint32_t lo, uint32_t hi,
                                              uint32_t unit) {
    std::vector<uint32_t> res;
    for (uint32_t i = 0; i < buckets; ++i) res.push_back(InputChunk::get(i, buckets, lo, hi, unit).lo);
    return res;
  }
  static inline std::vector<uint32_t> offsets(uint32_t bucket, uint32_t buckets, uint32_t size, uint32_t unit) {
    return InputChunk::offsets(bucket, buckets, 0, size, unit);
  }
};

} // namespace dpa

#endif // DPA_UTIL_INPUT_H