/* job218_native.c -- inFAMOUS 2 (BCES01143): kernels of SPU job 218 (jobmod_ab654d30fd90_efe70, the skeletal animation
 * job, internally job2025) natively: the pose blends 0x16320 and 0x17280, the rotation unpacker 0x1C510, the joint
 * matrix builder 0x17780, the matrix products 0x18B80 and the root transform loop 0x5814 (each has its own model,
 * below). Switch build only (switch/natives/: the PC build compiles every C file of natives/spu); written for this
 * port, not derived from another project.
 *
 * What 0x16320 does. It blends two arrays of joint transforms into a third, four joints at a time (a group is
 * 192 bytes: four joints of three quadwords each):
 *     r3 = destination   r5 = pose A   r7 = pose B   r9 = blend factor t (a float in word 0)   r10 = joints
 *     r6 / r8 = weight bytes of A / of B, one byte a joint (null = every byte 255)
 *     r4 = where the blended weight bytes go, one byte a joint
 * From the two weight bytes of a joint (w = byte * 256/255 / 256, all in SPU single precision) it takes a blend
 * factor s from t and the larger weight (a reciprocal estimate with one Newton step), and the output weight byte
 * (the weights blended by s, scaled by 255/256 plus 1/512, truncated to 0..255). A joint's first quadword (a
 * rotation quaternion) is blended along the shorter arc: |dot| is turned into an angle by the job's arccos
 * polynomial, and the two scales sin((1-s)*angle) / sin(angle) and sin(s*angle) / sin(angle) come from the job's
 * sine polynomial; nearly parallel quaternions (|dot| > 0.999) use (1-s) and s instead. Its other two quadwords
 * are blended linearly, a - a*s + b*s. A joint whose weight byte in A is zero takes B's three quadwords as they
 * are, and one whose byte in B is zero (and A's is not) takes A's. The padding joints of a last group that is
 * not full are computed and stored like the others, so the whole group's bytes match the lifted code's.
 * The lifted code is a software pipeline of this (the SPU code overlaps one group's loads and weight arithmetic
 * with the previous group's blend and stores); the model below is one group at a time, with the same operations
 * in the same order, so every float is the SPU's.
 *
 * What 0x17280 does. The same weights and zero-weight takes over arrays of one float a joint (a group is one
 * quadword): r3 = destination, r4 = output weight bytes, r5 / r7 = the arrays, r6 / r8 = their weight bytes,
 * r9 = flag bytes (null = none), r10 = t, r11 = joints. The float is blended linearly; where the joint's flag byte
 * has bit 0 set the result is clamped to 0..1. It spills no register and changes none a caller keeps.
 *
 * What it replaces. The hooks are at the functions' entries (natives-switch.txt). A call the model takes does what
 * the lifted function does: the local store (the destination, the weight bytes, and for 0x16320 the 30 quadwords
 * it spills r80..r109 into below the stack pointer, which the lifted code restores unchanged), and the registers
 * a caller can see (none changes: every function here returns void, and none of the callers reads r2..r79 after a
 * call; 0x5814 is no function: it takes over a loop of the job's skeleton function and leaves every register as the
 * loop would). Anything the model does not cover declines before it writes: a FPCR other than
 * round-toward-zero + flush-to-zero, a pointer that is not quadword (data) or word (weights, flags) aligned or
 * whose range leaves the local store, more than 0x10000 joints, or ranges that overlap other than the destination
 * over exactly array A or array B, or the output weights over exactly A's or B's weights (the lifted code's order
 * of loads and stores gives the same result there; every other overlap, and 0x16320's spill area, would not).
 *
 * Floats. The arithmetic is SPU single precision: truncating, denormals as zero, no infinity (exponent 255 is a
 * number), a zero is always +0. Under FPCR round-toward-zero + FZ the host's NEON operations give those bits for
 * every value below FLT_MAX in magnitude except the sign of a zero. A group runs on one of three forms:
 *   raw    plain NEON operations, no checks and no per-operation flush of -0 (only the results stored per joint
 *          are flushed: nothing else can see the sign of a zero, the helpers that read estimates are fed nonzero
 *          values and the compares read through their own flush). Taken when every float of the group's data
 *          is below 2^56 in magnitude and |t| <= 8: then no operation can reach FLT_MAX (the largest intermediate
 *          is a blend scale of a few million times 2^56) and none sees exponent 255.
 *   spec   the helpers' speculative forms (spu_*_spec: the fast form of each operation and one accumulated
 *          verdict), for a group with larger or exponent-255 values: right when no operation reached FLT_MAX or
 *          took an exponent-255 operand;
 *   exact  the lean forms, per operation exact: the group is redone on them when spec's verdict is no.
 * spec and exact are the lifted code's own helpers called with the lifted code's operands. Two substitutions keep
 * lanes whose result is discarded from forcing a slower form: a joint with both weight bytes zero has its
 * reciprocal taken of 1.0 (its factor is +0 either way: the numerator is +0), and in 0x16320 a joint with
 * |dot| > 0.999 has its angle chain run on 0.5 (the lifted code selects 1-s and s over that chain's result).
 *
 * PS3_NATIVE_J218: 1 (default) on, 0 off (the lifted code), 2 a check run: every call the model takes runs on a
 * copy of the local store, then the lifted function runs on the real one and the two stores are compared whole
 * (and r0, r1, r80..r127 against what the call found, and r3 for a function that returns a value, k_fn[].ret);
 * the lifted result is what stays. For the loop 0x5814 (the lifted function cannot be run on its own: it goes on
 * to the rest of the job) the model runs on a copy of the store and of all 128 registers, the loop's lifted
 * statements (loop5814_pass) run on the real context, and the stores and every register are compared. The check
 * run's copy of the store is one buffer a thread, kept for the thread's life (freed when the thread ends).
 * PS3_SPU_NATIVE=0 turns every native rewrite off. The stats line (every 16384 calls) counts, per function, the
 * calls the model took and declined, the groups by form and, in the check run, the calls compared and the ones
 * that differ. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "spu_context.h"
#include "spu_helpers.h"

int spu_native_register(int image_id, uint32_t addr, int (*fn)(spu_context*));
void (*spu_lookup(uint32_t addr, int image_id))(spu_context*);

#define J218_IMAGE 218          /* jobmod_ab654d30fd90_efe70 */

/* ---------------------------------------------------------------------------------- quads */
typedef uint32x4_t V;           /* a register: lane i = SPU word i (u128's _u32[i]) */
static inline u128 U_(V v) { u128 r; vst1q_u32(r._u32, v); return r; }
static inline V W_(u128 x) { return vld1q_u32(x._u32); }
#define K_(x) vdupq_n_u32(x)
/* lqd / stqd of the quad at a (16-byte aligned, inside the store): one load and one REV32 */
static inline V LQ(const uint8_t* ls, uint32_t a) { return vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(ls + a))); }
static inline void SQ(uint8_t* ls, uint32_t a, V v) { vst1q_u8(ls + a, vrev32q_u8(vreinterpretq_u8_u32(v))); }
/* r0..r3 are four joints' quadwords; x, y, z, w are word 0..3 of the four, one lane a joint */
static inline void tr4(V r0, V r1, V r2, V r3, V* x, V* y, V* z, V* w)
{
    const V t0 = vtrn1q_u32(r0, r1), t1 = vtrn2q_u32(r0, r1), t2 = vtrn1q_u32(r2, r3), t3 = vtrn2q_u32(r2, r3);
    *x = vreinterpretq_u32_u64(vtrn1q_u64(vreinterpretq_u64_u32(t0), vreinterpretq_u64_u32(t2)));
    *y = vreinterpretq_u32_u64(vtrn1q_u64(vreinterpretq_u64_u32(t1), vreinterpretq_u64_u32(t3)));
    *z = vreinterpretq_u32_u64(vtrn2q_u64(vreinterpretq_u64_u32(t0), vreinterpretq_u64_u32(t2)));
    *w = vreinterpretq_u32_u64(vtrn2q_u64(vreinterpretq_u64_u32(t1), vreinterpretq_u64_u32(t3)));
}

/* ---------------------------------------------------------------------------------- the group */
/* constants of the job's code, as words */
#define F_ONE   0x3F800000u
#define F_NEG1  0xBF800000u
#define F_HALF  0x3F000000u
#define F_WGT   0x3F808081u     /* 256/255: a weight byte b is (b / 256) * this */
#define F_K255  0x3F7F0000u     /* 255/256 */
#define F_K512  0x3B000000u     /* 1/512 */
#define F_PARA  0x3F7FBE77u     /* |dot| above this: nearly parallel */
#define F_AC0   0x3FC90FDAu     /* the arccos polynomial c0 - x*(c1 - x*(c2 - x*(x*c3))) */
#define F_AC1   0x3E5BBFCAu
#define F_AC2   0x3DB63A9Eu
#define F_AC3   0x3D4D8392u
#define F_S1    0xBE2AAAABu     /* the sine polynomial 1 + y*(s1 + y*(s3 + y*(s5 + y*s7))), y = angle^2 */
#define F_S3    0x3C088887u
#define F_S5    0xB9500C03u
#define F_S7    0x3638B94Fu
#define RAW_VMAX 0x5B800000u    /* the raw form needs |every input float| < 2^56 ... */
#define RAW_TMAX 0x41000000u    /* ... and |t| <= 8 */

enum { M_RAW = 0, M_SPEC = 1, M_EXACT = 2 };

/* The operations of the group, by form (m is a constant where these are inlined). The empty asm keeps the
 * compiler from doing a constant operation at compile time, where it would round to nearest. */
