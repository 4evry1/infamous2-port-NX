/* job211_native.c -- inFAMOUS 2 (BCES01143): the normal-map stage of SPU job 211 (jobmod_87926738de5c_e4640, LS
 * 0x45F8) natively. Switch build only (switch/natives/: the PC build compiles every C file of natives/spu); written for
 * this port, not derived from another project.
 *
 * What the job is. A 64 x 64 grid simulation, once a frame (water: "writes 16 KB of floats and 21.8 KB of 16-bit pairs
 * centred on 0x8000"). Its worker calls 0x45F8 with
 *     r3 = A  a 16 KB buffer of the spectrum, then at A + 0x4000 the word EA of where the 16-bit result goes
 *     r4 = F  the work buffer: two 16 KB arrays of floats (F + 0 and F + 0x4000, 64 rows of 64 floats), a table of 64 row
 *             pointers for each at F + 0x8000 and F + 0x8100, and from F + 0x82F0 the output
 *     r5 = the DMA tag
 * What 0x45F8 does, in order:
 *   1. fills the two pointer tables (F + 256 * i and F + 0x4000 + 256 * i) and clears a word at F + 0x8200;
 *   2. calls 0x40E8 (the first transform pass over A and F) and 0x80B8 twice (the second pass, over each table); these
 *      stay the lifted code: the native calls them as the lifted function does;
 *   3. makes a normal map from the two arrays: for every texel i of 4096, in the order of a Z curve (x from the even
 *      bits of i, y from the odd bits), h1 and h2 are the floats at pointer1[x] + 4 y and pointer2[x] + 4 y, the normal
 *      is n = (-h1, 1, -h2) / |n| (|n| from a reciprocal square root estimate with one Newton step, the reciprocal of
 *      that from a reciprocal estimate with one step), and the 16-bit pair (n.-h2 * 32767 + 32768, n.-h1 * 32767 +
 *      32768, each truncated) goes to F + 0x82F0 + 4 i;
 *   4. PUTs the 16 KB (LS F + 0x82F0 to the EA at A + 0x4000), builds the mip chain after it in the local store
 *      (32 x 32, 16 x 16 ... 2 x 2 texels, each the average of four consecutive texels of the level above, per 16-bit
 *      half (a + b + c + d + 2) >> 2, 5456 bytes), PUTs the chain (at EA + 16384), makes the 1 x 1 texel the same way and
 *      PUTs its 4 bytes (at EA + 16384 + 5456).
 * The normal-map loop was 0.46% of all busy samples lifted (hz-0.52, gameplay idle at a dock): 4096 texels, each a
 * bit-by-bit decode of the Z curve and about 40 float operations through the register file in memory.
 *
 * What the native does. It does 1. and 2. as the lifted function does (the same registers at each call, the same stack
 * pointer), then 3. and 4. on the local store: the LS words it reads are read where the lifted loop reads them (through
 * the pointer tables, which the second pass may have changed), in the same order as it writes the output, so a table
 * that pointed into the output would give the lifted result as well. The three PUTs are the lifted code's channel
 * writes (the MFC does them). The local store ends as the lifted function leaves it but for its stack frames below the
 * stack pointer (spills of r80..r84, r0, r1, and the callees' own), and the registers a caller can see (r0, r1, r80..r127)
 * are what it found. It declines, before it does anything, an argument that is not quadword aligned or whose buffers
 * leave the local store or overlap each other, the module (LS 0x4000..0x89FF) or the stack, and the job recorder being on.
 *
 * Floats. The arithmetic is the SPU's single precision. For ordinary heights (|h| < 2^24: n is the one thing here that
 * could overflow, and it cannot) the texel runs on plain floats under FPCR round-toward-zero + flush-to-zero, the
 * mode of the SPU threads, with the helpers' own estimate tables and steps; any other height, or any other mode, takes
 * the engine's float helpers (spu_fm_lean ...) with the lifted code's operands, exact for every input.
 *
 * PS3_NATIVE_J211: 1 (default) on, 0 off (the lifted function), 2 a check run: the native runs, its local store and
 * registers are kept, the state before it is put back, the lifted function runs, and the two are compared (the local
 * store outside the stack, r0, r1, r80..r127); the lifted result is what stays. In a check run the native's three PUTs
 * land in guest memory first and the lifted function's then overwrite them, and the PUTs' addresses and sizes are not
 * compared: a diagnostic mode, not one to play with. PS3_NATIVE_J211_SLOW=1 uses the
 * helpers for every texel. PS3_SPU_NATIVE=0 turns every native rewrite off. The stats line (every 10 s) counts the
 * calls the native took and declined, and in the check run the calls compared and the ones that differ. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

#include "spu_context.h"
#include "spu_helpers.h"

extern volatile int g_spu_jobrec_mode;
int spu_native_register(int image_id, uint32_t addr, int (*fn)(spu_context*));
void (*spu_lookup(uint32_t addr, int image_id))(spu_context*);
extern void jobmod_87926738de5c_e4640_spu_func_000040E8(spu_context* ctx);
extern void jobmod_87926738de5c_e4640_spu_func_000080B8(spu_context* ctx);

#ifndef J211_COV
#define J211_COV(n) ((void)0)
#endif

#define J211_IMAGE 211
#define J211_FUNC  0x45F8u

#define OUT0   0x82F0u              /* the normal map in the work buffer */
#define MIP0   (OUT0 + 0x4000u)     /* the mip chain after it */
#define MIPLEN 5456u
#define END    (MIP0 + MIPLEN + 16u)

