#include <rex/graphics/metal/exact_output_merger_state.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

#include <rex/graphics/pipeline/render_target/cache.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/util/draw.h>

namespace rex::graphics::metal {
namespace {

void SetError(std::string* error_out, std::string_view error) {
  if (error_out) {
    *error_out = error;
  }
}

constexpr uint32_t BitRangeMask(uint32_t first, uint32_t count) {
  return ((uint32_t(1) << count) - 1) << first;
}

float GetColorExponentBiasScale(int32_t exponent_bias) {
  const uint32_t bits =
      uint32_t(int64_t(UINT32_C(0x3F800000)) + int64_t(exponent_bias) * INT64_C(0x00800000));
  return std::bit_cast<float>(bits);
}

bool IsDefinedBlendFactor(uint32_t factor) {
  return factor <= uint32_t(xenos::BlendFactor::kOne) ||
         (factor >= uint32_t(xenos::BlendFactor::kSrcColor) &&
          factor <= uint32_t(xenos::BlendFactor::kSrcAlphaSaturate));
}

bool IsDefinedBlendState(uint32_t blend_factors_ops) {
  blend_factors_ops &= UINT32_C(0x1FFF1FFF);
  return IsDefinedBlendFactor(blend_factors_ops & 0x1Fu) &&
         ((blend_factors_ops >> 5) & 0x7u) <= uint32_t(xenos::BlendOp::kRevSubtract) &&
         IsDefinedBlendFactor((blend_factors_ops >> 8) & 0x1Fu) &&
         IsDefinedBlendFactor((blend_factors_ops >> 16) & 0x1Fu) &&
         ((blend_factors_ops >> 21) & 0x7u) <= uint32_t(xenos::BlendOp::kRevSubtract) &&
         IsDefinedBlendFactor((blend_factors_ops >> 24) & 0x1Fu);
}

// The resolve and clear kernels intentionally wrap the physical EDRAM tile at
// 2048, but all arithmetic before that wrap is 32-bit Metal integer math.
// Prove every intermediate for the complete dispatch here so malformed packed
// constants can't wrap to a different tile before the intentional modulo.
bool IsExactEdramDispatchAddressingBounded(const draw_util::ResolveEdramInfo& surface,
                                           const draw_util::ResolveCoordinateInfo& coordinates,
                                           uint32_t width, uint32_t height) {
  if (!surface.pitch_tiles || surface.msaa_samples > xenos::MsaaSamples::k4X || !width || !height) {
    return false;
  }

  const uint32_t sample_x_log2 = uint32_t(surface.msaa_samples >= xenos::MsaaSamples::k4X);
  const uint32_t sample_y_log2 = uint32_t(surface.msaa_samples >= xenos::MsaaSamples::k2X);
  const uint64_t source_x = uint64_t(coordinates.edram_offset_x_div_8) * 8;
  const uint64_t source_y = uint64_t(coordinates.edram_offset_y_div_8) * 8;
  const uint64_t pixel_x_last = source_x + width - 1;
  const uint64_t pixel_y_last = source_y + height - 1;
  const uint64_t sample_x_last = (pixel_x_last << sample_x_log2) + sample_x_log2;
  const uint64_t sample_y_last = (pixel_y_last << sample_y_log2) + sample_y_log2;
  if (sample_x_last > UINT32_MAX || sample_y_last > UINT32_MAX) {
    return false;
  }

  const uint64_t tile_x_last = sample_x_last / xenos::kEdramTileWidthSamples;
  const uint64_t tile_y_last = sample_y_last / xenos::kEdramTileHeightSamples;
  const uint64_t unwrapped_tile_last =
      uint64_t(surface.base_tiles) + tile_y_last * surface.pitch_tiles + tile_x_last;
  if (unwrapped_tile_last > UINT32_MAX) {
    return false;
  }

  // After the explicit 2048-tile wrap, the intra-tile address (including the
  // depth-bank X spill) is always within one physical 80x16 tile.
  const uint64_t wrapped_dword_last =
      uint64_t(xenos::kEdramTileCount - 1) * xenos::kEdramTileWidthSamples *
          xenos::kEdramTileHeightSamples +
      (xenos::kEdramTileHeightSamples - 1) * xenos::kEdramTileWidthSamples +
      (xenos::kEdramTileWidthSamples - 1);
  return wrapped_dword_last < kExactOutputMergerEdramDwordCount;
}

}  // namespace

bool BuildExactOutputMergerDrawState(const ExactOutputMergerDrawStateInput& input,
                                     ExactOutputMergerDrawState& state_out,
                                     std::string* error_out) {
  if (input.vertex_shader_memexport || input.pixel_shader_memexport ||
      input.reuses_vertex_position) {
    SetError(error_out,
             "exact output-merger primitive rejects memexport and vertex-position reuse");
    return false;
  }
  if (input.primitive_polygonal && input.polygon_dual_mode) {
    SetError(error_out, "exact output-merger does not route dual polygon point or line fill state");
    return false;
  }
  if (uint32_t(input.alpha_test_function) > uint32_t(xenos::CompareFunction::kAlways)) {
    SetError(error_out, "exact output-merger alpha comparison is invalid");
    return false;
  }
  if (input.depth_stencil.stencil_front_reference_masks & UINT32_C(0xFF000000) ||
      input.depth_stencil.stencil_back_reference_masks & UINT32_C(0xFF000000)) {
    SetError(error_out, "exact output-merger stencil reference or mask is invalid");
    return false;
  }
  if (input.depth_stencil.control.value & (uint32_t(1) << 3)) {
    SetError(error_out, "exact output-merger normalized depth control has reserved state");
    return false;
  }

  ExactOutputMergerDrawState candidate;
  candidate.system_constants = input.base_system_constants;
  SpirvShaderTranslator::SystemConstants& constants = candidate.system_constants;

  constexpr uint32_t kMsaaMask =
      BitRangeMask(SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift, xenos::kMsaaSamplesBits);
  constexpr uint32_t kAlphaTestMask =
      BitRangeMask(SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift, 3);
  constexpr uint32_t kGammaMask = BitRangeMask(
      SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma_Shift, xenos::kMaxColorRenderTargets);
  constexpr uint32_t kFsiMask = SpirvShaderTranslator::kSysFlag_FSIDepthStencil |
                                SpirvShaderTranslator::kSysFlag_FSIDepthPassIfLess |
                                SpirvShaderTranslator::kSysFlag_FSIDepthPassIfEqual |
                                SpirvShaderTranslator::kSysFlag_FSIDepthPassIfGreater |
                                SpirvShaderTranslator::kSysFlag_FSIDepthWrite |
                                SpirvShaderTranslator::kSysFlag_FSIStencilTest |
                                SpirvShaderTranslator::kSysFlag_FSIDepthStencilEarlyWrite;
  constants.flags &= ~(kMsaaMask | SpirvShaderTranslator::kSysFlag_DepthFloat24 | kAlphaTestMask |
                       kGammaMask | kFsiMask);
  constants.flags |= uint32_t(input.msaa_samples)
                     << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
  constants.flags |= uint32_t(input.alpha_test_function)
                     << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  constants.alpha_test_reference = input.alpha_test_reference;
  constants.alpha_to_mask =
      input.alpha_to_mask_enabled ? uint32_t(input.alpha_to_mask_dither) | (uint32_t(1) << 8) : 0;

  std::array<std::array<uint32_t, 2>, xenos::kMaxColorRenderTargets> keep_masks = {};
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    const ExactOutputMergerColorTargetInput& target = input.color_targets[i];
    if (target.write_mask & ~uint8_t(0xF)) {
      SetError(error_out, "exact output-merger color write mask is invalid");
      return false;
    }
    const bool target_active = (target.write_mask & 0xF) != 0;
    if (target_active && !IsDefinedBlendState(target.blend_factors_ops)) {
      SetError(error_out, "exact output-merger blend state is undefined");
      return false;
    }
    if (target_active && (target.exponent_bias < -32 || target.exponent_bias > 31)) {
      SetError(error_out, "exact output-merger color exponent bias is invalid");
      return false;
    }

    // Disabled targets are not part of the guest draw. Their registers are
    // allowed to contain stale or undefined values, so never validate or copy
    // them into the exact contract. Supplying a deterministic safe surface
    // also prevents old state from becoming observable if the shader happens
    // to calculate an inactive address before applying the keep mask.
    const uint32_t effective_base_tiles = target_active ? target.base_tiles : 0;
    const xenos::ColorRenderTargetFormat effective_format =
        target_active ? target.format : xenos::ColorRenderTargetFormat::k_8_8_8_8;
    const uint32_t effective_blend =
        target_active ? target.blend_factors_ops : UINT32_C(0x00010001);
    const int32_t effective_exponent_bias = target_active ? target.exponent_bias : 0;
    if (!BuildExactOutputMergerColorConstants(effective_base_tiles, input.surface_pitch_pixels,
                                              input.coverage_width, input.coverage_height,
                                              input.msaa_samples, effective_format,
                                              candidate.color_surfaces[i], error_out)) {
      return false;
    }

    float* clamp = constants.edram_rt_clamp[i];
    RenderTargetCache::GetPSIColorFormatInfo(effective_format, uint32_t(target.write_mask & 0xF),
                                             clamp[0], clamp[1], clamp[2], clamp[3],
                                             keep_masks[i][0], keep_masks[i][1]);
    constants.edram_rt_keep_mask[i][0] = keep_masks[i][0];
    constants.edram_rt_keep_mask[i][1] = keep_masks[i][1];
    constants.edram_rt_base_dwords_scaled[i] = candidate.color_surfaces[i].base_dwords;
    constants.edram_rt_format_flags[i] = candidate.color_surfaces[i].color_format_flags;
    constants.edram_rt_blend_factors_ops[i] = effective_blend & UINT32_C(0x1FFF1FFF);
    constants.color_exp_bias[i] = GetColorExponentBiasScale(effective_exponent_bias);

    if (keep_masks[i][0] != UINT32_MAX || keep_masks[i][1] != UINT32_MAX) {
      candidate.active_color_target_mask |= uint8_t(uint32_t(1) << i);
    }
    if (target_active && input.convert_gamma_targets &&
        effective_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      constants.flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
    }
  }

