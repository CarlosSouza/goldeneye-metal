#import <Metal/Metal.h>

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include <rex/graphics/metal/msl_compiler.h>

namespace {

using namespace rex::graphics;
using namespace rex::graphics::metal;

constexpr uint32_t kWidth = 24;
constexpr uint32_t kHeight = 20;

constexpr char kVertexMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VertexOutput { float4 position [[position]]; };
vertex VertexOutput main0(uint vertex_id [[vertex_id]]) {
  constexpr float2 positions[] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
  return {float4(positions[vertex_id], 0.0, 1.0)};
}
)MSL";

constexpr char kRt01FragmentMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Output { float4 rt0 [[color(0)]]; float4 rt1 [[color(1)]]; };
fragment Output main0() { return {float4(1, 0, 0, 1), float4(0, 1, 0, 1)}; }
)MSL";

constexpr char kRt1FragmentMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Output { float4 rt1 [[color(1)]]; };
fragment Output main0() { return {float4(0, 0, 1, 1)}; }
)MSL";

constexpr char kRt02FragmentMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Output { float4 rt0 [[color(0)]]; float4 rt2 [[color(2)]]; };
fragment Output main0() { return {float4(1, 0, 0, 1), float4(0, 0, 1, 1)}; }
)MSL";

constexpr char kRt0123FragmentMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Output {
  float4 rt0 [[color(0)]]; float4 rt1 [[color(1)]];
  float4 rt2 [[color(2)]]; float4 rt3 [[color(3)]];
};
fragment Output main0() {
  return {float4(1, 0, 0, 1), float4(0, 1, 0, 1),
          float4(0, 0, 1, 1), float4(1, 1, 0, 1)};
}
)MSL";

constexpr char kMaskBlendFragmentMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Output { float4 rt0 [[color(0)]]; float4 rt1 [[color(1)]]; };
fragment Output main0() { return {float4(0.9, 0.8, 0.7, 0.6), float4(1, 0, 0, 0.5)}; }
)MSL";

constexpr char kDepthMrtFragmentMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Output {
  float4 rt0 [[color(0)]]; float4 rt1 [[color(1)]]; float depth [[depth(any)]];
};
fragment Output main0() {
  return {float4(1, 0, 0, 1), float4(0, 1, 0, 1), 0.25};
}
)MSL";

constexpr char kFarDepthFragmentMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Output { float4 rt0 [[color(0)]]; float depth [[depth(any)]]; };
fragment Output main0() { return {float4(0, 1, 0, 1), 0.75}; }
)MSL";

constexpr char kBlueFragmentMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
fragment float4 main0() { return float4(0, 0, 1, 1); }
)MSL";

constexpr char kGeneralVertexMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VertexOutput { float4 position [[position]]; };
vertex VertexOutput main0(uint vertex_id [[vertex_id]], constant float4& offset [[buffer(0)]],
                          device const float2* positions [[buffer(3)]],
                          device atomic_uint* side_effects [[buffer(5)]]) {
  if (vertex_id == 0) {
    atomic_fetch_add_explicit(&side_effects[0], 1u, memory_order_relaxed);
  }
  return {float4(positions[vertex_id] + offset.xy, 0.0, 1.0)};
}
)MSL";

constexpr char kGeneralFragmentMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Output { float4 rt0 [[color(0)]]; float4 rt1 [[color(1)]]; };
fragment Output main0(float4 position [[position]], constant float4& tint [[buffer(4)]],
                      texture2d_array<float> source [[texture(0)]],
                      sampler source_sampler [[sampler(0)]],
                      device atomic_uint* side_effects [[buffer(6)]]) {
  if (uint(position.x) == 0u && uint(position.y) == 0u) {
    atomic_fetch_add_explicit(&side_effects[1], 1u, memory_order_relaxed);
  }
  return {source.sample(source_sampler, float2(0.5), 0), tint};
}
)MSL";

bool PixelNear(const std::vector<uint8_t>& bgra, const std::array<uint8_t, 4>& expected,
               uint8_t tolerance = 2) {
  if (bgra.size() < 4) {
    return false;
  }
  size_t offset = (size_t(kHeight / 2) * kWidth + kWidth / 2) * 4;
  for (uint32_t component = 0; component < 4; ++component) {
    int difference = int(bgra[offset + component]) - int(expected[component]);
    if (difference < -int(tolerance) || difference > int(tolerance)) {
      return false;
    }
  }
  return true;
}

bool Check(bool condition, const char* message, const std::string& detail = {}) {
  if (condition) {
    return true;
  }
  std::fprintf(stderr, "[metal_mrt_probe_test] FAIL: %s%s%s\n", message,
               detail.empty() ? "" : ": ", detail.c_str());
  return false;
}

