#import <Metal/Metal.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <rex/ui/metal/presenter.h>

namespace {

using rex::ui::RawImage;
using rex::ui::metal::MetalPresenter;

class ScopedMetalDevice {
 public:
  ScopedMetalDevice() : device_(MTLCreateSystemDefaultDevice()) {}
  ~ScopedMetalDevice() {
    if (device_) {
      [device_ release];
    }
  }

  ScopedMetalDevice(const ScopedMetalDevice&) = delete;
  ScopedMetalDevice& operator=(const ScopedMetalDevice&) = delete;

  id<MTLDevice> get() const { return device_; }

 private:
  id<MTLDevice> device_;
};

constexpr uint32_t kWidth = 3;
constexpr uint32_t kHeight = 2;
constexpr size_t kPackedRowPitch = size_t(kWidth) * 4;
constexpr uint32_t kIdentitySwizzle = 0x688;
// Output R <- source B, G <- source R, B <- one, A <- source A.
constexpr uint32_t kBlueRedOneAlphaSwizzle = 2 | (0 << 3) | (5 << 6) | (3 << 9);

const std::vector<uint8_t> kSourceBgra = {
    10,  20,  30,  40,  50,  60,  70,  80,  90,  100, 110, 120,
    130, 140, 150, 160, 170, 180, 190, 200, 210, 220, 230, 240,
};

const std::vector<uint8_t> kExpectedIdentityRgbx = {
    30,  20,  10,  255, 70,  60,  50,  255, 110, 100, 90,  255,
    150, 140, 130, 255, 190, 180, 170, 255, 230, 220, 210, 255,
};

const std::vector<uint8_t> kExpectedSwizzledRgbx = {
    10,  30,  255, 255, 50,  70,  255, 255, 90,  110, 255, 255,
    130, 150, 255, 255, 170, 190, 255, 255, 210, 230, 255, 255,
};

std::unique_ptr<MetalPresenter> CreatePresenter(id<MTLDevice> device) {
  return MetalPresenter::Create((void*)device, [](bool, bool) {});
}

void CheckImage(const RawImage& image, const std::vector<uint8_t>& expected) {
  CHECK(image.width == kWidth);
  CHECK(image.height == kHeight);
  CHECK(image.stride == kPackedRowPitch);
  CHECK(image.data == expected);
}

bool PublishCpuFallback(MetalPresenter& presenter) {
  constexpr size_t kPaddedRowPitch = kPackedRowPitch + 8;
  std::vector<uint8_t> padded_bgra(kPaddedRowPitch * kHeight, 0xCD);
  for (uint32_t y = 0; y < kHeight; ++y) {
    std::memcpy(padded_bgra.data() + size_t(y) * kPaddedRowPitch,
                kSourceBgra.data() + size_t(y) * kPackedRowPitch, kPackedRowPitch);
  }
  presenter.UpdateGuestFrontbuffer(kWidth, kHeight, padded_bgra.data(), kPaddedRowPitch);
  return presenter.RefreshGuestOutput(kWidth, kHeight, kWidth, kHeight,
                                      [](rex::ui::Presenter::GuestOutputRefreshContext& context) {
                                        context.SetIs8bpc(true);
                                        return true;
                                      });
}

bool AlignUp(size_t value, size_t alignment, size_t& aligned_out) {
  if (!alignment) {
    return false;
  }
  size_t remainder = value % alignment;
  if (!remainder) {
    aligned_out = value;
    return true;
  }
  size_t padding = alignment - remainder;
  if (value > std::numeric_limits<size_t>::max() - padding) {
    return false;
  }
  aligned_out = value + padding;
  return true;
}

bool PublishDirect(MetalPresenter& presenter, id<MTLDevice> device, uint32_t guest_swizzle) {
  return presenter.RefreshGuestOutput(
      kWidth, kHeight, kWidth, kHeight,
      [device, guest_swizzle](rex::ui::Presenter::GuestOutputRefreshContext& base_context) {
        auto* context =
            dynamic_cast<MetalPresenter::MetalGuestOutputRefreshContext*>(&base_context);
        if (!context || !context->writable_texture() || !context->command_queue()) {
          return false;
        }

        id<MTLTexture> texture = (id<MTLTexture>)context->writable_texture();
        id<MTLCommandQueue> command_queue = (id<MTLCommandQueue>)context->command_queue();
        size_t row_alignment =
            size_t([device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm]);
        size_t staging_row_pitch = 0;
        if (!AlignUp(kPackedRowPitch, row_alignment, staging_row_pitch) ||
            size_t(kHeight) > std::numeric_limits<size_t>::max() / staging_row_pitch) {
          return false;
        }
        size_t staging_size = staging_row_pitch * kHeight;
        id<MTLBuffer> staging = [device newBufferWithLength:staging_size
                                                    options:MTLResourceStorageModeShared];
        if (!staging || !staging.contents) {
          [staging release];
          return false;
        }
        for (uint32_t y = 0; y < kHeight; ++y) {
          std::memcpy(static_cast<uint8_t*>(staging.contents) + size_t(y) * staging_row_pitch,
                      kSourceBgra.data() + size_t(y) * kPackedRowPitch, kPackedRowPitch);
        }

        id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
        id<MTLBlitCommandEncoder> blit_encoder =
            command_buffer ? [command_buffer blitCommandEncoder] : nil;
        if (!command_buffer || !blit_encoder) {
          [staging release];
          return false;
        }
        [blit_encoder copyFromBuffer:staging
                        sourceOffset:0
                   sourceBytesPerRow:staging_row_pitch
                 sourceBytesPerImage:staging_size
                          sourceSize:MTLSizeMake(kWidth, kHeight, 1)
                           toTexture:texture
                    destinationSlice:0
                    destinationLevel:0
                   destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit_encoder endEncoding];
        [command_buffer commit];
        // Normal Metal command buffers retain referenced resources. Drop the
        // creator reference now so the presenter must provide the queue
        // ordering and completion wait promised by the refresh API.
        [staging release];

        context->SetIs8bpc(true);
        context->SetDirectOutputValid(true, guest_swizzle);
        return true;
      });
}

}  // namespace

