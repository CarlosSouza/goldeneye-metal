// GoldenEye Metal - private one-frame GPU capture UI and diagnostic helpers.

#pragma once

#include <rex/graphics/trace_capture.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace ge::gpu_capture {

constexpr char kCaptureDirectoryName[] = "GPU Captures";
constexpr char kCompletedCaptureName[] = "latest.xtr";
constexpr uint64_t kMaximumCaptureBytes =
    rex::graphics::FrameTraceCaptureSlot::kDefaultByteBudget;
constexpr bool kIncludeInDiagnosticsByDefault = false;
// Reserved inside the launcher's total diagnostic byte budget for README and
// capture metadata whose final sizes are known only after collection.
constexpr uint64_t kDiagnosticMetadataAllowance = 1ull * 1024ull * 1024ull;

// Read-only state used by the developer harness to prove that a requested
// trace starts in live Dam gameplay rather than during a menu or transition.
// The production app never instantiates this gate.
struct DamGameplayObservation {
  int32_t level_id = -1;
  int32_t player_count = 0;
  bool network_session = false;
  bool player_valid = false;
  bool position_valid = false;
  uint32_t player = 0;
  uint32_t coordinates = 0;
  uint32_t guest_frame = 0;
  uint32_t present = 0;
  uint32_t pause = 0;
  uint32_t control_disabled = 0;
  uint32_t watch = 0;
};

class DamGameplayCaptureGate {
 public:
  static constexpr int32_t kDamLevelId = 0x21;

  // Returns true exactly once, after two consecutive eligible observations
  // identify the same live player and both guest and presentation counters
  // advance. Any transition or ineligible sample resets the proof window.
  bool Observe(const DamGameplayObservation& observation) noexcept;
  bool ready() const noexcept { return ready_; }

 private:
  bool has_previous_ = false;
  bool ready_ = false;
  DamGameplayObservation previous_;
};

uint64_t BoundedDiagnosticCaptureAllowance(uint64_t already_counted_bytes,
                                           uint64_t bundle_byte_limit,
                                           uint64_t capture_byte_limit =
                                               kMaximumCaptureBytes);

struct StatusPresentation {
  std::string label;
  std::string detail;
  bool active = false;
  bool can_request = true;
  bool can_delete = false;
};

StatusPresentation PresentStatus(
    const rex::graphics::FrameTraceCaptureStatus& status);

// The menu marks a request, closes, resumes gameplay through on_closed, then
// consumes it. This makes it impossible for the capture to be queued while the
// host settings page is still the presented frame.
class DeferredMenuRequest {
 public:
  void Queue() { pending_ = true; }
  bool pending() const { return pending_; }
  bool TakeAfterClose(bool quitting);

 private:
  bool pending_ = false;
};

struct CompletedCaptureInfo {
  bool available = false;
  rex::graphics::FrameTraceCaptureState state =
      rex::graphics::FrameTraceCaptureState::kIdle;
  std::string reason;
  uint64_t byte_count = 0;
  std::string build_revision;
};

std::filesystem::path CaptureRoot(
    const std::filesystem::path& user_data_root);

// Creates the fixed capture directory through a held, non-symlink parent and
// tightens it to owner-only mode 0700.
bool EnsurePrivateRoot(const std::filesystem::path& user_data_root,
                       std::filesystem::path* capture_root,
                       std::string* error);

// Read-only availability probe for the launcher. It validates a held
// latest.xtr descriptor structurally and never considers latest.xtr.partial.
CompletedCaptureInfo InspectCompletedCapture(
    const std::filesystem::path& user_data_root,
    uint64_t maximum_bytes = kMaximumCaptureBytes);

// Adds a fixed "GPU Capture" directory to an already private diagnostic
// workspace. The source is re-opened and validated immediately before a
// bounded binary copy. On any failure all destination entries are removed and
// the source capture is left unchanged.
bool CopyCompletedCaptureForDiagnostics(
    const std::filesystem::path& user_data_root,
    const std::filesystem::path& diagnostic_bundle_root,
    CompletedCaptureInfo* copied_info, std::string* error,
    uint64_t maximum_bytes = kMaximumCaptureBytes);

std::string BuildMetadata(const CompletedCaptureInfo& info);

}  // namespace ge::gpu_capture
