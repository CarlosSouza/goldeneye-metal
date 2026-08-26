#import <Metal/Metal.h>

#include <rex/graphics/metal/exact_output_merger.h>
#include <rex/graphics/metal/exact_output_merger_state.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/graphics/pipeline/render_target/cache.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/util/draw.h>

namespace rex::graphics::metal {

class ExactOutputMergerPipeline {
 public:
  id<MTLDevice> device = nil;
  id<MTLRenderPipelineState> state = nil;
  uint32_t sample_count = 0;
  uint32_t edram_fragment_buffer_index = UINT32_MAX;
  ExactOutputMergerPipelineLayout layout;
  std::array<size_t, kExactOutputMergerMaxMetalBufferIndex + 1> vertex_min_buffer_lengths = {};
  std::array<size_t, kExactOutputMergerMaxMetalBufferIndex + 1> fragment_min_buffer_lengths = {};
  std::vector<MTLTextureType> vertex_texture_types;
  std::vector<MTLTextureType> fragment_texture_types;
  bool production_translation_contract = false;
  bool has_complete_layout = false;
};

namespace {

constexpr uint32_t kEdramWordsPerTile =
    xenos::kEdramTileWidthSamples * xenos::kEdramTileHeightSamples;
constexpr uint32_t kMaximumSurfacePitchPixels = (uint32_t(1) << xenos::kEdramPitchPixelsBits) - 1;
constexpr uint32_t kMaximumSurfacePitchTiles = (uint32_t(1) << xenos::kEdramPitchTilesBits) - 1;
// macOS Metal family devices capable of this path support at least 16384 in
// each 2D dimension. Keep the probe scaffold at that conservative portable
// ceiling rather than attempting an allocation that Metal would reject.
constexpr uint32_t kMaximumCoverageTextureDimension = 16384;
constexpr size_t kMaximumInlineArgumentBytes = 64 * 1024;
constexpr size_t kMaximumCpuTextureSlotBytes = 128 * 1024 * 1024;
constexpr size_t kMaximumCpuTextureAggregateBytes = 256 * 1024 * 1024;
constexpr size_t kMaximumCpuIndexUploadBytes = 64 * 1024 * 1024;
constexpr uint32_t kExactDrawsPerCommandBuffer = kExactOutputMergerDrawsPerCommandBuffer;
constexpr uint32_t kMaximumCommittedExactCommandBuffers = kExactOutputMergerCommandBufferCap;
constexpr size_t kExactUploadAlignment = 256;
constexpr size_t kExactUploadChunkSize = 1 << 20;
constexpr uint32_t kInvalidExactUploadArena = UINT32_MAX;
constexpr size_t kMaximumExactSamplerCacheEntries = 4096;
std::atomic<uint64_t> g_next_exact_context_identity{1};

struct CoverageTexture {
  id<MTLTexture> texture = nil;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct ExactUploadChunk {
  id<MTLBuffer> buffer = nil;
  size_t capacity = 0;
  size_t offset = 0;
};

struct ExactUploadArena {
  std::vector<ExactUploadChunk> chunks;
  bool in_use = false;
};

struct ExactUploadAllocation {
  id<MTLBuffer> buffer = nil;
  NSUInteger offset = 0;
};

struct ExactUploadArenaCheckpoint {
  uint32_t arena_index = kInvalidExactUploadArena;
  std::vector<size_t> chunk_offsets;
  PipelineProbeUploadStats upload_stats;
};

struct CommittedExactCommandBuffer {
  // Explicit +1 ownership transferred from the open command buffer.
  id<MTLCommandBuffer> command_buffer = nil;
  uint32_t draw_count = 0;
  // Auxiliary exact consumers count as one pending submission for bounded
  // backpressure and profile visibility even though they contain no draw.
  uint32_t pending_work_count = 0;
  uint32_t upload_arena_index = kInvalidExactUploadArena;
  void (*async_failure_callback)(void* context, uint32_t start, uint32_t length) = nullptr;
  void* async_failure_callback_context = nullptr;
  uint32_t async_failure_start = 0;
  uint32_t async_failure_length = 0;
};

struct ExactOutputMergerResources {
  // All authority transitions, whole-image CPU access, coverage-cache access,
  // queue pinning and submission queue mutations are one serialized
  // transaction. recursive_mutex permits the executor to call the public
  // coverage-pass configurator while retaining ownership.
  std::recursive_mutex mutex;
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> command_queue = nil;
  id<MTLBuffer> edram = nil;
  id<MTLComputePipelineState> resolve_pipeline = nil;
  id<MTLComputePipelineState> clear_pipeline = nil;
  std::array<CoverageTexture, 3> coverage;
  ExactOutputMergerAuthorityTracker authority;
  uint64_t gpu_epoch_sequence = 0;

