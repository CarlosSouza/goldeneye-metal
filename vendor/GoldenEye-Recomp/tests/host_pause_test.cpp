#include "ge_host_pause.h"
#include "ge_controller_shortcut.h"

#include <array>
#include <climits>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

namespace {

int failures = 0;

#define CHECK_TRUE(expression)                                                            \
  do {                                                                                    \
    if (!(expression)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expression); \
      ++failures;                                                                         \
    }                                                                                     \
  } while (false)

struct RetailPause {
  uint32_t value = 0;
  bool accept_writes = true;
  int set_host_count = 0;
  int set_zero_count = 0;
  std::optional<uint32_t> value_after_next_write;

  uint32_t Query() const { return value; }
  void Set(uint32_t new_value) {
    if (new_value == ge::host_pause::kHostPauseToken) {
      ++set_host_count;
    } else if (new_value == 0) {
      ++set_zero_count;
    }
    if (!accept_writes) {
      return;
    }
    value = value_after_next_write.value_or(new_value);
    value_after_next_write.reset();
  }
};

ge::host_pause::ProcessResult Process(ge::host_pause::State& state, bool eligible,
                                      RetailPause& retail) {
  return state.Process(
      eligible, [&] { return retail.Query(); }, [&](uint32_t value) { retail.Set(value); });
}

void TestLocalMissionEligibility() {
  using ge::host_pause::IsEligibleLocalMission;
  CHECK_TRUE(IsEligibleLocalMission(1, 1, false));
  CHECK_TRUE(IsEligibleLocalMission(89, 4, false));
  CHECK_TRUE(!IsEligibleLocalMission(0, 1, false));
  CHECK_TRUE(!IsEligibleLocalMission(90, 1, false));
  CHECK_TRUE(!IsEligibleLocalMission(1, 0, false));
  CHECK_TRUE(!IsEligibleLocalMission(1, 5, false));
  CHECK_TRUE(!IsEligibleLocalMission(1, 1, true));
  CHECK_TRUE(!IsEligibleLocalMission(89, 4, true));
}

void TestAcquireReleaseAndIdempotence() {
  ge::host_pause::State state;
  RetailPause retail;

  CHECK_TRUE(state.RequestPaused(true));
  CHECK_TRUE(!state.RequestPaused(true));
  auto result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kAcquire);
  CHECK_TRUE(result.action_succeeded);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);
  CHECK_TRUE(retail.set_host_count == 1);
  CHECK_TRUE(state.GetSnapshot().gameplay_paused);

  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kNone);
  CHECK_TRUE(retail.set_host_count == 1);

  CHECK_TRUE(state.RequestPaused(false));
  CHECK_TRUE(!state.RequestPaused(false));
  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kRelease);
  CHECK_TRUE(result.action_succeeded);
  CHECK_TRUE(retail.value == 0);
  CHECK_TRUE(retail.set_zero_count == 1);
  CHECK_TRUE(result.input_resume_pulse);
  CHECK_TRUE(state.GetSnapshot().request_applied);

  CHECK_TRUE(state.TryProcessIdle(&result));
  CHECK_TRUE(!result.input_resume_pulse);
}

void TestPreexistingRetailPauseIsRestored() {
  ge::host_pause::State state;
  RetailPause retail{.value = 1};

  state.RequestPaused(true);
  auto result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kAcquire);
  CHECK_TRUE(result.host_owned);
  CHECK_TRUE(result.gameplay_paused);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);
  CHECK_TRUE(retail.set_host_count == 1);

  state.RequestPaused(false);
  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kRelease);
  CHECK_TRUE(retail.value == 1);
  CHECK_TRUE(retail.set_zero_count == 0);
  CHECK_TRUE(result.input_resume_pulse);
}

void TestLaterRetailIntentIsPreserved() {
  ge::host_pause::State state;
  RetailPause retail;

  state.RequestPaused(true);
  auto result = Process(state, true, retail);
  CHECK_TRUE(result.host_owned);

  // GoldenEye asserts its own boolean pause after host acquisition. The strong
  // setter records that intent but keeps the host token continuously asserted.
  retail.value = state.FilterRetailWrite(1);
  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kNone);
  CHECK_TRUE(result.host_owned);
  CHECK_TRUE(result.gameplay_paused);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);

  state.RequestPaused(false);
  result = Process(state, true, retail);
  CHECK_TRUE(retail.value == 1);
  CHECK_TRUE(retail.set_zero_count == 0);
  CHECK_TRUE(result.input_resume_pulse);
}

