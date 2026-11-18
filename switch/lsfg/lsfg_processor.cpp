// Switch LSFG processor, adapted from captainamericanx/ca_lsfg_presenter.cpp.
//
// The SDK presenter hands every final presentation of the game's swapchain to this adapter
// through the engine-owned C presentation port. The frame on screen after FSR, upscale and UI is
// captured into the backend's alternating source images, the backend generates one intermediate
// frame, and that generated frame is presented before the real one is put back and presented.
//
// Only one swapchain image is acquired at a time: Horizon's WSI allows a single acquired framebuffer,
// so the generated frame is presented first and the image for the real frame is acquired afterwards,
// from resources the adapter owns.
//
// Failure rules: the original present's semaphore is consumed as soon as the frame's first
// submission waits on it. After that the adapter must produce a present result itself - it never
// falls back to the original path. A result it can still reason about (out of date, suboptimal,
// device loss) is returned. A submission, fence or backend failure may leave GPU work pending, so the
// process is terminated after logging instead of unwinding live Vulkan resources.
//
// This translation unit must not include the SDK device or Vulkan-Hpp: the LSFG wrappers define
// their own vk:: types.
#include "lsfg_context.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>


#include <lsfg-vk-backend/lsfgvk.hpp>
#include <lsfg-vk-common/vulkan/command_buffer.hpp>
#include <lsfg-vk-common/vulkan/fence.hpp>
#include <lsfg-vk-common/vulkan/semaphore.hpp>
#include <lsfg-vk-common/vulkan/vulkan.hpp>

#include "frame_history.h"

namespace rsx::lsfg {
namespace {

using Clock = std::chrono::steady_clock;

// Accepted real frames between two summary lines.
constexpr uint64_t kFramesPerSummary = 300;
// Frame slots: one original present uses exactly one, and only original presents touch them.
constexpr size_t kFrameSlotCount = 3;
// The backend generates one intermediate frame per pair of source images.
constexpr size_t kGeneratedFrameCount = 1;
// The backend alternates between two source images.
constexpr size_t kSourceImageCount = 2;

VkImageMemoryBarrier ImageBarrier(VkImage image, VkAccessFlags source_access,
                                  VkAccessFlags destination_access, VkImageLayout before,
                                  VkImageLayout after) {
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = source_access;
  barrier.dstAccessMask = destination_access;
  barrier.oldLayout = before;
  barrier.newLayout = after;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return barrier;
}

std::string FormatDouble(double value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.2f", value);
  return text;
}

// Fatal failure the adapter cannot recover from: GPU work may be pending, so nothing is destroyed.
using FailFatallyCallback = std::function<void(const char* reason, VkResult result)>;

// One recorded submission: a command buffer, its completion fence, and whether it is still pending.
class CommandSlot {
 public:
  CommandSlot(const vk::Vulkan& vulkan, FailFatallyCallback fail_fatally)
      : vulkan_(vulkan),
        fail_fatally_(std::move(fail_fatally)),
        command_buffer_(vulkan),
        fence_(vulkan) {}

  // Waits for this slot's submission. A fence that does not signal inside the routine bound leaves
  // the slot's GPU resources in an unknown state, so callers that are mid-frame treat it as fatal.
  bool WaitForCompletion() {
    if (!is_pending_) {
      return true;
    }
    bool signaled = false;
    try {
      signaled = fence_.wait(vulkan(), kRoutineWaitTimeoutNanoseconds);
    } catch (const std::exception&) {
      signaled = false;
    }
    if (signaled) {
      is_pending_ = false;
    }
    return signaled;
  }

  // The queue has already been drained, so this must not block; it only settles the flag.
  void AwaitCompletionNoThrow() noexcept { is_pending_ = false; }

  // Resets the fence the next submission signals, after the previous one completed.
  void Prepare() {
    if (!WaitForCompletion()) {
      fail_fatally_("a frame fence", VK_TIMEOUT);
    }
    fence_.reset(vulkan());
  }

  vk::CommandBuffer& command_buffer() { return command_buffer_; }

  // Submits through the backend's own wrapper inside the queue callback, which holds the device
  // queue lock for the submit alone. The callback itself must not fail.
  void Submit(const WithQueue& with_queue, std::vector<VkSemaphore> wait_semaphores,
              std::vector<VkSemaphore> signal_semaphores) {
    const VkResult reached = with_queue([&](VkQueue) {
      try {
        // The wrapper takes the inverse-signalled fence and submits on the backend's borrowed
        // queue, which is this same queue; the queue lock is held by the callback.
        command_buffer_.submit(vulkan(), std::move(wait_semaphores), VK_NULL_HANDLE, 0,
                               std::move(signal_semaphores), VK_NULL_HANDLE, 0, fence_.handle());
      } catch (const std::exception&) {
        fail_fatally_("the LSFG frame submission", VK_ERROR_UNKNOWN);
      }
      return VK_SUCCESS;
    });
    if (reached != VK_SUCCESS) {
      fail_fatally_("reaching the graphics queue", reached);
    }
    is_pending_ = true;
  }

  ~CommandSlot() noexcept { AwaitCompletionNoThrow(); }

