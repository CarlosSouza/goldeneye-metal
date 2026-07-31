/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <memory>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/trace_player.h>
#include <rex/graphics/xenos.h>
#include <rex/memory.h>

#include <snappy.h>

namespace rex::graphics {

namespace {

template <typename T>
bool ReadTracePod(const uint8_t*& trace_ptr, const uint8_t* trace_end, T& value_out) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (trace_ptr > trace_end || size_t(trace_end - trace_ptr) < sizeof(T)) {
    return false;
  }
  std::memcpy(&value_out, trace_ptr, sizeof(T));
  trace_ptr += sizeof(T);
  return true;
}

bool SkipTraceBytes(const uint8_t*& trace_ptr, const uint8_t* trace_end, size_t byte_count) {
  if (trace_ptr > trace_end || byte_count > size_t(trace_end - trace_ptr)) {
    return false;
  }
  trace_ptr += byte_count;
  return true;
}

bool GetArrayByteCount(uint32_t element_count, size_t element_size, size_t& byte_count_out) {
  if (element_size && size_t(element_count) > std::numeric_limits<size_t>::max() / element_size) {
    return false;
  }
  byte_count_out = size_t(element_count) * element_size;
  return true;
}

bool IsPhysicalRangeValid(uint32_t base_ptr, size_t byte_count) {
  constexpr uint64_t kPhysicalAddressMask = 0x1FFFFFFFu;
  constexpr uint64_t kPhysicalAddressSize = kPhysicalAddressMask + 1;
  uint64_t physical_offset = uint64_t(base_ptr) & kPhysicalAddressMask;
  return uint64_t(byte_count) <= kPhysicalAddressSize - physical_offset;
}

bool ValidateEncodedPayload(MemoryEncodingFormat encoding_format, const uint8_t* payload,
                            size_t encoded_length, size_t decoded_length) {
  switch (encoding_format) {
    case MemoryEncodingFormat::kNone:
      return encoded_length == decoded_length;
    case MemoryEncodingFormat::kSnappy: {
      size_t actual_decoded_length = 0;
      return snappy::GetUncompressedLength(reinterpret_cast<const char*>(payload), encoded_length,
                                           &actual_decoded_length) &&
             actual_decoded_length == decoded_length &&
             snappy::IsValidCompressedBuffer(reinterpret_cast<const char*>(payload),
                                             encoded_length);
    }
    default:
      return false;
  }
}

}  // namespace

TracePlayer::TracePlayer(GraphicsSystem* graphics_system)
    : graphics_system_(graphics_system), current_frame_index_(0), current_command_index_(-1) {
  // Need to allocate all of physical memory so that we can write to it during
  // playback. The 64 KB page heap is larger, covers the entire physical memory,
  // so it is used instead of the 4 KB page one.
  auto heap = graphics_system_->memory()->LookupHeapByType(true, 64 * 1024);
  heap->AllocFixed(heap->heap_base(), heap->heap_size(), heap->page_size(),
                   memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit,
                   memory::kMemoryProtectRead | memory::kMemoryProtectWrite);

  playback_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(playback_event_);
}

const TraceReader::Frame* TracePlayer::current_frame() const {
  if (current_frame_index_ < 0 || current_frame_index_ >= frame_count()) {
    return nullptr;
  }
  return frame(current_frame_index_);
}

bool TracePlayer::SeekFrame(int target_frame) {
  if (target_frame < 0 || target_frame >= frame_count() || is_playing_trace()) {
    return false;
  }
  if (current_frame_index_ == target_frame && current_command_index_ != -1) {
    return true;
  }
  const int previous_frame_index = current_frame_index_;
  const int previous_command_index = current_command_index_;
  current_frame_index_ = target_frame;
  auto frame = current_frame();
  current_command_index_ = int(frame->commands.size()) - 1;

  const auto* first_frame = frame_count() ? this->frame(0) : nullptr;
  if (!first_frame) {
    current_frame_index_ = previous_frame_index;
    current_command_index_ = previous_command_index;
    return false;
  }
  // Metal doesn't have a private render-target snapshot to restore for an
  // arbitrary frame. Rebuild the GPU state by replaying the trace from its
  // first completed frame through the requested frame.
  assert_true(first_frame->start_ptr <= frame->end_ptr);
  assert_true(frame->start_ptr <= frame->end_ptr);
  if (!PlayTrace(first_frame->start_ptr, frame->end_ptr - first_frame->start_ptr,
                 TracePlaybackMode::kUntilEnd, true)) {
    current_frame_index_ = previous_frame_index;
    current_command_index_ = previous_command_index;
    return false;
  }
  return true;
}

