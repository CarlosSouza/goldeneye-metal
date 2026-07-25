#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ge::player_stuck {

inline constexpr uint64_t kSampleIntervalMs = 500;
inline constexpr size_t kSampleCapacity = 16;
inline constexpr size_t kRequiredTrailingMovementSamples = 6;
inline constexpr size_t kRequiredProgressIntervals = 8;
inline constexpr size_t kRequiredTrailingProgressIntervals = 4;
inline constexpr float kStationaryDistance = 2.0f;
inline constexpr float kRearmDistance = 25.0f;
inline constexpr uint64_t kRearmGapMs = 2000;

struct Sample {
  uint64_t monotonic_ms = 0;
  uint64_t input_poll = 0;
  uint32_t guest_frame = 0;
  uint32_t present = 0;

  bool focused = false;
  bool input_active = false;
  bool mouse_capture_active = false;
  bool controller_connected = false;
  uint64_t controller_device_id = 0;
  uint16_t controller_buttons = 0;
  int16_t controller_lx = 0;
  int16_t controller_ly = 0;

  uint16_t guest_buttons = 0;
  uint8_t guest_left_trigger = 0;
  uint8_t guest_right_trigger = 0;
  int16_t guest_lx = 0;
  int16_t guest_ly = 0;
  int16_t guest_rx = 0;
  int16_t guest_ry = 0;

  bool player_valid = false;
  bool position_valid = false;
  uint32_t player = 0;
  uint32_t coordinates = 0;
  float position_x = 0.0f;
  float position_y = 0.0f;
  float position_z = 0.0f;
  float camera_yaw = 0.0f;
  float camera_pitch = 0.0f;
  uint32_t pause = 0;
  uint32_t control_disabled = 0;
  uint32_t watch = 0;
};

struct Report {
  std::array<Sample, kSampleCapacity> samples{};
  size_t sample_count = 0;
  size_t trailing_movement_sample_count = 0;
  size_t frame_progress_intervals = 0;
  size_t present_progress_intervals = 0;
  size_t trailing_frame_progress_intervals = 0;
  size_t trailing_present_progress_intervals = 0;
  float maximum_distance_squared = 0.0f;
};

inline bool HasMovementIntent(const Sample& sample) {
  constexpr int kMovementThreshold = 12000;
  return std::abs(static_cast<int>(sample.guest_lx)) >= kMovementThreshold ||
         std::abs(static_cast<int>(sample.guest_ly)) >= kMovementThreshold;
}

inline bool IsEligible(const Sample& sample) {
  return sample.focused && sample.input_active && sample.player_valid && sample.position_valid &&
         sample.pause == 0 && sample.control_disabled == 0 && sample.watch == 0;
}

inline float DistanceSquared(const Sample& first, const Sample& second) {
  const float dx = second.position_x - first.position_x;
  const float dy = second.position_y - first.position_y;
  const float dz = second.position_z - first.position_z;
  return dx * dx + dy * dy + dz * dz;
}

inline bool CounterAdvanced(uint32_t previous, uint32_t current) {
  const uint32_t delta = current - previous;
  return delta != 0 && delta < 0x80000000u;
}

class Tracker {
 public:
  bool Due(uint64_t monotonic_ms) const {
    return !has_sample_time_ || monotonic_ms < last_sample_ms_ ||
           monotonic_ms - last_sample_ms_ >= kSampleIntervalMs;
  }

