#include <algorithm> // for std::max
#include <cassert>   // for assert
#include <cmath>     // for std::round, std::fabs, powf
#include <cstdint>
#include <cstring>
#include <netinet/in.h>

#include "dpa/config.h"
#include "dpa/allreduce.h"
#include "dpa/util/quant.h"

// TODO: Should we use __restrict__ for those functions?

namespace dpa {
namespace quant {

float scaling_factor(int8_t exponent, int8_t headroom) {
  /* Clamp to prevent overflow when converting float to int32_t.
   *
   * INT32_MAX (2147483647) cannot be exactly represented in float32 due to its
   * 24-bit mantissa limitation. When cast to float, it rounds up to 2147483648.0f,
   * which exceeds INT32_MAX and causes integer overflow during quantization.
   *
   * 2147483520 is the largest integer less than INT32_MAX that can be exactly
   * represented in float32. This is because:
   * - float32 has 24 bits of mantissa
   * - 2147483520 = 0x7FFFFE80 = 2^31 - 128
   * - This value has its low 7 bits as zero, fitting within float's precision
   *
   * Using this value prevents overflow while maintaining 99.999994% of the range.
   */
  double scale = static_cast<double>(INT32_MAX) / (headroom * powf(2, exponent));
  // const float max_safe = 2147483520.0f;
  return static_cast<float>(std::min(scale, (double)2147483520.0f));

  // Another solution: if (headroom == 1 && exponent == 0) { return return 2147483520.0f }
  // return static_cast<double>(INT32_MAX) / (headroom * powf(2, exponent));
}

float scaling_factor(int8_t exponent) {
  double scale = static_cast<double>(INT32_MAX) / powf(2, exponent);
  return static_cast<float>(std::min(scale, (double)2147483520.0f));
}

float scaling_factor_rounded(int8_t exponent, int8_t headroom) {
  double scale = std::round(static_cast<double>(INT32_MAX) / (headroom * powf(2, exponent)));
  return static_cast<float>(std::min(scale, (double)2147483520.0f));
}

float scaling_factor_rounded(int8_t exponent) {
  double scale = std::round(static_cast<double>(INT32_MAX) / powf(2, exponent));
  return static_cast<float>(std::min(scale, (double)2147483520.0f));
}

float averaging_factor(AllReduceOptions const &op, uint8_t n, uint8_t k) {
  if (op.averaging) {
    if (op.prescaled && n > k) return float(n) / float(k);
    else if (!op.prescaled) return 1.0f / float(k);
  }
  return 1.0f;
}

float averaging_factor(uint8_t averaging_amount, uint8_t prescaling_amount) {
  // std::cout << "averaging_amount: " << (int) averaging_amount << ", prescaling_amount: " << (int) prescaling_amount
  // << '\n';
  DPA_ASSERT(averaging_amount && prescaling_amount, "invalid input 1");
  DPA_ASSERT(prescaling_amount < 2 || prescaling_amount >= averaging_amount,
             "invalid input 2: average: {}, prescale: {}", averaging_amount, prescaling_amount);
  // if (prescaling_amount <= 1)
  //   return 1.0 / averaging_amount;
  // if (prescaling_amount > averaging_amount)
  //   return float(prescaling_amount) / float(averaging_amount);
  // return 1.0;
  return static_cast<float>(std::max(prescaling_amount, static_cast<uint8_t>(1))) / averaging_amount;
}

int8_t extract(uint32_t exponents, uint8_t i) {
  // Extract the exponent for block i from the 32-bit packed exponents value
  // Format: exponent3|exponent2|exponent1|exponent0
  return static_cast<int8_t>((exponents >> (i * 8)) & 0xFF);
}

static constexpr uint32_t EXPONENT_MASK = 0x7F800000; // 01111111 10000000 ...
static constexpr uint32_t MANTISSA_MASK = 0x007FFFFF; // 00000000 01111111 ...

/// @brief Fast version of unbiased_exponent that always adds 1 to the exponent
/// regardless of mantissa
/// @param v The input floating-point value
/// @return Unbiased IEEE-754 exponent e such that 2^e >= v
int8_t unbiased_exponent_fast(float v) {
  // Taken from SwitchML:
  // https://github.com/p4lang/p4app-switchML/blob/b5c74a04fa66faa82acab154a4e21c8f76d913f2/dev_root/client_lib/src/prepostprocessors/cpu_exponent_quantizer_ppp.cc#L154
  // To calculate the exponent we select the 8 bits that represent the exponent
  // field in the IEEE float representation.
  // Shift the 8 bits to start from the LSB then subtract 127 to remove the
  // exponent bias and finally add 1 because we want the exponent e to make
  // 2^e >= the actual maximum float value.
  // Basically, with this, 2^e is always larger than v, it can never be equal to
  // v because we add 1 to the result unconditionally
  int16_t raw = ((*((int32_t *)(&v)) & EXPONENT_MASK) >> 23) - 127;

  if (raw < 127) {
    // Do not add 1 if it will overflow the int8_t range
    raw += 1;
  }

  // No need to check for underflow as IEEE 754 biased exponents are 0-255,
  // so raw = biased_exponent - 127 can never be less than -127.
  return static_cast<int8_t>(raw);
}

int8_t unbiased_exponent_fast2(float v) {
  uint32_t bits = *reinterpret_cast<uint32_t *>(&v);
  int16_t stored_exp = (bits >> 23) & 0xFF;
  if (stored_exp == 0) return -126;   // zero or subnormal
  if (stored_exp == 0xFF) return 127; // infinity or NaN
  int16_t raw = stored_exp - 127;
  return static_cast<int8_t>(raw) + (raw < 127);
}

/// @brief Extract the unbiased exponent of v, precisely checking if it's a power of two
/// @param v The input floating-point value
/// @return Unbiased IEEE-754 exponent e such that 2^e >= v, with special handling for powers of 2
int8_t unbiased_exponent(float v) {
  uint32_t bits = *reinterpret_cast<uint32_t *>(&v);
  int16_t stored_exp = (bits >> 23) & 0xFF;
  bool is_power_of_two = (bits & MANTISSA_MASK) == 0; // Check mantissa

  // Optimized implementation: Calculate base exponent first
  int16_t raw = stored_exp - 127; // -126

  // Only add 1 if it's not a power of 2 and won't overflow
  if (!is_power_of_two && raw < 127) { raw += 1; }

  return static_cast<int8_t>(raw);
}

uint32_t exponents(float const *source, size_t length, size_t blocks) {
#if DPA_AVX512_AVAILABLE
  return impl::exponents_avx512(source, length, blocks);
#elif DPA_AVX2_AVAILABLE
  return impl::exponents_avx2(source, length, blocks);
#else
  return impl::exponents_seq(source, length, blocks);
#endif
}

uint32_t exponents(std::vector<float> const &src, size_t blocks) {
#if DPA_AVX512_AVAILABLE
  return impl::exponents_avx512(src.data(), src.size(), blocks);
#elif DPA_AVX2_AVAILABLE
  return impl::exponents_avx2(src.data(), src.size(), blocks);
#else
  return impl::exponents_seq(src.data(), src.size(), blocks);
#endif
}

int quantize(void *dst, float const *src, size_t n, uint32_t exponents, int8_t blocks, int8_t headroom, bool hton) {
#if DPA_AVX512_AVAILABLE
  return impl::quantize_avx512(dst, src, n, exponents, blocks, headroom, hton);
#elif DPA_AVX2_AVAILABLE
  return impl::quantize_avx2(dst, src, n, exponents, blocks, headroom, hton);
#else
  DPA_THROW("USING SEQ QUANTIZE!");
  return impl::quantize_seq(dst, src, n, exponents, blocks, headroom, hton);
#endif
}

int dequantize(float *dst, void const *src, size_t n, uint32_t exponents, int8_t blocks, int8_t headroom,
               float averaging_factor, bool ntoh) {
#if DPA_AVX512_AVAILABLE
  return impl::dequantize_avx512(dst, src, n, exponents, blocks, headroom, averaging_factor, ntoh);
#elif DPA_AVX2_AVAILABLE
  return impl::dequantize_avx2(dst, src, n, exponents, blocks, headroom, averaging_factor, ntoh);
#else
  DPA_THROW("USING SEQ DEQUANTIZE!");
  return impl::dequantize_seq(dst, src, n, exponents, blocks, headroom, averaging_factor, ntoh);
#endif
}

int quantize_direct(void *dst, float const *src, size_t n, float quant_scaling_factor, bool hton) {
#if DPA_AVX512_AVAILABLE
  return impl::quantize_avx512_direct(dst, src, n, quant_scaling_factor, hton);
#elif DPA_AVX2_AVAILABLE
  return impl::quantize_avx2_direct(dst, src, n, quant_scaling_factor, hton);
#else
  return impl::quantize_seq_direct(dst, src, n, quant_scaling_factor, hton);
#endif
}

int dequantize_direct(float *dst, void const *src, size_t n, float quant_scaling_factor, float averaging_factor,
                      bool ntoh) {
#if DPA_AVX512_AVAILABLE
  return impl::dequantize_avx512_direct(dst, src, n, quant_scaling_factor, averaging_factor, ntoh);
#elif DPA_AVX2_AVAILABLE
  return impl::dequantize_avx2_direct(dst, src, n, quant_scaling_factor, averaging_factor, ntoh);
#else
  return impl::dequantize_seq_direct(dst, src, n, quant_scaling_factor, averaging_factor, ntoh);
#endif
}

namespace impl {

// For each block, find the max absolute value, and extract its unbiased exponent
// Exponents are packed in the order: exp3|exp2|exp1|exp0 for up to 4 blocks
uint32_t exponents_seq(float const *src, size_t len, size_t blocks) {
  if (len == 0 || blocks == 0) return 0;

  uint32_t packed = 0;
  size_t block_size = len / blocks;
  size_t remainder = len % blocks;

  for (size_t block = 0; block < blocks; block++) {
    size_t current_block_size = block_size + (block < remainder ? 1 : 0);
    size_t block_start = block * block_size + std::min(block, remainder);
    size_t block_end = block_start + current_block_size;
    // Find max absolute value in block
    float max_abs = 0.0f;
    for (size_t i = block_start; i < block_end; i++) max_abs = std::max(max_abs, std::fabs(src[i]));
    int8_t exponent = unbiased_exponent(max_abs);
    packed |= (exponent & 0xFF) << (block * 8);
  }

  return packed;
}

// Sequential implementation
int quantize_seq(void *dst, float const *src, size_t n, uint32_t exponents, int8_t blocks, int8_t headroom, bool hton) {
  // printf("quantize_seq dst=%p src=%p n=%zu exponents=%u blocks=%d headroom=%d hton=%d\n", dst, src, n, exponents,
  // blocks, headroom, hton);
  if (blocks < 1 || blocks > 4 || n == 0 || headroom < 1) return -1;

  int32_t *output = static_cast<int32_t *>(dst);
  const size_t base_block_size = n / blocks;
  const size_t remainder = n % blocks;

  for (size_t block = 0; block < blocks; ++block) {
    size_t block_size = base_block_size + (block < remainder ? 1 : 0);
    size_t start = block * base_block_size + std::min(block, remainder);
    size_t end = start + block_size;

    int8_t e = extract(exponents, block);
    float quant_scaling_factor = scaling_factor(e, headroom);
    size_t i;
    if (hton)
      for (i = start; i < end; ++i)
        // output[i] = htonl((int32_t)std::round(src[i] * quant_scaling_factor));
        output[i] = htonl(
            (int32_t)std::clamp(std::nearbyint(src[i] * quant_scaling_factor), float(INT32_MIN), float(INT32_MAX)));
    else
      for (i = start; i < end; ++i)
        // output[i] = std::round(src[i] * quant_scaling_factor);
        output[i] =
            (int32_t)std::clamp(std::nearbyint(src[i] * quant_scaling_factor), float(INT32_MIN), float(INT32_MAX));
  }

  return 0;
}

int dequantize_seq(float *dst, void const *src, size_t n, uint32_t exponents, int8_t blocks, int8_t headroom,
                   float averaging_factor, bool ntoh) {
  if (blocks < 1 || blocks > 4 || n == 0 || headroom < 1 || averaging_factor == 0.0f) return -1;

  const int32_t *input = static_cast<const int32_t *>(src);
  const size_t base_block_size = n / blocks;
  const size_t remainder = n % blocks;

  for (size_t block = 0; block < blocks; ++block) {
    size_t block_size = base_block_size + (block < remainder ? 1 : 0);
    size_t start = block * base_block_size + std::min(block, remainder);
    size_t end = start + block_size;
    int8_t e = extract(exponents, block);

    // We have the quantization scaling factor Q and need to multiply each value
    // by 1/Q to dequantize This would be enough if we did not want to average
    // the values. If we want to average, we have the following cases to
    // consider:
    //   1. Values are preaveraged, i.e. scaled by 1/N
    //      a. Contributions C == N, --> Nothing to do
    //      b. Contributions C < N,  --> value * 1/Q * N/C => value * N/C/Q
    //   2. Values are not preaveraged,
    //      a. Contributions C == N, --> value * 1/Q * 1/N => value * 1/N/Q
    //      b. Contributions C < N,  --> value * 1/Q * 1/C => value * 1/C/Q
    //
    // This is what the averaging_factor arguments aims to capture.
    //   The caller should set it to:
    //       1 ==> case 1a (no rescaling) --> v * 1/Q * averaging_factor = v *
    //       averaging_factor/Q (v/Q      )
    //     N/C ==> case 1b                --> v * 1/Q * averaging_factor = v *
    //     averaging_factor/Q (v * N/C/Q) 1/C ==> cases 2a and 2b.       --> v *
    //     1/Q * averaging_factor = v * averaging_factor/Q (v/C/Q    )
    float quant_scaling_factor = scaling_factor(e, headroom);
    float scaling = averaging_factor / quant_scaling_factor; // reciprocal
    size_t i;
    if (ntoh)
      // for (i = start; i < end; ++i) dst[i] = (int32_t)ntohl(input[i]) * scaling;
      for (i = start; i < end; ++i) dst[i] = (int32_t)ntohl(input[i]) * scaling;
    else
      for (i = start; i < end; ++i) dst[i] = input[i] * scaling;
  }
  return 0;
}

// Sequential implementation of quantize with a direct scaling factor
int quantize_seq_direct(void *dst, float const *src, size_t n, float quant_scaling_factor, bool hton) {
  if (n == 0 || quant_scaling_factor == 0.0f) return -1;

  int32_t *output = static_cast<int32_t *>(dst);

  if (hton) {
    for (size_t i = 0; i < n; ++i)
      // output[i] = htonl((int32_t)std::round(src[i] * quant_scaling_factor));
      output[i] =
          htonl((int32_t)std::clamp(std::nearbyint(src[i] * quant_scaling_factor), float(INT32_MIN), float(INT32_MAX)));
  } else {
    for (size_t i = 0; i < n; ++i)
      // output[i] = std::round(src[i] * quant_scaling_factor);
      output[i] =
          (int32_t)std::clamp(std::nearbyint(src[i] * quant_scaling_factor), float(INT32_MIN), float(INT32_MAX));
  }
  return 0;
}

// Sequential implementation of dequantize with a direct scaling factor
int dequantize_seq_direct(float *dst, void const *src, size_t n, float quant_scaling_factor, float averaging_factor,
                          bool ntoh) {
  if (n == 0 || quant_scaling_factor == 0.0f || averaging_factor == 0.0f) return -1;

  const int32_t *input = static_cast<const int32_t *>(src);
  const float scaling = averaging_factor / quant_scaling_factor; // reciprocal

  if (ntoh)
    for (size_t i = 0; i < n; ++i) dst[i] = (int32_t)ntohl(input[i]) * scaling;
  else
    for (size_t i = 0; i < n; ++i) dst[i] = input[i] * scaling;

  return 0;
}

} // namespace impl

/// AVX2
#if defined(DPA_AVX2_AVAILABLE)
namespace impl {

int mm256_horizontal_max(__m256i vec) {
  __m128i lo = _mm256_castsi256_si128(vec);
  __m128i hi = _mm256_extracti128_si256(vec, 1);
  __m128i max1 = _mm_max_epi32(lo, hi);
  __m128i max2 = _mm_max_epi32(max1, _mm_shuffle_epi32(max1, _MM_SHUFFLE(1, 0, 3, 2)));
  __m128i max3 = _mm_max_epi32(max2, _mm_shuffle_epi32(max2, _MM_SHUFFLE(2, 3, 0, 1)));
  return _mm_cvtsi128_si32(max3);
}

float mm256_horizontal_max(__m256 vec) {
  // Split 256-bit vector into two 128-bit lanes

  // [A,B,C,D,E,F,G,H]
  // -->
  // [ A , B , C , D ] (lo)
  // [ E , F , G , H ] (hi)
  //   |   |   |   |
  // [ m0, m1, m2, m3 ] (max)
  // [ m1, m0, m4, m3 ] (shuffle 0b01001110)
  //   |    |   |   |
  // [ mA, mA, mB, mB ] (max)
  // [ mB, mB, mA, mA ] (shuffle 0b00011011)
  //    |   |   |   |
  // [  R,  R,  R,  R ] (max)
  // R                  (extract)

  __m128 lo = _mm256_castps256_ps128(vec);
  __m128 hi = _mm256_extractf128_ps(vec, 1);
  __m128 max1 = _mm_max_ps(lo, hi);
  __m128 max2 = _mm_max_ps(max1, _mm_shuffle_ps(max1, max1, _MM_SHUFFLE(1, 0, 3, 2))); // 0b01001110
  __m128 max3 = _mm_max_ps(max2, _mm_shuffle_ps(max2, max2, _MM_SHUFFLE(2, 3, 0, 1))); // 0b10110001
  return _mm_cvtss_f32(max3);
}

uint32_t exponents_avx2(float const *source, size_t length, size_t blocks) {
  assert(blocks >= 1 && blocks <= 4);
  if (length == 0 || blocks == 0) return 0;

  uint32_t packed = 0;
  size_t block_size = length / blocks;
  size_t remainder = length % blocks;

  for (size_t block = 0; block < blocks; ++block) {
    size_t current_block_size = block_size + (block < remainder ? 1 : 0);
    size_t block_start = block * block_size + std::min(block, remainder);
    size_t block_end = block_start + current_block_size;

    // Find maximum absolute value using SIMD
    float max_abs = 0.0f;
    size_t i = block_start;

    __m256 max_abs_vec = _mm256_setzero_ps();
    __m256 sign_mask = _mm256_set1_ps(-0.0f); // Bit mask with sign bit set

    for (; i + 8 <= block_end; i += 8) {
      __m256 floats = _mm256_loadu_ps(source + i);
      // Get absolute values by clearing the sign bit
      __m256 abs_floats = _mm256_andnot_ps(sign_mask, floats);
      // Update max_abs_vec with the maximum absolute values
      max_abs_vec = _mm256_max_ps(max_abs_vec, abs_floats);
    }

    // Reduce the max_abs_vec to a single value
    max_abs = mm256_horizontal_max(max_abs_vec);
    for (; i < block_end; ++i) max_abs = std::max(max_abs, std::fabs(source[i]));
    int8_t expo = unbiased_exponent(max_abs);
    packed |= (expo & 0xFF) << (block * 8);
  }

  return packed;
}

int quantize_avx2(void *dst, float const *src, size_t n, uint32_t exponents, int8_t blocks, int8_t headroom,
                  bool hton) {
  if (blocks < 1 || blocks > 4 || n == 0 || headroom < 1) return -1;

  int32_t *output = static_cast<int32_t *>(dst);
  const size_t base_block_size = n / blocks;
  const size_t remainder = n % blocks;
  float scaling[4];
  for (int i = 0; i < blocks; ++i) {
    int8_t e = extract(exponents, i);
    scaling[i] = (float)scaling_factor(e, headroom);
  }

  const __m256i swap_mask = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5,
                                             4, 11, 10, 9, 8, 15, 14, 13, 12);
  const __m256 minv = _mm256_set1_ps((float)INT32_MIN);
  const __m256 maxv = _mm256_set1_ps((float)INT32_MAX);

