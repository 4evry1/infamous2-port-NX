/* Optional final-frame processor. The renderer owns the returned instance. */
#pragma once
#include <stdint.h>
#include <vulkan/vulkan_core.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_present_processor rsx_present_processor;
typedef struct rsx_present_device {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
    void (*log)(const char* message);
} rsx_present_device;
typedef struct rsx_present_swapchain {
    VkSwapchainKHR swapchain;
    VkExtent2D extent;
    VkFormat format;
    VkImageUsageFlags usage;
    VkPresentModeKHR mode;
    const VkImage* images;
    uint32_t image_count;
    VkExtent2D hud_corner; /* top-left pixels to preserve on interpolated frames */
} rsx_present_swapchain;

/* Platform composition selects the implementation and reads startup settings. */
int rsx_present_processor_requested(void);
/* NULL means setup declined before any original presentation wait is consumed. */
rsx_present_processor* rsx_present_processor_create(const rsx_present_device*, const rsx_present_swapchain*);
/* All calls, including create/destroy, require exclusive queue ownership from the
 * renderer. The processor consumes the original waits; never present again as a
 * fallback after calling this. Destroy before replacing the swapchain/device. */
VkResult rsx_present_processor_present(rsx_present_processor*, const VkPresentInfoKHR*, uint64_t serial, int allowed);
void rsx_present_processor_destroy(rsx_present_processor*);
#ifdef __cplusplus
}
#endif
