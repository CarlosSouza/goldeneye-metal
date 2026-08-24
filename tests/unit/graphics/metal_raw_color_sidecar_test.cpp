#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/metal/raw_color_sidecar.h>

namespace rex::graphics::metal {
namespace {

uint32_t ReferenceFloat32ToXenos7e3(float value) {
  if (!(value > 0.0f)) {
    return 0;
  }
  if (value >= 31.875f) {
    return 0x3FF;
  }
  uint32_t best = 0;
  double best_distance = std::numeric_limits<double>::infinity();
  for (uint32_t candidate = 0; candidate < 1024; ++candidate) {
    const double distance = std::abs(double(xenos::Float7e3To32(candidate)) - double(value));
    if (distance < best_distance || (distance == best_distance && !(candidate & 1) && (best & 1))) {
      best = candidate;
      best_distance = distance;
    }
  }
  return best;
}

uint32_t MakeRawWord(uint32_t x, uint32_t y, uint32_t sample, uint32_t word) {
  return 0x80000000u | ((sample & 3u) << 27) | ((word & 1u) << 26) | ((y & 0x3FFFu) << 12) |
         (x & 0xFFFu);
}

TEST_CASE("Raw color sidecar capability gates separate storage from execution",
          "[graphics][metal][edram][sidecar]") {
  constexpr std::array<xenos::ColorRenderTargetFormat, 12> kFormats = {
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
  for (xenos::ColorRenderTargetFormat format : kFormats) {
    INFO("format " << uint32_t(format));
    const RawColorSidecarFormatCapabilities capabilities =
        GetRawColorSidecarFormatCapabilities(format);
    CHECK(capabilities.words_per_sample == (xenos::IsColorRenderTargetFormat64bpp(format) ? 2 : 1));
    CHECK(CanRawColorSidecarStoreCanonicalLosslessly(format));
    CHECK_FALSE(CanRawColorSidecarRasterizeExactly(format));
    CHECK_FALSE(CanRawColorSidecarDirectResolveExactly(format));
  }

  constexpr auto kUnknownFormat = static_cast<xenos::ColorRenderTargetFormat>(9);
  CHECK_FALSE(CanRawColorSidecarStoreCanonicalLosslessly(kUnknownFormat));
  CHECK_FALSE(CanRawColorSidecarRasterizeExactly(kUnknownFormat));
  CHECK_FALSE(CanRawColorSidecarDirectResolveExactly(kUnknownFormat));
}

TEST_CASE("Raw color aliases share storage without sharing identity",
          "[graphics][metal][edram][sidecar]") {
  struct AliasPair {
    xenos::ColorRenderTargetFormat base;
    xenos::ColorRenderTargetFormat alias;
  };
  constexpr std::array<AliasPair, 2> kAliases = {{
      {xenos::ColorRenderTargetFormat::k_2_10_10_10,
       xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10},
      {xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT,
       xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16},
  }};

  for (const AliasPair& formats : kAliases) {
    RawColorSidecarStorageLayout base_layout;
    RawColorSidecarStorageLayout alias_layout;
    REQUIRE(GetRawColorSidecarStorageLayout(formats.base, 83, 19, xenos::MsaaSamples::k4X,
                                            base_layout));
    REQUIRE(GetRawColorSidecarStorageLayout(formats.alias, 83, 19, xenos::MsaaSamples::k4X,
                                            alias_layout));
    CHECK(base_layout == alias_layout);

    RawColorSidecar base;
    RawColorSidecar alias;
    REQUIRE(base.Configure(formats.base, 83, 19, xenos::MsaaSamples::k4X));
    REQUIRE(alias.Configure(formats.alias, 83, 19, xenos::MsaaSamples::k4X));
    CHECK(base.storage_format() == alias.storage_format());
    CHECK_FALSE(base.key() == alias.key());
    CHECK(base.guest_format() == formats.base);
    CHECK(alias.guest_format() == formats.alias);
  }
}

TEST_CASE("Raw color sidecar validates sample-major storage layout",
          "[graphics][metal][edram][sidecar]") {
  RawColorSidecarStorageLayout layout;
  REQUIRE(GetRawColorSidecarStorageLayout(xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT, 3, 2,
                                          xenos::MsaaSamples::k4X, layout));
  CHECK(layout.sample_count == 4);
  CHECK(layout.words_per_sample == 2);
  CHECK(layout.word_count == 48);

  for (uint32_t sample = 0; sample < 4; ++sample) {
    for (uint32_t y = 0; y < 2; ++y) {
      for (uint32_t x = 0; x < 3; ++x) {
        for (uint32_t word = 0; word < 2; ++word) {
          CHECK(GetRawColorSidecarWordIndex(layout, x, y, sample, word) ==
                (((size_t(sample) * 2 + y) * 3 + x) * 2 + word));
        }
      }
    }
  }
  CHECK(GetRawColorSidecarWordIndex(layout, 3, 0, 0, 0) == SIZE_MAX);
  CHECK(GetRawColorSidecarWordIndex(layout, 0, 2, 0, 0) == SIZE_MAX);
  CHECK(GetRawColorSidecarWordIndex(layout, 0, 0, 4, 0) == SIZE_MAX);
  CHECK(GetRawColorSidecarWordIndex(layout, 0, 0, 0, 2) == SIZE_MAX);

  RawColorSidecarStorageLayout invalid;
  CHECK_FALSE(GetRawColorSidecarStorageLayout(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA, 0, 1,
                                              xenos::MsaaSamples::k1X, invalid));
  CHECK_FALSE(GetRawColorSidecarStorageLayout(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA, 1, 1,
                                              static_cast<xenos::MsaaSamples>(3), invalid));
  CHECK_FALSE(GetRawColorSidecarStorageLayout(static_cast<xenos::ColorRenderTargetFormat>(9), 1, 1,
                                              xenos::MsaaSamples::k1X, invalid));
  CHECK_FALSE(GetRawColorSidecarStorageLayout(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA,
                                              (uint32_t(1) << xenos::kEdramPitchPixelsBits) + 1, 1,
                                              xenos::MsaaSamples::k1X, invalid));
  CHECK_FALSE(GetRawColorSidecarStorageLayout(
      xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT, xenos::kTexture2DCubeMaxWidthHeight,
      xenos::kTexture2DCubeMaxWidthHeight, xenos::MsaaSamples::k4X, invalid));
}

TEST_CASE("Raw color sidecar preserves canonical words at 1x 2x and 4x",
          "[graphics][metal][edram][sidecar]") {
  constexpr std::array<xenos::ColorRenderTargetFormat, 3> kFormats = {
      xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA,
      xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT,
      xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT,
  };
  constexpr std::array<xenos::MsaaSamples, 3> kSampleCounts = {
      xenos::MsaaSamples::k1X,
      xenos::MsaaSamples::k2X,
      xenos::MsaaSamples::k4X,
  };
  constexpr uint32_t kWidth = 83;
  constexpr uint32_t kHeight = 19;

  for (xenos::ColorRenderTargetFormat format : kFormats) {
    for (xenos::MsaaSamples msaa_samples : kSampleCounts) {
      INFO("format " << uint32_t(format) << " msaa " << uint32_t(msaa_samples));
      RawColorSidecar sidecar;
      REQUIRE(sidecar.Configure(format, kWidth, kHeight, msaa_samples));

      CanonicalEdramSurfaceLayout canonical_layout;
      canonical_layout.base_tiles = xenos::kEdramTileCount - 11;
      canonical_layout.pitch_tiles = 8;
      canonical_layout.msaa_samples = msaa_samples;
      canonical_layout.is_64bpp = xenos::IsColorRenderTargetFormat64bpp(format);

      std::vector<uint8_t> source(xenos::kEdramSizeBytes, 0x5A);
      const uint32_t sample_count = GetCanonicalEdramSampleCount(msaa_samples);
      for (uint32_t sample = 0; sample < sample_count; ++sample) {
        for (uint32_t y = 0; y < kHeight; ++y) {
          for (uint32_t x = 0; x < kWidth; ++x) {
            REQUIRE(WriteCanonicalEdramSample(
                source, canonical_layout, x, y, sample,
                {MakeRawWord(x, y, sample, 0), MakeRawWord(x, y, sample, 1)}));
          }
        }
      }

      REQUIRE(sidecar.RestoreCanonical(source, canonical_layout));
      CHECK(sidecar.authority() == RawColorSidecarAuthority::kRawAuthoritative);
      REQUIRE(sidecar.raw_words().size() == sidecar.storage_layout().word_count);
      for (uint32_t sample = 0; sample < sample_count; ++sample) {
        for (uint32_t y = 0; y < kHeight; ++y) {
          for (uint32_t x = 0; x < kWidth; ++x) {
            std::array<uint32_t, 2> words;
            REQUIRE(sidecar.ReadRawSample(x, y, sample, words));
            CHECK(words[0] == MakeRawWord(x, y, sample, 0));
            if (canonical_layout.is_64bpp) {
              CHECK(words[1] == MakeRawWord(x, y, sample, 1));
            } else {
              CHECK(words[1] == 0);
            }
          }
        }
      }

      std::vector<uint8_t> exported(xenos::kEdramSizeBytes, 0xCD);
      REQUIRE(sidecar.ExportCanonical(exported, canonical_layout));
      for (uint32_t sample = 0; sample < sample_count; ++sample) {
        for (uint32_t y = 0; y < kHeight; ++y) {
          for (uint32_t x = 0; x < kWidth; ++x) {
            std::array<uint32_t, 2> words;
            REQUIRE(ReadCanonicalEdramSample(exported, canonical_layout, x, y, sample, words));
            CHECK(words[0] == MakeRawWord(x, y, sample, 0));
            if (canonical_layout.is_64bpp) {
              CHECK(words[1] == MakeRawWord(x, y, sample, 1));
            }
          }
        }
      }
      std::array<uint32_t, 2> untouched;
      REQUIRE(ReadCanonicalEdramSample(exported, canonical_layout, kWidth + 5, 0, 0, untouched));
      CHECK(untouched[0] == 0xCDCDCDCDu);
      if (canonical_layout.is_64bpp) {
        CHECK(untouched[1] == 0xCDCDCDCDu);
      }

      CanonicalEdramSurfaceLayout overlapping_layout = canonical_layout;
      overlapping_layout.pitch_tiles = 1;
      const std::vector<uint8_t> before_rejected_export = exported;
      CHECK_FALSE(sidecar.ExportCanonical(exported, overlapping_layout));
      CHECK(exported == before_rejected_export);
    }
  }
}

TEST_CASE("Raw color sidecar rejects canonical layouts with physical aliases",
          "[graphics][metal][edram][sidecar]") {
  RawColorSidecar sidecar;
  REQUIRE(sidecar.Configure(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA, 1,
                            xenos::kTexture2DCubeMaxWidthHeight, xenos::MsaaSamples::k1X));
  CanonicalEdramSurfaceLayout aliased_layout;
  aliased_layout.base_tiles = 0;
  aliased_layout.pitch_tiles = 8;
  std::vector<uint8_t> canonical(xenos::kEdramSizeBytes, 0x7E);
  CHECK_FALSE(sidecar.RestoreCanonical(canonical, aliased_layout));
  CHECK(sidecar.authority() == RawColorSidecarAuthority::kUninitialized);

  aliased_layout.pitch_tiles = 1;
  aliased_layout.base_tiles = xenos::kEdramTileCount;
  CHECK_FALSE(sidecar.RestoreCanonical(canonical, aliased_layout));
  CHECK(sidecar.authority() == RawColorSidecarAuthority::kUninitialized);
}

TEST_CASE("Raw color sidecar preserves every PWL gamma byte code",
          "[graphics][metal][edram][sidecar]") {
  RawColorSidecar sidecar;
  REQUIRE(sidecar.Configure(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA, 256, 1,
                            xenos::MsaaSamples::k1X));
  CanonicalEdramSurfaceLayout layout;
  layout.base_tiles = xenos::kEdramTileCount - 2;
  layout.pitch_tiles = 4;

  std::vector<uint8_t> source(xenos::kEdramSizeBytes, 0);
  for (uint32_t code = 0; code < 256; ++code) {
    const uint32_t raw =
        code | ((255u - code) << 8) | ((code ^ 0xA5u) << 16) | ((code ^ 0x5Au) << 24);
    REQUIRE(WriteCanonicalEdramSample(source, layout, code, 0, 0, {raw, 0}));
  }
  REQUIRE(sidecar.RestoreCanonical(source, layout));

  std::vector<uint8_t> exported(xenos::kEdramSizeBytes, 0xCD);
  REQUIRE(sidecar.ExportCanonical(exported, layout));
  for (uint32_t code = 0; code < 256; ++code) {
    const uint32_t expected =
        code | ((255u - code) << 8) | ((code ^ 0xA5u) << 16) | ((code ^ 0x5Au) << 24);
    std::array<uint32_t, 2> actual;
    REQUIRE(ReadCanonicalEdramSample(exported, layout, code, 0, 0, actual));
    CHECK(actual[0] == expected);
  }
}

TEST_CASE("Raw color sidecar preserves every packed 7e3 code",
          "[graphics][metal][edram][sidecar]") {
  RawColorSidecar sidecar;
  REQUIRE(sidecar.Configure(xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT, 1024, 1,
                            xenos::MsaaSamples::k1X));
  CanonicalEdramSurfaceLayout layout;
  layout.base_tiles = xenos::kEdramTileCount - 7;
  layout.pitch_tiles = 13;

  std::vector<uint8_t> source(xenos::kEdramSizeBytes, 0);
  for (uint32_t code = 0; code < 1024; ++code) {
    const uint32_t raw =
        code | ((1023u - code) << 10) | (((code * 37u) & 0x3FFu) << 20) | ((code & 3u) << 30);
    REQUIRE(WriteCanonicalEdramSample(source, layout, code, 0, 0, {raw, 0}));
  }
  REQUIRE(sidecar.RestoreCanonical(source, layout));

  std::vector<uint8_t> exported(xenos::kEdramSizeBytes, 0xCD);
  REQUIRE(sidecar.ExportCanonical(exported, layout));
  for (uint32_t code = 0; code < 1024; ++code) {
    const uint32_t expected =
        code | ((1023u - code) << 10) | (((code * 37u) & 0x3FFu) << 20) | ((code & 3u) << 30);
    std::array<uint32_t, 2> actual;
    REQUIRE(ReadCanonicalEdramSample(exported, layout, code, 0, 0, actual));
    CHECK(actual[0] == expected);
  }
}

TEST_CASE("Xenos 7e3 CPU encoder matches exhaustive reference boundaries",
          "[graphics][metal][edram][sidecar]") {
  for (uint32_t code = 0; code < 1024; ++code) {
    const float decoded = xenos::Float7e3To32(code);
    INFO("7e3 code " << code);
    CHECK(Float32ToXenos7e3(decoded) == code);
    CHECK(Float32ToXenos7e3(decoded) == ReferenceFloat32ToXenos7e3(decoded));
  }

  for (uint32_t lower_code = 0; lower_code < 1023; ++lower_code) {
    const float lower = xenos::Float7e3To32(lower_code);
    const float upper = xenos::Float7e3To32(lower_code + 1);
    const float midpoint = lower + (upper - lower) * 0.5f;
    INFO("7e3 boundary " << lower_code);
    for (float value :
         {std::nextafter(midpoint, lower), midpoint, std::nextafter(midpoint, upper)}) {
      CHECK(Float32ToXenos7e3(value) == ReferenceFloat32ToXenos7e3(value));
    }
  }

  CHECK(Float32ToXenos7e3(-1.0f) == 0);
  CHECK(Float32ToXenos7e3(-std::numeric_limits<float>::infinity()) == 0);
  CHECK(Float32ToXenos7e3(std::numeric_limits<float>::quiet_NaN()) == 0);
  CHECK(Float32ToXenos7e3(std::numeric_limits<float>::infinity()) == 0x3FF);
  CHECK(Float32ToXenos7e3(32.0f) == 0x3FF);
}

TEST_CASE("Raw color sidecar authority refuses stale canonical export",
          "[graphics][metal][edram][sidecar]") {
  RawColorSidecarAuthorityState authority;
  CHECK(authority.state() == RawColorSidecarAuthority::kUninitialized);
  CHECK_FALSE(authority.can_export_canonical());
  CHECK_FALSE(authority.RecordNativeSynchronizedFromRaw());

  authority.RecordCompleteRawWrite();
  CHECK(authority.state() == RawColorSidecarAuthority::kRawAuthoritative);
  CHECK(authority.can_export_canonical());
  REQUIRE(authority.RecordNativeSynchronizedFromRaw());
  CHECK(authority.state() == RawColorSidecarAuthority::kCoherent);
  CHECK(authority.can_export_canonical());

  authority.RecordNativeWrite();
  CHECK(authority.state() == RawColorSidecarAuthority::kNativeAuthoritative);
  CHECK_FALSE(authority.can_export_canonical());
  CHECK_FALSE(authority.RecordNativeSynchronizedFromRaw());
  CHECK(authority.state() == RawColorSidecarAuthority::kNativeAuthoritative);
  authority.Reset();
  CHECK(authority.state() == RawColorSidecarAuthority::kUninitialized);

  RawColorSidecar sidecar;
  REQUIRE(sidecar.Configure(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA, 2, 2,
                            xenos::MsaaSamples::k1X));
  CanonicalEdramSurfaceLayout layout;
  layout.base_tiles = 5;
  layout.pitch_tiles = 1;
  std::vector<uint8_t> canonical(xenos::kEdramSizeBytes, 0x12);
  std::vector<uint8_t> exported(xenos::kEdramSizeBytes, 0xCD);
  const std::vector<uint8_t> untouched = exported;

  CHECK_FALSE(sidecar.can_export_canonical());
  CHECK_FALSE(sidecar.WriteRawSample(0, 0, 0, {0x12345678u, 0}));
  CHECK_FALSE(sidecar.ExportCanonical(exported, layout));
  CHECK(exported == untouched);

  std::vector<uint32_t> complete_words(sidecar.storage_layout().word_count, 0x89ABCDEFu);
  REQUIRE(sidecar.ReplaceRawWords(complete_words));
  CHECK(sidecar.authority() == RawColorSidecarAuthority::kRawAuthoritative);
  sidecar.Invalidate();
  complete_words.pop_back();
  CHECK_FALSE(sidecar.ReplaceRawWords(complete_words));
  CHECK_FALSE(sidecar.can_export_canonical());

  REQUIRE(sidecar.RestoreCanonical(canonical, layout));
  CHECK(sidecar.authority() == RawColorSidecarAuthority::kRawAuthoritative);
  REQUIRE(sidecar.RecordNativeSynchronizedFromRaw());
  CHECK(sidecar.authority() == RawColorSidecarAuthority::kCoherent);
  REQUIRE(sidecar.WriteRawSample(0, 0, 0, {0x12345678u, 0}));
  CHECK(sidecar.authority() == RawColorSidecarAuthority::kRawAuthoritative);

  sidecar.RecordNativeWrite();
  CHECK(sidecar.authority() == RawColorSidecarAuthority::kNativeAuthoritative);
  CHECK_FALSE(sidecar.raw_is_authoritative());
  CHECK(sidecar.raw_words().empty());
  std::array<uint32_t, 2> stale;
  CHECK_FALSE(sidecar.ReadRawSample(0, 0, 0, stale));
  CHECK_FALSE(sidecar.ExportCanonical(exported, layout));
  CHECK(exported == untouched);
  CHECK_FALSE(sidecar.RecordNativeSynchronizedFromRaw());

  sidecar.Invalidate();
  CHECK(sidecar.authority() == RawColorSidecarAuthority::kUninitialized);
  CHECK_FALSE(sidecar.can_export_canonical());
}

}  // namespace
}  // namespace rex::graphics::metal
