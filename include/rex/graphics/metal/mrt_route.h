#pragma once

#include <array>
#include <cstdint>

#include <rex/graphics/xenos.h>

namespace rex::graphics::metal {

struct MrtActiveAttachmentSet {
  std::array<uint8_t, xenos::kMaxColorRenderTargets> slots = {};
  uint8_t output_mask = 0;
  uint8_t count = 0;
};

constexpr MrtActiveAttachmentSet GetMrtActiveAttachmentSet(
    uint32_t normalized_color_mask) {
  MrtActiveAttachmentSet result;
  for (uint32_t target = 0; target < xenos::kMaxColorRenderTargets; ++target) {
    if (((normalized_color_mask >> (target * 4)) & 0xF) == 0) {
      continue;
    }
    result.slots[result.count++] = uint8_t(target);
    result.output_mask |= uint8_t(uint32_t(1) << target);
  }
  return result;
}

constexpr uint32_t CountActiveColorTargets(uint32_t normalized_color_mask) {
  return GetMrtActiveAttachmentSet(normalized_color_mask).count;
}

// Multi-target draws must use IssueDraw's one-encoder submission. This helper
// separates them from the single-target route; replaying a fragment shader per
// target would duplicate depth/stencil, discard, blend and memory-export
// semantics, so any unsupported MRT state must still fail closed.
constexpr bool RequiresTrueMrtSubmission(uint32_t normalized_color_mask) {
  return CountActiveColorTargets(normalized_color_mask) > 1;
}

// Reusing an older position-producing shader is only valid when the current
// positionless vertex program has no guest-visible memory side effects. The
// one-draw MRT encoder can't execute two different vertex programs.
constexpr bool IsMrtVertexRouteSideEffectSafe(bool reuses_position_shader,
                                              bool active_vertex_has_side_effects) {
  return !reuses_position_shader || !active_vertex_has_side_effects;
}

constexpr bool IsMrtSubmissionExactlyOnce(uint32_t expected_color_targets,
                                          uint32_t bound_color_targets,
                                          uint32_t encoded_draws,
                                          uint32_t depth_attachment_binds,
                                          uint32_t completed_command_buffers) {
  return expected_color_targets >= 2 &&
         bound_color_targets == expected_color_targets && encoded_draws == 1 &&
         depth_attachment_binds == 1 && completed_command_buffers == 1;
}

struct MrtEdramSurface {
  uint32_t base_tiles = 0;
  uint32_t pitch_tiles = 0;
  uint32_t tile_columns = 0;
  uint32_t tile_rows = 0;
  bool enabled = false;
};

constexpr MrtEdramSurface MakeMrtEdramSurface(
    uint32_t base_tiles, uint32_t pitch_tiles, xenos::MsaaSamples msaa_samples,
    bool is_64bpp, uint32_t width, uint32_t height) {
  uint32_t sample_width = width << uint32_t(msaa_samples >= xenos::MsaaSamples::k4X);
  uint32_t sample_height = height << uint32_t(msaa_samples >= xenos::MsaaSamples::k2X);
  uint32_t word_width = sample_width * (is_64bpp ? 2u : 1u);
  return {
      base_tiles & (xenos::kEdramTileCount - 1),
      pitch_tiles,
      (word_width + xenos::kEdramTileWidthSamples - 1) /
          xenos::kEdramTileWidthSamples,
      (sample_height + xenos::kEdramTileHeightSamples - 1) /
          xenos::kEdramTileHeightSamples,
      width != 0 && height != 0,
  };
}

enum class MrtEdramValidation : uint8_t {
  kValid,
  kInvalidLayout,
  kSelfAliasing,
  kTargetOverlap,
};

// Native attachments can't reproduce Xenos's periodic EDRAM aliases. Reject
// both overlap between targets and a single wrapping surface that reaches the
// same physical tile more than once.
inline MrtEdramValidation ValidateMrtEdramSurfaceSet(
    const std::array<MrtEdramSurface, xenos::kMaxColorRenderTargets + 1>& surfaces) {
  std::array<uint8_t, xenos::kEdramTileCount> owners = {};
  for (uint32_t surface_index = 0; surface_index < surfaces.size(); ++surface_index) {
    const MrtEdramSurface& surface = surfaces[surface_index];
    if (!surface.enabled) {
      continue;
    }
    if (!surface.pitch_tiles || !surface.tile_columns || !surface.tile_rows ||
        surface.tile_columns > surface.pitch_tiles ||
        surface.pitch_tiles > xenos::kEdramTileCount ||
        surface.tile_rows > xenos::kEdramTileCount) {
      return MrtEdramValidation::kInvalidLayout;
    }
    std::array<bool, xenos::kEdramTileCount> surface_tiles = {};
    for (uint32_t tile_y = 0; tile_y < surface.tile_rows; ++tile_y) {
      for (uint32_t tile_x = 0; tile_x < surface.tile_columns; ++tile_x) {
        uint32_t tile = (surface.base_tiles + tile_y * surface.pitch_tiles + tile_x) &
                        (xenos::kEdramTileCount - 1);
        if (surface_tiles[tile]) {
          return MrtEdramValidation::kSelfAliasing;
        }
        surface_tiles[tile] = true;
        if (owners[tile]) {
          return MrtEdramValidation::kTargetOverlap;
        }
        owners[tile] = uint8_t(surface_index + 1);
      }
    }
  }
  return MrtEdramValidation::kValid;
}

}  // namespace rex::graphics::metal