  // commandBuffer and renderCommandEncoderWithDescriptor return autoreleased
  // objects. These explicit +1 references keep an open batch alive across
  // ExecuteExactOutputMergerDraw calls and autorelease pools.
  id<MTLCommandBuffer> open_command_buffer = nil;
  id<MTLRenderCommandEncoder> open_render_encoder = nil;
  uint32_t open_coverage_width = 0;
  uint32_t open_coverage_height = 0;
  uint32_t open_sample_count = 0;
  uint32_t open_draw_count = 0;
  uint32_t open_upload_arena_index = kInvalidExactUploadArena;
  std::vector<CommittedExactCommandBuffer> committed_command_buffers;
  std::array<ExactUploadArena, kMaximumCommittedExactCommandBuffers> upload_arenas;
  std::unordered_map<uint64_t, id<MTLSamplerState>> sampler_cache;
  PipelineProbeUploadStats upload_stats;
  PipelineProbeSubmissionStats submission_stats;
};

void SetError(std::string* error_out, std::string_view error) {
  if (error_out) {
    *error_out = error;
  }
}

bool IsExactEdramDispatchAddressingBounded(const draw_util::ResolveEdramInfo& surface,
                                           const draw_util::ResolveCoordinateInfo& coordinates,
                                           uint32_t width, uint32_t height) {
  if (!surface.pitch_tiles || surface.msaa_samples > xenos::MsaaSamples::k4X || !width || !height) {
    return false;
  }
  const uint32_t sample_x_log2 = uint32_t(surface.msaa_samples >= xenos::MsaaSamples::k4X);
  const uint32_t sample_y_log2 = uint32_t(surface.msaa_samples >= xenos::MsaaSamples::k2X);
  const uint64_t pixel_x_last = uint64_t(coordinates.edram_offset_x_div_8) * 8 + width - 1;
  const uint64_t pixel_y_last = uint64_t(coordinates.edram_offset_y_div_8) * 8 + height - 1;
  const uint64_t sample_x_last = (pixel_x_last << sample_x_log2) + sample_x_log2;
  const uint64_t sample_y_last = (pixel_y_last << sample_y_log2) + sample_y_log2;
  if (sample_x_last > UINT32_MAX || sample_y_last > UINT32_MAX) {
    return false;
  }
  const uint64_t tile_x_last = sample_x_last / xenos::kEdramTileWidthSamples;
  const uint64_t tile_y_last = sample_y_last / xenos::kEdramTileHeightSamples;
  return uint64_t(surface.base_tiles) + tile_y_last * surface.pitch_tiles + tile_x_last <=
         UINT32_MAX;
}

constexpr char kExactResolveAndClearMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct ExactResolveConstants {
  uint edram_info;
  uint coordinate_info;
  uint destination_info;
  uint destination_coordinate_info;
  uint destination_base;
  uint copy_width;
  uint copy_height;
  uint destination_buffer_size;
};

struct ExactClearConstants {
  uint clear_value_0;
  uint clear_value_1;
  uint edram_info;
  uint coordinate_info;
  uint clear_width;
  uint clear_height;
};

static uint exact_edram_dword(uint edram_info, uint2 pixel, uint sample) {
  uint pitch_tiles = edram_info & 0x3FFu;
  uint msaa = (edram_info >> 10u) & 3u;
  bool is_depth = ((edram_info >> 12u) & 1u) != 0u;
  uint base_tiles = (edram_info >> 13u) & 0x7FFu;
  uint sample_x_log2 = msaa >= 2u ? 1u : 0u;
  uint sample_y_log2 = msaa >= 1u ? 1u : 0u;
  uint sample_x = (pixel.x << sample_x_log2) +
                  (sample_x_log2 != 0u ? (sample >> sample_y_log2) : 0u);
  uint sample_y = (pixel.y << sample_y_log2) +
                  (sample_y_log2 != 0u ? (sample & ((1u << sample_y_log2) - 1u)) : 0u);
  uint tile_x = sample_x / 80u;
  uint tile_y = sample_y / 16u;
  uint tile = (base_tiles + tile_y * pitch_tiles + tile_x) & 2047u;
  uint tile_x_sample = sample_x % 80u;
  if (is_depth) {
    tile_x_sample = (tile_x_sample + 40u) % 80u;
  }
  return tile * (80u * 16u) + (sample_y % 16u) * 80u + tile_x_sample;
}

static uint exact_sample_mask(uint msaa, uint selection) {
  uint sample_count = 1u << msaa;
  uint full_mask = (1u << sample_count) - 1u;
  if (selection <= 3u) {
    return selection < sample_count ? (1u << selection) : 0u;
  }
  if (selection == 4u) {
    return full_mask & 3u;
  }
  if (selection == 5u) {
    return full_mask & 12u;
  }
  return selection == 6u ? full_mask : 0u;
}

static uint exact_resolve_unorm_divide(uint sum, uint divisor) {
  // The canonical resolve averages UNORM values and repacks with +0.5 before
  // truncation, so exact half ties round upward (not to even).
  return (sum + divisor / 2u) / divisor;
}

static uint exact_average_8888(device const uint* edram, uint edram_info, uint2 pixel,
                               uint sample_mask) {
  uint4 sums = uint4(0u);
  uint count = 0u;
  for (uint sample = 0u; sample < 4u; ++sample) {
    if ((sample_mask & (1u << sample)) == 0u) {
      continue;
    }
    uint packed = edram[exact_edram_dword(edram_info, pixel, sample)];
    sums += uint4(packed & 255u, (packed >> 8u) & 255u, (packed >> 16u) & 255u,
                  packed >> 24u);
    ++count;
  }
  if (count == 0u) {
    return 0u;
  }
  uint4 averaged = uint4(exact_resolve_unorm_divide(sums.x, count),
                         exact_resolve_unorm_divide(sums.y, count),
                         exact_resolve_unorm_divide(sums.z, count),
                         exact_resolve_unorm_divide(sums.w, count));
  return averaged.x | (averaged.y << 8u) | (averaged.z << 16u) | (averaged.w << 24u);
}

static uint exact_tiled_rgba8_offset(uint x, uint y, uint pitch) {
  constexpr uint bytes_per_pixel_log2 = 2u;
  uint aligned_pitch = (pitch + 31u) & ~31u;
  uint row_macro = ((y / 32u) * (aligned_pitch / 32u)) << (bytes_per_pixel_log2 + 7u);
  uint row_micro = ((y & 6u) << 2u) << bytes_per_pixel_log2;
  uint row_base = row_macro + ((row_micro & ~15u) << 1u) + (row_micro & 15u) +
                  ((y & 8u) << (3u + bytes_per_pixel_log2)) + ((y & 1u) << 4u);
  uint column_macro = (x / 32u) << (bytes_per_pixel_log2 + 7u);
  uint column_micro = (x & 7u) << bytes_per_pixel_log2;
  uint offset = row_base + column_macro + ((column_micro & ~15u) << 1u) +
                (column_micro & 15u);
  return ((offset & ~511u) << 3u) + ((offset & 448u) << 2u) + (offset & 63u) +
         ((y & 16u) << 7u) + (((((y & 8u) >> 2u) + (x >> 3u)) & 3u) << 6u);
}

static uint exact_apply_endian(uint value, uint endian) {
  if (endian == 1u || endian == 2u) {
    value = ((value & 0x00FF00FFu) << 8u) | ((value & 0xFF00FF00u) >> 8u);
  }
  if (endian == 2u || endian == 3u) {
    value = (value << 16u) | (value >> 16u);
  }
  return value;
}

kernel void exact_resolve_8888_or_depth(
    device const uint* edram [[buffer(0)]], device uint* destination [[buffer(1)]],
    constant ExactResolveConstants& constants [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]) {
  if (position.x >= constants.copy_width || position.y >= constants.copy_height) {
    return;
  }
  uint msaa = (constants.edram_info >> 10u) & 3u;
  bool is_depth = ((constants.edram_info >> 12u) & 1u) != 0u;
  uint selection = (constants.destination_coordinate_info >> 28u) & 7u;
  uint sample_mask = exact_sample_mask(msaa, selection);
  uint source_x = ((constants.coordinate_info >> 0u) & 15u) * 8u + position.x;
  uint source_y = ((constants.coordinate_info >> 4u) & 1u) * 8u + position.y;
  uint packed = 0u;
  if (is_depth) {
    uint sample = ctz(sample_mask);
    packed = edram[exact_edram_dword(constants.edram_info, uint2(source_x, source_y), sample)];
  } else {
    packed = exact_average_8888(edram, constants.edram_info, uint2(source_x, source_y),
                                sample_mask);
    if ((constants.destination_info & 0x01000000u) != 0u) {
      packed = (packed & 0xFF00FF00u) | ((packed & 0x00FF0000u) >> 16u) |
               ((packed & 0x000000FFu) << 16u);
    }
  }
  packed = exact_apply_endian(packed, constants.destination_info & 7u);
  uint destination_pitch = (constants.destination_coordinate_info & 0x3FFu) * 32u;
  uint destination_x = ((constants.destination_coordinate_info >> 20u) & 15u) * 8u + position.x;
  uint destination_y = ((constants.destination_coordinate_info >> 24u) & 15u) * 8u + position.y;
  uint byte_offset = constants.destination_base +
                     exact_tiled_rgba8_offset(destination_x, destination_y, destination_pitch);
  if (byte_offset <= constants.destination_buffer_size - 4u) {
    destination[byte_offset >> 2u] = packed;
  }
}

kernel void exact_clear_32bpp_edram(
    device uint* edram [[buffer(0)]], constant ExactClearConstants& constants [[buffer(1)]],
    uint2 position [[thread_position_in_grid]]) {
  if (position.x >= constants.clear_width || position.y >= constants.clear_height) {
    return;
  }
  uint origin_x = ((constants.coordinate_info >> 0u) & 15u) * 8u;
  uint origin_y = ((constants.coordinate_info >> 4u) & 1u) * 8u;
  uint sample_count = 1u << ((constants.edram_info >> 10u) & 3u);
  uint2 pixel = uint2(origin_x + position.x, origin_y + position.y);
  for (uint sample = 0u; sample < sample_count; ++sample) {
    edram[exact_edram_dword(constants.edram_info, pixel, sample)] = constants.clear_value_0;
  }
}
)MSL";

bool EnsureExactResolveAndClearPipelines(ExactOutputMergerResources* resources,
                                         std::string* error_out) {
  if (!resources || !resources->device) {
    SetError(error_out, "exact resolve Metal device is unavailable");
    return false;
  }
  if (resources->resolve_pipeline && resources->clear_pipeline) {
    return true;
  }
  NSError* error = nil;
  id<MTLLibrary> library = [resources->device
      newLibraryWithSource:[NSString stringWithUTF8String:kExactResolveAndClearMsl]
                   options:nil
                     error:&error];
  if (!library) {
    SetError(error_out, error ? std::string_view([[error localizedDescription] UTF8String])
                              : std::string_view("exact resolve Metal library failed"));
    return false;
  }
  id<MTLFunction> resolve_function = [library newFunctionWithName:@"exact_resolve_8888_or_depth"];
  id<MTLFunction> clear_function = [library newFunctionWithName:@"exact_clear_32bpp_edram"];
  id<MTLComputePipelineState> resolve_pipeline =
      resolve_function
          ? [resources->device newComputePipelineStateWithFunction:resolve_function error:&error]
          : nil;
  id<MTLComputePipelineState> clear_pipeline =
      clear_function
          ? [resources->device newComputePipelineStateWithFunction:clear_function error:&error]
          : nil;
  [resolve_function release];
  [clear_function release];
  [library release];
  if (!resolve_pipeline || !clear_pipeline) {
    [resolve_pipeline release];
    [clear_pipeline release];
    SetError(error_out, error ? std::string_view([[error localizedDescription] UTF8String])
                              : std::string_view("exact resolve Metal pipelines failed"));
    return false;
  }
  [resources->resolve_pipeline release];
  [resources->clear_pipeline release];
  resources->resolve_pipeline = resolve_pipeline;
  resources->clear_pipeline = clear_pipeline;
  return true;
}

bool IsMslIdentifierCharacter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

void SkipMslWhitespaceForward(const std::string& source, size_t& position) {
  while (position < source.size() && (source[position] == ' ' || source[position] == '\t' ||
                                      source[position] == '\r' || source[position] == '\n')) {
    ++position;
  }
}

void SkipMslWhitespaceBackward(const std::string& source, size_t& position) {
  while (position && (source[position - 1] == ' ' || source[position - 1] == '\t' ||
                      source[position - 1] == '\r' || source[position - 1] == '\n')) {
    --position;
  }
}

// Comment text must never be able to attest a resource qualifier or Metal
// attribute. Preserve byte positions so parameter-local parsing can still use
// offsets into the stripped source.
std::string StripMslComments(std::string_view source) {
  std::string stripped(source);
  bool line_comment = false;
  bool block_comment = false;
  for (size_t i = 0; i < stripped.size(); ++i) {
    if (line_comment) {
      if (stripped[i] == '\n') {
        line_comment = false;
      } else {
        stripped[i] = ' ';
      }
      continue;
    }
    if (block_comment) {
      if (stripped[i] == '*' && i + 1 < stripped.size() && stripped[i + 1] == '/') {
        stripped[i] = stripped[i + 1] = ' ';
        ++i;
        block_comment = false;
      } else if (stripped[i] != '\n') {
        stripped[i] = ' ';
      }
      continue;
    }
    if (stripped[i] == '/' && i + 1 < stripped.size() && stripped[i + 1] == '/') {
      stripped[i] = stripped[i + 1] = ' ';
      ++i;
      line_comment = true;
    } else if (stripped[i] == '/' && i + 1 < stripped.size() && stripped[i + 1] == '*') {
      stripped[i] = stripped[i + 1] = ' ';
      ++i;
      block_comment = true;
    }
  }
  return stripped;
}

bool IsMslTokenAt(const std::string& source, size_t position, std::string_view token) {
  return position != std::string::npos && source.compare(position, token.size(), token) == 0 &&
         (!position || !IsMslIdentifierCharacter(source[position - 1])) &&
         (position + token.size() == source.size() ||
          !IsMslIdentifierCharacter(source[position + token.size()]));
}

bool FindSoleMslToken(const std::string& source, std::string_view token, size_t& position_out) {
  position_out = std::string::npos;
  for (size_t search = 0;;) {
    const size_t candidate = source.find(token, search);
    if (candidate == std::string::npos) {
      break;
    }
    search = candidate + token.size();
    if (!IsMslTokenAt(source, candidate, token)) {
      continue;
    }
    if (position_out != std::string::npos) {
      return false;
    }
    position_out = candidate;
  }
  return position_out != std::string::npos;
}

bool ValidateExactMslEntryPoint(std::string_view source_view, std::string_view stage,
                                std::string_view function_name, bool requires_edram,
                                uint32_t& edram_binding_out, std::string* error_out) {
  edram_binding_out = UINT32_MAX;
  if (source_view.empty() || function_name.empty()) {
    SetError(error_out, "exact output-merger MSL entry source is empty");
    return false;
  }
  const std::string source = StripMslComments(source_view);
  size_t stage_position = std::string::npos;
  if (!FindSoleMslToken(source, stage, stage_position)) {
    SetError(error_out, "exact output-merger MSL must expose exactly one selected-stage entry");
    return false;
  }

  size_t function_position = std::string::npos;
  size_t parameters_begin = std::string::npos;
  for (size_t search = stage_position + stage.size();;) {
    const size_t candidate = source.find(function_name, search);
    if (candidate == std::string::npos) {
      break;
    }
    search = candidate + function_name.size();
    if (!IsMslTokenAt(source, candidate, function_name)) {
      continue;
    }
    size_t after_name = search;
    SkipMslWhitespaceForward(source, after_name);
    if (after_name >= source.size() || source[after_name] != '(') {
      continue;
    }
    if (function_position != std::string::npos ||
        source.find_first_of(";{}", stage_position + stage.size()) < candidate) {
      SetError(error_out, "exact output-merger selected MSL entry point is ambiguous");
      return false;
    }
    function_position = candidate;
    parameters_begin = after_name;
  }
  if (function_position == std::string::npos) {
    SetError(error_out, "exact output-merger selected MSL entry point is unavailable");
    return false;
  }

  size_t depth = 0;
  size_t parameters_end = std::string::npos;
  for (size_t i = parameters_begin; i < source.size(); ++i) {
    if (source[i] == '(') {
      ++depth;
    } else if (source[i] == ')' && depth && !--depth) {
      parameters_end = i;
      break;
    }
  }
  if (parameters_end == std::string::npos) {
    SetError(error_out, "exact output-merger selected MSL parameter list is malformed");
    return false;
  }

  constexpr std::string_view kEdramName = "xe_edram";
  size_t edram_name_position = std::string::npos;
  for (size_t search = parameters_begin + 1; search < parameters_end;) {
    const size_t candidate = source.find(kEdramName, search);
    if (candidate == std::string::npos || candidate >= parameters_end) {
      break;
    }
    search = candidate + kEdramName.size();
    if (IsMslTokenAt(source, candidate, kEdramName)) {
      if (edram_name_position != std::string::npos) {
        SetError(error_out, "exact output-merger entry exposes duplicate EDRAM parameters");
        return false;
      }
      edram_name_position = candidate;
    }
  }
  if (requires_edram != (edram_name_position != std::string::npos)) {
    SetError(error_out, requires_edram
                            ? "exact output-merger fragment entry has no EDRAM parameter"
                            : "exact output-merger vertex entry unexpectedly exposes EDRAM");
    return false;
  }
  if (requires_edram &&
      !FindExactOutputMergerEdramBinding(std::string(source_view), edram_binding_out, error_out)) {
    return false;
  }
  return true;
}

bool GetDenseMslBindingCount(std::string_view source_view, std::string_view attribute_name,
                             uint32_t maximum_count, uint32_t& count_out, std::string* error_out) {
  count_out = 0;
  const std::string source = StripMslComments(source_view);
  const std::string prefix = "[[" + std::string(attribute_name) + "(";
  std::vector<bool> seen(maximum_count, false);
  for (size_t search = 0;;) {
    size_t position = source.find(prefix, search);
    if (position == std::string::npos) {
      break;
    }
    position += prefix.size();
    search = position;
    uint32_t index = 0;
    bool has_digit = false;
    while (position < source.size() && source[position] >= '0' && source[position] <= '9') {
      has_digit = true;
      const uint32_t digit = uint32_t(source[position++] - '0');
      if (index > (UINT32_MAX - digit) / 10u) {
        has_digit = false;
        break;
      }
      index = index * 10u + digit;
    }
    if (!has_digit || position >= source.size() || source[position] != ')' ||
        index >= maximum_count || seen[index]) {
      SetError(error_out, "exact output-merger MSL resource binding is malformed or duplicated");
      return false;
    }
    seen[index] = true;
    count_out = std::max(count_out, index + 1);
  }
  for (uint32_t i = 0; i < count_out; ++i) {
    if (!seen[i]) {
      SetError(error_out, "exact output-merger MSL resource bindings are not dense");
      return false;
    }
  }
  return true;
}

bool ConsumePreviousMslWord(const std::string& source, size_t& position,
                            std::string_view expected) {
  SkipMslWhitespaceBackward(source, position);
  const size_t end = position;
  while (position && IsMslIdentifierCharacter(source[position - 1])) {
    --position;
  }
  return end - position == expected.size() &&
         source.compare(position, expected.size(), expected) == 0;
}

bool ParseMslAttributeUnsigned(std::string_view attributes, std::string_view attribute_name,
                               uint32_t& value_out) {
  size_t invocation = std::string_view::npos;
  for (size_t search = 0;;) {
    const size_t candidate = attributes.find(attribute_name, search);
    if (candidate == std::string_view::npos) {
      break;
    }
    const bool token_begin = !candidate || !IsMslIdentifierCharacter(attributes[candidate - 1]);
    const size_t token_end = candidate + attribute_name.size();
    const bool token_end_valid =
        token_end == attributes.size() || !IsMslIdentifierCharacter(attributes[token_end]);
    if (token_begin && token_end_valid) {
      if (invocation != std::string_view::npos) {
        return false;
      }
      invocation = token_end;
    }
    search = token_end;
  }
  if (invocation == std::string_view::npos) {
    return false;
  }
  while (invocation < attributes.size() &&
         (attributes[invocation] == ' ' || attributes[invocation] == '\t' ||
          attributes[invocation] == '\r' || attributes[invocation] == '\n')) {
    ++invocation;
  }
  if (invocation >= attributes.size() || attributes[invocation++] != '(') {
    return false;
  }
  while (invocation < attributes.size() &&
         (attributes[invocation] == ' ' || attributes[invocation] == '\t' ||
          attributes[invocation] == '\r' || attributes[invocation] == '\n')) {
    ++invocation;
  }
  uint32_t value = 0;
  bool has_digit = false;
  while (invocation < attributes.size() && attributes[invocation] >= '0' &&
         attributes[invocation] <= '9') {
    has_digit = true;
    const uint32_t digit = uint32_t(attributes[invocation++] - '0');
    if (value > (std::numeric_limits<uint32_t>::max() - digit) / 10u) {
      return false;
    }
    value = value * 10u + digit;
  }
  while (invocation < attributes.size() &&
         (attributes[invocation] == ' ' || attributes[invocation] == '\t' ||
          attributes[invocation] == '\r' || attributes[invocation] == '\n')) {
    ++invocation;
  }
  if (!has_digit || invocation >= attributes.size() || attributes[invocation] != ')') {
    return false;
  }
  value_out = value;
  return true;
}

bool IsDefinedColorFormat(xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return true;
    default:
      return false;
  }
}

bool IsDefinedDepthFormat(xenos::DepthRenderTargetFormat format) {
  return format == xenos::DepthRenderTargetFormat::kD24S8 ||
         format == xenos::DepthRenderTargetFormat::kD24FS8;
}

bool BuildCommonConstants(uint32_t base_tiles, uint32_t surface_pitch_pixels, uint32_t width,
                          uint32_t height, xenos::MsaaSamples msaa_samples, bool is_64bpp,
                          bool is_depth, ExactOutputMergerSurfaceConstants& constants_out,
                          std::string* error_out) {
  constants_out = {};
  if (base_tiles >= xenos::kEdramTileCount) {
    SetError(error_out, "exact output-merger base is outside physical EDRAM");
    return false;
  }
  if (!surface_pitch_pixels || surface_pitch_pixels > kMaximumSurfacePitchPixels) {
    SetError(error_out, "exact output-merger pitch is outside Xenos bounds");
    return false;
  }
  if (!width || !height || width > surface_pitch_pixels) {
    SetError(error_out, "exact output-merger extent is empty or wider than its pitch");
    return false;
  }
  if (!IsCanonicalEdramMsaaSupportedByMetal(msaa_samples)) {
    SetError(error_out, "exact output-merger sample count is unsupported");
    return false;
  }
  if (is_depth && is_64bpp) {
    SetError(error_out, "exact output-merger depth cannot use 64bpp storage");
    return false;
  }

  const uint32_t sample_x_log2 = uint32_t(msaa_samples >= xenos::MsaaSamples::k4X);
  const uint32_t sample_y_log2 = uint32_t(msaa_samples >= xenos::MsaaSamples::k2X);
  const uint64_t maximum_sample_x = (uint64_t(width - 1) << sample_x_log2) + sample_x_log2;
  const uint64_t maximum_sample_y = (uint64_t(height - 1) << sample_y_log2) + sample_y_log2;
  const uint32_t pitch_tiles_32bpp =
      xenos::GetSurfacePitchTiles(surface_pitch_pixels, msaa_samples, false);
  const uint32_t pitch_tiles =
      xenos::GetSurfacePitchTiles(surface_pitch_pixels, msaa_samples, is_64bpp);
  if (!pitch_tiles_32bpp || pitch_tiles_32bpp > kMaximumSurfacePitchTiles || !pitch_tiles ||
      pitch_tiles > kMaximumSurfacePitchTiles) {
    SetError(error_out, "exact output-merger tiled pitch is outside shader bounds");
    return false;
  }

  // The translated shader performs the row-address arithmetic in uint32_t
  // before the final physical-EDRAM modulo. Refuse extents that would overflow
  // before that modulo rather than relying on a second, unintended wrap.
  const uint64_t maximum_tile_y = maximum_sample_y / xenos::kEdramTileHeightSamples;
  const uint64_t maximum_word_x = maximum_sample_x * (is_64bpp ? 2u : 1u) + (is_64bpp ? 1u : 0u);
  const uint64_t maximum_tile_x = maximum_word_x / xenos::kEdramTileWidthSamples;
  const uint64_t maximum_logical_tile =
      uint64_t(base_tiles) + maximum_tile_y * pitch_tiles + maximum_tile_x;
  if (maximum_logical_tile > uint64_t(std::numeric_limits<uint32_t>::max()) / kEdramWordsPerTile) {
    SetError(error_out, "exact output-merger extent overflows translated address arithmetic");
    return false;
  }

  constants_out.kind =
      is_depth ? ExactOutputMergerSurfaceKind::kDepthStencil : ExactOutputMergerSurfaceKind::kColor;
  constants_out.canonical_layout.base_tiles = base_tiles;
  constants_out.canonical_layout.pitch_tiles = pitch_tiles;
  constants_out.canonical_layout.msaa_samples = msaa_samples;
  constants_out.canonical_layout.is_64bpp = is_64bpp;
  constants_out.canonical_layout.is_depth = is_depth;
  constants_out.width = width;
  constants_out.height = height;
  constants_out.surface_pitch_pixels = surface_pitch_pixels;
  constants_out.sample_count = GetCanonicalEdramSampleCount(msaa_samples);
  constants_out.words_per_sample = is_64bpp ? 2u : 1u;
  constants_out.tile_dwords = kEdramWordsPerTile;
  constants_out.tile_pitch_dwords = pitch_tiles_32bpp * kEdramWordsPerTile;
  constants_out.base_dwords = base_tiles * kEdramWordsPerTile;
  return true;
}

bool ValidateConstants(const ExactOutputMergerSurfaceConstants& constants) {
  if (!constants.width || !constants.height || !constants.surface_pitch_pixels ||
      constants.width > constants.surface_pitch_pixels ||
      constants.canonical_layout.base_tiles >= xenos::kEdramTileCount ||
      !IsCanonicalEdramMsaaSupportedByMetal(constants.canonical_layout.msaa_samples) ||
      constants.sample_count !=
          GetCanonicalEdramSampleCount(constants.canonical_layout.msaa_samples) ||
      constants.tile_dwords != kEdramWordsPerTile ||
      constants.base_dwords != constants.canonical_layout.base_tiles * kEdramWordsPerTile) {
    return false;
  }
  const bool is_depth = constants.kind == ExactOutputMergerSurfaceKind::kDepthStencil;
  if (constants.canonical_layout.is_depth != is_depth ||
      (is_depth && constants.canonical_layout.is_64bpp)) {
    return false;
  }
  const bool is_64bpp = !is_depth && xenos::IsColorRenderTargetFormat64bpp(constants.color_format);
  if (constants.canonical_layout.is_64bpp != is_64bpp ||
      constants.words_per_sample != (is_64bpp ? 2u : 1u) ||
      constants.canonical_layout.pitch_tiles !=
          xenos::GetSurfacePitchTiles(constants.surface_pitch_pixels,
                                      constants.canonical_layout.msaa_samples, is_64bpp) ||
      constants.tile_pitch_dwords !=
          xenos::GetSurfacePitchTiles(constants.surface_pitch_pixels,
                                      constants.canonical_layout.msaa_samples, false) *
              kEdramWordsPerTile) {
    return false;
  }
  return is_depth ? IsDefinedDepthFormat(constants.depth_format)
                  : IsDefinedColorFormat(constants.color_format) &&
                        constants.color_format_flags ==
                            RenderTargetCache::AddPSIColorFormatFlags(constants.color_format);
}

bool GetSampleCountSlot(uint32_t sample_count, size_t& slot_out) {
  switch (sample_count) {
    case 1:
      slot_out = 0;
      return true;
    case 2:
      slot_out = 1;
      return true;
    case 4:
      slot_out = 2;
      return true;
    default:
      return false;
  }
}

void ResetExactOutputMergerCoveragePass(MTLRenderPassDescriptor* descriptor) {
  if (!descriptor) {
    return;
  }
  for (NSUInteger i = 0; i < 8; ++i) {
    descriptor.colorAttachments[i] = nil;
    MTLRenderPassColorAttachmentDescriptor* color = descriptor.colorAttachments[i];
    color.texture = nil;
    color.resolveTexture = nil;
    color.level = 0;
    color.slice = 0;
    color.depthPlane = 0;
    color.resolveLevel = 0;
    color.resolveSlice = 0;
    color.resolveDepthPlane = 0;
    color.loadAction = MTLLoadActionDontCare;
    color.storeAction = MTLStoreActionDontCare;
    color.clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
  }
  descriptor.depthAttachment = nil;
  descriptor.stencilAttachment = nil;
  MTLRenderPassDepthAttachmentDescriptor* depth = descriptor.depthAttachment;
  depth.texture = nil;
  depth.resolveTexture = nil;
  depth.level = 0;
  depth.slice = 0;
  depth.depthPlane = 0;
  depth.resolveLevel = 0;
  depth.resolveSlice = 0;
  depth.resolveDepthPlane = 0;
  depth.loadAction = MTLLoadActionDontCare;
  depth.storeAction = MTLStoreActionDontCare;
  depth.clearDepth = 1.0;
  depth.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
  MTLRenderPassStencilAttachmentDescriptor* stencil = descriptor.stencilAttachment;
  stencil.texture = nil;
  stencil.resolveTexture = nil;
  stencil.level = 0;
  stencil.slice = 0;
  stencil.depthPlane = 0;
  stencil.resolveLevel = 0;
  stencil.resolveSlice = 0;
  stencil.resolveDepthPlane = 0;
  stencil.loadAction = MTLLoadActionDontCare;
  stencil.storeAction = MTLStoreActionDontCare;
  stencil.clearStencil = 0;
  stencil.stencilResolveFilter = MTLMultisampleStencilResolveFilterSample0;
  descriptor.visibilityResultBuffer = nil;
  descriptor.renderTargetArrayLength = 0;
  descriptor.imageblockSampleLength = 0;
  descriptor.threadgroupMemoryLength = 0;
  descriptor.tileWidth = 0;
  descriptor.tileHeight = 0;
  descriptor.defaultRasterSampleCount = 1;
  descriptor.renderTargetWidth = 0;
  descriptor.renderTargetHeight = 0;
  [descriptor setSamplePositions:nullptr count:0];
  descriptor.rasterizationRateMap = nil;
  for (NSUInteger i = 0; i < 4; ++i) {
    descriptor.sampleBufferAttachments[i] = nil;
  }
}

bool CheckedMultiplySize(size_t left, size_t right, size_t& product_out) {
  if (left && right > std::numeric_limits<size_t>::max() / left) {
    return false;
  }
  product_out = left * right;
  return true;
}

bool CheckedAddSize(size_t left, size_t right, size_t& sum_out) {
  if (right > std::numeric_limits<size_t>::max() - left) {
    return false;
  }
  sum_out = left + right;
  return true;
}

bool IsExactBufferIndexValid(uint32_t index) {
  return index == UINT32_MAX || index <= kExactOutputMergerMaxMetalBufferIndex;
}

bool GetExactOutputMergerSamplePositions(uint32_t sample_count,
                                         std::array<MTLSamplePosition, 4>& positions_out) {
  positions_out = {};
  positions_out[0] = {0.5f, 0.5f};
  if (sample_count == 1) {
    return true;
  }
  const int8_t (*guest_positions)[2] = nullptr;
  if (sample_count == 2) {
    guest_positions = draw_util::kD3D10StandardSamplePositions2x;
  } else if (sample_count == 4) {
    guest_positions = draw_util::kD3D10StandardSamplePositions4x;
  } else {
    return false;
  }
  // Metal sample coordinates are relative to the pixel's top-left corner;
  // Xenos/D3D positions are signed sixteenth-pixel offsets from its center.
  // Preserve the standard 2x and 4x host index ordering expected by the
  // fragment-interlock translator's coverage-mask remap.
  for (uint32_t i = 0; i < sample_count; ++i) {
    positions_out[i] = {0.5f + float(guest_positions[i][0]) * (1.0f / 16.0f),
                        0.5f + float(guest_positions[i][1]) * (1.0f / 16.0f)};
  }
  return true;
}

bool ValidateExactStageLayout(const ExactOutputMergerStageLayout& layout,
                              uint32_t additional_buffer_index, std::string* error_out) {
  if (layout.texture_count > kExactOutputMergerMaxTextureCount ||
      layout.sampler_count > kExactOutputMergerMaxSamplerCount) {
    SetError(error_out, "exact output-merger stage resource count exceeds Metal limits");
    return false;
  }
  const std::array<uint32_t, 6> buffer_indices = {
      layout.system_constants_buffer_index,    layout.shared_memory_buffer_index,
      layout.float_constants_buffer_index,     layout.fetch_constants_buffer_index,
      layout.bool_loop_constants_buffer_index, layout.vertex_data_buffer_index};
  std::array<bool, kExactOutputMergerMaxMetalBufferIndex + 1> used = {};
  for (uint32_t index : buffer_indices) {
    if (!IsExactBufferIndexValid(index)) {
      SetError(error_out, "exact output-merger stage buffer index exceeds Metal limits");
      return false;
    }
    if (index != UINT32_MAX) {
      if (used[index]) {
        SetError(error_out, "exact output-merger stage buffer bindings collide");
        return false;
      }
      used[index] = true;
    }
  }
  if (!IsExactBufferIndexValid(additional_buffer_index)) {
    SetError(error_out, "exact output-merger EDRAM binding exceeds Metal limits");
    return false;
  }
  if (additional_buffer_index != UINT32_MAX && used[additional_buffer_index]) {
    SetError(error_out, "exact output-merger EDRAM binding collides with another buffer");
    return false;
  }
  return true;
}

bool ValidateExactPipelineLayout(const ExactOutputMergerPipelineLayout& layout,
                                 std::string* error_out) {
  if (layout.edram_fragment_buffer_index == UINT32_MAX ||
      !ValidateExactStageLayout(layout.vertex, UINT32_MAX, error_out) ||
      !ValidateExactStageLayout(layout.fragment, layout.edram_fragment_buffer_index, error_out)) {
    if (error_out && error_out->empty()) {
      *error_out = "exact output-merger pipeline layout is incomplete";
    }
    return false;
  }
  // Host-uploaded vertex data is a vertex-stage-only escape hatch used by
  // isolated probes. A fragment declaration would have no meaningful source.
  if (layout.fragment.vertex_data_buffer_index != UINT32_MAX) {
    SetError(error_out, "exact output-merger fragment stage declares vertex data");
    return false;
  }
  return true;
}

bool BuildExactStageLayoutFromTranslation(const MetalShader::MetalTranslation& translation,
                                          bool fragment_stage,
                                          ExactOutputMergerStageLayout& layout_out,
                                          std::string* error_out) {
  layout_out = {};
  const auto& reflection = translation.msl_reflection();
  layout_out.system_constants_buffer_index = reflection.system_constants_buffer_index;
  layout_out.shared_memory_buffer_index = reflection.shared_memory_buffer_index;
  layout_out.float_constants_buffer_index = reflection.float_constants_buffer_index;
  layout_out.fetch_constants_buffer_index = reflection.fetch_constants_buffer_index;
  layout_out.bool_loop_constants_buffer_index = reflection.bool_loop_constants_buffer_index;
  layout_out.vertex_data_buffer_index = UINT32_MAX;
  if (!GetDenseMslBindingCount(translation.msl_source(), "texture",
                               kExactOutputMergerMaxTextureCount, layout_out.texture_count,
                               error_out) ||
      !GetDenseMslBindingCount(translation.msl_source(), "sampler",
                               kExactOutputMergerMaxSamplerCount, layout_out.sampler_count,
                               error_out)) {
    return false;
  }
  if (fragment_stage) {
    if (!reflection.exact_output_merger_contract || !reflection.edram_raster_order_group ||
        reflection.edram_buffer_index == UINT32_MAX) {
      SetError(error_out, "exact output-merger fragment translation lacks its ROG contract");
      return false;
    }
  } else if (reflection.exact_output_merger_contract || reflection.edram_raster_order_group ||
             reflection.edram_buffer_index != UINT32_MAX) {
    SetError(error_out, "exact output-merger vertex translation exposes fragment EDRAM state");
    return false;
  }
  return true;
}

uint32_t GetExpectedExactBufferIndex(const ExactOutputMergerStageLayout& layout, NSString* name) {
  if ([name isEqualToString:@"xe_uniform_system_constants"]) {
    return layout.system_constants_buffer_index;
  }
  if ([name isEqualToString:@"xe_shared_memory"]) {
    return layout.shared_memory_buffer_index;
  }
  if ([name isEqualToString:@"xe_uniform_float_constants"]) {
    return layout.float_constants_buffer_index;
  }
  if ([name isEqualToString:@"xe_uniform_fetch_constants"]) {
    return layout.fetch_constants_buffer_index;
  }
  if ([name isEqualToString:@"xe_uniform_bool_loop_constants"]) {
    return layout.bool_loop_constants_buffer_index;
  }
  if ([name isEqualToString:@"xe_vertex_data"]) {
    return layout.vertex_data_buffer_index;
  }
  return UINT32_MAX;
}

bool ValidateExactStageReflection(
    NSArray<id<MTLBinding>>* bindings, const ExactOutputMergerStageLayout& layout,
    bool fragment_stage, uint32_t edram_index,
    std::array<size_t, kExactOutputMergerMaxMetalBufferIndex + 1>& min_buffer_lengths_out,
    std::vector<MTLTextureType>& texture_types_out, std::string* error_out) {
  min_buffer_lengths_out.fill(0);
  texture_types_out.assign(layout.texture_count, MTLTextureType2D);
  std::array<bool, 6> seen_buffers = {};
  std::array<bool, kExactOutputMergerMaxTextureCount> seen_textures = {};
  std::array<bool, kExactOutputMergerMaxSamplerCount> seen_samplers = {};
  bool seen_edram = false;
  auto buffer_slot = [&](NSString* name) -> size_t {
    if ([name isEqualToString:@"xe_uniform_system_constants"])
      return 0;
    if ([name isEqualToString:@"xe_shared_memory"])
      return 1;
    if ([name isEqualToString:@"xe_uniform_float_constants"])
      return 2;
    if ([name isEqualToString:@"xe_uniform_fetch_constants"])
      return 3;
    if ([name isEqualToString:@"xe_uniform_bool_loop_constants"])
      return 4;
    if ([name isEqualToString:@"xe_vertex_data"])
      return 5;
    return seen_buffers.size();
  };

  for (id<MTLBinding> binding in bindings) {
    NSString* name = binding.name;
    if (!name) {
      SetError(error_out, "exact output-merger reflection exposes an unnamed resource");
      return false;
    }
    switch (binding.type) {
      case MTLBindingTypeBuffer: {
        if ([name isEqualToString:@"xe_edram"]) {
          if (!fragment_stage || seen_edram || binding.index != edram_index ||
              binding.access != MTLBindingAccessReadWrite) {
            SetError(error_out, "exact output-merger reflected EDRAM binding is invalid");
            return false;
          }
          seen_edram = true;
          break;
        }
        const size_t slot = buffer_slot(name);
        const uint32_t expected = GetExpectedExactBufferIndex(layout, name);
        if (slot >= seen_buffers.size() || expected == UINT32_MAX || seen_buffers[slot] ||
            binding.index != expected || binding.access != MTLBindingAccessReadOnly) {
          SetError(error_out,
                   "exact output-merger reflected buffer is unknown, writable, or mismatched");
          return false;
        }
        id<MTLBufferBinding> buffer_binding = (id<MTLBufferBinding>)binding;
        min_buffer_lengths_out[binding.index] =
            std::max<size_t>(size_t(1), size_t(buffer_binding.bufferDataSize));
        seen_buffers[slot] = true;
        break;
      }
      case MTLBindingTypeTexture: {
        if (![name hasPrefix:@"xe_texture"] || binding.index >= layout.texture_count ||
            seen_textures[binding.index] || binding.access != MTLBindingAccessReadOnly) {
          SetError(error_out, "exact output-merger reflected texture layout is invalid");
          return false;
        }
        id<MTLTextureBinding> texture_binding = (id<MTLTextureBinding>)binding;
        // The current Metal texture cache exposes normalized or float samples
        // through one non-depth texture per binding. Integer/depth arguments
        // and Metal texture argument arrays require a different acquisition
        // contract and therefore fail before submission.
        if (texture_binding.textureDataType != MTLDataTypeFloat || texture_binding.depthTexture ||
            texture_binding.arrayLength != 1 ||
            (texture_binding.textureType != MTLTextureType2DArray &&
             texture_binding.textureType != MTLTextureTypeCube)) {
          SetError(error_out,
                   "exact output-merger reflected texture sample contract is unsupported");
          return false;
        }
        texture_types_out[binding.index] = texture_binding.textureType;
        seen_textures[binding.index] = true;
        break;
      }
      case MTLBindingTypeSampler:
        if (![name hasPrefix:@"xe_sampler"] || binding.index >= layout.sampler_count ||
            seen_samplers[binding.index]) {
          SetError(error_out, "exact output-merger reflected sampler layout is invalid");
          return false;
        }
        seen_samplers[binding.index] = true;
        break;
      default:
        SetError(error_out, "exact output-merger reflection exposes an unsupported resource");
        return false;
    }
  }

  const std::array<uint32_t, 6> expected_buffers = {
      layout.system_constants_buffer_index,    layout.shared_memory_buffer_index,
      layout.float_constants_buffer_index,     layout.fetch_constants_buffer_index,
      layout.bool_loop_constants_buffer_index, layout.vertex_data_buffer_index};
  for (size_t i = 0; i < expected_buffers.size(); ++i) {
    if ((expected_buffers[i] != UINT32_MAX) != seen_buffers[i]) {
      SetError(error_out, "exact output-merger reflection is missing a pinned buffer");
      return false;
    }
  }
  for (uint32_t i = 0; i < layout.texture_count; ++i) {
    if (!seen_textures[i]) {
      SetError(error_out, "exact output-merger reflection has a texture binding hole");
      return false;
    }
  }
  for (uint32_t i = 0; i < layout.sampler_count; ++i) {
    if (!seen_samplers[i]) {
      SetError(error_out, "exact output-merger reflection has a sampler binding hole");
      return false;
    }
  }
  if (fragment_stage != seen_edram) {
    SetError(error_out, fragment_stage
                            ? "exact output-merger fragment reflection is missing EDRAM"
                            : "exact output-merger vertex reflection unexpectedly exposes EDRAM");
    return false;
  }
  return true;
}

bool GetExactMetalPrimitiveType(uint32_t primitive_type, MTLPrimitiveType& type_out) {
  switch (primitive_type) {
    case 1:
      type_out = MTLPrimitiveTypePoint;
      return true;
    case 2:
      type_out = MTLPrimitiveTypeLine;
      return true;
    case 3:
      type_out = MTLPrimitiveTypeLineStrip;
      return true;
    case 4:
      type_out = MTLPrimitiveTypeTriangle;
      return true;
    case 6:
      type_out = MTLPrimitiveTypeTriangleStrip;
      return true;
    default:
      return false;
  }
}

MTLCullMode GetExactMetalCullMode(ProbeCullMode cull_mode) {
  switch (cull_mode) {
    case ProbeCullMode::kFront:
      return MTLCullModeFront;
    case ProbeCullMode::kBack:
      return MTLCullModeBack;
    default:
      return MTLCullModeNone;
  }
}

bool ValidateExactRasterizationState(const ProbeRasterizationState* state, uint32_t width,
                                     uint32_t height) {
  if (!state) {
    return true;
  }
  return std::isfinite(state->viewport_x) && std::isfinite(state->viewport_y) &&
         std::isfinite(state->viewport_width) && std::isfinite(state->viewport_height) &&
         std::isfinite(state->viewport_z_min) && std::isfinite(state->viewport_z_max) &&
         state->viewport_width > 0.0 && state->viewport_height > 0.0 && state->viewport_x >= 0.0 &&
         state->viewport_y >= 0.0 && state->viewport_x <= double(width) &&
         state->viewport_y <= double(height) &&
         state->viewport_width <= double(width) - state->viewport_x &&
         state->viewport_height <= double(height) - state->viewport_y &&
         state->viewport_z_min >= 0.0 && state->viewport_z_min <= 1.0 &&
         state->viewport_z_max >= 0.0 && state->viewport_z_max <= 1.0 && state->scissor_width &&
         state->scissor_height && state->scissor_x <= width && state->scissor_y <= height &&
         state->scissor_width <= width - state->scissor_x &&
         state->scissor_height <= height - state->scissor_y &&
         uint32_t(state->cull_mode) <= uint32_t(ProbeCullMode::kBack);
}

bool ValidateExactInlineArgument(const void* data, size_t size, uint32_t binding,
                                 size_t minimum_size, std::string* error_out) {
  if (binding == UINT32_MAX) {
    if (data || size) {
      SetError(error_out, "exact output-merger supplied an undeclared inline buffer");
      return false;
    }
    return true;
  }
  if (!data || size < minimum_size || size > kMaximumInlineArgumentBytes) {
    SetError(error_out, "exact output-merger inline buffer is truncated or oversized");
    return false;
  }
  return true;
}

bool IsExactFloatSampleTextureFormat(MTLPixelFormat format) {
  // Keep this in lockstep with the formats currently produced by the Metal
  // texture cache. All are sampled as float/normalized non-depth data.
  switch (format) {
    case MTLPixelFormatR8Unorm:
    case MTLPixelFormatR8Snorm:
    case MTLPixelFormatRG8Unorm:
    case MTLPixelFormatRG8Snorm:
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Snorm:
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatB5G6R5Unorm:
    case MTLPixelFormatBC1_RGBA:
    case MTLPixelFormatBC2_RGBA:
    case MTLPixelFormatBC3_RGBA:
    case MTLPixelFormatR32Float:
      return true;
    default:
      return false;
  }
}

bool ValidateExactTextureSlot(id<MTLDevice> device, const ExactOutputMergerTextureSlot& slot,
                              MTLTextureType expected_type, size_t& cpu_bytes_out,
                              std::string* error_out) {
  cpu_bytes_out = 0;
  const bool has_cpu_data = slot.rgba != nullptr;
  const bool has_metal_texture = slot.metal_texture != nullptr;
  if (has_cpu_data == has_metal_texture || !slot.width || !slot.height || !slot.array_length) {
    SetError(error_out, "exact output-merger texture slot has an invalid source or extent");
    return false;
  }
  if (has_metal_texture) {
    id<MTLTexture> texture = (id<MTLTexture>)slot.metal_texture;
    if (texture.device != device || texture.width != slot.width || texture.height != slot.height ||
        texture.arrayLength != slot.array_length || texture.textureType != expected_type ||
        texture.sampleCount != 1 || !(texture.usage & MTLTextureUsageShaderRead) ||
        !IsExactFloatSampleTextureFormat(texture.pixelFormat)) {
      SetError(error_out, "exact output-merger borrowed texture metadata is mismatched");
      return false;
    }
    return true;
  }
  if (expected_type != MTLTextureType2DArray) {
    SetError(error_out, "exact output-merger CPU textures require a reflected 2D-array binding");
    return false;
  }
  size_t minimum_row_bytes = 0;
  size_t minimum_image_bytes = 0;
  size_t total_bytes = 0;
  if (!CheckedMultiplySize(size_t(slot.width), size_t(4), minimum_row_bytes) ||
      !slot.bytes_per_row || slot.bytes_per_row < minimum_row_bytes ||
      !CheckedMultiplySize(slot.bytes_per_row, size_t(slot.height), minimum_image_bytes)) {
    SetError(error_out, "exact output-merger CPU texture row span is invalid");
    return false;
  }
  const size_t bytes_per_image = slot.bytes_per_image ? slot.bytes_per_image : minimum_image_bytes;
  if (bytes_per_image < minimum_image_bytes ||
      !CheckedMultiplySize(bytes_per_image, size_t(slot.array_length), total_bytes) ||
      slot.rgba_size < total_bytes || total_bytes > kMaximumCpuTextureSlotBytes) {
    SetError(error_out, "exact output-merger CPU texture byte span is truncated");
    return false;
  }
  cpu_bytes_out = total_bytes;
  return true;
}

bool ValidateExactSamplerSlot(const ProbeSamplerSlot& slot, std::string* error_out) {
  if (slot.min_linear > 1 || slot.mag_linear > 1 || slot.mip_linear > 1 ||
      slot.address_mode_s > 3 || slot.address_mode_t > 3 || slot.address_mode_r > 3 ||
      !slot.max_anisotropy || slot.max_anisotropy > 16) {
    SetError(error_out, "exact output-merger sampler state is invalid");
    return false;
  }
  return true;
}

id<MTLTexture> CreateExactTexture(id<MTLDevice> device, const ExactOutputMergerTextureSlot& slot) {
  if (slot.metal_texture) {
    return (id<MTLTexture>)slot.metal_texture;
  }
  MTLTextureDescriptor* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:slot.width
                                                        height:slot.height
                                                     mipmapped:NO];
  descriptor.textureType = MTLTextureType2DArray;
  descriptor.arrayLength = slot.array_length;
  descriptor.storageMode = MTLStorageModeShared;
  descriptor.usage = MTLTextureUsageShaderRead;
  id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
  if (!texture) {
    return nil;
  }
  const size_t bytes_per_image =
      slot.bytes_per_image ? slot.bytes_per_image : slot.bytes_per_row * size_t(slot.height);
  const MTLRegion region = MTLRegionMake2D(0, 0, slot.width, slot.height);
  for (uint32_t slice = 0; slice < slot.array_length; ++slice) {
    [texture replaceRegion:region
               mipmapLevel:0
                     slice:slice
                 withBytes:slot.rgba + bytes_per_image * slice
               bytesPerRow:slot.bytes_per_row
             bytesPerImage:bytes_per_image];
  }
  return texture;
}