#if defined(__aarch64__)
#define OPAQUE_(v) __asm__("" : "+w"(v))
#else
#define OPAQUE_(v) __asm__("" : "+x"(v))
#endif
#define FV_(v) vreinterpretq_f32_u32(v)
#define UV_(v) vreinterpretq_u32_f32(v)
#define ALW static inline __attribute__((always_inline))
ALW V fm_(int m, spu_spec_acc* c, V x, V y)
{
    if (m == M_RAW) { float32x4_t f = FV_(x); OPAQUE_(f); return UV_(vmulq_f32(f, FV_(y))); }
    return m == M_SPEC ? W_(spu_fm_spec(U_(x), U_(y), c)) : W_(spu_fm_lean(U_(x), U_(y)));
}
ALW V fs_(int m, spu_spec_acc* c, V x, V y)
{
    if (m == M_RAW) { float32x4_t f = FV_(x); OPAQUE_(f); return UV_(vsubq_f32(f, FV_(y))); }
    return m == M_SPEC ? W_(spu_fs_spec(U_(x), U_(y), c)) : W_(spu_fs_lean(U_(x), U_(y)));
}
ALW V fma_(int m, spu_spec_acc* c, V x, V y, V z)                      /* x*y + z */
{
    if (m == M_RAW) { float32x4_t f = FV_(x); OPAQUE_(f); return UV_(vfmaq_f32(FV_(z), f, FV_(y))); }
    return m == M_SPEC ? W_(spu_fma_spec(U_(x), U_(y), U_(z), c)) : W_(spu_fma_lean(U_(x), U_(y), U_(z)));
}
ALW V fnms_(int m, spu_spec_acc* c, V x, V y, V z)                     /* z - x*y */
{
    if (m == M_RAW) { float32x4_t f = FV_(x); OPAQUE_(f); return UV_(vfmsq_f32(FV_(z), f, FV_(y))); }
    return m == M_SPEC ? W_(spu_fnms_spec(U_(x), U_(y), U_(z), c)) : W_(spu_fnms_lean(U_(x), U_(y), U_(z)));
}
ALW V cflts_(int m, spu_spec_acc* c, V x, int i8)
{
    return m == M_EXACT ? W_(spu_cflts(U_(x), i8)) : W_(spu_cflts_spec(U_(x), i8, c));
}
/* a computed result as the SPU leaves it: a zero is +0 (the raw form's operations leave -0 where a product of
 * zeros of different signs, or a sum of -0s, comes out; the other forms' operations flush every result) */
ALW V flush_(int m, V x) { return m == M_RAW ? vbicq_u32(x, vceqzq_u32(vandq_u32(x, K_(0x7F800000u)))) : x; }
ALW V fcgt_(V x, V y) { return W_(spu_fcgt_lean(U_(x), U_(y))); }
ALW V sel_(V a, V b, V mask) { return vbslq_u32(mask, b, a); }              /* selb: mask ? b : a */
ALW V recip_(V a, V est) { return W_(spu_fi(U_(a), U_(est))); }             /* fi(a, frest/frsqest(a)) */

/* sin(angle) polynomial: fma(fma(fma(fma(s7, y, s5), y, s3), y, s1), y, 1) */
ALW V sinp_(int m, spu_spec_acc* c, V y)
{
    V p = fma_(m, c, K_(F_S7), y, K_(F_S5));
    p = fma_(m, c, p, y, K_(F_S3));
    p = fma_(m, c, p, y, K_(F_S1));
    return fma_(m, c, p, y, K_(F_ONE));
}

/* The weights of four joints (lane e = joint e), shared by both functions. wa4 / wb4: the four weight bytes of A
 * and of B as read from the store in address order (byte j in bits 8j..); tt: t in all lanes. Gives the blend
 * factor s of each joint, the masks of the joints whose byte in A / in B is zero, and the four output weight
 * bytes (in address order) in *wo4. */
ALW void weights_(const int m, spu_spec_acc* c, uint32_t wa4, uint32_t wb4, V tt, V* s_, V* za_, V* zb_, uint32_t* wo4)
{
#define FM(x, y)       fm_(m, c, (x), (y))
#define FS(x, y)       fs_(m, c, (x), (y))
#define FMA(x, y, z)   fma_(m, c, (x), (y), (z))
#define FNMS(x, y, z)  fnms_(m, c, (x), (y), (z))
    const V one = K_(F_ONE);
    const V ba = vmovl_u16(vget_low_u16(vmovl_u8(vcreate_u8((uint64_t)wa4))));
    const V bb = vmovl_u16(vget_low_u16(vmovl_u8(vcreate_u8((uint64_t)wb4))));
    *za_ = vceqzq_u32(ba); *zb_ = vceqzq_u32(bb);                             /* byte == 0 */
    const V wa = FM(W_(spu_cuflt(U_(ba), 147)), K_(F_WGT));
    const V wb = FM(W_(spu_cuflt(U_(bb), 147)), K_(F_WGT));
    const V gt = fcgt_(wb, wa);                                               /* wb > wa */
    const V mx = sel_(wa, wb, gt);                                            /* max(wa, wb) */
    /* both weights zero: the factor is +0 whatever the reciprocal is (its numerator is +0), so take 1/1 */
    const V mp = sel_(mx, one, vceqzq_u32(vandq_u32(mx, K_(0x7F800000u))));
    const V e0 = recip_(mp, W_(spu_frest(U_(mp))));
    const V e1 = FMA(FNMS(mp, e0, one), e0, e0);                              /* one Newton step */
    const V num = sel_(FM(tt, wb), FS(FMA(tt, wa, wb), wa), gt);
    const V s = FM(num, e1);                                                  /* the blend factor of each joint */
    *s_ = s;
    {   /* the output weights */
        const V l = FNMS(wa, s, FMA(wb, s, wa));                              /* wa + (wb - wa) * s */
        const V wi = cflts_(m, c, FMA(l, K_(F_K255), K_(F_K512)), 165);       /* * 256, truncated */
        const int32x4_t cl = vmaxq_s32(vminq_s32(vreinterpretq_s32_u32(wi), vdupq_n_s32(255)), vdupq_n_s32(0));
        *wo4 = vget_lane_u32(vreinterpret_u32_u8(vmovn_u16(vcombine_u16(vmovn_u32(vreinterpretq_u32_s32(cl)),
                                                                        vdup_n_u16(0)))), 0);
    }
#undef FM
#undef FS
#undef FMA
#undef FNMS
}

/* One group of four joints of 0x16320. a / b: the group's 12 quadwords of pose A / B (joint e = quads 3e..3e+2).
 * Gives the 12 destination quadwords o and the four output weight bytes in *wo4. */
ALW void blend_group(const int m, spu_spec_acc* c, const V* a, const V* b, uint32_t wa4, uint32_t wb4, V tt, V* o,
                     uint32_t* wo4)
{
#define FM(x, y)       fm_(m, c, (x), (y))
#define FS(x, y)       fs_(m, c, (x), (y))
#define FMA(x, y, z)   fma_(m, c, (x), (y), (z))
#define FNMS(x, y, z)  fnms_(m, c, (x), (y), (z))
    const V one = K_(F_ONE);
    V s, za, zb;
    weights_(m, c, wa4, wb4, tt, &s, &za, &zb, wo4);
    /* rotations: lane e = joint e */
    V ax, ay, az, aw, bx, by, bz, bw;
    tr4(a[0], a[3], a[6], a[9], &ax, &ay, &az, &aw);
    tr4(b[0], b[3], b[6], b[9], &bx, &by, &bz, &bw);
    const V dot = FMA(aw, bw, FMA(az, bz, FMA(ay, by, FM(ax, bx))));
    const V sg = sel_(one, K_(F_NEG1), fcgt_(W_(spu_zero()), dot));           /* -1 where dot < 0: the shorter arc */
    const V x = FM(dot, sg);                                                  /* |dot| */
    const V par = fcgt_(x, K_(F_PARA));                                       /* nearly parallel */
    const V xs = sel_(x, K_(F_HALF), par);                                    /* a parallel joint's angle chain is discarded */
    const V u = FS(one, xs);
    const V rs = recip_(u, W_(spu_frsqest(U_(u))));                           /* 1/sqrt(1 - |dot|) */
    V ac = FM(xs, K_(F_AC3));
    ac = FNMS(xs, ac, K_(F_AC2));
    ac = FNMS(xs, ac, K_(F_AC1));
    ac = FNMS(xs, ac, K_(F_AC0));
    const V th = FM(ac, FNMS(rs, xs, rs));                                    /* the angle between the two */
    const V om = FS(one, s);
    const V sth = FM(s, th), omt = FM(om, th);
    const V sn = FM(sinp_(m, c, FM(th, th)), th);                             /* sin(angle) */
    const V rc = recip_(sn, W_(spu_frest(U_(sn))));
    const V kb = sel_(FM(FM(sinp_(m, c, FM(sth, sth)), sth), rc), s, par);    /* weight of B */
    const V ka = sel_(FM(FM(sinp_(m, c, FM(omt, omt)), omt), rc), om, par);   /* weight of A */
    /* joints */
#define JOINT(E)                                                                                                 \
    do {                                                                                                         \
        const V sE = vdupq_laneq_u32(s, E), kbE = vdupq_laneq_u32(kb, E), kaE = vdupq_laneq_u32(ka, E);         \
        const V sgE = vdupq_laneq_u32(sg, E), zaE = vdupq_laneq_u32(za, E), zbE = vdupq_laneq_u32(zb, E);       \
        const V* pa = a + 3 * (E); const V* pb = b + 3 * (E);                                                    \
        const V r0 = flush_(m, FMA(FM(pb[0], sgE), kbE, FM(pa[0], kaE)));                                        \
        const V r1 = flush_(m, FNMS(pa[1], sE, FMA(pb[1], sE, pa[1])));                                          \
        const V r2 = flush_(m, FNMS(pa[2], sE, FMA(pb[2], sE, pa[2])));                                          \
        o[3 * (E)]     = vbslq_u32(zaE, pb[0], vbslq_u32(zbE, pa[0], r0));                                       \
        o[3 * (E) + 1] = vbslq_u32(zaE, pb[1], vbslq_u32(zbE, pa[1], r1));                                       \
        o[3 * (E) + 2] = vbslq_u32(zaE, pb[2], vbslq_u32(zbE, pa[2], r2));                                       \
    } while (0)
    JOINT(0); JOINT(1); JOINT(2); JOINT(3);
#undef JOINT
#undef FM
#undef FS
#undef FMA
#undef FNMS
}

