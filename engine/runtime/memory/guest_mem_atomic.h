/*
 * ps3recomp - guest memory accesses with the Cell's single-copy atomicity.
 *
 * On Cell an aligned load or store of up to 8 bytes is single-copy atomic:
 * another PPU thread or an SPU's DMA sees all of it or none of it. Guest code
 * depends on that -- a spin flag read with a plain lwz while another thread
 * stores it, a lock-free fast path reading a word a peer updates -- and does
 * not wrap those accesses in anything. Emulated with memcpy they are C data
 * races: undefined behaviour the compiler may tear, merge or cache, and what
 * ThreadSanitizer reports on every SPURS suite.
 *
 * These helpers do aligned 1/2/4/8-byte guest accesses as relaxed atomics
 * (the same single instruction on x86-64 and ARM64, so no cost) and fall back
 * to memcpy only when the address is misaligned, where the guest access was
 * not atomic either. Bulk copies (DMA, a 128-byte reservation line) move whole
 * aligned doublewords atomically, which is at least as strong as the MFC.
 * Values stay in guest (big-endian) byte order; callers byte-swap.
 */
#ifndef PS3RECOMP_GUEST_MEM_ATOMIC_H
#define PS3RECOMP_GUEST_MEM_ATOMIC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define GM_RELAXED __ATOMIC_RELAXED

static inline uint8_t gm_load8(const uint8_t* p)
{
    return __atomic_load_n(p, GM_RELAXED);
}
static inline uint16_t gm_load16(const uint8_t* p)
{
    if (((uintptr_t)p & 1u) == 0) return __atomic_load_n((const uint16_t*)p, GM_RELAXED);
    uint16_t v; memcpy(&v, p, 2); return v;
}
static inline uint32_t gm_load32(const uint8_t* p)
{
    if (((uintptr_t)p & 3u) == 0) return __atomic_load_n((const uint32_t*)p, GM_RELAXED);
    uint32_t v; memcpy(&v, p, 4); return v;
}
static inline uint64_t gm_load64(const uint8_t* p)
{
    if (((uintptr_t)p & 7u) == 0) return __atomic_load_n((const uint64_t*)p, GM_RELAXED);
    uint64_t v; memcpy(&v, p, 8); return v;
}

static inline void gm_store8(uint8_t* p, uint8_t v)
{
    __atomic_store_n(p, v, GM_RELAXED);
}
static inline void gm_store16(uint8_t* p, uint16_t v)
{
    if (((uintptr_t)p & 1u) == 0) { __atomic_store_n((uint16_t*)p, v, GM_RELAXED); return; }
    memcpy(p, &v, 2);
}
static inline void gm_store32(uint8_t* p, uint32_t v)
{
    if (((uintptr_t)p & 3u) == 0) { __atomic_store_n((uint32_t*)p, v, GM_RELAXED); return; }
    memcpy(p, &v, 4);
}
static inline void gm_store64(uint8_t* p, uint64_t v)
{
    if (((uintptr_t)p & 7u) == 0) { __atomic_store_n((uint64_t*)p, v, GM_RELAXED); return; }
    memcpy(p, &v, 8);
}

/* PS3_GM_WIDE_COPY (default 1 under __SWITCH__, 0 elsewhere; -DPS3_GM_WIDE_COPY=0 = the one-doubleword-per-step loops, kept below as
 * gm_copy_from_scalar / gm_copy_to_scalar for an A/B and as the reference in the tests).
 *
 * The bulk of a DMA (a GET of a few KB into local store, a 128-byte reservation line) used to move one
 * relaxed 8-byte atomic per loop step: on the Switch's Cortex-A57 that is one load and one store per
 * 8 bytes with no way for the compiler to do better, because it must not merge atomics. The wide form
 * moves 16 bytes per access, 64 per loop step, but only at 16-byte-aligned guest addresses, where a
 * 16-byte access is two aligned doublewords to the memory system: x86-64 (movdqa, a single access that
 * no processor splits inside an aligned doubleword) and AArch64 (LDR/STR of a Q register, which the
 * architecture treats as two single-copy-atomic 64-bit accesses when 16-byte aligned; an unaligned
 * 128-bit access is NOT given that guarantee, so none is ever issued). So the guarantee the header
 * states above -- every aligned guest doubleword is read or written whole, never torn -- is the one
 * kept; no ordering is added or removed either (the doubleword loop was relaxed and unordered too;
 * the order the SPU's commands take effect in comes from the fences in mfc_submit, not from here).
 * The guest side is accessed through a volatile pointer, so the compiler can neither split a 16-byte
 * access, merge two, nor turn the loop into a call to memcpy (whose own load granularity is not ours
 * to choose). Under ThreadSanitizer the doubleword atomics are what it understands, so the scalar
 * loops are selected there. The head (up to the first doubleword boundary) and the tail (bytes after
 * the last whole doubleword) are the original byte loops; bytes between a doubleword and a 16-byte
 * boundary move as one relaxed doubleword, exactly as before. */