  for (size_t block = 0; block < blocks; ++block) {
    size_t block_size = base_block_size + (block < remainder ? 1 : 0);
    size_t start = block * base_block_size + std::min(block, remainder);
    size_t end = start + block_size;

    const __m256 scaling_vec = _mm256_set1_ps(scaling[block]);

    size_t i = start;

    if (hton) {
      for (; i + 7 < end; i += 8) {
        __m256 scaled = _mm256_mul_ps(_mm256_loadu_ps(src + i), scaling_vec);
        __m256 clamped = _mm256_max_ps(minv, _mm256_min_ps(maxv, scaled));
        __m256i rounded = _mm256_cvtps_epi32(clamped);
        _mm256_storeu_si256((__m256i *)(output + i), _mm256_shuffle_epi8(rounded, swap_mask)); // store bswapped
      }
      for (; i < end; ++i)
        // output[i] = htonl((int32_t)std::round(src[i] * scaling[block]));
        output[i] =
            htonl((int32_t)std::clamp(std::nearbyint(src[i] * scaling[block]), float(INT32_MIN), float(INT32_MAX)));
    } else {
      for (; i + 7 < end; i += 8) {
        __m256 scaled = _mm256_mul_ps(_mm256_loadu_ps(src + i), scaling_vec);
        __m256 clamped = _mm256_max_ps(minv, _mm256_min_ps(maxv, scaled));
        __m256i rounded = _mm256_cvtps_epi32(clamped);
        _mm256_storeu_si256((__m256i *)(output + i), rounded);
      }
      for (; i < end; ++i)
        // output[i] = std::round(src[i] * scaling[block]);
        output[i] = (int32_t)std::clamp(std::nearbyint(src[i] * scaling[block]), float(INT32_MIN), float(INT32_MAX));
    }
  }
  return 0;
}

int dequantize_avx2(float *dst, void const *src, size_t n, uint32_t exponents, int8_t blocks, int8_t headroom,
                    float averaging_factor, bool ntoh) {
  if (blocks < 1 || blocks > 4 || n == 0 || headroom < 1 || averaging_factor == 0.0f) return -1;

  const int32_t *input = static_cast<const int32_t *>(src);
  const size_t base_block_size = n / blocks;
  const size_t remainder = n % blocks;

  float scaling[4]; // reciprocals
  for (int i = 0; i < blocks; ++i) {
    int8_t e = extract(exponents, i);
    float quant_scaling_factor = scaling_factor(e, headroom);
    scaling[i] = averaging_factor / quant_scaling_factor;
  }

  // AVX2 byte-swapping mask for network order conversion
  const __m256i swap_mask = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5,
                                             4, 11, 10, 9, 8, 15, 14, 13, 12);

