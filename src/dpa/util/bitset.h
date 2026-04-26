#ifndef DPA_UTIL_BITSET_H
#define DPA_UTIL_BITSET_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace dpa {

template <size_t N> struct Bitset {
  static_assert(N == 32 || N == 64 || N == 128 || N == 256 || N == 512 || N == 1024 || N == 2048,
                "Bitset only supports 32,64,128,256,512,1024 or 2045 bits");

private:
  // using T = std::conditional_t<N == 32, uint32_t, uint64_t>;
  static constexpr size_t words = (N + 63) / 64; // 1 for 32/64, 2 for 128, 4 for 256
  uint64_t bits[words] = {};

public:
  inline void set(uint16_t idx) {
    if constexpr (N <= 64) bits[0] |= (uint64_t{1} << idx);
    else bits[idx / 64] |= (1ULL << (idx % 64));
  }

  inline void rset(uint16_t n) {
    if constexpr (N <= 64) {
      if (n > 0) bits[0] |= (1ULL << (n > N ? N : n)) - 1; // Set rightmost min(n, N) bits to 1
    } else {
      size_t full_words = n / 64;     // Number of words with all bits set to 1
      size_t remaining_bits = n % 64; // Bits in the last partial word
      for (size_t i = 0; i < full_words && i < words; ++i) {
        bits[i] = ~0ULL; // Set all 64 bits to 1 (faster than |= for full words)
      }
      if (remaining_bits > 0 && full_words < words) {
        bits[full_words] |= (1ULL << remaining_bits) - 1; // Set rightmost remaining_bits to 1
      }
    }
  }

  inline void lset(uint16_t n) {
    if constexpr (N <= 64) {
      if (n > 0) bits[0] |= ~0ULL << (N - (n > N ? N : n)); // Set leftmost min(n, N) bits to 1
    } else {
      size_t full_words = n / 64;     // Number of words with all bits set to 1
      size_t remaining_bits = n % 64; // Bits in the last partial word
      // Start from the highest word
      size_t start_word = words - 1;
      for (size_t i = 0; i < full_words && i < words; ++i) {
        bits[start_word - i] = ~0ULL; // Set all 64 bits to 1 in high words
      }
      if (remaining_bits > 0 && full_words < words) {
        size_t partial_word = start_word - full_words;
        bits[partial_word] |= ~0ULL << (64 - remaining_bits); // Set leftmost remaining_bits to 1
      }
    }
  }

  inline void rclear(uint16_t n) {
    if constexpr (N <= 64) {
      if (n > 0) bits[0] &= ~((1ULL << (n > N ? N : n)) - 1); // Clear rightmost min(n, N) bits
    } else {
      size_t full_words = n / 64;     // Number of words to clear fully
      size_t remaining_bits = n % 64; // Bits in the last partial word
      for (size_t i = 0; i < full_words && i < words; ++i) {
        bits[i] = 0ULL; // Clear all 64 bits
      }
      if (remaining_bits > 0 && full_words < words) {
        bits[full_words] &= ~((1ULL << remaining_bits) - 1); // Clear rightmost remaining_bits
      }
    }
  }

  inline void lclear(uint16_t n) {
    if constexpr (N <= 64) {
      if (n > 0) bits[0] &= ~(~0ULL << (N - (n > N ? N : n))); // Clear leftmost min(n, N) bits
    } else {
      size_t full_words = n / 64;     // Number of words to clear fully
      size_t remaining_bits = n % 64; // Bits in the last partial word
      size_t start_word = words - 1;
      for (size_t i = 0; i < full_words && i < words; ++i) {
        bits[start_word - i] = 0ULL; // Clear all 64 bits in high words
      }
      if (remaining_bits > 0 && full_words < words) {
        size_t partial_word = start_word - full_words;
        bits[partial_word] &= ~(~0ULL << (64 - remaining_bits)); // Clear leftmost remaining_bits
      }
    }
  }

  inline void clear() { zeros(); }

  inline void clear(uint16_t idx) {
    if constexpr (N <= 64) bits[0] &= ~(uint64_t{1} << idx);
    else bits[idx / 64] &= ~(1ULL << (idx % 64));
  }

  inline bool check(uint16_t idx) const {
    if constexpr (N <= 64) return (bits[0] & (uint64_t{1} << idx)) != 0;
    else return (bits[idx / 64] & (1ULL << (idx % 64))) != 0;
  }

  inline bool empty() const {
    if constexpr (N <= 64) return bits[0] == 0;
    else
      for (size_t i = 0; i < words; ++i)
        if (bits[i] != 0) return false;
    return true;
  }

  inline void ones() {
    if constexpr (N <= 64) bits[0] = ~0ULL;
    else
      for (size_t i = 0; i < words; ++i) bits[i] = ~0ULL;
  }

  inline void zeros() {
    if constexpr (N <= 64) bits[0] = 0ULL;
    else
      for (size_t i = 0; i < words; ++i) bits[i] = 0ULL;
  }

  inline uint16_t count() const {
    uint16_t total = 0;
    if constexpr (N <= 64) total = __builtin_popcountll(bits[0]);
    else
      for (size_t i = 0; i < words; ++i) { total += __builtin_popcountll(bits[i]); }
    return total;
  }

  inline std::string bitstring() const {
    std::string result;
    result.reserve(N); // Pre-allocate to avoid reallocations
    if constexpr (N <= 64) {
      for (size_t i = N; i > 0; --i) { result += (bits[0] & (1ULL << (i - 1))) ? '1' : '0'; }
    } else {
      // Iterate from MSB (highest word, highest bit) to LSB (lowest word, lowest bit)
      for (size_t i = N; i > 0; --i) {
        size_t word = (N - i) / 64; // Little-endian: bits[0] is least significant
        size_t bit = (i - 1) % 64;
        result += (bits[word] & (1ULL << bit)) ? '1' : '0';
      }
    }
    return result;
  }
};

} // namespace dpa

#endif