ProbeRenderPipelineDescription MakeDescription(
    uint8_t mask,
    const std::array<xenos::ColorRenderTargetFormat, xenos::kMaxColorRenderTargets>& formats = {}) {
  ProbeRenderPipelineDescription description;
  description.output_mask = mask;
  for (uint32_t slot = 0; slot < xenos::kMaxColorRenderTargets; ++slot) {
    xenos::ColorRenderTargetFormat format =
        formats[slot] == xenos::ColorRenderTargetFormat(0)
            ? xenos::ColorRenderTargetFormat::k_8_8_8_8
            : formats[slot];
    description.color_targets[slot].color_format = format;
    description.color_targets[slot].metal_storage =
        GetColorTargetStorageStrategy(format).kind;
  }
  return description;
}

struct ContextSet {
  id<MTLCommandQueue> queue = nil;
  std::array<void*, xenos::kMaxColorRenderTargets> contexts = {};

  ~ContextSet() {
    for (void* context : contexts) {
      ReleasePipelineProbeContext(context);
    }
    if (queue) {
      [queue release];
    }
  }
};

bool CreateContexts(id<MTLDevice> device, const ProbeRenderPipelineDescription& description,
                    uint32_t sample_count, ContextSet& set, std::string& error) {
  set.queue = [device newCommandQueue];
  if (!set.queue) {
    error = "failed to create shared MRT queue";
    return false;
  }
  void* depth_owner = nullptr;
  for (uint32_t slot = 0; slot < xenos::kMaxColorRenderTargets; ++slot) {
    if (!(description.output_mask & (uint32_t(1) << slot))) {
      continue;
    }
    void* context = CreatePipelineProbeContext(device, set.queue, &error);
    if (!context ||
        !SetPipelineProbeContextColorFormat(context,
                                            description.color_targets[slot].color_format, &error) ||
        !SetPipelineProbeContextSampleCount(context, sample_count, &error)) {
      ReleasePipelineProbeContext(context);
      return false;
    }
    set.contexts[slot] = context;
    if (!depth_owner) {
      depth_owner = context;
    } else if (!SharePipelineProbeDepthStencilTarget(context, depth_owner, &error)) {
      return false;
    }
  }
  return true;
}

bool RunRoutingCase(id<MTLDevice> device, void* vertex_library, const char* fragment_source,
                    const ProbeRenderPipelineDescription& description, uint32_t sample_count,
                    const std::array<std::array<uint8_t, 4>, xenos::kMaxColorRenderTargets>&
                        expected) {
  std::string error;
  void* fragment_library = CreateMslLibrary(device, fragment_source, &error);
  void* pipeline = fragment_library
                       ? CreateRenderPipelineState(device, vertex_library, fragment_library, &error,
                                                   &description, nullptr, nullptr, sample_count)
                       : nullptr;
  ReleaseMslLibrary(fragment_library);
  ContextSet set;
  bool created = pipeline && CreateContexts(device, description, sample_count, set, error);
  PipelineProbeMrtTelemetry telemetry;
  bool rendered =
      created && RenderPipelineProbeMrtTriangleToContexts(
                     set.contexts, description, pipeline, kWidth, kHeight, nullptr, &telemetry,
                     &error);
  bool ok = Check(rendered, "MRT routing draw", error) &&
            Check(telemetry.bound_color_target_count ==
                      uint32_t(std::popcount(uint32_t(description.output_mask))),
                  "MRT bound-target telemetry") &&
            Check(telemetry.encoded_draw_count == 1 && telemetry.depth_attachment_bind_count == 1,
                  "MRT one-draw/one-depth telemetry");
  for (uint32_t slot = 0; slot < xenos::kMaxColorRenderTargets && ok; ++slot) {
    if (!(description.output_mask & (uint32_t(1) << slot))) {
      continue;
    }
    std::vector<uint8_t> bgra;
    ok = Check(GetPipelineProbeContextPendingSubmissionCount(set.contexts[slot]) == 0,
               "synchronous MRT left pending work") &&
         Check(ReadPipelineProbeContext(set.contexts[slot], kWidth, kHeight, bgra, &error),
               "MRT readback", error) &&
         Check(PixelNear(bgra, expected[slot]), "MRT output routed to wrong attachment");
  }
  ReleaseRenderPipelineState(pipeline);
  return ok;
}

