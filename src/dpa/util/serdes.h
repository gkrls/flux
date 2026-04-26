
#ifndef DPA_UTIL_SERDES_H
#define DPA_UTIL_SERDES_H

#include <cstddef>
#include <cstdint>

namespace dpa {
namespace serdes {

/// Convert a vector from host byte order to network byte order.
/// Returns the number of bytes converted.
size_t htonlv(void *dst, const uint32_t *src, size_t count);

/// Convert a vector from network byte order to host byte order.
/// Returns the number of bytes converted.
size_t ntohlv(uint32_t *dst, const void *src, size_t count);

/// Convert a vector from network byte order to host byte order and scale it to float.
/// Returns the number of bytes written to dst.
size_t ntohlv_scale_to_float(float *dst, const void *src, size_t count, float scale, bool input_signed);

/// Convert a vector from network byte order to host byte order and scale it (integer version).
/// Returns the number of bytes converted.
size_t ntohlv_scale(uint32_t *dst, const void *src, size_t count, uint32_t scale);

/// Network-to-host + average, in float.
inline size_t ntohlv_avg(float *dst, const void *src, size_t count, float avg_amount, bool input_signed = true) {
  return ntohlv_scale_to_float(dst, src, count, 1.0f / avg_amount, input_signed);
}

} // namespace serdes
} // namespace dpa

#endif // DPA_UTIL_SERDES_H

// #ifndef DPA_UTIL_SERDES
// #define DPA_UTIL_SERDES

// #include "dpa/config.h"

// #include "dpa/util/error.h"
// #include <assert.h>
// #include <cstddef>
// #include <cstdint>
// #include <netinet/in.h>

// #if (defined(__AVX2__) || defined(__AVX512F__))
// #include <immintrin.h>
// #endif

// namespace dpa {
// namespace serdes {

// inline size_t htonlv(void *dst, const uint32_t *src, size_t count) {
//   if (!dst || !src || !count) return 0;

//   uint32_t *dst_ptr = static_cast<uint32_t *>(dst);
//   const uint32_t *src_ptr = static_cast<const uint32_t *>(src);
//   size_t i = 0;

// #if defined(__AVX512BW__)
//   const size_t step = 16; // uint32_t's per vector
//   // static const __m512i swap_mask = _mm512_setr_epi8(
//   //     3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12,
//   //     3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12,
//   //     3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12,
//   //     3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12);
//   static const __m512i swap_mask =
//       _mm512_broadcast_i32x4(_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
//   for (; i + step <= count; i += step) {
//     __m512i v = _mm512_loadu_si512((const void *)(src_ptr + i));
//     v = _mm512_shuffle_epi8(v, swap_mask);
//     _mm512_storeu_si512((void *)(dst_ptr + i), v);
//   }

// #elif defined(__AVX2__)
//   const size_t step = 8; // uint32_t's per vector
//   static const __m256i swap_mask = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7,
//                                                     6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
//   for (; i + step <= count; i += step) {
//     __m256i v = _mm256_loadu_si256((const __m256i *)(src_ptr + i));
//     v = _mm256_shuffle_epi8(v, swap_mask);
//     _mm256_storeu_si256((__m256i *)(dst_ptr + i), v);
//   }
// #else
// #warning "Using sequential htonlv"
// #endif

//   for (; i < count; ++i) dst_ptr[i] = __builtin_bswap32(src_ptr[i]);
//   return count * sizeof(uint32_t);
// }

// /// Convert a vector from network byte order to host byte order
// /// Return the number of bytes converted
// inline size_t ntohlv(uint32_t *dst, const void *src, size_t count) {
//   assert(dst && src && count && "invalid input");

//   const uint32_t *src_ptr = static_cast<const uint32_t *>(src);
//   size_t i = 0;

// #if defined(__AVX512BW__)
//   const size_t step = 16;
//   // static const __m512i shuffle_mask =
//   //     _mm512_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
//   //     3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1,
//   //                      0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1,
//   //                      0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
//   static const __m512i shuffle_mask =
//       _mm512_broadcast_i32x4(_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));

//   for (; i + step <= count; i += step) {
//     __m512i chunk = _mm512_loadu_si512((const void *)(src_ptr + i));
//     __m512i swapped = _mm512_shuffle_epi8(chunk, shuffle_mask);
//     _mm512_storeu_si512((void *)(dst + i), swapped);
//   }

// #elif defined(__AVX2__)
//   const size_t step = 8;
//   const __m256i shuffle_mask = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6,
//                                                 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);

//   for (; i + step <= count; i += step) {
//     __m256i chunk = _mm256_loadu_si256((const __m256i *)(src_ptr + i));
//     __m256i swapped = _mm256_shuffle_epi8(chunk, shuffle_mask);
//     _mm256_storeu_si256((__m256i *)(dst + i), swapped);
//   }
// #endif

