// exact_resolve_probe_test.mm -- real-Metal exact EDRAM IssueCopy gate.

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <rex/graphics/metal/exact_output_merger.h>
#include <rex/graphics/metal/exact_output_merger_state.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/metal/command_processor.h>
#include <rex/graphics/metal/shared_memory.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/memory.h>

namespace rex::graphics::metal {
bool ProbeExactOutputMergerRejectsEdramResolveAliasesForInternalProbe(
    void* resources, const ExactOutputMergerResolvePlan& plan, void* other_buffer,
    size_t other_buffer_size, std::string* error_out);

struct MetalCommandProcessorTestPeer {
  static void AttachExactResources(MetalCommandProcessor& command_processor, void* resources) {
    command_processor.exact_output_merger_resources_ = resources;
  }

  static bool WriteGuestCompletion(MetalCommandProcessor& command_processor, uint32_t address,
                                   uint32_t value) {
    return command_processor.WriteGpuCompletionMemory(address, &value, sizeof(value));
  }

  static size_t PendingGuestCompletionCount(const MetalCommandProcessor& command_processor) {
    return command_processor.pending_gpu_completion_writes_.size();
  }

  static void DetachExactResourcesAndDiscardCompletion(MetalCommandProcessor& command_processor) {
    command_processor.exact_output_merger_resources_ = nullptr;
    command_processor.pending_gpu_completion_writes_.clear();
    command_processor.pending_gpu_completion_staging_.clear();
  }

  static void AttachSharedMemory(MetalCommandProcessor& command_processor,
                                 std::unique_ptr<MetalSharedMemory> shared_memory) {
    command_processor.shared_memory_ = std::move(shared_memory);
  }
};

struct MetalSharedMemoryTestPeer {
  static void ReplaceCommandQueue(MetalSharedMemory& shared_memory, void* command_queue) {
    id<MTLCommandQueue> replacement = (id<MTLCommandQueue>)command_queue;
    [replacement retain];
    [(id<MTLCommandQueue>)shared_memory.command_queue_ release];
    shared_memory.command_queue_ = command_queue;
  }
};
}  // namespace rex::graphics::metal

struct FaultCounters {
  uint32_t created = 0;
  uint32_t destroyed = 0;
  uint32_t injected = 0;
};

@interface ExactResolveFaultCommandBuffer : NSObject {
 @private
  id<MTLCommandBuffer> inner_;
  FaultCounters* counters_;
  bool inject_failure_;
}
- (instancetype)initWithInner:(id<MTLCommandBuffer>)inner
                     counters:(FaultCounters*)counters
                injectFailure:(bool)inject_failure;
@end

