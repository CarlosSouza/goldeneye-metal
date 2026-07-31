#include <rex/kernel/xboxkrnl/rtl.h>

#include <catch2/catch_test_macros.hpp>

namespace {

using rex::kernel::xboxkrnl::ClassifyRtlCriticalSectionLeave;
using rex::kernel::xboxkrnl::ClassifyRtlCriticalSectionWake;
using rex::kernel::xboxkrnl::RtlCriticalSectionLeaveDisposition;
using rex::kernel::xboxkrnl::RtlCriticalSectionNeedsLazyInitialization;
using rex::kernel::xboxkrnl::RtlCriticalSectionWakeDisposition;

}  // namespace

TEST_CASE("critical-section lazy initialization distinguishes an initialized acquisition window",
          "[kernel][critical_section_policy]") {
  SECTION("a truly pristine zero-filled section needs lazy initialization") {
    CHECK(RtlCriticalSectionNeedsLazyInitialization(0, 0, 0, 0, 0, 0, 0));
  }

  SECTION("an initialized section with lock owner and recursion temporarily zero does not") {
    constexpr uint32_t kInitializedDispatcherControl = 1;
    CHECK_FALSE(
        RtlCriticalSectionNeedsLazyInitialization(kInitializedDispatcherControl, 0, 0, 0, 0, 0, 0));
  }

  SECTION("a partially published lazy initialization is not reinitialized") {
    CHECK_FALSE(RtlCriticalSectionNeedsLazyInitialization(0, 0, 0, 0, -1, 0, 0));
  }
}

TEST_CASE("critical-section wake waits for a live owner before acquiring",
          "[kernel][critical_section_policy]") {
  CHECK(ClassifyRtlCriticalSectionWake(0x30039018) ==
        RtlCriticalSectionWakeDisposition::kWaitForOwnerRelease);
  CHECK(ClassifyRtlCriticalSectionWake(0) == RtlCriticalSectionWakeDisposition::kAcquire);
}

TEST_CASE("critical-section leave ignores the ge_023 foreign recursive handoff",
          "[kernel][critical_section_policy][goldeneye]") {
  constexpr uint32_t kCleanupWorker = 0x30039018;
  constexpr uint32_t kBlockedAudioThread = 0x3002A018;

  // ge_023 recorded the cleanup worker reaching 0x823CFD48 while guest memory
  // named the blocked audio thread as owner with recursion two. LockCount may
  // include a reserved waiter, but every non-negative value still describes a
  // live foreign ownership that this caller must not steal or decrement.
  CHECK(ClassifyRtlCriticalSectionLeave(kCleanupWorker, kBlockedAudioThread, 2, 2) ==
        RtlCriticalSectionLeaveDisposition::kIgnoreForeignOwner);
  CHECK(ClassifyRtlCriticalSectionLeave(kCleanupWorker, kBlockedAudioThread, 0, 2) ==
        RtlCriticalSectionLeaveDisposition::kIgnoreForeignOwner);
}

TEST_CASE("critical-section leave owns valid state and ignores invalid state",
          "[kernel][critical_section_policy]") {
  constexpr uint32_t kCurrentThread = 0x30039018;

  SECTION("valid recursive ownership follows the normal leave path") {
    CHECK(ClassifyRtlCriticalSectionLeave(kCurrentThread, kCurrentThread, 1, 2) ==
          RtlCriticalSectionLeaveDisposition::kOwned);
    CHECK(ClassifyRtlCriticalSectionLeave(kCurrentThread, kCurrentThread, 0, 1) ==
          RtlCriticalSectionLeaveDisposition::kOwned);
  }

  SECTION("invalid owned state is left untouched") {
    CHECK(ClassifyRtlCriticalSectionLeave(kCurrentThread, kCurrentThread, -1, 1) ==
          RtlCriticalSectionLeaveDisposition::kIgnoreInvalidState);
    CHECK(ClassifyRtlCriticalSectionLeave(kCurrentThread, kCurrentThread, 0, 0) ==
          RtlCriticalSectionLeaveDisposition::kIgnoreInvalidState);
  }

  SECTION("unowned or foreign state is always left untouched") {
    CHECK(ClassifyRtlCriticalSectionLeave(kCurrentThread, 0, -1, 0) ==
          RtlCriticalSectionLeaveDisposition::kIgnoreForeignOwner);
    CHECK(ClassifyRtlCriticalSectionLeave(kCurrentThread, 0x3002A018, -1, 2) ==
          RtlCriticalSectionLeaveDisposition::kIgnoreForeignOwner);
    CHECK(ClassifyRtlCriticalSectionLeave(kCurrentThread, 0x3002A018, 1, 0) ==
          RtlCriticalSectionLeaveDisposition::kIgnoreForeignOwner);
  }
}
