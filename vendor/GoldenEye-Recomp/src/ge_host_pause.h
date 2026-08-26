#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)
#include <functional>
#include <utility>
#endif

struct PPCContext;

namespace ge::host_pause {

// GoldenEye treats the retail word as a boolean everywhere in the supported
// build. A distinct nonzero token proves host ownership. While it is present,
// the strong retail setter wrapper records GoldenEye's latest 0/1 intent and
// keeps the token asserted; closing host settings restores that recorded value.
inline constexpr uint32_t kHostPauseToken = 0x47454850u;  // "GEHP"

inline constexpr bool IsEligibleLocalMission(int32_t level, int32_t players,
                                             bool network_session) noexcept {
  return level > 0 && level < 90 && players >= 1 && players <= 4 && !network_session;
}

enum class Action : uint8_t {
  kNone,
  kAcquire,
  kRelease,
  kRelinquish,
};

struct Snapshot {
  bool requested = false;
  bool request_applied = true;
  bool available = false;
  bool gameplay_paused = false;
  bool host_owned = false;
  uint64_t generation = 0;
  uint64_t applied_generation = 0;
};

struct ProcessResult {
  Action action = Action::kNone;
  bool action_succeeded = true;
  bool request_applied = false;
  bool gameplay_paused = false;
  bool host_owned = false;
  bool input_resume_pulse = false;
  uint64_t generation = 0;
  uint32_t pause_value = 0;
};

// Small game-thread latch used after the pause is actually released. It waits
// for complete neutral input rather than only the controller button that closed
// the host menu, covering mouse, keyboard, triggers and either stick. Two
// consecutive neutral polls are required and the final neutral poll is still
// swallowed, so host input cannot leak across the menu-to-game transition.
struct InputSample {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
  int16_t thumb_rx = 0;
  int16_t thumb_ry = 0;
};

class ResumeInputLatch {
 public:
  static constexpr uint8_t kTriggerThreshold = 30;
  // Dear ImGui's standard gamepad navigation starts immediately above 20%.
  // Keep the close-frame latch on that same raw-axis boundary so a stick that
  // can navigate the host menu cannot leak into gameplay on the next poll.
  static constexpr int16_t kStickDeadzone = 6553;
  static constexpr uint32_t kRequiredNeutralPolls = 2;
  static constexpr uint32_t kMaximumSuppressedPolls = 120;

  void Arm(const InputSample& /*sample*/) noexcept {
    suppressed_polls_ = 0;
    neutral_polls_ = 0;
    active_ = true;
  }

  bool active() const noexcept { return active_; }

  bool ShouldSuppress(const InputSample& sample) noexcept {
    if (!active_) {
      return false;
    }

    ++suppressed_polls_;
    if (IsNeutral(sample)) {
      ++neutral_polls_;
    } else {
      neutral_polls_ = 0;
    }
    if (neutral_polls_ >= kRequiredNeutralPolls || suppressed_polls_ >= kMaximumSuppressedPolls) {
      active_ = false;
    }

    // The poll that observes release is neutral. A fresh press on the next
    // poll is the first input allowed back into GoldenEye.
    return true;
  }

 private:
  static bool IsNeutral(const InputSample& sample) noexcept {
    return sample.buttons == 0 && sample.left_trigger < kTriggerThreshold &&
           sample.right_trigger < kTriggerThreshold && sample.thumb_lx <= kStickDeadzone &&
           sample.thumb_lx >= -kStickDeadzone && sample.thumb_ly <= kStickDeadzone &&
           sample.thumb_ly >= -kStickDeadzone && sample.thumb_rx <= kStickDeadzone &&
           sample.thumb_rx >= -kStickDeadzone && sample.thumb_ry <= kStickDeadzone &&
           sample.thumb_ry >= -kStickDeadzone;
  }

