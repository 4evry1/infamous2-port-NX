/* edge_simde.h -- the x86 intrinsic code of the natives/spu rewrites on a host without AVX2 (the Nintendo
 * Switch's aarch64), through SIMDe 0.8.2 (natives/spu/simde-0.8.2, MIT).
 *
 * Included by edge223_native.c (job216_bsp_native.c) INSTEAD of <immintrin.h> when EDGE_SIMDE is 1: on every
 * aarch64 target, and on x86 when the file is built with -DEDGE_FORCE_SIMDE=1 -DSIMDE_NO_NATIVE (the check
 * that the SIMDe path is bit-exact: tools/jobreplay.sh with an exe built that way). On a native AVX2 build
 * (the PC) this header is not read at all.
 *
 * What it does, and why each piece keeps the SPU's single-precision results:
 *  1. Maps every x86 intrinsic name the rewrites use onto SIMDe's simde_mm* of the same name, and the
 *     __m128i / __m256i / __m256 types onto SIMDe's. Done with plain macros after spu_helpers.h (which on x86
 *     uses the real immintrin.h types) instead of SIMDE_ENABLE_NATIVE_ALIASES, so the same text works in the
 *     x86 test build, where both worlds exist.
 *  2. Fused multiply-add: SIMDe's _mm256_fmadd_ps is fused on NEON only if __ARM_FEATURE_FMA is defined, its
 *     _mm256_fmsub_ps / _mm256_fnmadd_ps and the whole 512-bit family are an unfused multiply and add on
 *     every non-x86 target, and with SIMDE_NO_NATIVE the fmadd is too. The SPU's fma is one rounding, so the
 *     six ops are replaced here by clang's __builtin_elementwise_fma (llvm.fma: always fused; fmla on
 *     aarch64, vfmadd on x86). fmsub(a, b, c) = fma(a, b, -c) and fnmadd(a, b, c) = fma(-a, b, c): the
 *     negation is exact, so the single rounding is the instruction's.
 *  3. MXCSR -> FPCR. The x86 kernels run their fast paths at CSR_FAST (round toward zero, flush-to-zero,
 *     denormals-are-zero: the SPU's single precision) and restore the caller's MXCSR around the exact helper
 *     code. On aarch64 the SPU's single precision is FPCR round-toward-zero + FZ (FZ flushes denormal
 *     operands and results both), which every lifted SPU function already runs in (spu_float_mode_enter,
 *     spu_channels.c; the TU is built with SPU_FLOAT_RZ_ONLY, whose helpers assume it). So CSR_FAST is that
 *     FPCR value, edge_getcsr reads FPCR, and edge_setcsr writes it only when it differs (in a normal run it
 *     never does: the register is not touched). SIMDe's own _mm_setcsr / _mm_getcsr keep only the rounding
 *     mode and are not used. There are no sticky-flag reads in edge223_native.c (the fast paths bound their
 *     inputs instead), so FPSR is not read.
 *  4. __rdtsc (PS3RECOMP_EDGE_TSC statistics only) -> the virtual-count register the Horizon user mode can
 *     read (cntpct_el0: cntvct_el0 faults there); _mm_pause -> yield.
 */
#ifndef EDGE_SIMDE_H
#define EDGE_SIMDE_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#if !defined(__aarch64__)
#include <immintrin.h>          /* x86 forced-SIMDe test build: MXCSR and rdtsc stay native */
#endif

/* The names SIMDe's headers may not have to hand: only the layers the rewrites use. */
#include "simde-0.8.2/simde/x86/sse4.1.h"
#include "simde-0.8.2/simde/x86/avx2.h"
#include "simde-0.8.2/simde/x86/fma.h"