  const reg::RB_DEPTHCONTROL depth_control = input.depth_stencil.control;
  const bool depth_stencil_requested = depth_control.z_enable || depth_control.stencil_enable;
  const uint32_t effective_depth_base_tiles =
      depth_stencil_requested ? input.depth_stencil.base_tiles : 0;
  const xenos::DepthRenderTargetFormat effective_depth_format =
      depth_stencil_requested ? input.depth_stencil.format : xenos::DepthRenderTargetFormat::kD24S8;
  if (!BuildExactOutputMergerDepthConstants(effective_depth_base_tiles, input.surface_pitch_pixels,
                                            input.coverage_width, input.coverage_height,
                                            input.msaa_samples, effective_depth_format,
                                            candidate.depth_surface, error_out)) {
    return false;
  }
  constants.edram_32bpp_tile_pitch_dwords_scaled = candidate.depth_surface.tile_pitch_dwords;
  constants.edram_depth_base_dwords_scaled = candidate.depth_surface.base_dwords;
  if (depth_stencil_requested &&
      effective_depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    constants.flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  }

  candidate.depth_stencil_enabled = depth_stencil_requested;
  if (candidate.depth_stencil_enabled) {
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if ((candidate.active_color_target_mask & (uint32_t(1) << i)) &&
          input.depth_stencil.base_tiles == input.color_targets[i].base_tiles) {
        candidate.depth_stencil_enabled = false;
        break;
      }
    }
  }

  if (candidate.depth_stencil_enabled) {
    constants.flags |= SpirvShaderTranslator::kSysFlag_FSIDepthStencil;
    if (depth_control.z_enable) {
      constants.flags |= uint32_t(depth_control.zfunc)
                         << SpirvShaderTranslator::kSysFlag_FSIDepthPassIfLess_Shift;
      if (depth_control.z_write_enable) {
        constants.flags |= SpirvShaderTranslator::kSysFlag_FSIDepthWrite;
      }
    } else {
      constants.flags |= SpirvShaderTranslator::kSysFlag_FSIDepthPassIfLess |
                         SpirvShaderTranslator::kSysFlag_FSIDepthPassIfEqual |
                         SpirvShaderTranslator::kSysFlag_FSIDepthPassIfGreater;
    }
    if (depth_control.stencil_enable) {
      constants.flags |= SpirvShaderTranslator::kSysFlag_FSIStencilTest;
    }
    if (input.alpha_test_function == xenos::CompareFunction::kAlways &&
        !input.alpha_to_mask_enabled) {
      constants.flags |= SpirvShaderTranslator::kSysFlag_FSIDepthStencilEarlyWrite;
    }
  }

  float front_scale = 0.0f;
  float front_offset = 0.0f;
  float back_scale = 0.0f;
  float back_offset = 0.0f;
  if (input.primitive_polygonal) {
    if (input.polygon_offset_front_enabled) {
      front_scale = input.polygon_offset_front_scale;
      front_offset = input.polygon_offset_front_offset;
    }
    if (input.polygon_offset_back_enabled) {
      back_scale = input.polygon_offset_back_scale;
      back_offset = input.polygon_offset_back_offset;
    }
  } else if (input.polygon_offset_parameter_enabled) {
    front_scale = back_scale = input.polygon_offset_front_scale;
    front_offset = back_offset = input.polygon_offset_front_offset;
  }
  front_scale *= xenos::kPolygonOffsetScaleSubpixelUnit;
  back_scale *= xenos::kPolygonOffsetScaleSubpixelUnit;
  constants.edram_poly_offset_front_scale = front_scale;
  constants.edram_poly_offset_front_offset = front_offset;
  constants.edram_poly_offset_back_scale = back_scale;
  constants.edram_poly_offset_back_offset = back_offset;

  constants.edram_stencil_front_reference_masks = 0;
  constants.edram_stencil_front_func_ops = 0;
  constants.edram_stencil_back_reference_masks = 0;
  constants.edram_stencil_back_func_ops = 0;
  if (candidate.depth_stencil_enabled && depth_control.stencil_enable) {
    constants.edram_stencil_front_reference_masks =
        input.depth_stencil.stencil_front_reference_masks;
    constants.edram_stencil_front_func_ops = (depth_control.value >> 8) & ((uint32_t(1) << 12) - 1);
    if (input.primitive_polygonal && depth_control.backface_enable) {
      constants.edram_stencil_back_reference_masks =
          input.depth_stencil.stencil_back_reference_masks;
      constants.edram_stencil_back_func_ops =
          (depth_control.value >> 20) & ((uint32_t(1) << 12) - 1);
    } else {
      constants.edram_stencil_back_reference_masks = constants.edram_stencil_front_reference_masks;
      constants.edram_stencil_back_func_ops = constants.edram_stencil_front_func_ops;
    }
  }

  std::memcpy(constants.edram_blend_constant, input.blend_constant.data(),
              sizeof(constants.edram_blend_constant));

  state_out = candidate;
  if (error_out) {
    error_out->clear();
  }
  return true;
}