/* One group of four joints of 0x17280 (one float a joint, lane e = joint e). a / b: the group's quadword of pose A /
 * B; fl: the four flag bytes (0 where there is no flag array). The blend factor, the zero-weight takes and the
 * output weight bytes are the same as 0x16320's; a joint whose flag has bit 0 set has its float clamped to 0..1. */
ALW V chan_group(const int m, spu_spec_acc* c, V a, V b, uint32_t wa4, uint32_t wb4, uint32_t fl4, V tt, uint32_t* wo4)
{
    V s, za, zb;
    weights_(m, c, wa4, wb4, tt, &s, &za, &zb, wo4);
    const V lerp = flush_(m, fnms_(m, c, a, s, fma_(m, c, b, s, a)));         /* a - a*s + b*s */
    const V r = sel_(sel_(lerp, a, zb), b, za);                               /* a zero weight takes the other pose */
    const V one = K_(F_ONE), zero = W_(spu_zero());
    V cl = sel_(r, one, fcgt_(r, one));
    cl = sel_(cl, zero, fcgt_(zero, r));
    const V fl = vmovl_u16(vget_low_u16(vmovl_u8(vcreate_u8((uint64_t)fl4))));
    return sel_(r, cl, vtstq_u32(fl, K_(1)));                                 /* bit 0 of the flag byte: clamp */
}

/* the raw form's input test: every float of the given quadwords below RAW_VMAX in magnitude (the sign shifted
 * out; one unsigned maximum over all of them) */
ALW int raw_ok(const V* a, const V* b, int n)
{
    V mx = vshlq_n_u32(a[0], 1);
    for (int j = 1; j < n; j++) mx = vmaxq_u32(mx, vshlq_n_u32(a[j], 1));
    for (int j = 0; j < n; j++) mx = vmaxq_u32(mx, vshlq_n_u32(b[j], 1));
    return vmaxvq_u32(mx) < (RAW_VMAX << 1);
}
/* the same over n quadwords of one array */
ALW int raw_okv(const V* a, int n)
{
    V mx = vshlq_n_u32(a[0], 1);
    for (int j = 1; j < n; j++) mx = vmaxq_u32(mx, vshlq_n_u32(a[j], 1));
    return vmaxvq_u32(mx) < (RAW_VMAX << 1);
}

/* ---------------------------------------------------------------------------------- bit streams */
static inline uint32_t be32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
static inline uint32_t be16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }
/* the 32 bits of the stream (big-endian bit order) that start at bit position pos; 8 bytes are read from pos >> 3 */
static inline uint32_t bits32(const uint8_t* ls, uint64_t pos)
{
    uint64_t v; memcpy(&v, ls + (pos >> 3), 8); v = __builtin_bswap64(v);
    return (uint32_t)((v << (pos & 7)) >> 32);
}
/* quadwords of the job's constant pool (local store 0x1E100 up): the shuffle patterns and masks a native's model assumes,
 * as the store holds them (big-endian words). A call checks the quadwords it relies on and declines when the store holds
 * anything else. */
typedef struct { uint32_t a; uint8_t b[16]; } PoolQ;
#define PB_(w) (uint8_t)((w) >> 24), (uint8_t)((w) >> 16), (uint8_t)((w) >> 8), (uint8_t)(w)
#define PQ(addr, w0, w1, w2, w3) { (addr), { PB_(w0), PB_(w1), PB_(w2), PB_(w3) } }
static int pool_ok(const uint8_t* ls, const PoolQ* p, int n)
{
    uint8x16_t d = vdupq_n_u8(0);
    for (int i = 0; i < n; i++) d = vorrq_u8(d, veorq_u8(vld1q_u8(ls + p[i].a), vld1q_u8(p[i].b)));
    return vmaxvq_u8(d) == 0;
}

/* ---------------------------------------------------------------------------------- 0x1C510: 3-of-4 quaternions */
/* What 0x1C510 does. It unpacks a bit-packed array of rotation quaternions that store three components (the fourth,
 * the largest, is rebuilt from them) and writes each to the joint its slot names:
 *     r3 = the stream (a byte address)   r4 = quaternions   r5 = joint array (a joint is 48 bytes, the quaternion its
 *     first quadword)   r6 = the slot array (a halfword each: joint number)   r7 = the header   r8 = flags
 * The header is a word H. Each of the three stored components has three fields: a sign width s (bit 31/21/11 of H,
 * 0 or 1), an exponent width e (4 bits below it) and a mantissa width m (5 bits below that); the component takes
 * s + e + m bits of the stream, one after the other for the three components and then item after item. H & 3 says
 * where the rebuilt component goes in the quaternion (the others take the stored ones in order). The word of the
 * stream at the component's first bit goes through three word rotates (by s, e, m: it is the job's code, rotating
 * where a width crosses the word's end): with e > 0 the component is the float of sign, exponent field
 * E + 128 - 2^(e-1) and mantissa M << (23 - m) (taken as bits); with e = 0 it is the two's complement number of the
 * sign and the mantissa field scaled by the reciprocal of 2^m - 1 (the job's Newton step). The fourth component is
 * sqrt(max(1 - x*x - y*y - z*z, 0)) in the job's estimate-and-refine form, or 1.0 where bit 0 of r8 is clear. The
 * job does four items at a time; the items past r4 in the last group are decoded and stored like the others (their
 * stream bits and slots are whatever follows the arrays).
 * It spills r80..r86 below the stack pointer and restores them, and leaves r0..r79 to the caller. The model runs on the
 * raw form only: the one operation that sees arbitrary bits, the rebuilt component's radicand, has its odd lanes handled
 * in q3_w. It declines a call whose stores would land in anything the job reads (the stream, the slots, the header, its
 * spill area: the job loads the next group before it stores a group) or outside the store. */
typedef struct {
    uint32_t s, e, m, w, smask, emask, mmask, bias, mshift;
    V rcp;                                                 /* e = 0: 1 / (2^m - 1) the way the job computes it */
} Q3C;

/* 1 / (2^m - 1) the way the job computes it (cuflt of the number, a reciprocal estimate, one Newton step), m = 0..31: a table
 * made on first use (the same values whoever makes it) */
static uint32_t s_rcp[32];
static int s_rcp_ready;
static uint32_t q3_rcp(uint32_t m)
{
    if (__builtin_expect(!__atomic_load_n(&s_rcp_ready, __ATOMIC_ACQUIRE), 0)) {
        for (uint32_t i = 0; i < 32; i++) {
            const u128 sc = spu_cuflt(spu_splat_u32(0x7FFFFFFFu >> (31 - i)), 155);
            const u128 est = spu_frest(sc);
            const u128 r0 = spu_fi(sc, est);
            const u128 r1 = spu_fma_lean(spu_fnms_lean(sc, r0, spu_splat_u32(F_ONE)), r0, r0);
            __atomic_store_n(&s_rcp[i], sc._u32[0] ? r1._u32[0] : 0u, __ATOMIC_RELAXED);
        }
        __atomic_store_n(&s_rcp_ready, 1, __ATOMIC_RELEASE);
    }
    return __atomic_load_n(&s_rcp[m], __ATOMIC_RELAXED);
}

static void q3c_init(Q3C* c, uint32_t s, uint32_t e, uint32_t m)
{
    c->s = s; c->e = e; c->m = m; c->w = s + e + m;
    c->smask = s ? 1u : 0u; c->emask = e ? (1u << e) - 1u : 0u; c->mmask = m ? 0xFFFFFFFFu >> (32 - m) : 0u;
    c->bias = 128u - (e ? 1u << (e - 1) : 0u);
    c->mshift = (23u - m) & 63u;                           /* >= 32: the field's bits all shift out */
    c->rcp = e ? K_(0) : K_(q3_rcp(m));
}

ALW V q3_rot(V w, uint32_t n)                              /* each word rotated left by n (0..31) */
{
    return vorrq_u32(vshlq_u32(w, vdupq_n_s32((int32_t)n)), vshlq_u32(w, vdupq_n_s32((int32_t)n - 32)));
}

/* the four items' component (words w: the stream at the component's first bit) */
ALW V q3_comp(const int m, spu_spec_acc* acc, const Q3C* c, V w)
{
    const V w1 = q3_rot(w, c->s);
    const V sg = vandq_u32(w1, K_(c->smask));
    const V w2 = q3_rot(w1, c->e);
    const V w3 = q3_rot(w2, c->m);
    const V mt = vandq_u32(w3, K_(c->mmask));
    if (c->e) {
        const V ex = vshlq_n_u32(vaddq_u32(vandq_u32(w2, K_(c->emask)), K_(c->bias)), 23);
        const V mn = c->mshift < 32 ? vshlq_u32(mt, vdupq_n_s32((int32_t)c->mshift)) : K_(0);
        return vorrq_u32(vorrq_u32(ex, mn), vshlq_n_u32(sg, 31));
    }
    /* e = 0: the signed number (the sign field is the bits above the mantissa's: it extends them) times 1/(2^m - 1) */
    const V ng = vtstq_u32(sg, sg);
    const V si = vorrq_u32(mt, vandq_u32(ng, K_(c->m < 32 ? 0xFFFFFFFFu << c->m : 0u)));
    const V f = UV_(vcvtq_f32_s32(vreinterpretq_s32_u32(si)));
    return flush_(m, fm_(m, acc, f, c->rcp));
}