void TestRetailWritesStayPausedAndEligibilityLossIsSafe() {
  ge::host_pause::State state;
  RetailPause retail;

  state.RequestPaused(true);
  Process(state, true, retail);
  retail.value = state.FilterRetailWrite(1);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);
  retail.value = state.FilterRetailWrite(0);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);
  Process(state, true, retail);
  auto result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kNone);
  CHECK_TRUE(result.host_owned);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);

  result = Process(state, false, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kRelease);
  CHECK_TRUE(!result.host_owned);
  CHECK_TRUE(retail.value == 0);

  ge::host_pause::State foreign_state;
  RetailPause foreign_retail;
  foreign_state.RequestPaused(true);
  Process(foreign_state, true, foreign_retail);
  foreign_retail.value = foreign_state.FilterRetailWrite(1);
  result = Process(foreign_state, false, foreign_retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kRelease);
  CHECK_TRUE(!result.host_owned);
  CHECK_TRUE(foreign_retail.value == 1);
}

void TestUnavailableRapidCancellationAndResumePulse() {
  ge::host_pause::State state;
  RetailPause retail;

  state.RequestPaused(true);
  auto result = Process(state, false, retail);
  CHECK_TRUE(result.request_applied);
  const auto unavailable = state.GetSnapshot();
  CHECK_TRUE(!unavailable.available);
  CHECK_TRUE(unavailable.generation == unavailable.applied_generation);
  CHECK_TRUE(retail.value == 0);

  // Eligibility may appear without a new UI request (for example the mission
  // finishes loading while settings is already open). The still-true request
  // must acquire even though its earlier unavailable poll was acknowledged.
  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kAcquire);
  CHECK_TRUE(result.host_owned);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);

  state.RequestPaused(false);
  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kRelease);
  CHECK_TRUE(result.input_resume_pulse);
  CHECK_TRUE(state.TryProcessIdle(&result));
  CHECK_TRUE(!result.input_resume_pulse);

  state.RequestPaused(true);
  state.RequestPaused(false);
  state.RequestPaused(true);
  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kAcquire);
  CHECK_TRUE(!result.input_resume_pulse);
}

void TestFailedWritesRetryAndReleaseRace() {
  ge::host_pause::State state;
  RetailPause retail{.accept_writes = false};

  state.RequestPaused(true);
  auto result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kAcquire);
  CHECK_TRUE(!result.action_succeeded);
  CHECK_TRUE(!result.request_applied);

  retail.accept_writes = true;
  result = Process(state, true, retail);
  CHECK_TRUE(result.action_succeeded);
  CHECK_TRUE(result.host_owned);

  state.RequestPaused(false);
  retail.accept_writes = false;
  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kRelease);
  CHECK_TRUE(!result.action_succeeded);
  CHECK_TRUE(!result.input_resume_pulse);

  // A same-poll retail assertion wins over the host's zero write. Treat it as
  // successful ownership transfer and preserve the resulting retail pause.
  retail.accept_writes = true;
  retail.value_after_next_write = 1;
  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kRelinquish);
  CHECK_TRUE(result.action_succeeded);
  CHECK_TRUE(!result.host_owned);
  CHECK_TRUE(retail.value == 1);
  CHECK_TRUE(result.input_resume_pulse);
}

void TestRetailPauseWriteShadow() {
  ge::host_pause::State state;
  RetailPause retail;

  // Without host ownership, retail writes pass through unchanged.
  CHECK_TRUE(state.FilterRetailWrite(1) == 1);
  CHECK_TRUE(state.FilterRetailWrite(0) == 0);

  state.RequestPaused(true);
  auto result = Process(state, true, retail);
  CHECK_TRUE(result.host_owned);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);

  // Both sides of a retail 1 -> 0 transition are shadowed without exposing a
  // running frame. The latest retail intent is restored when the host closes.
  retail.value = state.FilterRetailWrite(1);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);
  retail.value = state.FilterRetailWrite(0);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);
  state.RequestPaused(false);
  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kRelease);
  CHECK_TRUE(retail.value == 0);
}