bool TestDescriptorKeys() {
  ProbeRenderPipelineDescription base = MakeDescription(0x3);
  std::unordered_set<uint64_t> keys;
  keys.insert(GetProbeRenderPipelineDescriptionKey(base, 1));
  auto changed = base;
  changed.output_mask = 0x5;
  keys.insert(GetProbeRenderPipelineDescriptionKey(changed, 1));
  changed = base;
  changed.color_targets[1].write_mask = 0x5;
  keys.insert(GetProbeRenderPipelineDescriptionKey(changed, 1));
  changed = base;
  changed.color_targets[1].blend_control = 0x07060706;
  keys.insert(GetProbeRenderPipelineDescriptionKey(changed, 1));
  changed = base;
  changed.color_targets[1].color_format = xenos::ColorRenderTargetFormat::k_2_10_10_10;
  changed.color_targets[1].metal_storage = ColorTargetStorageKind::kRgb10A2Unorm;
  keys.insert(GetProbeRenderPipelineDescriptionKey(changed, 1));
  changed = base;
  changed.color_targets[1].metal_storage = ColorTargetStorageKind::kRgb10A2Unorm;
  keys.insert(GetProbeRenderPipelineDescriptionKey(changed, 1));
  keys.insert(GetProbeRenderPipelineDescriptionKey(base, 4));
  return Check(keys.size() == 7, "MRT pipeline description key aliased explicit fields");
}

bool TestMaskBlendAndMixedFormat(id<MTLDevice> device, void* vertex_library) {
  std::string error;
  ProbeRenderPipelineDescription description = MakeDescription(0x3);
  description.color_targets[0].write_mask = 0x1;
  description.color_targets[1].blend_control = 0x07060706;
  void* fragment_library = CreateMslLibrary(device, kMaskBlendFragmentMsl, &error);
  void* pipeline = fragment_library
                       ? CreateRenderPipelineState(device, vertex_library, fragment_library, &error,
                                                   &description)
                       : nullptr;
  ReleaseMslLibrary(fragment_library);
  ContextSet set;
  bool ready = pipeline && CreateContexts(device, description, 1, set, error);
  ready = ready && ClearPipelineProbeContext(set.contexts[0], kWidth, kHeight, 0.1, 0.2, 0.3,
                                             0.4, &error) &&
          ClearPipelineProbeContext(set.contexts[1], kWidth, kHeight, 0.0, 0.0, 1.0, 1.0,
                                    &error);
  bool rendered = ready && RenderPipelineProbeMrtTriangleToContexts(
                               set.contexts, description, pipeline, kWidth, kHeight, nullptr,
                               nullptr, &error);
  std::vector<uint8_t> rt0;
  std::vector<uint8_t> rt1;
  bool ok = Check(rendered, "per-attachment mask/blend draw", error) &&
            Check(ReadPipelineProbeContext(set.contexts[0], kWidth, kHeight, rt0, &error) &&
                      ReadPipelineProbeContext(set.contexts[1], kWidth, kHeight, rt1, &error),
                  "per-attachment mask/blend readback", error) &&
            Check(PixelNear(rt0, {77, 51, 230, 102}), "RT0 write mask was not independent") &&
            Check(PixelNear(rt1, {128, 0, 128, 191}, 3), "RT1 blend state was not independent");
  ReleaseRenderPipelineState(pipeline);

  std::array<xenos::ColorRenderTargetFormat, xenos::kMaxColorRenderTargets> formats = {};
  formats[0] = xenos::ColorRenderTargetFormat::k_8_8_8_8;
  formats[1] = xenos::ColorRenderTargetFormat::k_2_10_10_10;
  ProbeRenderPipelineDescription mixed = MakeDescription(0x3, formats);
  std::array<std::array<uint8_t, 4>, xenos::kMaxColorRenderTargets> expected = {};
  expected[0] = {0, 0, 255, 255};
  expected[1] = {0, 255, 0, 255};
  return ok && RunRoutingCase(device, vertex_library, kRt01FragmentMsl, mixed, 1, expected);
}

