#pragma once

#include <cstddef>
#include <memory>

#include <rex/graphics/metal/shared_memory.h>
#include <rex/graphics/pipeline/texture/cache.h>

namespace rex::graphics::metal {

namespace texture_cache_detail {

// Converts packed Xenos D24S8 / D24FS8 texels to the R32Float representation
// used for sampling on Metal. The source endianness is applied before the
// stencil byte is discarded. Returns false for unsupported formats or partial
// texels.
bool ConvertDepthTextureData(xenos::TextureFormat format, xenos::Endian endianness, void* output,
                             const void* input, size_t length);

}  // namespace texture_cache_detail

class MetalTextureCache final : public TextureCache {
 public:
  static std::unique_ptr<MetalTextureCache> Create(const RegisterFile& register_file,
                                                   MetalSharedMemory& shared_memory,
                                                   void* metal_device,
                                                   uint32_t draw_resolution_scale_x,
                                                   uint32_t draw_resolution_scale_y);
  ~MetalTextureCache() override;

  void RequestTextures(uint32_t used_texture_mask) override;

  void* GetActiveTexture(uint32_t fetch_constant_index, bool is_signed = false);
  uint32_t GetActiveTextureWidth(uint32_t fetch_constant_index) const;
  uint32_t GetActiveTextureHeight(uint32_t fetch_constant_index) const;

 protected:
  bool IsSignedVersionSeparateForFormat(TextureKey key) const override;
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;
  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const override;
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const override;
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;
  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;

 private:
  class MetalTexture;

  MetalTextureCache(const RegisterFile& register_file, MetalSharedMemory& shared_memory,
                    void* metal_device, uint32_t draw_resolution_scale_x,
                    uint32_t draw_resolution_scale_y);

  bool Initialize();

  MetalSharedMemory& metal_shared_memory_;
  void* metal_device_ = nullptr;
};

}  // namespace rex::graphics::metal
