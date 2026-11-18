// Host checks for rsx::lsfg::FrameHistory (project/src/frame_history.h).
//
// The LSFG adapter decides from this policy whether a presented frame is passed straight through,
// only warms (and captures) the interpolation history, or is generated. Everything below is stated in
// the sequence itself: explicit serials, explicit monotonic millisecond stamps. No GPU, no clock.
//
// Build and run: g++ -std=c++23 -O1 -Wall -Wextra -Werror -I project/src
//                -o lsfg_history tests/lsfg_history.cpp
// Prints "PASS <name>" per check and "FAILURES <n>" last; exits 0 only when nothing failed.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "frame_history.h"

namespace {

using rsx::lsfg::FrameHistory;
using rsx::lsfg::PresentationDecision;
using rsx::lsfg::PresentationPlan;
using rsx::lsfg::kMaxContinuousIntervalMilliseconds;

constexpr uint64_t kSerialZero = 0;
constexpr uint64_t kUint64Max = UINT64_MAX;

int g_failures = 0;

void Check(bool condition, const std::string& name, const std::string& detail) {
  if (condition) {
    std::printf("PASS %s\n", name.c_str());
  } else {
    std::printf("FAIL %s: %s\n", name.c_str(), detail.c_str());
    ++g_failures;
  }
}

const char* Name(PresentationDecision decision) {
  switch (decision) {
    case PresentationDecision::kPassthrough:
      return "passthrough";
    case PresentationDecision::kCaptureWarmup:
      return "capture-warmup";
    case PresentationDecision::kGenerate:
      return "generate";
  }
  return "unknown";
}

// Describes a plan fully, so a mismatch reports the whole expectation.
std::string Describe(const PresentationPlan& plan) {
  return std::string(Name(plan.decision)) + " source=" + std::to_string(plan.source_index);
}

// One frame of a scenario: what the presenter reports and what the policy must answer.
struct Expectation {
  uint64_t serial;
  bool interpolation_allowed;
  uint64_t monotonic_ms;
  PresentationDecision decision;
};

void CheckSequence(const std::string& name, const FrameHistory& history,
                   const std::vector<Expectation>& expectations, bool check_alternation) {
  FrameHistory driven = history;
  uint32_t expected_source = 0;
  for (size_t i = 0; i < expectations.size(); ++i) {
    const Expectation& expectation = expectations[i];
    const PresentationPlan plan =
        driven.Observe(expectation.serial, expectation.interpolation_allowed, expectation.monotonic_ms);
    const std::string step = name + " step " + std::to_string(i);
    if (plan.decision != expectation.decision) {
      Check(false, step, "expected " + std::string(Name(expectation.decision)) + ", got " + Describe(plan));
      return;
    }
    if (check_alternation &&
        (expectation.decision == PresentationDecision::kCaptureWarmup ||
         expectation.decision == PresentationDecision::kGenerate)) {
      if (plan.source_index != expected_source) {
        Check(false, step, "expected source " + std::to_string(expected_source) + ", got " + Describe(plan));
        return;
      }
      expected_source ^= 1u;
    }
  }
  Check(true, name, "");
}

void ObserveAll(FrameHistory& history, uint64_t serial, bool interpolation_allowed, uint64_t ms,
                size_t count) {
  for (size_t i = 0; i < count; ++i) {
    history.Observe(serial + i, interpolation_allowed, ms + i);
  }
}

// --- Ordinary sequence -------------------------------------------------------------------------

void OrdinarySequence() {
  FrameHistory history;
  const std::vector<Expectation> expectations = {
      {1, true, 1000, PresentationDecision::kCaptureWarmup},
      {2, true, 1016, PresentationDecision::kCaptureWarmup},
      {3, true, 1032, PresentationDecision::kGenerate},
      {4, true, 1048, PresentationDecision::kGenerate},
      {5, true, 1064, PresentationDecision::kGenerate},
  };
  CheckSequence("ordinary: two warmup frames then generated", history, expectations, true);
}

void FirstFrameIsWarmup() {
  FrameHistory history;
  const PresentationPlan first = history.Observe(7, true, 500);
  Check(first.decision == PresentationDecision::kCaptureWarmup,
        "ordinary: the very first accepted frame never generates",
        Describe(first));
  Check(first.source_index == 0, "ordinary: first frame captures into source 0",
        Describe(first));
}

// --- Duplicates --------------------------------------------------------------------------------

void DuplicatesBypassWithoutResettingWarmup() {
  FrameHistory history;
  // Warmup is complete after serials 1 and 2; 3 and 4 generate.
  ObserveAll(history, 1, true, 1000, 2);
  const PresentationPlan generated = history.Observe(3, true, 1032);
  Check(generated.decision == PresentationDecision::kGenerate, "duplicates: precondition generate",
        Describe(generated));

  // The same guest refresh presented again for a UI pass and for a duplicate swap.
  const PresentationPlan first_bypass = history.Observe(3, true, 1033);
  const PresentationPlan second_bypass = history.Observe(3, true, 1040);
  Check(first_bypass.decision == PresentationDecision::kPassthrough &&
            second_bypass.decision == PresentationDecision::kPassthrough,
        "duplicates: repeated serial bypasses", Describe(first_bypass) + " / " + Describe(second_bypass));
  Check(history.warmup_remaining() == 0,
        "duplicates: a duplicate does not restart warmup",
        "remaining " + std::to_string(history.warmup_remaining()));

  // The next new frame must still generate: dropping warmup here would have forced two more seeds.
  const PresentationPlan after = history.Observe(4, true, 1048);
  Check(after.decision == PresentationDecision::kGenerate,
        "duplicates: the next new serial still generates", Describe(after));
}

void DuplicatesDoNotConsumeTheCaptureIndex() {
  FrameHistory history;
  ObserveAll(history, 1, true, 1000, 2);
  const PresentationPlan generated = history.Observe(3, true, 1032);
  history.Observe(3, true, 1033);
  const PresentationPlan next = history.Observe(4, true, 1048);
  Check(generated.decision == PresentationDecision::kGenerate && generated.source_index == 0 &&
            next.decision == PresentationDecision::kGenerate && next.source_index == 1,
        "duplicates: capture index alternates only on accepted frames",
        Describe(generated) + " / " + Describe(next));
}

void DuplicateDuringWarmupKeepsWarmup() {
  FrameHistory history;
  const PresentationPlan warm = history.Observe(1, true, 1000);
  const PresentationPlan duplicate = history.Observe(1, true, 1001);
  const PresentationPlan second = history.Observe(2, true, 1016);
  const PresentationPlan third = history.Observe(3, true, 1032);
  Check(warm.decision == PresentationDecision::kCaptureWarmup &&
            duplicate.decision == PresentationDecision::kPassthrough &&
            second.decision == PresentationDecision::kCaptureWarmup &&
            third.decision == PresentationDecision::kGenerate,
        "duplicates: a duplicate inside the warmup does not restart it",
        Describe(warm) + " / " + Describe(duplicate) + " / " + Describe(second) + " / " +
            Describe(third));
}

// --- Stale serials -----------------------------------------------------------------------------

void StaleSerialResetsWarmup() {
  FrameHistory history;
  ObserveAll(history, 10, true, 1000, 2);
  const PresentationPlan generated = history.Observe(12, true, 1032);
  const PresentationPlan stale = history.Observe(11, true, 1048);
  Check(generated.decision == PresentationDecision::kGenerate &&
            stale.decision == PresentationDecision::kPassthrough,
        "stale: a serial below the last accepted one passes through",
        Describe(generated) + " / " + Describe(stale));
  Check(history.warmup_remaining() == rsx::lsfg::kAcceptedFramesPerWarmup,
        "stale: conservatively restarts the warmup",
        "remaining " + std::to_string(history.warmup_remaining()));

  const PresentationPlan after = history.Observe(13, true, 1064);
  Check(after.decision == PresentationDecision::kCaptureWarmup,
        "stale: the next new frame warms again", Describe(after));
}

// --- UI and inactive frames --------------------------------------------------------------------

void DisallowedFramePassesThroughAndResets() {
  FrameHistory history;
  ObserveAll(history, 1, true, 1000, 2);
  const PresentationPlan generated = history.Observe(3, true, 1032);
  const PresentationPlan ui = history.Observe(3, false, 1033);
  Check(generated.decision == PresentationDecision::kGenerate &&
            ui.decision == PresentationDecision::kPassthrough,
        "ui: a disallowed frame of an accepted serial passes through", Describe(ui));
  Check(history.warmup_remaining() == rsx::lsfg::kAcceptedFramesPerWarmup,
        "ui: a disallowed frame restarts the warmup",
        "remaining " + std::to_string(history.warmup_remaining()));

  const PresentationPlan after = history.Observe(4, true, 1048);
  Check(after.decision == PresentationDecision::kCaptureWarmup,
        "ui: the following real frame warms again", Describe(after));
}

void DisallowedNewSerialDoesNotBecomeCurrent() {
  FrameHistory history;
  ObserveAll(history, 1, true, 1000, 2);
  history.Observe(3, true, 1032);
  // A UI paint of a frame the presenter never accepted as guest output.
  const PresentationPlan ui = history.Observe(9, false, 1040);
  const PresentationPlan same = history.Observe(3, true, 1041);
  Check(ui.decision == PresentationDecision::kPassthrough &&
            history.last_accepted_serial() == 3 && same.decision == PresentationDecision::kPassthrough,
        "ui: a disallowed frame does not take over the accepted serial",
        Describe(ui) + " / " + Describe(same) + " last=" + std::to_string(history.last_accepted_serial()));
}

// --- Zero serial -------------------------------------------------------------------------------

void ZeroSerialPassesThrough() {
  FrameHistory history;
  const PresentationPlan zero = history.Observe(kSerialZero, true, 1000);
  Check(zero.decision == PresentationDecision::kPassthrough,
        "zero: an inactive guest output passes through", Describe(zero));
  const PresentationPlan warm = history.Observe(1, true, 1016);
  Check(warm.decision == PresentationDecision::kCaptureWarmup, "zero: the next frame still warms",
        Describe(warm));
}

void ZeroSerialResetsWarmup() {
  FrameHistory history;
  ObserveAll(history, 1, true, 1000, 2);
  const PresentationPlan generated = history.Observe(3, true, 1032);
  const PresentationPlan zero = history.Observe(kSerialZero, true, 1048);
  Check(generated.decision == PresentationDecision::kGenerate &&
            zero.decision == PresentationDecision::kPassthrough &&
            history.warmup_remaining() == rsx::lsfg::kAcceptedFramesPerWarmup,
        "zero: an inactive frame restarts the warmup", Describe(zero));
}

// --- Gaps --------------------------------------------------------------------------------------

void GapBoundaryKeepsGenerating() {
  FrameHistory history;
  ObserveAll(history, 1, true, 0, 2);
  const uint64_t last_accepted_ms = 1;
  const PresentationPlan generated = history.Observe(3, true, last_accepted_ms);
  const PresentationPlan at_boundary =
      history.Observe(4, true, last_accepted_ms + kMaxContinuousIntervalMilliseconds);
  Check(generated.decision == PresentationDecision::kGenerate &&
            at_boundary.decision == PresentationDecision::kGenerate,
        "gap: exactly the maximum interval stays continuous", Describe(at_boundary));
}

void GapBeyondBoundaryRestartsWarmup() {
  FrameHistory history;
  ObserveAll(history, 1, true, 0, 2);
  const uint64_t last_accepted_ms = 1;
  const PresentationPlan generated = history.Observe(3, true, last_accepted_ms);
  const PresentationPlan beyond =
      history.Observe(4, true, last_accepted_ms + kMaxContinuousIntervalMilliseconds + 1);
  const PresentationPlan after = history.Observe(5, true, last_accepted_ms + 300);
  Check(generated.decision == PresentationDecision::kGenerate &&
            beyond.decision == PresentationDecision::kCaptureWarmup &&
            after.decision == PresentationDecision::kCaptureWarmup,
        "gap: one millisecond past the maximum restarts the warmup",
        Describe(beyond) + " / " + Describe(after));
}

void ClockRegressionRestartsWarmup() {
  FrameHistory history;
  ObserveAll(history, 1, true, 5000, 2);
  const PresentationPlan generated = history.Observe(3, true, 5032);
  const PresentationPlan regressed = history.Observe(4, true, 4000);
  const PresentationPlan after = history.Observe(5, true, 5016);
  Check(generated.decision == PresentationDecision::kGenerate &&
            regressed.decision == PresentationDecision::kCaptureWarmup &&
            after.decision == PresentationDecision::kCaptureWarmup,
        "clock: a regressing timestamp restarts the warmup without a huge gap interpretation",
        Describe(regressed) + " / " + Describe(after));
}

// --- Reset -------------------------------------------------------------------------------------

void ResetClearsContext() {
  FrameHistory history;
  ObserveAll(history, 1, true, 1000, 3);
  history.Reset();
  Check(!history.has_accepted_serial() &&
            history.warmup_remaining() == rsx::lsfg::kAcceptedFramesPerWarmup,
        "reset: clears the accepted serial and the warmup", "state after reset");
  // A serial that was already accepted is new again for the fresh backend context.
  const PresentationPlan first = history.Observe(3, true, 900);
  const PresentationPlan second = history.Observe(4, true, 916);
  const PresentationPlan third = history.Observe(5, true, 932);
  Check(first.decision == PresentationDecision::kCaptureWarmup &&
            second.decision == PresentationDecision::kCaptureWarmup &&
            third.decision == PresentationDecision::kGenerate,
        "reset: the warmup starts over and capture alternation from source 0",
        Describe(first) + " / " + Describe(second) + " / " + Describe(third));
  Check(first.source_index == 0, "reset: capture returns to source 0", Describe(first));
}

void ResetIsIdempotent() {
  FrameHistory history;
  history.Reset();
  history.Reset();
  const PresentationPlan first = history.Observe(1, true, 0);
  Check(first.decision == PresentationDecision::kCaptureWarmup && first.source_index == 0,
        "reset: repeated resets leave a fresh history", Describe(first));
}

// --- Large serials -----------------------------------------------------------------------------

void LargeSerialIsAccepted() {
  FrameHistory history;
  const PresentationPlan a = history.Observe(kUint64Max - 3, true, 1000);
  const PresentationPlan b = history.Observe(kUint64Max - 2, true, 1016);
  const PresentationPlan c = history.Observe(kUint64Max - 1, true, 1032);
  const PresentationPlan d = history.Observe(kUint64Max, true, 1048);
  const PresentationPlan duplicate = history.Observe(kUint64Max, true, 1049);
  Check(a.decision == PresentationDecision::kCaptureWarmup &&
            b.decision == PresentationDecision::kCaptureWarmup &&
            c.decision == PresentationDecision::kGenerate &&
            d.decision == PresentationDecision::kGenerate &&
            duplicate.decision == PresentationDecision::kPassthrough &&
            history.last_accepted_serial() == kUint64Max,
        "large: the maximum serial is a valid accepted serial",
        Describe(c) + " / " + Describe(d) + " / " + Describe(duplicate));
}

// --- The invariants the policy exists for -------------------------------------------------------

void WarmupIsTwoAcceptedFrames() {
  FrameHistory history;
  uint32_t warmups = 0;
  while (history.Observe(warmups + 1, true, warmups * 16).decision ==
         PresentationDecision::kCaptureWarmup) {
    ++warmups;
    if (warmups > 8) {
      Check(false, "invariant: warmup is two accepted frames", "warmup never ended");
      return;
    }
  }
  Check(warmups == rsx::lsfg::kAcceptedFramesPerWarmup, "invariant: warmup is two accepted frames",
        "counted " + std::to_string(warmups));
}

void AlternationIsIndependentOfPassthroughFrames() {
  FrameHistory history;
  const PresentationPlan first = history.Observe(1, true, 1000);
  history.Observe(2, false, 1016);   // UI pass between the two seeds
  const PresentationPlan second = history.Observe(3, true, 1032);
  Check(first.source_index == 0 && second.source_index == 1,
        "invariant: an interleaved UI pass does not disturb source alternation",
        Describe(first) + " / " + Describe(second));
}

}  // namespace

int main() {
  OrdinarySequence();
  FirstFrameIsWarmup();
  DuplicatesBypassWithoutResettingWarmup();
  DuplicatesDoNotConsumeTheCaptureIndex();
  DuplicateDuringWarmupKeepsWarmup();
  StaleSerialResetsWarmup();
  DisallowedFramePassesThroughAndResets();
  DisallowedNewSerialDoesNotBecomeCurrent();
  ZeroSerialPassesThrough();
  ZeroSerialResetsWarmup();
  GapBoundaryKeepsGenerating();
  GapBeyondBoundaryRestartsWarmup();
  ClockRegressionRestartsWarmup();
  ResetClearsContext();
  ResetIsIdempotent();
  LargeSerialIsAccepted();
  WarmupIsTwoAcceptedFrames();
  AlternationIsIndependentOfPassthroughFrames();
  std::printf("FAILURES %d\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
