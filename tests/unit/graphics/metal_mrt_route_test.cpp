#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/metal/mrt_route.h>

namespace rex::graphics::metal {

TEST_CASE("Metal legacy color replay fails closed when more than one target is active",
          "[graphics][metal][mrt]") {
  CHECK(CountActiveColorTargets(0x0000) == 0);
  CHECK(CountActiveColorTargets(0x000F) == 1);
  CHECK(CountActiveColorTargets(0x00A0) == 1);
  CHECK(CountActiveColorTargets(0xF00F) == 2);
  CHECK(CountActiveColorTargets(0x1111) == 4);

  CHECK_FALSE(RequiresTrueMrtSubmission(0x0000));
  CHECK_FALSE(RequiresTrueMrtSubmission(0x000F));
  CHECK_FALSE(RequiresTrueMrtSubmission(0x00A0));
  CHECK(RequiresTrueMrtSubmission(0x00FF));
  CHECK(RequiresTrueMrtSubmission(0xF00F));
  CHECK(RequiresTrueMrtSubmission(0x1111));

  CHECK(IsMrtVertexRouteSideEffectSafe(false, false));
  CHECK(IsMrtVertexRouteSideEffectSafe(false, true));
  CHECK(IsMrtVertexRouteSideEffectSafe(true, false));
  CHECK_FALSE(IsMrtVertexRouteSideEffectSafe(true, true));

  CHECK(IsMrtSubmissionExactlyOnce(2, 2, 1, 1, 1));
  CHECK(IsMrtSubmissionExactlyOnce(4, 4, 1, 1, 1));
  CHECK_FALSE(IsMrtSubmissionExactlyOnce(1, 1, 1, 1, 1));
  CHECK_FALSE(IsMrtSubmissionExactlyOnce(2, 2, 2, 1, 1));
  CHECK_FALSE(IsMrtSubmissionExactlyOnce(2, 2, 1, 2, 1));
  CHECK_FALSE(IsMrtSubmissionExactlyOnce(2, 2, 1, 1, 0));
}

TEST_CASE("Metal MRT attachment collection preserves sparse output locations",
          "[graphics][metal][mrt]") {
  MrtActiveAttachmentSet rt_0_2 = GetMrtActiveAttachmentSet(0x0F0F);
  REQUIRE(rt_0_2.count == 2);
  CHECK(rt_0_2.output_mask == 0x5);
  CHECK(rt_0_2.slots[0] == 0);
  CHECK(rt_0_2.slots[1] == 2);

  MrtActiveAttachmentSet all = GetMrtActiveAttachmentSet(0x1111);
  REQUIRE(all.count == 4);
  CHECK(all.output_mask == 0xF);
  CHECK(all.slots == std::array<uint8_t, 4>{0, 1, 2, 3});
}

TEST_CASE("Metal production MRT rejects physical EDRAM aliases",
          "[graphics][metal][mrt]") {
  std::array<MrtEdramSurface, xenos::kMaxColorRenderTargets + 1> surfaces = {};
  surfaces[0] = MakeMrtEdramSurface(0, 16, xenos::MsaaSamples::k1X, false, 640, 480);
  surfaces[1] = MakeMrtEdramSurface(512, 16, xenos::MsaaSamples::k1X, false, 640, 480);
  surfaces[4] = MakeMrtEdramSurface(1024, 16, xenos::MsaaSamples::k1X, false, 640, 480);
  CHECK(ValidateMrtEdramSurfaceSet(surfaces) == MrtEdramValidation::kValid);

  surfaces[1] = MakeMrtEdramSurface(4, 16, xenos::MsaaSamples::k1X, false, 640, 480);
  CHECK(ValidateMrtEdramSurfaceSet(surfaces) == MrtEdramValidation::kTargetOverlap);

  surfaces = {};
  surfaces[0] = MakeMrtEdramSurface(2044, 16, xenos::MsaaSamples::k1X, false, 640, 32);
  surfaces[1] = MakeMrtEdramSurface(0, 16, xenos::MsaaSamples::k1X, false, 640, 16);
  CHECK(ValidateMrtEdramSurfaceSet(surfaces) == MrtEdramValidation::kTargetOverlap);

  surfaces = {};
  surfaces[0] = MakeMrtEdramSurface(0, 2, xenos::MsaaSamples::k1X, false, 80, 16400);
  CHECK(ValidateMrtEdramSurfaceSet(surfaces) == MrtEdramValidation::kSelfAliasing);

  surfaces = {};
  surfaces[0] = MakeMrtEdramSurface(0, 4, xenos::MsaaSamples::k1X, false, 400, 16);
  CHECK(ValidateMrtEdramSurfaceSet(surfaces) == MrtEdramValidation::kInvalidLayout);
}

TEST_CASE("Metal production MRT EDRAM footprints preserve sample and word widths",
          "[graphics][metal][mrt]") {
  MrtEdramSurface one_x =
      MakeMrtEdramSurface(0, 4, xenos::MsaaSamples::k1X, false, 80, 16);
  CHECK(one_x.tile_columns == 1);
  CHECK(one_x.tile_rows == 1);

  MrtEdramSurface two_x =
      MakeMrtEdramSurface(0, 4, xenos::MsaaSamples::k2X, false, 80, 16);
  CHECK(two_x.tile_columns == 1);
  CHECK(two_x.tile_rows == 2);

  MrtEdramSurface four_x =
      MakeMrtEdramSurface(0, 4, xenos::MsaaSamples::k4X, false, 80, 16);
  CHECK(four_x.tile_columns == 2);
  CHECK(four_x.tile_rows == 2);

  MrtEdramSurface wide_words =
      MakeMrtEdramSurface(0, 4, xenos::MsaaSamples::k1X, true, 80, 16);
  CHECK(wide_words.tile_columns == 2);
  CHECK(wide_words.tile_rows == 1);
}

}  // namespace rex::graphics::metal
