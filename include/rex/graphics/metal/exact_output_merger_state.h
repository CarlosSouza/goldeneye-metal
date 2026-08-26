#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <rex/graphics/metal/exact_output_merger.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>

namespace rex::graphics {
class RegisterFile;
}

namespace rex::graphics::metal {

// Complete guest output-merger inputs for the exact Metal path. This is kept
// independent of Metal objects so register extraction and policy can be tested
// before the path is selectable by IssueDraw.
struct ExactOutputMergerColorTargetInput {
  uint32_t base_tiles = 0;
  xenos::ColorRenderTargetFormat format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
  int32_t exponent_bias = 0;
  uint8_t write_mask = 0;
  // RB_BLENDCONTROL with reserved bits ignored by the exact shader.
  uint32_t blend_factors_ops = 0x00010001;
};

struct ExactOutputMergerDepthStencilInput {
  uint32_t base_tiles = 0;
  xenos::DepthRenderTargetFormat format = xenos::DepthRenderTargetFormat::kD24S8;
  // Must already be normalized with draw_util::GetNormalizedDepthControl.
  reg::RB_DEPTHCONTROL control = {};
  uint32_t stencil_front_reference_masks = 0;
  uint32_t stencil_back_reference_masks = 0;
};

struct ExactOutputMergerDrawStateInput {
  // The base constants contain vertex, viewport, texture and index state. The
  // builder replaces every output-merger-owned field deterministically.
  SpirvShaderTranslator::SystemConstants base_system_constants = {};

  uint32_t surface_pitch_pixels = 0;
  uint32_t coverage_width = 0;
  uint32_t coverage_height = 0;
  xenos::MsaaSamples msaa_samples = xenos::MsaaSamples::k1X;

  std::array<ExactOutputMergerColorTargetInput, xenos::kMaxColorRenderTargets> color_targets = {};
  ExactOutputMergerDepthStencilInput depth_stencil;

  xenos::CompareFunction alpha_test_function = xenos::CompareFunction::kAlways;
  float alpha_test_reference = 0.0f;
  bool alpha_to_mask_enabled = false;
  uint8_t alpha_to_mask_dither = 0;
  bool convert_gamma_targets = true;

  bool primitive_polygonal = true;
  bool polygon_dual_mode = false;
  bool polygon_offset_front_enabled = false;
  bool polygon_offset_back_enabled = false;
  bool polygon_offset_parameter_enabled = false;
  float polygon_offset_front_scale = 0.0f;
  float polygon_offset_front_offset = 0.0f;
  float polygon_offset_back_scale = 0.0f;
  float polygon_offset_back_offset = 0.0f;

  std::array<float, 4> blend_constant = {};