void TestResumeInputLatch() {
  ge::host_pause::ResumeInputLatch latch;
  ge::host_pause::InputSample held{.buttons = 0x2000};
  latch.Arm(held);
  CHECK_TRUE(latch.ShouldSuppress(held));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress(held));
  CHECK_TRUE(latch.ShouldSuppress({}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({}));
  CHECK_TRUE(!latch.active());
  CHECK_TRUE(!latch.ShouldSuppress(held));

  // Input that first appears after arming (for example a mouse button reaching
  // the guest driver after the host UI closes) resets the neutral sequence.
  latch.Arm({});
  CHECK_TRUE(latch.ShouldSuppress({}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({.right_trigger = UINT8_MAX}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({}));
  CHECK_TRUE(!latch.active());

  ge::host_pause::InputSample analog{
      .left_trigger = ge::host_pause::ResumeInputLatch::kTriggerThreshold,
      .thumb_lx = 20000,
  };
  latch.Arm(analog);
  CHECK_TRUE(latch.ShouldSuppress(analog));
  CHECK_TRUE(latch.ShouldSuppress({.thumb_lx = 5000}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({.thumb_lx = 5000}));
  CHECK_TRUE(!latch.active());

  // Match Dear ImGui's strict 20% navigation boundary in both directions: the
  // exact deadzone is neutral, while the first raw value above it is not.
  constexpr int16_t deadzone = ge::host_pause::ResumeInputLatch::kStickDeadzone;
  latch.Arm({.thumb_ly = deadzone});
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = deadzone}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = deadzone}));
  CHECK_TRUE(!latch.active());

  latch.Arm({.thumb_ly = static_cast<int16_t>(deadzone + 1)});
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = static_cast<int16_t>(deadzone + 1)}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = deadzone}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = deadzone}));
  CHECK_TRUE(!latch.active());

  latch.Arm({.thumb_ly = static_cast<int16_t>(-deadzone)});
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = static_cast<int16_t>(-deadzone)}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = static_cast<int16_t>(-deadzone)}));
  CHECK_TRUE(!latch.active());

  latch.Arm({.thumb_ly = static_cast<int16_t>(-deadzone - 1)});
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = static_cast<int16_t>(-deadzone - 1)}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = static_cast<int16_t>(-deadzone)}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = static_cast<int16_t>(-deadzone)}));
  CHECK_TRUE(!latch.active());

  // Partial release is not enough; all controls must be neutral for two polls.
  latch.Arm({.buttons = 0x3000});
  CHECK_TRUE(latch.ShouldSuppress({.buttons = 0x3000}));
  CHECK_TRUE(latch.ShouldSuppress({.buttons = 0x1000}));
  CHECK_TRUE(latch.ShouldSuppress({}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({}));
  CHECK_TRUE(!latch.active());

  // Re-arming an active latch starts a fresh neutral sequence.
  latch.Arm({});
  CHECK_TRUE(latch.ShouldSuppress({}));
  latch.Arm(held);
  CHECK_TRUE(latch.ShouldSuppress(held));
  CHECK_TRUE(latch.active());

  latch.Arm(held);
  for (uint32_t poll = 0; poll < ge::host_pause::ResumeInputLatch::kMaximumSuppressedPolls;
       ++poll) {
    CHECK_TRUE(latch.ShouldSuppress(held));
  }
  CHECK_TRUE(!latch.active());
  CHECK_TRUE(!latch.ShouldSuppress(held));
}

void TestIndependentMultiplayerResumeLatches() {
  std::array<ge::host_pause::ResumeInputLatch, 4> latches;
  std::array<ge::host_pause::InputSample, 4> samples = {};
  samples[1].thumb_lx = INT16_MAX;  // drifting/held player 2

  for (size_t player = 0; player < latches.size(); ++player) {
    latches[player].Arm(samples[player]);
    CHECK_TRUE(latches[player].ShouldSuppress(samples[player]));
  }

  // Neutral players finish their two-poll transition independently. Player 2
  // remains latched instead of freezing all four guest ports.
  for (size_t player : {0u, 2u, 3u}) {
    CHECK_TRUE(latches[player].ShouldSuppress(samples[player]));
    CHECK_TRUE(!latches[player].active());
  }
  CHECK_TRUE(latches[1].ShouldSuppress(samples[1]));
  CHECK_TRUE(latches[1].active());

  samples[1] = {};
  CHECK_TRUE(latches[1].ShouldSuppress(samples[1]));
  CHECK_TRUE(latches[1].active());
  CHECK_TRUE(latches[1].ShouldSuppress(samples[1]));
  CHECK_TRUE(!latches[1].active());
}

