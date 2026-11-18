#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>



namespace rsx::lsfg {

// Borrowed handles of the application-owned Vulkan device the adapter submits through.
struct BorrowedDeviceHandles {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  // The family of the queue below; must be the graphics and compute family.
  uint32_t queue_family = UINT32_MAX;
  VkQueue queue = VK_NULL_HANDLE;
  // Loader entry points, used for the instance-level format queries and for waiting on the queue.
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
  PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
};

// Runs `operation` with the adapter's queue, holding the device's queue lock for the duration.
// The wiring layer supplies this through DeviceContext::with_queue; it must not wait on a fence or
// acquire an image inside `operation`, because those waits happen outside the queue lock.
//
// `operation` receives the queue and returns the VkResult to propagate.
using QueueOperation = std::function<VkResult(VkQueue)>;
using WithQueue = std::function<VkResult(const QueueOperation&)>;

// Called by the adapter for everything it wants reported, including fatal failures. The adapter has
// no logging dependency, so the wiring layer supplies this and the SDK log header never reaches it.
// `message` is one complete line without a trailing newline.
using LogCallback = std::function<void(const char* message)>;

struct Options {
  // Path of the personally supplied Lossless.dll the shaders are read from.
  std::string shader_dll_path;
  // Path of the backend pipeline cache; empty disables it.
  std::string pipeline_cache_path;
};

// Everything the adapter needs to run, injected once by the wiring layer and immutable afterwards.
struct DeviceContext {
  BorrowedDeviceHandles handles;
  Options options;
  WithQueue with_queue;
  LogCallback log;

};

// The largest swapchain extent the adapter accepts. A bigger (docked) backbuffer falls back to the
// original presentation path instead of failing the connection.
inline constexpr uint32_t kMaxExtentWidth = 1280;
inline constexpr uint32_t kMaxExtentHeight = 720;

// Frames generated between two real ones. The backend takes the inverse flow scale, so the adapter
// opens the context with 1 / kFlowScale.
inline constexpr float kFlowScale = 0.25f;

// Whether a swapchain format is one the adapter can interpolate in.
inline bool IsSupportedSwapchainFormat(VkFormat format) {
  return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM;
}

// Bounded waits for a routine frame fence or a swapchain image acquisition.
inline constexpr uint64_t kRoutineWaitTimeoutNanoseconds = 5'000'000'000ull;

struct Description {
  VkSwapchainKHR swapchain;
  VkExtent2D swapchain_extent;
  VkFormat swapchain_format;
  VkImageUsageFlags swapchain_usage;
  uint32_t present_queue_family;
  bool fifo_presentation;
  const std::vector<VkImage>* swapchain_images;
};
} // namespace rsx::lsfg