bool TracePlayer::SeekCommand(int target_command) {
  auto frame = current_frame();
  if (!frame || target_command < -1 || target_command >= int(frame->commands.size()) ||
      is_playing_trace()) {
    return false;
  }
  if (current_command_index_ == target_command) {
    return true;
  }
  int previous_command_index = current_command_index_;
  current_command_index_ = target_command;
  if (current_command_index_ == -1) {
    return true;
  }
  const auto& command = frame->commands[target_command];
  assert_true(frame->start_ptr <= command.end_ptr);
  bool playback_started = false;
  if (previous_command_index != -1 && target_command > previous_command_index) {
    // Seek forward.
    const auto& previous_command = frame->commands[previous_command_index];
    playback_started =
        PlayTrace(previous_command.end_ptr, command.end_ptr - previous_command.end_ptr,
                  TracePlaybackMode::kBreakOnSwap, false);
  } else {
    // Full playback from the trace's first frame. This is required for Metal,
    // where a later frame's render-target state can't be restored in isolation.
    const auto* first_frame = frame_count() ? this->frame(0) : nullptr;
    if (first_frame && first_frame->start_ptr <= command.end_ptr) {
      playback_started = PlayTrace(first_frame->start_ptr, command.end_ptr - first_frame->start_ptr,
                                   TracePlaybackMode::kUntilEnd, true);
    }
  }
  if (!playback_started) {
    current_command_index_ = previous_command_index;
    return false;
  }
  return true;
}

bool TracePlayer::WaitOnPlayback() {
  if (!is_playing_trace()) {
    return playback_succeeded_.load(std::memory_order_acquire);
  }
  rex::thread::Wait(playback_event_.get(), true);
  return playback_succeeded_.load(std::memory_order_acquire);
}

bool TracePlayer::PlayTrace(const uint8_t* trace_data, size_t trace_size,
                            TracePlaybackMode playback_mode, bool clear_caches) {
  bool expected = false;
  if (!playing_trace_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return false;
  }
  playback_succeeded_.store(false, std::memory_order_release);
  playback_event_->Reset();
  CommandProcessor* command_processor = graphics_system_->command_processor();
  if (!command_processor || !command_processor->CallInThread([this, trace_data, trace_size,
                                                              playback_mode, clear_caches]() {
        PlayTraceOnThread(trace_data, trace_size, playback_mode, clear_caches);
      })) {
    FinishPlayback(false);
    return false;
  }
  return true;
}

void TracePlayer::FinishPlayback(bool succeeded) {
  playback_succeeded_.store(succeeded, std::memory_order_release);
  playing_trace_.store(false, std::memory_order_release);
  playback_event_->Set();
}

