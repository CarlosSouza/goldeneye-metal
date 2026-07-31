#include "ge_watchdog.h"

#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

#define CHECK_TRUE(expression)                                                            \
  do {                                                                                    \
    if (!(expression)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expression); \
      ++failures;                                                                         \
    }                                                                                     \
  } while (false)

using ge::watchdog::ClassifyProgress;
using ge::watchdog::EpisodeTracker;
using ge::watchdog::ObservationContext;
using ge::watchdog::ObservationSuppressed;
using ge::watchdog::ProgressSample;
using ge::watchdog::SessionState;
using ge::watchdog::ShutdownOwnership;
using ge::watchdog::WorkerLifecycle;

constexpr ProgressSample Sample(uint32_t guest_present, uint32_t cp_swap, uint32_t ring_read,
                                uint32_t ring_write, uint64_t generation = 1,
                                bool ring_valid = true, bool ring_pending = true) {
  return {true,      guest_present, cp_swap,    generation,
          ring_read, ring_write,    ring_valid, ring_pending};
}

void TestFirstSampleIsWarmup() {
  constexpr auto progress = ClassifyProgress({}, Sample(12, 30, 4, 8));
  static_assert(progress.warmup);
  static_assert(!progress.visual_stall_candidate());
  static_assert(!progress.no_guest_present_candidate(12, 8));
}

void TestIndependentProgressSources() {
  constexpr ProgressSample baseline = Sample(12, 30, 4, 8);

  constexpr auto guest_only = ClassifyProgress(baseline, Sample(13, 30, 4, 8));
  static_assert(!guest_only.warmup);
  static_assert(guest_only.guest_present_advanced);
  static_assert(!guest_only.cp_swap_advanced);
  static_assert(!guest_only.ring_read_advanced);
  static_assert(!guest_only.ring_write_advanced);
  static_assert(guest_only.visual_stall_candidate());

  constexpr auto cp_only = ClassifyProgress(baseline, Sample(12, 31, 4, 8));
  static_assert(!cp_only.guest_present_advanced);
  static_assert(cp_only.cp_swap_advanced);
  static_assert(!cp_only.ring_read_advanced);
  static_assert(!cp_only.ring_write_advanced);
  static_assert(!cp_only.visual_stall_candidate());

  constexpr auto consumer_only = ClassifyProgress(baseline, Sample(12, 30, 5, 8));
  static_assert(!consumer_only.guest_present_advanced);
  static_assert(!consumer_only.cp_swap_advanced);
  static_assert(consumer_only.ring_read_advanced);
  static_assert(!consumer_only.ring_write_advanced);
  static_assert(!consumer_only.visual_stall_candidate());

  constexpr auto producer_only = ClassifyProgress(baseline, Sample(13, 30, 4, 9));
  static_assert(producer_only.guest_present_advanced);
  static_assert(!producer_only.ring_read_advanced);
  static_assert(producer_only.ring_write_advanced);
  static_assert(producer_only.visual_stall_candidate());

  constexpr auto all_progress = ClassifyProgress(baseline, Sample(14, 33, 6, 9));
  static_assert(all_progress.guest_present_delta == 2);
  static_assert(all_progress.cp_swap_delta == 3);
  static_assert(!all_progress.visual_stall_candidate());
}

void TestCpProgressSuppressesFalseVisualStall() {
  constexpr ProgressSample previous = Sample(100, 200, 20, 20);
  constexpr auto progress = ClassifyProgress(previous, Sample(101, 201, 20, 20));
  static_assert(progress.guest_present_advanced);
  static_assert(progress.cp_swap_advanced);
  static_assert(!progress.ring_read_advanced);
  static_assert(!progress.ring_write_advanced);
  static_assert(!progress.visual_stall_candidate());
}

void TestNoPresentClassificationUsesGuestCounterOnly() {
  constexpr ProgressSample previous = Sample(100, 200, 20, 20);
  constexpr auto gpu_still_finishing = ClassifyProgress(previous, Sample(100, 201, 21, 20));
  static_assert(!gpu_still_finishing.guest_present_advanced);
  static_assert(gpu_still_finishing.cp_swap_advanced);
  static_assert(gpu_still_finishing.ring_read_advanced);
  static_assert(gpu_still_finishing.no_guest_present_candidate(100, 8));
  static_assert(!gpu_still_finishing.no_guest_present_candidate(7, 8));
}

