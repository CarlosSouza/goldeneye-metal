#include <rex/graphics/metal/msl_compiler.h>

#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <system_error>
#include <unordered_map>
#include <unistd.h>
#include <utility>

namespace rex::graphics::metal {
namespace {

static_assert(MTLCompareFunctionNever == 0 && MTLCompareFunctionLess == 1 &&
              MTLCompareFunctionEqual == 2 && MTLCompareFunctionLessEqual == 3 &&
              MTLCompareFunctionGreater == 4 && MTLCompareFunctionNotEqual == 5 &&
              MTLCompareFunctionGreaterEqual == 6 && MTLCompareFunctionAlways == 7);
static_assert(MTLStencilOperationKeep == 0 && MTLStencilOperationZero == 1 &&
              MTLStencilOperationReplace == 2 && MTLStencilOperationIncrementClamp == 3 &&
              MTLStencilOperationDecrementClamp == 4 && MTLStencilOperationInvert == 5 &&
              MTLStencilOperationIncrementWrap == 6 && MTLStencilOperationDecrementWrap == 7);

MTLPrimitiveType ToMetalPrimitiveType(uint32_t primitive_type) {
  switch (primitive_type) {
    case 1:  // PointList
      return MTLPrimitiveTypePoint;
    case 2:  // LineList
      return MTLPrimitiveTypeLine;
    case 3:   // LineStrip
    case 12:  // LineLoop
    case 21:  // 2DLineStrip
      return MTLPrimitiveTypeLineStrip;
    case 6:   // TriangleStrip
    case 22:  // 2DTriStrip
      return MTLPrimitiveTypeTriangleStrip;
    case 4:  // TriangleList
    case 5:  // TriangleFan, expanded later.
    case 8:  // RectangleList, expanded later.
    default:
      return MTLPrimitiveTypeTriangle;
  }
}

MTLCullMode ToMetalCullMode(ProbeCullMode cull_mode) {
  switch (cull_mode) {
    case ProbeCullMode::kFront:
      return MTLCullModeFront;
    case ProbeCullMode::kBack:
      return MTLCullModeBack;
    default:
      return MTLCullModeNone;
  }
}

bool IsProbeIndexBufferValid(const ProbeIndexBuffer* index_buffer, uint32_t index_count) {
  if (!index_buffer) {
    return true;
  }
  bool has_data = index_buffer->data != nullptr;
  bool has_metal_buffer = index_buffer->metal_buffer != nullptr;
  if (has_data == has_metal_buffer ||
      (index_buffer->index_size != 2 && index_buffer->index_size != 4) ||
      index_buffer->offset % index_buffer->index_size) {
    return false;
  }
  size_t required_size = size_t(index_count) * index_buffer->index_size;
  if (index_buffer->offset > index_buffer->size ||
      required_size > index_buffer->size - index_buffer->offset) {
    return false;
  }
  return !has_metal_buffer ||
         index_buffer->offset + required_size <= [(id<MTLBuffer>)index_buffer->metal_buffer length];
}

bool IsProbeRasterizationStateValid(const ProbeRasterizationState* state, uint32_t width,
                                    uint32_t height) {
  if (!state) {
    return true;
  }
  if (!std::isfinite(state->viewport_x) || !std::isfinite(state->viewport_y) ||
      !std::isfinite(state->viewport_width) || !std::isfinite(state->viewport_height) ||
      !std::isfinite(state->viewport_z_min) || !std::isfinite(state->viewport_z_max) ||
      state->viewport_width <= 0.0 || state->viewport_height <= 0.0 ||
      !std::isfinite(state->viewport_x + state->viewport_width) ||
      !std::isfinite(state->viewport_y + state->viewport_height) || state->viewport_x < 0.0 ||
      state->viewport_y < 0.0 || state->viewport_x > double(width) ||
      state->viewport_y > double(height) ||
      state->viewport_width > double(width) - state->viewport_x ||
      state->viewport_height > double(height) - state->viewport_y || state->viewport_z_min < 0.0 ||
      state->viewport_z_min > 1.0 || state->viewport_z_max < 0.0 || state->viewport_z_max > 1.0 ||
      !state->scissor_width || !state->scissor_height || state->scissor_x > width ||
      state->scissor_y > height || state->scissor_width > width - state->scissor_x ||
      state->scissor_height > height - state->scissor_y || !std::isfinite(state->blend_red) ||
      !std::isfinite(state->blend_green) || !std::isfinite(state->blend_blue) ||
      !std::isfinite(state->blend_alpha) || !std::isfinite(state->depth_bias) ||
      !std::isfinite(state->depth_bias_slope_scale) ||
      uint32_t(state->cull_mode) > uint32_t(ProbeCullMode::kBack)) {
    return false;
  }
  return true;
}

bool IsProbeDepthStencilStateValid(const ProbeDepthStencilState* state) {
  if (!state) {
    return true;
  }
  auto face_valid = [](const ProbeStencilFaceState& face) {
    return face.compare_function <= 7 && face.stencil_failure_operation <= 7 &&
           face.depth_failure_operation <= 7 && face.depth_stencil_pass_operation <= 7;
  };
  return state->depth_compare_function <= 7 && face_valid(state->front) && face_valid(state->back);
}

uint64_t GetProbeDepthStencilKey(const ProbeDepthStencilState& state) {
  uint64_t key = uint64_t(state.depth_test_enabled);
  key |= uint64_t(state.depth_write_enabled) << 1;
  key |= uint64_t(state.depth_compare_function & 7) << 2;
  key |= uint64_t(state.stencil_test_enabled) << 5;
  uint32_t shift = 6;
  auto append_face = [&](const ProbeStencilFaceState& face) {
    key |= uint64_t(face.compare_function & 7) << shift;
    shift += 3;
    key |= uint64_t(face.stencil_failure_operation & 7) << shift;
    shift += 3;
    key |= uint64_t(face.depth_failure_operation & 7) << shift;
    shift += 3;
    key |= uint64_t(face.depth_stencil_pass_operation & 7) << shift;
    shift += 3;
    key |= uint64_t(face.read_mask) << shift;
    shift += 8;
    key |= uint64_t(face.write_mask) << shift;
    shift += 8;
  };
  append_face(state.front);
  append_face(state.back);
  return key;
}

id<MTLDepthStencilState> CreateProbeDepthStencilState(id<MTLDevice> device,
                                                      const ProbeDepthStencilState& state) {
  MTLDepthStencilDescriptor* descriptor = [[MTLDepthStencilDescriptor alloc] init];
  if (state.depth_test_enabled) {
    descriptor.depthCompareFunction = MTLCompareFunction(state.depth_compare_function);
    descriptor.depthWriteEnabled = state.depth_write_enabled;
  }
  if (state.stencil_test_enabled) {
    auto make_face = [](const ProbeStencilFaceState& face) {
      MTLStencilDescriptor* descriptor = [[MTLStencilDescriptor alloc] init];
      descriptor.stencilCompareFunction = MTLCompareFunction(face.compare_function);
      descriptor.stencilFailureOperation = MTLStencilOperation(face.stencil_failure_operation);
      descriptor.depthFailureOperation = MTLStencilOperation(face.depth_failure_operation);
      descriptor.depthStencilPassOperation = MTLStencilOperation(face.depth_stencil_pass_operation);
      descriptor.readMask = face.read_mask;
      descriptor.writeMask = face.write_mask;
      return descriptor;
    };
    MTLStencilDescriptor* front = make_face(state.front);
    MTLStencilDescriptor* back = make_face(state.back);
    descriptor.frontFaceStencil = front;
    descriptor.backFaceStencil = back;
    [front release];
    [back release];
  }
  id<MTLDepthStencilState> depth_stencil_state =
      [device newDepthStencilStateWithDescriptor:descriptor];
  [descriptor release];
  return depth_stencil_state;
}

MTLBlendFactor GetMetalBlendFactor(uint32_t factor, bool alpha) {
  switch (factor & 0x1F) {
    case 1:
      return MTLBlendFactorOne;
    case 4:
    case 6:
      return alpha ? MTLBlendFactorSourceAlpha
                   : (factor == 4 ? MTLBlendFactorSourceColor : MTLBlendFactorSourceAlpha);
    case 5:
    case 7:
      return alpha ? MTLBlendFactorOneMinusSourceAlpha
                   : (factor == 5 ? MTLBlendFactorOneMinusSourceColor
                                  : MTLBlendFactorOneMinusSourceAlpha);
    case 8:
    case 10:
      return alpha
                 ? MTLBlendFactorDestinationAlpha
                 : (factor == 8 ? MTLBlendFactorDestinationColor : MTLBlendFactorDestinationAlpha);
    case 9:
    case 11:
      return alpha ? MTLBlendFactorOneMinusDestinationAlpha
                   : (factor == 9 ? MTLBlendFactorOneMinusDestinationColor
                                  : MTLBlendFactorOneMinusDestinationAlpha);
    case 12:
    case 14:
      return alpha || factor == 14 ? MTLBlendFactorBlendAlpha : MTLBlendFactorBlendColor;
    case 13:
    case 15:
      return alpha || factor == 15 ? MTLBlendFactorOneMinusBlendAlpha
                                   : MTLBlendFactorOneMinusBlendColor;
    case 16:
      return MTLBlendFactorSourceAlphaSaturated;
    default:
      return MTLBlendFactorZero;
  }
}

MTLBlendOperation GetMetalBlendOperation(uint32_t operation) {
  switch (operation & 0x7) {
    case 1:
      return MTLBlendOperationSubtract;
    case 2:
      return MTLBlendOperationMin;
    case 3:
      return MTLBlendOperationMax;
    case 4:
      return MTLBlendOperationReverseSubtract;
    default:
      return MTLBlendOperationAdd;
  }
}

id<MTLTexture> CreateProbeTexture(id<MTLDevice> device, const ProbeTextureSlot& slot,
                                  bool retain_external) {
  if (slot.metal_texture) {
    id<MTLTexture> texture = (id<MTLTexture>)slot.metal_texture;
    return retain_external ? [texture retain] : texture;
  }
  if (!slot.rgba || !slot.width || !slot.height || !slot.bytes_per_row) {
    return nil;
  }
  uint32_t array_length = slot.array_length ? slot.array_length : 1;
  MTLTextureDescriptor* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:slot.width
                                                        height:slot.height
                                                     mipmapped:NO];
  descriptor.textureType = MTLTextureType2DArray;
  descriptor.arrayLength = array_length;
  descriptor.usage = MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
  if (!texture) {
    return nil;
  }
  size_t bytes_per_image =
      slot.bytes_per_image ? slot.bytes_per_image : slot.bytes_per_row * size_t(slot.height);
  MTLRegion region = MTLRegionMake2D(0, 0, slot.width, slot.height);
  for (uint32_t slice = 0; slice < array_length; ++slice) {
    [texture replaceRegion:region
               mipmapLevel:0
                     slice:slice
                 withBytes:slot.rgba + bytes_per_image * slice
               bytesPerRow:slot.bytes_per_row
             bytesPerImage:bytes_per_image];
  }
  return texture;
}

void CreateProbeTextures(id<MTLDevice> device, const ProbeTextureSlot* slots, size_t slot_count,
                         id<MTLTexture> fallback_texture, std::vector<id<MTLTexture>>& textures_out,
                         bool retain_external = true) {
  textures_out.clear();
  textures_out.reserve(slot_count);
  for (size_t i = 0; i < slot_count; ++i) {
    id<MTLTexture> texture = slots ? CreateProbeTexture(device, slots[i], retain_external) : nil;
    textures_out.push_back(texture ? texture : fallback_texture);
  }
}

void BindProbeTextures(id<MTLRenderCommandEncoder> encoder,
                       const std::vector<id<MTLTexture>>& textures, bool vertex_stage) {
  if (textures.empty()) {
    return;
  }
  NSRange range = NSMakeRange(0, textures.size());
  if (vertex_stage) {
    [encoder setVertexTextures:textures.data() withRange:range];
  } else {
    [encoder setFragmentTextures:textures.data() withRange:range];
  }
}

MTLSamplerAddressMode ToMetalAddressMode(uint8_t address_mode) {
  switch (address_mode) {
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

id<MTLSamplerState> CreateProbeSampler(id<MTLDevice> device, const ProbeSamplerSlot& slot) {
  MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
  descriptor.minFilter =
      slot.min_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
  descriptor.magFilter =
      slot.mag_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
  descriptor.mipFilter = slot.mip_linear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
  descriptor.sAddressMode = ToMetalAddressMode(slot.address_mode_s);
  descriptor.tAddressMode = ToMetalAddressMode(slot.address_mode_t);
  descriptor.rAddressMode = ToMetalAddressMode(slot.address_mode_r);
  descriptor.borderColor = MTLSamplerBorderColorTransparentBlack;
  descriptor.maxAnisotropy = std::max<NSUInteger>(slot.max_anisotropy, 1);
  id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
  [descriptor release];
  return sampler;
}

void CreateProbeSamplers(id<MTLDevice> device, const ProbeSamplerSlot* slots, size_t slot_count,
                         id<MTLSamplerState> fallback_sampler,
                         std::vector<id<MTLSamplerState>>& samplers_out) {
  samplers_out.clear();
  samplers_out.reserve(slot_count);
  for (size_t i = 0; i < slot_count; ++i) {
    id<MTLSamplerState> sampler = slots ? CreateProbeSampler(device, slots[i]) : nil;
    samplers_out.push_back(sampler ? sampler : fallback_sampler);
  }
}

void BindProbeSamplers(id<MTLRenderCommandEncoder> encoder,
                       const std::vector<id<MTLSamplerState>>& samplers, bool vertex_stage) {
  if (samplers.empty()) {
    return;
  }
  NSRange range = NSMakeRange(0, samplers.size());
  if (vertex_stage) {
    [encoder setVertexSamplerStates:samplers.data() withRange:range];
  } else {
    [encoder setFragmentSamplerStates:samplers.data() withRange:range];
  }
}

void ReleaseOwnedProbeSamplers(std::vector<id<MTLSamplerState>>& samplers,
                               id<MTLSamplerState> fallback_sampler) {
  for (id<MTLSamplerState> sampler : samplers) {
    if (sampler && sampler != fallback_sampler) {
      [sampler release];
    }
  }
  samplers.clear();
}

void ReleaseOwnedProbeTextures(std::vector<id<MTLTexture>>& textures,
                               id<MTLTexture> fallback_texture,
                               const ProbeTextureSlot* borrowed_slots = nullptr) {
  for (size_t i = 0; i < textures.size(); ++i) {
    id<MTLTexture> texture = textures[i];
    bool borrowed = borrowed_slots && borrowed_slots[i].metal_texture;
    if (texture && texture != fallback_texture && !borrowed) {
      [texture release];
    }
  }
  textures.clear();
}

}  // namespace

namespace {

constexpr uintmax_t kMaximumPipelineArchiveBytes = uintmax_t(128) * 1024 * 1024;

uint64_t PipelineCacheNowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

NSString* PathToNSString(const std::filesystem::path& path) {
  const std::string path_utf8 = path.string();
  return [NSString stringWithUTF8String:path_utf8.c_str()];
}

std::string NSErrorDescription(NSError* error, const char* fallback) {
  if (!error) {
    return fallback;
  }
  NSString* description = [error localizedDescription];
  return description ? std::string([description UTF8String]) : fallback;
}

bool FsyncPath(const std::filesystem::path& path, std::string* error_out) {
  int descriptor = open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    if (error_out) {
      *error_out = "open for fsync failed: " + std::string(std::strerror(errno));
    }
    return false;
  }
  bool succeeded = fsync(descriptor) == 0;
  int saved_errno = errno;
  close(descriptor);
  if (!succeeded && error_out) {
    *error_out = "fsync failed: " + std::string(std::strerror(saved_errno));
  }
  return succeeded;
}

}  // namespace

uint64_t GetMetalDeviceCacheKey(void* metal_device) {
  if (!metal_device) {
    return 0;
  }
  id<MTLDevice> device = (id<MTLDevice>)metal_device;
  uint64_t registry_id = uint64_t([device registryID]);
  if (registry_id) {
    return registry_id;
  }

  // registryID is available on every supported macOS release, but retain a
  // deterministic fallback for unusual virtual/test devices.
  constexpr uint64_t kFnvOffset = UINT64_C(14695981039346656037);
  constexpr uint64_t kFnvPrime = UINT64_C(1099511628211);
  uint64_t hash = kFnvOffset;
  const char* name = [[device name] UTF8String];
  if (name) {
    for (const unsigned char* character = reinterpret_cast<const unsigned char*>(name); *character;
         ++character) {
      hash ^= *character;
      hash *= kFnvPrime;
    }
  }
  return hash;
}

void* CreateMetalPipelineBinaryArchive(void* metal_device,
                                       const std::filesystem::path& archive_path,
                                       bool* loaded_existing_out,
                                       std::string* warning_or_error_out) {
  if (loaded_existing_out) {
    *loaded_existing_out = false;
  }
  if (warning_or_error_out) {
    warning_or_error_out->clear();
  }
  if (!metal_device || archive_path.empty()) {
    if (warning_or_error_out) {
      *warning_or_error_out = "missing Metal device or archive path";
    }
    return nullptr;
  }

  std::error_code filesystem_error;
  const bool archive_exists = std::filesystem::exists(archive_path, filesystem_error);
  bool load_existing = false;
  if (filesystem_error) {
    if (warning_or_error_out) {
      *warning_or_error_out = "could not inspect existing Metal pipeline archive";
    }
  } else if (archive_exists) {
    load_existing = std::filesystem::is_regular_file(archive_path, filesystem_error);
    if (filesystem_error || !load_existing) {
      if (warning_or_error_out) {
        *warning_or_error_out = filesystem_error
                                    ? "could not inspect existing Metal pipeline archive"
                                    : "existing Metal pipeline archive is not a regular file; "
                                      "creating a fresh archive";
      }
      load_existing = false;
    }
  }
  if (load_existing) {
    uintmax_t archive_size = std::filesystem::file_size(archive_path, filesystem_error);
    if (filesystem_error || archive_size == 0 || archive_size > kMaximumPipelineArchiveBytes) {
      load_existing = false;
      if (warning_or_error_out) {
        *warning_or_error_out =
            filesystem_error ? "could not inspect existing Metal pipeline archive"
            : archive_size == 0
                ? "existing Metal pipeline archive is empty; creating a fresh archive"
                : "existing Metal pipeline archive exceeds the 128 MiB safety limit; "
                  "creating a fresh archive";
      }
    }
  }

  MTLBinaryArchiveDescriptor* descriptor = [[MTLBinaryArchiveDescriptor alloc] init];
  id<MTLBinaryArchive> archive = nil;
  if (load_existing) {
    NSString* archive_path_string = PathToNSString(archive_path);
    if (archive_path_string) {
      descriptor.url = [NSURL fileURLWithPath:archive_path_string];
      NSError* load_error = nil;
      archive = [(id<MTLDevice>)metal_device newBinaryArchiveWithDescriptor:descriptor
                                                                      error:&load_error];
      if (!archive && warning_or_error_out) {
        *warning_or_error_out =
            "existing Metal pipeline archive was rejected; creating a fresh archive: " +
            NSErrorDescription(load_error, "unknown Metal archive load error");
      }
    }
  }

  if (!archive) {
    descriptor.url = nil;
    NSError* create_error = nil;
    archive = [(id<MTLDevice>)metal_device newBinaryArchiveWithDescriptor:descriptor
                                                                    error:&create_error];
    if (!archive && warning_or_error_out) {
      *warning_or_error_out =
          "could not create Metal pipeline archive: " +
          NSErrorDescription(create_error, "unknown Metal archive creation error");
    }
  } else if (loaded_existing_out) {
    *loaded_existing_out = true;
  }
  [descriptor release];

  if (archive) {
    [archive setLabel:@"ReXGlue title pipeline cache"];
  }
  return archive;
}

bool SerializeMetalPipelineBinaryArchive(void* binary_archive,
                                         const std::filesystem::path& archive_path,
                                         uint64_t* serialized_size_out, std::string* error_out) {
  if (serialized_size_out) {
    *serialized_size_out = 0;
  }
  if (error_out) {
    error_out->clear();
  }
  if (!binary_archive || archive_path.empty()) {
    if (error_out) {
      *error_out = "missing Metal pipeline archive or destination path";
    }
    return false;
  }

  std::error_code filesystem_error;
  std::filesystem::create_directories(archive_path.parent_path(), filesystem_error);
  if (filesystem_error) {
    if (error_out) {
      *error_out = "could not create Metal pipeline cache directory: " + filesystem_error.message();
    }
    return false;
  }

  static std::atomic<uint64_t> temporary_file_sequence{0};
  std::filesystem::path temporary_path = archive_path;
  temporary_path += ".tmp-" + std::to_string(uint64_t(getpid())) + "-" +
                    std::to_string(temporary_file_sequence.fetch_add(1));
  std::filesystem::remove(temporary_path, filesystem_error);
  filesystem_error.clear();

  // This path also runs on the background cache worker. Give temporary
  // Foundation and Metal objects an explicit lifetime on non-AppKit threads.
  bool serialized = false;
  std::string objective_c_error;
  @autoreleasepool {
    NSString* temporary_path_string = PathToNSString(temporary_path);
    if (!temporary_path_string) {
      objective_c_error = "Metal pipeline cache path is not valid UTF-8";
    } else {
      NSError* serialize_error = nil;
      serialized = [(id<MTLBinaryArchive>)binary_archive
          serializeToURL:[NSURL fileURLWithPath:temporary_path_string]
                   error:&serialize_error];
      if (!serialized) {
        objective_c_error =
            NSErrorDescription(serialize_error, "unknown Metal archive serialization error");
      }
    }
  }
  if (!serialized) {
    if (error_out) {
      *error_out = std::move(objective_c_error);
    }
    std::filesystem::remove(temporary_path, filesystem_error);
    return false;
  }

  uintmax_t temporary_size = std::filesystem::file_size(temporary_path, filesystem_error);
  if (filesystem_error || temporary_size == 0 || temporary_size > kMaximumPipelineArchiveBytes) {
    if (error_out) {
      *error_out = filesystem_error ? "could not inspect serialized Metal pipeline archive"
                   : temporary_size == 0
                       ? "Metal serialized an empty pipeline archive"
                       : "serialized Metal pipeline archive exceeds the 128 MiB safety limit";
    }
    std::filesystem::remove(temporary_path, filesystem_error);
    return false;
  }

  if (!FsyncPath(temporary_path, error_out)) {
    std::filesystem::remove(temporary_path, filesystem_error);
    return false;
  }

  // POSIX rename within one directory atomically replaces the old archive, so
  // interruption can leave either the previous complete file or the new one,
  // never a partially serialized destination.
  if (rename(temporary_path.c_str(), archive_path.c_str()) != 0) {
    if (error_out) {
      *error_out =
          "atomic Metal pipeline archive replacement failed: " + std::string(std::strerror(errno));
    }
    std::filesystem::remove(temporary_path, filesystem_error);
    return false;
  }

  // Best effort: the file itself is durable already. Syncing the directory
  // also persists the rename across a sudden power loss on filesystems that
  // require it.
  int directory_descriptor = open(archive_path.parent_path().c_str(), O_RDONLY);
  if (directory_descriptor >= 0) {
    fsync(directory_descriptor);
    close(directory_descriptor);
  }
  if (serialized_size_out) {
    *serialized_size_out = uint64_t(temporary_size);
  }
  return true;
}

void ReleaseMetalPipelineBinaryArchive(void* binary_archive) {
  if (binary_archive) {
    [(id<MTLBinaryArchive>)binary_archive release];
  }
}

void* CreateMslLibrary(void* metal_device, const std::string& source, std::string* error_out) {
  if (!metal_device || source.empty()) {
    if (error_out) {
      *error_out = "missing Metal device or MSL source";
    }
    return nullptr;
  }

  NSError* error = nil;
  NSString* source_string = [NSString stringWithUTF8String:source.c_str()];
  id<MTLLibrary> library = [(id<MTLDevice>)metal_device newLibraryWithSource:source_string
                                                                     options:nil
                                                                       error:&error];
  if (!library) {
    if (error_out) {
      *error_out = error ? [[error localizedDescription] UTF8String] : "unknown Metal error";
    }
    return nullptr;
  }

  return library;
}

void ReleaseMslLibrary(void* metal_library) {
  if (metal_library) {
    [(id)metal_library release];
  }
}

bool ValidateMslSource(void* metal_device, const std::string& source, std::string* error_out) {
  void* library = CreateMslLibrary(metal_device, source, error_out);
  if (!library) {
    return false;
  }
  ReleaseMslLibrary(library);
  return true;
}

void* CreateRenderPipelineState(void* metal_device, void* vertex_library, void* fragment_library,
                                std::string* error_out,
                                const ProbeColorTargetState* color_target_state,
                                void* binary_archive,
                                RenderPipelineCacheTelemetry* cache_telemetry_out,
                                uint32_t sample_count) {
  RenderPipelineCacheTelemetry cache_telemetry;
  if (cache_telemetry_out) {
    *cache_telemetry_out = {};
  }
  if (!metal_device || !vertex_library || !fragment_library ||
      (sample_count != 1 && sample_count != 2 && sample_count != 4) ||
      ![(id<MTLDevice>)metal_device supportsTextureSampleCount:sample_count]) {
    if (error_out) {
      *error_out = !metal_device || !vertex_library || !fragment_library
                       ? "missing Metal device or shader library"
                       : "unsupported Metal render-pipeline sample count";
    }
    return nullptr;
  }

  id<MTLFunction> vertex_function = [(id<MTLLibrary>)vertex_library newFunctionWithName:@"main0"];
  id<MTLFunction> fragment_function =
      [(id<MTLLibrary>)fragment_library newFunctionWithName:@"main0"];
  if (!vertex_function || !fragment_function) {
    if (error_out) {
      *error_out = "missing main0 entry point";
    }
    if (vertex_function) {
      [vertex_function release];
    }
    if (fragment_function) {
      [fragment_function release];
    }
    return nullptr;
  }

  MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = vertex_function;
  descriptor.fragmentFunction = fragment_function;
  descriptor.rasterSampleCount = sample_count;
  descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
  descriptor.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
  MTLRenderPipelineColorAttachmentDescriptor* color_attachment = descriptor.colorAttachments[0];
  color_attachment.pixelFormat = MTLPixelFormatBGRA8Unorm;
  if (color_target_state) {
    uint32_t write_mask = color_target_state->write_mask & 0xF;
    MTLColorWriteMask metal_write_mask = MTLColorWriteMaskNone;
    if (write_mask & 0x1) {
      metal_write_mask |= MTLColorWriteMaskRed;
    }
    if (write_mask & 0x2) {
      metal_write_mask |= MTLColorWriteMaskGreen;
    }
    if (write_mask & 0x4) {
      metal_write_mask |= MTLColorWriteMaskBlue;
    }
    if (write_mask & 0x8) {
      metal_write_mask |= MTLColorWriteMaskAlpha;
    }
    color_attachment.writeMask = metal_write_mask;

    uint32_t blend_control = color_target_state->blend_control & 0x1FFF1FFF;
    uint32_t color_source = blend_control & 0x1F;
    uint32_t color_operation = (blend_control >> 5) & 0x7;
    uint32_t color_destination = (blend_control >> 8) & 0x1F;
    uint32_t alpha_source = (blend_control >> 16) & 0x1F;
    uint32_t alpha_operation = (blend_control >> 21) & 0x7;
    uint32_t alpha_destination = (blend_control >> 24) & 0x1F;
    color_attachment.sourceRGBBlendFactor = GetMetalBlendFactor(color_source, false);
    color_attachment.rgbBlendOperation = GetMetalBlendOperation(color_operation);
    color_attachment.destinationRGBBlendFactor = GetMetalBlendFactor(color_destination, false);
    color_attachment.sourceAlphaBlendFactor = GetMetalBlendFactor(alpha_source, true);
    color_attachment.alphaBlendOperation = GetMetalBlendOperation(alpha_operation);
    color_attachment.destinationAlphaBlendFactor = GetMetalBlendFactor(alpha_destination, true);
    color_attachment.blendingEnabled = color_source != 1 || color_operation != 0 ||
                                       color_destination != 0 || alpha_source != 1 ||
                                       alpha_operation != 0 || alpha_destination != 0;
  }

  NSError* error = nil;
  id<MTLRenderPipelineState> pipeline_state = nil;
  if (binary_archive) {
    cache_telemetry.archive_enabled = true;
    descriptor.binaryArchives = @[ (id<MTLBinaryArchive>)binary_archive ];

    uint64_t lookup_start_ns = PipelineCacheNowNs();
    pipeline_state = [(id<MTLDevice>)metal_device
        newRenderPipelineStateWithDescriptor:descriptor
                                     options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                  reflection:nil
                                       error:&error];
    cache_telemetry.archive_lookup_ns = PipelineCacheNowNs() - lookup_start_ns;
    if (pipeline_state) {
      cache_telemetry.archive_hit = true;
      cache_telemetry.pipeline_build_ns = cache_telemetry.archive_lookup_ns;
    } else {
      cache_telemetry.archive_miss = true;
      NSError* archive_add_error = nil;
      uint64_t archive_add_start_ns = PipelineCacheNowNs();
      bool archive_add_succeeded = [(id<MTLBinaryArchive>)binary_archive
          addRenderPipelineFunctionsWithDescriptor:descriptor
                                             error:&archive_add_error];
      cache_telemetry.archive_add_ns = PipelineCacheNowNs() - archive_add_start_ns;
      if (!archive_add_succeeded) {
        cache_telemetry.archive_update_failed = true;
        cache_telemetry.archive_error =
            NSErrorDescription(archive_add_error, "unknown Metal archive update error");
      }

      // A miss or an outdated archive must never affect correctness. Compile
      // through Metal's normal path, still attaching the archive so functions
      // successfully added above can be reused immediately.
      error = nil;
      uint64_t build_start_ns = PipelineCacheNowNs();
      pipeline_state =
          [(id<MTLDevice>)metal_device newRenderPipelineStateWithDescriptor:descriptor
                                                                    options:MTLPipelineOptionNone
                                                                 reflection:nil
                                                                      error:&error];
      cache_telemetry.pipeline_build_ns = PipelineCacheNowNs() - build_start_ns;
      cache_telemetry.archive_updated = archive_add_succeeded && pipeline_state;
    }
  } else {
    uint64_t build_start_ns = PipelineCacheNowNs();
    pipeline_state = [(id<MTLDevice>)metal_device newRenderPipelineStateWithDescriptor:descriptor
                                                                                 error:&error];
    cache_telemetry.pipeline_build_ns = PipelineCacheNowNs() - build_start_ns;
  }
  [descriptor release];
  [vertex_function release];
  [fragment_function release];

  if (cache_telemetry_out) {
    *cache_telemetry_out = std::move(cache_telemetry);
  }

  if (!pipeline_state) {
    if (error_out) {
      *error_out = error ? [[error localizedDescription] UTF8String] : "unknown Metal error";
    }
    return nullptr;
  }
  return pipeline_state;
}

void ReleaseRenderPipelineState(void* pipeline_state) {
  if (pipeline_state) {
    [(id)pipeline_state release];
  }
}

namespace {

constexpr uint32_t kMaxProbeDrawsPerCommandBuffer = 64;
constexpr uint32_t kMaxCommittedProbeCommandBuffers = 4;
constexpr size_t kProbeUploadAlignment = 256;
constexpr size_t kProbeUploadChunkSize = 1 << 20;
constexpr uint32_t kInvalidProbeUploadArena = UINT32_MAX;

uint32_t GetTiledRgba8Offset(uint32_t x, uint32_t y, uint32_t pitch) {
  pitch = (pitch + 31u) & ~31u;
  uint32_t macro = ((x >> 5u) + (y >> 5u) * (pitch >> 5u)) << 9u;
  uint32_t micro = ((x & 7u) + ((y & 14u) << 2u)) << 2u;
  uint32_t offset = macro + ((micro & ~15u) << 1u) + (micro & 15u) + ((y & 1u) << 4u);
  return ((offset & ~511u) << 3u) + ((y & 16u) << 7u) + ((offset & 448u) << 2u) +
         (((((y & 8u) >> 2u) + (x >> 3u)) & 3u) << 6u) + (offset & 63u);
}

uint32_t GetTiledRgba8UpperBound(uint32_t right, uint32_t bottom, uint32_t pitch) {
  if (!right || !bottom) {
    return 0;
  }
  uint32_t tile_x = (right - 1u) & ~31u;
  uint32_t tile_y = (bottom - 1u) & ~31u;
  return GetTiledRgba8Offset(tile_x, tile_y, pitch) + 4096u;
}

struct CommittedProbeCommandBuffer {
  // Explicit +1 ownership transferred from PipelineProbeContext's open buffer.
  id<MTLCommandBuffer> command_buffer = nil;
  uint32_t draw_submission_count = 0;
  uint32_t auxiliary_submission_count = 0;
  uint32_t upload_arena_index = kInvalidProbeUploadArena;
  void (*async_failure_callback)(void* context, uint32_t start, uint32_t length) = nullptr;
  void* async_failure_callback_context = nullptr;
  uint32_t async_failure_start = 0;
  uint32_t async_failure_length = 0;
};

struct ProbeUploadChunk {
  id<MTLBuffer> buffer = nil;
  size_t capacity = 0;
  size_t offset = 0;
};

struct ProbeUploadArena {
  std::vector<ProbeUploadChunk> chunks;
  bool in_use = false;
};

struct ProbeUploadAllocation {
  id<MTLBuffer> buffer = nil;
  NSUInteger offset = 0;
};

struct PipelineProbeContext;
void ResetOpenProbeBindingTracking(PipelineProbeContext* context);

struct ProbeDepthStencilTarget {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> command_queue = nil;
  id<MTLTexture> texture = nil;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t sample_count = 1;
  bool initialized = false;
  // Once multiple color contexts share this target, its logical dimensions
  // may only be changed by an explicit coordinated depth reset.
  bool extent_locked = false;
  // An open render encoder retains exclusive logical ownership. Before a
  // different color context uses this target, the previous owner's command
  // buffer is finalized so commits to the shared queue preserve draw order.
  PipelineProbeContext* open_owner = nullptr;
  std::vector<PipelineProbeContext*> attached_contexts;
};

struct PipelineProbeContext {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> command_queue = nil;
  id<MTLTexture> render_texture = nil;
  // For multisampled contexts, drawing targets this texture. Consumers resolve
  // it into render_texture only when a read, guest copy, or presentation needs
  // single-sample color data.
  id<MTLTexture> multisample_render_texture = nil;
  ProbeDepthStencilTarget* depth_stencil_target = nullptr;
  id<MTLBuffer> private_readback_buffer = nil;
  size_t private_readback_capacity = 0;
  id<MTLTexture> dummy_texture = nil;
  id<MTLSamplerState> dummy_sampler = nil;
  std::unordered_map<uint64_t, id<MTLSamplerState>> sampler_cache;
  std::unordered_map<uint64_t, id<MTLDepthStencilState>> depth_stencil_state_cache;
  id<MTLRenderPipelineState> clear_pipeline_state = nil;
  id<MTLRenderPipelineState> depth_clear_pipeline_state = nil;
  id<MTLComputePipelineState> multisample_select_resolve_pipeline_state = nil;
  id<MTLComputePipelineState> tiled_resolve_pipeline_state = nil;
  id<MTLComputePipelineState> depth_tiled_resolve_pipeline_state = nil;
  id<MTLComputePipelineState> multisample_depth_tiled_resolve_pipeline_state = nil;
  id<MTLTexture> depth_resolve_dummy_snapshot_texture = nil;
  id<MTLTexture> depth_resolve_dummy_packed_snapshot_texture = nil;
  MTLStorageMode storage_mode = MTLStorageModeShared;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t sample_count = 1;
  bool initialized = false;
  bool color_resolve_dirty = false;
  uint64_t multisample_resolve_count = 0;
  // commandBuffer and renderCommandEncoderWithDescriptor return autoreleased
  // objects. The open objects each have an explicit +1 retain so they remain
  // valid across RenderPipelineProbeToContext's per-call autorelease pools.
  id<MTLCommandBuffer> open_command_buffer = nil;
  id<MTLRenderCommandEncoder> open_render_encoder = nil;
  uint32_t open_draw_submission_count = 0;
  uint32_t open_upload_arena_index = kInvalidProbeUploadArena;
  // Bindings persist within an encoder. Track everything optional so each draw
  // can clear the previous draw's state before installing its own resources.
  uint32_t tracked_vertex_buffer_mask = 0;
  uint32_t tracked_fragment_buffer_mask = 0;
  NSUInteger tracked_vertex_texture_count = 0;
  NSUInteger tracked_fragment_texture_count = 0;
  NSUInteger tracked_vertex_sampler_count = 0;
  NSUInteger tracked_fragment_sampler_count = 0;
  // All buffers use this context's single queue, so waiting for the newest also
  // completes older buffers while retaining their individual error status.
  std::vector<CommittedProbeCommandBuffer> committed_command_buffers;
  // Each arena belongs to exactly one open or committed command buffer. It is
  // reset only after that buffer completes, so draw data may be copied once
  // and bound by offset without allocating an MTLBuffer for every argument.
  ProbeUploadArena upload_arenas[kMaxCommittedProbeCommandBuffers];
  PipelineProbeUploadStats upload_stats;
};

ProbeDepthStencilTarget* CreateProbeDepthStencilTarget(PipelineProbeContext* context) {
  if (!context || !context->device || !context->command_queue) {
    return nullptr;
  }
  auto* target = new ProbeDepthStencilTarget();
  target->device = context->device;
  target->command_queue = context->command_queue;
  target->attached_contexts.push_back(context);
  context->depth_stencil_target = target;
  return target;
}

void DetachProbeDepthStencilTarget(PipelineProbeContext* context) {
  if (!context || !context->depth_stencil_target) {
    return;
  }
  ProbeDepthStencilTarget* target = context->depth_stencil_target;
  if (target->open_owner == context) {
    target->open_owner = nullptr;
  }
  auto attached_it =
      std::find(target->attached_contexts.begin(), target->attached_contexts.end(), context);
  if (attached_it != target->attached_contexts.end()) {
    target->attached_contexts.erase(attached_it);
  }
  if (target->attached_contexts.size() <= 1) {
    target->extent_locked = false;
  }
  context->depth_stencil_target = nullptr;
  if (!target->attached_contexts.empty()) {
    return;
  }
  if (target->texture) {
    [target->texture release];
  }
  delete target;
}

void AttachProbeDepthStencilTarget(PipelineProbeContext* context, ProbeDepthStencilTarget* target) {
  if (!context || !target || context->depth_stencil_target == target) {
    return;
  }
  DetachProbeDepthStencilTarget(context);
  target->attached_contexts.push_back(context);
  target->extent_locked = target->attached_contexts.size() > 1;
  context->depth_stencil_target = target;
}

void ReleaseOpenProbeDepthStencilOwnership(PipelineProbeContext* context) {
  if (context && context->depth_stencil_target &&
      context->depth_stencil_target->open_owner == context) {
    context->depth_stencil_target->open_owner = nullptr;
  }
}

void InvalidateProbeContextTargets(PipelineProbeContext* context) {
  if (!context) {
    return;
  }
  context->initialized = false;
  context->color_resolve_dirty = false;
  if (context->depth_stencil_target) {
    context->depth_stencil_target->initialized = false;
  }
}

void InvalidateProbeContextColorTarget(PipelineProbeContext* context) {
  if (!context) {
    return;
  }
  context->initialized = false;
  context->color_resolve_dirty = false;
}

struct TiledResolveConstants {
  uint32_t source_row_pitch;
  uint32_t destination_buffer_offset;
  uint32_t destination_pitch;
  uint32_t destination_x;
  uint32_t destination_y;
  uint32_t copy_width;
  uint32_t copy_height;
  uint32_t destination_endian;
};

struct MultisampleSelectResolveConstants {
  uint32_t source_x;
  uint32_t source_y;
  uint32_t copy_width;
  uint32_t copy_height;
  uint32_t destination_row_pitch;
  uint32_t host_sample_mask;
};

struct DepthTiledResolveConstants {
  uint32_t source_x;
  uint32_t source_y;
  uint32_t destination_buffer_offset;
  uint32_t destination_pitch;
  uint32_t destination_x;
  uint32_t destination_y;
  uint32_t copy_width;
  uint32_t copy_height;
  uint32_t destination_endian;
  uint32_t host_sample;
  uint32_t depth_float24;
  uint32_t depth_float24_round;
  uint32_t snapshot_enabled;
  uint32_t snapshot_destination_x;
  uint32_t snapshot_destination_y;
  uint32_t packed_snapshot_enabled;
  uint32_t packed_snapshot_destination_x;
  uint32_t packed_snapshot_destination_y;
  uint32_t packed_snapshot_fetch_endian;
};

void ConfigureProbeDepthStencilPass(MTLRenderPassDescriptor* pass,
                                    id<MTLTexture> depth_stencil_texture, MTLLoadAction load_action,
                                    double clear_depth = 1.0, uint32_t clear_stencil = 0) {
  pass.depthAttachment.texture = depth_stencil_texture;
  pass.depthAttachment.loadAction = load_action;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.depthAttachment.clearDepth = clear_depth;
  pass.stencilAttachment.texture = depth_stencil_texture;
  pass.stencilAttachment.loadAction = load_action;
  pass.stencilAttachment.storeAction = MTLStoreActionStore;
  pass.stencilAttachment.clearStencil = clear_stencil;
}

void ConfigureProbeColorPass(MTLRenderPassDescriptor* pass, PipelineProbeContext* context,
                             MTLLoadAction load_action, MTLClearColor clear_color) {
  MTLRenderPassColorAttachmentDescriptor* color = pass.colorAttachments[0];
  color.texture =
      context->sample_count > 1 ? context->multisample_render_texture : context->render_texture;
  color.loadAction = load_action;
  color.clearColor = clear_color;
  if (context->sample_count > 1) {
    color.resolveTexture = nil;
    color.storeAction = MTLStoreActionStore;
  } else {
    color.resolveTexture = nil;
    color.storeAction = MTLStoreActionStore;
  }
}

void ResetProbeUploadArena(ProbeUploadArena& arena) {
  for (ProbeUploadChunk& chunk : arena.chunks) {
    chunk.offset = 0;
  }
}

uint32_t AcquireProbeUploadArena(PipelineProbeContext* context) {
  for (uint32_t i = 0; i < kMaxCommittedProbeCommandBuffers; ++i) {
    ProbeUploadArena& arena = context->upload_arenas[i];
    if (!arena.in_use) {
      ResetProbeUploadArena(arena);
      arena.in_use = true;
      return i;
    }
  }
  return kInvalidProbeUploadArena;
}

void ReleaseProbeUploadArena(PipelineProbeContext* context, uint32_t arena_index) {
  if (arena_index >= kMaxCommittedProbeCommandBuffers) {
    return;
  }
  ProbeUploadArena& arena = context->upload_arenas[arena_index];
  ResetProbeUploadArena(arena);
  arena.in_use = false;
}

void NotifyProbeCommandFailure(const CommittedProbeCommandBuffer& committed) {
  if (committed.async_failure_callback && committed.async_failure_length) {
    committed.async_failure_callback(committed.async_failure_callback_context,
                                     committed.async_failure_start, committed.async_failure_length);
  }
}

ProbeUploadAllocation UploadProbeDrawData(PipelineProbeContext* context, const void* source,
                                          size_t length, std::string* error_out) {
  ProbeUploadAllocation allocation;
  if (!source || !length) {
    return allocation;
  }
  if (context->open_upload_arena_index >= kMaxCommittedProbeCommandBuffers ||
      length > SIZE_MAX - (kProbeUploadAlignment - 1)) {
    if (error_out) {
      *error_out = "persistent probe upload arena is unavailable or the upload is too large";
    }
    return allocation;
  }

  ProbeUploadArena& arena = context->upload_arenas[context->open_upload_arena_index];
  for (ProbeUploadChunk& chunk : arena.chunks) {
    if (chunk.offset > SIZE_MAX - (kProbeUploadAlignment - 1)) {
      continue;
    }
    size_t aligned_offset =
        (chunk.offset + (kProbeUploadAlignment - 1)) & ~(kProbeUploadAlignment - 1);
    if (aligned_offset <= chunk.capacity && length <= chunk.capacity - aligned_offset) {
      uint8_t* destination = static_cast<uint8_t*>([chunk.buffer contents]);
      if (!destination) {
        continue;
      }
      std::memcpy(destination + aligned_offset, source, length);
      chunk.offset = aligned_offset + length;
      allocation.buffer = chunk.buffer;
      allocation.offset = NSUInteger(aligned_offset);
      ++context->upload_stats.suballocation_count;
      context->upload_stats.suballocation_bytes += length;
      return allocation;
    }
  }

  size_t aligned_length = (length + (kProbeUploadAlignment - 1)) & ~(kProbeUploadAlignment - 1);
  size_t chunk_capacity = std::max(kProbeUploadChunkSize, aligned_length);
  id<MTLBuffer> buffer = [context->device newBufferWithLength:chunk_capacity
                                                      options:MTLResourceStorageModeShared];
  uint8_t* destination = buffer ? static_cast<uint8_t*>([buffer contents]) : nullptr;
  if (!buffer || !destination) {
    if (buffer) {
      [buffer release];
    }
    if (error_out) {
      *error_out = "failed to grow the persistent probe upload arena";
    }
    return allocation;
  }

  std::memcpy(destination, source, length);
  ProbeUploadChunk chunk;
  chunk.buffer = buffer;
  chunk.capacity = chunk_capacity;
  chunk.offset = length;
  arena.chunks.push_back(chunk);
  allocation.buffer = buffer;
  ++context->upload_stats.buffer_allocation_count;
  context->upload_stats.buffer_allocation_bytes += chunk_capacity;
  ++context->upload_stats.suballocation_count;
  context->upload_stats.suballocation_bytes += length;
  return allocation;
}

void DiscardEmptyOpenPipelineProbeCommandBuffer(PipelineProbeContext* context) {
  if (!context || context->open_draw_submission_count) {
    return;
  }
  if (context->open_render_encoder) {
    [context->open_render_encoder endEncoding];
    [context->open_render_encoder release];
    context->open_render_encoder = nil;
  }
  if (context->open_command_buffer) {
    [context->open_command_buffer release];
    context->open_command_buffer = nil;
  }
  ReleaseProbeUploadArena(context, context->open_upload_arena_index);
  context->open_upload_arena_index = kInvalidProbeUploadArena;
  ReleaseOpenProbeDepthStencilOwnership(context);
  ResetOpenProbeBindingTracking(context);
}

bool GetProbeColorSampleMask(uint32_t sample_count, uint32_t sample_select,
                             uint32_t& host_sample_mask_out) {
  uint32_t full_sample_mask = (uint32_t(1) << sample_count) - 1;
  if (sample_select == UINT32_MAX) {
    host_sample_mask_out = full_sample_mask;
    return true;
  }
  if (sample_count == 1) {
    if (sample_select != 0) {
      return false;
    }
    host_sample_mask_out = 1;
    return true;
  }
  if (sample_count == 2) {
    // Xenos 2x sample 0 is the top sample. In the standard native 2x pattern,
    // Metal sample 1 is top-left and sample 0 is bottom-right.
    switch (sample_select) {
      case 0:
        host_sample_mask_out = uint32_t(1) << 1;
        return true;
      case 1:
        host_sample_mask_out = uint32_t(1) << 0;
        return true;
      case 4:
        host_sample_mask_out = full_sample_mask;
        return true;
      default:
        return false;
    }
  }
  if (sample_count == 4) {
    // Xenos orders the samples TL, BL, TR, BR, while Metal uses the standard
    // host ordering TL, TR, BL, BR.
    constexpr uint32_t kGuestToHostSample[4] = {0, 2, 1, 3};
    switch (sample_select) {
      case 0:
      case 1:
      case 2:
      case 3:
        host_sample_mask_out = uint32_t(1) << kGuestToHostSample[sample_select];
        return true;
      case 4:
        host_sample_mask_out =
            (uint32_t(1) << kGuestToHostSample[0]) | (uint32_t(1) << kGuestToHostSample[1]);
        return true;
      case 5:
        host_sample_mask_out =
            (uint32_t(1) << kGuestToHostSample[2]) | (uint32_t(1) << kGuestToHostSample[3]);
        return true;
      case 6:
        host_sample_mask_out = full_sample_mask;
        return true;
      default:
        return false;
    }
  }
  return false;
}

bool GetProbeDepthSample(uint32_t sample_count, uint32_t sample_select, uint32_t& host_sample_out) {
  if (sample_count == 1) {
    if (sample_select != 0) {
      return false;
    }
    host_sample_out = 0;
    return true;
  }
  if (sample_count == 2) {
    if (sample_select > 1) {
      return false;
    }
    // Xenos sample 0 is the top sample. Metal's standard 2x ordering is the
    // inverse: sample 1 is top-left and sample 0 is bottom-right.
    host_sample_out = sample_select ? 0 : 1;
    return true;
  }
  if (sample_count == 4 && sample_select <= 3) {
    // Xenos TL, BL, TR, BR -> Metal TL, TR, BL, BR.
    constexpr uint32_t kGuestToHostSample[4] = {0, 2, 1, 3};
    host_sample_out = kGuestToHostSample[sample_select];
    return true;
  }
  return false;
}

bool EnsureMultisampleSelectResolvePipelineState(PipelineProbeContext* context,
                                                 std::string* error_out) {
  if (!context || !context->device) {
    if (error_out) {
      *error_out = "missing probe context or Metal device";
    }
    return false;
  }
  if (context->multisample_select_resolve_pipeline_state) {
    return true;
  }
  static constexpr char kMultisampleSelectResolveMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct MultisampleSelectResolveConstants {
  uint source_x;
  uint source_y;
  uint copy_width;
  uint copy_height;
  uint destination_row_pitch;
  uint host_sample_mask;
};

kernel void resolve_selected_color_samples(
    texture2d_ms<float, access::read> source [[texture(0)]],
    device uchar* destination [[buffer(0)]],
    constant MultisampleSelectResolveConstants& constants [[buffer(1)]],
    uint2 position [[thread_position_in_grid]]) {
  if (position.x >= constants.copy_width || position.y >= constants.copy_height) {
    return;
  }
  float4 rgba = 0.0f;
  uint selected_count = 0;
  for (uint sample = 0; sample < 4; ++sample) {
    if (constants.host_sample_mask & (1u << sample)) {
      rgba += source.read(position + uint2(constants.source_x, constants.source_y), sample);
      ++selected_count;
    }
  }
  rgba /= float(max(selected_count, 1u));
  uchar4 bgra = uchar4(clamp(rint(rgba.zyxw * 255.0f), 0.0f, 255.0f));
  uint destination_offset = position.y * constants.destination_row_pitch + position.x * 4u;
  destination[destination_offset] = bgra.x;
  destination[destination_offset + 1u] = bgra.y;
  destination[destination_offset + 2u] = bgra.z;
  destination[destination_offset + 3u] = bgra.w;
}
)MSL";

