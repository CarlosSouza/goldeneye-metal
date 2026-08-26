#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/metal/exact_output_merger_state.h>
#include <rex/graphics/metal/shader.h>
#include <rex/graphics/pipeline/render_target/cache.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/shared_memory.h>

namespace rex::graphics::metal {
namespace {

ExactOutputMergerDrawStateInput MakeStateInput() {
  ExactOutputMergerDrawStateInput input;
  input.surface_pitch_pixels = 160;
  input.coverage_width = 159;
  input.coverage_height = 33;
  input.msaa_samples = xenos::MsaaSamples::k1X;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    input.color_targets[i].base_tiles = 32 + i * 128;
    input.color_targets[i].format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
  }
  input.depth_stencil.base_tiles = 1024;
  input.depth_stencil.format = xenos::DepthRenderTargetFormat::kD24S8;
  return input;
}

void SetFloatRegister(RegisterFile& register_file, uint32_t index, float value) {
  std::memcpy(&register_file[index], &value, sizeof(value));
}

void InitializeValidExactRegisters(RegisterFile& register_file) {
  reg::RB_MODECONTROL mode_control = {};
  mode_control.edram_mode = xenos::EdramMode::kColorDepth;
  register_file[reg::RB_MODECONTROL::register_index] = mode_control.value;

  reg::RB_SURFACE_INFO surface_info = {};
  surface_info.surface_pitch = 160;
  surface_info.msaa_samples = xenos::MsaaSamples::k1X;
  register_file[reg::RB_SURFACE_INFO::register_index] = surface_info.value;

  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    reg::RB_COLOR_INFO color_info = {};
    color_info.color_base = 32 + i * 128;
    color_info.color_format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
    register_file[reg::RB_COLOR_INFO::rt_register_indices[i]] = color_info.value;
    register_file[reg::RB_BLENDCONTROL::rt_register_indices[i]] = UINT32_C(0x00010001);
  }

  reg::RB_DEPTH_INFO depth_info = {};
  depth_info.depth_base = 1024;
  depth_info.depth_format = xenos::DepthRenderTargetFormat::kD24S8;
  register_file[reg::RB_DEPTH_INFO::register_index] = depth_info.value;
}

constexpr std::array<xenos::ColorRenderTargetFormat, 12> kDefinedColorFormats = {
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

void RefreshResolveDestinationExtent(draw_util::ResolveInfo& info) {
  const uint32_t pitch = uint32_t(info.copy_dest_coordinate_info.pitch_aligned_div_32) *
                         xenos::kTextureTileWidthHeight;
  const uint32_t x =
      uint32_t(info.copy_dest_coordinate_info.offset_x_div_8) * xenos::kResolveAlignmentPixels;
  const uint32_t y =
      uint32_t(info.copy_dest_coordinate_info.offset_y_div_8) * xenos::kResolveAlignmentPixels;
  const uint32_t width =
      uint32_t(info.coordinate_info.width_div_8) * xenos::kResolveAlignmentPixels;
  const uint32_t height = info.height_div_8 * xenos::kResolveAlignmentPixels;
  info.copy_dest_extent_start =
      info.copy_dest_base + texture_util::GetTiledAddressLowerBound2D(x, y, pitch, 2);
  const uint32_t end = info.copy_dest_base +
                       texture_util::GetTiledAddressUpperBound2D(x + width, y + height, pitch, 2);
  info.copy_dest_extent_length = end - info.copy_dest_extent_start;
}

draw_util::ResolveInfo MakeColorResolveInfo(
    xenos::MsaaSamples msaa = xenos::MsaaSamples::k1X,
    xenos::CopySampleSelect sample = xenos::CopySampleSelect::k0) {
  draw_util::ResolveInfo info = {};
  info.rb_copy_control.copy_src_select = 0;
  info.rb_copy_control.copy_sample_select = sample;
  info.rb_copy_control.copy_command = xenos::CopyCommand::kConvert;
  info.color_edram_info.pitch_tiles = xenos::GetSurfacePitchTiles(160, msaa, false);
  info.color_edram_info.msaa_samples = msaa;
  info.color_edram_info.base_tiles = 7;
  info.color_edram_info.format = uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8);
  info.coordinate_info.edram_offset_x_div_8 = 1;
  info.coordinate_info.edram_offset_y_div_8 = 1;
  info.coordinate_info.width_div_8 = 2;
  info.coordinate_info.draw_resolution_scale_x = 1;
  info.coordinate_info.draw_resolution_scale_y = 1;
  info.height_div_8 = 1;
  info.copy_dest_info.copy_dest_format = xenos::ColorFormat::k_8_8_8_8;
  info.copy_dest_info.copy_dest_number = xenos::SurfaceNumberFormat::kUnsignedRepeatingFraction;
  info.copy_dest_coordinate_info.pitch_aligned_div_32 = 4;
  info.copy_dest_coordinate_info.height_aligned_div_32 = 2;
  info.copy_dest_coordinate_info.copy_sample_select = sample;
  info.copy_dest_base = UINT32_C(0x4000);
  RefreshResolveDestinationExtent(info);
  return info;
}

draw_util::ResolveInfo MakeDepthResolveInfo(xenos::DepthRenderTargetFormat format,
                                            xenos::MsaaSamples msaa,
                                            xenos::CopySampleSelect sample) {
  draw_util::ResolveInfo info = MakeColorResolveInfo(msaa, sample);
  info.rb_copy_control.copy_src_select = xenos::kMaxColorRenderTargets;
  info.color_edram_info.packed = 0;
  info.depth_edram_info.pitch_tiles = xenos::GetSurfacePitchTiles(160, msaa, false);
  info.depth_edram_info.msaa_samples = msaa;
  info.depth_edram_info.is_depth = 1;
  info.depth_edram_info.base_tiles = 900;
  info.depth_edram_info.format = uint32_t(format);
  info.copy_dest_info.copy_dest_format = xenos::ColorFormat(
      format == xenos::DepthRenderTargetFormat::kD24FS8 ? xenos::TextureFormat::k_24_8_FLOAT
                                                        : xenos::TextureFormat::k_24_8);
  return info;
}

}  // namespace

TEST_CASE("Exact Metal output-merger state covers every format and sample count",
          "[graphics][metal][exact-om]") {
  constexpr std::array<xenos::MsaaSamples, 3> kMsaa = {
      xenos::MsaaSamples::k1X,
      xenos::MsaaSamples::k2X,
      xenos::MsaaSamples::k4X,
  };
  for (xenos::MsaaSamples msaa : kMsaa) {
    for (xenos::ColorRenderTargetFormat format : kDefinedColorFormats) {
      INFO("msaa=" << uint32_t(msaa) << " format=" << uint32_t(format));
      ExactOutputMergerDrawStateInput input = MakeStateInput();
      input.msaa_samples = msaa;
      input.color_targets[1].format = format;
      input.color_targets[1].write_mask = 0xF;

      ExactOutputMergerDrawState state;
      std::string error;
      REQUIRE(BuildExactOutputMergerDrawState(input, state, &error));
      CHECK(error.empty());
      CHECK(state.active_color_target_mask == 0x2);
      CHECK(state.color_surfaces[1].sample_count == (msaa == xenos::MsaaSamples::k4X   ? 4
                                                     : msaa == xenos::MsaaSamples::k2X ? 2
                                                                                       : 1));
      CHECK(state.color_surfaces[1].words_per_sample ==
            (xenos::IsColorRenderTargetFormat64bpp(format) ? 2 : 1));
      CHECK(state.system_constants.edram_rt_format_flags[1] ==
            RenderTargetCache::AddPSIColorFormatFlags(format));
      CHECK(((state.system_constants.flags >> SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift) &
             ((uint32_t(1) << xenos::kMsaaSamplesBits) - 1)) == uint32_t(msaa));
    }
  }
}

