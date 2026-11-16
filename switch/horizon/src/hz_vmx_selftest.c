/* Boot check of the NEON forms of the PPU's vmaddfp / vnmsubfp (engine/runtime/ppu/ppu_vmx.h,
 * hz-0.32) against the scalar forms they replace, on the real CPU.
 *
 * The NEON forms were written on an x86 machine with no arm64 to run them on (as the SPU float
 * forms were, hz_spu_selftest.c), so the console checks them before the game starts: random
 * operand vectors, weighted toward the special classes (signed zeros, denormals, infinities, quiet
 * and signalling NaNs, FLT_MAX, values that cancel), go through both forms with VSCR[NJ] set and
 * clear, and the 16 result bytes are compared. Any difference means PPU vector arithmetic in the
 * game is wrong and the game is not started. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ppu_vmx.h"

#ifndef PPU_VMX_NEON
long hz_vmx_selftest(long vectors, long* checked) { (void)vectors; *checked = 0; return 0; }
#else
static uint64_t s_seed = 0xD1B54A32D192ED03ull;
static uint32_t next32(void) {
    s_seed ^= s_seed << 13; s_seed ^= s_seed >> 7; s_seed ^= s_seed << 17;
    return (uint32_t)(s_seed >> 16);
}
static uint32_t operand(void) {
    const uint32_t r = next32(), pick = next32() % 24;
    const uint32_t sign = r & 0x80000000u, frac = r & 0x007FFFFFu;
    switch (pick) {
    case 0: return sign;                                           /* +-0 */
    case 1: return sign | frac;                                    /* denormal (or zero) */
    case 2: return sign | 0x7F800000u;                             /* +-infinity */
    case 3: return sign | 0x7FC00000u | frac;                      /* quiet NaN */
    case 4: return sign | 0x7F800000u | (frac | 1u) >> 1;          /* signalling NaN */
    case 5: return sign | 0x7F7FFFFFu;                             /* FLT_MAX */
    case 6: return sign | 0x00800000u | (frac & 0xFF);             /* just above the smallest normal */
    case 7: return sign | ((1u + next32() % 40) << 23) | frac;     /* small exponents */
    case 8: return sign | ((200u + next32() % 54) << 23) | frac;   /* large exponents */
    default: return sign | ((120u + next32() % 16) << 23) | frac;  /* ordinary, close in exponent */
    }
}
static void put(uint8_t* v, int lane, uint32_t w) {
    v[4 * lane] = (uint8_t)(w >> 24); v[4 * lane + 1] = (uint8_t)(w >> 16);
    v[4 * lane + 2] = (uint8_t)(w >> 8); v[4 * lane + 3] = (uint8_t)w;
}
/* The reference for vnmsubfp: ppu_vmx.h's scalar form as the game's build (clang) compiles it --
 * fmaf(x, z, -y), a real call, then the negation -- so -(+0) is -0. gcc rewrites the scalar form's
 * -fmaf(x, z, -y) into one FMSUB (y - x*z, which gives +0 there), so here the fmaf result goes through
 * a volatile and the negation is the sign bit, steps gcc cannot merge. */
static void ref_vnmsubfp(uint8_t* d, const uint8_t* a, const uint8_t* b, const uint8_t* c, uint32_t vscr) {
    uint8_t r[16];
    for (int i = 0; i < 4; i++) {
        const float x = vmx_f(vmx_flush(vmx_w(a, i), vscr)), y = vmx_f(vmx_flush(vmx_w(b, i), vscr)),
                    z = vmx_f(vmx_flush(vmx_w(c, i), vscr));
        volatile float t = fmaf(x, z, -y);
        vmx_sw(r, i, vmx_flush(vmx_fix(vmx_u(t) ^ 0x80000000u), vscr));
    }
    memcpy(d, r, 16);
}
/* References for vcfux / vcfsx: the ldexpf forms they replace (hz-0.32 uses an exact power-of-two multiply) */
static void ref_vcfux(uint8_t* d, const uint8_t* b, unsigned uimm) {
    for (int i = 0; i < 4; i++) vmx_sw(d, i, vmx_u(ldexpf((float)vmx_w(b, i), -(int)uimm)));
}
static void ref_vcfsx(uint8_t* d, const uint8_t* b, unsigned uimm) {
    for (int i = 0; i < 4; i++) vmx_sw(d, i, vmx_u(ldexpf((float)(int32_t)vmx_w(b, i), -(int)uimm)));
}
long hz_vmx_selftest(long vectors, long* checked) {
    long bad = 0, n = 0;
    for (long k = 0; k < vectors; ++k) {
        uint8_t a[16] __attribute__((aligned(16))), b[16] __attribute__((aligned(16))), c[16] __attribute__((aligned(16)));
        uint8_t r1[16], r2[16];
        for (int lane = 0; lane < 4; ++lane) {
            put(a, lane, operand()); put(b, lane, operand());
            put(c, lane, (next32() & 7) == 0 ? operand() ^ 0x80000000u : operand());
        }
        if ((k & 15) == 0) memcpy(b, a, 16);                        /* a*c + a: cancellation shapes */
        for (int nj = 0; nj < 2; ++nj) {
            const uint32_t vscr = nj ? VMX_NJ : 0;
            vmx_vmaddfp_scalar(r1, a, b, c, vscr); vmx_vmaddfp_neon(r2, a, b, c, vscr);
            if (memcmp(r1, r2, 16)) { if (bad++ < 4) fprintf(stderr, "[hz] vmx self-test: vmaddfp differs (NJ %d)\n", nj); }
            ref_vnmsubfp(r1, a, b, c, vscr); vmx_vnmsubfp_neon(r2, a, b, c, vscr);
            if (memcmp(r1, r2, 16)) {
                if (bad++ < 8)
                    for (int l = 0; l < 4; ++l) if (memcmp(r1 + 4 * l, r2 + 4 * l, 4))
                        fprintf(stderr, "[hz] vmx self-test: vnmsubfp differs (NJ %d) a=%08X b=%08X c=%08X scalar=%08X neon=%08X\n", nj,
                                (unsigned)vmx_w(a, l), (unsigned)vmx_w(b, l), (unsigned)vmx_w(c, l), (unsigned)vmx_w(r1, l), (unsigned)vmx_w(r2, l));
            }
            n += 8;   /* lanes compared */
        }
        {   /* vcfux / vcfsx: every scale 0..31 over integer operands with the edge values */
            uint8_t iv[16], r1[16], r2[16];
            for (int lane = 0; lane < 4; ++lane) {
                const uint32_t pick = next32() % 6;
                put(iv, lane, pick == 0 ? 0u : pick == 1 ? 0x80000000u : pick == 2 ? 0x7FFFFFFFu : pick == 3 ? 0xFFFFFFFFu
                                    : pick == 4 ? (next32() & 0xFFFFFFu) : next32());
            }
            const unsigned uimm = next32() % 32;
            ref_vcfux(r1, iv, uimm); vmx_vcfux(r2, iv, uimm);
            if (memcmp(r1, r2, 16)) { if (bad++ < 8) fprintf(stderr, "[hz] vmx self-test: vcfux differs (scale %u)\n", uimm); }
            ref_vcfsx(r1, iv, uimm); vmx_vcfsx(r2, iv, uimm);
            if (memcmp(r1, r2, 16)) { if (bad++ < 8) fprintf(stderr, "[hz] vmx self-test: vcfsx differs (scale %u)\n", uimm); }
            n += 8;
        }
    }
    *checked = n;
    return bad;
}
#endif