void TestCounterWrapDeltas() {
  constexpr ProgressSample previous = Sample(UINT32_MAX, UINT32_MAX - 1, 7, 9);
  constexpr auto progress = ClassifyProgress(previous, Sample(1, 1, 7, 9));
  static_assert(progress.guest_present_delta == 2);
  static_assert(progress.cp_swap_delta == 3);
  static_assert(progress.guest_present_advanced);
  static_assert(progress.cp_swap_advanced);
}

void TestRingReconfigurationResetsClassification() {
  constexpr ProgressSample previous = Sample(100, 200, 20, 30, 4);
  constexpr auto progress = ClassifyProgress(previous, Sample(101, 200, 20, 30, 5));
  static_assert(progress.ring_reconfigured);
  static_assert(progress.ring_generation_changed);
  static_assert(!progress.ring_validity_changed);
  static_assert(!progress.ring_read_advanced);
  static_assert(!progress.ring_write_advanced);
  static_assert(!progress.visual_stall_candidate());
  static_assert(!progress.no_guest_present_candidate(101, 8));

  constexpr auto no_present = ClassifyProgress(previous, Sample(100, 200, 20, 30, 5));
  static_assert(no_present.ring_reconfigured);
  static_assert(no_present.no_guest_present_candidate(100, 8));
}

void TestPersistentlyInvalidRingAfterLiveRingIsDetected() {
  EpisodeTracker tracker(/*visual_stall_threshold=*/2, /*no_present_threshold=*/20,
                         /*minimum_present_count=*/8);

  auto update = tracker.Observe(Sample(8, 20, 4, 5));
  CHECK_TRUE(update.progress.warmup);

  // The valid-to-invalid transition is a grace boundary, not an incident.
  update = tracker.Observe(Sample(9, 20, 0, 0, 2, false, false));
  CHECK_TRUE(update.progress.ring_validity_changed);
  CHECK_TRUE(update.visual_stall_samples == 0);
  CHECK_TRUE(!update.log_visual_stall);

  update = tracker.Observe(Sample(10, 20, 0, 0, 2, false, false));
  CHECK_TRUE(update.visual_stall_started);
  CHECK_TRUE(update.visual_stall_samples == 1);

  // Reprogramming an already-invalid ring must not permanently hide the
  // incident if the guest continues presenting without CP progress.
  update = tracker.Observe(Sample(11, 20, 0, 0, 3, false, false));
  CHECK_TRUE(update.progress.ring_generation_changed);
  CHECK_TRUE(update.visual_stall_samples == 2);
  CHECK_TRUE(update.log_visual_stall);

  update = tracker.Observe(Sample(12, 21, 6, 6, 4));
  CHECK_TRUE(update.progress.ring_validity_changed);
  CHECK_TRUE(update.visual_stall_samples == 0);

  // Invalid rings seen only during startup remain ignored.
  EpisodeTracker boot_tracker(/*visual_stall_threshold=*/2, /*no_present_threshold=*/20,
                              /*minimum_present_count=*/8);
  for (uint32_t present = 8; present < 13; ++present) {
    update = boot_tracker.Observe(Sample(present, 20, 0, 0, 1, false, false));
    CHECK_TRUE(update.visual_stall_samples == 0);
    CHECK_TRUE(!update.log_visual_stall);
  }
}