TEST_CASE("Exact Metal output-merger supplies only reflected shared constants",
          "[graphics][metal][exact-om]") {
  ExactOutputMergerPipelineLayout layout;
  CHECK_FALSE(ExactOutputMergerStageUsesFloatConstants(layout.vertex));
  CHECK_FALSE(ExactOutputMergerStageUsesFloatConstants(layout.fragment));
  CHECK_FALSE(ExactOutputMergerPipelineUsesFetchConstants(layout));
  CHECK_FALSE(ExactOutputMergerPipelineUsesBoolLoopConstants(layout));

  layout.vertex.float_constants_buffer_index = 4;
  CHECK(ExactOutputMergerStageUsesFloatConstants(layout.vertex));
  CHECK_FALSE(ExactOutputMergerStageUsesFloatConstants(layout.fragment));
  layout.vertex.float_constants_buffer_index = UINT32_MAX;
  layout.fragment.float_constants_buffer_index = 5;
  CHECK_FALSE(ExactOutputMergerStageUsesFloatConstants(layout.vertex));
  CHECK(ExactOutputMergerStageUsesFloatConstants(layout.fragment));

  layout.vertex.fetch_constants_buffer_index = 2;
  CHECK(ExactOutputMergerPipelineUsesFetchConstants(layout));
  CHECK_FALSE(ExactOutputMergerPipelineUsesBoolLoopConstants(layout));

  layout.vertex.fetch_constants_buffer_index = UINT32_MAX;
  layout.fragment.fetch_constants_buffer_index = 7;
  layout.fragment.bool_loop_constants_buffer_index = 8;
  CHECK(ExactOutputMergerPipelineUsesFetchConstants(layout));
  CHECK(ExactOutputMergerPipelineUsesBoolLoopConstants(layout));

  layout.fragment.fetch_constants_buffer_index = UINT32_MAX;
  layout.fragment.bool_loop_constants_buffer_index = UINT32_MAX;
  layout.vertex.bool_loop_constants_buffer_index = 3;
  CHECK_FALSE(ExactOutputMergerPipelineUsesFetchConstants(layout));
  CHECK(ExactOutputMergerPipelineUsesBoolLoopConstants(layout));
}

TEST_CASE("Exact Metal output-merger residency materializes every route transition",
          "[graphics][metal][exact-om]") {
  ExactOutputMergerResidencyTracker residency;
  CHECK(residency.residency() == ExactOutputMergerResidency::kCanonical);
  CHECK_FALSE(residency.exact_gpu_current());
  CHECK_FALSE(residency.RecordMaterialized());

  REQUIRE(residency.RecordExactDraw());
  CHECK(residency.exact_gpu_current());
  REQUIRE(residency.RecordExactDraw());
  CHECK(residency.exact_gpu_current());
  REQUIRE(residency.RecordMaterialized());
  CHECK(residency.residency() == ExactOutputMergerResidency::kCanonical);

  REQUIRE(residency.RecordExactDraw());
  residency.Invalidate();
  CHECK(residency.invalid());
  CHECK_FALSE(residency.RecordExactDraw());
  CHECK_FALSE(residency.RecordMaterialized());

  residency.RecordCanonicalReplacement();
  CHECK(residency.residency() == ExactOutputMergerResidency::kCanonical);
  REQUIRE(residency.RecordExactDraw());
}

TEST_CASE("Exact Metal output-merger state preserves sparse masks and complete RT policy",
          "[graphics][metal][exact-om]") {
  ExactOutputMergerDrawStateInput input = MakeStateInput();
  input.color_targets[0].write_mask = 0b0101;
  input.color_targets[0].blend_factors_ops = UINT32_C(0xE001E001);
  input.color_targets[0].exponent_bias = -3;
  input.color_targets[2].format = xenos::ColorRenderTargetFormat::k_16_16_16_16;
  input.color_targets[2].write_mask = 0b1001;
  input.color_targets[2].blend_factors_ops = UINT32_C(0x01040104);
  input.color_targets[2].exponent_bias = 4;

  ExactOutputMergerDrawState state;
  REQUIRE(BuildExactOutputMergerDrawState(input, state));
  CHECK(state.active_color_target_mask == 0b0101);
  CHECK(state.system_constants.edram_rt_keep_mask[0][0] == UINT32_C(0xFF00FF00));
  CHECK(state.system_constants.edram_rt_keep_mask[0][1] == 0);
  CHECK(state.system_constants.edram_rt_keep_mask[1][0] == UINT32_MAX);
  CHECK(state.system_constants.edram_rt_keep_mask[1][1] == UINT32_MAX);
  CHECK(state.system_constants.edram_rt_keep_mask[2][0] == UINT32_C(0xFFFF0000));
  CHECK(state.system_constants.edram_rt_keep_mask[2][1] == UINT32_C(0x0000FFFF));
  CHECK(state.system_constants.edram_rt_blend_factors_ops[0] == UINT32_C(0x00010001));
  CHECK(state.system_constants.edram_rt_blend_factors_ops[2] == UINT32_C(0x01040104));
  CHECK(state.system_constants.color_exp_bias[0] == Catch::Approx(std::ldexp(1.0f, -3)));
  CHECK(state.system_constants.color_exp_bias[2] == Catch::Approx(std::ldexp(1.0f, 4)));
  CHECK(state.system_constants.edram_rt_base_dwords_scaled[2] ==
        input.color_targets[2].base_tiles * xenos::kEdramTileWidthSamples *
            xenos::kEdramTileHeightSamples);
  CHECK(state.system_constants.edram_rt_clamp[2][0] == -32.0f);
  CHECK(state.system_constants.edram_rt_clamp[2][2] == 32.0f);
}

