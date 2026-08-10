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

// sub_821448F8 writes a 3D sound object's fields through sound_buffer+16..28
// and reads three coordinates through position+0..8. Sound creation is allowed
// to fail and return a null handle, but several retail callers assume success.
// Reject only pointers that touch the runtime's protected guest low page or
// wrap around the 32-bit guest address space; valid sound behavior is unchanged.
constexpr bool AudioLocationNeedsRecovery(uint32_t sound_buffer, uint32_t position) {
  return sound_buffer < kProtectedGuestLowPageEnd || sound_buffer > 0xFFFFFFE0u ||
         position < kProtectedGuestLowPageEnd || position > 0xFFFFFFF4u;
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
static_assert(AudioLocationNeedsRecovery(0, 0x830CB318u));
static_assert(AudioLocationNeedsRecovery(0x0000FFFFu, 0x830CB318u));
static_assert(AudioLocationNeedsRecovery(0x40000000u, 0));
static_assert(AudioLocationNeedsRecovery(0xFFFFFFE1u, 0x830CB318u));
static_assert(AudioLocationNeedsRecovery(0x40000000u, 0xFFFFFFF5u));
static_assert(!AudioLocationNeedsRecovery(0x00010000u, 0x830CB318u));
static_assert(!AudioLocationNeedsRecovery(0xFFFFFFE0u, 0xFFFFFFF4u));

}  // namespace ge::crash_guards
