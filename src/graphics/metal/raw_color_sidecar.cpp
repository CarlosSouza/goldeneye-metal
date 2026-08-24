#include <rex/graphics/metal/raw_color_sidecar.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace rex::graphics::metal {

bool GetRawColorSidecarStorageLayout(xenos::ColorRenderTargetFormat guest_format, uint32_t width,
                                     uint32_t height, xenos::MsaaSamples msaa_samples,
                                     RawColorSidecarStorageLayout& layout_out) {
  layout_out = {};
  const RawColorSidecarFormatCapabilities capabilities =
      GetRawColorSidecarFormatCapabilities(guest_format);
  if (!capabilities.lossless_canonical_storage || !width || !height ||
      width > (uint32_t(1) << xenos::kEdramPitchPixelsBits) ||
      (msaa_samples != xenos::MsaaSamples::k1X && msaa_samples != xenos::MsaaSamples::k2X &&
       msaa_samples != xenos::MsaaSamples::k4X)) {
    return false;
  }

  const uint32_t sample_count = GetCanonicalEdramSampleCount(msaa_samples);
  constexpr size_t kSizeMax = std::numeric_limits<size_t>::max();
  if (size_t(width) > kSizeMax / size_t(height)) {
    return false;
  }
  size_t word_count = size_t(width) * size_t(height);
  if (word_count > kSizeMax / sample_count) {
    return false;
  }
  word_count *= sample_count;
  if (word_count > kSizeMax / capabilities.words_per_sample) {
    return false;
  }
  word_count *= capabilities.words_per_sample;
  if (word_count > xenos::kEdramSizeBytes / sizeof(uint32_t)) {
    return false;
  }

  layout_out.width = width;
  layout_out.height = height;
  layout_out.sample_count = sample_count;
  layout_out.words_per_sample = capabilities.words_per_sample;
  layout_out.word_count = word_count;
  return true;
}

size_t GetRawColorSidecarWordIndex(const RawColorSidecarStorageLayout& layout, uint32_t x,
                                   uint32_t y, uint32_t sample, uint32_t word) {
  if (!layout.width || !layout.height || !layout.sample_count || !layout.words_per_sample ||
      !layout.word_count || x >= layout.width || y >= layout.height ||
      sample >= layout.sample_count || word >= layout.words_per_sample) {
    return SIZE_MAX;
  }
  const size_t index =
      (((size_t(sample) * layout.height + y) * layout.width + x) * layout.words_per_sample) + word;
  return index < layout.word_count ? index : SIZE_MAX;
}

void RawColorSidecarAuthorityState::Reset() {
  state_ = RawColorSidecarAuthority::kUninitialized;
}

void RawColorSidecarAuthorityState::RecordCompleteRawWrite() {
  state_ = RawColorSidecarAuthority::kRawAuthoritative;
}

void RawColorSidecarAuthorityState::RecordNativeWrite() {
  state_ = RawColorSidecarAuthority::kNativeAuthoritative;
}

bool RawColorSidecarAuthorityState::RecordNativeSynchronizedFromRaw() {
  if (!raw_is_authoritative()) {
    return false;
  }
  state_ = RawColorSidecarAuthority::kCoherent;
  return true;
}

bool RawColorSidecar::Configure(xenos::ColorRenderTargetFormat guest_format, uint32_t width,
                                uint32_t height, xenos::MsaaSamples msaa_samples) noexcept {
  RawColorSidecarStorageLayout new_layout;
  if (!GetRawColorSidecarStorageLayout(guest_format, width, height, msaa_samples, new_layout)) {
    Reset();
    return false;
  }

  std::vector<uint32_t> new_words;
  try {
    new_words.resize(new_layout.word_count);
  } catch (const std::bad_alloc&) {
    Reset();
    return false;
  } catch (const std::length_error&) {
    Reset();
    return false;
  }
  key_ = {guest_format, msaa_samples, width, height};
  storage_layout_ = new_layout;
  words_.swap(new_words);
  authority_.Reset();
  return true;
}

