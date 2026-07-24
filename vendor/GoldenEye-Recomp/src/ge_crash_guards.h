#pragma once

#include <cstdint>

namespace ge::crash_guards {

// This is the title's MSVC pure-virtual-call handler for the supported XEX.
// Only the packed-data accessor's two vtable+16 dispatches use this predicate;
// pure virtual calls anywhere else remain fatal and visible.
inline constexpr uint32_t kPackedDataPureVirtualTarget = 0x823EDF20u;
inline constexpr uint32_t kProtectedGuestLowPageEnd = 0x00010000u;

constexpr bool IsPackedDataPureVirtualTarget(uint32_t callback_target) {
  return callback_target == kPackedDataPureVirtualTarget;
}

constexpr bool RecoverPackedDataPureVirtualDispatch(uint32_t callback_target, uint64_t& result) {
  if (!IsPackedDataPureVirtualTarget(callback_target)) {
    return false;
  }
  result = 0;
  return true;
}

// sub_823DACE0 walks a guest intrusive list and reads node->next at node+4.
// The runtime deliberately leaves the first 64 KiB inaccessible, and a
// four-byte load that crosses the top of the guest address space is invalid as
// well. Treat either case as a corrupt remaining list.
constexpr bool CleanupListNodeNeedsRecovery(uint32_t node) {
  return node < kProtectedGuestLowPageEnd || node > 0xFFFFFFF8u;
}

static_assert(IsPackedDataPureVirtualTarget(0x823EDF20u));
static_assert(!IsPackedDataPureVirtualTarget(0));
static_assert(!IsPackedDataPureVirtualTarget(0x823EDF1Cu));
static_assert(!IsPackedDataPureVirtualTarget(0x823EDF24u));
static_assert(!IsPackedDataPureVirtualTarget(0x823D5E48u));
static_assert(!IsPackedDataPureVirtualTarget(0x823EF6C0u));
static_assert(CleanupListNodeNeedsRecovery(0));
static_assert(CleanupListNodeNeedsRecovery(0x00006920u));
static_assert(CleanupListNodeNeedsRecovery(0x0000FFFFu));
static_assert(!CleanupListNodeNeedsRecovery(0x00010000u));
static_assert(!CleanupListNodeNeedsRecovery(0x40000000u));
static_assert(!CleanupListNodeNeedsRecovery(0xFFFFFFF8u));
static_assert(CleanupListNodeNeedsRecovery(0xFFFFFFF9u));
static_assert(CleanupListNodeNeedsRecovery(0xFFFFFFFBu));
static_assert(CleanupListNodeNeedsRecovery(0xFFFFFFFCu));
static_assert(CleanupListNodeNeedsRecovery(0xFFFFFFFFu));

}  // namespace ge::crash_guards