void TestControllerHostSettingsShortcut() {
  using ge::controller_shortcut::HoldTracker;
  using ge::controller_shortcut::kButtonChord;
  using ge::controller_shortcut::kHoldDurationMs;
  using ge::controller_shortcut::kLeftStickButton;
  using ge::controller_shortcut::kRightStickButton;

  std::array<ge::controller_shortcut::ControllerSample,
             ge::controller_shortcut::kControllerSlotCount>
      controllers = {};
  for (size_t slot = 0; slot < controllers.size(); ++slot) {
    controllers[slot].device_id = slot + 1;
  }
  HoldTracker tracker;

  // Normal L3/R3 presses, a short chord, and buttons split across controllers
  // remain ordinary gameplay input.
  controllers[0].buttons = kLeftStickButton;
  CHECK_TRUE(!tracker.Observe(controllers, 0));
  controllers[0].buttons = kRightStickButton;
  CHECK_TRUE(!tracker.Observe(controllers, 100));
  controllers[0].buttons = kButtonChord;
  CHECK_TRUE(!tracker.Observe(controllers, 200));
  CHECK_TRUE(!tracker.Observe(controllers, 200 + kHoldDurationMs - 1));
  for (auto& controller : controllers) {
    controller.buttons = 0;
  }
  CHECK_TRUE(!tracker.Observe(controllers, 200 + kHoldDurationMs));
  controllers[0].buttons = kLeftStickButton;
  controllers[1].buttons = kRightStickButton;
  CHECK_TRUE(!tracker.Observe(controllers, 2000));
  CHECK_TRUE(!tracker.Observe(controllers, 3000));

  // One controller must hold both buttons continuously for the full duration.
  for (auto& controller : controllers) {
    controller.buttons = 0;
  }
  CHECK_TRUE(!tracker.Observe(controllers, 3100));
  controllers[2].buttons = kButtonChord;
  CHECK_TRUE(!tracker.Observe(controllers, 4000));
  CHECK_TRUE(!tracker.Observe(controllers, 4000 + kHoldDurationMs - 1));
  const auto opened = tracker.Observe(controllers, 4000 + kHoldDurationMs);
  CHECK_TRUE(opened && *opened == 2);
  CHECK_TRUE(!tracker.armed());

  // A long hold fires once. Even moving the held chord directly to another
  // controller cannot close the menu until every controller has released it.
  CHECK_TRUE(!tracker.Observe(controllers, 6000));
  controllers[2].buttons = 0;
  controllers[3].buttons = kButtonChord;
  CHECK_TRUE(!tracker.Observe(controllers, 7000));
  CHECK_TRUE(!tracker.Observe(controllers, 7000 + kHoldDurationMs));
  for (auto& controller : controllers) {
    controller.buttons = 0;
  }
  CHECK_TRUE(!tracker.Observe(controllers, 8000));
  CHECK_TRUE(tracker.armed());

  controllers[3].buttons = kButtonChord;
  CHECK_TRUE(!tracker.Observe(controllers, 9000));
  const auto closed = tracker.Observe(controllers, 9000 + kHoldDurationMs);
  CHECK_TRUE(closed && *closed == 3);

  // A replacement pad in the same guest slot starts a new hold interval even
  // if both physical devices happened to have the chord down.
  HoldTracker replacement_tracker;
  for (auto& controller : controllers) {
    controller.buttons = 0;
  }
  controllers[0].buttons = kButtonChord;
  CHECK_TRUE(!replacement_tracker.Observe(controllers, 10000));
  controllers[0].device_id = 99;
  CHECK_TRUE(!replacement_tracker.Observe(controllers, 10000 + kHoldDurationMs - 1));
  CHECK_TRUE(!replacement_tracker.Observe(controllers, 10000 + (2 * kHoldDurationMs) - 2));
  const auto replacement_completed =
      replacement_tracker.Observe(controllers, 10000 + (2 * kHoldDurationMs) - 1);
  CHECK_TRUE(replacement_completed && *replacement_completed == 0);

  // A backward clock sample restarts the continuous-hold interval safely.
  HoldTracker regressed_clock_tracker;
  for (auto& controller : controllers) {
    controller.buttons = 0;
  }
  controllers[1].buttons = kButtonChord;
  CHECK_TRUE(!regressed_clock_tracker.Observe(controllers, 1000));
  CHECK_TRUE(!regressed_clock_tracker.Observe(controllers, 900));
  CHECK_TRUE(!regressed_clock_tracker.Observe(controllers, 900 + kHoldDurationMs - 1));
  const auto after_regression = regressed_clock_tracker.Observe(controllers, 900 + kHoldDurationMs);
  CHECK_TRUE(after_regression && *after_regression == 1);

  ge::controller_shortcut::ToggleHandlerRegistry registry;
  uint32_t requested_slot = UINT32_MAX;
  registry.Set([&requested_slot](uint32_t slot) { requested_slot = slot; });
  CHECK_TRUE(registry.Request(2));
  CHECK_TRUE(requested_slot == 2);
  registry.Clear();
  CHECK_TRUE(!registry.Request(1));
  CHECK_TRUE(requested_slot == 2);
}

