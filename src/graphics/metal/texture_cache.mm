#include <rex/graphics/metal/texture_cache.h>

#import <Metal/Metal.h>

#include <xxhash.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/logging.h>
#include <rex/memory.h>

namespace rex::graphics::metal {
namespace {

MTLTextureType GetMetalTextureType(xenos::DataDimension dimension) {
  if (dimension == xenos::DataDimension::kCube) {
    return MTLTextureTypeCube;
  }
  // The Xenos 2D/stacked translator declares texture2d_array even for a
  // single-layer fetch. Metal validates the texture type at draw time, so a
  // plain MTLTextureType2D is not binding-compatible with that shader type.
  return MTLTextureType2DArray;
}

MTLPixelFormat GetMetalPixelFormat(xenos::TextureFormat format, bool is_signed) {
  switch (format) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
      return is_signed ? MTLPixelFormatR8Snorm : MTLPixelFormatR8Unorm;
    case xenos::TextureFormat::k_8_8:
      return is_signed ? MTLPixelFormatRG8Snorm : MTLPixelFormatRG8Unorm;
    case xenos::TextureFormat::k_8_8_8_8:
      return is_signed ? MTLPixelFormatRGBA8Snorm : MTLPixelFormatRGBA8Unorm;
    case xenos::TextureFormat::k_5_6_5:
      return is_signed ? MTLPixelFormatInvalid : MTLPixelFormatB5G6R5Unorm;
    case xenos::TextureFormat::k_DXT1:
      return is_signed ? MTLPixelFormatInvalid : MTLPixelFormatBC1_RGBA;
    case xenos::TextureFormat::k_DXT2_3:
      return is_signed ? MTLPixelFormatInvalid : MTLPixelFormatBC2_RGBA;
    case xenos::TextureFormat::k_DXT4_5:
      return is_signed ? MTLPixelFormatInvalid : MTLPixelFormatBC3_RGBA;
    case xenos::TextureFormat::k_24_8:
    case xenos::TextureFormat::k_24_8_FLOAT:
      // Stencil sampling would require a separate texture. Match the D3D12 and
      // Vulkan backends by exposing only depth as a scalar float texture.
      return MTLPixelFormatR32Float;
    default:
      return MTLPixelFormatInvalid;
  }
}

}  // namespace

namespace texture_cache_detail {

bool ConvertDepthTextureData(xenos::TextureFormat format, xenos::Endian endianness, void* output,
                             const void* input, size_t length) {
  if ((format != xenos::TextureFormat::k_24_8 && format != xenos::TextureFormat::k_24_8_FLOAT) ||
      !output || !input || (length & (sizeof(uint32_t) - 1))) {
    return false;
  }

  auto* output_bytes = static_cast<uint8_t*>(output);
  const auto* input_bytes = static_cast<const uint8_t*>(input);
  for (size_t offset = 0; offset < length; offset += sizeof(uint32_t)) {
    uint32_t packed_depth_stencil;
    texture_conversion::CopySwapBlock(endianness, &packed_depth_stencil, input_bytes + offset,
                                      sizeof(packed_depth_stencil));
    uint32_t depth_24 = packed_depth_stencil >> 8;
    uint32_t depth_float_bits;
    if (format == xenos::TextureFormat::k_24_8) {
      // This is the same endpoint-preserving UNORM conversion used by the
      // D3D12 and Vulkan texture-load shaders. The top-bit correction makes
      // 0xFFFFFF map exactly to 1.0 while retaining 24-bit precision.
      float depth = float(depth_24 + (depth_24 >> 23)) * 0x1.0p-24f;
      std::memcpy(&depth_float_bits, &depth, sizeof(depth_float_bits));
    } else if (!depth_24) {
      depth_float_bits = 0;
    } else {
      // Xenos float24 depth is an unsigned 4e20 value. Expand it directly to
      // IEEE float32 (8e23), including normalization of float24 denormals.
      uint32_t exponent = depth_24 >> 20;
      uint32_t mantissa = depth_24 & 0xFFFFF;
      if (!exponent) {
        uint32_t mantissa_msb = 31u - uint32_t(__builtin_clz(mantissa));
        exponent = uint32_t(int32_t(mantissa_msb) - 19);
        mantissa = (mantissa << (20 - mantissa_msb)) & 0xFFFFF;
      }
      depth_float_bits = ((exponent + 112) << 23) | (mantissa << 3);
    }
    std::memcpy(output_bytes + offset, &depth_float_bits, sizeof(depth_float_bits));
  }
  return true;
}

}  // namespace texture_cache_detail

