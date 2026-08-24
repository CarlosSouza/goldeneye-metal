#pragma once

#include <cstdint>

namespace rex::graphics {

// Renderer-independent classification of a resolve after all compatible
// routes have been attempted. Diagnostic output is deliberately not a valid
// completion: a guest copy succeeds only after an exact native resolve or an
// exact CPU fallback has written the requested destination.
enum class ResolveRouteOutcome : uint8_t {
  kExactNativeSuccess,
  kExactCpuFallbackSuccess,
  kInvalidCopy,
  kDestinationOutOfBounds,
  kDiagnosticOnly,
  kCompatibleRouteFailed,
  kNoCompatibleRoute,
};

struct ResolveRouteState {
  bool copy_valid = false;
  bool destination_bounds_valid = false;
  bool native_compatible = false;
  bool native_completed = false;
  bool cpu_fallback_compatible = false;
  bool cpu_fallback_completed = false;
  bool diagnostic_completed = false;
};

constexpr bool IsResolveDestinationRangeValid(uint32_t start, uint32_t length,
                                              uint32_t address_space_size) {
  return length && start < address_space_size && length <= address_space_size - start;
}

constexpr ResolveRouteOutcome ClassifyResolveRoute(const ResolveRouteState& state) {
  if (!state.copy_valid) {
    return ResolveRouteOutcome::kInvalidCopy;
  }
  if (!state.destination_bounds_valid) {
    return ResolveRouteOutcome::kDestinationOutOfBounds;
  }
  if (state.native_compatible && state.native_completed) {
    return ResolveRouteOutcome::kExactNativeSuccess;
  }
  if (state.cpu_fallback_compatible && state.cpu_fallback_completed) {
    return ResolveRouteOutcome::kExactCpuFallbackSuccess;
  }
  if (state.diagnostic_completed) {
    return ResolveRouteOutcome::kDiagnosticOnly;
  }
  if (state.native_compatible || state.cpu_fallback_compatible) {
    return ResolveRouteOutcome::kCompatibleRouteFailed;
  }
  return ResolveRouteOutcome::kNoCompatibleRoute;
}

constexpr bool ResolveRouteSucceeded(ResolveRouteOutcome outcome) {
  return outcome == ResolveRouteOutcome::kExactNativeSuccess ||
         outcome == ResolveRouteOutcome::kExactCpuFallbackSuccess;
}

constexpr const char* ResolveRouteOutcomeName(ResolveRouteOutcome outcome) {
  switch (outcome) {
    case ResolveRouteOutcome::kExactNativeSuccess:
      return "exact-native-success";
    case ResolveRouteOutcome::kExactCpuFallbackSuccess:
      return "exact-cpu-fallback-success";
    case ResolveRouteOutcome::kInvalidCopy:
      return "invalid-copy";
    case ResolveRouteOutcome::kDestinationOutOfBounds:
      return "destination-out-of-bounds";
    case ResolveRouteOutcome::kDiagnosticOnly:
      return "diagnostic-only";
    case ResolveRouteOutcome::kCompatibleRouteFailed:
      return "compatible-route-failed";
    case ResolveRouteOutcome::kNoCompatibleRoute:
      return "no-compatible-route";
  }
  return "unknown";
}

}  // namespace rex::graphics