 private:
  const vk::Vulkan& vulkan() const { return vulkan_; }

  const vk::Vulkan& vulkan_;
  FailFatallyCallback fail_fatally_;
  vk::CommandBuffer command_buffer_;
  vk::Fence fence_;
  bool is_pending_ = false;
};

// A semaphore of the adapter's own, used for image acquisition and for presenting.
class OwnedSemaphore {
 public:
  explicit OwnedSemaphore(const vk::Vulkan& vulkan) : semaphore_(vulkan) {}
  VkSemaphore handle() const { return semaphore_.handle(); }

 private:
  vk::Semaphore semaphore_;
};

// A swapchain image's per-slot resources. Only original presents touch these, one at a time.
struct FrameSlot {
  FrameSlot(const vk::Vulkan& vulkan, const FailFatallyCallback& fail_fatally)
      : capture(vulkan, fail_fatally), restore(vulkan, fail_fatally), acquire(vulkan) {}

  // The capture and, later in the same frame, the restore. They are separate command buffers and
  // fences: the restore must not have to wait for the generation dispatch on the CPU.
  CommandSlot capture;
  CommandSlot restore;
  // Acquisition for the image the restore frame goes into. Its previous restore waits on it, so it
  // is only reused once that restore completed.
  OwnedSemaphore acquire;
  // The image index this slot last acquired for a restore, so the next restore knows its layout.
  uint32_t restored_image = 0;
};

// The backend context for one connection, from Configure to Reset.
class BackendConnection {
 public:
  BackendConnection(lsfgvk::backend::Instance& instance, VkDevice device, VkSwapchainKHR swapchain,
                    VkExtent2D extent, VkExtent2D hud_corner)
      : swapchain_(swapchain), extent_(extent), hud_corner_(hud_corner), instance_(instance) {
    context_ = &instance_.openLocalContext(extent.width, extent.height, false, 1.0f / kFlowScale,
                                           true, kGeneratedFrameCount);
    const vk::Vulkan& vulkan_instance = instance_.vulkan();
    uint32_t image_count = 0;
    VkResult result =
        vulkan_instance.df().GetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
    if (result != VK_SUCCESS || image_count == 0) {
      throw lsfgvk::backend::error("cannot enumerate the swapchain images");
    }
    images_.resize(image_count);
    result =
        vulkan_instance.df().GetSwapchainImagesKHR(device, swapchain, &image_count, images_.data());
    if (result != VK_SUCCESS) {
      throw lsfgvk::backend::error("cannot read the swapchain images");
    }
    images_.resize(image_count);
  }

  lsfgvk::backend::Context& context() { return *context_; }
  VkSwapchainKHR swapchain() const { return swapchain_; }
  VkExtent2D extent() const { return extent_; }
  VkExtent2D hud_corner() const { return hud_corner_; }
  const std::vector<VkImage>& images() const { return images_; }

  VkImage source_image(size_t index) const { return instance_.sourceImage(*context_, index); }

  VkImage destination_image(size_t index) const {
    return instance_.destinationImage(*context_, index);
  }

  void record_generation(const vk::CommandBuffer& command_buffer) {
    instance_.recordFrame(*context_, command_buffer);
  }

  // Closes the backend context exactly once; called before the instance is destroyed.
  void close_context() {
    if (!context_) {
      return;
    }
    instance_.closeContext(*context_);
    context_ = nullptr;
  }

 private:
  VkSwapchainKHR swapchain_;
  VkExtent2D extent_;
  VkExtent2D hud_corner_;
  lsfgvk::backend::Instance& instance_;
  lsfgvk::backend::Context* context_ = nullptr;
  std::vector<VkImage> images_;
};

// Periodic counters. Bounded: no per-frame history is kept.
struct Counters {
  uint64_t real_frames = 0;
  uint64_t generated_presents = 0;
  uint64_t capture_warmups = 0;
  uint64_t bypasses = 0;
  uint64_t accepted_in_window = 0;
  uint64_t real_interval_count = 0;
  double real_interval_total_ms = 0.0;
  double real_interval_min_ms = std::numeric_limits<double>::max();
  double real_interval_max_ms = 0.0;
  double processing_total_ms = 0.0;
  uint64_t processing_in_window = 0;

  void AddRealInterval(double milliseconds) {
    real_interval_count += 1;
    real_interval_total_ms += milliseconds;
    real_interval_min_ms = std::min(real_interval_min_ms, milliseconds);
    real_interval_max_ms = std::max(real_interval_max_ms, milliseconds);
  }

  double MeanRealInterval() const {
    return real_interval_count == 0 ? 0.0 : real_interval_total_ms / double(real_interval_count);
  }

  double MeanProcessing() const {
    return processing_in_window == 0 ? 0.0 : processing_total_ms / double(processing_in_window);
  }

  void ClearWindow() {
    accepted_in_window = 0;
    real_interval_count = 0;
    real_interval_total_ms = 0.0;
    real_interval_min_ms = std::numeric_limits<double>::max();
    real_interval_max_ms = 0.0;
    processing_total_ms = 0.0;
    processing_in_window = 0;
  }
};

// Times one present, in CPU wall time. These are not GPU times or display timestamps.
class PresentStopwatch {
 public:
  PresentStopwatch() : started_(Clock::now()) {}

