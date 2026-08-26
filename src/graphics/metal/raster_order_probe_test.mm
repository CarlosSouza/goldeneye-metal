// Real-Metal go/no-go probe for the existing SPIR-V fragment-interlock path.
// This does not enable the path in production. It proves that the translated
// buffer binding lowers to a Metal raster-order group and that ordered packed
// word updates survive overlapping fragments at every supported sample count.

#import <Metal/Metal.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>
#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <rex/graphics/metal/exact_output_merger.h>
#include <rex/graphics/metal/exact_output_merger_state.h>
#include <rex/graphics/metal/msl_compiler.h>
#include <rex/graphics/metal/shader.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/shared_memory.h>
#include <rex/string/buffer.h>

namespace rex::graphics::metal {
// Deliberately absent from the shipping header. This hidden symbol exists only
// so this real-Metal executable can compile synthetic attested MSL fixtures.
ExactOutputMergerPipeline* CreateExactOutputMergerPipelineForInternalProbeFromMslSources(
    void* resources, const char* vertex_source, const char* vertex_function_name,
    const char* fragment_source, const char* fragment_function_name, uint32_t sample_count,
    const ExactOutputMergerPipelineLayout& layout, std::string* error_out);
}  // namespace rex::graphics::metal

// Test-only Metal protocol wrappers. The real command buffer is encoded,
// committed and completed normally; the wrapper reports one deterministic
// post-submit error to exercise the executor's asynchronous failure contract
// without adding a production environment switch or corrupting GPU work.
struct ExactFaultInjectionCounters {
  uint32_t command_buffers_created = 0;
  uint32_t command_buffers_destroyed = 0;
  uint32_t active_command_buffers = 0;
  uint32_t peak_active_command_buffers = 0;
  uint32_t injected_command_buffers = 0;
};

@interface ExactFaultInjectingCommandBuffer : NSObject {
 @private
  id<MTLCommandBuffer> inner_;
  ExactFaultInjectionCounters* counters_;
  bool inject_failure_;
}

- (instancetype)initWithCommandBuffer:(id<MTLCommandBuffer>)command_buffer
                             counters:(ExactFaultInjectionCounters*)counters
                        injectFailure:(bool)inject_failure;

@end

@implementation ExactFaultInjectingCommandBuffer

- (instancetype)initWithCommandBuffer:(id<MTLCommandBuffer>)command_buffer
                             counters:(ExactFaultInjectionCounters*)counters
                        injectFailure:(bool)inject_failure {
  self = [super init];
  if (self) {
    inner_ = [command_buffer retain];
    counters_ = counters;
    inject_failure_ = inject_failure;
    ++counters_->command_buffers_created;
    ++counters_->active_command_buffers;
    counters_->peak_active_command_buffers =
        std::max(counters_->peak_active_command_buffers, counters_->active_command_buffers);
    if (inject_failure_) {
      ++counters_->injected_command_buffers;
    }
  }
  return self;
}

- (void)dealloc {
  [inner_ release];
  ++counters_->command_buffers_destroyed;
  --counters_->active_command_buffers;
  [super dealloc];
}

- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:
    (MTLRenderPassDescriptor*)descriptor {
  return [inner_ renderCommandEncoderWithDescriptor:descriptor];
}

- (void)commit {
  [inner_ commit];
}

- (void)waitUntilCompleted {
  [inner_ waitUntilCompleted];
}

- (MTLCommandBufferStatus)status {
  const MTLCommandBufferStatus status = inner_.status;
  return inject_failure_ && status == MTLCommandBufferStatusCompleted ? MTLCommandBufferStatusError
                                                                      : status;
}

- (NSError*)error {
  if (inject_failure_ && inner_.status == MTLCommandBufferStatusCompleted) {
    return [NSError
        errorWithDomain:@"GoldenEyeExactOutputMergerProbe"
                   code:1
               userInfo:@{
                 NSLocalizedDescriptionKey : @"injected exact output-merger command-buffer failure"
               }];
  }
  return inner_.error;
}

- (BOOL)respondsToSelector:(SEL)selector {
  return [super respondsToSelector:selector] || [inner_ respondsToSelector:selector];
}

- (id)forwardingTargetForSelector:(SEL)selector {
  return [inner_ respondsToSelector:selector] ? inner_
                                              : [super forwardingTargetForSelector:selector];
}

@end

@interface ExactFaultInjectingCommandQueue : NSObject {
 @private
  id<MTLCommandQueue> inner_;
  ExactFaultInjectionCounters* counters_;
  uint32_t failures_to_inject_;
}

- (instancetype)initWithCommandQueue:(id<MTLCommandQueue>)command_queue
                            counters:(ExactFaultInjectionCounters*)counters;
- (void)injectNextCommandBufferFailure;

@end

@implementation ExactFaultInjectingCommandQueue

- (instancetype)initWithCommandQueue:(id<MTLCommandQueue>)command_queue
                            counters:(ExactFaultInjectionCounters*)counters {
  self = [super init];
  if (self) {
    inner_ = [command_queue retain];
    counters_ = counters;
  }
  return self;
}

- (void)dealloc {
  [inner_ release];
  [super dealloc];
}

- (id<MTLDevice>)device {
  return inner_.device;
}

- (id<MTLCommandBuffer>)commandBuffer {
  id<MTLCommandBuffer> command_buffer = [inner_ commandBuffer];
  const bool inject_failure = failures_to_inject_ != 0;
  if (inject_failure) {
    --failures_to_inject_;
  }
  return [(id)[[ExactFaultInjectingCommandBuffer alloc] initWithCommandBuffer:command_buffer
                                                                     counters:counters_
                                                                injectFailure:inject_failure]
      autorelease];
}

- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences {
  return [self commandBuffer];
}

- (void)injectNextCommandBufferFailure {
  ++failures_to_inject_;
}

- (BOOL)respondsToSelector:(SEL)selector {
  return [super respondsToSelector:selector] || [inner_ respondsToSelector:selector];
}

- (id)forwardingTargetForSelector:(SEL)selector {
  return [inner_ respondsToSelector:selector] ? inner_
                                              : [super forwardingTargetForSelector:selector];
}

@end

namespace {

constexpr uint32_t kOperationCount = 96;
constexpr uint32_t kRepeatCount = 24;

using ExactEdramImage = std::vector<uint32_t>;

ExactEdramImage MakeExactEdramImage(uint32_t fill = 0) {
  return ExactEdramImage(rex::graphics::metal::kExactOutputMergerEdramDwordCount, fill);
}

bool UploadExactEdramImage(void* resources, const ExactEdramImage& image, std::string& error_out) {
  return image.size() == rex::graphics::metal::kExactOutputMergerEdramDwordCount &&
         rex::graphics::metal::UploadExactOutputMergerEdram(
             resources, image.data(), image.size() * sizeof(uint32_t), &error_out);
}

bool DownloadExactEdramImage(void* resources, ExactEdramImage& image, std::string& error_out) {
  return image.size() == rex::graphics::metal::kExactOutputMergerEdramDwordCount &&
         rex::graphics::metal::DownloadExactOutputMergerEdram(
             resources, image.data(), image.size() * sizeof(uint32_t), &error_out);
}

struct ProbeConstants {
  uint32_t mode;
  uint32_t operation_count;
  uint32_t salt;
  uint32_t sample_count;
  uint32_t base_tiles;
  uint32_t pitch_tiles;
  uint32_t logical_x;
  uint32_t logical_y;
  uint32_t words_per_sample;
  uint32_t dword;
  uint32_t is_depth;
  uint32_t padding;
};
static_assert(sizeof(ProbeConstants) == 48);

struct BorrowedSystem {
  uint32_t target_dword;
  uint32_t expected_value;
  uint32_t vertex_magic;
  uint32_t fragment_magic;
};
static_assert(sizeof(BorrowedSystem) == 16);

struct SampleIdentityConstants {
  uint32_t base_dword;
  uint32_t sample_count;
};
static_assert(sizeof(SampleIdentityConstants) == 8);

constexpr char kProbeVertexMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct ProbeVertexOutput {
  float4 position [[position]];
  uint operation [[user(locn0), flat]];
};

vertex ProbeVertexOutput probe_vertex(uint vertex_id [[vertex_id]]) {
  ProbeVertexOutput output;
  uint corner = vertex_id % 3u;
  output.position = float4(corner == 1u ? 3.0f : -1.0f,
                           corner == 2u ? 3.0f : -1.0f, 0.0f, 1.0f);
  output.operation = vertex_id / 3u;
  return output;
}
)MSL";

constexpr char kLeftHalfVertexMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct EdgeVertexOutput {
  float4 position [[position]];
};

vertex EdgeVertexOutput edge_vertex(uint vertex_id [[vertex_id]]) {
  constexpr float2 positions[6] = {
      float2(-1.0f, -1.0f), float2(0.0f, -1.0f),
      float2(-1.0f, 1.0f), float2(0.0f, -1.0f),
      float2(0.0f, 1.0f), float2(-1.0f, 1.0f),
  };
  EdgeVertexOutput output;
  output.position = float4(positions[min(vertex_id, 5u)], 0.0f, 1.0f);
  return output;
}
)MSL";

constexpr char kSampleIdentityFragmentMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct EdgeVertexOutput {
  float4 position [[position]];
};

struct SampleIdentityConstants {
  uint base_dword;
  uint sample_count;
};

struct XeEdram {
  uint edram[1];
};

fragment float4 sample_identity_fragment(
    EdgeVertexOutput input [[stage_in]],
    constant SampleIdentityConstants& xe_uniform_system_constants [[buffer(0)]],
    coherent device XeEdram& xe_edram [[buffer(1), raster_order_group(0)]],
    uint sample_id [[sample_id]], uint sample_mask [[sample_mask]]) {
  if (sample_id < xe_uniform_system_constants.sample_count) {
    float2 position = get_sample_position(sample_id);
    uint position_x = uint(round(position.x * 16.0f));
    uint position_y = uint(round(position.y * 16.0f));
    xe_edram.edram[xe_uniform_system_constants.base_dword + sample_id] =
        sample_id | (position_x << 8u) | (position_y << 16u) |
        ((sample_mask & 0xFu) << 24u);
  }
  return float4(0.0f);
}
)MSL";

constexpr char kProbeFragmentMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct ProbeVertexOutput {
  float4 position [[position]];
  uint operation [[user(locn0), flat]];
};

struct ProbeConstants {
  uint mode;
  uint operation_count;
  uint salt;
  uint sample_count;
  uint base_tiles;
  uint pitch_tiles;
  uint logical_x;
  uint logical_y;
  uint words_per_sample;
  uint dword;
  uint is_depth;
  uint padding;
};

struct XeEdram {
  uint edram[1];
};

static inline uint probe_source(uint operation, uint salt) {
  uint value = (operation + 1u) * 0x45D9F3Bu;
  value ^= salt * 0x119DE1F3u;
  value ^= value >> 16u;
  return value;
}

static inline uint blend_channel(uint destination, uint source, uint alpha) {
  return (source * alpha + destination * (255u - alpha) + 127u) / 255u;
}

static inline uint canonical_edram_dword(constant ProbeConstants& constants,
                                         uint sample_id) {
  uint sample_x_log2 = constants.sample_count >= 4u ? 1u : 0u;
  uint sample_y_log2 = constants.sample_count >= 2u ? 1u : 0u;
  uint sample_x = (constants.logical_x << sample_x_log2) +
                  (sample_x_log2 != 0u ? (sample_id & 1u) : 0u);
  uint sample_y = (constants.logical_y << sample_y_log2) +
                  (sample_id >> sample_x_log2);
  uint word_x = sample_x * constants.words_per_sample + constants.dword;
  uint tile_x = word_x / 80u;
  uint tile_y = sample_y / 16u;
  uint tile = (constants.base_tiles + tile_y * constants.pitch_tiles + tile_x) & 2047u;
  uint tile_word_x = word_x % 80u;
  if (constants.is_depth != 0u) {
    tile_word_x = (tile_word_x + 40u) % 80u;
  }
  return tile * (80u * 16u) + (sample_y % 16u) * 80u + tile_word_x;
}

fragment float4 probe_fragment(
    ProbeVertexOutput input [[stage_in]],
    constant ProbeConstants& xe_uniform_system_constants [[buffer(0)]],
    coherent device XeEdram& xe_edram [[buffer(1), raster_order_group(0)]],
    uint sample_id [[sample_id]]) {
  uint operation = input.operation;
  if (operation >= xe_uniform_system_constants.operation_count ||
      sample_id >= xe_uniform_system_constants.sample_count) {
    return float4(float(sample_id));
  }

  uint source = probe_source(operation, xe_uniform_system_constants.salt);
  uint edram_dword = canonical_edram_dword(xe_uniform_system_constants, sample_id);
  uint old_value = xe_edram.edram[edram_dword];
  uint new_value = old_value;
  switch (xe_uniform_system_constants.mode) {
    case 0u:
      new_value = (old_value * 1664525u + 1013904223u) ^ source;
      break;
    case 1u: {
      uint lane = operation & 3u;
      uint mask = 0xFFu << (lane * 8u);
      new_value = (old_value & ~mask) | (source & mask);
      break;
    }
    case 2u: {
      uint alpha =
          32u + ((operation * 29u + xe_uniform_system_constants.salt) % 224u);
      new_value = 0u;
      for (uint lane = 0u; lane < 4u; ++lane) {
        uint shift = lane * 8u;
        uint destination_channel = (old_value >> shift) & 0xFFu;
        uint source_channel = (source >> shift) & 0xFFu;
        new_value |= blend_channel(destination_channel, source_channel, alpha) << shift;
      }
      break;
    }
    default: {
      uint incoming_depth = source & 0xFFFFFFu;
      uint stored_depth = old_value >> 8u;
      if (incoming_depth < stored_depth) {
        uint stencil_mask = 0x3Cu;
        uint stencil = ((old_value & 0xFFu) & ~stencil_mask) |
                       ((source >> 24u) & stencil_mask);
        new_value = (incoming_depth << 8u) | stencil;
      }
      break;
    }
  }
  xe_edram.edram[edram_dword] = new_value;
  return float4(float(sample_id));
}
)MSL";

constexpr char kBorrowedResourceMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct BorrowedSystem {
  uint target_dword;
  uint expected_value;
  uint vertex_magic;
  uint fragment_magic;
};

struct BorrowedVertexOutput {
  float4 position [[position]];
  uint marker [[user(locn0), flat]];
};

struct XeEdram {
  uint edram[1];
};

vertex BorrowedVertexOutput resource_vertex(
    uint vertex_id [[vertex_id]],
    constant BorrowedSystem& xe_uniform_system_constants [[buffer(0)]],
    constant float4* xe_uniform_float_constants [[buffer(1)]],
    constant uint4* xe_uniform_fetch_constants [[buffer(2)]],
    constant uint4* xe_uniform_bool_loop_constants [[buffer(3)]],
    const device uint* xe_shared_memory [[buffer(4)]],
    const device float4* xe_vertex_data [[buffer(5)]],
    texture2d_array<float> xe_texture0_2d [[texture(0)]],
    sampler xe_sampler0 [[sampler(0)]]) {
  float4 sampled = xe_texture0_2d.sample(xe_sampler0, float2(0.25f, 0.5f), 0u);
  bool ready = xe_uniform_float_constants[0].x == 5.0f &&
               xe_uniform_fetch_constants[0].x == 11u &&
               xe_uniform_bool_loop_constants[0].x == 12u &&
               xe_shared_memory[0] == 13u && sampled.r > 0.99f && sampled.g < 0.01f;
  BorrowedVertexOutput output;
  output.position = xe_vertex_data[vertex_id];
  output.marker = ready ? xe_uniform_system_constants.vertex_magic : 0u;
  return output;
}

fragment float4 resource_fragment(
    BorrowedVertexOutput input [[stage_in]],
    constant BorrowedSystem& xe_uniform_system_constants [[buffer(0)]],
    coherent device XeEdram& xe_edram [[buffer(1), raster_order_group(0)]],
    constant float4* xe_uniform_float_constants [[buffer(2)]],
    constant uint4* xe_uniform_fetch_constants [[buffer(3)]],
    constant uint4* xe_uniform_bool_loop_constants [[buffer(4)]],
    const device uint* xe_shared_memory [[buffer(5)]],
    texture2d_array<float> xe_texture0_2d [[texture(0)]],
    sampler xe_sampler0 [[sampler(0)]]) {
  float4 sampled = xe_texture0_2d.sample(xe_sampler0, float2(0.5f), 0u);
  bool ready = input.marker == xe_uniform_system_constants.vertex_magic &&
               xe_uniform_system_constants.fragment_magic == 0x12345678u &&
               xe_uniform_float_constants[0].x == 7.0f &&
               xe_uniform_fetch_constants[0].x == 11u &&
               xe_uniform_bool_loop_constants[0].x == 12u &&
               xe_shared_memory[0] == 13u && sampled.r < 0.01f &&
               sampled.g > 0.45f && sampled.g < 0.55f;
  xe_edram.edram[xe_uniform_system_constants.target_dword] =
      ready ? xe_uniform_system_constants.expected_value : 0u;
  return sampled;
}
)MSL";

constexpr char kBorrowedMetalIndexMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct BorrowedSystem {
  uint target_dword;
  uint expected_value;
  uint vertex_magic;
  uint fragment_magic;
};

struct BorrowedVertexOutput {
  float4 position [[position]];
  uint marker [[user(locn0), flat]];
};

struct XeEdram {
  uint edram[1];
};

vertex BorrowedVertexOutput metal_index_vertex(
    uint vertex_id [[vertex_id]],
    constant BorrowedSystem& xe_uniform_system_constants [[buffer(0)]]) {
  constexpr float4 positions[6] = {
      float4(2.0f, 2.0f, 0.0f, 1.0f),
      float4(2.0f, 2.0f, 0.0f, 1.0f),
      float4(2.0f, 2.0f, 0.0f, 1.0f),
      float4(-1.0f, -1.0f, 0.0f, 1.0f),
      float4(3.0f, -1.0f, 0.0f, 1.0f),
      float4(-1.0f, 3.0f, 0.0f, 1.0f),
  };
  BorrowedVertexOutput output;
  output.position = positions[min(vertex_id, 5u)];
  output.marker = xe_uniform_system_constants.vertex_magic;
  return output;
}

fragment float4 metal_index_fragment(
    BorrowedVertexOutput input [[stage_in]],
    constant BorrowedSystem& xe_uniform_system_constants [[buffer(0)]],
    coherent device XeEdram& xe_edram [[buffer(1), raster_order_group(0)]]) {
  xe_edram.edram[xe_uniform_system_constants.target_dword] =
      input.marker == xe_uniform_system_constants.vertex_magic
          ? xe_uniform_system_constants.expected_value
          : 0u;
  return float4(0.0f);
}
)MSL";

constexpr char kFixedPrimitiveRestartMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct BorrowedSystem {
  uint target_dword;
  uint expected_value;
  uint vertex_magic;
  uint fragment_magic;
};

struct RestartVertexOutput {
  float4 position [[position]];
  uint marker [[user(locn0), flat]];
};

struct XeEdram {
  uint edram[1];
};

vertex RestartVertexOutput fixed_restart_vertex(
    uint vertex_id [[vertex_id]],
    constant BorrowedSystem& xe_uniform_system_constants [[buffer(0)]]) {
  constexpr float4 positions[6] = {
      float4(-1.0f, -1.0f, 0.0f, 1.0f),
      float4(3.0f, -1.0f, 0.0f, 1.0f),
      float4(-1.0f, 3.0f, 0.0f, 1.0f),
      float4(-1.0f, -1.0f, 0.0f, 1.0f),
      float4(3.0f, -1.0f, 0.0f, 1.0f),
      float4(-1.0f, 3.0f, 0.0f, 1.0f),
  };
  RestartVertexOutput output;
  // Clamping also makes the no-restart control deterministic: its separator
  // replacement deliberately selects vertex 5 and creates one extra covering
  // bridge triangle.
  output.position = positions[min(vertex_id, 5u)];
  output.marker = xe_uniform_system_constants.vertex_magic;
  return output;
}

fragment float4 fixed_restart_fragment(
    RestartVertexOutput input [[stage_in]],
    constant BorrowedSystem& xe_uniform_system_constants [[buffer(0)]],
    coherent device XeEdram& xe_edram [[buffer(1), raster_order_group(0)]]) {
  if (input.marker == xe_uniform_system_constants.vertex_magic) {
    uint prior = xe_edram.edram[xe_uniform_system_constants.target_dword];
    xe_edram.edram[xe_uniform_system_constants.target_dword] = prior + 1u;
  }
  return float4(0.0f);
}
)MSL";

constexpr char kNonRogFragmentMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct XeEdram { uint edram[1]; };
fragment float4 non_rog_fragment(device XeEdram& xe_edram [[buffer(1)]]) {
  xe_edram.edram[0] = 1u;
  return float4(0.0f);
}
)MSL";

constexpr char kUnsupportedTextureFragmentMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct XeEdram { uint edram[1]; };
fragment float4 unsupported_texture_fragment(
    coherent device XeEdram& xe_edram [[buffer(1), raster_order_group(0)]],
    texture3d<float> xe_texture0_2d [[texture(0)]]) {
  float4 value = xe_texture0_2d.read(uint3(0));
  xe_edram.edram[0] = as_type<uint>(value.x);
  return value;
}
)MSL";

bool HasExecutionMode(const std::vector<uint8_t>& spirv, spv::ExecutionMode mode) {
  if (spirv.size() < 5 * sizeof(uint32_t) || (spirv.size() & 3u)) {
    return false;
  }
  std::vector<uint32_t> words(spirv.size() / sizeof(uint32_t));
  std::memcpy(words.data(), spirv.data(), spirv.size());
  for (size_t index = 5; index < words.size();) {
    uint32_t instruction = words[index];
    uint32_t word_count = instruction >> 16u;
    uint32_t opcode = instruction & 0xFFFFu;
    if (!word_count || index + word_count > words.size()) {
      return false;
    }
    if (opcode == uint32_t(spv::OpExecutionMode) && word_count >= 3u &&
        words[index + 2] == uint32_t(mode)) {
      return true;
    }
    index += word_count;
  }
  return false;
}

rex::graphics::metal::ExactOutputMergerPipeline* CreateCoveragePipeline(
    void* resources, const char* vertex_source, const char* vertex_name,
    const char* fragment_source, const char* fragment_name, uint32_t sample_count,
    uint32_t expected_edram_binding, std::string& error_out) {
  rex::graphics::metal::ExactOutputMergerPipelineLayout layout;
  layout.fragment.system_constants_buffer_index = 0;
  layout.edram_fragment_buffer_index = expected_edram_binding;
  return rex::graphics::metal::CreateExactOutputMergerPipelineForInternalProbeFromMslSources(
      resources, vertex_source, vertex_name, fragment_source, fragment_name, sample_count, layout,
      &error_out);
}

uint32_t ProbeSource(uint32_t operation, uint32_t salt) {
  uint32_t value = (operation + 1u) * UINT32_C(0x45D9F3B);
  value ^= salt * UINT32_C(0x119DE1F3);
  value ^= value >> 16u;
  return value;
}

uint32_t BlendChannel(uint32_t destination, uint32_t source, uint32_t alpha) {
  return (source * alpha + destination * (255u - alpha) + 127u) / 255u;
}

uint32_t ApplyReferenceOperation(uint32_t mode, uint32_t old_value, uint32_t operation,
                                 uint32_t salt) {
  uint32_t source = ProbeSource(operation, salt);
  switch (mode) {
    case 0:
      return (old_value * UINT32_C(1664525) + UINT32_C(1013904223)) ^ source;
    case 1: {
      uint32_t mask = UINT32_C(0xFF) << ((operation & 3u) * 8u);
      return (old_value & ~mask) | (source & mask);
    }
    case 2: {
      uint32_t alpha = 32u + ((operation * 29u + salt) % 224u);
      uint32_t result = 0;
      for (uint32_t lane = 0; lane < 4; ++lane) {
        uint32_t shift = lane * 8u;
        result |= BlendChannel((old_value >> shift) & 0xFFu, (source >> shift) & 0xFFu, alpha)
                  << shift;
      }
      return result;
    }
    default: {
      uint32_t incoming_depth = source & UINT32_C(0xFFFFFF);
      if (incoming_depth >= (old_value >> 8u)) {
        return old_value;
      }
      constexpr uint32_t kStencilMask = 0x3C;
      uint32_t stencil = ((old_value & 0xFFu) & ~kStencilMask) | ((source >> 24u) & kStencilMask);
      return (incoming_depth << 8u) | stencil;
    }
  }
}

constexpr std::array<rex::graphics::xenos::ColorRenderTargetFormat, 12> kDefinedColorFormats = {
    rex::graphics::xenos::ColorRenderTargetFormat::k_8_8_8_8,
    rex::graphics::xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA,
    rex::graphics::xenos::ColorRenderTargetFormat::k_2_10_10_10,
    rex::graphics::xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT,
    rex::graphics::xenos::ColorRenderTargetFormat::k_16_16,
    rex::graphics::xenos::ColorRenderTargetFormat::k_16_16_16_16,
    rex::graphics::xenos::ColorRenderTargetFormat::k_16_16_FLOAT,
    rex::graphics::xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT,
    rex::graphics::xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10,
    rex::graphics::xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16,
    rex::graphics::xenos::ColorRenderTargetFormat::k_32_FLOAT,
    rex::graphics::xenos::ColorRenderTargetFormat::k_32_32_FLOAT,
};

bool IsTargetIndex(const std::array<size_t, 4>& targets, uint32_t target_count, size_t index) {
  return std::find(targets.begin(), targets.begin() + target_count, index) !=
         targets.begin() + target_count;
}

size_t FindGuardIndex(const std::array<size_t, 4>& targets, uint32_t target_count, size_t start) {
  for (size_t distance = 1; distance < 32; ++distance) {
    size_t candidate = (start + distance) % rex::graphics::metal::kExactOutputMergerEdramDwordCount;
    if (!IsTargetIndex(targets, target_count, candidate)) {
      return candidate;
    }
  }
  return SIZE_MAX;
}

bool FindNamedBufferBinding(const std::string& source, const char* name, uint32_t& binding_out) {
  binding_out = UINT32_MAX;
  const std::string needle = std::string(name) + " [[buffer(";
  size_t position = source.find(needle);
  if (position == std::string::npos ||
      source.find(needle, position + needle.size()) != std::string::npos) {
    return false;
  }
  position += needle.size();
  uint32_t binding = 0;
  bool has_digit = false;
  while (position < source.size() && source[position] >= '0' && source[position] <= '9') {
    has_digit = true;
    binding = binding * 10u + uint32_t(source[position] - '0');
    ++position;
  }
  if (!has_digit || position >= source.size() || source[position] != ')' ||
      binding > rex::graphics::metal::kExactOutputMergerMaxMetalBufferIndex) {
    return false;
  }
  binding_out = binding;
  return true;
}

// Synthetic Xenos pixel shader assembled directly from the documented ucode
// fields. It allocates color 0 and exports constant 1 to RGBA. This fixture is
// authored for the probe and contains no title or SDK shader data.
constexpr std::array<uint32_t, 6> kSyntheticGuestColorShader = {
    // alloc colors 1; exece at ALU slot 1, count 1.
    UINT32_C(0x00000000),
    UINT32_C(0x1001C400),
    UINT32_C(0x28000000),
    // max oC0.1111, r0, r0 + retain_prev (the overlapping masks encode 1).
    UINT32_C(0xC8FF8000),
    UINT32_C(0x00000000),
    UINT32_C(0xE2000000),
};

// Synthetic Xenos vertex shader using the same constant-one ALU operation as
// the pixel fixture, but allocating and exporting oPos. It intentionally has
// no title data, fetches, interpolators or memory exports.
constexpr std::array<uint32_t, 6> kSyntheticGuestVertexShader = {
    // alloc position 1; exece at ALU slot 1, count 1.
    UINT32_C(0x00000000),
    UINT32_C(0x1001C200),
    UINT32_C(0x28000000),
    // max oPos.1111, r0, r0 + retain_prev.
    UINT32_C(0xC8FF803E),
    UINT32_C(0x00000000),
    UINT32_C(0xE2000000),
};

struct SyntheticGuestColorTranslation {
  std::unique_ptr<rex::graphics::metal::MetalShader> shader;
  rex::graphics::metal::MetalShader::MetalTranslation* translation = nullptr;
  uint32_t system_constants_binding = UINT32_MAX;
  uint32_t edram_binding = UINT32_MAX;
};

struct SyntheticGuestVertexTranslation {
  std::unique_ptr<rex::graphics::metal::MetalShader> shader;
  rex::graphics::metal::MetalShader::MetalTranslation* translation = nullptr;
};

