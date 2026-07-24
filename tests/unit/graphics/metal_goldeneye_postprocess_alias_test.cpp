/**
 ******************************************************************************
 * ReXGlue - Xbox 360 recompilation runtime                                  *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                       *
 *                                                                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <cstddef>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/metal/goldeneye_postprocess_alias.h>

namespace {

using namespace rex::graphics::metal;

constexpr GoldenEyePostprocessProducer MakeProducer(
    GoldenEyePostprocessProducerRole role,
    GoldenEyePostprocessBand band = GoldenEyePostprocessBand::kTop) {
  GoldenEyePostprocessProducer producer;
  producer.role = role;
  producer.band = band;
  producer.base = role == GoldenEyePostprocessProducerRole::kColor ? kGoldenEyePostprocessColorBase
                                                                   : kGoldenEyePostprocessDepthBase;
  producer.source_signature = role == GoldenEyePostprocessProducerRole::kColor
                                  ? GoldenEyePostprocessSourceSignature::kColorTarget0Rgba8
                                  : GoldenEyePostprocessSourceSignature::kDepthTarget24Stencil8;
  producer.control_signature =
      role == GoldenEyePostprocessProducerRole::kColor
          ? GoldenEyePostprocessControlSignature::kColorTarget0ConvertSample0
          : GoldenEyePostprocessControlSignature::kDepthRawSample0;
  producer.destination_signature =
      role == GoldenEyePostprocessProducerRole::kColor
          ? GoldenEyePostprocessDestinationSignature::kColorRgba8UnsignedSwapped
          : GoldenEyePostprocessDestinationSignature::kDepth24Stencil8;
  producer.width = 1280;
  producer.height = GoldenEyePostprocessBandHeight(band);
  producer.raw_height = 8191;
  producer.msaa = 2;
  producer.endian = 2;
  return producer;
}

constexpr GoldenEyePostprocessProducer MakeColorProducer(
    GoldenEyePostprocessBand band = GoldenEyePostprocessBand::kTop) {
  return MakeProducer(GoldenEyePostprocessProducerRole::kColor, band);
}

constexpr GoldenEyePostprocessProducer MakeDepthProducer(
    GoldenEyePostprocessBand band = GoldenEyePostprocessBand::kTop) {
  return MakeProducer(GoldenEyePostprocessProducerRole::kDepth, band);
}

constexpr std::array<GoldenEyePostprocessProducer, 6> MakeCompleteCohort() {
  return {
      MakeColorProducer(GoldenEyePostprocessBand::kTop),
      MakeDepthProducer(GoldenEyePostprocessBand::kTop),
      MakeColorProducer(GoldenEyePostprocessBand::kMiddle),
      MakeDepthProducer(GoldenEyePostprocessBand::kMiddle),
      MakeColorProducer(GoldenEyePostprocessBand::kBottom),
      MakeDepthProducer(GoldenEyePostprocessBand::kBottom),
  };
}

}  // namespace

TEST_CASE("GoldenEye restore consumers require exact shader and fetch words",
          "[graphics][metal][goldeneye][postprocess]") {
  CHECK(ClassifyGoldenEyePostprocessConsumer(kGoldenEyePostprocessPixelShaderHash,
                                             kGoldenEyePostprocessColorRgbaFetch) ==
        GoldenEyePostprocessConsumer::kColorRgba);
  CHECK(ClassifyGoldenEyePostprocessConsumer(kGoldenEyePostprocessPixelShaderHash,
                                             kGoldenEyePostprocessDepthRgbaFetch) ==
        GoldenEyePostprocessConsumer::kDepthRgba);
  CHECK(ClassifyGoldenEyePostprocessConsumer(kGoldenEyePostprocessPixelShaderHash,
                                             kGoldenEyePostprocessDepth24Stencil8Fetch) ==
        GoldenEyePostprocessConsumer::kDepth24Stencil8);

  CHECK(ClassifyGoldenEyePostprocessConsumer(kGoldenEyePostprocessPixelShaderHash ^ 1,
                                             kGoldenEyePostprocessColorRgbaFetch) ==
        GoldenEyePostprocessConsumer::kNone);

  const std::array<std::array<uint32_t, 6>, 3> exact_fetches = {
      kGoldenEyePostprocessColorRgbaFetch,
      kGoldenEyePostprocessDepthRgbaFetch,
      kGoldenEyePostprocessDepth24Stencil8Fetch,
  };
  for (const auto& exact_fetch : exact_fetches) {
    for (size_t word_index = 0; word_index < exact_fetch.size(); ++word_index) {
      auto near_miss = exact_fetch;
      near_miss[word_index] ^= 1;
      CHECK(ClassifyGoldenEyePostprocessConsumer(kGoldenEyePostprocessPixelShaderHash, near_miss) ==
            GoldenEyePostprocessConsumer::kNone);
    }
  }
}

TEST_CASE("GoldenEye restore bands require exact raw predicated-tile state",
          "[graphics][metal][goldeneye][postprocess]") {
  CHECK(ClassifyGoldenEyePostprocessBand(0x80000003, 0xFFFFFFFF, 0, 0, 0, 0, 1280, 256, false) ==
        GoldenEyePostprocessBand::kTop);
  CHECK(ClassifyGoldenEyePostprocessBand(0xC, 0x8000003F, 0, -256, 0, 256, 1280, 512, false) ==
        GoldenEyePostprocessBand::kMiddle);
  CHECK(ClassifyGoldenEyePostprocessBand(0x30, 0x8000003F, 0, -512, 0, 512, 1280, 720, false) ==
        GoldenEyePostprocessBand::kBottom);
  // The producing resolves use an all-ones mask. The consuming draws retain
  // the same select/window/scissor identity but narrow the mask.
  CHECK(ClassifyGoldenEyePostprocessBand(0xC, 0xFFFFFFFF, 0, -256, 0, 256, 1280, 512, false) ==
        GoldenEyePostprocessBand::kMiddle);
  CHECK(ClassifyGoldenEyePostprocessBand(0x30, 0xFFFFFFFF, 0, -512, 0, 512, 1280, 720, false) ==
        GoldenEyePostprocessBand::kBottom);

  CHECK(ClassifyGoldenEyePostprocessBand(0xC, 0x8000003F, 0, 0, 0, 256, 1280, 512, false) ==
        GoldenEyePostprocessBand::kNone);
  CHECK(ClassifyGoldenEyePostprocessBand(0xC, 0x8000003F, 0, -256, 0, 0, 1280, 256, false) ==
        GoldenEyePostprocessBand::kNone);
  CHECK(ClassifyGoldenEyePostprocessBand(0xC, 0x8000003F, 0, -256, 0, 256, 1280, 512, true) ==
        GoldenEyePostprocessBand::kNone);
  CHECK(ClassifyGoldenEyePostprocessBand(0xC, 0x7FFFFFFF, 0, -256, 0, 256, 1280, 512, false) ==
        GoldenEyePostprocessBand::kNone);

  CHECK(IsGoldenEyePostprocessProducerTileMask(
      GoldenEyePostprocessBand::kTop, 0xFFFFFFFF));
  CHECK(IsGoldenEyePostprocessProducerTileMask(
      GoldenEyePostprocessBand::kMiddle, 0xFFFFFFFF));
  CHECK(IsGoldenEyePostprocessProducerTileMask(
      GoldenEyePostprocessBand::kBottom, 0xFFFFFFFF));
  CHECK_FALSE(IsGoldenEyePostprocessProducerTileMask(
      GoldenEyePostprocessBand::kMiddle, 0x8000003F));

  CHECK(IsGoldenEyePostprocessConsumerTileMask(
      GoldenEyePostprocessBand::kTop, 0xFFFFFFFF));
  CHECK(IsGoldenEyePostprocessConsumerTileMask(
      GoldenEyePostprocessBand::kMiddle, 0x8000003F));
  CHECK(IsGoldenEyePostprocessConsumerTileMask(
      GoldenEyePostprocessBand::kBottom, 0x8000003F));
  CHECK_FALSE(IsGoldenEyePostprocessConsumerTileMask(
      GoldenEyePostprocessBand::kMiddle, 0xFFFFFFFF));
  CHECK_FALSE(IsGoldenEyePostprocessConsumerTileMask(
      GoldenEyePostprocessBand::kNone, 0xFFFFFFFF));
}

TEST_CASE("GoldenEye restore producer classification fails closed",
          "[graphics][metal][goldeneye][postprocess]") {
  for (GoldenEyePostprocessBand band :
       {GoldenEyePostprocessBand::kTop, GoldenEyePostprocessBand::kMiddle,
        GoldenEyePostprocessBand::kBottom}) {
    CHECK(ClassifyGoldenEyePostprocessProducer(MakeColorProducer(band)) ==
          GoldenEyePostprocessProducerRole::kColor);
    CHECK(ClassifyGoldenEyePostprocessProducer(MakeDepthProducer(band)) ==
          GoldenEyePostprocessProducerRole::kDepth);
  }

  auto check_rejected = [](const GoldenEyePostprocessProducer& producer) {
    CHECK(ClassifyGoldenEyePostprocessProducer(producer) ==
          GoldenEyePostprocessProducerRole::kNone);
  };

  auto producer = MakeColorProducer();
  producer.band = GoldenEyePostprocessBand::kNone;
  check_rejected(producer);
  producer = MakeColorProducer();
  producer.role = GoldenEyePostprocessProducerRole::kDepth;
  check_rejected(producer);
  producer = MakeColorProducer();
  producer.source_signature = GoldenEyePostprocessSourceSignature::kDepthTarget24Stencil8;
  check_rejected(producer);
  producer = MakeColorProducer();
  producer.control_signature = GoldenEyePostprocessControlSignature::kDepthRawSample0;
  check_rejected(producer);
  producer = MakeColorProducer();
  producer.destination_signature = GoldenEyePostprocessDestinationSignature::kDepth24Stencil8;
  check_rejected(producer);
  producer = MakeColorProducer();
  ++producer.base;
  check_rejected(producer);
  producer = MakeColorProducer();
  ++producer.source_x;
  check_rejected(producer);
  producer = MakeColorProducer();
  ++producer.source_y;
  check_rejected(producer);
  producer = MakeColorProducer();
  ++producer.x;
  check_rejected(producer);
  producer = MakeColorProducer();
  ++producer.y;
  check_rejected(producer);
  producer = MakeColorProducer();
  --producer.width;
  check_rejected(producer);
  producer = MakeColorProducer();
  producer.height = 208;
  check_rejected(producer);
  producer = MakeColorProducer();
  ++producer.raw_pitch;
  check_rejected(producer);
  producer = MakeColorProducer();
  --producer.raw_height;
  check_rejected(producer);
  producer = MakeColorProducer();
  --producer.msaa;
  check_rejected(producer);
  producer = MakeColorProducer();
  ++producer.sample;
  check_rejected(producer);
  producer = MakeColorProducer();
  --producer.endian;
  check_rejected(producer);
}

TEST_CASE("GoldenEye restore completes only one ordered aggregate cohort",
          "[graphics][metal][goldeneye][postprocess]") {
  GoldenEyePostprocessGenerationTracker tracker;
  constexpr auto kTop = GoldenEyePostprocessBand::kTop;
  constexpr auto kMiddle = GoldenEyePostprocessBand::kMiddle;
  constexpr auto kBottom = GoldenEyePostprocessBand::kBottom;

  CHECK_FALSE(tracker.has_complete_cohort());
  CHECK(tracker.next_expected_role() == GoldenEyePostprocessProducerRole::kColor);
  CHECK(tracker.next_expected_band() == kTop);

  REQUIRE(tracker.Publish(MakeColorProducer(kTop)));
  CHECK(tracker.generation() == 1);
  CHECK(tracker.color_generation(kTop) == 1);
  CHECK(tracker.depth_generation(kTop) == 0);
  CHECK(tracker.height(kTop) == 256);
  CHECK(tracker.has_color(kTop));
  CHECK_FALSE(tracker.has_depth(kTop));
  CHECK_FALSE(tracker.has_complete_pair(kTop));
  CHECK(tracker.next_expected_role() == GoldenEyePostprocessProducerRole::kDepth);
  CHECK(tracker.next_expected_band() == kTop);

  REQUIRE(tracker.Publish(MakeDepthProducer(kTop)));
  CHECK(tracker.generation() == 1);
  CHECK(tracker.has_depth(kTop));
  CHECK_FALSE(tracker.has_complete_pair(kTop));
  CHECK(tracker.next_expected_role() == GoldenEyePostprocessProducerRole::kColor);
  CHECK(tracker.next_expected_band() == kMiddle);

  REQUIRE(tracker.Publish(MakeColorProducer(kMiddle)));
  CHECK(tracker.generation() == 1);
  CHECK(tracker.height(kMiddle) == 256);
  CHECK_FALSE(tracker.has_depth(kMiddle));
  CHECK_FALSE(tracker.has_complete_pair(kTop));
  REQUIRE(tracker.Publish(MakeDepthProducer(kMiddle)));
  CHECK(tracker.depth_generation(kMiddle) == 1);
  CHECK_FALSE(tracker.has_complete_pair(kMiddle));
  CHECK(tracker.next_expected_role() == GoldenEyePostprocessProducerRole::kColor);
  CHECK(tracker.next_expected_band() == kBottom);

  REQUIRE(tracker.Publish(MakeColorProducer(kBottom)));
  CHECK(tracker.generation() == 1);
  CHECK(tracker.height(kBottom) == 208);
  CHECK_FALSE(tracker.has_complete_cohort());
  CHECK_FALSE(tracker.has_complete_pair(kBottom));

  REQUIRE(tracker.Publish(MakeDepthProducer(kBottom)));
  CHECK(tracker.has_complete_cohort());
  CHECK(tracker.has_complete_pair(kBottom));
  CHECK(tracker.has_complete_pair(kTop));
  CHECK(tracker.has_complete_pair(kMiddle));
  CHECK(tracker.next_expected_role() == GoldenEyePostprocessProducerRole::kNone);
  CHECK(tracker.next_expected_band() == GoldenEyePostprocessBand::kNone);
}

TEST_CASE("GoldenEye restore rejects disorder and invalidates the whole cohort",
          "[graphics][metal][goldeneye][postprocess]") {
  GoldenEyePostprocessGenerationTracker tracker;
  constexpr auto kTop = GoldenEyePostprocessBand::kTop;
  constexpr auto kMiddle = GoldenEyePostprocessBand::kMiddle;
  constexpr auto kBottom = GoldenEyePostprocessBand::kBottom;

  // A cohort cannot begin with depth.
  CHECK_FALSE(tracker.Publish(MakeDepthProducer(kTop)));
  CHECK_FALSE(tracker.has_complete_cohort());
  CHECK(tracker.next_expected_band() == kTop);
  CHECK(tracker.next_expected_role() == GoldenEyePostprocessProducerRole::kColor);

  // Skipping the top depth resolve discards the staged top color resolve.
  REQUIRE(tracker.Publish(MakeColorProducer(kTop)));
  CHECK_FALSE(tracker.Publish(MakeColorProducer(kMiddle)));
  CHECK_FALSE(tracker.has_color(kTop));
  CHECK_FALSE(tracker.has_depth(kTop));
  CHECK_FALSE(tracker.has_complete_cohort());

  // Once invalidated, a later step cannot resume the abandoned cohort.
  CHECK_FALSE(tracker.Publish(MakeDepthProducer(kMiddle)));
  CHECK(tracker.next_expected_band() == kTop);
  REQUIRE(tracker.Publish(MakeColorProducer(kTop)));
  REQUIRE(tracker.Publish(MakeDepthProducer(kTop)));
  CHECK_FALSE(tracker.Publish(MakeColorProducer(kBottom)));
  CHECK_FALSE(tracker.has_depth(kTop));
  CHECK_FALSE(tracker.has_color(kTop));

  // A completed cohort also fails closed if a non-start step follows it.
  for (const auto& producer : MakeCompleteCohort()) {
    REQUIRE(tracker.Publish(producer));
  }
  REQUIRE(tracker.has_complete_cohort());
  CHECK_FALSE(tracker.Publish(MakeDepthProducer(kBottom)));
  CHECK_FALSE(tracker.has_complete_cohort());
  CHECK_FALSE(tracker.has_depth(kTop));
  CHECK_FALSE(tracker.has_color(kTop));
  CHECK(tracker.height(kMiddle) == 0);
}

TEST_CASE("GoldenEye top color always starts a fresh generation",
          "[graphics][metal][goldeneye][postprocess]") {
  GoldenEyePostprocessGenerationTracker tracker;
  constexpr auto kTop = GoldenEyePostprocessBand::kTop;

  REQUIRE(tracker.Publish(MakeColorProducer(kTop)));
  REQUIRE(tracker.Publish(MakeDepthProducer(kTop)));
  CHECK(tracker.generation() == 1);
  CHECK(tracker.has_depth(kTop));

  // Repeating the exact start marker abandons the partial generation and is
  // accepted as the start of the next one.
  REQUIRE(tracker.Publish(MakeColorProducer(kTop)));
  CHECK(tracker.generation() == 2);
  CHECK(tracker.has_color(kTop));
  CHECK_FALSE(tracker.has_depth(kTop));
  CHECK(tracker.color_generation(kTop) == 2);

  const auto cohort = MakeCompleteCohort();
  for (size_t index = 1; index < cohort.size(); ++index) {
    REQUIRE(tracker.Publish(cohort[index]));
  }
  CHECK(tracker.generation() == 2);
  CHECK(tracker.has_complete_cohort());
  CHECK(tracker.has_complete_pair(GoldenEyePostprocessBand::kMiddle));
  CHECK(tracker.has_complete_pair(GoldenEyePostprocessBand::kBottom));
}

TEST_CASE("GoldenEye invalid producer invalidates a staged or complete cohort",
          "[graphics][metal][goldeneye][postprocess]") {
  GoldenEyePostprocessGenerationTracker tracker;
  constexpr auto kTop = GoldenEyePostprocessBand::kTop;

  REQUIRE(tracker.Publish(MakeColorProducer(kTop)));
  auto near_miss = MakeDepthProducer(kTop);
  --near_miss.width;
  CHECK_FALSE(tracker.Publish(near_miss));
  CHECK_FALSE(tracker.has_color(kTop));
  CHECK(tracker.next_expected_role() == GoldenEyePostprocessProducerRole::kColor);

  for (const auto& producer : MakeCompleteCohort()) {
    REQUIRE(tracker.Publish(producer));
  }
  REQUIRE(tracker.has_complete_cohort());

  near_miss = MakeColorProducer(kTop);
  ++near_miss.raw_pitch;
  CHECK_FALSE(tracker.Publish(near_miss));
  CHECK_FALSE(tracker.has_complete_cohort());
  CHECK_FALSE(tracker.has_complete_pair(kTop));
}

TEST_CASE("GoldenEye restore invalidation preserves exact base identity",
          "[graphics][metal][goldeneye][postprocess]") {
  GoldenEyePostprocessGenerationTracker tracker;
  constexpr auto kTop = GoldenEyePostprocessBand::kTop;
  constexpr auto kMiddle = GoldenEyePostprocessBand::kMiddle;
  constexpr auto kBottom = GoldenEyePostprocessBand::kBottom;

  for (const auto& producer : MakeCompleteCohort()) {
    REQUIRE(tracker.Publish(producer));
  }
  REQUIRE(tracker.has_complete_cohort());
  CHECK(tracker.generation() == 1);

  tracker.InvalidateBase(kGoldenEyePostprocessColorBase + 1);
  CHECK(tracker.has_complete_cohort());
  CHECK(tracker.has_complete_pair(kTop));
  CHECK(tracker.has_complete_pair(kMiddle));
  CHECK(tracker.has_complete_pair(kBottom));

  // Either exact alias base invalidates the entire aggregate cohort.
  tracker.InvalidateBase(kGoldenEyePostprocessColorBase);
  CHECK_FALSE(tracker.has_complete_cohort());
  for (auto band : {kTop, kMiddle, kBottom}) {
    CHECK_FALSE(tracker.has_color(band));
    CHECK_FALSE(tracker.has_depth(band));
    CHECK_FALSE(tracker.has_complete_pair(band));
    CHECK(tracker.height(band) == 0);
  }
  CHECK(tracker.generation() == 1);

  for (const auto& producer : MakeCompleteCohort()) {
    REQUIRE(tracker.Publish(producer));
  }
  REQUIRE(tracker.has_complete_cohort());
  CHECK(tracker.generation() == 2);

  tracker.InvalidateBase(kGoldenEyePostprocessDepthBase);
  CHECK_FALSE(tracker.has_complete_cohort());
  CHECK_FALSE(tracker.has_color(kTop));
  CHECK_FALSE(tracker.has_depth(kBottom));
  CHECK(tracker.next_expected_role() == GoldenEyePostprocessProducerRole::kColor);
  CHECK(tracker.next_expected_band() == kTop);

  tracker.Reset();
  CHECK(tracker.generation() == 0);
  CHECK_FALSE(tracker.has_complete_cohort());
  CHECK(tracker.height(kTop) == 0);
  CHECK(tracker.next_expected_role() == GoldenEyePostprocessProducerRole::kColor);
  CHECK(tracker.next_expected_band() == kTop);
}
