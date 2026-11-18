// Switch: temporal decisions for the LSFG presentation adapter.
//
// The adapter below this policy owns GPU history, warmup seeding and per-image semaphores; this
// header only answers what to do with the next presentation. It is pure: no Vulkan, no clocks, no
// globals, so tests/frame_history.cpp drives it directly with explicit serials and times.
//
// Rules:
//   - The first two accepted new guest frames only warm the interpolation history.
//   - A repeated serial is the same guest output presented again (a UI pass on top of it, a
//     duplicate swap of one refresh): it must bypass without touching warmup or the backend.
//   - A serial below the last accepted one, a zero serial and a disallowed frame reset the warmup
//     conservatively.
//   - A gap or a clock regression restarts the warmup, because the backend's previous pair is stale.
#pragma once

#include <cstdint>

namespace rsx::lsfg {

// Accepted new frames that must be captured before generated frames are presented.
inline constexpr uint32_t kAcceptedFramesPerWarmup = 2;

// Longest real-frame interval still treated as continuous motion.
inline constexpr uint64_t kMaxContinuousIntervalMilliseconds = 250;

enum class PresentationDecision : uint8_t {
  // Send the original presentation untouched to the queue.
  kPassthrough,
  // Capture (and dispatch) history, but present the original frame.
  kCaptureWarmup,
  // Capture, generate and present the generated frame, then the real one.
  kGenerate,
};

// The outcome plus the index of the backend source image this frame must be captured into.
struct PresentationPlan {
  PresentationDecision decision = PresentationDecision::kPassthrough;
  // Only meaningful for the capture decisions; real frames capture into index &1.
  uint32_t source_index = 0;
};

class FrameHistory {
 public:
  // `guest_serial` is the presenter's monotonic guest output serial, 0 when the presentation is
  // not a guest output frame. `interpolation_allowed` is false for UI-thread paints.
  // `source_index` of a capture decision alternates only on accepted new frames.
  PresentationPlan Observe(uint64_t guest_serial, bool interpolation_allowed,
                           uint64_t monotonic_milliseconds) {
    const bool is_new_guest_frame =
        interpolation_allowed && guest_serial != 0 && (!has_accepted_serial_ || guest_serial > last_accepted_serial_);
    if (!is_new_guest_frame) {
      // A repeated serial is the same guest output: not a new frame, so the warmup it would seed
      // stays where it is. Everything else starts the history over.
      const bool is_repeated_serial =
          interpolation_allowed && guest_serial != 0 && has_accepted_serial_ && guest_serial == last_accepted_serial_;
      if (!is_repeated_serial) {
        warmup_remaining_ = kAcceptedFramesPerWarmup;
      }
      return PresentationPlan{PresentationDecision::kPassthrough, 0};
    }

    if (has_accepted_serial_) {
      const bool clock_regressed = monotonic_milliseconds < last_accepted_milliseconds_;
      const bool gap_too_long = monotonic_milliseconds - last_accepted_milliseconds_ >
                                kMaxContinuousIntervalMilliseconds;
      if (clock_regressed || gap_too_long) {
        warmup_remaining_ = kAcceptedFramesPerWarmup;
      }
    }

    has_accepted_serial_ = true;
    last_accepted_serial_ = guest_serial;
    last_accepted_milliseconds_ = monotonic_milliseconds;

    const uint32_t source_index = capture_index_;
    capture_index_ ^= 1u;

    if (warmup_remaining_ != 0) {
      --warmup_remaining_;
      return PresentationPlan{PresentationDecision::kCaptureWarmup, source_index};
    }
    return PresentationPlan{PresentationDecision::kGenerate, source_index};
  }

  // Starts the history over for a new interpolation context.
  void Reset() {
    *this = FrameHistory{};
  }

  uint32_t warmup_remaining() const { return warmup_remaining_; }
  bool has_accepted_serial() const { return has_accepted_serial_; }
  uint64_t last_accepted_serial() const { return last_accepted_serial_; }

 private:
  bool has_accepted_serial_ = false;
  uint64_t last_accepted_serial_ = 0;
  uint64_t last_accepted_milliseconds_ = 0;
  uint32_t warmup_remaining_ = kAcceptedFramesPerWarmup;
  uint32_t capture_index_ = 0;
};

}  // namespace rsx::lsfg
