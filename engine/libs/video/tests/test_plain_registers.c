#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../rsx_dispatch.c"
#include "plain_register_reference.inc"
static rsx_dispatch actual, expected;
static void check_short_header_masks(void)
{
    const u32 decorations[] = {0, 3, 0x80000000u, 0xfff00003u};
    const u32 words[] = {0x12345678, 0x89abcdef};
    unsigned cases = 0;
    memset(&actual, 0, sizeof actual);
    memset(&expected, 0, sizeof expected);
    s_burst_lean = 1;
    rsx_dispatch_burst(NULL, 0xffffffffu, 1, NULL, 0);
    rsx_dispatch_burst(&actual, 0xffffffffu, 1, NULL, 0);
    assert(!memcmp(&actual, &expected, sizeof actual));
    for (u32 method = 0; method < 0x2000u; method += 4u) {
        if (!s_plain[method >> 2]) continue;
        for (u32 variant = 0; variant < 4; ++variant) {
            for (int incr = 0; incr < 2; ++incr)
            for (u32 count = 1; count <= 2; ++count) {
                if (count == 2 && incr && (method + 4u >= 0x2000u || !s_plain[(method + 4u) >> 2])) continue;
                const u32 decorated = method | decorations[variant];
                actual.no_seen = expected.no_seen = variant & 1;
                g_rsx_dsp_tex_gen = g_rsx_dsp_misc_gen = UINT32_MAX;
                rsx_dispatch_burst(&actual, decorated, incr, words, count);
                const u32 texture_generation = g_rsx_dsp_tex_gen;
                const u32 misc_generation = g_rsx_dsp_misc_gen;
                g_rsx_dsp_tex_gen = g_rsx_dsp_misc_gen = UINT32_MAX;
                reference_rsx_dispatch_burst(&expected, decorated, incr, words, count);
                assert(texture_generation == g_rsx_dsp_tex_gen);
                assert(misc_generation == g_rsx_dsp_misc_gen);
                assert(!memcmp(actual.regs, expected.regs, sizeof actual.regs));
                assert(!memcmp(actual.seen, expected.seen, sizeof actual.seen));
                ++cases;
            }
        }
    }
    printf("PASS: %u masked/unaligned one/two-word headers, increment modes, seen modes and zero-count null arguments\n", cases);
}

static void check_pair_source_alias(void)
{
    const u32 methods[] = {0x0400, 0x0900, 0x093c, 0x1478, M_TEX_OFFSET, M_TEX_SIZE1};
    unsigned cases = 0;
    s_burst_lean = 1;
    for (u32 mi = 0; mi < sizeof methods / sizeof methods[0]; ++mi)
    for (u32 mode = 0; mode < 16; ++mode)
    for (int incr = 0; incr < 2; ++incr) {
        const u32 method = methods[mi], index = method >> 2;
        assert(s_plain[index] && s_plain[index + (incr ? 1u : 0u)]);
        memset(&actual, 0, sizeof actual); memset(&expected, 0, sizeof expected);
        actual.no_seen = expected.no_seen = mode & 1;
        u32 at = UINT32_MAX, am = UINT32_MAX, et = UINT32_MAX, em = UINT32_MAX;
        if (mode & 8) { actual.tgen = &at; actual.mgen = &am; expected.tgen = &et; expected.mgen = &em; }
        u64 ad[RSX_DSP_DIRTY_WORDS + 1] = {0}, ed[RSX_DSP_DIRTY_WORDS + 1] = {0};
        if (mode & 2) { actual.dirty = ad; expected.dirty = ed; }
        u32* aw = (mode & 4 ? actual.seen : actual.regs) + index - 1;
        u32* ew = (mode & 4 ? expected.seen : expected.regs) + index - 1;
        aw[0] = ew[0] = 0x12345678; aw[1] = ew[1] = 0x89abcdef;
        g_rsx_dsp_tex_gen = g_rsx_dsp_misc_gen = UINT32_MAX;
        rsx_dispatch_burst(&actual, method, incr, aw, 2);
        const u32 texture_generation = g_rsx_dsp_tex_gen, misc_generation = g_rsx_dsp_misc_gen;
        g_rsx_dsp_tex_gen = g_rsx_dsp_misc_gen = UINT32_MAX;
        reference_rsx_dispatch_burst(&expected, method, incr, ew, 2);
        assert(texture_generation == g_rsx_dsp_tex_gen && misc_generation == g_rsx_dsp_misc_gen);
        assert(at == et && am == em);
        assert(!memcmp(actual.regs, expected.regs, sizeof actual.regs));
        assert(!memcmp(actual.seen, expected.seen, sizeof actual.seen));
        assert(!memcmp(ad, ed, sizeof ad));
        ++cases;
    }
    printf("PASS: %u pair source/register/seen alias cases, sequential reads, dirty block crossing and generation modes\n", cases);
}