  uint32_t suppressed_polls_ = 0;
  uint32_t neutral_polls_ = 0;
  bool active_ = false;
};

namespace detail {

struct RetryLogDecision {
  bool warn = false;
  bool recovered = false;
  uint32_t failed_attempts = 0;
};

// Game-thread-only warning gate. A retry episode is identified by the exact
// request generation and action. Cancellation, eligibility loss, or a newer
// request clears the old episode silently; only a matching successful action
// is reported as recovery.
class RetryLogGate {
 public:
  RetryLogDecision Observe(const ProcessResult& result) noexcept {
    const bool retryable_action =
        result.action == Action::kAcquire || result.action == Action::kRelease;
    if (!result.action_succeeded && retryable_action) {
      if (!active_ || generation_ != result.generation || action_ != result.action) {
        active_ = true;
        generation_ = result.generation;
        action_ = result.action;
        failed_attempts_ = 1;
        return {.warn = true, .failed_attempts = failed_attempts_};
      }
      ++failed_attempts_;
      return {.failed_attempts = failed_attempts_};
    }

    RetryLogDecision decision;
    if (active_ && result.action_succeeded && result.generation == generation_ &&
        result.action == action_) {
      decision.recovered = true;
      decision.failed_attempts = failed_attempts_;
    }
    Reset();
    return decision;
  }

  bool active() const noexcept { return active_; }

 private:
  void Reset() noexcept {
    active_ = false;
    generation_ = 0;
    action_ = Action::kNone;
    failed_attempts_ = 0;
  }

  bool active_ = false;
  uint64_t generation_ = 0;
  Action action_ = Action::kNone;
  uint32_t failed_attempts_ = 0;
};

#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)
enum class LiveTestPhase : uint8_t {
  kWaitingForGameplay,
  kWaitingForPause,
  kObservingPause,
  kWaitingForResume,
  kObservingResume,
  kComplete,
  kFailed,
};

enum class LiveTestRequest : uint8_t {
  kNone,
  kOpenHostSettings,
  kCloseHostSettings,
};

struct LiveTestObservation {
  uint64_t monotonic_ms = 0;
  bool dam_gameplay_ready = false;
  bool ui_open = false;
  bool world_valid = false;
  bool ammo_valid = false;
  bool input_neutral = false;
  uint32_t player = 0;
  uint32_t coordinates = 0;
  uint32_t guest_frame = 0;
  uint32_t present = 0;
  uint32_t pause_value = 0;
  float position_x = 0.0f;
  float position_y = 0.0f;
  float position_z = 0.0f;
  float camera_yaw = 0.0f;
  float camera_pitch = 0.0f;
  int32_t weapon = 0;
  int32_t ammo = 0;
  Snapshot pause;
};

struct LiveTestProof {
  uint64_t open_generation = 0;
  uint64_t resume_generation = 0;
  uint32_t frozen_samples = 0;
  uint64_t frozen_duration_ms = 0;
  uint32_t paused_frame_delta = 0;
  uint32_t paused_present_delta = 0;
  uint32_t resumed_samples = 0;
  uint64_t resumed_duration_ms = 0;
  uint32_t resumed_frame_delta = 0;
  uint32_t resumed_present_delta = 0;
};

struct LiveTestUpdate {
  LiveTestRequest request = LiveTestRequest::kNone;
  bool frozen_proven = false;
  bool completed = false;
  const char* failure_reason = nullptr;
};

// Deterministic, read-only classifier for the developer-only live Host
// Settings test. The runtime wrapper performs the requested UI action; this
// state machine only decides when the evidence is sufficient or contradictory.
class LiveTestGate {
 public:
  static constexpr uint32_t kRequiredReadySamples = 2;
  static constexpr uint32_t kRequiredFrozenSamples = 3;
  static constexpr uint64_t kRequiredFrozenDurationMs = 1000;
  static constexpr uint32_t kRequiredResumedSamples = 2;
  static constexpr uint64_t kRequiredResumedDurationMs = 500;
  static constexpr uint64_t kPhaseTimeoutMs = 5000;