#ifndef PS3_GM_WIDE_COPY
#  if defined(__SANITIZE_THREAD__)
#    define PS3_GM_WIDE_COPY 0
#  elif defined(__has_feature)
#    if __has_feature(thread_sanitizer)
#      define PS3_GM_WIDE_COPY 0
#    endif
#  endif
#endif
/* Default: ON only in the Switch build (__SWITCH__, aarch64). The PC builds (Windows, Linux, macOS)
 * keep upstream's one-doubleword-per-step loops and compare; -DPS3_GM_WIDE_COPY=1 turns the wide
 * forms on there too (x86-64 and AArch64 only). */
#ifndef PS3_GM_WIDE_COPY
#  if defined(__SWITCH__) && (defined(__x86_64__) || defined(__aarch64__))
#    define PS3_GM_WIDE_COPY 1
#  else
#    define PS3_GM_WIDE_COPY 0
#  endif
#endif

#if PS3_GM_WIDE_COPY
/* Two doublewords. may_alias: it is only ever used to read or write guest memory. */
typedef uint64_t gm_v128 __attribute__((vector_size(16), aligned(16), may_alias));
static inline gm_v128 gm_ld128(const uint8_t* p) { return *(const volatile gm_v128*)(const void*)p; }
static inline void gm_st128(uint8_t* p, gm_v128 v) { *(volatile gm_v128*)(void*)p = v; }
#endif

/* guest -> host, one aligned doubleword per step */
static inline void gm_copy_from_scalar(void* dst, const uint8_t* g, size_t n)
{
    uint8_t* d = (uint8_t*)dst;
    size_t i = 0;
    for (; i < n && (((uintptr_t)(g + i)) & 7u); i++) d[i] = gm_load8(g + i);
    for (; i + 8 <= n; i += 8) { uint64_t v = gm_load64(g + i); memcpy(d + i, &v, 8); }
    for (; i < n; i++) d[i] = gm_load8(g + i);
}

/* host -> guest, one aligned doubleword per step */
static inline void gm_copy_to_scalar(uint8_t* g, const void* src, size_t n)
{
    const uint8_t* s = (const uint8_t*)src;
    size_t i = 0;
    for (; i < n && (((uintptr_t)(g + i)) & 7u); i++) gm_store8(g + i, s[i]);
    for (; i + 8 <= n; i += 8) { uint64_t v; memcpy(&v, s + i, 8); gm_store64(g + i, v); }
    for (; i < n; i++) gm_store8(g + i, s[i]);
}

#if PS3_GM_WIDE_COPY
/* guest -> host */
static inline void gm_copy_from(void* dst, const uint8_t* g, size_t n)
{
    uint8_t* d = (uint8_t*)dst;
    size_t i = 0;
    /* one aligned quadword: the commonest small DMA (a job header, a descriptor) */
    if (n == 16 && !(((uintptr_t)g) & 15u)) { const gm_v128 a = gm_ld128(g); memcpy(d, &a, 16); return; }
    for (; i < n && (((uintptr_t)(g + i)) & 7u); i++) d[i] = gm_load8(g + i);
    if (n - i >= 16) {
        if (((uintptr_t)(g + i)) & 8u) { uint64_t v = gm_load64(g + i); memcpy(d + i, &v, 8); i += 8; }
        /* g + i is 16-byte aligned from here */
        for (; i + 64 <= n; i += 64) {
            /* hz-0.46: the source lines miss the cache (run 52: 80% of gm_copy_from on the first loads) */
            if (i + 256 < n) __builtin_prefetch(g + i + 256, 0, 0);
            const gm_v128 a = gm_ld128(g + i), b = gm_ld128(g + i + 16);
            const gm_v128 c = gm_ld128(g + i + 32), e = gm_ld128(g + i + 48);
            memcpy(d + i, &a, 16); memcpy(d + i + 16, &b, 16);
            memcpy(d + i + 32, &c, 16); memcpy(d + i + 48, &e, 16);
        }
        for (; i + 16 <= n; i += 16) { const gm_v128 a = gm_ld128(g + i); memcpy(d + i, &a, 16); }
    }
    for (; i + 8 <= n; i += 8) { uint64_t v = gm_load64(g + i); memcpy(d + i, &v, 8); }
    for (; i < n; i++) d[i] = gm_load8(g + i);
}

/* host -> guest */
static inline void gm_copy_to(uint8_t* g, const void* src, size_t n)
{
    const uint8_t* s = (const uint8_t*)src;
    size_t i = 0;
    if (n == 16 && !(((uintptr_t)g) & 15u)) { gm_v128 a; memcpy(&a, s, 16); gm_st128(g, a); return; }
    for (; i < n && (((uintptr_t)(g + i)) & 7u); i++) gm_store8(g + i, s[i]);
    if (n - i >= 16) {
        if (((uintptr_t)(g + i)) & 8u) { uint64_t v; memcpy(&v, s + i, 8); gm_store64(g + i, v); i += 8; }
        for (; i + 64 <= n; i += 64) {
            gm_v128 a, b, c, e;
            memcpy(&a, s + i, 16); memcpy(&b, s + i + 16, 16); memcpy(&c, s + i + 32, 16); memcpy(&e, s + i + 48, 16);
            gm_st128(g + i, a); gm_st128(g + i + 16, b); gm_st128(g + i + 32, c); gm_st128(g + i + 48, e);
        }
        for (; i + 16 <= n; i += 16) { gm_v128 a; memcpy(&a, s + i, 16); gm_st128(g + i, a); }
    }
    for (; i + 8 <= n; i += 8) { uint64_t v; memcpy(&v, s + i, 8); gm_store64(g + i, v); }
    for (; i < n; i++) gm_store8(g + i, s[i]);
}
#else
static inline void gm_copy_from(void* dst, const uint8_t* g, size_t n) { gm_copy_from_scalar(dst, g, n); }
static inline void gm_copy_to(uint8_t* g, const void* src, size_t n) { gm_copy_to_scalar(g, src, n); }
#endif