TEST_CASE("Exact Metal output-merger state derives depth stencil gamma and polygon policy",
          "[graphics][metal][exact-om]") {
  ExactOutputMergerDrawStateInput input = MakeStateInput();
  input.msaa_samples = xenos::MsaaSamples::k4X;
  input.color_targets[3].format = xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA;
  input.color_targets[3].write_mask = 0xF;
  input.alpha_test_function = xenos::CompareFunction::kLessEqual;
  input.alpha_test_reference = 0.375f;
  input.alpha_to_mask_enabled = true;
  input.alpha_to_mask_dither = 0x96;
  input.depth_stencil.format = xenos::DepthRenderTargetFormat::kD24FS8;
  input.depth_stencil.control.z_enable = 1;
  input.depth_stencil.control.z_write_enable = 1;
  input.depth_stencil.control.zfunc = xenos::CompareFunction::kGreaterEqual;
  input.depth_stencil.control.stencil_enable = 1;
  input.depth_stencil.control.backface_enable = 1;
  input.depth_stencil.control.stencilfunc = xenos::CompareFunction::kEqual;
  input.depth_stencil.control.stencilfail = xenos::StencilOp::kReplace;
  input.depth_stencil.control.stencilzpass = xenos::StencilOp::kIncrementWrap;
  input.depth_stencil.control.stencilzfail = xenos::StencilOp::kDecrementClamp;
  input.depth_stencil.control.stencilfunc_bf = xenos::CompareFunction::kNotEqual;
  input.depth_stencil.control.stencilfail_bf = xenos::StencilOp::kInvert;
  input.depth_stencil.control.stencilzpass_bf = xenos::StencilOp::kDecrementWrap;
  input.depth_stencil.control.stencilzfail_bf = xenos::StencilOp::kZero;
  input.depth_stencil.stencil_front_reference_masks = UINT32_C(0x0055AA33);
  input.depth_stencil.stencil_back_reference_masks = UINT32_C(0x00123456);
  input.polygon_offset_front_enabled = true;
  input.polygon_offset_back_enabled = true;
  input.polygon_offset_front_scale = 2.0f;
  input.polygon_offset_front_offset = 3.5f;
  input.polygon_offset_back_scale = -4.0f;
  input.polygon_offset_back_offset = -1.25f;
  input.blend_constant = {0.1f, 0.2f, 0.3f, 0.4f};

  ExactOutputMergerDrawState state;
  REQUIRE(BuildExactOutputMergerDrawState(input, state));
  const uint32_t flags = state.system_constants.flags;
  CHECK(state.depth_stencil_enabled);
  CHECK(flags & SpirvShaderTranslator::kSysFlag_DepthFloat24);
  CHECK(flags & SpirvShaderTranslator::kSysFlag_ConvertColor3ToGamma);
  CHECK(flags & SpirvShaderTranslator::kSysFlag_FSIDepthStencil);
  CHECK(flags & SpirvShaderTranslator::kSysFlag_FSIDepthWrite);
  CHECK(flags & SpirvShaderTranslator::kSysFlag_FSIStencilTest);
  CHECK_FALSE(flags & SpirvShaderTranslator::kSysFlag_FSIDepthStencilEarlyWrite);
  CHECK(((flags >> SpirvShaderTranslator::kSysFlag_FSIDepthPassIfLess_Shift) & 7) ==
        uint32_t(xenos::CompareFunction::kGreaterEqual));
  CHECK(state.system_constants.alpha_test_reference == 0.375f);
  CHECK(state.system_constants.alpha_to_mask == UINT32_C(0x196));
  CHECK(state.system_constants.edram_stencil_front_reference_masks == UINT32_C(0x0055AA33));
  CHECK(state.system_constants.edram_stencil_back_reference_masks == UINT32_C(0x00123456));
  CHECK(state.system_constants.edram_stencil_front_func_ops ==
        ((input.depth_stencil.control.value >> 8) & 0xFFF));
  CHECK(state.system_constants.edram_stencil_back_func_ops ==
        ((input.depth_stencil.control.value >> 20) & 0xFFF));
  CHECK(state.system_constants.edram_poly_offset_front_scale ==
        Catch::Approx(2.0f * xenos::kPolygonOffsetScaleSubpixelUnit));
  CHECK(state.system_constants.edram_poly_offset_front_offset == 3.5f);
  CHECK(state.system_constants.edram_poly_offset_back_scale ==
        Catch::Approx(-4.0f * xenos::kPolygonOffsetScaleSubpixelUnit));
  CHECK(state.system_constants.edram_poly_offset_back_offset == -1.25f);
  CHECK(state.system_constants.edram_blend_constant[2] == 0.3f);
}

TEST_CASE("Exact Metal output-merger disables aliased depth and mirrors front stencil",
          "[graphics][metal][exact-om]") {
  ExactOutputMergerDrawStateInput input = MakeStateInput();
  input.color_targets[0].write_mask = 0xF;
  input.depth_stencil.base_tiles = input.color_targets[0].base_tiles;
  input.depth_stencil.control.z_enable = 1;
  input.depth_stencil.control.stencil_enable = 1;

  ExactOutputMergerDrawState state;
  REQUIRE(BuildExactOutputMergerDrawState(input, state));
  CHECK_FALSE(state.depth_stencil_enabled);
  CHECK_FALSE(state.system_constants.flags & SpirvShaderTranslator::kSysFlag_FSIDepthStencil);
  CHECK(state.system_constants.edram_stencil_front_reference_masks == 0);
  CHECK(state.system_constants.edram_stencil_back_reference_masks == 0);

  input.depth_stencil.base_tiles = 1500;
  input.primitive_polygonal = false;
  input.depth_stencil.stencil_front_reference_masks = UINT32_C(0x00010203);
  input.polygon_offset_parameter_enabled = true;
  input.polygon_offset_front_scale = 6.0f;
  input.polygon_offset_front_offset = 7.0f;
  REQUIRE(BuildExactOutputMergerDrawState(input, state));
  CHECK(state.depth_stencil_enabled);
  CHECK(state.system_constants.edram_stencil_back_reference_masks ==
        state.system_constants.edram_stencil_front_reference_masks);
  CHECK(state.system_constants.edram_stencil_back_func_ops ==
        state.system_constants.edram_stencil_front_func_ops);
  CHECK(state.system_constants.edram_poly_offset_back_scale ==
        state.system_constants.edram_poly_offset_front_scale);
  CHECK(state.system_constants.edram_poly_offset_back_offset == 7.0f);
}