  NSError* error = nil;
  id<MTLLibrary> library = [context->device
      newLibraryWithSource:[NSString stringWithUTF8String:kMultisampleSelectResolveMsl]
                   options:nil
                     error:&error];
  if (!library) {
    if (error_out) {
      *error_out = error ? [[error localizedDescription] UTF8String]
                         : "multisample select resolve compute library failed";
    }
    return false;
  }
  id<MTLFunction> function = [library newFunctionWithName:@"resolve_selected_color_samples"];
  if (function) {
    context->multisample_select_resolve_pipeline_state =
        [context->device newComputePipelineStateWithFunction:function error:&error];
    [function release];
  }
  [library release];
  if (!context->multisample_select_resolve_pipeline_state) {
    if (error_out) {
      *error_out = error ? [[error localizedDescription] UTF8String]
                         : "multisample select resolve compute pipeline failed";
    }
    return false;
  }
  return true;
}

bool EnsureTiledResolvePipelineState(PipelineProbeContext* context, std::string* error_out) {
  if (!context || !context->device) {
    if (error_out) {
      *error_out = "missing probe context or Metal device";
    }
    return false;
  }
  if (context->tiled_resolve_pipeline_state) {
    return true;
  }
  static constexpr char kTiledResolveMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct TiledResolveConstants {
  uint source_row_pitch;
  uint destination_buffer_offset;
  uint destination_pitch;
  uint destination_x;
  uint destination_y;
  uint copy_width;
  uint copy_height;
  uint destination_endian;
};

uint tiled_rgba8_offset(uint x, uint y, uint pitch) {
  constexpr uint bytes_per_pixel_log2 = 2;
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

kernel void resolve_bgra8_to_xenos_tiled(
    device const uchar* source [[buffer(0)]], device uint* destination [[buffer(1)]],
    constant TiledResolveConstants& constants [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]) {
  if (position.x >= constants.copy_width || position.y >= constants.copy_height) {
    return;
  }
  uint source_offset = position.y * constants.source_row_pitch + position.x * 4u;
  uchar4 bgra = uchar4(source[source_offset], source[source_offset + 1u],
                       source[source_offset + 2u], source[source_offset + 3u]);
  uchar4 packed;
  switch (constants.destination_endian) {
    case 1u:  // Endian128::k8in16.
      packed = bgra.yzwx;
      break;
    case 2u:  // Endian128::k8in32.
      packed = bgra.wxyz;
      break;
    case 3u:  // Endian128::k16in32.
      packed = bgra.xwzy;
      break;
    default:  // Endian128::kNone: raw BGRA becomes guest RGBA.
      packed = bgra.zyxw;
      break;
  }
  uint tiled_offset = tiled_rgba8_offset(constants.destination_x + position.x,
                                         constants.destination_y + position.y,
                                         constants.destination_pitch);
  uint byte_offset = constants.destination_buffer_offset + tiled_offset;
  destination[byte_offset >> 2u] = uint(packed.x) | (uint(packed.y) << 8u) |
                                   (uint(packed.z) << 16u) | (uint(packed.w) << 24u);
}
)MSL";

  NSError* error = nil;
  id<MTLLibrary> library =
      [context->device newLibraryWithSource:[NSString stringWithUTF8String:kTiledResolveMsl]
                                    options:nil
                                      error:&error];
  if (!library) {
    if (error_out) {
      *error_out = error ? [[error localizedDescription] UTF8String]
                         : "tiled resolve compute library failed";
    }
    return false;
  }
  id<MTLFunction> function = [library newFunctionWithName:@"resolve_bgra8_to_xenos_tiled"];
  if (function) {
    context->tiled_resolve_pipeline_state =
        [context->device newComputePipelineStateWithFunction:function error:&error];
    [function release];
  }
  [library release];
  if (!context->tiled_resolve_pipeline_state) {
    if (error_out) {
      *error_out = error ? [[error localizedDescription] UTF8String]
                         : "tiled resolve compute pipeline failed";
    }
    return false;
  }
  return true;
}

bool EnsureDepthTiledResolvePipelineStates(PipelineProbeContext* context, std::string* error_out) {
  if (!context || !context->device) {
    if (error_out) {
      *error_out = "missing probe context or Metal device";
    }
    return false;
  }
  if (context->depth_tiled_resolve_pipeline_state &&
      context->multisample_depth_tiled_resolve_pipeline_state &&
      context->depth_resolve_dummy_snapshot_texture &&
      context->depth_resolve_dummy_packed_snapshot_texture) {
    return true;
  }
  static constexpr char kDepthTiledResolveMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct DepthTiledResolveConstants {
  uint source_x;
  uint source_y;
  uint destination_buffer_offset;
  uint destination_pitch;
  uint destination_x;
  uint destination_y;
  uint copy_width;
  uint copy_height;
  uint destination_endian;
  uint host_sample;
  uint depth_float24;
  uint depth_float24_round;
  uint snapshot_enabled;
  uint snapshot_destination_x;
  uint snapshot_destination_y;
  uint packed_snapshot_enabled;
  uint packed_snapshot_destination_x;
  uint packed_snapshot_destination_y;
  uint packed_snapshot_fetch_endian;
};

uint tiled_depth32_offset(uint x, uint y, uint pitch) {
  constexpr uint bytes_per_pixel_log2 = 2;
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

uint float32_to_20e4(float value, bool round_to_nearest_even) {
  if (!(value > 0.0f)) {
    return 0u;
  }
  uint bits = as_type<uint>(value);
  if (bits >= 0x3FFFFFF8u) {
    return 0xFFFFFFu;
  }
  if (bits < 0x38800000u) {
    uint shift = min(113u - (bits >> 23u), 24u);
    bits = (0x800000u | (bits & 0x7FFFFFu)) >> shift;
  } else {
    bits += 0xC8000000u;
  }
  if (round_to_nearest_even) {
    bits += 3u + ((bits >> 3u) & 1u);
  }
  return (bits >> 3u) & 0xFFFFFFu;
}

uint apply_depth_endian(uint value, uint endian) {
  if (endian == 1u || endian == 2u) {
    value = ((value & 0x00FF00FFu) << 8u) | ((value & 0xFF00FF00u) >> 8u);
  }
  if (endian == 2u || endian == 3u) {
    value = (value << 16u) | (value >> 16u);
  }
  return value;
}

float float20e4_to_32(uint value) {
  value &= 0xFFFFFFu;
  if (value == 0u) {
    return 0.0f;
  }
  uint mantissa = value & 0xFFFFFu;
  uint exponent = value >> 20u;
  if (exponent == 0u) {
    uint mantissa_lzcnt = clz(mantissa) - 11u;
    exponent = uint(1 - int(mantissa_lzcnt));
    mantissa = (mantissa << mantissa_lzcnt) & 0xFFFFFu;
  }
  return as_type<float>(((exponent + 112u) << 23u) | (mantissa << 3u));
}

uint quantize_depth(float host_depth,
                    constant DepthTiledResolveConstants& constants) {
  uint depth24;
  if (constants.depth_float24 != 0u) {
    // Float24 host depth is stored in 0...0.5 so host depth comparison remains
    // monotonic. Restore the guest sampling range before packing.
    depth24 = float32_to_20e4(host_depth * 2.0f,
                             constants.depth_float24_round != 0u);
  } else {
    depth24 = uint(rint(saturate(host_depth) * 16777215.0f));
  }
  return depth24;
}

float decode_sampled_depth(uint depth24,
                           constant DepthTiledResolveConstants& constants) {
  if (constants.depth_float24 != 0u) {
    return float20e4_to_32(depth24);
  }
  return float(depth24 + (depth24 >> 23u)) * (1.0f / 16777216.0f);
}

uint pack_depth_stencil(uint depth24, uint stencil,
                        constant DepthTiledResolveConstants& constants) {
  return apply_depth_endian((depth24 << 8u) | (stencil & 0xFFu),
                            constants.destination_endian);
}

float4 unpack_packed_depth_stencil_rgba(
    uint packed, constant DepthTiledResolveConstants& constants) {
  uint fetched = apply_depth_endian(
      packed, constants.packed_snapshot_fetch_endian);
  return float4(float(fetched & 0xFFu),
                float((fetched >> 8u) & 0xFFu),
                float((fetched >> 16u) & 0xFFu),
                float((fetched >> 24u) & 0xFFu)) * (1.0f / 255.0f);
}

kernel void resolve_depth_to_xenos_tiled(
    depth2d<float, access::read> depth [[texture(0)]],
    texture2d<uint, access::read> stencil [[texture(1)]],
    texture2d_array<float, access::write> snapshot [[texture(2)]],
    texture2d_array<float, access::write> packed_snapshot [[texture(3)]],
    device uint* destination [[buffer(0)]],
    constant DepthTiledResolveConstants& constants [[buffer(1)]],
    uint2 position [[thread_position_in_grid]]) {
  if (position.x >= constants.copy_width ||
      position.y >= constants.copy_height) {
    return;
  }
  uint2 source = uint2(constants.source_x, constants.source_y) + position;
  uint depth24 = quantize_depth(depth.read(source), constants);
  uint packed = pack_depth_stencil(depth24, stencil.read(source).x, constants);
  uint tiled_offset = tiled_depth32_offset(constants.destination_x + position.x,
                                           constants.destination_y + position.y,
                                           constants.destination_pitch);
  destination[(constants.destination_buffer_offset + tiled_offset) >> 2u] = packed;
  if (constants.snapshot_enabled != 0u) {
    uint2 snapshot_destination =
        uint2(constants.snapshot_destination_x,
              constants.snapshot_destination_y) + position;
    snapshot.write(decode_sampled_depth(depth24, constants),
                   snapshot_destination, 0u);
  }
  if (constants.packed_snapshot_enabled != 0u) {
    uint2 packed_snapshot_destination =
        uint2(constants.packed_snapshot_destination_x,
              constants.packed_snapshot_destination_y) + position;
    packed_snapshot.write(
        unpack_packed_depth_stencil_rgba(packed, constants),
        packed_snapshot_destination, 0u);
  }
}

kernel void resolve_multisample_depth_to_xenos_tiled(
    depth2d_ms<float, access::read> depth [[texture(0)]],
    texture2d_ms<uint, access::read> stencil [[texture(1)]],
    texture2d_array<float, access::write> snapshot [[texture(2)]],
    texture2d_array<float, access::write> packed_snapshot [[texture(3)]],
    device uint* destination [[buffer(0)]],
    constant DepthTiledResolveConstants& constants [[buffer(1)]],
    uint2 position [[thread_position_in_grid]]) {
  if (position.x >= constants.copy_width ||
      position.y >= constants.copy_height) {
    return;
  }
  uint2 source = uint2(constants.source_x, constants.source_y) + position;
  uint depth24 =
      quantize_depth(depth.read(source, constants.host_sample), constants);
  uint packed = pack_depth_stencil(
      depth24, stencil.read(source, constants.host_sample).x, constants);
  uint tiled_offset = tiled_depth32_offset(constants.destination_x + position.x,
                                           constants.destination_y + position.y,
                                           constants.destination_pitch);
  destination[(constants.destination_buffer_offset + tiled_offset) >> 2u] = packed;
  if (constants.snapshot_enabled != 0u) {
    uint2 snapshot_destination =
        uint2(constants.snapshot_destination_x,
              constants.snapshot_destination_y) + position;
    snapshot.write(decode_sampled_depth(depth24, constants),
                   snapshot_destination, 0u);
  }
  if (constants.packed_snapshot_enabled != 0u) {
    uint2 packed_snapshot_destination =
        uint2(constants.packed_snapshot_destination_x,
              constants.packed_snapshot_destination_y) + position;
    packed_snapshot.write(
        unpack_packed_depth_stencil_rgba(packed, constants),
        packed_snapshot_destination, 0u);
  }
}
)MSL";

  NSError* error = nil;
  id<MTLLibrary> library =
      [context->device newLibraryWithSource:[NSString stringWithUTF8String:kDepthTiledResolveMsl]
                                    options:nil
                                      error:&error];
  if (!library) {
    if (error_out) {
      *error_out = error ? [[error localizedDescription] UTF8String]
                         : "depth tiled resolve compute library failed";
    }
    return false;
  }
  id<MTLComputePipelineState> single_pipeline = nil;
  id<MTLComputePipelineState> multisample_pipeline = nil;
  id<MTLFunction> single_function = [library newFunctionWithName:@"resolve_depth_to_xenos_tiled"];
  if (single_function) {
    single_pipeline = [context->device newComputePipelineStateWithFunction:single_function
                                                                     error:&error];
    [single_function release];
  }
  id<MTLFunction> multisample_function =
      [library newFunctionWithName:@"resolve_multisample_depth_to_xenos_tiled"];
  if (multisample_function) {
    multisample_pipeline = [context->device newComputePipelineStateWithFunction:multisample_function
                                                                          error:&error];
    [multisample_function release];
  }
  [library release];
  if (!single_pipeline || !multisample_pipeline) {
    if (single_pipeline) {
      [single_pipeline release];
    }
    if (multisample_pipeline) {
      [multisample_pipeline release];
    }
    if (error_out) {
      *error_out = error ? [[error localizedDescription] UTF8String]
                         : "depth tiled resolve compute pipeline failed";
    }
    return false;
  }

  MTLTextureDescriptor* dummy_descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                                         width:1
                                                        height:1
                                                     mipmapped:NO];
  dummy_descriptor.textureType = MTLTextureType2DArray;
  dummy_descriptor.arrayLength = 1;
  dummy_descriptor.storageMode = MTLStorageModePrivate;
  dummy_descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  id<MTLTexture> dummy_snapshot = [context->device newTextureWithDescriptor:dummy_descriptor];
  if (!dummy_snapshot) {
    [single_pipeline release];
    [multisample_pipeline release];
    if (error_out) {
      *error_out = "failed to create depth resolve fallback snapshot texture";
    }
    return false;
  }
  dummy_descriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
  id<MTLTexture> dummy_packed_snapshot =
      [context->device newTextureWithDescriptor:dummy_descriptor];
  if (!dummy_packed_snapshot) {
    [dummy_snapshot release];
    [single_pipeline release];
    [multisample_pipeline release];
    if (error_out) {
      *error_out =
          "failed to create packed depth resolve fallback snapshot texture";
    }
    return false;
  }
  context->depth_tiled_resolve_pipeline_state = single_pipeline;
  context->multisample_depth_tiled_resolve_pipeline_state = multisample_pipeline;
  context->depth_resolve_dummy_snapshot_texture = dummy_snapshot;
  context->depth_resolve_dummy_packed_snapshot_texture =
      dummy_packed_snapshot;
  return true;
}