  LiveTestUpdate Observe(const LiveTestObservation& observation) noexcept {
    if (phase_ == LiveTestPhase::kComplete) {
      return {.completed = true};
    }
    if (phase_ == LiveTestPhase::kFailed) {
      return {.failure_reason = failure_reason_};
    }
    if (has_observation_time_ && observation.monotonic_ms < last_observation_ms_) {
      return Fail("clock-regressed");
    }
    has_observation_time_ = true;
    last_observation_ms_ = observation.monotonic_ms;

    if (phase_ != LiveTestPhase::kWaitingForGameplay &&
        (!observation.dam_gameplay_ready || !SameWorldIdentity(observation))) {
      return Fail("mission-state-lost");
    }
    if (phase_ != LiveTestPhase::kWaitingForGameplay &&
        observation.monotonic_ms - phase_started_ms_ > kPhaseTimeoutMs) {
      return Fail("phase-timeout");
    }

    switch (phase_) {
      case LiveTestPhase::kWaitingForGameplay:
        return ObserveReadiness(observation);
      case LiveTestPhase::kWaitingForPause:
        return ObservePauseAcquisition(observation);
      case LiveTestPhase::kObservingPause:
        return ObserveFrozenWorld(observation);
      case LiveTestPhase::kWaitingForResume:
        return ObservePauseRelease(observation);
      case LiveTestPhase::kObservingResume:
        return ObserveResumedWorld(observation);
      case LiveTestPhase::kComplete:
        return {.completed = true};
      case LiveTestPhase::kFailed:
        return {.failure_reason = failure_reason_};
    }
    return Fail("invalid-phase");
  }

  LiveTestUpdate Fail(const char* reason) noexcept {
    if (phase_ != LiveTestPhase::kFailed) {
      phase_ = LiveTestPhase::kFailed;
      failure_reason_ = reason ? reason : "unknown";
    }
    return {.failure_reason = failure_reason_};
  }

  LiveTestPhase phase() const noexcept { return phase_; }
  const LiveTestProof& proof() const noexcept { return proof_; }
  const char* failure_reason() const noexcept { return failure_reason_; }

 private:
  static constexpr bool CounterAdvanced(uint32_t previous, uint32_t current) noexcept {
    const uint32_t delta = current - previous;
    return delta != 0 && delta < 0x80000000u;
  }

  static constexpr bool CounterRegressed(uint32_t previous, uint32_t current) noexcept {
    return current != previous && !CounterAdvanced(previous, current);
  }

  static constexpr float Absolute(float value) noexcept {
    return value < 0.0f ? -value : value;
  }

  static bool RunningAndUnowned(const LiveTestObservation& observation) noexcept {
    return observation.dam_gameplay_ready && observation.world_valid &&
           observation.pause_value == 0 && !observation.ui_open &&
           !observation.pause.requested && observation.pause.request_applied &&
           !observation.pause.gameplay_paused && !observation.pause.host_owned;
  }

  static bool FullyHostPaused(const LiveTestObservation& observation) noexcept {
    return observation.ui_open && observation.pause_value == kHostPauseToken &&
           observation.pause.requested && observation.pause.request_applied &&
           observation.pause.available && observation.pause.gameplay_paused &&
           observation.pause.host_owned &&
           observation.pause.generation == observation.pause.applied_generation;
  }

  bool SameWorldIdentity(const LiveTestObservation& observation) const noexcept {
    return observation.world_valid && observation.player == identity_player_ &&
           observation.coordinates == identity_coordinates_;
  }