bool TestSharedDepth(id<MTLDevice> device, void* vertex_library) {
  std::string error;
  ProbeRenderPipelineDescription mrt_description = MakeDescription(0x3);
  void* mrt_fragment = CreateMslLibrary(device, kDepthMrtFragmentMsl, &error);
  void* mrt_pipeline = mrt_fragment
                           ? CreateRenderPipelineState(device, vertex_library, mrt_fragment, &error,
                                                       &mrt_description)
                           : nullptr;
  ReleaseMslLibrary(mrt_fragment);
  ProbeRenderPipelineDescription single_description = MakeDescription(0x1);
  void* far_fragment = CreateMslLibrary(device, kFarDepthFragmentMsl, &error);
  void* far_pipeline = far_fragment
                           ? CreateRenderPipelineState(device, vertex_library, far_fragment, &error,
                                                       &single_description)
                           : nullptr;
  ReleaseMslLibrary(far_fragment);
  ContextSet set;
  bool ready = mrt_pipeline && far_pipeline && CreateContexts(device, mrt_description, 1, set, error);
  ProbeDepthStencilState depth;
  depth.depth_test_enabled = true;
  depth.depth_write_enabled = true;
  depth.depth_compare_function = 1;  // MTLCompareFunctionLess.
  PipelineProbeMrtTelemetry telemetry;
  bool mrt_rendered = ready && RenderPipelineProbeMrtTriangleToContexts(
                                   set.contexts, mrt_description, mrt_pipeline, kWidth, kHeight,
                                   &depth, &telemetry, &error);
  uint32_t dummy_constants = 0;
  bool far_rendered =
      mrt_rendered && RenderPipelineProbeToContext(
                          set.contexts[0], far_pipeline, &dummy_constants,
                          sizeof(dummy_constants), nullptr, 0, nullptr, 0, nullptr, 0, nullptr,
                          nullptr, 0, 0, nullptr, 0, 0, 4, 3, kWidth, kHeight, &error,
                          UINT32_MAX, UINT32_MAX, UINT32_MAX, nullptr, 0, UINT32_MAX, UINT32_MAX,
                          nullptr, nullptr, nullptr, 0, UINT32_MAX, nullptr, 0, UINT32_MAX,
                          UINT32_MAX, nullptr, nullptr, &depth);
  std::vector<uint8_t> rt0;
  bool ok = Check(far_rendered, "shared-depth follow-up draw", error) &&
            Check(telemetry.depth_attachment_bind_count == 1,
                  "MRT encoded the shared depth attachment more than once") &&
            Check(ReadPipelineProbeContext(set.contexts[0], kWidth, kHeight, rt0, &error),
                  "shared-depth readback", error) &&
            Check(PixelNear(rt0, {0, 0, 255, 255}),
                  "shared depth write did not reject the farther follow-up draw");
  ReleaseRenderPipelineState(far_pipeline);
  ReleaseRenderPipelineState(mrt_pipeline);
  return ok;
}

bool TestSharedStencilExactlyOnce(id<MTLDevice> device, void* vertex_library) {
  std::string error;
  ProbeRenderPipelineDescription mrt_description = MakeDescription(0x3);
  void* mrt_fragment = CreateMslLibrary(device, kRt01FragmentMsl, &error);
  void* mrt_pipeline = mrt_fragment
                           ? CreateRenderPipelineState(device, vertex_library, mrt_fragment, &error,
                                                       &mrt_description)
                           : nullptr;
  ReleaseMslLibrary(mrt_fragment);
  ProbeRenderPipelineDescription single_description = MakeDescription(0x1);
  void* blue_fragment = CreateMslLibrary(device, kBlueFragmentMsl, &error);
  void* blue_pipeline =
      blue_fragment ? CreateRenderPipelineState(device, vertex_library, blue_fragment, &error,
                                                &single_description)
                    : nullptr;
  ReleaseMslLibrary(blue_fragment);
  ContextSet set;
  bool ready =
      mrt_pipeline && blue_pipeline && CreateContexts(device, mrt_description, 1, set, error);

  ProbeDepthStencilState increment;
  increment.stencil_test_enabled = true;
  increment.front.compare_function = 7;                 // Always.
  increment.front.depth_stencil_pass_operation = 3;    // IncrementClamp.
  increment.front.read_mask = 0xFF;
  increment.front.write_mask = 0xFF;
  increment.back = increment.front;
  PipelineProbeMrtTelemetry telemetry;
  bool mrt_rendered = ready && RenderPipelineProbeMrtTriangleToContexts(
                                   set.contexts, mrt_description, mrt_pipeline, kWidth, kHeight,
                                   &increment, &telemetry, &error);

  // If the MRT draw were secretly replayed once per color target, stencil
  // would become 2 and this Equal(1) draw would be rejected. Passing and
  // turning RT0 blue is semantic proof of one shared stencil side effect.
  ProbeDepthStencilState equal_one;
  equal_one.stencil_test_enabled = true;
  equal_one.front.compare_function = 2;  // Equal.
  equal_one.front.read_mask = 0xFF;
  equal_one.front.write_mask = 0xFF;
  equal_one.front.reference = 1;
  equal_one.back = equal_one.front;
  uint32_t dummy_constants = 0;
  bool compare_rendered =
      mrt_rendered && RenderPipelineProbeToContext(
                          set.contexts[0], blue_pipeline, &dummy_constants,
                          sizeof(dummy_constants), nullptr, 0, nullptr, 0, nullptr, 0, nullptr,
                          nullptr, 0, 0, nullptr, 0, 0, 4, 3, kWidth, kHeight, &error,
                          UINT32_MAX, UINT32_MAX, UINT32_MAX, nullptr, 0, UINT32_MAX, UINT32_MAX,
                          nullptr, nullptr, nullptr, 0, UINT32_MAX, nullptr, 0, UINT32_MAX,
                          UINT32_MAX, nullptr, nullptr, &equal_one);
  std::vector<uint8_t> rt0;
  bool ok = Check(compare_rendered, "shared-stencil compare follow-up draw", error) &&
            Check(telemetry.encoded_draw_count == 1 &&
                      telemetry.depth_attachment_bind_count == 1,
                  "shared-stencil MRT was not one draw with one attachment") &&
            Check(ReadPipelineProbeContext(set.contexts[0], kWidth, kHeight, rt0, &error),
                  "shared-stencil readback", error) &&
            Check(PixelNear(rt0, {255, 0, 0, 255}),
                  "shared stencil was not incremented exactly once");
  ReleaseRenderPipelineState(blue_pipeline);
  ReleaseRenderPipelineState(mrt_pipeline);
  return ok;
}

