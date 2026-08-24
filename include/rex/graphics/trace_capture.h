/**
 ******************************************************************************
 * ReXGlue - Xbox 360 recompilation runtime                                  *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                       *
 *                                                                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

#include <rex/graphics/trace_writer.h>

namespace rex::graphics {

enum class FrameTraceCaptureState {
  kIdle,
  kQueued,
  kArmed,
  kCapturing,
  kComplete,
  kCompleteBestEffortOnly,
  kFailed,
  kCancelled,
};

struct FrameTraceCaptureStatus {
  uint64_t request_token = 0;
  FrameTraceCaptureState state = FrameTraceCaptureState::kIdle;
  std::string reason;
  std::filesystem::path final_path;
  uint64_t byte_count = 0;

  bool terminal() const {
    return state == FrameTraceCaptureState::kComplete ||
           state == FrameTraceCaptureState::kCompleteBestEffortOnly ||
           state == FrameTraceCaptureState::kFailed ||
           state == FrameTraceCaptureState::kCancelled;
  }
};

// Owns the single private one-frame capture slot. The caller supplies a
// dedicated, trusted root (the macOS app-support trace directory in production,
// or a temporary directory in tests). Only `latest.xtr.partial` and
// `latest.xtr` are ever used beneath that root.
class FrameTraceCaptureSlot {
 public:
  static constexpr uint64_t kDefaultByteBudget =
      TraceWriter::kDefaultByteBudget;
  static constexpr size_t kMaximumReasonLength = 240;
  static constexpr size_t kMaximumPathLength = 1024;

  FrameTraceCaptureSlot() = default;
  ~FrameTraceCaptureSlot();

  FrameTraceCaptureSlot(const FrameTraceCaptureSlot&) = delete;
  FrameTraceCaptureSlot& operator=(const FrameTraceCaptureSlot&) = delete;

  // Returns zero when another request already owns the slot. Invalid roots get
  // a nonzero token with an immediately observable Failed status.
  uint64_t Queue(const std::filesystem::path& safe_root,
                 uint64_t byte_budget = kDefaultByteBudget);
  // Records a failed user request when preparation outside the graphics
  // worker (such as creating the private root) could not be completed.
  uint64_t RecordFailedRequest(std::string reason);
  bool Arm(uint64_t token);

  // Creates the fixed partial with O_EXCL, O_NOFOLLOW and mode 0600. Ownership
  // of the returned descriptor transfers to TraceWriter.
  int CreatePartial(uint64_t token, std::filesystem::path* display_path_out);
  bool MarkCapturing(uint64_t token);

  // Verifies the closed partial and atomically publishes it as latest.xtr.
  // Deterministic captures become Complete; readable captures with an explicit
  // but incomplete replay contract become CompleteBestEffortOnly.
  bool Publish(uint64_t token);
  void Fail(uint64_t token, std::string reason);
  void Cancel(uint64_t token, std::string reason);

  // Removes only the published latest.xtr entry from an explicitly supplied
  // safe root. Active captures are never interrupted, symbolic links and
  // non-regular entries are rejected, and the partial is never touched.
  bool DeletePublishedCapture(const std::filesystem::path& safe_root,
                              std::string* reason_out = nullptr);

  FrameTraceCaptureStatus status() const;
  uint64_t byte_budget(uint64_t token) const;
  bool owns_active_request(uint64_t token) const;

  // Fault seam for unit coverage of publication failures. This never changes
  // the trace format or production behavior.
  void SetRenameFailureForTesting(bool fail);
  void SetSyncFailureForTesting(bool fail);

 private:
  static bool IsActiveState(FrameTraceCaptureState state);
  static std::string BoundedReason(std::string reason);
  static std::filesystem::path BoundedPath(const std::filesystem::path& path);

  bool OpenSafeRootLocked(std::string* reason_out);
  void CleanupPartialLocked();
  void CloseRootLocked();
  void SetTerminalLocked(FrameTraceCaptureState state, std::string reason,
                         const std::filesystem::path& final_path = {},
                         uint64_t byte_count = 0);

  mutable std::mutex mutex_;
  FrameTraceCaptureStatus status_;
  uint64_t next_token_ = 1;
  uint64_t byte_budget_ = kDefaultByteBudget;
  std::filesystem::path root_;
  int root_descriptor_ = -1;
  bool partial_created_ = false;
  bool fail_rename_for_testing_ = false;
  bool fail_sync_for_testing_ = false;
};

const char* FrameTraceCaptureStateName(FrameTraceCaptureState state);

}  // namespace rex::graphics
