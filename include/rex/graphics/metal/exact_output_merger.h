#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <string>

#include <rex/graphics/metal/edram_snapshot.h>
#include <rex/graphics/metal/msl_compiler.h>
#include <rex/graphics/metal/shader.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::metal {

// Exact Metal output-merger building blocks used by the default-off production
// route and its validation probes. The resource owns one physical, wrapping
// Xenos EDRAM image, so aliased surfaces share the same 10 MiB allocation.
constexpr size_t kExactOutputMergerEdramSizeBytes = xenos::kEdramSizeBytes;
constexpr size_t kExactOutputMergerEdramDwordCount =
    kExactOutputMergerEdramSizeBytes / sizeof(uint32_t);
constexpr uint32_t kExactOutputMergerMaxMetalBufferIndex = 30;
constexpr uint32_t kExactOutputMergerMaxTextureCount = 128;
constexpr uint32_t kExactOutputMergerMaxSamplerCount = 16;
constexpr uint32_t kExactOutputMergerDrawsPerCommandBuffer = 128;
constexpr uint32_t kExactOutputMergerCommandBufferCap = 4;

enum class ExactOutputMergerSurfaceKind : uint8_t {
  kColor,
  kDepthStencil,
};

// Checked address/layout constants shared by the CPU validation probe and the
// existing SpirvShaderTranslator fragment-interlock system constants. This is
// intentionally only the address and format subset; blend, write-mask,
// depth/stencil and gamma state must be populated separately before a future
// production route is allowed to use it.
struct ExactOutputMergerSurfaceConstants {
  ExactOutputMergerSurfaceKind kind = ExactOutputMergerSurfaceKind::kColor;
  CanonicalEdramSurfaceLayout canonical_layout;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t surface_pitch_pixels = 0;
  uint32_t sample_count = 0;
  uint32_t words_per_sample = 0;
  uint32_t tile_dwords = 0;
  uint32_t tile_pitch_dwords = 0;
  uint32_t base_dwords = 0;
  xenos::ColorRenderTargetFormat color_format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
  xenos::DepthRenderTargetFormat depth_format = xenos::DepthRenderTargetFormat::kD24S8;
  uint32_t color_format_flags = 0;
};

bool BuildExactOutputMergerColorConstants(uint32_t base_tiles, uint32_t surface_pitch_pixels,
                                          uint32_t width, uint32_t height,
                                          xenos::MsaaSamples msaa_samples,
                                          xenos::ColorRenderTargetFormat format,
                                          ExactOutputMergerSurfaceConstants& constants_out,
                                          std::string* error_out);

bool BuildExactOutputMergerDepthConstants(uint32_t base_tiles, uint32_t surface_pitch_pixels,
                                          uint32_t width, uint32_t height,
                                          xenos::MsaaSamples msaa_samples,
                                          xenos::DepthRenderTargetFormat format,
                                          ExactOutputMergerSurfaceConstants& constants_out,
                                          std::string* error_out);

// Returns false for invalid pixel/sample/component coordinates. Successful
// addresses are always inside the one physical 10 MiB allocation, including
// layouts whose base or later rows wrap through tile 2047 to tile 0.
bool GetExactOutputMergerDwordIndex(const ExactOutputMergerSurfaceConstants& constants, uint32_t x,
                                    uint32_t y, uint32_t sample, uint32_t dword, size_t& index_out);

// Applies only the sample-count, pitch, base and format fields proven by this
// scaffold. It deliberately doesn't manufacture output-merger policy state.
bool ApplyExactOutputMergerAddressConstants(
    const ExactOutputMergerSurfaceConstants& surface, uint32_t color_target,
    SpirvShaderTranslator::SystemConstants& system_constants, std::string* error_out);

// Parses the translated SPIRV-Cross declaration and requires a coherent
// read-write xe_edram binding in raster_order_group(0). This fails closed on
// duplicate, malformed, out-of-range or non-ROG declarations.
bool FindExactOutputMergerEdramBinding(const std::string& msl_source, uint32_t& binding_out,
                                       std::string* error_out);