void TestRetryLogGate() {
  ge::host_pause::detail::RetryLogGate gate;
  ge::host_pause::ProcessResult failed_acquire{
      .action = ge::host_pause::Action::kAcquire,
      .action_succeeded = false,
      .generation = 7,
  };

  auto decision = gate.Observe(failed_acquire);
  CHECK_TRUE(decision.warn);
  CHECK_TRUE(!decision.recovered);
  CHECK_TRUE(decision.failed_attempts == 1);

  decision = gate.Observe(failed_acquire);
  CHECK_TRUE(!decision.warn);
  CHECK_TRUE(!decision.recovered);
  CHECK_TRUE(decision.failed_attempts == 2);

  // A newer idle/canceled request must discard the old failure episode without
  // claiming that a later, unrelated success recovered it.
  decision = gate.Observe({.request_applied = true, .generation = 8});
  CHECK_TRUE(!decision.warn);
  CHECK_TRUE(!decision.recovered);
  CHECK_TRUE(!gate.active());

  decision = gate.Observe({
      .action = ge::host_pause::Action::kAcquire,
      .action_succeeded = true,
      .generation = 9,
  });
  CHECK_TRUE(!decision.recovered);

  ge::host_pause::ProcessResult failed_release{
      .action = ge::host_pause::Action::kRelease,
      .action_succeeded = false,
      .generation = 10,
  };
  CHECK_TRUE(gate.Observe(failed_release).warn);
  CHECK_TRUE(!gate.Observe(failed_release).warn);
  decision = gate.Observe({
      .action = ge::host_pause::Action::kRelease,
      .action_succeeded = true,
      .generation = 10,
  });
  CHECK_TRUE(decision.recovered);
  CHECK_TRUE(decision.failed_attempts == 2);
  CHECK_TRUE(!gate.active());

  // A different action under the same generation is an ownership transition,
  // not recovery of the failed action.
  CHECK_TRUE(gate.Observe(failed_acquire).warn);
  decision = gate.Observe({
      .action = ge::host_pause::Action::kRelinquish,
      .action_succeeded = true,
      .generation = 7,
  });
  CHECK_TRUE(!decision.recovered);
  CHECK_TRUE(!gate.active());
}

ge::host_pause::detail::LiveTestObservation RunningLiveObservation(
    uint64_t monotonic_ms, uint32_t guest_frame, uint32_t present) {
  return {
      .monotonic_ms = monotonic_ms,
      .dam_gameplay_ready = true,
      .ui_open = false,
      .world_valid = true,
      .ammo_valid = true,
      .input_neutral = true,
      .player = 0x83001000u,
      .coordinates = 0x83002000u,
      .guest_frame = guest_frame,
      .present = present,
      .pause_value = 0,
      .position_x = 10.0f,
      .position_y = 20.0f,
      .position_z = 30.0f,
      .camera_yaw = 0.25f,
      .camera_pitch = -0.1f,
      .weapon = 3,
      .ammo = 7,
      .pause = {.requested = false,
                .request_applied = true,
                .available = false,
                .gameplay_paused = false,
                .host_owned = false,
                .generation = 0,
                .applied_generation = 0},
  };
}