class MetalTextureCache::MetalTexture final : public TextureCache::Texture {
 public:
  MetalTexture(MetalTextureCache& texture_cache, TextureKey key, void* texture)
      : Texture(texture_cache, key), texture_(texture) {
    const FormatInfo* format_info = FormatInfo::Get(key.format);
    uint32_t bytes_per_block = format_info ? format_info->bytes_per_block() : 4;
    SetHostMemoryUsage(uint64_t(key.GetWidth()) * key.GetHeight() * key.GetDepthOrArraySize() *
                       bytes_per_block);
  }

  ~MetalTexture() override {
    if (texture_) {
      [(id)texture_ release];
    }
  }

  void* texture() const { return texture_; }

  bool SourceFingerprintMatches(XXH128_hash_t fingerprint, size_t source_span) const {
    return source_fingerprint_valid_ && source_span == source_span_ &&
           XXH128_isEqual(source_fingerprint_, fingerprint);
  }

  void SetSourceFingerprint(XXH128_hash_t fingerprint, size_t source_span) {
    source_fingerprint_ = fingerprint;
    source_span_ = source_span;
    source_fingerprint_valid_ = true;
  }

 private:
  void* texture_ = nullptr;
  // SharedMemory intentionally widens ordinary CPU write invalidations to as
  // much as 256 KiB. Games commonly place immutable texture data next to
  // frequently rewritten vertex or index data in the same invalidation block.
  // Keep a bounded content fingerprint so those conservative watch hits can be
  // re-armed without repeating the much more expensive CPU untile and Metal
  // texture upload. The immutable TextureKey supplies all layout metadata.
  XXH128_hash_t source_fingerprint_ = {};
  size_t source_span_ = 0;
  bool source_fingerprint_valid_ = false;
};

std::unique_ptr<MetalTextureCache> MetalTextureCache::Create(const RegisterFile& register_file,
                                                             MetalSharedMemory& shared_memory,
                                                             void* metal_device,
                                                             uint32_t draw_resolution_scale_x,
                                                             uint32_t draw_resolution_scale_y) {
  std::unique_ptr<MetalTextureCache> texture_cache(
      new MetalTextureCache(register_file, shared_memory, metal_device, draw_resolution_scale_x,
                            draw_resolution_scale_y));
  if (!texture_cache->Initialize()) {
    return nullptr;
  }
  return texture_cache;
}

MetalTextureCache::MetalTextureCache(const RegisterFile& register_file,
                                     MetalSharedMemory& shared_memory, void* metal_device,
                                     uint32_t draw_resolution_scale_x,
                                     uint32_t draw_resolution_scale_y)
    : TextureCache(register_file, shared_memory, draw_resolution_scale_x, draw_resolution_scale_y),
      metal_shared_memory_(shared_memory),
      metal_device_(metal_device) {}

MetalTextureCache::~MetalTextureCache() {
  DestroyAllTextures(true);
}

bool MetalTextureCache::Initialize() {
  return metal_device_ != nullptr;
}

void MetalTextureCache::RequestTextures(uint32_t used_texture_mask) {
  TextureCache::RequestTextures(used_texture_mask);
}

void* MetalTextureCache::GetActiveTexture(uint32_t fetch_constant_index, bool is_signed) {
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (!binding) {
    return nullptr;
  }
  Texture* texture =
      is_signed && binding->texture_signed ? binding->texture_signed : binding->texture;
  if (!texture) {
    return nullptr;
  }
  texture->MarkAsUsed();
  return static_cast<MetalTexture*>(texture)->texture();
}

uint32_t MetalTextureCache::GetActiveTextureWidth(uint32_t fetch_constant_index) const {
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  return binding ? binding->key.GetWidth() : 0;
}

uint32_t MetalTextureCache::GetActiveTextureHeight(uint32_t fetch_constant_index) const {
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  return binding ? binding->key.GetHeight() : 0;
}

bool MetalTextureCache::IsSignedVersionSeparateForFormat(TextureKey key) const {
  switch (key.format) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_8:
    case xenos::TextureFormat::k_8_8_8_8:
      return true;
    default:
      return false;
  }
}