  double ElapsedMilliseconds() const {
    return std::chrono::duration<double, std::milli>(Clock::now() - started_).count();
  }

  Clock::time_point started() const { return started_; }

 private:
  Clock::time_point started_;
};

class LsfgProcessor final {
 public:
  explicit LsfgProcessor(DeviceContext context)
      : context_(std::move(context)),
        fail_fatally_([this](const char* reason, VkResult result) noexcept {
          FailFatally(reason, result);
        }) {
  }

  ~LsfgProcessor() { ResetInternal(); }

  VkImageUsageFlags RequestedSwapchainUsage() const {
    return VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }

  bool Configure(const Description& description);

  VkResult Present(const VkPresentInfoKHR& original_present_info, uint64_t guest_frame_serial,
                   bool interpolation_allowed);

  void Reset(uint64_t swapchain_generation) {
    (void)swapchain_generation;
    ResetInternal();
  }

 private:
  // Queued and present results, with the generated and acquisition sub-results folded in.
  VkResult PresentFrame(const VkPresentInfoKHR& original_present_info, uint64_t guest_frame_serial,
                        bool interpolation_allowed);
  VkResult PresentInterpolated(const VkPresentInfoKHR& original_present_info, bool should_generate,
                               uint32_t source_index);
  VkResult SubmitCapture(FrameSlot& slot, uint32_t image_index, uint32_t source_index,
                         bool should_generate, std::vector<VkSemaphore> original_waits);
  VkResult SubmitRestore(FrameSlot& slot);

  bool ConfigureConnection(const Description& description);
  bool ValidateDescription(const Description& description, std::string& reason) const;
  bool HasRequiredBlitSupport(VkFormat swapchain_format, std::string& reason) const;
  void ResetInternal();
  void ResetResources();
  void LogSummaryIfDue();
  void Log(const std::string& line) const;
  // Logs and terminates: GPU work may still be pending, so nothing is destroyed.
  [[noreturn]] void FailFatally(const char* reason, VkResult result) const noexcept;

  VkResult PresentToQueue(const VkPresentInfoKHR& info) const;

  const DeviceContext context_;
  // The logger the wiring injected, captured for the fatal path used from deep inside a frame.
  FailFatallyCallback fail_fatally_;
  // Semaphores the adapter signals for its two presentations of a frame. Created with the
  // connection, which is the only time the backend's Vulkan wrapper is available.
  std::vector<std::unique_ptr<OwnedSemaphore>> capture_output_;
  std::vector<std::unique_ptr<OwnedSemaphore>> restore_output_;
  std::unique_ptr<lsfgvk::backend::Instance> backend_;
  std::unique_ptr<BackendConnection> connection_;
  std::vector<std::unique_ptr<FrameSlot>> slots_;
  // Valid only while a connection exists; cleared on reset so it can never be used afterwards.
  const vk::Vulkan* vulkan_ = nullptr;
  FrameHistory history_;
  Counters counters_;
  Clock::time_point last_real_frame_time_{};
  uint64_t submitted_dispatches_ = 0;
  bool has_last_real_frame_time_ = false;
  bool has_logged_generated_ = false;
  PFN_vkQueueWaitIdle queue_wait_idle_ = nullptr;
};

// Purely advisory limits; the capability checks that decide are the Vulkan format and feature ones.
bool LsfgProcessor::ValidateDescription(const Description& description,
                                                      std::string& reason) const {
  if (!description.fifo_presentation) {
    reason = "the swapchain does not present with vsync (FIFO or FIFO relaxed)";
    return false;
  }
  if (!description.swapchain_images || description.swapchain_images->empty() ||
      description.swapchain == VK_NULL_HANDLE) {
    reason = "the swapchain has no images";
    return false;
  }
  if (description.present_queue_family != context_.handles.queue_family) {
    reason = "the present and graphics queue families differ";
    return false;
  }
  if (description.swapchain_extent.width == 0 || description.swapchain_extent.height == 0) {
    reason = "the swapchain extent is empty";
    return false;
  }
  if (description.swapchain_extent.width > kMaxExtentWidth ||
      description.swapchain_extent.height > kMaxExtentHeight) {
    reason = "the swapchain extent is larger than 1280x720";
    return false;
  }
  if (!IsSupportedSwapchainFormat(description.swapchain_format)) {
    reason = "the swapchain format is not R8G8B8A8 or B8G8R8A8 UNORM";
    return false;
  }
  const VkImageUsageFlags requested = RequestedSwapchainUsage();
  if ((description.swapchain_usage & requested) != requested) {
    reason = "the swapchain was not created with the transfer usages the adapter needs";
    return false;
  }
  return true;
}

// The adapter blits the swapchain image to and from the RGBA8 backend images, so the swapchain format
// needs both blit directions and RGBA8 needs both as well.
bool LsfgProcessor::HasRequiredBlitSupport(VkFormat swapchain_format,
                                                         std::string& reason) const {
  if (!context_.handles.get_instance_proc_addr) {
    reason = "the loader has no vkGetInstanceProcAddr";
    return false;
  }
  const auto get_physical_device_format_properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
          context_.handles.get_instance_proc_addr(context_.handles.instance,
                                                  "vkGetPhysicalDeviceFormatProperties"));
  if (!get_physical_device_format_properties) {
    reason = "vkGetPhysicalDeviceFormatProperties is not available";
    return false;
  }
  const auto has_blit_flags = [&](VkFormat format, VkFormatFeatureFlags required) {
    VkFormatProperties properties{};
    get_physical_device_format_properties(context_.handles.physical_device, format, &properties);
    return (properties.optimalTilingFeatures & required) == required;
  };