/* ---------------------------------------------------------------- the FP control register */
#if defined(__aarch64__)
#define EDGE_A64 1
#define EDGE_CSR_FAST 0x01C00000u   /* FPCR: FZ (bit 24) | RMode = toward zero (bits 23:22), no trapping, DN off */
static inline __attribute__((always_inline)) uint32_t edge_getcsr(void)
{
    uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return (uint32_t)v;
}
static inline __attribute__((always_inline)) void edge_setcsr(uint32_t v)
{
    if (edge_getcsr() != v) __asm__ volatile("msr fpcr, %0" : : "r"((uint64_t)v) : "memory");
}
static inline unsigned long long edge_rdtsc(void)
{
    uint64_t v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v;
}
static inline void edge_pause(void) { __asm__ volatile("yield" ::: "memory"); }
#else
#define EDGE_A64 0
static inline uint32_t edge_getcsr(void) { return _mm_getcsr(); }
static inline void edge_setcsr(uint32_t v) { _mm_setcsr(v); }
static inline unsigned long long edge_rdtsc(void) { return __rdtsc(); }
static inline void edge_pause(void) { _mm_pause(); }
#endif
#define _mm_getcsr edge_getcsr
#define _mm_setcsr edge_setcsr
#define __rdtsc    edge_rdtsc
#undef _mm_pause    /* the Switch's horizon_pre.h defines it too */
#define _mm_pause  edge_pause

/* ---------------------------------------------------------------- fused multiply-add */
/* EDGE_TEST_UNFUSED=1 (a test knob, never for a real build): an unfused multiply and add, to show the replay
 * and the start-up self-test see the difference. */
#ifdef EDGE_TEST_UNFUSED
#define EDGE_FMA3(a, b, c) ((a) * (b) + (c))
#else
#define EDGE_FMA3(a, b, c) __builtin_elementwise_fma((a), (b), (c))
#endif
typedef float edge_f4 __attribute__((vector_size(16)));
typedef float edge_f8 __attribute__((vector_size(32)));
typedef float edge_f16 __attribute__((vector_size(64)));   /* used by edge_simde_512.h */
#define EDGE_FMA_FAMILY(W, T, V, VT)                                                                            \
    static inline __attribute__((always_inline)) T edge_fmadd##W(T a, T b, T c)                                  \
    { VT x, y, z; memcpy(&x, &a, sizeof x); memcpy(&y, &b, sizeof y); memcpy(&z, &c, sizeof z);                  \
      x = EDGE_FMA3(x, y, z); T r; memcpy(&r, &x, sizeof r); return r; }                         \
    static inline __attribute__((always_inline)) T edge_fmsub##W(T a, T b, T c)                                  \
    { VT x, y, z; memcpy(&x, &a, sizeof x); memcpy(&y, &b, sizeof y); memcpy(&z, &c, sizeof z);                  \
      x = EDGE_FMA3(x, y, -z); T r; memcpy(&r, &x, sizeof r); return r; }                        \
    static inline __attribute__((always_inline)) T edge_fnmadd##W(T a, T b, T c)                                 \
    { VT x, y, z; memcpy(&x, &a, sizeof x); memcpy(&y, &b, sizeof y); memcpy(&z, &c, sizeof z);                  \
      x = EDGE_FMA3(-x, y, z); T r; memcpy(&r, &x, sizeof r); return r; }
EDGE_FMA_FAMILY(128, simde__m128, , edge_f4)
EDGE_FMA_FAMILY(256, simde__m256, , edge_f8)
/* the 512-bit family: edge_simde_512.h, once SIMDe's avx512 types are in (edge223_native.c) */
#define _mm_fmadd_ps     edge_fmadd128
#define _mm_fnmadd_ps    edge_fnmadd128
#define _mm256_fmadd_ps  edge_fmadd256
#define _mm256_fmsub_ps  edge_fmsub256
#define _mm256_fnmadd_ps edge_fnmadd256