bool CreateSyntheticGuestVertexTranslation(rex::graphics::SpirvShaderTranslator& translator,
                                           id<MTLDevice> device,
                                           SyntheticGuestVertexTranslation& result_out,
                                           std::string& error_out) {
  using rex::graphics::metal::MetalShader;
  SyntheticGuestVertexTranslation candidate;
  candidate.shader = std::make_unique<MetalShader>(
      rex::graphics::xenos::ShaderType::kVertex, UINT64_C(0x45584143544F4D32),
      kSyntheticGuestVertexShader.data(), kSyntheticGuestVertexShader.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  candidate.shader->AnalyzeUcode(disassembly);
  if (!candidate.shader->writes_position() || candidate.shader->memexport_eM_written() ||
      candidate.shader->writes_interpolators()) {
    error_out = "synthetic guest vertex shader analysis contract mismatch";
    return false;
  }

  const uint64_t modification = translator.GetDefaultVertexShaderModification(
      0, rex::graphics::Shader::HostVertexShaderType::kPointListAsTriangleStrip);
  bool is_new = false;
  candidate.translation =
      candidate.shader->GetOrCreateExactOutputMergerTranslation(modification, &is_new);
  if (!candidate.translation || !is_new ||
      !translator.TranslateAnalyzedShader(*candidate.translation) ||
      !candidate.translation->TranslateMslFromSpirv() ||
      candidate.translation->mode() != MetalShader::TranslationMode::kExactOutputMerger ||
      candidate.translation->msl_reflection().exact_output_merger_contract ||
      !candidate.translation->CompileMslLibrary(device, &error_out)) {
    if (error_out.empty()) {
      error_out = "synthetic guest vertex shader exact translation failed";
    }
    return false;
  }
  result_out = std::move(candidate);
  return true;
}

bool CreateSyntheticGuestColorTranslation(rex::graphics::SpirvShaderTranslator& translator,
                                          id<MTLDevice> device,
                                          SyntheticGuestColorTranslation& result_out,
                                          std::string& error_out) {
  using rex::graphics::metal::MetalShader;
  SyntheticGuestColorTranslation candidate;
  candidate.shader = std::make_unique<MetalShader>(
      rex::graphics::xenos::ShaderType::kPixel, UINT64_C(0x45584143544F4D31),
      kSyntheticGuestColorShader.data(), kSyntheticGuestColorShader.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  candidate.shader->AnalyzeUcode(disassembly);
  if (candidate.shader->memexport_eM_written() || candidate.shader->writes_color_targets() != 1) {
    error_out = "synthetic guest color shader analysis contract mismatch";
    return false;
  }

  const uint64_t modification = translator.GetDefaultPixelShaderModification(0);
  bool is_new = false;
  candidate.translation =
      candidate.shader->GetOrCreateExactOutputMergerTranslation(modification, &is_new);
  if (!candidate.translation || !is_new ||
      !translator.TranslateAnalyzedShader(*candidate.translation) ||
      !candidate.translation->TranslateMslFromSpirv()) {
    error_out = "synthetic guest color shader exact translation failed";
    return false;
  }
  const auto& reflection = candidate.translation->msl_reflection();
  if (candidate.translation->mode() != MetalShader::TranslationMode::kExactOutputMerger ||
      !reflection.exact_output_merger_contract || !reflection.edram_raster_order_group ||
      reflection.edram_buffer_index == UINT32_MAX ||
      !FindNamedBufferBinding(candidate.translation->msl_source(), "xe_uniform_system_constants",
                              candidate.system_constants_binding)) {
    error_out = "synthetic guest color shader reflection contract mismatch";
    return false;
  }
  candidate.edram_binding = reflection.edram_buffer_index;
  if (!candidate.translation->CompileMslLibrary(device, &error_out)) {
    return false;
  }
  result_out = std::move(candidate);
  return true;
}

bool RunProductionTranslationPipelineCase(void* resources, id<MTLDevice> device,
                                          uint32_t sample_count,
                                          SyntheticGuestVertexTranslation& vertex,
                                          SyntheticGuestColorTranslation& fragment,
                                          std::string& error_out) {
  using rex::graphics::metal::ExactOutputMergerBorrowedDraw;
  using rex::graphics::metal::ExactOutputMergerExecutionResult;
  using rex::graphics::metal::ExactOutputMergerPipelineLayout;
  using rex::graphics::metal::ExactOutputMergerUnroutableDraw;
  using rex::graphics::metal::ProbeIndexBuffer;

  if (sample_count == 1) {
    const uint64_t source_generation = fragment.translation->msl_source_generation();
    if (!fragment.translation->TranslateMslFromSpirv() ||
        fragment.translation->msl_source_generation() <= source_generation ||
        fragment.translation->metal_library_is_current()) {
      error_out = "regenerated exact translation retained a stale Metal library";
      return false;
    }
  }

  auto* pipeline = rex::graphics::metal::CreateExactOutputMergerPipelineFromTranslations(
      resources, *vertex.translation, *fragment.translation, sample_count, &error_out);
  ExactOutputMergerPipelineLayout layout;
  bool succeeded = pipeline && fragment.translation->metal_library_is_current_for_device(device) &&
                   vertex.translation->metal_library_is_current_for_device(device) &&
                   rex::graphics::metal::GetExactOutputMergerPipelineLayout(pipeline, layout) &&
                   layout.vertex.system_constants_buffer_index != UINT32_MAX &&
                   layout.fragment.system_constants_buffer_index != UINT32_MAX &&
                   layout.edram_fragment_buffer_index == fragment.edram_binding &&
                   layout.vertex.shared_memory_buffer_index != UINT32_MAX &&
                   layout.fragment.shared_memory_buffer_index == UINT32_MAX &&
                   layout.vertex.float_constants_buffer_index == UINT32_MAX &&
                   layout.fragment.float_constants_buffer_index == UINT32_MAX &&
                   layout.vertex.fetch_constants_buffer_index == UINT32_MAX &&
                   layout.fragment.fetch_constants_buffer_index == UINT32_MAX &&
                   layout.vertex.bool_loop_constants_buffer_index == UINT32_MAX &&
                   layout.fragment.bool_loop_constants_buffer_index == UINT32_MAX &&
                   !layout.vertex.texture_count && !layout.fragment.texture_count &&
                   !layout.vertex.sampler_count && !layout.fragment.sampler_count;
  if (!succeeded) {
    rex::graphics::metal::ReleaseExactOutputMergerPipeline(pipeline);
    if (error_out.empty()) {
      char message[512];
      std::snprintf(
          message, sizeof(message),
          "production exact translation pipeline layout mismatch "
          "v(sys=%u shared=%u float=%u fetch=%u bool=%u vertex=%u tex=%u samp=%u) "
          "f(sys=%u shared=%u float=%u fetch=%u bool=%u tex=%u samp=%u) edram=%u",
          layout.vertex.system_constants_buffer_index, layout.vertex.shared_memory_buffer_index,
          layout.vertex.float_constants_buffer_index, layout.vertex.fetch_constants_buffer_index,
          layout.vertex.bool_loop_constants_buffer_index, layout.vertex.vertex_data_buffer_index,
          layout.vertex.texture_count, layout.vertex.sampler_count,
          layout.fragment.system_constants_buffer_index, layout.fragment.shared_memory_buffer_index,
          layout.fragment.float_constants_buffer_index,
          layout.fragment.fetch_constants_buffer_index,
          layout.fragment.bool_loop_constants_buffer_index, layout.fragment.texture_count,
          layout.fragment.sampler_count, layout.edram_fragment_buffer_index);
      error_out = message;
    }
    return false;
  }
  id<MTLBuffer> shared_memory = [device newBufferWithLength:rex::graphics::SharedMemory::kBufferSize
                                                    options:MTLResourceStorageModePrivate];
  if (!shared_memory) {
    rex::graphics::metal::ReleaseExactOutputMergerPipeline(pipeline);
    error_out = "production exact translation shared-memory allocation failed";
    return false;
  }

  ExactEdramImage baseline = MakeExactEdramImage(UINT32_C(0x6B8B4567));
  ExactEdramImage after = MakeExactEdramImage();
  ExactOutputMergerUnroutableDraw probe_draw;
  probe_draw.pipeline = pipeline;
  probe_draw.coverage_width = 1;
  probe_draw.coverage_height = 1;
  probe_draw.sample_count = sample_count;
  probe_draw.vertex_count = 3;
  std::string rejection;
  succeeded = UploadExactEdramImage(resources, baseline, error_out) &&
              rex::graphics::metal::ExecuteExactOutputMergerUnroutableDraw(resources, probe_draw,
                                                                           &rejection) ==
                  ExactOutputMergerExecutionResult::kRejectedBeforeSubmit &&
              !rejection.empty() && DownloadExactEdramImage(resources, after, error_out) &&
              after == baseline;

  rex::graphics::metal::ExactOutputMergerDrawStateInput state_input;
  state_input.surface_pitch_pixels = 80;
  state_input.coverage_width = 1;
  state_input.coverage_height = 1;
  state_input.msaa_samples =
      static_cast<rex::graphics::xenos::MsaaSamples>(sample_count == 4   ? 2
                                                     : sample_count == 2 ? 1
                                                                         : 0);
  state_input.base_system_constants.point_constant_diameter[0] = 1.0f;
  state_input.base_system_constants.point_constant_diameter[1] = 1.0f;
  state_input.base_system_constants.point_screen_diameter_to_ndc_radius[0] = 1.0f;
  state_input.base_system_constants.point_screen_diameter_to_ndc_radius[1] = 1.0f;
  state_input.color_targets[0].base_tiles = 17;
  state_input.color_targets[0].write_mask = 0xF;
  state_input.primitive_polygonal = false;
  rex::graphics::metal::ExactOutputMergerDrawState state;
  succeeded = succeeded &&
              rex::graphics::metal::BuildExactOutputMergerDrawState(state_input, state, &error_out);
  if (succeeded) {
    std::array<size_t, 4> target_indices = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
    for (uint32_t sample = 0; sample < sample_count; ++sample) {
      if (!rex::graphics::metal::GetExactOutputMergerDwordIndex(
              state.color_surfaces[0], 0, 0, sample, 0, target_indices[sample]) ||
          IsTargetIndex(target_indices, sample, target_indices[sample])) {
        error_out = "production exact draw generated invalid sample addresses";
        succeeded = false;
        break;
      }
      baseline[target_indices[sample]] = UINT32_C(0x10203040);
    }
    const size_t guard_index =
        succeeded ? FindGuardIndex(target_indices, sample_count, target_indices[0]) : SIZE_MAX;
    if (succeeded && guard_index == SIZE_MAX) {
      error_out = "production exact draw could not reserve a guard word";
      succeeded = false;
    }
    if (succeeded) {
      baseline[guard_index] = UINT32_C(0xA55AA55A);
    }

    ExactOutputMergerBorrowedDraw draw;
    draw.pipeline = pipeline;
    draw.pipeline_layout = layout;
    draw.coverage_width = 1;
    draw.coverage_height = 1;
    draw.sample_count = sample_count;
    draw.system_constants = &state.system_constants;
    draw.system_constants_size = sizeof(state.system_constants);
    draw.shared_memory_metal_buffer = shared_memory;
    draw.shared_memory_size = rex::graphics::SharedMemory::kBufferSize;
    draw.primitive_type = uint32_t(rex::graphics::xenos::PrimitiveType::kTriangleStrip);
    const std::array<uint32_t, 4> expanded_point_indices = {0, 1, 2, 3};
    ProbeIndexBuffer expanded_point_index_buffer;
    expanded_point_index_buffer.data = expanded_point_indices.data();
    expanded_point_index_buffer.size = sizeof(expanded_point_indices);
    expanded_point_index_buffer.index_size = sizeof(uint32_t);
    expanded_point_index_buffer.production_host_data_trusted = true;
    draw.index_buffer = &expanded_point_index_buffer;
    draw.primitive_restart_enabled = true;
    draw.primitive_restart_index = UINT32_MAX;
    draw.vertex_count = 4;
    auto expect_rejected_without_edram_change =
        [&](const ExactOutputMergerBorrowedDraw& invalid_draw) {
          std::string invalid_error;
          return UploadExactEdramImage(resources, baseline, error_out) &&
                 rex::graphics::metal::ExecuteExactOutputMergerDraw(resources, invalid_draw,
                                                                    &invalid_error) ==
                     ExactOutputMergerExecutionResult::kRejectedBeforeSubmit &&
                 !invalid_error.empty() && DownloadExactEdramImage(resources, after, error_out) &&
                 after == baseline;
        };
    ExactOutputMergerBorrowedDraw invalid_draw = draw;
    invalid_draw.base_vertex = 1;
    succeeded = succeeded && expect_rejected_without_edram_change(invalid_draw);
    invalid_draw = draw;
    invalid_draw.base_instance = 1;
    succeeded = succeeded && expect_rejected_without_edram_change(invalid_draw);
    auto mismatched_system_constants = state.system_constants;
    constexpr uint32_t kMsaaMask =
        ((uint32_t(1) << rex::graphics::xenos::kMsaaSamplesBits) - 1)
        << rex::graphics::SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
    const uint32_t mismatched_msaa = sample_count == 1 ? 1 : 0;
    mismatched_system_constants.flags =
        (mismatched_system_constants.flags & ~kMsaaMask) |
        (mismatched_msaa << rex::graphics::SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift);
    invalid_draw = draw;
    invalid_draw.system_constants = &mismatched_system_constants;
    succeeded = succeeded && expect_rejected_without_edram_change(invalid_draw);
    succeeded = succeeded && UploadExactEdramImage(resources, baseline, error_out) &&
                rex::graphics::metal::ExecuteExactOutputMergerDraw(resources, draw, &error_out) ==
                    ExactOutputMergerExecutionResult::kEnqueued &&
                DownloadExactEdramImage(resources, after, error_out);
    for (uint32_t sample = 0; succeeded && sample < sample_count; ++sample) {
      if (after[target_indices[sample]] != UINT32_MAX) {
        char message[192];
        std::snprintf(message, sizeof(message),
                      "production exact point write mismatch sample=%u at %ux "
                      "expected=FFFFFFFF actual=%08X",
                      sample, sample_count, after[target_indices[sample]]);
        error_out = message;
        succeeded = false;
      }
    }
    if (succeeded && after[guard_index] != UINT32_C(0xA55AA55A)) {
      error_out = "production exact point write modified a guard word";
      succeeded = false;
    }
  }

  if (sample_count == 1) {
    std::string wrong_stage_rejection;
    auto* wrong_stage = rex::graphics::metal::CreateExactOutputMergerPipelineFromTranslations(
        resources, *fragment.translation, *fragment.translation, sample_count,
        &wrong_stage_rejection);
    succeeded = succeeded && !wrong_stage && !wrong_stage_rejection.empty();
    rex::graphics::metal::ReleaseExactOutputMergerPipeline(wrong_stage);
  }
  [shared_memory release];
  rex::graphics::metal::ReleaseExactOutputMergerPipeline(pipeline);
  if (!succeeded && error_out.empty()) {
    error_out = "production exact translation pipeline contract failed";
  }
  return succeeded;
}

bool RunLayoutCases(std::string& error_out) {
  using rex::graphics::metal::ApplyExactOutputMergerAddressConstants;
  using rex::graphics::metal::BuildExactOutputMergerColorConstants;
  using rex::graphics::metal::BuildExactOutputMergerDepthConstants;
  using rex::graphics::metal::ExactOutputMergerSurfaceConstants;
  using rex::graphics::metal::GetCanonicalEdramDwordIndex;
  using rex::graphics::metal::GetExactOutputMergerDwordIndex;
  using rex::graphics::xenos::DepthRenderTargetFormat;
  using rex::graphics::xenos::MsaaSamples;

  constexpr std::array<MsaaSamples, 3> kMsaa = {MsaaSamples::k1X, MsaaSamples::k2X,
                                                MsaaSamples::k4X};
  constexpr std::array<std::array<uint32_t, 2>, 8> kCoordinates = {{
      {0, 0},
      {39, 0},
      {40, 0},
      {79, 15},
      {80, 16},
      {119, 16},
      {120, 31},
      {159, 32},
  }};

  for (MsaaSamples msaa : kMsaa) {
    for (auto format : kDefinedColorFormats) {
      ExactOutputMergerSurfaceConstants constants;
      if (!BuildExactOutputMergerColorConstants(rex::graphics::xenos::kEdramTileCount - 1, 160, 160,
                                                33, msaa, format, constants, &error_out)) {
        return false;
      }
      const bool is_64bpp = rex::graphics::xenos::IsColorRenderTargetFormat64bpp(format);
      const uint32_t expected_pitch_tiles =
          rex::graphics::xenos::GetSurfacePitchTiles(160, msaa, is_64bpp);
      const uint32_t expected_32bpp_pitch_tiles =
          rex::graphics::xenos::GetSurfacePitchTiles(160, msaa, false);
      if (constants.canonical_layout.pitch_tiles != expected_pitch_tiles ||
          constants.words_per_sample != (is_64bpp ? 2u : 1u) ||
          constants.tile_pitch_dwords != expected_32bpp_pitch_tiles * 80u * 16u ||
          constants.base_dwords != (rex::graphics::xenos::kEdramTileCount - 1) * 80u * 16u) {
        error_out = "exact output-merger color constant derivation mismatch";
        return false;
      }

      rex::graphics::SpirvShaderTranslator::SystemConstants system = {};
      system.flags = UINT32_C(0xFFFFFFFF);
      if (!ApplyExactOutputMergerAddressConstants(constants, 3, system, &error_out)) {
        return false;
      }
      constexpr uint32_t kMsaaMask =
          ((uint32_t(1) << rex::graphics::xenos::kMsaaSamplesBits) - 1)
          << rex::graphics::SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
      if (((system.flags & kMsaaMask) >>
           rex::graphics::SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift) != uint32_t(msaa) ||
          system.edram_32bpp_tile_pitch_dwords_scaled != constants.tile_pitch_dwords ||
          system.edram_rt_base_dwords_scaled[3] != constants.base_dwords ||
          system.edram_rt_format_flags[3] != constants.color_format_flags) {
        error_out = "exact output-merger system constant application mismatch";
        return false;
      }

      for (const auto& coordinate : kCoordinates) {
        for (uint32_t sample = 0; sample < constants.sample_count; ++sample) {
          for (uint32_t dword = 0; dword < constants.words_per_sample; ++dword) {
            size_t actual = SIZE_MAX;
            if (!GetExactOutputMergerDwordIndex(constants, coordinate[0], coordinate[1], sample,
                                                dword, actual)) {
              error_out = "valid exact output-merger coordinate was rejected";
              return false;
            }
            size_t expected = GetCanonicalEdramDwordIndex(constants.canonical_layout, coordinate[0],
                                                          coordinate[1], sample, dword);
            if (actual != expected ||
                actual >= rex::graphics::metal::kExactOutputMergerEdramDwordCount) {
              error_out = "exact output-merger canonical address mismatch";
              return false;
            }
          }
        }
      }
      size_t rejected = 0;
      if (GetExactOutputMergerDwordIndex(constants, constants.width, 0, 0, 0, rejected) ||
          GetExactOutputMergerDwordIndex(constants, 0, constants.height, 0, 0, rejected) ||
          GetExactOutputMergerDwordIndex(constants, 0, 0, constants.sample_count, 0, rejected) ||
          GetExactOutputMergerDwordIndex(constants, 0, 0, 0, constants.words_per_sample,
                                         rejected)) {
        error_out = "exact output-merger coordinate bounds did not fail closed";
        return false;
      }
    }

    for (DepthRenderTargetFormat format :
         {DepthRenderTargetFormat::kD24S8, DepthRenderTargetFormat::kD24FS8}) {
      ExactOutputMergerSurfaceConstants constants;
      if (!BuildExactOutputMergerDepthConstants(rex::graphics::xenos::kEdramTileCount - 1, 160, 160,
                                                33, msaa, format, constants, &error_out)) {
        return false;
      }
      for (const auto& coordinate : kCoordinates) {
        for (uint32_t sample = 0; sample < constants.sample_count; ++sample) {
          size_t actual = SIZE_MAX;
          if (!GetExactOutputMergerDwordIndex(constants, coordinate[0], coordinate[1], sample, 0,
                                              actual) ||
              actual != GetCanonicalEdramDwordIndex(constants.canonical_layout, coordinate[0],
                                                    coordinate[1], sample, 0)) {
            error_out = "exact output-merger depth address mismatch";
            return false;
          }
        }
      }
    }
  }

  // Two different logical layouts must converge on the same physical word
  // when one crosses tile 2047. This is the aliasing behavior that independent
  // host render targets can't preserve.
  ExactOutputMergerSurfaceConstants wrapping;
  ExactOutputMergerSurfaceConstants zero_based;
  size_t wrapped_index = SIZE_MAX;
  size_t zero_index = SIZE_MAX;
  if (!BuildExactOutputMergerColorConstants(rex::graphics::xenos::kEdramTileCount - 1, 80, 80, 17,
                                            MsaaSamples::k1X, kDefinedColorFormats[0], wrapping,
                                            &error_out) ||
      !BuildExactOutputMergerColorConstants(0, 80, 80, 1, MsaaSamples::k1X, kDefinedColorFormats[0],
                                            zero_based, &error_out) ||
      !GetExactOutputMergerDwordIndex(wrapping, 0, 16, 0, 0, wrapped_index) ||
      !GetExactOutputMergerDwordIndex(zero_based, 0, 0, 0, 0, zero_index) ||
      wrapped_index != zero_index) {
    error_out = "exact output-merger physical wrap/alias contract mismatch";
    return false;
  }

  ExactOutputMergerSurfaceConstants invalid;
  std::string rejection;
  if (BuildExactOutputMergerColorConstants(rex::graphics::xenos::kEdramTileCount, 80, 80, 1,
                                           MsaaSamples::k1X, kDefinedColorFormats[0], invalid,
                                           &rejection) ||
      BuildExactOutputMergerColorConstants(0, 0, 1, 1, MsaaSamples::k1X, kDefinedColorFormats[0],
                                           invalid, &rejection) ||
      BuildExactOutputMergerColorConstants(0, 80, 81, 1, MsaaSamples::k1X, kDefinedColorFormats[0],
                                           invalid, &rejection) ||
      BuildExactOutputMergerColorConstants(0, 80, 80, 0, MsaaSamples::k1X, kDefinedColorFormats[0],
                                           invalid, &rejection) ||
      BuildExactOutputMergerColorConstants(0, 80, 80, 1, static_cast<MsaaSamples>(3),
                                           kDefinedColorFormats[0], invalid, &rejection) ||
      BuildExactOutputMergerColorConstants(
          0, 80, 80, 1, MsaaSamples::k1X,
          static_cast<rex::graphics::xenos::ColorRenderTargetFormat>(9), invalid, &rejection) ||
      BuildExactOutputMergerDepthConstants(0, 80, 80, 1, MsaaSamples::k1X,
                                           static_cast<DepthRenderTargetFormat>(2), invalid,
                                           &rejection)) {
    error_out = "invalid exact output-merger layout did not fail closed";
    return false;
  }
  rex::graphics::SpirvShaderTranslator::SystemConstants invalid_system = {};
  if (ApplyExactOutputMergerAddressConstants(invalid, 0, invalid_system, &rejection)) {
    error_out = "invalid exact output-merger constants were applied";
    return false;
  }

  ExactOutputMergerSurfaceConstants valid_for_transaction;
  if (!BuildExactOutputMergerColorConstants(7, 80, 80, 1, MsaaSamples::k1X, kDefinedColorFormats[0],
                                            valid_for_transaction, &error_out)) {
    return false;
  }
  std::memset(&invalid_system, 0xA5, sizeof(invalid_system));
  const auto system_before_rejection = invalid_system;
  if (ApplyExactOutputMergerAddressConstants(valid_for_transaction,
                                             rex::graphics::xenos::kMaxColorRenderTargets,
                                             invalid_system, &rejection) ||
      std::memcmp(&invalid_system, &system_before_rejection, sizeof(invalid_system)) != 0) {
    error_out = "rejected exact output-merger target mutated system constants";
    return false;
  }
  ++valid_for_transaction.tile_pitch_dwords;
  if (ApplyExactOutputMergerAddressConstants(valid_for_transaction, 0, invalid_system,
                                             &rejection) ||
      std::memcmp(&invalid_system, &system_before_rejection, sizeof(invalid_system)) != 0) {
    error_out = "tampered exact output-merger layout mutated system constants";
    return false;
  }

  uint32_t parsed_binding = UINT32_MAX;
  const std::string valid_binding = "fragment void f(coherent device XeEdram& xe_edram "
                                    "[[buffer(7), raster_order_group(0)]]) {}\n";
  const std::string duplicate_binding = valid_binding + valid_binding;
  const std::string comment_then_valid_binding = "// coherent device Fake& xe_edram [[buffer(2), "
                                                 "raster_order_group(0)]]\n" +
                                                 valid_binding;
  if (!rex::graphics::metal::FindExactOutputMergerEdramBinding(valid_binding, parsed_binding,
                                                               &rejection) ||
      parsed_binding != 7 ||
      !rex::graphics::metal::FindExactOutputMergerEdramBinding(comment_then_valid_binding,
                                                               parsed_binding, &rejection) ||
      parsed_binding != 7 ||
      rex::graphics::metal::FindExactOutputMergerEdramBinding(
          "fragment void f(device XeEdram& xe_edram [[buffer(1), "
          "raster_order_group(0)]]) {}\n",
          parsed_binding, &rejection) ||
      rex::graphics::metal::FindExactOutputMergerEdramBinding(
          "fragment void f(coherent device Other& other [[buffer(0), "
          "raster_order_group(0)]], device XeEdram& xe_edram "
          "[[buffer(1)]]) {}\n",
          parsed_binding, &rejection) ||
      rex::graphics::metal::FindExactOutputMergerEdramBinding(
          "fragment void f(/* coherent */ device XeEdram& xe_edram "
          "[[buffer(1), raster_order_group(0)]]) {}\n",
          parsed_binding, &rejection) ||
      rex::graphics::metal::FindExactOutputMergerEdramBinding(
          "fragment void f(coherent device XeEdram& xe_edram "
          "[[buffer(1) /*, raster_order_group(0) */]]) {}\n",
          parsed_binding, &rejection) ||
      rex::graphics::metal::FindExactOutputMergerEdramBinding(
          "fragment void f(coherent device XeEdram& xe_edram "
          "[[buffer(1), buffer(2), raster_order_group(0)]]) {}\n",
          parsed_binding, &rejection) ||
      rex::graphics::metal::FindExactOutputMergerEdramBinding(
          "fragment void f(coherent device XeEdram& xe_edram [[buffer(1), "
          "raster_order_group(1)]]) {}\n",
          parsed_binding, &rejection) ||
      rex::graphics::metal::FindExactOutputMergerEdramBinding(
          "fragment void f(coherent device XeEdram& xe_edram [[buffer(31), "
          "raster_order_group(0)]]) {}\n",
          parsed_binding, &rejection) ||
      rex::graphics::metal::FindExactOutputMergerEdramBinding(duplicate_binding, parsed_binding,
                                                              &rejection) ||
      rex::graphics::metal::FindExactOutputMergerEdramBinding("fragment void f() {}\n",
                                                              parsed_binding, &rejection)) {
    error_out = "exact output-merger binding parser did not fail closed";
    return false;
  }
  return true;
}

bool RunCoverageResourceCases(id<MTLDevice> device, id<MTLCommandQueue> pinned_queue,
                              void* resources, std::string& error_out) {
  std::string rejection;
  ExactEdramImage upload = MakeExactEdramImage(UINT32_C(0xC001D00D));
  ExactEdramImage download = MakeExactEdramImage();
  if (rex::graphics::metal::CreateExactOutputMergerResources(nullptr, &rejection) ||
      rex::graphics::metal::GetExactOutputMergerEdramBufferLength(nullptr) != 0 ||
      rex::graphics::metal::UploadExactOutputMergerEdram(
          nullptr, upload.data(), upload.size() * sizeof(uint32_t), &rejection) ||
      rex::graphics::metal::DownloadExactOutputMergerEdram(
          nullptr, download.data(), download.size() * sizeof(uint32_t), &rejection) ||
      rex::graphics::metal::ConfigureExactOutputMergerCoveragePipeline(nullptr, 1, &rejection) ||
      rex::graphics::metal::ConfigureExactOutputMergerCoveragePass(resources, nullptr, 1, 1, 1,
                                                                   &rejection) ||
      rex::graphics::metal::SetExactOutputMergerCommandQueue(resources, nullptr, &rejection)) {
    error_out = "null exact output-merger resource arguments were accepted";
    return false;
  }

  id<MTLCommandQueue> replacement_queue = [device newCommandQueue];
  const bool queue_contract_ok =
      pinned_queue && replacement_queue &&
      rex::graphics::metal::SetExactOutputMergerCommandQueue(resources, pinned_queue, &error_out) &&
      !rex::graphics::metal::SetExactOutputMergerCommandQueue(resources, replacement_queue,
                                                              &rejection) &&
      rex::graphics::metal::SetExactOutputMergerCommandQueue(resources, pinned_queue, &error_out);
  [replacement_queue release];
  if (!queue_contract_ok) {
    error_out = "exact output-merger command queue identity was not pinned";
    return false;
  }

  if (rex::graphics::metal::UploadExactOutputMergerEdram(
          resources, upload.data(), upload.size() * sizeof(uint32_t) - 1, &rejection) ||
      rex::graphics::metal::DownloadExactOutputMergerEdram(
          resources, download.data(), download.size() * sizeof(uint32_t) - 1, &rejection) ||
      !UploadExactEdramImage(resources, upload, error_out) ||
      !DownloadExactEdramImage(resources, download, error_out) || upload != download) {
    error_out = "exact output-merger full-image transfer contract failed";
    return false;
  }

  for (uint32_t sample_count : {1u, 2u, 4u}) {
    if (![device supportsTextureSampleCount:sample_count]) {
      error_out = "required exact output-merger sample count is unsupported";
      return false;
    }
    MTLRenderPipelineDescriptor* pipeline_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    pipeline_descriptor.alphaToCoverageEnabled = YES;
    pipeline_descriptor.alphaToOneEnabled = YES;
    pipeline_descriptor.rasterizationEnabled = NO;
    pipeline_descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    pipeline_descriptor.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
    pipeline_descriptor.supportIndirectCommandBuffers = YES;
    for (NSUInteger i = 0; i < 8; ++i) {
      pipeline_descriptor.colorAttachments[i].pixelFormat = MTLPixelFormatBGRA8Unorm;
      pipeline_descriptor.colorAttachments[i].blendingEnabled = YES;
      pipeline_descriptor.colorAttachments[i].writeMask = MTLColorWriteMaskAll;
      pipeline_descriptor.colorAttachments[i].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
      pipeline_descriptor.colorAttachments[i].destinationRGBBlendFactor =
          MTLBlendFactorOneMinusSourceAlpha;
    }
    bool pipeline_ok =
        rex::graphics::metal::ConfigureExactOutputMergerCoveragePipeline(
            pipeline_descriptor, sample_count, &error_out) &&
        pipeline_descriptor.rasterSampleCount == sample_count &&
        !pipeline_descriptor.alphaToCoverageEnabled && !pipeline_descriptor.alphaToOneEnabled &&
        pipeline_descriptor.rasterizationEnabled &&
        pipeline_descriptor.depthAttachmentPixelFormat == MTLPixelFormatInvalid &&
        pipeline_descriptor.stencilAttachmentPixelFormat == MTLPixelFormatInvalid &&
        !pipeline_descriptor.supportIndirectCommandBuffers &&
        pipeline_descriptor.colorAttachments[0].pixelFormat == MTLPixelFormatBGRA8Unorm &&
        pipeline_descriptor.colorAttachments[0].writeMask == MTLColorWriteMaskNone;
    for (NSUInteger i = 0; i < 8; ++i) {
      MTLRenderPipelineColorAttachmentDescriptor* attachment =
          pipeline_descriptor.colorAttachments[i];
      pipeline_ok =
          pipeline_ok &&
          attachment.pixelFormat == (i ? MTLPixelFormatInvalid : MTLPixelFormatBGRA8Unorm) &&
          !attachment.blendingEnabled && attachment.writeMask == MTLColorWriteMaskNone &&
          attachment.sourceRGBBlendFactor == MTLBlendFactorOne &&
          attachment.destinationRGBBlendFactor == MTLBlendFactorZero &&
          attachment.rgbBlendOperation == MTLBlendOperationAdd &&
          attachment.sourceAlphaBlendFactor == MTLBlendFactorOne &&
          attachment.destinationAlphaBlendFactor == MTLBlendFactorZero &&
          attachment.alphaBlendOperation == MTLBlendOperationAdd;
    }
    [pipeline_descriptor release];
    if (!pipeline_ok) {
      error_out = "exact output-merger coverage pipeline contract mismatch";
      return false;
    }

    MTLRenderPassDescriptor* render_pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (!rex::graphics::metal::ConfigureExactOutputMergerCoveragePass(resources, render_pass, 3, 2,
                                                                      sample_count, &error_out)) {
      return false;
    }
    id<MTLTexture> first_texture = render_pass.colorAttachments[0].texture;
    if (!first_texture || first_texture.width != 3 || first_texture.height != 2 ||
        first_texture.sampleCount != sample_count ||
        first_texture.pixelFormat != MTLPixelFormatBGRA8Unorm ||
        first_texture.usage != MTLTextureUsageRenderTarget) {
      error_out = "exact output-merger coverage attachment contract mismatch";
      return false;
    }

    for (NSUInteger i = 0; i < 8; ++i) {
      render_pass.colorAttachments[i].texture = first_texture;
      render_pass.colorAttachments[i].resolveTexture = first_texture;
      render_pass.colorAttachments[i].level = 7;
      render_pass.colorAttachments[i].slice = 5;
      render_pass.colorAttachments[i].depthPlane = 3;
      render_pass.colorAttachments[i].loadAction = MTLLoadActionClear;
      render_pass.colorAttachments[i].storeAction = MTLStoreActionStore;
      render_pass.colorAttachments[i].clearColor = MTLClearColorMake(1.0, 2.0, 3.0, 4.0);
    }
    render_pass.depthAttachment.texture = first_texture;
    render_pass.depthAttachment.resolveTexture = first_texture;
    render_pass.depthAttachment.loadAction = MTLLoadActionClear;
    render_pass.depthAttachment.storeAction = MTLStoreActionStore;
    render_pass.depthAttachment.clearDepth = 0.25;
    render_pass.stencilAttachment.texture = first_texture;
    render_pass.stencilAttachment.resolveTexture = first_texture;
    render_pass.stencilAttachment.loadAction = MTLLoadActionClear;
    render_pass.stencilAttachment.storeAction = MTLStoreActionStore;
    render_pass.stencilAttachment.clearStencil = 0xA5;
    id<MTLBuffer> stale_visibility = [device newBufferWithLength:64
                                                         options:MTLResourceStorageModeShared];
    render_pass.visibilityResultBuffer = stale_visibility;
    render_pass.renderTargetArrayLength = 3;
    render_pass.imageblockSampleLength = 64;
    render_pass.threadgroupMemoryLength = 128;
    render_pass.tileWidth = 8;
    render_pass.tileHeight = 8;
    render_pass.defaultRasterSampleCount = sample_count;
    render_pass.renderTargetWidth = 2;
    render_pass.renderTargetHeight = 1;
    const std::array<MTLSamplePosition, 4> stale_sample_positions = {{
        {0.125f, 0.125f},
        {0.375f, 0.375f},
        {0.625f, 0.625f},
        {0.875f, 0.875f},
    }};
    if (sample_count > 1) {
      [render_pass setSamplePositions:stale_sample_positions.data() count:sample_count];
    }

    const bool pass_reconfigured = rex::graphics::metal::ConfigureExactOutputMergerCoveragePass(
        resources, render_pass, 3, 2, sample_count, &error_out);
    std::array<MTLSamplePosition, 4> sanitized_sample_positions = {};
    const NSUInteger sanitized_sample_position_count = [render_pass getSamplePositions:nullptr
                                                                                 count:0];
    if (sanitized_sample_position_count) {
      [render_pass getSamplePositions:sanitized_sample_positions.data()
                                count:sanitized_sample_position_count];
    }
    const std::array<MTLSamplePosition, 4> expected_2x_positions = {{
        {0.75f, 0.75f},
        {0.25f, 0.25f},
    }};
    const std::array<MTLSamplePosition, 4> expected_4x_positions = {{
        {0.375f, 0.125f},
        {0.875f, 0.375f},
        {0.125f, 0.625f},
        {0.625f, 0.875f},
    }};
    const MTLSamplePosition* expected_sample_positions =
        sample_count == 2 ? expected_2x_positions.data() : expected_4x_positions.data();
    const bool sample_positions_sanitized =
        sample_count == 1
            ? sanitized_sample_position_count == 0
            : sanitized_sample_position_count == sample_count &&
                  std::memcmp(sanitized_sample_positions.data(), expected_sample_positions,
                              sample_count * sizeof(MTLSamplePosition)) == 0;
    bool pass_sanitized =
        pass_reconfigured && sample_positions_sanitized &&
        render_pass.colorAttachments[0].texture == first_texture &&
        render_pass.colorAttachments[0].resolveTexture == nil &&
        render_pass.colorAttachments[0].level == 0 && render_pass.colorAttachments[0].slice == 0 &&
        render_pass.colorAttachments[0].depthPlane == 0 &&
        render_pass.colorAttachments[0].loadAction == MTLLoadActionDontCare &&
        render_pass.colorAttachments[0].storeAction == MTLStoreActionDontCare &&
        render_pass.depthAttachment.texture == nil &&
        render_pass.depthAttachment.resolveTexture == nil &&
        render_pass.depthAttachment.loadAction == MTLLoadActionDontCare &&
        render_pass.depthAttachment.storeAction == MTLStoreActionDontCare &&
        render_pass.depthAttachment.clearDepth == 1.0 &&
        render_pass.stencilAttachment.texture == nil &&
        render_pass.stencilAttachment.resolveTexture == nil &&
        render_pass.stencilAttachment.loadAction == MTLLoadActionDontCare &&
        render_pass.stencilAttachment.storeAction == MTLStoreActionDontCare &&
        render_pass.stencilAttachment.clearStencil == 0 &&
        render_pass.visibilityResultBuffer == nil && render_pass.renderTargetArrayLength == 0 &&
        render_pass.imageblockSampleLength == 0 && render_pass.threadgroupMemoryLength == 0 &&
        render_pass.tileWidth == 0 && render_pass.tileHeight == 0 &&
        render_pass.defaultRasterSampleCount == sample_count &&
        render_pass.renderTargetWidth == 0 && render_pass.renderTargetHeight == 0 &&
        render_pass.rasterizationRateMap == nil;
    for (NSUInteger i = 1; i < 8; ++i) {
      pass_sanitized = pass_sanitized && render_pass.colorAttachments[i].texture == nil &&
                       render_pass.colorAttachments[i].resolveTexture == nil &&
                       render_pass.colorAttachments[i].loadAction == MTLLoadActionDontCare &&
                       render_pass.colorAttachments[i].storeAction == MTLStoreActionDontCare;
    }
    [stale_visibility release];
    if (!pass_sanitized) {
      char message[768];
      std::snprintf(
          message, sizeof(message),
          "coverage pass retained stale state: c0=%d/%d/%lu/%lu/%lu/%lu/%lu "
          "depth=%d/%d/%lu/%lu/%g stencil=%d/%d/%lu/%lu/%u "
          "globals=%d/%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu rate=%d",
          render_pass.colorAttachments[0].texture == first_texture,
          render_pass.colorAttachments[0].resolveTexture == nil,
          render_pass.colorAttachments[0].level, render_pass.colorAttachments[0].slice,
          render_pass.colorAttachments[0].depthPlane,
          NSUInteger(render_pass.colorAttachments[0].loadAction),
          NSUInteger(render_pass.colorAttachments[0].storeAction),
          render_pass.depthAttachment.texture == nil,
          render_pass.depthAttachment.resolveTexture == nil,
          NSUInteger(render_pass.depthAttachment.loadAction),
          NSUInteger(render_pass.depthAttachment.storeAction),
          render_pass.depthAttachment.clearDepth, render_pass.stencilAttachment.texture == nil,
          render_pass.stencilAttachment.resolveTexture == nil,
          NSUInteger(render_pass.stencilAttachment.loadAction),
          NSUInteger(render_pass.stencilAttachment.storeAction),
          render_pass.stencilAttachment.clearStencil, render_pass.visibilityResultBuffer == nil,
          render_pass.renderTargetArrayLength, render_pass.imageblockSampleLength,
          render_pass.threadgroupMemoryLength, render_pass.tileWidth, render_pass.tileHeight,
          render_pass.defaultRasterSampleCount, render_pass.renderTargetWidth,
          render_pass.renderTargetHeight, sanitized_sample_position_count,
          NSUInteger(render_pass.colorAttachments[7].texture != nil),
          render_pass.rasterizationRateMap == nil);
      error_out = message;
      return false;
    }
  }

  MTLRenderPipelineDescriptor* rejected_pipeline = [[MTLRenderPipelineDescriptor alloc] init];
  rejected_pipeline.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
  rejected_pipeline.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
  for (NSUInteger i = 0; i < 8; ++i) {
    rejected_pipeline.colorAttachments[i].pixelFormat = MTLPixelFormatBGRA8Unorm;
    rejected_pipeline.colorAttachments[i].blendingEnabled = YES;
  }
  bool rejected_pipeline_ok =
      !rex::graphics::metal::ConfigureExactOutputMergerCoveragePipeline(rejected_pipeline, 3,
                                                                        &rejection) &&
      rejected_pipeline.rasterSampleCount == 1 &&
      rejected_pipeline.depthAttachmentPixelFormat == MTLPixelFormatInvalid &&
      rejected_pipeline.stencilAttachmentPixelFormat == MTLPixelFormatInvalid;
  for (NSUInteger i = 0; i < 8; ++i) {
    rejected_pipeline_ok =
        rejected_pipeline_ok &&
        rejected_pipeline.colorAttachments[i].pixelFormat == MTLPixelFormatInvalid &&
        !rejected_pipeline.colorAttachments[i].blendingEnabled &&
        rejected_pipeline.colorAttachments[i].writeMask == MTLColorWriteMaskNone;
  }
  [rejected_pipeline release];
  MTLRenderPassDescriptor* rejected_pass = [MTLRenderPassDescriptor renderPassDescriptor];
  id<MTLTexture> rejected_stale_texture = [device
      newTextureWithDescriptor:[MTLTextureDescriptor
                                   texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                width:1
                                                               height:1
                                                            mipmapped:NO]];
  rejected_pass.colorAttachments[7].texture = rejected_stale_texture;
  rejected_pass.depthAttachment.texture = rejected_pass.colorAttachments[7].texture;
  [rejected_stale_texture release];
  const bool rejected_pass_ok = !rex::graphics::metal::ConfigureExactOutputMergerCoveragePass(
                                    resources, rejected_pass, 0, 1, 1, &rejection) &&
                                !rex::graphics::metal::ConfigureExactOutputMergerCoveragePass(
                                    resources, rejected_pass, 1, 0, 1, &rejection) &&
                                !rex::graphics::metal::ConfigureExactOutputMergerCoveragePass(
                                    resources, rejected_pass, 1, 1, 3, &rejection) &&
                                !rex::graphics::metal::ConfigureExactOutputMergerCoveragePass(
                                    resources, rejected_pass, 16385, 1, 1, &rejection) &&
                                rejected_pass.colorAttachments[0].texture == nil &&
                                rejected_pass.colorAttachments[7].texture == nil &&
                                rejected_pass.depthAttachment.texture == nil &&
                                rejected_pass.stencilAttachment.texture == nil;
  if (!rejected_pipeline_ok || !rejected_pass_ok) {
    error_out = "exact output-merger coverage bounds did not fail closed";
    return false;
  }
  return true;
}

bool RunOrderedCases(rex::graphics::metal::ExactOutputMergerPipeline* pipeline, void* resources,
                     uint32_t sample_count, std::string& error_out) {
  ExactEdramImage words = MakeExactEdramImage(UINT32_C(0xDEADC0DE));
  if (rex::graphics::metal::GetExactOutputMergerEdramBufferLength(resources) !=
      rex::graphics::metal::kExactOutputMergerEdramSizeBytes) {
    error_out = "exact output-merger canonical EDRAM resource is invalid";
    return false;
  }

  for (uint32_t mode = 0; mode < 4; ++mode) {
    for (uint32_t repeat = 0; repeat < kRepeatCount; ++repeat) {
      uint32_t salt =
          UINT32_C(0x31415927) ^ (mode * UINT32_C(0x9E3779B9)) ^ (repeat * UINT32_C(0x7F4A7C15));
      uint32_t seed = mode == 3 ? UINT32_C(0xF00000A5) : UINT32_C(0x13579BDF) ^ salt;

      const auto msaa_samples =
          static_cast<rex::graphics::xenos::MsaaSamples>(sample_count == 4   ? 2
                                                         : sample_count == 2 ? 1
                                                                             : 0);
      rex::graphics::metal::ExactOutputMergerSurfaceConstants layout;
      const uint32_t x_positions[] = {0, 39, 40, 79, 80, 119, 120, 159};
      const uint32_t y_positions[] = {0, 15, 16, 31, 32};
      const uint32_t logical_x = x_positions[repeat % std::size(x_positions)];
      const uint32_t logical_y = y_positions[(repeat / 2) % std::size(y_positions)];
      const uint32_t base_tiles = (repeat & 1) ? rex::graphics::xenos::kEdramTileCount - 1 : 37;
      bool layout_ok = false;
      if (mode == 3) {
        layout_ok = rex::graphics::metal::BuildExactOutputMergerDepthConstants(
            base_tiles, 160, 160, 33, msaa_samples,
            (repeat & 1) ? rex::graphics::xenos::DepthRenderTargetFormat::kD24FS8
                         : rex::graphics::xenos::DepthRenderTargetFormat::kD24S8,
            layout, &error_out);
      } else {
        layout_ok = rex::graphics::metal::BuildExactOutputMergerColorConstants(
            base_tiles, 160, 160, 33, msaa_samples,
            kDefinedColorFormats[repeat % kDefinedColorFormats.size()], layout, &error_out);
      }
      if (!layout_ok) {
        return false;
      }

      const uint32_t dword = repeat % layout.words_per_sample;
      std::array<size_t, 4> target_indices = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
      for (uint32_t sample = 0; sample < sample_count; ++sample) {
        if (!rex::graphics::metal::GetExactOutputMergerDwordIndex(
                layout, logical_x, logical_y, sample, dword, target_indices[sample]) ||
            IsTargetIndex(target_indices, sample, target_indices[sample])) {
          error_out = "exact output-merger GPU case generated an invalid alias";
          return false;
        }
        words[target_indices[sample]] = seed;
      }
      const size_t guard_index = FindGuardIndex(target_indices, sample_count, target_indices[0]);
      if (guard_index == SIZE_MAX) {
        error_out = "failed to reserve exact output-merger guard word";
        return false;
      }
      words[guard_index] = UINT32_C(0xDEADC0DE);

      ProbeConstants constants = {
          mode,
          kOperationCount,
          salt,
          sample_count,
          layout.canonical_layout.base_tiles,
          layout.canonical_layout.pitch_tiles,
          logical_x,
          logical_y,
          layout.words_per_sample,
          dword,
          uint32_t(layout.kind ==
                   rex::graphics::metal::ExactOutputMergerSurfaceKind::kDepthStencil),
          0,
      };

      rex::graphics::metal::ExactOutputMergerUnroutableDraw draw;
      draw.pipeline = pipeline;
      draw.coverage_width = 1;
      draw.coverage_height = 1;
      draw.sample_count = sample_count;
      draw.fragment_inline_buffers[0] = {&constants, sizeof(constants), 0};
      draw.fragment_inline_buffer_count = 1;
      draw.vertex_count = kOperationCount * 3u;
      if (!UploadExactEdramImage(resources, words, error_out) ||
          rex::graphics::metal::ExecuteExactOutputMergerUnroutableDraw(resources, draw,
                                                                       &error_out) !=
              rex::graphics::metal::ExactOutputMergerExecutionResult::kCompleted ||
          !DownloadExactEdramImage(resources, words, error_out)) {
        return false;
      }

      uint32_t expected = seed;
      for (uint32_t operation = 0; operation < kOperationCount; ++operation) {
        expected = ApplyReferenceOperation(mode, expected, operation, salt);
      }
      for (uint32_t sample = 0; sample < sample_count; ++sample) {
        const size_t index = target_indices[sample];
        if (words[index] != expected) {
          char message[256];
          std::snprintf(message, sizeof(message),
                        "ordered case mismatch sample_count=%u mode=%u repeat=%u sample=%u "
                        "dword=%zu expected=%08X actual=%08X",
                        sample_count, mode, repeat, sample, index, expected, words[index]);
          error_out = message;
          return false;
        }
      }
      if (words[guard_index] != UINT32_C(0xDEADC0DE)) {
        error_out = "pixel-interlock fragment wrote outside its canonical packed-word address";
        return false;
      }
    }
  }
  return true;
}

bool RunTranslatedCoverageCase(rex::graphics::metal::ExactOutputMergerPipeline* pipeline,
                               void* resources, uint32_t sample_count,
                               uint32_t system_constants_binding, std::string& error_out) {
  using rex::graphics::SpirvShaderTranslator;
  using rex::graphics::metal::ExactOutputMergerSurfaceConstants;
  using rex::graphics::xenos::DepthRenderTargetFormat;
  using rex::graphics::xenos::MsaaSamples;

  const auto msaa_samples = static_cast<MsaaSamples>(sample_count == 4   ? 2
                                                     : sample_count == 2 ? 1
                                                                         : 0);
  ExactOutputMergerSurfaceConstants layout;
  if (!rex::graphics::metal::BuildExactOutputMergerDepthConstants(
          rex::graphics::xenos::kEdramTileCount - 1, 80, 1, 1, msaa_samples,
          DepthRenderTargetFormat::kD24S8, layout, &error_out)) {
    return false;
  }

  SpirvShaderTranslator::SystemConstants system = {};
  if (!rex::graphics::metal::ApplyExactOutputMergerAddressConstants(layout, 0, system,
                                                                    &error_out)) {
    return false;
  }
  system.flags |= SpirvShaderTranslator::kSysFlag_FSIDepthStencil |
                  SpirvShaderTranslator::kSysFlag_FSIDepthPassIfLess |
                  SpirvShaderTranslator::kSysFlag_FSIDepthWrite;

  ExactEdramImage words = MakeExactEdramImage();
  std::array<size_t, 4> target_indices = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
  for (uint32_t sample = 0; sample < sample_count; ++sample) {
    if (!rex::graphics::metal::GetExactOutputMergerDwordIndex(layout, 0, 0, sample, 0,
                                                              target_indices[sample]) ||
        IsTargetIndex(target_indices, sample, target_indices[sample])) {
      error_out = "translated coverage test generated invalid sample addresses";
      return false;
    }
    words[target_indices[sample]] = UINT32_C(0xFFFFFF5A);
  }
  const size_t guard_index = FindGuardIndex(target_indices, sample_count, target_indices[0]);
  if (guard_index == SIZE_MAX) {
    error_out = "translated coverage test could not reserve a guard word";
    return false;
  }
  words[guard_index] = UINT32_C(0xA55AA55A);

  rex::graphics::metal::ExactOutputMergerUnroutableDraw draw;
  draw.pipeline = pipeline;
  draw.coverage_width = 1;
  draw.coverage_height = 1;
  draw.sample_count = sample_count;
  draw.fragment_inline_buffers[0] = {&system, sizeof(system), system_constants_binding};
  draw.fragment_inline_buffer_count = 1;
  draw.vertex_count = 3;
  if (!UploadExactEdramImage(resources, words, error_out) ||
      rex::graphics::metal::ExecuteExactOutputMergerUnroutableDraw(resources, draw, &error_out) !=
          rex::graphics::metal::ExactOutputMergerExecutionResult::kCompleted ||
      !DownloadExactEdramImage(resources, words, error_out)) {
    return false;
  }

  for (uint32_t sample = 0; sample < sample_count; ++sample) {
    const uint32_t actual = words[target_indices[sample]];
    if (actual == UINT32_C(0xFFFFFF5A) || (actual & 0xFFu) != 0x5Au) {
      char message[192];
      std::snprintf(message, sizeof(message),
                    "translated output merger did not update depth sample %u "
                    "at %ux (actual=%08X)",
                    sample, sample_count, actual);
      error_out = message;
      return false;
    }
  }
  if (words[guard_index] != UINT32_C(0xA55AA55A)) {
    error_out = "translated output merger wrote outside the expected depth words";
    return false;
  }
  return true;
}

bool RunBorrowedExecutorCases(
    rex::graphics::metal::ExactOutputMergerPipeline* pipeline, void* resources,
    uint32_t sample_count,
    const rex::graphics::metal::ExactOutputMergerPipelineLayout& pipeline_layout,
    std::string& error_out) {
  using rex::graphics::SpirvShaderTranslator;
  using rex::graphics::metal::ExactOutputMergerBorrowedDraw;
  using rex::graphics::metal::ExactOutputMergerExecutionResult;
  using rex::graphics::metal::ExactOutputMergerSurfaceConstants;
  using rex::graphics::xenos::DepthRenderTargetFormat;
  using rex::graphics::xenos::MsaaSamples;

  const auto msaa_samples = static_cast<MsaaSamples>(sample_count == 4   ? 2
                                                     : sample_count == 2 ? 1
                                                                         : 0);
  ExactOutputMergerSurfaceConstants surface;
  if (!rex::graphics::metal::BuildExactOutputMergerDepthConstants(
          rex::graphics::xenos::kEdramTileCount - 1, 80, 1, 1, msaa_samples,
          DepthRenderTargetFormat::kD24S8, surface, &error_out)) {
    return false;
  }
  SpirvShaderTranslator::SystemConstants system = {};
  if (!rex::graphics::metal::ApplyExactOutputMergerAddressConstants(surface, 0, system,
                                                                    &error_out)) {
    return false;
  }
  system.flags |= SpirvShaderTranslator::kSysFlag_FSIDepthStencil |
                  SpirvShaderTranslator::kSysFlag_FSIDepthPassIfLess |
                  SpirvShaderTranslator::kSysFlag_FSIDepthWrite;

  ExactEdramImage baseline = MakeExactEdramImage();
  std::array<size_t, 4> target_indices = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
  for (uint32_t sample = 0; sample < sample_count; ++sample) {
    if (!rex::graphics::metal::GetExactOutputMergerDwordIndex(surface, 0, 0, sample, 0,
                                                              target_indices[sample])) {
      error_out = "borrowed exact executor generated an invalid target address";
      return false;
    }
    baseline[target_indices[sample]] = UINT32_C(0xFFFFFF39);
  }

  ExactOutputMergerBorrowedDraw draw;
  draw.pipeline = pipeline;
  draw.pipeline_layout = pipeline_layout;
  draw.coverage_width = 1;
  draw.coverage_height = 1;
  draw.sample_count = sample_count;
  draw.system_constants = &system;
  draw.system_constants_size = sizeof(system);
  draw.vertex_count = 3;

  ExactEdramImage result = baseline;
  if (!UploadExactEdramImage(resources, baseline, error_out) ||
      rex::graphics::metal::ExecuteExactOutputMergerDraw(resources, draw, &error_out) !=
          ExactOutputMergerExecutionResult::kEnqueued ||
      !DownloadExactEdramImage(resources, result, error_out)) {
    return false;
  }
  for (uint32_t sample = 0; sample < sample_count; ++sample) {
    if (result[target_indices[sample]] == baseline[target_indices[sample]]) {
      error_out = "borrowed exact executor did not publish its completed depth write";
      return false;
    }
  }

  // A malformed draw is rejected before commit and leaves both EDRAM bytes
  // and authority untouched, so the caller may safely choose another route.
  ExactOutputMergerBorrowedDraw rejected_draw = draw;
  rejected_draw.sample_count = sample_count == 1 ? 2 : 1;
  std::string rejection;
  ExactEdramImage after_rejection = MakeExactEdramImage();
  if (rex::graphics::metal::ExecuteExactOutputMergerDraw(resources, rejected_draw, &rejection) !=
          ExactOutputMergerExecutionResult::kRejectedBeforeSubmit ||
      rejection.empty() || !DownloadExactEdramImage(resources, after_rejection, error_out) ||
      after_rejection != result) {
    error_out = "borrowed exact executor crossed the submission boundary on rejection";
    return false;
  }

  return true;
}

bool RunBorrowedPipelineReflectionNegativeCase(void* resources, const char* vertex_source,
                                               const char* fragment_source,
                                               uint32_t system_constants_binding,
                                               uint32_t edram_binding, std::string& error_out) {
  rex::graphics::metal::ExactOutputMergerPipelineLayout bad_layout;
  bad_layout.edram_fragment_buffer_index = edram_binding;
  // Deliberately omit the reflected system constants buffer.
  std::string rejection;
  auto* pipeline =
      rex::graphics::metal::CreateExactOutputMergerPipelineForInternalProbeFromMslSources(
          resources, vertex_source, "probe_vertex", fragment_source, "main0", 1, bad_layout,
          &rejection);
  if (pipeline || rejection.empty() || system_constants_binding == UINT32_MAX) {
    rex::graphics::metal::ReleaseExactOutputMergerPipeline(pipeline);
    error_out = "exact borrowed pipeline accepted a missing reflected buffer contract";
    return false;
  }
  return true;
}

bool RunSourceAttestationNegativeCases(void* resources, std::string& error_out) {
  rex::graphics::metal::ExactOutputMergerPipelineLayout layout;
  layout.edram_fragment_buffer_index = 1;
  std::string rejection;
  auto* pipeline =
      rex::graphics::metal::CreateExactOutputMergerPipelineForInternalProbeFromMslSources(
          resources, kProbeVertexMsl, "probe_vertex", kNonRogFragmentMsl, "non_rog_fragment", 1,
          layout, &rejection);
  if (pipeline || rejection.empty()) {
    rex::graphics::metal::ReleaseExactOutputMergerPipeline(pipeline);
    error_out = "exact output-merger accepted a non-ROG fragment entry";
    return false;
  }
  rejection.clear();
  pipeline = rex::graphics::metal::CreateExactOutputMergerPipelineForInternalProbeFromMslSources(
      resources, kProbeVertexMsl, "missing_vertex", kProbeFragmentMsl, "probe_fragment", 1, layout,
      &rejection);
  if (pipeline || rejection.empty()) {
    rex::graphics::metal::ReleaseExactOutputMergerPipeline(pipeline);
    error_out = "exact output-merger accepted an unattested selected entry point";
    return false;
  }
  layout.fragment.texture_count = 1;
  rejection.clear();
  pipeline = rex::graphics::metal::CreateExactOutputMergerPipelineForInternalProbeFromMslSources(
      resources, kProbeVertexMsl, "probe_vertex", kUnsupportedTextureFragmentMsl,
      "unsupported_texture_fragment", 1, layout, &rejection);
  if (pipeline || rejection.empty()) {
    rex::graphics::metal::ReleaseExactOutputMergerPipeline(pipeline);
    error_out = "exact output-merger accepted an unsupported reflected texture type";
    return false;
  }
  return true;
}

bool RunFullBorrowedResourceCase(id<MTLDevice> device, void* resources, uint32_t sample_count,
                                 std::string& error_out) {
  using rex::graphics::metal::ExactOutputMergerBorrowedDraw;
  using rex::graphics::metal::ExactOutputMergerExecutionResult;
  using rex::graphics::metal::ExactOutputMergerPipelineLayout;
  using rex::graphics::metal::ExactOutputMergerTextureSlot;
  using rex::graphics::metal::ProbeCullMode;
  using rex::graphics::metal::ProbeIndexBuffer;
  using rex::graphics::metal::ProbeRasterizationState;
  using rex::graphics::metal::ProbeSamplerSlot;

  ExactOutputMergerPipelineLayout layout;
  layout.vertex.system_constants_buffer_index = 0;
  layout.vertex.float_constants_buffer_index = 1;
  layout.vertex.fetch_constants_buffer_index = 2;
  layout.vertex.bool_loop_constants_buffer_index = 3;
  layout.vertex.shared_memory_buffer_index = 4;
  layout.vertex.vertex_data_buffer_index = 5;
  layout.vertex.texture_count = 1;
  layout.vertex.sampler_count = 1;
  layout.fragment.system_constants_buffer_index = 0;
  layout.fragment.float_constants_buffer_index = 2;
  layout.fragment.fetch_constants_buffer_index = 3;
  layout.fragment.bool_loop_constants_buffer_index = 4;
  layout.fragment.shared_memory_buffer_index = 5;
  layout.fragment.texture_count = 1;
  layout.fragment.sampler_count = 1;
  layout.edram_fragment_buffer_index = 1;
  auto* pipeline =
      rex::graphics::metal::CreateExactOutputMergerPipelineForInternalProbeFromMslSources(
          resources, kBorrowedResourceMsl, "resource_vertex", kBorrowedResourceMsl,
          "resource_fragment", sample_count, layout, &error_out);
  if (!pipeline) {
    return false;
  }

  constexpr uint32_t kTargetDword = 12345;
  constexpr uint32_t kExpectedValue = UINT32_C(0xC0DEC0DE);
  const BorrowedSystem system = {kTargetDword, kExpectedValue, UINT32_C(0x89ABCDEF),
                                 UINT32_C(0x12345678)};
  const std::array<float, 4> vertex_float = {5.0f, 0.0f, 0.0f, 0.0f};
  const std::array<float, 4> fragment_float = {7.0f, 0.0f, 0.0f, 0.0f};
  const std::array<uint32_t, 4> fetch = {11, 0, 0, 0};
  const std::array<uint32_t, 4> bool_loop = {12, 0, 0, 0};
  const std::array<uint32_t, 4> shared = {13, 0, 0, 0};
  const std::array<float, 24> positions = {
      2.0f,  2.0f,  0.0f, 1.0f, 2.0f, 2.0f,  0.0f, 1.0f, 2.0f,  2.0f, 0.0f, 1.0f,
      -1.0f, -1.0f, 0.0f, 1.0f, 3.0f, -1.0f, 0.0f, 1.0f, -1.0f, 3.0f, 0.0f, 1.0f,
  };
  const std::array<uint16_t, 3> indices = {3, 4, 5};
  const std::array<uint8_t, 8> vertex_texels = {255, 0, 0, 255, 0, 0, 0, 255};
  const std::array<uint8_t, 8> fragment_texels = {0, 0, 0, 255, 0, 255, 0, 255};
  id<MTLBuffer> shared_buffer = [device newBufferWithBytes:shared.data()
                                                    length:sizeof(shared)
                                                   options:MTLResourceStorageModeShared];
  if (!shared_buffer) {
    rex::graphics::metal::ReleaseExactOutputMergerPipeline(pipeline);
    error_out = "failed to allocate full borrowed-resource shared memory";
    return false;
  }

  ExactOutputMergerTextureSlot vertex_texture;
  vertex_texture.rgba = vertex_texels.data();
  vertex_texture.rgba_size = vertex_texels.size();
  vertex_texture.width = 2;
  vertex_texture.height = 1;
  vertex_texture.array_length = 1;
  vertex_texture.bytes_per_row = 8;
  vertex_texture.bytes_per_image = 8;
  ExactOutputMergerTextureSlot fragment_texture = vertex_texture;
  fragment_texture.rgba = fragment_texels.data();
  fragment_texture.rgba_size = fragment_texels.size();
  ProbeSamplerSlot vertex_sampler;
  ProbeSamplerSlot fragment_sampler;
  fragment_sampler.min_linear = 1;
  fragment_sampler.mag_linear = 1;
  ProbeIndexBuffer index_buffer;
  index_buffer.data = indices.data();
  index_buffer.size = sizeof(indices);
  index_buffer.index_size = 2;
  ProbeRasterizationState raster;
  raster.viewport_width = 1.0;
  raster.viewport_height = 1.0;
  raster.scissor_width = 1;
  raster.scissor_height = 1;
  raster.cull_mode = ProbeCullMode::kNone;

  ExactOutputMergerBorrowedDraw draw;
  draw.pipeline = pipeline;
  draw.pipeline_layout = layout;
  draw.coverage_width = 1;
  draw.coverage_height = 1;
  draw.sample_count = sample_count;
  draw.system_constants = &system;
  draw.system_constants_size = sizeof(system);
  draw.vertex_float_constants = vertex_float.data();
  draw.vertex_float_constants_size = sizeof(vertex_float);
  draw.fragment_float_constants = fragment_float.data();
  draw.fragment_float_constants_size = sizeof(fragment_float);
  draw.fetch_constants = fetch.data();
  draw.fetch_constants_size = sizeof(fetch);
  draw.bool_loop_constants = bool_loop.data();
  draw.bool_loop_constants_size = sizeof(bool_loop);
  draw.shared_memory_metal_buffer = shared_buffer;
  draw.shared_memory_size = sizeof(shared);
  draw.vertex_textures = &vertex_texture;
  draw.vertex_texture_count = 1;
  draw.vertex_samplers = &vertex_sampler;
  draw.vertex_sampler_count = 1;
  draw.fragment_textures = &fragment_texture;
  draw.fragment_texture_count = 1;
  draw.fragment_samplers = &fragment_sampler;
  draw.fragment_sampler_count = 1;
  draw.vertex_data = positions.data();
  draw.vertex_data_size = sizeof(positions);
  draw.vertex_data_stride = sizeof(float) * 4;
  draw.index_buffer = &index_buffer;
  draw.rasterization_state = &raster;
  draw.vertex_count = 3;

  ExactEdramImage baseline = MakeExactEdramImage(UINT32_C(0x5A5A5A5A));
  std::string rejection;
  ExactEdramImage after_rejection = MakeExactEdramImage();
  bool succeeded = UploadExactEdramImage(resources, baseline, error_out);
  auto expect_rejection = [&]() {
    rejection.clear();
    return rex::graphics::metal::ExecuteExactOutputMergerDraw(resources, draw, &rejection) ==
               ExactOutputMergerExecutionResult::kRejectedBeforeSubmit &&
           !rejection.empty();
  };
  if (succeeded) {
    vertex_texture.rgba_size = vertex_texels.size() - 1;
    succeeded = expect_rejection();
    vertex_texture.rgba_size = vertex_texels.size();

    draw.system_constants_size = sizeof(system) - 1;
    succeeded = succeeded && expect_rejection();
    draw.system_constants_size = sizeof(system);
    draw.vertex_float_constants_size = sizeof(vertex_float) - 1;
    succeeded = succeeded && expect_rejection();
    draw.vertex_float_constants_size = sizeof(vertex_float);
    draw.fragment_float_constants_size = sizeof(fragment_float) - 1;
    succeeded = succeeded && expect_rejection();
    draw.fragment_float_constants_size = sizeof(fragment_float);
    draw.fetch_constants_size = sizeof(fetch) - 1;
    succeeded = succeeded && expect_rejection();
    draw.fetch_constants_size = sizeof(fetch);
    draw.bool_loop_constants_size = sizeof(bool_loop) - 1;
    succeeded = succeeded && expect_rejection();
    draw.bool_loop_constants_size = sizeof(bool_loop);
    draw.shared_memory_size = sizeof(uint32_t) - 1;
    succeeded = succeeded && expect_rejection();
    draw.shared_memory_size = sizeof(shared);
    draw.vertex_data_size = sizeof(positions) - 1;
    succeeded = succeeded && expect_rejection();
    draw.vertex_data_size = sizeof(positions);
    index_buffer.offset = 1;
    succeeded = succeeded && expect_rejection();
    index_buffer.offset = 0;
    index_buffer.size = sizeof(indices) - 1;
    succeeded = succeeded && expect_rejection();
    index_buffer.size = sizeof(indices);
    fragment_sampler.min_linear = 2;
    succeeded = succeeded && expect_rejection();
    fragment_sampler.min_linear = 1;

    MTLTextureDescriptor* wrong_type_descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:2
                                                          height:1
                                                       mipmapped:NO];
    wrong_type_descriptor.storageMode = MTLStorageModeShared;
    wrong_type_descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> wrong_type_texture = [device newTextureWithDescriptor:wrong_type_descriptor];
    ExactOutputMergerTextureSlot saved_fragment_texture = fragment_texture;
    fragment_texture.rgba = nullptr;
    fragment_texture.rgba_size = 0;
    fragment_texture.metal_texture = wrong_type_texture;
    fragment_texture.bytes_per_row = 0;
    fragment_texture.bytes_per_image = 0;
    succeeded = succeeded && wrong_type_texture && expect_rejection();
    fragment_texture = saved_fragment_texture;
    [wrong_type_texture release];

    succeeded = succeeded && DownloadExactEdramImage(resources, after_rejection, error_out) &&
                after_rejection == baseline;
  }
  if (succeeded) {
    ExactEdramImage result = MakeExactEdramImage();
    succeeded = UploadExactEdramImage(resources, baseline, error_out) &&
                rex::graphics::metal::ExecuteExactOutputMergerDraw(resources, draw, &error_out) ==
                    ExactOutputMergerExecutionResult::kEnqueued &&
                DownloadExactEdramImage(resources, result, error_out) &&
                result[kTargetDword] == kExpectedValue;
  }
  if (succeeded) {
    // The production texture cache lends one-layer BGRA textures to the exact
    // route. Prove that the reflected texture2d_array contract accepts that
    // resident shape and samples its normalized channels correctly.
    const std::array<uint8_t, 8> bgra_texels = {0, 0, 0, 255, 0, 255, 0, 255};
    MTLTextureDescriptor* bgra_descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:2
                                                          height:1
                                                       mipmapped:NO];
    bgra_descriptor.textureType = MTLTextureType2DArray;
    bgra_descriptor.arrayLength = 1;
    bgra_descriptor.storageMode = MTLStorageModeShared;
    bgra_descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> bgra_texture = [device newTextureWithDescriptor:bgra_descriptor];
    if (bgra_texture) {
      [bgra_texture replaceRegion:MTLRegionMake2D(0, 0, 2, 1)
                      mipmapLevel:0
                            slice:0
                        withBytes:bgra_texels.data()
                      bytesPerRow:bgra_texels.size()
                    bytesPerImage:bgra_texels.size()];
    }
    ExactOutputMergerTextureSlot saved_fragment_texture = fragment_texture;
    fragment_texture.rgba = nullptr;
    fragment_texture.rgba_size = 0;
    fragment_texture.metal_texture = bgra_texture;
    fragment_texture.bytes_per_row = 0;
    fragment_texture.bytes_per_image = 0;
    ExactEdramImage result = MakeExactEdramImage();
    succeeded = bgra_texture && bgra_texture.textureType == MTLTextureType2DArray &&
                bgra_texture.arrayLength == 1 &&
                bgra_texture.pixelFormat == MTLPixelFormatBGRA8Unorm &&
                UploadExactEdramImage(resources, baseline, error_out) &&
                rex::graphics::metal::ExecuteExactOutputMergerDraw(resources, draw, &error_out) ==
                    ExactOutputMergerExecutionResult::kEnqueued &&
                DownloadExactEdramImage(resources, result, error_out) &&
                result[kTargetDword] == kExpectedValue;
    fragment_texture = saved_fragment_texture;
    [bgra_texture release];
  }
  [shared_buffer release];
  rex::graphics::metal::ReleaseExactOutputMergerPipeline(pipeline);
  if (!succeeded && error_out.empty()) {
    error_out = "full borrowed-resource exact draw failed validation or execution";
  }
  return succeeded;
}

