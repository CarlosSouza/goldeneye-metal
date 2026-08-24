/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cinttypes>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

#include <rex/filesystem.h>
#include <rex/graphics/packet_disassembler.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/trace_protocol.h>
#include <rex/graphics/trace_reader.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/memory/mapped_memory.h>
#include <rex/platform.h>

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

bool TraceReader::Open(const std::string_view path) {
  Close();

  mmap_.reset();
#if REX_PLATFORM_ANDROID
  if (rex::filesystem::IsAndroidContentUri(path)) {
    mmap_ = memory::MappedMemory::OpenForAndroidContentUri(path, memory::MappedMemory::Mode::kRead);
  }
#endif  // REX_PLATFORM_ANDROID
  if (!mmap_) {
    mmap_ = memory::MappedMemory::Open(rex::to_path(path), memory::MappedMemory::Mode::kRead);
  }
  if (!mmap_) {
    return false;
  }

  trace_data_ = reinterpret_cast<const uint8_t*>(mmap_->data());
  trace_size_ = mmap_->size();
  if (!trace_data_ || trace_size_ < sizeof(TraceHeader)) {
    REXGPU_ERROR("Trace is truncated before its header");
    Close();
    return false;
  }

  // Verify version.
  TraceHeader header;
  std::memcpy(&header, trace_data_, sizeof(header));
  if (header.version != kTraceFormatVersion) {
    REXGPU_ERROR("Trace format version mismatch, code has {}, file has {}", kTraceFormatVersion,
                 header.version);
    if (header.version < kTraceFormatVersion) {
      REXGPU_ERROR("You need to regenerate your trace for the latest version");
    }
    Close();
    return false;
  }
  if (!IsTraceEdramRequirementsManifestValid(header.edram_manifest)) {
    REXGPU_ERROR("Trace has an invalid EDRAM requirements manifest");
    Close();
    return false;
  }

  REXGPU_INFO("Mapped {}b trace from {}", trace_size_, rex::path_to_utf8(path));
  REXGPU_INFO("   Version: {}", header.version);
  size_t commit_length = 0;
  while (commit_length < rex::countof(header.build_commit_sha) &&
         header.build_commit_sha[commit_length]) {
    ++commit_length;
  }
  const std::string commit_str(header.build_commit_sha, commit_length);
  REXGPU_INFO("    Commit: {}", commit_str);
  REXGPU_INFO("  Title ID: {}", header.title_id);
  REXGPU_INFO("     EDRAM: {}{} (color=0x{:04X}, depth=0x{:02X}, MSAA=0x{:02X})",
              (header.edram_manifest.flags & kTraceEdramManifestFlagFinalized) ? "finalized"
                                                                              : "unfinalized",
              (header.edram_manifest.flags & kTraceEdramManifestFlagRequirementsTracked)
                  ? ", tracked"
                  : ", unknown",
              header.edram_manifest.requirements.color_format_mask,
              header.edram_manifest.requirements.depth_format_mask,
              header.edram_manifest.requirements.msaa_samples_mask);

  if (!ParseTrace()) {
    Close();
    return false;
  }

  return true;
}

void TraceReader::Close() {
  frames_.clear();
  has_initial_edram_snapshot_ = false;
  mmap_.reset();
  trace_data_ = nullptr;
  trace_size_ = 0;
}

