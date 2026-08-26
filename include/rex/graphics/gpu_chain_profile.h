/**
 ******************************************************************************
 * ReXGlue - Xbox 360 recompilation runtime                                  *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                       *
 *                                                                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace rex::graphics {

struct GpuChainProfileSnapshot {
  uint64_t wptr_attempts = 0;
  uint64_t wptr_accepted = 0;
  uint64_t wptr_rejected = 0;
  uint64_t wptr_same = 0;
  uint64_t wptr_with_work = 0;
  uint64_t wptr_zero_work = 0;
  uint64_t wptr_coalesced = 0;
  uint64_t wptr_pending_dwords_total = 0;
  uint64_t wptr_pending_dwords_max = 0;
  uint64_t wptr_to_worker_samples = 0;
  uint64_t wptr_to_worker_total_ns = 0;
  uint64_t wptr_to_worker_max_ns = 0;
  uint64_t pm4_interrupts = 0;
  uint64_t isr_dispatches = 0;
  uint64_t isr_total_ns = 0;
  uint64_t isr_max_ns = 0;
  uint64_t interrupt_to_wptr_samples = 0;
  uint64_t interrupt_to_wptr_total_ns = 0;
  uint64_t interrupt_to_wptr_max_ns = 0;
  uint64_t interrupt_unpaired = 0;
  uint64_t interrupt_outstanding = 0;
  uint64_t ring_batches = 0;
  uint64_t ring_commit_rejected = 0;
  uint64_t ring_concurrent_wptr = 0;
  uint64_t ring_pending_after_commit = 0;
  uint64_t metal_pending_samples = 0;
  uint64_t metal_pending_total = 0;
  uint64_t metal_pending_max = 0;
  uint64_t metal_pending_nonzero = 0;
  uint64_t throttle_calls = 0;
  uint64_t throttle_requested_ns = 0;
  uint64_t throttle_actual_ns = 0;
  uint64_t throttle_max_ns = 0;
  uint64_t throttle_overshoot_ns = 0;
  uint64_t throttle_ring_pending = 0;
  uint64_t throttle_metal_pending = 0;
  uint64_t throttle_wptr_wakes = 0;
  uint64_t throttle_timeouts = 0;
  uint64_t throttle_reconfigured = 0;
  uint64_t throttle_skipped_pending = 0;

  [[nodiscard]] static GpuChainProfileSnapshot Delta(
      const GpuChainProfileSnapshot& current, const GpuChainProfileSnapshot& previous) noexcept {
    GpuChainProfileSnapshot delta;
#define REX_GPU_CHAIN_DELTA(field) \
  delta.field = current.field >= previous.field ? current.field - previous.field : current.field
    REX_GPU_CHAIN_DELTA(wptr_attempts);
    REX_GPU_CHAIN_DELTA(wptr_accepted);
    REX_GPU_CHAIN_DELTA(wptr_rejected);
    REX_GPU_CHAIN_DELTA(wptr_same);
    REX_GPU_CHAIN_DELTA(wptr_with_work);
    REX_GPU_CHAIN_DELTA(wptr_zero_work);
    REX_GPU_CHAIN_DELTA(wptr_coalesced);
    REX_GPU_CHAIN_DELTA(wptr_pending_dwords_total);
    REX_GPU_CHAIN_DELTA(wptr_to_worker_samples);
    REX_GPU_CHAIN_DELTA(wptr_to_worker_total_ns);
    REX_GPU_CHAIN_DELTA(pm4_interrupts);
    REX_GPU_CHAIN_DELTA(isr_dispatches);
    REX_GPU_CHAIN_DELTA(isr_total_ns);
    REX_GPU_CHAIN_DELTA(interrupt_to_wptr_samples);
    REX_GPU_CHAIN_DELTA(interrupt_to_wptr_total_ns);
    REX_GPU_CHAIN_DELTA(interrupt_unpaired);
    REX_GPU_CHAIN_DELTA(ring_batches);
    REX_GPU_CHAIN_DELTA(ring_commit_rejected);
    REX_GPU_CHAIN_DELTA(ring_concurrent_wptr);
    REX_GPU_CHAIN_DELTA(ring_pending_after_commit);
    REX_GPU_CHAIN_DELTA(metal_pending_samples);
    REX_GPU_CHAIN_DELTA(metal_pending_total);
    REX_GPU_CHAIN_DELTA(metal_pending_nonzero);
    REX_GPU_CHAIN_DELTA(throttle_calls);
    REX_GPU_CHAIN_DELTA(throttle_requested_ns);
    REX_GPU_CHAIN_DELTA(throttle_actual_ns);
    REX_GPU_CHAIN_DELTA(throttle_overshoot_ns);
    REX_GPU_CHAIN_DELTA(throttle_ring_pending);
    REX_GPU_CHAIN_DELTA(throttle_metal_pending);
    REX_GPU_CHAIN_DELTA(throttle_wptr_wakes);
    REX_GPU_CHAIN_DELTA(throttle_timeouts);
    REX_GPU_CHAIN_DELTA(throttle_reconfigured);
    REX_GPU_CHAIN_DELTA(throttle_skipped_pending);
#undef REX_GPU_CHAIN_DELTA
    // Maxima and live state are lifetime/current values, not additive counters.
    delta.wptr_pending_dwords_max = current.wptr_pending_dwords_max;
    delta.wptr_to_worker_max_ns = current.wptr_to_worker_max_ns;
    delta.isr_max_ns = current.isr_max_ns;
    delta.interrupt_to_wptr_max_ns = current.interrupt_to_wptr_max_ns;
    delta.interrupt_outstanding = current.interrupt_outstanding;
    delta.metal_pending_max = current.metal_pending_max;
    delta.throttle_max_ns = current.throttle_max_ns;
    return delta;
  }
};

// Shadow-only telemetry for replacing GoldenEye's fixed command-ring throttle.
// Nothing here blocks or changes command execution; callers invoke it only
// while Metal profiling is enabled.
class GpuChainProfiler {
 public:
  void RecordWritePointer(uint64_t epoch, bool accepted, bool same_value, uint32_t pending_dwords,
                          uint64_t now_ns) noexcept {
    ++wptr_attempts_;
    if (!accepted) {
      ++wptr_rejected_;
      return;
    }
    ++wptr_accepted_;
    if (same_value) {
      ++wptr_same_;
    }
    if (pending_dwords) {
      ++wptr_with_work_;
    } else {
      ++wptr_zero_work_;
    }
    wptr_pending_dwords_total_.fetch_add(pending_dwords, std::memory_order_relaxed);
    UpdateMaximum(wptr_pending_dwords_max_, pending_dwords);
    if (!pending_dwords) {
      return;
    }

    std::lock_guard<std::mutex> lock(correlation_mutex_);
    if (!first_pending_wptr_epoch_) {
      first_pending_wptr_epoch_ = epoch;
      first_pending_wptr_ns_ = now_ns;
    }
    last_accepted_wptr_epoch_ = epoch;
    last_accepted_wptr_ns_ = now_ns;
    if (outstanding_interrupt_ns_) {
      RecordDuration(interrupt_to_wptr_samples_, interrupt_to_wptr_total_ns_,
                     interrupt_to_wptr_max_ns_, now_ns - outstanding_interrupt_ns_);
      outstanding_interrupt_ns_ = 0;
    }
  }

  void RecordInterrupt(uint64_t now_ns) noexcept {
    ++pm4_interrupts_;
    std::lock_guard<std::mutex> lock(correlation_mutex_);
    if (outstanding_interrupt_ns_) {
      ++interrupt_unpaired_;
    }
    outstanding_interrupt_ns_ = now_ns;
  }

  void RecordIsrDispatch(uint64_t duration_ns) noexcept {
    RecordDuration(isr_dispatches_, isr_total_ns_, isr_max_ns_, duration_ns);
  }

  void RecordRingBatchStart(uint64_t accepted_wptr_epoch, uint64_t now_ns) noexcept {
    ++ring_batches_;
    std::lock_guard<std::mutex> lock(correlation_mutex_);
    if (accepted_wptr_epoch > last_worker_wptr_epoch_ + 1) {
      wptr_coalesced_.fetch_add(accepted_wptr_epoch - last_worker_wptr_epoch_ - 1,
                                std::memory_order_relaxed);
    }
    last_worker_wptr_epoch_ = accepted_wptr_epoch;
    if (first_pending_wptr_epoch_ && first_pending_wptr_epoch_ <= accepted_wptr_epoch) {
      RecordDuration(wptr_to_worker_samples_, wptr_to_worker_total_ns_, wptr_to_worker_max_ns_,
                     now_ns - first_pending_wptr_ns_);
      first_pending_wptr_epoch_ = 0;
      first_pending_wptr_ns_ = 0;
      if (last_accepted_wptr_epoch_ > accepted_wptr_epoch) {
        first_pending_wptr_epoch_ = last_accepted_wptr_epoch_;
        first_pending_wptr_ns_ = last_accepted_wptr_ns_;
      }
    }
  }

  void RecordRingBatchEnd(uint64_t start_wptr_epoch, uint64_t end_wptr_epoch, bool commit_accepted,
                          bool ring_pending) noexcept {
    if (!commit_accepted) {
      ++ring_commit_rejected_;
    }
    if (end_wptr_epoch > start_wptr_epoch) {
      ++ring_concurrent_wptr_;
    }
    if (ring_pending) {
      ++ring_pending_after_commit_;
    }
  }

  void RecordMetalPending(uint32_t pending_submissions) noexcept {
    ++metal_pending_samples_;
    metal_pending_total_.fetch_add(pending_submissions, std::memory_order_relaxed);
    UpdateMaximum(metal_pending_max_, pending_submissions);
    if (pending_submissions) {
      ++metal_pending_nonzero_;
    }
  }

  void RecordHandoffWait(uint64_t requested_ns, uint64_t actual_ns, bool wptr_wake,
                         bool reconfigured, bool metal_pending) noexcept {
    ++throttle_calls_;
    throttle_requested_ns_.fetch_add(requested_ns, std::memory_order_relaxed);
    throttle_actual_ns_.fetch_add(actual_ns, std::memory_order_relaxed);
    UpdateMaximum(throttle_max_ns_, actual_ns);
    if (actual_ns > requested_ns) {
      throttle_overshoot_ns_.fetch_add(actual_ns - requested_ns, std::memory_order_relaxed);
    }
    if (metal_pending) {
      ++throttle_metal_pending_;
    }
    if (wptr_wake) {
      ++throttle_wptr_wakes_;
    } else if (reconfigured) {
      ++throttle_reconfigured_;
    } else {
      ++throttle_timeouts_;
    }
  }

  void RecordHandoffWaitSkipped(bool metal_pending) noexcept {
    ++throttle_ring_pending_;
    ++throttle_skipped_pending_;
    if (metal_pending) {
      ++throttle_metal_pending_;
    }
  }

  [[nodiscard]] GpuChainProfileSnapshot Snapshot() const noexcept {
    GpuChainProfileSnapshot snapshot;
#define REX_GPU_CHAIN_LOAD(field) snapshot.field = field##_.load(std::memory_order_relaxed)
    REX_GPU_CHAIN_LOAD(wptr_attempts);
    REX_GPU_CHAIN_LOAD(wptr_accepted);
    REX_GPU_CHAIN_LOAD(wptr_rejected);
    REX_GPU_CHAIN_LOAD(wptr_same);
    REX_GPU_CHAIN_LOAD(wptr_with_work);
    REX_GPU_CHAIN_LOAD(wptr_zero_work);
    REX_GPU_CHAIN_LOAD(wptr_coalesced);
    REX_GPU_CHAIN_LOAD(wptr_pending_dwords_total);
    REX_GPU_CHAIN_LOAD(wptr_pending_dwords_max);
    REX_GPU_CHAIN_LOAD(wptr_to_worker_samples);
    REX_GPU_CHAIN_LOAD(wptr_to_worker_total_ns);
    REX_GPU_CHAIN_LOAD(wptr_to_worker_max_ns);
    REX_GPU_CHAIN_LOAD(pm4_interrupts);
    REX_GPU_CHAIN_LOAD(isr_dispatches);
    REX_GPU_CHAIN_LOAD(isr_total_ns);
    REX_GPU_CHAIN_LOAD(isr_max_ns);
    REX_GPU_CHAIN_LOAD(interrupt_to_wptr_samples);
    REX_GPU_CHAIN_LOAD(interrupt_to_wptr_total_ns);
    REX_GPU_CHAIN_LOAD(interrupt_to_wptr_max_ns);
    REX_GPU_CHAIN_LOAD(interrupt_unpaired);
    REX_GPU_CHAIN_LOAD(ring_batches);
    REX_GPU_CHAIN_LOAD(ring_commit_rejected);
    REX_GPU_CHAIN_LOAD(ring_concurrent_wptr);
    REX_GPU_CHAIN_LOAD(ring_pending_after_commit);
    REX_GPU_CHAIN_LOAD(metal_pending_samples);
    REX_GPU_CHAIN_LOAD(metal_pending_total);
    REX_GPU_CHAIN_LOAD(metal_pending_max);
    REX_GPU_CHAIN_LOAD(metal_pending_nonzero);
    REX_GPU_CHAIN_LOAD(throttle_calls);
    REX_GPU_CHAIN_LOAD(throttle_requested_ns);
    REX_GPU_CHAIN_LOAD(throttle_actual_ns);
    REX_GPU_CHAIN_LOAD(throttle_max_ns);
    REX_GPU_CHAIN_LOAD(throttle_overshoot_ns);
    REX_GPU_CHAIN_LOAD(throttle_ring_pending);
    REX_GPU_CHAIN_LOAD(throttle_metal_pending);
    REX_GPU_CHAIN_LOAD(throttle_wptr_wakes);
    REX_GPU_CHAIN_LOAD(throttle_timeouts);
    REX_GPU_CHAIN_LOAD(throttle_reconfigured);
    REX_GPU_CHAIN_LOAD(throttle_skipped_pending);
#undef REX_GPU_CHAIN_LOAD
    std::lock_guard<std::mutex> lock(correlation_mutex_);
    snapshot.interrupt_outstanding = outstanding_interrupt_ns_ ? 1 : 0;
    return snapshot;
  }

 private:
  static void UpdateMaximum(std::atomic<uint64_t>& maximum, uint64_t value) noexcept {
    uint64_t current = maximum.load(std::memory_order_relaxed);
    while (current < value &&
           !maximum.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
  }

  static void RecordDuration(std::atomic<uint64_t>& samples, std::atomic<uint64_t>& total,
                             std::atomic<uint64_t>& maximum, uint64_t duration_ns) noexcept {
    ++samples;
    total.fetch_add(duration_ns, std::memory_order_relaxed);
    UpdateMaximum(maximum, duration_ns);
  }

#define REX_GPU_CHAIN_COUNTER(field) std::atomic<uint64_t> field##_{0}
  REX_GPU_CHAIN_COUNTER(wptr_attempts);
  REX_GPU_CHAIN_COUNTER(wptr_accepted);
  REX_GPU_CHAIN_COUNTER(wptr_rejected);
  REX_GPU_CHAIN_COUNTER(wptr_same);
  REX_GPU_CHAIN_COUNTER(wptr_with_work);
  REX_GPU_CHAIN_COUNTER(wptr_zero_work);
  REX_GPU_CHAIN_COUNTER(wptr_coalesced);
  REX_GPU_CHAIN_COUNTER(wptr_pending_dwords_total);
  REX_GPU_CHAIN_COUNTER(wptr_pending_dwords_max);
  REX_GPU_CHAIN_COUNTER(wptr_to_worker_samples);
  REX_GPU_CHAIN_COUNTER(wptr_to_worker_total_ns);
  REX_GPU_CHAIN_COUNTER(wptr_to_worker_max_ns);
  REX_GPU_CHAIN_COUNTER(pm4_interrupts);
  REX_GPU_CHAIN_COUNTER(isr_dispatches);
  REX_GPU_CHAIN_COUNTER(isr_total_ns);
  REX_GPU_CHAIN_COUNTER(isr_max_ns);
  REX_GPU_CHAIN_COUNTER(interrupt_to_wptr_samples);
  REX_GPU_CHAIN_COUNTER(interrupt_to_wptr_total_ns);
  REX_GPU_CHAIN_COUNTER(interrupt_to_wptr_max_ns);
  REX_GPU_CHAIN_COUNTER(interrupt_unpaired);
  REX_GPU_CHAIN_COUNTER(ring_batches);
  REX_GPU_CHAIN_COUNTER(ring_commit_rejected);
  REX_GPU_CHAIN_COUNTER(ring_concurrent_wptr);
  REX_GPU_CHAIN_COUNTER(ring_pending_after_commit);
  REX_GPU_CHAIN_COUNTER(metal_pending_samples);
  REX_GPU_CHAIN_COUNTER(metal_pending_total);
  REX_GPU_CHAIN_COUNTER(metal_pending_max);
  REX_GPU_CHAIN_COUNTER(metal_pending_nonzero);
  REX_GPU_CHAIN_COUNTER(throttle_calls);
  REX_GPU_CHAIN_COUNTER(throttle_requested_ns);
  REX_GPU_CHAIN_COUNTER(throttle_actual_ns);
  REX_GPU_CHAIN_COUNTER(throttle_max_ns);
  REX_GPU_CHAIN_COUNTER(throttle_overshoot_ns);
  REX_GPU_CHAIN_COUNTER(throttle_ring_pending);
  REX_GPU_CHAIN_COUNTER(throttle_metal_pending);
  REX_GPU_CHAIN_COUNTER(throttle_wptr_wakes);
  REX_GPU_CHAIN_COUNTER(throttle_timeouts);
  REX_GPU_CHAIN_COUNTER(throttle_reconfigured);
  REX_GPU_CHAIN_COUNTER(throttle_skipped_pending);
#undef REX_GPU_CHAIN_COUNTER

  mutable std::mutex correlation_mutex_;
  uint64_t first_pending_wptr_epoch_ = 0;
  uint64_t first_pending_wptr_ns_ = 0;
  uint64_t last_accepted_wptr_epoch_ = 0;
  uint64_t last_accepted_wptr_ns_ = 0;
  uint64_t last_worker_wptr_epoch_ = 0;
  uint64_t outstanding_interrupt_ns_ = 0;
};

}  // namespace rex::graphics
