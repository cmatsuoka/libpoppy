/*
 * mp7_fixed.h - fixed-point helpers for the Mini Pops library.
 *
 * This file belongs to the SHARED layer: it must stay free of any mp7_ prefix
 * so that a future instrument variant can link it unchanged. Every symbol here
 * is therefore named poppy_*.
 *
 * Number formats used by the library:
 *   Q31  int32_t, 1.0 == 0x7FFFFFFF, -1.0 == 0x80000000 (audio samples)
 *   Q30  int32_t, 1.0 == 1 << 30 (envelopes and filter coefficients)
 *   Q15  int16_t, 1.0 == 32767 (16-bit audio samples)
 *   Q16  int32_t, 1.0 == 65536 (gain *parameters*, which may exceed unity)
 *   Q8.8 fixed point for the resonance parameter
 *
 * Multiplications that cross formats go through a 64-bit intermediate.
 * Nothing here uses floating point.
 */

#ifndef MP7_FIXED_H
#define MP7_FIXED_H

#include <stdint.h>

/* Q30 is used for envelopes and filter coefficients. Unlike Q31 it can hold
 * values slightly above 1.0: the attack target is 10/9 and biquad b1/a1
 * coefficients reach about -2.0 at low frequencies. */
#define POPPY_Q30_ONE ((int32_t)1 << 30)

/* Q16 is used for gain parameters. 1.0 is exactly 65536, so unity gain is
 * exactly the identity in a multiply-and-shift-by-16. */
#define POPPY_Q16_ONE ((int32_t)1 << 16)

/* Saturating (a * b) >> 31, Q31 x Q31 -> Q31.
 *
 * The only value that does not fit in int32_t is (-1.0 * -1.0) == +1.0, which
 * is one LSB above INT32_MAX; it clamps to INT32_MAX.
 */
#define POPPY_MUL_Q31(a, b) poppy_mul_q31_sat(a, b)

/* (a * b) >> 15, Q15 x Q15 -> Q15. */
#define POPPY_MUL_Q15(a, b) ((int32_t)(((int64_t)(a) * (int64_t)(b)) >> 15))

/* Rounded shift right by 32: (x + 2^31) >> 32.
 *
 * Ties are resolved towards +infinity, which is the usual audio convention.
 */
#define POPPY_SAR32_ROUND(x) \
    ((int32_t)((((int64_t)(x)) + ((int64_t)1 << 31)) >> 32))

/* Saturating add of two Q31 values. */
static inline int32_t poppy_sat_add_q31(int32_t a, int32_t b)
{
    int64_t sum = (int64_t)a + (int64_t)b;

    if (sum > (int64_t)INT32_MAX) {
        return INT32_MAX;
    }
    if (sum < (int64_t)INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)sum;
}

/* Saturating subtract of two Q31 values. */
static inline int32_t poppy_sat_sub_q31(int32_t a, int32_t b)
{
    int64_t diff = (int64_t)a - (int64_t)b;

    if (diff > (int64_t)INT32_MAX) {
        return INT32_MAX;
    }
    if (diff < (int64_t)INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)diff;
}

/* Clamp to an inclusive range. */
static inline int32_t poppy_clamp_i32(int32_t value, int32_t low, int32_t high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

/* Exact 64-bit product of two Q31 values. */
static inline int64_t poppy_mul_i64(int32_t a, int32_t b)
{
    return (int64_t)a * (int64_t)b;
}

/* (a * b) >> shift, product computed in 64 bits.
 *
 * The right shift of a negative value is an arithmetic shift on every
 * supported toolchain (gcc, clang and the pi32v2 compiler).
 */
static inline int64_t poppy_mulshift_i64(int32_t a, int32_t b, uint32_t shift)
{
    return ((int64_t)a * (int64_t)b) >> shift;
}

/* Saturating (a * b) >> 31, Q31 x Q31 -> Q31.
 *
 * The saturation only ever fires for (-1.0 * -1.0) == +1.0, which is one LSB
 * above INT32_MAX.
 */
static inline int32_t poppy_mul_q31_sat(int32_t a, int32_t b)
{
    int64_t product = ((int64_t)a * (int64_t)b) >> 31;

    if (product > (int64_t)INT32_MAX) {
        return INT32_MAX;
    }
    if (product < (int64_t)INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)product;
}

/* Q15 x Q15 -> Q15. The result always fits in int32_t. */
static inline int32_t poppy_mul_q15(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * (int64_t)b) >> 15);
}

