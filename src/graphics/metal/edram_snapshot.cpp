#include <rex/graphics/metal/edram_snapshot.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include <rex/math.h>

namespace rex::graphics::metal {
namespace {

constexpr size_t kCanonicalEdramTileBytes =
    size_t(xenos::kEdramTileWidthSamples) * xenos::kEdramTileHeightSamples * sizeof(uint32_t);

bool IsCanonicalEdramSpan(std::span<const uint8_t> edram) {
  return edram.size() == xenos::kEdramSizeBytes;
}

uint32_t RoundUNorm(float value, uint32_t maximum) {
  if (std::isnan(value)) {
    value = 0.0f;
  }
  value = std::clamp(value, 0.0f, 1.0f);
  return uint32_t(std::floor(value * float(maximum) + 0.5f));
}

uint32_t Float32To7e3(float value) {
  if (std::isnan(value)) {
    value = 0.0f;
  }
  value = std::clamp(value, 0.0f, 31.875f);
  uint32_t best = 0;
  double best_distance = std::numeric_limits<double>::infinity();
  for (uint32_t candidate = 0; candidate < 1024; ++candidate) {
    double decoded = double(xenos::Float7e3To32(candidate));
    double distance = std::abs(decoded - double(value));
    if (distance < best_distance ||
        (distance == best_distance && !(candidate & 1) && (best & 1))) {
      best = candidate;
      best_distance = distance;
    }
  }
  return best;
}

int16_t Float32ToFixed16(float value) {
  if (std::isnan(value)) {
    value = 0.0f;
  }
  value = std::clamp(value, -32.0f, 32.0f);
  if (value <= -32.0f) {
    return INT16_MIN;
  }
  if (value >= 32.0f) {
    return INT16_MAX;
  }
  float scaled = value * (32767.0f / 32.0f);
  scaled += scaled < 0.0f ? -0.5f : 0.5f;
  return int16_t(int32_t(scaled));
}

}  // namespace

bool IsCanonicalEdramColorFormatSupportedByMetal(xenos::ColorRenderTargetFormat format) {
  format = xenos::GetStorageColorFormat(format);
  // Gamma storage is not exact through an 8-bit linear BGRA target: decoding
  // and re-encoding can collapse dark raw codes. It remains available to the
  // CPU conversion helpers, but cannot be advertised for canonical replay.
  return format == xenos::ColorRenderTargetFormat::k_8_8_8_8;
}

bool IsCanonicalEdramDepthFormatSupportedByMetal(xenos::DepthRenderTargetFormat format) {
  return format == xenos::DepthRenderTargetFormat::kD24S8 ||
         format == xenos::DepthRenderTargetFormat::kD24FS8;
}

bool IsCanonicalEdramMsaaSupportedByMetal(xenos::MsaaSamples msaa_samples) {
  return msaa_samples == xenos::MsaaSamples::k1X ||
         msaa_samples == xenos::MsaaSamples::k2X ||
         msaa_samples == xenos::MsaaSamples::k4X;
}

size_t GetCanonicalEdramDwordIndex(const CanonicalEdramSurfaceLayout& layout, uint32_t x,
                                   uint32_t y, uint32_t sample, uint32_t dword) {
  const uint32_t sample_x_log2 =
      uint32_t(layout.msaa_samples >= xenos::MsaaSamples::k4X);
  const uint32_t sample_y_log2 =
      uint32_t(layout.msaa_samples >= xenos::MsaaSamples::k2X);
  const uint32_t sample_count = GetCanonicalEdramSampleCount(layout.msaa_samples);
  const uint32_t words_per_sample = layout.is_64bpp ? 2 : 1;
  if (!layout.pitch_tiles || sample >= sample_count || dword >= words_per_sample ||
      (layout.is_depth && layout.is_64bpp)) {
    return SIZE_MAX;
  }

  const uint32_t sample_x =
      (x << sample_x_log2) +
      (sample_x_log2 ? (sample & ((uint32_t(1) << sample_x_log2) - 1)) : 0);
  const uint32_t sample_y = (y << sample_y_log2) + (sample >> sample_x_log2);
  const uint32_t word_x = sample_x * words_per_sample + std::min(dword, words_per_sample - 1);
  const uint32_t tile_x = word_x / xenos::kEdramTileWidthSamples;
  const uint32_t tile_y = sample_y / xenos::kEdramTileHeightSamples;
  const uint32_t tile =
      (layout.base_tiles + tile_y * layout.pitch_tiles + tile_x) &
      (xenos::kEdramTileCount - 1);
  uint32_t tile_word_x = word_x % xenos::kEdramTileWidthSamples;
  if (layout.is_depth && !layout.is_64bpp) {
    tile_word_x =
        (tile_word_x + xenos::kEdramTileWidthSamples / 2) % xenos::kEdramTileWidthSamples;
  }
  const uint32_t tile_word_y = sample_y % xenos::kEdramTileHeightSamples;
  return size_t(tile) * xenos::kEdramTileWidthSamples * xenos::kEdramTileHeightSamples +
         size_t(tile_word_y) * xenos::kEdramTileWidthSamples + tile_word_x;
}

bool ReadCanonicalEdramSample(std::span<const uint8_t> edram,
                              const CanonicalEdramSurfaceLayout& layout, uint32_t x, uint32_t y,
                              uint32_t sample, std::array<uint32_t, 2>& words_out) {
  if (!IsCanonicalEdramSpan(edram) || !layout.pitch_tiles) {
    return false;
  }
  const uint32_t sample_count = GetCanonicalEdramSampleCount(layout.msaa_samples);
  if (sample >= sample_count || (layout.is_depth && layout.is_64bpp)) {
    return false;
  }
  words_out = {};
  const uint32_t word_count = layout.is_64bpp ? 2 : 1;
  for (uint32_t word = 0; word < word_count; ++word) {
    const size_t dword_offset = GetCanonicalEdramDwordIndex(layout, x, y, sample, word);
    if (dword_offset == SIZE_MAX) {
      return false;
    }
    const size_t byte_offset = dword_offset * 4;
    std::memcpy(&words_out[word], edram.data() + byte_offset, sizeof(uint32_t));
  }
  return true;
}

bool WriteCanonicalEdramSample(std::span<uint8_t> edram,
                               const CanonicalEdramSurfaceLayout& layout, uint32_t x, uint32_t y,
                               uint32_t sample, const std::array<uint32_t, 2>& words) {
  if (!IsCanonicalEdramSpan(edram) || !layout.pitch_tiles) {
    return false;
  }
  const uint32_t sample_count = GetCanonicalEdramSampleCount(layout.msaa_samples);
  if (sample >= sample_count || (layout.is_depth && layout.is_64bpp)) {
    return false;
  }
  const uint32_t word_count = layout.is_64bpp ? 2 : 1;
  for (uint32_t word = 0; word < word_count; ++word) {
    const size_t dword_offset = GetCanonicalEdramDwordIndex(layout, x, y, sample, word);
    if (dword_offset == SIZE_MAX) {
      return false;
    }
    const size_t byte_offset = dword_offset * 4;
    std::memcpy(edram.data() + byte_offset, &words[word], sizeof(uint32_t));
  }
  return true;
}

bool PackCanonicalEdramColor(const std::array<float, 4>& rgba,
                             xenos::ColorRenderTargetFormat format,
                             std::array<uint32_t, 2>& words_out) {
  words_out = {};
  format = xenos::GetStorageColorFormat(format);
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
      std::array<float, 4> packed_rgba = rgba;
      if (format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        for (uint32_t i = 0; i < 3; ++i) {
          packed_rgba[i] = xenos::LinearToPWLGamma(packed_rgba[i]);
        }
      }
      for (uint32_t i = 0; i < 4; ++i) {
        words_out[0] |= RoundUNorm(packed_rgba[i], 255) << (i * 8);
      }
      return true;
    }
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      words_out[0] = RoundUNorm(rgba[0], 1023) | (RoundUNorm(rgba[1], 1023) << 10) |
                     (RoundUNorm(rgba[2], 1023) << 20) | (RoundUNorm(rgba[3], 3) << 30);
      return true;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      words_out[0] = Float32To7e3(rgba[0]) | (Float32To7e3(rgba[1]) << 10) |
                     (Float32To7e3(rgba[2]) << 20) | (RoundUNorm(rgba[3], 3) << 30);
      return true;
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16: {
      const uint32_t component_count =
          format == xenos::ColorRenderTargetFormat::k_16_16 ? 2 : 4;
      for (uint32_t component = 0; component < component_count; ++component) {
        uint32_t packed = uint16_t(Float32ToFixed16(rgba[component]));
        words_out[component >> 1] |= packed << (16 * (component & 1));
      }
      return true;
    }
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
      const uint32_t component_count =
          format == xenos::ColorRenderTargetFormat::k_16_16_FLOAT ? 2 : 4;
      for (uint32_t component = 0; component < component_count; ++component) {
        float value = std::isnan(rgba[component]) ? 0.0f : rgba[component];
        value = std::clamp(value, -65504.0f, 65504.0f);
        uint32_t packed = rex::float_to_xenos_half(value, false, true);
        words_out[component >> 1] |= packed << (16 * (component & 1));
      }
      return true;
    }
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      words_out[0] = std::bit_cast<uint32_t>(rgba[0]);
      return true;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      words_out[0] = std::bit_cast<uint32_t>(rgba[0]);
      words_out[1] = std::bit_cast<uint32_t>(rgba[1]);
      return true;
    default:
      return false;
  }
}