/* ---------------------------------------------------------------- x86 names -> SIMDe */
#define __m128i simde__m128i
#define __m256i simde__m256i
#define __m256  simde__m256
#define __m128  simde__m128
#undef _mm256_add_epi32
#define _mm256_add_epi32             simde_mm256_add_epi32
#undef _mm256_add_ps
#define _mm256_add_ps                simde_mm256_add_ps
#undef _mm256_and_si256
#define _mm256_and_si256             simde_mm256_and_si256
#undef _mm256_andnot_si256
#define _mm256_andnot_si256          simde_mm256_andnot_si256
#undef _mm256_blend_epi32
#define _mm256_blend_epi32           simde_mm256_blend_epi32
#undef _mm256_blendv_epi8
#define _mm256_blendv_epi8           simde_mm256_blendv_epi8
#undef _mm256_castps_si256
#define _mm256_castps_si256          simde_mm256_castps_si256
#undef _mm256_castsi128_si256
#define _mm256_castsi128_si256       simde_mm256_castsi128_si256
#undef _mm256_castsi256_ps
#define _mm256_castsi256_ps          simde_mm256_castsi256_ps
#undef _mm256_castsi256_si128
#define _mm256_castsi256_si128       simde_mm256_castsi256_si128
#undef _mm256_cmpeq_epi16
#define _mm256_cmpeq_epi16           simde_mm256_cmpeq_epi16
#undef _mm256_cmpeq_epi32
#define _mm256_cmpeq_epi32           simde_mm256_cmpeq_epi32
#undef _mm256_cmpgt_epi32
#define _mm256_cmpgt_epi32           simde_mm256_cmpgt_epi32
#undef _mm256_cvtepi32_ps
#define _mm256_cvtepi32_ps           simde_mm256_cvtepi32_ps
#undef _mm256_cvttps_epi32
#define _mm256_cvttps_epi32          simde_mm256_cvttps_epi32
#undef _mm256_extracti128_si256
#define _mm256_extracti128_si256     simde_mm256_extracti128_si256
#undef _mm256_i32gather_epi32
#define _mm256_i32gather_epi32       simde_mm256_i32gather_epi32
#undef _mm256_inserti128_si256
#define _mm256_inserti128_si256      simde_mm256_inserti128_si256
#undef _mm256_load_si256
#define _mm256_load_si256            simde_mm256_load_si256
#undef _mm256_loadu_si256
#define _mm256_loadu_si256           simde_mm256_loadu_si256
#undef _mm256_mask_i32gather_epi32
#define _mm256_mask_i32gather_epi32  simde_mm256_mask_i32gather_epi32
#undef _mm256_maskstore_epi32
#define _mm256_maskstore_epi32       simde_mm256_maskstore_epi32
#undef _mm256_max_epu16
#define _mm256_max_epu16             simde_mm256_max_epu16
#undef _mm256_max_epu32
#define _mm256_max_epu32             simde_mm256_max_epu32
#undef _mm256_movemask_ps
#define _mm256_movemask_ps           simde_mm256_movemask_ps
#undef _mm256_mul_ps
#define _mm256_mul_ps                simde_mm256_mul_ps
#undef _mm256_mullo_epi32
#define _mm256_mullo_epi32           simde_mm256_mullo_epi32
#undef _mm256_or_si256
#define _mm256_or_si256              simde_mm256_or_si256
#undef _mm256_permute2x128_si256
#define _mm256_permute2x128_si256    simde_mm256_permute2x128_si256
#undef _mm256_permutevar8x32_epi32
#define _mm256_permutevar8x32_epi32  simde_mm256_permutevar8x32_epi32
#undef _mm256_set1_epi16
#define _mm256_set1_epi16            simde_mm256_set1_epi16
#undef _mm256_set1_epi32
#define _mm256_set1_epi32            simde_mm256_set1_epi32
#undef _mm256_set1_ps
#define _mm256_set1_ps               simde_mm256_set1_ps
#undef _mm256_setr_epi16
#define _mm256_setr_epi16            simde_mm256_setr_epi16
#undef _mm256_setr_epi32
#define _mm256_setr_epi32            simde_mm256_setr_epi32
#undef _mm256_setr_epi8
#define _mm256_setr_epi8             simde_mm256_setr_epi8
#undef _mm256_setzero_si256
#define _mm256_setzero_si256         simde_mm256_setzero_si256
#undef _mm256_shuffle_epi32
#define _mm256_shuffle_epi32         simde_mm256_shuffle_epi32
#undef _mm256_shuffle_epi8
#define _mm256_shuffle_epi8          simde_mm256_shuffle_epi8
#undef _mm256_slli_epi32
#define _mm256_slli_epi32            simde_mm256_slli_epi32
#undef _mm256_sllv_epi32
#define _mm256_sllv_epi32            simde_mm256_sllv_epi32
#undef _mm256_srai_epi32
#define _mm256_srai_epi32            simde_mm256_srai_epi32
#undef _mm256_srav_epi32
#define _mm256_srav_epi32            simde_mm256_srav_epi32
#undef _mm256_srli_epi32
#define _mm256_srli_epi32            simde_mm256_srli_epi32
#undef _mm256_store_si256
#define _mm256_store_si256           simde_mm256_store_si256
#undef _mm256_storeu_si256
#define _mm256_storeu_si256          simde_mm256_storeu_si256
#undef _mm256_sub_epi32
#define _mm256_sub_epi32             simde_mm256_sub_epi32
#undef _mm256_sub_ps
#define _mm256_sub_ps                simde_mm256_sub_ps
#undef _mm256_testz_si256
#define _mm256_testz_si256           simde_mm256_testz_si256
#undef _mm256_unpackhi_epi32
#define _mm256_unpackhi_epi32        simde_mm256_unpackhi_epi32
#undef _mm256_unpackhi_epi64
#define _mm256_unpackhi_epi64        simde_mm256_unpackhi_epi64
#undef _mm256_unpacklo_epi32
#define _mm256_unpacklo_epi32        simde_mm256_unpacklo_epi32
#undef _mm256_unpacklo_epi64
#define _mm256_unpacklo_epi64        simde_mm256_unpacklo_epi64
#undef _mm256_xor_si256
#define _mm256_xor_si256             simde_mm256_xor_si256
#undef _mm256_zextsi128_si256
#define _mm256_zextsi128_si256       simde_mm256_zextsi128_si256
#undef _mm_add_epi16
#define _mm_add_epi16                simde_mm_add_epi16
#undef _mm_add_epi8
#define _mm_add_epi8                 simde_mm_add_epi8
#undef _mm_and_si128
#define _mm_and_si128                simde_mm_and_si128
#undef _mm_blend_epi32
#define _mm_blend_epi32              simde_mm_blend_epi32
#undef _mm_blendv_epi8
#define _mm_blendv_epi8              simde_mm_blendv_epi8
#undef _mm_cmpgt_epi8
#define _mm_cmpgt_epi8               simde_mm_cmpgt_epi8
#undef _mm_cvtsi128_si32
#define _mm_cvtsi128_si32            simde_mm_cvtsi128_si32
#undef _mm_extract_epi32
#define _mm_extract_epi32            simde_mm_extract_epi32
#undef _mm_load_si128
#define _mm_load_si128               simde_mm_load_si128
#undef _mm_loadu_si128
#define _mm_loadu_si128              simde_mm_loadu_si128
#undef _mm_max_epu16
#define _mm_max_epu16                simde_mm_max_epu16
#undef _mm_minpos_epu16
#define _mm_minpos_epu16             simde_mm_minpos_epu16
#undef _mm_or_si128
#define _mm_or_si128                 simde_mm_or_si128
#undef _mm_set1_epi16
#define _mm_set1_epi16               simde_mm_set1_epi16
#undef _mm_set1_epi32
#define _mm_set1_epi32               simde_mm_set1_epi32
#undef _mm_set1_epi8
#define _mm_set1_epi8                simde_mm_set1_epi8
#undef _mm_setr_epi32
#define _mm_setr_epi32               simde_mm_setr_epi32
#undef _mm_setr_epi8
#define _mm_setr_epi8                simde_mm_setr_epi8
#undef _mm_setzero_si128
#define _mm_setzero_si128            simde_mm_setzero_si128
#undef _mm_shuffle_epi32
#define _mm_shuffle_epi32            simde_mm_shuffle_epi32
#undef _mm_shuffle_epi8
#define _mm_shuffle_epi8             simde_mm_shuffle_epi8
#undef _mm_store_si128
#define _mm_store_si128              simde_mm_store_si128
#undef _mm_storel_epi64
#define _mm_storel_epi64             simde_mm_storel_epi64
#undef _mm_storeu_si128
#define _mm_storeu_si128             simde_mm_storeu_si128
#undef _mm_storeu_si32
#define _mm_storeu_si32              simde_mm_storeu_si32
#undef _mm_sub_epi16
#define _mm_sub_epi16                simde_mm_sub_epi16
#undef _mm_xor_si128
#define _mm_xor_si128                simde_mm_xor_si128