bool TestGeneralProductionSubmission(id<MTLDevice> device) {
  std::string error;
  void* vertex_library = CreateMslLibrary(device, kGeneralVertexMsl, &error);
  void* fragment_library = CreateMslLibrary(device, kGeneralFragmentMsl, &error);
  ProbeRenderPipelineDescription description = MakeDescription(0x3);
  void* pipeline = vertex_library && fragment_library
                       ? CreateRenderPipelineState(device, vertex_library, fragment_library,
                                                   &error, &description)
                       : nullptr;
  ReleaseMslLibrary(fragment_library);
  ReleaseMslLibrary(vertex_library);
  ContextSet set;
  bool ready = pipeline && CreateContexts(device, description, 1, set, error);

  const std::array<float, 4> system_constants = {0, 0, 0, 0};
  const std::array<float, 4> tint = {0.2f, 0.4f, 0.6f, 1.0f};
  const std::array<float, 6> positions = {-1, -1, 3, -1, -1, 3};
  const std::array<uint16_t, 3> indices = {0, 1, 2};
  const std::array<uint8_t, 4> texture_rgba = {32, 96, 224, 255};
  ProbeTextureSlot texture;
  texture.rgba = texture_rgba.data();
  texture.width = 1;
  texture.height = 1;
  texture.bytes_per_row = 4;
  texture.bytes_per_image = 4;
  ProbeSamplerSlot sampler;
  ProbeIndexBuffer index_buffer;
  index_buffer.data = indices.data();
  index_buffer.size = sizeof(indices);
  index_buffer.index_size = 2;
  ProbeRasterizationState rasterization;
  rasterization.viewport_width = kWidth;
  rasterization.viewport_height = kHeight;
  rasterization.scissor_width = kWidth;
  rasterization.scissor_height = kHeight;
  ProbeDepthStencilState depth;
  depth.depth_test_enabled = true;
  depth.depth_write_enabled = true;
  depth.depth_compare_function = 1;
  id<MTLBuffer> side_effect_buffer =
      [device newBufferWithLength:sizeof(uint32_t) * 2
                          options:MTLResourceStorageModeShared];
  if (side_effect_buffer) {
    std::memset([side_effect_buffer contents], 0, sizeof(uint32_t) * 2);
  }

  PipelineProbeMrtDraw draw;
  draw.system_constants = system_constants.data();
  draw.system_constants_size = sizeof(system_constants);
  draw.fragment_float_constants = tint.data();
  draw.fragment_float_constants_size = sizeof(tint);
  draw.shared_memory_metal_buffer = side_effect_buffer;
  draw.fragment_textures = &texture;
  draw.fragment_texture_count = 1;
  draw.fragment_sampler_count = 1;
  draw.fragment_samplers = &sampler;
  draw.vertex_data = positions.data();
  draw.vertex_data_size = sizeof(positions);
  draw.index_buffer = &index_buffer;
  draw.rasterization_state = &rasterization;
  draw.depth_stencil_state = &depth;
  draw.vertex_count = 3;
  draw.vertex_data_buffer_index = 3;
  draw.vertex_shared_memory_buffer_index = 5;
  draw.fragment_shared_memory_buffer_index = 6;
  draw.fragment_float_constants_buffer_index = 4;
  PipelineProbeMrtTelemetry telemetry;
  bool rendered = ready && side_effect_buffer && RenderPipelineProbeMrtToContexts(
                                                   set.contexts, description, pipeline, kWidth,
                                                   kHeight, draw, &telemetry, &error);
  std::vector<uint8_t> rt0;
  std::vector<uint8_t> rt1;
  const uint32_t* side_effects = side_effect_buffer
                                     ? static_cast<const uint32_t*>([side_effect_buffer contents])
                                     : nullptr;
  bool ok = Check(rendered, "general synchronous MRT submission", error) &&
            Check(telemetry.bound_color_target_count == 2 &&
                      telemetry.encoded_draw_count == 1 &&
                      telemetry.completed_command_buffer_count == 1 &&
                      telemetry.vertex_shared_memory_binding_count == 1 &&
                      telemetry.fragment_shared_memory_binding_count == 1,
                  "general MRT exactly-once telemetry") &&
            Check(side_effects && side_effects[0] == 1 && side_effects[1] == 1,
                  "vertex or fragment side effect was not executed exactly once") &&
            Check(ReadPipelineProbeContext(set.contexts[0], kWidth, kHeight, rt0, &error) &&
                      ReadPipelineProbeContext(set.contexts[1], kWidth, kHeight, rt1, &error),
                  "general MRT readback", error) &&
            Check(PixelNear(rt0, {224, 96, 32, 255}),
                  "general MRT texture/sampler binding was incorrect") &&
            Check(PixelNear(rt1, {153, 102, 51, 255}),
                  "general MRT fragment constant binding was incorrect");
  if (side_effect_buffer) {
    [side_effect_buffer release];
  }
  ReleaseRenderPipelineState(pipeline);
  return ok;
}

