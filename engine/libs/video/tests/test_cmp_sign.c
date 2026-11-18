#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"
#include <assert.h>
#include <fenv.h>
#include <stdio.h>
#include <string.h>

#define BATCH 2048u
static u32 state;
static u32 next(void) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
static void check_batch(const u8* source, const rsx_vertex_ref* refs) {
    rsx_vertex_fetch_plan plan = {0};
    rsx_vertex_layout_plan_init(&plan.layout, 1);
    rsx_vertex_fetch_attr* f = &plan.attr[0];
    f->desc.type = RSX_VTX_TYPE_CMP32; f->desc.size = 3;
    f->elem_size = 4; f->stride = 4;
    f->stable_base = source; f->stable_last = BATCH - 1;
    u8 expected[BATCH * 16], actual[BATCH * 16];
    for (u32 i = 0; i < BATCH; ++i) {
        float value[4];
        assert(rsx_vertex_decode_element(RSX_VTX_TYPE_CMP32, 3, source + i * 4, value));
        memcpy(expected + i * 16, value, 16);
    }
    for (int consecutive = 0; consecutive <= 1; ++consecutive) {
        assert(rsx_vertex_fetch_all_ex(&plan, refs, BATCH, actual, consecutive));
        assert(!memcmp(expected, actual, sizeof actual));
    }
}
int main(void) {
    const int rounding[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    const int saved = fegetround();
    u8 bytes[BATCH * 4 + 1]; rsx_vertex_ref refs[BATCH];
    for (u32 i = 0; i < BATCH; ++i) refs[i] = (rsx_vertex_ref){i, 0};
    unsigned long long words = 0;
    for (u32 mode = 0; mode < 4; ++mode) {
        assert(!fesetround(rounding[mode])); state = 0xabcd1234;
        for (u32 batch = 0; batch < 518; ++batch) {
            for (u32 i = 0; i < BATCH; ++i) {
                u32 word = next();
                if (batch < 6) {
                    const u32 field = batch / 2, shift = field * 11;
                    const u32 mask = field == 2 ? 1023u : 2047u;
                    word = batch & 1 ? ~0u : 0u;
                    word = (word & ~(mask << shift)) | ((i & mask) << shift);
                }
                for (u32 k = 0; k < 4; ++k) bytes[1 + i * 4 + k] = (u8)(word >> (24 - k * 8));
            }
            check_batch(bytes + 1, refs); words += BATCH;
        }
    }
    assert(!fesetround(saved));
    printf("PASS:%llu CMP32 words in four rounding modes, generic/fast output equals scalar decoder\n", words);
}