  static bool SameWorldState(const LiveTestObservation& first,
                             const LiveTestObservation& second) noexcept {
    constexpr float kPositionTolerance = 0.0001f;
    constexpr float kCameraTolerance = 0.0001f;
    return first.player == second.player && first.coordinates == second.coordinates &&
           Absolute(first.position_x - second.position_x) <= kPositionTolerance &&
           Absolute(first.position_y - second.position_y) <= kPositionTolerance &&
           Absolute(first.position_z - second.position_z) <= kPositionTolerance &&
           Absolute(first.camera_yaw - second.camera_yaw) <= kCameraTolerance &&
           Absolute(first.camera_pitch - second.camera_pitch) <= kCameraTolerance &&
           first.weapon == second.weapon && first.ammo_valid == second.ammo_valid &&
           (!first.ammo_valid || first.ammo == second.ammo);
  }

  void CaptureIdentity(const LiveTestObservation& observation) noexcept {
    identity_player_ = observation.player;
    identity_coordinates_ = observation.coordinates;
  }

  LiveTestUpdate ObserveReadiness(const LiveTestObservation& observation) noexcept {
    if (!RunningAndUnowned(observation) || !observation.input_neutral) {
      ready_samples_ = 0;
      return {};
    }
    if (ready_samples_ == 0 || observation.player != identity_player_ ||
        observation.coordinates != identity_coordinates_ ||
        CounterRegressed(ready_baseline_.guest_frame, observation.guest_frame) ||
        CounterRegressed(ready_baseline_.present, observation.present)) {
      ready_samples_ = 1;
      ready_baseline_ = observation;
      CaptureIdentity(observation);
      return {};
    }
    if (!CounterAdvanced(ready_baseline_.guest_frame, observation.guest_frame) ||
        !CounterAdvanced(ready_baseline_.present, observation.present)) {
      return {};
    }
    if (++ready_samples_ < kRequiredReadySamples) {
      return {};
    }
    phase_ = LiveTestPhase::kWaitingForPause;
    phase_started_ms_ = observation.monotonic_ms;
    return {.request = LiveTestRequest::kOpenHostSettings};
  }

  LiveTestUpdate ObservePauseAcquisition(const LiveTestObservation& observation) noexcept {
    if (observation.ui_open && !observation.pause.requested) {
      return Fail("ui-open-without-pause-request");
    }
    if (!FullyHostPaused(observation)) {
      return {};
    }
    if (!SameWorldIdentity(observation)) {
      return Fail("world-identity-changed");
    }
    proof_.open_generation = observation.pause.generation;
    if (proof_.open_generation == 0) {
      return Fail("pause-generation-missing");
    }
    frozen_baseline_ = observation;
    proof_.frozen_samples = 1;
    phase_ = LiveTestPhase::kObservingPause;
    phase_started_ms_ = observation.monotonic_ms;
    return {};
  }

  LiveTestUpdate ObserveFrozenWorld(const LiveTestObservation& observation) noexcept {
    if (!FullyHostPaused(observation)) {
      return Fail(observation.ui_open ? "pause-ownership-lost" : "ui-closed-early");
    }
    if (observation.pause.generation != proof_.open_generation) {
      return Fail("pause-generation-changed");
    }
    if (!SameWorldState(frozen_baseline_, observation)) {
      return Fail("world-advanced-while-paused");
    }
    if (CounterRegressed(frozen_baseline_.guest_frame, observation.guest_frame) ||
        CounterRegressed(frozen_baseline_.present, observation.present)) {
      return Fail("progress-counter-regressed");
    }
    ++proof_.frozen_samples;
    proof_.frozen_duration_ms = observation.monotonic_ms - frozen_baseline_.monotonic_ms;
    proof_.paused_frame_delta = observation.guest_frame - frozen_baseline_.guest_frame;
    proof_.paused_present_delta = observation.present - frozen_baseline_.present;
    if (proof_.frozen_duration_ms < kRequiredFrozenDurationMs ||
        proof_.frozen_samples < kRequiredFrozenSamples) {
      return {};
    }
    if (proof_.paused_present_delta == 0) {
      return Fail("paused-presentation-stalled");
    }
    phase_ = LiveTestPhase::kWaitingForResume;
    phase_started_ms_ = observation.monotonic_ms;
    return {.request = LiveTestRequest::kCloseHostSettings, .frozen_proven = true};
  }