static inline uint32_t R32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint32_t R16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static inline void W32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline void W16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline u128 V4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u128 v; v._u32[0] = a; v._u32[1] = b; v._u32[2] = c; v._u32[3] = d; return v; }
static inline u128 SPLAT(uint32_t w) { return V4(w, w, w, w); }
static inline int ovl(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) { return a0 < b1 && b0 < a1 && a0 < a1 && b0 < b1; }
/* lqd + rotqby: the word at byte `a` of the local store as the job's scalar load reads it (the quadword of a, rotated by a) */
static inline uint32_t LSW(const uint8_t* ls, uint32_t a)
{
    const uint8_t* q = ls + (a & (SPU_LS_MASK & ~0xFu));
    const uint32_t r = a & 15;
    return (uint32_t)q[r] << 24 | (uint32_t)q[(r + 1) & 15] << 16 | (uint32_t)q[(r + 2) & 15] << 8 | q[(r + 3) & 15];
}

/* ---------------------------------------------------------------------------------- the scalar float form */
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
static inline float FF(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }
static inline uint32_t FU(float f) { uint32_t w; memcpy(&w, &f, 4); return w; }
static inline int small_w(uint32_t w) { return ((w >> 23) & 0xFFu) < 0x97u; }      /* |x| < 2^24 (or a zero / denormal) */
static inline int fp_rzfz(void)
{
#if defined(__aarch64__)
    uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return (v & 0x01C00000u) == 0x01C00000u;                /* RMode = round toward zero, FZ */
#elif defined(__x86_64__) || defined(__i386__)
    return (_mm_getcsr() & 0xE040u) == 0xE040u;             /* RC = toward zero, FTZ, DAZ */
#else
    return 0;
#endif
}
/* spu_frsqest, spu_frest and spu_fi on one lane: the helpers' own tables and steps */
static inline uint32_t rsqrt_est(uint32_t bits)
{
    const uint32_t exp = (bits >> 23) & 0xFFu;
    const uint32_t rexp = exp == 0u ? 0xFFu : 190u - (exp + 1u) / 2u;
    return (rexp << 23) | (spu_frsqest_fraction_lut[(bits >> 18) & 0x3Fu] & 0x7FFFFFu);
}
static inline uint32_t recip_est(uint32_t bits)
{
    const uint32_t exp = (bits >> 23) & 0xFFu;
    const uint32_t rexp = exp == 0u ? 255u : exp >= 253u ? 0u : 253u - exp;
    return (bits & 0x80000000u) | (rexp << 23) | (spu_frest_fraction_lut[(bits >> 18) & 0x1Fu] & 0x7FFFFFu);
}
static inline uint32_t interp(uint32_t a, uint32_t est)
{
    const uint32_t y = a & 0x7FFFFu, hi = ((est >> 10) & 0x1FFFu) << 19, p = (est & 0x3FFu) * y;
    const uint32_t d = hi - p;
    const int brw = hi < p;
    const uint32_t frac = (brw ? d >> 8 : d >> 9) & 0x7FFFFFu;
    const uint32_t e = (est >> 23) & 0xFFu, e2 = e - (uint32_t)brw;
    if (e == 0 || e2 == 0) return 0;
    return (est & 0x80000000u) | (e2 << 23) | frac;
}

