/**
 * Headless native Metal GPU trace dump entry point.
 *
 * Replays a captured ReXGlue GPU trace through the Metal backend without a
 * window and writes the selected guest frame as BMP and raw RGBX data.
 *
 * Usage: trace_dump_metal <trace_file.xtr> [output_base] [frame_index]
 *                         [--best-effort]
 */

#import <Foundation/Foundation.h>

#include <memory>
#include <string>
#include <vector>

#include <rex/graphics/metal/edram_snapshot.h>
#include <rex/graphics/metal/graphics_system.h>
#include <rex/graphics/trace_dump.h>

namespace rex::graphics::metal {

class MetalTraceDump final : public TraceDump {
 protected:
  std::unique_ptr<GraphicsSystem> CreateGraphicsSystem() override {
    return std::make_unique<MetalGraphicsSystem>();
  }

  void BeginHostCapture() override {}
  void EndHostCapture() override {}
  bool SupportsCanonicalEdramRequirements(
      const TraceEdramRequirements& requirements,
      std::string& limitation_out) const override {
    for (uint32_t value = 0; value < 32; ++value) {
      if ((requirements.color_format_mask & (1u << value)) &&
          !IsCanonicalEdramColorFormatSupportedByMetal(
              xenos::ColorRenderTargetFormat(value))) {
        limitation_out =
            "Metal cannot round-trip required color format " + std::to_string(value);
        return false;
      }
      if ((requirements.depth_format_mask & (1u << value)) &&
          !IsCanonicalEdramDepthFormatSupportedByMetal(
              xenos::DepthRenderTargetFormat(value))) {
        limitation_out =
            "Metal cannot round-trip required depth format " + std::to_string(value);
        return false;
      }
      if ((requirements.msaa_samples_mask & (1u << value)) &&
          !IsCanonicalEdramMsaaSupportedByMetal(xenos::MsaaSamples(value))) {
        limitation_out =
            "Metal cannot round-trip required MSAA mode " + std::to_string(value);
        return false;
      }
    }
    limitation_out.clear();
    return true;
  }
};

}  // namespace rex::graphics::metal

int main(int argc, char** argv) {
  @autoreleasepool {
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
      args.emplace_back(argv[i]);
    }

    rex::graphics::metal::MetalTraceDump dump;
    return dump.Main(args);
  }
}
