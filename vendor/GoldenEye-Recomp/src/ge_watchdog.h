#pragma once

#include <atomic>
#include <cstdint>

namespace ge::watchdog {

struct SessionSnapshot {
  uint32_t guest_present = 0;
  uint32_t present_cp_swap = 0;
  uint32_t debug_poll_count = 0;
  uint32_t guest_device = 0;
  uint32_t guest_id_block = 0;
};

// State published by guest hooks and sampled by the host watchdog. Resetting
// this object before each worker launch prevents a new in-process title launch
// from inheriting guest addresses from the previous address space.
class SessionState {
 public:
  void Reset() noexcept {
    guest_present_.store(0, std::memory_order_relaxed);
    present_cp_swap_.store(0, std::memory_order_relaxed);
    debug_poll_count_.store(0, std::memory_order_relaxed);
    guest_addresses_.store(0, std::memory_order_relaxed);
  }

  void PublishDebugPoll(uint32_t guest_device, uint32_t guest_id_block) noexcept {
    const uint64_t guest_addresses =
        (static_cast<uint64_t>(guest_device) << 32) | guest_id_block;
    guest_addresses_.store(guest_addresses, std::memory_order_relaxed);
    debug_poll_count_.fetch_add(1, std::memory_order_relaxed);
  }

  void PublishPresentCpSwap(uint32_t cp_swap) noexcept {
    present_cp_swap_.store(cp_swap, std::memory_order_relaxed);
  }

  uint32_t AdvanceGuestPresent() noexcept {
    return guest_present_.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  SessionSnapshot Load() const noexcept {
    const uint64_t guest_addresses = guest_addresses_.load(std::memory_order_relaxed);
    return {
        .guest_present = guest_present_.load(std::memory_order_relaxed),
        .present_cp_swap = present_cp_swap_.load(std::memory_order_relaxed),
        .debug_poll_count = debug_poll_count_.load(std::memory_order_relaxed),
        .guest_device = static_cast<uint32_t>(guest_addresses >> 32),
        .guest_id_block = static_cast<uint32_t>(guest_addresses),
    };
  }

 private:
  std::atomic<uint32_t> guest_present_{0};
  std::atomic<uint32_t> present_cp_swap_{0};
  std::atomic<uint32_t> debug_poll_count_{0};
  std::atomic<uint64_t> guest_addresses_{0};
};

enum class ShutdownOwnership : uint8_t {
  kNotStarted,
  kWaitForOwner,
  kOwnShutdown,
};

// Lock-external state machine for the watchdog worker. Its owner serializes all
// calls with the worker mutex; keeping the transitions independent from guest
// memory makes restart and concurrent-shutdown ownership deterministic to test.
class WorkerLifecycle {
 public:
  constexpr bool TryStart() noexcept {
    if (started_ || stopping_) {
      return false;
    }
    stop_requested_ = false;
    started_ = true;
    return true;
  }

  constexpr void AbortStart() noexcept {
    started_ = false;
    stop_requested_ = false;
  }

  constexpr ShutdownOwnership BeginShutdown() noexcept {
    if (stopping_) {
      return ShutdownOwnership::kWaitForOwner;
    }
    if (!started_) {
      return ShutdownOwnership::kNotStarted;
    }
    stopping_ = true;
    stop_requested_ = true;
    return ShutdownOwnership::kOwnShutdown;
  }

  constexpr void FinishShutdown() noexcept {
    started_ = false;
    stopping_ = false;
    stop_requested_ = false;
  }

  constexpr bool started() const noexcept { return started_; }
  constexpr bool stopping() const noexcept { return stopping_; }
  constexpr bool stop_requested() const noexcept { return stop_requested_; }

 private:
  bool started_ = false;
  bool stopping_ = false;
  bool stop_requested_ = false;
};

struct ObservationContext {
  bool host_pause_active = false;
  bool host_input_available = false;
  bool focused = true;
  bool input_active = true;
};

constexpr bool ObservationSuppressed(const ObservationContext& context) noexcept {
  return context.host_pause_active ||
         (context.host_input_available && (!context.focused || !context.input_active));
}

struct ProgressSample {
  bool valid = false;
  uint32_t guest_present = 0;
  uint32_t cp_swap = 0;
  uint64_t ring_generation = 0;
  uint32_t ring_read = 0;
  uint32_t ring_write = 0;
  bool ring_valid = false;
  bool ring_pending = false;
};

struct Progress {
  bool warmup = true;
  uint32_t guest_present_delta = 0;
  uint32_t cp_swap_delta = 0;
  bool guest_present_advanced = false;
  bool cp_swap_advanced = false;
  bool ring_read_advanced = false;
  bool ring_write_advanced = false;
  bool ring_generation_changed = false;
  bool ring_validity_changed = false;
  bool ring_reconfigured = false;
  bool ring_valid = false;
  bool ring_pending = false;

  // A visual-stall candidate requires the title to keep reaching its present
  // hook while neither the command-processor swap timeline nor the consumer
  // read pointer advances. Producer-only WPTR movement is useful evidence, but
  // it must not hide a stalled GPU consumer.
  constexpr bool visual_stall_candidate() const {
    return !warmup && !ring_reconfigured && ring_valid && guest_present_advanced &&
           !cp_swap_advanced && !ring_read_advanced;
  }

  // Once a title has successfully configured its ring, losing that ring while
  // the guest keeps presenting is also a visual-stall signal. The episode
  // tracker gives the validity transition one sample of grace.
  constexpr bool invalid_ring_visual_stall_candidate(bool previously_saw_valid_ring) const {
    return !warmup && previously_saw_valid_ring && !ring_valid && guest_present_advanced &&
           !cp_swap_advanced;
  }

