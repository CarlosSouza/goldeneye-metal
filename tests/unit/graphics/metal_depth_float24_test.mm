#import <Metal/Metal.h>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/metal/msl_compiler.h>
#include <rex/graphics/metal/shader.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>

namespace rex::graphics::metal {

TEST_CASE("Metal compiles both float24 depth-only fragment modes", "[graphics][metal]") {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);

    SpirvShaderTranslator::Features features(true);
    features.image_view_format_swizzle = false;
    SpirvShaderTranslator translator(features, true, false, false);
    using DepthStencilMode = SpirvShaderTranslator::Modification::DepthStencilMode;
    constexpr DepthStencilMode modes[] = {
        DepthStencilMode::kFloat24Truncating,
        DepthStencilMode::kFloat24Rounding,
    };

    for (DepthStencilMode mode : modes) {
      DYNAMIC_SECTION("mode " << uint32_t(mode)) {
        std::string error;
        void* library = CreateDepthOnlyFragmentMslLibrary(device, translator, mode, &error);
        INFO(error);
        REQUIRE(library != nullptr);
        ReleaseMslLibrary(library);
      }
    }
  }
}

}  // namespace rex::graphics::metal