bool BuildExactOutputMergerDrawStateFromRegisters(
    const RegisterFile& register_file,
    const SpirvShaderTranslator::SystemConstants& base_system_constants, uint32_t coverage_width,
    uint32_t coverage_height, uint32_t shader_color_target_mask, bool primitive_polygonal,
    bool vertex_shader_memexport, bool pixel_shader_memexport, bool reuses_vertex_position,
    ExactOutputMergerDrawState& state_out, std::string* error_out) {
  ExactOutputMergerDrawStateInput input;
  input.base_system_constants = base_system_constants;
  const reg::RB_SURFACE_INFO surface_info = register_file.Get<reg::RB_SURFACE_INFO>();
  input.surface_pitch_pixels = surface_info.surface_pitch;
  input.coverage_width = coverage_width;
  input.coverage_height = coverage_height;
  input.msaa_samples = surface_info.msaa_samples;

  const uint32_t normalized_color_mask =
      draw_util::GetNormalizedColorMask(register_file, shader_color_target_mask);
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    const reg::RB_COLOR_INFO color_info =
        register_file.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    ExactOutputMergerColorTargetInput& target = input.color_targets[i];
    target.base_tiles = (color_info.color_base | (color_info.color_base_bit_11 << 11)) &
                        (xenos::kEdramTileCount - 1);
    target.format = color_info.color_format;
    target.exponent_bias = color_info.color_exp_bias;
    target.write_mask = uint8_t((normalized_color_mask >> (i * 4)) & 0xF);
    target.blend_factors_ops =
        register_file[reg::RB_BLENDCONTROL::rt_register_indices[i]] & UINT32_C(0x1FFF1FFF);
  }

  const reg::RB_DEPTH_INFO depth_info = register_file.Get<reg::RB_DEPTH_INFO>();
  input.depth_stencil.base_tiles =
      (depth_info.depth_base | (depth_info.depth_base_bit_11 << 11)) & (xenos::kEdramTileCount - 1);
  input.depth_stencil.format = depth_info.depth_format;
  input.depth_stencil.control = draw_util::GetNormalizedDepthControl(register_file);
  input.depth_stencil.stencil_front_reference_masks =
      register_file.Get<reg::RB_STENCILREFMASK>().value & UINT32_C(0x00FFFFFF);
  input.depth_stencil.stencil_back_reference_masks =
      register_file.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF).value &
      UINT32_C(0x00FFFFFF);

  const reg::RB_COLORCONTROL color_control = register_file.Get<reg::RB_COLORCONTROL>();
  input.alpha_test_function =
      color_control.alpha_test_enable ? color_control.alpha_func : xenos::CompareFunction::kAlways;
  input.alpha_test_reference = register_file.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  input.alpha_to_mask_enabled = color_control.alpha_to_mask_enable != 0;
  input.alpha_to_mask_dither = uint8_t(color_control.value >> 24);

  input.primitive_polygonal = primitive_polygonal;
  const reg::PA_SU_SC_MODE_CNTL mode_control = register_file.Get<reg::PA_SU_SC_MODE_CNTL>();
  input.polygon_dual_mode = mode_control.poly_mode == xenos::PolygonModeEnable::kDualMode;
  input.polygon_offset_front_enabled = mode_control.poly_offset_front_enable != 0;
  input.polygon_offset_back_enabled = mode_control.poly_offset_back_enable != 0;
  input.polygon_offset_parameter_enabled = mode_control.poly_offset_para_enable != 0;
  input.polygon_offset_front_scale =
      register_file.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
  input.polygon_offset_front_offset =
      register_file.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
  input.polygon_offset_back_scale =
      register_file.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
  input.polygon_offset_back_offset =
      register_file.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);

  input.blend_constant = {
      register_file.Get<float>(XE_GPU_REG_RB_BLEND_RED),
      register_file.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
      register_file.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
      register_file.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
  };
  input.vertex_shader_memexport = vertex_shader_memexport;
  input.pixel_shader_memexport = pixel_shader_memexport;
  input.reuses_vertex_position = reuses_vertex_position;
  return BuildExactOutputMergerDrawState(input, state_out, error_out);
}

