#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
typedef uint32_t u32;
static struct { VkCommandBuffer cmd; } s_vk;
static struct { VkPipelineLayout pipe_layout; } s_g;
static u32 s_dset_gen;
static struct { VkPipelineLayout pl; } s_ropt;
static VkDescriptorSet recorded_input;
static unsigned calls, last_first, last_count;
static VkCommandBuffer recorded_cmd;
static VkPipelineLayout recorded_layout;
static VkDescriptorSet recorded_set;
static u32 recorded_offsets[2];
static void record_bind(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout,
                       u32 first, u32 count, const VkDescriptorSet* sets, u32 offsets, const u32* dyn) {
    assert(point == VK_PIPELINE_BIND_POINT_GRAPHICS && (first == 0 || first == 1));
    assert(count && first + count <= 2 && offsets == (first ? 0u : 2u));
    last_first = first; last_count = count;
    ++calls; recorded_cmd = cmd; recorded_layout = layout;
    if (!first) {
        recorded_set = sets[0];
        memcpy(recorded_offsets, dyn, sizeof recorded_offsets);
    }
    if (first + count == 2) recorded_input = sets[count - 1];
}
static PFN_vkCmdBindDescriptorSets pvkCmdBindDescriptorSets = record_bind;
#define VK_LOG(...) ((void)0)
#include "descriptor_under_test.h"
#define HANDLE(type, value) ((type)(uintptr_t)(value))

