/**
 ******************************************************************************
 * ReXGlue - Xbox 360 recompilation runtime                                  *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors                                       *
 *                                                                            *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import <Metal/Metal.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/metal/shared_memory.h>
#include <rex/graphics/trace_writer.h>
#include <rex/memory.h>
#include <rex/thread.h>

namespace rex::graphics::metal {

struct MetalSharedMemoryTestPeer {
  static void SetCompletionEvent(MetalSharedMemory& shared_memory,
                                 std::shared_ptr<rex::thread::Event> completion_event) {
    shared_memory.ordered_guest_memory_write_completion_event_ = std::move(completion_event);
    shared_memory.ordered_guest_memory_write_completion_event_wait_available_ = true;
  }
};

}  // namespace rex::graphics::metal

namespace {

class UnsupportedWaitEvent final : public rex::thread::Event {
 public:
  void* native_handle() const override { return nullptr; }
  void Set() override {}
  void Reset() override {}
  void Pulse() override {}
};

struct SharedEventSignalGuard {
  id<MTLSharedEvent> event = nil;

  ~SharedEventSignalGuard() {
    if (event) {
      event.signaledValue = 1;
    }
  }
};

struct CpuInvalidationProbe {
  std::unique_ptr<rex::thread::Event> event = rex::thread::Event::CreateAutoResetEvent(false);
  std::atomic<uint32_t> first{UINT32_MAX};
  std::atomic<uint32_t> last{0};

  void Arm(uint32_t address, uint32_t length) {
    first.store(UINT32_MAX, std::memory_order_release);
    event->Reset();
    last.store(address + length - 1, std::memory_order_relaxed);
    first.store(address, std::memory_order_release);
  }

  static void Callback(const std::unique_lock<std::recursive_mutex>&, void* context,
                       uint32_t address_first, uint32_t address_last, bool invalidated_by_gpu) {
    auto* probe = static_cast<CpuInvalidationProbe*>(context);
    if (!probe || invalidated_by_gpu || address_last < address_first) {
      return;
    }
    uint32_t probe_first = probe->first.load(std::memory_order_acquire);
    uint32_t probe_last = probe->last.load(std::memory_order_relaxed);
    if (probe_first != UINT32_MAX && address_first <= probe_last && address_last >= probe_first) {
      probe->event->Set();
    }
  }
};

struct GpuPublicationProbe {
  std::atomic<uint32_t> count{0};

  static void Callback(const std::unique_lock<std::recursive_mutex>&, void* context, uint32_t,
                       uint32_t, bool invalidated_by_gpu) {
    auto* probe = static_cast<GpuPublicationProbe*>(context);
    if (probe && invalidated_by_gpu) {
      probe->count.fetch_add(1, std::memory_order_release);
    }
  }
};

struct TemporaryFile {
  std::filesystem::path path;

  ~TemporaryFile() {
    std::error_code error;
    std::filesystem::remove(path, error);
  }
};

}  // namespace

TEST_CASE("Metal trace initialization captures completed GPU-written shared memory",
          "[graphics][metal][shared-memory][trace][integration]") {
  @autoreleasepool {
    rex::memory::Memory memory;
    if (!memory.Initialize()) {
      SKIP("Guest memory mapping is unavailable in this environment");
    }
    rex::graphics::TraceWriter trace_writer(memory.physical_membase());
    rex::graphics::metal::MetalSharedMemory shared_memory(memory, trace_writer);

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      SKIP("No Metal device is available in this environment");
    }
    if (!shared_memory.Initialize((void*)device)) {
      SKIP("Metal shared memory is unavailable in this environment");
    }

    constexpr uint32_t kTargetAddress = 0x001A0040;
    constexpr uint32_t kTargetValue = 0xC0DEC0DE;
    REQUIRE(shared_memory.EnqueueGpuOrderedGuestMemoryWrite(kTargetAddress, &kTargetValue,
                                                            sizeof(kTargetValue)));

    std::atomic<uint32_t> synchronization_call_count = 0;
    shared_memory.SetHostResourceMutationCallback([&]() {
      synchronization_call_count.fetch_add(1, std::memory_order_relaxed);
      return true;
    });

    TemporaryFile trace_file{
        std::filesystem::temp_directory_path() /
        ("rex-metal-shared-memory-trace-" +
         std::to_string(reinterpret_cast<uintptr_t>(&shared_memory)) + ".xtr")};
    REQUIRE(trace_writer.Open(trace_file.path, 0x584108A9));
    REQUIRE(shared_memory.InitializeTraceDownload());
    trace_writer.Close();
    shared_memory.SetHostResourceMutationCallback({});

    CHECK(synchronization_call_count.load(std::memory_order_relaxed) == 1);

    std::ifstream trace_stream(trace_file.path, std::ios::binary | std::ios::ate);
    REQUIRE(trace_stream.is_open());
    std::streamsize trace_size = trace_stream.tellg();
    REQUIRE(trace_size >=
            std::streamsize(sizeof(rex::graphics::TraceHeader) +
                            sizeof(rex::graphics::MemoryCommand)));
    trace_stream.seekg(0, std::ios::beg);
    std::vector<uint8_t> trace_bytes(static_cast<size_t>(trace_size));
    REQUIRE(trace_stream.read(reinterpret_cast<char*>(trace_bytes.data()), trace_size));

    rex::graphics::TraceHeader header = {};
    std::memcpy(&header, trace_bytes.data(), sizeof(header));
    CHECK(header.version == rex::graphics::kTraceFormatVersion);
    CHECK(header.title_id == 0x584108A9);

    rex::graphics::MemoryCommand command = {};
    std::memcpy(&command, trace_bytes.data() + sizeof(header), sizeof(command));
    uint32_t page_size = uint32_t(rex::memory::page_size());
    uint32_t page_base = kTargetAddress & ~(page_size - 1);
    CHECK(command.type == rex::graphics::TraceCommandType::kMemoryRead);
    CHECK(command.base_ptr == page_base);
    CHECK(command.encoding_format == rex::graphics::MemoryEncodingFormat::kNone);
    CHECK(command.encoded_length == page_size);
    CHECK(command.decoded_length == page_size);
    REQUIRE(trace_bytes.size() == sizeof(header) + sizeof(command) + page_size);

    uint32_t captured_value = 0;
    std::memcpy(&captured_value,
                trace_bytes.data() + sizeof(header) + sizeof(command) +
                    (kTargetAddress - page_base),
                sizeof(captured_value));
    CHECK(captured_value == kTargetValue);

    shared_memory.Shutdown();
  }
}

TEST_CASE("Metal ordered completion staging is recycled only after GPU completion",
          "[graphics][metal][completion][integration]") {
  @autoreleasepool {
    rex::memory::Memory memory;
    REQUIRE(memory.Initialize());
    rex::graphics::TraceWriter trace_writer(memory.physical_membase());
    rex::graphics::metal::MetalSharedMemory shared_memory(memory, trace_writer);

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);
    REQUIRE(shared_memory.Initialize((void*)device));

    constexpr uint32_t kFirstTargetAddress = 0x00140000;
    constexpr uint32_t kSecondTargetAddress = 0x00140040;
    auto read_guest = [&](uint32_t address) {
      uint32_t value = 0;
      std::memcpy(&value, memory.TranslatePhysical(address), sizeof(value));
      return value;
    };
    auto read_resident = [&](uint32_t address) {
      id<MTLBuffer> resident = (id<MTLBuffer>)shared_memory.buffer();
      uint32_t value = 0;
      std::memcpy(&value, reinterpret_cast<const uint8_t*>([resident contents]) + address,
                  sizeof(value));
      return value;
    };

    // Hold the queue before either completion command. The second enqueue must
    // allocate another staging buffer rather than overwrite the first buffer
    // while Metal can still read it.
    id<MTLSharedEvent> blocker_event = [device newSharedEvent];
    REQUIRE(blocker_event != nil);
    id<MTLCommandQueue> command_queue = (id<MTLCommandQueue>)shared_memory.command_queue();
    id<MTLCommandBuffer> blocker = [command_queue commandBuffer];
    REQUIRE(blocker != nil);
    [blocker encodeWaitForEvent:blocker_event value:1];
    [blocker commit];
    SharedEventSignalGuard blocker_signal_guard{blocker_event};

    constexpr uint32_t kFirstValue = 0x12345678;
    REQUIRE(shared_memory.EnqueueGpuOrderedGuestMemoryWrite(kFirstTargetAddress, &kFirstValue,
                                                            sizeof(kFirstValue)));
    constexpr uint32_t kSecondValue = 0x89ABCDEF;
    REQUIRE(shared_memory.EnqueueGpuOrderedGuestMemoryWrite(kSecondTargetAddress, &kSecondValue,
                                                            sizeof(kSecondValue)));

    auto blocked_stats = shared_memory.completion_staging_stats();
    CHECK(blocked_stats.allocations == 2);
    CHECK(blocked_stats.reuses == 0);
    CHECK(blocked_stats.idle_buffers == 0);
    CHECK(blocked_stats.in_flight_buffers == 2);
    CHECK(blocked_stats.peak_in_flight_buffers == 2);
    CHECK(read_guest(kFirstTargetAddress) != kFirstValue);
    CHECK(read_guest(kSecondTargetAddress) != kSecondValue);

    using CompletionWaitResult =
        rex::graphics::metal::MetalSharedMemory::OrderedGuestMemoryWriteWaitResult;
    CHECK(shared_memory.WaitForGpuOrderedGuestMemoryWrite(
              kSecondTargetAddress + 0x1000, sizeof(uint32_t), std::chrono::milliseconds(0)) ==
          CompletionWaitResult::kUnavailable);
    auto targeted_wait_started = std::chrono::steady_clock::now();
    CompletionWaitResult targeted_wait_result = shared_memory.WaitForGpuOrderedGuestMemoryWrite(
        kFirstTargetAddress, sizeof(uint32_t), std::chrono::milliseconds(1));
    auto targeted_wait_duration = std::chrono::steady_clock::now() - targeted_wait_started;
    // The command processor owns WAIT_REG_MEM's bounded retry loop. A queued
    // Metal write that is still blocked must be reported as pending promptly,
    // never waited here without a shutdown/deadline check.
    CHECK(targeted_wait_result == CompletionWaitResult::kPending);
    CHECK(targeted_wait_duration < std::chrono::milliseconds(250));

    // The completion handler must wake the bounded host wait without requiring
    // the command processor to poll Metal command-buffer status every 1 ms.
    std::thread unblocker([blocker_event]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      blocker_event.signaledValue = 1;
    });
    targeted_wait_started = std::chrono::steady_clock::now();
    targeted_wait_result = shared_memory.WaitForGpuOrderedGuestMemoryWrite(
        kFirstTargetAddress, sizeof(uint32_t), std::chrono::milliseconds(2000));
    targeted_wait_duration = std::chrono::steady_clock::now() - targeted_wait_started;
    unblocker.join();
    blocker_signal_guard.event = nil;
    CHECK(targeted_wait_result == CompletionWaitResult::kCompleted);
    CHECK(targeted_wait_duration < std::chrono::milliseconds(1000));
    auto wait_stats = shared_memory.completion_staging_stats();
    CHECK(wait_stats.completion_wait_unavailable == 1);
    CHECK(wait_stats.completion_wait_pending == 1);
    CHECK(wait_stats.completion_wait_completed == 1);
    // A heavily descheduled test thread may observe completion before entering
    // the event wait. Both zero and one consumed wake are correct in that race.
    CHECK(wait_stats.completion_event_wakeups <= 1);
    CHECK(wait_stats.completion_event_timeouts == 1);
    REQUIRE(shared_memory.WaitForPendingUploads());
    [blocker_event release];
    CHECK(read_guest(kFirstTargetAddress) == kFirstValue);
    CHECK(read_resident(kFirstTargetAddress) == kFirstValue);
    CHECK(read_guest(kSecondTargetAddress) == kSecondValue);
    CHECK(read_resident(kSecondTargetAddress) == kSecondValue);

    auto completed_stats = shared_memory.completion_staging_stats();
    CHECK(completed_stats.allocations == 2);
    CHECK(completed_stats.reuses == 0);
    CHECK(completed_stats.idle_buffers == 2);
    CHECK(completed_stats.in_flight_buffers == 0);

    constexpr uint32_t kReusedValue = 0x10203040;
    REQUIRE(shared_memory.EnqueueGpuOrderedGuestMemoryWrite(kFirstTargetAddress, &kReusedValue,
                                                            sizeof(kReusedValue)));
    REQUIRE(shared_memory.WaitForPendingUploads());
    CHECK(read_guest(kFirstTargetAddress) == kReusedValue);
    CHECK(read_resident(kFirstTargetAddress) == kReusedValue);

    auto reused_stats = shared_memory.completion_staging_stats();
    CHECK(reused_stats.allocations == 2);
    CHECK(reused_stats.reuses == 1);

    // A pre-commit failure may return the acquired slot immediately because no
    // GPU command can still read it.
    shared_memory.SetGpuResourceMutationCallback([]() { return false; });
    constexpr uint32_t kRejectedValue = 0xDEADBEEF;
    CHECK_FALSE(shared_memory.EnqueueGpuOrderedGuestMemoryWrite(
        kFirstTargetAddress, &kRejectedValue, sizeof(kRejectedValue)));
    shared_memory.SetGpuResourceMutationCallback({});
    auto rejected_stats = shared_memory.completion_staging_stats();
    CHECK(rejected_stats.allocations == 2);
    CHECK(rejected_stats.idle_buffers == 2);
    CHECK(rejected_stats.in_flight_buffers == 0);

    // Two writes to one word must retain command-queue order even when their
    // staging buffers have independent lifetimes.
    constexpr uint32_t kThirdValue = 0x0BADF00D;
    constexpr uint32_t kFinalValue = 0xC001D00D;
    REQUIRE(shared_memory.EnqueueGpuOrderedGuestMemoryWrite(kFirstTargetAddress, &kThirdValue,
                                                            sizeof(kThirdValue)));
    REQUIRE(shared_memory.EnqueueGpuOrderedGuestMemoryWrite(kFirstTargetAddress, &kFinalValue,
                                                            sizeof(kFinalValue)));
    REQUIRE(shared_memory.WaitForPendingUploads());
    CHECK(read_guest(kFirstTargetAddress) == kFinalValue);
    CHECK(read_resident(kFirstTargetAddress) == kFinalValue);

    // Shutdown is also a completion fence for an outstanding pooled slot.
    constexpr uint32_t kShutdownValue = 0x13579BDF;
    REQUIRE(shared_memory.EnqueueGpuOrderedGuestMemoryWrite(kFirstTargetAddress, &kShutdownValue,
                                                            sizeof(kShutdownValue)));
    shared_memory.Shutdown();
    CHECK(read_guest(kFirstTargetAddress) == kShutdownValue);
  }
}

TEST_CASE("Metal completion wait permanently falls back after a host event failure",
          "[graphics][metal][completion][integration]") {
  @autoreleasepool {
    rex::memory::Memory memory;
    REQUIRE(memory.Initialize());
    rex::graphics::TraceWriter trace_writer(memory.physical_membase());
    rex::graphics::metal::MetalSharedMemory shared_memory(memory, trace_writer);
    rex::graphics::metal::MetalSharedMemoryTestPeer::SetCompletionEvent(
        shared_memory, std::make_shared<UnsupportedWaitEvent>());

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);
    REQUIRE(shared_memory.Initialize((void*)device));

    id<MTLSharedEvent> blocker_event = [device newSharedEvent];
    REQUIRE(blocker_event != nil);
    id<MTLCommandQueue> command_queue = (id<MTLCommandQueue>)shared_memory.command_queue();
    id<MTLCommandBuffer> blocker = [command_queue commandBuffer];
    REQUIRE(blocker != nil);
    [blocker encodeWaitForEvent:blocker_event value:1];
    [blocker commit];
    SharedEventSignalGuard blocker_signal_guard{blocker_event};

    constexpr uint32_t kTargetAddress = 0x00160000;
    constexpr uint32_t kTargetValue = 0xA5A55A5A;
    REQUIRE(shared_memory.EnqueueGpuOrderedGuestMemoryWrite(kTargetAddress, &kTargetValue,
                                                            sizeof(kTargetValue)));

    using CompletionWaitResult =
        rex::graphics::metal::MetalSharedMemory::OrderedGuestMemoryWriteWaitResult;
    CHECK(shared_memory.WaitForGpuOrderedGuestMemoryWrite(kTargetAddress, sizeof(kTargetValue),
                                                          std::chrono::milliseconds(250)) ==
          CompletionWaitResult::kUnavailable);
    auto failed_stats = shared_memory.completion_staging_stats();
    CHECK(failed_stats.completion_event_failures == 1);
    CHECK(failed_stats.completion_wait_unavailable == 1);
    CHECK(failed_stats.completion_event_wakeups == 0);
    CHECK(failed_stats.completion_event_timeouts == 0);

    // The failed event is latched off. A second packet-side query must take the
    // unavailable fallback without retrying the broken host wait.
    CHECK(shared_memory.WaitForGpuOrderedGuestMemoryWrite(kTargetAddress, sizeof(kTargetValue),
                                                          std::chrono::milliseconds(250)) ==
          CompletionWaitResult::kUnavailable);
    auto fallback_stats = shared_memory.completion_staging_stats();
    CHECK(fallback_stats.completion_event_failures == 1);
    CHECK(fallback_stats.completion_wait_unavailable == 2);
    CHECK(fallback_stats.completion_event_wakeups == 0);
    CHECK(fallback_stats.completion_event_timeouts == 0);

    blocker_event.signaledValue = 1;
    blocker_signal_guard.event = nil;
    REQUIRE(shared_memory.WaitForPendingUploads());
    [blocker_event release];
  }
}

TEST_CASE("Metal guest CPU writes wait for older GPU alias writes",
          "[graphics][metal][shared-memory][integration]") {
  @autoreleasepool {
    rex::memory::Memory memory;
    REQUIRE(memory.Initialize());
    rex::graphics::TraceWriter trace_writer(memory.physical_membase());
    rex::graphics::metal::MetalSharedMemory shared_memory(memory, trace_writer);

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);
    REQUIRE(shared_memory.Initialize((void*)device));

    constexpr uint32_t kTargetAddress = 0x00180000;
    constexpr uint32_t kOlderGpuValue = 0x12345678;
    constexpr uint32_t kNewerCpuValue = 0xA1B2C3D4;
    auto read_guest = [&]() {
      uint32_t value = 0;
      std::memcpy(&value, memory.TranslatePhysical(kTargetAddress), sizeof(value));
      return value;
    };
    auto read_resident = [&]() {
      id<MTLBuffer> resident = (id<MTLBuffer>)shared_memory.buffer();
      uint32_t value = 0;
      std::memcpy(&value, reinterpret_cast<const uint8_t*>([resident contents]) + kTargetAddress,
                  sizeof(value));
      return value;
    };

    // Hold an older GPU write to both the resident buffer and its no-copy
    // guest alias. The CPU mutation must not begin until this command wins its
    // place in queue order.
    id<MTLSharedEvent> blocker_event = [device newSharedEvent];
    REQUIRE(blocker_event != nil);
    id<MTLCommandQueue> command_queue = (id<MTLCommandQueue>)shared_memory.command_queue();
    id<MTLCommandBuffer> blocker = [command_queue commandBuffer];
    REQUIRE(blocker != nil);
    [blocker encodeWaitForEvent:blocker_event value:1];
    [blocker commit];
    SharedEventSignalGuard blocker_signal_guard{blocker_event};
    REQUIRE(shared_memory.EnqueueGpuOrderedGuestMemoryWrite(kTargetAddress, &kOlderGpuValue,
                                                            sizeof(kOlderGpuValue)));

    std::atomic<uint32_t> synchronization_call_count = 0;
    std::atomic<bool> synchronization_entered = false;
    std::atomic<bool> writer_entered = false;
    shared_memory.SetHostResourceMutationCallback([&]() {
      synchronization_call_count.fetch_add(1, std::memory_order_relaxed);
      synchronization_entered.store(true, std::memory_order_release);
      return true;
    });
    GpuPublicationProbe publication_probe;
    auto publication_watch =
        shared_memory.RegisterGlobalWatch(GpuPublicationProbe::Callback, &publication_probe);
    REQUIRE(publication_watch != nullptr);

    bool transaction_succeeded = false;
    uint32_t value_seen_by_cpu_writer = 0;
    uint32_t publications_seen_by_cpu_writer = 0;
    std::thread cpu_writer([&]() {
      transaction_succeeded = shared_memory.CommitSynchronizedGuestCpuWriteAsGpu(
          kTargetAddress, sizeof(kNewerCpuValue), [&]() {
            writer_entered.store(true, std::memory_order_release);
            publications_seen_by_cpu_writer =
                publication_probe.count.load(std::memory_order_acquire);
            value_seen_by_cpu_writer = read_guest();
            std::memcpy(memory.TranslatePhysical(kTargetAddress), &kNewerCpuValue,
                        sizeof(kNewerCpuValue));
            return true;
          });
    });

    while (!synchronization_entered.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    CHECK_FALSE(writer_entered.load(std::memory_order_acquire));

    blocker_event.signaledValue = 1;
    blocker_signal_guard.event = nil;
    cpu_writer.join();
    shared_memory.SetHostResourceMutationCallback({});
    shared_memory.UnregisterGlobalWatch(publication_watch);
    [blocker_event release];

    REQUIRE(transaction_succeeded);
    CHECK(writer_entered.load(std::memory_order_acquire));
    CHECK(synchronization_call_count.load(std::memory_order_relaxed) == 1);
    CHECK(publications_seen_by_cpu_writer == 1);
    CHECK(publication_probe.count.load(std::memory_order_acquire) == 1);
    CHECK(value_seen_by_cpu_writer == kOlderGpuValue);
    CHECK(read_guest() == kNewerCpuValue);
    CHECK(read_resident() == kNewerCpuValue);
  }
}

TEST_CASE("Shared-memory global watches distinguish CPU fence changes from GPU invalidations",
          "[graphics][metal][shared-memory][integration]") {
  @autoreleasepool {
    rex::memory::Memory memory;
    REQUIRE(memory.Initialize());
    rex::graphics::TraceWriter trace_writer(memory.physical_membase());
    rex::graphics::metal::MetalSharedMemory shared_memory(memory, trace_writer);

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    REQUIRE(device != nil);
    REQUIRE(shared_memory.Initialize((void*)device));

    CpuInvalidationProbe probe;
    REQUIRE(probe.event != nullptr);
    auto watch = shared_memory.RegisterGlobalWatch(CpuInvalidationProbe::Callback, &probe);
    REQUIRE(watch != nullptr);

    constexpr uint32_t kTargetAddress = 0x00180004;
    probe.Arm(kTargetAddress, sizeof(uint32_t));

    shared_memory.RangeWrittenByGpu(kTargetAddress, sizeof(uint32_t));
    CHECK(rex::thread::Wait(probe.event.get(), false, std::chrono::milliseconds(0)) ==
          rex::thread::WaitResult::kTimeout);

    shared_memory.MemoryInvalidationCallback(kTargetAddress + 0x4000, sizeof(uint32_t), true);
    CHECK(rex::thread::Wait(probe.event.get(), false, std::chrono::milliseconds(0)) ==
          rex::thread::WaitResult::kTimeout);

    shared_memory.MemoryInvalidationCallback(kTargetAddress, sizeof(uint32_t), true);
    CHECK(rex::thread::Wait(probe.event.get(), false, std::chrono::milliseconds(10)) ==
          rex::thread::WaitResult::kSuccess);

    shared_memory.UnregisterGlobalWatch(watch);
    shared_memory.Shutdown();
  }
}
