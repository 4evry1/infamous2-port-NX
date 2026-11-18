#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../rsx_dispatch.c"
#include "constant_burst_reference.inc"
#include "plain_register_reference.inc"
static rsx_dispatch actual, expected;
static void set_routing_mode(u32 mode)
{
    s_ed_on = mode == 1;
    s_clog_lo = mode == 2 ? 999 : mode == 3 ? -2 : -1;
    s_clog_hi = 999;
    s_plain_ready = mode != 4;
    s_burst_lean = mode != 5;
}

static void check_constant_header_routing(void)
{
    const u32 methods[] = {M_VP_UPLOAD_CONST_ID, M_VP_UPLOAD_CONST,
        M_VP_UPLOAD_CONST + 4, M_VP_UPLOAD_CONST + 31 * 4, M_VP_UPLOAD_CONST + 32 * 4};
    const u32 counts[] = {0, 1, 2, 3, 31, 32, 33, 34};
    u32 words[34];
    for (u32 i = 0; i < 34; ++i) words[i] = __builtin_bswap32(0x123000u + i);
    reference_plain_init();
    plain_init();
    unsetenv("RSX_CONST_LOG");
    unsetenv("RSX_EMPTY_DRAW_LOG");
    unsigned cases = 0;
    for (u32 mi = 0; mi < sizeof methods / sizeof methods[0]; ++mi)
    for (u32 ci = 0; ci < sizeof counts / sizeof counts[0]; ++ci)
    for (u32 mode = 0; mode < 8; ++mode)
    for (int incr = 0; incr < 2; ++incr) {
        memset(&actual, 0, sizeof actual); memset(&expected, 0, sizeof expected);
        actual.no_seen = expected.no_seen = mode & 1;
        actual.regs[M_VP_UPLOAD_CONST_ID >> 2] = expected.regs[M_VP_UPLOAD_CONST_ID >> 2] = 3;
        u64 ad[RSX_DSP_DIRTY_WORDS + 1] = {0}, ed[RSX_DSP_DIRTY_WORDS + 1] = {0};
        if (mode & 2) { actual.dirty = ad; expected.dirty = ed; }
        set_routing_mode(mode);
        rsx_dispatch_burst(&actual, methods[mi] | 0xfff00003u, incr, counts[ci] ? words : NULL, counts[ci]);
        set_routing_mode(mode);
        reference_rsx_dispatch_burst(&expected, methods[mi] | 0xfff00003u, incr, counts[ci] ? words : NULL, counts[ci]);
        assert(!memcmp(actual.constants, expected.constants, sizeof actual.constants));
        assert(!memcmp(actual.regs, expected.regs, sizeof actual.regs));
        assert(!memcmp(actual.seen, expected.seen, sizeof actual.seen));
        assert(!memcmp(ad, ed, sizeof ad));
        assert(actual.const_gen == expected.const_gen);
        ++cases;
    }
    printf("PASS: %u constant-header routing cases; diagnostics/init/lean modes, nonincrementing and crossing windows, method masks, zero-count NULL source\n", cases);
}

int main(void) {
    const u32 bases[] = {0, 1, 504, 505, 510, 511, 512, 513, UINT32_MAX-7, UINT32_MAX-1, UINT32_MAX};
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    u8* memory = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    assert(mprotect(memory + page, page, PROT_READ | PROT_WRITE) == 0);
    u32 warmup = 0;
    rsx_dispatch_burst(&actual, 0, 1, &warmup, 1);
    unsigned cases = 0;
    for (unsigned bi = 0; bi < sizeof bases / sizeof bases[0]; ++bi)
    for (u32 first = 0; first < 32; ++first)
    for (u32 count = 0; count <= 32 - first; ++count)
    for (u32 mode = 0; mode < 4; ++mode) {
        u32* words = (u32*)(memory + page * 2 - count * 4u);
        for (u32 i = 0; i < count; ++i) words[i] = __builtin_bswap32(0x80000000u ^ (i * 0x123457u + cases));
        memset(actual.constants, 0, sizeof actual.constants);
        memset(expected.constants, 0, sizeof expected.constants);
        memset(actual.regs, 0, sizeof actual.regs);
        memset(expected.regs, 0, sizeof expected.regs);
        memset(actual.seen, 0xff, sizeof actual.seen);
        memset(expected.seen, 0xff, sizeof expected.seen);
        actual.regs[M_VP_UPLOAD_CONST_ID >> 2] = expected.regs[M_VP_UPLOAD_CONST_ID >> 2] = bases[bi];
        actual.no_seen = expected.no_seen = mode & 1;
        actual.const_gen = expected.const_gen = UINT32_MAX;
        u64 actual_dirty[RSX_DSP_DIRTY_WORDS + 1] = {0}, expected_dirty[RSX_DSP_DIRTY_WORDS + 1] = {0};
        actual.dirty = mode & 2 ? actual_dirty : NULL;
        expected.dirty = mode & 2 ? expected_dirty : NULL;
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            if ((cases + repeat) & 1u)
                rsx_dispatch_burst(&actual, M_VP_UPLOAD_CONST + first * 4u, 1, words, count);
            else
                burst_const_run(&actual, M_VP_UPLOAD_CONST + first * 4u, words, count);
            reference_const_run(&expected, M_VP_UPLOAD_CONST + first * 4u, words, count);
            /* The frozen helper omitted dirty bits after an unsigned slot wrap.
             * Keep its store/generation oracle, correcting only that known omission. */
            if (expected.dirty) for (u32 k = 0; k < count; ++k) {
                const u32 slot = bases[bi] + ((first + k) >> 2);
                if (slot < RSX_DSP_NUM_CONSTANTS) {
                    const u32 block = RSX_DSP_BLK_CONST0 + slot / 4u;
                    expected_dirty[block / 64u] |= UINT64_C(1) << (block % 64u);
                    expected_dirty[RSX_DSP_DIRTY_WORDS] |= UINT64_C(1) << (block / 64u);
                }
            }
            assert(!memcmp(actual.constants, expected.constants, sizeof actual.constants));
            assert(!memcmp(actual.regs, expected.regs, sizeof actual.regs));
            assert(!memcmp(actual.seen, expected.seen, sizeof actual.seen));
            assert(!memcmp(actual_dirty, expected_dirty, sizeof actual_dirty));
            assert(actual.const_gen == expected.const_gen);
        }
        ++cases;
    }
    check_constant_header_routing();
    assert(munmap(memory, page * 3) == 0);
    printf("PASS: %u constant burst cases, repeated unchanged uploads, slot/word edges, wrapping bases, dirty/seen modes, generations, protected source ends\n", cases);
}