  LiveTestUpdate ObservePauseRelease(const LiveTestObservation& observation) noexcept {
    if (observation.ui_open || observation.pause.requested ||
        !observation.pause.request_applied || observation.pause.gameplay_paused ||
        observation.pause.host_owned || observation.pause_value != 0) {
      return {};
    }
    proof_.resume_generation = observation.pause.generation;
    if (proof_.resume_generation != proof_.open_generation + 1) {
      return Fail("resume-generation-mismatch");
    }
    if (!observation.input_neutral) {
      return Fail("resume-input-not-neutral");
    }
    resumed_baseline_ = observation;
    proof_.resumed_samples = 1;
    phase_ = LiveTestPhase::kObservingResume;
    phase_started_ms_ = observation.monotonic_ms;
    return {};
  }

  LiveTestUpdate ObserveResumedWorld(const LiveTestObservation& observation) noexcept {
    if (!RunningAndUnowned(observation)) {
      return Fail("pause-not-released");
    }
    if (!observation.input_neutral) {
      return Fail("resume-input-not-neutral");
    }
    if (CounterRegressed(resumed_baseline_.guest_frame, observation.guest_frame) ||
        CounterRegressed(resumed_baseline_.present, observation.present)) {
      return Fail("progress-counter-regressed");
    }
    ++proof_.resumed_samples;
    proof_.resumed_duration_ms = observation.monotonic_ms - resumed_baseline_.monotonic_ms;
    proof_.resumed_frame_delta = observation.guest_frame - resumed_baseline_.guest_frame;
    proof_.resumed_present_delta = observation.present - resumed_baseline_.present;
    if (proof_.resumed_duration_ms < kRequiredResumedDurationMs ||
        proof_.resumed_samples < kRequiredResumedSamples) {
      return {};
    }
    if (proof_.resumed_frame_delta == 0 || proof_.resumed_present_delta == 0) {
      return Fail("resume-progress-stalled");
    }
    phase_ = LiveTestPhase::kComplete;
    return {.completed = true};
  }

  LiveTestPhase phase_ = LiveTestPhase::kWaitingForGameplay;
  const char* failure_reason_ = nullptr;
  bool has_observation_time_ = false;
  uint64_t last_observation_ms_ = 0;
  uint64_t phase_started_ms_ = 0;
  uint32_t ready_samples_ = 0;
  uint32_t identity_player_ = 0;
  uint32_t identity_coordinates_ = 0;
  LiveTestObservation ready_baseline_;
  LiveTestObservation frozen_baseline_;
  LiveTestObservation resumed_baseline_;
  LiveTestProof proof_;
};
#endif

}  // namespace detail

#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)
namespace test_harness {

using MenuRequestHandler = std::function<bool(bool open)>;

// The game thread requests an exact desired menu state; GeApp owns the only
// handler and schedules the real TogglePauseMenu path on the UI thread.
class MenuRequestBridge {
 public:
  void SetHandler(MenuRequestHandler handler) {
    std::lock_guard lock(mutex_);
    handler_ = std::move(handler);
  }

  void ClearHandler() {
    std::lock_guard lock(mutex_);
    handler_ = {};
  }

  bool Request(bool open) {
    std::lock_guard lock(mutex_);
    return handler_ && handler_(open);
  }

  void PublishOpen(bool open) noexcept {
    open_.store(open, std::memory_order_release);
  }

  bool open() const noexcept { return open_.load(std::memory_order_acquire); }

