/* Boot check of the arm64 SPU_FLOAT_RZ forms (engine/runtime/spu/spu_helpers.h) on the real CPU.
 *
 * The lifted SPU code in this build has only the native single-precision forms: FADD / FMUL /
 * FMLA under FPCR round toward zero + FZ, standing in for the SPU's truncating arithmetic. That
 * was written on an x86 machine with no arm64 to run it on, so the console is where it gets
 * checked: before the game starts, random operand vectors (weighted toward zeros, denormals,
 * the extended range, FLT_MAX and near-cancelling pairs) go through each op and are compared,
 * lane by lane, with the exact integer lanes that do not read FPCR. Any difference means SPU
 * arithmetic in the game is wrong and the build must not be trusted. */
#include <stdint.h>
#include <stdio.h>
#include "spu_helpers.h"

static uint64_t s_seed = 0x9E3779B97F4A7C15ull;
static uint32_t next32(void) {
    s_seed ^= s_seed << 13; s_seed ^= s_seed >> 7; s_seed ^= s_seed << 17;
    return (uint32_t)(s_seed >> 16);
}
/* One operand word. Mostly ordinary values close together in exponent (so sums cancel and
 * products stay in range), with the special classes mixed in. */
static uint32_t operand(void) {
    const uint32_t r = next32(), pick = next32() % 32;
    const uint32_t sign = r & 0x80000000u, frac = r & 0x007FFFFFu;
    switch (pick) {
    case 0: return sign;                                           /* +-0 */
    case 1: return sign | frac;                                    /* exponent 0, fraction set */
    case 2: return sign | 0x7F800000u | frac;                      /* exponent 255 (extended range) */
    case 3: return sign | 0x7F7FFFFFu;                             /* FLT_MAX */
    case 4: return sign | 0x00800000u | (frac & 0xFF);             /* just above the smallest normal */
    case 5: return sign | ((1u + next32() % 40) << 23) | frac;     /* small exponents */
    case 6: return sign | ((215u + next32() % 40) << 23) | frac;   /* large exponents */
    case 7: return sign | ((120u + next32() % 16) << 23) | (frac & 0x7FF000u);   /* short fractions */
    case 8: case 9: return r;                                      /* anything */
    default: return sign | ((112u + next32() % 32) << 23) | frac;  /* around 1.0 */
    }
}

static long s_bad;
long hz_spu_spec_ok;   /* vectors whose speculative run passed its verdict */
#define s_spec_ok hz_spu_spec_ok
static void differ(const char* op, uint32_t x, uint32_t y, uint32_t z, uint32_t got, uint32_t want) {
    if (s_bad++ < 12)
        fprintf(stderr, "[hz] SPU FLOAT SELF-TEST MISMATCH %s a=%08X b=%08X c=%08X got=%08X want=%08X\n",
                op, (unsigned)x, (unsigned)y, (unsigned)z, (unsigned)got, (unsigned)want);
}

