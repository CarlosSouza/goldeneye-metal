/**
 ******************************************************************************
 * ReXGlue - Xbox 360 recompilation runtime                                  *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                       *
 *                                                                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import <Metal/Metal.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/metal/texture_cache.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/trace_writer.h>
#include <rex/graphics/xenos.h>
#include <rex/memory.h>

namespace {

using rex::graphics::metal::texture_cache_detail::ConvertDepthTextureData;
using rex::graphics::xenos::Endian;
using rex::graphics::xenos::TextureFormat;

uint32_t ConvertOne(TextureFormat format, uint32_t packed_depth_stencil,
                    Endian endian = Endian::kNone) {
  uint32_t source = rex::graphics::xenos::GpuSwap(packed_depth_stencil, endian);
  uint32_t output = 0xFFFFFFFF;
  REQUIRE(ConvertDepthTextureData(format, endian, &output, &source, sizeof(source)));
  return output;
}

}  // namespace

TEST_CASE("Metal depth texture conversion matches Xenos D24S8 sampling",
          "[graphics][metal][texture-cache][depth]") {
  CHECK(ConvertOne(TextureFormat::k_24_8, 0x000000AB) == std::bit_cast<uint32_t>(0.0f));
  CHECK(ConvertOne(TextureFormat::k_24_8, 0xFFFFFF00) == std::bit_cast<uint32_t>(1.0f));

  constexpr float kMiddleDepth = float(0x800000 + 1) * 0x1.0p-24f;
  CHECK(ConvertOne(TextureFormat::k_24_8, 0x80000011) == std::bit_cast<uint32_t>(kMiddleDepth));

  // Stencil is the low byte and must not affect sampled depth.
  CHECK(ConvertOne(TextureFormat::k_24_8, 0x12345600) ==
        ConvertOne(TextureFormat::k_24_8, 0x123456FF));
}

TEST_CASE("Metal depth texture conversion expands Xenos float24 exactly",
          "[graphics][metal][texture-cache][depth]") {
  CHECK(ConvertOne(TextureFormat::k_24_8_FLOAT, 0x000000A5) == std::bit_cast<uint32_t>(0.0f));
  CHECK(ConvertOne(TextureFormat::k_24_8_FLOAT, 0xF0000000) == std::bit_cast<uint32_t>(1.0f));
  CHECK(ConvertOne(TextureFormat::k_24_8_FLOAT, 0xE0000000) == std::bit_cast<uint32_t>(0.5f));
  CHECK(ConvertOne(TextureFormat::k_24_8_FLOAT, 0xF8000000) == std::bit_cast<uint32_t>(1.5f));

  // Largest and smallest float24 denormals exercise the explicit
  // normalization path used by the reference backends.
  CHECK(ConvertOne(TextureFormat::k_24_8_FLOAT, 0x08000000) == std::bit_cast<uint32_t>(0x1.0p-15f));
  CHECK(ConvertOne(TextureFormat::k_24_8_FLOAT, 0x00000100) == std::bit_cast<uint32_t>(0x1.0p-34f));
}

TEST_CASE("Metal packed depth conversion applies every Xenos endian mode",
          "[graphics][metal][texture-cache][depth]") {
  constexpr std::array<Endian, 4> kEndians = {Endian::kNone, Endian::k8in16, Endian::k8in32,
                                              Endian::k16in32};
  for (Endian endian : kEndians) {
    CHECK(ConvertOne(TextureFormat::k_24_8, 0xFFFFFF5A, endian) == std::bit_cast<uint32_t>(1.0f));
    CHECK(ConvertOne(TextureFormat::k_24_8_FLOAT, 0xF800005A, endian) ==
          std::bit_cast<uint32_t>(1.5f));
  }
}

TEST_CASE("Metal packed depth conversion rejects invalid input",
          "[graphics][metal][texture-cache][depth]") {
  uint32_t input = 0;
  uint32_t output = 0;
  CHECK_FALSE(ConvertDepthTextureData(TextureFormat::k_8_8_8_8, Endian::kNone, &output, &input,
                                      sizeof(input)));
  CHECK_FALSE(ConvertDepthTextureData(TextureFormat::k_24_8, Endian::kNone, &output, &input, 3));
  CHECK_FALSE(ConvertDepthTextureData(TextureFormat::k_24_8, Endian::kNone, nullptr, &input,
                                      sizeof(input)));
}

TEST_CASE("Metal texture cache untiles packed depth into scalar float",
          "[graphics][metal][texture-cache][depth][integration]") {
  @autoreleasepool {
    rex::memory::Memory memory;
    REQUIRE(memory.Initialize());
    rex::graphics::TraceWriter trace_writer(memory.physical_membase());
    rex::graphics::metal::MetalSharedMemory shared_memory(memory, trace_writer);

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);
    REQUIRE(shared_memory.Initialize((void*)device));

    rex::graphics::RegisterFile register_file;
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch = {};
    fetch.type = rex::graphics::xenos::FetchConstantType::kTexture;
    fetch.pitch = 1;  // One 32-texel tile.
    fetch.tiled = 1;
    fetch.format = TextureFormat::k_24_8;
    fetch.endianness = Endian::k8in32;
    constexpr uint32_t kBasePhysical = 0x00200000;
    fetch.base_address = kBasePhysical >> 12;
    fetch.size_2d.width = 31;
    fetch.size_2d.height = 31;
    fetch.swizzle = rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
    fetch.mip_max_level = 0;
    fetch.dimension = rex::graphics::xenos::DataDimension::k2DOrStacked;
    std::memcpy(&register_file.values[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], &fetch,
                sizeof(fetch));

    auto* source = memory.TranslatePhysical<uint8_t*>(kBasePhysical);
    REQUIRE(source != nullptr);
    for (uint32_t y = 0; y < 32; ++y) {
      for (uint32_t x = 0; x < 32; ++x) {
        uint32_t linear_index = y * 32 + x;
        uint32_t packed = linear_index == 0   ? 0u
                          : linear_index == 1 ? 0xFFFFFF00u
                                              : 0x80000000u | linear_index;
        uint32_t guest = rex::graphics::xenos::GpuSwap(packed, fetch.endianness);
        uint32_t tiled_offset =
            uint32_t(rex::graphics::texture_util::GetTiledOffset2D(x, y, 32, 2));
        std::memcpy(source + tiled_offset, &guest, sizeof(guest));
      }
    }

    auto texture_cache = rex::graphics::metal::MetalTextureCache::Create(
        register_file, shared_memory, (void*)device, 1, 1);
    REQUIRE(texture_cache != nullptr);
    texture_cache->RequestTextures(1);

    id<MTLTexture> texture = (id<MTLTexture>)texture_cache->GetActiveTexture(0);
    REQUIRE(texture != nil);
    CHECK(texture.pixelFormat == MTLPixelFormatR32Float);
    CHECK(texture.textureType == MTLTextureType2DArray);
    CHECK(texture.width == 32);
    CHECK(texture.height == 32);
    CHECK(texture_cache->GetActiveTextureHostSwizzle(0) ==
          rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR);

    std::array<uint32_t, 32 * 32> uploaded = {};
    [texture getBytes:uploaded.data()
          bytesPerRow:32 * sizeof(uploaded[0])
        bytesPerImage:uploaded.size() * sizeof(uploaded[0])
           fromRegion:MTLRegionMake2D(0, 0, 32, 32)
          mipmapLevel:0
                slice:0];
    CHECK(uploaded[0] == std::bit_cast<uint32_t>(0.0f));
    CHECK(uploaded[1] == std::bit_cast<uint32_t>(1.0f));
    constexpr float kMiddleDepth = float(0x800000 + 1) * 0x1.0p-24f;
    CHECK(uploaded[2] == std::bit_cast<uint32_t>(kMiddleDepth));

    texture_cache.reset();
    shared_memory.Shutdown();
  }
}