uint32_t MetalTextureCache::GetHostFormatSwizzle(TextureKey key) const {
  switch (key.format) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
      return xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR;
    case xenos::TextureFormat::k_8_8:
      return xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG;
    case xenos::TextureFormat::k_8_8_8_8:
      return xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
    case xenos::TextureFormat::k_5_6_5:
      return xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB;
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT4_5:
      return xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
    case xenos::TextureFormat::k_24_8:
    case xenos::TextureFormat::k_24_8_FLOAT:
      return xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR;
    default:
      return xenos::XE_GPU_TEXTURE_SWIZZLE_0000;
  }
}

uint32_t MetalTextureCache::GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const {
  (void)dimension;
  return 16384;
}

uint32_t MetalTextureCache::GetMaxHostTextureDepthOrArraySize(
    xenos::DataDimension dimension) const {
  return dimension == xenos::DataDimension::kCube ? 6 : 2048;
}

std::unique_ptr<TextureCache::Texture> MetalTextureCache::CreateTexture(TextureKey key) {
  MTLPixelFormat pixel_format = GetMetalPixelFormat(key.format, key.signed_separate != 0);
  if (!metal_device_ || pixel_format == MTLPixelFormatInvalid || key.scaled_resolve ||
      key.dimension == xenos::DataDimension::k1D || key.dimension == xenos::DataDimension::k3D) {
    return nullptr;
  }

  // Reject impossible guest ranges before asking Metal for what may be a very
  // large allocation. The normal load path checks this too, but doing it after
  // allocation is particularly costly for malformed near-maximum descriptors.
  texture_util::TextureGuestLayout guest_layout = key.GetGuestLayout();
  auto guest_range_fits = [](uint32_t page, uint32_t extent) {
    uint64_t start = uint64_t(page) << 12;
    return !extent || (page && start < SharedMemory::kBufferSize &&
                       uint64_t(extent) <= SharedMemory::kBufferSize - start);
  };
  if (!guest_range_fits(key.base_page, guest_layout.base.level_data_extent_bytes) ||
      !guest_range_fits(key.mip_page, guest_layout.mips_total_extent_bytes)) {
    REXGPU_WARN("MetalTextureCache: rejecting texture whose guest range is outside "
                "physical memory (base=0x{:08X} base_extent=0x{:X}, mips=0x{:08X} "
                "mip_extent=0x{:X})",
                key.base_page << 12, guest_layout.base.level_data_extent_bytes, key.mip_page << 12,
                guest_layout.mips_total_extent_bytes);
    return nullptr;
  }

  id<MTLDevice> device = (id<MTLDevice>)metal_device_;
  uint32_t width = key.GetWidth();
  uint32_t height = key.GetHeight();
  uint32_t depth_or_array_size = key.GetDepthOrArraySize();
  MTLTextureDescriptor* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel_format
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  descriptor.textureType = GetMetalTextureType(key.dimension);
  // A cube texture has one cube in Metal; its six faces are addressed as
  // slices. Cube arrays would increase arrayLength, but Xenos cube fetches here
  // represent a single cube.
  descriptor.arrayLength =
      key.dimension == xenos::DataDimension::kCube ? 1 : std::max(depth_or_array_size, 1u);
  descriptor.usage = MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
  if (!texture) {
    REXGPU_WARN("MetalTextureCache: failed to create {}x{} texture", width, height);
    return nullptr;
  }
  return std::unique_ptr<Texture>(new MetalTexture(*this, key, texture));
}