void TestEpisodeThresholdsAndRearming() {
  EpisodeTracker tracker(/*visual_stall_threshold=*/2, /*no_present_threshold=*/3,
                         /*minimum_present_count=*/8);

  auto update = tracker.Observe(Sample(8, 20, 4, 5));
  CHECK_TRUE(update.progress.warmup);

  update = tracker.Observe(Sample(9, 20, 4, 6));
  CHECK_TRUE(update.visual_stall_started);
  CHECK_TRUE(update.visual_stall_samples == 1);
  CHECK_TRUE(!update.log_visual_stall);

  update = tracker.Observe(Sample(10, 20, 4, 7));
  CHECK_TRUE(update.visual_stall_samples == 2);
  CHECK_TRUE(update.log_visual_stall);

  update = tracker.Observe(Sample(11, 20, 4, 8));
  CHECK_TRUE(update.visual_stall_samples == 3);
  CHECK_TRUE(!update.log_visual_stall);

  // CP progress recovers and rearms the one-shot visual episode.
  update = tracker.Observe(Sample(12, 21, 4, 8));
  CHECK_TRUE(update.visual_stall_samples == 0);
  update = tracker.Observe(Sample(13, 21, 4, 8));
  CHECK_TRUE(update.visual_stall_started);
  update = tracker.Observe(Sample(14, 21, 4, 8));
  CHECK_TRUE(update.log_visual_stall);

  update = tracker.Observe(Sample(14, 22, 5, 8));
  CHECK_TRUE(update.no_present_started);
  CHECK_TRUE(update.no_present_samples == 1);
  update = tracker.Observe(Sample(14, 22, 5, 8));
  CHECK_TRUE(update.no_present_samples == 2);
  update = tracker.Observe(Sample(14, 22, 5, 8));
  CHECK_TRUE(update.log_no_present_stall);
  update = tracker.Observe(Sample(15, 22, 5, 8));
  CHECK_TRUE(update.no_present_samples == 0);

  // Ring reconfiguration clears the ring-dependent visual episode, but must
  // not let repeated GPU recovery activity mask a title-side no-present stall.
  update = tracker.Observe(Sample(15, 22, 5, 8));
  CHECK_TRUE(update.no_present_samples == 1);
  update = tracker.Observe(Sample(15, 22, 5, 8, 2));
  CHECK_TRUE(update.progress.ring_reconfigured);
  CHECK_TRUE(update.visual_stall_samples == 0);
  CHECK_TRUE(update.no_present_samples == 2);
  update = tracker.Observe(Sample(15, 22, 5, 8, 3));
  CHECK_TRUE(update.progress.ring_reconfigured);
  CHECK_TRUE(update.no_present_samples == 3);
  CHECK_TRUE(update.log_no_present_stall);
}

void TestSessionResetInvalidatesPublishedGuestState() {
  SessionState session;
  session.PublishDebugPoll(0x82001000u, 0x82002000u);
  session.PublishPresentCpSwap(41);
  CHECK_TRUE(session.AdvanceGuestPresent() == 1);
  CHECK_TRUE(session.AdvanceGuestPresent() == 2);

  auto snapshot = session.Load();
  CHECK_TRUE(snapshot.guest_present == 2);
  CHECK_TRUE(snapshot.present_cp_swap == 41);
  CHECK_TRUE(snapshot.debug_poll_count == 1);
  CHECK_TRUE(snapshot.guest_device == 0x82001000u);
  CHECK_TRUE(snapshot.guest_id_block == 0x82002000u);

  // StartWatchdog performs this reset before constructing the replacement
  // worker. No pointer or progress value from the old guest may survive.
  session.Reset();
  snapshot = session.Load();
  CHECK_TRUE(snapshot.guest_present == 0);
  CHECK_TRUE(snapshot.present_cp_swap == 0);
  CHECK_TRUE(snapshot.debug_poll_count == 0);
  CHECK_TRUE(snapshot.guest_device == 0);
  CHECK_TRUE(snapshot.guest_id_block == 0);

  session.PublishDebugPoll(0x83001000u, 0x83002000u);
  snapshot = session.Load();
  CHECK_TRUE(snapshot.guest_device == 0x83001000u);
  CHECK_TRUE(snapshot.guest_id_block == 0x83002000u);
}

void TestDeliberateInactiveObservationGate() {
  static_assert(!ObservationSuppressed({}));
  static_assert(ObservationSuppressed({.host_pause_active = true}));
  static_assert(!ObservationSuppressed(
      {.host_input_available = false, .focused = false, .input_active = false}));
  static_assert(
      ObservationSuppressed({.host_input_available = true, .focused = false, .input_active = true}));
  static_assert(
      ObservationSuppressed({.host_input_available = true, .focused = true, .input_active = false}));
  static_assert(!ObservationSuppressed(
      {.host_input_available = true, .focused = true, .input_active = true}));
}