 private:
  std::mutex mutex_;
  MenuRequestHandler handler_;
  std::atomic<bool> open_{false};
};

inline MenuRequestBridge& Bridge() {
  static MenuRequestBridge bridge;
  return bridge;
}

inline void SetMenuRequestHandler(MenuRequestHandler handler) {
  Bridge().SetHandler(std::move(handler));
}

inline void ClearMenuRequestHandler() { Bridge().ClearHandler(); }
inline bool RequestMenuState(bool open) { return Bridge().Request(open); }
inline void PublishMenuOpen(bool open) noexcept { Bridge().PublishOpen(open); }
inline bool MenuOpen() noexcept { return Bridge().open(); }

}  // namespace test_harness
#endif

// Thread-safe request bridge between the host UI and GoldenEye's game thread.
// Only ProcessGameThread (or a test standing in for it) may call Process.
class State {
 public:
  bool RequestPaused(bool paused) noexcept {
    std::lock_guard lock(operation_mutex_);
    const uint64_t current = request_.load(std::memory_order_acquire);
    if ((current & 1u) == (paused ? 1u : 0u)) {
      return false;
    }
    const uint64_t next = (((current >> 1) + 1) << 1) | (paused ? 1u : 0u);
    request_.store(next, std::memory_order_release);
    return true;
  }

  Snapshot GetSnapshot() const noexcept {
    const uint64_t request = request_.load(std::memory_order_acquire);
    const uint64_t applied = applied_request_.load(std::memory_order_acquire);
    const uint8_t status = status_.load(std::memory_order_acquire);
    return {
        .requested = (request & 1u) != 0,
        .request_applied = request == applied,
        .available = (status & kAvailable) != 0,
        .gameplay_paused = (status & kGameplayPaused) != 0,
        .host_owned = (status & kHostOwned) != 0,
        .generation = request >> 1,
        .applied_generation = applied >> 1,
    };
  }

  // Called only by the strong retail setter wrapper while the pause-word
  // transaction mutex is held. GoldenEye may continue writing its normal 0/1
  // state behind host settings; remember the latest retail intent while
  // keeping the host token continuously asserted. On host release Process
  // restores that remembered value instead of blindly clearing a retail pause.
  uint32_t FilterRetailWrite(uint32_t requested_value) noexcept {
    std::lock_guard lock(operation_mutex_);
    if (owns_pause_) {
      retail_restore_value_ = requested_value != 0 ? 1u : 0u;
      return kHostPauseToken;
    }
    return requested_value;
  }

  // Handles the common no-request/no-ownership case without touching guest
  // state. The same mutex used by RequestPaused makes the acknowledgement and
  // all retail setter actions generation-consistent.
  bool TryProcessIdle(ProcessResult* out_result) noexcept {
    std::lock_guard lock(operation_mutex_);
    const uint64_t request = request_.load(std::memory_order_acquire);
    if ((request & 1u) != 0 || owns_pause_) {
      return false;
    }
    const bool pending = request != applied_request_.load(std::memory_order_acquire);
    Publish(request, false, false, false, true);
    if (out_result) {
      *out_result = {
          .request_applied = true,
          .input_resume_pulse = pending,
          .generation = request >> 1,
      };
    }
    return true;
  }

