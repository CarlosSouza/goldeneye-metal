// GoldenEye Metal - private one-frame GPU capture UI and diagnostic helpers.

#include "ge_gpu_capture.h"

#include <rex/graphics/trace_reader.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <sstream>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#error "ge_gpu_capture.cpp is currently a macOS-only source"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ge::gpu_capture {

namespace {

constexpr char kDiagnosticDirectoryName[] = "GPU Capture";
constexpr char kMetadataName[] = "GPU Capture.txt";

std::string ErrnoMessage(const char* action) {
  return std::string(action) + ": " + std::strerror(errno);
}

int OpenDirectoryWithoutSymlinks(const std::filesystem::path& path,
                                 std::string* error) {
  if (!path.is_absolute()) {
    *error = "The path must be absolute.";
    return -1;
  }
  int descriptor = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0) {
    *error = ErrnoMessage("Could not open the filesystem root");
    return -1;
  }
  for (const auto& component_path : path.lexically_normal().relative_path()) {
    const std::string component = component_path.string();
    if (component.empty() || component == ".") {
      continue;
    }
    if (component == "..") {
      ::close(descriptor);
      *error = "The path contains parent traversal.";
      return -1;
    }
    const int next = ::openat(descriptor, component.c_str(),
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (next < 0) {
      ::close(descriptor);
      *error = "The path is missing, not a directory, or contains a symbolic link.";
      return -1;
    }
    ::close(descriptor);
    descriptor = next;
  }
  return descriptor;
}

int OpenCanonicalDirectory(const std::filesystem::path& supplied,
                           std::filesystem::path* canonical_out,
                           std::string* error) {
  if (!supplied.is_absolute()) {
    *error = "The path must be absolute.";
    return -1;
  }
  std::error_code status_error;
  const auto supplied_status =
      std::filesystem::symlink_status(supplied, status_error);
  if (status_error || !std::filesystem::is_directory(supplied_status) ||
      std::filesystem::is_symlink(supplied_status)) {
    *error = "The path is missing, not a directory, or is a symbolic link.";
    return -1;
  }
  const auto canonical = std::filesystem::canonical(supplied, status_error);
  if (status_error) {
    *error = "The path could not be resolved safely.";
    return -1;
  }
  int descriptor = OpenDirectoryWithoutSymlinks(canonical, error);
  if (descriptor >= 0 && canonical_out) {
    *canonical_out = canonical;
  }
  return descriptor;
}

int OpenCaptureRoot(const std::filesystem::path& user_data_root,
                    std::filesystem::path* canonical_capture_root,
                    std::string* error) {
  std::filesystem::path canonical_user_root;
  const int user_descriptor =
      OpenCanonicalDirectory(user_data_root, &canonical_user_root, error);
  if (user_descriptor < 0) {
    return -1;
  }
  const int capture_descriptor =
      ::openat(user_descriptor, kCaptureDirectoryName,
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  const int open_error = errno;
  ::close(user_descriptor);
  if (capture_descriptor < 0) {
    errno = open_error;
    *error = "No private completed GPU capture is available.";
    return -1;
  }
  struct stat root_status {};
  if (::fstat(capture_descriptor, &root_status) != 0 ||
      !S_ISDIR(root_status.st_mode) || (root_status.st_mode & 0077) != 0) {
    ::close(capture_descriptor);
    *error = "The GPU capture directory is not private.";
    return -1;
  }
  if (canonical_capture_root) {
    *canonical_capture_root = canonical_user_root / kCaptureDirectoryName;
  }
  return capture_descriptor;
}

std::string TraceDescriptorPath(int descriptor) {
#if defined(__APPLE__)
  return "/dev/fd/" + std::to_string(descriptor);
#else
  return "/proc/self/fd/" + std::to_string(descriptor);
#endif
}

std::string BuildRevision(const rex::graphics::TraceReader& reader) {
  const char* revision = reader.header()->build_commit_sha;
  size_t length = 0;
  while (length < sizeof(reader.header()->build_commit_sha) && revision[length]) {
    ++length;
  }
  std::string result(revision, length);
  for (char& value : result) {
    const unsigned char byte = static_cast<unsigned char>(value);
    if (!(std::isalnum(byte) || value == '.' || value == '_' || value == '-')) {
      value = '?';
    }
  }
  return result.empty() ? "unknown" : result;
}

CompletedCaptureInfo ValidateDescriptor(int descriptor, uint64_t maximum_bytes) {
  CompletedCaptureInfo info;
  struct stat status {};
  if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
    info.reason = "latest.xtr is not a regular file.";
    return info;
  }
  const mode_t permissions = status.st_mode & 0777;
  if ((permissions & ~mode_t(S_IRUSR | S_IWUSR)) != 0 ||
      (permissions & S_IRUSR) == 0) {
    info.reason = "latest.xtr does not have private read permissions.";
    return info;
  }
  if (status.st_size <= 0) {
    info.reason = "latest.xtr is empty.";
    return info;
  }
  if (!maximum_bytes || static_cast<uint64_t>(status.st_size) > maximum_bytes) {
    info.reason = "latest.xtr exceeds the diagnostic capture size limit.";
    return info;
  }

  rex::graphics::TraceReader reader;
  if (!reader.Open(TraceDescriptorPath(descriptor))) {
    info.reason = "latest.xtr failed trace validation.";
    return info;
  }
  if (reader.frame_count() != 1) {
    reader.Close();
    info.reason = "latest.xtr is not exactly one completed frame.";
    return info;
  }
  if (!reader.edram_requirements_finalized()) {
    reader.Close();
    info.reason = "latest.xtr has no finalized replay requirements.";
    return info;
  }

  const bool deterministic = reader.has_initial_edram_snapshot() &&
                             reader.has_finalized_edram_requirements();
  info.available = true;
  info.state = deterministic
                   ? rex::graphics::FrameTraceCaptureState::kComplete
                   : rex::graphics::FrameTraceCaptureState::kCompleteBestEffortOnly;
  if (!reader.has_initial_edram_snapshot()) {
    info.reason = "Initial EDRAM state is missing; replay is best effort only.";
  } else if (!reader.has_finalized_edram_requirements()) {
    info.reason = "Tracked EDRAM requirements are missing; replay is best effort only.";
  } else {
    info.reason = "Validated one-frame capture with complete replay requirements.";
  }
  info.byte_count = static_cast<uint64_t>(status.st_size);
  info.build_revision = BuildRevision(reader);
  reader.Close();
  return info;
}

bool WriteAll(int descriptor, std::string_view contents) {
  while (!contents.empty()) {
    const ssize_t written =
        ::write(descriptor, contents.data(), contents.size());
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return false;
    }
    contents.remove_prefix(static_cast<size_t>(written));
  }
  return true;
}