int main(void) {
    setenv("PS3RECOMP_VK_DESCRIPTOR_STATS", "1", 1);
    s_vk.cmd = HANDLE(VkCommandBuffer, 1);
    s_g.pipe_layout = HANDLE(VkPipelineLayout, 2);
    VkDescriptorSet set = HANDLE(VkDescriptorSet, 3);
    u32 offsets[2] = {256, 512};
    vk_eng_bind_descriptor_set(0, 1, set, offsets, VK_NULL_HANDLE); assert(calls == 1);
    vk_eng_bind_descriptor_set(1, 1, set, offsets, VK_NULL_HANDLE); assert(calls == 1);
    vk_eng_bind_descriptor_set(1, 0, set, offsets, VK_NULL_HANDLE); assert(calls == 2);
    vk_eng_bind_descriptor_set(0, 1, set, offsets, VK_NULL_HANDLE); assert(calls == 3);
    for (unsigned change = 0; change < 7; ++change) {
        switch (change) {
        case 0: set = HANDLE(VkDescriptorSet, 4); break;
        case 1: offsets[0] += 256; break;
        case 2: offsets[1] += 256; break;
        case 3: ++s_dset_gen; break;
        case 4: s_g.pipe_layout = HANDLE(VkPipelineLayout, 5); break;
        case 5: s_vk.cmd = HANDLE(VkCommandBuffer, 6); break;
        case 6: s_ecs.descriptor_valid = 0; break;
        }
        unsigned before = calls;
        vk_eng_bind_descriptor_set(1, 1, set, offsets, VK_NULL_HANDLE); assert(calls == before + 1);
        vk_eng_bind_descriptor_set(1, 1, set, offsets, VK_NULL_HANDLE); assert(calls == before + 1);
    }
    uint32_t random = 0x13579bdf;
    for (unsigned i = 0; i < 100000; ++i) {
        random = random * 1664525u + 1013904223u;
        unsigned mode = random >> 28;
        int known = mode != 0, reuse = mode != 1;
        if (mode == 2) s_ecs.descriptor_valid = 0;
        if (mode == 3) ++s_dset_gen;
        if (mode == 4) offsets[0] ^= 256;
        if (mode == 5) offsets[1] ^= 512;
        if (mode == 6) set = HANDLE(VkDescriptorSet, 7 + (i & 1));
        if (mode == 7) s_vk.cmd = HANDLE(VkCommandBuffer, 9 + (i & 1));
        if (mode == 8) s_g.pipe_layout = HANDLE(VkPipelineLayout, 11 + (i & 1));
        if (!known || mode == 2 || mode == 3) recorded_set = VK_NULL_HANDLE;
        vk_eng_bind_descriptor_set(known, reuse, set, offsets, VK_NULL_HANDLE);
        assert(recorded_cmd == s_vk.cmd && recorded_layout == s_g.pipe_layout && recorded_set == set);
        assert(memcmp(recorded_offsets, offsets, sizeof offsets) == 0);
    }
    __typeof__(s_ecs) before_dry = s_ecs;
    offsets[0] += 256;
    vk_eng_bind_descriptor_set(1, 1, set, offsets, VK_NULL_HANDLE);
    __typeof__(s_ecs) after_dry = s_ecs;
    s_ecs = before_dry;
    vk_eng_bind_descriptor_set(1, 1, set, offsets, VK_NULL_HANDLE);
    assert(memcmp(&s_ecs, &after_dry, sizeof s_ecs) == 0);
    s_ropt.pl = HANDLE(VkPipelineLayout, 90);
    VkDescriptorSet input = HANDLE(VkDescriptorSet, 70);
    unsigned before = calls;
    vk_eng_bind_descriptor_set(0, 1, set, offsets, input);
    assert(calls == ++before && recorded_layout == s_ropt.pl && recorded_input == input && recorded_set == set);
    assert(last_first == 0 && last_count == 2);
    vk_eng_bind_descriptor_set(1, 1, set, offsets, input);
    assert(calls == before);
    ++offsets[1];
    vk_eng_bind_descriptor_set(1, 1, set, offsets, input);
    assert(calls == ++before && recorded_offsets[1] == offsets[1] && recorded_input == input);
    assert(last_first == 0 && last_count == 1);
    input = HANDLE(VkDescriptorSet, 71);
    vk_eng_bind_descriptor_set(1, 1, set, offsets, input);
    assert(calls == ++before && recorded_input == input);
    assert(last_first == 1 && last_count == 1);
    vk_eng_bind_descriptor_set(1, 1, set, offsets, VK_NULL_HANDLE);
    assert(calls == ++before && recorded_layout == s_g.pipe_layout);
    vk_eng_bind_descriptor_set(1, 1, set, offsets, input);
    assert(calls == ++before && recorded_layout == s_ropt.pl);
    ++s_dset_gen;
    vk_eng_bind_descriptor_set(1, 1, set, offsets, input);
    assert(calls == ++before);
    /* Disturb the recording adapter just as a fresh command buffer or new
     * pool generation would. Every following draw must establish both sets. */
    for (unsigned i = 0; i < 30000; ++i) {
        random = random * 1664525u + 1013904223u;
        unsigned mode = random >> 28;
        if (mode == 2) set = HANDLE(VkDescriptorSet, 32 + (i & 1));
        if (mode == 3) input = HANDLE(VkDescriptorSet, 72 + (i & 1));
        if (mode == 4) offsets[1] ^= 256;
        if (mode == 5) offsets[0] ^= 512;
        if (mode == 6) { ++s_dset_gen; recorded_set = recorded_input = VK_NULL_HANDLE; }
        if (mode == 7) { s_ecs.descriptor_valid = 0; recorded_set = recorded_input = VK_NULL_HANDLE; }
        int attachment = mode != 8;
        if (recorded_layout != (attachment ? s_ropt.pl : s_g.pipe_layout))
            recorded_set = recorded_input = VK_NULL_HANDLE;
        before = calls;
        vk_eng_bind_descriptor_set(mode != 0, mode != 1, set, offsets, attachment ? input : VK_NULL_HANDLE);
        assert(calls - before <= 1);
        assert(recorded_set == set && (!attachment || recorded_input == input));
        assert(!memcmp(recorded_offsets, offsets, sizeof offsets));
    }
    assert(s_ecs.descriptor_skips > 1000 && s_ecs.descriptor_combined && s_ecs.descriptor_new_offsets);
    assert(s_ecs.descriptor_new_set && s_ecs.descriptor_input_only && s_ecs.descriptor_unknown);
    assert(s_ecs.descriptor_attempts == s_ecs.descriptor_unknown + s_ecs.descriptor_new_set +
           s_ecs.descriptor_new_offsets + s_ecs.descriptor_input_only + s_ecs.descriptor_skips);
    puts("PASS: exact repeats, invalidation, disabled mode, handle/epoch/offset changes, 130000 replayed draw states");
}