  constexpr bool no_guest_present_candidate(uint32_t current_guest_present,
                                            uint32_t minimum_present_count) const {
    return !warmup && !guest_present_advanced && current_guest_present >= minimum_present_count;
  }
};

constexpr Progress ClassifyProgress(const ProgressSample& previous, const ProgressSample& current) {
  Progress progress;
  progress.ring_valid = current.ring_valid;
  progress.ring_pending = current.ring_pending;
  if (!previous.valid || !current.valid) {
    return progress;
  }
  progress.warmup = false;
  // Unsigned subtraction intentionally preserves useful deltas across a
  // 32-bit counter wrap.
  progress.guest_present_delta = current.guest_present - previous.guest_present;
  progress.cp_swap_delta = current.cp_swap - previous.cp_swap;
  progress.guest_present_advanced = progress.guest_present_delta != 0;
  progress.cp_swap_advanced = progress.cp_swap_delta != 0;
  progress.ring_generation_changed = current.ring_generation != previous.ring_generation;
  progress.ring_validity_changed = current.ring_valid != previous.ring_valid;
  progress.ring_reconfigured = progress.ring_generation_changed || progress.ring_validity_changed;
  progress.ring_read_advanced =
      !progress.ring_reconfigured && current.ring_valid && current.ring_read != previous.ring_read;
  progress.ring_write_advanced = !progress.ring_reconfigured && current.ring_valid &&
                                 current.ring_write != previous.ring_write;
  return progress;
}

struct EpisodeUpdate {
  Progress progress;
  uint32_t visual_stall_samples = 0;
  uint32_t no_present_samples = 0;
  bool observation_suppressed = false;
  bool visual_stall_started = false;
  bool no_present_started = false;
  bool log_visual_stall = false;
  bool log_no_present_stall = false;
};

class EpisodeTracker {
 public:
  static constexpr uint32_t kDefaultVisualStallThreshold = 6;
  static constexpr uint32_t kDefaultNoPresentThreshold = 24;
  static constexpr uint32_t kDefaultMinimumPresentCount = 8;

  constexpr explicit EpisodeTracker(uint32_t visual_stall_threshold = kDefaultVisualStallThreshold,
                                    uint32_t no_present_threshold = kDefaultNoPresentThreshold,
                                    uint32_t minimum_present_count = kDefaultMinimumPresentCount)
      : visual_stall_threshold_(visual_stall_threshold),
        no_present_threshold_(no_present_threshold),
        minimum_present_count_(minimum_present_count) {}

  constexpr EpisodeUpdate Observe(const ProgressSample& sample, bool observation_suppressed = false) {
    EpisodeUpdate update;
    if (observation_suppressed) {
      Reset();
      update.observation_suppressed = true;
      return update;
    }
    update.progress = ClassifyProgress(previous_, sample);
    previous_ = sample;

    if (update.progress.warmup) {
      ResetEpisodes();
      saw_valid_ring_ = sample.ring_valid;
      return update;
    }

    const bool previously_saw_valid_ring = saw_valid_ring_;
    saw_valid_ring_ = saw_valid_ring_ || sample.ring_valid;
    const bool visual_boundary = update.progress.ring_validity_changed ||
                                 (update.progress.ring_generation_changed && sample.ring_valid);
    const bool visual_stall_candidate =
        update.progress.visual_stall_candidate() ||
        update.progress.invalid_ring_visual_stall_candidate(previously_saw_valid_ring);

    if (visual_boundary) {
      ResetVisualEpisode();
    } else if (visual_stall_candidate) {
      update.visual_stall_started = visual_stall_samples_ == 0;
      ++visual_stall_samples_;
      if (visual_stall_samples_ >= visual_stall_threshold_ && !visual_stall_logged_) {
        visual_stall_logged_ = true;
        update.log_visual_stall = true;
      }
    } else {
      visual_stall_samples_ = 0;
      visual_stall_logged_ = false;
    }

    if (update.progress.no_guest_present_candidate(sample.guest_present, minimum_present_count_)) {
      update.no_present_started = no_present_samples_ == 0;
      ++no_present_samples_;
      if (no_present_samples_ >= no_present_threshold_ && !no_present_logged_) {
        no_present_logged_ = true;
        update.log_no_present_stall = true;
      }
    } else {
      no_present_samples_ = 0;
      no_present_logged_ = false;
    }

    update.visual_stall_samples = visual_stall_samples_;
    update.no_present_samples = no_present_samples_;
    return update;
  }

  constexpr void Reset() {
    previous_ = {};
    saw_valid_ring_ = false;
    ResetEpisodes();
  }

 private:
  constexpr void ResetVisualEpisode() {
    visual_stall_samples_ = 0;
    visual_stall_logged_ = false;
  }

  constexpr void ResetEpisodes() {
    ResetVisualEpisode();
    no_present_samples_ = 0;
    no_present_logged_ = false;
  }

  ProgressSample previous_;
  uint32_t visual_stall_threshold_;
  uint32_t no_present_threshold_;
  uint32_t minimum_present_count_;
  uint32_t visual_stall_samples_ = 0;
  uint32_t no_present_samples_ = 0;
  bool visual_stall_logged_ = false;
  bool no_present_logged_ = false;
  bool saw_valid_ring_ = false;
};

}  // namespace ge::watchdog