void RawColorSidecar::Reset() {
  key_ = {};
  storage_layout_ = {};
  words_.clear();
  authority_.Reset();
}

void RawColorSidecar::Invalidate() {
  authority_.Reset();
}

void RawColorSidecar::RecordNativeWrite() {
  if (configured()) {
    authority_.RecordNativeWrite();
  }
}

bool RawColorSidecar::RecordNativeSynchronizedFromRaw() {
  return configured() && authority_.RecordNativeSynchronizedFromRaw();
}

bool RawColorSidecar::ReplaceRawWords(std::span<const uint32_t> words) {
  if (!configured() || words.size() != storage_layout_.word_count) {
    return false;
  }
  std::copy(words.begin(), words.end(), words_.begin());
  authority_.RecordCompleteRawWrite();
  return true;
}

bool RawColorSidecar::ReadRawSample(uint32_t x, uint32_t y, uint32_t sample,
                                    std::array<uint32_t, 2>& words_out) const {
  words_out = {};
  if (!raw_is_authoritative()) {
    return false;
  }
  for (uint32_t word = 0; word < storage_layout_.words_per_sample; ++word) {
    const size_t index = GetRawColorSidecarWordIndex(storage_layout_, x, y, sample, word);
    if (index == SIZE_MAX) {
      words_out = {};
      return false;
    }
    words_out[word] = words_[index];
  }
  return true;
}

bool RawColorSidecar::WriteRawSample(uint32_t x, uint32_t y, uint32_t sample,
                                     const std::array<uint32_t, 2>& words) {
  if (!raw_is_authoritative()) {
    return false;
  }
  for (uint32_t word = 0; word < storage_layout_.words_per_sample; ++word) {
    const size_t index = GetRawColorSidecarWordIndex(storage_layout_, x, y, sample, word);
    if (index == SIZE_MAX) {
      return false;
    }
    words_[index] = words[word];
  }
  authority_.RecordCompleteRawWrite();
  return true;
}

bool RawColorSidecar::IsCanonicalLayoutCompatible(
    const CanonicalEdramSurfaceLayout& canonical_layout) const {
  if (!configured() || !canonical_layout.pitch_tiles || canonical_layout.is_depth ||
      canonical_layout.base_tiles >= xenos::kEdramTileCount ||
      canonical_layout.pitch_tiles >= (uint32_t(1) << xenos::kEdramPitchTilesBits) ||
      canonical_layout.msaa_samples != key_.msaa_samples ||
      canonical_layout.is_64bpp != (storage_layout_.words_per_sample == 2)) {
    return false;
  }

  const uint32_t sample_x_log2 = uint32_t(key_.msaa_samples >= xenos::MsaaSamples::k4X);
  const uint32_t sample_y_log2 = uint32_t(key_.msaa_samples >= xenos::MsaaSamples::k2X);
  const uint64_t word_width =
      (uint64_t(storage_layout_.width) << sample_x_log2) * storage_layout_.words_per_sample;
  const uint64_t sample_height = uint64_t(storage_layout_.height) << sample_y_log2;
  const uint64_t tile_columns =
      (word_width + xenos::kEdramTileWidthSamples - 1) / xenos::kEdramTileWidthSamples;
  const uint64_t tile_rows =
      (sample_height + xenos::kEdramTileHeightSamples - 1) / xenos::kEdramTileHeightSamples;
  if (!tile_columns || !tile_rows || tile_columns > canonical_layout.pitch_tiles) {
    return false;
  }

  // Crossing physical tile 2047 is valid because EDRAM wraps, but visiting
  // more than one complete physical cycle would make two logical samples map
  // to the same canonical word.
  const uint64_t tile_span = (tile_rows - 1) * canonical_layout.pitch_tiles + tile_columns;
  return tile_span <= xenos::kEdramTileCount;
}