/* job216_bsp_native.c's further names */
#undef _mm_add_epi32
#define _mm_add_epi32     simde_mm_add_epi32
#undef _mm_add_ss
#define _mm_add_ss        simde_mm_add_ss
#undef _mm_andnot_si128
#define _mm_andnot_si128  simde_mm_andnot_si128
#undef _mm_castps_si128
#define _mm_castps_si128  simde_mm_castps_si128
#undef _mm_castsi128_ps
#define _mm_castsi128_ps  simde_mm_castsi128_ps
#undef _mm_cmpeq_epi32
#define _mm_cmpeq_epi32   simde_mm_cmpeq_epi32
#undef _mm_comigt_ss
#define _mm_comigt_ss     simde_mm_comigt_ss
#undef _mm_insert_epi32
#define _mm_insert_epi32  simde_mm_insert_epi32
#undef _mm_movehdup_ps
#define _mm_movehdup_ps   simde_mm_movehdup_ps
#undef _mm_movehl_ps
#define _mm_movehl_ps     simde_mm_movehl_ps
#undef _mm_movemask_ps
#define _mm_movemask_ps   simde_mm_movemask_ps
#undef _mm_mul_ps
#define _mm_mul_ps        simde_mm_mul_ps
#undef _mm_slli_epi32
#define _mm_slli_epi32    simde_mm_slli_epi32
#undef _mm_sub_epi32
#define _mm_sub_epi32     simde_mm_sub_epi32
#undef _mm_sub_ps
#define _mm_sub_ps        simde_mm_sub_ps
#undef _mm_sub_ss
#define _mm_sub_ss        simde_mm_sub_ss

