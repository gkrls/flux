#ifndef DPA_UTIL_GEN_H
#define DPA_UTIL_GEN_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <random>
#include <vector>

#if defined(__AVX512F__) || defined(__AVX2__)
#include <immintrin.h>
#endif

namespace dpa {

namespace gen {
namespace detail {
inline std::atomic<uint32_t> _default_seed = 0;
constexpr size_t SIMD_THRESHOLD = 512;

// xorshift32 of simd randv

#ifdef __AVX512F__
inline void randv_avx512_f32(float *dst, size_t count, uint32_t seed, float min, float max) {
  __m512i state = _mm512_set1_epi32(seed ? seed : _default_seed.load(std::memory_order_relaxed));
  const __m512 scale = _mm512_set1_ps(2.3283064e-10f); // 1/(2^32)
  const __m512 vmin = _mm512_set1_ps(min);
  const __m512 vrange = _mm512_set1_ps(max - min);

  size_t i = 0;
  for (; i + 16 <= count; i += 16) {
    state = _mm512_xor_si512(state, _mm512_slli_epi32(state, 13));
    state = _mm512_xor_si512(state, _mm512_srli_epi32(state, 17));
    state = _mm512_xor_si512(state, _mm512_slli_epi32(state, 5));

    __m512 normalized = _mm512_mul_ps(_mm512_cvtepi32_ps(state), scale);
    __m512 result = _mm512_fmadd_ps(normalized, vrange, vmin);
    _mm512_storeu_ps(&dst[i], result);
  }

  // Handle remainder
  if (i < count) {
    float temp[16];
    state = _mm512_xor_si512(state, _mm512_slli_epi32(state, 13));
    state = _mm512_xor_si512(state, _mm512_srli_epi32(state, 17));
    state = _mm512_xor_si512(state, _mm512_slli_epi32(state, 5));

    __m512 normalized = _mm512_mul_ps(_mm512_cvtepi32_ps(state), scale);
    __m512 result = _mm512_fmadd_ps(normalized, vrange, vmin);
    _mm512_storeu_ps(temp, result);

    for (size_t j = 0; j < count - i; ++j) { dst[i + j] = temp[j]; }
  }
}

inline void randv_avx512_i32(int32_t *dst, size_t count, uint32_t seed, int32_t min, int32_t max) {
  __m512i state = _mm512_set1_epi32(seed ? seed : 1);
  const uint32_t range = static_cast<uint32_t>(max - min + 1);

  size_t i = 0;
  for (; i + 16 <= count; i += 16) {
    state = _mm512_xor_si512(state, _mm512_slli_epi32(state, 13));
    state = _mm512_xor_si512(state, _mm512_srli_epi32(state, 17));
    state = _mm512_xor_si512(state, _mm512_slli_epi32(state, 5));

    int32_t temp[16];
    _mm512_storeu_si512(temp, state);
    for (int j = 0; j < 16; ++j) { dst[i + j] = min + (static_cast<uint32_t>(temp[j]) % range); }
  }

  // Handle remainder
  if (i < count) {
    state = _mm512_xor_si512(state, _mm512_slli_epi32(state, 13));
    state = _mm512_xor_si512(state, _mm512_srli_epi32(state, 17));
    state = _mm512_xor_si512(state, _mm512_slli_epi32(state, 5));

    int32_t temp[16];
    _mm512_storeu_si512(temp, state);
    for (size_t j = 0; j < count - i; ++j) { dst[i + j] = min + (static_cast<uint32_t>(temp[j]) % range); }
  }
}

inline void randv_avx512_u32(uint32_t *dst, size_t count, uint32_t seed, uint32_t min, uint32_t max) {
  __m512i state = _mm512_set1_epi32(seed ? seed : 1);
  const uint32_t range = max - min + 1;

  size_t i = 0;
  for (; i + 16 <= count; i += 16) {
    state = _mm512_xor_si512(state, _mm512_slli_epi32(state, 13));
    state = _mm512_xor_si512(state, _mm512_srli_epi32(state, 17));
    state = _mm512_xor_si512(state, _mm512_slli_epi32(state, 5));

    uint32_t temp[16];
    _mm512_storeu_si512(temp, state);
    for (int j = 0; j < 16; ++j) { dst[i + j] = min + (temp[j] % range); }
  }

  // Handle remainder
  if (i < count) {
    state = _mm512_xor_si512(state, _mm512_slli_epi32(state, 13));
    state = _mm512_xor_si512(state, _mm512_srli_epi32(state, 17));
    state = _mm512_xor_si512(state, _mm512_slli_epi32(state, 5));

    uint32_t temp[16];
    _mm512_storeu_si512(temp, state);
    for (size_t j = 0; j < count - i; ++j) { dst[i + j] = min + (temp[j] % range); }
  }
}
#endif

#ifdef __AVX2__
inline void randv_avx2_f32(float *dst, size_t count, uint32_t seed, float min, float max) {
  __m256i state = _mm256_set1_epi32(seed ? seed : 1);
  const __m256 scale = _mm256_set1_ps(2.3283064e-10f);
  const __m256 vmin = _mm256_set1_ps(min);
  const __m256 vrange = _mm256_set1_ps(max - min);

  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    state = _mm256_xor_si256(state, _mm256_slli_epi32(state, 13));
    state = _mm256_xor_si256(state, _mm256_srli_epi32(state, 17));
    state = _mm256_xor_si256(state, _mm256_slli_epi32(state, 5));

    __m256 normalized = _mm256_mul_ps(_mm256_cvtepi32_ps(state), scale);
    __m256 result = _mm256_fmadd_ps(normalized, vrange, vmin);
    _mm256_storeu_ps(&dst[i], result);
  }