// Opaque Objective-C Metal resource owner. The canonical buffer is exactly
// 10 MiB and is never exposed directly. CPU observability is limited to
// authority-checked, whole-image upload/download operations so no persistent
// mutable pointer or arbitrary encoder can bypass submission ownership.
void* CreateExactOutputMergerResources(void* metal_device, std::string* error_out);
void ReleaseExactOutputMergerResources(void* resources);
// The exact primitive must use the same queue as shared-memory uploads and all
// later resolves. The first queue identity is permanently pinned; replacing it
// (including with another queue from the same device) fails closed.
bool SetExactOutputMergerCommandQueue(void* resources, void* command_queue, std::string* error_out);
bool UploadExactOutputMergerEdram(void* resources, const void* source, size_t source_size,
                                  std::string* error_out);
bool DownloadExactOutputMergerEdram(void* resources, void* destination, size_t destination_size,
                                    std::string* error_out);
void InvalidateExactOutputMergerResources(void* resources);
size_t GetExactOutputMergerEdramBufferLength(void* resources);

// Configures the only conventional attachment used by the exact path: a
// throwaway BGRA8 target that supplies raster coverage while every guest color
// and depth/stencil operation is performed through the canonical buffer.
bool ConfigureExactOutputMergerCoveragePipeline(void* pipeline_descriptor, uint32_t sample_count,
                                                std::string* error_out);
bool ConfigureExactOutputMergerCoveragePass(void* resources, void* render_pass_descriptor,
                                            uint32_t width, uint32_t height, uint32_t sample_count,
                                            std::string* error_out);

// Opaque exact-only pipeline identity. Draw submission never accepts an
// arbitrary Metal pipeline or encoder.
class ExactOutputMergerPipeline;

// Complete translated shader resource layout pinned into an exact pipeline.
// UINT32_MAX means the shader stage does not expose that buffer. Texture and
// sampler bindings must be dense from zero, matching the existing translated
// guest draw contract.
struct ExactOutputMergerStageLayout {
  uint32_t system_constants_buffer_index = UINT32_MAX;
  uint32_t shared_memory_buffer_index = UINT32_MAX;
  uint32_t float_constants_buffer_index = UINT32_MAX;
  uint32_t fetch_constants_buffer_index = UINT32_MAX;
  uint32_t bool_loop_constants_buffer_index = UINT32_MAX;
  uint32_t vertex_data_buffer_index = UINT32_MAX;
  uint32_t texture_count = 0;
  uint32_t sampler_count = 0;

  bool operator==(const ExactOutputMergerStageLayout&) const = default;
};

struct ExactOutputMergerPipelineLayout {
  ExactOutputMergerStageLayout vertex;
  ExactOutputMergerStageLayout fragment;
  uint32_t edram_fragment_buffer_index = UINT32_MAX;

  bool operator==(const ExactOutputMergerPipelineLayout&) const = default;
};

// Production creation only accepts current exact-mode translations. The MSL
// source, compiled library generation, Metal device, sole main0 entry points,
// coherent raster-order EDRAM declaration and reflected resource layout are
// all attested together before the pipeline is returned.
ExactOutputMergerPipeline* CreateExactOutputMergerPipelineFromTranslations(
    void* resources, MetalShader::MetalTranslation& vertex_translation,
    MetalShader::MetalTranslation& fragment_translation, uint32_t sample_count,
    std::string* error_out);

bool GetExactOutputMergerPipelineLayout(const ExactOutputMergerPipeline* pipeline,
                                        ExactOutputMergerPipelineLayout& layout_out);
void ReleaseExactOutputMergerPipeline(ExactOutputMergerPipeline* pipeline);

struct ExactOutputMergerInlineBuffer {
  const void* data = nullptr;
  size_t size = 0;
  uint32_t index = UINT32_MAX;
};