bool RunBorrowedMetalIndexCase(id<MTLDevice> device, void* resources, uint32_t sample_count,
                               std::string& error_out) {
  using rex::graphics::metal::ExactOutputMergerBorrowedDraw;
  using rex::graphics::metal::ExactOutputMergerExecutionResult;
  using rex::graphics::metal::ExactOutputMergerPipelineLayout;
  using rex::graphics::metal::ProbeIndexBuffer;

  ExactOutputMergerPipelineLayout layout;
  layout.vertex.system_constants_buffer_index = 0;
  layout.fragment.system_constants_buffer_index = 0;
  layout.edram_fragment_buffer_index = 1;
  auto* pipeline =
      rex::graphics::metal::CreateExactOutputMergerPipelineForInternalProbeFromMslSources(
          resources, kBorrowedMetalIndexMsl, "metal_index_vertex", kBorrowedMetalIndexMsl,
          "metal_index_fragment", sample_count, layout, &error_out);
  if (!pipeline) {
    return false;
  }

  constexpr uint32_t kTargetDword = 54321;
  constexpr uint32_t kExpectedValue = UINT32_C(0xD15EA5ED);
  const BorrowedSystem system = {kTargetDword, kExpectedValue, UINT32_C(0x89ABCDEF), 0};
  const std::array<uint32_t, 3> indices = {3, 4, 5};
  id<MTLBuffer> index_metal_buffer = [device newBufferWithBytes:indices.data()
                                                         length:sizeof(indices)
                                                        options:MTLResourceStorageModeShared];
  ProbeIndexBuffer index;
  index.metal_buffer = index_metal_buffer;
  index.size = sizeof(indices);
  index.index_size = 4;
  ExactOutputMergerBorrowedDraw draw;
  draw.pipeline = pipeline;
  draw.pipeline_layout = layout;
  draw.coverage_width = 1;
  draw.coverage_height = 1;
  draw.sample_count = sample_count;
  draw.system_constants = &system;
  draw.system_constants_size = sizeof(system);
  draw.index_buffer = &index;
  draw.vertex_count = 3;

  ExactEdramImage baseline = MakeExactEdramImage(UINT32_C(0xA5A5A5A5));
  ExactEdramImage after_rejection = MakeExactEdramImage();
  std::string rejection;
  bool succeeded = index_metal_buffer && UploadExactEdramImage(resources, baseline, error_out);
  index.offset = 2;
  succeeded = succeeded &&
              rex::graphics::metal::ExecuteExactOutputMergerDraw(resources, draw, &rejection) ==
                  ExactOutputMergerExecutionResult::kRejectedBeforeSubmit &&
              !rejection.empty();
  index.offset = 0;
  index.size = sizeof(indices) + sizeof(uint32_t);
  rejection.clear();
  succeeded = succeeded &&
              rex::graphics::metal::ExecuteExactOutputMergerDraw(resources, draw, &rejection) ==
                  ExactOutputMergerExecutionResult::kRejectedBeforeSubmit &&
              !rejection.empty();
  index.size = sizeof(indices);
  succeeded = succeeded && DownloadExactEdramImage(resources, after_rejection, error_out) &&
              after_rejection == baseline;
  if (succeeded) {
    ExactEdramImage result = MakeExactEdramImage();
    succeeded = rex::graphics::metal::ExecuteExactOutputMergerDraw(resources, draw, &error_out) ==
                    ExactOutputMergerExecutionResult::kEnqueued &&
                DownloadExactEdramImage(resources, result, error_out) &&
                result[kTargetDword] == kExpectedValue;
  }

  [index_metal_buffer release];
  rex::graphics::metal::ReleaseExactOutputMergerPipeline(pipeline);
  if (!succeeded && error_out.empty()) {
    error_out = "borrowed Metal uint32 index draw failed";
  }
  return succeeded;
}