  for (size_t block = 0; block < blocks; ++block) {
    size_t block_size = base_block_size + (block < remainder ? 1 : 0);
    size_t start = block * base_block_size + std::min(block, remainder);
    size_t end = start + block_size;

    const __m256 scaling_vec = _mm256_set1_ps(scaling[block]);

    size_t i = start;

    if (ntoh) {
      for (; i + 7 < end; i += 8) {
        __m256i ints = _mm256_loadu_si256((const __m256i *)(input + i));
        ints = _mm256_shuffle_epi8(ints, swap_mask);
        __m256 floats = _mm256_cvtepi32_ps(ints); // This correctly handles signed integers
        __m256 result = _mm256_mul_ps(floats, scaling_vec);
        _mm256_storeu_ps(dst + i, result);
      }
      for (; i < end; ++i) dst[i] = (int32_t)ntohl(input[i]) * scaling[block];
    } else {
      for (; i + 7 < end; i += 8) {
        __m256i ints = _mm256_loadu_si256((const __m256i *)(input + i));
        __m256 floats = _mm256_cvtepi32_ps(ints);
        __m256 result = _mm256_mul_ps(floats, scaling_vec);
        _mm256_storeu_ps(dst + i, result);
      }
      for (; i < end; ++i) dst[i] = input[i] * scaling[block];
    }
  }