void TestPauseSuppressionResetsAndRearmsEpisodes() {
  EpisodeTracker tracker(/*visual_stall_threshold=*/2, /*no_present_threshold=*/2,
                         /*minimum_present_count=*/8);

  auto update = tracker.Observe(Sample(8, 20, 4, 5));
  CHECK_TRUE(update.progress.warmup);
  update = tracker.Observe(Sample(8, 20, 4, 5));
  CHECK_TRUE(update.no_present_samples == 1);

  update = tracker.Observe({}, true);
  CHECK_TRUE(update.observation_suppressed);
  CHECK_TRUE(update.no_present_samples == 0);
  CHECK_TRUE(update.visual_stall_samples == 0);
  CHECK_TRUE(!update.log_no_present_stall);
  CHECK_TRUE(!update.log_visual_stall);
  update = tracker.Observe({}, true);
  CHECK_TRUE(update.observation_suppressed);
  CHECK_TRUE(!update.log_no_present_stall);

  // Resuming starts from a fresh baseline. Time spent paused or inactive does
  // not carry an almost-complete episode into the first active sample.
  update = tracker.Observe(Sample(8, 20, 4, 5));
  CHECK_TRUE(update.progress.warmup);
  CHECK_TRUE(update.no_present_samples == 0);
  update = tracker.Observe(Sample(8, 20, 4, 5));
  CHECK_TRUE(update.no_present_samples == 1);
  CHECK_TRUE(!update.log_no_present_stall);
  update = tracker.Observe(Sample(8, 20, 4, 5));
  CHECK_TRUE(update.no_present_samples == 2);
  CHECK_TRUE(update.log_no_present_stall);
}

void TestWorkerLifecycleOwnershipAndRestart() {
  WorkerLifecycle lifecycle;
  CHECK_TRUE(lifecycle.BeginShutdown() == ShutdownOwnership::kNotStarted);
  CHECK_TRUE(lifecycle.TryStart());
  CHECK_TRUE(lifecycle.started());
  CHECK_TRUE(!lifecycle.stop_requested());
  CHECK_TRUE(!lifecycle.TryStart());

  CHECK_TRUE(lifecycle.BeginShutdown() == ShutdownOwnership::kOwnShutdown);
  CHECK_TRUE(lifecycle.stopping());
  CHECK_TRUE(lifecycle.stop_requested());
  CHECK_TRUE(lifecycle.BeginShutdown() == ShutdownOwnership::kWaitForOwner);
  CHECK_TRUE(!lifecycle.TryStart());

  lifecycle.FinishShutdown();
  CHECK_TRUE(!lifecycle.started());
  CHECK_TRUE(!lifecycle.stopping());
  CHECK_TRUE(!lifecycle.stop_requested());
  CHECK_TRUE(lifecycle.TryStart());
  lifecycle.AbortStart();
  CHECK_TRUE(!lifecycle.started());
  CHECK_TRUE(!lifecycle.stop_requested());
}

}  // namespace

int main() {
  TestFirstSampleIsWarmup();
  TestIndependentProgressSources();
  TestCpProgressSuppressesFalseVisualStall();
  TestNoPresentClassificationUsesGuestCounterOnly();
  TestCounterWrapDeltas();
  TestRingReconfigurationResetsClassification();
  TestPersistentlyInvalidRingAfterLiveRingIsDetected();
  TestEpisodeThresholdsAndRearming();
  TestSessionResetInvalidatesPublishedGuestState();
  TestDeliberateInactiveObservationGate();
  TestPauseSuppressionResetsAndRearmsEpisodes();
  TestWorkerLifecycleOwnershipAndRestart();
  if (failures != 0) {
    std::fprintf(stderr, "%d watchdog test(s) failed\n", failures);
    return 1;
  }
  std::puts("watchdog tests passed");
  return 0;
}