MTLSamplerAddressMode GetExactSamplerAddressMode(uint8_t mode) {
  switch (mode) {
    case 0:
      return MTLSamplerAddressModeRepeat;
    case 1:
      return MTLSamplerAddressModeMirrorRepeat;
    case 3:
      return MTLSamplerAddressModeClampToBorderColor;
    case 2:
    default:
      return MTLSamplerAddressModeClampToEdge;
  }
}

id<MTLSamplerState> CreateExactSampler(id<MTLDevice> device, const ProbeSamplerSlot& slot) {
  if (!ValidateExactSamplerSlot(slot, nullptr)) {
    return nil;
  }
  MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
  descriptor.minFilter =
      slot.min_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
  descriptor.magFilter =
      slot.mag_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
  descriptor.mipFilter = slot.mip_linear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
  descriptor.sAddressMode = GetExactSamplerAddressMode(slot.address_mode_s);
  descriptor.tAddressMode = GetExactSamplerAddressMode(slot.address_mode_t);
  descriptor.rAddressMode = GetExactSamplerAddressMode(slot.address_mode_r);
  descriptor.borderColor = MTLSamplerBorderColorTransparentBlack;
  descriptor.maxAnisotropy = slot.max_anisotropy;
  id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
  [descriptor release];
  return sampler;
}

uint64_t ExactSubmissionNowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

uint32_t GetPendingExactDrawCount(const ExactOutputMergerResources* resources) {
  if (!resources) {
    return 0;
  }
  uint64_t draw_count = resources->open_draw_count;
  for (const CommittedExactCommandBuffer& committed : resources->committed_command_buffers) {
    draw_count += committed.pending_work_count;
  }
  return uint32_t(std::min<uint64_t>(draw_count, UINT32_MAX));
}

void UpdateExactSubmissionHighWatermarks(ExactOutputMergerResources* resources) {
  resources->submission_stats.peak_committed_draw_command_buffer_count =
      std::max(resources->submission_stats.peak_committed_draw_command_buffer_count,
               uint32_t(resources->committed_command_buffers.size()));
  resources->submission_stats.peak_pending_submission_count =
      std::max(resources->submission_stats.peak_pending_submission_count,
               GetPendingExactDrawCount(resources));
}

void ResetExactUploadArena(ExactUploadArena& arena) {
  for (ExactUploadChunk& chunk : arena.chunks) {
    chunk.offset = 0;
  }
}

uint32_t AcquireExactUploadArena(ExactOutputMergerResources* resources) {
  for (uint32_t i = 0; i < kMaximumCommittedExactCommandBuffers; ++i) {
    ExactUploadArena& arena = resources->upload_arenas[i];
    if (!arena.in_use) {
      ResetExactUploadArena(arena);
      arena.in_use = true;
      return i;
    }
  }
  return kInvalidExactUploadArena;
}

void ReleaseExactUploadArena(ExactOutputMergerResources* resources, uint32_t arena_index) {
  if (arena_index >= kMaximumCommittedExactCommandBuffers) {
    return;
  }
  ExactUploadArena& arena = resources->upload_arenas[arena_index];
  ResetExactUploadArena(arena);
  arena.in_use = false;
}

