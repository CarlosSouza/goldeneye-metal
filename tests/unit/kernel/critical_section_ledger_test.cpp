#include <rex/system/critical_section_ledger.h>

#include <catch2/catch_test_macros.hpp>

namespace {

using rex::system::CriticalSectionLeaveMismatchReason;
using rex::system::CriticalSectionOwnershipLedger;
using rex::system::CriticalSectionOwnershipSnapshot;

constexpr uint32_t kThread = 0x40001000;
constexpr uint32_t kCriticalSectionA = 0x50001000;
constexpr uint32_t kCriticalSectionB = 0x50002000;

CriticalSectionOwnershipSnapshot Query(const CriticalSectionOwnershipLedger& ledger,
                                       uint32_t critical_section) {
  CriticalSectionOwnershipSnapshot snapshot;
  REQUIRE(ledger.Query(critical_section, &snapshot));
  REQUIRE(snapshot.coherent);
  return snapshot;
}

}  // namespace

TEST_CASE("critical-section ledger tracks recursive ownership",
          "[kernel][critical_section_ledger]") {
  CriticalSectionOwnershipLedger ledger;

  ledger.RecordEnter(kCriticalSectionA, 0x82001004, 1);
  auto snapshot = Query(ledger, kCriticalSectionA);
  CHECK(snapshot.ownership_found);
  CHECK_FALSE(snapshot.provenance_incomplete);
  CHECK(snapshot.first_enter_lr == 0x82001004);
  CHECK(snapshot.last_enter_lr == 0x82001004);
  CHECK(snapshot.recorded_depth == 1);
  CHECK(snapshot.enter_count == 1);
  CHECK(snapshot.first_enter_sequence == 1);
  CHECK(snapshot.last_event_sequence == 1);

  ledger.RecordEnter(kCriticalSectionA, 0x82002004, 2);
  snapshot = Query(ledger, kCriticalSectionA);
  CHECK(snapshot.ownership_found);
  CHECK_FALSE(snapshot.provenance_incomplete);
  CHECK(snapshot.first_enter_lr == 0x82001004);
  CHECK(snapshot.last_enter_lr == 0x82002004);
  CHECK(snapshot.recorded_depth == 2);
  CHECK(snapshot.enter_count == 2);
  CHECK(snapshot.last_event_sequence == 2);

  ledger.RecordLeave(kCriticalSectionA, 0x82003004, kThread, kThread, 2, 1);
  snapshot = Query(ledger, kCriticalSectionA);
  CHECK(snapshot.ownership_found);
  CHECK(snapshot.recorded_depth == 1);
  CHECK(snapshot.last_event_sequence == 3);

  ledger.RecordLeave(kCriticalSectionA, 0x82004004, kThread, kThread, 1, 0);
  snapshot = Query(ledger, kCriticalSectionA);
  CHECK_FALSE(snapshot.ownership_found);
  CHECK(snapshot.total_leave_mismatches == 0);
}

TEST_CASE("critical-section ledger supports non-LIFO releases",
          "[kernel][critical_section_ledger]") {
  CriticalSectionOwnershipLedger ledger;
  ledger.RecordEnter(kCriticalSectionA, 0x82001004, 1);
  ledger.RecordEnter(kCriticalSectionB, 0x82002004, 1);

  ledger.RecordLeave(kCriticalSectionA, 0x82003004, kThread, kThread, 1, 0);
  CHECK_FALSE(Query(ledger, kCriticalSectionA).ownership_found);

  auto snapshot_b = Query(ledger, kCriticalSectionB);
  CHECK(snapshot_b.ownership_found);
  CHECK(snapshot_b.first_enter_lr == 0x82002004);
  CHECK(snapshot_b.recorded_depth == 1);

  ledger.RecordLeave(kCriticalSectionB, 0x82004004, kThread, kThread, 1, 0);
  CHECK_FALSE(Query(ledger, kCriticalSectionB).ownership_found);
}

TEST_CASE("critical-section ledger marks incomplete provenance and resets stale entries",
          "[kernel][critical_section_ledger]") {
  CriticalSectionOwnershipLedger ledger;

  ledger.RecordEnter(kCriticalSectionA, 0x82001004, 3);
  auto snapshot = Query(ledger, kCriticalSectionA);
  CHECK(snapshot.ownership_found);
  CHECK(snapshot.provenance_incomplete);
  CHECK(snapshot.recorded_depth == 3);

  // A depth-one acquisition is necessarily a new outer acquisition. It
  // replaces stale diagnostic state left by a reinitialized guest object.
  ledger.RecordEnter(kCriticalSectionA, 0x82005004, 1);
  snapshot = Query(ledger, kCriticalSectionA);
  CHECK(snapshot.ownership_found);
  CHECK_FALSE(snapshot.provenance_incomplete);
  CHECK(snapshot.first_enter_lr == 0x82005004);
  CHECK(snapshot.last_enter_lr == 0x82005004);
  CHECK(snapshot.recorded_depth == 1);
  CHECK(snapshot.enter_count == 1);
}