  if (i < count) {
    float temp[8];
    state = _mm256_xor_si256(state, _mm256_slli_epi32(state, 13));
    state = _mm256_xor_si256(state, _mm256_srli_epi32(state, 17));
    state = _mm256_xor_si256(state, _mm256_slli_epi32(state, 5));

    __m256 normalized = _mm256_mul_ps(_mm256_cvtepi32_ps(state), scale);
    __m256 result = _mm256_fmadd_ps(normalized, vrange, vmin);
    _mm256_storeu_ps(temp, result);

    for (size_t j = 0; j < count - i; ++j) { dst[i + j] = temp[j]; }
  }
}

inline void randv_avx2_i32(int32_t *dst, size_t count, uint32_t seed, int32_t min, int32_t max) {
  __m256i state = _mm256_set1_epi32(seed ? seed : 1);
  const uint32_t range = static_cast<uint32_t>(max - min + 1);

  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    state = _mm256_xor_si256(state, _mm256_slli_epi32(state, 13));
    state = _mm256_xor_si256(state, _mm256_srli_epi32(state, 17));
    state = _mm256_xor_si256(state, _mm256_slli_epi32(state, 5));

    int32_t temp[8];
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(temp), state);
    for (int j = 0; j < 8; ++j) { dst[i + j] = min + (static_cast<uint32_t>(temp[j]) % range); }
  }

  if (i < count) {
    state = _mm256_xor_si256(state, _mm256_slli_epi32(state, 13));
    state = _mm256_xor_si256(state, _mm256_srli_epi32(state, 17));
    state = _mm256_xor_si256(state, _mm256_slli_epi32(state, 5));

    int32_t temp[8];
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(temp), state);
    for (size_t j = 0; j < count - i; ++j) { dst[i + j] = min + (static_cast<uint32_t>(temp[j]) % range); }
  }
}