bool RunFixedPrimitiveRestartCases(void* resources, std::string& error_out) {
  using rex::graphics::metal::ExactOutputMergerBorrowedDraw;
  using rex::graphics::metal::ExactOutputMergerExecutionResult;
  using rex::graphics::metal::ExactOutputMergerPipelineLayout;
  using rex::graphics::metal::ProbeIndexBuffer;

  ExactOutputMergerPipelineLayout layout;
  layout.vertex.system_constants_buffer_index = 0;
  layout.fragment.system_constants_buffer_index = 0;
  layout.edram_fragment_buffer_index = 1;
  auto* pipeline =
      rex::graphics::metal::CreateExactOutputMergerPipelineForInternalProbeFromMslSources(
          resources, kFixedPrimitiveRestartMsl, "fixed_restart_vertex", kFixedPrimitiveRestartMsl,
          "fixed_restart_fragment", 1, layout, &error_out);
  if (!pipeline) {
    return false;
  }

  constexpr uint32_t kTargetDword = 65432;
  constexpr uint32_t kGuardDword = kTargetDword + 1;
  constexpr uint32_t kGuardValue = UINT32_C(0xA55AA55A);
  const BorrowedSystem system = {kTargetDword, 0, UINT32_C(0x89ABCDEF), 0};
  ExactEdramImage baseline = MakeExactEdramImage(UINT32_C(0x13579BDF));
  baseline[kTargetDword] = 0;
  baseline[kGuardDword] = kGuardValue;

  auto run_index_case = [&](const void* index_data, size_t index_data_size, uint32_t index_size,
                            bool restart_enabled, uint32_t restart_index,
                            uint32_t expected_fragments_per_draw, uint32_t draw_count,
                            const char* label) {
    ProbeIndexBuffer index;
    index.data = index_data;
    index.size = index_data_size;
    index.index_size = index_size;
    index.production_host_data_trusted = true;

    ExactOutputMergerBorrowedDraw draw;
    draw.pipeline = pipeline;
    draw.pipeline_layout = layout;
    draw.coverage_width = 1;
    draw.coverage_height = 1;
    draw.sample_count = 1;
    draw.system_constants = &system;
    draw.system_constants_size = sizeof(system);
    draw.index_buffer = &index;
    draw.primitive_type = uint32_t(rex::graphics::xenos::PrimitiveType::kTriangleStrip);
    draw.primitive_restart_enabled = restart_enabled;
    draw.primitive_restart_index = restart_index;
    draw.vertex_count = 7;

    ExactEdramImage result = MakeExactEdramImage();
    bool completed = UploadExactEdramImage(resources, baseline, error_out);
    for (uint32_t draw_index = 0; completed && draw_index < draw_count; ++draw_index) {
      completed = rex::graphics::metal::ExecuteExactOutputMergerDraw(resources, draw, &error_out) ==
                  ExactOutputMergerExecutionResult::kEnqueued;
    }
    const uint32_t pending_before_finalize =
        rex::graphics::metal::GetExactOutputMergerPendingDrawCount(resources);
    completed = completed && pending_before_finalize == draw_count &&
                rex::graphics::metal::FinalizeExactOutputMergerDraws(resources, &error_out);
    const uint32_t pending_after_finalize =
        rex::graphics::metal::GetExactOutputMergerPendingDrawCount(resources);
    uint32_t explicitly_waited = UINT32_MAX;
    completed = completed && pending_after_finalize == draw_count &&
                rex::graphics::metal::WaitExactOutputMergerDraws(resources, &error_out,
                                                                 &explicitly_waited) &&
                explicitly_waited == draw_count &&
                rex::graphics::metal::GetExactOutputMergerPendingDrawCount(resources) == 0;
    if (!completed || !DownloadExactEdramImage(resources, result, error_out)) {
      if (error_out.empty()) {
        error_out = std::string("fixed primitive-restart draw failed: ") + label;
      }
      return false;
    }
    const uint32_t expected_fragments = expected_fragments_per_draw * draw_count;
    if (result[kTargetDword] != expected_fragments || result[kGuardDword] != kGuardValue) {
      char message[256];
      std::snprintf(message, sizeof(message),
                    "fixed primitive-restart fragment count mismatch (%s): "
                    "expected=%u actual=%u guard=%08X",
                    label, expected_fragments, result[kTargetDword], result[kGuardDword]);
      error_out = message;
      return false;
    }
    return true;
  };

  // Each fixed separator splits two full-coverage triangles, producing two
  // ordered fragments. Replacing the separator with vertex 5 keeps the stream
  // continuous and deterministically adds a third covering bridge triangle,
  // proving this fixture would detect a missing restart.
  const std::array<uint16_t, 7> indices_u16 = {0, 1, 2, UINT16_MAX, 3, 4, 5};
  const std::array<uint16_t, 7> bridged_u16 = {0, 1, 2, 5, 3, 4, 5};
  const std::array<uint32_t, 7> indices_u32 = {0, 1, 2, UINT32_MAX, 3, 4, 5};
  const std::array<uint32_t, 7> bridged_u32 = {0, 1, 2, 5, 3, 4, 5};
  bool succeeded = run_index_case(indices_u16.data(), sizeof(indices_u16), sizeof(uint16_t), true,
                                  UINT16_MAX, 2, 1, "uint16 restart") &&
                   run_index_case(bridged_u16.data(), sizeof(bridged_u16), sizeof(uint16_t), false,
                                  UINT32_MAX, 3, 1, "uint16 no-restart control") &&
                   run_index_case(indices_u32.data(), sizeof(indices_u32), sizeof(uint32_t), true,
                                  UINT32_MAX, 2, 1, "uint32 restart") &&
                   run_index_case(bridged_u32.data(), sizeof(bridged_u32), sizeof(uint32_t), false,
                                  UINT32_MAX, 3, 1, "uint32 no-restart control") &&
                   run_index_case(indices_u16.data(), sizeof(indices_u16), sizeof(uint16_t), true,
                                  UINT16_MAX, 2, 2, "uint16 consecutive GPU-authority draws");

  if (succeeded) {
    using rex::graphics::metal::PipelineProbeSubmissionStats;
    using rex::graphics::metal::PipelineProbeUploadStats;

    constexpr uint32_t kDrawsPerCommandBuffer =
        rex::graphics::metal::kExactOutputMergerDrawsPerCommandBuffer;
    constexpr uint32_t kCommandBufferCap = rex::graphics::metal::kExactOutputMergerCommandBufferCap;
    constexpr uint32_t kStressDrawCount = kDrawsPerCommandBuffer * kCommandBufferCap + 1;
    BorrowedSystem stress_system = system;
    std::array<uint16_t, 7> stress_indices = indices_u16;
    ProbeIndexBuffer stress_index;
    stress_index.data = stress_indices.data();
    stress_index.size = sizeof(stress_indices);
    stress_index.index_size = sizeof(uint16_t);
    stress_index.production_host_data_trusted = true;

    ExactOutputMergerBorrowedDraw stress_draw;
    stress_draw.pipeline = pipeline;
    stress_draw.pipeline_layout = layout;
    stress_draw.coverage_width = 1;
    stress_draw.coverage_height = 1;
    stress_draw.sample_count = 1;
    stress_draw.system_constants = &stress_system;
    stress_draw.system_constants_size = sizeof(stress_system);
    stress_draw.index_buffer = &stress_index;
    stress_draw.primitive_type = uint32_t(rex::graphics::xenos::PrimitiveType::kTriangleStrip);
    stress_draw.primitive_restart_enabled = true;
    stress_draw.primitive_restart_index = UINT16_MAX;
    stress_draw.vertex_count = 7;

    PipelineProbeUploadStats upload_before = {};
    PipelineProbeSubmissionStats submission_before = {};
    succeeded =
        rex::graphics::metal::GetExactOutputMergerUploadStats(resources, &upload_before) &&
        rex::graphics::metal::GetExactOutputMergerSubmissionStats(resources, &submission_before) &&
        submission_before.context_identity != 0 &&
        submission_before.max_draws_per_command_buffer == kDrawsPerCommandBuffer &&
        submission_before.max_committed_draw_command_buffer_count == kCommandBufferCap &&
        UploadExactEdramImage(resources, baseline, error_out);
    for (uint32_t draw_index = 0; succeeded && draw_index < kStressDrawCount; ++draw_index) {
      stress_system.target_dword = kTargetDword;
      stress_indices[3] = UINT16_MAX;
      succeeded =
          rex::graphics::metal::ExecuteExactOutputMergerDraw(resources, stress_draw, &error_out) ==
          ExactOutputMergerExecutionResult::kEnqueued;
      // Borrowed CPU bytes must already have been copied when kEnqueued is
      // returned. Leave deliberately wrong source data behind after every
      // call, including the final one.
      stress_system.target_dword = kGuardDword;
      stress_indices[3] = 5;
    }

    const uint32_t pending_before_finalize =
        rex::graphics::metal::GetExactOutputMergerPendingDrawCount(resources);
    succeeded = succeeded && pending_before_finalize > 0 &&
                pending_before_finalize <= kDrawsPerCommandBuffer * kCommandBufferCap &&
                rex::graphics::metal::FinalizeExactOutputMergerDraws(resources, &error_out);
    const uint32_t pending_after_finalize =
        rex::graphics::metal::GetExactOutputMergerPendingDrawCount(resources);
    uint32_t waited_draw_count = UINT32_MAX;
    succeeded = succeeded && pending_after_finalize == pending_before_finalize &&
                rex::graphics::metal::WaitExactOutputMergerDraws(resources, &error_out,
                                                                 &waited_draw_count) &&
                waited_draw_count == pending_before_finalize &&
                rex::graphics::metal::GetExactOutputMergerPendingDrawCount(resources) == 0;

    ExactEdramImage result = MakeExactEdramImage();
    PipelineProbeUploadStats upload_after = {};
    PipelineProbeSubmissionStats submission_after = {};
    succeeded =
        succeeded && DownloadExactEdramImage(resources, result, error_out) &&
        rex::graphics::metal::GetExactOutputMergerUploadStats(resources, &upload_after) &&
        rex::graphics::metal::GetExactOutputMergerSubmissionStats(resources, &submission_after);
    const uint64_t allocation_delta =
        upload_after.buffer_allocation_count - upload_before.buffer_allocation_count;
    const uint64_t suballocation_delta =
        upload_after.suballocation_count - upload_before.suballocation_count;
    const uint64_t commit_delta = submission_after.draw_command_buffer_commit_count -
                                  submission_before.draw_command_buffer_commit_count;
    const uint64_t backpressure_delta =
        submission_after.backpressure_check_count - submission_before.backpressure_check_count;
    const uint64_t reclaimed_delta =
        submission_after.nonblocking_completed_command_buffer_reclamation_count -
        submission_before.nonblocking_completed_command_buffer_reclamation_count;
    const uint64_t blocking_wait_delta = submission_after.blocking_backpressure_wait_count -
                                         submission_before.blocking_backpressure_wait_count;
    const uint64_t retired_delta = reclaimed_delta + blocking_wait_delta;
    const bool staging_bounded =
        succeeded && result[kTargetDword] == kStressDrawCount * 2 &&
        result[kGuardDword] == kGuardValue && allocation_delta <= kCommandBufferCap &&
        suballocation_delta == uint64_t(kStressDrawCount) * 2 &&
        commit_delta == kCommandBufferCap + 1 && backpressure_delta == 1 && retired_delta >= 1 &&
        retired_delta <= kCommandBufferCap && blocking_wait_delta <= 1 &&
        submission_after.context_identity == submission_before.context_identity &&
        submission_after.peak_committed_draw_command_buffer_count == kCommandBufferCap &&
        submission_after.peak_pending_submission_count >=
            kDrawsPerCommandBuffer * kCommandBufferCap;
    if (!staging_bounded) {
      char message[512];
      std::snprintf(message, sizeof(message),
                    "bounded exact staging failed: pending=%u/%u waited=%u target=%u guard=%08X "
                    "alloc=%llu suballoc=%llu commits=%llu checks=%llu retired=%llu waits=%llu "
                    "peak=%u/%u",
                    pending_before_finalize, pending_after_finalize, waited_draw_count,
                    result[kTargetDword], result[kGuardDword],
                    static_cast<unsigned long long>(allocation_delta),
                    static_cast<unsigned long long>(suballocation_delta),
                    static_cast<unsigned long long>(commit_delta),
                    static_cast<unsigned long long>(backpressure_delta),
                    static_cast<unsigned long long>(retired_delta),
                    static_cast<unsigned long long>(blocking_wait_delta),
                    submission_after.peak_committed_draw_command_buffer_count,
                    submission_after.peak_pending_submission_count);
      error_out = message;
      succeeded = false;
    }

    if (succeeded) {
      stress_system.target_dword = kTargetDword;
      stress_indices[3] = UINT16_MAX;
      ExactEdramImage fence_result = MakeExactEdramImage();
      const bool download_fence_ok =
          UploadExactEdramImage(resources, baseline, error_out) &&
          rex::graphics::metal::ExecuteExactOutputMergerDraw(resources, stress_draw, &error_out) ==
              ExactOutputMergerExecutionResult::kEnqueued &&
          rex::graphics::metal::GetExactOutputMergerPendingDrawCount(resources) == 1 &&
          DownloadExactEdramImage(resources, fence_result, error_out) &&
          rex::graphics::metal::GetExactOutputMergerPendingDrawCount(resources) == 0 &&
          fence_result[kTargetDword] == 2;

      stress_system.target_dword = kTargetDword;
      stress_indices[3] = UINT16_MAX;
      const bool upload_fence_ok =
          download_fence_ok && UploadExactEdramImage(resources, baseline, error_out) &&
          rex::graphics::metal::ExecuteExactOutputMergerDraw(resources, stress_draw, &error_out) ==
              ExactOutputMergerExecutionResult::kEnqueued &&
          rex::graphics::metal::GetExactOutputMergerPendingDrawCount(resources) == 1 &&
          UploadExactEdramImage(resources, baseline, error_out) &&
          rex::graphics::metal::GetExactOutputMergerPendingDrawCount(resources) == 0 &&
          DownloadExactEdramImage(resources, fence_result, error_out) && fence_result == baseline;
      if (!upload_fence_ok) {
        error_out = "exact EDRAM upload/download did not drain the asynchronous draw epoch";
        succeeded = false;
      }
    }
  }

  rex::graphics::metal::ReleaseExactOutputMergerPipeline(pipeline);
  return succeeded;
}