bool TestInjectedNonCompletionFailsClosed(id<MTLDevice> device, void* vertex_library) {
  std::string error;
  ProbeRenderPipelineDescription mrt_description = MakeDescription(0x3);
  void* mrt_fragment = CreateMslLibrary(device, kDepthMrtFragmentMsl, &error);
  void* mrt_pipeline =
      mrt_fragment ? CreateRenderPipelineState(device, vertex_library, mrt_fragment, &error,
                                               &mrt_description)
                   : nullptr;
  ReleaseMslLibrary(mrt_fragment);

  ProbeRenderPipelineDescription single_description = MakeDescription(0x1);
  void* far_fragment = CreateMslLibrary(device, kFarDepthFragmentMsl, &error);
  void* far_pipeline =
      far_fragment ? CreateRenderPipelineState(device, vertex_library, far_fragment, &error,
                                               &single_description)
                   : nullptr;
  ReleaseMslLibrary(far_fragment);

  ContextSet set;
  bool ready = mrt_pipeline && far_pipeline &&
               CreateContexts(device, mrt_description, 1, set, error);
  ProbeDepthStencilState depth;
  depth.depth_test_enabled = true;
  depth.depth_write_enabled = true;
  depth.depth_compare_function = 1;  // MTLCompareFunctionLess.

  PipelineProbeMrtDraw draw;
  draw.vertex_count = 3;
  draw.depth_stencil_state = &depth;
  draw.test_completion =
      PipelineProbeMrtTestCompletion::kReportNonCompletedAfterWait;
  PipelineProbeMrtTelemetry telemetry;
  bool rendered = ready && RenderPipelineProbeMrtToContexts(
                               set.contexts, mrt_description, mrt_pipeline, kWidth,
                               kHeight, draw, &telemetry, &error);

  constexpr const char* kInjectedError =
      "TEST-ONLY injected non-completed synchronous MRT command buffer";
  bool ok = Check(ready, "failure-injection setup", error) &&
            Check(!rendered, "injected non-completion was accepted") &&
            Check(error == kInjectedError,
                  "injected non-completion did not report the test-only marker", error) &&
            Check(telemetry.bound_color_target_count == 2 &&
                      telemetry.encoded_draw_count == 1 &&
                      telemetry.depth_attachment_bind_count == 1 &&
                      telemetry.completed_command_buffer_count == 0,
                  "failed MRT telemetry claimed command completion") &&
            Check(GetPipelineProbeContextPendingSubmissionCount(set.contexts[0]) == 0 &&
                      GetPipelineProbeContextPendingSubmissionCount(set.contexts[1]) == 0,
                  "failed synchronous MRT retained pending work");

  // The real command was submitted under Metal validation, but the injected
  // observation says it did not complete. Both color targets must therefore
  // be unreadable rather than exposing bytes from an unauthoritative command.
  for (uint32_t slot = 0; slot < 2; ++slot) {
    std::vector<uint8_t> rejected_readback;
    std::string read_error;
    bool read = ReadPipelineProbeContext(set.contexts[slot], kWidth, kHeight,
                                         rejected_readback, &read_error);
    ok = Check(!read && read_error.find("unavailable") != std::string::npos,
               "failed MRT color target remained authoritative", read_error) &&
         ok;
  }

  // The injected MRT physically wrote depth 0.25 before its completion was
  // rejected. A later depth-0.75 Less draw can pass only if failure also
  // invalidated shared depth, causing a clear to 1.0 instead of a stale load.
  uint32_t dummy_constants = 0;
  std::string recovery_error;
  bool recovered =
      ready && RenderPipelineProbeToContext(
                   set.contexts[0], far_pipeline, &dummy_constants,
                   sizeof(dummy_constants), nullptr, 0, nullptr, 0, nullptr, 0,
                   nullptr, nullptr, 0, 0, nullptr, 0, 0, 4, 3, kWidth, kHeight,
                   &recovery_error, UINT32_MAX, UINT32_MAX, UINT32_MAX, nullptr, 0,
                   UINT32_MAX, UINT32_MAX, nullptr, nullptr, nullptr, 0,
                   UINT32_MAX, nullptr, 0, UINT32_MAX, UINT32_MAX, nullptr,
                   nullptr, &depth);
  std::vector<uint8_t> recovered_rt0;
  ok = Check(recovered, "draw after failed MRT did not recover", recovery_error) &&
       Check(ReadPipelineProbeContext(set.contexts[0], kWidth, kHeight,
                                      recovered_rt0, &recovery_error),
             "recovered MRT target readback", recovery_error) &&
       Check(PixelNear(recovered_rt0, {0, 255, 0, 255}),
             "failed MRT left stale shared depth authoritative") &&
       ok;

  ReleaseRenderPipelineState(far_pipeline);
  ReleaseRenderPipelineState(mrt_pipeline);
  return ok;
}