/* the rebuilt fourth component (flag: bit 0 of r8): sqrt(max(1 - a*a - b*b - c*c, 0)) by the job's estimate and one
 * refinement. A lane whose radicand is zero after the clamp has +0 for its result whatever the estimates give (the last
 * step multiplies by that zero, and a product with a zero is +0): the clamp takes every lane with a component of 2 or
 * more in magnitude (its square, however large, leaves the radicand below zero) and every lane that rounds to zero. In
 * the raw form those lanes go through the estimates as 1.0 (no operation then sees a value it must not) and are
 * cleared at the end; the other lanes' radicand is a normal number no greater than 1 and every operand below is bounded. */
ALW V q3_w(const int m, spu_spec_acc* acc, V a, V b, V cc, uint32_t flag)
{
    const V one = K_(F_ONE), zero = W_(spu_zero());
    if (!flag) return one;
    V t = fnms_(m, acc, a, a, one);
    t = fnms_(m, acc, b, b, t);
    t = fnms_(m, acc, cc, cc, t);
    t = sel_(t, zero, fcgt_(zero, t));                     /* max(t, 0) */
    V zl = K_(0);
    if (m == M_RAW) {
        const V mx = vmaxq_u32(vandq_u32(a, K_(0x7FFFFFFFu)), vmaxq_u32(vandq_u32(b, K_(0x7FFFFFFFu)), vandq_u32(cc, K_(0x7FFFFFFFu))));
        zl = vorrq_u32(vceqzq_u32(vandq_u32(t, K_(0x7F800000u))), vcgeq_u32(mx, K_(0x40000000u)));
        t = sel_(t, one, zl);
    }
    const V r = recip_(t, W_(spu_frsqest(U_(t))));
    const V p = fm_(m, acc, t, r), h = fm_(m, acc, r, K_(F_HALF));
    const V g = fma_(m, acc, h, fnms_(m, acc, p, r, one), r);
    return vbicq_u32(fm_(m, acc, g, t), zl);
}

/* the pool quadwords of 0x1C510 */
static const PoolQ k_pool_q3[] = {
    PQ(0x1E150u, 0x0C0D0E0Fu, 0x1C1D1E1Fu, 0x08090A0Bu, 0x18191A1Bu), PQ(0x1E160u, 0x0C0D0E0Fu, 0x0C0D0E0Fu, 0x0C0D0E0Fu, 0x0C0D0E0Fu),
    PQ(0x1E170u, 0x08090A0Bu, 0x18191A1Bu, 0x0C0D0E0Fu, 0x1C1D1E1Fu), PQ(0x1E180u, 0x08090A0Bu, 0x0C0D0E0Fu, 0x10111213u, 0x14151617u),
    PQ(0x1E1A0u, 0x04050607u, 0x14151617u, 0x00010203u, 0x10111213u), PQ(0x1E1D0u, 0x00010203u, 0x10111213u, 0x04050607u, 0x14151617u),
    PQ(0x1E1E0u, 0x00010203u, 0x04050607u, 0x18191A1Bu, 0x1C1D1E1Fu), PQ(0x1E200u, 0x00010203u, 0x00010203u, 0x00010203u, 0x00010203u),
    PQ(0x1E340u, 0xE0000000u, 0xE0000000u, 0xE0000000u, 0xE0000000u), PQ(0x1E3A0u, 0x80800001u, 0x80800203u, 0x80800405u, 0x80800607u),
    PQ(0x1E4A0u, 0x7FFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu), PQ(0x1E4C0u, 0x00010203u, 0x10111213u, 0x80808080u, 0x80808080u),
    PQ(0x1E4E0u, 0x80808080u, 0x80808080u, 0x00010203u, 0x10111213u),
};

/* ---------------------------------------------------------------------------------- 0x17780: joint matrices */
/* What 0x17780 does. r3 = destination, r4 = source, r5 = joints. For each joint (a group is four: 192 bytes, three
 * quadwords a joint) it turns a rotation quaternion (quadword 0: x y z w), a translation (quadword 1) and a scale
 * (quadword 2: sx sy sz) into the joint's 3x4 matrix: three quadwords, the rows (R * S | t) with the rotation's
 * entries (0.5 - y*y - z*z) * (2 sx) and so on in the job's order of operations. Registers: none a caller can see.
 * The job's code is a software pipeline: it loads the next group before it stores a group, so a destination that
 * overlaps the source other than exactly (an in-place run) is declined. */
ALW void xform_group(const int m, spu_spec_acc* c, const V* a, V* o)
{
#define FM(x, y)       fm_(m, c, (x), (y))
#define FMA(x, y, z)   fma_(m, c, (x), (y), (z))
#define FNMS(x, y, z)  fnms_(m, c, (x), (y), (z))
    const V half = K_(F_HALF), two = K_(0x40000000u);
    V x, y, z, w, tx, ty, tz, tq, sx, sy, sz, sq;
    tr4(a[0], a[3], a[6], a[9], &x, &y, &z, &w);
    tr4(a[1], a[4], a[7], a[10], &tx, &ty, &tz, &tq);
    tr4(a[2], a[5], a[8], a[11], &sx, &sy, &sz, &sq);
    const V x2 = FNMS(x, x, half), xz = FM(x, z), y2 = FNMS(y, y, half), yz = FM(y, z);
    const V s2x = FM(sx, two), s2z = FM(sz, two), s2y = FM(sy, two);
    const V xy = FM(x, y);
    const V c22 = FNMS(y, y, x2), a20 = FNMS(w, y, xz), a12 = FNMS(w, x, yz), a02 = FMA(w, y, xz), a21 = FMA(w, x, yz);
    const V c00 = FNMS(z, z, y2), a01 = FNMS(w, z, xy);
    const V m22 = FM(c22, s2z), m20 = FM(a20, s2x), m02 = FM(a02, s2z);
    const V a10 = FMA(w, z, xy), c11 = FNMS(z, z, x2);
    const V m21 = FM(a21, s2y), m01 = FM(a01, s2y), m00 = FM(c00, s2x), m12 = FM(a12, s2z), m10 = FM(a10, s2x), m11 = FM(c11, s2y);
    V r[12];     /* row 0..2 of the four joints: (m r0, m r1, m r2, t r) as the lanes of a column each */
    tr4(flush_(m, m00), flush_(m, m01), flush_(m, m02), tx, &r[0], &r[3], &r[6], &r[9]);
    tr4(flush_(m, m10), flush_(m, m11), flush_(m, m12), ty, &r[1], &r[4], &r[7], &r[10]);
    tr4(flush_(m, m20), flush_(m, m21), flush_(m, m22), tz, &r[2], &r[5], &r[8], &r[11]);
    for (int i = 0; i < 12; i++) o[i] = r[i];
#undef FM
#undef FMA
#undef FNMS
}

/* ---------------------------------------------------------------------------------- 0x18B80: matrix products */
/* What 0x18B80 does. r3 = destination, r4 = matrices A, r5 = matrices B, r6 = count. A matrix is a 3x4 (three
 * quadwords, rows) and the job takes C[i] = A[i] * B[i] as affine transforms: row r of C is
 * A[r].x * B[0] + A[r].y * B[1] + A[r].z * B[2] + (0, 0, 0, A[r].w), the products added in that order (a
 * multiply, two multiply-adds, then the constant row (0 0 0 1) times A[r] added). The code loads the next
 * element's data before it stores an element: a destination that overlaps A or B other than exactly is declined. */
ALW void matmul_elem(const int m, spu_spec_acc* c, const V* a, const V* b, V k, V* o)
{
    for (int r = 0; r < 3; r++) {
        V t = fm_(m, c, b[0], vdupq_laneq_u32(a[r], 0));
        t = fma_(m, c, b[1], vdupq_laneq_u32(a[r], 1), t);
        t = fma_(m, c, b[2], vdupq_laneq_u32(a[r], 2), t);
        o[r] = flush_(m, fma_(m, c, k, a[r], t));
    }
}

static const PoolQ k_pool_x[] = {
    PQ(0x1E150u, 0x0C0D0E0Fu, 0x1C1D1E1Fu, 0x08090A0Bu, 0x18191A1Bu), PQ(0x1E170u, 0x08090A0Bu, 0x18191A1Bu, 0x0C0D0E0Fu, 0x1C1D1E1Fu),
    PQ(0x1E180u, 0x08090A0Bu, 0x0C0D0E0Fu, 0x10111213u, 0x14151617u), PQ(0x1E1A0u, 0x04050607u, 0x14151617u, 0x00010203u, 0x10111213u),
    PQ(0x1E1D0u, 0x00010203u, 0x10111213u, 0x04050607u, 0x14151617u),
};
static const PoolQ k_pool_mm[] = {
    PQ(0x1E190u, 0x08090A0Bu, 0x08090A0Bu, 0x08090A0Bu, 0x08090A0Bu), PQ(0x1E1B0u, 0x04050607u, 0x04050607u, 0x04050607u, 0x04050607u),
    PQ(0x1E200u, 0x00010203u, 0x00010203u, 0x00010203u, 0x00010203u), PQ(0x1E250u, 0x00000000u, 0x00000000u, 0x00000000u, 0x3F800000u),
};

/* ---------------------------------------------------------------------------------- 0x5814: the root transform loop */
/* What the loop at 0x5814 does. It is a loop inside the job's skeleton function (a "function" of its own in the lift:
 * the hook is at its entry and the job goes on at 0x5928 after it, natives-switch.txt). The joint matrices that 0x17780
 * made (r18 = the first, r25 = the end, three quadwords = rows a joint) are each multiplied by a 4x4 matrix whose
 * columns are r22, r21, r24, r23 (the matrix's fourth row is r86): with the rows R0 R1 R2 R3 = (R0, R1, R2, r86)
 * and column k of the element c_k = (R0[k], R1[k], R2[k], r86[k]), each result column is
 *     r22 * c_k.x + r21 * c_k.y + r24 * c_k.z + r23 * c_k.w   (a multiply, then three multiply-adds),
 * and the first three rows of the result take the element's place. r17, r19, r20 are the job's shuffle patterns (it
 * keeps them in registers; the model declines when they are not the ones it assumes). The loop does a pass first and
 * goes on while r25 > r18 (word 0, unsigned) after the pass's r18 += 48.
 * The registers: every pass leaves the same temporaries, so the model does the passes but the last natively and the
 * last one as the job's code has it (loop5814_pass, a transcription of the lifted statements): the registers and the
 * stores then are the loop's, exactly. */