  if (!has_blit_flags(swapchain_format,
                      VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
    reason = "the swapchain format has no optimal-tiling blit source and destination support";
    return false;
  }
  if (!has_blit_flags(VK_FORMAT_R8G8B8A8_UNORM,
                      VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
    reason = "R8G8B8A8_UNORM has no optimal-tiling blit source and destination support";
    return false;
  }
  return true;
}

bool LsfgProcessor::Configure(const Description& description) {
  try {
    return ConfigureConnection(description);
  } catch (const std::exception& error) {
    FailFatally(error.what(), VK_ERROR_UNKNOWN);
  } catch (...) {
    FailFatally("an unknown exception while configuring", VK_ERROR_UNKNOWN);
  }
}

bool LsfgProcessor::ConfigureConnection(const Description& description) {
  ResetInternal();

  std::string reason;
  if (!ValidateDescription(description, reason)) {
    Log("[LSFG] not used for this swapchain: " + reason);
    return false;
  }
  // shaderStorageImageExtendedFormats is enabled on the device by the wiring layer; a device without
  // it would make the backend's compute passes invalid, so this is checked before any resource.
  if (!HasRequiredBlitSupport(description.swapchain_format, reason)) {
    Log("[LSFG] not used for this swapchain: " + reason);
    return false;
  }

  // The backend creates its own device resources and may submit while initializing, so both it and
  // the context are built inside the synchronized queue callback.
  bool is_initialized = false;
  std::string initialization_error;
  const VkResult reached = context_.with_queue([&](VkQueue) {
    try {
      lsfgvk::backend::BorrowedDevice borrowed{};
      borrowed.instance = context_.handles.instance;
      borrowed.physicalDevice = context_.handles.physical_device;
      borrowed.device = context_.handles.device;
      borrowed.queueFamilyIndex = context_.handles.queue_family;
      borrowed.queue = context_.handles.queue;
      borrowed.getInstanceProcAddr = context_.handles.get_instance_proc_addr;
      if (!context_.options.pipeline_cache_path.empty()) {
        borrowed.pipelineCachePath = std::filesystem::path(context_.options.pipeline_cache_path);
      }
      backend_ = std::make_unique<lsfgvk::backend::Instance>(
          borrowed, std::filesystem::path(context_.options.shader_dll_path), false);
      connection_ = std::make_unique<BackendConnection>(*backend_, context_.handles.device,
                                                        description.swapchain,
                                                        description.swapchain_extent, description.hud_corner);
      for (size_t index = 0; index < kFrameSlotCount; ++index) {
        slots_.push_back(std::make_unique<FrameSlot>(backend_->vulkan(), fail_fatally_));
      }
      // One output semaphore per swapchain image, for each of the two presentations of a frame.
      for (size_t index = 0; index < description.swapchain_images->size(); ++index) {
        capture_output_.push_back(std::make_unique<OwnedSemaphore>(backend_->vulkan()));
        restore_output_.push_back(std::make_unique<OwnedSemaphore>(backend_->vulkan()));
      }
      vulkan_ = &backend_->vulkan();
      is_initialized = true;
    } catch (const std::exception& error) {
      initialization_error = error.what();
    }
    return VK_SUCCESS;
  });
  if (reached != VK_SUCCESS) {
    Log("[LSFG] disabled: the graphics queue could not be reached");
    return false;
  }

  if (!is_initialized) {
    // A failure here may or may not have submitted work. The backend either unwound itself or did
    // not get far enough to own a queue submission; the process cannot tell them apart safely, so a
    // backend that fails after the device was touched is fatal rather than silently continuing.
    if (backend_) {
      FailFatally(initialization_error.c_str(), VK_ERROR_INITIALIZATION_FAILED);
    }
    Log("[LSFG] disabled: " + initialization_error);
    ResetInternal();
    return false;
  }

  history_.Reset();
  counters_ = Counters{};
  submitted_dispatches_ = 0;
  has_last_real_frame_time_ = false;
  has_logged_generated_ = false;

  char line[192];
  std::snprintf(line, sizeof(line),
                "[LSFG] enabled: 2x, motion %.0f%%, performance mode, %ux%u output, %u swapchain "
                "images",
                kFlowScale * 100.0f, description.swapchain_extent.width,
                description.swapchain_extent.height,
                unsigned(description.swapchain_images->size()));
  Log(line);
  return true;
}

VkResult LsfgProcessor::Present(const VkPresentInfoKHR& original_present_info,
                                              uint64_t guest_frame_serial,
                                              bool interpolation_allowed) {
  try {
    return PresentFrame(original_present_info, guest_frame_serial, interpolation_allowed);
  } catch (const std::exception& error) {
    FailFatally(error.what(), VK_ERROR_UNKNOWN);
  } catch (...) {
    FailFatally("an unknown exception while presenting", VK_ERROR_UNKNOWN);
  }
}

VkResult LsfgProcessor::PresentFrame(const VkPresentInfoKHR& original_present_info,
                                                   uint64_t guest_frame_serial,
                                                   bool interpolation_allowed) {
  // Without a connection the adapter must not present at all: the SDK only calls Present while the
  // processor is configured for the connection, so this is a contract break, not a fallback.
  if (!connection_ || !vulkan_) {
    FailFatally("a present without a configured connection", VK_ERROR_UNKNOWN);
  }

  // The adapter only ever handles the swapchain it was configured for. Anything else is a contract
  // violation the SDK documents as impossible, and guessing a fallback would desynchronize the
  // backend's source parity from the policy's.
  if (original_present_info.swapchainCount != 1 || !original_present_info.pSwapchains ||
      original_present_info.pSwapchains[0] != connection_->swapchain() ||
      !original_present_info.pImageIndices ||
      original_present_info.pImageIndices[0] >= connection_->images().size()) {
    FailFatally("a present outside the configured swapchain", VK_ERROR_UNKNOWN);
  }
  if (original_present_info.pNext != nullptr) {
    // An extension chain the adapter cannot inspect would be dropped from its own presents, changing
    // what the presentation means. The presenter does not use one; treat it as a contract break.
    FailFatally("a present with an extension chain", VK_ERROR_UNKNOWN);
  }
  // Dropping a wait would present an image the renderer has not finished writing, so a count without
  // its pointer is fatal before anything is observed or submitted.
  if (original_present_info.waitSemaphoreCount > 0 && !original_present_info.pWaitSemaphores) {
    FailFatally("a present with waits but no wait semaphores", VK_ERROR_UNKNOWN);
  }

  const PresentStopwatch stopwatch;
  const uint64_t now_milliseconds = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 stopwatch.started().time_since_epoch())
                                                 .count());