/* Q15 -> Q31. */
static inline int32_t poppy_q15_to_q31(int32_t q15)
{
    return (int32_t)((int64_t)q15 * 65536);
}

/* Multiply a Q31 sample by a Q16 *gain* and return the result in Q31.
 *
 * The gain is a parameter value, so it may exceed unity: Level and Volume
 * run up to 4.0, i.e. 262144 in Q16 units. A Q31 gain cannot represent that
 * (4.0 would need 33 bits), so the gain is NOT converted to Q31 first; the
 * multiply and shift are done in 64 bits and the result is a Q31 value that
 * may itself exceed 1.0. The caller must accumulate in 64 bits and
 * saturate once, at the end of the mix.
 *
 * The shift is by 16, so a gain of 1.0 (65536) is exactly the identity and
 * a gain of 4.0 is exactly 4x. The shift is arithmetic on a signed value,
 * so a negative sample stays negative.
 *
 * The sample is int64_t because it may already be outside the Q31 range:
 * the noise engine's soft clip saturates at +/-10, so a voice sample can be
 * ten times full scale before any gain is applied.
 */
static inline int64_t poppy_mul_q31_gain(int64_t sample_q31, int32_t gain_q16)
{
    return (sample_q31 * (int64_t)gain_q16) >> 16;
}

/* Q31 -> Q15, rounded and saturated. */
static inline int32_t poppy_q31_to_q15(int32_t q31)
{
    int32_t value = (int32_t)((((int64_t)q31) + 0x8000) >> 16);

    if (value > 32767) {
        return 32767;
    }
    if (value < -32768) {
        return -32768;
    }
    return value;
}

/* Saturate an int64_t to int32_t. */
static inline int32_t poppy_sat_i64_to_i32(int64_t value)
{
    if (value > (int64_t)INT32_MAX) {
        return INT32_MAX;
    }
    if (value < (int64_t)INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)value;
}

/* Q16 x Q16 -> Q16. */
static inline int32_t poppy_mul_q16(int32_t a_q16, int32_t b_q16)
{
    return (int32_t)(((int64_t)a_q16 * (int64_t)b_q16) >> 16);
}

/* Q30 x Q31 -> Q31. */
static inline int32_t poppy_mul_q30_q31(int32_t a_q30, int32_t b_q31)
{
    return poppy_sat_i64_to_i32(((int64_t)a_q30 * (int64_t)b_q31) >> 30);
}

/* Q31 x Q30 -> Q31. */
static inline int32_t poppy_mul_q31_q30(int32_t a_q31, int32_t b_q30)
{
    return poppy_sat_i64_to_i32(((int64_t)a_q31 * (int64_t)b_q30) >> 30);
}

/* Q30 x Q30 -> Q30. */
static inline int32_t poppy_mul_q30(int32_t a_q30, int32_t b_q30)
{
    return poppy_sat_i64_to_i32(((int64_t)a_q30 * (int64_t)b_q30) >> 30);
}

/* Saturating add of two Q30 values. */
static inline int32_t poppy_sat_add_q30(int32_t a, int32_t b)
{
    int64_t sum = (int64_t)a + (int64_t)b;

    if (sum > (int64_t)INT32_MAX) {
        return INT32_MAX;
    }
    if (sum < (int64_t)INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)sum;
}

/* Absolute value of a signed 32-bit value, without the INT32_MIN overflow. */
static inline int32_t poppy_abs_i32(int32_t value)
{
    if (value == INT32_MIN) {
        return INT32_MAX;
    }
    return (value < 0) ? -value : value;
}

/* xorshift32 PRNG. One word of state per consumer; the state must be non-zero.
 *
 * Three shifts and three XORs, no multiply. The output is uniform over the
 * full uint32_t range, so casting it to int32_t gives Q31 noise in [-1, 1). */
static inline uint32_t poppy_prng_next(uint32_t *state)
{
    uint32_t x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

#endif /* MP7_FIXED_H */