bool UnpackCanonicalEdramColor(const std::array<uint32_t, 2>& words,
                               xenos::ColorRenderTargetFormat format,
                               std::array<float, 4>& rgba_out) {
  rgba_out = {0.0f, 0.0f, 0.0f, 1.0f};
  format = xenos::GetStorageColorFormat(format);
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      for (uint32_t i = 0; i < 4; ++i) {
        rgba_out[i] = float((words[0] >> (i * 8)) & 0xFF) * (1.0f / 255.0f);
      }
      if (format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        for (uint32_t i = 0; i < 3; ++i) {
          rgba_out[i] = xenos::PWLGammaToLinear(rgba_out[i]);
        }
      }
      return true;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      rgba_out[0] = float(words[0] & 0x3FF) * (1.0f / 1023.0f);
      rgba_out[1] = float((words[0] >> 10) & 0x3FF) * (1.0f / 1023.0f);
      rgba_out[2] = float((words[0] >> 20) & 0x3FF) * (1.0f / 1023.0f);
      rgba_out[3] = float(words[0] >> 30) * (1.0f / 3.0f);
      return true;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      rgba_out[0] = xenos::Float7e3To32(words[0] & 0x3FF);
      rgba_out[1] = xenos::Float7e3To32((words[0] >> 10) & 0x3FF);
      rgba_out[2] = xenos::Float7e3To32((words[0] >> 20) & 0x3FF);
      rgba_out[3] = float(words[0] >> 30) * (1.0f / 3.0f);
      return true;
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16: {
      const uint32_t component_count =
          format == xenos::ColorRenderTargetFormat::k_16_16 ? 2 : 4;
      for (uint32_t component = 0; component < component_count; ++component) {
        int16_t packed = int16_t(words[component >> 1] >> (16 * (component & 1)));
        rgba_out[component] =
            packed == INT16_MIN ? -32.0f : float(packed) * (32.0f / 32767.0f);
      }
      return true;
    }
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
      const uint32_t component_count =
          format == xenos::ColorRenderTargetFormat::k_16_16_FLOAT ? 2 : 4;
      for (uint32_t component = 0; component < component_count; ++component) {
        uint16_t packed = uint16_t(words[component >> 1] >> (16 * (component & 1)));
        rgba_out[component] = rex::xenos_half_to_float(packed);
      }
      return true;
    }
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      rgba_out[0] = std::bit_cast<float>(words[0]);
      return true;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      rgba_out[0] = std::bit_cast<float>(words[0]);
      rgba_out[1] = std::bit_cast<float>(words[1]);
      return true;
    default:
      return false;
  }
}

