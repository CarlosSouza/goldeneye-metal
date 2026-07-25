#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rex::system {

enum class CriticalSectionLeaveMismatchReason : uint32_t {
  kNone = 0,
  kNoOwnershipRecord,
  kGuestOwnerMismatch,
  kInvalidGuestRecursion,
  kRecordedDepthMismatch,
};

struct CriticalSectionLeaveMismatch {
  uint64_t sequence = 0;
  uint32_t critical_section = 0;
  uint32_t leave_lr = 0;
  uint32_t current_thread = 0;
  uint32_t observed_owner = 0;
  int32_t observed_recursion = 0;
  CriticalSectionLeaveMismatchReason reason = CriticalSectionLeaveMismatchReason::kNone;
};

struct CriticalSectionOwnershipSnapshot {
  bool coherent = false;
  bool ownership_found = false;
  bool provenance_incomplete = false;
  uint32_t critical_section = 0;
  uint32_t first_enter_lr = 0;
  uint32_t last_enter_lr = 0;
  uint32_t recorded_depth = 0;
  uint32_t enter_count = 0;
  uint64_t first_enter_sequence = 0;
  uint64_t last_event_sequence = 0;
  uint32_t dropped_ownership_records = 0;
  uint64_t total_leave_mismatches = 0;
  bool recent_leave_mismatch_found = false;
  CriticalSectionLeaveMismatch recent_leave_mismatch;
  bool most_recent_leave_mismatch_found = false;
  CriticalSectionLeaveMismatch most_recent_leave_mismatch;
};

// A bounded, allocation-free provenance ledger owned by one XThread. Only the
// owning guest thread writes it; diagnostic threads may take atomic snapshots.
// Guest critical-section memory remains authoritative.
class CriticalSectionOwnershipLedger {
 public:
  static constexpr size_t kMaximumOwnedCriticalSections = 16;
  static constexpr size_t kLeaveMismatchHistorySize = 8;

  CriticalSectionOwnershipLedger() = default;
  CriticalSectionOwnershipLedger(const CriticalSectionOwnershipLedger&) = delete;
  CriticalSectionOwnershipLedger& operator=(const CriticalSectionOwnershipLedger&) = delete;

  void RecordEnter(uint32_t critical_section, uint32_t enter_lr,
                   int32_t observed_recursion) noexcept;
  void RecordLeave(uint32_t critical_section, uint32_t leave_lr, uint32_t current_thread,
                   uint32_t observed_owner, int32_t observed_recursion_before,
                   int32_t observed_recursion_after) noexcept;
  void RecordLeaveMismatch(uint32_t critical_section, uint32_t leave_lr, uint32_t current_thread,
                           uint32_t observed_owner, int32_t observed_recursion,
                           CriticalSectionLeaveMismatchReason reason) noexcept;

  // Returns false only when arguments are invalid or a coherent snapshot could
  // not be obtained after bounded retries.
  bool Query(uint32_t critical_section,
             CriticalSectionOwnershipSnapshot* out_snapshot) const noexcept;

 private:
  struct AtomicOwnershipEntry {
    std::atomic<uint32_t> critical_section{0};
    std::atomic<uint32_t> first_enter_lr{0};
    std::atomic<uint32_t> last_enter_lr{0};
    std::atomic<uint32_t> recorded_depth{0};
    std::atomic<uint32_t> enter_count{0};
    std::atomic<uint32_t> provenance_incomplete{0};
    std::atomic<uint64_t> first_enter_sequence{0};
    std::atomic<uint64_t> last_event_sequence{0};
  };

  struct AtomicLeaveMismatch {
    std::atomic<uint64_t> sequence{0};
    std::atomic<uint32_t> critical_section{0};
    std::atomic<uint32_t> leave_lr{0};
    std::atomic<uint32_t> current_thread{0};
    std::atomic<uint32_t> observed_owner{0};
    std::atomic<int32_t> observed_recursion{0};
    std::atomic<uint32_t> reason{0};
  };

  class WriteScope {
   public:
    explicit WriteScope(std::atomic<uint64_t>& generation) noexcept;
    ~WriteScope();

    WriteScope(const WriteScope&) = delete;
    WriteScope& operator=(const WriteScope&) = delete;

   private:
    std::atomic<uint64_t>& generation_;
  };

  size_t FindOwnershipEntry(uint32_t critical_section, size_t active_count) const noexcept;
  void RemoveOwnershipEntry(size_t index, size_t active_count) noexcept;
  void AppendLeaveMismatch(uint64_t sequence, uint32_t critical_section, uint32_t leave_lr,
                           uint32_t current_thread, uint32_t observed_owner,
                           int32_t observed_recursion,
                           CriticalSectionLeaveMismatchReason reason) noexcept;

  std::atomic<uint64_t> generation_{0};
  std::atomic<uint64_t> event_sequence_{0};
  std::atomic<uint32_t> active_count_{0};
  std::atomic<uint32_t> dropped_ownership_records_{0};
  std::atomic<uint64_t> total_leave_mismatches_{0};
  std::array<AtomicOwnershipEntry, kMaximumOwnedCriticalSections> ownership_entries_{};
  std::array<AtomicLeaveMismatch, kLeaveMismatchHistorySize> leave_mismatches_{};
};

}  // namespace rex::system