bool CaptureExactUploadArenaCheckpoint(ExactOutputMergerResources* resources,
                                       ExactUploadArenaCheckpoint& checkpoint_out,
                                       std::string* error_out) {
  const uint32_t arena_index = resources->open_upload_arena_index;
  if (arena_index >= kMaximumCommittedExactCommandBuffers) {
    SetError(error_out, "exact output-merger upload arena checkpoint is unavailable");
    return false;
  }
  const ExactUploadArena& arena = resources->upload_arenas[arena_index];
  if (!arena.in_use) {
    SetError(error_out, "exact output-merger upload arena checkpoint is invalid");
    return false;
  }
  ExactUploadArenaCheckpoint checkpoint;
  checkpoint.arena_index = arena_index;
  checkpoint.chunk_offsets.reserve(arena.chunks.size());
  for (const ExactUploadChunk& chunk : arena.chunks) {
    checkpoint.chunk_offsets.push_back(chunk.offset);
  }
  checkpoint.upload_stats = resources->upload_stats;
  checkpoint_out = std::move(checkpoint);
  return true;
}

void RollbackExactUploadArena(ExactOutputMergerResources* resources,
                              const ExactUploadArenaCheckpoint& checkpoint) {
  if (checkpoint.arena_index >= kMaximumCommittedExactCommandBuffers) {
    return;
  }
  ExactUploadArena& arena = resources->upload_arenas[checkpoint.arena_index];
  while (arena.chunks.size() > checkpoint.chunk_offsets.size()) {
    [arena.chunks.back().buffer release];
    arena.chunks.pop_back();
  }
  for (size_t i = 0; i < arena.chunks.size(); ++i) {
    arena.chunks[i].offset = checkpoint.chunk_offsets[i];
  }
  resources->upload_stats = checkpoint.upload_stats;
}

ExactUploadAllocation UploadExactDrawData(ExactOutputMergerResources* resources, const void* source,
                                          size_t length, std::string* error_out) {
  ExactUploadAllocation allocation;
  if (!source || !length) {
    return allocation;
  }
  if (resources->open_upload_arena_index >= kMaximumCommittedExactCommandBuffers ||
      length > SIZE_MAX - (kExactUploadAlignment - 1)) {
    SetError(error_out, "exact output-merger upload arena is unavailable or upload is too large");
    return allocation;
  }

  ExactUploadArena& arena = resources->upload_arenas[resources->open_upload_arena_index];
  for (ExactUploadChunk& chunk : arena.chunks) {
    if (chunk.offset > SIZE_MAX - (kExactUploadAlignment - 1)) {
      continue;
    }
    const size_t aligned_offset =
        (chunk.offset + (kExactUploadAlignment - 1)) & ~(kExactUploadAlignment - 1);
    if (aligned_offset <= chunk.capacity && length <= chunk.capacity - aligned_offset) {
      uint8_t* destination = static_cast<uint8_t*>(chunk.buffer.contents);
      if (!destination) {
        continue;
      }
      std::memcpy(destination + aligned_offset, source, length);
      chunk.offset = aligned_offset + length;
      allocation.buffer = chunk.buffer;
      allocation.offset = NSUInteger(aligned_offset);
      ++resources->upload_stats.suballocation_count;
      resources->upload_stats.suballocation_bytes += length;
      return allocation;
    }
  }

  const size_t aligned_length =
      (length + (kExactUploadAlignment - 1)) & ~(kExactUploadAlignment - 1);
  const size_t chunk_capacity = std::max(kExactUploadChunkSize, aligned_length);
  id<MTLBuffer> buffer = [resources->device newBufferWithLength:chunk_capacity
                                                        options:MTLResourceStorageModeShared];
  uint8_t* destination = buffer ? static_cast<uint8_t*>(buffer.contents) : nullptr;
  if (!buffer || !destination) {
    [buffer release];
    SetError(error_out, "failed to grow exact output-merger upload arena");
    return allocation;
  }

  std::memcpy(destination, source, length);
  arena.chunks.push_back({buffer, chunk_capacity, length});
  allocation.buffer = buffer;
  ++resources->upload_stats.buffer_allocation_count;
  resources->upload_stats.buffer_allocation_bytes += chunk_capacity;
  ++resources->upload_stats.suballocation_count;
  resources->upload_stats.suballocation_bytes += length;
  return allocation;
}

void ResetOpenExactSubmissionState(ExactOutputMergerResources* resources) {
  resources->open_command_buffer = nil;
  resources->open_render_encoder = nil;
  resources->open_coverage_width = 0;
  resources->open_coverage_height = 0;
  resources->open_sample_count = 0;
  resources->open_draw_count = 0;
  resources->open_upload_arena_index = kInvalidExactUploadArena;
}

void DiscardOpenExactCommandBuffer(ExactOutputMergerResources* resources) {
  if (resources->open_render_encoder) {
    [resources->open_render_encoder endEncoding];
    [resources->open_render_encoder release];
  }
  if (resources->open_command_buffer) {
    [resources->open_command_buffer release];
  }
  ReleaseExactUploadArena(resources, resources->open_upload_arena_index);
  ResetOpenExactSubmissionState(resources);
}

bool FinalizeOpenExactCommandBuffer(ExactOutputMergerResources* resources, std::string* error_out) {
  if (!resources->open_command_buffer && !resources->open_render_encoder) {
    return true;
  }
  if (!resources->open_command_buffer || !resources->open_render_encoder ||
      !resources->open_draw_count ||
      resources->open_upload_arena_index >= kMaximumCommittedExactCommandBuffers ||
      !resources->gpu_epoch_sequence ||
      !resources->authority.CanContinueGpuSubmission(resources->gpu_epoch_sequence)) {
    DiscardOpenExactCommandBuffer(resources);
    resources->authority.Invalidate();
    SetError(error_out, "inconsistent open exact output-merger command buffer state");
    return false;
  }

  id<MTLRenderCommandEncoder> encoder = resources->open_render_encoder;
  [encoder endEncoding];
  CommittedExactCommandBuffer committed;
  committed.command_buffer = resources->open_command_buffer;
  committed.draw_count = resources->open_draw_count;
  committed.pending_work_count = resources->open_draw_count;
  committed.upload_arena_index = resources->open_upload_arena_index;
  if (resources->committed_command_buffers.size() >= kMaximumCommittedExactCommandBuffers) {
    [committed.command_buffer release];
    ReleaseExactUploadArena(resources, committed.upload_arena_index);
    ResetOpenExactSubmissionState(resources);
    [encoder release];
    resources->authority.Invalidate();
    SetError(error_out, "exact output-merger command-buffer cap was exceeded");
    return false;
  }
  try {
    resources->committed_command_buffers.push_back(committed);
  } catch (...) {
    [committed.command_buffer release];
    ReleaseExactUploadArena(resources, committed.upload_arena_index);
    ResetOpenExactSubmissionState(resources);
    [encoder release];
    resources->authority.Invalidate();
    SetError(error_out, "failed to retain exact output-merger command-buffer metadata");
    return false;
  }
  ResetOpenExactSubmissionState(resources);
  [committed.command_buffer commit];
  ++resources->submission_stats.draw_command_buffer_commit_count;
  UpdateExactSubmissionHighWatermarks(resources);
  [encoder release];
  return true;
}

std::string GetExactCommandBufferError(id<MTLCommandBuffer> command_buffer) {
  NSError* command_error = command_buffer.error;
  const char* description =
      command_error ? [[command_error localizedDescription] UTF8String] : nullptr;
  return description ? description : "asynchronous exact output-merger command buffer failed";
}

void NotifyExactCommandBufferFailure(CommittedExactCommandBuffer& committed) {
  auto callback = committed.async_failure_callback;
  void* callback_context = committed.async_failure_callback_context;
  const uint32_t callback_start = committed.async_failure_start;
  const uint32_t callback_length = committed.async_failure_length;
  committed.async_failure_callback = nullptr;
  committed.async_failure_callback_context = nullptr;
  committed.async_failure_start = 0;
  committed.async_failure_length = 0;
  if (callback) {
    callback(callback_context, callback_start, callback_length);
  }
}

bool WaitExactOutputMergerDrawsLocked(ExactOutputMergerResources* resources, std::string* error_out,
                                      uint32_t* waited_draw_count_out) {
  if (waited_draw_count_out) {
    *waited_draw_count_out = 0;
  }
  if (!resources) {
    SetError(error_out, "missing exact output-merger resources");
    return false;
  }

  const uint32_t pending_draw_count = GetPendingExactDrawCount(resources);
  if (waited_draw_count_out) {
    *waited_draw_count_out = pending_draw_count;
  }
  bool succeeded = FinalizeOpenExactCommandBuffer(resources, error_out);
  if (!resources->committed_command_buffers.empty()) {
    [resources->committed_command_buffers.back().command_buffer waitUntilCompleted];
  }

  std::string first_error = succeeded ? std::string() : (error_out ? *error_out : std::string());
  for (const CommittedExactCommandBuffer& committed : resources->committed_command_buffers) {
    if (committed.command_buffer.status != MTLCommandBufferStatusCompleted) {
      succeeded = false;
      if (first_error.empty()) {
        first_error = GetExactCommandBufferError(committed.command_buffer);
      }
    }
  }

  const bool authority_was_valid =
      resources->authority.snapshot().authority != ExactOutputMergerResourceAuthority::kInvalid;
  if (resources->gpu_epoch_sequence) {
    if (!resources->authority.CompleteGpuSubmission(resources->gpu_epoch_sequence, succeeded) ||
        !authority_was_valid) {
      succeeded = false;
      if (first_error.empty()) {
        first_error = "exact output-merger GPU epoch completion was invalid";
      }
    }
    resources->gpu_epoch_sequence = 0;
  } else if (pending_draw_count) {
    succeeded = false;
    if (first_error.empty()) {
      first_error = "exact output-merger pending draws had no GPU authority epoch";
    }
  } else if (!authority_was_valid) {
    succeeded = false;
    if (first_error.empty()) {
      first_error = "exact output-merger authority is invalid";
    }
  }

  for (CommittedExactCommandBuffer& committed : resources->committed_command_buffers) {
    // All command buffers in this vector belong to one ordered authority
    // epoch. If any predecessor fails, a later resolve may report Completed
    // after reading invalid EDRAM, so revoke every still-tracked publication.
    if (!succeeded || committed.command_buffer.status != MTLCommandBufferStatusCompleted) {
      NotifyExactCommandBufferFailure(committed);
    }
    [committed.command_buffer release];
    ReleaseExactUploadArena(resources, committed.upload_arena_index);
  }
  resources->committed_command_buffers.clear();

  if (!succeeded) {
    resources->authority.Invalidate();
    SetError(error_out,
             first_error.empty() ? "exact output-merger asynchronous wait failed" : first_error);
    return false;
  }
  if (error_out) {
    error_out->clear();
  }
  return true;
}

bool ConsumeCompletedExactCommandBuffer(ExactOutputMergerResources* resources,
                                        std::string* error_out) {
  if (resources->committed_command_buffers.empty()) {
    return true;
  }
  const CommittedExactCommandBuffer committed = resources->committed_command_buffers.front();
  if (committed.command_buffer.status != MTLCommandBufferStatusCompleted) {
    SetError(error_out, GetExactCommandBufferError(committed.command_buffer));
    return false;
  }
  resources->committed_command_buffers.erase(resources->committed_command_buffers.begin());
  [committed.command_buffer release];
  ReleaseExactUploadArena(resources, committed.upload_arena_index);
  return true;
}

bool RelieveExactSubmissionBackpressure(ExactOutputMergerResources* resources,
                                        std::string* error_out) {
  ++resources->submission_stats.backpressure_check_count;
  while (!resources->committed_command_buffers.empty()) {
    const MTLCommandBufferStatus status =
        resources->committed_command_buffers.front().command_buffer.status;
    if (status != MTLCommandBufferStatusCompleted && status != MTLCommandBufferStatusError) {
      break;
    }
    if (status == MTLCommandBufferStatusError) {
      return WaitExactOutputMergerDrawsLocked(resources, error_out, nullptr);
    }
    if (!ConsumeCompletedExactCommandBuffer(resources, error_out)) {
      return false;
    }
    ++resources->submission_stats.nonblocking_completed_command_buffer_reclamation_count;
  }
  if (resources->committed_command_buffers.size() < kMaximumCommittedExactCommandBuffers) {
    return true;
  }

  id<MTLCommandBuffer> oldest = resources->committed_command_buffers.front().command_buffer;
  MTLCommandBufferStatus status = oldest.status;
  if (status != MTLCommandBufferStatusCompleted && status != MTLCommandBufferStatusError) {
    ++resources->submission_stats.blocking_backpressure_wait_count;
    const uint64_t wait_start_ns = ExactSubmissionNowNs();
    [oldest waitUntilCompleted];
    resources->submission_stats.blocking_backpressure_wait_ns +=
        ExactSubmissionNowNs() - wait_start_ns;
    status = oldest.status;
  }
  if (status != MTLCommandBufferStatusCompleted) {
    return WaitExactOutputMergerDrawsLocked(resources, error_out, nullptr);
  }
  return ConsumeCompletedExactCommandBuffer(resources, error_out);
}

bool EnsureOpenExactCommandBuffer(ExactOutputMergerResources* resources, uint32_t width,
                                  uint32_t height, uint32_t sample_count, std::string* error_out) {
  if (resources->open_command_buffer && resources->open_render_encoder &&
      resources->open_coverage_width == width && resources->open_coverage_height == height &&
      resources->open_sample_count == sample_count) {
    return true;
  }
  if ((resources->open_command_buffer || resources->open_render_encoder) &&
      !FinalizeOpenExactCommandBuffer(resources, error_out)) {
    return false;
  }
  if (resources->committed_command_buffers.size() >= kMaximumCommittedExactCommandBuffers &&
      !RelieveExactSubmissionBackpressure(resources, error_out)) {
    return false;
  }

  MTLRenderPassDescriptor* render_pass = [MTLRenderPassDescriptor renderPassDescriptor];
  if (!ConfigureExactOutputMergerCoveragePass(resources, render_pass, width, height, sample_count,
                                              error_out)) {
    return false;
  }
  const uint32_t upload_arena_index = AcquireExactUploadArena(resources);
  if (upload_arena_index == kInvalidExactUploadArena) {
    SetError(error_out, "no reusable exact output-merger upload arena is available");
    return false;
  }
  id<MTLCommandBuffer> command_buffer = [[resources->command_queue commandBuffer] retain];
  id<MTLRenderCommandEncoder> encoder =
      command_buffer ? [[command_buffer renderCommandEncoderWithDescriptor:render_pass] retain]
                     : nil;
  if (!command_buffer || !encoder) {
    [encoder release];
    [command_buffer release];
    ReleaseExactUploadArena(resources, upload_arena_index);
    SetError(error_out, "failed to create asynchronous exact output-merger command buffer");
    return false;
  }
  resources->open_command_buffer = command_buffer;
  resources->open_render_encoder = encoder;
  resources->open_coverage_width = width;
  resources->open_coverage_height = height;
  resources->open_sample_count = sample_count;
  resources->open_upload_arena_index = upload_arena_index;
  return true;
}

id<MTLSamplerState> GetCachedExactSampler(ExactOutputMergerResources* resources,
                                          const ProbeSamplerSlot& slot, bool* created_out,
                                          std::string* error_out) {
  if (created_out) {
    *created_out = false;
  }
  const uint64_t key = GetProbeSamplerKey(slot);
  auto existing = resources->sampler_cache.find(key);
  if (existing != resources->sampler_cache.end()) {
    return existing->second;
  }
  if (resources->sampler_cache.size() >= kMaximumExactSamplerCacheEntries) {
    SetError(error_out, "exact output-merger sampler cache limit is exceeded");
    return nil;
  }
  id<MTLSamplerState> sampler = CreateExactSampler(resources->device, slot);
  if (!sampler) {
    SetError(error_out, "failed to create exact output-merger sampler");
    return nil;
  }
  resources->sampler_cache.emplace(key, sampler);
  if (created_out) {
    *created_out = true;
  }
  return sampler;
}

void RollbackExactSamplerInsertions(ExactOutputMergerResources* resources,
                                    const std::vector<uint64_t>& inserted_keys) {
  for (uint64_t key : inserted_keys) {
    auto inserted = resources->sampler_cache.find(key);
    if (inserted == resources->sampler_cache.end()) {
      continue;
    }
    [inserted->second release];
    resources->sampler_cache.erase(inserted);
  }
}

}  // namespace

bool BuildExactOutputMergerColorConstants(uint32_t base_tiles, uint32_t surface_pitch_pixels,
                                          uint32_t width, uint32_t height,
                                          xenos::MsaaSamples msaa_samples,
                                          xenos::ColorRenderTargetFormat format,
                                          ExactOutputMergerSurfaceConstants& constants_out,
                                          std::string* error_out) {
  if (!IsDefinedColorFormat(format)) {
    constants_out = {};
    SetError(error_out, "exact output-merger color format is undefined");
    return false;
  }
  if (!BuildCommonConstants(base_tiles, surface_pitch_pixels, width, height, msaa_samples,
                            xenos::IsColorRenderTargetFormat64bpp(format), false, constants_out,
                            error_out)) {
    return false;
  }
  constants_out.color_format_flags = RenderTargetCache::AddPSIColorFormatFlags(format);
  constants_out.color_format = format;
  return true;
}

bool BuildExactOutputMergerDepthConstants(uint32_t base_tiles, uint32_t surface_pitch_pixels,
                                          uint32_t width, uint32_t height,
                                          xenos::MsaaSamples msaa_samples,
                                          xenos::DepthRenderTargetFormat format,
                                          ExactOutputMergerSurfaceConstants& constants_out,
                                          std::string* error_out) {
  if (!IsDefinedDepthFormat(format)) {
    constants_out = {};
    SetError(error_out, "exact output-merger depth format is undefined");
    return false;
  }
  if (!BuildCommonConstants(base_tiles, surface_pitch_pixels, width, height, msaa_samples, false,
                            true, constants_out, error_out)) {
    return false;
  }
  constants_out.depth_format = format;
  return true;
}

bool GetExactOutputMergerDwordIndex(const ExactOutputMergerSurfaceConstants& constants, uint32_t x,
                                    uint32_t y, uint32_t sample, uint32_t dword,
                                    size_t& index_out) {
  index_out = SIZE_MAX;
  if (!ValidateConstants(constants) || x >= constants.width || y >= constants.height ||
      sample >= constants.sample_count || dword >= constants.words_per_sample) {
    return false;
  }
  const size_t index = GetCanonicalEdramDwordIndex(constants.canonical_layout, x, y, sample, dword);
  if (index == SIZE_MAX || index >= kExactOutputMergerEdramDwordCount) {
    return false;
  }
  index_out = index;
  return true;
}

bool ApplyExactOutputMergerAddressConstants(
    const ExactOutputMergerSurfaceConstants& surface, uint32_t color_target,
    SpirvShaderTranslator::SystemConstants& system_constants, std::string* error_out) {
  if (!ValidateConstants(surface) || surface.base_dwords >= kExactOutputMergerEdramDwordCount) {
    SetError(error_out, "exact output-merger constants are incomplete");
    return false;
  }
  if (surface.kind == ExactOutputMergerSurfaceKind::kColor &&
      color_target >= xenos::kMaxColorRenderTargets) {
    SetError(error_out, "exact output-merger color target index is invalid");
    return false;
  }
  const uint32_t msaa_value = uint32_t(surface.canonical_layout.msaa_samples);
  constexpr uint32_t kMsaaMask = ((uint32_t(1) << xenos::kMsaaSamplesBits) - 1)
                                 << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
  system_constants.flags = (system_constants.flags & ~kMsaaMask) |
                           (msaa_value << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift);
  system_constants.edram_32bpp_tile_pitch_dwords_scaled = surface.tile_pitch_dwords;
  if (surface.kind == ExactOutputMergerSurfaceKind::kDepthStencil) {
    system_constants.edram_depth_base_dwords_scaled = surface.base_dwords;
    return true;
  }
  system_constants.edram_rt_base_dwords_scaled[color_target] = surface.base_dwords;
  system_constants.edram_rt_format_flags[color_target] = surface.color_format_flags;
  return true;
}

