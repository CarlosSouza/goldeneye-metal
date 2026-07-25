#include "ge_player_stuck_telemetry.h"

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

ge::player_stuck::Sample EligibleSample(size_t index) {
  ge::player_stuck::Sample sample;
  sample.monotonic_ms = index * ge::player_stuck::kSampleIntervalMs;
  sample.input_poll = index * 30;
  sample.guest_frame = static_cast<uint32_t>(100 + index * 30);
  sample.present = static_cast<uint32_t>(200 + index * 30);
  sample.focused = true;
  sample.input_active = true;
  sample.controller_connected = true;
  sample.controller_device_id = 44;
  sample.guest_ly = 20000;
  sample.player_valid = true;
  sample.position_valid = true;
  sample.player = 0x40010000;
  sample.coordinates = 0x40020000;
  sample.position_x = 10.0f;
  sample.position_y = 20.0f;
  sample.position_z = 30.0f;
  return sample;
}

void TestDetectsSustainedMovementWithoutPositionProgress() {
  ge::player_stuck::Tracker tracker;
  for (size_t index = 0; index + 1 < ge::player_stuck::kSampleCapacity; ++index) {
    CHECK_TRUE(!tracker.Observe(EligibleSample(index)));
  }
  const auto report = tracker.Observe(EligibleSample(ge::player_stuck::kSampleCapacity - 1));
  CHECK_TRUE(report.has_value());
  if (report) {
    CHECK_TRUE(report->sample_count == ge::player_stuck::kSampleCapacity);
    CHECK_TRUE(report->trailing_movement_sample_count == ge::player_stuck::kSampleCapacity);
    CHECK_TRUE(report->frame_progress_intervals == ge::player_stuck::kSampleCapacity - 1);
    CHECK_TRUE(report->present_progress_intervals == ge::player_stuck::kSampleCapacity - 1);
    CHECK_TRUE(report->trailing_frame_progress_intervals == ge::player_stuck::kSampleCapacity - 1);
    CHECK_TRUE(report->trailing_present_progress_intervals ==
               ge::player_stuck::kSampleCapacity - 1);
    CHECK_TRUE(report->samples.front().monotonic_ms == 0);
    CHECK_TRUE(report->samples.back().monotonic_ms ==
               (ge::player_stuck::kSampleCapacity - 1) * ge::player_stuck::kSampleIntervalMs);
    CHECK_TRUE(report->maximum_distance_squared == 0.0f);
  }
}

void TestRequiresLiveFrameAndPresentProgress() {
  for (int stopped_counter = 0; stopped_counter < 2; ++stopped_counter) {
    ge::player_stuck::Tracker tracker;
    for (size_t index = 0; index < ge::player_stuck::kSampleCapacity; ++index) {
      auto sample = EligibleSample(index);
      if (stopped_counter == 0) {
        sample.guest_frame = 100;
      } else {
        sample.present = 200;
      }
      CHECK_TRUE(!tracker.Observe(sample));
    }
  }

  // One late counter tick is not enough to classify the game as live.
  for (int sparse_counter = 0; sparse_counter < 2; ++sparse_counter) {
    ge::player_stuck::Tracker tracker;
    for (size_t index = 0; index < ge::player_stuck::kSampleCapacity; ++index) {
      auto sample = EligibleSample(index);
      if (sparse_counter == 0) {
        sample.guest_frame = index + 1 == ge::player_stuck::kSampleCapacity ? 101 : 100;
      } else {
        sample.present = index + 1 == ge::player_stuck::kSampleCapacity ? 201 : 200;
      }
      CHECK_TRUE(!tracker.Observe(sample));
    }
  }

  // Earlier progress followed by a multi-second render stall is not live.
  for (int stale_counter = 0; stale_counter < 2; ++stale_counter) {
    ge::player_stuck::Tracker tracker;
    for (size_t index = 0; index < ge::player_stuck::kSampleCapacity; ++index) {
      auto sample = EligibleSample(index);
      if (stale_counter == 0 && index > ge::player_stuck::kRequiredProgressIntervals) {
        sample.guest_frame =
            static_cast<uint32_t>(100 + ge::player_stuck::kRequiredProgressIntervals * 30);
      } else if (stale_counter == 1 && index > ge::player_stuck::kRequiredProgressIntervals) {
        sample.present =
            static_cast<uint32_t>(200 + ge::player_stuck::kRequiredProgressIntervals * 30);
      }
      CHECK_TRUE(!tracker.Observe(sample));
    }
  }
}

void TestMovementAndNormalGatesPreventFalseReports() {
  {
    ge::player_stuck::Tracker tracker;
    for (size_t index = 0; index < ge::player_stuck::kSampleCapacity; ++index) {
      auto sample = EligibleSample(index);
      sample.position_x += static_cast<float>(index);
      CHECK_TRUE(!tracker.Observe(sample));
    }
  }

  for (int blocked_gate = 0; blocked_gate < 7; ++blocked_gate) {
    ge::player_stuck::Tracker tracker;
    for (size_t index = 0; index < ge::player_stuck::kSampleCapacity; ++index) {
      auto sample = EligibleSample(index);
      switch (blocked_gate) {
        case 0:
          sample.focused = false;
          break;
        case 1:
          sample.input_active = false;
          break;
        case 2:
          sample.pause = 1;
          break;
        case 3:
          sample.control_disabled = 1;
          break;
        case 4:
          sample.watch = 1;
          break;
        case 5:
          sample.player_valid = false;
          break;
        case 6:
          sample.position_valid = false;
          break;
      }
      CHECK_TRUE(!tracker.Observe(sample));
    }
  }

  {
    ge::player_stuck::Tracker tracker;
    for (size_t index = 0; index < ge::player_stuck::kSampleCapacity; ++index) {
      auto sample = EligibleSample(index);
      if (index == ge::player_stuck::kSampleCapacity / 2) {
        sample.coordinates += 0x100;
      }
      CHECK_TRUE(!tracker.Observe(sample));
    }
  }
}