int main(void) {
    u32 warmup = 0;
    rsx_dispatch_burst(&actual, 0, 1, &warmup, 1);
    reference_plain_init();
    u8 original_plain[sizeof s_plain]; memcpy(original_plain, s_plain, sizeof s_plain);
    plain_init();
    for (u32 i = 0; i < sizeof s_plain; ++i) assert(!!s_plain[i] == !!original_plain[i]);
    check_short_header_masks();
    check_pair_source_alias();
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    u8* memory = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    assert(mprotect(memory + page, page, PROT_READ | PROT_WRITE) == 0);
    const u32 lengths[] = {1, 2, 3, 7, 16, 32, 64};
    unsigned cases = 0;
    for (u32 mode = 0; mode < 16; ++mode) {
        memset(&actual, 0, sizeof actual); memset(&expected, 0, sizeof expected);
        memset(actual.seen, 0xff, sizeof actual.seen);
        memset(expected.seen, 0xff, sizeof expected.seen);
        u32 at = UINT32_MAX, am = UINT32_MAX, et = UINT32_MAX, em = UINT32_MAX;
        if (mode & 8) { actual.tgen = &at; actual.mgen = &am; expected.tgen = &et; expected.mgen = &em; }
        actual.no_seen = expected.no_seen = mode & 1;
        s_burst_lean = !!(mode & 4);
        u64 ad[RSX_DSP_DIRTY_WORDS + 1] = {0}, ed[RSX_DSP_DIRTY_WORDS + 1] = {0};
        if (mode & 2) { actual.dirty = ad; expected.dirty = ed; }
        for (u32 method = 0; method < 0x2000; method += 4)
        for (u32 li = 0; li < sizeof lengths / sizeof lengths[0]; ++li)
        for (int incr = 0; incr < 2; ++incr) {
            const u32 n = lengths[li];
            if (method + (incr ? (n - 1) * 4 : 0) >= 0x2000) continue;
            int all_plain = 1;
            for (u32 i = 0; i < n; ++i) all_plain &= !!original_plain[(method >> 2) + (incr ? i : 0)];
            if (!all_plain) continue;
            u32* words = (u32*)(memory + page * 2 - n * sizeof(u32));
            for (u32 i = 0; i < n; ++i) words[i] = __builtin_bswap32(cases ^ (i * 0x123457u));
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                g_rsx_dsp_tex_gen = g_rsx_dsp_misc_gen = UINT32_MAX;
                rsx_dispatch_burst(&actual, method, incr, words, n);
                const u32 global_t = g_rsx_dsp_tex_gen, global_m = g_rsx_dsp_misc_gen;
                g_rsx_dsp_tex_gen = g_rsx_dsp_misc_gen = UINT32_MAX;
                reference_rsx_dispatch_burst(&expected, method, incr, words, n);
                assert(global_t == g_rsx_dsp_tex_gen && global_m == g_rsx_dsp_misc_gen);
                assert(at == et && am == em);
                assert(!memcmp(actual.regs, expected.regs, sizeof actual.regs));
                assert(!memcmp(actual.seen, expected.seen, sizeof actual.seen));
                assert(!memcmp(ad, ed, sizeof ad));
            }
            ++cases;
        }
        const u32 bases[] = {0, 504, UINT32_MAX};
        for (unsigned base = 0; base < sizeof bases / sizeof bases[0]; ++base) {
            u32* words = (u32*)(memory + page * 2 - 33 * sizeof(u32));
            words[0] = __builtin_bswap32(bases[base]);
            for (u32 i = 1; i < 33; ++i) words[i] = __builtin_bswap32(0x12345678u ^ i);
            rsx_dispatch_burst(&actual, M_VP_UPLOAD_CONST_ID, 1, words, 33);
            reference_rsx_dispatch_burst(&expected, M_VP_UPLOAD_CONST_ID, 1, words, 33);
            assert(!memcmp(actual.constants, expected.constants, sizeof actual.constants));
            assert(!memcmp(actual.regs, expected.regs, sizeof actual.regs));
            assert(!memcmp(actual.seen, expected.seen, sizeof actual.seen));
            assert(!memcmp(ad, ed, sizeof ad));
            assert(actual.const_gen == expected.const_gen);
        }
    }
    assert(munmap(memory, page * 3) == 0);
    printf("PASS: %u plain-header cases; class coverage, both loops, global/private generations, repeated values, seen/dirty, protected ends, mixed base/constant headers\n", cases);
}