inline void randv_avx2_u32(uint32_t *dst, size_t count, uint32_t seed, uint32_t min, uint32_t max) {
  __m256i state = _mm256_set1_epi32(seed ? seed : 1);
  const uint32_t range = max - min + 1;

  size_t i = 0;
  for (; i + 8 <= count; i += 8) {
    state = _mm256_xor_si256(state, _mm256_slli_epi32(state, 13));
    state = _mm256_xor_si256(state, _mm256_srli_epi32(state, 17));
    state = _mm256_xor_si256(state, _mm256_slli_epi32(state, 5));

    uint32_t temp[8];
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(temp), state);
    for (int j = 0; j < 8; ++j) { dst[i + j] = min + (temp[j] % range); }
  }

  if (i < count) {
    state = _mm256_xor_si256(state, _mm256_slli_epi32(state, 13));
    state = _mm256_xor_si256(state, _mm256_srli_epi32(state, 17));
    state = _mm256_xor_si256(state, _mm256_slli_epi32(state, 5));

    uint32_t temp[8];
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(temp), state);
    for (size_t j = 0; j < count - i; ++j) { dst[i + j] = min + (temp[j] % range); }
  }
}
#endif

} // namespace detail

inline int seed(int n = 0) { return detail::_default_seed.exchange(n); }

template <typename T> void randv(std::vector<T> &dst, T min, T max, size_t count, uint32_t seed = 0) {
  if (count > dst.size()) dst.resize(count);
  
  // Use SIMD for large vectors
  if (count >= detail::SIMD_THRESHOLD) {
#ifdef __AVX512F__
    if constexpr (std::is_same_v<T, float>) {
      detail::randv_avx512_f32(dst.data(), count, seed, min, max);
      return;
    } else if constexpr (std::is_same_v<T, int32_t>) {
      detail::randv_avx512_i32(dst.data(), count, seed, min, max);
      return;
    } else if constexpr (std::is_same_v<T, uint32_t>) {
      detail::randv_avx512_u32(dst.data(), count, seed, min, max);
      return;
    }
#elif defined(__AVX2__)
    if constexpr (std::is_same_v<T, float>) {
      detail::randv_avx2_f32(dst.data(), count, seed, min, max);
      return;
    } else if constexpr (std::is_same_v<T, int32_t>) {
      detail::randv_avx2_i32(dst.data(), count, seed, min, max);
      return;
    } else if constexpr (std::is_same_v<T, uint32_t>) {
      detail::randv_avx2_u32(dst.data(), count, seed, min, max);
      return;
    }
#endif
  }
  
  // Standard fallback
  static std::random_device rd;
  static std::minstd_rand gen(rd());
  if (seed) gen.seed(seed);
  if constexpr (std::is_floating_point_v<T>) {
    std::uniform_real_distribution<T> dis(min, max);
    std::generate(dst.begin(), dst.begin() + count, [&]() { return dis(gen); });
  } else {
    std::uniform_int_distribution<T> dis(min, max);
    std::generate(dst.begin(), dst.begin() + count, [&]() { return dis(gen); });
  }
}


// template <typename T> void randv(std::vector<T> &dst, T min, T max, size_t count, uint32_t seed = 0) {
//   if (count > dst.size()) dst.resize(count);
//   static std::random_device rd;
//   static std::minstd_rand gen(rd());
//   if (seed) gen.seed(seed);
//   if constexpr (std::is_floating_point_v<T>) {
//     std::uniform_real_distribution<T> dis(min, max);
//     std::generate(dst.begin(), dst.begin() + count, [&]() { return dis(gen); });
//   } else {
//     std::uniform_int_distribution<T> dis(min, max);
//     std::generate(dst.begin(), dst.begin() + count, [&]() { return dis(gen); });
//   }
// }

template <typename T> void stepv(std::vector<T> &dst, T start, T inc, size_t count) {
  dst.resize(count);
  dst[0] = start;
  for (auto i = 1; i < dst.size(); ++i) dst[i] = dst[i - 1] + inc;
}

} // namespace gen
} // namespace dpa

#endif // DPA_UTIL_GEN_H