bool FindExactOutputMergerEdramBinding(const std::string& msl_source, uint32_t& binding_out,
                                       std::string* error_out) {
  binding_out = UINT32_MAX;
  constexpr std::string_view kParameterName = "xe_edram";
  const std::string source = StripMslComments(msl_source);
  size_t name_position = std::string::npos;
  size_t attributes_begin = std::string::npos;
  size_t attributes_end = std::string::npos;
  for (size_t search = 0;;) {
    const size_t candidate = source.find(kParameterName, search);
    if (candidate == std::string::npos) {
      break;
    }
    search = candidate + kParameterName.size();
    if ((candidate && IsMslIdentifierCharacter(source[candidate - 1])) ||
        (search < source.size() && IsMslIdentifierCharacter(source[search]))) {
      continue;
    }
    size_t attribute = search;
    SkipMslWhitespaceForward(source, attribute);
    if (source.compare(attribute, 2, "[[") != 0) {
      continue;
    }
    const size_t end = source.find("]]", attribute + 2);
    if (end == std::string::npos || name_position != std::string::npos) {
      SetError(error_out, "translated output merger must expose exactly one xe_edram parameter");
      return false;
    }
    name_position = candidate;
    attributes_begin = attribute + 2;
    attributes_end = end;
  }
  if (name_position == std::string::npos) {
    SetError(error_out, "translated output merger must expose exactly one xe_edram parameter");
    return false;
  }

  // Bind the qualifiers to this parameter by consuming the declaration
  // backwards from the xe_edram identifier. Qualifiers elsewhere on the line
  // or in another parameter cannot satisfy this sequence.
  size_t qualifier_position = name_position;
  SkipMslWhitespaceBackward(source, qualifier_position);
  if (!qualifier_position || source[qualifier_position - 1] != '&') {
    SetError(error_out, "translated xe_edram is not a coherent raster-order binding");
    return false;
  }
  --qualifier_position;
  SkipMslWhitespaceBackward(source, qualifier_position);
  const size_t type_end = qualifier_position;
  while (qualifier_position && (IsMslIdentifierCharacter(source[qualifier_position - 1]) ||
                                source[qualifier_position - 1] == ':')) {
    --qualifier_position;
  }
  if (type_end == qualifier_position ||
      !ConsumePreviousMslWord(source, qualifier_position, "device") ||
      !ConsumePreviousMslWord(source, qualifier_position, "coherent")) {
    SetError(error_out, "translated xe_edram is not a coherent raster-order binding");
    return false;
  }

  size_t after_attributes = attributes_end + 2;
  SkipMslWhitespaceForward(source, after_attributes);
  if (after_attributes >= source.size() ||
      (source[after_attributes] != ',' && source[after_attributes] != ')')) {
    SetError(error_out, "translated xe_edram Metal attributes are not parameter-local");
    return false;
  }

  const std::string_view attributes(source.data() + attributes_begin,
                                    attributes_end - attributes_begin);
  uint32_t binding = UINT32_MAX;
  uint32_t raster_order_group = UINT32_MAX;
  if (!ParseMslAttributeUnsigned(attributes, "buffer", binding) ||
      !ParseMslAttributeUnsigned(attributes, "raster_order_group", raster_order_group)) {
    SetError(error_out, "translated xe_edram buffer binding is malformed");
    return false;
  }
  if (binding > kExactOutputMergerMaxMetalBufferIndex) {
    SetError(error_out, "translated xe_edram buffer binding is out of range");
    return false;
  }
  if (raster_order_group != 0) {
    SetError(error_out, "translated xe_edram is not in raster_order_group(0)");
    return false;
  }
  binding_out = binding;
  if (error_out) {
    error_out->clear();
  }
  return true;
}

void* CreateExactOutputMergerResources(void* metal_device, std::string* error_out) {
  id<MTLDevice> device = (id<MTLDevice>)metal_device;
  if (!device) {
    SetError(error_out, "exact output-merger Metal device is null");
    return nullptr;
  }
  auto* resources = new ExactOutputMergerResources;
  resources->device = [device retain];
  resources->edram = [device newBufferWithLength:kExactOutputMergerEdramSizeBytes
                                         options:MTLResourceStorageModeShared];
  if (!resources->edram || resources->edram.length != kExactOutputMergerEdramSizeBytes ||
      !resources->edram.contents) {
    SetError(error_out, "failed to allocate exact 10 MiB canonical Metal EDRAM buffer");
    ReleaseExactOutputMergerResources(resources);
    return nullptr;
  }
  resources->edram.label = @"GoldenEye exact output-merger canonical EDRAM";
  std::memset(resources->edram.contents, 0, kExactOutputMergerEdramSizeBytes);
  resources->committed_command_buffers.reserve(kMaximumCommittedExactCommandBuffers);
  resources->submission_stats.context_identity =
      g_next_exact_context_identity.fetch_add(1, std::memory_order_relaxed);
  resources->submission_stats.max_draws_per_command_buffer = kExactDrawsPerCommandBuffer;
  resources->submission_stats.max_committed_draw_command_buffer_count =
      kMaximumCommittedExactCommandBuffers;
  if (!resources->authority.MarkCpuUpload()) {
    SetError(error_out, "failed to establish exact output-merger CPU authority");
    ReleaseExactOutputMergerResources(resources);
    return nullptr;
  }
  return resources;
}

void ReleaseExactOutputMergerResources(void* resources_ptr) {
  auto* resources = static_cast<ExactOutputMergerResources*>(resources_ptr);
  if (!resources) {
    return;
  }
  {
    std::lock_guard<std::recursive_mutex> lock(resources->mutex);
    std::string ignored_error;
    WaitExactOutputMergerDrawsLocked(resources, &ignored_error, nullptr);
    DiscardOpenExactCommandBuffer(resources);
    for (CommittedExactCommandBuffer& committed : resources->committed_command_buffers) {
      [committed.command_buffer waitUntilCompleted];
      if (committed.command_buffer.status != MTLCommandBufferStatusCompleted) {
        NotifyExactCommandBufferFailure(committed);
      }
      [committed.command_buffer release];
      ReleaseExactUploadArena(resources, committed.upload_arena_index);
    }
    resources->committed_command_buffers.clear();
    for (auto& [key, sampler] : resources->sampler_cache) {
      (void)key;
      [sampler release];
    }
    resources->sampler_cache.clear();
    for (ExactUploadArena& arena : resources->upload_arenas) {
      for (ExactUploadChunk& chunk : arena.chunks) {
        [chunk.buffer release];
      }
      arena.chunks.clear();
      arena.in_use = false;
    }
  }
  for (CoverageTexture& coverage : resources->coverage) {
    [coverage.texture release];
    coverage = {};
  }
  [resources->resolve_pipeline release];
  [resources->clear_pipeline release];
  [resources->command_queue release];
  [resources->edram release];
  [resources->device release];
  delete resources;
}