/* One texel of the normal map from the two heights (float words): the two 16-bit halves, first the one from -h2.
 * Plain floats for ordinary heights, the helpers for the rest. */
static inline uint32_t texel_fast(uint32_t h1b, uint32_t h2b)
{
    const float h1 = FF(h1b), h2 = FF(h2b);
    const float s = __builtin_fmaf(h2, h2, __builtin_fmaf(h1, h1, 1.0f));      /* >= 1: the minimum test of the job is always true */
    const float r = FF(interp(FU(s), rsqrt_est(FU(s))));
    const float r2 = r * r, rh = r * 0.5f;
    const float e = __builtin_fmaf(-s, r2, 1.0f);
    const float y = __builtin_fmaf(e, rh, r);
    const float sq = y * s;
    const uint32_t sb = FU(sq);
    const float rr = FF(interp(sb, recip_est(sb)));
    const float e2 = __builtin_fmaf(-rr, sq, 1.0f);
    const float inv = __builtin_fmaf(e2, rr, rr);
    const float nx = inv * FF(h2b ^ 0x80000000u), nz = inv * FF(h1b ^ 0x80000000u);
    const float ox = __builtin_fmaf(nx, 32767.0f, 32768.0f), oz = __builtin_fmaf(nz, 32767.0f, 32768.0f);
    const uint32_t u1 = ox > 0.0f ? (uint32_t)ox : 0u, u2 = oz > 0.0f ? (uint32_t)oz : 0u;
    return (u1 & 0xFFFFu) << 16 | (u2 & 0xFFFFu);
}
static uint32_t texel_slow(uint32_t h1b, uint32_t h2b)
{
    const u128 one = SPLAT(0x3F800000u), half = SPLAT(0x3F000000u), sg = SPLAT(0x80000000u);
    const u128 H1 = SPLAT(h1b), H2 = SPLAT(h2b);
    const u128 s1 = spu_fma_lean(H1, H1, one);
    const u128 n2 = spu_xor(H2, sg), n1 = spu_xor(H1, sg);
    const u128 s = spu_fma_lean(H2, H2, s1);
    const u128 big = spu_fcgt_lean(s, SPLAT(0x00800000u));
    const u128 est = spu_frsqest(s);
    const u128 r = spu_fi(s, est);
    const u128 r2 = spu_fm_lean(r, r), rh = spu_fm_lean(r, half);
    const u128 e = spu_fnms_lean(s, r2, one);
    const u128 y = spu_fma_lean(e, rh, r);
    const u128 sq = spu_fm_lean(y, s);
    const u128 z = spu_selb(V4(0, 0, 0, 0), sq, big);
    const u128 rc = spu_frest(z);
    const u128 rr = spu_fi(z, rc);
    const u128 e2 = spu_fnms_lean(rr, z, one);
    const u128 inv = spu_fma_lean(e2, rr, rr);
    const u128 nx = spu_fm_lean(inv, n2), nz = spu_fm_lean(inv, n1);
    const u128 c1 = SPLAT(0x46FFFE00u), c2 = SPLAT(0x47000000u);
    const u128 ox = spu_fma_lean(nx, c1, c2), oz = spu_fma_lean(nz, c1, c2);
    const u128 u1 = spu_cfltu(ox, 173), u2 = spu_cfltu(oz, 173);
    return (u1._u32[0] & 0xFFFFu) << 16 | (u2._u32[0] & 0xFFFFu);
}