id<MTLBuffer> EnsureProbeReadbackBuffer(PipelineProbeContext* context, uint32_t full_width,
                                        uint32_t full_height, size_t row_pitch,
                                        uint32_t read_height, std::string* error_out) {
  size_t readback_size = row_pitch * read_height;
  if (context->private_readback_capacity < readback_size) {
    size_t full_row_pitch = (size_t(full_width) * 4 + 255) & ~size_t(255);
    size_t allocation_size = std::max(readback_size, full_row_pitch * full_height);
    id<MTLBuffer> larger_readback_buffer =
        [context->device newBufferWithLength:allocation_size options:MTLResourceStorageModeShared];
    if (larger_readback_buffer) {
      if (context->private_readback_buffer) {
        [context->private_readback_buffer release];
      }
      context->private_readback_buffer = larger_readback_buffer;
      context->private_readback_capacity = allocation_size;
    }
  }
  id<MTLBuffer> readback_buffer =
      context->private_readback_capacity >= readback_size ? context->private_readback_buffer : nil;
  if (!readback_buffer && error_out) {
    *error_out = "failed to create persistent render target staging buffer";
  }
  return readback_buffer;
}

bool EnsureDummyProbeResources(PipelineProbeContext* context, std::string* error_out) {
  if (context->dummy_texture && context->dummy_sampler) {
    return true;
  }
  if (!context->dummy_texture) {
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:1
                                                          height:1
                                                       mipmapped:NO];
    descriptor.textureType = MTLTextureType2DArray;
    descriptor.arrayLength = 1;
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    context->dummy_texture = [context->device newTextureWithDescriptor:descriptor];
    uint32_t zero = 0;
    if (context->dummy_texture) {
      MTLRegion region = MTLRegionMake3D(0, 0, 0, 1, 1, 1);
      [context->dummy_texture replaceRegion:region
                                mipmapLevel:0
                                      slice:0
                                  withBytes:&zero
                                bytesPerRow:4
                              bytesPerImage:4];
    }
  }
  if (!context->dummy_sampler) {
    MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
    descriptor.minFilter = MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    context->dummy_sampler = [context->device newSamplerStateWithDescriptor:descriptor];
    [descriptor release];
  }
  if (context->dummy_texture && context->dummy_sampler) {
    return true;
  }
  if (error_out) {
    *error_out = "failed to create persistent probe fallback texture or sampler";
  }
  return false;
}

void CreateCachedProbeSamplers(PipelineProbeContext* context, const ProbeSamplerSlot* slots,
                               size_t slot_count, id<MTLSamplerState> fallback_sampler,
                               std::vector<id<MTLSamplerState>>& samplers_out) {
  samplers_out.clear();
  samplers_out.reserve(slot_count);
  for (size_t i = 0; i < slot_count; ++i) {
    id<MTLSamplerState> sampler = nil;
    if (slots) {
      uint64_t key = GetProbeSamplerKey(slots[i]);
      auto existing = context->sampler_cache.find(key);
      if (existing != context->sampler_cache.end()) {
        sampler = existing->second;
      } else {
        sampler = CreateProbeSampler(context->device, slots[i]);
        if (sampler) {
          context->sampler_cache.emplace(key, sampler);
        }
      }
    }
    samplers_out.push_back(sampler ? sampler : fallback_sampler);
  }
}

bool GetCachedProbeDepthStencilState(PipelineProbeContext* context,
                                     const ProbeDepthStencilState* state,
                                     id<MTLDepthStencilState>& state_out, std::string* error_out) {
  state_out = nil;
  ProbeDepthStencilState disabled_state;
  const ProbeDepthStencilState& effective_state = state ? *state : disabled_state;
  uint64_t key = GetProbeDepthStencilKey(effective_state);
  auto existing = context->depth_stencil_state_cache.find(key);
  if (existing != context->depth_stencil_state_cache.end()) {
    state_out = existing->second;
    return true;
  }
  id<MTLDepthStencilState> created = CreateProbeDepthStencilState(context->device, effective_state);
  if (!created) {
    if (error_out) {
      *error_out = "failed to create persistent probe depth/stencil state";
    }
    return false;
  }
  context->depth_stencil_state_cache.emplace(key, created);
  state_out = created;
  return true;
}

void ResetOpenProbeBindingTracking(PipelineProbeContext* context) {
  context->tracked_vertex_buffer_mask = 0;
  context->tracked_fragment_buffer_mask = 0;
  context->tracked_vertex_texture_count = 0;
  context->tracked_fragment_texture_count = 0;
  context->tracked_vertex_sampler_count = 0;
  context->tracked_fragment_sampler_count = 0;
}

uint32_t GetPendingProbeSubmissionCount(const PipelineProbeContext* context) {
  if (!context) {
    return 0;
  }
  uint64_t submission_count = context->open_draw_submission_count;
  for (const CommittedProbeCommandBuffer& committed : context->committed_command_buffers) {
    submission_count +=
        uint64_t(committed.draw_submission_count) + committed.auxiliary_submission_count;
  }
  return uint32_t(std::min<uint64_t>(submission_count, UINT32_MAX));
}

uint32_t GetCommittedProbeDrawCommandBufferCount(const PipelineProbeContext* context) {
  if (!context) {
    return 0;
  }
  uint32_t command_buffer_count = 0;
  for (const CommittedProbeCommandBuffer& committed : context->committed_command_buffers) {
    command_buffer_count += committed.draw_submission_count != 0;
  }
  return command_buffer_count;
}

bool FinalizeOpenPipelineProbeCommandBuffer(PipelineProbeContext* context, std::string* error_out) {
  if (!context->open_command_buffer && !context->open_render_encoder) {
    ReleaseOpenProbeDepthStencilOwnership(context);
    return true;
  }
  if (!context->open_command_buffer || !context->open_render_encoder ||
      !context->open_draw_submission_count ||
      context->open_upload_arena_index >= kMaxCommittedProbeCommandBuffers) {
    if (context->open_render_encoder) {
      [context->open_render_encoder endEncoding];
      [context->open_render_encoder release];
      context->open_render_encoder = nil;
    }
    if (context->open_command_buffer) {
      [context->open_command_buffer release];
      context->open_command_buffer = nil;
    }
    ReleaseProbeUploadArena(context, context->open_upload_arena_index);
    context->open_upload_arena_index = kInvalidProbeUploadArena;
    context->open_draw_submission_count = 0;
    ReleaseOpenProbeDepthStencilOwnership(context);
    ResetOpenProbeBindingTracking(context);
    InvalidateProbeContextTargets(context);
    if (error_out) {
      *error_out = "inconsistent open probe command buffer state";
    }
    return false;
  }

  id<MTLRenderCommandEncoder> encoder = context->open_render_encoder;
  [encoder endEncoding];
  context->open_render_encoder = nil;
  ReleaseOpenProbeDepthStencilOwnership(context);
  CommittedProbeCommandBuffer committed;
  committed.command_buffer = context->open_command_buffer;
  committed.draw_submission_count = context->open_draw_submission_count;
  committed.upload_arena_index = context->open_upload_arena_index;
  context->open_command_buffer = nil;
  context->open_draw_submission_count = 0;
  context->open_upload_arena_index = kInvalidProbeUploadArena;
  ResetOpenProbeBindingTracking(context);
  context->committed_command_buffers.push_back(committed);
  [committed.command_buffer commit];
  [encoder release];
  return true;
}

bool FinalizeProbeColorForConsumer(PipelineProbeContext* context, std::string* error_out) {
  if (!context) {
    if (error_out) {
      *error_out = "missing probe context";
    }
    return false;
  }
  if (!FinalizeOpenPipelineProbeCommandBuffer(context, error_out)) {
    return false;
  }
  if (context->sample_count == 1 || !context->color_resolve_dirty) {
    return true;
  }
  if (!context->initialized || !context->multisample_render_texture || !context->render_texture) {
    InvalidateProbeContextTargets(context);
    if (error_out) {
      *error_out = "multisample probe color target is unavailable for resolve";
    }
    return false;
  }

  id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
  if (!command_buffer) {
    if (error_out) {
      *error_out = "failed to create multisample color resolve command buffer";
    }
    return false;
  }
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  MTLRenderPassColorAttachmentDescriptor* color = pass.colorAttachments[0];
  color.texture = context->multisample_render_texture;
  color.loadAction = MTLLoadActionLoad;
  color.resolveTexture = context->render_texture;
  // Preserve the multisample attachment because later render passes continue
  // it with Load until another consumer needs an updated single-sample image.
  color.storeAction = MTLStoreActionStoreAndMultisampleResolve;
  id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
  if (!encoder) {
    if (error_out) {
      *error_out = "failed to create multisample color resolve encoder";
    }
    return false;
  }
  [encoder endEncoding];

  CommittedProbeCommandBuffer committed;
  committed.command_buffer = [command_buffer retain];
  committed.auxiliary_submission_count = 1;
  context->committed_command_buffers.push_back(committed);
  [command_buffer commit];
  context->color_resolve_dirty = false;
  ++context->multisample_resolve_count;
  return true;
}

bool ConsumeCompletedPipelineProbeCommands(PipelineProbeContext* context, std::string* error_out) {
  std::string first_error;
  for (const CommittedProbeCommandBuffer& committed : context->committed_command_buffers) {
    id<MTLCommandBuffer> command_buffer = committed.command_buffer;
    if ([command_buffer status] != MTLCommandBufferStatusCompleted && first_error.empty()) {
      NSError* command_error = [command_buffer error];
      const char* description =
          command_error ? [[command_error localizedDescription] UTF8String] : nullptr;
      first_error = description ? description : "asynchronous probe command buffer failed";
    }
    if ([command_buffer status] != MTLCommandBufferStatusCompleted) {
      NotifyProbeCommandFailure(committed);
    }
    [command_buffer release];
    ReleaseProbeUploadArena(context, committed.upload_arena_index);
  }
  context->committed_command_buffers.clear();
  if (first_error.empty()) {
    return true;
  }
  InvalidateProbeContextTargets(context);
  if (error_out) {
    *error_out = std::move(first_error);
  }
  return false;
}

bool ConsumeOldestPipelineProbeCommand(PipelineProbeContext* context, std::string* error_out) {
  if (!context || context->committed_command_buffers.empty()) {
    return true;
  }

  CommittedProbeCommandBuffer committed = context->committed_command_buffers.front();
  context->committed_command_buffers.erase(context->committed_command_buffers.begin());
  id<MTLCommandBuffer> command_buffer = committed.command_buffer;
  bool succeeded = [command_buffer status] == MTLCommandBufferStatusCompleted;
  if (!succeeded) {
    NSError* command_error = [command_buffer error];
    const char* description =
        command_error ? [[command_error localizedDescription] UTF8String] : nullptr;
    InvalidateProbeContextTargets(context);
    if (error_out) {
      *error_out = description ? description : "asynchronous probe command buffer failed";
    }
    NotifyProbeCommandFailure(committed);
  }
  [command_buffer release];
  ReleaseProbeUploadArena(context, committed.upload_arena_index);
  return succeeded;
}

bool WaitOldestPipelineProbeCommand(PipelineProbeContext* context, std::string* error_out) {
  if (!context) {
    if (error_out) {
      *error_out = "missing probe context";
    }
    return false;
  }
  if ((context->open_command_buffer || context->open_render_encoder) &&
      !FinalizeOpenPipelineProbeCommandBuffer(context, error_out)) {
    return false;
  }
  if (context->committed_command_buffers.empty()) {
    return true;
  }
  [context->committed_command_buffers.front().command_buffer waitUntilCompleted];
  return ConsumeOldestPipelineProbeCommand(context, error_out);
}

bool WaitPendingPipelineProbeCommands(PipelineProbeContext* context, std::string* error_out,
                                      uint32_t* waited_submission_count_out) {
  if (waited_submission_count_out) {
    *waited_submission_count_out = 0;
  }
  if (!context) {
    if (error_out) {
      *error_out = "missing probe context";
    }
    return false;
  }
  uint32_t pending_submission_count = GetPendingProbeSubmissionCount(context);
  if (!pending_submission_count) {
    DiscardEmptyOpenPipelineProbeCommandBuffer(context);
    return true;
  }
  if (waited_submission_count_out) {
    *waited_submission_count_out = pending_submission_count;
  }
  if (!FinalizeOpenPipelineProbeCommandBuffer(context, error_out)) {
    return false;
  }
  [context->committed_command_buffers.back().command_buffer waitUntilCompleted];
  return ConsumeCompletedPipelineProbeCommands(context, error_out);
}

bool PrepareProbeDepthStencilSubmission(PipelineProbeContext* context, bool acquire_open_ownership,
                                        std::string* error_out) {
  if (!context || !context->depth_stencil_target) {
    if (error_out) {
      *error_out = "missing persistent probe depth/stencil target";
    }
    return false;
  }
  ProbeDepthStencilTarget* target = context->depth_stencil_target;
  PipelineProbeContext* previous_owner = target->open_owner;
  if (previous_owner && previous_owner != context) {
    std::string finalize_error;
    if (!FinalizeOpenPipelineProbeCommandBuffer(previous_owner, &finalize_error)) {
      if (error_out) {
        *error_out =
            finalize_error.empty()
                ? "failed to finalize the previous shared depth/stencil owner"
                : "failed to finalize the previous shared depth/stencil owner: " + finalize_error;
      }
      return false;
    }
  }
  if (target->open_owner && target->open_owner != context) {
    if (error_out) {
      *error_out = "shared depth/stencil target retained an inconsistent open owner";
    }
    return false;
  }
  target->open_owner = acquire_open_ownership ? context : nullptr;
  return true;
}

bool EnsureProbeDepthStencilTexture(PipelineProbeContext* context, uint32_t width, uint32_t height,
                                    std::string* error_out) {
  if (!context || !context->depth_stencil_target || !width || !height) {
    if (error_out) {
      *error_out = "missing persistent probe depth/stencil target or size";
    }
    return false;
  }
  ProbeDepthStencilTarget* target = context->depth_stencil_target;
  if (target->device != context->device || target->command_queue != context->command_queue) {
    if (error_out) {
      *error_out = "persistent probe depth/stencil target belongs to a different device or queue";
    }
    return false;
  }
  if (target->texture && target->width == width && target->height == height) {
    return true;
  }
  if (target->width && target->height && (target->width != width || target->height != height) &&
      (target->extent_locked || target->attached_contexts.size() > 1)) {
    if (error_out) {
      *error_out = "shared depth/stencil target dimensions are locked to another color target";
    }
    return false;
  }

  // A resize replaces the texture shared by every attached color context.
  // Drain all of them first so no encoder can retain the old texture while
  // another context starts using its replacement.
  for (PipelineProbeContext* attached_context : target->attached_contexts) {
    if (!WaitPendingPipelineProbeCommands(attached_context, error_out, nullptr)) {
      return false;
    }
  }
  MTLTextureDescriptor* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  if (target->sample_count > 1) {
    descriptor.textureType = MTLTextureType2DMultisample;
    descriptor.sampleCount = target->sample_count;
  }
  // Compute depth resolves read the combined texture directly and create an
  // X32_Stencil8 view for stencil. PixelFormatView is required by Metal for
  // that view even though depth remains bound through the original texture.
  descriptor.usage =
      MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
  descriptor.storageMode = MTLStorageModePrivate;
  id<MTLTexture> replacement_texture = [target->device newTextureWithDescriptor:descriptor];
  if (!replacement_texture) {
    if (error_out) {
      *error_out = "failed to create persistent probe depth/stencil texture";
    }
    return false;
  }
  target->open_owner = nullptr;
  if (target->texture) {
    [target->texture release];
  }
  target->texture = replacement_texture;
  target->width = width;
  target->height = height;
  target->initialized = false;
  target->extent_locked = target->attached_contexts.size() > 1;
  return true;
}