bool SetExactOutputMergerCommandQueue(void* resources_ptr, void* command_queue_ptr,
                                      std::string* error_out) {
  auto* resources = static_cast<ExactOutputMergerResources*>(resources_ptr);
  id<MTLCommandQueue> command_queue = (id<MTLCommandQueue>)command_queue_ptr;
  if (!resources) {
    SetError(error_out, "exact output-merger command queue is invalid or busy");
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(resources->mutex);
  if (!resources->device || !command_queue || command_queue.device != resources->device ||
      !resources->authority.CanEncodeGpuWork()) {
    SetError(error_out, "exact output-merger command queue is invalid or busy");
    return false;
  }
  if (resources->command_queue && resources->command_queue != command_queue) {
    SetError(error_out, "exact output-merger command queue identity is already pinned");
    return false;
  }
  if (!resources->command_queue) {
    resources->command_queue = [command_queue retain];
  }
  if (error_out) {
    error_out->clear();
  }
  return true;
}

bool UploadExactOutputMergerEdram(void* resources_ptr, const void* source, size_t source_size,
                                  std::string* error_out) {
  auto* resources = static_cast<ExactOutputMergerResources*>(resources_ptr);
  if (!resources) {
    SetError(error_out, "exact output-merger upload is invalid or races GPU work");
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(resources->mutex);
  if ((resources->gpu_epoch_sequence || GetPendingExactDrawCount(resources)) &&
      !WaitExactOutputMergerDrawsLocked(resources, error_out, nullptr)) {
    return false;
  }
  if (!resources->edram || !source || source_size != kExactOutputMergerEdramSizeBytes) {
    SetError(error_out, "exact output-merger upload is invalid or races GPU work");
    return false;
  }
  // A complete 10 MiB upload is the only operation allowed to rehydrate an
  // invalid resource. Publish CPU authority first so an in-flight command
  // buffer rejects the upload before any bytes are modified.
  if (!resources->authority.MarkCpuUpload()) {
    SetError(error_out, "failed to publish exact output-merger CPU upload");
    return false;
  }
  std::memcpy(resources->edram.contents, source, source_size);
  if (error_out) {
    error_out->clear();
  }
  return true;
}

bool DownloadExactOutputMergerEdram(void* resources_ptr, void* destination, size_t destination_size,
                                    std::string* error_out) {
  auto* resources = static_cast<ExactOutputMergerResources*>(resources_ptr);
  if (!resources) {
    SetError(error_out, "exact output-merger download is invalid or races GPU work");
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(resources->mutex);
  if (!WaitExactOutputMergerDrawsLocked(resources, error_out, nullptr)) {
    return false;
  }
  if (!resources->edram || !destination || destination_size != kExactOutputMergerEdramSizeBytes ||
      !resources->authority.CanCpuAccess()) {
    SetError(error_out, "exact output-merger download is invalid or races GPU work");
    return false;
  }
  std::memcpy(destination, resources->edram.contents, destination_size);
  if (error_out) {
    error_out->clear();
  }
  return true;
}

void InvalidateExactOutputMergerResources(void* resources_ptr) {
  auto* resources = static_cast<ExactOutputMergerResources*>(resources_ptr);
  if (resources) {
    std::lock_guard<std::recursive_mutex> lock(resources->mutex);
    resources->authority.Invalidate();
  }
}

size_t GetExactOutputMergerEdramBufferLength(void* resources_ptr) {
  auto* resources = static_cast<ExactOutputMergerResources*>(resources_ptr);
  if (!resources) {
    return 0;
  }
  std::lock_guard<std::recursive_mutex> lock(resources->mutex);
  return resources->edram ? resources->edram.length : 0;
}

bool FinalizeExactOutputMergerDraws(void* resources_ptr, std::string* error_out) {
  auto* resources = static_cast<ExactOutputMergerResources*>(resources_ptr);
  if (!resources) {
    SetError(error_out, "missing exact output-merger resources");
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(resources->mutex);
  if (!FinalizeOpenExactCommandBuffer(resources, error_out)) {
    return false;
  }
  if (resources->authority.snapshot().authority == ExactOutputMergerResourceAuthority::kInvalid) {
    SetError(error_out, "exact output-merger authority is invalid");
    return false;
  }
  if (error_out) {
    error_out->clear();
  }
  return true;
}

bool WaitExactOutputMergerDraws(void* resources_ptr, std::string* error_out,
                                uint32_t* waited_draw_count_out) {
  auto* resources = static_cast<ExactOutputMergerResources*>(resources_ptr);
  if (!resources) {
    if (waited_draw_count_out) {
      *waited_draw_count_out = 0;
    }
    SetError(error_out, "missing exact output-merger resources");
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(resources->mutex);
  return WaitExactOutputMergerDrawsLocked(resources, error_out, waited_draw_count_out);
}

uint32_t GetExactOutputMergerPendingDrawCount(void* resources_ptr) {
  auto* resources = static_cast<ExactOutputMergerResources*>(resources_ptr);
  if (!resources) {
    return 0;
  }
  std::lock_guard<std::recursive_mutex> lock(resources->mutex);
  return GetPendingExactDrawCount(resources);
}

bool GetExactOutputMergerUploadStats(void* resources_ptr, PipelineProbeUploadStats* stats_out) {
  auto* resources = static_cast<ExactOutputMergerResources*>(resources_ptr);
  if (!resources || !stats_out) {
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(resources->mutex);
  *stats_out = resources->upload_stats;
  return true;
}

bool GetExactOutputMergerSubmissionStats(void* resources_ptr,
                                         PipelineProbeSubmissionStats* stats_out) {
  auto* resources = static_cast<ExactOutputMergerResources*>(resources_ptr);
  if (!resources || !stats_out) {
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(resources->mutex);
  *stats_out = resources->submission_stats;
  return true;
}

ExactOutputMergerExecutionResult ExecuteExactOutputMergerResolveAndClear(
    void* resources_ptr, const ExactOutputMergerResolvePlan& plan,
    const ExactOutputMergerResolveDestination& destination, std::string* error_out) {
  struct ResolveDispatchConstants {
    uint32_t edram_info;
    uint32_t coordinate_info;
    uint32_t destination_info;
    uint32_t destination_coordinate_info;
    uint32_t destination_base;
    uint32_t copy_width;
    uint32_t copy_height;
    uint32_t destination_buffer_size;
  };
  struct ClearDispatchConstants {
    uint32_t clear_value_0;
    uint32_t clear_value_1;
    uint32_t edram_info;
    uint32_t coordinate_info;
    uint32_t clear_width;
    uint32_t clear_height;
  };
  static_assert(sizeof(ResolveDispatchConstants) == sizeof(uint32_t) * 8);
  static_assert(sizeof(ClearDispatchConstants) == sizeof(uint32_t) * 6);

  auto* resources = static_cast<ExactOutputMergerResources*>(resources_ptr);
  if (!resources) {
    SetError(error_out, "missing exact output-merger resources for resolve");
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }
  std::lock_guard<std::recursive_mutex> lock(resources->mutex);
  if (resources->authority.snapshot().authority == ExactOutputMergerResourceAuthority::kInvalid) {
    SetError(error_out, "exact output-merger authority is invalid before resolve");
    return ExactOutputMergerExecutionResult::kFailedAfterSubmit;
  }

  id<MTLBuffer> destination_buffer = (id<MTLBuffer>)destination.metal_buffer;
  id<MTLBuffer> guest_memory_buffer = (id<MTLBuffer>)destination.guest_memory_metal_buffer;
  const draw_util::ResolveEdramInfo source = plan.copy_constants.dest_relative.edram_info;
  const draw_util::ResolveCoordinateInfo coordinates =
      plan.copy_constants.dest_relative.coordinate_info;
  const reg::RB_COPY_DEST_INFO destination_info = plan.copy_constants.dest_relative.dest_info;
  const draw_util::ResolveCopyDestCoordinateInfo destination_coordinates =
      plan.copy_constants.dest_relative.dest_coordinate_info;
  const uint32_t destination_pitch = uint32_t(destination_coordinates.pitch_aligned_div_32) * 32;
  const uint32_t destination_height = uint32_t(destination_coordinates.height_aligned_div_32) * 32;
  const uint32_t destination_x = uint32_t(destination_coordinates.offset_x_div_8) * 8;
  const uint32_t destination_y = uint32_t(destination_coordinates.offset_y_div_8) * 8;
  const uint64_t expected_start =
      uint64_t(plan.copy_constants.dest_base) +
      texture_util::GetTiledAddressLowerBound2D(destination_x, destination_y, destination_pitch, 2);
  const uint64_t expected_end =
      uint64_t(plan.copy_constants.dest_base) +
      texture_util::GetTiledAddressUpperBound2D(
          destination_x + plan.copy_width, destination_y + plan.copy_height, destination_pitch, 2);
  const uint64_t reported_end = uint64_t(plan.destination_start) + plan.destination_length;
  const bool source_depth = source.is_depth != 0;
  const xenos::CopySampleSelect sanitized_sample = draw_util::SanitizeCopySampleSelect(
      destination_coordinates.copy_sample_select, source.msaa_samples, source_depth);
  const bool color_format_valid =
      source_depth ||
      (xenos::ColorRenderTargetFormat(source.format) == xenos::ColorRenderTargetFormat::k_8_8_8_8 &&
       destination_info.copy_dest_format == xenos::ColorFormat::k_8_8_8_8);
  const bool depth_format_valid =
      !source_depth ||
      ((xenos::DepthRenderTargetFormat(source.format) == xenos::DepthRenderTargetFormat::kD24S8 ||
        xenos::DepthRenderTargetFormat(source.format) == xenos::DepthRenderTargetFormat::kD24FS8) &&
       uint32_t(destination_info.copy_dest_format) ==
           uint32_t(xenos::DepthRenderTargetFormat(source.format) ==
                            xenos::DepthRenderTargetFormat::kD24FS8
                        ? xenos::TextureFormat::k_24_8_FLOAT
                        : xenos::TextureFormat::k_24_8) &&
       xenos::IsSingleCopySampleSelected(destination_coordinates.copy_sample_select) &&
       !destination_info.copy_dest_swap);
  if (!resources->device || !resources->command_queue || !resources->edram || !destination_buffer ||
      !guest_memory_buffer || destination_buffer == guest_memory_buffer ||
      destination_buffer == resources->edram || guest_memory_buffer == resources->edram ||
      destination_buffer.device != resources->device ||
      guest_memory_buffer.device != resources->device ||
      destination_buffer.storageMode != MTLStorageModeShared ||
      guest_memory_buffer.storageMode != MTLStorageModeShared ||
      destination.metal_buffer_size > destination_buffer.length ||
      destination.guest_memory_metal_buffer_size > guest_memory_buffer.length ||
      destination.metal_buffer_size > UINT32_MAX || destination.metal_buffer_size < 4 ||
      ((plan.copy_constants.dest_base | plan.destination_start | plan.destination_length) &
       (sizeof(uint32_t) - 1)) ||
      !plan.copy_width || !plan.copy_height || !destination_pitch || !destination_height ||
      destination_x >= destination_pitch || destination_y >= destination_height ||
      plan.copy_width > destination_pitch - destination_x ||
      plan.copy_height > destination_height - destination_y ||
      coordinates.width_div_8 * 8 != plan.copy_width || coordinates.draw_resolution_scale_x != 1 ||
      coordinates.draw_resolution_scale_y != 1 || !source.pitch_tiles || source.format_is_64bpp ||
      source.fill_half_pixel_offset || source.msaa_samples > xenos::MsaaSamples::k4X ||
      source_depth != plan.copying_depth ||
      !IsExactEdramDispatchAddressingBounded(source, coordinates, plan.copy_width,
                                             plan.copy_height) ||
      sanitized_sample != destination_coordinates.copy_sample_select ||
      destination_info.copy_dest_array || destination_info.copy_dest_slice ||
      destination_info.copy_dest_exp_bias ||
      destination_info.copy_dest_number != xenos::SurfaceNumberFormat::kUnsignedRepeatingFraction ||
      uint32_t(destination_info.copy_dest_endian) > uint32_t(xenos::Endian128::k16in32) ||
      !color_format_valid || !depth_format_valid || expected_end <= expected_start ||
      expected_start != plan.destination_start || expected_end != reported_end ||
      reported_end > destination.metal_buffer_size ||
      reported_end > destination.guest_memory_metal_buffer_size ||
      !destination.submission_callback || !destination.async_failure_callback) {
    SetError(error_out, "exact output-merger resolve plan or shared destination is invalid");
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }

  auto validate_clear = [&](const draw_util::ResolveClearShaderConstants& clear,
                            bool expected_depth) {
    const draw_util::ResolveEdramInfo clear_surface = clear.rt_specific.edram_info;
    const bool format_valid = expected_depth
                                  ? (xenos::DepthRenderTargetFormat(clear_surface.format) ==
                                         xenos::DepthRenderTargetFormat::kD24S8 ||
                                     xenos::DepthRenderTargetFormat(clear_surface.format) ==
                                         xenos::DepthRenderTargetFormat::kD24FS8)
                                  : (IsCanonicalEdramColorFormatSupportedByMetal(
                                         xenos::ColorRenderTargetFormat(clear_surface.format)) &&
                                     !xenos::IsColorRenderTargetFormat64bpp(
                                         xenos::ColorRenderTargetFormat(clear_surface.format)));
    return clear.coordinate_info.packed == coordinates.packed &&
           uint32_t(clear.coordinate_info.width_div_8) * 8 == plan.copy_width &&
           clear.coordinate_info.draw_resolution_scale_x == 1 &&
           clear.coordinate_info.draw_resolution_scale_y == 1 && clear_surface.pitch_tiles &&
           !clear_surface.format_is_64bpp && !clear_surface.fill_half_pixel_offset &&
           clear_surface.msaa_samples <= xenos::MsaaSamples::k4X &&
           (clear_surface.is_depth != 0) == expected_depth && format_valid &&
           IsExactEdramDispatchAddressingBounded(clear_surface, clear.coordinate_info,
                                                 plan.copy_width, plan.copy_height);
  };
  if ((plan.clear_color && !validate_clear(plan.color_clear_constants, false)) ||
      (plan.clear_depth && !validate_clear(plan.depth_clear_constants, true)) ||
      (!plan.clear_color && plan.color_clear_constants.rt_specific.edram_info.packed) ||
      (!plan.clear_depth && plan.depth_clear_constants.rt_specific.edram_info.packed) ||
      (plan.copying_depth && plan.clear_color)) {
    SetError(error_out, "exact output-merger resolve clear plan is inconsistent");
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }
  if (!EnsureExactResolveAndClearPipelines(resources, error_out)) {
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }

  if (!FinalizeOpenExactCommandBuffer(resources, error_out)) {
    resources->authority.Invalidate();
    return ExactOutputMergerExecutionResult::kFailedAfterSubmit;
  }
  if (!RelieveExactSubmissionBackpressure(resources, error_out)) {
    resources->authority.Invalidate();
    return ExactOutputMergerExecutionResult::kFailedAfterSubmit;
  }

  bool began_epoch = false;
  if (!resources->gpu_epoch_sequence) {
    if (!resources->authority.BeginGpuSubmission(resources->gpu_epoch_sequence)) {
      SetError(error_out, "exact output-merger resolve could not begin its authority epoch");
      return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
    }
    began_epoch = true;
  } else if (!resources->authority.CanContinueGpuSubmission(resources->gpu_epoch_sequence)) {
    resources->authority.Invalidate();
    SetError(error_out, "exact output-merger resolve authority epoch is inconsistent");
    return ExactOutputMergerExecutionResult::kFailedAfterSubmit;
  }
  auto cancel_new_epoch = [&]() {
    if (began_epoch) {
      resources->authority.CancelGpuSubmission(resources->gpu_epoch_sequence);
      resources->gpu_epoch_sequence = 0;
    }
  };

  id<MTLCommandBuffer> command_buffer = [[resources->command_queue commandBuffer] retain];
  id<MTLComputeCommandEncoder> resolve_encoder =
      command_buffer ? [command_buffer computeCommandEncoder] : nil;
  if (!command_buffer || !resolve_encoder) {
    [command_buffer release];
    cancel_new_epoch();
    SetError(error_out, "failed to create exact output-merger resolve command encoder");
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }
  ResolveDispatchConstants resolve_constants = {
      source.packed,
      coordinates.packed,
      destination_info.value,
      destination_coordinates.packed,
      plan.copy_constants.dest_base,
      plan.copy_width,
      plan.copy_height,
      uint32_t(destination.metal_buffer_size),
  };
  [resolve_encoder setComputePipelineState:resources->resolve_pipeline];
  [resolve_encoder setBuffer:resources->edram offset:0 atIndex:0];
  [resolve_encoder setBuffer:destination_buffer offset:0 atIndex:1];
  [resolve_encoder setBytes:&resolve_constants length:sizeof(resolve_constants) atIndex:2];
  [resolve_encoder dispatchThreads:MTLSizeMake(plan.copy_width, plan.copy_height, 1)
             threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
  [resolve_encoder endEncoding];

  id<MTLBlitCommandEncoder> publication_encoder = [command_buffer blitCommandEncoder];
  if (!publication_encoder) {
    [command_buffer release];
    cancel_new_epoch();
    SetError(error_out, "failed to create exact output-merger resolve publication encoder");
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }
  [publication_encoder copyFromBuffer:destination_buffer
                         sourceOffset:plan.destination_start
                             toBuffer:guest_memory_buffer
                    destinationOffset:plan.destination_start
                                 size:plan.destination_length];
  [publication_encoder endEncoding];

  if (plan.clear_color || plan.clear_depth) {
    id<MTLComputeCommandEncoder> clear_encoder = [command_buffer computeCommandEncoder];
    if (!clear_encoder) {
      [command_buffer release];
      cancel_new_epoch();
      SetError(error_out, "failed to create exact output-merger clear command encoder");
      return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
    }
    [clear_encoder setComputePipelineState:resources->clear_pipeline];
    [clear_encoder setBuffer:resources->edram offset:0 atIndex:0];
    auto encode_clear = [&](const draw_util::ResolveClearShaderConstants& clear) {
      ClearDispatchConstants clear_constants = {clear.rt_specific.clear_value[0],
                                                clear.rt_specific.clear_value[1],
                                                clear.rt_specific.edram_info.packed,
                                                clear.coordinate_info.packed,
                                                plan.copy_width,
                                                plan.copy_height};
      [clear_encoder setBytes:&clear_constants length:sizeof(clear_constants) atIndex:1];
      [clear_encoder dispatchThreads:MTLSizeMake(plan.copy_width, plan.copy_height, 1)
               threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    };
    if (plan.clear_depth) {
      encode_clear(plan.depth_clear_constants);
    }
    if (plan.clear_color) {
      encode_clear(plan.color_clear_constants);
    }
    [clear_encoder endEncoding];
  }

  CommittedExactCommandBuffer committed;
  committed.command_buffer = command_buffer;
  committed.pending_work_count = 1;
  committed.async_failure_callback = destination.async_failure_callback;
  committed.async_failure_callback_context = destination.async_failure_callback_context;
  committed.async_failure_start = plan.destination_start;
  committed.async_failure_length = plan.destination_length;
  if (resources->committed_command_buffers.size() >= kMaximumCommittedExactCommandBuffers) {
    [command_buffer release];
    cancel_new_epoch();
    SetError(error_out, "exact output-merger resolve command-buffer cap was exceeded");
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }
  try {
    resources->committed_command_buffers.push_back(committed);
  } catch (...) {
    [command_buffer release];
    cancel_new_epoch();
    SetError(error_out, "failed to retain exact output-merger resolve metadata");
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }
  UpdateExactSubmissionHighWatermarks(resources);
  // Publish ownership only after the retained command buffer and its failure
  // revocation metadata are safely tracked, but before Metal can execute it.
  destination.submission_callback(destination.submission_callback_context, plan.destination_start,
                                  plan.destination_length);
  [command_buffer commit];
  ++resources->submission_stats.auxiliary_command_buffer_commit_count;
  if (error_out) {
    error_out->clear();
  }
  return ExactOutputMergerExecutionResult::kEnqueued;
}

// Test-only bounded probe: verifies both possible destination aliases are
// rejected without exposing the canonical EDRAM buffer to the caller.
bool ProbeExactOutputMergerRejectsEdramResolveAliasesForInternalProbe(
    void* resources_ptr, const ExactOutputMergerResolvePlan& plan, void* other_buffer_ptr,
    size_t other_buffer_size, std::string* error_out) {
  struct CallbackCounts {
    uint32_t submitted = 0;
    uint32_t failed = 0;
  } callbacks;
  auto submission_callback = [](void* context, uint32_t, uint32_t) {
    ++static_cast<CallbackCounts*>(context)->submitted;
  };
  auto failure_callback = [](void* context, uint32_t, uint32_t) {
    ++static_cast<CallbackCounts*>(context)->failed;
  };
  auto* resources = static_cast<ExactOutputMergerResources*>(resources_ptr);
  if (!resources || !resources->edram || !other_buffer_ptr) {
    SetError(error_out, "exact resolve alias probe arguments are invalid");
    return false;
  }
  ExactOutputMergerResolveDestination destination;
  destination.submission_callback = submission_callback;
  destination.submission_callback_context = &callbacks;
  destination.async_failure_callback = failure_callback;
  destination.async_failure_callback_context = &callbacks;

  std::string alias_error;
  destination.metal_buffer = resources->edram;
  destination.metal_buffer_size = resources->edram.length;
  destination.guest_memory_metal_buffer = other_buffer_ptr;
  destination.guest_memory_metal_buffer_size = other_buffer_size;
  const bool resident_alias_rejected =
      ExecuteExactOutputMergerResolveAndClear(resources, plan, destination, &alias_error) ==
      ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;

  destination.metal_buffer = other_buffer_ptr;
  destination.metal_buffer_size = other_buffer_size;
  destination.guest_memory_metal_buffer = resources->edram;
  destination.guest_memory_metal_buffer_size = resources->edram.length;
  const bool guest_alias_rejected =
      ExecuteExactOutputMergerResolveAndClear(resources, plan, destination, &alias_error) ==
      ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  if (!resident_alias_rejected || !guest_alias_rejected || callbacks.submitted ||
      callbacks.failed) {
    SetError(error_out, "exact EDRAM resolve alias was not rejected before callbacks");
    return false;
  }
  if (error_out) {
    error_out->clear();
  }
  return true;
}

bool ConfigureExactOutputMergerCoveragePipeline(void* descriptor_ptr, uint32_t sample_count,
                                                std::string* error_out) {
  MTLRenderPipelineDescriptor* descriptor = (MTLRenderPipelineDescriptor*)descriptor_ptr;
  if (!descriptor) {
    SetError(error_out, "exact output-merger pipeline descriptor is null");
    return false;
  }
  id<MTLFunction> vertex_function = [descriptor.vertexFunction retain];
  id<MTLFunction> fragment_function = [descriptor.fragmentFunction retain];
  [descriptor reset];
  descriptor.vertexFunction = vertex_function;
  descriptor.fragmentFunction = fragment_function;
  [vertex_function release];
  [fragment_function release];
  for (NSUInteger i = 0; i < 8; ++i) {
    descriptor.colorAttachments[i].writeMask = MTLColorWriteMaskNone;
  }

  size_t slot = 0;
  if (!GetSampleCountSlot(sample_count, slot)) {
    SetError(error_out, "exact output-merger pipeline sample count is invalid");
    return false;
  }
  descriptor.rasterSampleCount = sample_count;
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  descriptor.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
  if (error_out) {
    error_out->clear();
  }
  return true;
}

bool ConfigureExactOutputMergerCoveragePass(void* resources_ptr, void* descriptor_ptr,
                                            uint32_t width, uint32_t height, uint32_t sample_count,
                                            std::string* error_out) {
  auto* resources = static_cast<ExactOutputMergerResources*>(resources_ptr);
  MTLRenderPassDescriptor* descriptor = (MTLRenderPassDescriptor*)descriptor_ptr;
  ResetExactOutputMergerCoveragePass(descriptor);
  size_t slot = 0;
  if (!resources) {
    SetError(error_out, "exact output-merger coverage pass arguments are invalid");
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(resources->mutex);
  if (!resources->device || !descriptor || !width || !height ||
      !GetSampleCountSlot(sample_count, slot)) {
    SetError(error_out, "exact output-merger coverage pass arguments are invalid");
    return false;
  }
  if (![resources->device supportsTextureSampleCount:sample_count]) {
    SetError(error_out, "exact output-merger coverage sample count is unsupported");
    return false;
  }
  if (width > kMaximumCoverageTextureDimension || height > kMaximumCoverageTextureDimension) {
    SetError(error_out, "exact output-merger coverage extent exceeds Metal limits");
    return false;
  }

  CoverageTexture& coverage = resources->coverage[slot];
  if (!coverage.texture || coverage.width != width || coverage.height != height) {
    MTLTextureDescriptor* texture_descriptor = [[MTLTextureDescriptor alloc] init];
    texture_descriptor.textureType =
        sample_count == 1 ? MTLTextureType2D : MTLTextureType2DMultisample;
    texture_descriptor.pixelFormat = MTLPixelFormatBGRA8Unorm;
    texture_descriptor.width = width;
    texture_descriptor.height = height;
    texture_descriptor.mipmapLevelCount = 1;
    texture_descriptor.sampleCount = sample_count;
    texture_descriptor.storageMode = MTLStorageModePrivate;
    texture_descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [resources->device newTextureWithDescriptor:texture_descriptor];
    [texture_descriptor release];
    if (!texture) {
      SetError(error_out, "failed to allocate exact output-merger coverage attachment");
      return false;
    }
    texture.label = @"GoldenEye exact output-merger dummy coverage";
    [coverage.texture release];
    coverage.texture = texture;
    coverage.width = width;
    coverage.height = height;
  }
  descriptor.colorAttachments[0].texture = coverage.texture;
  descriptor.colorAttachments[0].loadAction = MTLLoadActionDontCare;
  descriptor.colorAttachments[0].storeAction = MTLStoreActionDontCare;
  descriptor.defaultRasterSampleCount = sample_count;
  if (sample_count > 1) {
    std::array<MTLSamplePosition, 4> sample_positions = {};
    if (!GetExactOutputMergerSamplePositions(sample_count, sample_positions)) {
      SetError(error_out, "exact output-merger sample positions are unsupported");
      return false;
    }
    [descriptor setSamplePositions:sample_positions.data() count:sample_count];
  }
  if (error_out) {
    error_out->clear();
  }
  return true;
}

static ExactOutputMergerPipeline* CreateExactOutputMergerPipelineFromLibrariesInternal(
    void* resources_ptr, void* vertex_library_ptr, const char* vertex_function_name,
    void* fragment_library_ptr, const char* fragment_function_name, uint32_t sample_count,
    const ExactOutputMergerPipelineLayout& layout, std::string* error_out) {
  auto* resources = static_cast<ExactOutputMergerResources*>(resources_ptr);
  id<MTLLibrary> vertex_library = (id<MTLLibrary>)vertex_library_ptr;
  id<MTLLibrary> fragment_library = (id<MTLLibrary>)fragment_library_ptr;
  if (!resources || !resources->device || !vertex_library || !fragment_library ||
      vertex_library.device != resources->device || fragment_library.device != resources->device ||
      !vertex_function_name || !vertex_function_name[0] || !fragment_function_name ||
      !fragment_function_name[0] || !ValidateExactPipelineLayout(layout, error_out)) {
    if (error_out && error_out->empty()) {
      *error_out = "exact output-merger translated pipeline arguments are invalid";
    }
    return nullptr;
  }
  size_t sample_slot = 0;
  if (!GetSampleCountSlot(sample_count, sample_slot) ||
      ![resources->device supportsTextureSampleCount:sample_count]) {
    SetError(error_out, "exact output-merger translated pipeline sample count is unsupported");
    return nullptr;
  }

  NSString* vertex_name = [NSString stringWithUTF8String:vertex_function_name];
  NSString* fragment_name = [NSString stringWithUTF8String:fragment_function_name];
  if (!vertex_name || !fragment_name) {
    SetError(error_out, "exact output-merger translated function name is invalid UTF-8");
    return nullptr;
  }
  id<MTLFunction> vertex_function = [vertex_library newFunctionWithName:vertex_name];
  id<MTLFunction> fragment_function = [fragment_library newFunctionWithName:fragment_name];
  if (!vertex_function || !fragment_function) {
    [vertex_function release];
    [fragment_function release];
    SetError(error_out, "exact output-merger translated Metal function is unavailable");
    return nullptr;
  }

  MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.label = @"GoldenEye exact output-merger translated pipeline";
  descriptor.vertexFunction = vertex_function;
  descriptor.fragmentFunction = fragment_function;
  if (!ConfigureExactOutputMergerCoveragePipeline(descriptor, sample_count, error_out)) {
    [descriptor release];
    [vertex_function release];
    [fragment_function release];
    return nullptr;
  }

  NSError* error = nil;
  MTLRenderPipelineReflection* reflection = nil;
  id<MTLRenderPipelineState> state =
      [resources->device newRenderPipelineStateWithDescriptor:descriptor
                                                      options:MTLPipelineOptionBindingInfo
                                                   reflection:&reflection
                                                        error:&error];
  [descriptor release];
  [vertex_function release];
  [fragment_function release];
  if (!state || state.device != resources->device || !reflection) {
    const char* description = error ? [[error localizedDescription] UTF8String] : nullptr;
    SetError(error_out,
             description
                 ? std::string_view(description)
                 : std::string_view("failed to create reflected exact output-merger pipeline"));
    [state release];
    return nullptr;
  }
  std::array<size_t, kExactOutputMergerMaxMetalBufferIndex + 1> vertex_min_buffer_lengths = {};
  std::array<size_t, kExactOutputMergerMaxMetalBufferIndex + 1> fragment_min_buffer_lengths = {};
  std::vector<MTLTextureType> vertex_texture_types;
  std::vector<MTLTextureType> fragment_texture_types;
  if (!ValidateExactStageReflection(reflection.vertexBindings, layout.vertex, false, UINT32_MAX,
                                    vertex_min_buffer_lengths, vertex_texture_types, error_out) ||
      !ValidateExactStageReflection(reflection.fragmentBindings, layout.fragment, true,
                                    layout.edram_fragment_buffer_index, fragment_min_buffer_lengths,
                                    fragment_texture_types, error_out)) {
    [state release];
    return nullptr;
  }

  auto* pipeline = new ExactOutputMergerPipeline;
  pipeline->device = [resources->device retain];
  pipeline->state = state;
  pipeline->sample_count = sample_count;
  pipeline->edram_fragment_buffer_index = layout.edram_fragment_buffer_index;
  pipeline->layout = layout;
  pipeline->vertex_min_buffer_lengths = vertex_min_buffer_lengths;
  pipeline->fragment_min_buffer_lengths = fragment_min_buffer_lengths;
  pipeline->vertex_texture_types = std::move(vertex_texture_types);
  pipeline->fragment_texture_types = std::move(fragment_texture_types);
  pipeline->has_complete_layout = true;
  if (error_out) {
    error_out->clear();
  }
  return pipeline;
}

ExactOutputMergerPipeline* CreateExactOutputMergerPipelineForInternalProbeFromMslSources(
    void* resources_ptr, const char* vertex_source, const char* vertex_function_name,
    const char* fragment_source, const char* fragment_function_name, uint32_t sample_count,
    const ExactOutputMergerPipelineLayout& layout, std::string* error_out) {
  auto* resources = static_cast<ExactOutputMergerResources*>(resources_ptr);
  uint32_t vertex_edram_binding = UINT32_MAX;
  uint32_t fragment_edram_binding = UINT32_MAX;
  if (!resources || !resources->device || !vertex_source || !fragment_source ||
      !vertex_function_name || !fragment_function_name ||
      !ValidateExactMslEntryPoint(vertex_source, "vertex", vertex_function_name, false,
                                  vertex_edram_binding, error_out) ||
      !ValidateExactMslEntryPoint(fragment_source, "fragment", fragment_function_name, true,
                                  fragment_edram_binding, error_out) ||
      fragment_edram_binding != layout.edram_fragment_buffer_index) {
    if (error_out && error_out->empty()) {
      *error_out = "exact output-merger probe source attestation does not match layout";
    }
    return nullptr;
  }

  void* vertex_library = CreateMslLibrary(resources->device, vertex_source, error_out);
  if (!vertex_library) {
    return nullptr;
  }
  void* fragment_library = CreateMslLibrary(resources->device, fragment_source, error_out);
  if (!fragment_library) {
    ReleaseMslLibrary(vertex_library);
    return nullptr;
  }
  ExactOutputMergerPipeline* pipeline = CreateExactOutputMergerPipelineFromLibrariesInternal(
      resources, vertex_library, vertex_function_name, fragment_library, fragment_function_name,
      sample_count, layout, error_out);
  ReleaseMslLibrary(vertex_library);
  ReleaseMslLibrary(fragment_library);
  return pipeline;
}

ExactOutputMergerPipeline* CreateExactOutputMergerPipelineFromTranslations(
    void* resources_ptr, MetalShader::MetalTranslation& vertex_translation,
    MetalShader::MetalTranslation& fragment_translation, uint32_t sample_count,
    std::string* error_out) {
  auto* resources = static_cast<ExactOutputMergerResources*>(resources_ptr);
  if (!resources || !resources->device || !vertex_translation.is_valid() ||
      !fragment_translation.is_valid() ||
      vertex_translation.mode() != MetalShader::TranslationMode::kExactOutputMerger ||
      fragment_translation.mode() != MetalShader::TranslationMode::kExactOutputMerger ||
      vertex_translation.shader().type() != xenos::ShaderType::kVertex ||
      fragment_translation.shader().type() != xenos::ShaderType::kPixel) {
    SetError(error_out, "exact output-merger production translations are invalid or wrong-stage");
    return nullptr;
  }

  uint32_t unexpected_vertex_edram = UINT32_MAX;
  uint32_t fragment_edram_binding = UINT32_MAX;
  if (!ValidateExactMslEntryPoint(vertex_translation.msl_source(), "vertex", "main0", false,
                                  unexpected_vertex_edram, error_out) ||
      !ValidateExactMslEntryPoint(fragment_translation.msl_source(), "fragment", "main0", true,
                                  fragment_edram_binding, error_out) ||
      fragment_edram_binding != fragment_translation.msl_reflection().edram_buffer_index) {
    if (error_out && error_out->empty()) {
      *error_out = "exact output-merger translation source attestation is inconsistent";
    }
    return nullptr;
  }

  ExactOutputMergerPipelineLayout layout;
  if (!BuildExactStageLayoutFromTranslation(vertex_translation, false, layout.vertex, error_out) ||
      !BuildExactStageLayoutFromTranslation(fragment_translation, true, layout.fragment,
                                            error_out)) {
    return nullptr;
  }
  layout.edram_fragment_buffer_index = fragment_edram_binding;
  if (!ValidateExactPipelineLayout(layout, error_out) ||
      !vertex_translation.CompileMslLibrary(resources->device, error_out) ||
      !fragment_translation.CompileMslLibrary(resources->device, error_out) ||
      !vertex_translation.metal_library_is_current_for_device(resources->device) ||
      !fragment_translation.metal_library_is_current_for_device(resources->device)) {
    if (error_out && error_out->empty()) {
      *error_out = "exact output-merger translated libraries are stale or unavailable";
    }
    return nullptr;
  }

  ExactOutputMergerPipeline* pipeline = CreateExactOutputMergerPipelineFromLibrariesInternal(
      resources, vertex_translation.metal_library(), "main0", fragment_translation.metal_library(),
      "main0", sample_count, layout, error_out);
  if (!pipeline) {
    return nullptr;
  }

  auto enforce_minimum = [](auto& minimums, uint32_t binding, size_t minimum_size) {
    if (binding != UINT32_MAX) {
      minimums[binding] = std::max(minimums[binding], minimum_size);
    }
  };
  constexpr size_t kFetchConstantBytes = 32 * 6 * sizeof(uint32_t);
  constexpr size_t kBoolLoopConstantBytes = (8 + 32) * sizeof(uint32_t);
  const size_t vertex_float_constant_bytes =
      size_t(vertex_translation.shader().constant_register_map().float_count) * 4 *
      sizeof(uint32_t);
  const size_t fragment_float_constant_bytes =
      size_t(fragment_translation.shader().constant_register_map().float_count) * 4 *
      sizeof(uint32_t);
  enforce_minimum(pipeline->vertex_min_buffer_lengths, layout.vertex.system_constants_buffer_index,
                  sizeof(SpirvShaderTranslator::SystemConstants));
  enforce_minimum(pipeline->fragment_min_buffer_lengths,
                  layout.fragment.system_constants_buffer_index,
                  sizeof(SpirvShaderTranslator::SystemConstants));
  enforce_minimum(pipeline->vertex_min_buffer_lengths, layout.vertex.shared_memory_buffer_index,
                  SharedMemory::kBufferSize);
  enforce_minimum(pipeline->fragment_min_buffer_lengths, layout.fragment.shared_memory_buffer_index,
                  SharedMemory::kBufferSize);
  enforce_minimum(pipeline->vertex_min_buffer_lengths, layout.vertex.float_constants_buffer_index,
                  vertex_float_constant_bytes);
  enforce_minimum(pipeline->fragment_min_buffer_lengths,
                  layout.fragment.float_constants_buffer_index, fragment_float_constant_bytes);
  enforce_minimum(pipeline->vertex_min_buffer_lengths, layout.vertex.fetch_constants_buffer_index,
                  kFetchConstantBytes);
  enforce_minimum(pipeline->fragment_min_buffer_lengths,
                  layout.fragment.fetch_constants_buffer_index, kFetchConstantBytes);
  enforce_minimum(pipeline->vertex_min_buffer_lengths,
                  layout.vertex.bool_loop_constants_buffer_index, kBoolLoopConstantBytes);
  enforce_minimum(pipeline->fragment_min_buffer_lengths,
                  layout.fragment.bool_loop_constants_buffer_index, kBoolLoopConstantBytes);
  pipeline->production_translation_contract = true;
  return pipeline;
}

bool GetExactOutputMergerPipelineLayout(const ExactOutputMergerPipeline* pipeline,
                                        ExactOutputMergerPipelineLayout& layout_out) {
  layout_out = {};
  if (!pipeline || !pipeline->has_complete_layout) {
    return false;
  }
  layout_out = pipeline->layout;
  return true;
}

void ReleaseExactOutputMergerPipeline(ExactOutputMergerPipeline* pipeline) {
  if (!pipeline) {
    return;
  }
  [pipeline->state release];
  [pipeline->device release];
  delete pipeline;
}

ExactOutputMergerExecutionResult ExecuteExactOutputMergerUnroutableDraw(
    void* resources_ptr, const ExactOutputMergerUnroutableDraw& draw, std::string* error_out) {
  auto* resources = static_cast<ExactOutputMergerResources*>(resources_ptr);
  ExactOutputMergerPipeline* pipeline = draw.pipeline;
  if (!resources) {
    SetError(error_out, "exact output-merger unroutable draw arguments are invalid");
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }
  std::lock_guard<std::recursive_mutex> lock(resources->mutex);
  if ((resources->gpu_epoch_sequence || GetPendingExactDrawCount(resources)) &&
      !WaitExactOutputMergerDrawsLocked(resources, error_out, nullptr)) {
    return ExactOutputMergerExecutionResult::kFailedAfterSubmit;
  }
  if (!resources->device || !resources->command_queue || !resources->edram || !pipeline ||
      !pipeline->state || pipeline->device != resources->device ||
      pipeline->state.device != resources->device || pipeline->sample_count != draw.sample_count ||
      pipeline->edram_fragment_buffer_index > kExactOutputMergerMaxMetalBufferIndex ||
      pipeline->production_translation_contract || !draw.coverage_width || !draw.coverage_height ||
      !draw.vertex_count || !draw.instance_count ||
      draw.fragment_inline_buffer_count > draw.fragment_inline_buffers.size() ||
      !resources->authority.CanEncodeGpuWork()) {
    SetError(error_out, "exact output-merger unroutable draw arguments are invalid");
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }
  constexpr size_t kMaximumInlineBytes = 4096;
  std::array<bool, kExactOutputMergerMaxMetalBufferIndex + 1> used_bindings = {};
  used_bindings[pipeline->edram_fragment_buffer_index] = true;
  for (uint32_t i = 0; i < draw.fragment_inline_buffer_count; ++i) {
    const ExactOutputMergerInlineBuffer& buffer = draw.fragment_inline_buffers[i];
    if (!buffer.data || !buffer.size || buffer.size > kMaximumInlineBytes ||
        buffer.index > kExactOutputMergerMaxMetalBufferIndex || used_bindings[buffer.index]) {
      SetError(error_out, "exact output-merger inline fragment binding is invalid or colliding");
      return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
    }
    used_bindings[buffer.index] = true;
  }

  MTLRenderPassDescriptor* render_pass = [MTLRenderPassDescriptor renderPassDescriptor];
  if (!ConfigureExactOutputMergerCoveragePass(resources, render_pass, draw.coverage_width,
                                              draw.coverage_height, draw.sample_count, error_out)) {
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }
  id<MTLCommandBuffer> command_buffer = [resources->command_queue commandBuffer];
  if (!command_buffer) {
    SetError(error_out, "failed to create exact output-merger command buffer");
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }
  uint64_t submission_sequence = 0;
  if (!resources->authority.BeginGpuSubmission(submission_sequence)) {
    SetError(error_out, "exact output-merger resource authority is busy or invalid");
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }

  id<MTLRenderCommandEncoder> encoder =
      [command_buffer renderCommandEncoderWithDescriptor:render_pass];
  if (!encoder) {
    resources->authority.CancelGpuSubmission(submission_sequence);
    SetError(error_out, "failed to create exact output-merger render encoder");
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }
  [encoder setRenderPipelineState:pipeline->state];
  for (uint32_t i = 0; i < draw.fragment_inline_buffer_count; ++i) {
    const ExactOutputMergerInlineBuffer& buffer = draw.fragment_inline_buffers[i];
    [encoder setFragmentBytes:buffer.data length:buffer.size atIndex:buffer.index];
  }
  [encoder setFragmentBuffer:resources->edram
                      offset:0
                     atIndex:pipeline->edram_fragment_buffer_index];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle
              vertexStart:draw.vertex_start
              vertexCount:draw.vertex_count
            instanceCount:draw.instance_count];
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  const bool succeeded = command_buffer.status == MTLCommandBufferStatusCompleted;
  if (!resources->authority.CompleteGpuSubmission(submission_sequence, succeeded)) {
    resources->authority.Invalidate();
    SetError(error_out, "exact output-merger completion sequence was invalid");
    return ExactOutputMergerExecutionResult::kFailedAfterSubmit;
  }
  if (!succeeded) {
    NSError* error = command_buffer.error;
    SetError(error_out, error ? std::string_view([[error localizedDescription] UTF8String])
                              : std::string_view("exact output-merger command buffer failed"));
    return ExactOutputMergerExecutionResult::kFailedAfterSubmit;
  }
  if (error_out) {
    error_out->clear();
  }
  return ExactOutputMergerExecutionResult::kCompleted;
}

ExactOutputMergerExecutionResult ExecuteExactOutputMergerDraw(
    void* resources_ptr, const ExactOutputMergerBorrowedDraw& draw, std::string* error_out) {
  auto* resources = static_cast<ExactOutputMergerResources*>(resources_ptr);
  ExactOutputMergerPipeline* pipeline = draw.pipeline;
  size_t sample_slot = 0;
  MTLPrimitiveType primitive_type = MTLPrimitiveTypeTriangle;
  if (!resources) {
    SetError(error_out, "exact output-merger borrowed draw arguments are invalid");
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }
  std::lock_guard<std::recursive_mutex> lock(resources->mutex);
  if (!resources->device || !resources->command_queue || !resources->edram || !pipeline ||
      !pipeline->state || !pipeline->has_complete_layout || pipeline->device != resources->device ||
      pipeline->state.device != resources->device || pipeline->sample_count != draw.sample_count ||
      pipeline->layout != draw.pipeline_layout ||
      pipeline->edram_fragment_buffer_index != draw.pipeline_layout.edram_fragment_buffer_index ||
      !ValidateExactPipelineLayout(draw.pipeline_layout, error_out) ||
      !GetSampleCountSlot(draw.sample_count, sample_slot) || !draw.coverage_width ||
      !draw.coverage_height || draw.coverage_width > kMaximumCoverageTextureDimension ||
      draw.coverage_height > kMaximumCoverageTextureDimension || !draw.vertex_count ||
      !draw.instance_count || !GetExactMetalPrimitiveType(draw.primitive_type, primitive_type) ||
      (draw.primitive_restart_enabled &&
       (!draw.index_buffer ||
        (draw.primitive_type != uint32_t(xenos::PrimitiveType::kLineStrip) &&
         draw.primitive_type != uint32_t(xenos::PrimitiveType::kTriangleStrip)) ||
        (draw.index_buffer->index_size == 2 ? draw.primitive_restart_index != UINT16_MAX
                                            : draw.primitive_restart_index != UINT32_MAX))) ||
      (!draw.primitive_restart_enabled && draw.primitive_restart_index != UINT32_MAX) ||
      (pipeline->production_translation_contract && (draw.base_vertex || draw.base_instance)) ||
      !ValidateExactRasterizationState(draw.rasterization_state, draw.coverage_width,
                                       draw.coverage_height) ||
      !(resources->authority.CanEncodeGpuWork() ||
        resources->authority.CanContinueGpuSubmission(resources->gpu_epoch_sequence))) {
    if (error_out && error_out->empty()) {
      *error_out = "exact output-merger borrowed draw arguments are invalid";
    }
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }

  const ExactOutputMergerStageLayout& vertex_layout = draw.pipeline_layout.vertex;
  const ExactOutputMergerStageLayout& fragment_layout = draw.pipeline_layout.fragment;
  auto reflected_minimum = [](const auto& minimums, uint32_t binding) {
    return binding == UINT32_MAX ? size_t(0) : minimums[binding];
  };
  if (!ValidateExactInlineArgument(draw.vertex_float_constants, draw.vertex_float_constants_size,
                                   vertex_layout.float_constants_buffer_index,
                                   reflected_minimum(pipeline->vertex_min_buffer_lengths,
                                                     vertex_layout.float_constants_buffer_index),
                                   error_out) ||
      !ValidateExactInlineArgument(draw.fragment_float_constants,
                                   draw.fragment_float_constants_size,
                                   fragment_layout.float_constants_buffer_index,
                                   reflected_minimum(pipeline->fragment_min_buffer_lengths,
                                                     fragment_layout.float_constants_buffer_index),
                                   error_out) ||
      !ValidateExactInlineArgument(draw.vertex_data, draw.vertex_data_size,
                                   vertex_layout.vertex_data_buffer_index,
                                   reflected_minimum(pipeline->vertex_min_buffer_lengths,
                                                     vertex_layout.vertex_data_buffer_index),
                                   error_out)) {
    if (error_out && error_out->empty()) {
      *error_out = "exact output-merger cross-stage inline buffer contract is invalid";
    }
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }
  if ((vertex_layout.vertex_data_buffer_index == UINT32_MAX && draw.vertex_data_stride) ||
      (vertex_layout.vertex_data_buffer_index != UINT32_MAX &&
       (!draw.vertex_data_stride || draw.vertex_data_stride > 2048 ||
        draw.vertex_data_stride != reflected_minimum(pipeline->vertex_min_buffer_lengths,
                                                     vertex_layout.vertex_data_buffer_index)))) {
    SetError(error_out, "exact output-merger vertex-data stride is invalid");
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }

  // System/fetch/bool constants are one canonical payload shared by both
  // translated stages. Either stage may optimize the declaration away, but
  // both may not demand different payloads.
  auto validate_shared_inline = [&](const void* data, size_t size, uint32_t vertex_index,
                                    uint32_t fragment_index) {
    const bool required = vertex_index != UINT32_MAX || fragment_index != UINT32_MAX;
    if (!required) {
      return data == nullptr && size == 0;
    }
    const size_t minimum_size =
        std::max(reflected_minimum(pipeline->vertex_min_buffer_lengths, vertex_index),
                 reflected_minimum(pipeline->fragment_min_buffer_lengths, fragment_index));
    return data != nullptr && size >= minimum_size && size <= kMaximumInlineArgumentBytes;
  };
  if (!validate_shared_inline(draw.system_constants, draw.system_constants_size,
                              vertex_layout.system_constants_buffer_index,
                              fragment_layout.system_constants_buffer_index) ||
      !validate_shared_inline(draw.fetch_constants, draw.fetch_constants_size,
                              vertex_layout.fetch_constants_buffer_index,
                              fragment_layout.fetch_constants_buffer_index) ||
      !validate_shared_inline(draw.bool_loop_constants, draw.bool_loop_constants_size,
                              vertex_layout.bool_loop_constants_buffer_index,
                              fragment_layout.bool_loop_constants_buffer_index)) {
    SetError(error_out, "exact output-merger shared inline payload is missing or oversized");
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }
  if (pipeline->production_translation_contract) {
    if (draw.system_constants_size != sizeof(SpirvShaderTranslator::SystemConstants)) {
      SetError(error_out, "exact output-merger production system constants have an invalid size");
      return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
    }
    const auto& system_constants =
        *static_cast<const SpirvShaderTranslator::SystemConstants*>(draw.system_constants);
    const uint32_t msaa_samples =
        (system_constants.flags >> SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift) &
        ((uint32_t(1) << xenos::kMsaaSamplesBits) - 1);
    if (msaa_samples > uint32_t(xenos::MsaaSamples::k4X) ||
        (uint32_t(1) << msaa_samples) != draw.sample_count) {
      SetError(error_out,
               "exact output-merger system constants disagree with the pipeline sample count");
      return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
    }
  }

  const bool needs_shared_memory = vertex_layout.shared_memory_buffer_index != UINT32_MAX ||
                                   fragment_layout.shared_memory_buffer_index != UINT32_MAX;
  if (needs_shared_memory) {
    id<MTLBuffer> shared_memory = (id<MTLBuffer>)draw.shared_memory_metal_buffer;
    const size_t minimum_shared_memory_size =
        std::max(reflected_minimum(pipeline->vertex_min_buffer_lengths,
                                   vertex_layout.shared_memory_buffer_index),
                 reflected_minimum(pipeline->fragment_min_buffer_lengths,
                                   fragment_layout.shared_memory_buffer_index));
    if (!shared_memory || draw.shared_memory_size < minimum_shared_memory_size ||
        (pipeline->production_translation_contract &&
         draw.shared_memory_size != SharedMemory::kBufferSize) ||
        shared_memory.device != resources->device ||
        shared_memory.length < draw.shared_memory_size) {
      SetError(error_out, "exact output-merger resident shared-memory buffer is invalid");
      return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
    }
  } else if (draw.shared_memory_metal_buffer || draw.shared_memory_size) {
    SetError(error_out, "exact output-merger supplied undeclared shared memory");
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }

  if (draw.vertex_texture_count != vertex_layout.texture_count ||
      draw.fragment_texture_count != fragment_layout.texture_count ||
      draw.vertex_sampler_count != vertex_layout.sampler_count ||
      draw.fragment_sampler_count != fragment_layout.sampler_count ||
      (draw.vertex_texture_count && !draw.vertex_textures) ||
      (draw.fragment_texture_count && !draw.fragment_textures) ||
      (draw.vertex_sampler_count && !draw.vertex_samplers) ||
      (draw.fragment_sampler_count && !draw.fragment_samplers)) {
    SetError(error_out, "exact output-merger borrowed resource counts do not match reflection");
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }
  if (pipeline->production_translation_contract) {
    for (size_t i = 0; i < draw.vertex_texture_count; ++i) {
      if (draw.vertex_textures[i].rgba) {
        SetError(error_out, "exact output-merger production draws require resident Metal textures");
        return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
      }
    }
    for (size_t i = 0; i < draw.fragment_texture_count; ++i) {
      if (draw.fragment_textures[i].rgba) {
        SetError(error_out, "exact output-merger production draws require resident Metal textures");
        return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
      }
    }
  }
  size_t cpu_texture_bytes = 0;
  for (size_t i = 0; i < draw.vertex_texture_count; ++i) {
    size_t slot_bytes = 0;
    if (!ValidateExactTextureSlot(resources->device, draw.vertex_textures[i],
                                  pipeline->vertex_texture_types[i], slot_bytes, error_out) ||
        !CheckedAddSize(cpu_texture_bytes, slot_bytes, cpu_texture_bytes) ||
        cpu_texture_bytes > kMaximumCpuTextureAggregateBytes) {
      if (error_out && error_out->empty()) {
        *error_out = "exact output-merger CPU texture staging budget is exceeded";
      }
      return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
    }
  }
  for (size_t i = 0; i < draw.fragment_texture_count; ++i) {
    size_t slot_bytes = 0;
    if (!ValidateExactTextureSlot(resources->device, draw.fragment_textures[i],
                                  pipeline->fragment_texture_types[i], slot_bytes, error_out) ||
        !CheckedAddSize(cpu_texture_bytes, slot_bytes, cpu_texture_bytes) ||
        cpu_texture_bytes > kMaximumCpuTextureAggregateBytes) {
      if (error_out && error_out->empty()) {
        *error_out = "exact output-merger CPU texture staging budget is exceeded";
      }
      return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
    }
  }
  for (size_t i = 0; i < draw.vertex_sampler_count; ++i) {
    if (!ValidateExactSamplerSlot(draw.vertex_samplers[i], error_out)) {
      return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
    }
  }
  for (size_t i = 0; i < draw.fragment_sampler_count; ++i) {
    if (!ValidateExactSamplerSlot(draw.fragment_samplers[i], error_out)) {
      return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
    }
  }

  id<MTLBuffer> borrowed_index_buffer = nil;
  size_t index_offset = 0;
  size_t index_required_bytes = 0;
  size_t index_first_byte_offset = 0;
  size_t maximum_vertex_data_index = 0;
  if (draw.index_buffer) {
    const ProbeIndexBuffer& index = *draw.index_buffer;
    const bool has_cpu_data = index.data != nullptr;
    const bool has_metal_buffer = index.metal_buffer != nullptr;
    size_t vertex_start_bytes = 0;
    size_t index_data_bytes = 0;
    size_t first_index_offset = 0;
    size_t end_offset = 0;
    if (has_cpu_data == has_metal_buffer ||
        (pipeline->production_translation_contract && has_cpu_data &&
         !index.production_host_data_trusted) ||
        (index.index_size != 2 && index.index_size != 4) || index.offset % index.index_size ||
        !CheckedMultiplySize(size_t(draw.vertex_start), size_t(index.index_size),
                             vertex_start_bytes) ||
        !CheckedMultiplySize(size_t(draw.vertex_count), size_t(index.index_size),
                             index_data_bytes) ||
        !CheckedAddSize(index.offset, vertex_start_bytes, first_index_offset) ||
        !CheckedAddSize(first_index_offset, index_data_bytes, end_offset) ||
        end_offset > index.size ||
        (has_cpu_data && index_data_bytes > kMaximumCpuIndexUploadBytes)) {
      SetError(error_out, "exact output-merger index buffer byte range is invalid");
      return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
    }
    if (has_metal_buffer) {
      borrowed_index_buffer = (id<MTLBuffer>)index.metal_buffer;
      if (borrowed_index_buffer.device != resources->device ||
          borrowed_index_buffer.length < index.size) {
        SetError(error_out, "exact output-merger borrowed index buffer is invalid");
        return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
      }
      if (vertex_layout.vertex_data_buffer_index != UINT32_MAX) {
        SetError(error_out,
                 "exact output-merger host vertex data cannot use an opaque Metal index buffer");
        return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
      }
    } else if (vertex_layout.vertex_data_buffer_index != UINT32_MAX) {
      const uint8_t* index_bytes = static_cast<const uint8_t*>(index.data) + first_index_offset;
      for (uint32_t i = 0; i < draw.vertex_count; ++i) {
        uint32_t index_value = 0;
        if (index.index_size == 2) {
          uint16_t index_u16 = 0;
          std::memcpy(&index_u16, index_bytes + size_t(i) * 2, sizeof(index_u16));
          index_value = index_u16;
        } else {
          std::memcpy(&index_value, index_bytes + size_t(i) * 4, sizeof(index_value));
        }
        const int64_t vertex_index = int64_t(index_value) + int64_t(draw.base_vertex);
        if (vertex_index < 0 ||
            uint64_t(vertex_index) > uint64_t(std::numeric_limits<size_t>::max())) {
          SetError(error_out, "exact output-merger indexed vertex-data range is invalid");
          return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
        }
        maximum_vertex_data_index = std::max(maximum_vertex_data_index, size_t(vertex_index));
      }
    }
    index_offset = has_cpu_data ? 0 : first_index_offset;
    index_required_bytes = index_data_bytes;
    index_first_byte_offset = first_index_offset;
  } else if (draw.base_vertex) {
    SetError(error_out, "exact output-merger non-indexed draw has a base vertex");
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  } else if (vertex_layout.vertex_data_buffer_index != UINT32_MAX) {
    size_t vertex_end = 0;
    if (!CheckedAddSize(size_t(draw.vertex_start), size_t(draw.vertex_count), vertex_end) ||
        !vertex_end) {
      SetError(error_out, "exact output-merger non-indexed vertex-data range is invalid");
      return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
    }
    maximum_vertex_data_index = vertex_end - 1;
  }

  if (vertex_layout.vertex_data_buffer_index != UINT32_MAX) {
    size_t required_vertices = 0;
    size_t required_vertex_bytes = 0;
    if (!CheckedAddSize(maximum_vertex_data_index, size_t(1), required_vertices) ||
        !CheckedMultiplySize(required_vertices, draw.vertex_data_stride, required_vertex_bytes) ||
        required_vertex_bytes > draw.vertex_data_size) {
      SetError(error_out, "exact output-merger host vertex data is truncated");
      return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
    }
  }

  if (!EnsureOpenExactCommandBuffer(resources, draw.coverage_width, draw.coverage_height,
                                    draw.sample_count, error_out)) {
    return resources->authority.snapshot().authority == ExactOutputMergerResourceAuthority::kInvalid
               ? ExactOutputMergerExecutionResult::kFailedAfterSubmit
               : ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }

  ExactUploadArenaCheckpoint upload_checkpoint;
  if (!CaptureExactUploadArenaCheckpoint(resources, upload_checkpoint, error_out)) {
    if (!resources->open_draw_count) {
      DiscardOpenExactCommandBuffer(resources);
    }
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }
  std::vector<uint64_t> inserted_sampler_keys;
  auto rollback_prepared_resources = [&]() {
    RollbackExactUploadArena(resources, upload_checkpoint);
    RollbackExactSamplerInsertions(resources, inserted_sampler_keys);
    if (!resources->open_draw_count) {
      DiscardOpenExactCommandBuffer(resources);
    }
  };

  const ExactUploadAllocation system_buffer =
      UploadExactDrawData(resources, draw.system_constants, draw.system_constants_size, error_out);
  const ExactUploadAllocation vertex_float_buffer = UploadExactDrawData(
      resources, draw.vertex_float_constants, draw.vertex_float_constants_size, error_out);
  const ExactUploadAllocation fragment_float_buffer = UploadExactDrawData(
      resources, draw.fragment_float_constants, draw.fragment_float_constants_size, error_out);
  const ExactUploadAllocation fetch_buffer =
      UploadExactDrawData(resources, draw.fetch_constants, draw.fetch_constants_size, error_out);
  const ExactUploadAllocation bool_loop_buffer = UploadExactDrawData(
      resources, draw.bool_loop_constants, draw.bool_loop_constants_size, error_out);
  const ExactUploadAllocation vertex_data_buffer =
      UploadExactDrawData(resources, draw.vertex_data, draw.vertex_data_size, error_out);
  ExactUploadAllocation uploaded_index_buffer;
  id<MTLBuffer> index_buffer = borrowed_index_buffer;
  if (draw.index_buffer && draw.index_buffer->data) {
    uploaded_index_buffer = UploadExactDrawData(
        resources, static_cast<const uint8_t*>(draw.index_buffer->data) + index_first_byte_offset,
        index_required_bytes, error_out);
    index_buffer = uploaded_index_buffer.buffer;
    index_offset = uploaded_index_buffer.offset;
  }
  const bool buffer_allocation_failed =
      (draw.system_constants_size && !system_buffer.buffer) ||
      (draw.vertex_float_constants_size && !vertex_float_buffer.buffer) ||
      (draw.fragment_float_constants_size && !fragment_float_buffer.buffer) ||
      (draw.fetch_constants_size && !fetch_buffer.buffer) ||
      (draw.bool_loop_constants_size && !bool_loop_buffer.buffer) ||
      (draw.vertex_data_size && !vertex_data_buffer.buffer) || (draw.index_buffer && !index_buffer);
  if (buffer_allocation_failed) {
    rollback_prepared_resources();
    if (error_out && error_out->empty()) {
      *error_out = "failed to stage exact output-merger argument buffers";
    }
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }

  std::vector<id<MTLTexture>> vertex_textures;
  std::vector<id<MTLTexture>> fragment_textures;
  std::vector<id<MTLSamplerState>> vertex_samplers;
  std::vector<id<MTLSamplerState>> fragment_samplers;
  vertex_textures.reserve(draw.vertex_texture_count);
  fragment_textures.reserve(draw.fragment_texture_count);
  vertex_samplers.reserve(draw.vertex_sampler_count);
  fragment_samplers.reserve(draw.fragment_sampler_count);
  bool resource_allocation_failed = false;
  for (size_t i = 0; i < draw.vertex_texture_count; ++i) {
    id<MTLTexture> texture = CreateExactTexture(resources->device, draw.vertex_textures[i]);
    resource_allocation_failed |= texture == nil;
    vertex_textures.push_back(texture);
  }
  for (size_t i = 0; i < draw.fragment_texture_count; ++i) {
    id<MTLTexture> texture = CreateExactTexture(resources->device, draw.fragment_textures[i]);
    resource_allocation_failed |= texture == nil;
    fragment_textures.push_back(texture);
  }
  for (size_t i = 0; i < draw.vertex_sampler_count; ++i) {
    bool created = false;
    id<MTLSamplerState> sampler =
        GetCachedExactSampler(resources, draw.vertex_samplers[i], &created, error_out);
    resource_allocation_failed |= sampler == nil;
    vertex_samplers.push_back(sampler);
    if (created) {
      inserted_sampler_keys.push_back(GetProbeSamplerKey(draw.vertex_samplers[i]));
    }
  }
  for (size_t i = 0; i < draw.fragment_sampler_count; ++i) {
    bool created = false;
    id<MTLSamplerState> sampler =
        GetCachedExactSampler(resources, draw.fragment_samplers[i], &created, error_out);
    resource_allocation_failed |= sampler == nil;
    fragment_samplers.push_back(sampler);
    if (created) {
      inserted_sampler_keys.push_back(GetProbeSamplerKey(draw.fragment_samplers[i]));
    }
  }
  auto release_cpu_textures = [&]() {
    for (size_t i = 0; i < vertex_textures.size(); ++i) {
      if (draw.vertex_textures[i].rgba) {
        [vertex_textures[i] release];
      }
    }
    for (size_t i = 0; i < fragment_textures.size(); ++i) {
      if (draw.fragment_textures[i].rgba) {
        [fragment_textures[i] release];
      }
    }
  };
  if (resource_allocation_failed) {
    release_cpu_textures();
    rollback_prepared_resources();
    if (error_out && error_out->empty()) {
      *error_out = "failed to allocate exact output-merger textures or samplers";
    }
    return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
  }

  if (!resources->gpu_epoch_sequence) {
    uint64_t sequence = 0;
    if (!resources->authority.BeginGpuSubmission(sequence)) {
      release_cpu_textures();
      rollback_prepared_resources();
      SetError(error_out, "exact output-merger resource authority is busy or invalid");
      return ExactOutputMergerExecutionResult::kRejectedBeforeSubmit;
    }
    resources->gpu_epoch_sequence = sequence;
  } else if (!resources->authority.CanContinueGpuSubmission(resources->gpu_epoch_sequence)) {
    release_cpu_textures();
    rollback_prepared_resources();
    resources->authority.Invalidate();
    SetError(error_out, "exact output-merger asynchronous GPU epoch is invalid");
    return ExactOutputMergerExecutionResult::kFailedAfterSubmit;
  }

  id<MTLRenderCommandEncoder> encoder = resources->open_render_encoder;

  [encoder setRenderPipelineState:pipeline->state];
  const ProbeRasterizationState* raster = draw.rasterization_state;
  const MTLViewport viewport =
      raster ? MTLViewport{raster->viewport_x,      raster->viewport_y,     raster->viewport_width,
                           raster->viewport_height, raster->viewport_z_min, raster->viewport_z_max}
             : MTLViewport{0.0, 0.0, double(draw.coverage_width), double(draw.coverage_height),
                           0.0, 1.0};
  const MTLScissorRect scissor =
      raster ? MTLScissorRect{raster->scissor_x, raster->scissor_y, raster->scissor_width,
                              raster->scissor_height}
             : MTLScissorRect{0, 0, draw.coverage_width, draw.coverage_height};
  [encoder setViewport:viewport];
  [encoder setScissorRect:scissor];
  [encoder setCullMode:GetExactMetalCullMode(raster ? raster->cull_mode : ProbeCullMode::kNone)];
  [encoder setFrontFacingWinding:raster && raster->front_face_clockwise
                                     ? MTLWindingClockwise
                                     : MTLWindingCounterClockwise];
  // Fragment interlock owns depth/stencil and polygon offset. Never let a
  // conventional Metal attachment perform those operations a second time.
  [encoder setDepthBias:0.0 slopeScale:0.0 clamp:0.0];
  [encoder setDepthClipMode:raster && raster->depth_clamp_enabled ? MTLDepthClipModeClamp
                                                                  : MTLDepthClipModeClip];

  auto bind_stage_buffer = [&](id<MTLBuffer> buffer, NSUInteger offset, uint32_t vertex_index,
                               uint32_t fragment_index) {
    if (vertex_index != UINT32_MAX) {
      [encoder setVertexBuffer:buffer offset:offset atIndex:vertex_index];
    }
    if (fragment_index != UINT32_MAX) {
      [encoder setFragmentBuffer:buffer offset:offset atIndex:fragment_index];
    }
  };
  bind_stage_buffer(system_buffer.buffer, system_buffer.offset,
                    vertex_layout.system_constants_buffer_index,
                    fragment_layout.system_constants_buffer_index);
  bind_stage_buffer(fetch_buffer.buffer, fetch_buffer.offset,
                    vertex_layout.fetch_constants_buffer_index,
                    fragment_layout.fetch_constants_buffer_index);
  bind_stage_buffer(bool_loop_buffer.buffer, bool_loop_buffer.offset,
                    vertex_layout.bool_loop_constants_buffer_index,
                    fragment_layout.bool_loop_constants_buffer_index);
  if (vertex_layout.float_constants_buffer_index != UINT32_MAX) {
    [encoder setVertexBuffer:vertex_float_buffer.buffer
                      offset:vertex_float_buffer.offset
                     atIndex:vertex_layout.float_constants_buffer_index];
  }
  if (fragment_layout.float_constants_buffer_index != UINT32_MAX) {
    [encoder setFragmentBuffer:fragment_float_buffer.buffer
                        offset:fragment_float_buffer.offset
                       atIndex:fragment_layout.float_constants_buffer_index];
  }
  if (needs_shared_memory) {
    bind_stage_buffer((id<MTLBuffer>)draw.shared_memory_metal_buffer, 0,
                      vertex_layout.shared_memory_buffer_index,
                      fragment_layout.shared_memory_buffer_index);
  }
  if (vertex_layout.vertex_data_buffer_index != UINT32_MAX) {
    [encoder setVertexBuffer:vertex_data_buffer.buffer
                      offset:vertex_data_buffer.offset
                     atIndex:vertex_layout.vertex_data_buffer_index];
  }
  [encoder setFragmentBuffer:resources->edram
                      offset:0
                     atIndex:draw.pipeline_layout.edram_fragment_buffer_index];
  if (!vertex_textures.empty()) {
    [encoder setVertexTextures:vertex_textures.data()
                     withRange:NSMakeRange(0, vertex_textures.size())];
  }
  if (!fragment_textures.empty()) {
    [encoder setFragmentTextures:fragment_textures.data()
                       withRange:NSMakeRange(0, fragment_textures.size())];
  }
  if (!vertex_samplers.empty()) {
    [encoder setVertexSamplerStates:vertex_samplers.data()
                          withRange:NSMakeRange(0, vertex_samplers.size())];
  }
  if (!fragment_samplers.empty()) {
    [encoder setFragmentSamplerStates:fragment_samplers.data()
                            withRange:NSMakeRange(0, fragment_samplers.size())];
  }

  if (index_buffer) {
    [encoder drawIndexedPrimitives:primitive_type
                        indexCount:draw.vertex_count
                         indexType:draw.index_buffer->index_size == 2 ? MTLIndexTypeUInt16
                                                                      : MTLIndexTypeUInt32
                       indexBuffer:index_buffer
                 indexBufferOffset:index_offset
                     instanceCount:draw.instance_count
                        baseVertex:draw.base_vertex
                      baseInstance:draw.base_instance];
  } else {
    [encoder drawPrimitives:primitive_type
                vertexStart:draw.vertex_start
                vertexCount:draw.vertex_count
              instanceCount:draw.instance_count
               baseInstance:draw.base_instance];
  }

  ++resources->open_draw_count;
  UpdateExactSubmissionHighWatermarks(resources);
  release_cpu_textures();
  if (resources->open_draw_count >= kExactDrawsPerCommandBuffer) {
    if (!FinalizeOpenExactCommandBuffer(resources, error_out) ||
        (resources->committed_command_buffers.size() >= kMaximumCommittedExactCommandBuffers &&
         !RelieveExactSubmissionBackpressure(resources, error_out))) {
      resources->authority.Invalidate();
      return ExactOutputMergerExecutionResult::kFailedAfterSubmit;
    }
  }
  if (error_out) {
    error_out->clear();
  }
  return ExactOutputMergerExecutionResult::kEnqueued;
}

}  // namespace rex::graphics::metal
