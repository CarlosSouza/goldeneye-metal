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
#include <rex/graphics/metal/color_target_storage.h>

namespace rex::graphics::metal {
namespace {

struct ScalarCanonicalEdramTileOwnership {
  std::array<CanonicalEdramTileOwner, xenos::kEdramTileCount> owners = {};
  uint64_t sequence = 0;

  uint64_t MarkSurface(const CanonicalEdramSurfaceLayout& layout, uint32_t width, uint32_t height,
                       CanonicalEdramOwnerKind kind, uint64_t target_key) {
    if (!layout.pitch_tiles || !width || !height || kind == CanonicalEdramOwnerKind::kRawSnapshot ||
        !target_key) {
      return sequence;
    }
    ++sequence;
    const uint32_t sample_width = width << uint32_t(layout.msaa_samples >= xenos::MsaaSamples::k4X);
    const uint32_t sample_height = height
                                   << uint32_t(layout.msaa_samples >= xenos::MsaaSamples::k2X);
    const uint32_t word_width = sample_width * (layout.is_64bpp ? 2 : 1);
    const uint32_t tile_columns =
        (word_width + xenos::kEdramTileWidthSamples - 1) / xenos::kEdramTileWidthSamples;
    const uint32_t tile_rows =
        (sample_height + xenos::kEdramTileHeightSamples - 1) / xenos::kEdramTileHeightSamples;
    const CanonicalEdramTileOwner new_owner = {kind, target_key, sequence};
    for (uint32_t tile_y = 0; tile_y < tile_rows; ++tile_y) {
      for (uint32_t tile_x = 0; tile_x < tile_columns; ++tile_x) {
        const uint32_t tile = (layout.base_tiles + tile_y * layout.pitch_tiles + tile_x) &
                              (xenos::kEdramTileCount - 1);
        owners[tile] = new_owner;
      }
    }
    return sequence;
  }

  bool SurfaceNeedsHydration(const CanonicalEdramSurfaceLayout& layout, uint32_t width,
                             uint32_t height, CanonicalEdramOwnerKind kind, uint64_t target_key,
                             uint64_t hydrated_sequence) const {
    if (!layout.pitch_tiles || !width || !height || !target_key) {
      return true;
    }
    const uint32_t sample_width = width << uint32_t(layout.msaa_samples >= xenos::MsaaSamples::k4X);
    const uint32_t sample_height = height
                                   << uint32_t(layout.msaa_samples >= xenos::MsaaSamples::k2X);
    const uint32_t word_width = sample_width * (layout.is_64bpp ? 2 : 1);
    const uint32_t tile_columns =
        (word_width + xenos::kEdramTileWidthSamples - 1) / xenos::kEdramTileWidthSamples;
    const uint32_t tile_rows =
        (sample_height + xenos::kEdramTileHeightSamples - 1) / xenos::kEdramTileHeightSamples;
    for (uint32_t tile_y = 0; tile_y < tile_rows; ++tile_y) {
      for (uint32_t tile_x = 0; tile_x < tile_columns; ++tile_x) {
        const uint32_t tile = (layout.base_tiles + tile_y * layout.pitch_tiles + tile_x) &
                              (xenos::kEdramTileCount - 1);
        const CanonicalEdramTileOwner& owner = owners[tile];
        if (owner.sequence > hydrated_sequence &&
            (owner.kind != kind || owner.target_key != target_key)) {
          return true;
        }
      }
    }
    return false;
  }
};

void CheckOwnershipMatches(const CanonicalEdramTileOwnership& optimized,
                           const ScalarCanonicalEdramTileOwnership& scalar) {
  REQUIRE(optimized.sequence() == scalar.sequence);
  for (uint32_t tile = 0; tile < xenos::kEdramTileCount; ++tile) {
    if (optimized.owner(tile) != scalar.owners[tile]) {
      INFO("physical tile " << tile);
      CHECK(optimized.owner(tile) == scalar.owners[tile]);
      return;
    }
  }
  CHECK(true);
}

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