  // Runs on the guest game thread. Zero means running, kHostPauseToken means
  // this bridge owns the pause, and every other nonzero value is retail-owned.
  // The host replaces the word only while serialized with the strong retail
  // setter wrapper, and restores the title's latest recorded boolean on close.
  template <typename QueryPauseValue, typename SetPauseValue>
  ProcessResult Process(bool eligible, QueryPauseValue&& query_pause_value,
                        SetPauseValue&& set_pause_value) noexcept {
    std::lock_guard lock(operation_mutex_);
    const uint64_t request = request_.load(std::memory_order_acquire);
    const bool desired = (request & 1u) != 0;
    const bool pending = request != applied_request_.load(std::memory_order_acquire);
    Action action = Action::kNone;
    bool action_succeeded = true;
    uint32_t pause_value = 0;

    if (!desired && !owns_pause_) {
      Publish(request, false, false, false, true);
      return {
          .request_applied = true,
          .input_resume_pulse = pending,
          .generation = request >> 1,
      };
    }

    if (desired && !eligible && !owns_pause_) {
      Publish(request, false, false, false, true);
      return {
          .request_applied = true,
          .generation = request >> 1,
      };
    }

    pause_value = query_pause_value();

    if (owns_pause_ && (!desired || !eligible)) {
      if (pause_value == kHostPauseToken) {
        action = Action::kRelease;
        set_pause_value(retail_restore_value_);
        pause_value = query_pause_value();
        if (pause_value == kHostPauseToken) {
          action_succeeded = false;
        } else {
          owns_pause_ = false;
          if (pause_value != retail_restore_value_ && pause_value != 0) {
            action = Action::kRelinquish;
          }
        }
      } else {
        // A later retail write replaced our token. It is now the game's pause,
        // so relinquish ownership without ever clearing the word.
        owns_pause_ = false;
        if (pause_value != 0) {
          action = Action::kRelinquish;
        }
      }
    } else if (desired && eligible) {
      if (!owns_pause_ || pause_value != kHostPauseToken) {
        // Save the title's current boolean pause before replacing it with the
        // host token. A direct/unwrapped write observed while already owning is
        // treated as updated retail intent and repaired immediately.
        retail_restore_value_ = pause_value != 0 ? 1u : 0u;
        action = Action::kAcquire;
        set_pause_value(kHostPauseToken);
        pause_value = query_pause_value();
        if (pause_value == kHostPauseToken) {
          owns_pause_ = true;
        } else if (pause_value != 0) {
          // Retail won a same-poll race with acquisition. The requested pause
          // is satisfied, but the host must not claim or later clear it.
          owns_pause_ = false;
          action = Action::kRelinquish;
        } else {
          action_succeeded = false;
        }
      }
    }

    const bool gameplay_paused = desired && eligible && pause_value != 0;
    const bool complete = desired ? (eligible ? pause_value != 0 : !owns_pause_) : !owns_pause_;
    Publish(request, eligible, gameplay_paused, owns_pause_, complete);
    return {
        .action = action,
        .action_succeeded = action_succeeded,
        .request_applied = complete,
        .gameplay_paused = gameplay_paused,
        .host_owned = owns_pause_,
        .input_resume_pulse = pending && !desired && complete,
        .generation = request >> 1,
        .pause_value = pause_value,
    };
  }

 private:
  static constexpr uint8_t kAvailable = 1u << 0;
  static constexpr uint8_t kGameplayPaused = 1u << 1;
  static constexpr uint8_t kHostOwned = 1u << 2;

  void Publish(uint64_t request, bool available, bool gameplay_paused, bool host_owned,
               bool applied) noexcept {
    uint8_t status = 0;
    if (available) {
      status |= kAvailable;
    }
    if (gameplay_paused) {
      status |= kGameplayPaused;
    }
    if (host_owned) {
      status |= kHostOwned;
    }
    status_.store(status, std::memory_order_release);
    if (applied) {
      applied_request_.store(request, std::memory_order_release);
    }
  }

  // Bit 0 is desired pause state; upper bits are the request generation.
  std::atomic<uint64_t> request_{0};
  std::atomic<uint64_t> applied_request_{0};
  std::atomic<uint8_t> status_{0};
  std::mutex operation_mutex_;
  bool owns_pause_ = false;  // game-thread-only, serialized with request changes
  uint32_t retail_restore_value_ = 0;
};

// Host/UI thread API.
void RequestPaused(bool paused) noexcept;
Snapshot GetSnapshot() noexcept;

// GoldenEye game-thread bridge. Called from ge_inject_keyboard before other
// host requests and never directly from the host render/UI thread.
ProcessResult ProcessGameThread(PPCContext& context, uint8_t* base) noexcept;

}  // namespace ge::host_pause