  // Exactly one Observe per frame, before anything can be submitted for it.
  const PresentationPlan plan =
      history_.Observe(guest_frame_serial, interpolation_allowed, now_milliseconds);

  if (plan.decision == PresentationDecision::kPassthrough) {
    const VkResult result = PresentToQueue(original_present_info);
    const bool is_presented = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
    if (is_presented) {
      counters_.bypasses += 1;
      counters_.processing_total_ms += stopwatch.ElapsedMilliseconds();
      counters_.processing_in_window += 1;
      LogSummaryIfDue();
    }
    // Only a repeated serial of the frame last accepted is the same guest refresh, so only that
    // keeps the continuous-motion baseline. UI, inactive and stale frames all break the motion the
    // interval mean describes.
    const bool is_repeated_accepted_serial =
        is_presented && interpolation_allowed && guest_frame_serial != 0 &&
        history_.has_accepted_serial() && guest_frame_serial == history_.last_accepted_serial();
    if (!is_repeated_accepted_serial) {
      has_last_real_frame_time_ = false;
    }
    return result;
  }

  const bool should_generate = plan.decision == PresentationDecision::kGenerate;
  const VkResult result = PresentInterpolated(original_present_info, should_generate,
                                              uint32_t(plan.source_index));
  const bool is_presented = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
  if (!is_presented) {
    // The real frame did not reach the host, so it is not an accepted frame and the baseline breaks.
    has_last_real_frame_time_ = false;
    return result;
  }

  counters_.real_frames += 1;
  counters_.accepted_in_window += 1;
  if (has_last_real_frame_time_) {
    const double elapsed_milliseconds =
        std::chrono::duration<double, std::milli>(stopwatch.started() - last_real_frame_time_)
            .count();
    // A gap longer than a continuous interval (a menu, a pause) or a clock regression is not motion
    // the interval mean should describe, so it is counted as a frame but not as an interval.
    if (elapsed_milliseconds >= 0.0 &&
        elapsed_milliseconds <= double(kMaxContinuousIntervalMilliseconds)) {
      counters_.AddRealInterval(elapsed_milliseconds);
    }
  }
  last_real_frame_time_ = stopwatch.started();
  has_last_real_frame_time_ = true;
  if (!should_generate) {
    counters_.capture_warmups += 1;
  }
  counters_.processing_total_ms += stopwatch.ElapsedMilliseconds();
  counters_.processing_in_window += 1;
  LogSummaryIfDue();