  return 0;
}

// AVX2-optimized implementation of quantize with a direct scaling factor
int quantize_avx2_direct(void *dst, float const *src, size_t n, float quant_scaling_factor, bool hton) {
  if (n == 0 || quant_scaling_factor == 0.0f) return -1;

  int32_t *output = static_cast<int32_t *>(dst);

  // AVX2 constants for byteswapping
  const __m256i swap_mask = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5,
                                             4, 11, 10, 9, 8, 15, 14, 13, 12);
  const __m256 minv = _mm256_set1_ps((float)INT32_MIN);
  const __m256 maxv = _mm256_set1_ps((float)INT32_MAX);

  __m256 scaling_vec = _mm256_set1_ps(quant_scaling_factor);

  size_t i = 0;
  // 8 floats at a time using AVX2
  if (hton) {
    for (; i + 7 < n; i += 8) {
      __m256 floats = _mm256_loadu_ps(src + i);
      __m256 scaled = _mm256_mul_ps(floats, scaling_vec);
      __m256 clamped = _mm256_max_ps(minv, _mm256_min_ps(maxv, scaled));
      __m256i rounded = _mm256_cvtps_epi32(clamped);
      _mm256_storeu_si256((__m256i *)(output + i), _mm256_shuffle_epi8(rounded, swap_mask)); // store bswapped
    }
    for (; i < n; ++i)
      // output[i] = htonl((int32_t)std::round(src[i] * quant_scaling_factor));
      output[i] =
          htonl((int32_t)std::clamp(std::nearbyint(src[i] * quant_scaling_factor), float(INT32_MIN), float(INT32_MAX)));
  } else {
    for (; i + 7 < n; i += 8) {
      __m256 floats = _mm256_loadu_ps(src + i);
      __m256 scaled = _mm256_mul_ps(floats, scaling_vec);
      __m256 clamped = _mm256_max_ps(minv, _mm256_min_ps(maxv, scaled));
      __m256i rounded = _mm256_cvtps_epi32(clamped);
      _mm256_storeu_si256((__m256i *)(output + i), rounded);
    }
    for (; i < n; ++i)
      // output[i] = std::round(src[i] * quant_scaling_factor);
      output[i] =
          (int32_t)std::clamp(std::nearbyint(src[i] * quant_scaling_factor), float(INT32_MIN), float(INT32_MAX));
  }