void CanonicalEdramTileOwnership::Reset() {
  owners_.fill({});
  sequence_ = 0;
}

void CanonicalEdramAuthorityState::Reset() {
  has_snapshot_ = false;
  target_hydration_enabled_ = false;
}

void CanonicalEdramAuthorityState::RecordCapture() {
  has_snapshot_ = true;
}

void CanonicalEdramAuthorityState::RecordRestore() {
  has_snapshot_ = true;
  target_hydration_enabled_ = true;
}

uint64_t CanonicalEdramTileOwnership::MarkSurface(const CanonicalEdramSurfaceLayout& layout,
                                                  uint32_t width, uint32_t height,
                                                  CanonicalEdramOwnerKind kind,
                                                  uint64_t target_key) {
  if (!layout.pitch_tiles || !width || !height || kind == CanonicalEdramOwnerKind::kRawSnapshot ||
      !target_key) {
    return sequence_;
  }
  ++sequence_;
  const uint32_t sample_width =
      width << uint32_t(layout.msaa_samples >= xenos::MsaaSamples::k4X);
  const uint32_t sample_height =
      height << uint32_t(layout.msaa_samples >= xenos::MsaaSamples::k2X);
  const uint32_t word_width = sample_width * (layout.is_64bpp ? 2 : 1);
  const uint32_t tile_columns =
      (word_width + xenos::kEdramTileWidthSamples - 1) / xenos::kEdramTileWidthSamples;
  const uint32_t tile_rows =
      (sample_height + xenos::kEdramTileHeightSamples - 1) / xenos::kEdramTileHeightSamples;
  const CanonicalEdramTileOwner new_owner = {kind, target_key, sequence_};
  for (uint32_t tile_y = 0; tile_y < tile_rows; ++tile_y) {
    for (uint32_t tile_x = 0; tile_x < tile_columns; ++tile_x) {
      const uint32_t tile =
          (layout.base_tiles + tile_y * layout.pitch_tiles + tile_x) &
          (xenos::kEdramTileCount - 1);
      owners_[tile] = new_owner;
    }
  }
  return sequence_;
}