bool EnsureOpenPipelineProbeEncoder(PipelineProbeContext* context, std::string* error_out) {
  if (context->open_command_buffer && context->open_render_encoder) {
    if (context->depth_stencil_target && context->depth_stencil_target->open_owner == context) {
      return true;
    }
    if (error_out) {
      *error_out = "persistent probe encoder lost shared depth/stencil ownership";
    }
    return false;
  }
  if (context->open_command_buffer || context->open_render_encoder) {
    FinalizeOpenPipelineProbeCommandBuffer(context, error_out);
    return false;
  }

  uint32_t upload_arena_index = AcquireProbeUploadArena(context);
  while (upload_arena_index == kInvalidProbeUploadArena &&
         !context->committed_command_buffers.empty()) {
    // Keep three command buffers in flight while recycling only the oldest
    // draw arena. Auxiliary resolve buffers don't own arenas, so consume
    // completed queue entries until an actual draw arena becomes reusable.
    if (!WaitOldestPipelineProbeCommand(context, error_out)) {
      return false;
    }
    upload_arena_index = AcquireProbeUploadArena(context);
  }
  if (upload_arena_index == kInvalidProbeUploadArena) {
    ReleaseOpenProbeDepthStencilOwnership(context);
    if (error_out) {
      *error_out = "no reusable persistent probe upload arena is available";
    }
    return false;
  }

  if (!PrepareProbeDepthStencilSubmission(context, true, error_out)) {
    ReleaseProbeUploadArena(context, upload_arena_index);
    return false;
  }

  id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
  if (!command_buffer) {
    ReleaseProbeUploadArena(context, upload_arena_index);
    ReleaseOpenProbeDepthStencilOwnership(context);
    if (error_out) {
      *error_out = "failed to create persistent probe command buffer";
    }
    return false;
  }
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  ConfigureProbeColorPass(pass, context,
                          context->initialized ? MTLLoadActionLoad : MTLLoadActionClear,
                          MTLClearColorMake(0.0, 0.0, 0.0, 1.0));
  ProbeDepthStencilTarget* depth_stencil_target = context->depth_stencil_target;
  ConfigureProbeDepthStencilPass(
      pass, depth_stencil_target->texture,
      depth_stencil_target->initialized ? MTLLoadActionLoad : MTLLoadActionClear);
  id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
  if (!encoder) {
    ReleaseProbeUploadArena(context, upload_arena_index);
    ReleaseOpenProbeDepthStencilOwnership(context);
    if (error_out) {
      *error_out = "failed to create persistent probe command encoder";
    }
    return false;
  }

  context->open_command_buffer = [command_buffer retain];
  context->open_render_encoder = [encoder retain];
  context->open_draw_submission_count = 0;
  context->open_upload_arena_index = upload_arena_index;
  ResetOpenProbeBindingTracking(context);
  return true;
}

void ClearTrackedOpenProbeBindings(PipelineProbeContext* context) {
  id<MTLRenderCommandEncoder> encoder = context->open_render_encoder;
  static id<MTLBuffer> const kNilBuffers[32] = {};
  static NSUInteger const kZeroBufferOffsets[32] = {};
  static id<MTLTexture> const kNilTextures[128] = {};
  static id<MTLSamplerState> const kNilSamplers[16] = {};
  auto buffer_range_count = [](uint32_t mask) {
    NSUInteger count = 0;
    while (mask) {
      ++count;
      mask >>= 1;
    }
    return count;
  };
  NSUInteger vertex_buffer_count = buffer_range_count(context->tracked_vertex_buffer_mask);
  if (vertex_buffer_count) {
    [encoder setVertexBuffers:kNilBuffers
                      offsets:kZeroBufferOffsets
                    withRange:NSMakeRange(0, vertex_buffer_count)];
  }
  NSUInteger fragment_buffer_count = buffer_range_count(context->tracked_fragment_buffer_mask);
  if (fragment_buffer_count) {
    [encoder setFragmentBuffers:kNilBuffers
                        offsets:kZeroBufferOffsets
                      withRange:NSMakeRange(0, fragment_buffer_count)];
  }
  if (context->tracked_vertex_texture_count) {
    if (context->tracked_vertex_texture_count <= 128) {
      [encoder setVertexTextures:kNilTextures
                       withRange:NSMakeRange(0, context->tracked_vertex_texture_count)];
    } else {
      for (NSUInteger index = 0; index < context->tracked_vertex_texture_count; ++index) {
        [encoder setVertexTexture:nil atIndex:index];
      }
    }
  }
  if (context->tracked_fragment_texture_count) {
    if (context->tracked_fragment_texture_count <= 128) {
      [encoder setFragmentTextures:kNilTextures
                         withRange:NSMakeRange(0, context->tracked_fragment_texture_count)];
    } else {
      for (NSUInteger index = 0; index < context->tracked_fragment_texture_count; ++index) {
        [encoder setFragmentTexture:nil atIndex:index];
      }
    }
  }
  if (context->tracked_vertex_sampler_count) {
    if (context->tracked_vertex_sampler_count <= 16) {
      [encoder setVertexSamplerStates:kNilSamplers
                            withRange:NSMakeRange(0, context->tracked_vertex_sampler_count)];
    } else {
      for (NSUInteger index = 0; index < context->tracked_vertex_sampler_count; ++index) {
        [encoder setVertexSamplerState:nil atIndex:index];
      }
    }
  }
  if (context->tracked_fragment_sampler_count) {
    if (context->tracked_fragment_sampler_count <= 16) {
      [encoder setFragmentSamplerStates:kNilSamplers
                              withRange:NSMakeRange(0, context->tracked_fragment_sampler_count)];
    } else {
      for (NSUInteger index = 0; index < context->tracked_fragment_sampler_count; ++index) {
        [encoder setFragmentSamplerState:nil atIndex:index];
      }
    }
  }
  ResetOpenProbeBindingTracking(context);
}

void TrackOpenProbeBufferBinding(PipelineProbeContext* context, bool vertex_stage, uint32_t index) {
  if (index >= 32) {
    return;
  }
  uint32_t& mask =
      vertex_stage ? context->tracked_vertex_buffer_mask : context->tracked_fragment_buffer_mask;
  mask |= uint32_t(1) << index;
}

bool EnsureProbeContextTexture(PipelineProbeContext* context, uint32_t width, uint32_t height,
                               std::string* error_out) {
  if (!context || !context->device || !width || !height) {
    if (error_out) {
      *error_out = "missing probe context, Metal device, or target size";
    }
    return false;
  }
  if (!EnsureProbeDepthStencilTexture(context, width, height, error_out)) {
    return false;
  }
  if (context->render_texture &&
      (context->sample_count == 1 || context->multisample_render_texture) &&
      context->width == width && context->height == height) {
    return true;
  }
  if (!WaitPendingPipelineProbeCommands(context, error_out, nullptr)) {
    return false;
  }
  if (context->render_texture) {
    [context->render_texture release];
    context->render_texture = nil;
  }
  if (context->multisample_render_texture) {
    [context->multisample_render_texture release];
    context->multisample_render_texture = nil;
  }
  MTLTextureDescriptor* texture_descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  texture_descriptor.usage =
      MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  texture_descriptor.storageMode = context->storage_mode;
  context->render_texture = [context->device newTextureWithDescriptor:texture_descriptor];
  if (context->render_texture && context->sample_count > 1) {
    MTLTextureDescriptor* multisample_descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    multisample_descriptor.textureType = MTLTextureType2DMultisample;
    multisample_descriptor.sampleCount = context->sample_count;
    multisample_descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    multisample_descriptor.storageMode = MTLStorageModePrivate;
    context->multisample_render_texture =
        [context->device newTextureWithDescriptor:multisample_descriptor];
  }
  if (!context->render_texture ||
      (context->sample_count > 1 && !context->multisample_render_texture)) {
    if (context->render_texture) {
      [context->render_texture release];
      context->render_texture = nil;
    }
    if (context->multisample_render_texture) {
      [context->multisample_render_texture release];
      context->multisample_render_texture = nil;
    }
    context->width = 0;
    context->height = 0;
    context->initialized = false;
    if (error_out) {
      *error_out = "failed to create persistent probe color texture";
    }
    return false;
  }
  context->width = width;
  context->height = height;
  context->initialized = false;
  context->color_resolve_dirty = false;
  return true;
}

bool EnsureClearPipelineState(PipelineProbeContext* context, std::string* error_out) {
  if (!context || !context->device) {
    if (error_out) {
      *error_out = "missing probe context or Metal device";
    }
    return false;
  }
  if (context->clear_pipeline_state) {
    return true;
  }
  static constexpr char kClearMsl[] = R"(
#include <metal_stdlib>
using namespace metal;

struct ClearConstants {
  float4 color;
};

vertex float4 rex_clear_vertex(uint vertex_id [[vertex_id]]) {
  constexpr float2 positions[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0),
  };
  return float4(positions[vertex_id], 0.0, 1.0);
}

fragment float4 rex_clear_fragment(constant ClearConstants& constants [[buffer(0)]]) {
  return constants.color;
}
)";
  NSError* error = nil;
  id<MTLLibrary> library =
      [context->device newLibraryWithSource:[NSString stringWithUTF8String:kClearMsl]
                                    options:nil
                                      error:&error];
  if (!library) {
    if (error_out) {
      *error_out = error ? [[error localizedDescription] UTF8String] : "clear library failed";
    }
    return false;
  }
  id<MTLFunction> vertex_function = [library newFunctionWithName:@"rex_clear_vertex"];
  id<MTLFunction> fragment_function = [library newFunctionWithName:@"rex_clear_fragment"];
  if (!vertex_function || !fragment_function) {
    if (error_out) {
      *error_out = "clear shader functions not found";
    }
    if (vertex_function) {
      [vertex_function release];
    }
    if (fragment_function) {
      [fragment_function release];
    }
    [library release];
    return false;
  }
  MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = vertex_function;
  descriptor.fragmentFunction = fragment_function;
  descriptor.rasterSampleCount = context->sample_count;
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
  descriptor.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
  context->clear_pipeline_state = [context->device newRenderPipelineStateWithDescriptor:descriptor
                                                                                  error:&error];
  [descriptor release];
  [vertex_function release];
  [fragment_function release];
  [library release];
  if (!context->clear_pipeline_state) {
    if (error_out) {
      *error_out = error ? [[error localizedDescription] UTF8String] : "clear pipeline failed";
    }
    return false;
  }
  return true;
}

bool EnsureDepthClearPipelineState(PipelineProbeContext* context, std::string* error_out) {
  if (!context || !context->device) {
    if (error_out) {
      *error_out = "missing probe context or Metal device";
    }
    return false;
  }
  if (context->depth_clear_pipeline_state) {
    return true;
  }
  static constexpr char kDepthClearMsl[] = R"(
#include <metal_stdlib>
using namespace metal;

struct DepthClearConstants {
  float depth;
};

vertex float4 rex_depth_clear_vertex(uint vertex_id [[vertex_id]],
                                     constant DepthClearConstants& constants [[buffer(0)]]) {
  constexpr float2 positions[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0),
  };
  return float4(positions[vertex_id], constants.depth, 1.0);
}

fragment float4 rex_depth_clear_fragment() {
  return float4(0.0);
}
)";
  NSError* error = nil;
  id<MTLLibrary> library =
      [context->device newLibraryWithSource:[NSString stringWithUTF8String:kDepthClearMsl]
                                    options:nil
                                      error:&error];
  if (!library) {
    if (error_out) {
      *error_out = error ? [[error localizedDescription] UTF8String] : "depth clear library failed";
    }
    return false;
  }
  id<MTLFunction> vertex_function = [library newFunctionWithName:@"rex_depth_clear_vertex"];
  id<MTLFunction> fragment_function = [library newFunctionWithName:@"rex_depth_clear_fragment"];
  MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = vertex_function;
  descriptor.fragmentFunction = fragment_function;
  descriptor.rasterSampleCount = context->sample_count;
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  descriptor.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
  descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
  descriptor.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
  if (vertex_function && fragment_function) {
    context->depth_clear_pipeline_state =
        [context->device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  }
  [descriptor release];
  if (vertex_function) {
    [vertex_function release];
  }
  if (fragment_function) {
    [fragment_function release];
  }
  [library release];
  if (!context->depth_clear_pipeline_state) {
    if (error_out) {
      *error_out =
          error ? [[error localizedDescription] UTF8String] : "depth clear pipeline failed";
    }
    return false;
  }
  return true;
}

}  // namespace

namespace {

void* CreatePersistentRenderContext(void* metal_device, void* metal_command_queue,
                                    MTLStorageMode storage_mode, const char* label,
                                    std::string* error_out) {
  if (!metal_device) {
    if (error_out) {
      *error_out = "missing Metal device";
    }
    return nullptr;
  }
  auto* context = new PipelineProbeContext();
  context->committed_command_buffers.reserve(kMaxCommittedProbeCommandBuffers);
  context->device = [(id<MTLDevice>)metal_device retain];
  context->storage_mode = storage_mode;
  if (metal_command_queue) {
    id<MTLCommandQueue> command_queue = (id<MTLCommandQueue>)metal_command_queue;
    if ([command_queue device] != context->device) {
      if (error_out) {
        *error_out = std::string("persistent ") + label +
                     " command queue belongs to a different Metal device";
      }
      [context->device release];
      delete context;
      return nullptr;
    }
    context->command_queue = [command_queue retain];
  } else {
    context->command_queue = [context->device newCommandQueue];
  }
  if (!context->command_queue) {
    if (error_out) {
      *error_out = std::string("failed to create persistent ") + label + " command queue";
    }
    [context->device release];
    delete context;
    return nullptr;
  }
  if (!CreateProbeDepthStencilTarget(context)) {
    if (error_out) {
      *error_out = std::string("failed to create persistent ") + label + " depth/stencil target";
    }
    [context->command_queue release];
    [context->device release];
    delete context;
    return nullptr;
  }
  return context;
}

}  // namespace

void* CreatePipelineProbeContext(void* metal_device, std::string* error_out) {
  return CreatePipelineProbeContext(metal_device, nullptr, error_out);
}

void* CreatePipelineProbeContext(void* metal_device, void* metal_command_queue,
                                 std::string* error_out) {
  return CreatePersistentRenderContext(metal_device, metal_command_queue, MTLStorageModeShared,
                                       "probe", error_out);
}

void* CreateHostRenderTargetContext(void* metal_device, std::string* error_out) {
  return CreateHostRenderTargetContext(metal_device, nullptr, error_out);
}

void* CreateHostRenderTargetContext(void* metal_device, void* metal_command_queue,
                                    std::string* error_out) {
  return CreatePersistentRenderContext(metal_device, metal_command_queue, MTLStorageModePrivate,
                                       "host render target", error_out);
}

bool SharePipelineProbeDepthStencilTarget(void* opaque_destination_context,
                                          void* opaque_source_context, std::string* error_out) {
  auto* destination_context = static_cast<PipelineProbeContext*>(opaque_destination_context);
  auto* source_context = static_cast<PipelineProbeContext*>(opaque_source_context);
  if (!destination_context || !source_context || !source_context->depth_stencil_target) {
    if (error_out) {
      *error_out = "missing source or destination persistent probe context";
    }
    return false;
  }
  if (destination_context == source_context ||
      destination_context->depth_stencil_target == source_context->depth_stencil_target) {
    return true;
  }
  if (destination_context->device != source_context->device ||
      destination_context->command_queue != source_context->command_queue) {
    if (error_out) {
      *error_out = "shared depth/stencil contexts must use the same Metal device and command queue";
    }
    return false;
  }
  if (destination_context->sample_count != source_context->sample_count ||
      source_context->depth_stencil_target->sample_count != source_context->sample_count) {
    if (error_out) {
      *error_out = "shared depth/stencil contexts must use the same sample count";
    }
    return false;
  }
  ProbeDepthStencilTarget* source_target = source_context->depth_stencil_target;
  ProbeDepthStencilTarget* destination_target = destination_context->depth_stencil_target;
  uint32_t source_width = source_target->width;
  uint32_t source_height = source_target->height;
  if ((!source_width || !source_height) && source_context->render_texture) {
    source_width = source_context->width;
    source_height = source_context->height;
  }
  uint32_t destination_width = destination_context->render_texture
                                   ? destination_context->width
                                   : (destination_target ? destination_target->width : 0);
  uint32_t destination_height = destination_context->render_texture
                                    ? destination_context->height
                                    : (destination_target ? destination_target->height : 0);
  if (source_width && source_height && destination_width && destination_height &&
      (source_width != destination_width || source_height != destination_height)) {
    if (error_out) {
      *error_out = "shared depth/stencil contexts have incompatible target dimensions";
    }
    return false;
  }
  if (!WaitPendingPipelineProbeCommands(destination_context, error_out, nullptr)) {
    return false;
  }
  if ((!source_width || !source_height) && destination_width && destination_height) {
    source_target->width = destination_width;
    source_target->height = destination_height;
  }
  AttachProbeDepthStencilTarget(destination_context, source_target);
  return true;
}

bool SetPipelineProbeContextSampleCount(void* opaque_context, uint32_t sample_count,
                                        std::string* error_out) {
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    if (!context || !context->device || !context->depth_stencil_target) {
      if (error_out) {
        *error_out = "missing persistent probe context";
      }
      return false;
    }
    if ((sample_count != 1 && sample_count != 2 && sample_count != 4) ||
        ![context->device supportsTextureSampleCount:sample_count]) {
      if (error_out) {
        *error_out = "unsupported persistent probe sample count";
      }
      return false;
    }
    if (context->sample_count == sample_count) {
      return true;
    }
    ProbeDepthStencilTarget* target = context->depth_stencil_target;
    if (target->attached_contexts.size() != 1 || target->attached_contexts.front() != context) {
      if (error_out) {
        *error_out = "cannot change the sample count of an already shared depth/stencil target";
      }
      return false;
    }
    if (!WaitPendingPipelineProbeCommands(context, error_out, nullptr)) {
      return false;
    }
    if (context->render_texture) {
      [context->render_texture release];
      context->render_texture = nil;
    }
    if (context->multisample_render_texture) {
      [context->multisample_render_texture release];
      context->multisample_render_texture = nil;
    }
    if (context->clear_pipeline_state) {
      [context->clear_pipeline_state release];
      context->clear_pipeline_state = nil;
    }
    if (context->depth_clear_pipeline_state) {
      [context->depth_clear_pipeline_state release];
      context->depth_clear_pipeline_state = nil;
    }
    if (target->texture) {
      [target->texture release];
      target->texture = nil;
    }
    context->sample_count = sample_count;
    context->width = 0;
    context->height = 0;
    context->initialized = false;
    context->color_resolve_dirty = false;
    target->sample_count = sample_count;
    target->width = 0;
    target->height = 0;
    target->initialized = false;
    target->extent_locked = false;
    return true;
  }
}

void* CreatePipelineProbeSnapshotTexture(void* metal_device, uint32_t width, uint32_t height,
                                         std::string* error_out) {
  @autoreleasepool {
    id<MTLDevice> device = (id<MTLDevice>)metal_device;
    if (!device || !width || !height) {
      if (error_out) {
        *error_out = "missing Metal device or snapshot dimensions";
      }
      return nullptr;
    }
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    // Guest 2D fetches are translated as texture2d_array so one-layer and
    // stacked textures share a binding type. Keep the resolve snapshot directly
    // bindable by those shaders while presentation continues to use slice 0.
    descriptor.textureType = MTLTextureType2DArray;
    descriptor.arrayLength = 1;
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    if (!texture) {
      if (error_out) {
        *error_out = "failed to allocate the private presentation snapshot texture";
      }
      return nullptr;
    }
    texture.label = @"ReX Metal exact resolved surface snapshot";
    return (void*)texture;
  }
}

void* CreatePipelineProbeDepthSnapshotTexture(void* metal_device, uint32_t width, uint32_t height,
                                              std::string* error_out) {
  @autoreleasepool {
    id<MTLDevice> device = (id<MTLDevice>)metal_device;
    if (!device || !width || !height) {
      if (error_out) {
        *error_out = "missing Metal device or depth snapshot dimensions";
      }
      return nullptr;
    }
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    descriptor.textureType = MTLTextureType2DArray;
    descriptor.arrayLength = 1;
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    if (!texture) {
      if (error_out) {
        *error_out = "failed to allocate the private depth resolve snapshot texture";
      }
      return nullptr;
    }
    texture.label = @"ReX Metal resolved depth snapshot";
    return (void*)texture;
  }
}

void* CreatePipelineProbePackedDepthSnapshotTexture(void* metal_device, uint32_t width,
                                                    uint32_t height,
                                                    std::string* error_out) {
  @autoreleasepool {
    id<MTLDevice> device = (id<MTLDevice>)metal_device;
    if (!device || !width || !height) {
      if (error_out) {
        *error_out = "missing Metal device or packed depth snapshot dimensions";
      }
      return nullptr;
    }
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    descriptor.textureType = MTLTextureType2DArray;
    descriptor.arrayLength = 1;
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    if (!texture) {
      if (error_out) {
        *error_out = "failed to allocate the private packed depth/stencil snapshot texture";
      }
      return nullptr;
    }
    texture.label = @"ReX Metal packed depth/stencil snapshot";
    return (void*)texture;
  }
}

void ReleasePipelineProbeSnapshotTexture(void* snapshot_texture) {
  if (snapshot_texture) {
    [(id<MTLTexture>)snapshot_texture release];
  }
}

bool QueuePipelineProbeSnapshotCopy(void* metal_command_queue, void* source_texture,
                                    void* destination_texture, uint32_t width, uint32_t height,
                                    std::string* error_out) {
  @autoreleasepool {
    id<MTLCommandQueue> command_queue = (id<MTLCommandQueue>)metal_command_queue;
    id<MTLTexture> source = (id<MTLTexture>)source_texture;
    id<MTLTexture> destination = (id<MTLTexture>)destination_texture;
    if (!command_queue || !source || !destination || !width || !height ||
        [source device] != [command_queue device] ||
        [destination device] != [command_queue device] ||
        [source pixelFormat] != MTLPixelFormatBGRA8Unorm ||
        [destination pixelFormat] != MTLPixelFormatBGRA8Unorm || width > [source width] ||
        height > [source height] || width > [destination width] || height > [destination height]) {
      if (error_out) {
        *error_out = "invalid snapshot source, destination, queue, or dimensions";
      }
      return false;
    }
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    id<MTLBlitCommandEncoder> encoder = command_buffer ? [command_buffer blitCommandEncoder] : nil;
    if (!command_buffer || !encoder) {
      if (error_out) {
        *error_out = "failed to create the presentation snapshot copy command";
      }
      return false;
    }
    command_buffer.label = @"ReX Metal presentation snapshot mailbox copy";
    [encoder copyFromTexture:source
                 sourceSlice:0
                 sourceLevel:0
                sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake(width, height, 1)
                   toTexture:destination
            destinationSlice:0
            destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
    [encoder endEncoding];
    [command_buffer commit];
    return true;
  }
}

bool FinalizePipelineProbeContext(void* opaque_context, std::string* error_out) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  if (!context) {
    if (error_out) {
      *error_out = "missing probe context";
    }
    return false;
  }
  return FinalizeOpenPipelineProbeCommandBuffer(context, error_out);
}

bool WaitPipelineProbeContext(void* opaque_context, std::string* error_out,
                              uint32_t* waited_submission_count_out) {
  return WaitPendingPipelineProbeCommands(static_cast<PipelineProbeContext*>(opaque_context),
                                          error_out, waited_submission_count_out);
}

uint32_t GetPipelineProbeContextPendingSubmissionCount(void* opaque_context) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  return GetPendingProbeSubmissionCount(context);
}

bool GetPipelineProbeContextUploadStats(void* opaque_context, PipelineProbeUploadStats* stats_out) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  if (!context || !stats_out) {
    return false;
  }
  *stats_out = context->upload_stats;
  return true;
}

uint64_t GetPipelineProbeContextMultisampleResolveCount(void* opaque_context) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  return context ? context->multisample_resolve_count : 0;
}

void ResetPipelineProbeContext(void* opaque_context) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  if (context) {
    std::string finalize_error;
    if (!FinalizeOpenPipelineProbeCommandBuffer(context, &finalize_error)) {
      std::fprintf(stderr, "[metal] probe context reset failed to finalize: %s\n",
                   finalize_error.c_str());
    } else if (GetCommittedProbeDrawCommandBufferCount(context) >=
               kMaxCommittedProbeCommandBuffers) {
      uint32_t waited_submission_count = 0;
      if (!WaitPendingPipelineProbeCommands(context, &finalize_error, &waited_submission_count)) {
        std::fprintf(stderr, "[metal] probe context reset drained %u submissions with error: %s\n",
                     waited_submission_count, finalize_error.c_str());
      }
    }
    bool depth_stencil_exclusive = context->depth_stencil_target &&
                                   context->depth_stencil_target->attached_contexts.size() == 1;
    InvalidateProbeContextColorTarget(context);
    if (depth_stencil_exclusive) {
      context->depth_stencil_target->initialized = false;
    }
  }
}

void ResetPipelineProbeDepthStencilTarget(void* opaque_context) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  if (!context || !context->depth_stencil_target) {
    return;
  }
  ProbeDepthStencilTarget* target = context->depth_stencil_target;
  std::string wait_error;
  for (PipelineProbeContext* attached_context : target->attached_contexts) {
    uint32_t waited_submission_count = 0;
    if (!WaitPendingPipelineProbeCommands(attached_context, &wait_error,
                                          &waited_submission_count)) {
      std::fprintf(stderr,
                   "[metal] shared depth/stencil reset drained %u failed submission(s): %s\n",
                   waited_submission_count, wait_error.c_str());
      wait_error.clear();
    }
  }
  target->open_owner = nullptr;
  if (target->texture) {
    [target->texture release];
    target->texture = nil;
  }
  target->width = 0;
  target->height = 0;
  target->initialized = false;
  target->extent_locked = target->attached_contexts.size() > 1;
}