  std::optional<Report> Observe(const Sample& sample) {
    if (!Due(sample.monotonic_ms)) {
      return std::nullopt;
    }
    const bool clock_reset = has_sample_time_ && sample.monotonic_ms < last_sample_ms_;
    const bool long_gap =
        has_sample_time_ && !clock_reset && sample.monotonic_ms - last_sample_ms_ > kRearmGapMs;
    if (clock_reset || long_gap) {
      ResetWindow();
      latched_ = false;
    }
    has_sample_time_ = true;
    last_sample_ms_ = sample.monotonic_ms;

    if (latched_) {
      const bool new_player =
          sample.player_valid && latched_player_ != 0 && sample.player != latched_player_;
      const bool escaped =
          sample.position_valid && latched_position_valid_ &&
          DistanceSquared(sample, latched_position_) >= kRearmDistance * kRearmDistance;
      const bool new_coordinates = sample.position_valid && latched_position_valid_ &&
                                   sample.coordinates != latched_position_.coordinates;
      const bool counters_restarted =
          sample.guest_frame < latched_guest_frame_ || sample.present < latched_present_;
      if (!IsEligible(sample) || !HasMovementIntent(sample) || new_player || new_coordinates ||
          escaped || counters_restarted) {
        latched_ = false;
        ResetWindow();
      } else {
        return std::nullopt;
      }
    }

    if (!IsEligible(sample)) {
      ResetWindow();
      return std::nullopt;
    }

    samples_[next_index_] = sample;
    next_index_ = (next_index_ + 1) % kSampleCapacity;
    if (sample_count_ < kSampleCapacity) {
      ++sample_count_;
    }
    if (sample_count_ != kSampleCapacity) {
      return std::nullopt;
    }

    Report report = OrderedReport();
    const Sample& first = report.samples.front();
    const Sample& last = report.samples[report.sample_count - 1];
    const uint64_t required_window_ms = (kSampleCapacity - 1) * kSampleIntervalMs;
    if (last.monotonic_ms - first.monotonic_ms < required_window_ms) {
      return std::nullopt;
    }

    for (size_t index = 0; index < report.sample_count; ++index) {
      const Sample& candidate = report.samples[index];
      if (candidate.player != first.player || candidate.coordinates != first.coordinates) {
        return std::nullopt;
      }
      if (index != 0) {
        const Sample& previous = report.samples[index - 1];
        report.frame_progress_intervals +=
            CounterAdvanced(previous.guest_frame, candidate.guest_frame) ? 1 : 0;
        report.present_progress_intervals +=
            CounterAdvanced(previous.present, candidate.present) ? 1 : 0;
      }
      report.maximum_distance_squared =
          std::max(report.maximum_distance_squared, DistanceSquared(first, candidate));
    }
    for (size_t index = report.sample_count;
         index != 0 && HasMovementIntent(report.samples[index - 1]); --index) {
      ++report.trailing_movement_sample_count;
    }
    for (size_t index = report.sample_count - 1;
         index != 0 &&
         CounterAdvanced(report.samples[index - 1].guest_frame, report.samples[index].guest_frame);
         --index) {
      ++report.trailing_frame_progress_intervals;
    }
    for (size_t index = report.sample_count - 1;
         index != 0 &&
         CounterAdvanced(report.samples[index - 1].present, report.samples[index].present);
         --index) {
      ++report.trailing_present_progress_intervals;
    }
    if (report.trailing_movement_sample_count < kRequiredTrailingMovementSamples ||
        report.frame_progress_intervals < kRequiredProgressIntervals ||
        report.present_progress_intervals < kRequiredProgressIntervals ||
        report.trailing_frame_progress_intervals < kRequiredTrailingProgressIntervals ||
        report.trailing_present_progress_intervals < kRequiredTrailingProgressIntervals ||
        report.maximum_distance_squared > kStationaryDistance * kStationaryDistance) {
      return std::nullopt;
    }

    latched_ = true;
    latched_player_ = last.player;
    latched_position_ = last;
    latched_position_valid_ = true;
    latched_guest_frame_ = last.guest_frame;
    latched_present_ = last.present;
    return report;
  }

  void Reset() {
    samples_ = {};
    next_index_ = 0;
    sample_count_ = 0;
    has_sample_time_ = false;
    last_sample_ms_ = 0;
    latched_ = false;
    latched_player_ = 0;
    latched_position_ = {};
    latched_position_valid_ = false;
    latched_guest_frame_ = 0;
    latched_present_ = 0;
  }

 private:
  Report OrderedReport() const {
    Report report;
    report.sample_count = sample_count_;
    const size_t first_index = sample_count_ == kSampleCapacity ? next_index_ : 0;
    for (size_t index = 0; index < sample_count_; ++index) {
      report.samples[index] = samples_[(first_index + index) % kSampleCapacity];
    }
    return report;
  }

  void ResetWindow() {
    samples_ = {};
    next_index_ = 0;
    sample_count_ = 0;
  }

  std::array<Sample, kSampleCapacity> samples_{};
  size_t next_index_ = 0;
  size_t sample_count_ = 0;
  bool has_sample_time_ = false;
  uint64_t last_sample_ms_ = 0;

  bool latched_ = false;
  uint32_t latched_player_ = 0;
  Sample latched_position_{};
  bool latched_position_valid_ = false;
  uint32_t latched_guest_frame_ = 0;
  uint32_t latched_present_ = 0;
};

}  // namespace ge::player_stuck