static inline u128 ls_rd(const uint8_t* ls, uint32_t a) { return U_(LQ(ls, a & (SPU_LS_MASK & ~0xFu))); }
static inline void ls_wr(uint8_t* ls, uint32_t a, u128 v) { SQ(ls, a & (SPU_LS_MASK & ~0xFu), W_(v)); }

/* one pass of the lifted loop (its non-speculative statements), over the registers g and the store ls */
static void loop5814_pass(u128* g, uint8_t* ls)
{
    g[70] = ls_rd(ls, g[18]._u32[0] + 0x0);
    g[84] = ls_rd(ls, g[18]._u32[0] + 0x10);
    g[71] = ls_rd(ls, g[18]._u32[0] + 0x20);
    g[69] = spu_shufb(g[84], g[86], g[20]);
    g[5] = spu_shufb(g[70], g[71], g[20]);
    g[67] = spu_shufb(g[70], g[71], g[19]);
    g[68] = spu_shufb(g[84], g[86], g[19]);
    g[36] = spu_shufb(g[5], g[69], g[20]);
    g[39] = spu_shufb(g[5], g[69], g[19]);
    g[42] = spu_shufb(g[67], g[68], g[20]);
    g[44] = spu_shufb(g[67], g[68], g[19]);
    g[64] = spu_shufb(g[36], g[36], g[17]);
    g[63] = spu_shufb(g[39], g[39], g[17]);
    g[62] = spu_shufb(g[42], g[42], g[17]);
    g[61] = spu_shufb(g[44], g[44], g[17]);
    g[57] = spu_fm_lean(g[22], g[64]);
    g[60] = spu_rotqbyi(g[36], 4);
    g[51] = spu_fm_lean(g[22], g[63]);
    g[59] = spu_rotqbyi(g[44], 4);
    g[53] = spu_fm_lean(g[22], g[62]);
    g[58] = spu_rotqbyi(g[42], 4);
    g[55] = spu_fm_lean(g[22], g[61]);
    g[81] = spu_rotqbyi(g[39], 4);
    g[56] = spu_shufb(g[60], g[60], g[17]);
    g[54] = spu_shufb(g[59], g[59], g[17]);
    g[52] = spu_shufb(g[58], g[58], g[17]);
    g[50] = spu_shufb(g[81], g[81], g[17]);
    g[38] = spu_fma_lean(g[21], g[56], g[57]);
    g[49] = spu_rotqbyi(g[44], 8);
    g[46] = spu_fma_lean(g[21], g[54], g[55]);
    g[48] = spu_rotqbyi(g[42], 8);
    g[43] = spu_fma_lean(g[21], g[52], g[53]);
    g[47] = spu_rotqbyi(g[39], 8);
    g[41] = spu_fma_lean(g[21], g[50], g[51]);
    g[6] = spu_rotqbyi(g[36], 8);
    g[45] = spu_shufb(g[49], g[49], g[17]);
    g[14] = spu_shufb(g[48], g[48], g[17]);
    g[40] = spu_shufb(g[47], g[47], g[17]);
    g[37] = spu_shufb(g[6], g[6], g[17]);
    g[32] = spu_fma_lean(g[24], g[45], g[46]);
    g[35] = spu_rotqbyi(g[44], 12);
    g[15] = spu_fma_lean(g[24], g[14], g[43]);
    g[34] = spu_rotqbyi(g[42], 12);
    g[31] = spu_fma_lean(g[24], g[40], g[41]);
    g[33] = spu_rotqbyi(g[39], 12);
    g[29] = spu_fma_lean(g[24], g[37], g[38]);
    g[82] = spu_rotqbyi(g[36], 12);
    g[3] = spu_shufb(g[35], g[35], g[17]);
    g[10] = spu_shufb(g[34], g[34], g[17]);
    g[30] = spu_shufb(g[33], g[33], g[17]);
    g[28] = spu_shufb(g[82], g[82], g[17]);
    g[26] = spu_fma_lean(g[23], g[3], g[32]);
    g[13] = spu_fma_lean(g[23], g[10], g[15]);
    g[27] = spu_fma_lean(g[23], g[30], g[31]);
    g[7] = spu_fma_lean(g[23], g[28], g[29]);
    g[2] = spu_shufb(g[26], g[27], g[19]);
    g[4] = spu_shufb(g[13], g[7], g[19]);
    g[9] = spu_shufb(g[26], g[27], g[20]);
    g[11] = spu_shufb(g[13], g[7], g[20]);
    g[12] = spu_shufb(g[2], g[4], g[20]);
    g[79] = spu_shufb(g[2], g[4], g[19]);
    g[8] = spu_shufb(g[9], g[11], g[19]);
    ls_wr(ls, g[18]._u32[0] + 0x10, g[12]);
    ls_wr(ls, g[18]._u32[0] + 0x0, g[79]);
    ls_wr(ls, g[18]._u32[0] + 0x20, g[8]);
    g[18] = spu_ai(g[18], 48);
    g[78] = spu_clgt(g[25], g[18]);
}

ALW void loop_elem(const int m, spu_spec_acc* c, const V* r, V m22, V m21, V m24, V m23, V* o)
{
    V col[4];
#define COL(K)                                                                        \
    do {                                                                              \
        V a = fm_(m, c, m22, vdupq_laneq_u32(r[0], K));                               \
        a = fma_(m, c, m21, vdupq_laneq_u32(r[1], K), a);                             \
        a = fma_(m, c, m24, vdupq_laneq_u32(r[2], K), a);                             \
        col[K] = fma_(m, c, m23, vdupq_laneq_u32(r[3], K), a);                        \
    } while (0)
    COL(0); COL(1); COL(2); COL(3);
#undef COL
    V o0, o1, o2, o3;
    tr4(col[0], col[1], col[2], col[3], &o0, &o1, &o2, &o3);
    o[0] = flush_(m, o0); o[1] = flush_(m, o1); o[2] = flush_(m, o2);
}

static const uint32_t k_loop_pat[3][4] = {      /* r17, r19, r20 as the job holds them (checked against the registers) */
    { 0x00010203u, 0x00010203u, 0x00010203u, 0x00010203u }, { 0x00010203u, 0x10111213u, 0x04050607u, 0x14151617u },
    { 0x08090A0Bu, 0x18191A1Bu, 0x0C0D0E0Fu, 0x1C1D1E1Fu },
};

/* ---------------------------------------------------------------------------------- the calls */
enum { DECL_NONE = 0, DECL_ARGS, DECL_OVERLAP, DECL_FPCR, DECL_SHORT, DECL_N };   /* short: a loop of one pass */
enum { FN_BLEND = 0, FN_CHAN, FN_Q3, FN_XFORM, FN_MATMUL, FN_LOOP, FN_N };
typedef struct {
    unsigned long long calls, native, declined[DECL_N], groups, spec, exact, cmp, bad, regbad;
} Stat;                                                    /* groups: all; on the spec form; redone exact */
static Stat s_st[FN_N];
static const struct { const char* name; uint32_t addr; int ret; } k_fn[FN_N] = {   /* ret: returns a value in r3 */
    { "blend", 0x16320u, 0 }, { "channel", 0x17280u, 0 }, { "quat3", 0x1C510u, 0 }, { "xform", 0x17780u, 0 },
    { "matmul", 0x18B80u, 0 }, { "xloop", 0x5814u, 0 } };

#define OVL(l1, h1, l2, h2) ((l1) < (h2) && (l2) < (h1))
#define ADD(f, n) do { if (n) __atomic_fetch_add(&(f), (n), __ATOMIC_RELAXED); } while (0)

/* the weights and flags of group k: the four bytes at p + 4k, or 255 each (flags: 0) where the array is absent */
static inline uint32_t grp4(const uint8_t* ls, uint32_t p, uint32_t k, uint32_t absent)
{
    uint32_t w = absent;
    if (p) memcpy(&w, ls + p + 4u * k, 4);
    return w;
}