bool CopyExact(int source, int destination, uint64_t byte_count) {
  if (::lseek(source, 0, SEEK_SET) < 0) {
    return false;
  }
  std::array<char, 64 * 1024> buffer{};
  uint64_t remaining = byte_count;
  while (remaining) {
    const size_t requested =
        static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
    ssize_t read_count = ::read(source, buffer.data(), requested);
    if (read_count < 0 && errno == EINTR) {
      continue;
    }
    if (read_count <= 0) {
      return false;
    }
    size_t offset = 0;
    while (offset < static_cast<size_t>(read_count)) {
      ssize_t write_count =
          ::write(destination, buffer.data() + offset,
                  static_cast<size_t>(read_count) - offset);
      if (write_count < 0 && errno == EINTR) {
        continue;
      }
      if (write_count <= 0) {
        return false;
      }
      offset += static_cast<size_t>(write_count);
    }
    remaining -= static_cast<uint64_t>(read_count);
  }
  char extra = 0;
  ssize_t extra_count;
  do {
    extra_count = ::read(source, &extra, 1);
  } while (extra_count < 0 && errno == EINTR);
  return extra_count == 0;
}

void RemoveDiagnosticCaptureEntries(int bundle_descriptor) {
  const int directory_descriptor =
      ::openat(bundle_descriptor, kDiagnosticDirectoryName,
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (directory_descriptor >= 0) {
    ::unlinkat(directory_descriptor, kCompletedCaptureName, 0);
    ::unlinkat(directory_descriptor, kMetadataName, 0);
    ::close(directory_descriptor);
  }
  ::unlinkat(bundle_descriptor, kDiagnosticDirectoryName, AT_REMOVEDIR);
}

}  // namespace