ge::host_pause::detail::LiveTestObservation PausedLiveObservation(
    uint64_t monotonic_ms, uint32_t guest_frame, uint32_t present) {
  auto observation =
      RunningLiveObservation(monotonic_ms, guest_frame, present);
  observation.ui_open = true;
  observation.pause_value = ge::host_pause::kHostPauseToken;
  observation.pause = {
      .requested = true,
      .request_applied = true,
      .available = true,
      .gameplay_paused = true,
      .host_owned = true,
      .generation = 1,
      .applied_generation = 1,
  };
  return observation;
}

void ReachLiveTestPauseObservation(
    ge::host_pause::detail::LiveTestGate& gate) {
  using ge::host_pause::detail::LiveTestRequest;
  CHECK_TRUE(gate.Observe(RunningLiveObservation(0, 100, 200)).request ==
             LiveTestRequest::kNone);
  CHECK_TRUE(gate.Observe(RunningLiveObservation(500, 101, 201)).request ==
             LiveTestRequest::kOpenHostSettings);
  CHECK_TRUE(gate.Observe(PausedLiveObservation(600, 102, 202)).request ==
             LiveTestRequest::kNone);
}

void ReachLiveTestResumeWait(ge::host_pause::detail::LiveTestGate& gate) {
  using ge::host_pause::detail::LiveTestRequest;
  ReachLiveTestPauseObservation(gate);
  CHECK_TRUE(gate.Observe(PausedLiveObservation(1100, 103, 203)).request ==
             LiveTestRequest::kNone);
  const auto frozen = gate.Observe(PausedLiveObservation(1600, 104, 204));
  CHECK_TRUE(frozen.frozen_proven);
  CHECK_TRUE(frozen.request == LiveTestRequest::kCloseHostSettings);
}

void TestLiveHostPauseGateSuccess() {
  using ge::host_pause::detail::LiveTestPhase;
  ge::host_pause::detail::LiveTestGate gate;
  ReachLiveTestResumeWait(gate);
  CHECK_TRUE(gate.phase() == LiveTestPhase::kWaitingForResume);
  CHECK_TRUE(gate.proof().open_generation == 1);
  CHECK_TRUE(gate.proof().frozen_samples == 3);
  CHECK_TRUE(gate.proof().frozen_duration_ms == 1000);
  CHECK_TRUE(gate.proof().paused_frame_delta == 2);
  CHECK_TRUE(gate.proof().paused_present_delta == 2);

  auto resumed = RunningLiveObservation(1700, 105, 205);
  resumed.pause.generation = 2;
  resumed.pause.applied_generation = 2;
  CHECK_TRUE(!gate.Observe(resumed).completed);
  resumed = RunningLiveObservation(2200, 106, 206);
  resumed.pause.generation = 2;
  resumed.pause.applied_generation = 2;
  const auto complete = gate.Observe(resumed);
  CHECK_TRUE(complete.completed);
  CHECK_TRUE(gate.phase() == LiveTestPhase::kComplete);
  CHECK_TRUE(gate.proof().resume_generation == 2);
  CHECK_TRUE(gate.proof().resumed_samples == 2);
  CHECK_TRUE(gate.proof().resumed_duration_ms == 500);
  CHECK_TRUE(gate.proof().resumed_frame_delta == 1);
  CHECK_TRUE(gate.proof().resumed_present_delta == 1);
  CHECK_TRUE(gate.Observe(resumed).completed);
}