void ReleasePipelineProbeContext(void* opaque_context) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  if (!context) {
    return;
  }
  std::string wait_error;
  uint32_t waited_submission_count = 0;
  if (!WaitPendingPipelineProbeCommands(context, &wait_error, &waited_submission_count)) {
    std::fprintf(stderr, "[metal] probe context release drained %u failed submission(s): %s\n",
                 waited_submission_count, wait_error.c_str());
  }
  if (context->clear_pipeline_state) {
    [context->clear_pipeline_state release];
  }
  if (context->depth_clear_pipeline_state) {
    [context->depth_clear_pipeline_state release];
  }
  if (context->multisample_select_resolve_pipeline_state) {
    [context->multisample_select_resolve_pipeline_state release];
  }
  if (context->tiled_resolve_pipeline_state) {
    [context->tiled_resolve_pipeline_state release];
  }
  if (context->depth_tiled_resolve_pipeline_state) {
    [context->depth_tiled_resolve_pipeline_state release];
  }
  if (context->multisample_depth_tiled_resolve_pipeline_state) {
    [context->multisample_depth_tiled_resolve_pipeline_state release];
  }
  if (context->depth_resolve_dummy_snapshot_texture) {
    [context->depth_resolve_dummy_snapshot_texture release];
  }
  if (context->depth_resolve_dummy_packed_snapshot_texture) {
    [context->depth_resolve_dummy_packed_snapshot_texture release];
  }
  if (context->render_texture) {
    [context->render_texture release];
  }
  if (context->multisample_render_texture) {
    [context->multisample_render_texture release];
  }
  DetachProbeDepthStencilTarget(context);
  if (context->private_readback_buffer) {
    [context->private_readback_buffer release];
  }
  if (context->dummy_texture) {
    [context->dummy_texture release];
  }
  if (context->dummy_sampler) {
    [context->dummy_sampler release];
  }
  for (auto& sampler : context->sampler_cache) {
    [sampler.second release];
  }
  context->sampler_cache.clear();
  for (auto& depth_stencil_state : context->depth_stencil_state_cache) {
    [depth_stencil_state.second release];
  }
  context->depth_stencil_state_cache.clear();
  for (ProbeUploadArena& arena : context->upload_arenas) {
    for (ProbeUploadChunk& chunk : arena.chunks) {
      if (chunk.buffer) {
        [chunk.buffer release];
      }
    }
    arena.chunks.clear();
    arena.in_use = false;
  }
  if (context->command_queue) {
    [context->command_queue release];
  }
  if (context->device) {
    [context->device release];
  }
  delete context;
}

bool ClearPipelineProbeContext(void* opaque_context, uint32_t width, uint32_t height, double red,
                               double green, double blue, double alpha, std::string* error_out) {
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    if (!WaitPendingPipelineProbeCommands(context, error_out, nullptr) ||
        !EnsureProbeContextTexture(context, width, height, error_out)) {
      return false;
    }
    if (!PrepareProbeDepthStencilSubmission(context, false, error_out)) {
      return false;
    }

    id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
    if (!command_buffer) {
      if (error_out) {
        *error_out = "failed to create probe clear command buffer";
      }
      return false;
    }
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    ConfigureProbeColorPass(pass, context, MTLLoadActionClear,
                            MTLClearColorMake(red, green, blue, alpha));
    ConfigureProbeDepthStencilPass(pass, context->depth_stencil_target->texture,
                                   MTLLoadActionClear);

    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    if (!encoder) {
      if (error_out) {
        *error_out = "failed to create probe clear command encoder";
      }
      return false;
    }
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];

    bool succeeded = [command_buffer status] == MTLCommandBufferStatusCompleted;
    if (succeeded) {
      context->initialized = true;
      context->color_resolve_dirty = context->sample_count > 1;
      context->depth_stencil_target->initialized = true;
    } else if (error_out) {
      NSError* error = [command_buffer error];
      *error_out = error ? [[error localizedDescription] UTF8String] : "command buffer failed";
    }
    return succeeded;
  }
}

bool ClearPipelineProbeContextRect(void* opaque_context, uint32_t width, uint32_t height,
                                   uint32_t x, uint32_t y, uint32_t clear_width,
                                   uint32_t clear_height, double red, double green, double blue,
                                   double alpha, std::string* error_out) {
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    if (!WaitPendingPipelineProbeCommands(context, error_out, nullptr)) {
      return false;
    }
    if (!clear_width || !clear_height || x >= width || y >= height) {
      return true;
    }
    clear_width = std::min(clear_width, width - x);
    clear_height = std::min(clear_height, height - y);
    if (!x && !y && clear_width == width && clear_height == height) {
      return ClearPipelineProbeContext(opaque_context, width, height, red, green, blue, alpha,
                                       error_out);
    }
    if (!EnsureProbeContextTexture(context, width, height, error_out) ||
        !EnsureClearPipelineState(context, error_out)) {
      return false;
    }
    if (!PrepareProbeDepthStencilSubmission(context, false, error_out)) {
      return false;
    }
    id<MTLDepthStencilState> disabled_depth_stencil_state = nil;
    if (!GetCachedProbeDepthStencilState(context, nullptr, disabled_depth_stencil_state,
                                         error_out)) {
      return false;
    }

    float clear_constants[4] = {float(red), float(green), float(blue), float(alpha)};
    id<MTLBuffer> constants_buffer =
        [context->device newBufferWithBytes:clear_constants
                                     length:sizeof(clear_constants)
                                    options:MTLResourceStorageModeShared];
    if (!constants_buffer) {
      if (error_out) {
        *error_out = "failed to create clear constants buffer";
      }
      return false;
    }

    id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
    if (!command_buffer) {
      [constants_buffer release];
      if (error_out) {
        *error_out = "failed to create rectangular probe clear command buffer";
      }
      return false;
    }
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    ConfigureProbeColorPass(pass, context,
                            context->initialized ? MTLLoadActionLoad : MTLLoadActionClear,
                            MTLClearColorMake(0.0, 0.0, 0.0, 0.0));
    ConfigureProbeDepthStencilPass(
        pass, context->depth_stencil_target->texture,
        context->depth_stencil_target->initialized ? MTLLoadActionLoad : MTLLoadActionClear);

    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    if (!encoder) {
      [constants_buffer release];
      if (error_out) {
        *error_out = "failed to create rectangular probe clear command encoder";
      }
      return false;
    }
    MTLScissorRect scissor = {x, y, clear_width, clear_height};
    [encoder setScissorRect:scissor];
    [encoder setRenderPipelineState:context->clear_pipeline_state];
    [encoder setDepthStencilState:disabled_depth_stencil_state];
    [encoder setFragmentBuffer:constants_buffer offset:0 atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];

    bool succeeded = [command_buffer status] == MTLCommandBufferStatusCompleted;
    if (succeeded) {
      context->initialized = true;
      context->color_resolve_dirty = context->sample_count > 1;
      context->depth_stencil_target->initialized = true;
    } else if (error_out) {
      NSError* error = [command_buffer error];
      *error_out = error ? [[error localizedDescription] UTF8String] : "command buffer failed";
    }
    [constants_buffer release];
    return succeeded;
  }
}

bool QueuePipelineProbeContextClearRect(void* opaque_context, uint32_t width, uint32_t height,
                                        uint32_t x, uint32_t y, uint32_t clear_width,
                                        uint32_t clear_height, double red, double green,
                                        double blue, double alpha, std::string* error_out) {
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    if (!context) {
      if (error_out) {
        *error_out = "missing probe context";
      }
      return false;
    }
    if (!clear_width || !clear_height || x >= width || y >= height) {
      return true;
    }
    clear_width = std::min(clear_width, width - x);
    clear_height = std::min(clear_height, height - y);
    if (!EnsureProbeContextTexture(context, width, height, error_out) ||
        !EnsureClearPipelineState(context, error_out)) {
      return false;
    }
    id<MTLDepthStencilState> disabled_depth_stencil_state = nil;
    if (!GetCachedProbeDepthStencilState(context, nullptr, disabled_depth_stencil_state,
                                         error_out)) {
      return false;
    }

    if (!EnsureOpenPipelineProbeEncoder(context, error_out)) {
      return false;
    }
    float clear_constants[4] = {float(red), float(green), float(blue), float(alpha)};
    ProbeUploadAllocation constants =
        UploadProbeDrawData(context, clear_constants, sizeof(clear_constants), error_out);
    if (!constants.buffer) {
      DiscardEmptyOpenPipelineProbeCommandBuffer(context);
      if (error_out) {
        if (error_out->empty()) {
          *error_out = "failed to upload queued clear constants";
        }
      }
      return false;
    }

    id<MTLRenderCommandEncoder> encoder = context->open_render_encoder;
    ClearTrackedOpenProbeBindings(context);
    MTLViewport viewport = {0.0, 0.0, double(width), double(height), 0.0, 1.0};
    MTLScissorRect scissor = {x, y, clear_width, clear_height};
    [encoder setViewport:viewport];
    [encoder setScissorRect:scissor];
    [encoder setTriangleFillMode:MTLTriangleFillModeFill];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
    [encoder setDepthBias:0.0 slopeScale:0.0 clamp:0.0];
    [encoder setDepthClipMode:MTLDepthClipModeClip];
    [encoder setBlendColorRed:0.0 green:0.0 blue:0.0 alpha:0.0];
    [encoder setRenderPipelineState:context->clear_pipeline_state];
    [encoder setDepthStencilState:disabled_depth_stencil_state];
    [encoder setStencilFrontReferenceValue:0 backReferenceValue:0];
    [encoder setFragmentBuffer:constants.buffer offset:constants.offset atIndex:0];
    TrackOpenProbeBufferBinding(context, false, 0);
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    ++context->open_draw_submission_count;
    context->initialized = true;
    context->color_resolve_dirty = context->sample_count > 1;
    context->depth_stencil_target->initialized = true;

    if (context->open_draw_submission_count >= kMaxProbeDrawsPerCommandBuffer &&
        !FinalizeOpenPipelineProbeCommandBuffer(context, error_out)) {
      return false;
    }
    if (GetCommittedProbeDrawCommandBufferCount(context) >= kMaxCommittedProbeCommandBuffers) {
      return WaitOldestPipelineProbeCommand(context, error_out);
    }
    return true;
  }
}

bool QueuePipelineProbeContextDepthStencilClearRect(void* opaque_context, uint32_t width,
                                                    uint32_t height, uint32_t x, uint32_t y,
                                                    uint32_t clear_width, uint32_t clear_height,
                                                    float depth, uint8_t stencil,
                                                    std::string* error_out) {
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    if (!context || !std::isfinite(depth)) {
      if (error_out) {
        *error_out = "missing probe context or invalid depth clear value";
      }
      return false;
    }
    if (!clear_width || !clear_height || x >= width || y >= height) {
      return true;
    }
    clear_width = std::min(clear_width, width - x);
    clear_height = std::min(clear_height, height - y);
    if (!EnsureProbeContextTexture(context, width, height, error_out) ||
        !EnsureDepthClearPipelineState(context, error_out)) {
      return false;
    }

    ProbeDepthStencilState clear_state;
    clear_state.depth_test_enabled = true;
    clear_state.depth_write_enabled = true;
    clear_state.depth_compare_function = uint8_t(MTLCompareFunctionAlways);
    clear_state.stencil_test_enabled = true;
    clear_state.front.compare_function = uint8_t(MTLCompareFunctionAlways);
    clear_state.front.depth_stencil_pass_operation = uint8_t(MTLStencilOperationReplace);
    clear_state.front.read_mask = 0xFF;
    clear_state.front.write_mask = 0xFF;
    clear_state.front.reference = stencil;
    clear_state.back = clear_state.front;
    id<MTLDepthStencilState> metal_clear_state = nil;
    if (!GetCachedProbeDepthStencilState(context, &clear_state, metal_clear_state, error_out) ||
        !EnsureOpenPipelineProbeEncoder(context, error_out)) {
      return false;
    }

    depth = std::clamp(depth, 0.0f, 1.0f);
    ProbeUploadAllocation constants =
        UploadProbeDrawData(context, &depth, sizeof(depth), error_out);
    if (!constants.buffer) {
      DiscardEmptyOpenPipelineProbeCommandBuffer(context);
      if (error_out && error_out->empty()) {
        *error_out = "failed to upload depth clear constants";
      }
      return false;
    }

    id<MTLRenderCommandEncoder> encoder = context->open_render_encoder;
    ClearTrackedOpenProbeBindings(context);
    MTLViewport viewport = {0.0, 0.0, double(width), double(height), 0.0, 1.0};
    MTLScissorRect scissor = {x, y, clear_width, clear_height};
    [encoder setViewport:viewport];
    [encoder setScissorRect:scissor];
    [encoder setTriangleFillMode:MTLTriangleFillModeFill];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
    [encoder setDepthBias:0.0 slopeScale:0.0 clamp:0.0];
    [encoder setDepthClipMode:MTLDepthClipModeClip];
    [encoder setRenderPipelineState:context->depth_clear_pipeline_state];
    [encoder setDepthStencilState:metal_clear_state];
    [encoder setStencilFrontReferenceValue:stencil backReferenceValue:stencil];
    [encoder setVertexBuffer:constants.buffer offset:constants.offset atIndex:0];
    TrackOpenProbeBufferBinding(context, true, 0);
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    ++context->open_draw_submission_count;
    context->initialized = true;
    // A first depth-only operation initializes the multisample color
    // attachment through the render pass clear. Make that deterministic black
    // initialization visible to any immediate single-sample read or resolve.
    context->color_resolve_dirty = context->sample_count > 1;
    context->depth_stencil_target->initialized = true;

    if (context->open_draw_submission_count >= kMaxProbeDrawsPerCommandBuffer &&
        !FinalizeOpenPipelineProbeCommandBuffer(context, error_out)) {
      return false;
    }
    if (GetCommittedProbeDrawCommandBufferCount(context) >= kMaxCommittedProbeCommandBuffers) {
      return WaitOldestPipelineProbeCommand(context, error_out);
    }
    return true;
  }
}

bool RenderPipelineProbeToContext(
    void* opaque_context, void* pipeline_state, const void* system_constants,
    size_t system_constants_size, const void* float_constants, size_t float_constants_size,
    const void* fetch_constants, size_t fetch_constants_size, void* shared_memory,
    size_t shared_memory_size, void* shared_memory_metal_buffer,
    const ProbeTextureSlot* vertex_textures, size_t vertex_texture_count,
    size_t vertex_sampler_count, const ProbeTextureSlot* fragment_textures,
    size_t fragment_texture_count, size_t fragment_sampler_count, uint32_t primitive_type,
    uint32_t vertex_count, uint32_t width, uint32_t height, std::string* error_out,
    uint32_t vertex_shared_memory_buffer_index, uint32_t vertex_float_constants_buffer_index,
    uint32_t vertex_fetch_constants_buffer_index, const void* fragment_float_constants,
    size_t fragment_float_constants_size, uint32_t fragment_float_constants_buffer_index,
    uint32_t fragment_fetch_constants_buffer_index, const ProbeSamplerSlot* vertex_samplers,
    const ProbeSamplerSlot* fragment_samplers, const void* vertex_data, size_t vertex_data_size,
    uint32_t vertex_data_buffer_index, const void* bool_loop_constants,
    size_t bool_loop_constants_size, uint32_t vertex_bool_loop_constants_buffer_index,
    uint32_t fragment_bool_loop_constants_buffer_index, const ProbeIndexBuffer* index_buffer,
    const ProbeRasterizationState* rasterization_state,
    const ProbeDepthStencilState* depth_stencil_state,
    uint32_t fragment_shared_memory_buffer_index) {
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    if (!context || !pipeline_state || !system_constants || !system_constants_size || !width ||
        !height || !vertex_count) {
      if (error_out) {
        *error_out = "missing probe context, pipeline state, constants, or target size";
      }
      return false;
    }
    if (!IsProbeIndexBufferValid(index_buffer, vertex_count)) {
      if (error_out) {
        *error_out = "invalid probe index buffer";
      }
      return false;
    }
    if (!IsProbeRasterizationStateValid(rasterization_state, width, height)) {
      if (error_out) {
        *error_out = "invalid probe rasterization state";
      }
      return false;
    }
    if (!IsProbeDepthStencilStateValid(depth_stencil_state)) {
      if (error_out) {
        *error_out = "invalid probe depth/stencil state";
      }
      return false;
    }
    if (!EnsureProbeContextTexture(context, width, height, error_out) ||
        !EnsureDummyProbeResources(context, error_out)) {
      return false;
    }

    if (!EnsureOpenPipelineProbeEncoder(context, error_out)) {
      return false;
    }

    id<MTLDepthStencilState> metal_depth_stencil_state = nil;
    if (!GetCachedProbeDepthStencilState(context, depth_stencil_state, metal_depth_stencil_state,
                                         error_out)) {
      DiscardEmptyOpenPipelineProbeCommandBuffer(context);
      return false;
    }

    id<MTLDevice> device = context->device;
    ProbeUploadAllocation system_buffer =
        UploadProbeDrawData(context, system_constants, system_constants_size, error_out);
    ProbeUploadAllocation float_buffer =
        UploadProbeDrawData(context, float_constants, float_constants_size, error_out);
    ProbeUploadAllocation fragment_float_buffer = UploadProbeDrawData(
        context, fragment_float_constants, fragment_float_constants_size, error_out);
    ProbeUploadAllocation fetch_buffer =
        UploadProbeDrawData(context, fetch_constants, fetch_constants_size, error_out);
    ProbeUploadAllocation bool_loop_buffer =
        UploadProbeDrawData(context, bool_loop_constants, bool_loop_constants_size, error_out);
    ProbeUploadAllocation vertex_data_buffer;
    if (vertex_data_buffer_index != UINT32_MAX) {
      vertex_data_buffer = UploadProbeDrawData(context, vertex_data, vertex_data_size, error_out);
    }
    ProbeUploadAllocation uploaded_index_buffer;
    id<MTLBuffer> index_buffer_object = nil;
    NSUInteger index_buffer_offset = 0;
    if (index_buffer) {
      if (index_buffer->metal_buffer) {
        index_buffer_object = (id<MTLBuffer>)index_buffer->metal_buffer;
        index_buffer_offset = NSUInteger(index_buffer->offset);
      } else {
        uploaded_index_buffer =
            UploadProbeDrawData(context, index_buffer->data, index_buffer->size, error_out);
        index_buffer_object = uploaded_index_buffer.buffer;
        index_buffer_offset = uploaded_index_buffer.offset + NSUInteger(index_buffer->offset);
      }
    }
    id<MTLBuffer> shared_memory_buffer = nil;
    bool owns_shared_memory_buffer = false;
    if (shared_memory_metal_buffer) {
      shared_memory_buffer = (id<MTLBuffer>)shared_memory_metal_buffer;
    } else if (shared_memory && shared_memory_size) {
      shared_memory_buffer = [device newBufferWithBytesNoCopy:shared_memory
                                                       length:shared_memory_size
                                                      options:MTLResourceStorageModeShared
                                                  deallocator:nil];
      owns_shared_memory_buffer = shared_memory_buffer != nil;
    }
    bool missing_argument_buffer =
        !system_buffer.buffer ||
        ((vertex_shared_memory_buffer_index != UINT32_MAX ||
          fragment_shared_memory_buffer_index != UINT32_MAX) &&
         !shared_memory_buffer) ||
        (float_constants && float_constants_size && !float_buffer.buffer) ||
        (fragment_float_constants && fragment_float_constants_size &&
         !fragment_float_buffer.buffer) ||
        (fetch_constants && fetch_constants_size && !fetch_buffer.buffer) ||
        (bool_loop_constants && bool_loop_constants_size && !bool_loop_buffer.buffer) ||
        (vertex_data && vertex_data_size && vertex_data_buffer_index != UINT32_MAX &&
         !vertex_data_buffer.buffer) ||
        (index_buffer && !index_buffer_object);
    if (missing_argument_buffer) {
      if (owns_shared_memory_buffer) {
        [shared_memory_buffer release];
      }
      if (error_out) {
        if (error_out->empty()) {
          *error_out = index_buffer && !index_buffer_object
                           ? "failed to upload persistent probe index data"
                           : ((vertex_shared_memory_buffer_index != UINT32_MAX ||
                               fragment_shared_memory_buffer_index != UINT32_MAX) &&
                                      !shared_memory_buffer
                                  ? "required persistent shared-memory buffer is unavailable"
                                  : "failed to upload persistent probe argument data");
        }
      }
      DiscardEmptyOpenPipelineProbeCommandBuffer(context);
      return false;
    }

    id<MTLTexture> dummy_texture = context->dummy_texture;
    id<MTLSamplerState> dummy_sampler = context->dummy_sampler;
    std::vector<id<MTLTexture>> vertex_texture_objects;
    std::vector<id<MTLTexture>> fragment_texture_objects;
    std::vector<id<MTLSamplerState>> vertex_sampler_objects;
    std::vector<id<MTLSamplerState>> fragment_sampler_objects;
    CreateProbeTextures(device, vertex_textures, vertex_texture_count, dummy_texture,
                        vertex_texture_objects, false);
    CreateProbeTextures(device, fragment_textures, fragment_texture_count, dummy_texture,
                        fragment_texture_objects, false);
    CreateCachedProbeSamplers(context, vertex_samplers, vertex_sampler_count, dummy_sampler,
                              vertex_sampler_objects);
    CreateCachedProbeSamplers(context, fragment_samplers, fragment_sampler_count, dummy_sampler,
                              fragment_sampler_objects);

    auto release_submission_resources = [&]() {
      if (owns_shared_memory_buffer) {
        [shared_memory_buffer release];
      }
      ReleaseOwnedProbeTextures(vertex_texture_objects, dummy_texture, vertex_textures);
      ReleaseOwnedProbeTextures(fragment_texture_objects, dummy_texture, fragment_textures);
      vertex_sampler_objects.clear();
      fragment_sampler_objects.clear();
    };

    id<MTLRenderCommandEncoder> encoder = context->open_render_encoder;
    ClearTrackedOpenProbeBindings(context);
    [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline_state];
    MTLViewport viewport;
    MTLScissorRect scissor;
    double blend_red = 0.0;
    double blend_green = 0.0;
    double blend_blue = 0.0;
    double blend_alpha = 0.0;
    if (rasterization_state) {
      viewport = {rasterization_state->viewport_x,     rasterization_state->viewport_y,
                  rasterization_state->viewport_width, rasterization_state->viewport_height,
                  rasterization_state->viewport_z_min, rasterization_state->viewport_z_max};
      scissor = {rasterization_state->scissor_x, rasterization_state->scissor_y,
                 rasterization_state->scissor_width, rasterization_state->scissor_height};
      blend_red = rasterization_state->blend_red;
      blend_green = rasterization_state->blend_green;
      blend_blue = rasterization_state->blend_blue;
      blend_alpha = rasterization_state->blend_alpha;
    } else {
      viewport = {0.0, 0.0, double(width), double(height), 0.0, 1.0};
      scissor = {0, 0, width, height};
    }
    [encoder setViewport:viewport];
    [encoder setScissorRect:scissor];
    [encoder setCullMode:ToMetalCullMode(rasterization_state ? rasterization_state->cull_mode
                                                             : ProbeCullMode::kNone)];
    [encoder setFrontFacingWinding:rasterization_state && rasterization_state->front_face_clockwise
                                       ? MTLWindingClockwise
                                       : MTLWindingCounterClockwise];
    [encoder setDepthBias:rasterization_state ? rasterization_state->depth_bias : 0.0
               slopeScale:rasterization_state ? rasterization_state->depth_bias_slope_scale : 0.0
                    clamp:0.0];
    [encoder setDepthClipMode:rasterization_state && rasterization_state->depth_clamp_enabled
                                  ? MTLDepthClipModeClamp
                                  : MTLDepthClipModeClip];
    [encoder setDepthStencilState:metal_depth_stencil_state];
    [encoder
        setStencilFrontReferenceValue:depth_stencil_state ? depth_stencil_state->front.reference : 0
                   backReferenceValue:depth_stencil_state ? depth_stencil_state->back.reference
                                                          : 0];
    [encoder setBlendColorRed:blend_red green:blend_green blue:blend_blue alpha:blend_alpha];
    id<MTLBuffer> vertex_buffers[32] = {};
    NSUInteger vertex_buffer_offsets[32] = {};
    id<MTLBuffer> fragment_buffers[32] = {};
    NSUInteger fragment_buffer_offsets[32] = {};
    uint32_t vertex_buffer_mask = 0;
    uint32_t fragment_buffer_mask = 0;
    auto queue_buffer = [&](bool vertex_stage, id<MTLBuffer> buffer, NSUInteger offset,
                            uint32_t index) {
      if (index == UINT32_MAX) {
        return;
      }
      if (index < 32) {
        id<MTLBuffer>* buffers = vertex_stage ? vertex_buffers : fragment_buffers;
        NSUInteger* offsets = vertex_stage ? vertex_buffer_offsets : fragment_buffer_offsets;
        buffers[index] = buffer;
        offsets[index] = offset;
        uint32_t& mask = vertex_stage ? vertex_buffer_mask : fragment_buffer_mask;
        mask |= uint32_t(1) << index;
      } else if (vertex_stage) {
        [encoder setVertexBuffer:buffer offset:offset atIndex:index];
      } else {
        [encoder setFragmentBuffer:buffer offset:offset atIndex:index];
      }
    };
    queue_buffer(true, system_buffer.buffer, system_buffer.offset, 0);
    queue_buffer(false, system_buffer.buffer, system_buffer.offset, 0);
    queue_buffer(true, fetch_buffer.buffer, fetch_buffer.offset,
                 vertex_fetch_constants_buffer_index);
    queue_buffer(false, fetch_buffer.buffer, fetch_buffer.offset,
                 fragment_fetch_constants_buffer_index);
    queue_buffer(true, bool_loop_buffer.buffer, bool_loop_buffer.offset,
                 vertex_bool_loop_constants_buffer_index);
    queue_buffer(false, bool_loop_buffer.buffer, bool_loop_buffer.offset,
                 fragment_bool_loop_constants_buffer_index);
    queue_buffer(true, float_buffer.buffer, float_buffer.offset,
                 vertex_float_constants_buffer_index);
    const ProbeUploadAllocation& fragment_constants_to_bind =
        fragment_float_buffer.buffer ? fragment_float_buffer : float_buffer;
    queue_buffer(false, fragment_constants_to_bind.buffer, fragment_constants_to_bind.offset,
                 fragment_float_constants_buffer_index);
    queue_buffer(true, shared_memory_buffer, 0, vertex_shared_memory_buffer_index);
    queue_buffer(false, shared_memory_buffer, 0, fragment_shared_memory_buffer_index);
    queue_buffer(true, vertex_data_buffer.buffer, vertex_data_buffer.offset,
                 vertex_data_buffer_index);
    auto buffer_range_count = [](uint32_t mask) {
      NSUInteger count = 0;
      while (mask) {
        ++count;
        mask >>= 1;
      }
      return count;
    };
    NSUInteger vertex_buffer_count = buffer_range_count(vertex_buffer_mask);
    if (vertex_buffer_count) {
      [encoder setVertexBuffers:vertex_buffers
                        offsets:vertex_buffer_offsets
                      withRange:NSMakeRange(0, vertex_buffer_count)];
    }
    NSUInteger fragment_buffer_count = buffer_range_count(fragment_buffer_mask);
    if (fragment_buffer_count) {
      [encoder setFragmentBuffers:fragment_buffers
                          offsets:fragment_buffer_offsets
                        withRange:NSMakeRange(0, fragment_buffer_count)];
    }
    context->tracked_vertex_buffer_mask = vertex_buffer_mask;
    context->tracked_fragment_buffer_mask = fragment_buffer_mask;
    BindProbeTextures(encoder, vertex_texture_objects, true);
    BindProbeTextures(encoder, fragment_texture_objects, false);
    BindProbeSamplers(encoder, vertex_sampler_objects, true);
    BindProbeSamplers(encoder, fragment_sampler_objects, false);
    context->tracked_vertex_texture_count = vertex_texture_objects.size();
    context->tracked_fragment_texture_count = fragment_texture_objects.size();
    context->tracked_vertex_sampler_count = vertex_sampler_objects.size();
    context->tracked_fragment_sampler_count = fragment_sampler_objects.size();
    if (index_buffer_object) {
      [encoder drawIndexedPrimitives:ToMetalPrimitiveType(primitive_type)
                          indexCount:vertex_count
                           indexType:index_buffer->index_size == 2 ? MTLIndexTypeUInt16
                                                                   : MTLIndexTypeUInt32
                         indexBuffer:index_buffer_object
                   indexBufferOffset:index_buffer_offset];
    } else {
      [encoder drawPrimitives:ToMetalPrimitiveType(primitive_type)
                  vertexStart:0
                  vertexCount:vertex_count];
    }
    ++context->open_draw_submission_count;
    context->initialized = true;
    context->color_resolve_dirty = context->sample_count > 1;
    context->depth_stencil_target->initialized = true;

    // Normal command buffers retain every encoded resource. Release resources
    // created by this call immediately; cache/caller-owned Metal objects were
    // borrowed without redundant per-draw retain/release pairs.
    release_submission_resources();

    // A no-copy buffer aliases caller-owned bytes rather than snapshotting them.
    // Preserve the old synchronous contract whenever that buffer is actually
    // bound to the vertex stage. The resident MTLBuffer path remains asynchronous.
    bool raw_nocopy_buffer_bound = !shared_memory_metal_buffer && shared_memory &&
                                   (vertex_shared_memory_buffer_index != UINT32_MAX ||
                                    fragment_shared_memory_buffer_index != UINT32_MAX);
    if (context->open_draw_submission_count >= kMaxProbeDrawsPerCommandBuffer &&
        !FinalizeOpenPipelineProbeCommandBuffer(context, error_out)) {
      return false;
    }
    bool committed_limit_reached =
        GetCommittedProbeDrawCommandBufferCount(context) >= kMaxCommittedProbeCommandBuffers;
    if (raw_nocopy_buffer_bound) {
      return WaitPendingPipelineProbeCommands(context, error_out, nullptr);
    }
    if (committed_limit_reached) {
      return WaitOldestPipelineProbeCommand(context, error_out);
    }
    return true;
  }
}