bool RunAsyncCommandBufferFailureCase(id<MTLDevice> device, id<MTLCommandQueue> command_queue,
                                      std::string& error_out) {
  using rex::graphics::metal::ExactOutputMergerBorrowedDraw;
  using rex::graphics::metal::ExactOutputMergerExecutionResult;
  using rex::graphics::metal::ExactOutputMergerPipelineLayout;
  using rex::graphics::metal::PipelineProbeSubmissionStats;
  using rex::graphics::metal::PipelineProbeUploadStats;

  ExactFaultInjectionCounters counters;
  bool succeeded = true;
  std::string detail;
  @autoreleasepool {
    auto* fault_queue = [[ExactFaultInjectingCommandQueue alloc] initWithCommandQueue:command_queue
                                                                             counters:&counters];
    void* resources = rex::graphics::metal::CreateExactOutputMergerResources(device, &detail);
    succeeded =
        fault_queue && resources &&
        rex::graphics::metal::SetExactOutputMergerCommandQueue(resources, fault_queue, &detail);

    ExactOutputMergerPipelineLayout layout;
    layout.vertex.system_constants_buffer_index = 0;
    layout.fragment.system_constants_buffer_index = 0;
    layout.edram_fragment_buffer_index = 1;
    constexpr uint32_t kTargetDword = 76543;
    constexpr uint32_t kGuardDword = kTargetDword + 1;
    constexpr uint32_t kGuardValue = UINT32_C(0xC001D00D);
    const BorrowedSystem system = {kTargetDword, 0, UINT32_C(0x89ABCDEF), 0};
    ExactEdramImage baseline = MakeExactEdramImage(UINT32_C(0x13579BDF));
    baseline[kTargetDword] = 0;
    baseline[kGuardDword] = kGuardValue;

    auto submit_with_released_pipeline = [&](ExactOutputMergerExecutionResult expected,
                                             std::string& submission_error) {
      auto* pipeline =
          rex::graphics::metal::CreateExactOutputMergerPipelineForInternalProbeFromMslSources(
              resources, kFixedPrimitiveRestartMsl, "fixed_restart_vertex",
              kFixedPrimitiveRestartMsl, "fixed_restart_fragment", 1, layout, &submission_error);
      if (!pipeline) {
        return false;
      }
      ExactOutputMergerBorrowedDraw draw;
      draw.pipeline = pipeline;
      draw.pipeline_layout = layout;
      draw.coverage_width = 1;
      draw.coverage_height = 1;
      draw.sample_count = 1;
      draw.system_constants = &system;
      draw.system_constants_size = sizeof(system);
      draw.vertex_count = 3;
      const ExactOutputMergerExecutionResult actual =
          rex::graphics::metal::ExecuteExactOutputMergerDraw(resources, draw, &submission_error);
      // The borrowed pipeline wrapper is deliberately gone before any wait.
      // Metal must retain the encoded pipeline state for the real GPU work.
      rex::graphics::metal::ReleaseExactOutputMergerPipeline(pipeline);
      return actual == expected;
    };

    PipelineProbeUploadStats upload_after_wait_failure = {};
    if (succeeded) {
      [fault_queue injectNextCommandBufferFailure];
      detail.clear();
      succeeded =
          UploadExactEdramImage(resources, baseline, detail) &&
          submit_with_released_pipeline(ExactOutputMergerExecutionResult::kEnqueued, detail) &&
          rex::graphics::metal::GetExactOutputMergerPendingDrawCount(resources) == 1;
    }
    if (succeeded) {
      uint32_t waited_draw_count = UINT32_MAX;
      std::string wait_error;
      const bool wait_succeeded = rex::graphics::metal::WaitExactOutputMergerDraws(
          resources, &wait_error, &waited_draw_count);
      succeeded = !wait_succeeded && waited_draw_count == 1 &&
                  wait_error.find("injected exact output-merger command-buffer failure") !=
                      std::string::npos &&
                  rex::graphics::metal::GetExactOutputMergerPendingDrawCount(resources) == 0 &&
                  rex::graphics::metal::GetExactOutputMergerUploadStats(
                      resources, &upload_after_wait_failure) &&
                  upload_after_wait_failure.buffer_allocation_count == 1;
      if (!succeeded) {
        detail = wait_error.empty() ? "injected exact wait failure was not surfaced and drained"
                                    : wait_error;
      }
    }

    // Invalid authority rejects before a second submission. A complete upload
    // is then the one legal rehydration path, and the already allocated upload
    // arena must be reusable without growth.
    if (succeeded) {
      std::string rejection;
      succeeded = submit_with_released_pipeline(
                      ExactOutputMergerExecutionResult::kRejectedBeforeSubmit, rejection) &&
                  !rejection.empty() &&
                  rex::graphics::metal::GetExactOutputMergerPendingDrawCount(resources) == 0;
      if (!succeeded) {
        detail = "invalid exact authority did not reject before resubmission";
      }
    }
    PipelineProbeUploadStats upload_after_wait_recovery = {};
    if (succeeded) {
      ExactEdramImage recovered = MakeExactEdramImage();
      detail.clear();
      succeeded =
          UploadExactEdramImage(resources, baseline, detail) &&
          submit_with_released_pipeline(ExactOutputMergerExecutionResult::kEnqueued, detail) &&
          DownloadExactEdramImage(resources, recovered, detail) && recovered[kTargetDword] == 1 &&
          recovered[kGuardDword] == kGuardValue &&
          rex::graphics::metal::GetExactOutputMergerUploadStats(resources,
                                                                &upload_after_wait_recovery) &&
          upload_after_wait_recovery.buffer_allocation_count ==
              upload_after_wait_failure.buffer_allocation_count;
      if (!succeeded && detail.empty()) {
        detail = "exact upload arena was not reusable after wait failure";
      }
    }

    // Exercise the same asynchronous error through Download's implicit fence.
    // The destination must remain untouched when the wait fails.
    PipelineProbeUploadStats upload_after_download_failure = {};
    if (succeeded) {
      [fault_queue injectNextCommandBufferFailure];
      detail.clear();
      succeeded =
          UploadExactEdramImage(resources, baseline, detail) &&
          submit_with_released_pipeline(ExactOutputMergerExecutionResult::kEnqueued, detail);
    }
    if (succeeded) {
      ExactEdramImage untouched = MakeExactEdramImage(UINT32_C(0xA5A5A5A5));
      std::string download_error;
      const bool download_succeeded = DownloadExactEdramImage(resources, untouched, download_error);
      succeeded = !download_succeeded &&
                  download_error.find("injected exact output-merger command-buffer failure") !=
                      std::string::npos &&
                  rex::graphics::metal::GetExactOutputMergerPendingDrawCount(resources) == 0 &&
                  std::all_of(untouched.begin(), untouched.end(),
                              [](uint32_t value) { return value == UINT32_C(0xA5A5A5A5); }) &&
                  rex::graphics::metal::GetExactOutputMergerUploadStats(
                      resources, &upload_after_download_failure);
      if (!succeeded) {
        detail = download_error.empty()
                     ? "injected exact download failure did not fail closed and drain"
                     : download_error;
      }
    }

    PipelineProbeUploadStats upload_after_download_recovery = {};
    PipelineProbeSubmissionStats submission_after_recovery = {};
    if (succeeded) {
      ExactEdramImage recovered = MakeExactEdramImage();
      detail.clear();
      succeeded =
          UploadExactEdramImage(resources, baseline, detail) &&
          submit_with_released_pipeline(ExactOutputMergerExecutionResult::kEnqueued, detail) &&
          DownloadExactEdramImage(resources, recovered, detail) && recovered[kTargetDword] == 1 &&
          recovered[kGuardDword] == kGuardValue &&
          rex::graphics::metal::GetExactOutputMergerUploadStats(resources,
                                                                &upload_after_download_recovery) &&
          rex::graphics::metal::GetExactOutputMergerSubmissionStats(resources,
                                                                    &submission_after_recovery) &&
          upload_after_download_recovery.buffer_allocation_count ==
              upload_after_download_failure.buffer_allocation_count &&
          submission_after_recovery.peak_committed_draw_command_buffer_count <=
              rex::graphics::metal::kExactOutputMergerCommandBufferCap &&
          submission_after_recovery.max_draws_per_command_buffer ==
              rex::graphics::metal::kExactOutputMergerDrawsPerCommandBuffer &&
          submission_after_recovery.max_committed_draw_command_buffer_count ==
              rex::graphics::metal::kExactOutputMergerCommandBufferCap;
      if (!succeeded && detail.empty()) {
        detail = "exact resources did not recover cleanly after download failure";
      }
    }

    rex::graphics::metal::ReleaseExactOutputMergerResources(resources);
    [fault_queue release];
  }

  succeeded = succeeded && counters.command_buffers_created == 4 &&
              counters.command_buffers_destroyed == counters.command_buffers_created &&
              counters.active_command_buffers == 0 && counters.injected_command_buffers == 2 &&
              counters.peak_active_command_buffers <=
                  rex::graphics::metal::kExactOutputMergerCommandBufferCap;
  if (!succeeded) {
    if (detail.empty()) {
      char message[256];
      std::snprintf(message, sizeof(message),
                    "exact async fault teardown failed: wrappers=%u/%u active=%u peak=%u "
                    "injected=%u",
                    counters.command_buffers_destroyed, counters.command_buffers_created,
                    counters.active_command_buffers, counters.peak_active_command_buffers,
                    counters.injected_command_buffers);
      detail = message;
    }
    error_out = detail;
  }
  return succeeded;
}