void TestLiveHostPauseGateFailsClosed() {
  using ge::host_pause::detail::LiveTestPhase;

  {
    ge::host_pause::detail::LiveTestGate gate;
    ReachLiveTestPauseObservation(gate);
    auto moved = PausedLiveObservation(1100, 103, 203);
    moved.position_x += 0.01f;
    const auto result = gate.Observe(moved);
    CHECK_TRUE(result.failure_reason != nullptr);
    CHECK_TRUE(std::string_view(result.failure_reason) ==
               "world-advanced-while-paused");
    CHECK_TRUE(gate.phase() == LiveTestPhase::kFailed);
  }

  {
    ge::host_pause::detail::LiveTestGate gate;
    ReachLiveTestPauseObservation(gate);
    CHECK_TRUE(!gate.Observe(PausedLiveObservation(1100, 103, 202))
                    .failure_reason);
    const auto result = gate.Observe(PausedLiveObservation(1600, 104, 202));
    CHECK_TRUE(result.failure_reason != nullptr);
    CHECK_TRUE(std::string_view(result.failure_reason) ==
               "paused-presentation-stalled");
  }

  {
    ge::host_pause::detail::LiveTestGate gate;
    CHECK_TRUE(!gate.Observe(RunningLiveObservation(0, 100, 200))
                    .failure_reason);
    CHECK_TRUE(gate.Observe(RunningLiveObservation(500, 101, 201)).request ==
               ge::host_pause::detail::LiveTestRequest::kOpenHostSettings);
    const auto result = gate.Observe(RunningLiveObservation(5601, 102, 202));
    CHECK_TRUE(result.failure_reason != nullptr);
    CHECK_TRUE(std::string_view(result.failure_reason) == "phase-timeout");
  }

  {
    ge::host_pause::detail::LiveTestGate gate;
    ReachLiveTestPauseObservation(gate);
    auto early_close = PausedLiveObservation(1100, 103, 203);
    early_close.ui_open = false;
    const auto result = gate.Observe(early_close);
    CHECK_TRUE(result.failure_reason != nullptr);
    CHECK_TRUE(std::string_view(result.failure_reason) == "ui-closed-early");
  }

  {
    ge::host_pause::detail::LiveTestGate gate;
    ReachLiveTestResumeWait(gate);
    auto wrong_generation = RunningLiveObservation(1700, 105, 205);
    wrong_generation.pause.generation = 9;
    wrong_generation.pause.applied_generation = 9;
    const auto result = gate.Observe(wrong_generation);
    CHECK_TRUE(result.failure_reason != nullptr);
    CHECK_TRUE(std::string_view(result.failure_reason) ==
               "resume-generation-mismatch");
  }

  {
    ge::host_pause::detail::LiveTestGate gate;
    ReachLiveTestResumeWait(gate);
    auto held_input = RunningLiveObservation(1700, 105, 205);
    held_input.pause.generation = 2;
    held_input.pause.applied_generation = 2;
    held_input.input_neutral = false;
    const auto result = gate.Observe(held_input);
    CHECK_TRUE(result.failure_reason != nullptr);
    CHECK_TRUE(std::string_view(result.failure_reason) ==
               "resume-input-not-neutral");
  }

  {
    ge::host_pause::detail::LiveTestGate gate;
    ReachLiveTestResumeWait(gate);
    auto resumed = RunningLiveObservation(1700, 105, 205);
    resumed.pause.generation = 2;
    resumed.pause.applied_generation = 2;
    CHECK_TRUE(!gate.Observe(resumed).failure_reason);
    resumed.monotonic_ms = 2200;
    const auto result = gate.Observe(resumed);
    CHECK_TRUE(result.failure_reason != nullptr);
    CHECK_TRUE(std::string_view(result.failure_reason) ==
               "resume-progress-stalled");
  }

  {
    ge::host_pause::detail::LiveTestGate gate;
    ReachLiveTestPauseObservation(gate);
    auto ownership_lost = PausedLiveObservation(1100, 103, 203);
    ownership_lost.pause.host_owned = false;
    const auto result = gate.Observe(ownership_lost);
    CHECK_TRUE(result.failure_reason != nullptr);
    CHECK_TRUE(std::string_view(result.failure_reason) ==
               "pause-ownership-lost");
  }

  {
    ge::host_pause::detail::LiveTestGate gate;
    CHECK_TRUE(!gate.Observe(RunningLiveObservation(500, 100, 200))
                    .failure_reason);
    const auto result = gate.Observe(RunningLiveObservation(499, 101, 201));
    CHECK_TRUE(result.failure_reason != nullptr);
    CHECK_TRUE(std::string_view(result.failure_reason) == "clock-regressed");
  }
}

#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)
void TestDeveloperMenuRequestBridge() {
  ge::host_pause::test_harness::MenuRequestBridge bridge;
  CHECK_TRUE(!bridge.Request(true));

  bool requested = false;
  bool desired_open = false;
  bridge.SetHandler([&](bool open) {
    requested = true;
    desired_open = open;
    return true;
  });
  CHECK_TRUE(bridge.Request(true));
  CHECK_TRUE(requested);
  CHECK_TRUE(desired_open);
  CHECK_TRUE(!bridge.open());
  bridge.PublishOpen(true);
  CHECK_TRUE(bridge.open());

  requested = false;
  CHECK_TRUE(bridge.Request(false));
  CHECK_TRUE(requested);
  CHECK_TRUE(!desired_open);
  bridge.ClearHandler();
  CHECK_TRUE(!bridge.Request(true));
}
#endif