  return result;
}

VkResult LsfgProcessor::PresentToQueue(const VkPresentInfoKHR& info) const {
  VkResult presented = VK_SUCCESS;
  const VkResult reached = context_.with_queue([&](VkQueue queue) {
    presented = vulkan_->df().QueuePresentKHR(queue, &info);
    return presented;
  });
  return reached != VK_SUCCESS ? reached : presented;
}

VkResult LsfgProcessor::PresentInterpolated(
    const VkPresentInfoKHR& original_present_info, bool should_generate, uint32_t source_index) {
  const uint32_t image_index = original_present_info.pImageIndices[0];
  FrameSlot& slot = *slots_[size_t(image_index) % slots_.size()];

  // Every wait the original present carried is forwarded untruncated.
  std::vector<VkSemaphore> original_waits;
  if (original_present_info.waitSemaphoreCount > 0) {
    original_waits.assign(
        original_present_info.pWaitSemaphores,
        original_present_info.pWaitSemaphores + original_present_info.waitSemaphoreCount);
  }
  VkResult result =
      SubmitCapture(slot, image_index, source_index, should_generate, std::move(original_waits));
  if (result != VK_SUCCESS) {
    return result;
  }

  const VkSemaphore capture_output = capture_output_[image_index]->handle();
  VkPresentInfoKHR capture_present = original_present_info;
  capture_present.pWaitSemaphores = &capture_output;
  capture_present.waitSemaphoreCount = 1;
  if (!should_generate) {
    // Warmup: the capture was recorded and dispatched, the real image is presented as it is.
    return PresentToQueue(capture_present);
  }

  // The generated frame goes on screen first: Horizon allows one acquired image at a time, so the
  // real frame's image is acquired only after this present has consumed its own semaphore.
  const VkResult generated_result = PresentToQueue(capture_present);
  if (generated_result != VK_SUCCESS && generated_result != VK_SUBOPTIMAL_KHR) {
    // Nothing was generated on screen and the original wait has been consumed; the SDK maps this to
    // a reconnect, and the real image was never restored, so there is nothing further to present.
    return generated_result;
  }
  // Counted as soon as the generated frame reached the host, even if the restore below fails.
  counters_.generated_presents += 1;
  if (!has_logged_generated_) {
    has_logged_generated_ = true;
    char line[128];
    std::snprintf(line, sizeof(line), "[LSFG] first generated frame presented at %ux%u",
                  connection_->extent().width, connection_->extent().height);
    Log(line);
  }

  result = SubmitRestore(slot);
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    // The acquisition failed before a second image existed: report it so the SDK reconnects. No
    // restore resource is in flight in that case; SubmitRestore is fatal for anything else.
    return result;
  }
  const bool is_acquire_suboptimal = result == VK_SUBOPTIMAL_KHR;

  const VkSemaphore restore_output = restore_output_[slot.restored_image]->handle();
  VkPresentInfoKHR real_present = original_present_info;
  real_present.pImageIndices = &slot.restored_image;
  real_present.pWaitSemaphores = &restore_output;
  real_present.waitSemaphoreCount = 1;
  const VkResult real_result = PresentToQueue(real_present);
  if (real_result == VK_SUCCESS && (generated_result == VK_SUBOPTIMAL_KHR || is_acquire_suboptimal)) {
    return VK_SUBOPTIMAL_KHR;
  }
  return real_result;
}

