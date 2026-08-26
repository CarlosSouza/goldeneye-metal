#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include <rex/graphics/resolve_route.h>

namespace rex::graphics {
namespace {

ResolveRouteState ValidResolve() {
  ResolveRouteState state;
  state.copy_valid = true;
  state.destination_bounds_valid = true;
  return state;
}

TEST_CASE("Resolve route accepts only a completed exact native route",
          "[graphics][resolve-route]") {
  ResolveRouteState state = ValidResolve();
  state.native_compatible = true;
  CHECK(ClassifyResolveRoute(state) == ResolveRouteOutcome::kCompatibleRouteFailed);

  state.native_completed = true;
  ResolveRouteOutcome outcome = ClassifyResolveRoute(state);
  CHECK(outcome == ResolveRouteOutcome::kExactNativeSuccess);
  CHECK(ResolveRouteSucceeded(outcome));
}

TEST_CASE("Resolve route accepts a completed exact CPU fallback", "[graphics][resolve-route]") {
  ResolveRouteState state = ValidResolve();
  state.cpu_fallback_compatible = true;
  CHECK(ClassifyResolveRoute(state) == ResolveRouteOutcome::kCompatibleRouteFailed);

  state.cpu_fallback_completed = true;
  ResolveRouteOutcome outcome = ClassifyResolveRoute(state);
  CHECK(outcome == ResolveRouteOutcome::kExactCpuFallbackSuccess);
  CHECK(ResolveRouteSucceeded(outcome));
}

TEST_CASE("Resolve route rejects diagnostic-only and missing routes", "[graphics][resolve-route]") {
  ResolveRouteState state = ValidResolve();
  state.diagnostic_completed = true;
  CHECK(ClassifyResolveRoute(state) == ResolveRouteOutcome::kDiagnosticOnly);
  CHECK_FALSE(ResolveRouteSucceeded(ClassifyResolveRoute(state)));

  state.diagnostic_completed = false;
  CHECK(ClassifyResolveRoute(state) == ResolveRouteOutcome::kNoCompatibleRoute);
  CHECK_FALSE(ResolveRouteSucceeded(ClassifyResolveRoute(state)));
}

TEST_CASE("Resolve route rejects invalid copies and destination bounds",
          "[graphics][resolve-route]") {
  ResolveRouteState state;
  state.destination_bounds_valid = true;
  state.native_compatible = true;
  state.native_completed = true;
  CHECK(ClassifyResolveRoute(state) == ResolveRouteOutcome::kInvalidCopy);

  state.copy_valid = true;
  state.destination_bounds_valid = false;
  CHECK(ClassifyResolveRoute(state) == ResolveRouteOutcome::kDestinationOutOfBounds);
  CHECK_FALSE(ResolveRouteSucceeded(ClassifyResolveRoute(state)));
}

TEST_CASE("Resolve destination bounds reject empty and overflowing ranges",
          "[graphics][resolve-route]") {
  constexpr uint32_t kAddressSpaceSize = 0x20000000;
  CHECK(IsResolveDestinationRangeValid(0, 1, kAddressSpaceSize));
  CHECK(IsResolveDestinationRangeValid(0x1FFFF000, 0x1000, kAddressSpaceSize));
  CHECK_FALSE(IsResolveDestinationRangeValid(0, 0, kAddressSpaceSize));
  CHECK_FALSE(IsResolveDestinationRangeValid(kAddressSpaceSize, 1, kAddressSpaceSize));
  CHECK_FALSE(IsResolveDestinationRangeValid(0x1FFFF000, 0x1001, kAddressSpaceSize));
  CHECK_FALSE(IsResolveDestinationRangeValid(0xFFFFFFF0, 0x40, kAddressSpaceSize));
}

TEST_CASE("Resolve route ignores unclassified completion signals", "[graphics][resolve-route]") {
  ResolveRouteState state = ValidResolve();
  state.native_completed = true;
  state.cpu_fallback_completed = true;
  CHECK(ClassifyResolveRoute(state) == ResolveRouteOutcome::kNoCompatibleRoute);
}

TEST_CASE("Resolve route outcome names are stable diagnostics", "[graphics][resolve-route]") {
  CHECK(std::string_view(ResolveRouteOutcomeName(ResolveRouteOutcome::kExactNativeSuccess)) ==
        "exact-native-success");
  CHECK(std::string_view(ResolveRouteOutcomeName(ResolveRouteOutcome::kExactCpuFallbackSuccess)) ==
        "exact-cpu-fallback-success");
  CHECK(std::string_view(ResolveRouteOutcomeName(ResolveRouteOutcome::kInvalidCopy)) ==
        "invalid-copy");
  CHECK(std::string_view(ResolveRouteOutcomeName(ResolveRouteOutcome::kDestinationOutOfBounds)) ==
        "destination-out-of-bounds");
  CHECK(std::string_view(ResolveRouteOutcomeName(ResolveRouteOutcome::kDiagnosticOnly)) ==
        "diagnostic-only");
  CHECK(std::string_view(ResolveRouteOutcomeName(ResolveRouteOutcome::kCompatibleRouteFailed)) ==
        "compatible-route-failed");
  CHECK(std::string_view(ResolveRouteOutcomeName(ResolveRouteOutcome::kNoCompatibleRoute)) ==
        "no-compatible-route");
}

}  // namespace
}  // namespace rex::graphics