bool RunSyntheticGuestColorCase(rex::graphics::metal::ExactOutputMergerPipeline* pipeline,
                                void* resources, uint32_t sample_count,
                                const SyntheticGuestColorTranslation& translation,
                                std::string& error_out) {
  using rex::graphics::metal::ExactOutputMergerDrawState;
  using rex::graphics::metal::ExactOutputMergerDrawStateInput;
  using rex::graphics::xenos::MsaaSamples;
  const auto msaa_samples = static_cast<MsaaSamples>(sample_count == 4   ? 2
                                                     : sample_count == 2 ? 1
                                                                         : 0);
  ExactOutputMergerDrawStateInput input;
  input.surface_pitch_pixels = 80;
  input.coverage_width = 1;
  input.coverage_height = 1;
  input.msaa_samples = msaa_samples;
  input.color_targets[0].base_tiles = rex::graphics::xenos::kEdramTileCount - 1;
  input.color_targets[0].write_mask = 0xF;
  input.depth_stencil.base_tiles = 1024;

  ExactOutputMergerDrawState state;
  if (!rex::graphics::metal::BuildExactOutputMergerDrawState(input, state, &error_out) ||
      state.active_color_target_mask != 1 || state.depth_stencil_enabled) {
    if (error_out.empty()) {
      error_out = "synthetic guest color state contract mismatch";
    }
    return false;
  }

  ExactEdramImage words = MakeExactEdramImage();
  std::array<size_t, 4> target_indices = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
  for (uint32_t sample = 0; sample < sample_count; ++sample) {
    if (!rex::graphics::metal::GetExactOutputMergerDwordIndex(state.color_surfaces[0], 0, 0, sample,
                                                              0, target_indices[sample]) ||
        IsTargetIndex(target_indices, sample, target_indices[sample])) {
      error_out = "synthetic guest color test generated invalid sample addresses";
      return false;
    }
    words[target_indices[sample]] = UINT32_C(0x10203040);
  }
  const size_t guard_index = FindGuardIndex(target_indices, sample_count, target_indices[0]);
  if (guard_index == SIZE_MAX) {
    error_out = "synthetic guest color test could not reserve a guard word";
    return false;
  }
  words[guard_index] = UINT32_C(0xA55AA55A);

  rex::graphics::metal::ExactOutputMergerUnroutableDraw draw;
  draw.pipeline = pipeline;
  draw.coverage_width = 1;
  draw.coverage_height = 1;
  draw.sample_count = sample_count;
  draw.fragment_inline_buffers[0] = {&state.system_constants, sizeof(state.system_constants),
                                     translation.system_constants_binding};
  draw.fragment_inline_buffer_count = 1;
  draw.vertex_count = 3;
  if (!UploadExactEdramImage(resources, words, error_out) ||
      rex::graphics::metal::ExecuteExactOutputMergerUnroutableDraw(resources, draw, &error_out) !=
          rex::graphics::metal::ExactOutputMergerExecutionResult::kCompleted ||
      !DownloadExactEdramImage(resources, words, error_out)) {
    return false;
  }

  for (uint32_t sample = 0; sample < sample_count; ++sample) {
    if (words[target_indices[sample]] != UINT32_MAX) {
      char message[192];
      std::snprintf(message, sizeof(message),
                    "synthetic translated guest color mismatch sample=%u at %ux "
                    "expected=FFFFFFFF actual=%08X",
                    sample, sample_count, words[target_indices[sample]]);
      error_out = message;
      return false;
    }
  }
  if (words[guard_index] != UINT32_C(0xA55AA55A)) {
    error_out = "synthetic translated guest color wrote outside its packed words";
    return false;
  }
  return true;
}