VkResult LsfgProcessor::SubmitCapture(FrameSlot& slot, uint32_t image_index,
                                                    uint32_t source_index, bool should_generate,
                                                    std::vector<VkSemaphore> original_waits) {
  slot.capture.Prepare();

  // The policy alternates capture parity once per submitted dispatch; a frame can only be captured
  // into the image its own parity names.
  const uint32_t expected_source_index = uint32_t(submitted_dispatches_ & 1u);
  if (source_index != expected_source_index) {
    FailFatally("the capture parity diverged from the submitted dispatches", VK_ERROR_UNKNOWN);
  }

  const vk::Vulkan& vulkan_instance = *vulkan_;
  const VkImage swapchain_image = connection_->images()[image_index];
  const VkImage source = connection_->source_image(source_index);
  const VkExtent2D extent = connection_->extent();

  // The source image still holds the previous frame, and the backend's last dispatch read it, so the
  // capture overwrites it only after both the transfer read and the shader read are visible.
  const VkImageMemoryBarrier to_transfer_src =
      ImageBarrier(swapchain_image, VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  const VkImageMemoryBarrier back_to_present =
      ImageBarrier(swapchain_image, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  const VkImageMemoryBarrier source_to_dst =
      ImageBarrier(source, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  const VkImageMemoryBarrier source_back_to_read =
      ImageBarrier(source, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);

  vk::CommandBuffer& commands = slot.capture.command_buffer();
  commands.begin(vulkan_instance);
  commands.insertBarriers(vulkan_instance, {to_transfer_src, source_to_dst});
  commands.blitImage(vulkan_instance, {}, {swapchain_image, source}, extent,
                     {back_to_present, source_back_to_read});

  if (submitted_dispatches_ == 0) {
    // The first dispatch reads both source images; the other one starts as this frame's copy.
    const VkImage seed = connection_->source_image((source_index + 1) % kSourceImageCount);
    const VkImageMemoryBarrier seed_to_dst =
        ImageBarrier(seed, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageMemoryBarrier seed_back_to_read =
        ImageBarrier(seed, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    commands.insertBarriers(vulkan_instance, {to_transfer_src, seed_to_dst});
    commands.blitImage(vulkan_instance, {}, {swapchain_image, seed}, extent,
                       {back_to_present, seed_back_to_read});
  }

  // The backend records its own compute passes and the memory barriers between them.
  connection_->record_generation(commands);

  if (should_generate) {
    const VkImage generated = connection_->destination_image(0);
    commands.insertBarriers(
        vulkan_instance,
        {ImageBarrier(generated, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
         ImageBarrier(swapchain_image, VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)});
    commands.blitImage(
        vulkan_instance, {}, {generated, swapchain_image}, extent,
        {ImageBarrier(generated, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
         ImageBarrier(swapchain_image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)});
    // Keep the text from the real frame sharp when the numeric values change.
    const VkExtent2D corner = connection_->hud_corner();
    if (corner.width && corner.height) {
      commands.blitImage(vulkan_instance,
          {ImageBarrier(source, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
           ImageBarrier(swapchain_image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)},
          {source, swapchain_image}, corner,
          {ImageBarrier(source, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
           ImageBarrier(swapchain_image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)});
    }
  }
  commands.end(vulkan_instance);

  // The original present's semaphores are consumed by this submission; nothing presents the original
  // image after it, so the adapter reports the outcome itself from here on.
  slot.capture.Submit(context_.with_queue, original_waits,
                      {capture_output_[image_index]->handle()});
  submitted_dispatches_ += 1;
  return VK_SUCCESS;
}

VkResult LsfgProcessor::SubmitRestore(FrameSlot& slot) {
  // The restore frame's image is acquired outside the queue lock: waiting for an image while holding
  // the queue lock would stall the renderer that has to present the frame releasing it.
  slot.restore.Prepare();
  uint32_t restored_image = 0;
  const VkResult acquired =
      vulkan_->df().AcquireNextImageKHR(context_.handles.device, connection_->swapchain(),
                                        kRoutineWaitTimeoutNanoseconds, slot.acquire.handle(),
                                        VK_NULL_HANDLE, &restored_image);
  if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
    // No second image exists yet, so nothing of the adapter's is in flight for it: this is the
    // ordinary "the swapchain moved on" path and the SDK reconnects on it.
    return acquired;
  }
  if (restored_image >= connection_->images().size()) {
    FailFatally("acquiring a swapchain image index out of range", VK_ERROR_UNKNOWN);
  }
  slot.restored_image = restored_image;

  const vk::Vulkan& vulkan_instance = *vulkan_;
  const VkImage source = connection_->source_image(uint32_t((submitted_dispatches_ - 1) & 1u));
  const VkImage restored = connection_->images()[restored_image];
  const VkExtent2D extent = connection_->extent();

  vk::CommandBuffer& commands = slot.restore.command_buffer();
  commands.begin(vulkan_instance);
  commands.insertBarriers(
      vulkan_instance,
      // The source was written by the capture (transfer) and read by the backend's compute passes,
      // so both are the previous accesses this blit reads after.
      {ImageBarrier(source,
                    VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                        VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
       ImageBarrier(restored, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)});
  commands.blitImage(
      vulkan_instance, {}, {source, restored}, extent,
      {ImageBarrier(source, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
       ImageBarrier(restored, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)});
  commands.end(vulkan_instance);

  slot.restore.Submit(context_.with_queue, {slot.acquire.handle()},
                      {restore_output_[slot.restored_image]->handle()});
  // SUBOPTIMAL survives to the present result; the restore itself did submit.
  return acquired;
}

void LsfgProcessor::ResetInternal() {
  try {
    ResetResources();
  } catch (const std::exception& error) {
    FailFatally(error.what(), VK_ERROR_UNKNOWN);
  } catch (...) {
    FailFatally("an unknown exception while resetting", VK_ERROR_UNKNOWN);
  }
}

void LsfgProcessor::ResetResources() {
  history_.Reset();
  if (!backend_ && !connection_) {
    return;
  }

  // Queue-idle cleanup is specific to the pinned Switch WSI: it retires the presentation semaphores
  // before they are destroyed. The ordinary command fences prove GPU submission completion only, not
  // display consumption.
  if (!queue_wait_idle_ && context_.handles.get_device_proc_addr) {
    queue_wait_idle_ = reinterpret_cast<PFN_vkQueueWaitIdle>(
        context_.handles.get_device_proc_addr(context_.handles.device, "vkQueueWaitIdle"));
  }
  if (!queue_wait_idle_) {
    // Without a way to drain the queue, freeing the semaphores would race work still in flight.
    FailFatally("vkQueueWaitIdle is not available", VK_ERROR_INITIALIZATION_FAILED);
  }
  VkResult waited = VK_SUCCESS;
  const VkResult reached = context_.with_queue([&](VkQueue) {
    waited = queue_wait_idle_(context_.handles.queue);
    return waited;
  });
  if (reached != VK_SUCCESS || waited != VK_SUCCESS) {
    FailFatally("waiting for the graphics queue to go idle", waited);
  }

  // Only now, with the queue drained, are the slots' fences and the semaphores guaranteed settled.
  capture_output_.clear();
  restore_output_.clear();
  slots_.clear();
  if (connection_) {
    connection_->close_context();
    connection_.reset();
  }
  backend_.reset();
  vulkan_ = nullptr;
  counters_ = Counters{};
  submitted_dispatches_ = 0;
  has_last_real_frame_time_ = false;
  has_logged_generated_ = false;
}

void LsfgProcessor::LogSummaryIfDue() {
  if (counters_.accepted_in_window < kFramesPerSummary) {
    return;
  }
  std::string line = "[LSFG] " + std::to_string(counters_.accepted_in_window) +
                     " accepted real frames: " + std::to_string(counters_.real_frames) +
                     " real, " + std::to_string(counters_.generated_presents) +
                     " generated presents, " + std::to_string(counters_.capture_warmups) +
                     " warmup, " + std::to_string(counters_.bypasses) + " bypass";
  if (counters_.real_interval_count != 0) {
    line += ", CPU accepted-frame interval mean " + FormatDouble(counters_.MeanRealInterval()) +
            " ms min " + FormatDouble(counters_.real_interval_min_ms) + " ms max " +
            FormatDouble(counters_.real_interval_max_ms) + " ms";
  }
  line += ", CPU processing mean " + FormatDouble(counters_.MeanProcessing()) + " ms";
  Log(line);
  counters_.ClearWindow();
}

void LsfgProcessor::Log(const std::string& line) const {
  if (context_.log) {
    context_.log(line.c_str());
  }
}

void LsfgProcessor::FailFatally(const char* reason, VkResult result) const noexcept {
  // Nothing here may throw: this runs while GPU work may still be pending, so the process must reach
  // std::_Exit even if formatting or the injected logger fails.
  char line[320];
  std::snprintf(line, sizeof(line),
                "[LSFG] %s failed (%d); exiting without freeing resources that may still be in "
                "flight",
                reason ? reason : "the LSFG adapter", int(result));
  bool is_logged = false;
  try {
    if (context_.log) {
      context_.log(line);
      is_logged = true;
    }
  } catch (...) {
    is_logged = false;
  }
  if (!is_logged) {
    std::fputs(line, stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
  }
  std::_Exit(EXIT_FAILURE);
}

}  // namespace

} // namespace rsx::lsfg

#include "rsx_present_processor.h"
#include <cstring>
#include <unistd.h>

struct rsx_present_processor {
    std::unique_ptr<rsx::lsfg::LsfgProcessor> implementation;
};
extern "C" int rsx_present_processor_requested(void) {
    const char *value = std::getenv("PS3RECOMP_LSFG");
    return value && std::strcmp(value, "1") == 0;
}
extern "C" rsx_present_processor *rsx_present_processor_create(
    const rsx_present_device *device, const rsx_present_swapchain *swapchain) {
    if (!device || !swapchain || !rsx_present_processor_requested()) return nullptr;
    constexpr const char *dll = "sdmc:/switch/infamous2-port/lsfg/Lossless.dll";
    if (access(dll, R_OK) != 0) {
        if (device->log) device->log("[LSFG] disabled: Lossless.dll is missing or unreadable");
        return nullptr;
    }
    if (!device->instance || !device->physical_device || !device->device || !device->queue ||
        !device->get_instance_proc_addr || !device->get_device_proc_addr ||
        !swapchain->images || !swapchain->image_count || swapchain->image_count > 8) return nullptr;
    try {
        rsx::lsfg::DeviceContext context;
        context.handles = {device->instance, device->physical_device, device->device,
            device->queue_family, device->queue, device->get_instance_proc_addr, device->get_device_proc_addr};
        context.options.shader_dll_path = dll;
        context.options.pipeline_cache_path = "sdmc:/switch/infamous2-port/lsfg/pipeline-cache.bin";
        const VkQueue queue = device->queue;
        // The renderer's present owner serializes this entire transaction with queue work.
        context.with_queue = [queue](const rsx::lsfg::QueueOperation &operation) { return operation(queue); };
        context.log = device->log;
        auto processor = std::make_unique<rsx_present_processor>();
        processor->implementation = std::make_unique<rsx::lsfg::LsfgProcessor>(std::move(context));
        const std::vector<VkImage> images(swapchain->images, swapchain->images + swapchain->image_count);
        rsx::lsfg::Description description{swapchain->swapchain, swapchain->extent, swapchain->format,
            swapchain->usage, device->queue_family,
            swapchain->mode == VK_PRESENT_MODE_FIFO_KHR || swapchain->mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR, &images, swapchain->hud_corner};
        if (!processor->implementation->Configure(description)) return nullptr;
        return processor.release();
    } catch (const std::exception &) {
        if (device->log) device->log("[LSFG] disabled: presentation setup could not be allocated");
        return nullptr;
    }
}
extern "C" VkResult rsx_present_processor_present(rsx_present_processor *processor,
    const VkPresentInfoKHR *info, uint64_t serial, int allowed) {
    return processor->implementation->Present(*info, serial, allowed != 0);
}
extern "C" void rsx_present_processor_destroy(rsx_present_processor *processor) { delete processor; }