void TracePlayer::PlayTraceOnThread(const uint8_t* trace_data, size_t trace_size,
                                    TracePlaybackMode playback_mode, bool clear_caches) {
  auto memory = graphics_system_->memory();
  auto command_processor = graphics_system_->command_processor();
  uintptr_t mapped_begin = reinterpret_cast<uintptr_t>(trace_data_);
  uintptr_t mapped_data = reinterpret_cast<uintptr_t>(trace_data);
  if (!memory || !command_processor || !trace_data_ || !trace_data || mapped_data < mapped_begin ||
      mapped_data - mapped_begin > trace_size_ ||
      trace_size > trace_size_ - size_t(mapped_data - mapped_begin)) {
    REXGPU_ERROR("Trace playback received an invalid mapped range");
    FinishPlayback(false);
    return;
  }

  if (clear_caches) {
    command_processor->ClearCaches();
  }

  playback_percent_ = 0;
  auto trace_end = trace_data + trace_size;

  auto trace_ptr = trace_data;
  PacketStartCommand pending_packet = {};
  const uint8_t* pending_packet_start = nullptr;
  auto fail = [&](const uint8_t* failure_ptr, std::string_view reason) {
    size_t failure_offset = failure_ptr >= trace_data && failure_ptr <= trace_end
                                ? size_t(failure_ptr - trace_data)
                                : 0;
    REXGPU_ERROR("Trace playback failed at range byte {}: {}", failure_offset, reason);
    FinishPlayback(false);
  };

  while (trace_ptr < trace_end) {
    playback_percent_ =
        uint32_t((float(trace_ptr - trace_data) / float(trace_end - trace_data)) * 10000);

    const uint8_t* command_start = trace_ptr;
    TraceCommandType type;
    if (!ReadTracePod(trace_ptr, trace_end, type)) {
      fail(command_start, "truncated command type");
      return;
    }
    switch (type) {
      case TraceCommandType::kPrimaryBufferStart: {
        PrimaryBufferStartCommand cmd;
        trace_ptr = command_start;
        size_t packet_bytes = 0;
        if (!ReadTracePod(trace_ptr, trace_end, cmd) ||
            !GetArrayByteCount(cmd.count, sizeof(uint32_t), packet_bytes) ||
            !SkipTraceBytes(trace_ptr, trace_end, packet_bytes)) {
          fail(command_start, "truncated primary-buffer command");
          return;
        }
        break;
      }
      case TraceCommandType::kPrimaryBufferEnd: {
        PrimaryBufferEndCommand cmd;
        trace_ptr = command_start;
        if (!ReadTracePod(trace_ptr, trace_end, cmd)) {
          fail(command_start, "truncated primary-buffer end");
          return;
        }
        break;
      }
      case TraceCommandType::kIndirectBufferStart: {
        IndirectBufferStartCommand cmd;
        trace_ptr = command_start;
        size_t packet_bytes = 0;
        if (!ReadTracePod(trace_ptr, trace_end, cmd) ||
            !GetArrayByteCount(cmd.count, sizeof(uint32_t), packet_bytes) ||
            !SkipTraceBytes(trace_ptr, trace_end, packet_bytes)) {
          fail(command_start, "truncated indirect-buffer command");
          return;
        }
        break;
      }
      case TraceCommandType::kIndirectBufferEnd: {
        IndirectBufferEndCommand cmd;
        trace_ptr = command_start;
        if (!ReadTracePod(trace_ptr, trace_end, cmd)) {
          fail(command_start, "truncated indirect-buffer end");
          return;
        }
        break;
      }
      case TraceCommandType::kPacketStart: {
        PacketStartCommand cmd;
        trace_ptr = command_start;
        size_t packet_bytes = 0;
        if (!ReadTracePod(trace_ptr, trace_end, cmd) || !cmd.count ||
            !GetArrayByteCount(cmd.count, sizeof(uint32_t), packet_bytes) ||
            !IsPhysicalRangeValid(cmd.base_ptr, packet_bytes) ||
            size_t(trace_end - trace_ptr) < packet_bytes) {
          fail(command_start, "invalid or truncated packet");
          return;
        }
        std::memcpy(memory->TranslatePhysical(cmd.base_ptr), trace_ptr, packet_bytes);
        trace_ptr += packet_bytes;
        pending_packet = cmd;
        pending_packet_start = command_start;
        break;
      }
      case TraceCommandType::kPacketEnd: {
        PacketEndCommand cmd;
        trace_ptr = command_start;
        if (!ReadTracePod(trace_ptr, trace_end, cmd)) {
          fail(command_start, "truncated packet end");
          return;
        }
        if (pending_packet_start) {
          if (!command_processor->ExecutePacketForTrace(
                  pending_packet.base_ptr, pending_packet.count)) {
            fail(pending_packet_start, "GPU packet execution failed");
            return;
          }
          pending_packet_start = nullptr;
        }
        break;
      }
      case TraceCommandType::kMemoryRead: {
        MemoryCommand cmd;
        trace_ptr = command_start;
        if (!ReadTracePod(trace_ptr, trace_end, cmd) ||
            !IsPhysicalRangeValid(cmd.base_ptr, cmd.decoded_length) ||
            size_t(cmd.encoded_length) > size_t(trace_end - trace_ptr) ||
            !DecompressMemory(cmd.encoding_format, trace_ptr, cmd.encoded_length,
                              memory->TranslatePhysical(cmd.base_ptr), cmd.decoded_length) ||
            !SkipTraceBytes(trace_ptr, trace_end, cmd.encoded_length)) {
          fail(command_start, "invalid memory-read payload");
          return;
        }
        command_processor->TracePlaybackWroteMemory(cmd.base_ptr, cmd.decoded_length);
        break;
      }
      case TraceCommandType::kMemoryWrite: {
        MemoryCommand cmd;
        trace_ptr = command_start;
        if (!ReadTracePod(trace_ptr, trace_end, cmd) ||
            !IsPhysicalRangeValid(cmd.base_ptr, cmd.decoded_length) ||
            size_t(cmd.encoded_length) > size_t(trace_end - trace_ptr)) {
          fail(command_start, "invalid memory-write payload");
          return;
        }
        // ?
        // Assuming the command processor will do the same write.
        if (!ValidateEncodedPayload(cmd.encoding_format, trace_ptr, cmd.encoded_length,
                                    cmd.decoded_length) ||
            !SkipTraceBytes(trace_ptr, trace_end, cmd.encoded_length)) {
          fail(command_start, "invalid memory-write payload");
          return;
        }
        break;
      }
      case TraceCommandType::kEdramSnapshot: {
        EdramSnapshotCommand cmd;
        trace_ptr = command_start;
        if (!ReadTracePod(trace_ptr, trace_end, cmd) ||
            size_t(cmd.encoded_length) > size_t(trace_end - trace_ptr)) {
          fail(command_start, "invalid EDRAM snapshot payload");
          return;
        }
        std::unique_ptr<uint8_t[]> edram_snapshot(new uint8_t[xenos::kEdramSizeBytes]);
        if (!DecompressMemory(cmd.encoding_format, trace_ptr, cmd.encoded_length,
                              edram_snapshot.get(), xenos::kEdramSizeBytes) ||
            !SkipTraceBytes(trace_ptr, trace_end, cmd.encoded_length)) {
          fail(command_start, "invalid EDRAM snapshot payload");
          return;
        }
        if (!command_processor->RestoreEdramSnapshot(edram_snapshot.get())) {
          fail(command_start, "backend rejected canonical EDRAM snapshot");
          return;
        }
        break;
      }
      case TraceCommandType::kEvent: {
        EventCommand cmd;
        trace_ptr = command_start;
        if (!ReadTracePod(trace_ptr, trace_end, cmd)) {
          fail(command_start, "truncated event");
          return;
        }
        switch (cmd.event_type) {
          case EventCommand::Type::kSwap: {
            if (pending_packet_start) {
              fail(command_start, "swap event occurs before its packet end");
              return;
            }
            if (playback_mode == TracePlaybackMode::kBreakOnSwap) {
              playback_percent_.store(10000, std::memory_order_release);
              FinishPlayback(true);
              return;
            }
            break;
          }
          default:
            fail(command_start, "unknown event type");
            return;
        }
        break;
      }
      case TraceCommandType::kRegisters: {
        RegistersCommand cmd;
        trace_ptr = command_start;
        size_t register_bytes = 0;
        if (!ReadTracePod(trace_ptr, trace_end, cmd) ||
            cmd.first_register > RegisterFile::kRegisterCount ||
            cmd.register_count > RegisterFile::kRegisterCount - cmd.first_register ||
            !GetArrayByteCount(cmd.register_count, sizeof(uint32_t), register_bytes) ||
            size_t(cmd.encoded_length) > size_t(trace_end - trace_ptr)) {
          fail(command_start, "invalid register snapshot payload");
          return;
        }
        std::unique_ptr<uint32_t[]> register_values(
            cmd.register_count ? new uint32_t[cmd.register_count] : nullptr);
        if (!DecompressMemory(cmd.encoding_format, trace_ptr, cmd.encoded_length,
                              register_values.get(), register_bytes) ||
            !SkipTraceBytes(trace_ptr, trace_end, cmd.encoded_length)) {
          fail(command_start, "invalid register snapshot payload");
          return;
        }
        if (cmd.register_count) {
          command_processor->RestoreRegisters(cmd.first_register, register_values.get(),
                                              cmd.register_count, cmd.execute_callbacks);
        }
        break;
      }
      case TraceCommandType::kGammaRamp: {
        GammaRampCommand cmd;
        trace_ptr = command_start;
        if (!ReadTracePod(trace_ptr, trace_end, cmd) || cmd.rw_component >= 3 ||
            size_t(cmd.encoded_length) > size_t(trace_end - trace_ptr)) {
          fail(command_start, "invalid gamma-ramp payload");
          return;
        }
        std::unique_ptr<uint32_t[]> gamma_ramps(new uint32_t[256 + 3 * 128]);
        if (!DecompressMemory(cmd.encoding_format, trace_ptr, cmd.encoded_length, gamma_ramps.get(),
                              sizeof(uint32_t) * (256 + 3 * 128)) ||
            !SkipTraceBytes(trace_ptr, trace_end, cmd.encoded_length)) {
          fail(command_start, "invalid gamma-ramp payload");
          return;
        }
        command_processor->RestoreGammaRamp(
            reinterpret_cast<const reg::DC_LUT_30_COLOR*>(gamma_ramps.get()),
            reinterpret_cast<const reg::DC_LUT_PWL_DATA*>(gamma_ramps.get() + 256),
            cmd.rw_component);
        break;
      }
      default:
        fail(command_start, "unknown command type");
        return;
    }
    if (trace_ptr <= command_start) {
      fail(command_start, "command parser made no progress");
      return;
    }
  }

  if (pending_packet_start) {
    fail(pending_packet_start, "trace range ends inside a packet");
    return;
  }
  playback_percent_.store(10000, std::memory_order_release);
  FinishPlayback(true);
}

}  // namespace rex::graphics