bool RawColorSidecar::RestoreCanonical(std::span<const uint8_t> canonical_edram,
                                       const CanonicalEdramSurfaceLayout& canonical_layout) {
  if (!IsCanonicalLayoutCompatible(canonical_layout) ||
      canonical_edram.size() != xenos::kEdramSizeBytes) {
    return false;
  }

  std::vector<uint32_t> restored_words;
  try {
    restored_words.resize(storage_layout_.word_count);
  } catch (const std::bad_alloc&) {
    return false;
  } catch (const std::length_error&) {
    return false;
  }
  for (uint32_t sample = 0; sample < storage_layout_.sample_count; ++sample) {
    for (uint32_t y = 0; y < storage_layout_.height; ++y) {
      for (uint32_t x = 0; x < storage_layout_.width; ++x) {
        std::array<uint32_t, 2> words;
        if (!ReadCanonicalEdramSample(canonical_edram, canonical_layout, x, y, sample, words)) {
          return false;
        }
        for (uint32_t word = 0; word < storage_layout_.words_per_sample; ++word) {
          const size_t index = GetRawColorSidecarWordIndex(storage_layout_, x, y, sample, word);
          if (index == SIZE_MAX) {
            return false;
          }
          restored_words[index] = words[word];
        }
      }
    }
  }

  words_.swap(restored_words);
  authority_.RecordCompleteRawWrite();
  return true;
}

bool RawColorSidecar::ExportCanonical(std::span<uint8_t> canonical_edram,
                                      const CanonicalEdramSurfaceLayout& canonical_layout) const {
  if (!can_export_canonical() || !IsCanonicalLayoutCompatible(canonical_layout) ||
      canonical_edram.size() != xenos::kEdramSizeBytes) {
    return false;
  }

  // Commit only after the full operation succeeds so every false return keeps
  // the caller's canonical image byte-for-byte unchanged.
  std::vector<uint8_t> exported;
  try {
    exported.assign(canonical_edram.begin(), canonical_edram.end());
  } catch (const std::bad_alloc&) {
    return false;
  } catch (const std::length_error&) {
    return false;
  }

  for (uint32_t sample = 0; sample < storage_layout_.sample_count; ++sample) {
    for (uint32_t y = 0; y < storage_layout_.height; ++y) {
      for (uint32_t x = 0; x < storage_layout_.width; ++x) {
        std::array<uint32_t, 2> words = {};
        for (uint32_t word = 0; word < storage_layout_.words_per_sample; ++word) {
          const size_t index = GetRawColorSidecarWordIndex(storage_layout_, x, y, sample, word);
          if (index == SIZE_MAX) {
            return false;
          }
          words[word] = words_[index];
        }
        if (!WriteCanonicalEdramSample(exported, canonical_layout, x, y, sample, words)) {
          return false;
        }
      }
    }
  }
  std::copy(exported.begin(), exported.end(), canonical_edram.begin());
  return true;
}

uint32_t Float32ToXenos7e3(float value) {
  if (!(value > 0.0f)) {
    return 0;
  }
  if (value >= 31.875f) {
    return 0x3FF;
  }

  uint32_t lower = 0;
  uint32_t upper = 0x3FF;
  while (upper - lower > 1) {
    const uint32_t midpoint = lower + (upper - lower) / 2;
    if (xenos::Float7e3To32(midpoint) < value) {
      lower = midpoint;
    } else {
      upper = midpoint;
    }
  }
  const double lower_distance = std::abs(double(value) - double(xenos::Float7e3To32(lower)));
  const double upper_distance = std::abs(double(xenos::Float7e3To32(upper)) - double(value));
  if (lower_distance < upper_distance) {
    return lower;
  }
  if (upper_distance < lower_distance) {
    return upper;
  }
  return (lower & 1u) ? upper : lower;
}

}  // namespace rex::graphics::metal