@implementation ExactResolveFaultCommandBuffer
- (instancetype)initWithInner:(id<MTLCommandBuffer>)inner
                     counters:(FaultCounters*)counters
                injectFailure:(bool)inject_failure {
  self = [super init];
  if (self) {
    inner_ = [inner retain];
    counters_ = counters;
    inject_failure_ = inject_failure;
    ++counters_->created;
    counters_->injected += inject_failure_ ? 1u : 0u;
  }
  return self;
}
- (void)dealloc {
  [inner_ release];
  ++counters_->destroyed;
  [super dealloc];
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
    return
        [NSError errorWithDomain:@"GoldenEyeExactResolveProbe"
                            code:1
                        userInfo:@{
                          NSLocalizedDescriptionKey : @"injected exact resolve predecessor failure"
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

@interface ExactResolveFaultQueue : NSObject {
 @private
  id<MTLCommandQueue> inner_;
  FaultCounters* counters_;
  uint32_t failures_to_inject_;
}
- (instancetype)initWithInner:(id<MTLCommandQueue>)inner counters:(FaultCounters*)counters;
- (void)injectNextFailure;
@end

@implementation ExactResolveFaultQueue
- (instancetype)initWithInner:(id<MTLCommandQueue>)inner counters:(FaultCounters*)counters {
  self = [super init];
  if (self) {
    inner_ = [inner retain];
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
  id<MTLCommandBuffer> inner_buffer = [inner_ commandBuffer];
  const bool inject_failure = failures_to_inject_ != 0;
  failures_to_inject_ -= inject_failure ? 1u : 0u;
  return [(id)[[ExactResolveFaultCommandBuffer alloc] initWithInner:inner_buffer
                                                           counters:counters_
                                                      injectFailure:inject_failure] autorelease];
}
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences {
  return [self commandBuffer];
}
- (void)injectNextFailure {
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

using namespace rex::graphics;
using namespace rex::graphics::metal;

class ProbeGraphicsSystem final : public GraphicsSystem {
 public:
  explicit ProbeGraphicsSystem(rex::memory::Memory& memory) { memory_ = &memory; }

  std::string name() const override { return "Metal exact resolve probe"; }

 protected:
  void CreateProvider(bool with_presentation) override { (void)with_presentation; }
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override { return nullptr; }
};

struct CallbackState {
  uint32_t submitted = 0;
  uint32_t failed = 0;
  uint32_t start = 0;
  uint32_t length = 0;
};

void SubmissionCallback(void* context, uint32_t start, uint32_t length) {
  auto& state = *static_cast<CallbackState*>(context);
  ++state.submitted;
  state.start = start;
  state.length = length;
}

void FailureCallback(void* context, uint32_t start, uint32_t length) {
  auto& state = *static_cast<CallbackState*>(context);
  ++state.failed;
  state.start = start;
  state.length = length;
}

void RefreshDestinationExtent(draw_util::ResolveInfo& info) {
  const uint32_t pitch = uint32_t(info.copy_dest_coordinate_info.pitch_aligned_div_32) * 32;
  const uint32_t x = uint32_t(info.copy_dest_coordinate_info.offset_x_div_8) * 8;
  const uint32_t y = uint32_t(info.copy_dest_coordinate_info.offset_y_div_8) * 8;
  const uint32_t width = uint32_t(info.coordinate_info.width_div_8) * 8;
  const uint32_t height = info.height_div_8 * 8;
  info.copy_dest_extent_start =
      info.copy_dest_base + texture_util::GetTiledAddressLowerBound2D(x, y, pitch, 2);
  const uint32_t end = info.copy_dest_base +
                       texture_util::GetTiledAddressUpperBound2D(x + width, y + height, pitch, 2);
  info.copy_dest_extent_length = end - info.copy_dest_extent_start;
}

draw_util::ResolveInfo MakeColorInfo(xenos::MsaaSamples msaa, xenos::CopySampleSelect sample) {
  draw_util::ResolveInfo info = {};
  info.rb_copy_control.copy_src_select = 0;
  info.rb_copy_control.copy_sample_select = sample;
  info.rb_copy_control.copy_command = xenos::CopyCommand::kConvert;
  info.color_edram_info.pitch_tiles = xenos::GetSurfacePitchTiles(160, msaa, false);
  info.color_edram_info.msaa_samples = msaa;
  info.color_edram_info.base_tiles = 7;
  info.color_edram_info.format = uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8);
  info.coordinate_info.edram_offset_x_div_8 = 1;
  info.coordinate_info.edram_offset_y_div_8 = 1;
  info.coordinate_info.width_div_8 = 2;
  info.coordinate_info.draw_resolution_scale_x = 1;
  info.coordinate_info.draw_resolution_scale_y = 1;
  info.height_div_8 = 1;
  info.copy_dest_info.copy_dest_format = xenos::ColorFormat::k_8_8_8_8;
  info.copy_dest_info.copy_dest_number = xenos::SurfaceNumberFormat::kUnsignedRepeatingFraction;
  info.copy_dest_coordinate_info.pitch_aligned_div_32 = 4;
  info.copy_dest_coordinate_info.height_aligned_div_32 = 2;
  info.copy_dest_coordinate_info.copy_sample_select = sample;
  info.copy_dest_base = UINT32_C(0x4000);
  RefreshDestinationExtent(info);
  return info;
}

draw_util::ResolveInfo MakeDepthInfo(xenos::DepthRenderTargetFormat format, xenos::MsaaSamples msaa,
                                     xenos::CopySampleSelect sample) {
  draw_util::ResolveInfo info = MakeColorInfo(msaa, sample);
  info.rb_copy_control.copy_src_select = xenos::kMaxColorRenderTargets;
  info.color_edram_info.packed = 0;
  info.depth_edram_info.pitch_tiles = xenos::GetSurfacePitchTiles(160, msaa, false);
  info.depth_edram_info.msaa_samples = msaa;
  info.depth_edram_info.is_depth = 1;
  info.depth_edram_info.base_tiles = 900;
  info.depth_edram_info.format = uint32_t(format);
  info.copy_dest_info.copy_dest_format = xenos::ColorFormat(
      format == xenos::DepthRenderTargetFormat::kD24FS8 ? xenos::TextureFormat::k_24_8_FLOAT
                                                        : xenos::TextureFormat::k_24_8);
  return info;
}

bool WriteColorSamples(std::vector<uint32_t>& edram, const draw_util::ResolveInfo& info,
                       const std::array<uint32_t, 4>& values, std::string& error) {
  ExactOutputMergerSurfaceConstants surface;
  if (!BuildExactOutputMergerColorConstants(
          info.color_edram_info.base_tiles, 160, 160, 32, info.color_edram_info.msaa_samples,
          xenos::ColorRenderTargetFormat(info.color_edram_info.format), surface, &error)) {
    return false;
  }
  const uint32_t sample_count = 1u << uint32_t(info.color_edram_info.msaa_samples);
  for (uint32_t sample = 0; sample < sample_count; ++sample) {
    size_t index = 0;
    if (!GetExactOutputMergerDwordIndex(surface, 8, 8, sample, 0, index)) {
      error = "color source address was rejected";
      return false;
    }
    edram[index] = values[sample];
  }
  return true;
}

bool WriteDepthSamples(std::vector<uint32_t>& edram, const draw_util::ResolveInfo& info,
                       const std::array<uint32_t, 4>& values, std::string& error) {
  ExactOutputMergerSurfaceConstants surface;
  if (!BuildExactOutputMergerDepthConstants(
          info.depth_edram_info.base_tiles, 160, 160, 32, info.depth_edram_info.msaa_samples,
          xenos::DepthRenderTargetFormat(info.depth_edram_info.format), surface, &error)) {
    return false;
  }
  const uint32_t sample_count = 1u << uint32_t(info.depth_edram_info.msaa_samples);
  for (uint32_t sample = 0; sample < sample_count; ++sample) {
    size_t index = 0;
    if (!GetExactOutputMergerDwordIndex(surface, 8, 8, sample, 0, index)) {
      error = "depth source address was rejected";
      return false;
    }
    edram[index] = values[sample];
  }
  return true;
}

uint32_t ApplyEndian(uint32_t value, xenos::Endian128 endian) {
  const uint32_t raw = uint32_t(endian);
  if (raw == 1 || raw == 2) {
    value = ((value & UINT32_C(0x00FF00FF)) << 8) | ((value & UINT32_C(0xFF00FF00)) >> 8);
  }
  if (raw == 2 || raw == 3) {
    value = (value << 16) | (value >> 16);
  }
  return value;
}

uint32_t ReadResolvedDword(id<MTLBuffer> buffer, const draw_util::ResolveInfo& info) {
  const uint32_t pitch = uint32_t(info.copy_dest_coordinate_info.pitch_aligned_div_32) * 32;
  const uint32_t x = uint32_t(info.copy_dest_coordinate_info.offset_x_div_8) * 8;
  const uint32_t y = uint32_t(info.copy_dest_coordinate_info.offset_y_div_8) * 8;
  const uint32_t offset = info.copy_dest_base + uint32_t(texture_util::GetTiledOffset2D(
                                                    int32_t(x), int32_t(y), pitch, 2));
  uint32_t value = 0;
  std::memcpy(&value, static_cast<const uint8_t*>(buffer.contents) + offset, sizeof(value));
  return value;
}

ExactOutputMergerResolveDestination MakeDestination(id<MTLBuffer> resident, id<MTLBuffer> guest,
                                                    CallbackState& callback) {
  ExactOutputMergerResolveDestination destination;
  destination.metal_buffer = resident;
  destination.metal_buffer_size = resident.length;
  destination.guest_memory_metal_buffer = guest;
  destination.guest_memory_metal_buffer_size = guest.length;
  destination.submission_callback = SubmissionCallback;
  destination.submission_callback_context = &callback;
  destination.async_failure_callback = FailureCallback;
  destination.async_failure_callback_context = &callback;
  return destination;
}

bool RunOneResolve(void* resources, id<MTLBuffer> resident, id<MTLBuffer> guest,
                   draw_util::ResolveInfo info, const std::array<uint32_t, 4>& samples,
                   uint32_t expected, bool copying_depth, std::string& error) {
  std::vector<uint32_t> edram(kExactOutputMergerEdramDwordCount, 0);
  if (!(copying_depth ? WriteDepthSamples(edram, info, samples, error)
                      : WriteColorSamples(edram, info, samples, error)) ||
      !UploadExactOutputMergerEdram(resources, edram.data(), edram.size() * sizeof(uint32_t),
                                    &error)) {
    return false;
  }
  std::memset(resident.contents, 0xCD, resident.length);
  std::memset(guest.contents, 0xEF, guest.length);
  ExactOutputMergerResolvePlan plan;
  if (!BuildExactOutputMergerResolvePlan(info, resident.length, plan, &error)) {
    return false;
  }
  CallbackState callback;
  PipelineProbeSubmissionStats before = {};
  PipelineProbeSubmissionStats after = {};
  if (!GetExactOutputMergerSubmissionStats(resources, &before) ||
      ExecuteExactOutputMergerResolveAndClear(resources, plan,
                                              MakeDestination(resident, guest, callback), &error) !=
          ExactOutputMergerExecutionResult::kEnqueued ||
      callback.submitted != 1 || callback.failed != 0 || callback.start != plan.destination_start ||
      callback.length != plan.destination_length ||
      GetExactOutputMergerPendingDrawCount(resources) != 1 ||
      !WaitExactOutputMergerDraws(resources, &error) || callback.failed != 0 ||
      GetExactOutputMergerPendingDrawCount(resources) != 0 ||
      !GetExactOutputMergerSubmissionStats(resources, &after) ||
      after.auxiliary_command_buffer_commit_count !=
          before.auxiliary_command_buffer_commit_count + 1 ||
      after.draw_command_buffer_commit_count != before.draw_command_buffer_commit_count ||
      ReadResolvedDword(resident, info) != expected || ReadResolvedDword(guest, info) != expected) {
    if (error.empty()) {
      error = "exact resolve output, callback, pending count, or submission stats mismatched";
    }
    return false;
  }
  return true;
}

bool RunResolveSuccessCases(id<MTLDevice> device, id<MTLCommandQueue> queue, std::string& error) {
  constexpr NSUInteger kDestinationSize = 8u << 20;
  void* resources = CreateExactOutputMergerResources(device, &error);
  id<MTLBuffer> resident = [device newBufferWithLength:kDestinationSize
                                               options:MTLResourceStorageModeShared];
  id<MTLBuffer> guest = [device newBufferWithLength:kDestinationSize
                                            options:MTLResourceStorageModeShared];
  bool ok =
      resources && resident && guest && SetExactOutputMergerCommandQueue(resources, queue, &error);

  if (ok) {
    draw_util::ResolveInfo info =
        MakeColorInfo(xenos::MsaaSamples::k1X, xenos::CopySampleSelect::k0);
    info.copy_dest_info.copy_dest_swap = 1;
    info.copy_dest_info.copy_dest_endian = xenos::Endian128::k8in16;
    const uint32_t source = UINT32_C(0x44332211);
    const uint32_t swapped = (source & UINT32_C(0xFF00FF00)) |
                             ((source & UINT32_C(0x00FF0000)) >> 16) |
                             ((source & UINT32_C(0x000000FF)) << 16);
    ok = RunOneResolve(resources, resident, guest, info, {source, 0, 0, 0},
                       ApplyEndian(swapped, xenos::Endian128::k8in16), false, error);
  }
  if (ok) {
    ok = RunOneResolve(resources, resident, guest,
                       MakeColorInfo(xenos::MsaaSamples::k2X, xenos::CopySampleSelect::k01),
                       {0, UINT32_C(0x01010101), 0, 0}, UINT32_C(0x01010101), false, error);
  }
  if (ok) {
    ok = RunOneResolve(resources, resident, guest,
                       MakeColorInfo(xenos::MsaaSamples::k4X, xenos::CopySampleSelect::k23),
                       {9, 9, 0, UINT32_C(0x01010101)}, UINT32_C(0x01010101), false, error);
  }
  if (ok) {
    ok = RunOneResolve(resources, resident, guest,
                       MakeColorInfo(xenos::MsaaSamples::k4X, xenos::CopySampleSelect::k0123),
                       {0, 0, UINT32_C(0x01010101), UINT32_C(0x01010101)}, UINT32_C(0x01010101),
                       false, error);
  }
  if (ok) {
    ok = RunOneResolve(resources, resident, guest,
                       MakeDepthInfo(xenos::DepthRenderTargetFormat::kD24S8,
                                     xenos::MsaaSamples::k4X, xenos::CopySampleSelect::k3),
                       {1, 2, 3, UINT32_C(0xDEADBEEF)}, UINT32_C(0xDEADBEEF), true, error);
  }
  if (ok) {
    ok = RunOneResolve(resources, resident, guest,
                       MakeDepthInfo(xenos::DepthRenderTargetFormat::kD24FS8,
                                     xenos::MsaaSamples::k2X, xenos::CopySampleSelect::k1),
                       {UINT32_C(0x01020304), UINT32_C(0x89ABCDEF), 0, 0}, UINT32_C(0x89ABCDEF),
                       true, error);
  }

  // Copy-before-clear is guest-visible: both destinations retain the old
  // source while the authoritative exact image contains both requested clears.
  if (ok) {
    draw_util::ResolveInfo info =
        MakeColorInfo(xenos::MsaaSamples::k4X, xenos::CopySampleSelect::k0);
    info.coordinate_info.width_div_8 = 10;
    RefreshDestinationExtent(info);
    info.rb_copy_control.color_clear_enable = 1;
    info.rb_copy_control.depth_clear_enable = 1;
    info.rb_color_clear = UINT32_C(0xAABBCCDD);
    info.rb_color_clear_lo = UINT32_C(0x55667788);
    info.rb_depth_clear = UINT32_C(0x12345678);
    info.depth_edram_info.pitch_tiles =
        xenos::GetSurfacePitchTiles(160, xenos::MsaaSamples::k4X, false);
    info.depth_edram_info.msaa_samples = xenos::MsaaSamples::k4X;
    info.depth_edram_info.is_depth = 1;
    // Canonical backends clear depth before color. Deliberately alias the two
    // surfaces so this fixture observes the final side-effect ordering.
    info.depth_edram_info.base_tiles = info.color_edram_info.base_tiles;
    info.depth_edram_info.format = uint32_t(xenos::DepthRenderTargetFormat::kD24S8);
    std::vector<uint32_t> edram(kExactOutputMergerEdramDwordCount, 0);
    ok = WriteColorSamples(edram, info, {UINT32_C(0x11223344), 5, 6, 7}, error) &&
         WriteDepthSamples(edram, info, {8, 9, 10, 11}, error) &&
         UploadExactOutputMergerEdram(resources, edram.data(), edram.size() * sizeof(uint32_t),
                                      &error);
    ExactOutputMergerResolvePlan plan;
    CallbackState callback;
    if (ok) {
      ok = BuildExactOutputMergerResolvePlan(info, resident.length, plan, &error) &&
           ExecuteExactOutputMergerResolveAndClear(
               resources, plan, MakeDestination(resident, guest, callback), &error) ==
               ExactOutputMergerExecutionResult::kEnqueued &&
           WaitExactOutputMergerDraws(resources, &error) && callback.submitted == 1 &&
           callback.failed == 0 && ReadResolvedDword(resident, info) == UINT32_C(0x11223344) &&
           ReadResolvedDword(guest, info) == UINT32_C(0x11223344) &&
           DownloadExactOutputMergerEdram(resources, edram.data(), edram.size() * sizeof(uint32_t),
                                          &error);
    }
    ExactOutputMergerSurfaceConstants color_surface;
    ExactOutputMergerSurfaceConstants depth_surface;
    if (ok) {
      ok = BuildExactOutputMergerColorConstants(
               info.color_edram_info.base_tiles, 160, 160, 32, info.color_edram_info.msaa_samples,
               xenos::ColorRenderTargetFormat::k_8_8_8_8, color_surface, &error) &&
           BuildExactOutputMergerDepthConstants(
               info.depth_edram_info.base_tiles, 160, 160, 32, info.depth_edram_info.msaa_samples,
               xenos::DepthRenderTargetFormat::kD24S8, depth_surface, &error);
    }
    for (uint32_t sample = 0; ok && sample < 4; ++sample) {
      size_t color_index = 0;
      size_t depth_index = 0;
      ok = GetExactOutputMergerDwordIndex(color_surface, 28, 8, sample, 0, color_index) &&
           GetExactOutputMergerDwordIndex(depth_surface, 8, 8, sample, 0, depth_index);
      if (ok && (color_index != depth_index || edram[color_index] != UINT32_C(0xAABBCCDD))) {
        error = "overlapping clear sample " + std::to_string(sample) + " had color index " +
                std::to_string(color_index) + ", depth index " + std::to_string(depth_index) +
                ", value " + std::to_string(edram[color_index]);
        ok = false;
      }
    }
    if (!ok && error.empty()) {
      error = "copy-before-clear or depth-before-color clear ordering mismatched";
    }
  }

  // A forged overlapping publication buffer is rejected before callbacks or
  // submission, leaving the legal fallback boundary intact.
  if (ok) {
    draw_util::ResolveInfo info =
        MakeColorInfo(xenos::MsaaSamples::k1X, xenos::CopySampleSelect::k0);
    ExactOutputMergerResolvePlan plan;
    CallbackState callback;
    ok = BuildExactOutputMergerResolvePlan(info, resident.length, plan, &error);
    if (ok) {
      ExactOutputMergerResolveDestination same = MakeDestination(resident, resident, callback);
      ok = ExecuteExactOutputMergerResolveAndClear(resources, plan, same, &error) ==
               ExactOutputMergerExecutionResult::kRejectedBeforeSubmit &&
           callback.submitted == 0 && callback.failed == 0;
    }
    if (!ok && error.empty()) {
      error = "overlapping resident/guest destination was not rejected transactionally";
    }
  }

  // GoldenEye reuses a 1280-wide 4x EDRAM band three times (256 + 256 +
  // 208 rows). Keep the producer and every resolve/clear on exact EDRAM so no
  // logically self-aliasing 1280x720 native target ever becomes authoritative.
  if (ok) {
    constexpr std::array<uint32_t, 3> kBandHeights = {256, 256, 208};
    constexpr std::array<uint32_t, 3> kBandDestinations = {UINT32_C(0x010000), UINT32_C(0x160000),
                                                           UINT32_C(0x2B0000)};
    constexpr uint32_t kInitialColor = UINT32_C(0x11223344);
    constexpr uint32_t kClearColor = UINT32_C(0xAABBCCDD);
    std::vector<uint32_t> edram(kExactOutputMergerEdramDwordCount, kInitialColor);
    ok = UploadExactOutputMergerEdram(resources, edram.data(), edram.size() * sizeof(uint32_t),
                                      &error);
    std::array<draw_util::ResolveInfo, 3> infos;
    std::array<ExactOutputMergerResolvePlan, 3> plans;
    std::array<CallbackState, 3> callbacks;
    for (uint32_t band = 0; ok && band < infos.size(); ++band) {
      draw_util::ResolveInfo& info = infos[band];
      info = MakeColorInfo(xenos::MsaaSamples::k4X, xenos::CopySampleSelect::k0123);
      info.color_edram_info.pitch_tiles =
          xenos::GetSurfacePitchTiles(1280, xenos::MsaaSamples::k4X, false);
      info.color_edram_info.base_tiles = 0;
      info.coordinate_info.edram_offset_x_div_8 = 0;
      info.coordinate_info.edram_offset_y_div_8 = 0;
      info.coordinate_info.width_div_8 = 1280 / 8;
      info.height_div_8 = kBandHeights[band] / 8;
      info.copy_dest_coordinate_info.pitch_aligned_div_32 = 1280 / 32;
      info.copy_dest_coordinate_info.height_aligned_div_32 = 720 / 32 + 1;
      info.copy_dest_base = kBandDestinations[band];
      info.rb_copy_control.color_clear_enable = 1;
      info.rb_copy_control.depth_clear_enable = 1;
      info.rb_color_clear = kClearColor;
      info.rb_color_clear_lo = UINT32_C(0x55667788);
      info.rb_depth_clear = UINT32_C(0x12345678);
      info.depth_edram_info.pitch_tiles = info.color_edram_info.pitch_tiles;
      info.depth_edram_info.msaa_samples = xenos::MsaaSamples::k4X;
      info.depth_edram_info.is_depth = 1;
      info.depth_edram_info.base_tiles = 1024;
      info.depth_edram_info.format = uint32_t(xenos::DepthRenderTargetFormat::kD24S8);
      RefreshDestinationExtent(info);
      ok = BuildExactOutputMergerResolvePlan(info, resident.length, plans[band], &error) &&
           ExecuteExactOutputMergerResolveAndClear(
               resources, plans[band], MakeDestination(resident, guest, callbacks[band]), &error) ==
               ExactOutputMergerExecutionResult::kEnqueued;
    }
    if (ok) {
      ok = GetExactOutputMergerPendingDrawCount(resources) == infos.size() &&
           WaitExactOutputMergerDraws(resources, &error);
    }
    for (uint32_t band = 0; ok && band < infos.size(); ++band) {
      const uint32_t expected = band ? kClearColor : kInitialColor;
      ok = callbacks[band].submitted == 1 && callbacks[band].failed == 0 &&
           ReadResolvedDword(resident, infos[band]) == expected &&
           ReadResolvedDword(guest, infos[band]) == expected;
    }
    if (!ok && error.empty()) {
      error = "GoldenEye exact 256+256+208 band resolve/clear sequence mismatched";
    }
  }
  if (ok) {
    draw_util::ResolveInfo info =
        MakeColorInfo(xenos::MsaaSamples::k1X, xenos::CopySampleSelect::k0);
    ExactOutputMergerResolvePlan plan;
    ok = BuildExactOutputMergerResolvePlan(info, resident.length, plan, &error) &&
         ProbeExactOutputMergerRejectsEdramResolveAliasesForInternalProbe(
             resources, plan, resident, resident.length, &error) &&
         GetExactOutputMergerPendingDrawCount(resources) == 0;
    if (!ok && error.empty()) {
      error = "canonical EDRAM destination aliases were not rejected transactionally";
    }
  }

  ReleaseExactOutputMergerResources(resources);
  [resident release];
  [guest release];
  return ok;
}

bool RunDependentFailureCase(id<MTLDevice> device, id<MTLCommandQueue> queue, std::string& error) {
  constexpr NSUInteger kDestinationSize = 1u << 20;
  FaultCounters fault_counters;
  auto* fault_queue = [[ExactResolveFaultQueue alloc] initWithInner:queue counters:&fault_counters];
  void* resources = CreateExactOutputMergerResources(device, &error);
  id<MTLBuffer> resident = [device newBufferWithLength:kDestinationSize
                                               options:MTLResourceStorageModeShared];
  id<MTLBuffer> guest = [device newBufferWithLength:kDestinationSize
                                            options:MTLResourceStorageModeShared];
  bool ok = fault_queue && resources && resident && guest &&
            SetExactOutputMergerCommandQueue(resources, fault_queue, &error);
  std::vector<uint32_t> edram(kExactOutputMergerEdramDwordCount, 0);
  draw_util::ResolveInfo first_info =
      MakeColorInfo(xenos::MsaaSamples::k1X, xenos::CopySampleSelect::k0);
  ExactOutputMergerResolvePlan first_plan;
  ExactOutputMergerResolvePlan second_plan;
  CallbackState first_callback;
  CallbackState second_callback;
  if (ok) {
    ok = WriteColorSamples(edram, first_info, {UINT32_C(0x10203040), 0, 0, 0}, error) &&
         UploadExactOutputMergerEdram(resources, edram.data(), edram.size() * sizeof(uint32_t),
                                      &error) &&
         BuildExactOutputMergerResolvePlan(first_info, resident.length, first_plan, &error);
  }
  draw_util::ResolveInfo second_info = first_info;
  second_info.copy_dest_base = UINT32_C(0x8000);
  RefreshDestinationExtent(second_info);
  if (ok) {
    ok = BuildExactOutputMergerResolvePlan(second_info, resident.length, second_plan, &error);
  }
  PipelineProbeSubmissionStats before = {};
  PipelineProbeSubmissionStats after = {};
  if (ok) {
    [fault_queue injectNextFailure];
    ok = GetExactOutputMergerSubmissionStats(resources, &before) &&
         ExecuteExactOutputMergerResolveAndClear(
             resources, first_plan, MakeDestination(resident, guest, first_callback), &error) ==
             ExactOutputMergerExecutionResult::kEnqueued &&
         ExecuteExactOutputMergerResolveAndClear(
             resources, second_plan, MakeDestination(resident, guest, second_callback), &error) ==
             ExactOutputMergerExecutionResult::kEnqueued &&
         first_callback.submitted == 1 && second_callback.submitted == 1 &&
         GetExactOutputMergerPendingDrawCount(resources) == 2;
  }
  if (ok) {
    constexpr uint32_t kCompletionAddress = UINT32_C(0x10000);
    constexpr uint32_t kCompletionSentinel = UINT32_C(0xCCCCCCCC);
    constexpr uint32_t kCompletionValue = UINT32_C(0x12345678);
    rex::memory::Memory memory;
    ok = memory.Initialize();
    if (ok) {
      ProbeGraphicsSystem graphics_system(memory);
      MetalCommandProcessor command_processor(&graphics_system, nullptr);
      MetalCommandProcessorTestPeer::AttachExactResources(command_processor, resources);
      std::memcpy(memory.TranslatePhysical(kCompletionAddress), &kCompletionSentinel,
                  sizeof(kCompletionSentinel));
      const bool completion_published = MetalCommandProcessorTestPeer::WriteGuestCompletion(
          command_processor, kCompletionAddress, kCompletionValue);
      uint32_t observed_completion = 0;
      std::memcpy(&observed_completion, memory.TranslatePhysical(kCompletionAddress),
                  sizeof(observed_completion));
      ok = !completion_published && observed_completion == kCompletionSentinel &&
           MetalCommandProcessorTestPeer::PendingGuestCompletionCount(command_processor) == 1 &&
           first_callback.failed == 1 && second_callback.failed == 1 &&
           GetExactOutputMergerPendingDrawCount(resources) == 0 &&
           GetExactOutputMergerSubmissionStats(resources, &after) &&
           after.auxiliary_command_buffer_commit_count ==
               before.auxiliary_command_buffer_commit_count + 2 &&
           fault_counters.injected == 1;
      MetalCommandProcessorTestPeer::DetachExactResourcesAndDiscardCompletion(command_processor);
    }
  }
  if (ok) {
    std::string second_wait_error;
    const bool second_wait = WaitExactOutputMergerDraws(resources, &second_wait_error);
    ok = !second_wait && first_callback.failed == 1 && second_callback.failed == 1;
  }
  if (!ok && error.empty()) {
    error = "failed exact resolve published a guest completion or did not revoke dependents";
  }

  ReleaseExactOutputMergerResources(resources);
  [resident release];
  [guest release];
  [fault_queue release];
  return ok;
}

bool RunFailedPredecessorUploadFenceCase(id<MTLDevice> device, std::string& error) {
  constexpr uint32_t kUploadAddress = UINT32_C(0x18000);
  constexpr uint32_t kCompletionAddress = UINT32_C(0x1C000);
  constexpr uint32_t kCompletionSentinel = UINT32_C(0xCCCCCCCC);
  constexpr uint32_t kCompletionValue = UINT32_C(0x12345678);
  rex::memory::Memory memory;
  if (!memory.Initialize()) {
    error = "failed to initialize guest memory for predecessor upload fault";
    return false;
  }
  TraceWriter trace_writer(memory.physical_membase());
  auto shared_memory = std::make_unique<MetalSharedMemory>(memory, trace_writer);
  if (!shared_memory->Initialize(device)) {
    error = "failed to initialize Metal shared memory for predecessor upload fault";
    return false;
  }
  FaultCounters fault_counters;
  auto* fault_queue = [[ExactResolveFaultQueue alloc]
      initWithInner:(id<MTLCommandQueue>)shared_memory->command_queue()
           counters:&fault_counters];
  MetalSharedMemoryTestPeer::ReplaceCommandQueue(*shared_memory, fault_queue);
  [fault_queue injectNextFailure];
  constexpr uint32_t kUploadValue = UINT32_C(0xA5A55A5A);
  std::memcpy(memory.TranslatePhysical(kUploadAddress), &kUploadValue, sizeof(kUploadValue));
  bool ok = shared_memory->RequestRange(kUploadAddress, sizeof(kUploadValue));

  ProbeGraphicsSystem graphics_system(memory);
  MetalCommandProcessor command_processor(&graphics_system, nullptr);
  MetalCommandProcessorTestPeer::AttachSharedMemory(command_processor, std::move(shared_memory));
  std::memcpy(memory.TranslatePhysical(kCompletionAddress), &kCompletionSentinel,
              sizeof(kCompletionSentinel));
  const bool completion_published = MetalCommandProcessorTestPeer::WriteGuestCompletion(
      command_processor, kCompletionAddress, kCompletionValue);
  uint32_t observed_completion = 0;
  std::memcpy(&observed_completion, memory.TranslatePhysical(kCompletionAddress),
              sizeof(observed_completion));
  ok = ok && !completion_published && observed_completion == kCompletionSentinel &&
       MetalCommandProcessorTestPeer::PendingGuestCompletionCount(command_processor) == 1 &&
       fault_counters.injected == 1;
  MetalCommandProcessorTestPeer::DetachExactResourcesAndDiscardCompletion(command_processor);
  [fault_queue release];
  if (!ok) {
    error = "failed predecessor upload was consumed before blocking guest completion";
  }
  return ok;
}

int Run() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!device || !queue) {
      std::fprintf(stderr,
                   "[metal_exact_resolve_probe_test] FAIL: Metal device/queue unavailable\n");
      [queue release];
      return 1;
    }
    std::string error;
    const bool success_cases = RunResolveSuccessCases(device, queue, error);
    const bool failure_case = success_cases && RunDependentFailureCase(device, queue, error);
    const bool upload_failure_case =
        failure_case && RunFailedPredecessorUploadFenceCase(device, error);
    [queue release];
    if (!success_cases || !failure_case || !upload_failure_case) {
      std::fprintf(stderr, "[metal_exact_resolve_probe_test] FAIL: %s\n",
                   error.empty() ? "unknown exact resolve failure" : error.c_str());
      return 1;
    }
    std::fprintf(stdout, "[metal_exact_resolve_probe_test] PASS: exact 8888/depth resolves, "
                         "MSAA half-up ties, endian/swap, copy-before-clear, GoldenEye 3-band "
                         "resolve/clear, bounded publication, "
                         "dependent async failure revocation, predecessor-upload rejection, and "
                         "completion fencing matched\n");
    return 0;
  }
}

}  // namespace

int main() {
  return Run();
}
