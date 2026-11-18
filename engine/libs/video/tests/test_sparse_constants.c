#include "rsx_vp_decompiler.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put_le(u8* p, u32 x)
{ for (u32 i = 0; i < 4; i++) p[i] = (u8)(x >> (8 * i)); }

static void instruction(u8* p, u32 row, int relative, int end)
{
    memset(p, 0, 16);
    put_le(p + 4, row << 12);
    put_le(p + 8, 3u << 23); /* constant source 0 */
    put_le(p + 12, (relative ? 2u : 0u) | (end ? 1u : 0u));
}

int main(void)
{
    u8 code[5 * 16];
    const u32 rows[] = {256, 257, 258, 259, 467};
    for (u32 i = 0; i < 5; i++) instruction(code + i * 16, rows[i], 0, i == 4);
    rsx_vp_const_usage usage;
    assert(rsx_vp_analyze_constants(code, sizeof code, &usage) == 5);
    assert(usage.valid && !usage.relative && usage.lo == 256 && usage.hi == 467);
#ifdef __SWITCH__
    assert(usage.sparse.count == 2 && usage.sparse.rows == 5);
    assert(usage.sparse.spans[0].first == 256 && usage.sparse.spans[0].count == 4);
    assert(usage.sparse.spans[1].first == 467 && usage.sparse.spans[1].count == 1);
    assert(rsx_constant_plan_valid(&usage.sparse, 256, 212));
    rsx_constant_plan invalid = usage.sparse;
    invalid.spans[1].first = 258;
    assert(!rsx_constant_plan_valid(&invalid, 256, 212));
    invalid = usage.sparse; invalid.rows++;
    assert(!rsx_constant_plan_valid(&invalid, 256, 212));
    invalid = usage.sparse; invalid.count = RSX_CONSTANT_MAX_SPANS + 1;
    assert(!rsx_constant_plan_valid(&invalid, 256, 212));
    assert(!rsx_constant_plan_valid(&usage.sparse, 512, 1));
    instruction(code + 4 * 16, 467, 1, 1);
    assert(rsx_vp_analyze_constants(code, sizeof code, &usage) == 5);
    assert(usage.relative && !usage.sparse.count);
    instruction(code + 4 * 16, 467, 0, 0);
    assert(rsx_vp_analyze_constants(code, sizeof code, &usage) < 0);
    assert(!usage.valid && !usage.sparse.count);
    instruction(code, 0, 0, 0); instruction(code + 16, 511, 0, 1);
    assert(rsx_vp_analyze_constants(code, 32, &usage) == 2);
    assert(usage.sparse.count == 2 && usage.sparse.rows == 2);
    instruction(code, 511, 0, 1);
    assert(rsx_vp_analyze_constants(code, 16, &usage) == 1);
    assert(!usage.sparse.count); /* contiguous fallback */
    assert(rsx_vp_analyze_constants(NULL, 16, &usage) < 0 && !usage.sparse.count);
    assert(rsx_vp_analyze_constants(code, 15, &usage) < 0 && !usage.sparse.count);
    assert(rsx_vp_analyze_constants(code, 16, NULL) < 0);
    memset(code, 0, sizeof code); put_le(code + 12, 1);
    assert(rsx_vp_analyze_constants(code, 16, &usage) == 1);
    assert(usage.lo > usage.hi && !usage.sparse.count);
    u64 bits[8] = {0}; rsx_constant_plan plan;
    bits[0] = 1ull | (1ull << 16);
    rsx_constant_plan_build(&plan, bits, 0, 17);
    assert(!plan.count); /* 240 B saved: below threshold */
    bits[0] = 1ull | (1ull << 17);
    rsx_constant_plan_build(&plan, bits, 0, 18);
    assert(plan.count == 2 && plan.rows == 2); /* exactly 256 B saved */
    for (u32 i = 0; i < 9; i++) bits[(i * 50) / 64] |= 1ull << ((i * 50) % 64);
    rsx_constant_plan_build(&plan, bits, 0, 401);
    assert(!plan.count); /* bounded metadata, dense fallback */
    instruction(code, 256, 0, 0); instruction(code + 16, 256, 0, 0);
    instruction(code + 32, 467, 0, 1);
    assert(rsx_vp_analyze_constants(code, 48, &usage) == 3);
    assert(usage.sparse.count == 2 && usage.sparse.rows == 2);
#endif
    puts("sparse constant analysis checks passed");
    return 0;
}
