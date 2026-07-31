/**
 ******************************************************************************
 * ReXGlue - Xbox 360 recompilation runtime                                  *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                       *
 *                                                                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/metal/command_processor.h>
#include <rex/graphics/trace_reader.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory.h>

namespace rex::graphics::metal {

struct MetalCommandProcessorTestPeer {
  static bool EnsureEdramBacking(MetalCommandProcessor& command_processor) {
    return command_processor.EnsureEdramBgraBacking();
  }

  static const std::vector<uint8_t>& EdramBacking(
      const MetalCommandProcessor& command_processor) {
    return command_processor.edram_bgra_;
  }

  static const CanonicalEdramAuthorityState& AuthorityState(
      const MetalCommandProcessor& command_processor) {
    return command_processor.canonical_edram_state_;
  }

  static bool UnsupportedState(const MetalCommandProcessor& command_processor) {
    return command_processor.canonical_edram_unsupported_state_;
  }

  static void SetUnsupportedState(MetalCommandProcessor& command_processor, bool unsupported) {
    command_processor.canonical_edram_unsupported_state_ = unsupported;
  }

  static bool FreshStartupTargetsSkipHydration(MetalCommandProcessor& command_processor) {
    MetalCommandProcessor::HostRenderTarget color_target;
    color_target.color_info = 0x00000000;
    color_target.surface_info = 0x14020500;
    color_target.canonical_width = 1280;
    color_target.canonical_height = 720;

    MetalCommandProcessor::HostDepthStencilTarget depth_target;
    depth_target.depth_info = 0x00000400;
    depth_target.surface_info = 0x14020500;
    depth_target.width = 1280;
    depth_target.height = 720;

    std::string error;
    const bool color_ok =
        command_processor.RestoreCanonicalColorTarget(color_target, 1, &error);
    const bool depth_ok =
        command_processor.RestoreCanonicalDepthTarget(depth_target, 2, &error);
    return color_ok && depth_ok && error.empty() && !color_target.canonical_hydrated &&
           !depth_target.canonical_hydrated &&
           command_processor.canonical_edram_ownership_.sequence() == 0;
  }
};

}  // namespace rex::graphics::metal

namespace {

class TestGraphicsSystem final : public rex::graphics::GraphicsSystem {
 public:
  explicit TestGraphicsSystem(rex::memory::Memory& memory) { memory_ = &memory; }

  std::string name() const override { return "Metal"; }

 protected:
  void CreateProvider(bool with_presentation) override { (void)with_presentation; }

  std::unique_ptr<rex::graphics::CommandProcessor> CreateCommandProcessor() override {
    return nullptr;
  }
};

class RoutingCommandProcessor final : public rex::graphics::CommandProcessor {
 public:
  enum class WriteKind {
    kImmediate,
    kCompletion,
  };

  RoutingCommandProcessor(TestGraphicsSystem& graphics_system)
      : CommandProcessor(&graphics_system, nullptr) {}

  void IssueSwap(uint32_t, uint32_t, uint32_t) override {}
  void TracePlaybackWroteMemory(uint32_t, uint32_t) override {}
  bool RestoreEdramSnapshot(const void*) override { return true; }

  const std::vector<WriteKind>& writes() const { return writes_; }
  void ConfigureCompletionWait(uint32_t address, uint32_t value, uint32_t pending_wait_count = 0,
                               uint32_t stale_completed_wait_count = 0) {
    completion_wait_address_ = address;
    completion_wait_value_ = value;
    completion_wait_pending_count_ = pending_wait_count;
    completion_wait_stale_completed_count_ = stale_completed_wait_count;
  }
  void ConfigureMemoryChangeWait(uint32_t address, std::vector<uint32_t> values,
                                 uint32_t timeout_count = 0) {
    memory_change_wait_address_ = address;
    memory_change_wait_values_ = std::move(values);
    memory_change_wait_value_index_ = 0;
    memory_change_wait_timeouts_remaining_ = timeout_count;
    memory_change_wait_signal_pattern_.clear();
    memory_change_wait_signal_pattern_index_ = 0;
    memory_change_wait_timeouts_.clear();
  }
  void ConfigureMemoryChangeWaitPattern(uint32_t address, std::vector<uint32_t> values,
                                        std::vector<bool> signal_pattern) {
    ConfigureMemoryChangeWait(address, std::move(values));
    memory_change_wait_signal_pattern_ = std::move(signal_pattern);
  }
  uint32_t completion_wait_call_count() const { return completion_wait_call_count_; }
  std::chrono::milliseconds completion_wait_timeout() const { return completion_wait_timeout_; }
  uint32_t begin_memory_change_wait_call_count() const {
    return begin_memory_change_wait_call_count_;
  }
  uint32_t memory_change_wait_call_count() const { return memory_change_wait_call_count_; }
  const std::vector<std::chrono::milliseconds>& memory_change_wait_timeouts() const {
    return memory_change_wait_timeouts_;
  }
  uint32_t end_memory_change_wait_call_count() const { return end_memory_change_wait_call_count_; }
  uint32_t prepare_wait_call_count() const { return prepare_wait_call_count_; }
  uint32_t return_wait_call_count() const { return return_wait_call_count_; }
  uint64_t completed_wait_poll_count() const { return completed_wait_poll_count_; }
  bool completed_wait_matched() const { return completed_wait_matched_; }
  void StopWorkerDuringNextCompletionWait() {
    worker_running_.store(true, std::memory_order_release);
    stop_worker_during_completion_wait_ = true;
  }
  void SetTraceInitializationFailure(bool fail) { fail_trace_initialization_ = fail; }
  bool OpenTraceForTest(const std::filesystem::path& path) {
    return OpenAndInitializeTrace(path, 0x584108A9);
  }
  bool trace_is_open_for_test() const { return trace_writer_.is_open(); }
  bool trace_is_disabled_for_test() const { return trace_state_ == TraceState::kDisabled; }
  bool trace_is_streaming_for_test() const { return trace_state_ == TraceState::kStreaming; }
  bool trace_is_single_frame_for_test() const { return trace_state_ == TraceState::kSingleFrame; }
  void AcceptCallsForTest() {
    std::lock_guard<std::mutex> lock(pending_fns_mutex_);
    worker_running_.store(true, std::memory_order_release);
    worker_accepting_functions_ = true;
  }
  bool RunOnePendingCallForTest() {
    std::function<void()> fn;
    if (!TryPopPendingFunction(&fn)) {
      return false;
    }
    fn();
    return true;
  }
  void BlockNextGpuWriteForTest() {
    std::lock_guard<std::mutex> lock(blocked_write_mutex_);
    block_next_gpu_write_ = true;
    blocked_write_entered_ = false;
    release_blocked_write_ = false;
  }
  bool WaitForBlockedGpuWriteForTest(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(blocked_write_mutex_);
    return blocked_write_cv_.wait_for(
        lock, timeout, [this]() { return blocked_write_entered_; });
  }
  void ReleaseBlockedGpuWriteForTest() {
    {
      std::lock_guard<std::mutex> lock(blocked_write_mutex_);
      release_blocked_write_ = true;
    }
    blocked_write_cv_.notify_all();
  }

 protected:
  bool SetupContext() override { return true; }
  void ShutdownContext() override {}
  void InitializeTrace() override {
    CommandProcessor::InitializeTrace();
    if (fail_trace_initialization_) {
      MarkTraceInitializationIncomplete();
    }
  }

  bool WriteGpuMemory(uint32_t address, const void* data, size_t length) override {
    {
      std::unique_lock<std::mutex> lock(blocked_write_mutex_);
      if (block_next_gpu_write_) {
        block_next_gpu_write_ = false;
        blocked_write_entered_ = true;
        blocked_write_cv_.notify_all();
        blocked_write_cv_.wait(
            lock, [this]() { return release_blocked_write_; });
      }
    }
    writes_.push_back(WriteKind::kImmediate);
    return CommandProcessor::WriteGpuMemory(address, data, length);
  }

  bool WriteGpuCompletionMemory(uint32_t address, const void* data, size_t length) override {
    writes_.push_back(WriteKind::kCompletion);
    // Bypass the virtual immediate-write hook so this records exactly the
    // packet route selected by the base command processor.
    return CommandProcessor::WriteGpuMemory(address, data, length);
  }

  void PrepareForWait() override {
    ++prepare_wait_call_count_;
    CommandProcessor::PrepareForWait();
  }

  void ReturnFromWait() override { ++return_wait_call_count_; }

  GpuCompletionMemoryWriteWaitResult WaitForGpuCompletionMemoryWrite(
      uint32_t address, uint32_t length, std::chrono::milliseconds timeout) override {
    ++completion_wait_call_count_;
    completion_wait_timeout_ = timeout;
    if (stop_worker_during_completion_wait_) {
      stop_worker_during_completion_wait_ = false;
      worker_running_.store(false, std::memory_order_release);
      return GpuCompletionMemoryWriteWaitResult::kPending;
    }
    if (!completion_wait_address_ || address != completion_wait_address_ ||
        length != sizeof(completion_wait_value_)) {
      return GpuCompletionMemoryWriteWaitResult::kUnavailable;
    }
    if (completion_wait_pending_count_) {
      --completion_wait_pending_count_;
      return GpuCompletionMemoryWriteWaitResult::kPending;
    }
    if (completion_wait_stale_completed_count_) {
      --completion_wait_stale_completed_count_;
      return GpuCompletionMemoryWriteWaitResult::kCompleted;
    }
    std::memcpy(memory_->TranslatePhysical(address), &completion_wait_value_,
                sizeof(completion_wait_value_));
    return GpuCompletionMemoryWriteWaitResult::kCompleted;
  }

  bool BeginWaitRegMemMemoryChange(uint32_t address, uint32_t length) override {
    ++begin_memory_change_wait_call_count_;
    memory_change_wait_active_ = memory_change_wait_address_ &&
                                 address == memory_change_wait_address_ &&
                                 length == sizeof(uint32_t);
    return memory_change_wait_active_;
  }

  WaitRegMemMemoryChangeResult WaitForWaitRegMemMemoryChange(
      std::chrono::milliseconds timeout) override {
    ++memory_change_wait_call_count_;
    if (!memory_change_wait_active_ || timeout > std::chrono::milliseconds(4)) {
      return WaitRegMemMemoryChangeResult::kUnavailable;
    }
    memory_change_wait_timeouts_.push_back(timeout);
    if (!memory_change_wait_signal_pattern_.empty()) {
      if (memory_change_wait_signal_pattern_index_ >= memory_change_wait_signal_pattern_.size() ||
          !memory_change_wait_signal_pattern_[memory_change_wait_signal_pattern_index_++]) {
        return WaitRegMemMemoryChangeResult::kTimeout;
      }
    } else if (memory_change_wait_timeouts_remaining_) {
      --memory_change_wait_timeouts_remaining_;
      return WaitRegMemMemoryChangeResult::kTimeout;
    }
    if (memory_change_wait_value_index_ >= memory_change_wait_values_.size()) {
      return WaitRegMemMemoryChangeResult::kTimeout;
    }
    uint32_t value = memory_change_wait_values_[memory_change_wait_value_index_++];
    std::memcpy(memory_->TranslatePhysical(memory_change_wait_address_), &value, sizeof(value));
    return WaitRegMemMemoryChangeResult::kSignaled;
  }

  void EndWaitRegMemMemoryChange() override {
    ++end_memory_change_wait_call_count_;
    memory_change_wait_active_ = false;
  }

  void OnWaitRegMemComplete(bool, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                            uint32_t, uint64_t poll_count, uint64_t, bool matched, bool) override {
    completed_wait_poll_count_ = poll_count;
    completed_wait_matched_ = matched;
  }

  rex::graphics::Shader* LoadShader(rex::graphics::xenos::ShaderType, uint32_t, const uint32_t*,
                                    uint32_t) override {
    return nullptr;
  }
  bool IssueDraw(rex::graphics::xenos::PrimitiveType, uint32_t, IndexBufferInfo*, bool) override {
    return true;
  }
  bool IssueCopy() override { return true; }

 private:
  std::vector<WriteKind> writes_;
  uint32_t completion_wait_address_ = 0;
  uint32_t completion_wait_value_ = 0;
  uint32_t completion_wait_pending_count_ = 0;
  uint32_t completion_wait_stale_completed_count_ = 0;
  std::chrono::milliseconds completion_wait_timeout_{0};
  uint32_t memory_change_wait_address_ = 0;
  std::vector<uint32_t> memory_change_wait_values_;
  std::vector<std::chrono::milliseconds> memory_change_wait_timeouts_;
  std::vector<bool> memory_change_wait_signal_pattern_;
  size_t memory_change_wait_value_index_ = 0;
  size_t memory_change_wait_signal_pattern_index_ = 0;
  uint32_t memory_change_wait_timeouts_remaining_ = 0;
  bool memory_change_wait_active_ = false;
  uint32_t completion_wait_call_count_ = 0;
  uint32_t begin_memory_change_wait_call_count_ = 0;
  uint32_t memory_change_wait_call_count_ = 0;
  uint32_t end_memory_change_wait_call_count_ = 0;
  uint32_t prepare_wait_call_count_ = 0;
  uint32_t return_wait_call_count_ = 0;
  uint64_t completed_wait_poll_count_ = 0;
  bool completed_wait_matched_ = false;
  bool stop_worker_during_completion_wait_ = false;
  bool fail_trace_initialization_ = false;
  std::mutex blocked_write_mutex_;
  std::condition_variable blocked_write_cv_;
  bool block_next_gpu_write_ = false;
  bool blocked_write_entered_ = false;
  bool release_blocked_write_ = false;
};

struct TemporaryDirectory {
  std::filesystem::path path;

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

void StorePacketDword(rex::memory::Memory& memory, uint32_t packet_address, uint32_t index,
                      uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(
      memory.TranslatePhysical(packet_address + index * sizeof(uint32_t)), value);
}

void StoreWaitRegMemPacket(rex::memory::Memory& memory, uint32_t packet_address,
                           uint32_t target_address, uint32_t reference) {
  StorePacketDword(
      memory, packet_address, 0,
      rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_WAIT_REG_MEM, 5));
  StorePacketDword(memory, packet_address, 1, 0x10 | 0x3);
  StorePacketDword(memory, packet_address, 2, target_address);
  StorePacketDword(memory, packet_address, 3, reference);
  StorePacketDword(memory, packet_address, 4, UINT32_MAX);
  StorePacketDword(memory, packet_address, 5, 0x100);
}

}  // namespace

TEST_CASE("Incomplete trace initialization is discarded and disarms the request",
          "[graphics][trace]") {
  rex::InitLogging();
  rex::memory::Memory memory;

  TestGraphicsSystem graphics_system(memory);
  RoutingCommandProcessor command_processor(graphics_system);
  TemporaryDirectory temporary{
      std::filesystem::temp_directory_path() /
      ("rex-trace-initialization-" +
       std::to_string(reinterpret_cast<uintptr_t>(&command_processor)))};
  REQUIRE(std::filesystem::create_directories(temporary.path));

  CHECK_FALSE(command_processor.BeginTracing(temporary.path));
  CHECK(command_processor.trace_is_disabled_for_test());
  CHECK_FALSE(command_processor.EndTracing());

  command_processor.AcceptCallsForTest();
  command_processor.SetTraceInitializationFailure(true);

  REQUIRE(command_processor.BeginTracing(temporary.path));
  REQUIRE(command_processor.RunOnePendingCallForTest());
  REQUIRE(command_processor.trace_is_streaming_for_test());
  const auto streaming_path = temporary.path / "stream.xtr";
  CHECK_FALSE(command_processor.OpenTraceForTest(streaming_path));
  CHECK(command_processor.trace_is_disabled_for_test());
  CHECK_FALSE(command_processor.trace_is_open_for_test());
  CHECK_FALSE(std::filesystem::exists(streaming_path));

  REQUIRE(command_processor.RequestFrameTrace(temporary.path));
  REQUIRE(command_processor.RunOnePendingCallForTest());
  REQUIRE(command_processor.trace_is_single_frame_for_test());
  const auto frame_path = temporary.path / "frame.xtr";
  CHECK_FALSE(command_processor.OpenTraceForTest(frame_path));
  CHECK(command_processor.trace_is_disabled_for_test());
  CHECK_FALSE(command_processor.trace_is_open_for_test());
  CHECK_FALSE(std::filesystem::exists(frame_path));

  REQUIRE(command_processor.BeginTracing(temporary.path));
  REQUIRE(command_processor.RunOnePendingCallForTest());
  REQUIRE(command_processor.trace_is_streaming_for_test());
  CHECK_FALSE(command_processor.OpenTraceForTest(temporary.path));
  CHECK(command_processor.trace_is_disabled_for_test());

  command_processor.SetTraceInitializationFailure(false);
  REQUIRE(command_processor.BeginTracing(temporary.path));
  REQUIRE(command_processor.RunOnePendingCallForTest());
  const auto successful_path = temporary.path / "complete.xtr";
  REQUIRE(command_processor.OpenTraceForTest(successful_path));
  CHECK(command_processor.trace_is_open_for_test());
  REQUIRE(command_processor.EndTracing());
  REQUIRE(command_processor.RunOnePendingCallForTest());
  CHECK(command_processor.trace_is_disabled_for_test());
  CHECK(std::filesystem::exists(successful_path));
  command_processor.Shutdown();
}

TEST_CASE("Trace stop cancels pending and discards incomplete frame captures",
          "[graphics][trace][trace_lifecycle]") {
  rex::InitLogging();
  rex::memory::Memory memory;

  TestGraphicsSystem graphics_system(memory);
  RoutingCommandProcessor command_processor(graphics_system);
  TemporaryDirectory temporary{
      std::filesystem::temp_directory_path() /
      ("rex-trace-stop-" +
       std::to_string(reinterpret_cast<uintptr_t>(&command_processor)))};
  REQUIRE(std::filesystem::create_directories(temporary.path));
  command_processor.AcceptCallsForTest();

  REQUIRE(command_processor.BeginTracing(temporary.path));
  CHECK(command_processor.trace_is_disabled_for_test());
  REQUIRE(command_processor.RunOnePendingCallForTest());
  CHECK(command_processor.trace_is_streaming_for_test());
  REQUIRE(command_processor.EndTracing());
  CHECK(command_processor.trace_is_streaming_for_test());
  REQUIRE(command_processor.RunOnePendingCallForTest());
  CHECK(command_processor.trace_is_disabled_for_test());

  REQUIRE(command_processor.RequestFrameTrace(temporary.path));
  REQUIRE(command_processor.RunOnePendingCallForTest());
  REQUIRE(command_processor.trace_is_single_frame_for_test());
  const auto incomplete_frame_path = temporary.path / "incomplete-frame.xtr";
  REQUIRE(command_processor.OpenTraceForTest(incomplete_frame_path));
  REQUIRE(std::filesystem::exists(incomplete_frame_path));
  REQUIRE(command_processor.EndTracing());
  REQUIRE(command_processor.RunOnePendingCallForTest());
  CHECK(command_processor.trace_is_disabled_for_test());
  CHECK_FALSE(command_processor.trace_is_open_for_test());
  CHECK_FALSE(std::filesystem::exists(incomplete_frame_path));

  command_processor.Shutdown();
  CHECK_FALSE(command_processor.BeginTracing(temporary.path));
}

TEST_CASE("Trace finalization waits for an active direct packet span",
          "[graphics][trace][trace_lifecycle]") {
  using namespace std::chrono_literals;

  rex::InitLogging();
  rex::memory::Memory memory;
  REQUIRE(memory.Initialize());

  TestGraphicsSystem graphics_system(memory);
  RoutingCommandProcessor command_processor(graphics_system);
  TemporaryDirectory temporary{
      std::filesystem::temp_directory_path() /
      ("rex-trace-packet-serialization-" +
       std::to_string(reinterpret_cast<uintptr_t>(&command_processor)))};
  REQUIRE(std::filesystem::create_directories(temporary.path));
  command_processor.AcceptCallsForTest();

  REQUIRE(command_processor.BeginTracing(temporary.path));
  REQUIRE(command_processor.RunOnePendingCallForTest());
  const auto trace_path = temporary.path / "serialized.xtr";
  REQUIRE(command_processor.OpenTraceForTest(trace_path));

  constexpr uint32_t kPacketAddress = 0x00100000;
  constexpr uint32_t kWriteAddress = 0x00110000;
  StorePacketDword(
      memory, kPacketAddress, 0,
      rex::graphics::xenos::MakePacketType3(
          rex::graphics::xenos::PM4_REG_TO_MEM, 2));
  StorePacketDword(memory, kPacketAddress, 1, 0);
  StorePacketDword(memory, kPacketAddress, 2, kWriteAddress);

  command_processor.BlockNextGpuWriteForTest();
  auto packet_future = std::async(std::launch::async, [&]() {
    return command_processor.ExecutePacket(kPacketAddress, 3);
  });
  REQUIRE(command_processor.WaitForBlockedGpuWriteForTest(1s));

  REQUIRE(command_processor.EndTracing());
  auto stop_future = std::async(std::launch::async, [&]() {
    return command_processor.RunOnePendingCallForTest();
  });
  CHECK(stop_future.wait_for(25ms) == std::future_status::timeout);

  command_processor.ReleaseBlockedGpuWriteForTest();
  REQUIRE(packet_future.wait_for(1s) == std::future_status::ready);
  CHECK(packet_future.get());
  REQUIRE(stop_future.wait_for(1s) == std::future_status::ready);
  CHECK(stop_future.get());
  CHECK(command_processor.trace_is_disabled_for_test());
  CHECK_FALSE(command_processor.trace_is_open_for_test());

  rex::graphics::TraceReader reader;
  CHECK(reader.Open(trace_path.string()));
  command_processor.Shutdown();
}

TEST_CASE("EVENT_WRITE_EXT reports are not routed through the completion fence hook",
          "[graphics][completion]") {
  rex::InitLogging();
  rex::memory::Memory memory;
  REQUIRE(memory.Initialize());

  TestGraphicsSystem graphics_system(memory);
  RoutingCommandProcessor command_processor(graphics_system);

  constexpr uint32_t kPacketAddress = 0x00100000;
  constexpr uint32_t kExtTargetAddress = 0x00110000;
  constexpr uint32_t kShdTargetAddress = 0x00120000;
  constexpr uint32_t kShdValue = 0x12345678;

  StorePacketDword(
      memory, kPacketAddress, 0,
      rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_EVENT_WRITE_EXT, 2));
  StorePacketDword(memory, kPacketAddress, 1, 0);
  StorePacketDword(memory, kPacketAddress, 2,
                   kExtTargetAddress | uint32_t(rex::graphics::xenos::Endian::k8in16));
  for (uint32_t report = 0; report < 3; ++report) {
    command_processor.ExecutePacket(kPacketAddress, 3);
  }

  StorePacketDword(
      memory, kPacketAddress, 0,
      rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_EVENT_WRITE_SHD, 3));
  StorePacketDword(memory, kPacketAddress, 1, 0);
  StorePacketDword(memory, kPacketAddress, 2, kShdTargetAddress);
  StorePacketDword(memory, kPacketAddress, 3, kShdValue);
  command_processor.ExecutePacket(kPacketAddress, 4);

  const std::vector<RoutingCommandProcessor::WriteKind> expected = {
      RoutingCommandProcessor::WriteKind::kImmediate,
      RoutingCommandProcessor::WriteKind::kImmediate,
      RoutingCommandProcessor::WriteKind::kImmediate,
      RoutingCommandProcessor::WriteKind::kCompletion,
  };
  CHECK(command_processor.writes() == expected);
}

TEST_CASE("WAIT_REG_MEM gives a queued GPU completion write one direct wait",
          "[graphics][completion]") {
  rex::InitLogging();
  rex::memory::Memory memory;
  REQUIRE(memory.Initialize());

  TestGraphicsSystem graphics_system(memory);
  RoutingCommandProcessor command_processor(graphics_system);

  constexpr uint32_t kPacketAddress = 0x00100000;
  constexpr uint32_t kTargetAddress = 0x00130000;
  constexpr uint32_t kPendingValue = 1;
  constexpr uint32_t kCompletedValue = 0;
  std::memcpy(memory.TranslatePhysical(kTargetAddress), &kPendingValue, sizeof(kPendingValue));
  command_processor.ConfigureCompletionWait(kTargetAddress, kCompletedValue);
  command_processor.ConfigureMemoryChangeWait(kTargetAddress, {});

  StoreWaitRegMemPacket(memory, kPacketAddress, kTargetAddress, kCompletedValue);

  command_processor.ExecutePacket(kPacketAddress, 6);

  CHECK(command_processor.prepare_wait_call_count() == 1);
  CHECK(command_processor.completion_wait_call_count() == 1);
  CHECK(command_processor.completion_wait_timeout() == std::chrono::milliseconds(1));
  CHECK(command_processor.begin_memory_change_wait_call_count() == 1);
  CHECK(command_processor.memory_change_wait_call_count() == 0);
  CHECK(command_processor.end_memory_change_wait_call_count() == 1);
  CHECK(command_processor.return_wait_call_count() == 1);
  CHECK(command_processor.completed_wait_poll_count() == 2);
  CHECK(command_processor.completed_wait_matched());
}

TEST_CASE("WAIT_REG_MEM retries a pending GPU completion without generic polling",
          "[graphics][completion]") {
  rex::InitLogging();
  rex::memory::Memory memory;
  REQUIRE(memory.Initialize());

  TestGraphicsSystem graphics_system(memory);
  RoutingCommandProcessor command_processor(graphics_system);

  constexpr uint32_t kPacketAddress = 0x00100000;
  constexpr uint32_t kTargetAddress = 0x00130000;
  constexpr uint32_t kPendingValue = 1;
  constexpr uint32_t kCompletedValue = 0;
  std::memcpy(memory.TranslatePhysical(kTargetAddress), &kPendingValue, sizeof(kPendingValue));
  command_processor.ConfigureCompletionWait(kTargetAddress, kCompletedValue, 2);
  command_processor.ConfigureMemoryChangeWait(kTargetAddress, {});

  StoreWaitRegMemPacket(memory, kPacketAddress, kTargetAddress, kCompletedValue);
  command_processor.ExecutePacket(kPacketAddress, 6);

  CHECK(command_processor.prepare_wait_call_count() == 1);
  CHECK(command_processor.completion_wait_call_count() == 3);
  CHECK(command_processor.completion_wait_timeout() == std::chrono::milliseconds(1));
  CHECK(command_processor.begin_memory_change_wait_call_count() == 1);
  CHECK(command_processor.memory_change_wait_call_count() == 0);
  CHECK(command_processor.end_memory_change_wait_call_count() == 1);
  CHECK(command_processor.return_wait_call_count() == 1);
  CHECK(command_processor.completed_wait_poll_count() == 4);
  CHECK(command_processor.completed_wait_matched());
}

TEST_CASE("WAIT_REG_MEM rechecks a completed GPU notification as a hint",
          "[graphics][completion]") {
  rex::InitLogging();
  rex::memory::Memory memory;
  REQUIRE(memory.Initialize());

  TestGraphicsSystem graphics_system(memory);
  RoutingCommandProcessor command_processor(graphics_system);

  constexpr uint32_t kPacketAddress = 0x00100000;
  constexpr uint32_t kTargetAddress = 0x00130000;
  constexpr uint32_t kPendingValue = 1;
  constexpr uint32_t kCompletedValue = 0;
  std::memcpy(memory.TranslatePhysical(kTargetAddress), &kPendingValue, sizeof(kPendingValue));
  command_processor.ConfigureCompletionWait(kTargetAddress, kCompletedValue, 0, 1);
  command_processor.ConfigureMemoryChangeWait(kTargetAddress, {});

  StoreWaitRegMemPacket(memory, kPacketAddress, kTargetAddress, kCompletedValue);
  command_processor.ExecutePacket(kPacketAddress, 6);

  CHECK(command_processor.completion_wait_call_count() == 2);
  CHECK(command_processor.memory_change_wait_call_count() == 0);
  CHECK(command_processor.completed_wait_poll_count() == 3);
  CHECK(command_processor.completed_wait_matched());
}

TEST_CASE("WAIT_REG_MEM wakes for a guest CPU memory change and rechecks the predicate",
          "[graphics][completion]") {
  rex::InitLogging();
  rex::memory::Memory memory;
  REQUIRE(memory.Initialize());

  TestGraphicsSystem graphics_system(memory);
  RoutingCommandProcessor command_processor(graphics_system);

  constexpr uint32_t kPacketAddress = 0x00100000;
  constexpr uint32_t kTargetAddress = 0x00130000;
  constexpr uint32_t kPendingValue = 1;
  constexpr uint32_t kCompletedValue = 0;
  std::memcpy(memory.TranslatePhysical(kTargetAddress), &kPendingValue, sizeof(kPendingValue));
  command_processor.ConfigureMemoryChangeWait(kTargetAddress, {kCompletedValue});
  StoreWaitRegMemPacket(memory, kPacketAddress, kTargetAddress, kCompletedValue);

  command_processor.ExecutePacket(kPacketAddress, 6);

  CHECK(command_processor.begin_memory_change_wait_call_count() == 1);
  CHECK(command_processor.completion_wait_call_count() == 1);
  CHECK(command_processor.memory_change_wait_call_count() == 1);
  CHECK(command_processor.end_memory_change_wait_call_count() == 1);
  CHECK(command_processor.prepare_wait_call_count() == 1);
  CHECK(command_processor.return_wait_call_count() == 1);
  CHECK(command_processor.completed_wait_poll_count() == 2);
  CHECK(command_processor.completed_wait_matched());
}

TEST_CASE("WAIT_REG_MEM backs off bounded CPU notification timeouts", "[graphics][completion]") {
  rex::InitLogging();
  rex::memory::Memory memory;
  REQUIRE(memory.Initialize());

  TestGraphicsSystem graphics_system(memory);
  RoutingCommandProcessor command_processor(graphics_system);

  constexpr uint32_t kPacketAddress = 0x00100000;
  constexpr uint32_t kTargetAddress = 0x00130000;
  constexpr uint32_t kPendingValue = 1;
  constexpr uint32_t kCompletedValue = 0;
  std::memcpy(memory.TranslatePhysical(kTargetAddress), &kPendingValue, sizeof(kPendingValue));
  command_processor.ConfigureMemoryChangeWait(kTargetAddress, {kCompletedValue}, 2);
  StoreWaitRegMemPacket(memory, kPacketAddress, kTargetAddress, kCompletedValue);

  command_processor.ExecutePacket(kPacketAddress, 6);

  const std::vector<std::chrono::milliseconds> expected_timeouts = {
      std::chrono::milliseconds(1), std::chrono::milliseconds(2), std::chrono::milliseconds(4)};
  CHECK(command_processor.memory_change_wait_timeouts() == expected_timeouts);
  CHECK(command_processor.completion_wait_call_count() == 1);
  CHECK(command_processor.memory_change_wait_call_count() == 3);
  CHECK(command_processor.completed_wait_poll_count() == 4);
  CHECK(command_processor.completed_wait_matched());
}

TEST_CASE("WAIT_REG_MEM resets CPU notification backoff after a signal", "[graphics][completion]") {
  rex::InitLogging();
  rex::memory::Memory memory;
  REQUIRE(memory.Initialize());

  TestGraphicsSystem graphics_system(memory);
  RoutingCommandProcessor command_processor(graphics_system);

  constexpr uint32_t kPacketAddress = 0x00100000;
  constexpr uint32_t kTargetAddress = 0x00130000;
  constexpr uint32_t kPendingValue = 1;
  constexpr uint32_t kSpuriousValue = 2;
  constexpr uint32_t kCompletedValue = 0;
  std::memcpy(memory.TranslatePhysical(kTargetAddress), &kPendingValue, sizeof(kPendingValue));
  command_processor.ConfigureMemoryChangeWaitPattern(
      kTargetAddress, {kSpuriousValue, kCompletedValue}, {false, true, false, true});
  StoreWaitRegMemPacket(memory, kPacketAddress, kTargetAddress, kCompletedValue);

  command_processor.ExecutePacket(kPacketAddress, 6);

  const std::vector<std::chrono::milliseconds> expected_timeouts = {
      std::chrono::milliseconds(1), std::chrono::milliseconds(2), std::chrono::milliseconds(1),
      std::chrono::milliseconds(2)};
  CHECK(command_processor.memory_change_wait_timeouts() == expected_timeouts);
  CHECK(command_processor.completion_wait_call_count() == 1);
  CHECK(command_processor.memory_change_wait_call_count() == 4);
  CHECK(command_processor.completed_wait_poll_count() == 5);
  CHECK(command_processor.completed_wait_matched());
}

TEST_CASE("WAIT_REG_MEM treats memory notifications as hints until the predicate matches",
          "[graphics][completion]") {
  rex::InitLogging();
  rex::memory::Memory memory;
  REQUIRE(memory.Initialize());

  TestGraphicsSystem graphics_system(memory);
  RoutingCommandProcessor command_processor(graphics_system);

  constexpr uint32_t kPacketAddress = 0x00100000;
  constexpr uint32_t kTargetAddress = 0x00130000;
  constexpr uint32_t kPendingValue = 1;
  constexpr uint32_t kSpuriousValue = 2;
  constexpr uint32_t kCompletedValue = 0;
  std::memcpy(memory.TranslatePhysical(kTargetAddress), &kPendingValue, sizeof(kPendingValue));
  command_processor.ConfigureMemoryChangeWait(kTargetAddress, {kSpuriousValue, kCompletedValue});
  StoreWaitRegMemPacket(memory, kPacketAddress, kTargetAddress, kCompletedValue);

  command_processor.ExecutePacket(kPacketAddress, 6);

  CHECK(command_processor.begin_memory_change_wait_call_count() == 1);
  CHECK(command_processor.completion_wait_call_count() == 1);
  CHECK(command_processor.memory_change_wait_call_count() == 2);
  CHECK(command_processor.end_memory_change_wait_call_count() == 1);
  CHECK(command_processor.completed_wait_poll_count() == 3);
  CHECK(command_processor.completed_wait_matched());
}

TEST_CASE("WAIT_REG_MEM disarms an initial-match memory notification without blocking",
          "[graphics][completion]") {
  rex::InitLogging();
  rex::memory::Memory memory;
  REQUIRE(memory.Initialize());

  TestGraphicsSystem graphics_system(memory);
  RoutingCommandProcessor command_processor(graphics_system);

  constexpr uint32_t kPacketAddress = 0x00100000;
  constexpr uint32_t kTargetAddress = 0x00130000;
  constexpr uint32_t kCompletedValue = 0;
  std::memcpy(memory.TranslatePhysical(kTargetAddress), &kCompletedValue, sizeof(kCompletedValue));
  command_processor.ConfigureMemoryChangeWait(kTargetAddress, {});
  StoreWaitRegMemPacket(memory, kPacketAddress, kTargetAddress, kCompletedValue);

  command_processor.ExecutePacket(kPacketAddress, 6);

  CHECK(command_processor.begin_memory_change_wait_call_count() == 1);
  CHECK(command_processor.completion_wait_call_count() == 0);
  CHECK(command_processor.memory_change_wait_call_count() == 0);
  CHECK(command_processor.end_memory_change_wait_call_count() == 1);
  CHECK(command_processor.prepare_wait_call_count() == 0);
  CHECK(command_processor.return_wait_call_count() == 0);
  CHECK(command_processor.completed_wait_poll_count() == 1);
  CHECK(command_processor.completed_wait_matched());
}

TEST_CASE("WAIT_REG_MEM distinguishes direct execution from an active worker shutdown",
          "[graphics][completion]") {
  rex::InitLogging();
  rex::memory::Memory memory;
  REQUIRE(memory.Initialize());

  TestGraphicsSystem graphics_system(memory);
  RoutingCommandProcessor command_processor(graphics_system);

  constexpr uint32_t kPacketAddress = 0x00100000;
  constexpr uint32_t kTargetAddress = 0x00130000;
  constexpr uint32_t kPendingValue = 1;
  constexpr uint32_t kCompletedValue = 0;
  std::memcpy(memory.TranslatePhysical(kTargetAddress), &kPendingValue, sizeof(kPendingValue));
  command_processor.ConfigureCompletionWait(kTargetAddress, kCompletedValue);
  command_processor.ConfigureMemoryChangeWait(kTargetAddress, {});
  command_processor.StopWorkerDuringNextCompletionWait();
  StoreWaitRegMemPacket(memory, kPacketAddress, kTargetAddress, kCompletedValue);

  command_processor.ExecutePacket(kPacketAddress, 6);

  CHECK(command_processor.completion_wait_call_count() == 1);
  CHECK(command_processor.end_memory_change_wait_call_count() == 1);
  CHECK(command_processor.return_wait_call_count() == 1);
  CHECK(command_processor.completed_wait_poll_count() == 1);
  CHECK_FALSE(command_processor.completed_wait_matched());
}

TEST_CASE("Metal packet writes remain guest-visible without an initialized Metal context",
          "[graphics][metal][completion]") {
  rex::InitLogging();
  rex::memory::Memory memory;
  REQUIRE(memory.Initialize());

  TestGraphicsSystem graphics_system(memory);
  rex::graphics::metal::MetalCommandProcessor command_processor(&graphics_system, nullptr);

  constexpr uint32_t kPacketAddress = 0x00100000;
  constexpr uint32_t kShdTargetAddress = 0x00110000;
  constexpr uint32_t kExtTargetAddress = 0x00120000;
  constexpr uint32_t kShdValue = 0x12345678;

  auto* shd_target = memory.TranslatePhysical(kShdTargetAddress);
  std::memset(shd_target, 0xCD, sizeof(kShdValue));

  StorePacketDword(
      memory, kPacketAddress, 0,
      rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_EVENT_WRITE_SHD, 3));
  StorePacketDword(memory, kPacketAddress, 1, 0);
  StorePacketDword(memory, kPacketAddress, 2, kShdTargetAddress);
  StorePacketDword(memory, kPacketAddress, 3, kShdValue);

  command_processor.ExecutePacket(kPacketAddress, 4);

  uint32_t shd_actual = 0;
  std::memcpy(&shd_actual, shd_target, sizeof(shd_actual));
  CHECK(shd_actual == kShdValue);

  auto* ext_target = memory.TranslatePhysical(kExtTargetAddress);
  std::memset(ext_target, 0xCD, 12);

  StorePacketDword(
      memory, kPacketAddress, 0,
      rex::graphics::xenos::MakePacketType3(rex::graphics::xenos::PM4_EVENT_WRITE_EXT, 2));
  StorePacketDword(memory, kPacketAddress, 1, 0);
  StorePacketDword(memory, kPacketAddress, 2,
                   kExtTargetAddress | uint32_t(rex::graphics::xenos::Endian::k8in16));

  command_processor.ExecutePacket(kPacketAddress, 3);

  constexpr std::array<uint8_t, 12> kExpectedExtents = {
      0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01,
  };
  std::array<uint8_t, 12> ext_actual;
  std::memcpy(ext_actual.data(), ext_target, ext_actual.size());
  CHECK(ext_actual == kExpectedExtents);
}

TEST_CASE("Metal fresh canonical EDRAM backing remains non-authoritative",
          "[graphics][metal][edram]") {
  rex::InitLogging();
  rex::memory::Memory memory;
  REQUIRE(memory.Initialize());

  TestGraphicsSystem graphics_system(memory);
  rex::graphics::metal::MetalCommandProcessor command_processor(&graphics_system, nullptr);
  using TestPeer = rex::graphics::metal::MetalCommandProcessorTestPeer;

  // Allocation is storage-only. It must not authorize zero-filled bytes for
  // target hydration or erase a previously detected unsupported state.
  TestPeer::SetUnsupportedState(command_processor, true);
  REQUIRE(TestPeer::EnsureEdramBacking(command_processor));
  const auto& fresh_backing = TestPeer::EdramBacking(command_processor);
  REQUIRE(fresh_backing.size() == rex::graphics::xenos::kEdramSizeBytes);
  CHECK(std::all_of(fresh_backing.begin(), fresh_backing.end(),
                    [](uint8_t value) { return value == 0; }));
  CHECK_FALSE(TestPeer::AuthorityState(command_processor).has_snapshot());
  CHECK_FALSE(TestPeer::AuthorityState(command_processor).target_hydration_enabled());
  CHECK(TestPeer::UnsupportedState(command_processor));

  // These are the exact dimensions and 4x surface configuration used at
  // GoldenEye startup. With fresh backing they must return before any Metal
  // import is attempted.
  TestPeer::SetUnsupportedState(command_processor, false);
  CHECK(TestPeer::FreshStartupTargetsSkipHydration(command_processor));

  // A real replay restore is the transition that makes canonical bytes
  // authoritative for future private Metal targets.
  std::vector<uint8_t> restored(rex::graphics::xenos::kEdramSizeBytes, 0x5A);
  REQUIRE(command_processor.RestoreEdramSnapshot(restored.data()));
  CHECK(TestPeer::AuthorityState(command_processor).has_snapshot());
  CHECK(TestPeer::AuthorityState(command_processor).target_hydration_enabled());
  CHECK_FALSE(TestPeer::UnsupportedState(command_processor));
  CHECK(TestPeer::EdramBacking(command_processor) == restored);
}