TEST_CASE("critical-section ledger records leave mismatches without changing ownership",
          "[kernel][critical_section_ledger]") {
  CriticalSectionOwnershipLedger ledger;
  ledger.RecordEnter(kCriticalSectionA, 0x82001004, 1);

  ledger.RecordLeaveMismatch(kCriticalSectionA, 0x82006004, kThread, 0x40002000, 1,
                             CriticalSectionLeaveMismatchReason::kGuestOwnerMismatch);
  auto snapshot = Query(ledger, kCriticalSectionA);
  CHECK(snapshot.ownership_found);
  CHECK(snapshot.recorded_depth == 1);
  CHECK(snapshot.total_leave_mismatches == 1);
  REQUIRE(snapshot.recent_leave_mismatch_found);
  CHECK(snapshot.recent_leave_mismatch.sequence == 2);
  CHECK(snapshot.recent_leave_mismatch.leave_lr == 0x82006004);
  CHECK(snapshot.recent_leave_mismatch.current_thread == kThread);
  CHECK(snapshot.recent_leave_mismatch.observed_owner == 0x40002000);
  CHECK(snapshot.recent_leave_mismatch.observed_recursion == 1);
  CHECK(snapshot.recent_leave_mismatch.reason ==
        CriticalSectionLeaveMismatchReason::kGuestOwnerMismatch);
  REQUIRE(snapshot.most_recent_leave_mismatch_found);
  CHECK(snapshot.most_recent_leave_mismatch.critical_section == kCriticalSectionA);
  CHECK(snapshot.most_recent_leave_mismatch.leave_lr == 0x82006004);

  ledger.RecordLeave(kCriticalSectionA, 0x82007004, kThread, kThread, 2, 1);
  snapshot = Query(ledger, kCriticalSectionA);
  CHECK(snapshot.ownership_found);
  CHECK(snapshot.provenance_incomplete);
  CHECK(snapshot.recorded_depth == 1);
  CHECK(snapshot.total_leave_mismatches == 2);
  REQUIRE(snapshot.recent_leave_mismatch_found);
  CHECK(snapshot.recent_leave_mismatch.reason ==
        CriticalSectionLeaveMismatchReason::kRecordedDepthMismatch);

  ledger.RecordLeave(kCriticalSectionB, 0x82008004, kThread, kThread, 1, 0);
  auto missing_snapshot = Query(ledger, kCriticalSectionB);
  CHECK_FALSE(missing_snapshot.ownership_found);
  CHECK(missing_snapshot.total_leave_mismatches == 3);
  REQUIRE(missing_snapshot.recent_leave_mismatch_found);
  CHECK(missing_snapshot.recent_leave_mismatch.reason ==
        CriticalSectionLeaveMismatchReason::kNoOwnershipRecord);

  // A corrupted callback may attempt to leave the wrong pointer while the
  // actual lock remains stranded. Querying the actual lock must still expose
  // the owner's newest cross-address mismatch.
  snapshot = Query(ledger, kCriticalSectionA);
  CHECK(snapshot.ownership_found);
  REQUIRE(snapshot.recent_leave_mismatch_found);
  CHECK(snapshot.recent_leave_mismatch.critical_section == kCriticalSectionA);
  REQUIRE(snapshot.most_recent_leave_mismatch_found);
  CHECK(snapshot.most_recent_leave_mismatch.critical_section == kCriticalSectionB);
  CHECK(snapshot.most_recent_leave_mismatch.leave_lr == 0x82008004);
  CHECK(snapshot.most_recent_leave_mismatch.reason ==
        CriticalSectionLeaveMismatchReason::kNoOwnershipRecord);
}