TEST_CASE("Metal presenter captures inactive and CPU fallback guest output",
          "[ui][metal][capture]") {
  @autoreleasepool {
    ScopedMetalDevice device_owner;
    id<MTLDevice> device = device_owner.get();
    if (!device) {
      SKIP("No Metal device is available in this environment");
    }
    std::unique_ptr<MetalPresenter> presenter = CreatePresenter(device);
    REQUIRE(presenter != nullptr);

    RawImage image{99, 88, 77, {1, 2, 3}};
    CHECK_FALSE(presenter->CaptureGuestOutput(image));
    CHECK(image.width == 0);
    CHECK(image.height == 0);
    CHECK(image.stride == 0);
    CHECK(image.data.empty());

    REQUIRE(PublishCpuFallback(*presenter));
    REQUIRE(presenter->CaptureGuestOutput(image));
    CheckImage(image, kExpectedIdentityRgbx);

    // Updating the CPU source alone must not mutate the already-published
    // mailbox image that deterministic capture has acquired.
    std::vector<uint8_t> unpublished_bgra(kSourceBgra.size(), 1);
    presenter->UpdateGuestFrontbuffer(kWidth, kHeight, std::move(unpublished_bgra));
    REQUIRE(presenter->CaptureGuestOutput(image));
    CheckImage(image, kExpectedIdentityRgbx);
  }
}

TEST_CASE("Metal presenter captures private BGRA output with guest swizzle",
          "[ui][metal][capture]") {
  @autoreleasepool {
    ScopedMetalDevice device_owner;
    id<MTLDevice> device = device_owner.get();
    if (!device) {
      SKIP("No Metal device is available in this environment");
    }
    std::unique_ptr<MetalPresenter> presenter = CreatePresenter(device);
    REQUIRE(presenter != nullptr);

    REQUIRE(PublishDirect(*presenter, device, kIdentitySwizzle));
    RawImage image;
    REQUIRE(presenter->CaptureGuestOutput(image));
    CheckImage(image, kExpectedIdentityRgbx);

    REQUIRE(PublishDirect(*presenter, device, kBlueRedOneAlphaSwizzle));
    REQUIRE(presenter->CaptureGuestOutput(image));
    CheckImage(image, kExpectedSwizzledRgbx);
  }
}