/* 0x16320 on (gpr, ls): 1 = done, 0 = declined (*why) and nothing written */
static int j218_blend(const u128* gpr, uint8_t* ls, int* why)
{
    const uint32_t sp = gpr[1]._u32[0], dst = gpr[3]._u32[0], wo = gpr[4]._u32[0], pa = gpr[5]._u32[0],
                   wa = gpr[6]._u32[0], pb = gpr[7]._u32[0], wb = gpr[8]._u32[0], n = gpr[10]._u32[0];
    uint32_t groups = 0;
    if (n) {
        if (n > 0x10000u) { *why = DECL_ARGS; return 0; }
        groups = (n + 3) >> 2;
        const uint64_t span = 192ull * groups, ws = 4ull * groups, lim = SPU_LS_SIZE;
        if ((dst | pa | pb) & 15u || (wo | wa | wb) & 3u || sp < 0x1E0u || sp > lim ||
            dst + span > lim || pa + span > lim || pb + span > lim || wo + ws > lim || (wa && wa + ws > lim) ||
            (wb && wb + ws > lim)) { *why = DECL_ARGS; return 0; }
        const uint64_t dh = dst + span, ah = pa + span, bh = pb + span, oh = wo + ws, sl = sp - 0x1E0u;
        const uint64_t aeh = wa + ws, beh = wb + ws;
        /* data: the destination may sit exactly on a pose; the output weights exactly on a pose's weights */
        if ((dst != pa && OVL(dst, dh, pa, ah)) || (dst != pb && OVL(dst, dh, pb, bh)) ||
            OVL(dst, dh, wo, oh) || (wa && OVL(dst, dh, wa, aeh)) || (wb && OVL(dst, dh, wb, beh)) ||
            OVL(wo, oh, pa, ah) || OVL(wo, oh, pb, bh) ||
            (wa && wo != wa && OVL(wo, oh, wa, aeh)) || (wb && wo != wb && OVL(wo, oh, wb, beh)) ||
            OVL(sl, (uint64_t)sp, dst, dh) || OVL(sl, (uint64_t)sp, pa, ah) || OVL(sl, (uint64_t)sp, pb, bh) ||
            OVL(sl, (uint64_t)sp, wo, oh) || (wa && OVL(sl, (uint64_t)sp, wa, aeh)) ||
            (wb && OVL(sl, (uint64_t)sp, wb, beh))) { *why = DECL_OVERLAP; return 0; }
    }
    /* the spills of r80..r109 (the lifted code writes them first and restores them at the end) */
    for (int i = 0; i < 30; i++) SQ(ls, (sp - 0x10u * (uint32_t)(i + 1)) & (SPU_LS_MASK & ~0xFu), W_(gpr[80 + i]));
    if (!n) return 1;
    const uint32_t tw = gpr[9]._u32[0];
    const V tt = K_(tw);
    const int traw = (tw & 0x7FFFFFFFu) <= RAW_TMAX;       /* the raw form's bound on t */
    unsigned nspec = 0, nexact = 0;
    for (uint32_t k = 0; k < groups; k++) {
        V a[12], b[12], o[12];
        for (int j = 0; j < 12; j++) { a[j] = LQ(ls, pa + 192u * k + 16u * (uint32_t)j); b[j] = LQ(ls, pb + 192u * k + 16u * (uint32_t)j); }
        const uint32_t ba = grp4(ls, wa, k, 0xFFFFFFFFu), bb = grp4(ls, wb, k, 0xFFFFFFFFu);
        uint32_t wo4;
        spu_spec_acc acc = SPU_SPEC_INIT();
        if (traw && raw_ok(a, b, 12)) blend_group(M_RAW, &acc, a, b, ba, bb, tt, o, &wo4);
        else {
            nspec++;
            blend_group(M_SPEC, &acc, a, b, ba, bb, tt, o, &wo4);
            if (__builtin_expect(!SPU_SPEC_OK(acc), 0)) { blend_group(M_EXACT, &acc, a, b, ba, bb, tt, o, &wo4); nexact++; }
        }
        for (int j = 0; j < 12; j++) SQ(ls, dst + 192u * k + 16u * (uint32_t)j, o[j]);
        memcpy(ls + wo + 4u * k, &wo4, 4);
    }
    ADD(s_st[FN_BLEND].groups, groups); ADD(s_st[FN_BLEND].spec, nspec); ADD(s_st[FN_BLEND].exact, nexact);
    return 1;
}

/* 0x17280 on (gpr, ls): the same, over one float a joint. r3 = destination, r4 = output weight bytes, r5 / r7 = the
 * two arrays, r6 / r8 = their weight bytes, r9 = flag bytes (null = none), r10 = t, r11 = joints. It spills nothing
 * and changes no register a caller keeps. */
static int j218_chan(const u128* gpr, uint8_t* ls, int* why)
{
    const uint32_t dst = gpr[3]._u32[0], wo = gpr[4]._u32[0], pa = gpr[5]._u32[0], wa = gpr[6]._u32[0],
                   pb = gpr[7]._u32[0], wb = gpr[8]._u32[0], fl = gpr[9]._u32[0], n = gpr[11]._u32[0];
    if (!n) return 1;
    if (n > 0x10000u) { *why = DECL_ARGS; return 0; }
    const uint32_t groups = (n + 3) >> 2;
    const uint64_t span = 16ull * groups, ws = 4ull * groups, lim = SPU_LS_SIZE;
    if ((dst | pa | pb) & 15u || (wo | wa | wb | fl) & 3u || dst + span > lim || pa + span > lim || pb + span > lim ||
        wo + ws > lim || (wa && wa + ws > lim) || (wb && wb + ws > lim) || (fl && fl + ws > lim)) { *why = DECL_ARGS; return 0; }
    const uint64_t dh = dst + span, ah = pa + span, bh = pb + span, oh = wo + ws, aeh = wa + ws, beh = wb + ws, feh = fl + ws;
    if ((dst != pa && OVL(dst, dh, pa, ah)) || (dst != pb && OVL(dst, dh, pb, bh)) ||
        OVL(dst, dh, wo, oh) || (wa && OVL(dst, dh, wa, aeh)) || (wb && OVL(dst, dh, wb, beh)) || (fl && OVL(dst, dh, fl, feh)) ||
        OVL(wo, oh, pa, ah) || OVL(wo, oh, pb, bh) || (fl && OVL(wo, oh, fl, feh)) ||
        (wa && wo != wa && OVL(wo, oh, wa, aeh)) || (wb && wo != wb && OVL(wo, oh, wb, beh))) { *why = DECL_OVERLAP; return 0; }
    const uint32_t tw = gpr[10]._u32[0];
    const V tt = K_(tw);
    const int traw = (tw & 0x7FFFFFFFu) <= RAW_TMAX;
    unsigned nspec = 0, nexact = 0;
    for (uint32_t k = 0; k < groups; k++) {
        const V a = LQ(ls, pa + 16u * k), b = LQ(ls, pb + 16u * k);
        const uint32_t ba = grp4(ls, wa, k, 0xFFFFFFFFu), bb = grp4(ls, wb, k, 0xFFFFFFFFu), bf = grp4(ls, fl, k, 0);
        uint32_t wo4;
        V o;
        spu_spec_acc acc = SPU_SPEC_INIT();
        if (traw && raw_ok(&a, &b, 1)) o = chan_group(M_RAW, &acc, a, b, ba, bb, bf, tt, &wo4);
        else {
            nspec++;
            o = chan_group(M_SPEC, &acc, a, b, ba, bb, bf, tt, &wo4);
            if (__builtin_expect(!SPU_SPEC_OK(acc), 0)) { o = chan_group(M_EXACT, &acc, a, b, ba, bb, bf, tt, &wo4); nexact++; }
        }
        SQ(ls, dst + 16u * k, o);
        memcpy(ls + wo + 4u * k, &wo4, 4);
    }
    ADD(s_st[FN_CHAN].groups, groups); ADD(s_st[FN_CHAN].spec, nspec); ADD(s_st[FN_CHAN].exact, nexact);
    return 1;
}

/* the stack frame's spills of r80..r(80+cnt-1) (below sp, the first thing the job's code does) */
static void spill_regs(const u128* gpr, uint8_t* ls, uint32_t sp, int cnt)
{
    for (int i = 0; i < cnt; i++) SQ(ls, (sp - 0x10u * (uint32_t)(i + 1)) & (SPU_LS_MASK & ~0xFu), W_(gpr[80 + i]));
}

/* 0x1C510 on (gpr, ls): see the model above */
static int j218_q3(const u128* gpr, uint8_t* ls, int* why)
{
    const uint32_t sp = gpr[1]._u32[0], r3 = gpr[3]._u32[0], n = gpr[4]._u32[0], dst = gpr[5]._u32[0], slots = gpr[6]._u32[0],
                   hp = gpr[7]._u32[0], flag = gpr[8]._u32[0] & 1u;
    const uint64_t lim = SPU_LS_SIZE;
    if (sp < 0x80u || sp > lim) { *why = DECL_ARGS; return 0; }
    if (!n) { spill_regs(gpr, ls, sp, 7); return 1; }
    if (n > 0x10000u || (slots & 7u) || hp + 32ull > lim) { *why = DECL_ARGS; return 0; }
    if (!pool_ok(ls, k_pool_q3, (int)(sizeof k_pool_q3 / sizeof k_pool_q3[0]))) { *why = DECL_ARGS; return 0; }
    const uint32_t groups = (n + 3) >> 2;
    const uint32_t hdr = be32(ls + hp);                    /* (the job reads it after its spills: checked off below) */
    Q3C ca, cb, cc;
    q3c_init(&ca, (hdr >> 31) & 1u, (hdr >> 27) & 15u, (hdr >> 22) & 31u);
    q3c_init(&cb, (hdr >> 21) & 1u, (hdr >> 17) & 15u, (hdr >> 12) & 31u);
    q3c_init(&cc, (hdr >> 11) & 1u, (hdr >> 7) & 15u, (hdr >> 2) & 31u);
    const uint32_t wa = ca.w, wb = cb.w, wtot = wa + wb + cc.w, place = hdr & 3u;
    const uint64_t bit0 = (uint64_t)r3 * 8u;
    /* read ranges: the stream (item windows are two quadwords; the job also loads the next group's), the slot array
     * (the next group's as well), the header's two quadwords */
    const uint64_t sLo = r3 & ~15ull, sHi = (((bit0 + (4ull * groups + 3) * wtot) >> 3) & ~15ull) + 32;
    const uint64_t aLo = slots & ~15ull, aHi = ((slots + 8ull * groups) & ~15ull) + 16;
    const uint64_t hLo = hp & ~15ull, hHi = hLo + 32;
    const uint64_t spLo = (sp - 0x70u) & ~15ull, spHi = ((sp - 0x10u) & ~15ull) + 16;
    /* the model's own reads reach a few bytes past the windows the job loads: all of them must be in the store */
    if (r3 + (((4ull * groups + 3) * wtot) >> 3) + 48 > lim || aHi > lim) { *why = DECL_ARGS; return 0; }
    if (OVL(spLo, spHi, sLo, sHi) || OVL(spLo, spHi, aLo, aHi) || OVL(spLo, spHi, hLo, hHi)) { *why = DECL_OVERLAP; return 0; }
    for (uint32_t i = 0; i < 4u * groups; i++) {           /* where each item goes: inside the store, off every range read or spilled */
        const uint64_t at = (uint64_t)dst + 48ull * be16(ls + slots + 2u * i);
        const uint64_t lo = at & ~15ull;
        if (at + 16 > lim || OVL(lo, lo + 16, sLo, sHi) || OVL(lo, lo + 16, aLo, aHi) || OVL(lo, lo + 16, hLo, hHi) ||
            OVL(lo, lo + 16, spLo, spHi)) { *why = at + 16 > lim ? DECL_ARGS : DECL_OVERLAP; return 0; }
    }
    spill_regs(gpr, ls, sp, 7);
    for (uint32_t g = 0; g < groups; g++) {
        uint32_t xa[4], xb[4], xc[4], at[4];
        for (uint32_t k = 0; k < 4; k++) {
            const uint64_t pos = bit0 + (uint64_t)(4u * g + k) * wtot;
            xa[k] = bits32(ls, pos); xb[k] = bits32(ls, pos + wa); xc[k] = bits32(ls, pos + wa + wb);
            at[k] = (uint32_t)(dst + 48u * be16(ls + slots + 2u * (4u * g + k))) & (SPU_LS_MASK & ~0xFu);
        }
        spu_spec_acc acc = SPU_SPEC_INIT();
        const V va = q3_comp(M_RAW, &acc, &ca, vld1q_u32(xa)), vb = q3_comp(M_RAW, &acc, &cb, vld1q_u32(xb)),
                vc = q3_comp(M_RAW, &acc, &cc, vld1q_u32(xc));
        const V w = q3_w(M_RAW, &acc, va, vb, vc, flag);
        V q0, q1, q2, q3;
        switch (place) {
        case 0:  q0 = w;  q1 = va; q2 = vb; q3 = vc; break;
        case 1:  q0 = va; q1 = w;  q2 = vb; q3 = vc; break;
        case 2:  q0 = va; q1 = vb; q2 = w;  q3 = vc; break;
        default: q0 = va; q1 = vb; q2 = vc; q3 = w;  break;
        }
        V i0, i1, i2, i3;
        tr4(q0, q1, q2, q3, &i0, &i1, &i2, &i3);
        SQ(ls, at[0], i0); SQ(ls, at[1], i1); SQ(ls, at[2], i2); SQ(ls, at[3], i3);
    }
    ADD(s_st[FN_Q3].groups, groups);
    return 1;
}