TEST_CASE("Exact Metal output-merger captures live register state",
          "[graphics][metal][exact-om][registers]") {
  RegisterFile register_file;
  InitializeValidExactRegisters(register_file);

  reg::RB_SURFACE_INFO surface_info = {};
  surface_info.surface_pitch = 320;
  surface_info.msaa_samples = xenos::MsaaSamples::k4X;
  register_file[reg::RB_SURFACE_INFO::register_index] = surface_info.value;

  reg::RB_COLOR_INFO color0 = {};
  color0.color_base = 37;
  color0.color_base_bit_11 = 1;
  color0.color_format = xenos::ColorRenderTargetFormat::k_16_16;
  color0.color_exp_bias = -3;
  register_file[reg::RB_COLOR_INFO::rt_register_indices[0]] = color0.value;

  reg::RB_COLOR_INFO color2 = {};
  color2.color_base = 640;
  color2.color_format = xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA;
  color2.color_exp_bias = 4;
  register_file[reg::RB_COLOR_INFO::rt_register_indices[2]] = color2.value;

  // RT0 has two real components, so the missing B/A components are normalized
  // to written (0x1 becomes 0xD). RT1 and RT3 are masked out by the shader.
  register_file[XE_GPU_REG_RB_COLOR_MASK] = UINT32_C(0xFAF1);

  reg::RB_BLENDCONTROL blend0 = {};
  blend0.color_srcblend = xenos::BlendFactor::kSrcAlpha;
  blend0.color_comb_fcn = xenos::BlendOp::kSubtract;
  blend0.color_destblend = xenos::BlendFactor::kOneMinusSrcAlpha;
  blend0.alpha_srcblend = xenos::BlendFactor::kOne;
  blend0.alpha_comb_fcn = xenos::BlendOp::kRevSubtract;
  blend0.alpha_destblend = xenos::BlendFactor::kZero;
  blend0.value |= UINT32_C(0xE000E000);
  register_file[reg::RB_BLENDCONTROL::rt_register_indices[0]] = blend0.value;

  reg::RB_BLENDCONTROL blend2 = {};
  blend2.color_srcblend = xenos::BlendFactor::kConstantColor;
  blend2.color_comb_fcn = xenos::BlendOp::kMax;
  blend2.color_destblend = xenos::BlendFactor::kOneMinusConstantColor;
  blend2.alpha_srcblend = xenos::BlendFactor::kConstantAlpha;
  blend2.alpha_comb_fcn = xenos::BlendOp::kMin;
  blend2.alpha_destblend = xenos::BlendFactor::kOneMinusConstantAlpha;
  register_file[reg::RB_BLENDCONTROL::rt_register_indices[2]] = blend2.value;

  reg::RB_DEPTH_INFO depth_info = {};
  depth_info.depth_base = 1500;
  depth_info.depth_base_bit_11 = 1;
  depth_info.depth_format = xenos::DepthRenderTargetFormat::kD24FS8;
  register_file[reg::RB_DEPTH_INFO::register_index] = depth_info.value;

  reg::RB_DEPTHCONTROL depth_control = {};
  depth_control.z_enable = 1;
  depth_control.z_write_enable = 1;
  depth_control.zfunc = xenos::CompareFunction::kGreaterEqual;
  depth_control.stencil_enable = 1;
  depth_control.backface_enable = 1;
  depth_control.stencilfunc = xenos::CompareFunction::kEqual;
  depth_control.stencilfail = xenos::StencilOp::kReplace;
  depth_control.stencilzpass = xenos::StencilOp::kIncrementWrap;
  depth_control.stencilzfail = xenos::StencilOp::kDecrementClamp;
  depth_control.stencilfunc_bf = xenos::CompareFunction::kNotEqual;
  depth_control.stencilfail_bf = xenos::StencilOp::kInvert;
  depth_control.stencilzpass_bf = xenos::StencilOp::kDecrementWrap;
  depth_control.stencilzfail_bf = xenos::StencilOp::kZero;
  register_file[reg::RB_DEPTHCONTROL::register_index] = depth_control.value;
  register_file[XE_GPU_REG_RB_STENCILREFMASK] = UINT32_C(0xA5123456);
  register_file[XE_GPU_REG_RB_STENCILREFMASK_BF] = UINT32_C(0x5A654321);

  reg::RB_COLORCONTROL color_control = {};
  color_control.alpha_test_enable = 1;
  color_control.alpha_func = xenos::CompareFunction::kLessEqual;
  color_control.alpha_to_mask_enable = 1;
  color_control.alpha_to_mask_offset0 = 2;
  color_control.alpha_to_mask_offset1 = 1;
  color_control.alpha_to_mask_offset2 = 1;
  color_control.alpha_to_mask_offset3 = 2;
  register_file[reg::RB_COLORCONTROL::register_index] = color_control.value;
  SetFloatRegister(register_file, XE_GPU_REG_RB_ALPHA_REF, 0.375f);

  reg::PA_SU_SC_MODE_CNTL mode_control = {};
  mode_control.poly_offset_front_enable = 1;
  mode_control.poly_offset_back_enable = 1;
  mode_control.poly_offset_para_enable = 1;
  register_file[reg::PA_SU_SC_MODE_CNTL::register_index] = mode_control.value;
  SetFloatRegister(register_file, XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE, 2.5f);
  SetFloatRegister(register_file, XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET, 3.25f);
  SetFloatRegister(register_file, XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE, -4.0f);
  SetFloatRegister(register_file, XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET, -1.5f);
  SetFloatRegister(register_file, XE_GPU_REG_RB_BLEND_RED, 0.1f);
  SetFloatRegister(register_file, XE_GPU_REG_RB_BLEND_GREEN, 0.2f);
  SetFloatRegister(register_file, XE_GPU_REG_RB_BLEND_BLUE, 0.3f);
  SetFloatRegister(register_file, XE_GPU_REG_RB_BLEND_ALPHA, 0.4f);

  SpirvShaderTranslator::SystemConstants base_constants = {};
  base_constants.flags = SpirvShaderTranslator::kSysFlag_XYDividedByW;
  base_constants.vertex_base_index = 73;
  ExactOutputMergerDrawState state;
  std::string error;
  REQUIRE(BuildExactOutputMergerDrawStateFromRegisters(
      register_file, base_constants, 319, 45, 0b0101, true, false, false, false, state, &error));
  CHECK(error.empty());

  CHECK(state.active_color_target_mask == 0b0101);
  CHECK(state.color_surfaces[0].canonical_layout.base_tiles == 37);
  CHECK(state.color_surfaces[0].canonical_layout.msaa_samples == xenos::MsaaSamples::k4X);
  CHECK(state.color_surfaces[0].surface_pitch_pixels == 320);
  CHECK(state.color_surfaces[0].sample_count == 4);
  CHECK(state.color_surfaces[0].color_format == xenos::ColorRenderTargetFormat::k_16_16);
  CHECK(state.color_surfaces[2].canonical_layout.base_tiles == 640);
  CHECK(state.system_constants.edram_rt_base_dwords_scaled[0] ==
        37 * xenos::kEdramTileWidthSamples * xenos::kEdramTileHeightSamples);
  CHECK(state.system_constants.edram_rt_format_flags[0] ==
        RenderTargetCache::AddPSIColorFormatFlags(xenos::ColorRenderTargetFormat::k_16_16));
  CHECK(state.system_constants.edram_rt_blend_factors_ops[0] ==
        (blend0.value & UINT32_C(0x1FFF1FFF)));
  CHECK(state.system_constants.edram_rt_blend_factors_ops[2] == blend2.value);
  CHECK(state.system_constants.color_exp_bias[0] == Catch::Approx(std::ldexp(1.0f, -3)));
  CHECK(state.system_constants.color_exp_bias[2] == Catch::Approx(std::ldexp(1.0f, 4)));
  CHECK(state.system_constants.edram_rt_keep_mask[1][0] == UINT32_MAX);
  CHECK(state.system_constants.edram_rt_keep_mask[1][1] == UINT32_MAX);
  CHECK(state.system_constants.edram_rt_keep_mask[2][0] == UINT32_C(0x00FF00FF));
  CHECK(state.system_constants.edram_32bpp_tile_pitch_dwords_scaled ==
        xenos::GetSurfacePitchTiles(320, xenos::MsaaSamples::k4X, false) *
            xenos::kEdramTileWidthSamples * xenos::kEdramTileHeightSamples);

  CHECK(state.depth_stencil_enabled);
  CHECK(state.depth_surface.canonical_layout.base_tiles == 1500);
  CHECK(state.depth_surface.depth_format == xenos::DepthRenderTargetFormat::kD24FS8);
  const uint32_t flags = state.system_constants.flags;
  CHECK(flags & SpirvShaderTranslator::kSysFlag_XYDividedByW);
  CHECK(flags & SpirvShaderTranslator::kSysFlag_DepthFloat24);
  CHECK(flags & SpirvShaderTranslator::kSysFlag_ConvertColor2ToGamma);
  CHECK(flags & SpirvShaderTranslator::kSysFlag_FSIDepthStencil);
  CHECK(flags & SpirvShaderTranslator::kSysFlag_FSIDepthWrite);
  CHECK(flags & SpirvShaderTranslator::kSysFlag_FSIStencilTest);
  CHECK(((flags >> SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift) & 3) ==
        uint32_t(xenos::MsaaSamples::k4X));
  CHECK(((flags >> SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift) & 7) ==
        uint32_t(xenos::CompareFunction::kLessEqual));
  CHECK(((flags >> SpirvShaderTranslator::kSysFlag_FSIDepthPassIfLess_Shift) & 7) ==
        uint32_t(xenos::CompareFunction::kGreaterEqual));
  CHECK(state.system_constants.vertex_base_index == 73);
  CHECK(state.system_constants.alpha_test_reference == 0.375f);
  CHECK(state.system_constants.alpha_to_mask == UINT32_C(0x196));
  CHECK(state.system_constants.edram_stencil_front_reference_masks == UINT32_C(0x00123456));
  CHECK(state.system_constants.edram_stencil_back_reference_masks == UINT32_C(0x00654321));
  CHECK(state.system_constants.edram_stencil_front_func_ops ==
        ((depth_control.value >> 8) & 0xFFF));
  CHECK(state.system_constants.edram_stencil_back_func_ops ==
        ((depth_control.value >> 20) & 0xFFF));
  CHECK(state.system_constants.edram_poly_offset_front_scale ==
        Catch::Approx(2.5f * xenos::kPolygonOffsetScaleSubpixelUnit));
  CHECK(state.system_constants.edram_poly_offset_front_offset == 3.25f);
  CHECK(state.system_constants.edram_poly_offset_back_scale ==
        Catch::Approx(-4.0f * xenos::kPolygonOffsetScaleSubpixelUnit));
  CHECK(state.system_constants.edram_poly_offset_back_offset == -1.5f);
  CHECK(state.system_constants.edram_blend_constant[0] == 0.1f);
  CHECK(state.system_constants.edram_blend_constant[1] == 0.2f);
  CHECK(state.system_constants.edram_blend_constant[2] == 0.3f);
  CHECK(state.system_constants.edram_blend_constant[3] == 0.4f);
}

