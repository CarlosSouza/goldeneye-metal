/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>

#include <rex/system/critical_section_ledger.h>
#include <rex/system/xtypes.h>

namespace rex::system {
class XThread;
}

namespace rex::kernel::xboxkrnl {

struct X_RTL_CRITICAL_SECTION;

// Read-only snapshot used by title-specific diagnostics. lock_count is native
// endian in guest memory; the remaining fields retain the Xbox 360 big-endian
// layout.
struct RtlCriticalSectionDebugInfo {
  uint32_t signal_state = 0;
  int32_t lock_count = 0;
  int32_t recursion_count = 0;
  uint32_t owning_thread = 0;
  uint32_t estimated_waiters = 0;
  bool coherent = false;
};

constexpr uint32_t RtlCriticalSectionEstimatedWaiters(int32_t lock_count, int32_t recursion_count) {
  const int64_t estimate =
      static_cast<int64_t>(lock_count) - static_cast<int64_t>(recursion_count) + 1;
  return estimate <= 0
             ? 0
             : (estimate > static_cast<int64_t>(UINT32_MAX) ? UINT32_MAX
                                                            : static_cast<uint32_t>(estimate));
}

constexpr bool RtlCriticalSectionStateIsCoherent(int32_t lock_count, int32_t recursion_count,
                                                 uint32_t owning_thread) {
  return owning_thread == 0 ? lock_count == -1 && recursion_count == 0
                            : recursion_count > 0 && lock_count >= recursion_count - 1;
}

static_assert(RtlCriticalSectionEstimatedWaiters(-1, 0) == 0);
static_assert(RtlCriticalSectionEstimatedWaiters(0, 1) == 0);
static_assert(RtlCriticalSectionEstimatedWaiters(1, 2) == 0);
static_assert(RtlCriticalSectionEstimatedWaiters(1, 1) == 1);
static_assert(RtlCriticalSectionEstimatedWaiters(2, 1) == 2);
static_assert(RtlCriticalSectionStateIsCoherent(-1, 0, 0));
static_assert(!RtlCriticalSectionStateIsCoherent(-2, 0, 0));
static_assert(!RtlCriticalSectionStateIsCoherent(0, 0, 0));
static_assert(RtlCriticalSectionStateIsCoherent(0, 1, 0x40000000u));
static_assert(RtlCriticalSectionStateIsCoherent(1, 2, 0x40000000u));
static_assert(RtlCriticalSectionStateIsCoherent(1, 1, 0x40000000u));
static_assert(!RtlCriticalSectionStateIsCoherent(-1, 1, 0x40000000u));

void xeRtlInitializeCriticalSection(X_RTL_CRITICAL_SECTION* cs, uint32_t cs_ptr);
X_STATUS xeRtlInitializeCriticalSectionAndSpinCount(X_RTL_CRITICAL_SECTION* cs, uint32_t cs_ptr,
                                                    uint32_t spin_count);

// Returns false without touching guest memory when the full critical-section
// object isn't in readable committed memory.
bool QueryRtlCriticalSectionDebugInfo(uint32_t guest_address,
                                      RtlCriticalSectionDebugInfo* out_info);

using RtlCriticalSectionOwnershipDebugInfo = rex::system::CriticalSectionOwnershipSnapshot;

// Takes a read-only, bounded snapshot of provenance recorded by one XThread.
// The guest critical-section fields queried above remain authoritative.
bool QueryRtlCriticalSectionOwnershipDebugInfo(const rex::system::XThread* owner_thread,
                                               uint32_t guest_address,
                                               RtlCriticalSectionOwnershipDebugInfo* out_info);

}  // namespace rex::kernel::xboxkrnl