/* Returns the number of lanes that differ (0 = good); *checked gets the lanes compared. */
long hz_spu_float_selftest(long vectors, long* checked) {
    const uint64_t outer = spu__fpcr_get();
    spu__fpcr_set(outer | SPU_FPCR_RZFZ);
    s_bad = 0;
    hz_spu_spec_ok = 0;
    long n = 0;
    for (long v = 0; v < vectors; v++) {
        u128 a, b, c;
        for (int i = 0; i < 4; i++) { a._u32[i] = operand(); b._u32[i] = operand(); c._u32[i] = operand(); }
        if (v % 8 == 0) for (int i = 0; i < 4; i++)                 /* near-cancelling sums */
            b._u32[i] = (a._u32[i] ^ 0x80000000u) + (next32() % 5) - 2;
        const u128 fa = spu_fa_lean(a, b), fs = spu_fs_lean(a, b), fm = spu_fm_lean(a, b);
        const u128 fma = spu_fma_lean(a, b, c), fms = spu_fms_lean(a, b, c), fnms = spu_fnms_lean(a, b, c);
        for (int i = 0; i < 4; i++) {
            const uint32_t x = a._u32[i], y = b._u32[i], z = c._u32[i];
            uint32_t w;
            if (fa._u32[i] != (w = spu__fa_lane(x, y))) differ("fa", x, y, 0, fa._u32[i], w);
            if (fs._u32[i] != (w = spu__fa_lane(x, y ^ 0x80000000u))) differ("fs", x, y, 0, fs._u32[i], w);
            if (fm._u32[i] != (w = spu__fm_lane(x, y))) differ("fm", x, y, 0, fm._u32[i], w);
            if (fma._u32[i] != (w = spu__fma_lane(x, y, z, 0, 0))) differ("fma", x, y, z, fma._u32[i], w);
            if (fms._u32[i] != (w = spu__fma_lane(x, y, z, 0, 0x80000000u))) differ("fms", x, y, z, fms._u32[i], w);
            if (fnms._u32[i] != (w = spu__fma_lane(x, y, z, 0x80000000u, 0))) differ("fnms", x, y, z, fnms._u32[i], w);
            n += 6;
        }
        {   /* hz-0.32: csflt / cuflt, the four-lane SPU_A64_CVT forms (the spu_csflt macro) against the
             * scalar lanes ((spu_csflt) calls the function itself), over integer operands with the
             * edge values mixed in and scale fields across 0..255 (the scalar lanes take the scales
             * outside 0..155 in both forms) */
            u128 iv;
            for (int i = 0; i < 4; i++) {
                const uint32_t pick = next32() % 8;
                iv._u32[i] = pick == 0 ? 0u : pick == 1 ? 0x80000000u : pick == 2 ? 0x7FFFFFFFu : pick == 3 ? 0xFFFFFFFFu
                           : pick == 4 ? (next32() & 0xFFFFu) : pick == 5 ? (1u << (next32() % 32)) : next32();
            }
            const int i8 = (int)(next32() % 256);
            const u128 s1 = spu_csflt(iv, i8), s2 = (spu_csflt)(iv, i8);
            const u128 u1 = spu_cuflt(iv, i8), u2 = (spu_cuflt)(iv, i8);
            for (int i = 0; i < 4; i++) {
                if (s1._u32[i] != s2._u32[i]) differ("csflt", iv._u32[i], (uint32_t)i8, 0, s1._u32[i], s2._u32[i]);
                if (u1._u32[i] != u2._u32[i]) differ("cuflt", iv._u32[i], (uint32_t)i8, 0, u1._u32[i], u2._u32[i]);
                n += 2;
            }
        }
        {   /* the deferred-check forms (a "speculative run"): where the run's one verdict says
             * every op stayed on its fast path, each result must be the exact one; and the
             * verdict must not pass a lane the exact lanes handle differently, which the
             * comparison shows as a mismatch. cflts / cfltu with a few scales ride along. */
            spu_spec_acc acc = SPU_SPEC_INIT();
            const int i8 = 173 - (int)(next32() % 40);
            const u128 r[8] = { spu_fa_spec(a, b, &acc), spu_fs_spec(a, b, &acc), spu_fm_spec(a, b, &acc),
                                spu_fma_spec(a, b, c, &acc), spu_fms_spec(a, b, c, &acc), spu_fnms_spec(a, b, c, &acc),
                                spu_cflts_spec(a, i8, &acc), spu_cfltu_spec(a, i8, &acc) };
            if (SPU_SPEC_OK(acc)) {
                const u128 ts = spu_cflts_exact(a, i8), tu = spu_cfltu_exact(a, i8);
                for (int i = 0; i < 4; i++) {
                    const uint32_t x = a._u32[i], y = b._u32[i], z = c._u32[i];
                    const uint32_t want[8] = { spu__fa_lane(x, y), spu__fa_lane(x, y ^ 0x80000000u), spu__fm_lane(x, y),
                                               spu__fma_lane(x, y, z, 0, 0), spu__fma_lane(x, y, z, 0, 0x80000000u),
                                               spu__fma_lane(x, y, z, 0x80000000u, 0), ts._u32[i], tu._u32[i] };
                    static const char* const name[8] = { "fa_spec", "fs_spec", "fm_spec", "fma_spec", "fms_spec",
                                                         "fnms_spec", "cflts_spec", "cfltu_spec" };
                    for (int k = 0; k < 8; k++)
                        if (r[k]._u32[i] != want[k]) differ(name[k], x, y, z, r[k]._u32[i], want[k]);
                    n += 8;
                }
                s_spec_ok++;
            }
        }
    }
    spu__fpcr_set(outer);
    *checked = n;
    return s_bad;
}