bool ReadPipelineProbeContextRect(void* opaque_context, uint32_t width, uint32_t height, uint32_t x,
                                  uint32_t y, uint32_t read_width, uint32_t read_height,
                                  std::vector<uint8_t>& bgra_out, std::string* error_out) {
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    if (!context) {
      if (error_out) {
        *error_out = "missing probe context";
      }
      return false;
    }
    bool texture_valid = context->render_texture && context->initialized && width && height &&
                         context->width == width && context->height == height;
    if (!texture_valid) {
      // Read is a fence even if the requested texture metadata is invalid.
      if (!WaitPendingPipelineProbeCommands(context, error_out, nullptr)) {
        return false;
      }
      if (error_out) {
        *error_out = "persistent probe texture is unavailable or has a different size";
      }
      return false;
    }
    if (!read_width || !read_height || x >= width || y >= height || read_width > width - x ||
        read_height > height - y || size_t(read_width) > SIZE_MAX / 4 ||
        size_t(read_height) > SIZE_MAX / (size_t(read_width) * 4)) {
      if (!WaitPendingPipelineProbeCommands(context, error_out, nullptr)) {
        return false;
      }
      if (error_out) {
        *error_out = "persistent probe read rectangle is empty or out of bounds";
      }
      return false;
    }
    bgra_out.resize(size_t(read_width) * read_height * 4);
    if (context->storage_mode == MTLStorageModePrivate) {
      // The readback blit is submitted to the same queue as the render work.
      // Commit pending draws without a separate CPU wait; waiting for the blit
      // completes all earlier work in queue order.
      if (!FinalizeProbeColorForConsumer(context, error_out)) {
        return false;
      }
      size_t row_pitch = (size_t(read_width) * 4 + 255) & ~size_t(255);
      id<MTLBuffer> readback_buffer =
          EnsureProbeReadbackBuffer(context, width, height, row_pitch, read_height, error_out);
      if (!readback_buffer) {
        WaitPendingPipelineProbeCommands(context, nullptr, nullptr);
        return false;
      }
      id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
      if (!command_buffer) {
        WaitPendingPipelineProbeCommands(context, nullptr, nullptr);
        if (error_out) {
          *error_out = "failed to create private render target readback command buffer";
        }
        return false;
      }
      id<MTLBlitCommandEncoder> blit_encoder = [command_buffer blitCommandEncoder];
      if (!blit_encoder) {
        WaitPendingPipelineProbeCommands(context, nullptr, nullptr);
        if (error_out) {
          *error_out = "failed to create private render target readback encoder";
        }
        return false;
      }
      [blit_encoder copyFromTexture:context->render_texture
                        sourceSlice:0
                        sourceLevel:0
                       sourceOrigin:MTLOriginMake(x, y, 0)
                         sourceSize:MTLSizeMake(read_width, read_height, 1)
                           toBuffer:readback_buffer
                  destinationOffset:0
             destinationBytesPerRow:row_pitch
           destinationBytesPerImage:row_pitch * read_height];
      [blit_encoder endEncoding];
      [command_buffer commit];
      [command_buffer waitUntilCompleted];
      bool succeeded = [command_buffer status] == MTLCommandBufferStatusCompleted;
      bool prior_commands_succeeded = ConsumeCompletedPipelineProbeCommands(context, error_out);
      if (!prior_commands_succeeded) {
        return false;
      }
      if (!succeeded) {
        if (error_out) {
          NSError* error = [command_buffer error];
          *error_out = error ? [[error localizedDescription] UTF8String]
                             : "private render target readback failed";
        }
        return false;
      }
      const uint8_t* source = static_cast<const uint8_t*>([readback_buffer contents]);
      size_t tight_row_pitch = size_t(read_width) * 4;
      if (row_pitch == tight_row_pitch) {
        std::memcpy(bgra_out.data(), source, tight_row_pitch * read_height);
      } else {
        for (uint32_t row = 0; row < read_height; ++row) {
          std::memcpy(bgra_out.data() + size_t(row) * tight_row_pitch,
                      source + size_t(row) * row_pitch, tight_row_pitch);
        }
      }
      return true;
    }
    if (!FinalizeProbeColorForConsumer(context, error_out) ||
        !WaitPendingPipelineProbeCommands(context, error_out, nullptr)) {
      return false;
    }
    MTLRegion region = MTLRegionMake2D(x, y, read_width, read_height);
    [context->render_texture getBytes:bgra_out.data()
                          bytesPerRow:size_t(read_width) * 4
                           fromRegion:region
                          mipmapLevel:0];
    return true;
  }
}

bool ReadPipelineProbeContextRectSampleSelected(void* opaque_context, uint32_t width,
                                                uint32_t height, uint32_t x, uint32_t y,
                                                uint32_t read_width, uint32_t read_height,
                                                uint32_t color_sample_select,
                                                std::vector<uint8_t>& bgra_out,
                                                std::string* error_out) {
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    bgra_out.clear();
    if (!context) {
      if (error_out) {
        *error_out = "missing probe context";
      }
      return false;
    }
    auto reject_and_drain = [&](const std::string& reason) {
      std::string drain_error;
      bool drained = WaitPendingPipelineProbeCommands(context, &drain_error, nullptr);
      if (error_out) {
        *error_out = reason;
        if (!drained && !drain_error.empty()) {
          error_out->append("; prior render work failed: ");
          error_out->append(drain_error);
        }
      }
      return false;
    };

    bool texture_valid = context->render_texture && context->initialized && width && height &&
                         context->width == width && context->height == height;
    if (!texture_valid) {
      return reject_and_drain(
          "persistent multisample probe texture is unavailable or has a different size");
    }
    uint32_t host_sample_mask = 0;
    if (!GetProbeColorSampleMask(context->sample_count, color_sample_select, host_sample_mask)) {
      return reject_and_drain("probe read sample selection is invalid for the sample count");
    }
    uint32_t full_host_sample_mask = (uint32_t(1) << context->sample_count) - 1;
    if (host_sample_mask == full_host_sample_mask) {
      return ReadPipelineProbeContextRect(opaque_context, width, height, x, y, read_width,
                                          read_height, bgra_out, error_out);
    }
    if (!context->multisample_render_texture) {
      return reject_and_drain("persistent multisample probe texture is unavailable");
    }
    if (!read_width || !read_height || x >= width || y >= height || read_width > width - x ||
        read_height > height - y || size_t(read_width) > SIZE_MAX / 4 ||
        size_t(read_height) > SIZE_MAX / (size_t(read_width) * 4)) {
      return reject_and_drain("persistent probe read rectangle is empty or out of bounds");
    }

    std::string setup_error;
    if (!EnsureMultisampleSelectResolvePipelineState(context, &setup_error)) {
      return reject_and_drain(setup_error);
    }
    size_t row_pitch = (size_t(read_width) * 4 + 255) & ~size_t(255);
    id<MTLBuffer> readback_buffer =
        EnsureProbeReadbackBuffer(context, width, height, row_pitch, read_height, &setup_error);
    if (!readback_buffer) {
      return reject_and_drain(setup_error);
    }
    if (!FinalizeOpenPipelineProbeCommandBuffer(context, &setup_error)) {
      return reject_and_drain(setup_error.empty()
                                  ? "failed to finalize pending render work for selected read"
                                  : setup_error);
    }

    id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
    if (!command_buffer) {
      return reject_and_drain("failed to create selected-sample read command buffer");
    }
    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    if (!encoder) {
      return reject_and_drain("failed to create selected-sample read compute encoder");
    }
    MultisampleSelectResolveConstants constants = {
        x, y, read_width, read_height, uint32_t(row_pitch), host_sample_mask};
    id<MTLComputePipelineState> pipeline_state = context->multisample_select_resolve_pipeline_state;
    [encoder setComputePipelineState:pipeline_state];
    [encoder setTexture:context->multisample_render_texture atIndex:0];
    [encoder setBuffer:readback_buffer offset:0 atIndex:0];
    [encoder setBytes:&constants length:sizeof(constants) atIndex:1];
    NSUInteger thread_width = std::max<NSUInteger>(
        1, std::min<NSUInteger>(read_width, [pipeline_state threadExecutionWidth]));
    NSUInteger max_threads = [pipeline_state maxTotalThreadsPerThreadgroup];
    NSUInteger thread_height =
        std::max<NSUInteger>(1, std::min<NSUInteger>(read_height, max_threads / thread_width));
    [encoder dispatchThreads:MTLSizeMake(read_width, read_height, 1)
        threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1)];
    [encoder endEncoding];
    ++context->multisample_resolve_count;
    [command_buffer commit];
    [command_buffer waitUntilCompleted];

    std::string prior_error;
    bool prior_commands_succeeded = ConsumeCompletedPipelineProbeCommands(context, &prior_error);
    bool read_succeeded = [command_buffer status] == MTLCommandBufferStatusCompleted;
    if (!prior_commands_succeeded || !read_succeeded) {
      if (error_out) {
        error_out->clear();
        if (!prior_commands_succeeded) {
          error_out->append("prior render work failed: ");
          error_out->append(prior_error);
        }
        if (!read_succeeded) {
          if (!error_out->empty()) {
            error_out->append("; ");
          }
          NSError* command_error = [command_buffer error];
          const char* description =
              command_error ? [[command_error localizedDescription] UTF8String] : nullptr;
          error_out->append(description ? description : "selected-sample read failed");
        }
      }
      return false;
    }

    size_t tight_row_pitch = size_t(read_width) * 4;
    bgra_out.resize(tight_row_pitch * read_height);
    const uint8_t* source = static_cast<const uint8_t*>([readback_buffer contents]);
    if (row_pitch == tight_row_pitch) {
      std::memcpy(bgra_out.data(), source, tight_row_pitch * read_height);
    } else {
      for (uint32_t row = 0; row < read_height; ++row) {
        std::memcpy(bgra_out.data() + size_t(row) * tight_row_pitch,
                    source + size_t(row) * row_pitch, tight_row_pitch);
      }
    }
    return true;
  }
}

bool ResolvePipelineProbeContextToXenosTiled(void* opaque_context, uint32_t width, uint32_t height,
                                             uint32_t source_x, uint32_t source_y,
                                             uint32_t resolve_width, uint32_t resolve_height,
                                             const ProbeTiledResolveTarget& destination,
                                             std::vector<uint8_t>* bgra_out,
                                             std::string* error_out) {
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    if (bgra_out) {
      bgra_out->clear();
    }
    if (!context) {
      if (error_out) {
        *error_out = "missing probe context";
      }
      return false;
    }

    auto reject_and_drain = [&](const std::string& reason) {
      std::string drain_error;
      bool drained = WaitPendingPipelineProbeCommands(context, &drain_error, nullptr);
      if (error_out) {
        *error_out = reason;
        if (!drained && !drain_error.empty()) {
          error_out->append("; prior render work failed: ");
          error_out->append(drain_error);
        }
      }
      return false;
    };

    bool texture_valid = context->render_texture && context->initialized && width && height &&
                         context->width == width && context->height == height;
    if (!texture_valid) {
      return reject_and_drain("persistent probe texture is unavailable or has a different size");
    }
    uint32_t host_sample_mask = 0;
    if (!GetProbeColorSampleMask(context->sample_count, destination.color_sample_select,
                                 host_sample_mask)) {
      return reject_and_drain("tiled resolve sample selection is invalid for the sample count");
    }
    uint32_t full_host_sample_mask = (uint32_t(1) << context->sample_count) - 1;
    bool selective_multisample_resolve =
        context->sample_count > 1 && host_sample_mask != full_host_sample_mask;
    id<MTLTexture> presentation_snapshot =
        (id<MTLTexture>)destination.presentation_snapshot_texture;
    bool snapshot_only = !destination.metal_buffer && presentation_snapshot &&
                         !destination.guest_memory_metal_buffer &&
                         !destination.guest_memory_copy_length;
    if (!resolve_width || !resolve_height || source_x >= width || source_y >= height ||
        resolve_width > width - source_x || resolve_height > height - source_y ||
        (!snapshot_only &&
         (!destination.metal_buffer || !destination.pitch ||
          !destination.height || destination.x >= destination.pitch ||
          destination.y >= destination.height ||
          resolve_width > destination.pitch - destination.x ||
          resolve_height > destination.height - destination.y ||
          destination.endian > 3 || (destination.buffer_offset & 3) ||
          destination.buffer_offset > UINT32_MAX)) ||
        resolve_width > UINT32_MAX / 4 || size_t(resolve_width) > SIZE_MAX / 4 ||
        size_t(resolve_height) > SIZE_MAX / (size_t(resolve_width) * 4)) {
      return reject_and_drain("invalid tiled resolve rectangle, surface, buffer, or endian");
    }

    id<MTLBuffer> destination_buffer = (id<MTLBuffer>)destination.metal_buffer;
    id<MTLBuffer> guest_memory_buffer = (id<MTLBuffer>)destination.guest_memory_metal_buffer;
    uint64_t aligned_destination_pitch = (uint64_t(destination.pitch) + 31u) & ~uint64_t(31u);
    uint64_t aligned_destination_height = (uint64_t(destination.height) + 31u) & ~uint64_t(31u);
    if (!snapshot_only && aligned_destination_height &&
        aligned_destination_pitch >
            uint64_t(UINT32_MAX / 4) / aligned_destination_height) {
      return reject_and_drain("tiled resolve surface byte extent exceeds 32-bit addressing");
    }
    uint32_t tiled_surface_extent =
        snapshot_only
            ? 0
            : GetTiledRgba8UpperBound(destination.pitch, destination.height,
                                     destination.pitch);
    uint64_t destination_end = uint64_t(destination.buffer_offset) + tiled_surface_extent;
    if (!snapshot_only &&
        ([destination_buffer storageMode] != MTLStorageModeShared ||
         !tiled_surface_extent || destination_end > [destination_buffer length] ||
         destination_end > UINT32_MAX)) {
      return reject_and_drain(
          "tiled resolve destination is not a sufficiently large shared Metal buffer");
    }
    if (guest_memory_buffer) {
      uint64_t mirror_source_end = uint64_t(destination.guest_memory_copy_source_offset) +
                                   destination.guest_memory_copy_length;
      uint64_t mirror_destination_end = uint64_t(destination.guest_memory_copy_destination_offset) +
                                        destination.guest_memory_copy_length;
      if (!destination.guest_memory_copy_length ||
          [guest_memory_buffer storageMode] != MTLStorageModeShared ||
          mirror_source_end > [destination_buffer length] ||
          mirror_destination_end > [guest_memory_buffer length]) {
        return reject_and_drain(
            "tiled resolve guest mirror range is invalid or not backed by shared storage");
      }
    } else if (destination.guest_memory_copy_length) {
      return reject_and_drain("tiled resolve guest mirror range has no destination buffer");
    }
    if (presentation_snapshot &&
        ([presentation_snapshot device] != context->device ||
         [presentation_snapshot pixelFormat] != MTLPixelFormatBGRA8Unorm ||
         destination.presentation_snapshot_x >= [presentation_snapshot width] ||
         destination.presentation_snapshot_y >= [presentation_snapshot height] ||
         resolve_width > [presentation_snapshot width] - destination.presentation_snapshot_x ||
         resolve_height > [presentation_snapshot height] - destination.presentation_snapshot_y)) {
      return reject_and_drain("tiled resolve presentation snapshot texture is incompatible");
    }

    std::string setup_error;
    if ((!snapshot_only &&
         !EnsureTiledResolvePipelineState(context, &setup_error)) ||
        (selective_multisample_resolve &&
         !EnsureMultisampleSelectResolvePipelineState(context, &setup_error))) {
      return reject_and_drain(setup_error);
    }
    size_t row_pitch = (size_t(resolve_width) * 4 + 255) & ~size_t(255);
    id<MTLBuffer> staging_buffer =
        EnsureProbeReadbackBuffer(context, width, height, row_pitch, resolve_height, &setup_error);
    if (!staging_buffer) {
      return reject_and_drain(setup_error);
    }

    // Commit render work without waiting. The blit and compute command buffer is
    // on the same queue, so waiting for it completes every earlier submission.
    std::string finalize_error;
    bool color_finalized = selective_multisample_resolve
                               ? FinalizeOpenPipelineProbeCommandBuffer(context, &finalize_error)
                               : FinalizeProbeColorForConsumer(context, &finalize_error);
    if (!color_finalized) {
      return reject_and_drain(finalize_error.empty()
                                  ? "failed to finalize pending render work for tiled resolve"
                                  : finalize_error);
    }
    id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
    if (!command_buffer) {
      return reject_and_drain("failed to create tiled resolve command buffer");
    }
    if (selective_multisample_resolve) {
      id<MTLComputeCommandEncoder> sample_resolve_encoder = [command_buffer computeCommandEncoder];
      if (!sample_resolve_encoder) {
        return reject_and_drain("failed to create multisample select resolve encoder");
      }
      MultisampleSelectResolveConstants sample_resolve_constants = {
          source_x, source_y, resolve_width, resolve_height, uint32_t(row_pitch), host_sample_mask};
      id<MTLComputePipelineState> sample_resolve_pipeline_state =
          context->multisample_select_resolve_pipeline_state;
      [sample_resolve_encoder setComputePipelineState:sample_resolve_pipeline_state];
      [sample_resolve_encoder setTexture:context->multisample_render_texture atIndex:0];
      [sample_resolve_encoder setBuffer:staging_buffer offset:0 atIndex:0];
      [sample_resolve_encoder setBytes:&sample_resolve_constants
                                length:sizeof(sample_resolve_constants)
                               atIndex:1];
      NSUInteger sample_thread_width = std::max<NSUInteger>(
          1, std::min<NSUInteger>(resolve_width,
                                  [sample_resolve_pipeline_state threadExecutionWidth]));
      NSUInteger sample_max_threads = [sample_resolve_pipeline_state maxTotalThreadsPerThreadgroup];
      NSUInteger sample_thread_height = std::max<NSUInteger>(
          1, std::min<NSUInteger>(resolve_height, sample_max_threads / sample_thread_width));
      [sample_resolve_encoder
                dispatchThreads:MTLSizeMake(resolve_width, resolve_height, 1)
          threadsPerThreadgroup:MTLSizeMake(sample_thread_width, sample_thread_height, 1)];
      [sample_resolve_encoder endEncoding];
      ++context->multisample_resolve_count;

      if (presentation_snapshot) {
        id<MTLBlitCommandEncoder> snapshot_encoder = [command_buffer blitCommandEncoder];
        if (!snapshot_encoder) {
          return reject_and_drain("failed to create selected-sample snapshot blit encoder");
        }
        [snapshot_encoder copyFromBuffer:staging_buffer
                            sourceOffset:0
                       sourceBytesPerRow:row_pitch
                     sourceBytesPerImage:row_pitch * resolve_height
                              sourceSize:MTLSizeMake(resolve_width, resolve_height, 1)
                               toTexture:presentation_snapshot
                        destinationSlice:0
                        destinationLevel:0
                       destinationOrigin:MTLOriginMake(destination.presentation_snapshot_x,
                                                       destination.presentation_snapshot_y, 0)];
        [snapshot_encoder endEncoding];
      }
    } else {
      id<MTLBlitCommandEncoder> blit_encoder = [command_buffer blitCommandEncoder];
      if (!blit_encoder) {
        return reject_and_drain("failed to create tiled resolve blit encoder");
      }
      [blit_encoder copyFromTexture:context->render_texture
                        sourceSlice:0
                        sourceLevel:0
                       sourceOrigin:MTLOriginMake(source_x, source_y, 0)
                         sourceSize:MTLSizeMake(resolve_width, resolve_height, 1)
                           toBuffer:staging_buffer
                  destinationOffset:0
             destinationBytesPerRow:row_pitch
           destinationBytesPerImage:row_pitch * resolve_height];
      if (presentation_snapshot) {
        [blit_encoder copyFromTexture:context->render_texture
                          sourceSlice:0
                          sourceLevel:0
                         sourceOrigin:MTLOriginMake(source_x, source_y, 0)
                           sourceSize:MTLSizeMake(resolve_width, resolve_height, 1)
                            toTexture:presentation_snapshot
                     destinationSlice:0
                     destinationLevel:0
                    destinationOrigin:MTLOriginMake(destination.presentation_snapshot_x,
                                                    destination.presentation_snapshot_y, 0)];
      }
      [blit_encoder endEncoding];
    }

    if (!snapshot_only) {
      id<MTLComputeCommandEncoder> tiled_compute_encoder =
          [command_buffer computeCommandEncoder];
      if (!tiled_compute_encoder) {
        return reject_and_drain("failed to create tiled resolve compute encoder");
      }
      TiledResolveConstants constants = {
          uint32_t(row_pitch), uint32_t(destination.buffer_offset),
          destination.pitch,   destination.x,
          destination.y,       resolve_width,
          resolve_height,      destination.endian,
      };
      id<MTLComputePipelineState> pipeline_state =
          context->tiled_resolve_pipeline_state;
      [tiled_compute_encoder setComputePipelineState:pipeline_state];
      [tiled_compute_encoder setBuffer:staging_buffer offset:0 atIndex:0];
      [tiled_compute_encoder setBuffer:destination_buffer offset:0 atIndex:1];
      [tiled_compute_encoder setBytes:&constants length:sizeof(constants) atIndex:2];
      NSUInteger thread_width = std::max<NSUInteger>(
          1, std::min<NSUInteger>(resolve_width,
                                  [pipeline_state threadExecutionWidth]));
      NSUInteger max_threads = [pipeline_state maxTotalThreadsPerThreadgroup];
      NSUInteger thread_height = std::max<NSUInteger>(
          1, std::min<NSUInteger>(resolve_height, max_threads / thread_width));
      [tiled_compute_encoder
                dispatchThreads:MTLSizeMake(resolve_width, resolve_height, 1)
          threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1)];
      [tiled_compute_encoder endEncoding];
    }

    if (guest_memory_buffer) {
      id<MTLBlitCommandEncoder> mirror_encoder = [command_buffer blitCommandEncoder];
      if (!mirror_encoder) {
        return reject_and_drain("failed to create tiled resolve guest mirror blit encoder");
      }
      [mirror_encoder copyFromBuffer:destination_buffer
                        sourceOffset:destination.guest_memory_copy_source_offset
                            toBuffer:guest_memory_buffer
                   destinationOffset:destination.guest_memory_copy_destination_offset
                                size:destination.guest_memory_copy_length];
      [mirror_encoder endEncoding];
    }

    if (!bgra_out) {
      CommittedProbeCommandBuffer committed;
      committed.command_buffer = [command_buffer retain];
      committed.auxiliary_submission_count = 1;
      committed.async_failure_callback = destination.async_failure_callback;
      committed.async_failure_callback_context = destination.async_failure_callback_context;
      committed.async_failure_start = destination.async_failure_start;
      committed.async_failure_length = destination.async_failure_length;
      try {
        context->committed_command_buffers.push_back(committed);
      } catch (...) {
        [committed.command_buffer release];
        if (error_out) {
          *error_out = "failed to retain asynchronous tiled resolve submission";
        }
        return false;
      }
      if (destination.submission_callback && destination.submission_length) {
        destination.submission_callback(destination.submission_callback_context,
                                        destination.submission_start,
                                        destination.submission_length);
      }
      [command_buffer commit];
      return true;
    }

    if (destination.submission_callback && destination.submission_length) {
      destination.submission_callback(destination.submission_callback_context,
                                      destination.submission_start, destination.submission_length);
    }
    [command_buffer commit];
    [command_buffer waitUntilCompleted];

    std::string prior_error;
    bool prior_commands_succeeded = ConsumeCompletedPipelineProbeCommands(context, &prior_error);
    bool resolve_succeeded = [command_buffer status] == MTLCommandBufferStatusCompleted;
    if (!prior_commands_succeeded || !resolve_succeeded) {
      if (error_out) {
        error_out->clear();
        if (!prior_commands_succeeded) {
          error_out->append("prior render work failed: ");
          error_out->append(prior_error);
        }
        if (!resolve_succeeded) {
          if (!error_out->empty()) {
            error_out->append("; ");
          }
          NSError* command_error = [command_buffer error];
          const char* description =
              command_error ? [[command_error localizedDescription] UTF8String] : nullptr;
          error_out->append(description ? description : "tiled resolve blit or compute failed");
        }
      }
      return false;
    }

    if (bgra_out) {
      size_t tight_row_pitch = size_t(resolve_width) * 4;
      bgra_out->resize(tight_row_pitch * resolve_height);
      const uint8_t* source = static_cast<const uint8_t*>([staging_buffer contents]);
      if (row_pitch == tight_row_pitch) {
        std::memcpy(bgra_out->data(), source, tight_row_pitch * resolve_height);
      } else {
        for (uint32_t row = 0; row < resolve_height; ++row) {
          std::memcpy(bgra_out->data() + size_t(row) * tight_row_pitch,
                      source + size_t(row) * row_pitch, tight_row_pitch);
        }
      }
    }
    return true;
  }
}