TEST_CASE("Exact Metal register state extraction preserves output on memexport rejection",
          "[graphics][metal][exact-om][registers]") {
  RegisterFile register_file;
  InitializeValidExactRegisters(register_file);
  register_file[XE_GPU_REG_RB_COLOR_MASK] = 0xF;

  SpirvShaderTranslator::SystemConstants base_constants = {};
  base_constants.vertex_base_index = -91;
  ExactOutputMergerDrawState state = {};
  state.system_constants.flags = UINT32_C(0xA5A55A5A);
  state.system_constants.vertex_base_index = 123;
  state.color_surfaces[2].base_dwords = UINT32_C(0x12345678);
  state.depth_surface.base_dwords = UINT32_C(0x87654321);
  state.active_color_target_mask = 0xA;
  state.depth_stencil_enabled = true;
  const ExactOutputMergerDrawState sentinel = state;

  SECTION("vertex memexport") {
    std::string error;
    CHECK_FALSE(BuildExactOutputMergerDrawStateFromRegisters(
        register_file, base_constants, 159, 33, 1, true, true, false, false, state, &error));
    CHECK(error.find("memexport") != std::string::npos);
    CHECK(std::memcmp(&state, &sentinel, sizeof(state)) == 0);
  }

  SECTION("pixel memexport") {
    std::string error;
    CHECK_FALSE(BuildExactOutputMergerDrawStateFromRegisters(
        register_file, base_constants, 159, 33, 1, true, false, true, false, state, &error));
    CHECK(error.find("memexport") != std::string::npos);
    CHECK(std::memcmp(&state, &sentinel, sizeof(state)) == 0);
  }

  SECTION("dual polygon fill") {
    reg::PA_SU_SC_MODE_CNTL mode_control = {};
    mode_control.poly_mode = xenos::PolygonModeEnable::kDualMode;
    mode_control.polymode_front_ptype = xenos::PolygonType::kLines;
    mode_control.polymode_back_ptype = xenos::PolygonType::kTriangles;
    register_file[reg::PA_SU_SC_MODE_CNTL::register_index] = mode_control.value;
    std::string error;
    CHECK_FALSE(BuildExactOutputMergerDrawStateFromRegisters(
        register_file, base_constants, 159, 33, 1, true, false, false, false, state, &error));
    CHECK(error.find("polygon") != std::string::npos);
    CHECK(std::memcmp(&state, &sentinel, sizeof(state)) == 0);
  }
}

TEST_CASE("Exact Metal output-merger state is transactional and rejects replay hazards",
          "[graphics][metal][exact-om]") {
  ExactOutputMergerDrawState state;
  state.system_constants.flags = UINT32_C(0xA5A5A5A5);
  state.active_color_target_mask = 0xA;
  state.depth_stencil_enabled = true;
  const ExactOutputMergerDrawState before = state;

  ExactOutputMergerDrawStateInput input = MakeStateInput();
  input.vertex_shader_memexport = true;
  std::string error;
  CHECK_FALSE(BuildExactOutputMergerDrawState(input, state, &error));
  CHECK_FALSE(error.empty());
  CHECK(std::memcmp(&state, &before, sizeof(state)) == 0);

  input.vertex_shader_memexport = false;
  input.pixel_shader_memexport = true;
  CHECK_FALSE(BuildExactOutputMergerDrawState(input, state));
  CHECK(std::memcmp(&state, &before, sizeof(state)) == 0);

  input.pixel_shader_memexport = false;
  input.reuses_vertex_position = true;
  CHECK_FALSE(BuildExactOutputMergerDrawState(input, state));
  CHECK(std::memcmp(&state, &before, sizeof(state)) == 0);

  input.reuses_vertex_position = false;
  input.polygon_dual_mode = true;
  CHECK_FALSE(BuildExactOutputMergerDrawState(input, state));
  CHECK(std::memcmp(&state, &before, sizeof(state)) == 0);

  input.polygon_dual_mode = false;
  input.color_targets[2].base_tiles = xenos::kEdramTileCount;
  input.color_targets[2].write_mask = 0xF;
  CHECK_FALSE(BuildExactOutputMergerDrawState(input, state));
  CHECK(std::memcmp(&state, &before, sizeof(state)) == 0);

  input = MakeStateInput();
  input.color_targets[0].write_mask = 0x10;
  CHECK_FALSE(BuildExactOutputMergerDrawState(input, state));
  CHECK(std::memcmp(&state, &before, sizeof(state)) == 0);

  input = MakeStateInput();
  input.color_targets[1].blend_factors_ops = 2;
  input.color_targets[1].write_mask = 0xF;
  CHECK_FALSE(BuildExactOutputMergerDrawState(input, state));
  CHECK(std::memcmp(&state, &before, sizeof(state)) == 0);

  input = MakeStateInput();
  input.depth_stencil.control.value = uint32_t(1) << 3;
  CHECK_FALSE(BuildExactOutputMergerDrawState(input, state));
  CHECK(std::memcmp(&state, &before, sizeof(state)) == 0);

  input = MakeStateInput();
  input.color_targets[3].format = static_cast<xenos::ColorRenderTargetFormat>(8);
  input.color_targets[3].write_mask = 0xF;
  CHECK_FALSE(BuildExactOutputMergerDrawState(input, state));
  CHECK(std::memcmp(&state, &before, sizeof(state)) == 0);

  input = MakeStateInput();
  input.depth_stencil.format = static_cast<xenos::DepthRenderTargetFormat>(2);
  input.depth_stencil.control.z_enable = 1;
  CHECK_FALSE(BuildExactOutputMergerDrawState(input, state));
  CHECK(std::memcmp(&state, &before, sizeof(state)) == 0);
}