  state.Reset();
  state.RecordExactDraw();
  CHECK(state.has_snapshot());
  CHECK(state.target_hydration_enabled());
}

TEST_CASE("Metal canonical EDRAM classifies full-frame 4x self-aliasing",
          "[graphics][metal][edram]") {
  CanonicalEdramSurfaceLayout layout;
  layout.pitch_tiles = 32;
  layout.msaa_samples = xenos::MsaaSamples::k4X;
  CHECK(ClassifyCanonicalEdramSurfaceLayout(layout, 1280, 256) ==
        CanonicalEdramSurfaceLayoutClass::kNonAliasing);
  CHECK(ClassifyCanonicalEdramSurfaceLayout(layout, 1280, 512) ==
        CanonicalEdramSurfaceLayoutClass::kNonAliasing);
  CHECK(ClassifyCanonicalEdramSurfaceLayout(layout, 1280, 720) ==
        CanonicalEdramSurfaceLayoutClass::kSelfAliasing);

  layout.base_tiles = xenos::kEdramTileCount;
  CHECK(ClassifyCanonicalEdramSurfaceLayout(layout, 1280, 256) ==
        CanonicalEdramSurfaceLayoutClass::kInvalid);
  layout.base_tiles = 0;
  CHECK(ClassifyCanonicalEdramSurfaceLayout(layout, 1281, 256) ==
        CanonicalEdramSurfaceLayoutClass::kInvalid);
  layout.is_depth = true;
  CHECK(ClassifyCanonicalEdramSurfaceLayout(layout, 1280, 720) ==
        CanonicalEdramSurfaceLayoutClass::kSelfAliasing);

  // A sparse surface may span more than 2048 in its unwrapped tile address
  // range without revisiting a physical tile, while a large modular stride can
  // revisit one after only a few rows.
  layout = {};
  layout.pitch_tiles = 100;
  CHECK(ClassifyCanonicalEdramSurfaceLayout(layout, 80, 16 * 22) ==
        CanonicalEdramSurfaceLayoutClass::kNonAliasing);
  layout.pitch_tiles = 512;
  CHECK(ClassifyCanonicalEdramSurfaceLayout(layout, 80, 16 * 5) ==
        CanonicalEdramSurfaceLayoutClass::kSelfAliasing);
}

TEST_CASE("Metal native alias containment leaves exact-disabled routing unchanged",
          "[graphics][metal][edram]") {
  CHECK_FALSE(ShouldContainCanonicalEdramNativeAlias(false, false));
  CHECK(ShouldContainCanonicalEdramNativeAlias(true, false));
  CHECK(ShouldContainCanonicalEdramNativeAlias(false, true));
  CHECK(ShouldContainCanonicalEdramNativeAlias(true, true));
}

TEST_CASE("Metal wrapped 4x frame ownership spans every physical EDRAM tile",
          "[graphics][metal][edram]") {
  CanonicalEdramTileOwnership ownership;
  CanonicalEdramSurfaceLayout color_layout;
  color_layout.base_tiles = 0;
  color_layout.pitch_tiles = 32;
  color_layout.msaa_samples = xenos::MsaaSamples::k4X;
  REQUIRE(ownership.MarkSurface(color_layout, 1280, 720, CanonicalEdramOwnerKind::kColorTarget,
                                1) == 1);
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

TEST_CASE("Metal canonical ownership fast paths match scalar tile marking",
          "[graphics][metal][edram]") {
  struct SurfaceShape {
    uint32_t pitch_tiles;
    uint32_t width;
    uint32_t height;
    xenos::MsaaSamples msaa_samples;
    bool is_64bpp;
  };
  constexpr std::array<SurfaceShape, 6> kExhaustiveBaseShapes = {{
      // Partial rows, including a physical wrap for high base tiles.
      {8, 83, 19, xenos::MsaaSamples::k1X, false},
      // Tightly packed rows without full physical coverage.
      {4, 320, 31, xenos::MsaaSamples::k1X, false},
      // A common full-frame 4x shape that visits all 2048 tiles and then wraps.
      {32, 1280, 720, xenos::MsaaSamples::k4X, false},
      // Overlapping logical rows (tile columns exceed pitch).
      {2, 320, 33, xenos::MsaaSamples::k2X, true},
      // 64-bpp row wrapping with a non-power-of-two pitch.
      {5, 201, 47, xenos::MsaaSamples::k1X, true},
      // 4x 64-bpp coverage with gaps between rows.
      {31, 401, 65, xenos::MsaaSamples::k4X, true},
  }};

  for (const SurfaceShape& shape : kExhaustiveBaseShapes) {
    for (uint32_t base = 0; base < xenos::kEdramTileCount; ++base) {
      INFO("base " << base << " pitch " << shape.pitch_tiles << " size " << shape.width << "x"
                   << shape.height << " msaa " << uint32_t(shape.msaa_samples) << " 64bpp "
                   << shape.is_64bpp);
      CanonicalEdramSurfaceLayout layout;
      layout.base_tiles = base;
      layout.pitch_tiles = shape.pitch_tiles;
      layout.msaa_samples = shape.msaa_samples;
      layout.is_64bpp = shape.is_64bpp;
      layout.is_depth = (base & 1) != 0;

      CanonicalEdramTileOwnership optimized;
      ScalarCanonicalEdramTileOwnership scalar;
      REQUIRE(optimized.MarkSurface(layout, shape.width, shape.height,
                                    CanonicalEdramOwnerKind::kColorTarget, 0x1000 + base) ==
              scalar.MarkSurface(layout, shape.width, shape.height,
                                 CanonicalEdramOwnerKind::kColorTarget, 0x1000 + base));
      CheckOwnershipMatches(optimized, scalar);
    }
  }

  // Exercise mixed aliases cumulatively so old ownership must survive outside
  // every new surface. The fixed generator makes failures reproducible.
  CanonicalEdramTileOwnership optimized;
  ScalarCanonicalEdramTileOwnership scalar;
  uint32_t random_state = 0xC001D00Du;
  auto next_random = [&]() {
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
  };
  constexpr std::array<xenos::MsaaSamples, 3> kMsaaSamples = {
      xenos::MsaaSamples::k1X,
      xenos::MsaaSamples::k2X,
      xenos::MsaaSamples::k4X,
  };
  for (uint32_t iteration = 0; iteration < 2048; ++iteration) {
    CanonicalEdramSurfaceLayout layout;
    layout.base_tiles = next_random() & (xenos::kEdramTileCount - 1);
    layout.pitch_tiles = 1 + next_random() % 96;
    layout.msaa_samples = kMsaaSamples[next_random() % kMsaaSamples.size()];
    layout.is_64bpp = (next_random() & 1) != 0;
    layout.is_depth = (next_random() & 1) != 0;
    const uint32_t width = 1 + next_random() % 1280;
    const uint32_t height = 1 + next_random() % 720;
    const CanonicalEdramOwnerKind kind = (next_random() & 1)
                                             ? CanonicalEdramOwnerKind::kColorTarget
                                             : CanonicalEdramOwnerKind::kDepthStencilTarget;
    const uint64_t target_key = 1 + next_random() % 17;
    INFO("random iteration " << iteration);
    REQUIRE(optimized.MarkSurface(layout, width, height, kind, target_key) ==
            scalar.MarkSurface(layout, width, height, kind, target_key));
    CheckOwnershipMatches(optimized, scalar);

    const CanonicalEdramOwnerKind query_kind = (next_random() & 1)
                                                   ? CanonicalEdramOwnerKind::kColorTarget
                                                   : CanonicalEdramOwnerKind::kDepthStencilTarget;
    const uint64_t query_key = 1 + next_random() % 17;
    const uint64_t hydrated_sequence = next_random() % (scalar.sequence + 1);
    CHECK(optimized.SurfaceNeedsHydration(layout, width, height, query_kind, query_key,
                                          hydrated_sequence) ==
          scalar.SurfaceNeedsHydration(layout, width, height, query_kind, query_key,
                                       hydrated_sequence));
  }
}

TEST_CASE("Metal canonical ownership repeated writes preserve stale alias ordering",
          "[graphics][metal][edram]") {
  CanonicalEdramSurfaceLayout layout;
  layout.base_tiles = xenos::kEdramTileCount - 7;
  layout.pitch_tiles = 32;
  layout.msaa_samples = xenos::MsaaSamples::k4X;

  CanonicalEdramTileOwnership optimized;
  ScalarCanonicalEdramTileOwnership scalar;
  const uint64_t first_sequence =
      optimized.MarkSurface(layout, 1280, 720, CanonicalEdramOwnerKind::kColorTarget, 0xA);
  REQUIRE(first_sequence ==
          scalar.MarkSurface(layout, 1280, 720, CanonicalEdramOwnerKind::kColorTarget, 0xA));
  REQUIRE(first_sequence == 1);
  CheckOwnershipMatches(optimized, scalar);

  const uint64_t repeated_sequence =
      optimized.MarkSurface(layout, 1280, 720, CanonicalEdramOwnerKind::kColorTarget, 0xA);
  REQUIRE(repeated_sequence ==
          scalar.MarkSurface(layout, 1280, 720, CanonicalEdramOwnerKind::kColorTarget, 0xA));
  REQUIRE(repeated_sequence == 2);
  CheckOwnershipMatches(optimized, scalar);

  // A cache for B synchronized after sequence 1 is stale after the repeated A
  // write, even though A was already the owner before that write.
  CHECK(optimized.SurfaceNeedsHydration(
      layout, 1280, 720, CanonicalEdramOwnerKind::kDepthStencilTarget, 0xB, first_sequence));
  CHECK_FALSE(optimized.SurfaceNeedsHydration(
      layout, 1280, 720, CanonicalEdramOwnerKind::kColorTarget, 0xA, first_sequence));
  for (uint32_t tile = 0; tile < xenos::kEdramTileCount; ++tile) {
    REQUIRE(optimized.owner(tile).sequence == repeated_sequence);
    REQUIRE(optimized.owner(tile).target_key == 0xA);
  }

  CanonicalEdramSurfaceLayout overlapping = layout;
  overlapping.pitch_tiles = 2;
  overlapping.msaa_samples = xenos::MsaaSamples::k2X;
  overlapping.is_64bpp = true;
  REQUIRE(
      optimized.MarkSurface(overlapping, 320, 33, CanonicalEdramOwnerKind::kDepthStencilTarget,
                            0xB) ==
      scalar.MarkSurface(overlapping, 320, 33, CanonicalEdramOwnerKind::kDepthStencilTarget, 0xB));
  CheckOwnershipMatches(optimized, scalar);
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

  CanonicalEdramSurfaceLayout sample_order;
  sample_order.pitch_tiles = 1;
  sample_order.msaa_samples = xenos::MsaaSamples::k2X;
  CHECK(GetCanonicalEdramDwordIndex(sample_order, 0, 0, 0, 0) == 0);
  CHECK(GetCanonicalEdramDwordIndex(sample_order, 0, 0, 1, 0) == 80);
  sample_order.msaa_samples = xenos::MsaaSamples::k4X;
  CHECK(GetCanonicalEdramDwordIndex(sample_order, 0, 0, 0, 0) == 0);
  CHECK(GetCanonicalEdramDwordIndex(sample_order, 0, 0, 1, 0) == 80);
  CHECK(GetCanonicalEdramDwordIndex(sample_order, 0, 0, 2, 0) == 1);
  CHECK(GetCanonicalEdramDwordIndex(sample_order, 0, 0, 3, 0) == 81);

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

TEST_CASE("Metal color storage strategy separates exact native and raw-sidecar formats",
          "[graphics][metal][edram]") {
  CHECK(GetColorTargetStorageStrategy(xenos::ColorRenderTargetFormat::k_8_8_8_8).kind ==
        ColorTargetStorageKind::kBgra8Unorm);
  CHECK(GetColorTargetStorageStrategy(xenos::ColorRenderTargetFormat::k_2_10_10_10).kind ==
        ColorTargetStorageKind::kRgb10A2Unorm);
  CHECK(GetColorTargetStorageStrategy(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA).kind ==
        ColorTargetStorageKind::kRaw32SidecarRequired);
  CHECK_FALSE(IsCanonicalEdramColorFormatSupportedByMetal(
      xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA));
  CHECK(IsCanonicalEdramColorFormatSupportedByMetal(
      xenos::ColorRenderTargetFormat::k_2_10_10_10));
  CHECK_FALSE(IsCanonicalEdramColorFormatSupportedByMetal(
      xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10));
  CHECK(GetColorTargetStorageKey(xenos::ColorRenderTargetFormat::k_8_8_8_8, 1) !=
        GetColorTargetStorageKey(xenos::ColorRenderTargetFormat::k_2_10_10_10, 1));
  CHECK(GetColorTargetStorageKey(xenos::ColorRenderTargetFormat::k_2_10_10_10, 1) !=
        GetColorTargetStorageKey(xenos::ColorRenderTargetFormat::k_2_10_10_10, 4));
}

TEST_CASE("Xenos PWL gamma float conversion documents non-invertible raw codes",
          "[graphics][metal][edram]") {
  constexpr std::array<uint32_t, 8> kNonInvertibleCodes = {17, 21, 34, 42, 66, 74, 82, 90};
  std::vector<uint32_t> non_invertible_codes;
  for (uint32_t code = 0; code < 256; ++code) {
    uint32_t packed = code | (code << 8) | (code << 16) | (uint32_t(0xA5) << 24);
    std::array<float, 4> linear;
    std::array<uint32_t, 2> repacked;
    INFO("gamma code " << code);
    REQUIRE(UnpackCanonicalEdramColor(
        {packed, 0}, xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA, linear));
    REQUIRE(PackCanonicalEdramColor(
        linear, xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA, repacked));
    if (repacked[0] != packed) {
      non_invertible_codes.push_back(code);
    }
  }
  CHECK(non_invertible_codes ==
        std::vector<uint32_t>(kNonInvertibleCodes.begin(), kNonInvertibleCodes.end()));
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