  return 0;
}

// AVX2-optimized implementation of dequantize with a direct scaling factor
int dequantize_avx2_direct(float *dst, void const *src, size_t n, float quant_scaling_factor, float averaging_factor,
                           bool ntoh) {
  if (n == 0 || quant_scaling_factor == 0.0f || averaging_factor == 0.0f) return -1;

  // AVX2 byte-swapping mask for network order conversion
  const __m256i swap_mask = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5,
                                             4, 11, 10, 9, 8, 15, 14, 13, 12);
  const int32_t *input = static_cast<const int32_t *>(src);
  const float scaling = averaging_factor / quant_scaling_factor; // reciprocal
  const __m256 scaling_vec = _mm256_set1_ps(scaling);            // reciprocals

  size_t i = 0;

  if (ntoh) {
    for (; i + 7 < n; i += 8) {
      __m256i ints = _mm256_loadu_si256((const __m256i *)(input + i));
      ints = _mm256_shuffle_epi8(ints, swap_mask);
      __m256 float_vals = _mm256_cvtepi32_ps(ints);
      __m256 result = _mm256_mul_ps(float_vals, scaling_vec);
      _mm256_storeu_ps(dst + i, result);
    }
    for (; i < n; ++i) dst[i] = (int32_t)ntohl(input[i]) * scaling;
  } else {
    for (; i + 7 < n; i += 8) {
      __m256i ints = _mm256_loadu_si256((const __m256i *)(input + i));
      __m256 float_vals = _mm256_cvtepi32_ps(ints);
      __m256 result = _mm256_mul_ps(float_vals, scaling_vec);
      _mm256_storeu_ps(dst + i, result);
    }
    for (; i < n; ++i) dst[i] = input[i] * scaling;
  }

  return 0;
}

} // namespace impl
#endif // DPA_AVX2_AVAILABLE

