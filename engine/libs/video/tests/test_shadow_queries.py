"""Run the renderer's timestamp helper on an actual Vulkan queue, including slot reuse."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

video = Path(__file__).resolve().parents[1]
backend = (video / "rsx_vulkan_backend.c").read_text()
source = (video / "vk_shadow_queries.inc").read_text()
for name in ("vk_create_host_buffer", "vk_destroy_idle_buffer"):
    match = re.search(r"static (int|void) " + name + r"\([^;{}]+\)\n\{.*?\n\}", backend, re.S)
    assert match
    source = match[0] + "\n" + source
aliases = "\n".join("#define p" + entry + " " + entry for entry in sorted(set(re.findall(r"p(vk[A-Z]\w+)\(", source))))
setup = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
typedef uint32_t u32;
typedef struct { VkBuffer buf; VkDeviceMemory mem; void *ptr; VkDeviceSize size; int coherent; } vk_buffer;
static struct { VkDevice device; VkPhysicalDevice phys; VkCommandBuffer cmd; unsigned timestamp_bits; } s_vk;
static unsigned s_fif_cur;
#define VK_FIF_MAX 2
#define VK_CHECK(call, msg) do { if ((call) != VK_SUCCESS) return -1; } while (0)
#define VK_LOG(...) fprintf(stderr,__VA_ARGS__)
static uint64_t ps3_ft_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000000000ull+t.tv_nsec; }
static int vk_find_memory_type(u32 bits, VkMemoryPropertyFlags flags, u32 *out) {
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(s_vk.phys,&p);
    for (u32 i=0; i<p.memoryTypeCount; ++i)
        if ((bits & (1u<<i)) && (p.memoryTypes[i].propertyFlags & flags)==flags) { *out=i; return 0; }
    return -1;
}
"""
main = r"""
#define CHECK(call) assert((call)==VK_SUCCESS)
int main(void) {
    VkInstance instance;
    VkInstanceCreateInfo init = { .sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    CHECK(vkCreateInstance(&init,NULL,&instance));
    u32 count=1;
    VkResult enumerated=vkEnumeratePhysicalDevices(instance,&count,&s_vk.phys);
    assert((enumerated==VK_SUCCESS || enumerated==VK_INCOMPLETE) && count);
    VkQueueFamilyProperties families[32];
    count=32; vkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys,&count,families);
    u32 family=0;
    while(family<count && (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !families[family].timestampValidBits)) ++family;
    assert(family<count);
    s_vk.timestamp_bits=families[family].timestampValidBits;
    float priority=1;
    VkDeviceQueueCreateInfo qi={ .sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex=family,
        .queueCount=1, .pQueuePriorities=&priority };
    VkDeviceCreateInfo di={ .sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi };
    CHECK(vkCreateDevice(s_vk.phys,&di,NULL,&s_vk.device));
    VkQueue queue; vkGetDeviceQueue(s_vk.device,family,0,&queue);
    VkCommandPool pool;
    VkCommandPoolCreateInfo pi={ .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    CHECK(vkCreateCommandPool(s_vk.device,&pi,NULL,&pool));
    VkCommandBuffer cmd[2];
    VkCommandBufferAllocateInfo ai={ .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.commandBufferCount=2 };
    CHECK(vkAllocateCommandBuffers(s_vk.device,&ai,cmd));
    VkFence fence[2];
    VkFenceCreateInfo fi={ .sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vk_buffer scratch={0};
    assert(!vk_create_host_buffer(&scratch,1<<20,VK_BUFFER_USAGE_TRANSFER_DST_BIT));
    for (unsigned slot=0;slot<2;++slot) CHECK(vkCreateFence(s_vk.device,&fi,NULL,&fence[slot]));
    for (unsigned round=0;round<2;++round) {
        for (s_fif_cur=0;s_fif_cur<2;++s_fif_cur) {
            s_vk.cmd=cmd[s_fif_cur];
            CHECK(vkResetCommandBuffer(s_vk.cmd,0));
            VkCommandBufferBeginInfo bi={ .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            CHECK(vkBeginCommandBuffer(s_vk.cmd,&bi));
            vk_shadow_query_batch_begin();
            for (unsigned j=0;j<2+s_fif_cur;++j) {
                vk_shadow_query_begin();
                vkCmdFillBuffer(s_vk.cmd,scratch.buf,0,VK_WHOLE_SIZE,j+round);
                vk_shadow_query_end();
            }
            vk_shadow_query_resolve();
            CHECK(vkEndCommandBuffer(s_vk.cmd));
            VkSubmitInfo si={ .sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&s_vk.cmd };
            CHECK(vkQueueSubmit(queue,1,&si,fence[s_fif_cur]));
        }
        for (s_fif_cur=0;s_fif_cur<2;++s_fif_cur) {
            CHECK(vkWaitForFences(s_vk.device,1,&fence[s_fif_cur],VK_TRUE,10000000000ull));
            shadow_query_slot *slot=&shadow_queries.slots[s_fif_cur];
            assert(slot->count==4+2*s_fif_cur && slot->readback.ptr && !slot->dropped);
            uint64_t stamp[6];
            CHECK(vkGetQueryPoolResults(s_vk.device,slot->pool,0,slot->count,sizeof stamp,stamp,8,VK_QUERY_RESULT_64_BIT));
            const uint64_t *copied=slot->readback.ptr;
            for(unsigned i=0;i<slot->count;++i) assert(copied[i*2+1] && copied[i*2]==stamp[i]);
            vk_shadow_query_collect();
            assert(!slot->count);
            CHECK(vkResetFences(s_vk.device,1,&fence[s_fif_cur]));
        }
    }
    vk_shadow_query_destroy(); vk_destroy_idle_buffer(&scratch);
    for(unsigned i=0;i<2;++i) vkDestroyFence(s_vk.device,fence[i],NULL);
    vkDestroyCommandPool(s_vk.device,pool,NULL); vkDestroyDevice(s_vk.device,NULL); vkDestroyInstance(instance,NULL);
}
"""
with tempfile.TemporaryDirectory() as tmp:
    p = Path(tmp)
    (p / "run.c").write_text(setup + "\n" + aliases + "\n" + source + main)
    subprocess.run(["cc", "-std=gnu17", "-O2", str(p / "run.c"), "-lvulkan", "-o", str(p / "run")], check=True)
    r = subprocess.run([p / "run"], capture_output=True, text=True, check=True, timeout=30,
                       env=dict(os.environ, PS3RECOMP_ROUTE_TIMING="1"))
    print(r.stderr, end="")
    rows = re.findall(r"passes=(\d+) gpu_ms=([\d.]+) dropped=0 result=0", r.stderr)
    assert [int(n) for n, _ in rows] == [2, 3, 2, 3] and all(float(ms) > 0 for _, ms in rows)
    print("Vulkan timestamps and GPU copies agreed across two in-flight slots and reuse")
