#include "ge_gpu_capture.h"

#include <rex/graphics/trace_writer.h>
#include <rex/graphics/xenos.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

class TemporaryTree {
 public:
  TemporaryTree() {
    const auto unique =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    path = std::filesystem::temp_directory_path() /
           ("goldeneye-gpu-capture-test-" + unique);
    std::filesystem::create_directories(path);
    chmod(path.c_str(), 0700);
  }
  ~TemporaryTree() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

bool Check(bool condition, const char* expression, int line) {
  if (!condition) {
    std::cerr << "FAIL line " << line << ": " << expression << '\n';
  }
  return condition;
}

#define CHECK(value)                                                        \
  do {                                                                      \
    if (!Check(bool(value), #value, __LINE__)) return false;                \
  } while (false)

bool WriteBestEffortTrace(const std::filesystem::path& path) {
  rex::graphics::TraceWriter writer(nullptr);
  if (!writer.Open(path, 0x584108A9) || !writer.WritePrimaryBufferEnd() ||
      !writer.WriteEvent(rex::graphics::EventCommand::Type::kSwap) ||
      !writer.Close()) {
    return false;
  }
  return chmod(path.c_str(), 0600) == 0;
}

bool WriteCompleteTrace(const std::filesystem::path& path) {
  rex::graphics::TraceWriter writer(nullptr);
  std::vector<uint8_t> edram(rex::graphics::xenos::kEdramSizeBytes);
  if (!writer.Open(path, 0x584108A9) ||
      !writer.BeginEdramRequirementsTracking() ||
      !writer.WriteEdramSnapshot(edram.data()) ||
      !writer.WritePrimaryBufferEnd() ||
      !writer.WriteEvent(rex::graphics::EventCommand::Type::kSwap) ||
      !writer.Close()) {
    return false;
  }
  return chmod(path.c_str(), 0600) == 0;
}

std::vector<char> ReadBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::vector<char>(std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>());
}

bool TestDeferredRequestAndStatus() {
  static_assert(!ge::gpu_capture::kIncludeInDiagnosticsByDefault);
  CHECK(ge::gpu_capture::BoundedDiagnosticCaptureAllowance(900, 1000, 200) ==
        100);
  CHECK(ge::gpu_capture::BoundedDiagnosticCaptureAllowance(999, 1000, 200) ==
        1);
  CHECK(ge::gpu_capture::BoundedDiagnosticCaptureAllowance(1000, 1000, 200) ==
        0);
  CHECK(ge::gpu_capture::BoundedDiagnosticCaptureAllowance(1001, 1000, 200) ==
        0);
  ge::gpu_capture::DeferredMenuRequest request;
  CHECK(!request.pending());
  request.Queue();
  CHECK(request.pending());
  CHECK(!request.TakeAfterClose(true));
  CHECK(!request.pending());
  request.Queue();
  CHECK(request.TakeAfterClose(false));
  CHECK(!request.TakeAfterClose(false));

  using State = rex::graphics::FrameTraceCaptureState;
  const struct {
    State state;
    const char* label;
    bool active;
    bool can_delete;
  } cases[] = {
      {State::kIdle, "No GPU capture", false, false},
      {State::kQueued, "Queued", true, false},
      {State::kArmed, "Armed", true, false},
      {State::kCapturing, "Capturing", true, false},
      {State::kComplete, "Complete", false, true},
      {State::kCompleteBestEffortOnly, "Best Effort", false, true},
      {State::kFailed, "Failed", false, false},
      {State::kCancelled, "Cancelled", false, false},
  };
  for (const auto& test : cases) {
    rex::graphics::FrameTraceCaptureStatus status;
    status.state = test.state;
    status.reason = "bounded reason";
    const auto presentation = ge::gpu_capture::PresentStatus(status);
    CHECK(presentation.label == test.label);
    CHECK(presentation.active == test.active);
    CHECK(presentation.can_delete == test.can_delete);
    CHECK(presentation.can_request == !test.active);
    CHECK(!presentation.detail.empty());
  }
  return true;
}

bool TestDamGameplayCaptureGate() {
  using ge::gpu_capture::DamGameplayCaptureGate;
  using ge::gpu_capture::DamGameplayObservation;

  auto eligible = [] {
    DamGameplayObservation observation;
    observation.level_id = DamGameplayCaptureGate::kDamLevelId;
    observation.player_count = 1;
    observation.player_valid = true;
    observation.position_valid = true;
    observation.player = 0x1000;
    observation.coordinates = 0x2000;
    observation.guest_frame = 100;
    observation.present = 200;
    return observation;
  };

  DamGameplayCaptureGate gate;
  auto sample = eligible();
  CHECK(!gate.Observe(sample));
  CHECK(!gate.ready());

  // Advancing only one counter is not sufficient proof of live gameplay.
  sample.guest_frame++;
  CHECK(!gate.Observe(sample));
  sample.guest_frame++;
  sample.present++;
  CHECK(gate.Observe(sample));
  CHECK(gate.ready());
  sample.guest_frame++;
  sample.present++;
  CHECK(!gate.Observe(sample));

  // Every menu/mission/player transition resets the two-sample proof window.
  DamGameplayCaptureGate transitions;
  sample = eligible();
  CHECK(!transitions.Observe(sample));
  sample.level_id = 90;
  sample.guest_frame++;
  sample.present++;
  CHECK(!transitions.Observe(sample));
  sample = eligible();
  sample.guest_frame += 10;
  sample.present += 10;
  CHECK(!transitions.Observe(sample));
  sample.player = 0x3000;
  sample.guest_frame++;
  sample.present++;
  CHECK(!transitions.Observe(sample));
  sample.guest_frame++;
  sample.present++;
  CHECK(transitions.Observe(sample));

  const auto ineligible_cases = [] {
    std::vector<DamGameplayObservation> cases;
    DamGameplayObservation base;
    base.level_id = DamGameplayCaptureGate::kDamLevelId;
    base.player_count = 1;
    base.player_valid = true;
    base.position_valid = true;
    base.player = 1;
    base.coordinates = 2;
    base.guest_frame = 1;
    base.present = 1;
    auto add = [&](auto mutate) {
      auto value = base;
      mutate(value);
      cases.push_back(value);
    };
    add([](auto& value) { value.level_id = 34; });
    add([](auto& value) { value.player_count = 2; });
    add([](auto& value) { value.network_session = true; });
    add([](auto& value) { value.player_valid = false; });
    add([](auto& value) { value.position_valid = false; });
    add([](auto& value) { value.player = 0; });
    add([](auto& value) { value.coordinates = 0; });
    add([](auto& value) { value.pause = 1; });
    add([](auto& value) { value.control_disabled = 1; });
    add([](auto& value) { value.watch = 1; });
    return cases;
  }();
  for (const auto& ineligible : ineligible_cases) {
    DamGameplayCaptureGate isolated;
    CHECK(!isolated.Observe(ineligible));
    CHECK(!isolated.ready());
  }

  // Unsigned wrap is accepted only for normal forward counter movement.
  DamGameplayCaptureGate wrapping;
  sample = eligible();
  sample.guest_frame = UINT32_MAX;
  sample.present = UINT32_MAX;
  CHECK(!wrapping.Observe(sample));
  sample.guest_frame = 0;
  sample.present = 0;
  CHECK(wrapping.Observe(sample));

  DamGameplayCaptureGate backwards;
  sample = eligible();
  sample.guest_frame = 100;
  sample.present = 100;
  CHECK(!backwards.Observe(sample));
  sample.guest_frame = 99;
  sample.present = 99;
  CHECK(!backwards.Observe(sample));
  return true;
}

bool TestPrivateRootAndPartialExclusion() {
  TemporaryTree tree;
  const auto user = tree.path / "user";
  CHECK(std::filesystem::create_directory(user));
  CHECK(chmod(user.c_str(), 0700) == 0);
  std::filesystem::path capture_root;
  std::string error;
  CHECK(ge::gpu_capture::EnsurePrivateRoot(user, &capture_root, &error));
  struct stat root_status {};
  CHECK(stat(capture_root.c_str(), &root_status) == 0);
  CHECK((root_status.st_mode & 0777) == 0700);
  CHECK(WriteBestEffortTrace(capture_root / "latest.xtr.partial"));
  const auto inspection = ge::gpu_capture::InspectCompletedCapture(user);
  CHECK(!inspection.available);
  CHECK(!std::filesystem::exists(capture_root / "latest.xtr"));

  TemporaryTree symlink_tree;
  const auto symlink_user = symlink_tree.path / "user";
  const auto outside = symlink_tree.path / "outside";
  CHECK(std::filesystem::create_directory(symlink_user));
  CHECK(std::filesystem::create_directory(outside));
  std::filesystem::create_directory_symlink(outside,
                                            symlink_user / "GPU Captures");
  CHECK(!ge::gpu_capture::EnsurePrivateRoot(symlink_user, nullptr, &error));
  return true;
}

bool TestInspectionRejectsUnsafeFiles() {
  TemporaryTree tree;
  const auto user = tree.path / "user";
  CHECK(std::filesystem::create_directory(user));
  CHECK(chmod(user.c_str(), 0700) == 0);
  std::filesystem::path root;
  std::string error;
  CHECK(ge::gpu_capture::EnsurePrivateRoot(user, &root, &error));
  const auto final = root / "latest.xtr";

  CHECK(WriteCompleteTrace(final));
  auto valid = ge::gpu_capture::InspectCompletedCapture(user);
  CHECK(valid.available);
  CHECK(valid.state == rex::graphics::FrameTraceCaptureState::kComplete);
  CHECK(valid.byte_count == std::filesystem::file_size(final));
  CHECK(!valid.build_revision.empty());

  auto oversize = ge::gpu_capture::InspectCompletedCapture(
      user, valid.byte_count - 1);
  CHECK(!oversize.available);
  CHECK(oversize.reason.find("size limit") != std::string::npos);

  CHECK(chmod(final.c_str(), 0644) == 0);
  auto broad = ge::gpu_capture::InspectCompletedCapture(user);
  CHECK(!broad.available);
  CHECK(broad.reason.find("private") != std::string::npos);
  CHECK(chmod(final.c_str(), 0600) == 0);

  std::filesystem::remove(final);
  CHECK(WriteBestEffortTrace(final));
  auto best_effort = ge::gpu_capture::InspectCompletedCapture(user);
  CHECK(best_effort.available);
  CHECK(best_effort.state ==
        rex::graphics::FrameTraceCaptureState::kCompleteBestEffortOnly);

  std::filesystem::remove(final);
  {
    std::ofstream bad(final, std::ios::binary);
    bad << "not a trace";
  }
  CHECK(chmod(final.c_str(), 0600) == 0);
  CHECK(!ge::gpu_capture::InspectCompletedCapture(user).available);

  std::filesystem::remove(final);
  const auto outside = tree.path / "outside.xtr";
  CHECK(WriteBestEffortTrace(outside));
  std::filesystem::create_symlink(outside, final);
  CHECK(!ge::gpu_capture::InspectCompletedCapture(user).available);
  return true;
}

bool TestOptInCopyAndCleanup() {
  TemporaryTree tree;
  const auto user = tree.path / "user";
  const auto bundle = tree.path / "bundle";
  CHECK(std::filesystem::create_directory(user));
  CHECK(std::filesystem::create_directory(bundle));
  CHECK(chmod(user.c_str(), 0700) == 0);
  CHECK(chmod(bundle.c_str(), 0700) == 0);
  std::filesystem::path root;
  std::string error;
  CHECK(ge::gpu_capture::EnsurePrivateRoot(user, &root, &error));
  const auto source = root / "latest.xtr";
  CHECK(WriteBestEffortTrace(source));
  CHECK(WriteBestEffortTrace(root / "latest.xtr.partial"));

  // Default export flow does not invoke the opt-in helper.
  CHECK(!std::filesystem::exists(bundle / "GPU Capture"));

  ge::gpu_capture::CompletedCaptureInfo copied;
  CHECK(ge::gpu_capture::CopyCompletedCaptureForDiagnostics(
      user, bundle, &copied, &error));
  const auto copied_path = bundle / "GPU Capture/latest.xtr";
  const auto metadata_path = bundle / "GPU Capture/GPU Capture.txt";
  CHECK(ReadBytes(source) == ReadBytes(copied_path));
  CHECK(!std::filesystem::exists(bundle / "GPU Capture/latest.xtr.partial"));
  const auto metadata_bytes = ReadBytes(metadata_path);
  const std::string stable_metadata(metadata_bytes.begin(), metadata_bytes.end());
  CHECK(stable_metadata.find("State: CompleteBestEffortOnly") != std::string::npos);
  CHECK(stable_metadata.find("Reason:") != std::string::npos);
  CHECK(stable_metadata.find("Bytes: " + std::to_string(copied.byte_count)) !=
        std::string::npos);
  CHECK(stable_metadata.find("Build revision: " + copied.build_revision) !=
        std::string::npos);
  CHECK(stable_metadata.find(user.string()) == std::string::npos);
  struct stat copied_status {};
  CHECK(stat(copied_path.c_str(), &copied_status) == 0);
  CHECK((copied_status.st_mode & 0777) == 0600);

  TemporaryTree failed_tree;
  const auto failed_bundle = failed_tree.path / "bundle";
  CHECK(std::filesystem::create_directory(failed_bundle));
  CHECK(chmod(failed_bundle.c_str(), 0700) == 0);
  CHECK(!ge::gpu_capture::CopyCompletedCaptureForDiagnostics(
      user, failed_bundle, nullptr, &error, copied.byte_count - 1));
  CHECK(!std::filesystem::exists(failed_bundle / "GPU Capture"));
  CHECK(std::filesystem::exists(source));
  return true;
}

}  // namespace

int main() {
  if (!TestDeferredRequestAndStatus() || !TestDamGameplayCaptureGate() ||
      !TestPrivateRootAndPartialExclusion() ||
      !TestInspectionRejectsUnsafeFiles() || !TestOptInCopyAndCleanup()) {
    return 1;
  }
  std::cout << "GoldenEye GPU capture tests passed\n";
  return 0;
}