  // The first shared primitive is deliberately fail-closed for shader-side
  // effects that would make pre-submit fallback or replay unsafe.
  bool vertex_shader_memexport = false;
  bool pixel_shader_memexport = false;
  bool reuses_vertex_position = false;
};

struct ExactOutputMergerDrawState {
  SpirvShaderTranslator::SystemConstants system_constants = {};
  std::array<ExactOutputMergerSurfaceConstants, xenos::kMaxColorRenderTargets> color_surfaces = {};
  ExactOutputMergerSurfaceConstants depth_surface = {};
  uint8_t active_color_target_mask = 0;
  bool depth_stencil_enabled = false;
};

// Builds all Xenos output-merger constants into a temporary candidate and
// publishes it only on success. On failure, state_out is byte-for-byte
// unchanged and no resource authority is modified.
bool BuildExactOutputMergerDrawState(const ExactOutputMergerDrawStateInput& input,
                                     ExactOutputMergerDrawState& state_out,
                                     std::string* error_out = nullptr);

// Captures the complete live register-owned output-merger state and then uses
// the same transactional builder above. Shader and primitive facts remain
// explicit so a caller cannot accidentally hide memexport or position reuse.
bool BuildExactOutputMergerDrawStateFromRegisters(
    const RegisterFile& register_file,
    const SpirvShaderTranslator::SystemConstants& base_system_constants, uint32_t coverage_width,
    uint32_t coverage_height, uint32_t shader_color_target_mask, bool primitive_polygonal,
    bool vertex_shader_memexport, bool pixel_shader_memexport, bool reuses_vertex_position,
    ExactOutputMergerDrawState& state_out, std::string* error_out = nullptr);

// Fully validated, immutable description of the first GPU-native IssueCopy
// consumer of exact EDRAM. The plan contains every EDRAM side effect in guest
// order: resolve to shared memory first, then optional depth and color clears.
// It deliberately contains no Metal objects or mutable EDRAM handle.
struct ExactOutputMergerResolvePlan {
  draw_util::ResolveCopyShaderConstants copy_constants = {};
  draw_util::ResolveClearShaderConstants color_clear_constants = {};
  draw_util::ResolveClearShaderConstants depth_clear_constants = {};
  uint32_t copy_width = 0;
  uint32_t copy_height = 0;
  uint32_t destination_start = 0;
  uint32_t destination_length = 0;
  bool copying_depth = false;
  bool clear_color = false;
  bool clear_depth = false;
};

// Accepts only the exact subset implemented by the controlled Metal consumer:
// scale-1 8888 color or packed D24S8/D24FS8 resolves into a 32bpp tiled
// destination, with 1x/2x/4x sample selection and 32-bit EDRAM clears. If any
// requested copy or clear side effect is unsupported, the entire plan is
// rejected and plan_out is unchanged so IssueCopy can materialize and use its
// existing path without having partially applied the packet.
bool BuildExactOutputMergerResolvePlan(const draw_util::ResolveInfo& resolve_info,
                                       size_t destination_buffer_size,
                                       ExactOutputMergerResolvePlan& plan_out,
                                       std::string* error_out = nullptr);

// Shared constant payloads must be supplied only when at least one translated
// stage declares the corresponding Metal binding. The executor deliberately
// rejects extra payloads so stale host data can't hide a reflection mismatch.
bool ExactOutputMergerStageUsesFloatConstants(const ExactOutputMergerStageLayout& layout);
bool ExactOutputMergerPipelineUsesFetchConstants(const ExactOutputMergerPipelineLayout& layout);
bool ExactOutputMergerPipelineUsesBoolLoopConstants(const ExactOutputMergerPipelineLayout& layout);

enum class ExactOutputMergerResourceAuthority : uint8_t {
  kInvalid,
  kCpu,
  kGpu,
};

struct ExactOutputMergerAuthoritySnapshot {
  ExactOutputMergerResourceAuthority authority = ExactOutputMergerResourceAuthority::kInvalid;
  uint64_t submitted_sequence = 0;
  uint64_t completed_sequence = 0;
  uint64_t in_flight_sequence = 0;
};

// Small synchronization state machine shared by the Objective-C resource
// owner and pure tests. One sequence may own a bounded asynchronous epoch of
// command buffers. CPU access remains forbidden until the complete epoch is
// drained; later exact batches may append only with the matching sequence.
class ExactOutputMergerAuthorityTracker {
 public:
  const ExactOutputMergerAuthoritySnapshot& snapshot() const { return snapshot_; }

  bool MarkCpuUpload();
  bool BeginGpuSubmission(uint64_t& sequence_out);
  bool CanContinueGpuSubmission(uint64_t sequence) const;
  bool CancelGpuSubmission(uint64_t sequence);
  bool CompleteGpuSubmission(uint64_t sequence, bool succeeded);
  bool CanCpuAccess() const;
  bool CanEncodeGpuWork() const;
  void Invalidate();

 private:
  ExactOutputMergerAuthoritySnapshot snapshot_;
};

enum class ExactOutputMergerResidency : uint8_t {
  kCanonical,
  kExactGpu,
  kInvalid,
};

// Tracks which complete 10 MiB image is current at command-processor level.
// Consecutive exact draws may remain GPU-authoritative, but every native,
// resolve, trace or presentation boundary must materialize first.
class ExactOutputMergerResidencyTracker {
 public:
  ExactOutputMergerResidency residency() const { return residency_; }
  bool exact_gpu_current() const { return residency_ == ExactOutputMergerResidency::kExactGpu; }
  bool invalid() const { return residency_ == ExactOutputMergerResidency::kInvalid; }

  bool RecordExactDraw();
  bool RecordMaterialized();
  void RecordCanonicalReplacement();
  void Invalidate();

 private:
  ExactOutputMergerResidency residency_ = ExactOutputMergerResidency::kCanonical;
};

}  // namespace rex::graphics::metal