/* the x and y of a texel on the job's Z curve: the even and the odd bits of i */
static inline uint32_t compact1(uint32_t v)
{
    v &= 0x55555555u; v = (v | (v >> 1)) & 0x33333333u; v = (v | (v >> 2)) & 0x0F0F0F0Fu;
    v = (v | (v >> 4)) & 0x00FF00FFu; return (v | (v >> 8)) & 0x0000FFFFu;
}

/* ---------------------------------------------------------------------------------- the function */
static int s_fast = 1;                                       /* PS3_NATIVE_J211_SLOW=1: always the helpers */

/* a PUT as the lifted code issues it: the five channel writes between a di and an ei */
static void put_cmd(spu_context* ctx, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag_word)
{
    ctx->int_enable = 0;
    spu_wrch(ctx, MFC_LSA, SPLAT(lsa));
    spu_wrch(ctx, MFC_EAL, SPLAT(ea));
    spu_wrch(ctx, MFC_Size, SPLAT(size));
    spu_wrch(ctx, MFC_TagID, SPLAT(tag_word));
    spu_wrch(ctx, MFC_Cmd, SPLAT(32));
    ctx->int_enable = 1;
}

static void call_lifted(spu_context* ctx, void (*fn)(spu_context*), uint32_t link)
{
    ctx->gpr[0] = spu_link(link);
    { int32_t _si = (int32_t)ctx->image_id; ctx->host_depth++; fn(ctx); spu_drain_call(ctx, link); ctx->host_depth--; spu_img_restore(ctx, _si); }
}