bool BuildExactOutputMergerResolvePlan(const draw_util::ResolveInfo& resolve_info,
                                       size_t destination_buffer_size,
                                       ExactOutputMergerResolvePlan& plan_out,
                                       std::string* error_out) {
  const bool copying_depth = resolve_info.IsCopyingDepth();
  const draw_util::ResolveEdramInfo& source =
      copying_depth ? resolve_info.depth_edram_info : resolve_info.color_edram_info;
  const uint32_t width =
      uint32_t(resolve_info.coordinate_info.width_div_8) * xenos::kResolveAlignmentPixels;
  const uint32_t height = resolve_info.height_div_8 * xenos::kResolveAlignmentPixels;
  const uint32_t destination_pitch =
      uint32_t(resolve_info.copy_dest_coordinate_info.pitch_aligned_div_32) *
      xenos::kTextureTileWidthHeight;
  const uint32_t destination_height =
      uint32_t(resolve_info.copy_dest_coordinate_info.height_aligned_div_32) *
      xenos::kTextureTileWidthHeight;
  const uint32_t destination_x = uint32_t(resolve_info.copy_dest_coordinate_info.offset_x_div_8) *
                                 xenos::kResolveAlignmentPixels;
  const uint32_t destination_y = uint32_t(resolve_info.copy_dest_coordinate_info.offset_y_div_8) *
                                 xenos::kResolveAlignmentPixels;

  if (!destination_buffer_size || destination_buffer_size > UINT32_MAX || !width || !height ||
      ((resolve_info.copy_dest_base | resolve_info.copy_dest_extent_start |
        resolve_info.copy_dest_extent_length) &
       (sizeof(uint32_t) - 1)) ||
      resolve_info.coordinate_info.draw_resolution_scale_x != 1 ||
      resolve_info.coordinate_info.draw_resolution_scale_y != 1 || !source.pitch_tiles ||
      source.format_is_64bpp || source.fill_half_pixel_offset ||
      source.msaa_samples > xenos::MsaaSamples::k4X || source.is_depth != copying_depth ||
      !IsExactEdramDispatchAddressingBounded(source, resolve_info.coordinate_info, width, height) ||
      resolve_info.rb_copy_control.copy_command != xenos::CopyCommand::kConvert ||
      !destination_pitch || !destination_height || destination_x >= destination_pitch ||
      destination_y >= destination_height || width > destination_pitch - destination_x ||
      height > destination_height - destination_y || resolve_info.copy_dest_info.copy_dest_array ||
      resolve_info.copy_dest_info.copy_dest_slice ||
      resolve_info.copy_dest_info.copy_dest_exp_bias ||
      resolve_info.copy_dest_info.copy_dest_number !=
          xenos::SurfaceNumberFormat::kUnsignedRepeatingFraction ||
      uint32_t(resolve_info.copy_dest_info.copy_dest_endian) >
          uint32_t(xenos::Endian128::k16in32)) {
    SetError(error_out, "exact output-merger resolve geometry or destination is unsupported");
    return false;
  }

  const xenos::CopySampleSelect sanitized_sample =
      draw_util::SanitizeCopySampleSelect(resolve_info.copy_dest_coordinate_info.copy_sample_select,
                                          source.msaa_samples, copying_depth);
  if (sanitized_sample != resolve_info.copy_dest_coordinate_info.copy_sample_select) {
    SetError(error_out, "exact output-merger resolve sample selection is not normalized");
    return false;
  }
  if (copying_depth) {
    const xenos::DepthRenderTargetFormat depth_format =
        xenos::DepthRenderTargetFormat(source.format);
    const uint32_t expected_destination_format = uint32_t(
        depth_format == xenos::DepthRenderTargetFormat::kD24FS8 ? xenos::TextureFormat::k_24_8_FLOAT
                                                                : xenos::TextureFormat::k_24_8);
    if ((depth_format != xenos::DepthRenderTargetFormat::kD24S8 &&
         depth_format != xenos::DepthRenderTargetFormat::kD24FS8) ||
        uint32_t(resolve_info.copy_dest_info.copy_dest_format) != expected_destination_format ||
        !xenos::IsSingleCopySampleSelected(sanitized_sample) ||
        resolve_info.copy_dest_info.copy_dest_swap) {
      SetError(error_out, "exact output-merger packed depth resolve is unsupported");
      return false;
    }
  } else if (xenos::ColorRenderTargetFormat(source.format) !=
                 xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
             resolve_info.copy_dest_info.copy_dest_format != xenos::ColorFormat::k_8_8_8_8) {
    SetError(error_out, "exact output-merger color resolve is not 8888 to 8888");
    return false;
  }

  const uint32_t tiled_start =
      texture_util::GetTiledAddressLowerBound2D(destination_x, destination_y, destination_pitch, 2);
  const uint32_t tiled_end = texture_util::GetTiledAddressUpperBound2D(
      destination_x + width, destination_y + height, destination_pitch, 2);
  const uint64_t expected_start = uint64_t(resolve_info.copy_dest_base) + tiled_start;
  const uint64_t expected_end = uint64_t(resolve_info.copy_dest_base) + tiled_end;
  const uint64_t reported_end =
      uint64_t(resolve_info.copy_dest_extent_start) + resolve_info.copy_dest_extent_length;
  if (expected_end <= expected_start || expected_end > destination_buffer_size ||
      expected_start != resolve_info.copy_dest_extent_start || expected_end != reported_end ||
      !resolve_info.copy_dest_extent_length) {
    SetError(error_out, "exact output-merger resolve destination range is inconsistent");
    return false;
  }

  const bool clear_color = resolve_info.IsClearingColor();
  const bool clear_depth = resolve_info.IsClearingDepth();
  // RB_COPY_CONTROL can't select a depth source and a color clear owner at the
  // same time. Refuse this ambiguous packet rather than silently dropping a
  // requested side effect.
  if (copying_depth && resolve_info.rb_copy_control.color_clear_enable) {
    SetError(error_out, "exact output-merger depth resolve requests an unsupported color clear");
    return false;
  }
  auto clear_surface_supported = [&](const draw_util::ResolveEdramInfo& clear_surface, bool depth) {
    if (!clear_surface.pitch_tiles || clear_surface.format_is_64bpp ||
        clear_surface.fill_half_pixel_offset ||
        clear_surface.msaa_samples > xenos::MsaaSamples::k4X || clear_surface.is_depth != depth ||
        !IsExactEdramDispatchAddressingBounded(clear_surface, resolve_info.coordinate_info, width,
                                               height)) {
      return false;
    }
    if (depth) {
      return xenos::DepthRenderTargetFormat(clear_surface.format) ==
                 xenos::DepthRenderTargetFormat::kD24S8 ||
             xenos::DepthRenderTargetFormat(clear_surface.format) ==
                 xenos::DepthRenderTargetFormat::kD24FS8;
    }
    const xenos::ColorRenderTargetFormat color_format =
        xenos::ColorRenderTargetFormat(clear_surface.format);
    return IsCanonicalEdramColorFormatSupportedByMetal(color_format) &&
           !xenos::IsColorRenderTargetFormat64bpp(color_format);
  };
  if ((clear_color && !clear_surface_supported(resolve_info.color_edram_info, false)) ||
      (clear_depth && !clear_surface_supported(resolve_info.depth_edram_info, true))) {
    SetError(error_out, "exact output-merger resolve clear requires unsupported EDRAM storage");
    return false;
  }

  ExactOutputMergerResolvePlan candidate;
  candidate.copy_constants.dest_relative.edram_info = source;
  candidate.copy_constants.dest_relative.coordinate_info = resolve_info.coordinate_info;
  candidate.copy_constants.dest_relative.dest_info = resolve_info.copy_dest_info;
  candidate.copy_constants.dest_relative.dest_coordinate_info =
      resolve_info.copy_dest_coordinate_info;
  candidate.copy_constants.dest_base = resolve_info.copy_dest_base;
  if (clear_color) {
    resolve_info.GetColorClearShaderConstants(candidate.color_clear_constants);
  }
  if (clear_depth) {
    resolve_info.GetDepthClearShaderConstants(candidate.depth_clear_constants);
  }
  candidate.copy_width = width;
  candidate.copy_height = height;
  candidate.destination_start = resolve_info.copy_dest_extent_start;
  candidate.destination_length = resolve_info.copy_dest_extent_length;
  candidate.copying_depth = copying_depth;
  candidate.clear_color = clear_color;
  candidate.clear_depth = clear_depth;
  plan_out = candidate;
  if (error_out) {
    error_out->clear();
  }
  return true;
}

