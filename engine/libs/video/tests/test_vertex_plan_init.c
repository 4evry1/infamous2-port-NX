#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {
    VERTEX_BUFFER_METHOD = 0x1680,
    VERTEX_FORMAT_METHOD = 0x1740,
    VERTEX_DEFAULT_METHOD = 0x1c00,
    VERTEX_DIVIDER_METHOD = 0x1fc0
};

static u32 random_state = 0x124875ab;
static u32 next_word(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}
static const u8* unused_resolver(void* user, u32 location, u32 offset, u32 bytes) {
    (void)user; (void)location; (void)offset; (void)bytes;
    assert(!"plan initialization must not resolve guest memory");
    return NULL;
}
int main(void) {
    static rsx_dispatch rsx;
    rsx_vertex_fetch_plan plan, before;
    const float written_default[4] = {2.0f, 3.0f, 4.0f, 5.0f};
    for (u32 mask = 0; mask < 65536; ++mask) {
        rsx_vertex_layout_plan layout;
        rsx_vertex_layout_plan_init(&layout, mask);
        for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; ++a) {
            rsx.regs[(VERTEX_FORMAT_METHOD + a * 4) >> 2] = next_word();
            rsx.regs[(VERTEX_BUFFER_METHOD + a * 4) >> 2] = next_word();
            if ((mask + a) & 1)
                memcpy(&rsx.regs[(VERTEX_DEFAULT_METHOD + a * 16) >> 2], written_default, 16);
            else memset(&rsx.regs[(VERTEX_DEFAULT_METHOD + a * 16) >> 2], 0, 16);
        }
        rsx.regs[VERTEX_DIVIDER_METHOD >> 2] = next_word();
        memset(&plan, (int)(0xa5 ^ (mask & 0xff)), sizeof plan);
        before = plan;
        rsx_vertex_fetch_plan_init_layout(&plan, &rsx, &layout, unused_resolver, &rsx);
        assert(!memcmp(&plan.layout, &layout, sizeof layout));
        assert(plan.base_offset == rsx_dsp_vertex_data_base_offset(&rsx));
        assert(plan.divider_mask == rsx.regs[VERTEX_DIVIDER_METHOD >> 2]);
        assert(plan.guest_ptr == unused_resolver && plan.guest_user == &rsx);
        assert(!plan.inline_data && !plan.inline_bytes && !plan.inline_stride);
        for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; ++a) {
            assert(!plan.inline_off[a]);
            const rsx_vertex_fetch_attr* f = &plan.attr[a];
            if (!(mask & (1u << a))) {
                assert(!memcmp(f, &before.attr[a], sizeof *f));
                continue;
            }
            rsx_dsp_vertex_attr desc;
            rsx_dsp_get_vertex_attr(&rsx, a, &desc);
            assert(!memcmp(&f->desc, &desc, sizeof desc));
            const u32 size = rsx_vertex_attrib_size(desc.type, desc.size);
            assert(f->elem_size == size && f->stride == (desc.stride ? desc.stride : size));
            assert(!f->stable_base && !f->stable_first && !f->stable_last);
            const float unwritten_default[4] = {a == 3 ? 1.0f : 0.0f,
                a == 3 ? 1.0f : 0.0f, a == 3 ? 1.0f : 0.0f, 1.0f};
            assert(!memcmp(f->default_value, ((mask + a) & 1) ? written_default : unwritten_default, 16));
        }
    }
    before = plan;
    rsx_vertex_fetch_plan_init_layout(&plan, NULL, &plan.layout, unused_resolver, &rsx);
    assert(!memcmp(&plan, &before, sizeof plan));
    rsx_vertex_fetch_plan_init_layout(&plan, &rsx, NULL, unused_resolver, &rsx);
    assert(!memcmp(&plan, &before, sizeof plan));
    rsx_vertex_fetch_plan_init_layout(NULL, &rsx, &plan.layout, unused_resolver, &rsx);
    puts("PASS:65536 poisoned-plan masks; active fields reset, inactive attributes retained, inline reset, null guards");
}
