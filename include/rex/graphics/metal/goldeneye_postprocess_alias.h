/**
 ******************************************************************************
 * ReXGlue - Xbox 360 recompilation runtime                                  *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                       *
 *                                                                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rex::graphics::metal {

inline constexpr uint64_t kGoldenEyePostprocessPixelShaderHash = 0x5D448681EF02A235ull;
inline constexpr uint64_t kGoldenEyePostprocessVertexShaderHash = 0xBC83CA8D00933874ull;
inline constexpr uint32_t kGoldenEyePostprocessColorBase = 0x1EBFE000u;
inline constexpr uint32_t kGoldenEyePostprocessDepthBase = 0x1EBFD000u;

enum class GoldenEyePostprocessConsumer : uint8_t {
  kNone,
  kColorRgba,
  kDepthRgba,
  kDepth24Stencil8,
};

inline constexpr std::array<uint32_t, 6> kGoldenEyePostprocessColorRgbaFetch = {
    0x80024802u, 0x1EBFE086u, 0x03FFDFFFu, 0x00800C14u, 0x00000000u, 0x00000200u,
};
inline constexpr std::array<uint32_t, 6> kGoldenEyePostprocessDepthRgbaFetch = {
    0x80024802u, 0x1EBFD086u, 0x03FFDFFFu, 0x00A80C14u, 0x00000003u, 0x00000200u,
};
inline constexpr std::array<uint32_t, 6> kGoldenEyePostprocessDepth24Stencil8Fetch = {
    0x80024802u, 0x1EBFD096u, 0x03FFDFFFu, 0x00801690u, 0x00000000u, 0x00000200u,
};

constexpr GoldenEyePostprocessConsumer ClassifyGoldenEyePostprocessConsumer(
    uint64_t pixel_shader_hash, const std::array<uint32_t, 6>& fetch_words) {
  if (pixel_shader_hash != kGoldenEyePostprocessPixelShaderHash) {
    return GoldenEyePostprocessConsumer::kNone;
  }
  if (fetch_words == kGoldenEyePostprocessColorRgbaFetch) {
    return GoldenEyePostprocessConsumer::kColorRgba;
  }
  if (fetch_words == kGoldenEyePostprocessDepthRgbaFetch) {
    return GoldenEyePostprocessConsumer::kDepthRgba;
  }
  if (fetch_words == kGoldenEyePostprocessDepth24Stencil8Fetch) {
    return GoldenEyePostprocessConsumer::kDepth24Stencil8;
  }
  return GoldenEyePostprocessConsumer::kNone;
}

constexpr GoldenEyePostprocessConsumer ClassifyGoldenEyePostprocessConsumer(
    uint64_t pixel_shader_hash, const uint32_t (&fetch_words)[6]) {
  return ClassifyGoldenEyePostprocessConsumer(
      pixel_shader_hash, std::array<uint32_t, 6>{fetch_words[0], fetch_words[1], fetch_words[2],
                                                 fetch_words[3], fetch_words[4], fetch_words[5]});
}

enum class GoldenEyePostprocessProducerRole : uint8_t {
  kNone,
  kColor,
  kDepth,
};

enum class GoldenEyePostprocessBand : uint8_t {
  kNone,
  kTop,
  kMiddle,
  kBottom,
};

constexpr uint16_t GoldenEyePostprocessBandY(GoldenEyePostprocessBand band) {
  switch (band) {
    case GoldenEyePostprocessBand::kTop:
      return 0;
    case GoldenEyePostprocessBand::kMiddle:
      return 256;
    case GoldenEyePostprocessBand::kBottom:
      return 512;
    default:
      return 0;
  }
}

constexpr uint16_t GoldenEyePostprocessBandHeight(GoldenEyePostprocessBand band) {
  return band == GoldenEyePostprocessBand::kTop || band == GoldenEyePostprocessBand::kMiddle ? 256
         : band == GoldenEyePostprocessBand::kBottom                                         ? 208
                                                                                             : 0;
}

constexpr GoldenEyePostprocessBand ClassifyGoldenEyePostprocessBand(
    uint64_t bin_select, uint64_t bin_mask, int32_t window_offset_x, int32_t window_offset_y,
    uint32_t scissor_tl_x, uint32_t scissor_tl_y, uint32_t scissor_br_x, uint32_t scissor_br_y,
    bool window_offset_disable) {
  if (window_offset_disable || window_offset_x || scissor_tl_x || scissor_br_x != 1280) {
    return GoldenEyePostprocessBand::kNone;
  }
  if (bin_select == UINT64_C(0x80000003) && bin_mask == UINT64_C(0xFFFFFFFF) && !window_offset_y &&
      !scissor_tl_y && scissor_br_y == 256) {
    return GoldenEyePostprocessBand::kTop;
  }
  if (bin_select == UINT64_C(0x0000000C) &&
      (bin_mask == UINT64_C(0xFFFFFFFF) || bin_mask == UINT64_C(0x8000003F)) &&
      window_offset_y == -256 && scissor_tl_y == 256 && scissor_br_y == 512) {
    return GoldenEyePostprocessBand::kMiddle;
  }
  if (bin_select == UINT64_C(0x00000030) &&
      (bin_mask == UINT64_C(0xFFFFFFFF) || bin_mask == UINT64_C(0x8000003F)) &&
      window_offset_y == -512 && scissor_tl_y == 512 && scissor_br_y == 720) {
    return GoldenEyePostprocessBand::kBottom;
  }
  return GoldenEyePostprocessBand::kNone;
}

constexpr bool IsGoldenEyePostprocessProducerTileMask(
    GoldenEyePostprocessBand band, uint64_t bin_mask) {
  return band != GoldenEyePostprocessBand::kNone &&
         bin_mask == UINT64_C(0xFFFFFFFF);
}

constexpr bool IsGoldenEyePostprocessConsumerTileMask(
    GoldenEyePostprocessBand band, uint64_t bin_mask) {
  switch (band) {
    case GoldenEyePostprocessBand::kTop:
      return bin_mask == UINT64_C(0xFFFFFFFF);
    case GoldenEyePostprocessBand::kMiddle:
    case GoldenEyePostprocessBand::kBottom:
      return bin_mask == UINT64_C(0x8000003F);
    default:
      return false;
  }
}

// These signatures are deliberately semantic rather than raw register words.
// Callers must select them only after validating the complete RB source,
// RB_COPY_CONTROL and RB_COPY_DEST_INFO state represented by each value.
enum class GoldenEyePostprocessSourceSignature : uint8_t {
  kNone,
  kColorTarget0Rgba8,
  kDepthTarget24Stencil8,
};

enum class GoldenEyePostprocessControlSignature : uint8_t {
  kNone,
  kColorTarget0ConvertSample0,
  kDepthRawSample0,
};

enum class GoldenEyePostprocessDestinationSignature : uint8_t {
  kNone,
  kColorRgba8UnsignedSwapped,
  kDepth24Stencil8,
};

// Compact, renderer-independent description of a resolve producer. raw_pitch
// and raw_height are the decoded values from RB_COPY_DEST_PITCH before any
// fallback dimensions are applied.
struct GoldenEyePostprocessProducer {
  GoldenEyePostprocessProducerRole role = GoldenEyePostprocessProducerRole::kNone;
  GoldenEyePostprocessBand band = GoldenEyePostprocessBand::kNone;
  GoldenEyePostprocessSourceSignature source_signature = GoldenEyePostprocessSourceSignature::kNone;
  GoldenEyePostprocessControlSignature control_signature =
      GoldenEyePostprocessControlSignature::kNone;
  GoldenEyePostprocessDestinationSignature destination_signature =
      GoldenEyePostprocessDestinationSignature::kNone;
  uint32_t base = 0;
  uint16_t source_x = 0;
  uint16_t source_y = 0;
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t raw_pitch = 0;
  uint16_t raw_height = 0;
  uint8_t msaa = 0;
  uint8_t sample = 0;
  uint8_t endian = 0;
};

constexpr bool IsGoldenEyePostprocessBandHeight(uint32_t height) {
  return height == 208 || height == 256;
}

constexpr GoldenEyePostprocessProducerRole ClassifyGoldenEyePostprocessProducer(
    const GoldenEyePostprocessProducer& producer) {
  if (producer.band == GoldenEyePostprocessBand::kNone ||
      producer.source_x != 0 || producer.source_y != 0 || producer.x != 0 ||
      producer.y != 0 ||
      producer.width != 1280 || producer.height != GoldenEyePostprocessBandHeight(producer.band) ||
      producer.raw_pitch != 0 || producer.raw_height != 8191 || producer.msaa != 2 ||
      producer.sample != 0 || producer.endian != 2) {
    return GoldenEyePostprocessProducerRole::kNone;
  }

  if (producer.role == GoldenEyePostprocessProducerRole::kColor &&
      producer.base == kGoldenEyePostprocessColorBase &&
      producer.source_signature == GoldenEyePostprocessSourceSignature::kColorTarget0Rgba8 &&
      producer.control_signature ==
          GoldenEyePostprocessControlSignature::kColorTarget0ConvertSample0 &&
      producer.destination_signature ==
          GoldenEyePostprocessDestinationSignature::kColorRgba8UnsignedSwapped) {
    return GoldenEyePostprocessProducerRole::kColor;
  }

  if (producer.role == GoldenEyePostprocessProducerRole::kDepth &&
      producer.base == kGoldenEyePostprocessDepthBase &&
      producer.source_signature == GoldenEyePostprocessSourceSignature::kDepthTarget24Stencil8 &&
      producer.control_signature == GoldenEyePostprocessControlSignature::kDepthRawSample0 &&
      producer.destination_signature ==
          GoldenEyePostprocessDestinationSignature::kDepth24Stencil8) {
    return GoldenEyePostprocessProducerRole::kDepth;
  }

  return GoldenEyePostprocessProducerRole::kNone;
}

class GoldenEyePostprocessGenerationTracker {
 public:
  bool Publish(const GoldenEyePostprocessProducer& producer) {
    GoldenEyePostprocessProducerRole role = ClassifyGoldenEyePostprocessProducer(producer);
    if (role == GoldenEyePostprocessProducerRole::kNone) {
      InvalidateCohort();
      return false;
    }

    // The top color resolve is the unambiguous start marker for a new
    // aggregate frame. Seeing it again abandons any partial or completed
    // cohort and starts a fresh generation.
    if (role == GoldenEyePostprocessProducerRole::kColor &&
        producer.band == GoldenEyePostprocessBand::kTop) {
      BeginCohort(producer.height);
      return true;
    }

    if (role != NextExpectedRole() || producer.band != NextExpectedBand()) {
      InvalidateCohort();
      return false;
    }

    size_t band_index = GetBandIndex(producer.band);
    if (band_index >= bands_.size()) {
      InvalidateCohort();
      return false;
    }
    BandState& band = bands_[band_index];
    if (role == GoldenEyePostprocessProducerRole::kColor) {
      if (band.color_valid || band.depth_valid) {
        InvalidateCohort();
        return false;
      }
      band.height = producer.height;
      band.color_generation = generation_;
      band.color_valid = true;
    } else {
      if (!band.color_valid || band.depth_valid || band.color_generation != generation_ ||
          producer.height != band.height) {
        InvalidateCohort();
        return false;
      }
      band.depth_generation = generation_;
      band.depth_valid = true;
    }

    AdvanceCohort();
    return true;
  }

  void InvalidateBase(uint32_t base) {
    if (base == kGoldenEyePostprocessColorBase || base == kGoldenEyePostprocessDepthBase) {
      InvalidateCohort();
    }
  }

  void Reset() { *this = {}; }

  bool has_complete_cohort() const {
    if (!cohort_complete_ || generation_ == 0) {
      return false;
    }
    for (const BandState& band : bands_) {
      if (!band.color_valid || !band.depth_valid || band.color_generation != generation_ ||
          band.depth_generation != generation_) {
        return false;
      }
    }
    return true;
  }

  GoldenEyePostprocessProducerRole next_expected_role() const { return NextExpectedRole(); }
  GoldenEyePostprocessBand next_expected_band() const { return NextExpectedBand(); }

  bool has_depth(GoldenEyePostprocessBand band) const {
    const BandState* state = GetBandState(band);
    return state && state->depth_valid;
  }
  bool has_color(GoldenEyePostprocessBand band) const {
    const BandState* state = GetBandState(band);
    return state && state->color_valid;
  }
  bool has_complete_pair(GoldenEyePostprocessBand band) const {
    const BandState* state = GetBandState(band);
    return has_complete_cohort() && state && state->depth_valid && state->color_valid &&
           state->depth_generation == generation_ && state->color_generation == generation_;
  }
  uint16_t height(GoldenEyePostprocessBand band) const {
    const BandState* state = GetBandState(band);
    return state ? state->height : 0;
  }
  uint64_t generation() const { return generation_; }
  uint64_t depth_generation(GoldenEyePostprocessBand band) const {
    const BandState* state = GetBandState(band);
    return state ? state->depth_generation : 0;
  }
  uint64_t color_generation(GoldenEyePostprocessBand band) const {
    const BandState* state = GetBandState(band);
    return state ? state->color_generation : 0;
  }

 private:
  enum class CohortStep : uint8_t {
    kTopColor,
    kTopDepth,
    kMiddleColor,
    kMiddleDepth,
    kBottomColor,
    kBottomDepth,
    kComplete,
  };

  struct BandState {
    uint64_t depth_generation = 0;
    uint64_t color_generation = 0;
    uint16_t height = 0;
    bool depth_valid = false;
    bool color_valid = false;
  };

  static constexpr size_t GetBandIndex(GoldenEyePostprocessBand band) {
    switch (band) {
      case GoldenEyePostprocessBand::kTop:
        return 0;
      case GoldenEyePostprocessBand::kMiddle:
        return 1;
      case GoldenEyePostprocessBand::kBottom:
        return 2;
      default:
        return 3;
    }
  }

  const BandState* GetBandState(GoldenEyePostprocessBand band) const {
    size_t index = GetBandIndex(band);
    return index < bands_.size() ? &bands_[index] : nullptr;
  }

  GoldenEyePostprocessProducerRole NextExpectedRole() const {
    switch (cohort_step_) {
      case CohortStep::kTopColor:
      case CohortStep::kMiddleColor:
      case CohortStep::kBottomColor:
        return GoldenEyePostprocessProducerRole::kColor;
      case CohortStep::kTopDepth:
      case CohortStep::kMiddleDepth:
      case CohortStep::kBottomDepth:
        return GoldenEyePostprocessProducerRole::kDepth;
      default:
        return GoldenEyePostprocessProducerRole::kNone;
    }
  }

  GoldenEyePostprocessBand NextExpectedBand() const {
    switch (cohort_step_) {
      case CohortStep::kTopColor:
      case CohortStep::kTopDepth:
        return GoldenEyePostprocessBand::kTop;
      case CohortStep::kMiddleColor:
      case CohortStep::kMiddleDepth:
        return GoldenEyePostprocessBand::kMiddle;
      case CohortStep::kBottomColor:
      case CohortStep::kBottomDepth:
        return GoldenEyePostprocessBand::kBottom;
      default:
        return GoldenEyePostprocessBand::kNone;
    }
  }

  void BeginCohort(uint16_t height) {
    InvalidateCohort();
    // Generation zero is reserved for invalid state.
    ++generation_;
    if (generation_ == 0) {
      ++generation_;
    }
    BandState& band = bands_[GetBandIndex(GoldenEyePostprocessBand::kTop)];
    band.height = height;
    band.color_generation = generation_;
    band.color_valid = true;
    cohort_step_ = CohortStep::kTopDepth;
  }

  void AdvanceCohort() {
    switch (cohort_step_) {
      case CohortStep::kTopDepth:
        cohort_step_ = CohortStep::kMiddleColor;
        break;
      case CohortStep::kMiddleColor:
        cohort_step_ = CohortStep::kMiddleDepth;
        break;
      case CohortStep::kMiddleDepth:
        cohort_step_ = CohortStep::kBottomColor;
        break;
      case CohortStep::kBottomColor:
        cohort_step_ = CohortStep::kBottomDepth;
        break;
      case CohortStep::kBottomDepth:
        cohort_step_ = CohortStep::kComplete;
        cohort_complete_ = true;
        break;
      default:
        InvalidateCohort();
        break;
    }
  }

  void InvalidateCohort() {
    bands_ = {};
    cohort_step_ = CohortStep::kTopColor;
    cohort_complete_ = false;
  }

  uint64_t generation_ = 0;
  std::array<BandState, 3> bands_ = {};
  CohortStep cohort_step_ = CohortStep::kTopColor;
  bool cohort_complete_ = false;
};

}  // namespace rex::graphics::metal