TEST_CASE("critical-section ledger preserves the ge_023 recursive handoff signature",
          "[kernel][critical_section_ledger][goldeneye]") {
  // ge_023 captured the cleanup worker as the recorded owner after entering
  // sub_823CFC00, but guest memory named the blocked audio thread as owner with
  // recursion two when the cleanup worker reached the leave callsite. A foreign
  // leave must remain diagnostic-only in this ledger: it must not consume the
  // cleanup worker's recorded outer ownership.
  constexpr uint32_t kGeCriticalSection = 0x4466DA44;
  constexpr uint32_t kCleanupWorker = 0x30039018;
  constexpr uint32_t kBlockedAudioThread = 0x3002A018;
  constexpr uint32_t kCleanupEnterLr = 0x823CFC30;
  constexpr uint32_t kCleanupLeaveLr = 0x823CFD48;

  CriticalSectionOwnershipLedger cleanup_worker_ledger;
  cleanup_worker_ledger.RecordEnter(kGeCriticalSection, kCleanupEnterLr, 1);
  cleanup_worker_ledger.RecordEnter(kGeCriticalSection, kCleanupEnterLr, 2);
  cleanup_worker_ledger.RecordLeave(kGeCriticalSection, kCleanupLeaveLr, kCleanupWorker,
                                    kCleanupWorker, 2, 1);

  cleanup_worker_ledger.RecordLeaveMismatch(
      kGeCriticalSection, kCleanupLeaveLr, kCleanupWorker, kBlockedAudioThread, 2,
      CriticalSectionLeaveMismatchReason::kGuestOwnerMismatch);

  const auto snapshot = Query(cleanup_worker_ledger, kGeCriticalSection);
  CHECK(snapshot.ownership_found);
  CHECK_FALSE(snapshot.provenance_incomplete);
  CHECK(snapshot.first_enter_lr == kCleanupEnterLr);
  CHECK(snapshot.last_enter_lr == kCleanupEnterLr);
  CHECK(snapshot.recorded_depth == 1);
  CHECK(snapshot.enter_count == 2);
  CHECK(snapshot.total_leave_mismatches == 1);
  REQUIRE(snapshot.recent_leave_mismatch_found);
  CHECK(snapshot.recent_leave_mismatch.leave_lr == kCleanupLeaveLr);
  CHECK(snapshot.recent_leave_mismatch.current_thread == kCleanupWorker);
  CHECK(snapshot.recent_leave_mismatch.observed_owner == kBlockedAudioThread);
  CHECK(snapshot.recent_leave_mismatch.observed_recursion == 2);
  CHECK(snapshot.recent_leave_mismatch.reason ==
        CriticalSectionLeaveMismatchReason::kGuestOwnerMismatch);
}

TEST_CASE("critical-section ledger bounds active ownership and mismatch history",
          "[kernel][critical_section_ledger]") {
  CriticalSectionOwnershipLedger ledger;

  for (size_t index = 0; index < CriticalSectionOwnershipLedger::kMaximumOwnedCriticalSections;
       ++index) {
    ledger.RecordEnter(0x50010000 + static_cast<uint32_t>(index * 0x100),
                       0x82010004 + static_cast<uint32_t>(index * 4), 1);
  }

  constexpr uint32_t kOverflowCriticalSection = 0x50020000;
  ledger.RecordEnter(kOverflowCriticalSection, 0x82020004, 1);
  auto overflow_snapshot = Query(ledger, kOverflowCriticalSection);
  CHECK_FALSE(overflow_snapshot.ownership_found);
  CHECK(overflow_snapshot.dropped_ownership_records == 1);

  constexpr size_t kMismatchCount = CriticalSectionOwnershipLedger::kLeaveMismatchHistorySize + 3;
  for (size_t index = 0; index < kMismatchCount; ++index) {
    ledger.RecordLeaveMismatch(kCriticalSectionA, 0x82030004 + static_cast<uint32_t>(index * 4),
                               kThread, 0x40002000, static_cast<int32_t>(index + 1),
                               CriticalSectionLeaveMismatchReason::kGuestOwnerMismatch);
  }

  auto mismatch_snapshot = Query(ledger, kCriticalSectionA);
  CHECK(mismatch_snapshot.total_leave_mismatches == kMismatchCount);
  REQUIRE(mismatch_snapshot.recent_leave_mismatch_found);
  CHECK(mismatch_snapshot.recent_leave_mismatch.leave_lr ==
        0x82030004 + static_cast<uint32_t>((kMismatchCount - 1) * 4));
  CHECK(mismatch_snapshot.recent_leave_mismatch.observed_recursion ==
        static_cast<int32_t>(kMismatchCount));
  REQUIRE(mismatch_snapshot.most_recent_leave_mismatch_found);
  CHECK(mismatch_snapshot.most_recent_leave_mismatch.sequence ==
        mismatch_snapshot.recent_leave_mismatch.sequence);
}