bool TraceReader::ParseTrace() {
  frames_.clear();
  has_initial_edram_snapshot_ = false;
  if (!trace_data_ || trace_size_ < sizeof(TraceHeader)) {
    return false;
  }

  // Skip file header.
  const uint8_t* trace_ptr = trace_data_ + sizeof(TraceHeader);
  const uint8_t* trace_end = trace_data_ + trace_size_;

  Frame current_frame;
  current_frame.start_ptr = trace_ptr;
  const uint8_t* packet_start_ptr = nullptr;
  uint32_t packet_start_count = 0;
  bool saw_gpu_packet = false;
  const uint8_t* last_ptr = trace_ptr;
  auto current_command_buffer = new CommandBuffer();
  current_frame.command_tree = std::unique_ptr<CommandBuffer>(current_command_buffer);

  auto fail = [&](const uint8_t* failure_ptr, std::string_view reason) {
    size_t failure_offset = failure_ptr >= trace_data_ && failure_ptr <= trace_end
                                ? size_t(failure_ptr - trace_data_)
                                : trace_size_;
    REXGPU_ERROR("Invalid trace at byte {}: {}", failure_offset, reason);
    frames_.clear();
    has_initial_edram_snapshot_ = false;
    return false;
  };

  auto start_next_frame = [&]() {
    current_frame = Frame();
    current_frame.start_ptr = trace_ptr;
    current_command_buffer = new CommandBuffer();
    current_frame.command_tree = std::unique_ptr<CommandBuffer>(current_command_buffer);
    last_ptr = trace_ptr;
    packet_start_ptr = nullptr;
    packet_start_count = 0;
  };

  while (trace_ptr < trace_end) {
    const uint8_t* command_start = trace_ptr;
    TraceCommandType type;
    if (!ReadTracePod(trace_ptr, trace_end, type)) {
      return fail(command_start, "truncated command type");
    }
    if (current_frame.command_count == std::numeric_limits<int>::max()) {
      return fail(command_start, "too many commands in one frame");
    }
    ++current_frame.command_count;
    switch (type) {
      case TraceCommandType::kPrimaryBufferStart: {
        PrimaryBufferStartCommand cmd;
        trace_ptr = command_start;
        size_t packet_bytes = 0;
        if (!ReadTracePod(trace_ptr, trace_end, cmd) ||
            !GetArrayByteCount(cmd.count, sizeof(uint32_t), packet_bytes) ||
            !SkipTraceBytes(trace_ptr, trace_end, packet_bytes)) {
          return fail(command_start, "truncated primary-buffer command");
        }
        break;
      }
      case TraceCommandType::kPrimaryBufferEnd: {
        PrimaryBufferEndCommand cmd;
        trace_ptr = command_start;
        if (!ReadTracePod(trace_ptr, trace_end, cmd)) {
          return fail(command_start, "truncated primary-buffer end");
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
          return fail(command_start, "truncated indirect-buffer command");
        }

        // Traverse down a level.
        auto sub_command_buffer = new CommandBuffer();
        sub_command_buffer->parent = current_command_buffer;
        current_command_buffer->commands.push_back(CommandBuffer::Command(sub_command_buffer));
        current_command_buffer = sub_command_buffer;
        break;
      }
      case TraceCommandType::kIndirectBufferEnd: {
        IndirectBufferEndCommand cmd;
        trace_ptr = command_start;
        if (!ReadTracePod(trace_ptr, trace_end, cmd)) {
          return fail(command_start, "truncated indirect-buffer end");
        }

        // IB packet is wrapped in a kPacketStart/kPacketEnd. Skip the end.
        PacketEndCommand end_cmd;
        if (!ReadTracePod(trace_ptr, trace_end, end_cmd) ||
            end_cmd.type != TraceCommandType::kPacketEnd) {
          return fail(command_start, "indirect-buffer end is missing its packet end");
        }
        packet_start_ptr = nullptr;
        packet_start_count = 0;

        // Go back up a level. If parent is null, this frame started in an
        // indirect buffer.
        if (current_command_buffer->parent) {
          current_command_buffer = current_command_buffer->parent;
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
            !SkipTraceBytes(trace_ptr, trace_end, packet_bytes)) {
          return fail(command_start, "invalid or truncated packet");
        }
        saw_gpu_packet = true;
        packet_start_ptr = command_start;
        packet_start_count = cmd.count;
        break;
      }
      case TraceCommandType::kPacketEnd: {
        PacketEndCommand cmd;
        trace_ptr = command_start;
        if (!ReadTracePod(trace_ptr, trace_end, cmd)) {
          return fail(command_start, "truncated packet end");
        }
        if (!packet_start_ptr) {
          break;
        }
        if (!packet_start_count) {
          return fail(command_start, "packet end has an invalid packet start");
        }
        auto packet_category =
            PacketDisassembler::GetPacketCategory(packet_start_ptr + sizeof(PacketStartCommand));
        switch (packet_category) {
          case PacketCategory::kDraw: {
            Frame::Command command;
            command.type = Frame::Command::Type::kDraw;
            command.head_ptr = packet_start_ptr;
            command.start_ptr = last_ptr;
            command.end_ptr = trace_ptr;
            current_frame.commands.push_back(std::move(command));
            last_ptr = trace_ptr;
            current_command_buffer->commands.push_back(
                CommandBuffer::Command(uint32_t(current_frame.commands.size() - 1)));
            break;
          }
          case PacketCategory::kSwap: {
            Frame::Command command;
            command.type = Frame::Command::Type::kSwap;
            command.head_ptr = packet_start_ptr;
            command.start_ptr = last_ptr;
            command.end_ptr = trace_ptr;
            current_frame.commands.push_back(std::move(command));
            last_ptr = trace_ptr;
            current_command_buffer->commands.push_back(
                CommandBuffer::Command(uint32_t(current_frame.commands.size() - 1)));
          } break;
          case PacketCategory::kGeneric: {
            // Ignored.
            break;
          }
        }
        packet_start_ptr = nullptr;
        packet_start_count = 0;
        break;
      }
      case TraceCommandType::kMemoryRead: {
        MemoryCommand cmd;
        trace_ptr = command_start;
        if (!ReadTracePod(trace_ptr, trace_end, cmd) ||
            !IsPhysicalRangeValid(cmd.base_ptr, cmd.decoded_length) ||
            size_t(cmd.encoded_length) > size_t(trace_end - trace_ptr) ||
            !ValidateEncodedPayload(cmd.encoding_format, trace_ptr, cmd.encoded_length,
                                    cmd.decoded_length) ||
            !SkipTraceBytes(trace_ptr, trace_end, cmd.encoded_length)) {
          return fail(command_start, "invalid memory-read payload");
        }
        break;
      }
      case TraceCommandType::kMemoryWrite: {
        MemoryCommand cmd;
        trace_ptr = command_start;
        if (!ReadTracePod(trace_ptr, trace_end, cmd) ||
            !IsPhysicalRangeValid(cmd.base_ptr, cmd.decoded_length) ||
            size_t(cmd.encoded_length) > size_t(trace_end - trace_ptr) ||
            !ValidateEncodedPayload(cmd.encoding_format, trace_ptr, cmd.encoded_length,
                                    cmd.decoded_length) ||
            !SkipTraceBytes(trace_ptr, trace_end, cmd.encoded_length)) {
          return fail(command_start, "invalid memory-write payload");
        }
        break;
      }
      case TraceCommandType::kEdramSnapshot: {
        EdramSnapshotCommand cmd;
        trace_ptr = command_start;
        if (!ReadTracePod(trace_ptr, trace_end, cmd) ||
            size_t(cmd.encoded_length) > size_t(trace_end - trace_ptr) ||
            !ValidateEncodedPayload(cmd.encoding_format, trace_ptr, cmd.encoded_length,
                                    xenos::kEdramSizeBytes) ||
            !SkipTraceBytes(trace_ptr, trace_end, cmd.encoded_length)) {
          return fail(command_start, "invalid EDRAM snapshot payload");
        }
        // Trace initialization writes the EDRAM snapshot before command
        // packets. A later snapshot can't retroactively initialize packets
        // that standalone playback has already consumed.
        if (!saw_gpu_packet && frames_.empty()) {
          has_initial_edram_snapshot_ = true;
        }
        break;
      }
      case TraceCommandType::kEvent: {
        EventCommand cmd;
        trace_ptr = command_start;
        if (!ReadTracePod(trace_ptr, trace_end, cmd)) {
          return fail(command_start, "truncated event");
        }
        switch (cmd.event_type) {
          case EventCommand::Type::kSwap: {
            if (packet_start_ptr) {
              return fail(command_start, "swap event occurs before its packet end");
            }
            // The writer emits the swap event after the swap packet's
            // PacketEnd. End the frame here so the first packet after the
            // event belongs exclusively to the next frame.
            current_frame.end_ptr = trace_ptr;
            frames_.push_back(std::move(current_frame));
            start_next_frame();
            break;
          }
          default:
            return fail(command_start, "unknown event type");
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
            size_t(cmd.encoded_length) > size_t(trace_end - trace_ptr) ||
            !ValidateEncodedPayload(cmd.encoding_format, trace_ptr, cmd.encoded_length,
                                    register_bytes) ||
            !SkipTraceBytes(trace_ptr, trace_end, cmd.encoded_length)) {
          return fail(command_start, "invalid register snapshot payload");
        }
        break;
      }
      case TraceCommandType::kGammaRamp: {
        GammaRampCommand cmd;
        trace_ptr = command_start;
        constexpr size_t kGammaRampByteCount = sizeof(uint32_t) * (256 + 3 * 128);
        if (!ReadTracePod(trace_ptr, trace_end, cmd) || cmd.rw_component >= 3 ||
            size_t(cmd.encoded_length) > size_t(trace_end - trace_ptr) ||
            !ValidateEncodedPayload(cmd.encoding_format, trace_ptr, cmd.encoded_length,
                                    kGammaRampByteCount) ||
            !SkipTraceBytes(trace_ptr, trace_end, cmd.encoded_length)) {
          return fail(command_start, "invalid gamma-ramp payload");
        }
        break;
      }
      default:
        return fail(command_start, "unknown command type");
    }
    if (trace_ptr <= command_start) {
      return fail(command_start, "command parser made no progress");
    }
  }
  if (packet_start_ptr || current_frame.command_count) {
    size_t tail_offset = size_t(current_frame.start_ptr - trace_data_);
    REXGPU_WARN(
        "Ignoring incomplete trace tail starting at byte {} (a frame is complete only after a "
        "swap event)",
        tail_offset);
  }
  return true;
}

bool TraceReader::DecompressMemory(MemoryEncodingFormat encoding_format, const void* src,
                                   size_t src_size, void* dest, size_t dest_size) {
  if ((src_size && !src) || (dest_size && !dest)) {
    return false;
  }
  switch (encoding_format) {
    case MemoryEncodingFormat::kNone:
      if (src_size != dest_size) {
        return false;
      }
      if (src_size) {
        std::memcpy(dest, src, src_size);
      }
      return true;
    case MemoryEncodingFormat::kSnappy: {
      if (!src) {
        return false;
      }
      size_t actual_dest_size = 0;
      if (!snappy::GetUncompressedLength(reinterpret_cast<const char*>(src), src_size,
                                         &actual_dest_size) ||
          actual_dest_size != dest_size) {
        return false;
      }
      char empty_output = 0;
      return snappy::RawUncompress(reinterpret_cast<const char*>(src), src_size,
                                   dest_size ? reinterpret_cast<char*>(dest) : &empty_output);
    }
    default:
      return false;
  }
}

}  // namespace rex::graphics