bool ExactOutputMergerPipelineUsesFetchConstants(const ExactOutputMergerPipelineLayout& layout) {
  return layout.vertex.fetch_constants_buffer_index != UINT32_MAX ||
         layout.fragment.fetch_constants_buffer_index != UINT32_MAX;
}

bool ExactOutputMergerStageUsesFloatConstants(const ExactOutputMergerStageLayout& layout) {
  return layout.float_constants_buffer_index != UINT32_MAX;
}

bool ExactOutputMergerPipelineUsesBoolLoopConstants(const ExactOutputMergerPipelineLayout& layout) {
  return layout.vertex.bool_loop_constants_buffer_index != UINT32_MAX ||
         layout.fragment.bool_loop_constants_buffer_index != UINT32_MAX;
}

bool ExactOutputMergerAuthorityTracker::MarkCpuUpload() {
  if (snapshot_.in_flight_sequence) {
    return false;
  }
  snapshot_.authority = ExactOutputMergerResourceAuthority::kCpu;
  return true;
}

bool ExactOutputMergerAuthorityTracker::BeginGpuSubmission(uint64_t& sequence_out) {
  sequence_out = 0;
  if (!CanEncodeGpuWork() || snapshot_.submitted_sequence == std::numeric_limits<uint64_t>::max()) {
    return false;
  }
  snapshot_.in_flight_sequence = ++snapshot_.submitted_sequence;
  sequence_out = snapshot_.in_flight_sequence;
  return true;
}

