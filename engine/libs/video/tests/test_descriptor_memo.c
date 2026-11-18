#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "rsx_draw_engine.h"

#define VK_ENG_MAX_PIPE 4
#define VK_ENG_MAX_OBJ 32
#define VK_LOG(...) ((void)0)
#define VK_FD_BUMP(ch) ((void)(ch))
enum { VK_ENG_TEXTURE = 1 };
typedef struct { u32 cube_mask, vol_mask; } vk_eng_pipe;
static vk_eng_pipe s_epipe[VK_ENG_MAX_PIPE];
static struct { u32 kind, faces, depth, w, h; } s_eobj[VK_ENG_MAX_OBJ];
static struct {
    u32 pipeline, tex_mask, vs_bytes, ps_bytes, tex[RSX_MAX_TEXTURES];
    rsx_be_sampler_desc samp[RSX_MAX_TEXTURES];
} s_e2;
static struct { VkSampler sampler; struct { VkImageView view; } dummy; } s_vk;
static struct { int two, verify; } s_fdb;
static u32 s_eobj_gen = 1;
static int g_eng_bst_on, g_eng_bst_path;
static u64 g_eng_bst_t, g_eng_bset[2][4], g_eng_bsetn[2][3], g_eng_fd2[4];
static u64 vk_fd_tsc(void) { return 1; }
static int vk_postfx_aniso(void) { return 0; }
static VkImageView vk_eng_sample_view(u32 handle) { return (VkImageView)(uintptr_t)(100 + handle); }
static VkImageView vk_eng_dummy_view(int volume) { return (VkImageView)(uintptr_t)(200 + volume); }
static rsx_be_sampler_desc sampler_keys[64];
static u32 sampler_count;
static VkSampler vk_eng_sampler(const rsx_be_sampler_desc* desc)
{
    for (u32 i = 0; i < sampler_count; ++i)
        if (!memcmp(desc, &sampler_keys[i], sizeof *desc)) return (VkSampler)(uintptr_t)(1000 + i);
    assert(sampler_count < 64);
    sampler_keys[sampler_count] = *desc;
    return (VkSampler)(uintptr_t)(1000 + sampler_count++);
}
static struct { VkDescriptorImageInfo images[16], samplers[16]; } records[64];
static u32 record_count;
static VkDescriptorSet vk_batch_alloc_set(void)
{
    assert(record_count + 1 < 64);
    return (VkDescriptorSet)(uintptr_t)++record_count;
}
static void vk_eng_write_set(VkDescriptorSet set, VkBuffer ubuf,
                             const VkDescriptorImageInfo* images, const VkDescriptorImageInfo* samplers)
{
    assert(ubuf);
    memcpy(records[(uintptr_t)set].images, images, sizeof records[0].images);
    memcpy(records[(uintptr_t)set].samplers, samplers, sizeof records[0].samplers);
}
#include "memo_under_test.inc"

static void check_recent_hash(void)
{
    u32 state = 0x12345678;
    for (u32 mask = 0; mask < 65536; ++mask) {
        state = state * 1664525u + 1013904223u;
        s_e2.pipeline = state;
        s_e2.tex_mask = mask | (state & 0xffff0000u);
        for (u32 unit = 0; unit < RSX_MAX_TEXTURES; ++unit) {
            state = state * 1664525u + 1013904223u;
            s_e2.tex[unit] = state;
        }
        u64 expected = 0x9E3779B97F4A7C15ull ^ ((u64)s_e2.pipeline << 32) ^ s_e2.tex_mask;
        for (u32 unit = 0; unit < RSX_MAX_TEXTURES; ++unit)
            if ((s_e2.tex_mask >> unit) & 1u) expected = (expected ^ s_e2.tex[unit]) * 0x100000001B3ull;
        expected ^= expected >> 31;
        assert(test_recent_hash() == expected);
    }
    memset(&s_e2, 0, sizeof s_e2);
    puts("65536 recent-descriptor hash cases passed");
}

static VkDescriptorSet checked_draw(void)
{
    VkDescriptorSet set = vk_eng_batch_set((VkBuffer)(uintptr_t)1);
    VkDescriptorImageInfo images[16], samplers[16];
    vk_eng_image_infos(images, samplers);
    assert(!memcmp(records[(uintptr_t)set].images, images, sizeof images));
    assert(!memcmp(records[(uintptr_t)set].samplers, samplers, sizeof samplers));
    return set;
}

int main(int argc, char** argv)
{
    assert(argc == 5);
    s_fdb.two = atoi(argv[1]);
    const int volume = atoi(argv[2]);
    const u32 unit = (u32)atoi(argv[4]);
    assert(unit < RSX_MAX_TEXTURES);
    if (!s_fdb.two && volume == 0 && !atoi(argv[3]) && unit == 0) check_recent_hash();
    const u32 unit_mask = 1u << unit;
    s_vk.sampler = (VkSampler)(uintptr_t)10;
    s_vk.dummy.view = (VkImageView)(uintptr_t)20;
    s_e2.pipeline = 1; s_e2.vs_bytes = 16; s_e2.ps_bytes = 16;
    if (volume == 1) s_epipe[0].vol_mask = unit_mask;
    else if (volume == 0) s_epipe[0].cube_mask = unit_mask;
    s_eobj[1].kind = VK_ENG_TEXTURE;
    s_eobj[1].faces = volume == 0 ? 6 : 1;
    s_eobj[1].depth = volume == 1 ? 4 : 1;
    u32 textures[16] = {0};
    textures[unit] = 1;
    rsx_be_sampler_desc sampler[16] = {{0}};
    sampler[unit].min_linear = 1;
    vk_eng_bind_textures(NULL, textures, sampler, unit_mask);
    vk_eng_bind_textures(NULL, textures, sampler, 0);
    VkDescriptorSet first = checked_draw();
    assert(checked_draw() == first);
    sampler[unit].mag_linear = 1;
    vk_eng_bind_textures(NULL, textures, sampler, unit_mask);
    if (atoi(argv[3])) checked_draw();
    vk_eng_bind_textures(NULL, textures, sampler, 0);
    VkDescriptorSet returned = checked_draw();
    assert((returned == first) == (volume == 2));
    assert(checked_draw() == returned);
    /* A fenced slot can issue the same numeric handle again. Neither memo
     * may serve the preceding generation, even for identical draw state. */
    for (unsigned epoch = 0; epoch < 3; ++epoch) {
        ++s_dset_gen;
        record_count = 0;
        memset(records, 0xa5, sizeof records);
        if (epoch == 1) vk_eng_bind_textures(NULL, textures, sampler, unit_mask);
        assert(checked_draw() == first && record_count == 1);
        assert(checked_draw() == first && record_count == 1);
    }
    puts("descriptor memo placeholder and recycled handle transitions passed");
}