/* the whole function on the real context; 0 = declined, nothing done */
static int j211_run(spu_context* ctx, int fast_ok)
{
    uint8_t* const ls = SPU_LS_BASE(ctx);
    const uint32_t A = ctx->gpr[3]._u32[0], F = ctx->gpr[4]._u32[0], sp = ctx->gpr[1]._u32[0];
    if (g_spu_jobrec_mode || ((A | F | sp) & 15u) || (uint64_t)A + 0x4010u > SPU_LS_SIZE || (uint64_t)F + END > SPU_LS_SIZE || sp < 0x1000u ||
        (uint64_t)sp + 0x40u > SPU_LS_SIZE || ovl(F, F + END, A, A + 0x4010u) || ovl(F, F + END, sp - 0x400u, sp + 0x20u) ||
        ovl(A, A + 0x4010u, sp - 0x400u, sp + 0x20u) || ovl(F, F + END, 0x4000u, 0x8A00u) || ovl(A, A + 0x4010u, 0x4000u, 0x8A00u))
        return 0;
    const u128 r0 = ctx->gpr[0], r1 = ctx->gpr[1], r4 = ctx->gpr[4];
    const u128 tagq = ctx->gpr[5];
    const int fast = fast_ok && fp_rzfz();
    /* 1. the pointer tables, the cleared word */
    for (uint32_t i = 0; i < 64; i++) { W32(ls + F + 0x8000u + 4 * i, F + 256u * i); W32(ls + F + 0x8100u + 4 * i, F + 0x4000u + 256u * i); }
    ctx->gpr[1] = spu_ai(ctx->gpr[1], -112);
    { const uint32_t wa = F + 0x8200u + (ctx->gpr[1]._u32[0] & 0xCu);       /* cwx(r1, 0x8200): the word of the quadword r1 selects */
      W32(ls + wa, 0); }
    /* 2. the transform passes, lifted */
    call_lifted(ctx, jobmod_87926738de5c_e4640_spu_func_000040E8, 0x46B0u);
    {
        const u128 a8 = spu_a(r4, spu_ila(0x8228)), a2 = spu_a(r4, spu_ila(0x8200));
        ctx->gpr[5] = spu_fsmbi(0xFFFF); ctx->gpr[6] = spu_a(r4, spu_ila(0x8000)); ctx->gpr[7] = spu_shlqbyi(a2, 0); ctx->gpr[8] = spu_ori(a8, 0);
        ctx->gpr[3] = spu_il(64); ctx->gpr[4] = spu_il(64);
        call_lifted(ctx, jobmod_87926738de5c_e4640_spu_func_000080B8, 0x46E0u);
        ctx->gpr[7] = spu_shlqbyi(a2, 0); ctx->gpr[5] = spu_fsmbi(0xFFFF); ctx->gpr[6] = spu_a(r4, spu_ila(0x8100)); ctx->gpr[8] = spu_shlqbyi(a8, 0);
        ctx->gpr[3] = spu_il(64); ctx->gpr[4] = spu_il(64);
        call_lifted(ctx, jobmod_87926738de5c_e4640_spu_func_000080B8, 0x4710u);
    }
    /* 3. the normal map */
    uint8_t* out = ls + F + OUT0;
    for (uint32_t i = 0; i < 4096; i++) {
        const uint32_t x = compact1(i), y = compact1(i >> 1);
        const uint32_t p1 = LSW(ls, F + 0x8000u + 4 * x), p2 = LSW(ls, F + 0x8100u + 4 * x);
        const uint32_t h1 = LSW(ls, p1 + 4 * y), h2 = LSW(ls, p2 + 4 * y);
        const uint32_t v = fast && small_w(h1) && small_w(h2) ? texel_fast(h1, h2) : texel_slow(h1, h2);
        W32(out + 4 * i, v);
        J211_COV(fast && small_w(h1) && small_w(h2) ? 0 : 1);
    }
    /* 4. the PUT of the map, the mip chain, its PUT, the 1 x 1 texel, its PUT */
    const uint32_t ea0 = R32(ls + A + 0x4000u), tagw = tagq._u32[0];
    put_cmd(ctx, F + OUT0, ea0, 16384, tagw);
    uint32_t prev = F + OUT0, cur = F + MIP0, size = 32, end;
    for (;;) {
        const uint32_t n = size * size;
        for (uint32_t j = 0; j < n; j++) {
            const uint8_t* s = ls + prev + 16 * j;
            W16(ls + cur + 4 * j, (R16(s) + R16(s + 4) + R16(s + 8) + R16(s + 12) + 2u) >> 2);
            W16(ls + cur + 4 * j + 2, (R16(s + 2) + R16(s + 6) + R16(s + 10) + R16(s + 14) + 2u) >> 2);
        }
        end = cur + 4 * n;
        size >>= 1; prev = cur;
        if (!(size > 1)) break;
        cur = end;
    }
    put_cmd(ctx, F + MIP0, ea0 + 16384u, end - (F + MIP0), tagw);
    {
        const uint8_t* s = ls + prev;
        W16(ls + end, (R16(s) + R16(s + 4) + R16(s + 8) + R16(s + 12) + 2u) >> 2);
        W16(ls + end + 2, (R16(s + 2) + R16(s + 6) + R16(s + 10) + R16(s + 14) + 2u) >> 2);
    }
    put_cmd(ctx, end, ea0 + 16384u + (end - (F + MIP0)), 4, tagw);
    ctx->gpr[0] = r0; ctx->gpr[1] = r1;
    return 1;
}

/* ---------------------------------------------------------------------------------- entry, check run, stats */
typedef struct { unsigned long long calls, native, declined, cmp, bad; } Stat;
static Stat s_st;
static int s_mode = -1;                                      /* PS3_NATIVE_J211 */
static _Thread_local uint8_t* t_lsb;                         /* the check run's copies of the store */
static _Thread_local uint8_t* t_lsn;
static _Thread_local int t_bypass;

#define ADD(f, n) __atomic_fetch_add(&(f), (n), __ATOMIC_RELAXED)

