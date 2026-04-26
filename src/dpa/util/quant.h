#pragma once

/**
 * WARNING: THESE FUNCTIONS DO NOT HANDLE NaN OR INFINITY VALUES!
 *
 * The quantization and dequantization functions in this file assume that all
 * input values are normal or subnormal floating point numbers. Passing NaN or
 * Infinity values will result in undefined behavior. It is the caller's
 * responsibility to ensure that all inputs are valid normal or subnormal
 * floating point numbers.
 */

#include <cstdint>
#include <vector>

#if defined(__AVX2__) || defined(__AVX512F__)
#include <immintrin.h>
#endif

// #include "dpa/allreduce.h"

namespace dpa {

class AllReduceOptions;

namespace quant {

// Core utility functions
/// @brief Fast version of unbiased_exponent that always adds 1 to the exponent
/// regardless of mantissa
/// @param v The input floating-point value
/// @return Unbiased IEEE-754 exponent e such that 2^e >= v
int8_t unbiased_exponent_fast(float v);
int8_t unbiased_exponent_fast_2(float v);

/// @brief Extract the unbiased exponent of v, precisely checking if it's a
/// power of two
/// @param v The input floating-point value
/// @return Unbiased IEEE-754 exponent e such that 2^e >= v, with special
/// handling for powers of 2
int8_t unbiased_exponent(float v);

float scaling_factor(int8_t exponent);
float scaling_factor(int8_t exponent, int8_t headroom);
float scaling_factor_rounded(int8_t exponent);
float scaling_factor_rounded(int8_t exponent, int8_t headroom);

float averaging_factor(AllReduceOptions const &opt, uint8_t n, uint8_t k);
float averaging_factor(uint8_t averaging_amount, uint8_t prescaling_amount);

/// @brief Extract the i-th exponent from exponents
// int8_t extract_exponent(uint32_t exponents, uint8_t i);
int8_t extract(uint32_t exponents, uint8_t i);

/// Split src into n blocks (1 to 4) and for each block return an
/// IEEE-754 unbiased exponent e (1 byte) such that 2^e >= max(block)
/// Return the exponents as a 32-bit int e3|e2|e1|e0
/// When n < 4 or len < n, the result is zero-padded on the left
uint32_t exponents(float const *src, size_t len, size_t blocks);
uint32_t exponents(std::vector<float> const &src, size_t blocks);

/// @brief Sequential implementation of quantize using headroom instead of
/// exp_scale
/// @param dst Destination buffer for quantized values (int32_t type)
/// @param src Source buffer of floating point values to quantize
/// @param n Number of values to quantize starting from src
/// @param exponents Packed exponents for each block (format: e3|e2|e1|e0),
/// where each e is an int8_t
///                 representing the power of 2 that bounds the maximum absolute
///                 value in the block
/// @param blocks Number of blocks to use (1 to 4)
/// @param headroom Scaling factor to reserve space below INT32_MAX to prevent
/// overflow.
///                 Higher values provide more safety margin but reduce
///                 precision.
/// @param hton If true, convert to network byte order (big endian)
/// @return 0 on success, -1 on error
int quantize(void *dst, float const *src, size_t n, uint32_t exponents, int8_t blocks, int8_t headroom,
             bool hton = true);
int quantize_direct(void *dst, float const *src, size_t n, float quant_scaling_factor, bool hton = true);

/// Dequantizes values with additional rescaling to handle averaging and
/// straggler mitigation.
///
/// During dequantization, each quantized value is multiplied by 1/Q (where Q is
/// the quantization scaling factor) and then by an additional averaging_factor
/// factor to handle different averaging scenarios:
///
/// averaging_factor factor selection:
/// 1. For pre-averaged values (each divided by N during training):
///    - No stragglers (all N workers contributed): averaging_factor = 1.0
///    - With stragglers (only C < N workers contributed): averaging_factor =
///    N/C
///
/// 2. For non-averaged values (raw gradient sums):
///    - No stragglers: averaging_factor = 1/N (to get the average)
///    - With stragglers: averaging_factor = 1/C (to get the average from actual
///    contributors)
///
/// @param dst Destination buffer for dequantized values
/// @param src Source buffer containing quantized values
/// @param n Number of elements
/// @param exponents Packed exponents (8-bit per block)
/// @param blocks Number of blocks (1-4)
/// @param headroom Value used during quantization to prevent overflow
/// @param ntoh Whether to convert from network byte order
/// @param averaging_factor Additional scaling factor to apply (defaults to 1.0
/// for no rescaling)
/// @return 0 on success, negative value on error
///
int dequantize(float *dst, void const *src, size_t n, uint32_t exponents, int8_t blocks, int8_t headroom,
               float averaging_factor = 1.0f, bool ntoh = true);
int dequantize_direct(float *dst, void const *src, size_t n, float quant_scaling_factor, float averaging_factor = 1.0f,
                      bool ntoh = true);

namespace impl {

uint32_t exponents_seq(float const *src, size_t len, size_t blocks);
int quantize_seq(void *dst, float const *src, size_t n, uint32_t exponents, int8_t blocks = 1, int8_t headroom = 1,
                 bool hton = true);
int quantize_seq_direct(void *dst, float const *src, size_t n, float quant_scaling_factor, bool hton = true);
int dequantize_seq(float *dst, void const *src, size_t n, uint32_t exponents, int8_t blocks = 1, int8_t headroom = 1,
                   float averaging_factor = 1.0f, bool ntoh = true);
int dequantize_seq_direct(float *dst, void const *src, size_t n, float quant_scaling_factor,
                          float averaging_factor = 1.0f, bool ntoh = true);

} // namespace impl

#if DPA_AVX2_AVAILABLE
namespace impl {
int mm256_horizontal_max(__m256i vec);
float mm256_horizontal_max(__m256 vec);

uint32_t exponents_avx2(float const *src, size_t len, size_t blocks);
int quantize_avx2(void *dst, float const *src, size_t n, uint32_t exponents = 1, int8_t blocks = 1, int8_t headroom = 1,
                  bool hton = true);
int quantize_avx2_direct(void *dst, float const *src, size_t n, float quant_scaling_factor, bool hton);
int dequantize_avx2(float *dst, void const *src, size_t n, uint32_t exponents, int8_t blocks = 1, int8_t headroom = 1,
                    float averaging_factor = 1.0f, bool ntoh = true);
int dequantize_avx2_direct(float *dst, void const *src, size_t n, float quant_scaling_factor,
                           float averaging_factor = 1.0f, bool ntoh = true);
} // namespace impl
#endif


#if DPA_AVX512_AVAILABLE
namespace impl {
uint32_t exponents_avx512(float const *src, size_t len, size_t blocks);
int quantize_avx512(void *dst, float const *src, size_t n, uint32_t exponents = 1, int8_t blocks = 1,
                    int8_t headroom = 1, bool hton = true);
int quantize_avx512_direct(void *dst, float const *src, size_t n, float quant_scaling_factor, bool hton = true);
int dequantize_avx512(float *dst, void const *src, size_t n, uint32_t exponents, int8_t blocks = 1, int8_t headroom = 1,
                      float averaging_factor = 1.0f, bool ntoh = true);
int dequantize_avx512_direct(float *dst, void const *src, size_t n, float quant_scaling_factor,
                             float averaging_factor = 1.0f, bool ntoh = true);
} // namespace impl
#endif

} // namespace quant
} // namespace dpa
