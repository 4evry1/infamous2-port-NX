#include "rsx_present_processor.h"
#ifndef HZ_HAS_LSFG
int rsx_present_processor_requested(void) { return 0; }
rsx_present_processor* rsx_present_processor_create(const rsx_present_device* d, const rsx_present_swapchain* s) { (void)d; (void)s; return 0; }
VkResult rsx_present_processor_present(rsx_present_processor* p, const VkPresentInfoKHR* i, uint64_t s, int a) { (void)p; (void)i; (void)s; (void)a; return VK_ERROR_INITIALIZATION_FAILED; }
void rsx_present_processor_destroy(rsx_present_processor* p) { (void)p; }
#endif