/* 0x17780 on (gpr, ls): see the model above */
static int j218_xform(const u128* gpr, uint8_t* ls, int* why)
{
    const uint32_t dst = gpr[3]._u32[0], src = gpr[4]._u32[0], n = gpr[5]._u32[0];
    if (!n) return 1;
    if (n > 0x10000u) { *why = DECL_ARGS; return 0; }
    const uint32_t groups = (n + 3) >> 2;
    const uint64_t span = 192ull * groups, lim = SPU_LS_SIZE;
    if ((dst | src) & 15u || dst + span > lim || src + span > lim ||
        !pool_ok(ls, k_pool_x, (int)(sizeof k_pool_x / sizeof k_pool_x[0]))) { *why = DECL_ARGS; return 0; }
    if (dst != src && OVL(dst, dst + span, src, src + span)) { *why = DECL_OVERLAP; return 0; }
    unsigned nspec = 0, nexact = 0;
    for (uint32_t g = 0; g < groups; g++) {
        V a[12], o[12];
        for (int j = 0; j < 12; j++) a[j] = LQ(ls, src + 192u * g + 16u * (uint32_t)j);
        /* the floats that are operated on: the rotations and the scales (a translation is only moved);
         * a product of three of them must stay below FLT_MAX: 2^36 */
        const V qs[8] = { a[0], a[3], a[6], a[9], a[2], a[5], a[8], a[11] };
        V mx = vshlq_n_u32(qs[0], 1);
        for (int j = 1; j < 8; j++) mx = vmaxq_u32(mx, vshlq_n_u32(qs[j], 1));
        spu_spec_acc acc = SPU_SPEC_INIT();
        if (vmaxvq_u32(mx) < (0x51800000u << 1)) xform_group(M_RAW, &acc, a, o);
        else {
            nspec++;
            xform_group(M_SPEC, &acc, a, o);
            if (__builtin_expect(!SPU_SPEC_OK(acc), 0)) { xform_group(M_EXACT, &acc, a, o); nexact++; }
        }
        for (int j = 0; j < 12; j++) SQ(ls, dst + 192u * g + 16u * (uint32_t)j, o[j]);
    }
    ADD(s_st[FN_XFORM].groups, groups); ADD(s_st[FN_XFORM].spec, nspec); ADD(s_st[FN_XFORM].exact, nexact);
    return 1;
}

/* 0x18B80 on (gpr, ls): see the model above */
static int j218_matmul(const u128* gpr, uint8_t* ls, int* why)
{
    const uint32_t dst = gpr[3]._u32[0], pa = gpr[4]._u32[0], pb = gpr[5]._u32[0], n = gpr[6]._u32[0];
    if (!n) return 1;
    if (n > 0x10000u) { *why = DECL_ARGS; return 0; }
    const uint64_t span = 48ull * n, lim = SPU_LS_SIZE;
    if ((dst | pa | pb) & 15u || dst + span > lim || pa + span > lim || pb + span > lim ||
        !pool_ok(ls, k_pool_mm, (int)(sizeof k_pool_mm / sizeof k_pool_mm[0]))) { *why = DECL_ARGS; return 0; }
    if ((dst != pa && OVL(dst, dst + span, pa, pa + span)) || (dst != pb && OVL(dst, dst + span, pb, pb + span))) { *why = DECL_OVERLAP; return 0; }
    const V k = LQ(ls, 0x1E250u);
    unsigned nspec = 0, nexact = 0;
    for (uint32_t e = 0; e < n; e++) {
        V a[3], b[3], o[3];
        for (int j = 0; j < 3; j++) { a[j] = LQ(ls, pa + 48u * e + 16u * (uint32_t)j); b[j] = LQ(ls, pb + 48u * e + 16u * (uint32_t)j); }
        spu_spec_acc acc = SPU_SPEC_INIT();
        if (raw_ok(a, b, 3)) matmul_elem(M_RAW, &acc, a, b, k, o);
        else {
            nspec++;
            matmul_elem(M_SPEC, &acc, a, b, k, o);
            if (__builtin_expect(!SPU_SPEC_OK(acc), 0)) { matmul_elem(M_EXACT, &acc, a, b, k, o); nexact++; }
        }
        for (int j = 0; j < 3; j++) SQ(ls, dst + 48u * e + 16u * (uint32_t)j, o[j]);
    }
    ADD(s_st[FN_MATMUL].groups, n); ADD(s_st[FN_MATMUL].spec, nspec); ADD(s_st[FN_MATMUL].exact, nexact);
    return 1;
}

/* 0x5814 on (g, ls): the passes but the last natively, the last one as the job has it; see the model above */
static int j218_loop(u128* g, uint8_t* ls, int* why)
{
    const uint32_t a = g[18]._u32[0], e = g[25]._u32[0];
    const uint64_t lim = SPU_LS_SIZE;
    for (int i = 0; i < 3; i++) {
        const u128* pr = &g[i == 0 ? 17 : i == 1 ? 19 : 20];
        for (int k = 0; k < 4; k++) if (pr->_u32[k] != k_loop_pat[i][k]) { *why = DECL_ARGS; return 0; }
    }
    if (a & 15u) { *why = DECL_ARGS; return 0; }
    const uint64_t passes = e > a ? ((uint64_t)(e - a) + 47) / 48 : 1;
    if (a + 48 * passes > lim) { *why = DECL_ARGS; return 0; }
    if (passes < 2) { *why = DECL_SHORT; return 0; }              /* one pass: nothing to do natively */
    const V m22 = W_(g[22]), m21 = W_(g[21]), m24 = W_(g[24]), m23 = W_(g[23]), r3 = W_(g[86]);
    const V mm[5] = { m22, m21, m24, m23, r3 };
    const int mraw = raw_okv(mm, 5);
    unsigned nspec = 0, nexact = 0;
    for (uint64_t k = 0; k + 1 < passes; k++) {
        const uint32_t at = a + 48u * (uint32_t)k;
        V r[4] = { LQ(ls, at), LQ(ls, at + 16), LQ(ls, at + 32), r3 }, o[3];
        spu_spec_acc acc = SPU_SPEC_INIT();
        if (mraw && raw_okv(r, 3)) loop_elem(M_RAW, &acc, r, m22, m21, m24, m23, o);
        else {
            nspec++;
            loop_elem(M_SPEC, &acc, r, m22, m21, m24, m23, o);
            if (__builtin_expect(!SPU_SPEC_OK(acc), 0)) { loop_elem(M_EXACT, &acc, r, m22, m21, m24, m23, o); nexact++; }
        }
        SQ(ls, at, o[0]); SQ(ls, at + 16, o[1]); SQ(ls, at + 32, o[2]);
    }
    const uint32_t adv = 48u * (uint32_t)(passes - 1);
    for (int k = 0; k < 4; k++) g[18]._u32[k] += adv;
    loop5814_pass(g, ls);
    ADD(s_st[FN_LOOP].groups, passes); ADD(s_st[FN_LOOP].spec, nspec); ADD(s_st[FN_LOOP].exact, nexact);
    return 1;
}

static int j218_core(int fn, u128* gpr, uint8_t* ls, int* why)
{
    switch (fn) {
    case FN_BLEND: return j218_blend(gpr, ls, why);
    case FN_CHAN:  return j218_chan(gpr, ls, why);
    case FN_XFORM: return j218_xform(gpr, ls, why);
    case FN_MATMUL: return j218_matmul(gpr, ls, why);
    case FN_LOOP:  return j218_loop(gpr, ls, why);
    default:       return j218_q3(gpr, ls, why);
    }
}

