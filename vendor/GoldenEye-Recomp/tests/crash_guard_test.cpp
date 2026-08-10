#include "ge_crash_guards.h"

#include <cstdint>

int main() {
  using ge::crash_guards::AudioLocationNeedsRecovery;
  using ge::crash_guards::CleanupListNodeNeedsRecovery;
  using ge::crash_guards::RecoverPackedDataPureVirtualDispatch;

  uint64_t result = 0xFEDCBA9876543210ull;
  if (RecoverPackedDataPureVirtualDispatch(0, result) || result != 0xFEDCBA9876543210ull) {
    return 1;
  }

  if (RecoverPackedDataPureVirtualDispatch(0x823D5E48u, result) ||
      result != 0xFEDCBA9876543210ull) {
    return 2;
  }

  if (!RecoverPackedDataPureVirtualDispatch(0x823EDF20u, result) || result != 0) {
    return 3;
  }
  if (!CleanupListNodeNeedsRecovery(0) || !CleanupListNodeNeedsRecovery(0x6920u) ||
      !CleanupListNodeNeedsRecovery(0xFFFFu)) {
    return 4;
  }
  if (CleanupListNodeNeedsRecovery(0x10000u) || CleanupListNodeNeedsRecovery(0x4466DA44u) ||
      CleanupListNodeNeedsRecovery(0xFFFFFFF8u)) {
    return 5;
  }
  if (!CleanupListNodeNeedsRecovery(0xFFFFFFF9u) || !CleanupListNodeNeedsRecovery(0xFFFFFFFBu) ||
      !CleanupListNodeNeedsRecovery(0xFFFFFFFCu) || !CleanupListNodeNeedsRecovery(0xFFFFFFFFu)) {
    return 6;
  }
  // Captured v0.4.1 signature: sound creation left a null output handle while
  // the caller still supplied a valid position.
  if (!AudioLocationNeedsRecovery(0, 0x830CB318u)) {
    return 7;
  }
  if (!AudioLocationNeedsRecovery(0x0000FFFFu, 0x830CB318u) ||
      !AudioLocationNeedsRecovery(0x40000000u, 0x0000FFFFu)) {
    return 8;
  }
  if (!AudioLocationNeedsRecovery(0xFFFFFFE1u, 0x830CB318u) ||
      !AudioLocationNeedsRecovery(0x40000000u, 0xFFFFFFF5u)) {
    return 9;
  }
  if (AudioLocationNeedsRecovery(0x00010000u, 0x830CB318u) ||
      AudioLocationNeedsRecovery(0xFFFFFFE0u, 0xFFFFFFF4u)) {
    return 10;
  }
  return 0;
}