static void stats(void)
{
    char line[300]; int l = snprintf(line, sizeof line, "[j211] %llu calls: native %llu, SPU %llu", s_st.calls, s_st.native, s_st.declined);
    if (s_mode == 2 && l > 0 && l < (int)sizeof line) snprintf(line + l, sizeof line - l, "; compared %llu, differ %llu", s_st.cmp, s_st.bad);
    fprintf(stderr, "%s\n", line);       /* one write: other threads print too */
}
static void stats_tick(unsigned long long n)
{
    static uint64_t next;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    const uint64_t now = (uint64_t)t.tv_sec;
    uint64_t w = __atomic_load_n(&next, __ATOMIC_RELAXED);
    (void)n;
    if (now < w || !__atomic_compare_exchange_n(&next, &w, now + 10, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    if (w) stats();
}

static int j211_entry(spu_context* ctx)
{
    if (t_bypass) return 0;
    if (__builtin_expect(__atomic_load_n(&s_mode, __ATOMIC_ACQUIRE) < 0, 0)) {
        const char* e = getenv("PS3_NATIVE_J211");
        { const char* sl = getenv("PS3_NATIVE_J211_SLOW"); if (sl && sl[0] == '1') s_fast = 0; }
        int m = e && e[0] ? (e[0] == '2' ? 2 : e[0] != '0') : 1, unset = -1;
        if (__atomic_compare_exchange_n(&s_mode, &unset, m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            fprintf(stderr, "[j211] native normal map (image %d, 0x%X): %s%s\n", J211_IMAGE, J211_FUNC,
                    m == 2 ? "check run against the lifted function" : m ? "on" : "off", s_fast ? "" : ", float helpers only");
    }
    if (!s_mode) return 0;
    const unsigned long long n = ADD(s_st.calls, 1) + 1;
    stats_tick(n);
    if (s_mode != 2) {
        if (!j211_run(ctx, s_fast)) { ADD(s_st.declined, 1); return 0; }
        ADD(s_st.native, 1);
        return 1;
    }
    /* the check run */
    if (!t_lsb) t_lsb = (uint8_t*)malloc(SPU_LS_SIZE);
    if (!t_lsn) t_lsn = (uint8_t*)malloc(SPU_LS_SIZE);
    void (*fn)(spu_context*) = spu_lookup(J211_FUNC, ctx->resident_ovl);
    if (!t_lsb || !t_lsn || !fn) return 0;
    uint8_t* const ls = SPU_LS_BASE(ctx);
    memcpy(t_lsb, ls, SPU_LS_SIZE);
    u128 g0[128]; memcpy(g0, ctx->gpr, sizeof g0);
    const uint32_t sp = ctx->gpr[1]._u32[0];
    const uint32_t r0 = ctx->gpr[0]._u32[0];
    if (!j211_run(ctx, s_fast)) { ADD(s_st.declined, 1); return 0; }
    ADD(s_st.native, 1);
    u128 gn[128]; memcpy(gn, ctx->gpr, sizeof gn);
    memcpy(t_lsn, ls, SPU_LS_SIZE);
    memcpy(ls, t_lsb, SPU_LS_SIZE); memcpy(ctx->gpr, g0, sizeof g0);
    const int32_t si = (int32_t)ctx->image_id;
    t_bypass = 1;
    ctx->host_depth++; fn(ctx); spu_drain_call(ctx, r0); ctx->host_depth--;
    spu_img_restore(ctx, si);
    t_bypass = 0;
    int bad = 0; uint32_t at = 0;
    for (int r = 0; r < 128 && !bad; r++)
        if ((r == 0 || r == 1 || r >= 80) && memcmp(&ctx->gpr[r], &gn[r], 16)) { bad = 1; at = (uint32_t)r; }
    const uint32_t lo = sp - 0x400u, hi = sp + 0x20u;
    for (uint32_t a = 0; a < SPU_LS_SIZE && !bad; a += 16) {
        if (a + 16 > lo && a < hi) continue;
        if (memcmp(ls + a, t_lsn + a, 16)) { bad = 1; at = 0x1000u + a; }
    }
    ADD(s_st.cmp, 1);
    if (bad) {
        const unsigned long long nb = __atomic_add_fetch(&s_st.bad, 1, __ATOMIC_RELAXED);
        if (nb <= 12) fprintf(stderr, "[j211] check DIFFER #%llu (%s %x): A=%05X F=%05X sp=%05X\n", nb, at < 0x1000u ? "register" : "local store",
                              at, g0[3]._u32[0], g0[4]._u32[0], sp);
    }
    return 1;
}

__attribute__((constructor)) static void j211_register(void)
{
    spu_native_register(J211_IMAGE, J211_FUNC, j211_entry);
}