/* ---------------------------------------------------------------------------------- entry, check run, stats */
static int s_mode = -1;                                    /* PS3_NATIVE_J218 */
static unsigned long long s_calls;
static _Thread_local uint8_t* t_ls;                        /* the check run's copy of the store: one a thread */
static _Thread_local int t_bypass;                         /* the lifted function runs inside a check run */
static pthread_key_t s_ls_key;
static pthread_once_t s_ls_once = PTHREAD_ONCE_INIT;
static int s_ls_key_ok;
static void ls_key_make(void) { s_ls_key_ok = pthread_key_create(&s_ls_key, free) == 0; }   /* the copy is freed when its thread ends */

/* this thread's copy buffer (allocated once, kept for the thread's life), or null */
static uint8_t* check_buf(void)
{
    if (t_ls) return t_ls;
    pthread_once(&s_ls_once, ls_key_make);
    if (!s_ls_key_ok) return 0;                            /* no key: no check run (the native still runs) */
    uint8_t* b = (uint8_t*)malloc(SPU_LS_SIZE);
    if (!b) return 0;
    if (pthread_setspecific(s_ls_key, b)) { free(b); return 0; }
    return t_ls = b;
}

static void stats(unsigned long long n)
{
    fprintf(stderr, "[j218] %llu calls", n);
    for (int f = 0; f < FN_N; f++) {
        const Stat* t = &s_st[f];
        fprintf(stderr, "; %s 0x%X: native %llu, declined %llu (args %llu, overlap %llu, fpcr %llu, short %llu), groups %llu "
                "(%llu not raw, %llu of those redone exact)", k_fn[f].name, k_fn[f].addr, t->native,
                t->declined[DECL_ARGS] + t->declined[DECL_OVERLAP] + t->declined[DECL_FPCR] + t->declined[DECL_SHORT],
                t->declined[DECL_ARGS], t->declined[DECL_OVERLAP], t->declined[DECL_FPCR], t->declined[DECL_SHORT], t->groups,
                t->spec, t->exact);
        if (s_mode == 2) fprintf(stderr, ", compared %llu, differ %llu (%llu in registers)", t->cmp, t->bad, t->regbad);
    }
    fputc('\n', stderr);
}

/* mode 2: the model on a copy, the lifted function on the real store, then the comparison. The registers a caller can
 * rely on are compared: r0, r1, the callee-saved r80..r127 and, for a function that returns a value, r3 (none of
 * these do: each returns void, and the callers never read r2..r79 after the call). */
static int j218_check(spu_context* ctx, int fn)
{
    Stat* st = &s_st[fn];
    void (*lifted)(spu_context*) = spu_lookup(k_fn[fn].addr, ctx->resident_ovl);
    if (!lifted) return 0;
    uint8_t* const cp = check_buf();
    if (!cp) return 0;
    memcpy(cp, ctx->ls, SPU_LS_SIZE);
    int why = DECL_NONE;
    u128 g[128];
    memcpy(g, ctx->gpr, sizeof g);
    if (fn == FN_LOOP) {
        /* a loop inside the job's function: it cannot be run as a function (it goes on to the rest of the job). The model
         * runs on a copy of the store and of every register; the loop's passes then run on the real context as the
         * job's code has them (loop5814_pass), and the stores and all 128 registers are compared. */
        if (!j218_core(fn, g, cp, &why)) { ADD(st->declined[why], 1); return 0; }
        unsigned pass = 0;
        do { loop5814_pass(ctx->gpr, ctx->ls); } while (ctx->gpr[78]._u32[0] != 0 && ++pass < 0x10000);
        uint32_t first = 0, last = 0, bytes = 0;
        for (uint32_t i = 0; i < SPU_LS_SIZE; i += 16) {
            if (!memcmp(ctx->ls + i, cp + i, 16)) continue;
            for (uint32_t k = i; k < i + 16; k++)
                if (ctx->ls[k] != cp[k]) { if (!bytes) first = k; last = k; bytes++; }
        }
        int reg = -1;
        for (int r = 0; r < 128 && reg < 0; r++) if (memcmp(&ctx->gpr[r], &g[r], 16)) reg = r;
        ADD(st->cmp, 1);
        if (reg >= 0) ADD(st->regbad, 1);
        if (bytes || reg >= 0) {
            const unsigned long long nb = __atomic_add_fetch(&st->bad, 1, __ATOMIC_RELAXED);
            if (nb <= 12)
                fprintf(stderr, "[j218] check DIFFER (%s) #%llu: r18=%X r25=%X: %u bytes differ in 0x%05X..0x%05X, register r%d\n",
                        k_fn[fn].name, nb, ctx->gpr[18]._u32[0], ctx->gpr[25]._u32[0], bytes, first, last, reg);
        }
        return 1;                                      /* the loop's result (the job's) is what stays */
    }
    if (!j218_core(fn, ctx->gpr, cp, &why)) { ADD(st->declined[why], 1); return 0; }
    const int32_t si = (int32_t)ctx->image_id;
    const uint32_t r0 = ctx->gpr[0]._u32[0];
    t_bypass = 1;
    ctx->host_depth++; lifted(ctx); spu_drain_call(ctx, r0); ctx->host_depth--;
    spu_img_restore(ctx, si);
    t_bypass = 0;
    uint32_t first = 0, last = 0, bytes = 0;
    for (uint32_t i = 0; i < SPU_LS_SIZE; i += 16) {
        if (!memcmp(ctx->ls + i, cp + i, 16)) continue;
        for (uint32_t k = i; k < i + 16; k++)
            if (ctx->ls[k] != cp[k]) { if (!bytes) first = k; last = k; bytes++; }
    }
    int reg = -1;
    for (int r = 0; r < 128 && reg < 0; r++)
        if ((r < 2 || r >= 80 || (r == 3 && k_fn[fn].ret)) && memcmp(&ctx->gpr[r], &g[r], 16)) reg = r;
    ADD(st->cmp, 1);
    if (reg >= 0) ADD(st->regbad, 1);
    if (bytes || reg >= 0) {
        const unsigned long long nb = __atomic_add_fetch(&st->bad, 1, __ATOMIC_RELAXED);
        if (nb <= 12)
            fprintf(stderr, "[j218] check DIFFER (%s) #%llu: r1=%05X r3..r9 = %X %X %X %X %X %X %X: %u bytes differ in "
                    "0x%05X..0x%05X (lifted %02X native %02X at the first), register r%d\n", k_fn[fn].name, nb, g[1]._u32[0],
                    g[3]._u32[0], g[4]._u32[0], g[5]._u32[0], g[6]._u32[0], g[7]._u32[0], g[8]._u32[0], g[9]._u32[0],
                    bytes, first, last, bytes ? ctx->ls[first] : 0, bytes ? cp[first] : 0, reg);
    }
    ctx->pc = ctx->gpr[0]._u32[0];
    return 1;
}

static int j218_entry(spu_context* ctx, int fn)
{
    if (t_bypass) return 0;
    if (__builtin_expect(__atomic_load_n(&s_mode, __ATOMIC_ACQUIRE) < 0, 0)) {
        const char* e = getenv("PS3_NATIVE_J218");
        const int m = e && e[0] ? (e[0] == '2' ? 2 : e[0] != '0') : 1;
        fprintf(stderr, "[j218] native skeletal animation kernels (image %d, %d functions): %s\n", J218_IMAGE, FN_N,
                m == 2 ? "check run against the lifted code" : m ? "on" : "off");
        __atomic_store_n(&s_mode, m, __ATOMIC_RELEASE);
    }
    if (!s_mode) return 0;
    Stat* st = &s_st[fn];
    const unsigned long long n = __atomic_add_fetch(&s_calls, 1, __ATOMIC_RELAXED);
    if (!(n & 0x3FFF)) stats(n);
    __atomic_fetch_add(&st->calls, 1, __ATOMIC_RELAXED);
#if SPU_A64_FPCR
    /* the helpers of this TU assume the SPU run's FPCR (RZ + FZ): any other, the lifted code decides */
    if ((spu__fpcr_get() & SPU_FPCR_RZFZ) != SPU_FPCR_RZFZ) { ADD(st->declined[DECL_FPCR], 1); return 0; }
#endif
    if (s_mode == 2) {
        const int r = j218_check(ctx, fn);
        if (r) ADD(st->native, 1);
        return r;
    }
    int why = DECL_NONE;
    if (!j218_core(fn, ctx->gpr, ctx->ls, &why)) { ADD(st->declined[why], 1); return 0; }
    ADD(st->native, 1);
    return 1;
}
static int j218_entry_blend(spu_context* ctx) { return j218_entry(ctx, FN_BLEND); }
static int j218_entry_chan(spu_context* ctx) { return j218_entry(ctx, FN_CHAN); }
static int j218_entry_q3(spu_context* ctx) { return j218_entry(ctx, FN_Q3); }
static int j218_entry_xform(spu_context* ctx) { return j218_entry(ctx, FN_XFORM); }
static int j218_entry_matmul(spu_context* ctx) { return j218_entry(ctx, FN_MATMUL); }
static int j218_entry_loop(spu_context* ctx) { return j218_entry(ctx, FN_LOOP); }

__attribute__((constructor)) static void j218_register(void)
{
    spu_native_register(J218_IMAGE, k_fn[FN_BLEND].addr, j218_entry_blend);
    spu_native_register(J218_IMAGE, k_fn[FN_CHAN].addr, j218_entry_chan);
    spu_native_register(J218_IMAGE, k_fn[FN_Q3].addr, j218_entry_q3);
    spu_native_register(J218_IMAGE, k_fn[FN_XFORM].addr, j218_entry_xform);
    spu_native_register(J218_IMAGE, k_fn[FN_MATMUL].addr, j218_entry_matmul);
    spu_native_register(J218_IMAGE, k_fn[FN_LOOP].addr, j218_entry_loop);
}
