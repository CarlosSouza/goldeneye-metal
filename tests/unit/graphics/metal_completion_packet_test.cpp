/**
 ******************************************************************************
 * ReXGlue - Xbox 360 recompilation runtime                                  *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                       *
 *                                                                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/metal/command_processor.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory.h>

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
  void RestoreEdramSnapshot(const void*) override {}

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

 protected:
  bool SetupContext() override { return true; }
  void ShutdownContext() override {}

  bool WriteGpuMemory(uint32_t address, const void* data, size_t length) override {
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