bool DamGameplayCaptureGate::Observe(
    const DamGameplayObservation& observation) noexcept {
  if (ready_) {
    return false;
  }
  const bool eligible =
      observation.level_id == kDamLevelId && observation.player_count == 1 &&
      !observation.network_session && observation.player_valid &&
      observation.position_valid && observation.player != 0 &&
      observation.coordinates != 0 && observation.pause == 0 &&
      observation.control_disabled == 0 && observation.watch == 0;
  if (!eligible) {
    has_previous_ = false;
    previous_ = {};
    return false;
  }

  const auto counter_advanced = [](uint32_t previous, uint32_t current) {
    const uint32_t delta = current - previous;
    return delta != 0 && delta < 0x80000000u;
  };
  const bool confirmed =
      has_previous_ && previous_.level_id == observation.level_id &&
      previous_.player_count == observation.player_count &&
      previous_.network_session == observation.network_session &&
      previous_.player == observation.player &&
      previous_.coordinates == observation.coordinates &&
      counter_advanced(previous_.guest_frame, observation.guest_frame) &&
      counter_advanced(previous_.present, observation.present);
  previous_ = observation;
  has_previous_ = true;
  if (!confirmed) {
    return false;
  }
  ready_ = true;
  return true;
}

uint64_t BoundedDiagnosticCaptureAllowance(uint64_t already_counted_bytes,
                                           uint64_t bundle_byte_limit,
                                           uint64_t capture_byte_limit) {
  if (already_counted_bytes >= bundle_byte_limit) {
    return 0;
  }
  return std::min(capture_byte_limit,
                  bundle_byte_limit - already_counted_bytes);
}

StatusPresentation PresentStatus(
    const rex::graphics::FrameTraceCaptureStatus& status) {
  StatusPresentation presentation;
  switch (status.state) {
    case rex::graphics::FrameTraceCaptureState::kIdle:
      presentation.label = "No GPU capture";
      presentation.detail = "Capture one gameplay frame only when requested for debugging.";
      break;
    case rex::graphics::FrameTraceCaptureState::kQueued:
      presentation.label = "Queued";
      presentation.detail = "Waiting for the graphics worker.";
      presentation.active = true;
      presentation.can_request = false;
      break;
    case rex::graphics::FrameTraceCaptureState::kArmed:
      presentation.label = "Armed";
      presentation.detail = "The next gameplay frame will be captured.";
      presentation.active = true;
      presentation.can_request = false;
      break;
    case rex::graphics::FrameTraceCaptureState::kCapturing:
      presentation.label = "Capturing";
      presentation.detail = "Recording one gameplay frame.";
      presentation.active = true;
      presentation.can_request = false;
      break;
    case rex::graphics::FrameTraceCaptureState::kComplete:
      presentation.label = "Complete";
      presentation.detail = "Ready for optional diagnostic export.";
      presentation.can_delete = true;
      break;
    case rex::graphics::FrameTraceCaptureState::kCompleteBestEffortOnly:
      presentation.label = "Best Effort";
      presentation.detail = status.reason.empty()
                                ? "Captured, but replay fidelity is limited."
                                : status.reason;
      presentation.can_delete = true;
      break;
    case rex::graphics::FrameTraceCaptureState::kFailed:
      presentation.label = "Failed";
      presentation.detail = status.reason.empty() ? "The capture failed safely." : status.reason;
      break;
    case rex::graphics::FrameTraceCaptureState::kCancelled:
      presentation.label = "Cancelled";
      presentation.detail = status.reason.empty() ? "The capture was cancelled safely."
                                                   : status.reason;
      break;
  }
  return presentation;
}