bool TestRejectionsAndLifecycle(id<MTLDevice> device, void* vertex_library) {
  std::string error;
  ProbeRenderPipelineDescription description = MakeDescription(0x3);
  void* fragment = CreateMslLibrary(device, kRt01FragmentMsl, &error);
  void* pipeline = fragment ? CreateRenderPipelineState(device, vertex_library, fragment, &error,
                                                         &description)
                            : nullptr;
  ReleaseMslLibrary(fragment);
  ContextSet valid;
  if (!pipeline || !CreateContexts(device, description, 1, valid, error)) {
    ReleaseRenderPipelineState(pipeline);
    return Check(false, "rejection-test setup", error);
  }
  auto expect_reject = [&](const std::array<void*, xenos::kMaxColorRenderTargets>& contexts,
                           const ProbeRenderPipelineDescription& candidate, const char* label) {
    error.clear();
    return Check(!RenderPipelineProbeMrtTriangleToContexts(
                     contexts, candidate, pipeline, kWidth, kHeight, nullptr, nullptr, &error) &&
                     !error.empty(),
                 label, error);
  };
  auto aliased = valid.contexts;
  aliased[1] = aliased[0];
  bool ok = expect_reject(aliased, description, "MRT context alias was accepted");
  auto extra = valid.contexts;
  ProbeRenderPipelineDescription rt1_only = MakeDescription(0x2);
  ok = ok && expect_reject(extra, rt1_only, "inactive MRT context was accepted");
  ProbeRenderPipelineDescription wrong_format = description;
  wrong_format.color_targets[1].color_format = xenos::ColorRenderTargetFormat::k_2_10_10_10;
  wrong_format.color_targets[1].metal_storage = ColorTargetStorageKind::kRgb10A2Unorm;
  ok = ok && expect_reject(valid.contexts, wrong_format, "MRT format mismatch was accepted");

  std::array<void*, xenos::kMaxColorRenderTargets> different_queue = {};
  different_queue[0] = CreatePipelineProbeContext(device, &error);
  different_queue[1] = CreatePipelineProbeContext(device, &error);
  ok = ok && expect_reject(different_queue, description, "MRT queue mismatch was accepted");
  ReleasePipelineProbeContext(different_queue[1]);
  ReleasePipelineProbeContext(different_queue[0]);

  ContextSet different_depth;
  different_depth.queue = [device newCommandQueue];
  different_depth.contexts[0] = CreatePipelineProbeContext(device, different_depth.queue, &error);
  different_depth.contexts[1] = CreatePipelineProbeContext(device, different_depth.queue, &error);
  ok = ok && expect_reject(different_depth.contexts, description,
                           "MRT separate depth targets were accepted");

  ContextSet different_samples;
  different_samples.queue = [device newCommandQueue];
  different_samples.contexts[0] =
      CreatePipelineProbeContext(device, different_samples.queue, &error);
  different_samples.contexts[1] =
      CreatePipelineProbeContext(device, different_samples.queue, &error);
  if ([device supportsTextureSampleCount:2]) {
    SetPipelineProbeContextSampleCount(different_samples.contexts[1], 2, &error);
    ok = ok && expect_reject(different_samples.contexts, description,
                             "MRT sample-count mismatch was accepted");
  }

  bool initialized = ClearPipelineProbeContext(valid.contexts[0], kWidth, kHeight, 0, 0, 0, 1,
                                                &error) &&
                     ClearPipelineProbeContext(valid.contexts[1], kWidth, kHeight, 0, 0, 0, 1,
                                               &error);
  error.clear();
  ok = ok && Check(initialized, "MRT dimension-test initialization", error) &&
       Check(!RenderPipelineProbeMrtTriangleToContexts(valid.contexts, description, pipeline,
                                                       kWidth + 1, kHeight, nullptr, nullptr,
                                                       &error),
             "MRT dimension mismatch was accepted");
  error.clear();
  uint32_t incomplete_constants = 0;
  PipelineProbeMrtDraw incomplete_draw;
  incomplete_draw.system_constants = &incomplete_constants;
  incomplete_draw.vertex_count = 3;
  ok = ok && Check(!RenderPipelineProbeMrtToContexts(
                       valid.contexts, description, pipeline, kWidth, kHeight,
                       incomplete_draw, nullptr, &error) &&
                       !error.empty(),
                   "MRT incomplete resource span was accepted", error);
  error.clear();
  PipelineProbeMrtTelemetry telemetry;
  ok = ok && Check(RenderPipelineProbeMrtTriangleToContexts(
                       valid.contexts, description, pipeline, kWidth, kHeight, nullptr,
                       &telemetry, &error),
                   "valid MRT lifecycle draw after rejections", error) &&
       Check(GetPipelineProbeContextPendingSubmissionCount(valid.contexts[0]) == 0 &&
                 GetPipelineProbeContextPendingSubmissionCount(valid.contexts[1]) == 0,
             "valid synchronous MRT lifecycle retained submissions");
  ReleaseRenderPipelineState(pipeline);
  return ok;
}