TEST_CASE("Exact Metal output-merger ignores stale inactive attachment registers",
          "[graphics][metal][exact-om]") {
  ExactOutputMergerDrawStateInput input = MakeStateInput();
  input.color_targets[0].base_tiles = xenos::kEdramTileCount + 91;
  input.color_targets[0].format = static_cast<xenos::ColorRenderTargetFormat>(UINT32_MAX);
  input.color_targets[0].exponent_bias = 999;
  input.color_targets[0].blend_factors_ops = UINT32_MAX;
  input.depth_stencil.base_tiles = xenos::kEdramTileCount + 73;
  input.depth_stencil.format = static_cast<xenos::DepthRenderTargetFormat>(UINT32_MAX);

  ExactOutputMergerDrawState state;
  std::string error;
  REQUIRE(BuildExactOutputMergerDrawState(input, state, &error));
  CHECK(error.empty());
  CHECK(state.active_color_target_mask == 0);
  CHECK_FALSE(state.depth_stencil_enabled);
  CHECK(state.color_surfaces[0].canonical_layout.base_tiles == 0);
  CHECK(state.color_surfaces[0].color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8);
  CHECK(state.system_constants.edram_rt_base_dwords_scaled[0] == 0);
  CHECK(state.system_constants.edram_rt_blend_factors_ops[0] == UINT32_C(0x00010001));
  CHECK(state.system_constants.color_exp_bias[0] == 1.0f);
  CHECK(state.depth_surface.canonical_layout.base_tiles == 0);
  CHECK(state.depth_surface.depth_format == xenos::DepthRenderTargetFormat::kD24S8);
  CHECK(state.system_constants.edram_depth_base_dwords_scaled == 0);
  CHECK_FALSE(state.system_constants.flags & SpirvShaderTranslator::kSysFlag_DepthFloat24);
}

TEST_CASE("Exact Metal output-merger replaces every stale output field",
          "[graphics][metal][exact-om]") {
  ExactOutputMergerDrawStateInput input = MakeStateInput();
  std::memset(&input.base_system_constants.alpha_test_reference, 0xA5,
              sizeof(input.base_system_constants.alpha_test_reference));
  input.base_system_constants.alpha_to_mask = UINT32_MAX;
  input.base_system_constants.edram_32bpp_tile_pitch_dwords_scaled = UINT32_MAX;
  input.base_system_constants.edram_depth_base_dwords_scaled = UINT32_MAX;
  std::fill_n(input.base_system_constants.color_exp_bias, 4, std::nanf(""));
  input.base_system_constants.edram_poly_offset_front_scale = std::nanf("");
  input.base_system_constants.edram_poly_offset_back_scale = std::nanf("");
  input.base_system_constants.edram_poly_offset_front_offset = std::nanf("");
  input.base_system_constants.edram_poly_offset_back_offset = std::nanf("");
  input.base_system_constants.edram_stencil_front_reference_masks = UINT32_MAX;
  input.base_system_constants.edram_stencil_front_func_ops = UINT32_MAX;
  input.base_system_constants.edram_stencil_back_reference_masks = UINT32_MAX;
  input.base_system_constants.edram_stencil_back_func_ops = UINT32_MAX;
  std::memset(input.base_system_constants.edram_rt_base_dwords_scaled, 0xA5,
              sizeof(input.base_system_constants.edram_rt_base_dwords_scaled));
  std::memset(input.base_system_constants.edram_rt_format_flags, 0xA5,
              sizeof(input.base_system_constants.edram_rt_format_flags));
  std::memset(input.base_system_constants.edram_rt_blend_factors_ops, 0xA5,
              sizeof(input.base_system_constants.edram_rt_blend_factors_ops));
  std::memset(input.base_system_constants.edram_rt_keep_mask, 0xA5,
              sizeof(input.base_system_constants.edram_rt_keep_mask));
  std::memset(input.base_system_constants.edram_rt_clamp, 0xA5,
              sizeof(input.base_system_constants.edram_rt_clamp));
  std::memset(input.base_system_constants.edram_blend_constant, 0xA5,
              sizeof(input.base_system_constants.edram_blend_constant));

  ExactOutputMergerDrawState state;
  REQUIRE(BuildExactOutputMergerDrawState(input, state));
  CHECK(state.active_color_target_mask == 0);
  CHECK_FALSE(state.depth_stencil_enabled);
  CHECK(state.system_constants.alpha_test_reference == 0.0f);
  CHECK(state.system_constants.alpha_to_mask == 0);
  CHECK(state.system_constants.edram_depth_base_dwords_scaled == 0);
  CHECK(state.system_constants.edram_poly_offset_front_scale == 0.0f);
  CHECK(state.system_constants.edram_poly_offset_back_scale == 0.0f);
  CHECK(state.system_constants.edram_poly_offset_front_offset == 0.0f);
  CHECK(state.system_constants.edram_poly_offset_back_offset == 0.0f);
  CHECK(state.system_constants.edram_stencil_front_reference_masks == 0);
  CHECK(state.system_constants.edram_stencil_front_func_ops == 0);
  CHECK(state.system_constants.edram_stencil_back_reference_masks == 0);
  CHECK(state.system_constants.edram_stencil_back_func_ops == 0);
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    CHECK(state.system_constants.color_exp_bias[i] == 1.0f);
    CHECK(state.system_constants.edram_rt_base_dwords_scaled[i] == 0);
    CHECK(state.system_constants.edram_rt_format_flags[i] ==
          RenderTargetCache::AddPSIColorFormatFlags(xenos::ColorRenderTargetFormat::k_8_8_8_8));
    CHECK(state.system_constants.edram_rt_blend_factors_ops[i] == UINT32_C(0x00010001));
    CHECK(state.system_constants.edram_rt_keep_mask[i][0] == UINT32_MAX);
    CHECK(state.system_constants.edram_rt_keep_mask[i][1] == UINT32_MAX);
    CHECK(state.system_constants.edram_rt_clamp[i][0] == 0.0f);
    CHECK(state.system_constants.edram_rt_clamp[i][1] == 0.0f);
    CHECK(state.system_constants.edram_rt_clamp[i][2] == 1.0f);
    CHECK(state.system_constants.edram_rt_clamp[i][3] == 1.0f);
    CHECK(state.system_constants.edram_blend_constant[i] == 0.0f);
  }
}

TEST_CASE("Exact Metal output-merger authority fails closed across submissions",
          "[graphics][metal][exact-om]") {
  ExactOutputMergerAuthorityTracker tracker;
  CHECK_FALSE(tracker.CanCpuAccess());
  CHECK_FALSE(tracker.CanEncodeGpuWork());

  REQUIRE(tracker.MarkCpuUpload());
  CHECK(tracker.CanCpuAccess());
  CHECK(tracker.CanEncodeGpuWork());
  CHECK(tracker.snapshot().authority == ExactOutputMergerResourceAuthority::kCpu);

  uint64_t first = 0;
  REQUIRE(tracker.BeginGpuSubmission(first));
  CHECK(first == 1);
  CHECK(tracker.CanContinueGpuSubmission(first));
  CHECK_FALSE(tracker.CanContinueGpuSubmission(first + 1));
  CHECK_FALSE(tracker.CanCpuAccess());
  CHECK_FALSE(tracker.CanEncodeGpuWork());
  CHECK_FALSE(tracker.MarkCpuUpload());
  uint64_t rejected = 99;
  CHECK_FALSE(tracker.BeginGpuSubmission(rejected));
  CHECK(rejected == 0);
  CHECK_FALSE(tracker.CompleteGpuSubmission(first + 1, true));
  REQUIRE(tracker.CompleteGpuSubmission(first, true));
  CHECK_FALSE(tracker.CanContinueGpuSubmission(first));
  CHECK(tracker.snapshot().authority == ExactOutputMergerResourceAuthority::kGpu);
  CHECK(tracker.snapshot().completed_sequence == first);
  CHECK(tracker.CanCpuAccess());

  uint64_t cancelled = 0;
  REQUIRE(tracker.BeginGpuSubmission(cancelled));
  REQUIRE(tracker.CancelGpuSubmission(cancelled));
  CHECK(tracker.CanCpuAccess());
  CHECK(tracker.snapshot().authority == ExactOutputMergerResourceAuthority::kGpu);

  uint64_t second = 0;
  REQUIRE(tracker.BeginGpuSubmission(second));
  REQUIRE(tracker.CompleteGpuSubmission(second, false));
  CHECK(tracker.snapshot().authority == ExactOutputMergerResourceAuthority::kInvalid);
  CHECK_FALSE(tracker.CanCpuAccess());
  CHECK_FALSE(tracker.CanEncodeGpuWork());

  REQUIRE(tracker.MarkCpuUpload());
  uint64_t invalidated_in_flight = 0;
  REQUIRE(tracker.BeginGpuSubmission(invalidated_in_flight));
  tracker.Invalidate();
  CHECK(tracker.snapshot().in_flight_sequence == invalidated_in_flight);
  CHECK_FALSE(tracker.MarkCpuUpload());
  REQUIRE(tracker.CompleteGpuSubmission(invalidated_in_flight, true));
  CHECK(tracker.snapshot().authority == ExactOutputMergerResourceAuthority::kInvalid);
  CHECK(tracker.snapshot().in_flight_sequence == 0);
  REQUIRE(tracker.MarkCpuUpload());
  tracker.Invalidate();
  CHECK_FALSE(tracker.CanCpuAccess());
}