// Shared synchronous coverage primitive for exact translated fragment
// shaders. It is intentionally not connected to IssueDraw and supports no
// memexport/reuse or guest resource bindings yet. Validation and encoding are
// completed before the command buffer is submitted; after submission, a Metal
// failure invalidates EDRAM authority and is never replayed through another
// path.
struct ExactOutputMergerUnroutableDraw {
  ExactOutputMergerPipeline* pipeline = nullptr;
  uint32_t coverage_width = 0;
  uint32_t coverage_height = 0;
  uint32_t sample_count = 0;
  std::array<ExactOutputMergerInlineBuffer, 2> fragment_inline_buffers = {};
  uint32_t fragment_inline_buffer_count = 0;
  uint32_t vertex_start = 0;
  uint32_t vertex_count = 0;
  uint32_t instance_count = 1;
};

// Only a rejection that occurs before Metal submission may be followed by a
// different rendering path. Once a command buffer is committed, failure is
// terminal for that guest draw because the shader may already have modified
// canonical EDRAM or other guest-visible resources.
enum class ExactOutputMergerExecutionResult : uint8_t {
  kRejectedBeforeSubmit,
  // The draw crossed the exact-route acceptance boundary and is owned by a
  // bounded asynchronous Metal batch. A later exact wait or CPU EDRAM access
  // reports command-buffer failure; this draw must never be replayed.
  kEnqueued,
  kCompleted,
  kFailedAfterSubmit,
};

enum class ExactOutputMergerRouteDecision : uint8_t {
  kContinueNative,
  kReturnSuccess,
  kReturnFailure,
};

// This table is deliberately pure so IssueDraw can enforce the at-most-once
// boundary without duplicating submission-state reasoning.
constexpr ExactOutputMergerRouteDecision GetExactOutputMergerRouteDecision(
    ExactOutputMergerExecutionResult result) {
  switch (result) {
    case ExactOutputMergerExecutionResult::kRejectedBeforeSubmit:
      return ExactOutputMergerRouteDecision::kContinueNative;
    case ExactOutputMergerExecutionResult::kEnqueued:
    case ExactOutputMergerExecutionResult::kCompleted:
      return ExactOutputMergerRouteDecision::kReturnSuccess;
    case ExactOutputMergerExecutionResult::kFailedAfterSubmit:
    default:
      return ExactOutputMergerRouteDecision::kReturnFailure;
  }
}

ExactOutputMergerExecutionResult ExecuteExactOutputMergerUnroutableDraw(
    void* resources, const ExactOutputMergerUnroutableDraw& draw, std::string* error_out);

// CPU-backed textures include their declared byte span so all row, image and
// array accesses can be checked and copied before submission. A slot contains
// either CPU RGBA8 data or a borrowed Metal texture, never both.
struct ExactOutputMergerTextureSlot {
  const uint8_t* rgba = nullptr;
  size_t rgba_size = 0;
  void* metal_texture = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t array_length = 1;
  size_t bytes_per_row = 0;
  size_t bytes_per_image = 0;
};

// Full borrowed guest draw. Metal objects and pointer spans are borrowed only
// until ExecuteExactOutputMergerDraw returns. All CPU data is copied to bounded
// per-command-buffer arenas before kEnqueued is returned. Shared memory must be
// a resident Metal buffer so the executor never aliases an unbounded guest CPU
// mapping.
struct ExactOutputMergerBorrowedDraw {
  ExactOutputMergerPipeline* pipeline = nullptr;
  ExactOutputMergerPipelineLayout pipeline_layout;
  uint32_t coverage_width = 0;
  uint32_t coverage_height = 0;
  uint32_t sample_count = 0;

  const void* system_constants = nullptr;
  size_t system_constants_size = 0;
  const void* vertex_float_constants = nullptr;
  size_t vertex_float_constants_size = 0;
  const void* fragment_float_constants = nullptr;
  size_t fragment_float_constants_size = 0;
  const void* fetch_constants = nullptr;
  size_t fetch_constants_size = 0;
  const void* bool_loop_constants = nullptr;
  size_t bool_loop_constants_size = 0;
  void* shared_memory_metal_buffer = nullptr;
  size_t shared_memory_size = 0;