bool ResolvePipelineProbeDepthStencilContextToXenosTiled(
    void* opaque_context, uint32_t width, uint32_t height, uint32_t source_x, uint32_t source_y,
    uint32_t resolve_width, uint32_t resolve_height, bool depth_float24, bool depth_float24_round,
    uint32_t depth_sample_select, const ProbeTiledResolveTarget& destination,
    bool wait_for_completion, std::string* error_out) {
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    if (!context || !context->depth_stencil_target) {
      if (error_out) {
        *error_out = "missing probe context or persistent depth/stencil target";
      }
      return false;
    }
    ProbeDepthStencilTarget* target = context->depth_stencil_target;

    auto reject_and_drain = [&](const std::string& reason) {
      std::string drain_error;
      bool drained = true;
      // A shared color context may own the open encoder that last wrote depth.
      // Invalid metadata is still a fence, so drain every attached context.
      for (PipelineProbeContext* attached_context : target->attached_contexts) {
        std::string attached_error;
        if (!WaitPendingPipelineProbeCommands(attached_context, &attached_error, nullptr)) {
          drained = false;
          if (drain_error.empty()) {
            drain_error = attached_error;
          }
        }
      }
      if (error_out) {
        *error_out = reason;
        if (!drained && !drain_error.empty()) {
          error_out->append("; prior depth/stencil work failed: ");
          error_out->append(drain_error);
        }
      }
      return false;
    };

    if (!target->texture || !target->initialized || !width || !height || target->width != width ||
        target->height != height ||
        [target->texture pixelFormat] != MTLPixelFormatDepth32Float_Stencil8) {
      return reject_and_drain(
          "persistent probe depth/stencil texture is unavailable or has a different size");
    }
    uint32_t host_sample = 0;
    if (!GetProbeDepthSample(target->sample_count, depth_sample_select, host_sample)) {
      return reject_and_drain("depth tiled resolve sample selection is invalid");
    }
    bool degenerate_zero_pitch = destination.pitch == 0;
    if (!resolve_width || !resolve_height || source_x >= width || source_y >= height ||
        resolve_width > width - source_x || resolve_height > height - source_y ||
        !destination.metal_buffer || !destination.height ||
        (!degenerate_zero_pitch && (destination.x >= destination.pitch ||
                                    resolve_width > destination.pitch - destination.x)) ||
        destination.y >= destination.height ||
        resolve_height > destination.height - destination.y || destination.endian > 3 ||
        (destination.buffer_offset & 3) || destination.buffer_offset > UINT32_MAX ||
        destination.x > UINT32_MAX - resolve_width || destination.y > UINT32_MAX - resolve_height) {
      return reject_and_drain("invalid depth tiled resolve rectangle, surface, buffer, or endian");
    }

    id<MTLBuffer> destination_buffer = (id<MTLBuffer>)destination.metal_buffer;
    id<MTLBuffer> guest_memory_buffer = (id<MTLBuffer>)destination.guest_memory_metal_buffer;
    uint32_t destination_right = destination.x + resolve_width;
    uint32_t destination_bottom = destination.y + resolve_height;
    uint32_t tiled_rect_upper_bound =
        GetTiledRgba8UpperBound(destination_right, destination_bottom, destination.pitch);
    uint64_t destination_end = uint64_t(destination.buffer_offset) + tiled_rect_upper_bound;
    if ([destination_buffer storageMode] != MTLStorageModeShared || !tiled_rect_upper_bound ||
        destination_end > [destination_buffer length] || destination_end > UINT32_MAX) {
      return reject_and_drain(
          "depth tiled resolve destination is not a sufficiently large shared Metal buffer");
    }
    if (guest_memory_buffer) {
      uint64_t mirror_source_end = uint64_t(destination.guest_memory_copy_source_offset) +
                                   destination.guest_memory_copy_length;
      uint64_t mirror_destination_end = uint64_t(destination.guest_memory_copy_destination_offset) +
                                        destination.guest_memory_copy_length;
      if (!destination.guest_memory_copy_length ||
          [guest_memory_buffer storageMode] != MTLStorageModeShared ||
          mirror_source_end > [destination_buffer length] ||
          mirror_destination_end > [guest_memory_buffer length]) {
        return reject_and_drain(
            "depth tiled resolve guest mirror range is invalid or not backed by shared storage");
      }
    } else if (destination.guest_memory_copy_length) {
      return reject_and_drain("depth tiled resolve guest mirror range has no destination buffer");
    }

    id<MTLTexture> snapshot = (id<MTLTexture>)destination.depth_snapshot_texture;
    uint32_t snapshot_width = snapshot ? uint32_t([snapshot width]) : 0;
    uint32_t snapshot_height = snapshot ? uint32_t([snapshot height]) : 0;
    if (snapshot &&
        ([snapshot device] != context->device || [snapshot pixelFormat] != MTLPixelFormatR32Float ||
         [snapshot textureType] != MTLTextureType2DArray || [snapshot arrayLength] < 1 ||
         destination.depth_snapshot_x >= snapshot_width ||
         destination.depth_snapshot_y >= snapshot_height ||
         resolve_width > snapshot_width - destination.depth_snapshot_x ||
         resolve_height > snapshot_height - destination.depth_snapshot_y)) {
      return reject_and_drain("depth resolve snapshot texture is incompatible");
    }
    id<MTLTexture> packed_snapshot =
        (id<MTLTexture>)destination.packed_depth_snapshot_texture;
    uint32_t packed_snapshot_width =
        packed_snapshot ? uint32_t([packed_snapshot width]) : 0;
    uint32_t packed_snapshot_height =
        packed_snapshot ? uint32_t([packed_snapshot height]) : 0;
    if (packed_snapshot &&
        ([packed_snapshot device] != context->device ||
         [packed_snapshot pixelFormat] != MTLPixelFormatRGBA8Unorm ||
         [packed_snapshot textureType] != MTLTextureType2DArray ||
         [packed_snapshot arrayLength] < 1 ||
         destination.packed_depth_snapshot_fetch_endian > 3 ||
         destination.packed_depth_snapshot_x >= packed_snapshot_width ||
         destination.packed_depth_snapshot_y >= packed_snapshot_height ||
         resolve_width >
             packed_snapshot_width - destination.packed_depth_snapshot_x ||
         resolve_height >
             packed_snapshot_height - destination.packed_depth_snapshot_y)) {
      return reject_and_drain(
          "packed depth resolve snapshot texture is incompatible");
    }

    std::string setup_error;
    if (!EnsureDepthTiledResolvePipelineStates(context, &setup_error)) {
      return reject_and_drain(setup_error);
    }

    // End any color encoder sharing this target. The resolve command uses the
    // same queue, so commit order is the required draw -> copy ordering without
    // a CPU stall.
    PipelineProbeContext* previous_owner = target->open_owner;
    if (previous_owner && !FinalizeOpenPipelineProbeCommandBuffer(previous_owner, &setup_error)) {
      return reject_and_drain(setup_error.empty()
                                  ? "failed to finalize pending depth/stencil render work"
                                  : setup_error);
    }
    if (target->open_owner) {
      return reject_and_drain("persistent depth/stencil target retained an open owner");
    }

    id<MTLTexture> stencil_view =
        [target->texture newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
    if (!stencil_view) {
      return reject_and_drain("failed to create the depth resolve stencil texture view");
    }
    id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
    if (!command_buffer) {
      [stencil_view release];
      return reject_and_drain("failed to create depth tiled resolve command buffer");
    }
    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    if (!encoder) {
      [stencil_view release];
      return reject_and_drain("failed to create depth tiled resolve compute encoder");
    }

    DepthTiledResolveConstants constants = {
        source_x,
        source_y,
        uint32_t(destination.buffer_offset),
        destination.pitch,
        destination.x,
        destination.y,
        resolve_width,
        resolve_height,
        destination.endian,
        host_sample,
        depth_float24 ? 1u : 0u,
        depth_float24_round ? 1u : 0u,
        snapshot ? 1u : 0u,
        destination.depth_snapshot_x,
        destination.depth_snapshot_y,
        packed_snapshot ? 1u : 0u,
        destination.packed_depth_snapshot_x,
        destination.packed_depth_snapshot_y,
        destination.packed_depth_snapshot_fetch_endian,
    };
    id<MTLComputePipelineState> pipeline_state =
        target->sample_count > 1 ? context->multisample_depth_tiled_resolve_pipeline_state
                                 : context->depth_tiled_resolve_pipeline_state;
    [encoder setComputePipelineState:pipeline_state];
    [encoder setTexture:target->texture atIndex:0];
    [encoder setTexture:stencil_view atIndex:1];
    [encoder setTexture:snapshot ? snapshot : context->depth_resolve_dummy_snapshot_texture
                atIndex:2];
    [encoder
          setTexture:packed_snapshot
                         ? packed_snapshot
                         : context->depth_resolve_dummy_packed_snapshot_texture
             atIndex:3];
    [encoder setBuffer:destination_buffer offset:0 atIndex:0];
    [encoder setBytes:&constants length:sizeof(constants) atIndex:1];
    NSUInteger thread_width = std::max<NSUInteger>(
        1, std::min<NSUInteger>(resolve_width, [pipeline_state threadExecutionWidth]));
    NSUInteger max_threads = [pipeline_state maxTotalThreadsPerThreadgroup];
    NSUInteger thread_height =
        std::max<NSUInteger>(1, std::min<NSUInteger>(resolve_height, max_threads / thread_width));
    [encoder dispatchThreads:MTLSizeMake(resolve_width, resolve_height, 1)
        threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1)];
    [encoder endEncoding];

    if (guest_memory_buffer) {
      id<MTLBlitCommandEncoder> mirror_encoder = [command_buffer blitCommandEncoder];
      if (!mirror_encoder) {
        [stencil_view release];
        return reject_and_drain("failed to create depth tiled resolve guest mirror encoder");
      }
      [mirror_encoder copyFromBuffer:destination_buffer
                        sourceOffset:destination.guest_memory_copy_source_offset
                            toBuffer:guest_memory_buffer
                   destinationOffset:destination.guest_memory_copy_destination_offset
                                size:destination.guest_memory_copy_length];
      [mirror_encoder endEncoding];
    }

    if (destination.submission_callback && destination.submission_length) {
      destination.submission_callback(destination.submission_callback_context,
                                      destination.submission_start, destination.submission_length);
    }
    if (!wait_for_completion) {
      CommittedProbeCommandBuffer committed;
      committed.command_buffer = [command_buffer retain];
      committed.auxiliary_submission_count = 1;
      committed.async_failure_callback = destination.async_failure_callback;
      committed.async_failure_callback_context = destination.async_failure_callback_context;
      committed.async_failure_start = destination.async_failure_start;
      committed.async_failure_length = destination.async_failure_length;
      try {
        context->committed_command_buffers.push_back(committed);
      } catch (...) {
        [committed.command_buffer release];
        [stencil_view release];
        if (error_out) {
          *error_out = "failed to retain asynchronous depth tiled resolve submission";
        }
        return false;
      }
      [command_buffer commit];
      [stencil_view release];
      return true;
    }

    [command_buffer commit];
    [stencil_view release];
    [command_buffer waitUntilCompleted];
    std::string prior_error;
    bool prior_commands_succeeded = ConsumeCompletedPipelineProbeCommands(context, &prior_error);
    if (previous_owner && previous_owner != context) {
      std::string owner_error;
      if (!ConsumeCompletedPipelineProbeCommands(previous_owner, &owner_error)) {
        prior_commands_succeeded = false;
        if (!owner_error.empty()) {
          if (!prior_error.empty()) {
            prior_error.append("; ");
          }
          prior_error.append(owner_error);
        }
      }
    }
    bool resolve_succeeded = [command_buffer status] == MTLCommandBufferStatusCompleted;
    if (!prior_commands_succeeded || !resolve_succeeded) {
      if (error_out) {
        error_out->clear();
        if (!prior_commands_succeeded) {
          error_out->append("prior depth/stencil work failed: ");
          error_out->append(prior_error);
        }
        if (!resolve_succeeded) {
          if (!error_out->empty()) {
            error_out->append("; ");
          }
          NSError* command_error = [command_buffer error];
          const char* description =
              command_error ? [[command_error localizedDescription] UTF8String] : nullptr;
          error_out->append(description ? description : "depth tiled resolve compute failed");
        }
      }
      return false;
    }
    return true;
  }
}

bool ReadPipelineProbeContext(void* opaque_context, uint32_t width, uint32_t height,
                              std::vector<uint8_t>& bgra_out, std::string* error_out) {
  return ReadPipelineProbeContextRect(opaque_context, width, height, 0, 0, width, height, bgra_out,
                                      error_out);
}

bool ExportPipelineProbeColorToCanonicalEdram(
    void* opaque_context, uint32_t width, uint32_t height,
    const CanonicalEdramSurfaceLayout& layout, xenos::ColorRenderTargetFormat format,
    void* canonical_edram, size_t canonical_edram_size, std::string* error_out) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  if (!context || !canonical_edram || canonical_edram_size != xenos::kEdramSizeBytes || !width ||
      !height || !layout.pitch_tiles || layout.is_depth ||
      layout.is_64bpp != xenos::IsColorRenderTargetFormat64bpp(format) ||
      !IsCanonicalEdramMsaaSupportedByMetal(layout.msaa_samples)) {
    if (error_out) {
      *error_out = "invalid canonical color export context, surface, or EDRAM destination";
    }
    return false;
  }
  // The current native target is BGRA8Unorm. Other guest formats need a
  // format-capable target (or raw blend sidecar) before they can be exported
  // without inventing precision that the private target never retained.
  if (!IsCanonicalEdramColorFormatSupportedByMetal(format)) {
    if (error_out) {
      *error_out = "Metal BGRA8 private targets cannot canonically export this Xenos color format";
    }
    return false;
  }
  const uint32_t sample_count = GetCanonicalEdramSampleCount(layout.msaa_samples);
  if (context->sample_count != sample_count) {
    if (error_out) {
      *error_out = "canonical color export sample count does not match the private target";
    }
    return false;
  }
  std::span<uint8_t> edram(static_cast<uint8_t*>(canonical_edram), canonical_edram_size);
  for (uint32_t sample = 0; sample < sample_count; ++sample) {
    std::vector<uint8_t> bgra;
    std::string read_error;
    if (!ReadPipelineProbeContextRectSampleSelected(opaque_context, width, height, 0, 0, width,
                                                    height, sample, bgra, &read_error)) {
      if (error_out) {
        *error_out = "canonical color sample read failed";
        if (!read_error.empty()) {
          error_out->append(": ");
          error_out->append(read_error);
        }
      }
      return false;
    }
    if (bgra.size() != size_t(width) * height * 4) {
      if (error_out) {
        *error_out = "canonical color sample read returned an invalid byte count";
      }
      return false;
    }
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const uint8_t* pixel = bgra.data() + (size_t(y) * width + x) * 4;
        std::array<float, 4> rgba = {
            float(pixel[2]) * (1.0f / 255.0f), float(pixel[1]) * (1.0f / 255.0f),
            float(pixel[0]) * (1.0f / 255.0f), float(pixel[3]) * (1.0f / 255.0f)};
        std::array<uint32_t, 2> words;
        if (!PackCanonicalEdramColor(rgba, format, words) ||
            !WriteCanonicalEdramSample(edram, layout, x, y, sample, words)) {
          if (error_out) {
            *error_out = "canonical color packing failed";
          }
          return false;
        }
      }
    }
  }
  return true;
}

bool RestorePipelineProbeColorFromCanonicalEdram(
    void* opaque_context, uint32_t width, uint32_t height,
    const CanonicalEdramSurfaceLayout& layout, xenos::ColorRenderTargetFormat format,
    const void* canonical_edram, size_t canonical_edram_size, std::string* error_out) {
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    if (!context || !canonical_edram || canonical_edram_size != xenos::kEdramSizeBytes || !width ||
        !height || !layout.pitch_tiles || layout.is_depth ||
        layout.is_64bpp != xenos::IsColorRenderTargetFormat64bpp(format) ||
        !IsCanonicalEdramMsaaSupportedByMetal(layout.msaa_samples)) {
      if (error_out) {
        *error_out = "invalid canonical color restore context, surface, or EDRAM source";
      }
      return false;
    }
    if (!IsCanonicalEdramColorFormatSupportedByMetal(format)) {
      if (error_out) {
        *error_out =
            "Metal BGRA8 private targets cannot canonically restore this Xenos color format";
      }
      return false;
    }
    const uint32_t sample_count = GetCanonicalEdramSampleCount(layout.msaa_samples);
    if (context->sample_count != sample_count ||
        !WaitPendingPipelineProbeCommands(context, error_out, nullptr) ||
        !EnsureProbeContextTexture(context, width, height, error_out)) {
      return false;
    }

    static constexpr char kCanonicalColorRestoreMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
vertex float4 canonical_color_vertex(uint vertex_id [[vertex_id]]) {
  float2 positions[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
  return float4(positions[vertex_id], 0.0, 1.0);
}
struct CanonicalColorResult {
  float4 color [[color(0)]];
  uint sample_mask [[sample_mask]];
};
fragment CanonicalColorResult canonical_color_fragment(
    float4 position [[position]], texture2d<float, access::read> source [[texture(0)]],
    constant uint& sample_mask [[buffer(0)]]) {
  CanonicalColorResult result;
  result.color = source.read(uint2(position.xy));
  result.sample_mask = sample_mask;
  return result;
}
)MSL";
    NSError* error = nil;
    id<MTLLibrary> library = [context->device
        newLibraryWithSource:[NSString stringWithUTF8String:kCanonicalColorRestoreMsl]
                     options:nil
                       error:&error];
    id<MTLFunction> vertex =
        library ? [library newFunctionWithName:@"canonical_color_vertex"] : nil;
    id<MTLFunction> fragment =
        library ? [library newFunctionWithName:@"canonical_color_fragment"] : nil;
    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.rasterSampleCount = sample_count;
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    descriptor.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    id<MTLRenderPipelineState> pipeline =
        vertex && fragment ? [context->device newRenderPipelineStateWithDescriptor:descriptor
                                                                              error:&error]
                           : nil;
    [descriptor release];
    [vertex release];
    [fragment release];
    [library release];
    if (!pipeline) {
      if (error_out) {
        *error_out = error ? [[error localizedDescription] UTF8String]
                           : "canonical color restore pipeline creation failed";
      }
      return false;
    }

    std::span<const uint8_t> edram(static_cast<const uint8_t*>(canonical_edram),
                                   canonical_edram_size);
    std::vector<id<MTLTexture>> sample_textures;
    sample_textures.reserve(sample_count);
    bool textures_ok = true;
    for (uint32_t sample = 0; sample < sample_count; ++sample) {
      std::vector<uint8_t> bgra(size_t(width) * height * 4);
      for (uint32_t y = 0; y < height && textures_ok; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
          std::array<uint32_t, 2> words;
          std::array<float, 4> rgba;
          if (!ReadCanonicalEdramSample(edram, layout, x, y, sample, words) ||
              !UnpackCanonicalEdramColor(words, format, rgba)) {
            textures_ok = false;
            break;
          }
          auto to_byte = [](float value) {
            if (std::isnan(value)) {
              value = 0.0f;
            }
            return uint8_t(std::clamp(std::floor(value * 255.0f + 0.5f), 0.0f, 255.0f));
          };
          uint8_t* pixel = bgra.data() + (size_t(y) * width + x) * 4;
          pixel[0] = to_byte(rgba[2]);
          pixel[1] = to_byte(rgba[1]);
          pixel[2] = to_byte(rgba[0]);
          pixel[3] = to_byte(rgba[3]);
        }
      }
      MTLTextureDescriptor* source_descriptor =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                             width:width
                                                            height:height
                                                         mipmapped:NO];
      source_descriptor.storageMode = MTLStorageModeShared;
      source_descriptor.usage = MTLTextureUsageShaderRead;
      id<MTLTexture> source =
          textures_ok ? [context->device newTextureWithDescriptor:source_descriptor] : nil;
      if (source) {
        [source replaceRegion:MTLRegionMake2D(0, 0, width, height)
                  mipmapLevel:0
                    withBytes:bgra.data()
                  bytesPerRow:size_t(width) * 4];
        sample_textures.push_back(source);
      } else {
        textures_ok = false;
      }
    }
    if (!textures_ok) {
      for (id<MTLTexture> texture : sample_textures) {
        [texture release];
      }
      [pipeline release];
      if (error_out) {
        *error_out = "canonical color restore source conversion or allocation failed";
      }
      return false;
    }

    id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
    bool encoded = command_buffer != nil;
    for (uint32_t sample = 0; sample < sample_count && encoded; ++sample) {
      if (!PrepareProbeDepthStencilSubmission(context, false, error_out)) {
        encoded = false;
        break;
      }
      MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
      ConfigureProbeColorPass(pass, context,
                              sample ? MTLLoadActionLoad : MTLLoadActionClear,
                              MTLClearColorMake(0.0, 0.0, 0.0, 0.0));
      ConfigureProbeDepthStencilPass(
          pass, context->depth_stencil_target->texture,
          context->depth_stencil_target->initialized ? MTLLoadActionLoad : MTLLoadActionClear);
      id<MTLRenderCommandEncoder> encoder =
          [command_buffer renderCommandEncoderWithDescriptor:pass];
      if (!encoder) {
        encoded = false;
        break;
      }
      uint32_t host_sample_mask = 0;
      if (!GetProbeColorSampleMask(sample_count, sample, host_sample_mask)) {
        [encoder endEncoding];
        encoded = false;
        break;
      }
      [encoder setRenderPipelineState:pipeline];
      [encoder setFragmentTexture:sample_textures[sample] atIndex:0];
      [encoder setFragmentBytes:&host_sample_mask length:sizeof(host_sample_mask)
                        atIndex:0];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      [encoder endEncoding];
    }
    if (encoded) {
      [command_buffer commit];
      [command_buffer waitUntilCompleted];
      encoded = [command_buffer status] == MTLCommandBufferStatusCompleted;
    }
    for (id<MTLTexture> texture : sample_textures) {
      [texture release];
    }
    [pipeline release];
    if (!encoded) {
      if (error_out && error_out->empty()) {
        NSError* command_error = command_buffer ? [command_buffer error] : nil;
        *error_out = command_error ? [[command_error localizedDescription] UTF8String]
                                   : "canonical color restore command failed";
      }
      return false;
    }
    context->initialized = true;
    context->color_resolve_dirty = sample_count > 1;
    // The import render pass loads the existing shared depth target, or
    // deterministically initializes a previously absent one to 1/0.
    context->depth_stencil_target->initialized = true;
    return true;
  }
}

bool ExportPipelineProbeDepthStencilToCanonicalEdram(
    void* opaque_context, uint32_t width, uint32_t height,
    const CanonicalEdramSurfaceLayout& layout, xenos::DepthRenderTargetFormat format,
    bool float24_round, void* canonical_edram, size_t canonical_edram_size,
    std::string* error_out) {
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    if (!context || !canonical_edram || canonical_edram_size != xenos::kEdramSizeBytes || !width ||
        !height || !layout.pitch_tiles || !layout.is_depth || layout.is_64bpp ||
        !IsCanonicalEdramDepthFormatSupportedByMetal(format) ||
        !IsCanonicalEdramMsaaSupportedByMetal(layout.msaa_samples)) {
      if (error_out) {
        *error_out = "invalid canonical depth export context, surface, or EDRAM destination";
      }
      return false;
    }
    const uint32_t sample_count = GetCanonicalEdramSampleCount(layout.msaa_samples);
    if (context->sample_count != sample_count) {
      if (error_out) {
        *error_out = "canonical depth export sample count does not match the private target";
      }
      return false;
    }
    const uint32_t tiled_extent = GetTiledRgba8UpperBound(width, height, width);
    id<MTLBuffer> tiled_buffer =
        tiled_extent ? [context->device newBufferWithLength:tiled_extent
                                                   options:MTLResourceStorageModeShared]
                     : nil;
    if (!tiled_buffer) {
      if (error_out) {
        *error_out = "failed to allocate canonical depth export staging";
      }
      return false;
    }
    std::span<uint8_t> edram(static_cast<uint8_t*>(canonical_edram), canonical_edram_size);
    bool succeeded = true;
    for (uint32_t sample = 0; sample < sample_count && succeeded; ++sample) {
      std::memset([tiled_buffer contents], 0, tiled_extent);
      ProbeTiledResolveTarget destination;
      destination.metal_buffer = tiled_buffer;
      destination.pitch = width;
      destination.height = height;
      destination.endian = 0;
      std::string resolve_error;
      if (!ResolvePipelineProbeDepthStencilContextToXenosTiled(
              opaque_context, width, height, 0, 0, width, height,
              format == xenos::DepthRenderTargetFormat::kD24FS8, float24_round, sample,
              destination, true, &resolve_error)) {
        succeeded = false;
        if (error_out) {
          *error_out = "canonical depth sample read failed";
          if (!resolve_error.empty()) {
            error_out->append(": ");
            error_out->append(resolve_error);
          }
        }
        break;
      }
      const uint8_t* tiled = static_cast<const uint8_t*>([tiled_buffer contents]);
      for (uint32_t y = 0; y < height && succeeded; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
          uint32_t packed = 0;
          std::memcpy(&packed, tiled + GetTiledRgba8Offset(x, y, width), sizeof(packed));
          if (!WriteCanonicalEdramSample(edram, layout, x, y, sample, {packed, 0})) {
            succeeded = false;
            break;
          }
        }
      }
    }
    [tiled_buffer release];
    if (!succeeded && error_out && error_out->empty()) {
      *error_out = "canonical depth packing failed";
    }
    return succeeded;
  }
}