bool CanonicalEdramTileOwnership::SurfaceNeedsHydration(
    const CanonicalEdramSurfaceLayout& layout, uint32_t width, uint32_t height,
    CanonicalEdramOwnerKind kind, uint64_t target_key, uint64_t hydrated_sequence) const {
  if (!layout.pitch_tiles || !width || !height || !target_key) {
    return true;
  }
  const uint32_t sample_width =
      width << uint32_t(layout.msaa_samples >= xenos::MsaaSamples::k4X);
  const uint32_t sample_height =
      height << uint32_t(layout.msaa_samples >= xenos::MsaaSamples::k2X);
  const uint32_t word_width = sample_width * (layout.is_64bpp ? 2 : 1);
  const uint32_t tile_columns =
      (word_width + xenos::kEdramTileWidthSamples - 1) / xenos::kEdramTileWidthSamples;
  const uint32_t tile_rows =
      (sample_height + xenos::kEdramTileHeightSamples - 1) / xenos::kEdramTileHeightSamples;
  for (uint32_t tile_y = 0; tile_y < tile_rows; ++tile_y) {
    for (uint32_t tile_x = 0; tile_x < tile_columns; ++tile_x) {
      const uint32_t tile =
          (layout.base_tiles + tile_y * layout.pitch_tiles + tile_x) &
          (xenos::kEdramTileCount - 1);
      const CanonicalEdramTileOwner& tile_owner = owners_[tile];
      if (tile_owner.sequence > hydrated_sequence &&
          (tile_owner.kind != kind || tile_owner.target_key != target_key)) {
        return true;
      }
    }
  }
  return false;
}

bool CanonicalEdramTileOwnership::MergeOwnedTiles(
    std::span<uint8_t> destination, std::span<const uint8_t> source,
    CanonicalEdramOwnerKind kind, uint64_t target_key) const {
  if (!IsCanonicalEdramSpan(destination) || !IsCanonicalEdramSpan(source) ||
      kind == CanonicalEdramOwnerKind::kRawSnapshot || !target_key) {
    return false;
  }
  for (uint32_t tile = 0; tile < xenos::kEdramTileCount; ++tile) {
    const CanonicalEdramTileOwner& tile_owner = owners_[tile];
    if (tile_owner.kind != kind || tile_owner.target_key != target_key) {
      continue;
    }
    const size_t offset = size_t(tile) * kCanonicalEdramTileBytes;
    std::memcpy(destination.data() + offset, source.data() + offset, kCanonicalEdramTileBytes);
  }
  return true;
}

}  // namespace rex::graphics::metal
