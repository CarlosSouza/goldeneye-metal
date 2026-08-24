#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <rex/graphics/xenos.h>

namespace rex::graphics::metal {

// Canonical Xenos EDRAM is always addressed as 2048 wrapping 80x16 tiles of
// 32-bit words. A 64-bpp sample consumes two adjacent words and therefore two
// tiles for every 80 samples in a row.
struct CanonicalEdramSurfaceLayout {
  uint32_t base_tiles = 0;
  uint32_t pitch_tiles = 0;
  xenos::MsaaSamples msaa_samples = xenos::MsaaSamples::k1X;
  bool is_64bpp = false;
  // Xenos depth/stencil swaps the two 40x16-sample halves inside every
  // physical 80x16 tile relative to 32-bpp color.
  bool is_depth = false;
};

enum class CanonicalEdramSurfaceLayoutClass : uint8_t {
  kInvalid,
  kNonAliasing,
  kSelfAliasing,
};

// Classifies whether a logical surface has a one-to-one mapping to physical
// EDRAM words. A self-aliasing private target may be read from a canonical
// snapshot, but it can't become an authoritative export owner after a native
// write because multiple independent Metal samples map to the same Xenos
// word.
CanonicalEdramSurfaceLayoutClass ClassifyCanonicalEdramSurfaceLayout(
    const CanonicalEdramSurfaceLayout& layout, uint32_t width, uint32_t height);

// Native self-alias containment is relevant only while a canonical image may
// be consumed by private targets or exact EDRAM is authoritative. With both
// states inactive (the default exact-output-merger-off route), native target
// tracking must retain its legacy behavior.
bool ShouldContainCanonicalEdramNativeAlias(bool target_hydration_enabled, bool exact_gpu_current);

constexpr uint32_t GetCanonicalEdramSampleCount(xenos::MsaaSamples msaa_samples) {
  return uint32_t(1) << uint32_t(msaa_samples);
}

// Exact private-target round trips are currently limited by the native Metal
// backing formats. Keep these predicates shared by capture and trace preflight
// so unsupported traces fail before replay rather than degrading silently.
bool IsCanonicalEdramColorFormatSupportedByMetal(xenos::ColorRenderTargetFormat format);
bool IsCanonicalEdramDepthFormatSupportedByMetal(xenos::DepthRenderTargetFormat format);
bool IsCanonicalEdramMsaaSupportedByMetal(xenos::MsaaSamples msaa_samples);

// Returns the canonical dword index for one component of one guest sample.
// The result is periodic across the physical 10 MiB EDRAM allocation.
size_t GetCanonicalEdramDwordIndex(const CanonicalEdramSurfaceLayout& layout, uint32_t x,
                                   uint32_t y, uint32_t sample, uint32_t dword);

bool ReadCanonicalEdramSample(std::span<const uint8_t> edram,
                              const CanonicalEdramSurfaceLayout& layout, uint32_t x, uint32_t y,
                              uint32_t sample, std::array<uint32_t, 2>& words_out);
bool WriteCanonicalEdramSample(std::span<uint8_t> edram,
                               const CanonicalEdramSurfaceLayout& layout, uint32_t x, uint32_t y,
                               uint32_t sample, const std::array<uint32_t, 2>& words);

// Converts between the logical RGBA value used by the Metal compatibility
// target and the exact Xenos storage layout. All defined color render-target
// storage formats are covered, including 64-bpp and floating-point formats.
bool PackCanonicalEdramColor(const std::array<float, 4>& rgba,
                             xenos::ColorRenderTargetFormat format,
                             std::array<uint32_t, 2>& words_out);
bool UnpackCanonicalEdramColor(const std::array<uint32_t, 2>& words,
                               xenos::ColorRenderTargetFormat format,
                               std::array<float, 4>& rgba_out);

enum class CanonicalEdramOwnerKind : uint8_t {
  kRawSnapshot,
  kColorTarget,
  kDepthStencilTarget,
};

struct CanonicalEdramTileOwner {
  CanonicalEdramOwnerKind kind = CanonicalEdramOwnerKind::kRawSnapshot;
  uint64_t target_key = 0;
  uint64_t sequence = 0;

  bool operator==(const CanonicalEdramTileOwner&) const = default;
};

// Separates having a complete canonical image from being allowed to import it
// into private Metal targets. Live captures are serialization-only: the
// full-surface ownership model may wrap and overlap physical EDRAM tiles, so
// promoting a live capture would make color and depth targets repeatedly
// overwrite each other. Only an explicit trace restore or a completed exact
// output-merger publication establishes hydration authority.
class CanonicalEdramAuthorityState {
 public:
  void Reset();
  void RecordCapture();
  void RecordRestore();
  // A completed exact output-merger draw publishes another complete raw
  // canonical image and is therefore a safe private-target hydration source.
  void RecordExactDraw();
  bool has_snapshot() const { return has_snapshot_; }
  bool target_hydration_enabled() const { return has_snapshot_ && target_hydration_enabled_; }

 private:
  bool has_snapshot_ = false;
  bool target_hydration_enabled_ = false;
};

// Tracks which private Metal target contains the authoritative value of each
// physical EDRAM tile. This prevents stale aliased target caches from
// overwriting newer contents when a canonical trace snapshot is exported.
class CanonicalEdramTileOwnership {
 public:
  void Reset();
  uint64_t MarkSurface(const CanonicalEdramSurfaceLayout& layout, uint32_t width, uint32_t height,
                       CanonicalEdramOwnerKind kind, uint64_t target_key);
  bool SurfaceNeedsHydration(const CanonicalEdramSurfaceLayout& layout, uint32_t width,
                             uint32_t height, CanonicalEdramOwnerKind kind, uint64_t target_key,
                             uint64_t hydrated_sequence) const;

  const CanonicalEdramTileOwner& owner(uint32_t tile) const { return owners_[tile]; }
  uint64_t sequence() const { return sequence_; }

  // Copies only tiles currently owned by (kind, target_key). Source and
  // destination must both be complete 10 MiB canonical images.
  bool MergeOwnedTiles(std::span<uint8_t> destination, std::span<const uint8_t> source,
                       CanonicalEdramOwnerKind kind, uint64_t target_key) const;

 private:
  std::array<CanonicalEdramTileOwner, xenos::kEdramTileCount> owners_ = {};
  uint64_t sequence_ = 0;
};

}  // namespace rex::graphics::metal
