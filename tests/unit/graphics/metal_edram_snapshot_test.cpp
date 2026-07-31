#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/metal/edram_snapshot.h>

namespace rex::graphics::metal {
namespace {

TEST_CASE("Metal canonical EDRAM capture does not authorize live target hydration",
          "[graphics][metal][edram]") {
  CanonicalEdramAuthorityState state;
  CHECK_FALSE(state.has_snapshot());
  CHECK_FALSE(state.target_hydration_enabled());

  state.RecordCapture();
  CHECK(state.has_snapshot());
  CHECK_FALSE(state.target_hydration_enabled());

  // Repeated live captures, including captures made while clearing caches,
  // must never promote the backing to a hydration source.
  state.RecordCapture();
  CHECK(state.has_snapshot());
  CHECK_FALSE(state.target_hydration_enabled());

  state.Reset();
  CHECK_FALSE(state.has_snapshot());
  CHECK_FALSE(state.target_hydration_enabled());

  state.RecordRestore();
  CHECK(state.has_snapshot());
  CHECK(state.target_hydration_enabled());

  // Capturing a restored state refreshes its canonical image without
  // revoking the authority established by the restore.
  state.RecordCapture();
  CHECK(state.target_hydration_enabled());
}

TEST_CASE("Metal wrapped 4x frame ownership spans every physical EDRAM tile",
          "[graphics][metal][edram]") {
  CanonicalEdramTileOwnership ownership;
  CanonicalEdramSurfaceLayout color_layout;
  color_layout.base_tiles = 0;
  color_layout.pitch_tiles = 32;
  color_layout.msaa_samples = xenos::MsaaSamples::k4X;
  REQUIRE(ownership.MarkSurface(color_layout, 1280, 720,
                                CanonicalEdramOwnerKind::kColorTarget, 1) == 1);
  for (uint32_t tile = 0; tile < xenos::kEdramTileCount; ++tile) {
    CHECK(ownership.owner(tile).kind == CanonicalEdramOwnerKind::kColorTarget);
    CHECK(ownership.owner(tile).target_key == 1);
  }

  CanonicalEdramSurfaceLayout depth_layout = color_layout;
  depth_layout.base_tiles = 1024;
  depth_layout.is_depth = true;
  REQUIRE(ownership.MarkSurface(depth_layout, 1280, 720,
                                CanonicalEdramOwnerKind::kDepthStencilTarget, 2) == 2);
  for (uint32_t tile = 0; tile < xenos::kEdramTileCount; ++tile) {
    CHECK(ownership.owner(tile).kind == CanonicalEdramOwnerKind::kDepthStencilTarget);
    CHECK(ownership.owner(tile).target_key == 2);
  }
}

TEST_CASE("Metal canonical EDRAM addressing preserves samples and wraps tiles",
          "[graphics][metal][edram]") {
  CanonicalEdramSurfaceLayout layout;
  layout.base_tiles = xenos::kEdramTileCount - 1;
  layout.pitch_tiles = 2;
  layout.msaa_samples = xenos::MsaaSamples::k4X;

  std::vector<uint8_t> edram(xenos::kEdramSizeBytes, 0);
  for (uint32_t sample = 0; sample < 4; ++sample) {
    REQUIRE(WriteCanonicalEdramSample(edram, layout, 79, 15, sample,
                                      {0xA5000000u | sample, 0}));
  }
  for (uint32_t sample = 0; sample < 4; ++sample) {
    std::array<uint32_t, 2> words;
    REQUIRE(ReadCanonicalEdramSample(edram, layout, 79, 15, sample, words));
    CHECK(words[0] == (0xA5000000u | sample));
  }

  // 64-bpp samples straddling word 79 must continue in the next wrapping tile.
  layout.is_64bpp = true;
  layout.msaa_samples = xenos::MsaaSamples::k1X;
  REQUIRE(WriteCanonicalEdramSample(edram, layout, 39, 0, 0,
                                    {0x11223344u, 0x55667788u}));
  std::array<uint32_t, 2> words;
  REQUIRE(ReadCanonicalEdramSample(edram, layout, 39, 0, 0, words));
  CHECK(words == std::array<uint32_t, 2>{0x11223344u, 0x55667788u});
  CHECK(GetCanonicalEdramDwordIndex(layout, 40, 0, 0, 0) /
            (xenos::kEdramTileWidthSamples * xenos::kEdramTileHeightSamples) ==
        0);
  CHECK(GetCanonicalEdramDwordIndex(layout, 0, 0, 1, 0) == SIZE_MAX);
  CHECK(GetCanonicalEdramDwordIndex(layout, 0, 0, 0, 2) == SIZE_MAX);
  CHECK_FALSE(ReadCanonicalEdramSample(edram, layout, 0, 0, 1, words));
  CHECK_FALSE(WriteCanonicalEdramSample(edram, layout, 0, 0, 1, {1, 2}));
}

TEST_CASE("Metal canonical depth addressing swaps 40-sample tile halves",
          "[graphics][metal][edram]") {
  CanonicalEdramSurfaceLayout color;
  color.base_tiles = xenos::kEdramTileCount - 1;
  color.pitch_tiles = 2;
  color.msaa_samples = xenos::MsaaSamples::k1X;
  CanonicalEdramSurfaceLayout depth = color;
  depth.is_depth = true;
  const size_t words_per_tile =
      size_t(xenos::kEdramTileWidthSamples) * xenos::kEdramTileHeightSamples;
  for (uint32_t x : {0u, 39u, 40u, 79u}) {
    size_t color_index = GetCanonicalEdramDwordIndex(color, x, 0, 0, 0);
    size_t depth_index = GetCanonicalEdramDwordIndex(depth, x, 0, 0, 0);
    CHECK(color_index / words_per_tile == depth_index / words_per_tile);
    CHECK(depth_index % xenos::kEdramTileWidthSamples ==
          (x + xenos::kEdramTileWidthSamples / 2) % xenos::kEdramTileWidthSamples);
  }
  for (xenos::MsaaSamples msaa :
       {xenos::MsaaSamples::k2X, xenos::MsaaSamples::k4X}) {
    depth.msaa_samples = msaa;
    uint32_t sample_count = GetCanonicalEdramSampleCount(msaa);
    for (uint32_t sample = 0; sample < sample_count; ++sample) {
      size_t left = GetCanonicalEdramDwordIndex(depth, 0, 16, sample, 0);
      size_t right = GetCanonicalEdramDwordIndex(depth, 79, 16, sample, 0);
      CHECK(left < xenos::kEdramSizeBytes / 4);
      CHECK(right < xenos::kEdramSizeBytes / 4);
    }
  }
}

TEST_CASE("Metal canonical EDRAM ownership merges only the newest alias",
          "[graphics][metal][edram]") {
  CanonicalEdramSurfaceLayout layout;
  layout.base_tiles = 7;
  layout.pitch_tiles = 1;
  layout.msaa_samples = xenos::MsaaSamples::k1X;

  CanonicalEdramTileOwnership ownership;
  ownership.Reset();
  ownership.MarkSurface(layout, 80, 16, CanonicalEdramOwnerKind::kColorTarget, 0x11);
  CHECK(ownership.SurfaceNeedsHydration(layout, 80, 16,
                                        CanonicalEdramOwnerKind::kDepthStencilTarget, 0x22, 0));
  ownership.MarkSurface(layout, 80, 16, CanonicalEdramOwnerKind::kDepthStencilTarget, 0x22);
  CHECK_FALSE(ownership.SurfaceNeedsHydration(
      layout, 80, 16, CanonicalEdramOwnerKind::kDepthStencilTarget, 0x22,
      ownership.sequence()));

  std::vector<uint8_t> destination(xenos::kEdramSizeBytes, 0xCD);
  std::vector<uint8_t> stale(xenos::kEdramSizeBytes, 0x11);
  std::vector<uint8_t> current(xenos::kEdramSizeBytes, 0x22);
  REQUIRE(ownership.MergeOwnedTiles(destination, stale, CanonicalEdramOwnerKind::kColorTarget,
                                    0x11));
  CHECK(destination[size_t(7) * 80 * 16 * 4] == 0xCD);
  REQUIRE(ownership.MergeOwnedTiles(destination, current,
                                    CanonicalEdramOwnerKind::kDepthStencilTarget, 0x22));
  CHECK(destination[size_t(7) * 80 * 16 * 4] == 0x22);
  CHECK(destination[size_t(8) * 80 * 16 * 4] == 0xCD);
}

TEST_CASE("Metal canonical EDRAM preserves untouched words across same-tile aliases",
          "[graphics][metal][edram]") {
  CanonicalEdramSurfaceLayout layout;
  layout.base_tiles = 91;
  layout.pitch_tiles = 1;
  layout.msaa_samples = xenos::MsaaSamples::k1X;

  CanonicalEdramTileOwnership ownership;
  ownership.Reset();
  std::vector<uint8_t> canonical(xenos::kEdramSizeBytes, 0);

  // Target A owns the full tile and writes two words far enough apart that a
  // smaller aliased target B will touch only the first one.
  std::vector<uint8_t> target_a = canonical;
  REQUIRE(WriteCanonicalEdramSample(target_a, layout, 0, 0, 0,
                                    {0xAAAAAAAAu, 0}));
  REQUIRE(WriteCanonicalEdramSample(target_a, layout, 60, 0, 0,
                                    {0xA0A0A0A0u, 0}));
  uint64_t target_a_sequence = ownership.MarkSurface(
      layout, 80, 16, CanonicalEdramOwnerKind::kColorTarget, 0xA);
  REQUIRE(ownership.MergeOwnedTiles(canonical, target_a,
                                    CanonicalEdramOwnerKind::kColorTarget, 0xA));

  // B must begin from the synchronized canonical tile before its partial
  // write. Whole-tile ownership is then safe because its export scratch still
  // contains A's untouched word at x=60.
  CHECK(ownership.SurfaceNeedsHydration(
      layout, 20, 16, CanonicalEdramOwnerKind::kColorTarget, 0xB, 0));
  std::vector<uint8_t> target_b = canonical;
  REQUIRE(WriteCanonicalEdramSample(target_b, layout, 0, 0, 0,
                                    {0xBBBBBBBBu, 0}));
  ownership.MarkSurface(layout, 20, 16, CanonicalEdramOwnerKind::kColorTarget,
                        0xB);
  REQUIRE(ownership.MergeOwnedTiles(canonical, target_b,
                                    CanonicalEdramOwnerKind::kColorTarget, 0xB));

  CHECK(ownership.SurfaceNeedsHydration(
      layout, 80, 16, CanonicalEdramOwnerKind::kColorTarget, 0xA,
      target_a_sequence));
  std::array<uint32_t, 2> words;
  REQUIRE(ReadCanonicalEdramSample(canonical, layout, 0, 0, 0, words));
  CHECK(words[0] == 0xBBBBBBBBu);
  REQUIRE(ReadCanonicalEdramSample(canonical, layout, 60, 0, 0, words));
  CHECK(words[0] == 0xA0A0A0A0u);
}

TEST_CASE("Metal canonical EDRAM preserves unowned raw floating sentinels",
          "[graphics][metal][edram]") {
  CanonicalEdramTileOwnership ownership;
  ownership.Reset();
  std::vector<uint8_t> canonical(xenos::kEdramSizeBytes, 0);
  std::vector<uint8_t> exported(xenos::kEdramSizeBytes, 0xEF);
  constexpr uint32_t kNanSentinel = 0x7FC12345u;
  std::memcpy(canonical.data() + 1234, &kNanSentinel, sizeof(kNanSentinel));
  REQUIRE(ownership.MergeOwnedTiles(canonical, exported, CanonicalEdramOwnerKind::kColorTarget,
                                    0x1234));
  uint32_t retained = 0;
  std::memcpy(&retained, canonical.data() + 1234, sizeof(retained));
  CHECK(retained == kNanSentinel);
}

TEST_CASE("Metal canonical color conversion covers every Xenos storage format",
          "[graphics][metal][edram]") {
  const std::array<xenos::ColorRenderTargetFormat, 12> formats = {
      xenos::ColorRenderTargetFormat::k_8_8_8_8,
      xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA,
      xenos::ColorRenderTargetFormat::k_2_10_10_10,
      xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT,
      xenos::ColorRenderTargetFormat::k_16_16,
      xenos::ColorRenderTargetFormat::k_16_16_16_16,
      xenos::ColorRenderTargetFormat::k_16_16_FLOAT,
      xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT,
      xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10,
      xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16,
      xenos::ColorRenderTargetFormat::k_32_FLOAT,
      xenos::ColorRenderTargetFormat::k_32_32_FLOAT,
  };
  const std::array<float, 4> rgba = {0.125f, 0.5f, 0.875f, 1.0f};
  for (xenos::ColorRenderTargetFormat format : formats) {
    std::array<uint32_t, 2> words;
    std::array<float, 4> unpacked;
    INFO("format " << uint32_t(format));
    REQUIRE(PackCanonicalEdramColor(rgba, format, words));
    REQUIRE(UnpackCanonicalEdramColor(words, format, unpacked));
    const uint32_t component_count = xenos::GetColorRenderTargetFormatComponentCount(format);
    for (uint32_t component = 0; component < component_count; ++component) {
      CHECK(std::isfinite(unpacked[component]));
    }
  }
}

TEST_CASE("Metal canonical fixed16 and non-finite conversion matches Xenos endpoints",
          "[graphics][metal][edram]") {
  CHECK(xenos::Float7e3To32(0x000) == 0.0f);
  CHECK(xenos::Float7e3To32(0x080) == 0.25f);
  CHECK(xenos::Float7e3To32(0x081) == Catch::Approx(0.251953125f));
  CHECK(xenos::Float7e3To32(0x3FF) == 31.875f);

  for (float boundary : {-32.0f, -1.0f, 0.0f, 1.0f, 32.0f}) {
    std::array<uint32_t, 2> words;
    std::array<float, 4> unpacked;
    REQUIRE(PackCanonicalEdramColor({boundary, boundary, boundary, boundary},
                                    xenos::ColorRenderTargetFormat::k_16_16_16_16, words));
    REQUIRE(UnpackCanonicalEdramColor(words,
                                      xenos::ColorRenderTargetFormat::k_16_16_16_16, unpacked));
    CHECK(unpacked[0] == Catch::Approx(boundary).margin(32.0f / 32767.0f));
  }

  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  std::array<uint32_t, 2> words;
  std::array<float, 4> unpacked;
  REQUIRE(PackCanonicalEdramColor({nan, inf, -inf, inf},
                                  xenos::ColorRenderTargetFormat::k_8_8_8_8, words));
  REQUIRE(UnpackCanonicalEdramColor(words, xenos::ColorRenderTargetFormat::k_8_8_8_8,
                                    unpacked));
  CHECK(unpacked[0] == 0.0f);
  CHECK(unpacked[1] == 1.0f);
  CHECK(unpacked[2] == 0.0f);
  CHECK(unpacked[3] == 1.0f);

  REQUIRE(PackCanonicalEdramColor({nan, inf, -inf, 1.0f},
                                  xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT, words));
  REQUIRE(UnpackCanonicalEdramColor(words,
                                    xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT,
                                    unpacked));
  CHECK(unpacked[0] == 0.0f);
  CHECK(unpacked[1] == Catch::Approx(31.875f));
  CHECK(unpacked[2] == 0.0f);
}

TEST_CASE("Metal canonical depth words preserve D24S8 stencil and every sample",
          "[graphics][metal][edram]") {
  CanonicalEdramSurfaceLayout layout;
  layout.base_tiles = 1024;
  layout.pitch_tiles = 32;
  layout.msaa_samples = xenos::MsaaSamples::k4X;
  layout.is_depth = true;
  std::vector<uint8_t> edram(xenos::kEdramSizeBytes, 0);
  for (uint32_t sample = 0; sample < 4; ++sample) {
    uint32_t depth = 0x102030u + sample;
    uint32_t stencil = 0x80u + sample;
    REQUIRE(WriteCanonicalEdramSample(edram, layout, 25, 9, sample,
                                      {(depth << 8) | stencil, 0}));
  }
  for (uint32_t sample = 0; sample < 4; ++sample) {
    std::array<uint32_t, 2> words;
    REQUIRE(ReadCanonicalEdramSample(edram, layout, 25, 9, sample, words));
    CHECK((words[0] >> 8) == 0x102030u + sample);
    CHECK((words[0] & 0xFF) == 0x80u + sample);
  }
}

}  // namespace
}  // namespace rex::graphics::metal
