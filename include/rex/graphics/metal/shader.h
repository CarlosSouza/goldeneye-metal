#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::metal {

class MetalShader : public SpirvShader {
 public:
  enum class TranslationMode : uint8_t {
    kNativeAttachments,
    kExactOutputMerger,
  };

  class MetalTranslation : public SpirvTranslation {
   public:
    static constexpr uint32_t kMslInterpolatorCount = 16;

    struct MslReflection {
      uint32_t shared_memory_buffer_index = UINT32_MAX;
      uint32_t system_constants_buffer_index = UINT32_MAX;
      uint32_t float_constants_buffer_index = UINT32_MAX;
      uint32_t bool_loop_constants_buffer_index = UINT32_MAX;
      uint32_t fetch_constants_buffer_index = UINT32_MAX;
      uint32_t edram_buffer_index = UINT32_MAX;
      std::vector<uint32_t> texture_fetch_constants_by_binding_index;
      std::array<uint32_t, kMslInterpolatorCount> pixel_interpolators_by_location = {
          0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
      bool writes_shared_memory = false;
      bool is_void_fragment = false;
      bool edram_raster_order_group = false;
      bool exact_output_merger_contract = false;
    };

    explicit MetalTranslation(MetalShader& shader, uint64_t modification,
                              TranslationMode mode = TranslationMode::kNativeAttachments)
        : SpirvTranslation(shader, modification), mode_(mode) {}
    ~MetalTranslation() override;

    bool TranslateMslFromSpirv();
    bool CompileMslLibrary(void* metal_device, std::string* error_out);
    const std::string& msl_source() const { return msl_source_; }
    const MslReflection& msl_reflection() const { return msl_reflection_; }
    void* metal_library() const { return metal_library_; }
    bool metal_library_is_current() const {
      return metal_library_ && msl_source_generation_ &&
             compiled_msl_generation_ == msl_source_generation_;
    }
    bool metal_library_is_current_for_device(void* metal_device) const {
      return metal_library_is_current() && metal_library_device_ == metal_device;
    }
    uint64_t msl_source_generation() const { return msl_source_generation_; }
    TranslationMode mode() const { return mode_; }

   private:
    bool ReflectMslSource(std::string* error_out);

    std::string msl_source_;
    MslReflection msl_reflection_;
    void* metal_library_ = nullptr;
    void* metal_library_device_ = nullptr;
    uint64_t msl_source_generation_ = 0;
    uint64_t compiled_msl_generation_ = 0;
    TranslationMode mode_ = TranslationMode::kNativeAttachments;
  };

  explicit MetalShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
                       const uint32_t* ucode_dwords, size_t ucode_dword_count,
                       std::endian ucode_source_endian = std::endian::big);

  // Exact translations live in a physically separate cache. The 64-bit guest
  // modification remains untouched, so no reserved bit can collide with a
  // future SpirvShaderTranslator modification layout.
  MetalTranslation* GetOrCreateExactOutputMergerTranslation(uint64_t modification,
                                                            bool* is_new = nullptr);
  MetalTranslation* GetExactOutputMergerTranslation(uint64_t modification) const;
  void DestroyExactOutputMergerTranslation(uint64_t modification);

 protected:
  Translation* CreateTranslationInstance(uint64_t modification) override;

 private:
  std::unordered_map<uint64_t, std::unique_ptr<MetalTranslation>> exact_output_merger_translations_;
};

// Exact output-merger shaders must never share the native-attachment
// translator configuration. This factory is used by both production setup and
// the real-Metal raster-order probe so the tested fragment-interlock contract
// cannot drift from the runtime one.
std::unique_ptr<SpirvShaderTranslator> CreateExactOutputMergerShaderTranslator();

// Creates the special no-color fragment shader used to quantize interpolated
// depth to the Xbox 360 float24 representation before host depth testing.
bool CreateDepthOnlyFragmentMslSource(
    SpirvShaderTranslator& shader_translator,
    SpirvShaderTranslator::Modification::DepthStencilMode depth_stencil_mode,
    std::string& source_out, std::string* error_out, std::vector<uint8_t>* spirv_out = nullptr);

void* CreateDepthOnlyFragmentMslLibrary(
    void* metal_device, SpirvShaderTranslator& shader_translator,
    SpirvShaderTranslator::Modification::DepthStencilMode depth_stencil_mode,
    std::string* error_out);

}  // namespace rex::graphics::metal
