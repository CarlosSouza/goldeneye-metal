#include <rex/system/critical_section_ledger.h>

#include <algorithm>

namespace rex::system {

namespace {

constexpr size_t kInvalidOwnershipIndex =
    CriticalSectionOwnershipLedger::kMaximumOwnedCriticalSections;
constexpr uint32_t kSnapshotAttemptCount = 8;

}  // namespace

CriticalSectionOwnershipLedger::WriteScope::WriteScope(std::atomic<uint64_t>& generation) noexcept
    : generation_(generation) {
  generation_.fetch_add(1, std::memory_order_acq_rel);
}

CriticalSectionOwnershipLedger::WriteScope::~WriteScope() {
  generation_.fetch_add(1, std::memory_order_release);
}

size_t CriticalSectionOwnershipLedger::FindOwnershipEntry(uint32_t critical_section,
                                                          size_t active_count) const noexcept {
  for (size_t index = 0; index < active_count; ++index) {
    if (ownership_entries_[index].critical_section.load(std::memory_order_relaxed) ==
        critical_section) {
      return index;
    }
  }
  return kInvalidOwnershipIndex;
}

void CriticalSectionOwnershipLedger::RemoveOwnershipEntry(size_t index,
                                                          size_t active_count) noexcept {
  const size_t last_index = active_count - 1;
  auto& destination = ownership_entries_[index];
  auto& source = ownership_entries_[last_index];
  if (index != last_index) {
    destination.critical_section.store(source.critical_section.load(std::memory_order_relaxed),
                                       std::memory_order_relaxed);
    destination.first_enter_lr.store(source.first_enter_lr.load(std::memory_order_relaxed),
                                     std::memory_order_relaxed);
    destination.last_enter_lr.store(source.last_enter_lr.load(std::memory_order_relaxed),
                                    std::memory_order_relaxed);
    destination.recorded_depth.store(source.recorded_depth.load(std::memory_order_relaxed),
                                     std::memory_order_relaxed);
    destination.enter_count.store(source.enter_count.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
    destination.provenance_incomplete.store(
        source.provenance_incomplete.load(std::memory_order_relaxed), std::memory_order_relaxed);
    destination.first_enter_sequence.store(
        source.first_enter_sequence.load(std::memory_order_relaxed), std::memory_order_relaxed);
    destination.last_event_sequence.store(
        source.last_event_sequence.load(std::memory_order_relaxed), std::memory_order_relaxed);
  }

  source.critical_section.store(0, std::memory_order_relaxed);
  source.first_enter_lr.store(0, std::memory_order_relaxed);
  source.last_enter_lr.store(0, std::memory_order_relaxed);
  source.recorded_depth.store(0, std::memory_order_relaxed);
  source.enter_count.store(0, std::memory_order_relaxed);
  source.provenance_incomplete.store(0, std::memory_order_relaxed);
  source.first_enter_sequence.store(0, std::memory_order_relaxed);
  source.last_event_sequence.store(0, std::memory_order_relaxed);
  active_count_.store(static_cast<uint32_t>(last_index), std::memory_order_relaxed);
}

void CriticalSectionOwnershipLedger::AppendLeaveMismatch(
    uint64_t sequence, uint32_t critical_section, uint32_t leave_lr, uint32_t current_thread,
    uint32_t observed_owner, int32_t observed_recursion,
    CriticalSectionLeaveMismatchReason reason) noexcept {
  const uint64_t mismatch_index = total_leave_mismatches_.fetch_add(1, std::memory_order_relaxed);
  auto& mismatch = leave_mismatches_[mismatch_index % kLeaveMismatchHistorySize];
  mismatch.sequence.store(sequence, std::memory_order_relaxed);
  mismatch.critical_section.store(critical_section, std::memory_order_relaxed);
  mismatch.leave_lr.store(leave_lr, std::memory_order_relaxed);
  mismatch.current_thread.store(current_thread, std::memory_order_relaxed);
  mismatch.observed_owner.store(observed_owner, std::memory_order_relaxed);
  mismatch.observed_recursion.store(observed_recursion, std::memory_order_relaxed);
  mismatch.reason.store(static_cast<uint32_t>(reason), std::memory_order_relaxed);
}

void CriticalSectionOwnershipLedger::RecordEnter(uint32_t critical_section, uint32_t enter_lr,
                                                 int32_t observed_recursion) noexcept {
  WriteScope write_scope(generation_);
  const uint64_t sequence = event_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t normalized_depth =
      observed_recursion > 0 ? static_cast<uint32_t>(observed_recursion) : 1;
  const size_t active_count = std::min<size_t>(active_count_.load(std::memory_order_relaxed),
                                               kMaximumOwnedCriticalSections);
  size_t index = FindOwnershipEntry(critical_section, active_count);

  if (index == kInvalidOwnershipIndex) {
    if (active_count == kMaximumOwnedCriticalSections) {
      dropped_ownership_records_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    index = active_count;
    active_count_.store(static_cast<uint32_t>(active_count + 1), std::memory_order_relaxed);
  }

  auto& entry = ownership_entries_[index];
  const uint32_t previous_depth = entry.recorded_depth.load(std::memory_order_relaxed);
  const bool fresh_acquisition =
      entry.critical_section.load(std::memory_order_relaxed) != critical_section ||
      normalized_depth == 1;
  if (fresh_acquisition) {
    entry.critical_section.store(critical_section, std::memory_order_relaxed);
    entry.first_enter_lr.store(enter_lr, std::memory_order_relaxed);
    entry.enter_count.store(1, std::memory_order_relaxed);
    entry.first_enter_sequence.store(sequence, std::memory_order_relaxed);
    entry.provenance_incomplete.store(observed_recursion == 1 ? 0u : 1u, std::memory_order_relaxed);
  } else {
    entry.enter_count.fetch_add(1, std::memory_order_relaxed);
    if (observed_recursion <= 0 || normalized_depth != previous_depth + 1) {
      entry.provenance_incomplete.store(1, std::memory_order_relaxed);
    }
  }
  entry.last_enter_lr.store(enter_lr, std::memory_order_relaxed);
  entry.recorded_depth.store(normalized_depth, std::memory_order_relaxed);
  entry.last_event_sequence.store(sequence, std::memory_order_relaxed);
}

void CriticalSectionOwnershipLedger::RecordLeave(uint32_t critical_section, uint32_t leave_lr,
                                                 uint32_t current_thread, uint32_t observed_owner,
                                                 int32_t observed_recursion_before,
                                                 int32_t observed_recursion_after) noexcept {
  WriteScope write_scope(generation_);
  const uint64_t sequence = event_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
  const size_t active_count = std::min<size_t>(active_count_.load(std::memory_order_relaxed),
                                               kMaximumOwnedCriticalSections);
  const size_t index = FindOwnershipEntry(critical_section, active_count);
  if (index == kInvalidOwnershipIndex) {
    AppendLeaveMismatch(sequence, critical_section, leave_lr, current_thread, observed_owner,
                        observed_recursion_before,
                        CriticalSectionLeaveMismatchReason::kNoOwnershipRecord);
    return;
  }

  auto& entry = ownership_entries_[index];
  const uint32_t recorded_depth = entry.recorded_depth.load(std::memory_order_relaxed);
  if (observed_recursion_before <= 0 || observed_recursion_after < 0 ||
      observed_recursion_after >= observed_recursion_before) {
    entry.provenance_incomplete.store(1, std::memory_order_relaxed);
    AppendLeaveMismatch(sequence, critical_section, leave_lr, current_thread, observed_owner,
                        observed_recursion_before,
                        CriticalSectionLeaveMismatchReason::kInvalidGuestRecursion);
    return;
  }
  if (recorded_depth != static_cast<uint32_t>(observed_recursion_before)) {
    entry.provenance_incomplete.store(1, std::memory_order_relaxed);
    AppendLeaveMismatch(sequence, critical_section, leave_lr, current_thread, observed_owner,
                        observed_recursion_before,
                        CriticalSectionLeaveMismatchReason::kRecordedDepthMismatch);
  }

  if (observed_recursion_after == 0) {
    RemoveOwnershipEntry(index, active_count);
    return;
  }
  entry.recorded_depth.store(static_cast<uint32_t>(observed_recursion_after),
                             std::memory_order_relaxed);
  entry.last_event_sequence.store(sequence, std::memory_order_relaxed);
}

void CriticalSectionOwnershipLedger::RecordLeaveMismatch(
    uint32_t critical_section, uint32_t leave_lr, uint32_t current_thread, uint32_t observed_owner,
    int32_t observed_recursion, CriticalSectionLeaveMismatchReason reason) noexcept {
  WriteScope write_scope(generation_);
  const uint64_t sequence = event_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
  AppendLeaveMismatch(sequence, critical_section, leave_lr, current_thread, observed_owner,
                      observed_recursion, reason);
}

bool CriticalSectionOwnershipLedger::Query(
    uint32_t critical_section, CriticalSectionOwnershipSnapshot* out_snapshot) const noexcept {
  if (!out_snapshot) {
    return false;
  }
  *out_snapshot = {};
  out_snapshot->critical_section = critical_section;

  for (uint32_t attempt = 0; attempt < kSnapshotAttemptCount; ++attempt) {
    const uint64_t generation_before = generation_.load(std::memory_order_acquire);
    if (generation_before & 1) {
      continue;
    }

    CriticalSectionOwnershipSnapshot snapshot;
    snapshot.critical_section = critical_section;
    snapshot.dropped_ownership_records = dropped_ownership_records_.load(std::memory_order_relaxed);
    snapshot.total_leave_mismatches = total_leave_mismatches_.load(std::memory_order_relaxed);

    const size_t active_count = std::min<size_t>(active_count_.load(std::memory_order_relaxed),
                                                 kMaximumOwnedCriticalSections);
    const size_t ownership_index = FindOwnershipEntry(critical_section, active_count);
    if (ownership_index != kInvalidOwnershipIndex) {
      const auto& entry = ownership_entries_[ownership_index];
      snapshot.ownership_found = true;
      snapshot.first_enter_lr = entry.first_enter_lr.load(std::memory_order_relaxed);
      snapshot.last_enter_lr = entry.last_enter_lr.load(std::memory_order_relaxed);
      snapshot.recorded_depth = entry.recorded_depth.load(std::memory_order_relaxed);
      snapshot.enter_count = entry.enter_count.load(std::memory_order_relaxed);
      snapshot.provenance_incomplete =
          entry.provenance_incomplete.load(std::memory_order_relaxed) != 0;
      snapshot.first_enter_sequence = entry.first_enter_sequence.load(std::memory_order_relaxed);
      snapshot.last_event_sequence = entry.last_event_sequence.load(std::memory_order_relaxed);
    }

    for (const auto& mismatch : leave_mismatches_) {
      const uint64_t sequence = mismatch.sequence.load(std::memory_order_relaxed);
      if (sequence == 0) {
        continue;
      }
      CriticalSectionLeaveMismatch mismatch_snapshot;
      mismatch_snapshot.sequence = sequence;
      mismatch_snapshot.critical_section =
          mismatch.critical_section.load(std::memory_order_relaxed);
      mismatch_snapshot.leave_lr = mismatch.leave_lr.load(std::memory_order_relaxed);
      mismatch_snapshot.current_thread = mismatch.current_thread.load(std::memory_order_relaxed);
      mismatch_snapshot.observed_owner = mismatch.observed_owner.load(std::memory_order_relaxed);
      mismatch_snapshot.observed_recursion =
          mismatch.observed_recursion.load(std::memory_order_relaxed);
      mismatch_snapshot.reason = static_cast<CriticalSectionLeaveMismatchReason>(
          mismatch.reason.load(std::memory_order_relaxed));

      if (!snapshot.most_recent_leave_mismatch_found ||
          sequence > snapshot.most_recent_leave_mismatch.sequence) {
        snapshot.most_recent_leave_mismatch_found = true;
        snapshot.most_recent_leave_mismatch = mismatch_snapshot;
      }
      if (mismatch_snapshot.critical_section == critical_section &&
          (!snapshot.recent_leave_mismatch_found ||
           sequence > snapshot.recent_leave_mismatch.sequence)) {
        snapshot.recent_leave_mismatch_found = true;
        snapshot.recent_leave_mismatch = mismatch_snapshot;
      }
    }

    const uint64_t generation_after = generation_.load(std::memory_order_acquire);
    if (generation_before == generation_after && !(generation_after & 1)) {
      snapshot.coherent = true;
      *out_snapshot = snapshot;
      return true;
    }
  }

  return false;
}

}  // namespace rex::system
