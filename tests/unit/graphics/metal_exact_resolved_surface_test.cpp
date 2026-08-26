/**
 ******************************************************************************
 * ReXGlue - Xbox 360 recompilation runtime                                  *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                       *
 *                                                                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/metal/command_processor.h>

TEST_CASE("Metal exact resolved surface accepts one complete ordered generation",
          "[graphics][metal][resolve]") {
  std::vector<uint8_t> rows;
  uint32_t valid_rows = 0;

  REQUIRE(rex::graphics::metal::UpdateExactResolvedSurfaceRowCoverage(rows, valid_rows, 720, 0,
                                                                      256));
  REQUIRE(rex::graphics::metal::UpdateExactResolvedSurfaceRowCoverage(rows, valid_rows, 720, 256,
                                                                      256));
  REQUIRE(rex::graphics::metal::UpdateExactResolvedSurfaceRowCoverage(rows, valid_rows, 720, 512,
                                                                      208));
  CHECK(valid_rows == 720);
}

TEST_CASE("Metal exact resolved surface never mixes generations or skips a band",
          "[graphics][metal][resolve]") {
  std::vector<uint8_t> rows;
  uint32_t valid_rows = 0;

  REQUIRE(rex::graphics::metal::UpdateExactResolvedSurfaceRowCoverage(rows, valid_rows, 720, 0,
                                                                      256));
  REQUIRE(rex::graphics::metal::UpdateExactResolvedSurfaceRowCoverage(rows, valid_rows, 720, 256,
                                                                      256));
  REQUIRE(rex::graphics::metal::UpdateExactResolvedSurfaceRowCoverage(rows, valid_rows, 720, 512,
                                                                      208));
  REQUIRE(valid_rows == 720);

  // Row zero begins a new frame, clearing the previous frame's lower rows.
  REQUIRE(rex::graphics::metal::UpdateExactResolvedSurfaceRowCoverage(rows, valid_rows, 720, 0,
                                                                      256));
  CHECK(valid_rows == 256);
  CHECK(rows[0] == 1);
  CHECK(rows[255] == 1);
  CHECK(rows[256] == 0);
  CHECK(rows[719] == 0);

  // A missing or repeated middle band cannot make the new generation valid.
  CHECK_FALSE(rex::graphics::metal::UpdateExactResolvedSurfaceRowCoverage(rows, valid_rows, 720,
                                                                          512, 208));
  CHECK_FALSE(rex::graphics::metal::UpdateExactResolvedSurfaceRowCoverage(rows, valid_rows, 720,
                                                                          0, 0));
  CHECK(valid_rows == 256);
}

namespace {

rex::graphics::metal::ExactResolvedSurfaceAssemblyIdentity MakeAssembly(
    uint32_t base = 0x01000000) {
  return {
      .base = base,
      .pitch = 1280,
      .snapshot_height = 720,
      .surface_height = 720,
      .tiled_extent = 0x400000,
      .endian = 2,
      .resolve_signature = UINT64_C(0x12345678ABCDEF00),
  };
}

rex::graphics::metal::ExactResolvedSurfacePublicationIdentity MakePublication(uint32_t base) {
  return {
      .base = base,
      .pitch = 1280,
      .surface_height = 720,
      .tiled_extent = 0x400000,
      .snapshot_y = 0,
      .write_height = 256,
      .endian = 2,
      .resolve_signature = UINT64_C(0x12345678ABCDEF00),
  };
}

}  // namespace

TEST_CASE("Metal exact resolved surface may speculatively start with a partial top band",
          "[graphics][metal][resolve]") {
  const auto assembly = MakeAssembly();
  std::vector<uint8_t> rows;
  uint32_t valid_rows = 0;

  CHECK(rex::graphics::metal::CanPrepareExactResolvedSurfaceAssemblyBand(nullptr, 0, 0, assembly, 0,
                                                                         256));
  REQUIRE(
      rex::graphics::metal::UpdateExactResolvedSurfaceRowCoverage(rows, valid_rows, 720, 0, 256));
  CHECK(rex::graphics::metal::CanPrepareExactResolvedSurfaceAssemblyBand(
      &assembly, uint32_t(rows.size()), valid_rows, assembly, 256, 256));
  REQUIRE(
      rex::graphics::metal::UpdateExactResolvedSurfaceRowCoverage(rows, valid_rows, 720, 256, 256));
  CHECK(rex::graphics::metal::CanPrepareExactResolvedSurfaceAssemblyBand(
      &assembly, uint32_t(rows.size()), valid_rows, assembly, 512, 208));
  REQUIRE(
      rex::graphics::metal::UpdateExactResolvedSurfaceRowCoverage(rows, valid_rows, 720, 512, 208));
  CHECK(valid_rows == 720);
}

TEST_CASE("Metal exact resolved surface speculative assembly keeps strict continuation identity",
          "[graphics][metal][resolve]") {
  const auto assembly = MakeAssembly();

  CHECK_FALSE(rex::graphics::metal::CanPrepareExactResolvedSurfaceAssemblyBand(nullptr, 0, 0,
                                                                               assembly, 256, 256));
  CHECK(rex::graphics::metal::CanPrepareExactResolvedSurfaceAssemblyBand(&assembly, 720, 256,
                                                                         assembly, 256, 256));
  CHECK_FALSE(rex::graphics::metal::CanPrepareExactResolvedSurfaceAssemblyBand(&assembly, 720, 255,
                                                                               assembly, 256, 256));
  CHECK_FALSE(rex::graphics::metal::CanPrepareExactResolvedSurfaceAssemblyBand(&assembly, 719, 256,
                                                                               assembly, 256, 256));

  auto other_base = assembly;
  other_base.base += 0x400000;
  CHECK_FALSE(rex::graphics::metal::CanPrepareExactResolvedSurfaceAssemblyBand(
      &assembly, 720, 256, other_base, 256, 256));
  auto other_signature = assembly;
  ++other_signature.resolve_signature;
  CHECK_FALSE(rex::graphics::metal::CanPrepareExactResolvedSurfaceAssemblyBand(
      &assembly, 720, 256, other_signature, 256, 256));
}

TEST_CASE("Metal exact resolved surface rejects malformed speculative bands",
          "[graphics][metal][resolve]") {
  const auto assembly = MakeAssembly();

  CHECK_FALSE(rex::graphics::metal::CanPrepareExactResolvedSurfaceAssemblyBand(nullptr, 0, 0,
                                                                               assembly, 0, 0));
  CHECK_FALSE(rex::graphics::metal::CanPrepareExactResolvedSurfaceAssemblyBand(nullptr, 0, 0,
                                                                               assembly, 720, 1));
  CHECK_FALSE(rex::graphics::metal::CanPrepareExactResolvedSurfaceAssemblyBand(nullptr, 0, 0,
                                                                               assembly, 700, 21));

  auto mismatched_height = assembly;
  --mismatched_height.surface_height;
  CHECK_FALSE(rex::graphics::metal::CanPrepareExactResolvedSurfaceAssemblyBand(
      nullptr, 0, 0, mismatched_height, 0, 256));
  auto missing_extent = assembly;
  missing_extent.tiled_extent = 0;
  CHECK_FALSE(rex::graphics::metal::CanPrepareExactResolvedSurfaceAssemblyBand(
      nullptr, 0, 0, missing_extent, 0, 256));
}

TEST_CASE("Metal exact resolved surface ignores unrelated invalidation while publication is pending",
          "[graphics][metal][resolve]") {
  rex::graphics::metal::ExactResolvedSurfacePublicationTracker tracker;
  auto publication = MakePublication(0x01000000);
  uint64_t token = tracker.Arm(publication);

  auto invalidation = tracker.Invalidate(false, 0, 0, 0x02000000, 0x1000);

  CHECK_FALSE(invalidation.any_overlap());
  CHECK(tracker.pending_active());
  auto wrong_generation = publication;
  wrong_generation.snapshot_y = 256;
  CHECK_FALSE(tracker.Consume(token, wrong_generation));
  CHECK(tracker.pending_active());
  CHECK(tracker.Consume(token, publication));
}

TEST_CASE("Metal exact resolved surface rejects overlapping pending publication",
          "[graphics][metal][resolve]") {
  rex::graphics::metal::ExactResolvedSurfacePublicationTracker tracker;
  auto publication = MakePublication(0x01000000);
  uint64_t token = tracker.Arm(publication);

  auto invalidation = tracker.Invalidate(false, 0, 0, 0x01200000, 0x1000);

  CHECK_FALSE(invalidation.current_overlap);
  CHECK(invalidation.pending_overlap);
  CHECK_FALSE(tracker.pending_active());
  CHECK_FALSE(tracker.Consume(token, publication));
}

TEST_CASE("Metal exact resolved surface preserves nonoverlapping pending publication when old cache is invalidated",
          "[graphics][metal][resolve]") {
  rex::graphics::metal::ExactResolvedSurfacePublicationTracker tracker;
  auto pending_publication = MakePublication(0x02000000);
  uint64_t token = tracker.Arm(pending_publication);

  auto invalidation =
      tracker.Invalidate(true, 0x01000000, 0x400000, 0x01100000, 0x1000);

  CHECK(invalidation.current_overlap);
  CHECK_FALSE(invalidation.pending_overlap);
  CHECK(tracker.pending_active());
  CHECK(tracker.Consume(token, pending_publication));
}