bool RunMetalEdgeSampleIdentityCase(void* resources, uint32_t sample_count,
                                    std::string& error_out) {
  using rex::graphics::metal::ExactOutputMergerExecutionResult;
  using rex::graphics::metal::ExactOutputMergerPipelineLayout;
  using rex::graphics::metal::ExactOutputMergerUnroutableDraw;
  ExactOutputMergerPipelineLayout layout;
  layout.fragment.system_constants_buffer_index = 0;
  layout.edram_fragment_buffer_index = 1;
  auto* pipeline =
      rex::graphics::metal::CreateExactOutputMergerPipelineForInternalProbeFromMslSources(
          resources, kLeftHalfVertexMsl, "edge_vertex", kSampleIdentityFragmentMsl,
          "sample_identity_fragment", sample_count, layout, &error_out);
  if (!pipeline) {
    return false;
  }

  constexpr uint32_t kBaseDword = 765432;
  constexpr uint32_t kUntouched = UINT32_C(0xDEADBEEF);
  SampleIdentityConstants constants = {kBaseDword, sample_count};
  ExactEdramImage words = MakeExactEdramImage(kUntouched);
  ExactOutputMergerUnroutableDraw draw;
  draw.pipeline = pipeline;
  draw.coverage_width = 1;
  draw.coverage_height = 1;
  draw.sample_count = sample_count;
  draw.fragment_inline_buffers[0] = {&constants, sizeof(constants), 0};
  draw.fragment_inline_buffer_count = 1;
  draw.vertex_count = 6;
  bool succeeded =
      UploadExactEdramImage(resources, words, error_out) &&
      rex::graphics::metal::ExecuteExactOutputMergerUnroutableDraw(resources, draw, &error_out) ==
          ExactOutputMergerExecutionResult::kCompleted &&
      DownloadExactEdramImage(resources, words, error_out);
  if (succeeded) {
    std::array<uint32_t, 4> actual = {};
    actual.fill(kUntouched);
    for (uint32_t sample = 0; sample < sample_count; ++sample) {
      actual[sample] = words[kBaseDword + sample];
    }
    std::array<uint32_t, 4> expected = {};
    expected.fill(kUntouched);
    if (sample_count == 2) {
      expected[1] = UINT32_C(0x02040401);
    } else if (sample_count == 4) {
      expected[0] = UINT32_C(0x01020600);
      expected[2] = UINT32_C(0x040A0202);
    } else {
      succeeded = false;
      error_out = "sample identity case requires 2x or 4x MSAA";
    }
    if (succeeded && actual != expected) {
      char message[320];
      std::snprintf(message, sizeof(message),
                    "Metal edge sample identity mismatch at %ux expected=%08X/%08X/%08X/%08X "
                    "actual=%08X/%08X/%08X/%08X",
                    sample_count, expected[0], expected[1], expected[2], expected[3], actual[0],
                    actual[1], actual[2], actual[3]);
      error_out = message;
      succeeded = false;
    }
  }
  rex::graphics::metal::ReleaseExactOutputMergerPipeline(pipeline);
  return succeeded;
}

bool RunSyntheticGuestColorEdgeCoverageCase(
    rex::graphics::metal::ExactOutputMergerPipeline* pipeline, void* resources,
    uint32_t sample_count, const SyntheticGuestColorTranslation& translation,
    std::string& error_out) {
  using rex::graphics::metal::ExactOutputMergerDrawState;
  using rex::graphics::metal::ExactOutputMergerDrawStateInput;
  using rex::graphics::xenos::MsaaSamples;
  if (sample_count != 2 && sample_count != 4) {
    error_out = "edge-coverage case requires 2x or 4x MSAA";
    return false;
  }
  const auto msaa_samples = static_cast<MsaaSamples>(sample_count == 4 ? 2 : 1);
  ExactOutputMergerDrawStateInput input;
  input.surface_pitch_pixels = 80;
  input.coverage_width = 1;
  input.coverage_height = 1;
  input.msaa_samples = msaa_samples;
  input.color_targets[0].base_tiles = 511;
  input.color_targets[0].write_mask = 0xF;
  ExactOutputMergerDrawState state;
  if (!rex::graphics::metal::BuildExactOutputMergerDrawState(input, state, &error_out)) {
    return false;
  }

  ExactEdramImage words = MakeExactEdramImage(UINT32_C(0x13579BDF));
  std::array<size_t, 4> target_indices = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
  for (uint32_t sample = 0; sample < sample_count; ++sample) {
    if (!rex::graphics::metal::GetExactOutputMergerDwordIndex(state.color_surfaces[0], 0, 0, sample,
                                                              0, target_indices[sample])) {
      error_out = "edge-coverage case generated an invalid sample address";
      return false;
    }
    words[target_indices[sample]] = UINT32_C(0x10203040);
  }

  rex::graphics::metal::ExactOutputMergerUnroutableDraw draw;
  draw.pipeline = pipeline;
  draw.coverage_width = 1;
  draw.coverage_height = 1;
  draw.sample_count = sample_count;
  draw.fragment_inline_buffers[0] = {&state.system_constants, sizeof(state.system_constants),
                                     translation.system_constants_binding};
  draw.fragment_inline_buffer_count = 1;
  draw.vertex_count = 6;
  if (!UploadExactEdramImage(resources, words, error_out) ||
      rex::graphics::metal::ExecuteExactOutputMergerUnroutableDraw(resources, draw, &error_out) !=
          rex::graphics::metal::ExactOutputMergerExecutionResult::kCompleted ||
      !DownloadExactEdramImage(resources, words, error_out)) {
    return false;
  }

  // The left half covers Metal sample 1 at 2x and samples 0/2 at 4x.
  // The translator remaps those to Xenos samples 0 and 0/1 respectively.
  const uint32_t expected_written_samples = sample_count == 2 ? 1 : 2;
  uint32_t actual_written_mask = 0;
  for (uint32_t sample = 0; sample < sample_count; ++sample) {
    if (words[target_indices[sample]] == UINT32_MAX) {
      actual_written_mask |= UINT32_C(1) << sample;
    }
  }
  for (uint32_t sample = 0; sample < sample_count; ++sample) {
    const uint32_t expected = sample < expected_written_samples ? UINT32_MAX : UINT32_C(0x10203040);
    if (words[target_indices[sample]] != expected) {
      char message[256];
      std::snprintf(message, sizeof(message),
                    "edge-coverage sample mapping mismatch at %ux sample=%u "
                    "expected=%08X actual=%08X written_mask=%X",
                    sample_count, sample, expected, words[target_indices[sample]],
                    actual_written_mask);
      error_out = message;
      return false;
    }
  }
  return true;
}

bool RunPipelineAttestationNegativeCase(rex::graphics::metal::ExactOutputMergerPipeline* pipeline,
                                        void* resources, uint32_t pipeline_sample_count,
                                        std::string& error_out) {
  ExactEdramImage before = MakeExactEdramImage(UINT32_C(0x5A17E57A));
  ExactEdramImage after = MakeExactEdramImage();
  if (!UploadExactEdramImage(resources, before, error_out)) {
    return false;
  }

  rex::graphics::metal::ExactOutputMergerUnroutableDraw mismatched_draw;
  mismatched_draw.pipeline = pipeline;
  mismatched_draw.coverage_width = 1;
  mismatched_draw.coverage_height = 1;
  mismatched_draw.sample_count = pipeline_sample_count == 1 ? 2 : 1;
  mismatched_draw.vertex_count = 3;
  std::string rejection;
  if (rex::graphics::metal::ExecuteExactOutputMergerUnroutableDraw(resources, mismatched_draw,
                                                                   &rejection) !=
          rex::graphics::metal::ExactOutputMergerExecutionResult::kRejectedBeforeSubmit ||
      rejection.empty() || !DownloadExactEdramImage(resources, after, error_out) ||
      before != after) {
    error_out = "exact output-merger accepted a pipeline/pass sample mismatch or "
                "changed authority before rejection";
    return false;
  }
  return true;
}

bool RunPipelineReflectionNegativeCase(void* resources, const char* vertex_source,
                                       const char* fragment_source, uint32_t actual_edram_binding,
                                       std::string& error_out) {
  std::string rejection;
  auto* pipeline = CreateCoveragePipeline(resources, vertex_source, "probe_vertex", fragment_source,
                                          "main0", 1, actual_edram_binding + 1, rejection);
  if (pipeline || rejection.empty()) {
    rex::graphics::metal::ReleaseExactOutputMergerPipeline(pipeline);
    error_out = "exact output-merger accepted a pipeline with the wrong reflected "
                "xe_edram binding";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      std::fprintf(stderr, "[metal_raster_order_probe_test] FAIL: no Metal device\n");
      return 1;
    }

    // The exact path has no guest color attachments, but the Metal scaffold
    // supplies a real dummy coverage attachment. Advertise native 2x for both
    // translator cases so its coverage-mask and sample-position mapping match
    // the 2x Metal pass instead of the attachmentless 4x fallback.
    auto translator = rex::graphics::metal::CreateExactOutputMergerShaderTranslator();
    if (!translator) {
      std::fprintf(stderr, "[metal_raster_order_probe_test] FAIL: exact translator creation\n");
      return 1;
    }
    std::string translated_source;
    std::vector<uint8_t> translated_spirv;
    std::string error;
    if (!rex::graphics::metal::CreateDepthOnlyFragmentMslSource(
            *translator,
            rex::graphics::SpirvShaderTranslator::Modification::DepthStencilMode::kNoModifiers,
            translated_source, &error, &translated_spirv)) {
      std::fprintf(stderr, "[metal_raster_order_probe_test] FAIL: interlock translation: %s\n",
                   error.c_str());
      return 1;
    }
    bool has_pixel_interlock =
        HasExecutionMode(translated_spirv, spv::ExecutionModePixelInterlockOrderedEXT);
    bool has_sample_interlock =
        HasExecutionMode(translated_spirv, spv::ExecutionModeSampleInterlockOrderedEXT);
    uint32_t translated_edram_binding = UINT32_MAX;
    bool has_rog_binding = rex::graphics::metal::FindExactOutputMergerEdramBinding(
        translated_source, translated_edram_binding, &error);
    uint32_t translated_system_constants_binding = UINT32_MAX;
    bool has_system_constants_binding = FindNamedBufferBinding(
        translated_source, "xe_uniform_system_constants", translated_system_constants_binding);
    bool has_edram_read = translated_source.find("= xe_edram.edram[") != std::string::npos;
    bool has_edram_write = translated_source.find("xe_edram.edram[") != std::string::npos &&
                           translated_source.find("] = ") != std::string::npos;
    if (!has_pixel_interlock || has_sample_interlock || !has_rog_binding ||
        !has_system_constants_binding || translated_edram_binding != 1u || !has_edram_read ||
        !has_edram_write) {
      std::fprintf(stderr,
                   "[metal_raster_order_probe_test] FAIL: translated contract "
                   "pixel=%d sample=%d rog=%d binding=%u constants=%d/%u "
                   "read=%d write=%d\n",
                   int(has_pixel_interlock), int(has_sample_interlock), int(has_rog_binding),
                   translated_edram_binding, int(has_system_constants_binding),
                   translated_system_constants_binding, int(has_edram_read), int(has_edram_write));
      return 1;
    }

    SyntheticGuestVertexTranslation synthetic_guest_vertex;
    if (!CreateSyntheticGuestVertexTranslation(*translator, device, synthetic_guest_vertex,
                                               error)) {
      std::fprintf(stderr,
                   "[metal_raster_order_probe_test] FAIL: synthetic guest "
                   "vertex translation: %s\n",
                   error.c_str());
      return 1;
    }
    SyntheticGuestColorTranslation synthetic_guest_color;
    if (!CreateSyntheticGuestColorTranslation(*translator, device, synthetic_guest_color, error)) {
      std::fprintf(stderr,
                   "[metal_raster_order_probe_test] FAIL: synthetic guest "
                   "color translation: %s\n",
                   error.c_str());
      return 1;
    }

    id<MTLCommandQueue> queue = [device newCommandQueue];
    void* resources = rex::graphics::metal::CreateExactOutputMergerResources(device, &error);
    bool succeeded =
        queue != nil && resources != nullptr &&
        rex::graphics::metal::SetExactOutputMergerCommandQueue(resources, queue, &error) &&
        RunLayoutCases(error) && RunCoverageResourceCases(device, queue, resources, error) &&
        RunPipelineReflectionNegativeCase(resources, kProbeVertexMsl, translated_source.c_str(),
                                          translated_edram_binding, error) &&
        RunSourceAttestationNegativeCases(resources, error) &&
        RunBorrowedPipelineReflectionNegativeCase(
            resources, kProbeVertexMsl, translated_source.c_str(),
            translated_system_constants_binding, translated_edram_binding, error) &&
        RunFixedPrimitiveRestartCases(resources, error);
    if (!succeeded) {
      if (error.empty()) {
        error = queue ? "failed to create exact output-merger resources"
                      : "failed to create Metal command queue";
      }
    }

    constexpr uint32_t kSampleCounts[] = {1, 2, 4};
    for (uint32_t sample_count : kSampleCounts) {
      if (!succeeded) {
        break;
      }
      if (![device supportsTextureSampleCount:sample_count]) {
        error = "required 1x/2x/4x sample count is unsupported";
        succeeded = false;
        break;
      }

      if (!RunProductionTranslationPipelineCase(resources, device, sample_count,
                                                synthetic_guest_vertex, synthetic_guest_color,
                                                error)) {
        succeeded = false;
        break;
      }

      auto* translated_pipeline = CreateCoveragePipeline(
          resources, kProbeVertexMsl, "probe_vertex", translated_source.c_str(), "main0",
          sample_count, translated_edram_binding, error);
      auto* probe_pipeline =
          CreateCoveragePipeline(resources, kProbeVertexMsl, "probe_vertex", kProbeFragmentMsl,
                                 "probe_fragment", sample_count, translated_edram_binding, error);
      auto* synthetic_guest_color_pipeline =
          CreateCoveragePipeline(resources, kProbeVertexMsl, "probe_vertex",
                                 synthetic_guest_color.translation->msl_source().c_str(), "main0",
                                 sample_count, synthetic_guest_color.edram_binding, error);
      auto* synthetic_guest_color_edge_pipeline =
          CreateCoveragePipeline(resources, kLeftHalfVertexMsl, "edge_vertex",
                                 synthetic_guest_color.translation->msl_source().c_str(), "main0",
                                 sample_count, synthetic_guest_color.edram_binding, error);
      rex::graphics::metal::ExactOutputMergerPipelineLayout borrowed_layout;
      borrowed_layout.fragment.system_constants_buffer_index = translated_system_constants_binding;
      borrowed_layout.edram_fragment_buffer_index = translated_edram_binding;
      auto* borrowed_pipeline =
          rex::graphics::metal::CreateExactOutputMergerPipelineForInternalProbeFromMslSources(
              resources, kProbeVertexMsl, "probe_vertex", translated_source.c_str(), "main0",
              sample_count, borrowed_layout, &error);
      if (!translated_pipeline || !probe_pipeline || !synthetic_guest_color_pipeline ||
          !synthetic_guest_color_edge_pipeline || !borrowed_pipeline) {
        char pipeline_error[512];
        std::snprintf(pipeline_error, sizeof(pipeline_error),
                      "pipeline creation failed at %ux translated=%d ordered=%d guest=%d "
                      "edge=%d borrowed=%d detail=%s",
                      sample_count, translated_pipeline != nullptr, probe_pipeline != nullptr,
                      synthetic_guest_color_pipeline != nullptr,
                      synthetic_guest_color_edge_pipeline != nullptr, borrowed_pipeline != nullptr,
                      error.c_str());
        error = pipeline_error;
      }
      bool edge_sample_identity_ok =
          sample_count == 1 || RunMetalEdgeSampleIdentityCase(resources, sample_count, error);
      succeeded =
          translated_pipeline && probe_pipeline && synthetic_guest_color_pipeline &&
          synthetic_guest_color_edge_pipeline && borrowed_pipeline && edge_sample_identity_ok &&
          RunFullBorrowedResourceCase(device, resources, sample_count, error) &&
          RunBorrowedMetalIndexCase(device, resources, sample_count, error) &&
          RunPipelineAttestationNegativeCase(translated_pipeline, resources, sample_count, error) &&
          RunTranslatedCoverageCase(translated_pipeline, resources, sample_count,
                                    translated_system_constants_binding, error) &&
          RunBorrowedExecutorCases(borrowed_pipeline, resources, sample_count, borrowed_layout,
                                   error) &&
          RunSyntheticGuestColorCase(synthetic_guest_color_pipeline, resources, sample_count,
                                     synthetic_guest_color, error) &&
          (sample_count == 1 ||
           RunSyntheticGuestColorEdgeCoverageCase(synthetic_guest_color_edge_pipeline, resources,
                                                  sample_count, synthetic_guest_color, error)) &&
          RunOrderedCases(probe_pipeline, resources, sample_count, error);
      rex::graphics::metal::ReleaseExactOutputMergerPipeline(translated_pipeline);
      rex::graphics::metal::ReleaseExactOutputMergerPipeline(probe_pipeline);
      rex::graphics::metal::ReleaseExactOutputMergerPipeline(synthetic_guest_color_pipeline);
      rex::graphics::metal::ReleaseExactOutputMergerPipeline(synthetic_guest_color_edge_pipeline);
      rex::graphics::metal::ReleaseExactOutputMergerPipeline(borrowed_pipeline);
    }

    if (succeeded) {
      succeeded = RunAsyncCommandBufferFailureCase(device, queue, error);
    }
    rex::graphics::metal::ReleaseExactOutputMergerResources(resources);
    [queue release];

    if (!succeeded) {
      std::fprintf(stderr, "[metal_raster_order_probe_test] FAIL: %s\n", error.c_str());
      return 1;
    }
    std::fprintf(stdout,
                 "[metal_raster_order_probe_test] PASS: PixelInterlockOrderedEXT lowered to "
                 "xe_edram buffer(1), raster_order_group(0); exact 10 MiB canonical "
                 "EDRAM ownership, all 12 color formats, depth, aliases/wrap/bounds, "
                 "translated depth and synthetic guest color writes, transactional "
                 "authority, resident BGRA 2D-array textures, fixed uint16/uint32 "
                 "strip restart, bounded async batching/fault recovery, dummy coverage "
                 "pipelines and %u ordered packed "
                 "RMW/write-mask/blend/depth repetitions passed at 1x/2x/4x\n",
                 4u * kRepeatCount);
    return 0;
  }
}