/* _MM_SHUFFLE (job216_bsp_native.c): the x86 headers have it, SIMDe spells it SIMDE_MM_SHUFFLE */
#ifndef _MM_SHUFFLE
#define _MM_SHUFFLE(z, y, x, w) (((z) << 6) | ((y) << 4) | ((x) << 2) | (w))
#endif

/* The sticky-flag bracket of job216_bsp_native.c: an overflow or an invalid operation anywhere in a run abandons it.
 * x86: MXCSR is set to CSR_FAST (which clears the flags) and read back (IE bit 0, OE bit 3) before it is restored.
 * aarch64: FPSR's cumulative flags (IOC bit 0, OFC bit 2: round toward zero overflow also sets them) are cleared
 * on entry, read at the end, and the caller's FPSR is put back with its FPCR. FPCR itself is the FZ + RZ of
 * every SPU run: edge_setcsr writes it only if it differs. */
typedef struct { uint32_t csr, fpsr; } edge_fp_saved;
#if EDGE_A64
static inline __attribute__((always_inline)) void edge_fp_enter(edge_fp_saved* sv)
{
    uint64_t f; __asm__ volatile("mrs %0, fpsr" : "=r"(f));
    sv->csr = edge_getcsr(); sv->fpsr = (uint32_t)f;
    edge_setcsr(EDGE_CSR_FAST);
    __asm__ volatile("msr fpsr, xzr" ::: "memory");
}
static inline __attribute__((always_inline)) int edge_fp_bad(void)
{
    uint64_t f; __asm__ volatile("mrs %0, fpsr" : "=r"(f)); return (f & 5u) != 0;
}
static inline __attribute__((always_inline)) void edge_fp_leave(const edge_fp_saved* sv)
{
    edge_setcsr(sv->csr);
    __asm__ volatile("msr fpsr, %0" : : "r"((uint64_t)sv->fpsr) : "memory");
}
#else
static inline void edge_fp_enter(edge_fp_saved* sv) { sv->csr = edge_getcsr(); sv->fpsr = 0; edge_setcsr(0xFFC0u); }
static inline int edge_fp_bad(void) { return (edge_getcsr() & 0x9u) != 0; }
static inline void edge_fp_leave(const edge_fp_saved* sv) { edge_setcsr(sv->csr); }
#endif

/* _aligned_malloc (an MSVC CRT name the rewrites use): msvc_compat.h supplies it on a Linux x86 build; a
 * Horizon build does not include that header. */
#if !defined(_WIN32) && !defined(PS3RECOMP_MSVC_COMPAT_H) && !defined(_aligned_malloc)
static inline void* edge_aligned_malloc(size_t n, size_t a) { void* p = 0; return posix_memalign(&p, a, n) ? 0 : p; }
#define _aligned_malloc edge_aligned_malloc
#endif

#endif /* EDGE_SIMDE_H */
