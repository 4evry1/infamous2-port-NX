#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <vulkan/vulkan.h>
#include "../rsx_shader_spirv.h"

#define VK_LOG(...) ((void)0)
#define VK_BATCH_SETS 8u
#define VK_RETAINED_SETS 4u
#define RSX_MAX_TEXTURES 16u
static struct { VkDevice device; } s_vk;
static struct { VkDescriptorSetLayout set_layout; } s_g;
static u32 s_fif_cur, allocs, resets, pools;
static int fail_alloc, fail_reset;
static VkResult pvkCreateDescriptorPool(VkDevice d, const VkDescriptorPoolCreateInfo *ci,
        const VkAllocationCallbacks *ac, VkDescriptorPool *pool) {
    (void)d; (void)ac; assert(ci->maxSets == VK_BATCH_SETS);
    *pool=(VkDescriptorPool)(uintptr_t)++pools; return VK_SUCCESS;
}
static VkResult pvkAllocateDescriptorSets(VkDevice d, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *set) {
    (void)d; assert(ai->descriptorPool && ai->descriptorSetCount==1 && *ai->pSetLayouts==s_g.set_layout);
    if (fail_alloc) return VK_ERROR_OUT_OF_POOL_MEMORY;
    *set=(VkDescriptorSet)(uintptr_t)++allocs; return VK_SUCCESS;
}
static VkResult pvkResetDescriptorPool(VkDevice d, VkDescriptorPool p, VkDescriptorPoolResetFlags f) {
    (void)d; assert(p && !f); ++resets; return fail_reset ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}
static struct { u32 vs_bytes, ps_bytes; } s_e2;
static VkDescriptorSet writing;
static VkBuffer expected_ubo;
static VkDescriptorImageInfo images[RSX_MAX_TEXTURES], samplers[RSX_MAX_TEXTURES];
static unsigned writes;
static void pvkUpdateDescriptorSets(VkDevice d, u32 count, const VkWriteDescriptorSet *w,
                                    u32 copies, const VkCopyDescriptorSet *copy) {
    (void)d; assert(count == 4 && !copies && !copy);
    unsigned seen = 0;
    for (u32 i=0; i<count; ++i) {
        assert(w[i].dstSet == writing && !w[i].dstArrayElement);
        switch (w[i].dstBinding) {
        case RSX_SPIRV_VPCONST_BINDING:
        case RSX_SPIRV_PSCONST_BINDING: {
            const int vertex = w[i].dstBinding == RSX_SPIRV_VPCONST_BINDING;
            seen |= vertex ? 1u : 2u;
            assert(w[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
            assert(w[i].descriptorCount == 1 && w[i].pBufferInfo[0].buffer == expected_ubo);
            assert(!w[i].pBufferInfo[0].offset &&
                   w[i].pBufferInfo[0].range == (vertex ? s_e2.vs_bytes : s_e2.ps_bytes));
            break;
        }
        case RSX_SPIRV_TEXTURE_BINDING:
            seen |= 4u;
            assert(w[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            assert(w[i].descriptorCount == RSX_MAX_TEXTURES && w[i].pImageInfo == images);
            break;
        case RSX_SPIRV_SAMPLER_BINDING:
            seen |= 8u;
            assert(w[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER);
            assert(w[i].descriptorCount == RSX_MAX_TEXTURES && w[i].pImageInfo == samplers);
            break;
        default: assert(0);
        }
    }
    assert(seen == 15u); ++writes;
}
#include "pool.inc"
int main(void) {
    s_g.set_layout=(VkDescriptorSetLayout)(uintptr_t)1;
    VkDescriptorSet orig[2][3];
    for (s_fif_cur=0; s_fif_cur<2; ++s_fif_cur)
        for (unsigned i=0;i<3;++i) orig[s_fif_cur][i]=vk_batch_alloc_set();
    assert(allocs==6 && pools==2);
    for (s_fif_cur=0; s_fif_cur<2; ++s_fif_cur) {
        vk_batch_complete_sets(&s_batch_sets[s_fif_cur]);
        for (unsigned i=0;i<3;++i) {
            VkDescriptorSet set=vk_batch_alloc_set();
            writing=set; expected_ubo=(VkBuffer)(uintptr_t)(100+i);
            s_e2.vs_bytes=16+i*16; s_e2.ps_bytes=32+s_fif_cur*16;
            vk_eng_write_set(set, expected_ubo, images, samplers);
#ifdef __SWITCH__
            assert(set==orig[s_fif_cur][i]);
#else
            assert(set!=orig[s_fif_cur][i]);
#endif
        }
    }
#ifdef __SWITCH__
    assert(allocs==6 && resets==0);
    assert(s_batch_sets[0].fresh==3 && s_batch_sets[1].fresh==3);
#else
    assert(allocs==12 && resets==2);
    assert(s_batch_sets[0].fresh==6 && s_batch_sets[1].fresh==6);
#endif
    assert(writes==6);
    s_fif_cur=0;
    fail_alloc=1; assert(!vk_batch_alloc_set() && s_batch_sets[0].used==3); fail_alloc=0;
    while (s_batch_sets[0].used < VK_BATCH_SETS) assert(vk_batch_alloc_set());
    assert(!vk_batch_alloc_set());
    unsigned reset0=resets;
    unsigned long long completed0=s_batch_sets[0].completed;
    fail_reset=1; vk_batch_complete_sets(&s_batch_sets[0]);
    assert(s_batch_sets[0].used==VK_BATCH_SETS && !vk_batch_alloc_set());
    assert(s_batch_sets[0].completed==completed0);
    fail_reset=0; vk_batch_complete_sets(&s_batch_sets[0]);
    assert(resets==reset0+2 && !s_batch_sets[0].used && s_batch_sets[1].used==3);
    assert(s_batch_sets[0].completed==completed0+VK_BATCH_SETS);
    assert(vk_batch_alloc_set()!=orig[0][0]);
    puts("pool ownership, completed-slot reuse, exhaustion and failure recovery passed");
}