TEST_CASE("Exact Metal shader translations cannot alias native cache entries",
          "[graphics][metal][exact-om]") {
  constexpr uint32_t kSyntheticUcode[] = {0, 0, 0};
  MetalShader shader(xenos::ShaderType::kPixel, UINT64_C(0x45584143544F4D), kSyntheticUcode,
                     std::size(kSyntheticUcode), std::endian::native);
  constexpr uint64_t kModification = UINT64_C(0x0123456789ABCDEF);

  bool native_is_new = false;
  auto* native = static_cast<MetalShader::MetalTranslation*>(
      shader.GetOrCreateTranslation(kModification, &native_is_new));
  bool exact_is_new = false;
  MetalShader::MetalTranslation* exact =
      shader.GetOrCreateExactOutputMergerTranslation(kModification, &exact_is_new);
  REQUIRE(native);
  REQUIRE(exact);
  CHECK(native_is_new);
  CHECK(exact_is_new);
  CHECK(native != exact);
  CHECK(native->mode() == MetalShader::TranslationMode::kNativeAttachments);
  CHECK(exact->mode() == MetalShader::TranslationMode::kExactOutputMerger);
  CHECK(shader.GetTranslation(kModification) == native);
  CHECK(shader.GetExactOutputMergerTranslation(kModification) == exact);

  CHECK(shader.GetOrCreateExactOutputMergerTranslation(kModification, &exact_is_new) == exact);
  CHECK_FALSE(exact_is_new);
  shader.DestroyExactOutputMergerTranslation(kModification);
  CHECK(shader.GetTranslation(kModification) == native);
  CHECK(shader.GetExactOutputMergerTranslation(kModification) == nullptr);
}

TEST_CASE("Exact Metal translator keeps output-merger depth semantics in the fragment shader",
          "[graphics][metal][exact-om]") {
  std::unique_ptr<SpirvShaderTranslator> translator = CreateExactOutputMergerShaderTranslator();
  REQUIRE(translator);
  SpirvShaderTranslator::Modification pixel_modification(
      translator->GetDefaultPixelShaderModification(0));
  CHECK(pixel_modification.pixel.depth_stencil_mode ==
        SpirvShaderTranslator::Modification::DepthStencilMode::kNoModifiers);
}

TEST_CASE("Exact Metal resolve plan covers color samples endian swap and ordered clears",
          "[graphics][metal][exact-om][resolve]") {
  struct SampleCase {
    xenos::MsaaSamples msaa;
    xenos::CopySampleSelect sample;
  };
  constexpr std::array<SampleCase, 11> kSampleCases = {{
      {xenos::MsaaSamples::k1X, xenos::CopySampleSelect::k0},
      {xenos::MsaaSamples::k2X, xenos::CopySampleSelect::k0},
      {xenos::MsaaSamples::k2X, xenos::CopySampleSelect::k1},
      {xenos::MsaaSamples::k2X, xenos::CopySampleSelect::k01},
      {xenos::MsaaSamples::k4X, xenos::CopySampleSelect::k0},
      {xenos::MsaaSamples::k4X, xenos::CopySampleSelect::k1},
      {xenos::MsaaSamples::k4X, xenos::CopySampleSelect::k2},
      {xenos::MsaaSamples::k4X, xenos::CopySampleSelect::k3},
      {xenos::MsaaSamples::k4X, xenos::CopySampleSelect::k01},
      {xenos::MsaaSamples::k4X, xenos::CopySampleSelect::k23},
      {xenos::MsaaSamples::k4X, xenos::CopySampleSelect::k0123},
  }};

  for (const SampleCase& sample_case : kSampleCases) {
    for (uint32_t endian = 0; endian <= uint32_t(xenos::Endian128::k16in32); ++endian) {
      for (bool swap : {false, true}) {
        INFO("msaa=" << uint32_t(sample_case.msaa) << " sample=" << uint32_t(sample_case.sample)
                     << " endian=" << endian << " swap=" << swap);
        draw_util::ResolveInfo info = MakeColorResolveInfo(sample_case.msaa, sample_case.sample);
        info.copy_dest_info.copy_dest_endian = xenos::Endian128(endian);
        info.copy_dest_info.copy_dest_swap = swap;
        ExactOutputMergerResolvePlan plan;
        std::string error;
        REQUIRE(BuildExactOutputMergerResolvePlan(info, SharedMemory::kBufferSize, plan, &error));
        CHECK(error.empty());
        CHECK(plan.copy_width == 16);
        CHECK(plan.copy_height == 8);
        CHECK(plan.destination_start == info.copy_dest_extent_start);
        CHECK(plan.destination_length == info.copy_dest_extent_length);
        CHECK_FALSE(plan.copying_depth);
        CHECK_FALSE(plan.clear_color);
        CHECK_FALSE(plan.clear_depth);
        CHECK(plan.copy_constants.dest_relative.dest_info.value == info.copy_dest_info.value);
      }
    }
  }

  draw_util::ResolveInfo clear_info = MakeColorResolveInfo();
  clear_info.rb_copy_control.color_clear_enable = 1;
  clear_info.rb_copy_control.depth_clear_enable = 1;
  clear_info.rb_color_clear = UINT32_C(0x11223344);
  clear_info.rb_color_clear_lo = UINT32_C(0x55667788);
  clear_info.rb_depth_clear = UINT32_C(0xA1B2C3D4);
  clear_info.depth_edram_info.pitch_tiles =
      xenos::GetSurfacePitchTiles(160, clear_info.color_edram_info.msaa_samples, false);
  clear_info.depth_edram_info.msaa_samples = clear_info.color_edram_info.msaa_samples;
  clear_info.depth_edram_info.is_depth = 1;
  clear_info.depth_edram_info.base_tiles = 1000;
  clear_info.depth_edram_info.format = uint32_t(xenos::DepthRenderTargetFormat::kD24S8);
  ExactOutputMergerResolvePlan clear_plan;
  REQUIRE(BuildExactOutputMergerResolvePlan(clear_info, SharedMemory::kBufferSize, clear_plan));
  CHECK(clear_plan.clear_color);
  CHECK(clear_plan.clear_depth);
  CHECK(clear_plan.color_clear_constants.rt_specific.clear_value[0] == UINT32_C(0x11223344));
  CHECK(clear_plan.depth_clear_constants.rt_specific.clear_value[0] == UINT32_C(0xA1B2C3D4));
}