  const ExactOutputMergerTextureSlot* vertex_textures = nullptr;
  size_t vertex_texture_count = 0;
  const ProbeSamplerSlot* vertex_samplers = nullptr;
  size_t vertex_sampler_count = 0;
  const ExactOutputMergerTextureSlot* fragment_textures = nullptr;
  size_t fragment_texture_count = 0;
  const ProbeSamplerSlot* fragment_samplers = nullptr;
  size_t fragment_sampler_count = 0;

  const void* vertex_data = nullptr;
  size_t vertex_data_size = 0;
  size_t vertex_data_stride = 0;
  const ProbeIndexBuffer* index_buffer = nullptr;
  const ProbeRasterizationState* rasterization_state = nullptr;
  // This must be PrimitiveProcessor::ProcessingResult::host_primitive_type,
  // never an unprocessed guest topology such as a fan, loop, quad or polygon.
  uint32_t primitive_type = uint32_t(xenos::PrimitiveType::kTriangleList);
  // Metal uses the fixed all-ones restart index for strip topologies. The
  // primitive processor must already have normalized any guest restart value.
  bool primitive_restart_enabled = false;
  uint32_t primitive_restart_index = UINT32_MAX;
  uint32_t vertex_start = 0;
  uint32_t vertex_count = 0;
  uint32_t instance_count = 1;
  int32_t base_vertex = 0;
  uint32_t base_instance = 0;
};

ExactOutputMergerExecutionResult ExecuteExactOutputMergerDraw(
    void* resources, const ExactOutputMergerBorrowedDraw& draw, std::string* error_out);

// Commits the current exact draw batch on the pinned queue without waiting.
// This is the ordering edge required before a later same-queue shared-memory
// upload. Wait is the corresponding host mutation / materialization fence.
bool FinalizeExactOutputMergerDraws(void* resources, std::string* error_out);
bool WaitExactOutputMergerDraws(void* resources, std::string* error_out,
                                uint32_t* waited_draw_count_out = nullptr);
uint32_t GetExactOutputMergerPendingDrawCount(void* resources);
bool GetExactOutputMergerUploadStats(void* resources, PipelineProbeUploadStats* stats_out);
bool GetExactOutputMergerSubmissionStats(void* resources, PipelineProbeSubmissionStats* stats_out);

struct ExactOutputMergerResolvePlan;

// Caller-owned shared-memory destination for a validated resolve plan. The
// exact EDRAM buffer remains private to resources. A guest-memory alias is
// mandatory for this asynchronous path so the resident resolve and its guest
// publication are ordered in the same Metal command buffer.
struct ExactOutputMergerResolveDestination {
  void* metal_buffer = nullptr;
  size_t metal_buffer_size = 0;
  void* guest_memory_metal_buffer = nullptr;
  size_t guest_memory_metal_buffer_size = 0;
  void (*submission_callback)(void* context, uint32_t start, uint32_t length) = nullptr;
  void* submission_callback_context = nullptr;
  void (*async_failure_callback)(void* context, uint32_t start, uint32_t length) = nullptr;
  void* async_failure_callback_context = nullptr;
};

// Enqueues one all-or-nothing same-queue consumer in guest order: resolve,
// resident-to-guest publication, depth clear, color clear. Rejection before
// commit may fall back to canonical IssueCopy. Once committed, the packet is
// owned by this path and must never be replayed; deferred failure invalidates
// exact EDRAM authority and invokes async_failure_callback exactly once.
ExactOutputMergerExecutionResult ExecuteExactOutputMergerResolveAndClear(
    void* resources, const ExactOutputMergerResolvePlan& plan,
    const ExactOutputMergerResolveDestination& destination, std::string* error_out);

}  // namespace rex::graphics::metal