/* host -> guest, highest address first, each store a release (critic2,
 * 2026-11-10). On Cell an SPU's DMA lands in whole 128-byte lines: the RSX
 * never sees a line half written. gm_copy_to writes ascending doublewords, so
 * the FIFO walker -- which waits on a word (a self-JUMP park at the write head
 * an SPU appends to) and then reads UPWARD from it -- could see that word
 * already new and the words after it still old, and decode stale ring words
 * (the m16 Kuo-cutscene freezes). Written top-down with release stores, a
 * reader that sees any new word sees every word above it new (x86 TSO keeps
 * the store order, release keeps the compiler from reordering). */
static inline void gm_copy_to_desc(uint8_t* g, const void* src, size_t n)
{
    const uint8_t* s = (const uint8_t*)src;
    size_t i = n;
    for (; i > 0 && (((uintptr_t)(g + i)) & 7u); i--) __atomic_store_n(g + i - 1, s[i - 1], __ATOMIC_RELEASE);
    for (; i >= 8; i -= 8) {
        uint64_t v; memcpy(&v, s + i - 8, 8);
        __atomic_store_n((uint64_t*)(void*)(g + i - 8), v, __ATOMIC_RELEASE);
    }
    for (; i > 0; i--) __atomic_store_n(g + i - 1, s[i - 1], __ATOMIC_RELEASE);
}

/* A guest store of n bytes from a host buffer: one atomic access for the
 * natural sizes, a word-wise copy otherwise. */
static inline void gm_store_bytes(uint8_t* g, const void* src, size_t n)
{
    switch (n) {
    case 1: gm_store8(g, *(const uint8_t*)src); return;
    case 2: { uint16_t v; memcpy(&v, src, 2); gm_store16(g, v); return; }
    case 4: { uint32_t v; memcpy(&v, src, 4); gm_store32(g, v); return; }
    case 8: { uint64_t v; memcpy(&v, src, 8); gm_store64(g, v); return; }
    default: gm_copy_to(g, src, n); return;
    }
}

/* Compare n guest bytes against a host snapshot (a reservation line). */
static inline int gm_equal(const uint8_t* g, const void* snap, size_t n)
{
#if PS3_GM_WIDE_COPY
    /* The 16-byte head test of a fixed-lift overlay source (spu_channels.c), at any doubleword-aligned
     * address: two doubleword loads, no copy. */
    if (n == 16 && !(((uintptr_t)g) & 7u)) {
        uint64_t b0, b1;
        memcpy(&b0, snap, 8); memcpy(&b1, (const uint8_t*)snap + 8, 8);
        return ((gm_load64(g) ^ b0) | (gm_load64(g + 8) ^ b1)) == 0;
    }
    /* Under 16 bytes (a small GET's head test): byte compares that stop at the first difference, which
     * for data that is not the image is the first byte. Nothing here needs the copy to be whole
     * doublewords: it only reports whether the bytes were equal while it looked. */
    if (n < 16) {
        const uint8_t* s = (const uint8_t*)snap;
        for (size_t i = 0; i < n; i++) if (gm_load8(g + i) != s[i]) return 0;
        return 1;
    }
    /* The reservation line case (128 bytes at a 16-byte-aligned guest address, every PUTLLC and the
     * reservation poll): compare straight from guest memory, one 16-byte access at a time, instead of
     * copying through a bounce buffer first. Same accesses as gm_copy_from's body and the same verdict:
     * equal iff every byte is. (The caller's snapshot is host memory and need not be aligned.) */
    if (n && !(((uintptr_t)g) & 15u) && !(n & 15u)) {
        const uint8_t* s = (const uint8_t*)snap;
        gm_v128 acc = { 0, 0 };
        for (size_t i = 0; i < n; i += 16) {
            gm_v128 b;
            memcpy(&b, s + i, 16);
            acc |= gm_ld128(g + i) ^ b;
        }
        return (acc[0] | acc[1]) == 0;
    }
#endif
    uint8_t buf[256];
    while (n) {
        size_t k = n < sizeof buf ? n : sizeof buf;
        gm_copy_from(buf, g, k);
        if (memcmp(buf, snap, k)) return 0;
        g += k; snap = (const uint8_t*)snap + k; n -= k;
    }
    return 1;
}

#endif
