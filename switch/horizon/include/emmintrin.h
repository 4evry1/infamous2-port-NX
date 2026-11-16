/* Horizon layer: the two SSE2 calls the port's own (non-lifted) code uses, with x86 results.
 * cheats.cpp computes the PPU's fctiwz through _mm_cvttsd_si32(_mm_set_sd(v)) for its always-on
 * ring fix: truncate toward zero, and 0x80000000 ("integer indefinite") for NaN and for anything
 * outside int32, which is what the caller then tests for. A plain C cast would be undefined there
 * and arm64's fcvtzs saturates instead, so the x86 rule is spelt out. */
#ifndef HZ_EMMINTRIN_H
#define HZ_EMMINTRIN_H
#include <stdint.h>
typedef struct { double hz_lo, hz_hi; } __m128d;
static inline __m128d _mm_set_sd(double v) { __m128d r = { v, 0.0 }; return r; }
static inline int32_t _mm_cvttsd_si32(__m128d a)
{
    const double v = a.hz_lo;
    if (!(v > -2147483649.0 && v < 2147483648.0)) return INT32_MIN;   /* NaN fails both tests */
    return (int32_t)v;
}
#endif