int Run() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!Check(device != nil, "no Metal device")) {
      return 1;
    }
    std::string error;
    void* vertex_library = CreateMslLibrary(device, kVertexMsl, &error);
    if (!Check(vertex_library != nullptr, "MRT vertex library", error) ||
        !TestDescriptorKeys()) {
      ReleaseMslLibrary(vertex_library);
      return 1;
    }

    std::array<std::array<uint8_t, 4>, xenos::kMaxColorRenderTargets> expected = {};
    expected[0] = {0, 0, 255, 255};
    expected[1] = {0, 255, 0, 255};
    for (uint32_t samples : {1u, 2u, 4u}) {
      if ([device supportsTextureSampleCount:samples] &&
          !RunRoutingCase(device, vertex_library, kRt01FragmentMsl, MakeDescription(0x3), samples,
                          expected)) {
        ReleaseMslLibrary(vertex_library);
        return 1;
      }
    }
    expected = {};
    expected[1] = {255, 0, 0, 255};
    bool ok = RunRoutingCase(device, vertex_library, kRt1FragmentMsl, MakeDescription(0x2), 1,
                             expected);
    expected = {};
    expected[0] = {0, 0, 255, 255};
    expected[2] = {255, 0, 0, 255};
    ok = ok && RunRoutingCase(device, vertex_library, kRt02FragmentMsl, MakeDescription(0x5), 1,
                              expected);
    expected = {};
    expected[0] = {0, 0, 255, 255};
    expected[1] = {0, 255, 0, 255};
    expected[2] = {255, 0, 0, 255};
    expected[3] = {0, 255, 255, 255};
    ok = ok && RunRoutingCase(device, vertex_library, kRt0123FragmentMsl,
                              MakeDescription(0xF), 1, expected) &&
         TestMaskBlendAndMixedFormat(device, vertex_library) &&
         TestSharedDepth(device, vertex_library) &&
         TestSharedStencilExactlyOnce(device, vertex_library) &&
         TestGeneralProductionSubmission(device) &&
         TestInjectedNonCompletionFailsClosed(device, vertex_library) &&
         TestRejectionsAndLifecycle(device, vertex_library);
    ReleaseMslLibrary(vertex_library);
    if (!ok) {
      return 1;
    }
    std::fprintf(stdout,
                 "[metal_mrt_probe_test] PASS: four-slot routing, formats, masks, blend, "
                 "shared depth/stencil, MSAA, full resources, exactly-once side effects, "
                 "failure invalidation, lifecycle and rejection matrix\n");
    return 0;
  }
}

}  // namespace

int main() { return Run(); }