TEST_CASE("Exact Metal resolve plan preserves packed depth format and selected samples",
          "[graphics][metal][exact-om][resolve]") {
  constexpr std::array<xenos::DepthRenderTargetFormat, 2> kDepthFormats = {
      xenos::DepthRenderTargetFormat::kD24S8,
      xenos::DepthRenderTargetFormat::kD24FS8,
  };
  constexpr std::array<xenos::MsaaSamples, 3> kMsaa = {
      xenos::MsaaSamples::k1X,
      xenos::MsaaSamples::k2X,
      xenos::MsaaSamples::k4X,
  };
  for (xenos::DepthRenderTargetFormat format : kDepthFormats) {
    for (xenos::MsaaSamples msaa : kMsaa) {
      const xenos::CopySampleSelect selected =
          msaa == xenos::MsaaSamples::k4X   ? xenos::CopySampleSelect::k3
          : msaa == xenos::MsaaSamples::k2X ? xenos::CopySampleSelect::k1
                                            : xenos::CopySampleSelect::k0;
      draw_util::ResolveInfo info = MakeDepthResolveInfo(format, msaa, selected);
      ExactOutputMergerResolvePlan plan;
      std::string error;
      REQUIRE(BuildExactOutputMergerResolvePlan(info, SharedMemory::kBufferSize, plan, &error));
      CHECK(error.empty());
      CHECK(plan.copying_depth);
      CHECK(uint32_t(plan.copy_constants.dest_relative.dest_info.copy_dest_format) ==
            uint32_t(format == xenos::DepthRenderTargetFormat::kD24FS8
                         ? xenos::TextureFormat::k_24_8_FLOAT
                         : xenos::TextureFormat::k_24_8));
    }
  }

  for (xenos::ColorFormat wrong_format : {xenos::ColorFormat::k_8_8_8_8, xenos::ColorFormat(7)}) {
    draw_util::ResolveInfo info =
        MakeDepthResolveInfo(xenos::DepthRenderTargetFormat::kD24S8, xenos::MsaaSamples::k1X,
                             xenos::CopySampleSelect::k0);
    info.copy_dest_info.copy_dest_format = wrong_format;
    ExactOutputMergerResolvePlan plan;
    CHECK_FALSE(BuildExactOutputMergerResolvePlan(info, SharedMemory::kBufferSize, plan));
  }
}

TEST_CASE("Exact Metal resolve plan rejects atomically before any IssueCopy side effect",
          "[graphics][metal][exact-om][resolve]") {
  ExactOutputMergerResolvePlan plan;
  std::memset(&plan, 0xA5, sizeof(plan));
  const ExactOutputMergerResolvePlan sentinel = plan;
  auto rejects_without_mutation = [&](const draw_util::ResolveInfo& info,
                                      size_t destination_size = SharedMemory::kBufferSize) {
    std::string error;
    CHECK_FALSE(BuildExactOutputMergerResolvePlan(info, destination_size, plan, &error));
    CHECK_FALSE(error.empty());
    CHECK(std::memcmp(&plan, &sentinel, sizeof(plan)) == 0);
  };

  draw_util::ResolveInfo info = MakeColorResolveInfo();
  info.rb_copy_control.copy_command = xenos::CopyCommand::kRaw;
  rejects_without_mutation(info);

  info = MakeColorResolveInfo();
  info.copy_dest_info.copy_dest_format = xenos::ColorFormat::k_2_10_10_10;
  rejects_without_mutation(info);

  info = MakeColorResolveInfo();
  info.copy_dest_info.copy_dest_exp_bias = 1;
  rejects_without_mutation(info);

  info = MakeColorResolveInfo();
  info.copy_dest_info.copy_dest_array = 1;
  rejects_without_mutation(info);

  info = MakeColorResolveInfo();
  info.copy_dest_base = 1;
  RefreshResolveDestinationExtent(info);
  rejects_without_mutation(info);

  info = MakeColorResolveInfo();
  info.coordinate_info.draw_resolution_scale_x = 2;
  rejects_without_mutation(info);

  info = MakeColorResolveInfo();
  info.color_edram_info.msaa_samples = xenos::MsaaSamples(3);
  rejects_without_mutation(info);

  info = MakeColorResolveInfo();
  rejects_without_mutation(
      info, uint64_t(info.copy_dest_extent_start) + info.copy_dest_extent_length - 1);

  info = MakeColorResolveInfo();
  info.rb_copy_control.color_clear_enable = 1;
  info.color_edram_info.format = uint32_t(xenos::ColorRenderTargetFormat::k_16_16_16_16);
  info.color_edram_info.format_is_64bpp = 1;
  rejects_without_mutation(info);

  info = MakeDepthResolveInfo(xenos::DepthRenderTargetFormat::kD24S8, xenos::MsaaSamples::k1X,
                              xenos::CopySampleSelect::k0);
  info.copy_dest_info.copy_dest_swap = 1;
  rejects_without_mutation(info);

  info = MakeDepthResolveInfo(xenos::DepthRenderTargetFormat::kD24S8, xenos::MsaaSamples::k1X,
                              xenos::CopySampleSelect::k0);
  info.copy_dest_info.copy_dest_format = xenos::ColorFormat::k_8_8_8_8;
  rejects_without_mutation(info);

  info = MakeDepthResolveInfo(xenos::DepthRenderTargetFormat::kD24S8, xenos::MsaaSamples::k1X,
                              xenos::CopySampleSelect::k0);
  info.rb_copy_control.color_clear_enable = 1;
  rejects_without_mutation(info);
}

TEST_CASE("Exact Metal resolve plan proves intentional EDRAM tile wrap before dispatch",
          "[graphics][metal][exact-om][resolve]") {
  draw_util::ResolveInfo info =
      MakeColorResolveInfo(xenos::MsaaSamples::k4X, xenos::CopySampleSelect::k0123);
  info.color_edram_info.base_tiles = xenos::kEdramTileCount - 1;
  info.color_edram_info.pitch_tiles = (uint32_t(1) << xenos::kEdramPitchTilesBits) - 1;
  info.coordinate_info.edram_offset_x_div_8 = 15;
  info.coordinate_info.edram_offset_y_div_8 = 1;

  ExactOutputMergerResolvePlan plan;
  std::string error;
  const bool built =
      BuildExactOutputMergerResolvePlan(info, SharedMemory::kBufferSize, plan, &error);
  INFO(error);
  REQUIRE(built);
  CHECK(plan.copy_width == 16);
  CHECK(plan.copy_height == 8);
}

TEST_CASE("Exact output-merger execution permits fallback only before submission",
          "[graphics][metal][exact-om]") {
  CHECK(
      GetExactOutputMergerRouteDecision(ExactOutputMergerExecutionResult::kRejectedBeforeSubmit) ==
      ExactOutputMergerRouteDecision::kContinueNative);
  CHECK(GetExactOutputMergerRouteDecision(ExactOutputMergerExecutionResult::kEnqueued) ==
        ExactOutputMergerRouteDecision::kReturnSuccess);
  CHECK(GetExactOutputMergerRouteDecision(ExactOutputMergerExecutionResult::kCompleted) ==
        ExactOutputMergerRouteDecision::kReturnSuccess);
  CHECK(GetExactOutputMergerRouteDecision(ExactOutputMergerExecutionResult::kFailedAfterSubmit) ==
        ExactOutputMergerRouteDecision::kReturnFailure);
}

}  // namespace rex::graphics::metal
