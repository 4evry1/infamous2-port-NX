
#include "rsx_present_processor.h"
#include <assert.h>
#include <stdio.h>
static struct {rsx_present_processor *present_processor; uint64_t present_processor_serial; VkQueue queue;} s_vk;
static int g_vk_overlay_active, s_vk_holding, raw_calls, processed_calls;
static VkResult injected_result;
static const VkPresentInfoKHR *expected;
static VkResult pvkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *info) {
    assert(q == s_vk.queue && info == expected); ++raw_calls; return VK_SUCCESS;
}
VkResult rsx_present_processor_present(rsx_present_processor *p, const VkPresentInfoKHR *info, uint64_t serial, int allowed) {
    assert(p == s_vk.present_processor && info == expected && serial == (uint64_t)processed_calls + 1);
    assert(allowed == (!g_vk_overlay_active && !s_vk_holding));
    ++processed_calls; return injected_result;
}
#include "production.inc"
int main(void) {
    VkSemaphore wait = (VkSemaphore)(uintptr_t)7;
    const VkPresentInfoKHR info = {.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,.pWaitSemaphores=&wait};
    expected=&info;
    assert(vk_present_processed(&info) == VK_SUCCESS && raw_calls == 1 && processed_calls == 0);
    s_vk.present_processor = (rsx_present_processor*)(uintptr_t)1;
    const VkResult cases[] = {VK_SUCCESS,VK_SUBOPTIMAL_KHR,VK_ERROR_OUT_OF_DATE_KHR,VK_TIMEOUT,VK_NOT_READY,VK_ERROR_DEVICE_LOST};
    for (unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        injected_result=cases[i];g_vk_overlay_active=i%2;s_vk_holding=i%3==0;
        const VkResult want = cases[i]==VK_TIMEOUT || cases[i]==VK_NOT_READY ? VK_ERROR_OUT_OF_DATE_KHR : cases[i];
        assert(vk_present_processed(&info)==want && raw_calls==1);
    }
    assert(processed_calls==6 && info.waitSemaphoreCount==1 && info.pWaitSemaphores==&wait);
    puts("PASS: one presentation owner, unchanged waits, monotonic serial, UI/hold bypass, failure propagation");
}
