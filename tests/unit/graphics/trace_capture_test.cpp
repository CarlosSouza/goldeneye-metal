/**
 ******************************************************************************
 * ReXGlue - Xbox 360 recompilation runtime                                  *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                       *
 *                                                                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/trace_capture.h>
#include <rex/graphics/trace_reader.h>
#include <rex/graphics/trace_writer.h>
#include <rex/graphics/xenos.h>
#include <rex/version.h>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    static std::atomic<uint64_t> next_id{0};
    path_ = std::filesystem::temp_directory_path() /
            ("rex-private-trace-" +
             std::to_string(next_id.fetch_add(1, std::memory_order_relaxed)));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

bool WriteCapture(rex::graphics::FrameTraceCaptureSlot& slot,
                  const std::filesystem::path& root, bool deterministic,
                  uint32_t swap_count = 1, uint32_t title_id = 0x584108A9,
                  uint64_t byte_budget =
                      rex::graphics::FrameTraceCaptureSlot::kDefaultByteBudget) {
  const uint64_t token = slot.Queue(root, byte_budget);
  if (!token || !slot.Arm(token)) {
    return false;
  }
  std::filesystem::path partial_path;
  const int descriptor = slot.CreatePartial(token, &partial_path);
  if (descriptor < 0) {
    return false;
  }

  rex::graphics::TraceWriter writer(nullptr);
  if (!writer.SetByteBudget(byte_budget) ||
      !writer.OpenFileDescriptor(descriptor, partial_path, title_id) ||
      !slot.MarkCapturing(token)) {
    writer.Discard();
    slot.Fail(token, "test initialization failed");
    return false;
  }
  if (deterministic) {
    std::vector<uint8_t> edram(rex::graphics::xenos::kEdramSizeBytes);
    if (!writer.BeginEdramRequirementsTracking() ||
        !writer.WriteEdramSnapshot(edram.data())) {
      writer.Discard();
      slot.Fail(token, "test EDRAM initialization failed");
      return false;
    }
  }
  for (uint32_t i = 0; i < swap_count; ++i) {
    if (!writer.WritePrimaryBufferEnd() ||
        !writer.WriteEvent(rex::graphics::EventCommand::Type::kSwap)) {
      writer.Discard();
      slot.Fail(token, "test frame write failed");
      return false;
    }
  }
  if (!writer.Close()) {
    slot.Fail(token, std::string(writer.failure_reason()));
    return false;
  }
  return slot.Publish(token);
}

}  // namespace

TEST_CASE("Private frame trace slot exposes bounded lifecycle state",
          "[graphics][trace][trace_capture]") {
  TemporaryDirectory temporary;
  rex::graphics::FrameTraceCaptureSlot slot;

  const uint64_t token = slot.Queue(temporary.path());
  REQUIRE(token != 0);
  CHECK(slot.status().request_token == token);
  CHECK(slot.status().state == rex::graphics::FrameTraceCaptureState::kQueued);
  std::atomic<bool> observed_consistent_status{true};
  std::jthread status_observer([&](std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
      const auto snapshot = slot.status();
      if (snapshot.request_token != token ||
          snapshot.reason.size() >
              rex::graphics::FrameTraceCaptureSlot::kMaximumReasonLength ||
          snapshot.final_path.string().size() >
              rex::graphics::FrameTraceCaptureSlot::kMaximumPathLength) {
        observed_consistent_status.store(false, std::memory_order_relaxed);
        return;
      }
    }
  });
  CHECK(slot.Queue(temporary.path()) == 0);
  REQUIRE(slot.Arm(token));
  CHECK(slot.status().state == rex::graphics::FrameTraceCaptureState::kArmed);

  std::filesystem::path partial_path;
  const int descriptor = slot.CreatePartial(token, &partial_path);
  REQUIRE(descriptor >= 0);
#ifndef _WIN32
  struct stat status {};
  REQUIRE(::fstat(descriptor, &status) == 0);
  CHECK((status.st_mode & 0777) == 0600);
#endif
  rex::graphics::TraceWriter writer(nullptr);
  REQUIRE(writer.OpenFileDescriptor(descriptor, partial_path, 0));
  REQUIRE(slot.MarkCapturing(token));
  CHECK(slot.status().state == rex::graphics::FrameTraceCaptureState::kCapturing);
  writer.Discard();
  slot.Cancel(token, std::string(400, 'x'));
  CHECK(slot.status().state == rex::graphics::FrameTraceCaptureState::kCancelled);
  CHECK(slot.status().reason.size() ==
        rex::graphics::FrameTraceCaptureSlot::kMaximumReasonLength);
  CHECK_FALSE(std::filesystem::exists(temporary.path() / "latest.xtr.partial"));
  status_observer.request_stop();
  status_observer.join();
  CHECK(observed_consistent_status.load(std::memory_order_relaxed));
}

TEST_CASE("Private trace path confinement rejects symlinks and root replacement",
          "[graphics][trace][trace_capture]") {
  TemporaryDirectory temporary;
  const auto real_root = temporary.path() / "real";
  const auto evil_root = temporary.path() / "evil";
  const auto root_link = temporary.path() / "root-link";
  REQUIRE(std::filesystem::create_directory(real_root));
  REQUIRE(std::filesystem::create_directory(evil_root));
  std::filesystem::create_directory_symlink(real_root, root_link);
  REQUIRE(std::filesystem::is_symlink(root_link));

  SECTION("root symlink") {
    rex::graphics::FrameTraceCaptureSlot slot;
    const uint64_t token = slot.Queue(root_link);
    REQUIRE(token != 0);
    CHECK(slot.status().state == rex::graphics::FrameTraceCaptureState::kFailed);
  }

  SECTION("partial symlink") {
    rex::graphics::FrameTraceCaptureSlot slot;
    const uint64_t token = slot.Queue(real_root);
    REQUIRE(slot.Arm(token));
    std::filesystem::create_symlink(evil_root / "escaped.xtr",
                                    real_root / "latest.xtr.partial");
    REQUIRE(std::filesystem::is_symlink(real_root / "latest.xtr.partial"));
    CHECK(slot.CreatePartial(token, nullptr) < 0);
    CHECK(slot.status().state == rex::graphics::FrameTraceCaptureState::kFailed);
    CHECK_FALSE(std::filesystem::exists(evil_root / "escaped.xtr"));
  }

  SECTION("partial collision") {
    const auto collision_path = real_root / "latest.xtr.partial";
    {
      std::ofstream collision(collision_path, std::ios::binary);
      collision << "do-not-overwrite";
    }
    rex::graphics::FrameTraceCaptureSlot slot;
    const uint64_t token = slot.Queue(real_root);
    REQUIRE(slot.Arm(token));
    CHECK(slot.CreatePartial(token, nullptr) < 0);
    CHECK(slot.status().state ==
          rex::graphics::FrameTraceCaptureState::kFailed);
    std::ifstream collision(collision_path, std::ios::binary);
    const std::string preserved((std::istreambuf_iterator<char>(collision)),
                                std::istreambuf_iterator<char>());
    CHECK(preserved == "do-not-overwrite");
  }

  SECTION("root path changes after arm") {
    rex::graphics::FrameTraceCaptureSlot slot;
    const uint64_t token = slot.Queue(real_root);
    REQUIRE(slot.Arm(token));
    const auto moved_root = temporary.path() / "moved";
    std::filesystem::rename(real_root, moved_root);
    std::filesystem::create_directory_symlink(evil_root, real_root);
    REQUIRE(std::filesystem::is_symlink(real_root));
    CHECK(slot.CreatePartial(token, nullptr) < 0);
    CHECK(slot.status().state == rex::graphics::FrameTraceCaptureState::kFailed);
    CHECK_FALSE(std::filesystem::exists(evil_root / "latest.xtr.partial"));
    CHECK_FALSE(std::filesystem::exists(moved_root / "latest.xtr.partial"));
  }

  SECTION("root path changes after partial creation") {
    rex::graphics::FrameTraceCaptureSlot slot;
    const uint64_t token = slot.Queue(real_root);
    REQUIRE(slot.Arm(token));
    std::filesystem::path partial_path;
    const int descriptor = slot.CreatePartial(token, &partial_path);
    REQUIRE(descriptor >= 0);

    rex::graphics::TraceWriter writer(nullptr);
    REQUIRE(writer.OpenFileDescriptor(descriptor, partial_path, 0));
    REQUIRE(writer.BeginEdramRequirementsTracking());
    std::vector<uint8_t> edram(rex::graphics::xenos::kEdramSizeBytes);
    REQUIRE(writer.WriteEdramSnapshot(edram.data()));
    REQUIRE(writer.WriteEvent(rex::graphics::EventCommand::Type::kSwap));
    REQUIRE(slot.MarkCapturing(token));
    REQUIRE(writer.Close());

    const auto moved_root = temporary.path() / "moved-after-create";
    std::filesystem::rename(real_root, moved_root);
    std::filesystem::create_directory_symlink(evil_root, real_root);
    REQUIRE(std::filesystem::is_symlink(real_root));
    CHECK_FALSE(slot.Publish(token));
    CHECK(slot.status().state ==
          rex::graphics::FrameTraceCaptureState::kFailed);
    CHECK(slot.status().reason.find("changed before publication") !=
          std::string::npos);
    CHECK_FALSE(std::filesystem::exists(evil_root / "latest.xtr"));
    CHECK_FALSE(std::filesystem::exists(evil_root / "latest.xtr.partial"));
    CHECK_FALSE(std::filesystem::exists(moved_root / "latest.xtr"));
    CHECK_FALSE(std::filesystem::exists(moved_root / "latest.xtr.partial"));
  }
}

TEST_CASE("Published capture deletion is explicit and symlink safe",
          "[graphics][trace][trace_capture]") {
  TemporaryDirectory temporary;
  const auto final_path = temporary.path() / "latest.xtr";
  const auto partial_path = temporary.path() / "latest.xtr.partial";
  const auto outside_path = temporary.path() / "outside.xtr";

  SECTION("regular final is removed without touching partial") {
    {
      std::ofstream final(final_path, std::ios::binary);
      final << "published";
      std::ofstream partial(partial_path, std::ios::binary);
      partial << "interrupted";
    }
#ifndef _WIN32
    REQUIRE(::chmod(final_path.c_str(), 0600) == 0);
#endif
    rex::graphics::FrameTraceCaptureSlot slot;
    std::string reason;
    CHECK(slot.DeletePublishedCapture(temporary.path(), &reason));
    CHECK(reason.empty());
    CHECK_FALSE(std::filesystem::exists(final_path));
    CHECK(std::filesystem::exists(partial_path));
    CHECK(slot.status().state == rex::graphics::FrameTraceCaptureState::kIdle);
    CHECK(slot.DeletePublishedCapture(temporary.path(), &reason));
  }

#ifndef _WIN32
  SECTION("symbolic link final is rejected and target is preserved") {
    {
      std::ofstream outside(outside_path, std::ios::binary);
      outside << "preserve";
    }
    std::filesystem::create_symlink(outside_path, final_path);
    rex::graphics::FrameTraceCaptureSlot slot;
    std::string reason;
    CHECK_FALSE(slot.DeletePublishedCapture(temporary.path(), &reason));
    CHECK(std::filesystem::is_symlink(final_path));
    CHECK(std::filesystem::exists(outside_path));
    CHECK_FALSE(reason.empty());
  }

  SECTION("symbolic link root is rejected") {
    const auto real_root = temporary.path() / "real-delete-root";
    const auto linked_root = temporary.path() / "linked-delete-root";
    REQUIRE(std::filesystem::create_directory(real_root));
    {
      std::ofstream final(real_root / "latest.xtr", std::ios::binary);
      final << "preserve";
    }
    std::filesystem::create_directory_symlink(real_root, linked_root);
    rex::graphics::FrameTraceCaptureSlot slot;
    std::string reason;
    CHECK_FALSE(slot.DeletePublishedCapture(linked_root, &reason));
    CHECK(std::filesystem::exists(real_root / "latest.xtr"));
    CHECK_FALSE(reason.empty());
  }
#endif

  SECTION("active capture cannot be deleted") {
    rex::graphics::FrameTraceCaptureSlot slot;
    REQUIRE(slot.Queue(temporary.path()) != 0);
    std::string reason;
    CHECK_FALSE(slot.DeletePublishedCapture(temporary.path(), &reason));
    CHECK(reason.find("active") != std::string::npos);
    CHECK(slot.status().state == rex::graphics::FrameTraceCaptureState::kQueued);
  }
}

TEST_CASE("TraceWriter enforces the exact decoded and worst-case byte limit",
          "[graphics][trace][trace_writer]") {
  TemporaryDirectory temporary;
  constexpr uint64_t kExactLimit =
      sizeof(rex::graphics::TraceHeader) +
      sizeof(rex::graphics::PrimaryBufferEndCommand);

  SECTION("exact limit") {
    const auto path = temporary.path() / "exact.xtr";
    rex::graphics::TraceWriter writer(nullptr);
    REQUIRE(writer.SetByteBudget(kExactLimit));
    REQUIRE(writer.Open(path, 0));
    REQUIRE(writer.WritePrimaryBufferEnd());
    CHECK(writer.decoded_byte_count() == kExactLimit);
    CHECK(writer.worst_case_byte_count() == kExactLimit);
    CHECK(writer.Close());
    CHECK(writer.closed_byte_count() == kExactLimit);

    std::ifstream trace(path, std::ios::binary);
    rex::graphics::TraceHeader header{};
    REQUIRE(static_cast<bool>(
        trace.read(reinterpret_cast<char*>(&header), sizeof(header))));
    size_t revision_length = 0;
    while (revision_length < sizeof(header.build_commit_sha) &&
           header.build_commit_sha[revision_length]) {
      ++revision_length;
    }
    const std::string revision(header.build_commit_sha, revision_length);
    const std::string version = REXGLUE_VERSION_STRING;
    const size_t revision_marker = version.rfind(".g");
    if (revision_marker != std::string::npos &&
        revision_marker + 2 < version.size()) {
      size_t expected_end = revision_marker + 2;
      while (expected_end < version.size() &&
             std::isxdigit(static_cast<unsigned char>(version[expected_end]))) {
        ++expected_end;
      }
      CHECK(revision ==
            version.substr(revision_marker + 2,
                           expected_end - (revision_marker + 2)));
    } else {
      CHECK(revision == "unknown");
    }
    CHECK(revision != "rexglue-dev");
  }

  SECTION("one byte below required limit") {
    rex::graphics::TraceWriter writer(nullptr);
    REQUIRE(writer.SetByteBudget(kExactLimit - 1));
    REQUIRE(writer.Open(temporary.path() / "over.xtr", 0));
    CHECK_FALSE(writer.WritePrimaryBufferEnd());
    CHECK(writer.has_error());
    CHECK(writer.failure_reason().find("budget") != std::string_view::npos);
    CHECK(writer.decoded_byte_count() == sizeof(rex::graphics::TraceHeader));
    CHECK(writer.worst_case_byte_count() ==
          sizeof(rex::graphics::TraceHeader));
    writer.Discard();
  }
}

TEST_CASE("TraceWriter makes every injected IO phase fail closed",
          "[graphics][trace][trace_writer]") {
  TemporaryDirectory temporary;
  using Failure = rex::graphics::TraceWriter::FailurePointForTesting;

  SECTION("open") {
    rex::graphics::TraceWriter writer(nullptr);
    writer.SetFailurePointForTesting(Failure::kOpen);
    CHECK_FALSE(writer.Open(temporary.path() / "open.xtr", 0));
    CHECK(writer.has_error());
  }
  SECTION("write") {
    const auto path = temporary.path() / "write.xtr";
    rex::graphics::TraceWriter writer(nullptr);
    REQUIRE(writer.Open(path, 0));
    writer.SetFailurePointForTesting(Failure::kWrite);
    CHECK_FALSE(writer.WritePrimaryBufferEnd());
    CHECK_FALSE(writer.Close());
    CHECK_FALSE(std::filesystem::exists(path));
  }
  SECTION("flush") {
    const auto path = temporary.path() / "flush.xtr";
    rex::graphics::TraceWriter writer(nullptr);
    REQUIRE(writer.Open(path, 0));
    writer.SetFailurePointForTesting(Failure::kFlush);
    CHECK_FALSE(writer.Flush());
    CHECK_FALSE(writer.Close());
    CHECK_FALSE(std::filesystem::exists(path));
  }
  SECTION("manifest finalization") {
    const auto path = temporary.path() / "finalize.xtr";
    rex::graphics::TraceWriter writer(nullptr);
    REQUIRE(writer.Open(path, 0));
    writer.SetFailurePointForTesting(Failure::kFinalize);
    CHECK_FALSE(writer.Close());
    CHECK_FALSE(std::filesystem::exists(path));
  }
  SECTION("close") {
    const auto path = temporary.path() / "close.xtr";
    rex::graphics::TraceWriter writer(nullptr);
    REQUIRE(writer.Open(path, 0));
    writer.SetFailurePointForTesting(Failure::kClose);
    CHECK_FALSE(writer.Close());
    CHECK_FALSE(std::filesystem::exists(path));
  }
}

TEST_CASE("Private trace slot removes descriptor-backed IO failures",
          "[graphics][trace][trace_capture]") {
  TemporaryDirectory temporary;
  using Failure = rex::graphics::TraceWriter::FailurePointForTesting;

  SECTION("open failure") {
    rex::graphics::FrameTraceCaptureSlot slot;
    const uint64_t token = slot.Queue(temporary.path());
    REQUIRE(slot.Arm(token));
    std::filesystem::path partial_path;
    const int descriptor = slot.CreatePartial(token, &partial_path);
    REQUIRE(descriptor >= 0);

    rex::graphics::TraceWriter writer(nullptr);
    writer.SetFailurePointForTesting(Failure::kOpen);
    CHECK_FALSE(writer.OpenFileDescriptor(descriptor, partial_path, 0));
    slot.Fail(token, std::string(writer.failure_reason()));
    CHECK(slot.status().state ==
          rex::graphics::FrameTraceCaptureState::kFailed);
    CHECK_FALSE(std::filesystem::exists(temporary.path() /
                                        "latest.xtr.partial"));
  }

  SECTION("close failure") {
    rex::graphics::FrameTraceCaptureSlot slot;
    const uint64_t token = slot.Queue(temporary.path());
    REQUIRE(slot.Arm(token));
    std::filesystem::path partial_path;
    const int descriptor = slot.CreatePartial(token, &partial_path);
    REQUIRE(descriptor >= 0);

    rex::graphics::TraceWriter writer(nullptr);
    REQUIRE(writer.OpenFileDescriptor(descriptor, partial_path, 0));
    REQUIRE(slot.MarkCapturing(token));
    REQUIRE(writer.WriteEvent(rex::graphics::EventCommand::Type::kSwap));
    writer.SetFailurePointForTesting(Failure::kClose);
    CHECK_FALSE(writer.Close());
    slot.Fail(token, std::string(writer.failure_reason()));
    CHECK(slot.status().state ==
          rex::graphics::FrameTraceCaptureState::kFailed);
    CHECK_FALSE(std::filesystem::exists(temporary.path() /
                                        "latest.xtr.partial"));
    CHECK_FALSE(std::filesystem::exists(temporary.path() / "latest.xtr"));
  }
}

TEST_CASE("Private frame traces publish atomically and classify replay fidelity",
          "[graphics][trace][trace_capture]") {
  TemporaryDirectory temporary;
  rex::graphics::FrameTraceCaptureSlot slot;

  REQUIRE(WriteCapture(slot, temporary.path(), true, 1, 0x11111111));
  auto status = slot.status();
  REQUIRE(status.state == rex::graphics::FrameTraceCaptureState::kComplete);
  CHECK(status.final_path == std::filesystem::canonical(temporary.path()) / "latest.xtr");
  CHECK(status.byte_count == std::filesystem::file_size(status.final_path));
  CHECK_FALSE(std::filesystem::exists(temporary.path() / "latest.xtr.partial"));
  rex::graphics::TraceReader first_reader;
  REQUIRE(first_reader.Open(status.final_path.string()));
  CHECK(first_reader.header()->title_id == 0x11111111);

  REQUIRE(WriteCapture(slot, temporary.path(), false, 1, 0x22222222));
  status = slot.status();
  REQUIRE(status.state ==
          rex::graphics::FrameTraceCaptureState::kCompleteBestEffortOnly);
  CHECK(status.reason.find("best-effort") != std::string::npos);
  rex::graphics::TraceReader replacement_reader;
  REQUIRE(replacement_reader.Open(status.final_path.string()));
  CHECK(replacement_reader.header()->title_id == 0x22222222);
  CHECK_FALSE(std::filesystem::exists(temporary.path() / "latest.xtr.partial"));
}

TEST_CASE("Private frame trace rejects multiple completed swaps and cleans partials",
          "[graphics][trace][trace_capture]") {
  TemporaryDirectory temporary;
  rex::graphics::FrameTraceCaptureSlot slot;
  CHECK_FALSE(WriteCapture(slot, temporary.path(), true, 2));
  CHECK(slot.status().state == rex::graphics::FrameTraceCaptureState::kFailed);
  CHECK(slot.status().reason.find("exactly one") != std::string::npos);
  CHECK_FALSE(std::filesystem::exists(temporary.path() / "latest.xtr.partial"));
  CHECK_FALSE(std::filesystem::exists(temporary.path() / "latest.xtr"));
}

TEST_CASE("Private frame trace publication failure preserves the prior capture",
          "[graphics][trace][trace_capture]") {
  TemporaryDirectory temporary;
  rex::graphics::FrameTraceCaptureSlot slot;
  REQUIRE(WriteCapture(slot, temporary.path(), true, 1, 0x11111111));
  slot.SetRenameFailureForTesting(true);
  CHECK_FALSE(WriteCapture(slot, temporary.path(), true, 1, 0x22222222));
  CHECK(slot.status().state == rex::graphics::FrameTraceCaptureState::kFailed);
  CHECK_FALSE(std::filesystem::exists(temporary.path() / "latest.xtr.partial"));

  rex::graphics::TraceReader reader;
  REQUIRE(reader.Open((temporary.path() / "latest.xtr").string()));
  CHECK(reader.header()->title_id == 0x11111111);
}

TEST_CASE("Private frame trace durable flush failure preserves the prior capture",
          "[graphics][trace][trace_capture]") {
  TemporaryDirectory temporary;
  rex::graphics::FrameTraceCaptureSlot slot;
  REQUIRE(WriteCapture(slot, temporary.path(), true, 1, 0x11111111));
  slot.SetSyncFailureForTesting(true);
  CHECK_FALSE(WriteCapture(slot, temporary.path(), true, 1, 0x22222222));
  CHECK(slot.status().state == rex::graphics::FrameTraceCaptureState::kFailed);
  CHECK(slot.status().reason.find("flush") != std::string::npos);
  CHECK_FALSE(std::filesystem::exists(temporary.path() / "latest.xtr.partial"));

  rex::graphics::TraceReader reader;
  REQUIRE(reader.Open((temporary.path() / "latest.xtr").string()));
  CHECK(reader.header()->title_id == 0x11111111);
}

TEST_CASE("Private frame trace shutdown cancellation removes only the partial",
          "[graphics][trace][trace_capture]") {
  TemporaryDirectory temporary;
  {
    std::ofstream previous(temporary.path() / "latest.xtr", std::ios::binary);
    previous << "previous";
  }
  rex::graphics::FrameTraceCaptureSlot slot;
  const uint64_t token = slot.Queue(temporary.path());
  REQUIRE(slot.Arm(token));
  const int descriptor = slot.CreatePartial(token, nullptr);
  REQUIRE(descriptor >= 0);
#ifdef _WIN32
  _close(descriptor);
#else
  ::close(descriptor);
#endif
  REQUIRE(slot.MarkCapturing(token));
  slot.Cancel(token, "shutdown");
  CHECK(slot.status().state == rex::graphics::FrameTraceCaptureState::kCancelled);
  CHECK_FALSE(std::filesystem::exists(temporary.path() / "latest.xtr.partial"));
  CHECK(std::filesystem::file_size(temporary.path() / "latest.xtr") == 8);
}
