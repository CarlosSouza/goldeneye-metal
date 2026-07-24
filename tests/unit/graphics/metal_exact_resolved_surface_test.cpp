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
