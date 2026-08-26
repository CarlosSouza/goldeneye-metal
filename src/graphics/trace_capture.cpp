/**
 ******************************************************************************
 * ReXGlue - Xbox 360 recompilation runtime                                  *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                       *
 *                                                                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cerrno>
#include <cstring>
#include <limits>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <rex/graphics/trace_capture.h>
#include <rex/graphics/trace_reader.h>
#include <rex/logging.h>

namespace rex::graphics {

namespace {

constexpr char kPartialName[] = "latest.xtr.partial";
constexpr char kFinalName[] = "latest.xtr";

std::string ErrnoReason(const char* action) {
  return std::string(action) + ": " + std::strerror(errno);
}

#ifndef _WIN32
int OpenDirectoryWithoutSymlinks(const std::filesystem::path& root,
                                 std::string* reason_out) {
  if (!root.is_absolute()) {
    *reason_out = "capture root must be absolute";
    return -1;
  }

  int descriptor = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0) {
    *reason_out = ErrnoReason("could not open filesystem root");
    return -1;
  }
  for (const auto& component_path : root.lexically_normal().relative_path()) {
    const std::string component = component_path.string();
    if (component.empty() || component == ".") {
      continue;
    }
    if (component == "..") {
      *reason_out = "capture root contains parent traversal";
      ::close(descriptor);
      return -1;
    }
    const int next = ::openat(descriptor, component.c_str(),
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (next < 0) {
      *reason_out = ErrnoReason("capture root is missing, not a directory, or contains a symlink");
      ::close(descriptor);
      return -1;
    }
    ::close(descriptor);
    descriptor = next;
  }
  return descriptor;
}

bool DirectoryPathStillMatchesDescriptor(const std::filesystem::path& root,
                                         int held_descriptor) {
  std::string ignored_reason;
  const int current_descriptor =
      OpenDirectoryWithoutSymlinks(root, &ignored_reason);
  if (current_descriptor < 0) {
    return false;
  }

  struct stat held_status {};
  struct stat current_status {};
  const bool matches = ::fstat(held_descriptor, &held_status) == 0 &&
                       ::fstat(current_descriptor, &current_status) == 0 &&
                       held_status.st_dev == current_status.st_dev &&
                       held_status.st_ino == current_status.st_ino;
  ::close(current_descriptor);
  return matches;
}
#endif

}  // namespace

FrameTraceCaptureSlot::~FrameTraceCaptureSlot() {
  std::lock_guard<std::mutex> lock(mutex_);
  CleanupPartialLocked();
  CloseRootLocked();
}

bool FrameTraceCaptureSlot::IsActiveState(FrameTraceCaptureState state) {
  return state == FrameTraceCaptureState::kQueued ||
         state == FrameTraceCaptureState::kArmed ||
         state == FrameTraceCaptureState::kCapturing;
}

std::string FrameTraceCaptureSlot::BoundedReason(std::string reason) {
  if (reason.size() > kMaximumReasonLength) {
    reason.resize(kMaximumReasonLength);
  }
  return reason;
}

std::filesystem::path FrameTraceCaptureSlot::BoundedPath(
    const std::filesystem::path& path) {
  const std::string value = path.string();
  if (value.size() <= kMaximumPathLength) {
    return path;
  }
  return std::filesystem::path(value.substr(0, kMaximumPathLength));
}

uint64_t FrameTraceCaptureSlot::Queue(const std::filesystem::path& safe_root,
                                      uint64_t byte_budget) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (IsActiveState(status_.state)) {
    return 0;
  }

  CleanupPartialLocked();
  CloseRootLocked();
  status_ = {};
  status_.request_token = next_token_++;
  if (!status_.request_token) {
    status_.request_token = next_token_++;
  }
  root_ = safe_root;
  byte_budget_ = byte_budget;

  const std::string root_string = safe_root.string();
  if (!safe_root.is_absolute()) {
    SetTerminalLocked(FrameTraceCaptureState::kFailed,
                      "capture root must be an absolute path");
  } else if (root_string.size() + 1 + sizeof(kPartialName) - 1 >
             kMaximumPathLength) {
    SetTerminalLocked(FrameTraceCaptureState::kFailed,
                      "capture root path exceeds the supported length");
  } else if (!byte_budget || byte_budget > kDefaultByteBudget) {
    SetTerminalLocked(FrameTraceCaptureState::kFailed,
                      "capture byte budget must be between 1 byte and 192 MiB");
  } else {
    std::error_code root_error;
    const auto supplied_status = std::filesystem::symlink_status(safe_root, root_error);
    if (root_error || !std::filesystem::is_directory(supplied_status)) {
      SetTerminalLocked(FrameTraceCaptureState::kFailed,
                        "capture root must already exist as a directory");
    } else if (std::filesystem::is_symlink(supplied_status)) {
      SetTerminalLocked(FrameTraceCaptureState::kFailed,
                        "capture root may not be a symbolic link");
    } else {
      // Resolve platform-owned aliases such as macOS /var -> /private/var once,
      // then walk and retain the resulting directory hierarchy with openat and
      // O_NOFOLLOW. The supplied root itself is still required not to be a
      // symlink, and no later path lookup can escape the held descriptor.
      root_ = std::filesystem::canonical(safe_root, root_error);
      if (root_error ||
          root_.string().size() + 1 + sizeof(kPartialName) - 1 >
              kMaximumPathLength) {
        SetTerminalLocked(FrameTraceCaptureState::kFailed,
                          "capture root could not be resolved safely");
      } else {
        status_.state = FrameTraceCaptureState::kQueued;
      }
    }
  }
  return status_.request_token;
}

uint64_t FrameTraceCaptureSlot::RecordFailedRequest(std::string reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (IsActiveState(status_.state)) {
    return 0;
  }
  CleanupPartialLocked();
  CloseRootLocked();
  status_ = {};
  status_.request_token = next_token_++;
  if (!status_.request_token) {
    status_.request_token = next_token_++;
  }
  root_.clear();
  SetTerminalLocked(FrameTraceCaptureState::kFailed, std::move(reason));
  return status_.request_token;
}

bool FrameTraceCaptureSlot::OpenSafeRootLocked(std::string* reason_out) {
  if (root_descriptor_ >= 0) {
    return true;
  }
#ifdef _WIN32
  std::error_code error;
  const auto root_status = std::filesystem::symlink_status(root_, error);
  if (error || !std::filesystem::is_directory(root_status) ||
      std::filesystem::is_symlink(root_status)) {
    *reason_out = "capture root is missing, not a directory, or is a symlink";
    return false;
  }
  // Windows uses the checked absolute root path for the fixed child names.
  root_descriptor_ = 0;
  return true;
#else
  root_descriptor_ = OpenDirectoryWithoutSymlinks(root_, reason_out);
  return root_descriptor_ >= 0;
#endif
}

bool FrameTraceCaptureSlot::Arm(uint64_t token) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (token != status_.request_token ||
      status_.state != FrameTraceCaptureState::kQueued) {
    return false;
  }
  std::string reason;
  if (!OpenSafeRootLocked(&reason)) {
    SetTerminalLocked(FrameTraceCaptureState::kFailed, std::move(reason));
    return false;
  }
  status_.state = FrameTraceCaptureState::kArmed;
  status_.reason.clear();
  return true;
}

int FrameTraceCaptureSlot::CreatePartial(
    uint64_t token, std::filesystem::path* display_path_out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (token != status_.request_token ||
      status_.state != FrameTraceCaptureState::kArmed || root_descriptor_ < 0) {
    return -1;
  }

  int descriptor = -1;
#ifdef _WIN32
  const auto partial_path = root_ / kPartialName;
  descriptor = _wopen(partial_path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                      _S_IREAD | _S_IWRITE);
#else
  if (!DirectoryPathStillMatchesDescriptor(root_, root_descriptor_)) {
    SetTerminalLocked(FrameTraceCaptureState::kFailed,
                      "capture root changed after the request was armed");
    return -1;
  }

  struct stat final_status {};
  const int final_status_result =
      ::fstatat(root_descriptor_, kFinalName, &final_status,
                AT_SYMLINK_NOFOLLOW);
  if (final_status_result == 0) {
    if (!S_ISREG(final_status.st_mode)) {
      SetTerminalLocked(FrameTraceCaptureState::kFailed,
                        "latest.xtr exists but is not a regular file");
      return -1;
    }
  } else if (errno != ENOENT) {
    SetTerminalLocked(FrameTraceCaptureState::kFailed,
                      ErrnoReason("could not inspect existing latest.xtr"));
    return -1;
  }
  descriptor = ::openat(root_descriptor_, kPartialName,
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                        S_IRUSR | S_IWUSR);
#endif
  if (descriptor < 0) {
    SetTerminalLocked(FrameTraceCaptureState::kFailed,
                      ErrnoReason("could not create exclusive trace partial"));
    return -1;
  }

#ifndef _WIN32
  struct stat opened_status {};
  if (::fstat(descriptor, &opened_status) != 0 || !S_ISREG(opened_status.st_mode) ||
      (opened_status.st_mode & 0777) != 0600) {
    ::close(descriptor);
    ::unlinkat(root_descriptor_, kPartialName, 0);
    SetTerminalLocked(FrameTraceCaptureState::kFailed,
                      "trace partial did not open as a private regular file");
    return -1;
  }
#endif

  partial_created_ = true;
  if (display_path_out) {
    *display_path_out = BoundedPath(root_ / kPartialName);
  }
  return descriptor;
}

bool FrameTraceCaptureSlot::MarkCapturing(uint64_t token) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (token != status_.request_token ||
      status_.state != FrameTraceCaptureState::kArmed || !partial_created_) {
    return false;
  }
  status_.state = FrameTraceCaptureState::kCapturing;
  return true;
}

bool FrameTraceCaptureSlot::Publish(uint64_t token) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (token != status_.request_token ||
      status_.state != FrameTraceCaptureState::kCapturing || !partial_created_ ||
      root_descriptor_ < 0) {
    return false;
  }

  const auto partial_path = root_ / kPartialName;
  uint64_t byte_count = 0;
  std::string validation_path;
#ifdef _WIN32
  std::error_code size_error;
  byte_count = std::filesystem::file_size(partial_path, size_error);
  if (size_error) {
    SetTerminalLocked(FrameTraceCaptureState::kFailed,
                      "could not determine final capture size");
    return false;
  }
  validation_path = partial_path.string();
#else
  const int validation_descriptor =
      ::openat(root_descriptor_, kPartialName, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
  if (validation_descriptor < 0) {
    SetTerminalLocked(FrameTraceCaptureState::kFailed,
                      ErrnoReason("could not reopen trace partial for validation"));
    return false;
  }
  struct stat partial_status {};
  if (::fstat(validation_descriptor, &partial_status) != 0 ||
      !S_ISREG(partial_status.st_mode) || partial_status.st_size < 0) {
    ::close(validation_descriptor);
    SetTerminalLocked(FrameTraceCaptureState::kFailed,
                      "trace partial validation target is not a regular file");
    return false;
  }
  byte_count = uint64_t(partial_status.st_size);
#if defined(__APPLE__)
  validation_path = "/dev/fd/" + std::to_string(validation_descriptor);
#else
  validation_path = "/proc/self/fd/" + std::to_string(validation_descriptor);
#endif
#endif

  TraceReader reader;
  if (!reader.Open(validation_path)) {
#ifndef _WIN32
    ::close(validation_descriptor);
#endif
    SetTerminalLocked(FrameTraceCaptureState::kFailed,
                      "closed trace partial failed structural validation");
    return false;
  }
  if (reader.frame_count() != 1) {
    reader.Close();
#ifndef _WIN32
    ::close(validation_descriptor);
#endif
    SetTerminalLocked(FrameTraceCaptureState::kFailed,
                      "capture must contain exactly one completed swap");
    return false;
  }
  if (!reader.edram_requirements_finalized()) {
    reader.Close();
#ifndef _WIN32
    ::close(validation_descriptor);
#endif
    SetTerminalLocked(FrameTraceCaptureState::kFailed,
                      "capture EDRAM requirements manifest was not finalized");
    return false;
  }
  if (byte_count > byte_budget_) {
    reader.Close();
#ifndef _WIN32
    ::close(validation_descriptor);
#endif
    SetTerminalLocked(FrameTraceCaptureState::kFailed,
                      "final capture exceeded its byte budget");
    return false;
  }

  const bool deterministic = reader.has_initial_edram_snapshot() &&
                             reader.has_finalized_edram_requirements();
  std::string best_effort_reason;
  if (!reader.has_initial_edram_snapshot()) {
    best_effort_reason = "capture has no initial EDRAM snapshot; replay is best-effort only";
  } else if (!reader.has_finalized_edram_requirements()) {
    best_effort_reason =
        "capture has no tracked EDRAM requirements; replay is best-effort only";
  }
  reader.Close();

  bool synced = !fail_sync_for_testing_;
#ifdef _WIN32
  int sync_descriptor = -1;
  if (synced) {
    sync_descriptor =
        _wopen(partial_path.c_str(), _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
    synced = sync_descriptor >= 0 && _commit(sync_descriptor) == 0;
  }
  if (sync_descriptor >= 0) {
    _close(sync_descriptor);
  }
#else
  if (synced) {
    synced = ::fsync(validation_descriptor) == 0;
  }
#endif
  if (!synced) {
#ifndef _WIN32
    ::close(validation_descriptor);
#endif
    SetTerminalLocked(
        FrameTraceCaptureState::kFailed,
        fail_sync_for_testing_ ? "durable trace flush failed (injected)"
                               : ErrnoReason("durable trace flush failed"));
    return false;
  }

#ifndef _WIN32
  ::close(validation_descriptor);

  // The partial itself is always accessed through the held directory, but the
  // path reported to callers must still name that same directory. Revalidate
  // immediately before publication so a rename/replacement while the trace
  // was being captured cannot produce a successful status with a misleading
  // final path.
  if (!DirectoryPathStillMatchesDescriptor(root_, root_descriptor_)) {
    SetTerminalLocked(FrameTraceCaptureState::kFailed,
                      "capture root changed before publication");
    return false;
  }
#endif

  bool renamed = false;
  if (!fail_rename_for_testing_) {
#ifdef _WIN32
    std::error_code rename_error;
    std::filesystem::rename(partial_path, root_ / kFinalName, rename_error);
    renamed = !rename_error;
#else
    renamed = ::renameat(root_descriptor_, kPartialName, root_descriptor_, kFinalName) == 0;
#endif
  }
  if (!renamed) {
    SetTerminalLocked(FrameTraceCaptureState::kFailed,
                      fail_rename_for_testing_
                          ? "atomic trace publication failed (injected)"
                          : ErrnoReason("atomic trace publication failed"));
    return false;
  }

#ifndef _WIN32
  if (::fsync(root_descriptor_) != 0) {
    SetTerminalLocked(FrameTraceCaptureState::kFailed,
                      ErrnoReason("durable trace publication failed"));
    return false;
  }
#endif

  partial_created_ = false;
  SetTerminalLocked(deterministic ? FrameTraceCaptureState::kComplete
                                  : FrameTraceCaptureState::kCompleteBestEffortOnly,
                    std::move(best_effort_reason), root_ / kFinalName, byte_count);
  return true;
}

void FrameTraceCaptureSlot::SetTerminalLocked(
    FrameTraceCaptureState state, std::string reason,
    const std::filesystem::path& final_path, uint64_t byte_count) {
  if (state == FrameTraceCaptureState::kFailed ||
      state == FrameTraceCaptureState::kCancelled) {
    CleanupPartialLocked();
  }
  status_.state = state;
  status_.reason = BoundedReason(std::move(reason));
  status_.final_path = BoundedPath(final_path);
  status_.byte_count = byte_count;
  CloseRootLocked();
}

void FrameTraceCaptureSlot::Fail(uint64_t token, std::string reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (token != status_.request_token || !IsActiveState(status_.state)) {
    return;
  }
  SetTerminalLocked(FrameTraceCaptureState::kFailed, std::move(reason));
}

void FrameTraceCaptureSlot::Cancel(uint64_t token, std::string reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (token != status_.request_token || !IsActiveState(status_.state)) {
    return;
  }
  SetTerminalLocked(FrameTraceCaptureState::kCancelled, std::move(reason));
}

bool FrameTraceCaptureSlot::DeletePublishedCapture(
    const std::filesystem::path& safe_root, std::string* reason_out) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto fail = [&](std::string reason) {
    if (reason_out) {
      *reason_out = BoundedReason(std::move(reason));
    }
    return false;
  };
  auto reset_deleted_status = [&] {
    status_ = {};
    root_.clear();
    byte_budget_ = kDefaultByteBudget;
  };
  if (reason_out) {
    reason_out->clear();
  }
  if (IsActiveState(status_.state)) {
    return fail("cannot delete a capture while a request is active");
  }
  if (!safe_root.is_absolute()) {
    return fail("capture root must be an absolute path");
  }

#ifdef _WIN32
  std::error_code error;
  const auto root_status = std::filesystem::symlink_status(safe_root, error);
  if (error || !std::filesystem::is_directory(root_status) ||
      std::filesystem::is_symlink(root_status)) {
    return fail("capture root is missing, not a directory, or is a symlink");
  }
  const auto final_path = safe_root / kFinalName;
  const auto final_status = std::filesystem::symlink_status(final_path, error);
  if (error == std::errc::no_such_file_or_directory) {
    reset_deleted_status();
    return true;
  }
  if (error || !std::filesystem::is_regular_file(final_status) ||
      std::filesystem::is_symlink(final_status)) {
    return fail("latest.xtr is not a regular file");
  }
  if (!std::filesystem::remove(final_path, error) || error) {
    return fail("could not delete latest.xtr");
  }
#else
  std::error_code canonical_error;
  const auto supplied_status =
      std::filesystem::symlink_status(safe_root, canonical_error);
  if (canonical_error || !std::filesystem::is_directory(supplied_status) ||
      std::filesystem::is_symlink(supplied_status)) {
    return fail("capture root is missing, not a directory, or is a symlink");
  }
  const auto canonical_root =
      std::filesystem::canonical(safe_root, canonical_error);
  if (canonical_error) {
    return fail("capture root could not be resolved safely");
  }
  std::string open_reason;
  const int root_descriptor =
      OpenDirectoryWithoutSymlinks(canonical_root, &open_reason);
  if (root_descriptor < 0) {
    return fail(std::move(open_reason));
  }

  struct stat final_status {};
  const int inspect_result =
      ::fstatat(root_descriptor, kFinalName, &final_status,
                AT_SYMLINK_NOFOLLOW);
  if (inspect_result != 0) {
    const int inspect_error = errno;
    ::close(root_descriptor);
    if (inspect_error == ENOENT) {
      reset_deleted_status();
      return true;
    }
    errno = inspect_error;
    return fail(ErrnoReason("could not inspect latest.xtr"));
  }
  if (!S_ISREG(final_status.st_mode)) {
    ::close(root_descriptor);
    return fail("latest.xtr is not a regular file");
  }
  if (::unlinkat(root_descriptor, kFinalName, 0) != 0) {
    const int unlink_error = errno;
    ::close(root_descriptor);
    errno = unlink_error;
    return fail(ErrnoReason("could not delete latest.xtr"));
  }
  ::close(root_descriptor);
#endif

  reset_deleted_status();
  return true;
}

void FrameTraceCaptureSlot::CleanupPartialLocked() {
  if (!partial_created_) {
    return;
  }
#ifdef _WIN32
  std::error_code error;
  std::filesystem::remove(root_ / kPartialName, error);
#else
  if (root_descriptor_ >= 0) {
    ::unlinkat(root_descriptor_, kPartialName, 0);
  }
#endif
  partial_created_ = false;
}

void FrameTraceCaptureSlot::CloseRootLocked() {
#ifdef _WIN32
  root_descriptor_ = -1;
#else
  if (root_descriptor_ >= 0) {
    ::close(root_descriptor_);
    root_descriptor_ = -1;
  }
#endif
}

FrameTraceCaptureStatus FrameTraceCaptureSlot::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

uint64_t FrameTraceCaptureSlot::byte_budget(uint64_t token) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return token == status_.request_token ? byte_budget_ : 0;
}

bool FrameTraceCaptureSlot::owns_active_request(uint64_t token) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return token == status_.request_token && IsActiveState(status_.state);
}

void FrameTraceCaptureSlot::SetRenameFailureForTesting(bool fail) {
  std::lock_guard<std::mutex> lock(mutex_);
  fail_rename_for_testing_ = fail;
}

void FrameTraceCaptureSlot::SetSyncFailureForTesting(bool fail) {
  std::lock_guard<std::mutex> lock(mutex_);
  fail_sync_for_testing_ = fail;
}

const char* FrameTraceCaptureStateName(FrameTraceCaptureState state) {
  switch (state) {
    case FrameTraceCaptureState::kIdle:
      return "Idle";
    case FrameTraceCaptureState::kQueued:
      return "Queued";
    case FrameTraceCaptureState::kArmed:
      return "Armed";
    case FrameTraceCaptureState::kCapturing:
      return "Capturing";
    case FrameTraceCaptureState::kComplete:
      return "Complete";
    case FrameTraceCaptureState::kCompleteBestEffortOnly:
      return "CompleteBestEffortOnly";
    case FrameTraceCaptureState::kFailed:
      return "Failed";
    case FrameTraceCaptureState::kCancelled:
      return "Cancelled";
  }
  return "Unknown";
}

}  // namespace rex::graphics
