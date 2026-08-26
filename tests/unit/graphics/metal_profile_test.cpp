#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include <rex/graphics/metal/profile.h>
#include <rex/graphics/gpu_chain_profile.h>

namespace {

namespace profiling = rex::graphics::metal::profiling;
using rex::graphics::GpuChainProfiler;
using rex::graphics::GpuChainProfileSnapshot;

TEST_CASE("GPU chain profile correlates WPTR, interrupt, batch and throttle",
          "[graphics][metal][profile]") {
  GpuChainProfiler profile;
  profile.RecordWritePointer(0, false, false, 0, 100);
  profile.RecordInterrupt(110);
  profile.RecordIsrDispatch(12);
  profile.RecordWritePointer(1, true, false, 8, 150);
  profile.RecordWritePointer(2, true, true, 0, 160);
  profile.RecordRingBatchStart(2, 200);
  profile.RecordRingBatchEnd(2, 3, false, true);
  profile.RecordMetalPending(7);
  profile.RecordHandoffWait(120000, 130000, true, false, true);
  profile.RecordHandoffWait(120000, 140000, false, false, false);
  profile.RecordHandoffWait(120000, 50000, false, true, false);
  profile.RecordHandoffWaitSkipped(true);

  const GpuChainProfileSnapshot snapshot = profile.Snapshot();
  CHECK(snapshot.wptr_attempts == 3);
  CHECK(snapshot.wptr_accepted == 2);
  CHECK(snapshot.wptr_rejected == 1);
  CHECK(snapshot.wptr_same == 1);
  CHECK(snapshot.wptr_with_work == 1);
  CHECK(snapshot.wptr_zero_work == 1);
  CHECK(snapshot.wptr_coalesced == 1);
  CHECK(snapshot.wptr_pending_dwords_total == 8);
  CHECK(snapshot.wptr_pending_dwords_max == 8);
  CHECK(snapshot.interrupt_to_wptr_samples == 1);
  CHECK(snapshot.interrupt_to_wptr_total_ns == 40);
  CHECK(snapshot.wptr_to_worker_samples == 1);
  CHECK(snapshot.wptr_to_worker_total_ns == 50);
  CHECK(snapshot.ring_commit_rejected == 1);
  CHECK(snapshot.ring_concurrent_wptr == 1);
  CHECK(snapshot.ring_pending_after_commit == 1);
  CHECK(snapshot.metal_pending_total == 7);
  CHECK(snapshot.metal_pending_max == 7);
  CHECK(snapshot.throttle_actual_ns == 320000);
  CHECK(snapshot.throttle_overshoot_ns == 30000);
  CHECK(snapshot.throttle_ring_pending == 1);
  CHECK(snapshot.throttle_metal_pending == 2);
  CHECK(snapshot.throttle_wptr_wakes == 1);
  CHECK(snapshot.throttle_timeouts == 1);
  CHECK(snapshot.throttle_reconfigured == 1);
  CHECK(snapshot.throttle_skipped_pending == 1);
}

TEST_CASE("GPU chain profile retains maxima while delta counters stay windowed",
          "[graphics][metal][profile]") {
  GpuChainProfiler profile;
  profile.RecordInterrupt(10);
  profile.RecordInterrupt(20);
  CHECK(profile.Snapshot().interrupt_unpaired == 1);
  CHECK(profile.Snapshot().interrupt_outstanding == 1);
  profile.RecordWritePointer(1, true, false, 4, 25);
  const GpuChainProfileSnapshot first = profile.Snapshot();
  CHECK(first.interrupt_to_wptr_total_ns == 5);
  CHECK(first.interrupt_outstanding == 0);

  profile.RecordMetalPending(9);
  profile.RecordHandoffWait(100, 150, false, false, true);
  const GpuChainProfileSnapshot current = profile.Snapshot();
  const GpuChainProfileSnapshot delta = GpuChainProfileSnapshot::Delta(current, first);
  CHECK(delta.wptr_accepted == 0);
  CHECK(delta.metal_pending_total == 9);
  CHECK(delta.metal_pending_max == 9);
  CHECK(delta.throttle_calls == 1);
  CHECK(delta.throttle_max_ns == 150);
}

TEST_CASE("Metal profile duration aggregation is swap-bounded", "[graphics][metal][profile]") {
  profiling::DurationWindow duration;
  duration.Add(10);
  duration.Add(30);
  duration.EndSwap();
  duration.Add(7);
  duration.EndSwap();

  CHECK(duration.total.call_count == 3);
  CHECK(duration.total.total_ns == 47);
  CHECK(duration.total.max_call_ns == 30);
  CHECK(duration.max_swap_ns == 40);
  CHECK(duration.max_calls_per_swap == 2);
}

TEST_CASE("Metal command profile reports exactly every 64 swaps", "[graphics][metal][profile]") {
  profiling::CommandProfileWindow window;
  for (uint32_t swap = 0; swap < profiling::kReportInterval; ++swap) {
    window.Record(profiling::CommandEvent::kIssueDraw, swap + 1);
    window.RecordWait(profiling::WaitReason::kGlobalCap, 100 + swap, swap % 3);
    CHECK(window.EndSwap() == (swap + 1 == profiling::kReportInterval));
  }

  const auto& draw = window.event(profiling::CommandEvent::kIssueDraw);
  CHECK(draw.total.call_count == profiling::kReportInterval);
  CHECK(draw.total.total_ns == 2080);
  CHECK(draw.total.max_call_ns == 64);
  CHECK(draw.max_swap_ns == 64);

  const auto& wait = window.wait(profiling::WaitReason::kGlobalCap);
  CHECK(wait.duration.total.call_count == profiling::kReportInterval);
  CHECK(wait.waited_call_count == 42);
  CHECK(wait.waited_submission_count == 63);
  CHECK(wait.max_waited_submissions_per_call == 2);
  CHECK(wait.max_waited_submissions_per_swap == 2);

  window.Reset();
  CHECK(window.swap_count() == 0);
  CHECK(window.event(profiling::CommandEvent::kIssueDraw).total.call_count == 0);
}

TEST_CASE("Metal wait reasons and presenter counters remain distinct",
          "[graphics][metal][profile]") {
  CHECK(std::string_view(profiling::CommandEventName(profiling::CommandEvent::kWaitRegMem)) ==
        "wait_reg_mem");
  CHECK(std::string_view(profiling::CommandEventName(profiling::CommandEvent::kDrawProbeSample)) ==
        "draw_probe_sample");
  CHECK(std::string_view(profiling::CommandEventName(profiling::CommandEvent::kDrawRenderSample)) ==
        "draw_render_sample");
  CHECK(profiling::GetWaitReason("resource-mutation") == profiling::WaitReason::kResourceMutation);
  CHECK(profiling::GetWaitReason("global-cap") == profiling::WaitReason::kGlobalCap);
  CHECK(profiling::GetWaitReason("future-reason") == profiling::WaitReason::kOther);
  CHECK(profiling::GetWaitReason(nullptr) == profiling::WaitReason::kOther);

  profiling::PresenterProfileWindow presenter;
  presenter.RecordSource(false);
  presenter.RecordSource(true);
  presenter.Record(profiling::PresenterEvent::kNextDrawable, 40);
  presenter.Record(profiling::PresenterEvent::kUpload, 20);
  presenter.RecordUpload(4096);
  presenter.Record(profiling::PresenterEvent::kPresentCommit, 10);
  presenter.RecordCommit();
  CHECK_FALSE(presenter.EndAttempt());

  CHECK(presenter.source_count() == 2);
  CHECK(presenter.unchanged_source_count() == 1);
  CHECK(presenter.upload_count() == 1);
  CHECK(presenter.upload_byte_count() == 4096);
  CHECK(presenter.commit_count() == 1);
  CHECK(presenter.event(profiling::PresenterEvent::kUpload).total.total_ns == 20);
}

}  // namespace
