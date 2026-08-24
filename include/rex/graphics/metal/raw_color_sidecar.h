#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <rex/graphics/metal/edram_snapshot.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::metal {

// Capabilities of the raw color sidecar are intentionally separate from the
// native Metal render-target capabilities. Raw words can be stored and moved
// losslessly before an exact Xenos output-merger or resolve implementation is
// available.
struct RawColorSidecarFormatCapabilities {
  uint8_t words_per_sample = 0;
  bool lossless_canonical_storage = false;
  bool exact_raster_draw = false;
  bool exact_direct_resolve = false;
};

constexpr RawColorSidecarFormatCapabilities GetRawColorSidecarFormatCapabilities(
    xenos::ColorRenderTargetFormat guest_format) {
  const xenos::ColorRenderTargetFormat storage_format = xenos::GetStorageColorFormat(guest_format);
  switch (storage_format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return {1, true, false, false};
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return {2, true, false, false};
    default:
      return {};
  }
}

constexpr bool CanRawColorSidecarStoreCanonicalLosslessly(
    xenos::ColorRenderTargetFormat guest_format) {
  return GetRawColorSidecarFormatCapabilities(guest_format).lossless_canonical_storage;
}

constexpr bool CanRawColorSidecarRasterizeExactly(xenos::ColorRenderTargetFormat guest_format) {
  return GetRawColorSidecarFormatCapabilities(guest_format).exact_raster_draw;
}

constexpr bool CanRawColorSidecarDirectResolveExactly(xenos::ColorRenderTargetFormat guest_format) {
  return GetRawColorSidecarFormatCapabilities(guest_format).exact_direct_resolve;
}

struct RawColorSidecarKey {
  // Keep the original enum rather than only GetStorageColorFormat: aliases
  // have identical stored bits, but different Xenos output-merger precision.
  xenos::ColorRenderTargetFormat guest_format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
  xenos::MsaaSamples msaa_samples = xenos::MsaaSamples::k1X;
  uint32_t width = 0;
  uint32_t height = 0;

  bool operator==(const RawColorSidecarKey&) const = default;
};

// Linear raw storage is guest-sample-major. This avoids host MSAA sample
// remapping and gives future compute paths a stable index independent of the
// native texture representation.
struct RawColorSidecarStorageLayout {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t sample_count = 0;
  uint32_t words_per_sample = 0;
  size_t word_count = 0;

  bool operator==(const RawColorSidecarStorageLayout&) const = default;
};

bool GetRawColorSidecarStorageLayout(xenos::ColorRenderTargetFormat guest_format, uint32_t width,
                                     uint32_t height, xenos::MsaaSamples msaa_samples,
                                     RawColorSidecarStorageLayout& layout_out);

// Returns SIZE_MAX if any coordinate is outside the validated linear layout.
size_t GetRawColorSidecarWordIndex(const RawColorSidecarStorageLayout& layout, uint32_t x,
                                   uint32_t y, uint32_t sample, uint32_t word);

enum class RawColorSidecarAuthority : uint8_t {
  kUninitialized,
  kRawAuthoritative,
  kNativeAuthoritative,
  kCoherent,
};

// Tracks whether the raw words are exact enough to export. There is
// deliberately no native-to-raw synchronization transition: it must not be
// introduced until that conversion is proven lossless for the guest format.
class RawColorSidecarAuthorityState {
 public:
  void Reset();
  void RecordCompleteRawWrite();
  void RecordNativeWrite();
  bool RecordNativeSynchronizedFromRaw();

  RawColorSidecarAuthority state() const { return state_; }
  bool raw_is_authoritative() const {
    return state_ == RawColorSidecarAuthority::kRawAuthoritative ||
           state_ == RawColorSidecarAuthority::kCoherent;
  }
  bool can_export_canonical() const { return raw_is_authoritative(); }

 private:
  RawColorSidecarAuthority state_ = RawColorSidecarAuthority::kUninitialized;
};

// CPU owner for the exact guest words. Logical RGBA conversion is explicitly
// outside this type: canonical restore/export never passes through float.
class RawColorSidecar {
 public:
  bool Configure(xenos::ColorRenderTargetFormat guest_format, uint32_t width, uint32_t height,
                 xenos::MsaaSamples msaa_samples) noexcept;
  void Reset();

  bool configured() const { return !words_.empty(); }
  const RawColorSidecarKey& key() const { return key_; }
  xenos::ColorRenderTargetFormat guest_format() const { return key_.guest_format; }
  xenos::ColorRenderTargetFormat storage_format() const {
    return xenos::GetStorageColorFormat(key_.guest_format);
  }
  const RawColorSidecarStorageLayout& storage_layout() const { return storage_layout_; }
  RawColorSidecarAuthority authority() const { return authority_.state(); }
  bool raw_is_authoritative() const { return configured() && authority_.raw_is_authoritative(); }
  bool can_export_canonical() const { return configured() && authority_.can_export_canonical(); }

  void Invalidate();
  void RecordNativeWrite();
  bool RecordNativeSynchronizedFromRaw();

  // Replaces the complete raw surface and establishes export authority.
  bool ReplaceRawWords(std::span<const uint32_t> words);
  bool ReadRawSample(uint32_t x, uint32_t y, uint32_t sample,
                     std::array<uint32_t, 2>& words_out) const;
  // Partial raw writes are accepted only if the full raw surface is already
  // authoritative.
  bool WriteRawSample(uint32_t x, uint32_t y, uint32_t sample,
                      const std::array<uint32_t, 2>& words);
  std::span<const uint32_t> raw_words() const {
    return raw_is_authoritative() ? std::span<const uint32_t>(words_) : std::span<const uint32_t>();
  }

  bool RestoreCanonical(std::span<const uint8_t> canonical_edram,
                        const CanonicalEdramSurfaceLayout& canonical_layout);
  bool ExportCanonical(std::span<uint8_t> canonical_edram,
                       const CanonicalEdramSurfaceLayout& canonical_layout) const;

 private:
  bool IsCanonicalLayoutCompatible(const CanonicalEdramSurfaceLayout& canonical_layout) const;

  RawColorSidecarKey key_;
  RawColorSidecarStorageLayout storage_layout_;
  RawColorSidecarAuthorityState authority_;
  std::vector<uint32_t> words_;
};

// Exact CPU conversion: clamps to the unsigned Xenos 7e3 range and rounds to
// the mathematically nearest value, choosing the even code on a tie.
uint32_t Float32ToXenos7e3(float value);

}  // namespace rex::graphics::metal