/// __AVX512BW__
#if defined(DPA_AVX512_AVAILABLE) && defined(__AVX512BW__) && defined(__AVX512DQ__)
namespace impl {

uint32_t exponents_avx512(float const *source, size_t length, size_t blocks) {
  assert(blocks >= 1 && blocks <= 4);
  if (length == 0 || blocks == 0) return 0;

  uint32_t packed = 0;
  size_t block_size = length / blocks;
  size_t remainder = length % blocks;

  for (size_t block = 0; block < blocks; ++block) {
    size_t current_block_size = block_size + (block < remainder ? 1 : 0);
    size_t block_start = block * block_size + std::min(block, remainder);
    size_t block_end = block_start + current_block_size;

    float max_abs = 0.0f;
    size_t i = block_start;

    __m512 max_abs_vec = _mm512_setzero_ps();

    for (; i + 16 <= block_end; i += 16) {
      __m512 values = _mm512_loadu_ps(source + i);
      __m512 abs_values = _mm512_abs_ps(values);
      max_abs_vec = _mm512_max_ps(max_abs_vec, abs_values);
    }

    float simd_max = _mm512_reduce_max_ps(max_abs_vec);
    max_abs = std::max(max_abs, simd_max);

    if (i + 8 <= block_end) {
      __m256 avx2_max = _mm256_setzero_ps();
      __m256 sign_mask = _mm256_set1_ps(-0.0f);

      __m256 values = _mm256_loadu_ps(source + i);
      __m256 abs_values = _mm256_andnot_ps(sign_mask, values);
      avx2_max = _mm256_max_ps(avx2_max, abs_values);

      float avx2_max_val = mm256_horizontal_max(avx2_max);
      max_abs = std::max(max_abs, avx2_max_val);
      i += 8;
    }

    for (; i < block_end; ++i) { max_abs = std::max(max_abs, std::fabs(source[i])); }
    int8_t expo = unbiased_exponent(max_abs);
    packed |= (expo & 0xFF) << (block * 8);
  }

  return packed;
}

// AVX-512-optimized implementation of quantize using headroom
int quantize_avx512(void *dst, float const *src, size_t n, uint32_t exponents, int8_t blocks, int8_t headroom,
                    bool hton) {
  if (blocks < 1 || blocks > 4 || n == 0 || headroom < 1) return -1;
  int32_t *output = static_cast<int32_t *>(dst);
  const size_t base_block_size = n / blocks;
  const size_t remainder = n % blocks;

  // Pre-compute scaling factors for each block
  float scaling[4];
  for (int i = 0; i < blocks; ++i) {
    int8_t e = extract(exponents, i);
    scaling[i] = static_cast<float>(scaling_factor(e, headroom));
  }

  const __m512i swap_mask =
      _mm512_set_epi8(60, 61, 62, 63, 56, 57, 58, 59, 52, 53, 54, 55, 48, 49, 50, 51, 44, 45, 46, 47, 40, 41, 42, 43,
                      36, 37, 38, 39, 32, 33, 34, 35, 28, 29, 30, 31, 24, 25, 26, 27, 20, 21, 22, 23, 16, 17, 18, 19,
                      12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  // _mm512_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 19, 18, 17, 16, 23, 22, 21, 20, 27, 26, 25,
  //                  24, 31, 30, 29, 28, 35, 34, 33, 32, 39, 38, 37, 36, 43, 42, 41, 40, 47, 46, 45, 44, 51, 50, 49,
  //                  48, 55, 54, 53, 52, 59, 58, 57, 56, 63, 62, 61, 60);

  const __m512 minv = _mm512_set1_ps((float)INT32_MIN);
  const __m512 maxv = _mm512_set1_ps((float)INT32_MAX);

  for (size_t block = 0; block < blocks; ++block) {
    size_t block_size = base_block_size + (block < remainder ? 1 : 0);
    size_t start = block * base_block_size + std::min(block, remainder);
    size_t end = start + block_size;

    // Use the pre-computed scaling factor for this block
    __m512 scaling_vec = _mm512_set1_ps(scaling[block]);

    size_t i = start;

    // Process 16 floats at a time
    if (hton) {
      for (; i + 15 < end; i += 16) {
        __m512 floats = _mm512_loadu_ps(src + i);
        __m512 scaled = _mm512_mul_ps(floats, scaling_vec);
        __m512 clamped = _mm512_max_ps(minv, _mm512_min_ps(maxv, scaled)); // clamp
        __m512i rounded = _mm512_cvtps_epi32(clamped);
        _mm512_storeu_si512((__m512i *)(output + i), _mm512_shuffle_epi8(rounded, swap_mask)); // store bswapped
      }
      // Handle remaining elements
      for (; i < end; ++i)
        // output[i] = htonl((int32_t)std::round(src[i] * scaling[block]));
        output[i] =
            htonl((int32_t)std::clamp(std::nearbyint(src[i] * scaling[block]), float(INT32_MIN), float(INT32_MAX)));
    } else {
      for (; i + 15 < end; i += 16) {
        __m512 floats = _mm512_loadu_ps(src + i);
        __m512 scaled = _mm512_mul_ps(floats, scaling_vec);
        __m512 clamped = _mm512_max_ps(minv, _mm512_min_ps(maxv, scaled)); // clamp
        __m512i rounded = _mm512_cvtps_epi32(clamped);
        _mm512_storeu_si512((__m512i *)(output + i), rounded);
      }
      // Handle remaining elements
      for (; i < end; ++i)
        // output[i] = std::round(src[i] * scaling[block]);
        output[i] = (int32_t)std::clamp(std::nearbyint(src[i] * scaling[block]), float(INT32_MIN), float(INT32_MAX));
    }
  }

  return 0;
}

// AVX-512-optimized implementation of dequantize using headroom
int dequantize_avx512(float *dst, void const *src, size_t n, uint32_t exponents, int8_t blocks, int8_t headroom,
                      float averaging_factor, bool ntoh) {

  if (blocks < 1 || blocks > 4 || n == 0 || headroom < 1 || averaging_factor == 0.0f) return -1;

  const int32_t *input = static_cast<const int32_t *>(src);
  const size_t base_block_size = n / blocks;
  const size_t remainder = n % blocks;

  // Pre-compute inverse scaling factors for each block
  float scaling[4]; // reciprocals
  for (int i = 0; i < blocks; ++i) {
    int8_t e = extract(exponents, i);
    float quant_scaling_factor = scaling_factor(e, headroom);
    scaling[i] = averaging_factor / quant_scaling_factor; // Include averaging_factor here
  }

  // AVX-512 byte-swapping mask for network order conversion
  const __m512i byte_swap_mask =
      _mm512_set_epi8(60, 61, 62, 63, 56, 57, 58, 59, 52, 53, 54, 55, 48, 49, 50, 51, 44, 45, 46, 47, 40, 41, 42, 43,
                      36, 37, 38, 39, 32, 33, 34, 35, 28, 29, 30, 31, 24, 25, 26, 27, 20, 21, 22, 23, 16, 17, 18, 19,
                      12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  // _mm512_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 19, 18, 17, 16, 23, 22, 21, 20, 27, 26, 25,
  //                  24, 31, 30, 29, 28, 35, 34, 33, 32, 39, 38, 37, 36, 43, 42, 41, 40, 47, 46, 45, 44, 51, 50, 49,
  //                  48, 55, 54, 53, 52, 59, 58, 57, 56, 63, 62, 61, 60);

  for (size_t block = 0; block < blocks; ++block) {
    size_t block_size = base_block_size + (block < remainder ? 1 : 0);
    size_t start = block * base_block_size + std::min(block, remainder);
    size_t end = start + block_size;

    __m512 scaling_vec = _mm512_set1_ps(scaling[block]);

    size_t i = start;

    if (ntoh) {
      for (; i + 15 < end; i += 16) {
        __m512i ints = _mm512_loadu_si512((const __m512i *)(input + i));
        ints = _mm512_shuffle_epi8(ints, byte_swap_mask);
        __m512 floats = _mm512_cvtepi32_ps(ints);
        __m512 result = _mm512_mul_ps(floats, scaling_vec);
        _mm512_storeu_ps(dst + i, result);
      }
      for (; i < end; ++i) dst[i] = (int32_t)ntohl(input[i]) * scaling[block];
    } else {
      for (; i + 15 < end; i += 16) {
        __m512i ints = _mm512_loadu_si512((const __m512i *)(input + i));
        __m512 floats = _mm512_cvtepi32_ps(ints);
        __m512 result = _mm512_mul_ps(floats, scaling_vec);
        _mm512_storeu_ps(dst + i, result);
      }
      for (; i < end; ++i) dst[i] = input[i] * scaling[block];
    }
  }

  return 0;
}

// AVX-512-optimized implementation of quantize with a direct scaling factor
int quantize_avx512_direct(void *dst, float const *src, size_t n, float quant_scaling_factor, bool hton) {
  if (n == 0 || quant_scaling_factor == 0.0f) return -1;

  int32_t *output = static_cast<int32_t *>(dst);
  const __m512 scaling_vec = _mm512_set1_ps(quant_scaling_factor);
  const __m512i byte_swap_mask =
      _mm512_set_epi8(60, 61, 62, 63, 56, 57, 58, 59, 52, 53, 54, 55, 48, 49, 50, 51, 44, 45, 46, 47, 40, 41, 42, 43,
                      36, 37, 38, 39, 32, 33, 34, 35, 28, 29, 30, 31, 24, 25, 26, 27, 20, 21, 22, 23, 16, 17, 18, 19,
                      12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  // _mm512_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 19, 18, 17, 16, 23, 22, 21, 20, 27, 26,
  // 25,
  //                  24, 31, 30, 29, 28, 35, 34, 33, 32, 39, 38, 37, 36, 43, 42, 41, 40, 47, 46, 45, 44, 51, 50,
  //                  49, 48, 55, 54, 53, 52, 59, 58, 57, 56, 63, 62, 61, 60);

  const __m512 minv = _mm512_set1_ps((float)INT32_MIN);
  const __m512 maxv = _mm512_set1_ps((float)INT32_MAX);
  size_t i = 0;

  if (hton) {
    for (; i + 15 < n; i += 16) {
      __m512 floats = _mm512_loadu_ps(src + i);
      __m512 scaled = _mm512_mul_ps(floats, scaling_vec);
      __m512 clamped = _mm512_max_ps(minv, _mm512_min_ps(maxv, scaled));
      __m512i rounded = _mm512_cvtps_epi32(clamped);
      _mm512_storeu_si512((__m512i *)(output + i), _mm512_shuffle_epi8(rounded, byte_swap_mask)); // store bswapped
    }
    for (; i < n; ++i)
      // output[i] = htonl((int32_t)std::round(src[i] * quant_scaling_factor));
      output[i] =
          htonl((int32_t)std::clamp(std::nearbyint(src[i] * quant_scaling_factor), float(INT32_MIN), float(INT32_MAX)));
  } else {
    for (; i + 15 < n; i += 16) {
      __m512 floats = _mm512_loadu_ps(src + i);
      __m512 scaled = _mm512_mul_ps(floats, scaling_vec);
      __m512 clamped = _mm512_max_ps(minv, _mm512_min_ps(maxv, scaled));
      __m512i rounded = _mm512_cvtps_epi32(clamped);
      _mm512_storeu_si512((__m512i *)(output + i), rounded);
    }
    for (; i < n; ++i)
      // output[i] = std::round(src[i] * quant_scaling_factor);
      output[i] =
          (int32_t)std::clamp(std::nearbyint(src[i] * quant_scaling_factor), float(INT32_MIN), float(INT32_MAX));
  }

  return 0;
}

// AVX-512-optimized implementation of dequantize with a direct scaling factor
int dequantize_avx512_direct(float *dst, void const *src, size_t n, float quant_scaling_factor, float averaging_factor,
                             bool ntoh) {
  if (n == 0 || quant_scaling_factor == 0.0f || averaging_factor == 0.0f) return -1;

  const int32_t *input = static_cast<const int32_t *>(src);
  const float scaling = averaging_factor / quant_scaling_factor;
  const __m512 scaling_vec = _mm512_set1_ps(scaling);
  const __m512i byte_swap_mask =
      _mm512_set_epi8(60, 61, 62, 63, 56, 57, 58, 59, 52, 53, 54, 55, 48, 49, 50, 51, 44, 45, 46, 47, 40, 41, 42, 43,
                      36, 37, 38, 39, 32, 33, 34, 35, 28, 29, 30, 31, 24, 25, 26, 27, 20, 21, 22, 23, 16, 17, 18, 19,
                      12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  // _mm512_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 19, 18, 17, 16, 23, 22, 21, 20, 27, 26, 25,
  //                  24, 31, 30, 29, 28, 35, 34, 33, 32, 39, 38, 37, 36, 43, 42, 41, 40, 47, 46, 45, 44, 51, 50, 49,
  //                  48, 55, 54, 53, 52, 59, 58, 57, 56, 63, 62, 61, 60);

  size_t i = 0;

  if (ntoh) {
    for (; i + 15 < n; i += 16) {
      __m512i ints = _mm512_loadu_si512((const __m512i *)(input + i));
      ints = _mm512_shuffle_epi8(ints, byte_swap_mask);
      __m512 float_vals = _mm512_cvtepi32_ps(ints);
      __m512 result = _mm512_mul_ps(float_vals, scaling_vec);
      _mm512_storeu_ps(dst + i, result);
    }
    for (; i < n; ++i) dst[i] = (int32_t)ntohl(input[i]) * scaling;

  } else {
    for (; i + 15 < n; i += 16) {
      __m512i ints = _mm512_loadu_si512((const __m512i *)(input + i));
      __m512 float_vals = _mm512_cvtepi32_ps(ints);
      __m512 result = _mm512_mul_ps(float_vals, scaling_vec);
      _mm512_storeu_ps(dst + i, result);
    }
    for (; i < n; ++i) dst[i] = input[i] * scaling;
  }

  return 0;
}

} // namespace impl
#endif // __AVX512BW__

} // namespace quant
} // namespace dpa