size_t CountOccurrences(const std::string& text, const std::string& needle) {
  size_t count = 0;
  size_t position = 0;
  while ((position = text.find(needle, position)) != std::string::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

void TestGeneratedPauseWordContract() {
#if defined(GOLDENEYE_GENERATED_DIRECTORY)
  const std::filesystem::path generated(GOLDENEYE_GENERATED_DIRECTORY);
  size_t getter_calls = 0;
  size_t setter_calls = 0;
  size_t raw_pause_word_references = 0;
  const std::string getter_call = "\tsub_8209F588(ctx, base);";
  const std::string setter_call = "\tsub_8209F578(ctx, base);";
  const std::string zero_compare = "ctx.cr6.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);";
  const size_t compare_r3_offset = zero_compare.find("ctx.r3");

  for (const auto& entry : std::filesystem::directory_iterator(generated)) {
    const std::string filename = entry.path().filename().string();
    if (!entry.is_regular_file() || !filename.starts_with("ge_recomp.") ||
        entry.path().extension() != ".cpp") {
      continue;
    }
    std::ifstream stream(entry.path(), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    raw_pause_word_references += CountOccurrences(text, "-6388");

    size_t position = 0;
    while ((position = text.find(getter_call, position)) != std::string::npos) {
      ++getter_calls;
      const size_t after_call = position + getter_call.size();
      const size_t function_end = text.find("\nDEFINE_REX_FUNC(", after_call);
      const size_t first_r3_use = text.find("ctx.r3", after_call);
      const size_t comparison = text.find(zero_compare, after_call);
      const bool contract_ok =
          first_r3_use != std::string::npos && comparison != std::string::npos &&
          (function_end == std::string::npos || comparison < function_end) &&
          first_r3_use == comparison + compare_r3_offset && comparison - after_call < 512;
      CHECK_TRUE(contract_ok);
      position = after_call;
    }

    position = 0;
    while ((position = text.find(setter_call, position)) != std::string::npos) {
      ++setter_calls;
      const size_t function_start = text.rfind("\nDEFINE_REX_FUNC(", position);
      const size_t zero_assignment = text.rfind("ctx.r3.s64 = 0;", position);
      const size_t one_assignment = text.rfind("ctx.r3.s64 = 1;", position);
      size_t reaching_assignment = zero_assignment;
      if (one_assignment != std::string::npos &&
          (reaching_assignment == std::string::npos || one_assignment > reaching_assignment)) {
        reaching_assignment = one_assignment;
      }
      const size_t intervening_call = reaching_assignment == std::string::npos
                                          ? std::string::npos
                                          : text.find("(ctx, base);", reaching_assignment);
      const bool setter_contract_ok =
          reaching_assignment != std::string::npos &&
          (function_start == std::string::npos || reaching_assignment > function_start) &&
          intervening_call >= position && intervening_call < position + setter_call.size();
      CHECK_TRUE(setter_contract_ok);
      position += setter_call.size();
    }
  }

  // Lock the exact supported generated build contract: 22 consumers all test
  // zero/nonzero as their first in-function r3 use, all five retail setter call
  // sites pass a locally established boolean 0/1 without an intervening call,
  // and only setter/getter contain the four textual raw-word references
  // (comment plus generated memory operation for each).
  CHECK_TRUE(getter_calls == 22);
  CHECK_TRUE(setter_calls == 5);
  CHECK_TRUE(raw_pause_word_references == 4);
#else
  CHECK_TRUE(false);
#endif
}

}  // namespace

int main() {
  TestLocalMissionEligibility();
  TestAcquireReleaseAndIdempotence();
  TestPreexistingRetailPauseIsRestored();
  TestLaterRetailIntentIsPreserved();
  TestRetailWritesStayPausedAndEligibilityLossIsSafe();
  TestUnavailableRapidCancellationAndResumePulse();
  TestFailedWritesRetryAndReleaseRace();
  TestRetailPauseWriteShadow();
  TestResumeInputLatch();
  TestIndependentMultiplayerResumeLatches();
  TestControllerHostSettingsShortcut();
  TestRetryLogGate();
  TestLiveHostPauseGateSuccess();
  TestLiveHostPauseGateFailsClosed();
#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)
  TestDeveloperMenuRequestBridge();
#endif
  TestGeneratedPauseWordContract();
  return failures == 0 ? 0 : 1;
}