bool RestorePipelineProbeDepthStencilFromCanonicalEdram(
    void* opaque_context, uint32_t width, uint32_t height,
    const CanonicalEdramSurfaceLayout& layout, xenos::DepthRenderTargetFormat format,
    const void* canonical_edram, size_t canonical_edram_size, std::string* error_out) {
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    if (!context || !canonical_edram || canonical_edram_size != xenos::kEdramSizeBytes || !width ||
        !height || !layout.pitch_tiles || !layout.is_depth || layout.is_64bpp ||
        !IsCanonicalEdramDepthFormatSupportedByMetal(format) ||
        !IsCanonicalEdramMsaaSupportedByMetal(layout.msaa_samples)) {
      if (error_out) {
        *error_out = "invalid canonical depth restore context, surface, or EDRAM source";
      }
      return false;
    }
    const uint32_t sample_count = GetCanonicalEdramSampleCount(layout.msaa_samples);
    if (context->sample_count != sample_count ||
        !WaitPendingPipelineProbeCommands(context, error_out, nullptr) ||
        !EnsureProbeContextTexture(context, width, height, error_out) ||
        !PrepareProbeDepthStencilSubmission(context, false, error_out)) {
      return false;
    }

    const size_t pixel_count = size_t(width) * height;
    std::vector<float> depth_values(pixel_count * sample_count);
    std::vector<uint8_t> stencil_values(pixel_count * sample_count);
    std::array<std::array<bool, 256>, 4> used_stencils = {};
    std::span<const uint8_t> edram(static_cast<const uint8_t*>(canonical_edram),
                                   canonical_edram_size);
    for (uint32_t sample = 0; sample < sample_count; ++sample) {
      for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
          std::array<uint32_t, 2> words;
          if (!ReadCanonicalEdramSample(edram, layout, x, y, sample, words)) {
            if (error_out) {
              *error_out = "canonical depth unpacking failed";
            }
            return false;
          }
          size_t index = size_t(sample) * pixel_count + size_t(y) * width + x;
          uint32_t depth24 = words[0] >> 8;
          depth_values[index] =
              format == xenos::DepthRenderTargetFormat::kD24FS8
                  ? xenos::Float20e4To32(depth24) * 0.5f
                  : xenos::UNorm24To32(depth24);
          stencil_values[index] = uint8_t(words[0]);
          used_stencils[sample][stencil_values[index]] = true;
        }
      }
    }

    MTLTextureDescriptor* depth_source_descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    depth_source_descriptor.textureType = MTLTextureType2DArray;
    depth_source_descriptor.arrayLength = sample_count;
    depth_source_descriptor.storageMode = MTLStorageModeShared;
    depth_source_descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> depth_source =
        [context->device newTextureWithDescriptor:depth_source_descriptor];
    depth_source_descriptor.pixelFormat = MTLPixelFormatR8Uint;
    id<MTLTexture> stencil_source =
        [context->device newTextureWithDescriptor:depth_source_descriptor];
    if (!depth_source || !stencil_source) {
      [depth_source release];
      [stencil_source release];
      if (error_out) {
        *error_out = "failed to allocate canonical depth restore sources";
      }
      return false;
    }
    for (uint32_t sample = 0; sample < sample_count; ++sample) {
      [depth_source replaceRegion:MTLRegionMake2D(0, 0, width, height)
                      mipmapLevel:0
                            slice:sample
                        withBytes:depth_values.data() + size_t(sample) * pixel_count
                      bytesPerRow:size_t(width) * sizeof(float)
                    bytesPerImage:pixel_count * sizeof(float)];
      [stencil_source replaceRegion:MTLRegionMake2D(0, 0, width, height)
                        mipmapLevel:0
                              slice:sample
                          withBytes:stencil_values.data() + size_t(sample) * pixel_count
                        bytesPerRow:width
                      bytesPerImage:pixel_count];
    }

    static constexpr char kCanonicalDepthRestoreMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct RestoreConstants { uint sample; uint stencil; uint sample_mask; };
struct RestoreResult {
  float depth [[depth(any)]];
  uint sample_mask [[sample_mask]];
};
vertex float4 canonical_depth_vertex(uint vertex_id [[vertex_id]]) {
  float2 positions[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
  return float4(positions[vertex_id], 0.0, 1.0);
}
fragment RestoreResult canonical_depth_fragment(
    float4 position [[position]], texture2d_array<float, access::read> depths [[texture(0)]],
    texture2d_array<uint, access::read> stencils [[texture(1)]],
    constant RestoreConstants& constants [[buffer(0)]]) {
  uint2 coordinate = uint2(position.xy);
  if (stencils.read(coordinate, constants.sample).x != constants.stencil) {
    discard_fragment();
  }
  RestoreResult result;
  result.depth = depths.read(coordinate, constants.sample).x;
  result.sample_mask = constants.sample_mask;
  return result;
}
)MSL";
    NSError* error = nil;
    id<MTLLibrary> library = [context->device
        newLibraryWithSource:[NSString stringWithUTF8String:kCanonicalDepthRestoreMsl]
                     options:nil
                       error:&error];
    id<MTLFunction> vertex =
        library ? [library newFunctionWithName:@"canonical_depth_vertex"] : nil;
    id<MTLFunction> fragment =
        library ? [library newFunctionWithName:@"canonical_depth_fragment"] : nil;
    MTLRenderPipelineDescriptor* pipeline_descriptor =
        [[MTLRenderPipelineDescriptor alloc] init];
    pipeline_descriptor.vertexFunction = vertex;
    pipeline_descriptor.fragmentFunction = fragment;
    pipeline_descriptor.rasterSampleCount = sample_count;
    pipeline_descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pipeline_descriptor.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
    pipeline_descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    pipeline_descriptor.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    id<MTLRenderPipelineState> pipeline =
        vertex && fragment
            ? [context->device newRenderPipelineStateWithDescriptor:pipeline_descriptor
                                                              error:&error]
            : nil;
    [pipeline_descriptor release];
    [vertex release];
    [fragment release];
    [library release];

    MTLDepthStencilDescriptor* state_descriptor = [[MTLDepthStencilDescriptor alloc] init];
    state_descriptor.depthCompareFunction = MTLCompareFunctionAlways;
    state_descriptor.depthWriteEnabled = YES;
    MTLStencilDescriptor* stencil_descriptor = [[MTLStencilDescriptor alloc] init];
    stencil_descriptor.stencilCompareFunction = MTLCompareFunctionAlways;
    stencil_descriptor.depthStencilPassOperation = MTLStencilOperationReplace;
    stencil_descriptor.readMask = 0xFF;
    stencil_descriptor.writeMask = 0xFF;
    state_descriptor.frontFaceStencil = stencil_descriptor;
    state_descriptor.backFaceStencil = stencil_descriptor;
    id<MTLDepthStencilState> depth_stencil_state =
        [context->device newDepthStencilStateWithDescriptor:state_descriptor];
    [stencil_descriptor release];
    [state_descriptor release];
    if (!pipeline || !depth_stencil_state) {
      [pipeline release];
      [depth_stencil_state release];
      [depth_source release];
      [stencil_source release];
      if (error_out) {
        *error_out = error ? [[error localizedDescription] UTF8String]
                           : "canonical depth restore pipeline creation failed";
      }
      return false;
    }

    id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
    bool encoded = command_buffer != nil;
    bool first_pass = true;
    for (uint32_t sample = 0; sample < sample_count && encoded; ++sample) {
      uint32_t host_sample = 0;
      if (!GetProbeDepthSample(sample_count, sample, host_sample)) {
        encoded = false;
        break;
      }
      for (uint32_t stencil = 0; stencil < 256; ++stencil) {
        if (!used_stencils[sample][stencil]) {
          continue;
        }
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        ConfigureProbeColorPass(pass, context,
                                context->initialized || !first_pass ? MTLLoadActionLoad
                                                                    : MTLLoadActionClear,
                                MTLClearColorMake(0.0, 0.0, 0.0, 0.0));
        ConfigureProbeDepthStencilPass(
            pass, context->depth_stencil_target->texture,
            first_pass ? MTLLoadActionClear : MTLLoadActionLoad);
        id<MTLRenderCommandEncoder> encoder =
            [command_buffer renderCommandEncoderWithDescriptor:pass];
        if (!encoder) {
          encoded = false;
          break;
        }
        struct {
          uint32_t sample;
          uint32_t stencil;
          uint32_t sample_mask;
        } constants = {sample, stencil, uint32_t(1) << host_sample};
        [encoder setRenderPipelineState:pipeline];
        [encoder setDepthStencilState:depth_stencil_state];
        [encoder setStencilFrontReferenceValue:stencil backReferenceValue:stencil];
        [encoder setFragmentTexture:depth_source atIndex:0];
        [encoder setFragmentTexture:stencil_source atIndex:1];
        [encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        first_pass = false;
      }
    }
    if (encoded) {
      [command_buffer commit];
      [command_buffer waitUntilCompleted];
      encoded = [command_buffer status] == MTLCommandBufferStatusCompleted;
    }
    [pipeline release];
    [depth_stencil_state release];
    [depth_source release];
    [stencil_source release];
    if (!encoded) {
      if (error_out && error_out->empty()) {
        NSError* command_error = command_buffer ? [command_buffer error] : nil;
        *error_out = command_error ? [[command_error localizedDescription] UTF8String]
                                   : "canonical depth restore command failed";
      }
      return false;
    }
    context->initialized = true;
    context->color_resolve_dirty = sample_count > 1;
    context->depth_stencil_target->initialized = true;
    return true;
  }
}

bool RenderPipelineProbe(
    void* metal_device, void* pipeline_state, const void* system_constants,
    size_t system_constants_size, const void* float_constants, size_t float_constants_size,
    const void* fetch_constants, size_t fetch_constants_size, void* shared_memory,
    size_t shared_memory_size, void* shared_memory_metal_buffer,
    const ProbeTextureSlot* vertex_textures, size_t vertex_texture_count,
    size_t vertex_sampler_count, const ProbeTextureSlot* fragment_textures,
    size_t fragment_texture_count, size_t fragment_sampler_count, uint32_t primitive_type,
    uint32_t vertex_count, uint32_t width, uint32_t height, std::vector<uint8_t>& bgra_out,
    std::string* error_out, uint32_t vertex_shared_memory_buffer_index,
    uint32_t vertex_float_constants_buffer_index, uint32_t vertex_fetch_constants_buffer_index,
    const uint8_t* initial_bgra, size_t initial_bgra_row_pitch,
    const void* fragment_float_constants, size_t fragment_float_constants_size,
    uint32_t fragment_float_constants_buffer_index, uint32_t fragment_fetch_constants_buffer_index,
    const ProbeSamplerSlot* vertex_samplers, const ProbeSamplerSlot* fragment_samplers,
    const void* vertex_data, size_t vertex_data_size, uint32_t vertex_data_buffer_index,
    const void* bool_loop_constants, size_t bool_loop_constants_size,
    uint32_t vertex_bool_loop_constants_buffer_index,
    uint32_t fragment_bool_loop_constants_buffer_index, const ProbeIndexBuffer* index_buffer,
    const ProbeRasterizationState* rasterization_state,
    const ProbeDepthStencilState* depth_stencil_state) {
  if (!metal_device || !pipeline_state || !system_constants || !system_constants_size || !width ||
      !height || !vertex_count) {
    if (error_out) {
      *error_out = "missing Metal device, pipeline state, system constants, or target size";
    }
    return false;
  }
  if (!IsProbeIndexBufferValid(index_buffer, vertex_count)) {
    if (error_out) {
      *error_out = "invalid probe index buffer";
    }
    return false;
  }
  if (!IsProbeRasterizationStateValid(rasterization_state, width, height)) {
    if (error_out) {
      *error_out = "invalid probe rasterization state";
    }
    return false;
  }
  if (!IsProbeDepthStencilStateValid(depth_stencil_state)) {
    if (error_out) {
      *error_out = "invalid probe depth/stencil state";
    }
    return false;
  }

  id<MTLDevice> device = (id<MTLDevice>)metal_device;
  id<MTLCommandQueue> command_queue = [device newCommandQueue];
  if (!command_queue) {
    if (error_out) {
      *error_out = "failed to create command queue";
    }
    return false;
  }

  MTLTextureDescriptor* texture_descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  texture_descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  texture_descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> render_texture = [device newTextureWithDescriptor:texture_descriptor];
  MTLTextureDescriptor* depth_stencil_descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  depth_stencil_descriptor.usage = MTLTextureUsageRenderTarget;
  depth_stencil_descriptor.storageMode = MTLStorageModePrivate;
  id<MTLTexture> depth_stencil_texture = [device newTextureWithDescriptor:depth_stencil_descriptor];
  if (!render_texture || !depth_stencil_texture) {
    if (render_texture) {
      [render_texture release];
    }
    if (depth_stencil_texture) {
      [depth_stencil_texture release];
    }
    [command_queue release];
    if (error_out) {
      *error_out = "failed to create color or depth/stencil render texture";
    }
    return false;
  }

  ProbeDepthStencilState disabled_depth_stencil_state;
  id<MTLDepthStencilState> metal_depth_stencil_state = CreateProbeDepthStencilState(
      device, depth_stencil_state ? *depth_stencil_state : disabled_depth_stencil_state);
  if (!metal_depth_stencil_state) {
    [depth_stencil_texture release];
    [render_texture release];
    [command_queue release];
    if (error_out) {
      *error_out = "failed to create probe depth/stencil state";
    }
    return false;
  }

  id<MTLBuffer> system_buffer = [device newBufferWithBytes:system_constants
                                                    length:system_constants_size
                                                   options:MTLResourceStorageModeShared];
  id<MTLBuffer> float_buffer = nil;
  if (float_constants && float_constants_size) {
    float_buffer = [device newBufferWithBytes:float_constants
                                       length:float_constants_size
                                      options:MTLResourceStorageModeShared];
  }
  id<MTLBuffer> fragment_float_buffer = nil;
  if (fragment_float_constants && fragment_float_constants_size) {
    fragment_float_buffer = [device newBufferWithBytes:fragment_float_constants
                                                length:fragment_float_constants_size
                                               options:MTLResourceStorageModeShared];
  }
  id<MTLBuffer> fetch_buffer = nil;
  if (fetch_constants && fetch_constants_size) {
    fetch_buffer = [device newBufferWithBytes:fetch_constants
                                       length:fetch_constants_size
                                      options:MTLResourceStorageModeShared];
  }
  id<MTLBuffer> bool_loop_buffer = nil;
  if (bool_loop_constants && bool_loop_constants_size) {
    bool_loop_buffer = [device newBufferWithBytes:bool_loop_constants
                                           length:bool_loop_constants_size
                                          options:MTLResourceStorageModeShared];
  }
  id<MTLBuffer> vertex_data_buffer = nil;
  if (vertex_data && vertex_data_size && vertex_data_buffer_index != UINT32_MAX) {
    vertex_data_buffer = [device newBufferWithBytes:vertex_data
                                             length:vertex_data_size
                                            options:MTLResourceStorageModeShared];
  }
  id<MTLBuffer> index_buffer_object = nil;
  if (index_buffer) {
    if (index_buffer->metal_buffer) {
      index_buffer_object = [(id<MTLBuffer>)index_buffer->metal_buffer retain];
    } else {
      index_buffer_object = [device newBufferWithBytes:index_buffer->data
                                                length:index_buffer->size
                                               options:MTLResourceStorageModeShared];
    }
  }
  uint32_t shared_dummy_words[4] = {};
  id<MTLBuffer> shared_memory_buffer = nil;
  if (shared_memory_metal_buffer) {
    shared_memory_buffer = [(id<MTLBuffer>)shared_memory_metal_buffer retain];
  } else if (shared_memory && shared_memory_size) {
    shared_memory_buffer = [device newBufferWithBytesNoCopy:shared_memory
                                                     length:shared_memory_size
                                                    options:MTLResourceStorageModeShared
                                                deallocator:nil];
  }
  if (!shared_memory_buffer && vertex_shared_memory_buffer_index == UINT32_MAX) {
    shared_memory_buffer = [device newBufferWithBytes:shared_dummy_words
                                               length:sizeof(shared_dummy_words)
                                              options:MTLResourceStorageModeShared];
  }

  if (!system_buffer || !shared_memory_buffer || (index_buffer && !index_buffer_object)) {
    if (system_buffer) {
      [system_buffer release];
    }
    if (float_buffer) {
      [float_buffer release];
    }
    if (fragment_float_buffer) {
      [fragment_float_buffer release];
    }
    if (fetch_buffer) {
      [fetch_buffer release];
    }
    if (bool_loop_buffer) {
      [bool_loop_buffer release];
    }
    if (vertex_data_buffer) {
      [vertex_data_buffer release];
    }
    if (index_buffer_object) {
      [index_buffer_object release];
    }
    if (shared_memory_buffer) {
      [shared_memory_buffer release];
    }
    [render_texture release];
    [depth_stencil_texture release];
    if (metal_depth_stencil_state) {
      [metal_depth_stencil_state release];
    }
    [command_queue release];
    if (error_out) {
      *error_out = index_buffer && !index_buffer_object
                       ? "failed to create probe index buffer"
                       : (vertex_shared_memory_buffer_index != UINT32_MAX && !shared_memory_buffer
                              ? "required shared-memory buffer is unavailable"
                              : "failed to create argument buffers");
    }
    return false;
  }

  MTLTextureDescriptor* dummy_texture_descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:1
                                                        height:1
                                                     mipmapped:NO];
  dummy_texture_descriptor.textureType = MTLTextureType2DArray;
  dummy_texture_descriptor.arrayLength = 1;
  dummy_texture_descriptor.usage = MTLTextureUsageShaderRead;
  dummy_texture_descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> dummy_texture = [device newTextureWithDescriptor:dummy_texture_descriptor];
  uint32_t dummy_texture_pixel = 0x00000000u;
  if (dummy_texture) {
    MTLRegion region = MTLRegionMake3D(0, 0, 0, 1, 1, 1);
    [dummy_texture replaceRegion:region
                     mipmapLevel:0
                           slice:0
                       withBytes:&dummy_texture_pixel
                     bytesPerRow:4
                   bytesPerImage:4];
  }
  MTLSamplerDescriptor* sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
  sampler_descriptor.minFilter = MTLSamplerMinMagFilterNearest;
  sampler_descriptor.magFilter = MTLSamplerMinMagFilterNearest;
  sampler_descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
  sampler_descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
  sampler_descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
  id<MTLSamplerState> dummy_sampler = [device newSamplerStateWithDescriptor:sampler_descriptor];
  [sampler_descriptor release];
  std::vector<id<MTLTexture>> vertex_texture_objects;
  std::vector<id<MTLTexture>> fragment_texture_objects;
  std::vector<id<MTLSamplerState>> vertex_sampler_objects;
  std::vector<id<MTLSamplerState>> fragment_sampler_objects;
  CreateProbeTextures(device, vertex_textures, vertex_texture_count, dummy_texture,
                      vertex_texture_objects);
  CreateProbeTextures(device, fragment_textures, fragment_texture_count, dummy_texture,
                      fragment_texture_objects);
  CreateProbeSamplers(device, vertex_samplers, vertex_sampler_count, dummy_sampler,
                      vertex_sampler_objects);
  CreateProbeSamplers(device, fragment_samplers, fragment_sampler_count, dummy_sampler,
                      fragment_sampler_objects);

  bool has_initial_bgra = initial_bgra && initial_bgra_row_pitch >= size_t(width) * 4;
  if (has_initial_bgra) {
    MTLRegion initial_region = MTLRegionMake2D(0, 0, width, height);
    [render_texture replaceRegion:initial_region
                      mipmapLevel:0
                        withBytes:initial_bgra
                      bytesPerRow:initial_bgra_row_pitch];
  }

  id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = render_texture;
  pass.colorAttachments[0].loadAction = has_initial_bgra ? MTLLoadActionLoad : MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
  ConfigureProbeDepthStencilPass(pass, depth_stencil_texture, MTLLoadActionClear);

  id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
  [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline_state];
  [encoder setCullMode:ToMetalCullMode(rasterization_state ? rasterization_state->cull_mode
                                                           : ProbeCullMode::kNone)];
  [encoder setFrontFacingWinding:rasterization_state && rasterization_state->front_face_clockwise
                                     ? MTLWindingClockwise
                                     : MTLWindingCounterClockwise];
  [encoder setDepthBias:rasterization_state ? rasterization_state->depth_bias : 0.0
             slopeScale:rasterization_state ? rasterization_state->depth_bias_slope_scale : 0.0
                  clamp:0.0];
  [encoder setDepthClipMode:rasterization_state && rasterization_state->depth_clamp_enabled
                                ? MTLDepthClipModeClamp
                                : MTLDepthClipModeClip];
  [encoder setDepthStencilState:metal_depth_stencil_state];
  [encoder
      setStencilFrontReferenceValue:depth_stencil_state ? depth_stencil_state->front.reference : 0
                 backReferenceValue:depth_stencil_state ? depth_stencil_state->back.reference : 0];
  if (rasterization_state) {
    MTLViewport viewport = {
        rasterization_state->viewport_x,     rasterization_state->viewport_y,
        rasterization_state->viewport_width, rasterization_state->viewport_height,
        rasterization_state->viewport_z_min, rasterization_state->viewport_z_max};
    [encoder setViewport:viewport];
    MTLScissorRect scissor = {rasterization_state->scissor_x, rasterization_state->scissor_y,
                              rasterization_state->scissor_width,
                              rasterization_state->scissor_height};
    [encoder setScissorRect:scissor];
    [encoder setBlendColorRed:rasterization_state->blend_red
                        green:rasterization_state->blend_green
                         blue:rasterization_state->blend_blue
                        alpha:rasterization_state->blend_alpha];
  }
  [encoder setVertexBuffer:system_buffer offset:0 atIndex:0];
  [encoder setFragmentBuffer:system_buffer offset:0 atIndex:0];
  if (fetch_buffer) {
    if (vertex_fetch_constants_buffer_index != UINT32_MAX) {
      [encoder setVertexBuffer:fetch_buffer offset:0 atIndex:vertex_fetch_constants_buffer_index];
    }
    if (fragment_fetch_constants_buffer_index != UINT32_MAX) {
      [encoder setFragmentBuffer:fetch_buffer
                          offset:0
                         atIndex:fragment_fetch_constants_buffer_index];
    }
  }
  if (bool_loop_buffer) {
    if (vertex_bool_loop_constants_buffer_index != UINT32_MAX) {
      [encoder setVertexBuffer:bool_loop_buffer
                        offset:0
                       atIndex:vertex_bool_loop_constants_buffer_index];
    }
    if (fragment_bool_loop_constants_buffer_index != UINT32_MAX) {
      [encoder setFragmentBuffer:bool_loop_buffer
                          offset:0
                         atIndex:fragment_bool_loop_constants_buffer_index];
    }
  }
  if (float_buffer) {
    if (vertex_float_constants_buffer_index != UINT32_MAX) {
      [encoder setVertexBuffer:float_buffer offset:0 atIndex:vertex_float_constants_buffer_index];
    }
  }
  id<MTLBuffer> fragment_constants_to_bind =
      fragment_float_buffer ? fragment_float_buffer : float_buffer;
  if (fragment_constants_to_bind && fragment_float_constants_buffer_index != UINT32_MAX) {
    [encoder setFragmentBuffer:fragment_constants_to_bind
                        offset:0
                       atIndex:fragment_float_constants_buffer_index];
  }
  if (vertex_shared_memory_buffer_index != UINT32_MAX) {
    [encoder setVertexBuffer:shared_memory_buffer
                      offset:0
                     atIndex:vertex_shared_memory_buffer_index];
  }
  if (vertex_data_buffer && vertex_data_buffer_index != UINT32_MAX) {
    [encoder setVertexBuffer:vertex_data_buffer offset:0 atIndex:vertex_data_buffer_index];
  }
  BindProbeTextures(encoder, vertex_texture_objects, true);
  BindProbeTextures(encoder, fragment_texture_objects, false);
  BindProbeSamplers(encoder, vertex_sampler_objects, true);
  BindProbeSamplers(encoder, fragment_sampler_objects, false);
  if (index_buffer_object) {
    [encoder drawIndexedPrimitives:ToMetalPrimitiveType(primitive_type)
                        indexCount:vertex_count
                         indexType:index_buffer->index_size == 2 ? MTLIndexTypeUInt16
                                                                 : MTLIndexTypeUInt32
                       indexBuffer:index_buffer_object
                 indexBufferOffset:index_buffer->offset];
  } else {
    [encoder drawPrimitives:ToMetalPrimitiveType(primitive_type)
                vertexStart:0
                vertexCount:vertex_count];
  }
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];

  bool succeeded = [command_buffer status] != MTLCommandBufferStatusError;
  if (succeeded) {
    bgra_out.resize(size_t(width) * height * 4);
    MTLRegion region = MTLRegionMake2D(0, 0, width, height);
    [render_texture getBytes:bgra_out.data()
                 bytesPerRow:size_t(width) * 4
                  fromRegion:region
                 mipmapLevel:0];
  } else if (error_out) {
    NSError* error = [command_buffer error];
    *error_out = error ? [[error localizedDescription] UTF8String] : "command buffer failed";
  }

  [system_buffer release];
  if (float_buffer) {
    [float_buffer release];
  }
  if (fragment_float_buffer) {
    [fragment_float_buffer release];
  }
  if (fetch_buffer) {
    [fetch_buffer release];
  }
  if (bool_loop_buffer) {
    [bool_loop_buffer release];
  }
  if (vertex_data_buffer) {
    [vertex_data_buffer release];
  }
  if (index_buffer_object) {
    [index_buffer_object release];
  }
  [shared_memory_buffer release];
  ReleaseOwnedProbeTextures(vertex_texture_objects, dummy_texture);
  ReleaseOwnedProbeTextures(fragment_texture_objects, dummy_texture);
  ReleaseOwnedProbeSamplers(vertex_sampler_objects, dummy_sampler);
  ReleaseOwnedProbeSamplers(fragment_sampler_objects, dummy_sampler);
  if (dummy_texture) {
    [dummy_texture release];
  }
  if (dummy_sampler) {
    [dummy_sampler release];
  }
  [render_texture release];
  [depth_stencil_texture release];
  if (metal_depth_stencil_state) {
    [metal_depth_stencil_state release];
  }
  [command_queue release];
  return succeeded;
}

}  // namespace rex::graphics::metal