bool DeferredMenuRequest::TakeAfterClose(bool quitting) {
  const bool take = pending_ && !quitting;
  pending_ = false;
  return take;
}

std::filesystem::path CaptureRoot(
    const std::filesystem::path& user_data_root) {
  return user_data_root / kCaptureDirectoryName;
}

bool EnsurePrivateRoot(const std::filesystem::path& user_data_root,
                       std::filesystem::path* capture_root,
                       std::string* error) {
  if (capture_root) {
    capture_root->clear();
  }
  if (error) {
    error->clear();
  }
  std::string local_error;
  std::filesystem::path canonical_user_root;
  const int user_descriptor =
      OpenCanonicalDirectory(user_data_root, &canonical_user_root, &local_error);
  if (user_descriptor < 0) {
    if (error) *error = std::move(local_error);
    return false;
  }
  if (::mkdirat(user_descriptor, kCaptureDirectoryName,
                S_IRWXU) != 0 && errno != EEXIST) {
    local_error = ErrnoMessage("Could not create the private GPU capture directory");
    ::close(user_descriptor);
    if (error) *error = std::move(local_error);
    return false;
  }
  const int capture_descriptor =
      ::openat(user_descriptor, kCaptureDirectoryName,
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  const int open_error = errno;
  ::close(user_descriptor);
  if (capture_descriptor < 0) {
    errno = open_error;
    if (error) *error = "GPU Captures exists but is not a safe directory.";
    return false;
  }
  struct stat status {};
  bool valid = ::fstat(capture_descriptor, &status) == 0 &&
               S_ISDIR(status.st_mode) &&
               ::fchmod(capture_descriptor, S_IRWXU) == 0;
  if (valid) {
    struct stat tightened_status {};
    valid = ::fstat(capture_descriptor, &tightened_status) == 0 &&
            (tightened_status.st_mode & 0777) == 0700;
  }
  ::close(capture_descriptor);
  if (!valid) {
    if (error) *error = "Could not make the GPU capture directory private.";
    return false;
  }
  if (capture_root) {
    *capture_root = canonical_user_root / kCaptureDirectoryName;
  }
  return true;
}

CompletedCaptureInfo InspectCompletedCapture(
    const std::filesystem::path& user_data_root, uint64_t maximum_bytes) {
  CompletedCaptureInfo info;
  std::string error;
  const int root_descriptor = OpenCaptureRoot(user_data_root, nullptr, &error);
  if (root_descriptor < 0) {
    info.reason = std::move(error);
    return info;
  }
  const int source_descriptor =
      ::openat(root_descriptor, kCompletedCaptureName,
               O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  const int open_error = errno;
  ::close(root_descriptor);
  if (source_descriptor < 0) {
    errno = open_error;
    info.reason = "No validated completed GPU capture is available.";
    return info;
  }
  info = ValidateDescriptor(source_descriptor, maximum_bytes);
  ::close(source_descriptor);
  return info;
}

std::string BuildMetadata(const CompletedCaptureInfo& info) {
  std::ostringstream text;
  text << "GoldenEye Metal GPU Capture\n\n"
       << "State: " << rex::graphics::FrameTraceCaptureStateName(info.state) << "\n"
       << "Reason: " << (info.reason.empty() ? "None" : info.reason) << "\n"
       << "Bytes: " << info.byte_count << "\n"
       << "Build revision: "
       << (info.build_revision.empty() ? "unknown" : info.build_revision) << "\n\n"
       << "Privacy: This trace may contain game memory. Share it only when requested.\n";
  return text.str();
}

bool CopyCompletedCaptureForDiagnostics(
    const std::filesystem::path& user_data_root,
    const std::filesystem::path& diagnostic_bundle_root,
    CompletedCaptureInfo* copied_info, std::string* error,
    uint64_t maximum_bytes) {
  if (copied_info) {
    *copied_info = {};
  }
  if (error) {
    error->clear();
  }
  std::string local_error;
  const int source_root =
      OpenCaptureRoot(user_data_root, nullptr, &local_error);
  if (source_root < 0) {
    if (error) *error = std::move(local_error);
    return false;
  }
  const int source = ::openat(source_root, kCompletedCaptureName,
                              O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  const int source_open_error = errno;
  ::close(source_root);
  if (source < 0) {
    errno = source_open_error;
    if (error) *error = "No validated completed GPU capture is available.";
    return false;
  }
  CompletedCaptureInfo info = ValidateDescriptor(source, maximum_bytes);
  if (!info.available) {
    ::close(source);
    if (error) *error = info.reason;
    return false;
  }

  std::filesystem::path ignored;
  const int bundle =
      OpenCanonicalDirectory(diagnostic_bundle_root, &ignored, &local_error);
  if (bundle < 0) {
    ::close(source);
    if (error) *error = std::move(local_error);
    return false;
  }
  struct stat bundle_status {};
  if (::fstat(bundle, &bundle_status) != 0 ||
      !S_ISDIR(bundle_status.st_mode) || (bundle_status.st_mode & 0077) != 0) {
    ::close(source);
    ::close(bundle);
    if (error) *error = "The diagnostic workspace is not private.";
    return false;
  }
  if (::mkdirat(bundle, kDiagnosticDirectoryName, S_IRWXU) != 0) {
    ::close(source);
    ::close(bundle);
    if (error) *error = "Could not create the private GPU capture export directory.";
    return false;
  }
  const int output_root =
      ::openat(bundle, kDiagnosticDirectoryName,
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (output_root < 0) {
    ::close(source);
    RemoveDiagnosticCaptureEntries(bundle);
    ::close(bundle);
    if (error) *error = "Could not open the private GPU capture export directory.";
    return false;
  }
  const int destination =
      ::openat(output_root, kCompletedCaptureName,
               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
               S_IRUSR | S_IWUSR);
  bool success = destination >= 0;
  if (success) {
    success = CopyExact(source, destination, info.byte_count) &&
              ::fsync(destination) == 0;
    if (::close(destination) != 0) {
      success = false;
    }
  }
  if (::close(source) != 0) {
    success = false;
  }

  if (success) {
    const int copied = ::openat(output_root, kCompletedCaptureName,
                                O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (copied < 0) {
      success = false;
    } else {
      const CompletedCaptureInfo copied_validation =
          ValidateDescriptor(copied, maximum_bytes);
      success = copied_validation.available &&
                copied_validation.state == info.state &&
                copied_validation.byte_count == info.byte_count &&
                copied_validation.build_revision == info.build_revision;
      if (::close(copied) != 0) {
        success = false;
      }
    }
  }

  if (success) {
    const int metadata =
        ::openat(output_root, kMetadataName,
                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                 S_IRUSR | S_IWUSR);
    if (metadata < 0) {
      success = false;
    } else {
      const std::string metadata_text = BuildMetadata(info);
      success = WriteAll(metadata, metadata_text) && ::fsync(metadata) == 0;
      if (::close(metadata) != 0) {
        success = false;
      }
    }
  }
  if (success && ::fsync(output_root) != 0) {
    success = false;
  }
  if (::close(output_root) != 0) {
    success = false;
  }
  if (!success) {
    RemoveDiagnosticCaptureEntries(bundle);
  }
  if (::close(bundle) != 0) {
    success = false;
    std::filesystem::path ignored_path;
    std::string ignored_error;
    const int cleanup_bundle = OpenCanonicalDirectory(
        diagnostic_bundle_root, &ignored_path, &ignored_error);
    if (cleanup_bundle >= 0) {
      RemoveDiagnosticCaptureEntries(cleanup_bundle);
      ::close(cleanup_bundle);
    }
  }
  if (!success) {
    if (error) *error = "The GPU capture could not be copied safely.";
    return false;
  }
  if (copied_info) {
    *copied_info = std::move(info);
  }
  return true;
}

}  // namespace ge::gpu_capture