bool ExactOutputMergerAuthorityTracker::CanContinueGpuSubmission(uint64_t sequence) const {
  return sequence && sequence == snapshot_.in_flight_sequence &&
         snapshot_.authority != ExactOutputMergerResourceAuthority::kInvalid;
}

bool ExactOutputMergerAuthorityTracker::CompleteGpuSubmission(uint64_t sequence, bool succeeded) {
  if (!sequence || sequence != snapshot_.in_flight_sequence) {
    return false;
  }
  const bool invalidated_while_in_flight =
      snapshot_.authority == ExactOutputMergerResourceAuthority::kInvalid;
  snapshot_.in_flight_sequence = 0;
  if (!succeeded || invalidated_while_in_flight) {
    snapshot_.authority = ExactOutputMergerResourceAuthority::kInvalid;
    return true;
  }
  snapshot_.completed_sequence = sequence;
  snapshot_.authority = ExactOutputMergerResourceAuthority::kGpu;
  return true;
}

bool ExactOutputMergerAuthorityTracker::CancelGpuSubmission(uint64_t sequence) {
  if (!sequence || sequence != snapshot_.in_flight_sequence) {
    return false;
  }
  snapshot_.in_flight_sequence = 0;
  return true;
}

bool ExactOutputMergerAuthorityTracker::CanCpuAccess() const {
  return snapshot_.authority != ExactOutputMergerResourceAuthority::kInvalid &&
         !snapshot_.in_flight_sequence;
}

bool ExactOutputMergerAuthorityTracker::CanEncodeGpuWork() const {
  return snapshot_.authority != ExactOutputMergerResourceAuthority::kInvalid &&
         !snapshot_.in_flight_sequence;
}

void ExactOutputMergerAuthorityTracker::Invalidate() {
  snapshot_.authority = ExactOutputMergerResourceAuthority::kInvalid;
}

bool ExactOutputMergerResidencyTracker::RecordExactDraw() {
  if (invalid()) {
    return false;
  }
  residency_ = ExactOutputMergerResidency::kExactGpu;
  return true;
}

bool ExactOutputMergerResidencyTracker::RecordMaterialized() {
  if (!exact_gpu_current()) {
    return false;
  }
  residency_ = ExactOutputMergerResidency::kCanonical;
  return true;
}

void ExactOutputMergerResidencyTracker::RecordCanonicalReplacement() {
  residency_ = ExactOutputMergerResidency::kCanonical;
}

void ExactOutputMergerResidencyTracker::Invalidate() {
  residency_ = ExactOutputMergerResidency::kInvalid;
}

}  // namespace rex::graphics::metal