//   for (; i < count; ++i) dst[i] = __builtin_bswap32(src_ptr[i]);

//   return count * sizeof(uint32_t);
// }

// /// Convert a vector from network byte order to host byte order and scale it
// inline size_t ntohlv_scale_to_float(float *dst, const void *src, size_t count, float scale, bool input_signed) {
//   assert(dst && src && "invalid input");
//   const uint32_t *src_ptr = static_cast<const uint32_t *>(src);
//   size_t i = 0;

// #if defined(__AVX512BW__) && defined(__AVX512F__)
//   const __m128i m128 = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
//   const size_t step = 16;
//   const __m512i mask512 = _mm512_broadcast_i32x4(m128);
//   const __m512 scale512 = _mm512_set1_ps(scale);
//   for (; i + step <= count; i += step) {
//     __m512i v = _mm512_loadu_si512((const void *)(src_ptr + i));
//     v = _mm512_shuffle_epi8(v, mask512);
//     __m512 f = input_signed ? _mm512_cvtepu32_ps(v) : _mm512_cvtepi32_ps(v);
//     f = _mm512_mul_ps(f, scale512);
//     _mm512_storeu_ps(dst + i, f);
//   }
// #elif defined(__AVX2__)
//   const __m128i m128 = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
//   const size_t step = 8;
//   const __m256i mask256 = _mm256_broadcastsi128_si256(m128);
//   const __m256 scale256 = _mm256_set1_ps(scale);
//   const __m256 two32 = _mm256_set1_ps(4294967296.0f); // 2^32
//   for (; i + step <= count; i += step) {
//     __m256i v = _mm256_loadu_si256((const __m256i *)(src_ptr + i));
//     v = _mm256_shuffle_epi8(v, mask256); // byte-swap within u32s
//     __m256 f = _mm256_cvtepi32_ps(v);    // signed convert
//     if (input_signed) {
//       // If the signed value is negative, it actually represents [2^31..2^32-1]
//       __m256i neg = _mm256_cmpgt_epi32(_mm256_setzero_si256(),
//                                        v); // x < 0 ? 0xFFFFFFFF : 0
//       __m256 add = _mm256_and_ps(_mm256_castsi256_ps(neg),
//                                  two32); // add 2^32 where needed
//       f = _mm256_add_ps(f, add);
//     }
//     f = _mm256_mul_ps(f, scale256);
//     _mm256_storeu_ps(dst + i, f);
//   }
// #endif

//   for (; i < count; ++i) {
//     uint32_t u = __builtin_bswap32(src_ptr[i]);
//     float f = input_signed ? (float)(int32_t)u : (float)u;
//     dst[i] = f * scale;
//   }
//   return count * sizeof(float);
// }

// /// Convert a vector from network byte order to host byte order and scale it
// /// (integer version)
// inline size_t ntohlv_scale(uint32_t *dst, const void *src, size_t count, uint32_t scale) {
//   assert(dst && src && count && "invalid input");

//   const uint32_t *src_ptr = static_cast<const uint32_t *>(src);
//   size_t i = 0;

// #if defined(__AVX512BW__)
//   const size_t step = 16;
//   // const __m512i shuffle_mask =
//   // _mm512_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3,
//   // 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1,
//   //                  0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0,
//   //                  7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
//   const __m512i shuffle_mask =
//       _mm512_broadcast_i32x4(_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
//   __m512i scale_vec = _mm512_set1_epi32(scale);

//   for (; i + step <= count; i += step) {
//     __m512i raw = _mm512_loadu_si512((const void *)(src_ptr + i));
//     __m512i swapped = _mm512_shuffle_epi8(raw, shuffle_mask);
//     __m512i result = _mm512_mullo_epi32(swapped, scale_vec);
//     _mm512_storeu_si512((void *)(dst + i), result);
//   }

// #elif defined(__AVX2__)
//   const size_t step = 8;
//   const __m256i shuffle_mask = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6,
//                                                 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
//   __m256i scale_vec = _mm256_set1_epi32(scale);

//   for (; i + step <= count; i += step) {
//     __m256i raw = _mm256_loadu_si256((const __m256i *)(src_ptr + i));
//     __m256i swapped = _mm256_shuffle_epi8(raw, shuffle_mask);
//     __m256i result = _mm256_mullo_epi32(swapped, scale_vec);
//     _mm256_storeu_si256((__m256i *)(dst + i), result);
//   }
// #endif

//   for (; i < count; ++i) dst[i] = __builtin_bswap32(src_ptr[i]) * scale;
//   return count * sizeof(uint32_t);
// }

// inline size_t ntohlv_avg(float *dst, const void *src, size_t count, float avg_amount, bool input_signed = true) {
//   return ntohlv_scale_to_float(dst, src, count, 1 / avg_amount, input_signed);
// }

// } // namespace serdes
// } // namespace dpa

// #endif