void TestRequiresRepeatedMovementIntent() {
  ge::player_stuck::Tracker tracker;
  for (size_t index = 0; index < ge::player_stuck::kSampleCapacity; ++index) {
    auto sample = EligibleSample(index);
    // Six early movement samples followed by neutral input must not be
    // misclassified as sustained current intent.
    if (index >= ge::player_stuck::kRequiredTrailingMovementSamples) {
      sample.guest_ly = 0;
    }
    CHECK_TRUE(!tracker.Observe(sample));
  }
}

void TestReportIsOneShotUntilPlayerEscapes() {
  ge::player_stuck::Tracker tracker;
  for (size_t index = 0; index < ge::player_stuck::kSampleCapacity; ++index) {
    (void)tracker.Observe(EligibleSample(index));
  }

  for (size_t index = ge::player_stuck::kSampleCapacity;
       index < ge::player_stuck::kSampleCapacity * 3; ++index) {
    CHECK_TRUE(!tracker.Observe(EligibleSample(index)));
  }

  auto escaped = EligibleSample(ge::player_stuck::kSampleCapacity * 3);
  escaped.position_x += ge::player_stuck::kRearmDistance;
  CHECK_TRUE(!tracker.Observe(escaped));

  bool reported_again = false;
  for (size_t offset = 1; offset <= ge::player_stuck::kSampleCapacity; ++offset) {
    auto sample = EligibleSample(ge::player_stuck::kSampleCapacity * 3 + offset);
    sample.position_x = escaped.position_x;
    reported_again |= tracker.Observe(sample).has_value();
  }
  CHECK_TRUE(reported_again);
}

void TestReportRearmsAfterNeutralInputOrPause() {
  for (int rearm_reason = 0; rearm_reason < 3; ++rearm_reason) {
    ge::player_stuck::Tracker tracker;
    size_t index = 0;
    for (; index < ge::player_stuck::kSampleCapacity; ++index) {
      (void)tracker.Observe(EligibleSample(index));
    }

    auto rearm = EligibleSample(index++);
    if (rearm_reason == 0) {
      rearm.guest_ly = 0;
    } else if (rearm_reason == 1) {
      rearm.pause = 1;
    } else {
      rearm.coordinates += 0x100;
    }
    CHECK_TRUE(!tracker.Observe(rearm));

    bool reported_again = false;
    for (size_t count = 0; count < ge::player_stuck::kSampleCapacity; ++count, ++index) {
      auto sample = EligibleSample(index);
      if (rearm_reason == 2) {
        sample.coordinates = rearm.coordinates;
      }
      reported_again |= tracker.Observe(sample).has_value();
    }
    CHECK_TRUE(reported_again);
  }
}

void TestSamplingIntervalIsBounded() {
  ge::player_stuck::Tracker tracker;
  CHECK_TRUE(tracker.Due(1000));
  auto sample = EligibleSample(0);
  sample.monotonic_ms = 1000;
  CHECK_TRUE(!tracker.Observe(sample));
  CHECK_TRUE(!tracker.Due(1499));
  CHECK_TRUE(tracker.Due(1500));

  sample.monotonic_ms = 900;
  CHECK_TRUE(!tracker.Observe(sample));
  CHECK_TRUE(!tracker.Due(900));
  CHECK_TRUE(tracker.Due(1400));
}

void TestLongSamplingGapResetsPartialWindow() {
  ge::player_stuck::Tracker tracker;
  size_t index = 0;
  for (; index + 2 < ge::player_stuck::kSampleCapacity; ++index) {
    CHECK_TRUE(!tracker.Observe(EligibleSample(index)));
  }

  const size_t gap_index = index;
  auto after_gap = EligibleSample(index++);
  after_gap.monotonic_ms += ge::player_stuck::kRearmGapMs + 1;
  const uint64_t gap_offset =
      after_gap.monotonic_ms - gap_index * ge::player_stuck::kSampleIntervalMs;
  CHECK_TRUE(!tracker.Observe(after_gap));

  // Fewer than a complete fresh window after the gap cannot report.
  for (size_t count = 1; count + 1 < ge::player_stuck::kSampleCapacity; ++count, ++index) {
    auto sample = EligibleSample(index);
    sample.monotonic_ms += gap_offset;
    CHECK_TRUE(!tracker.Observe(sample));
  }
}

}  // namespace

int main() {
  TestDetectsSustainedMovementWithoutPositionProgress();
  TestRequiresLiveFrameAndPresentProgress();
  TestMovementAndNormalGatesPreventFalseReports();
  TestRequiresRepeatedMovementIntent();
  TestReportIsOneShotUntilPlayerEscapes();
  TestReportRearmsAfterNeutralInputOrPause();
  TestSamplingIntervalIsBounded();
  TestLongSamplingGapResetsPartialWindow();
  if (failures != 0) {
    std::fprintf(stderr, "%d player-stuck telemetry test(s) failed\n", failures);
    return 1;
  }
  std::puts("player-stuck telemetry tests passed");
  return 0;
}
