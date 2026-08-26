#pragma once

#include <cstdint>

#include <rex/graphics/xenos.h>

namespace rex::graphics::metal {

// Host storage is selected from the guest render-target format, never from a
// generic 32-bpp assumption. kRaw32SidecarRequired documents formats whose
// bytes can't be represented by a native Metal attachment without changing
// their output-merger transfer function. Those formats remain fail-closed
// until the sidecar blend path is implemented.
enum class ColorTargetStorageKind : uint8_t {
  kUnsupported,
  kBgra8Unorm,
  kRgb10A2Unorm,
  kRaw32SidecarRequired,
};

struct ColorTargetStorageStrategy {
  ColorTargetStorageKind kind = ColorTargetStorageKind::kUnsupported;
  uint8_t bytes_per_pixel = 0;
  bool native_render_target = false;
  bool canonical_byte_round_trip = false;
};

constexpr ColorTargetStorageStrategy GetColorTargetStorageStrategy(
    xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      return {ColorTargetStorageKind::kBgra8Unorm, 4, true, true};
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      // Xenos PWL gamma is not sRGB. Native sRGB blending would use a
      // different transfer curve, while linear BGRA8 loses raw gamma codes.
      return {ColorTargetStorageKind::kRaw32SidecarRequired, 4, false, false};
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      return {ColorTargetStorageKind::kRgb10A2Unorm, 4, true, true};
    default:
      return {};
  }
}

constexpr bool IsNativeColorTargetStorageSupported(
    xenos::ColorRenderTargetFormat format) {
  ColorTargetStorageStrategy strategy = GetColorTargetStorageStrategy(format);
  return strategy.native_render_target && strategy.canonical_byte_round_trip;
}

// Stable key material shared by target and pipeline caches. Keep the original
// guest enum (rather than only the host kind) so future precision variants
// can't alias a pipeline accidentally.
constexpr uint64_t GetColorTargetStorageKey(xenos::ColorRenderTargetFormat format,
                                            uint32_t sample_count) {
  return uint64_t(uint32_t(format)) | (uint64_t(sample_count) << 32);
}

}  // namespace rex::graphics::metal
