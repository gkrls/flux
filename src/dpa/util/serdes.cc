#include "dpa/util/serdes.h"
#include "dpa/config.h"

#include <cassert>
#include <netinet/in.h>

#if DPA_AVX512_AVAILABLE || DPA_AVX2_AVAILABLE
#include <immintrin.h>
#endif

namespace dpa {
namespace serdes {

size_t htonlv(void *dst, const uint32_t *src, size_t count) {
  if (!dst || !src || !count) return 0;

  uint32_t *dst_ptr = static_cast<uint32_t *>(dst);
  const uint32_t *src_ptr = src;
  size_t i = 0;

#if DPA_AVX512_AVAILABLE
  const size_t step = 16;
  static const __m512i swap_mask =
      _mm512_broadcast_i32x4(_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
  for (; i + step <= count; i += step) {
    __m512i v = _mm512_loadu_si512(src_ptr + i);
    v = _mm512_shuffle_epi8(v, swap_mask);
    _mm512_storeu_si512(dst_ptr + i, v);
  }
#elif DPA_AVX2_AVAILABLE
  const size_t step = 8;
  static const __m256i swap_mask = _mm256_setr_epi8(
      3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
      3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  for (; i + step <= count; i += step) {
    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src_ptr + i));
    v = _mm256_shuffle_epi8(v, swap_mask);
    _mm256_storeu_si256(reinterpret_cast<__m256i *>(dst_ptr + i), v);
  }
#endif

  for (; i < count; ++i) dst_ptr[i] = __builtin_bswap32(src_ptr[i]);
  return count * sizeof(uint32_t);
}

size_t ntohlv(uint32_t *dst, const void *src, size_t count) {
  assert(dst && src && count && "invalid input");

  const uint32_t *src_ptr = static_cast<const uint32_t *>(src);
  size_t i = 0;

#if DPA_AVX512_AVAILABLE
  const size_t step = 16;
  static const __m512i shuffle_mask =
      _mm512_broadcast_i32x4(_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
  for (; i + step <= count; i += step) {
    __m512i chunk = _mm512_loadu_si512(src_ptr + i);
    __m512i swapped = _mm512_shuffle_epi8(chunk, shuffle_mask);
    _mm512_storeu_si512(dst + i, swapped);
  }
#elif DPA_AVX2_AVAILABLE
  const size_t step = 8;
  const __m256i shuffle_mask = _mm256_setr_epi8(
      3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
      3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  for (; i + step <= count; i += step) {
    __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src_ptr + i));
    __m256i swapped = _mm256_shuffle_epi8(chunk, shuffle_mask);
    _mm256_storeu_si256(reinterpret_cast<__m256i *>(dst + i), swapped);
  }
#endif

  for (; i < count; ++i) dst[i] = __builtin_bswap32(src_ptr[i]);
  return count * sizeof(uint32_t);
}

size_t ntohlv_scale_to_float(float *dst, const void *src, size_t count, float scale, bool input_signed) {
  assert(dst && src && "invalid input");
  const uint32_t *src_ptr = static_cast<const uint32_t *>(src);
  size_t i = 0;

#if DPA_AVX512_AVAILABLE
  const size_t step = 16;
  const __m512i mask512 =
      _mm512_broadcast_i32x4(_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
  const __m512 scale512 = _mm512_set1_ps(scale);
  for (; i + step <= count; i += step) {
    __m512i v = _mm512_loadu_si512(src_ptr + i);
    v = _mm512_shuffle_epi8(v, mask512);
    __m512 f = input_signed ? _mm512_cvtepu32_ps(v) : _mm512_cvtepi32_ps(v);
    f = _mm512_mul_ps(f, scale512);
    _mm512_storeu_ps(dst + i, f);
  }
#elif DPA_AVX2_AVAILABLE
  const size_t step = 8;
  const __m128i m128 = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  const __m256i mask256 = _mm256_broadcastsi128_si256(m128);
  const __m256 scale256 = _mm256_set1_ps(scale);
  const __m256 two32 = _mm256_set1_ps(4294967296.0f);
  for (; i + step <= count; i += step) {
    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src_ptr + i));
    v = _mm256_shuffle_epi8(v, mask256);
    __m256 f = _mm256_cvtepi32_ps(v);
    if (input_signed) {
      __m256i neg = _mm256_cmpgt_epi32(_mm256_setzero_si256(), v);
      __m256 add = _mm256_and_ps(_mm256_castsi256_ps(neg), two32);
      f = _mm256_add_ps(f, add);
    }
    f = _mm256_mul_ps(f, scale256);
    _mm256_storeu_ps(dst + i, f);
  }
#endif

  for (; i < count; ++i) {
    uint32_t u = __builtin_bswap32(src_ptr[i]);
    float f = input_signed ? static_cast<float>(static_cast<int32_t>(u)) : static_cast<float>(u);
    dst[i] = f * scale;
  }
  return count * sizeof(float);
}

size_t ntohlv_scale(uint32_t *dst, const void *src, size_t count, uint32_t scale) {
  assert(dst && src && count && "invalid input");

  const uint32_t *src_ptr = static_cast<const uint32_t *>(src);
  size_t i = 0;

#if DPA_AVX512_AVAILABLE
  const size_t step = 16;
  const __m512i shuffle_mask =
      _mm512_broadcast_i32x4(_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
  __m512i scale_vec = _mm512_set1_epi32(scale);
  for (; i + step <= count; i += step) {
    __m512i raw = _mm512_loadu_si512(src_ptr + i);
    __m512i swapped = _mm512_shuffle_epi8(raw, shuffle_mask);
    __m512i result = _mm512_mullo_epi32(swapped, scale_vec);
    _mm512_storeu_si512(dst + i, result);
  }
#elif DPA_AVX2_AVAILABLE
  const size_t step = 8;
  const __m256i shuffle_mask = _mm256_setr_epi8(
      3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
      3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  __m256i scale_vec = _mm256_set1_epi32(scale);
  for (; i + step <= count; i += step) {
    __m256i raw = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src_ptr + i));
    __m256i swapped = _mm256_shuffle_epi8(raw, shuffle_mask);
    __m256i result = _mm256_mullo_epi32(swapped, scale_vec);
    _mm256_storeu_si256(reinterpret_cast<__m256i *>(dst + i), result);
  }
#endif

  for (; i < count; ++i) dst[i] = __builtin_bswap32(src_ptr[i]) * scale;
  return count * sizeof(uint32_t);
}

} // namespace serdes
} // namespace dpa