bool MetalTextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                                              bool load_mips) {
  MetalTexture& metal_texture = static_cast<MetalTexture&>(texture);
  const TextureKey& key = metal_texture.key();
  if (!load_base) {
    return load_mips;
  }
  if (GetMetalPixelFormat(key.format, key.signed_separate != 0) == MTLPixelFormatInvalid) {
    return false;
  }

  const FormatInfo* format_info = FormatInfo::Get(key.format);
  if (!format_info) {
    return false;
  }

  if (!metal_shared_memory_.SynchronizeBeforeHostResourceMutation()) {
    return false;
  }

  uint32_t width = key.GetWidth();
  uint32_t height = key.GetHeight();
  uint32_t x_blocks = (width + format_info->block_width - 1) / format_info->block_width;
  uint32_t y_blocks = (height + format_info->block_height - 1) / format_info->block_height;
  uint32_t bytes_per_block = format_info->bytes_per_block();
  uint32_t array_size = key.GetDepthOrArraySize();
  const texture_util::TextureGuestLayout::Level& level = metal_texture.guest_layout().base;
  uint32_t base_physical = key.base_page << 12;
  memory::Memory& guest_memory = metal_shared_memory_.guest_memory();
  // Read source texels from the resident Metal shared-memory copy. CPU-backed
  // resolve writes explicitly commit their guest bytes to this buffer before
  // publishing the range as GPU-produced data.
  id<MTLBuffer> shared_buffer = (id<MTLBuffer>)metal_shared_memory_.buffer();
  if (!shared_buffer) {
    return false;
  }
  const uint8_t* buffer_contents = static_cast<const uint8_t*>([shared_buffer contents]);
  if (!buffer_contents) {
    return false;
  }

  // Validate every slice before accepting a fingerprint hit. Hash the layout's
  // full data extent, including a last slice whose tiled address upper bound
  // may be larger than the aligned distance between slices.
  uint64_t source_span = level.level_data_extent_bytes;
  for (uint32_t slice = 0; slice < array_size; ++slice) {
    uint64_t slice_offset = uint64_t(slice) * level.array_slice_stride_bytes;
    if (uint64_t(base_physical) + slice_offset >= SharedMemory::kBufferSize ||
        !guest_memory.TranslatePhysical<const uint8_t*>(
            uint32_t(uint64_t(base_physical) + slice_offset))) {
      return false;
    }
    if (uint64_t(base_physical) + slice_offset + level.array_slice_data_extent_bytes >
        SharedMemory::kBufferSize) {
      return false;
    }
  }
  if (!source_span || uint64_t(base_physical) + source_span > SharedMemory::kBufferSize ||
      source_span > SIZE_MAX) {
    return false;
  }
  XXH128_hash_t source_fingerprint =
      XXH3_128bits(buffer_contents + base_physical, size_t(source_span));
  if (metal_texture.SourceFingerprintMatches(source_fingerprint, size_t(source_span))) {
    return true;
  }

  std::vector<uint8_t> linear_data(size_t(x_blocks) * y_blocks * bytes_per_block);
  bool is_depth_texture = key.format == xenos::TextureFormat::k_24_8 ||
                          key.format == xenos::TextureFormat::k_24_8_FLOAT;
  for (uint32_t slice = 0; slice < array_size; ++slice) {
    uint32_t slice_offset = slice * level.array_slice_stride_bytes;
    const uint8_t* guest_base = buffer_contents + base_physical + slice_offset;

    if (key.tiled) {
      texture_conversion::UntileInfo untile_info;
      untile_info.offset_x = 0;
      untile_info.offset_y = 0;
      untile_info.width = x_blocks;
      untile_info.height = y_blocks;
      untile_info.input_pitch = level.row_pitch_bytes / bytes_per_block;
      untile_info.output_pitch = x_blocks;
      untile_info.input_format_info = format_info;
      untile_info.output_format_info = format_info;
      untile_info.copy_callback = [format = key.format, endian = key.endianness, is_depth_texture](
                                      void* output, const void* input, size_t length) {
        if (is_depth_texture) {
          texture_cache_detail::ConvertDepthTextureData(format, endian, output, input, length);
        } else {
          texture_conversion::CopySwapBlock(endian, output, input, length);
        }
      };
      texture_conversion::Untile(linear_data.data(), guest_base, &untile_info);
    } else {
      size_t output_row_pitch = size_t(x_blocks) * bytes_per_block;
      for (uint32_t y = 0; y < y_blocks; ++y) {
        if (is_depth_texture) {
          texture_cache_detail::ConvertDepthTextureData(
              key.format, key.endianness, linear_data.data() + size_t(y) * output_row_pitch,
              guest_base + size_t(y) * level.row_pitch_bytes, output_row_pitch);
        } else {
          texture_conversion::CopySwapBlock(
              key.endianness, linear_data.data() + size_t(y) * output_row_pitch,
              guest_base + size_t(y) * level.row_pitch_bytes, output_row_pitch);
        }
      }
    }

    MTLRegion region = MTLRegionMake2D(0, 0, width, height);
    [(id<MTLTexture>)metal_texture.texture() replaceRegion:region
                                               mipmapLevel:0
                                                     slice:slice
                                                 withBytes:linear_data.data()
                                               bytesPerRow:size_t(x_blocks) * bytes_per_block
                                             bytesPerImage:linear_data.size()];
  }

  metal_texture.SetSourceFingerprint(source_fingerprint, size_t(source_span));

  return true;
}

}  // namespace rex::graphics::metal
