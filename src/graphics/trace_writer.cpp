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

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <rex/graphics/trace_writer.h>

// TODO(tomc): Enable on other platforms once the RTTI linking issue is resolved.
#ifdef _WIN32
#define REX_TRACE_USE_SNAPPY 1
#include "snappy-sinksource.h"
#include "snappy.h"
#endif

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/version.h>

namespace rex::graphics {

namespace {

std::string BuildRevision() {
  const std::string version = REXGLUE_VERSION_STRING;
  const size_t marker = version.rfind(".g");
  if (marker == std::string::npos || marker + 2 >= version.size()) {
    return "unknown";
  }
  const size_t revision_begin = marker + 2;
  size_t revision_end = revision_begin;
  while (revision_end < version.size() &&
         std::isxdigit(static_cast<unsigned char>(version[revision_end]))) {
    ++revision_end;
  }
  if (revision_end == revision_begin) {
    return "unknown";
  }
  return version.substr(revision_begin, revision_end - revision_begin);
}

#if REX_TRACE_USE_SNAPPY
class CheckedSnappySink final : public snappy::Sink {
 public:
  explicit CheckedSnappySink(FILE* file) : file_(file) {}

  void Append(const char* bytes, size_t length) override {
    if (!ok_ || (length && std::fwrite(bytes, 1, length, file_) != length)) {
      ok_ = false;
    }
  }

  bool ok() const { return ok_ && std::ferror(file_) == 0; }

 private:
  FILE* file_ = nullptr;
  bool ok_ = true;
};
#endif

}  // namespace

TraceWriter::TraceWriter(uint8_t* membase) : membase_(membase) {}

TraceWriter::~TraceWriter() {
  if (file_) {
    Close();
  }
}

bool TraceWriter::SetByteBudget(uint64_t byte_budget) {
  if (file_ || !byte_budget || byte_budget > kDefaultByteBudget) {
    return false;
  }
  byte_budget_ = byte_budget;
  return true;
}

bool TraceWriter::Fail(std::string reason) {
  if (!failed_) {
    failed_ = true;
    failure_reason_ = std::move(reason);
    REXGPU_ERROR("TraceWriter: {}", failure_reason_);
  }
  return false;
}

bool TraceWriter::Reserve(uint64_t decoded_bytes, uint64_t worst_case_bytes) {
  if (!file_ || failed_) {
    return false;
  }
  if (decoded_byte_count_ > byte_budget_ ||
      decoded_bytes > byte_budget_ - decoded_byte_count_ ||
      worst_case_byte_count_ > byte_budget_ ||
      worst_case_bytes > byte_budget_ - worst_case_byte_count_) {
    return Fail("capture byte budget exceeded");
  }
  decoded_byte_count_ += decoded_bytes;
  worst_case_byte_count_ += worst_case_bytes;
  return true;
}

bool TraceWriter::WriteBytes(const void* data, size_t length) {
  if (!file_ || failed_) {
    return false;
  }
  if (failure_point_for_testing_ == FailurePointForTesting::kWrite) {
    return Fail("injected trace write failure");
  }
  if ((length && !data) || (length && std::fwrite(data, 1, length, file_) != length) ||
      std::ferror(file_)) {
    return Fail("trace write failed");
  }
  return true;
}

bool TraceWriter::Seek(long offset, int origin) {
  if (!file_ || failed_ || std::fseek(file_, offset, origin) != 0) {
    return Fail("trace seek failed");
  }
  return true;
}

uint64_t TraceWriter::WorstCasePayloadSize(size_t decoded_length,
                                           bool use_compression) const {
#if REX_TRACE_USE_SNAPPY
  if (compress_output_ && use_compression) {
    return uint64_t(snappy::MaxCompressedLength(decoded_length));
  }
#else
  (void)use_compression;
#endif
  return uint64_t(decoded_length);
}

bool TraceWriter::InitializeOpenFile(const std::filesystem::path& path,
                                     uint32_t title_id) {
  path_ = path;
  cached_memory_reads_.clear();
  decoded_byte_count_ = 0;
  worst_case_byte_count_ = 0;
  closed_byte_count_ = 0;
  failed_ = false;
  failure_reason_.clear();
  last_close_finalized_manifest_ = false;
  edram_requirements_tracking_locked_ = false;
  edram_requirements_tracking_enabled_ = false;
  edram_requirements_tracking_valid_ = false;
  allow_manifest_finalization_ = true;
  edram_requirements_ = {};

  TraceHeader header = {};
  header.version = kTraceFormatVersion;
  const std::string revision = BuildRevision();
  std::memcpy(header.build_commit_sha, revision.data(),
              std::min(revision.size(), sizeof(header.build_commit_sha)));
  header.title_id = title_id;
  header.edram_manifest = MakeTraceEdramRequirementsManifest();
  if (!Reserve(sizeof(header), sizeof(header)) || !WriteBytes(&header, sizeof(header))) {
    return false;
  }
  REXGPU_INFO("TraceWriter: Opened trace file: {}", path.string());
  return true;
}

bool TraceWriter::Open(const std::filesystem::path& path, uint32_t title_id) {
  if (file_ && !Close()) {
    return false;
  }
  failed_ = false;
  failure_reason_.clear();
  if (failure_point_for_testing_ == FailurePointForTesting::kOpen) {
    return Fail("injected trace open failure");
  }

  std::error_code filesystem_error;
  const auto absolute_path = std::filesystem::absolute(path, filesystem_error);
  if (filesystem_error) {
    return Fail("failed to resolve trace path: " + filesystem_error.message());
  }
  if (absolute_path.has_parent_path()) {
    std::filesystem::create_directories(absolute_path.parent_path(), filesystem_error);
    if (filesystem_error) {
      return Fail("failed to create trace directory: " + filesystem_error.message());
    }
  }
#ifdef _WIN32
  file_ = _wfopen(absolute_path.c_str(), L"wb");
#else
  file_ = std::fopen(absolute_path.c_str(), "wb");
#endif
  if (!file_) {
    return Fail("failed to open trace file: " + std::string(std::strerror(errno)));
  }
  remove_path_on_discard_ = true;
  if (!InitializeOpenFile(absolute_path, title_id)) {
    Discard();
    return false;
  }
  return true;
}

bool TraceWriter::OpenFileDescriptor(int descriptor,
                                     const std::filesystem::path& display_path,
                                     uint32_t title_id) {
  if (file_ && !Close()) {
#ifdef _WIN32
    _close(descriptor);
#else
    ::close(descriptor);
#endif
    return false;
  }
  failed_ = false;
  failure_reason_.clear();
  if (failure_point_for_testing_ == FailurePointForTesting::kOpen) {
#ifdef _WIN32
    _close(descriptor);
#else
    ::close(descriptor);
#endif
    return Fail("injected trace open failure");
  }
#ifdef _WIN32
  file_ = _fdopen(descriptor, "wb");
#else
  file_ = fdopen(descriptor, "wb");
#endif
  if (!file_) {
#ifdef _WIN32
    _close(descriptor);
#else
    ::close(descriptor);
#endif
    return Fail("failed to attach trace file descriptor: " +
                std::string(std::strerror(errno)));
  }
  // The display path is informational only. The private slot owns unlinking
  // through its held directory descriptor, so never re-resolve this path when
  // discarding a descriptor-backed capture.
  remove_path_on_discard_ = false;
  if (!InitializeOpenFile(display_path, title_id)) {
    Discard();
    return false;
  }
  return true;
}

bool TraceWriter::Flush() {
  if (!file_ || failed_) {
    return !failed_;
  }
  if (failure_point_for_testing_ == FailurePointForTesting::kFlush) {
    return Fail("injected trace flush failure");
  }
  if (std::fflush(file_) != 0 || std::ferror(file_)) {
    return Fail("trace flush failed");
  }
  return true;
}

bool TraceWriter::Close() {
  if (!file_) {
    return !failed_;
  }
  const std::filesystem::path closed_path = path_;
  const bool remove_closed_path_on_failure = remove_path_on_discard_;
  cached_memory_reads_.clear();
  bool success = !failed_;
  if (success && allow_manifest_finalization_) {
    success = FinalizeEdramRequirementsManifest();
    last_close_finalized_manifest_ = success;
  }
  if (success) {
    success = Flush();
  }
  if (success) {
    const long position = std::ftell(file_);
    if (position < 0) {
      success = Fail("could not determine closed trace size");
    } else {
      closed_byte_count_ = uint64_t(position);
    }
  }
  if (failure_point_for_testing_ == FailurePointForTesting::kClose) {
    success = Fail("injected trace close failure");
  }
  if (std::fclose(file_) != 0) {
    success = Fail("trace close failed");
  }
  file_ = nullptr;
  if (success) {
    REXGPU_INFO("TraceWriter: Closed trace file");
  } else if (remove_closed_path_on_failure && !closed_path.empty()) {
    // Descriptor-backed private captures are removed by their owning slot via
    // unlinkat. Legacy path-backed captures can be cleaned here without a
    // second path lookup by the caller after Close has cleared its state.
    std::error_code remove_error;
    if (!std::filesystem::remove(closed_path, remove_error) && remove_error) {
      REXGPU_ERROR("TraceWriter: Failed to remove failed trace {}: {}",
                   closed_path.string(), remove_error.message());
    }
  }
  path_.clear();
  edram_requirements_tracking_locked_ = false;
  edram_requirements_tracking_enabled_ = false;
  edram_requirements_tracking_valid_ = false;
  allow_manifest_finalization_ = true;
  edram_requirements_ = {};
  remove_path_on_discard_ = true;
  return success && !failed_;
}

bool TraceWriter::Discard() {
  const std::filesystem::path discarded_path = path_;
  bool cleanup_success = true;
  if (file_) {
    TraceHeader invalid_header = {};
    std::clearerr(file_);
    if (std::fseek(file_, 0, SEEK_SET) != 0 ||
        std::fwrite(&invalid_header, 1, sizeof(invalid_header), file_) !=
            sizeof(invalid_header) ||
        std::fflush(file_) != 0) {
      cleanup_success = false;
    }
    allow_manifest_finalization_ = false;
    if (std::fclose(file_) != 0) {
      cleanup_success = false;
    }
    file_ = nullptr;
  }
  path_.clear();
  cached_memory_reads_.clear();
  edram_requirements_tracking_locked_ = false;
  edram_requirements_tracking_enabled_ = false;
  edram_requirements_tracking_valid_ = false;
  allow_manifest_finalization_ = true;
  edram_requirements_ = {};
  if (remove_path_on_discard_ && !discarded_path.empty()) {
    std::error_code error;
    if (!std::filesystem::remove(discarded_path, error) && error) {
      cleanup_success = false;
      REXGPU_ERROR("TraceWriter: Failed to remove incomplete trace {}: {}",
                   discarded_path.string(), error.message());
    }
  }
  remove_path_on_discard_ = true;
  return cleanup_success;
}

bool TraceWriter::BeginEdramRequirementsTracking() {
  if (!file_ || failed_ || edram_requirements_tracking_locked_ ||
      edram_requirements_tracking_enabled_) {
    return false;
  }
  edram_requirements_tracking_enabled_ = true;
  edram_requirements_tracking_valid_ = true;
  edram_requirements_ = {};
  return true;
}

bool TraceWriter::RequireEdramColorFormat(xenos::ColorRenderTargetFormat format) {
  const uint32_t index = uint32_t(format);
  if (!file_ || !edram_requirements_tracking_enabled_ || index >= 32 ||
      !(kTraceEdramKnownColorFormatMask & (1u << index))) {
    InvalidateEdramRequirementsTracking();
    return false;
  }
  edram_requirements_.color_format_mask |= 1u << index;
  return true;
}

bool TraceWriter::RequireEdramDepthFormat(xenos::DepthRenderTargetFormat format) {
  const uint32_t index = uint32_t(format);
  if (!file_ || !edram_requirements_tracking_enabled_ || index >= 32 ||
      !(kTraceEdramKnownDepthFormatMask & (1u << index))) {
    InvalidateEdramRequirementsTracking();
    return false;
  }
  edram_requirements_.depth_format_mask |= 1u << index;
  return true;
}

bool TraceWriter::RequireEdramMsaaSamples(xenos::MsaaSamples samples) {
  const uint32_t index = uint32_t(samples);
  if (!file_ || !edram_requirements_tracking_enabled_ || index >= 32 ||
      !(kTraceEdramKnownMsaaSamplesMask & (1u << index))) {
    InvalidateEdramRequirementsTracking();
    return false;
  }
  edram_requirements_.msaa_samples_mask |= 1u << index;
  return true;
}

void TraceWriter::InvalidateEdramRequirementsTracking() {
  edram_requirements_tracking_valid_ = false;
}

void TraceWriter::LockEdramRequirementsTracking() {
  edram_requirements_tracking_locked_ = true;
}

bool TraceWriter::FinalizeEdramRequirementsManifest() {
  if (!file_ || failed_) {
    return false;
  }
  if (failure_point_for_testing_ == FailurePointForTesting::kFinalize) {
    return Fail("injected EDRAM manifest finalization failure");
  }
  if (std::fflush(file_) != 0 || std::ferror(file_)) {
    return Fail("trace flush before manifest finalization failed");
  }
  const bool tracked =
      edram_requirements_tracking_enabled_ && edram_requirements_tracking_valid_;
  const auto manifest =
      MakeTraceEdramRequirementsManifest(edram_requirements_, tracked, true);
  const long previous_position = std::ftell(file_);
  if (previous_position < 0 ||
      std::fseek(file_, long(offsetof(TraceHeader, edram_manifest)), SEEK_SET) != 0 ||
      std::fwrite(&manifest, 1, sizeof(manifest), file_) != sizeof(manifest) ||
      std::fflush(file_) != 0 || std::ferror(file_) ||
      std::fseek(file_, previous_position, SEEK_SET) != 0) {
    return Fail("failed to finalize EDRAM requirements manifest");
  }
  return true;
}

bool TraceWriter::WritePrimaryBufferStart(uint32_t base_ptr, uint32_t count) {
  (void)count;
  if (!file_) return false;
  LockEdramRequirementsTracking();
  const PrimaryBufferStartCommand command = {
      TraceCommandType::kPrimaryBufferStart, base_ptr, 0};
  return Reserve(sizeof(command), sizeof(command)) &&
         WriteBytes(&command, sizeof(command));
}

bool TraceWriter::WritePrimaryBufferEnd() {
  if (!file_) return false;
  LockEdramRequirementsTracking();
  const PrimaryBufferEndCommand command = {TraceCommandType::kPrimaryBufferEnd};
  return Reserve(sizeof(command), sizeof(command)) &&
         WriteBytes(&command, sizeof(command));
}

bool TraceWriter::WriteIndirectBufferStart(uint32_t base_ptr, uint32_t count) {
  (void)count;
  if (!file_) return false;
  LockEdramRequirementsTracking();
  const IndirectBufferStartCommand command = {
      TraceCommandType::kIndirectBufferStart, base_ptr, 0};
  return Reserve(sizeof(command), sizeof(command)) &&
         WriteBytes(&command, sizeof(command));
}

bool TraceWriter::WriteIndirectBufferEnd() {
  if (!file_) return false;
  LockEdramRequirementsTracking();
  const IndirectBufferEndCommand command = {TraceCommandType::kIndirectBufferEnd};
  return Reserve(sizeof(command), sizeof(command)) &&
         WriteBytes(&command, sizeof(command));
}

bool TraceWriter::WritePacketStart(uint32_t base_ptr, uint32_t count) {
  if (!file_) {
    return false;
  }
  if (count && !membase_) {
    return Fail("packet payload has no guest-memory source");
  }
  LockEdramRequirementsTracking();
  const PacketStartCommand command = {TraceCommandType::kPacketStart, base_ptr, count};
  const uint64_t payload_size = uint64_t(count) * sizeof(uint32_t);
  if (payload_size > std::numeric_limits<size_t>::max() ||
      !Reserve(sizeof(command) + payload_size, sizeof(command) + payload_size) ||
      !WriteBytes(&command, sizeof(command))) {
    return false;
  }
  if (!payload_size) {
    return true;
  }
  return WriteBytes(membase_ + base_ptr, size_t(payload_size));
}

bool TraceWriter::WritePacketEnd() {
  if (!file_) return false;
  LockEdramRequirementsTracking();
  const PacketEndCommand command = {TraceCommandType::kPacketEnd};
  return Reserve(sizeof(command), sizeof(command)) &&
         WriteBytes(&command, sizeof(command));
}

bool TraceWriter::WriteMemoryRead(uint32_t base_ptr, size_t length,
                                  const void* host_ptr) {
  return file_ && WriteMemoryCommand(TraceCommandType::kMemoryRead, base_ptr,
                                     length, host_ptr);
}

bool TraceWriter::WriteMemoryReadCached(uint32_t base_ptr, size_t length) {
  if (!file_) return false;
  const uint64_t key = uint64_t(base_ptr) << 32 | uint64_t(length);
  if (cached_memory_reads_.find(key) != cached_memory_reads_.end()) {
    return true;
  }
  if (!WriteMemoryCommand(TraceCommandType::kMemoryRead, base_ptr, length)) {
    return false;
  }
  cached_memory_reads_.insert(key);
  return true;
}

bool TraceWriter::WriteMemoryReadCachedNop(uint32_t base_ptr, size_t length) {
  if (!file_) return false;
  cached_memory_reads_.insert(uint64_t(base_ptr) << 32 | uint64_t(length));
  return true;
}

bool TraceWriter::WriteMemoryWrite(uint32_t base_ptr, size_t length,
                                   const void* host_ptr) {
  return file_ && WriteMemoryCommand(TraceCommandType::kMemoryWrite, base_ptr,
                                     length, host_ptr);
}

bool TraceWriter::WriteMemoryCommand(TraceCommandType type, uint32_t base_ptr,
                                     size_t length, const void* host_ptr) {
  if (!file_ || failed_) {
    return false;
  }
  if (length > UINT32_MAX) {
    return Fail("memory command exceeds the trace format length limit");
  }
  if (length && !host_ptr && !membase_) {
    return Fail("memory command has no guest-memory source");
  }
  LockEdramRequirementsTracking();
  MemoryCommand command = {};
  command.type = type;
  command.base_ptr = base_ptr;
  command.encoding_format = MemoryEncodingFormat::kNone;
  command.encoded_length = command.decoded_length = uint32_t(length);
  if (length && !host_ptr) {
    host_ptr = membase_ + base_ptr;
  }
  const uint64_t worst_payload = WorstCasePayloadSize(
      length, compress_output_ && length > compression_threshold_);
  if (!Reserve(sizeof(command) + length, sizeof(command) + worst_payload)) {
    return false;
  }

#if REX_TRACE_USE_SNAPPY
  if (compress_output_ && length > compression_threshold_) {
    const long header_position = std::ftell(file_);
    if (header_position < 0) return Fail("trace seek position failed");
    command.encoding_format = MemoryEncodingFormat::kSnappy;
    if (!WriteBytes(&command, sizeof(command))) return false;
    snappy::ByteArraySource source(reinterpret_cast<const char*>(host_ptr), length);
    CheckedSnappySink sink(file_);
    command.encoded_length = uint32_t(snappy::Compress(&source, &sink));
    if (!sink.ok() || !Seek(header_position, SEEK_SET) ||
        !WriteBytes(&command, sizeof(command)) ||
        !Seek(header_position + long(sizeof(command)) + command.encoded_length,
              SEEK_SET)) {
      return Fail("trace compression write failed");
    }
    return true;
  }
#endif
  return WriteBytes(&command, sizeof(command)) && WriteBytes(host_ptr, length);
}

bool TraceWriter::WriteEdramSnapshot(const void* snapshot) {
  if (!file_) return false;
  if (!snapshot) return Fail("EDRAM snapshot source is null");
  LockEdramRequirementsTracking();
  EdramSnapshotCommand command = {};
  command.type = TraceCommandType::kEdramSnapshot;
  const uint64_t worst_payload =
      WorstCasePayloadSize(xenos::kEdramSizeBytes, compress_output_);
  if (!Reserve(sizeof(command) + xenos::kEdramSizeBytes,
               sizeof(command) + worst_payload)) {
    return false;
  }
#if REX_TRACE_USE_SNAPPY
  if (compress_output_) {
    const long header_position = std::ftell(file_);
    if (header_position < 0) return Fail("trace seek position failed");
    command.encoding_format = MemoryEncodingFormat::kSnappy;
    if (!WriteBytes(&command, sizeof(command))) return false;
    snappy::ByteArraySource source(reinterpret_cast<const char*>(snapshot),
                                   xenos::kEdramSizeBytes);
    CheckedSnappySink sink(file_);
    command.encoded_length = uint32_t(snappy::Compress(&source, &sink));
    if (!sink.ok() || !Seek(header_position, SEEK_SET) ||
        !WriteBytes(&command, sizeof(command)) ||
        !Seek(header_position + long(sizeof(command)) + command.encoded_length,
              SEEK_SET)) {
      return Fail("EDRAM snapshot compression write failed");
    }
    return true;
  }
#endif
  command.encoding_format = MemoryEncodingFormat::kNone;
  command.encoded_length = xenos::kEdramSizeBytes;
  return WriteBytes(&command, sizeof(command)) &&
         WriteBytes(snapshot, xenos::kEdramSizeBytes);
}

bool TraceWriter::WriteEvent(EventCommand::Type event_type) {
  if (!file_) return false;
  LockEdramRequirementsTracking();
  const EventCommand command = {TraceCommandType::kEvent, event_type};
  return Reserve(sizeof(command), sizeof(command)) &&
         WriteBytes(&command, sizeof(command));
}

bool TraceWriter::WriteRegisters(uint32_t first_register,
                                 const uint32_t* register_values,
                                 uint32_t register_count,
                                 bool execute_callbacks_on_play) {
  if (!file_) return false;
  if (register_count && !register_values) {
    return Fail("register snapshot source is null");
  }
  if (register_count > UINT32_MAX / sizeof(uint32_t)) {
    return Fail("register snapshot exceeds the trace format length limit");
  }
  LockEdramRequirementsTracking();
  RegistersCommand command = {};
  command.type = TraceCommandType::kRegisters;
  command.first_register = first_register;
  command.register_count = register_count;
  command.execute_callbacks = execute_callbacks_on_play;
  const size_t length = sizeof(uint32_t) * size_t(register_count);
  command.encoding_format = MemoryEncodingFormat::kNone;
  command.encoded_length = uint32_t(length);
  const uint64_t worst_payload =
      WorstCasePayloadSize(length, compress_output_ && length != 0);
  if (!Reserve(sizeof(command) + length, sizeof(command) + worst_payload)) {
    return false;
  }
#if REX_TRACE_USE_SNAPPY
  if (compress_output_ && length) {
    const long header_position = std::ftell(file_);
    if (header_position < 0) return Fail("trace seek position failed");
    command.encoding_format = MemoryEncodingFormat::kSnappy;
    if (!WriteBytes(&command, sizeof(command))) return false;
    snappy::ByteArraySource source(reinterpret_cast<const char*>(register_values),
                                   length);
    CheckedSnappySink sink(file_);
    command.encoded_length = uint32_t(snappy::Compress(&source, &sink));
    if (!sink.ok() || !Seek(header_position, SEEK_SET) ||
        !WriteBytes(&command, sizeof(command)) ||
        !Seek(header_position + long(sizeof(command)) + command.encoded_length,
              SEEK_SET)) {
      return Fail("register compression write failed");
    }
    return true;
  }
#endif
  return WriteBytes(&command, sizeof(command)) &&
         WriteBytes(register_values, length);
}

bool TraceWriter::WriteGammaRamp(
    const reg::DC_LUT_30_COLOR* gamma_ramp_256_entry_table,
    const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl_rgb,
    uint32_t gamma_ramp_rw_component) {
  if (!file_) return false;
  if (!gamma_ramp_256_entry_table || !gamma_ramp_pwl_rgb) {
    return Fail("gamma-ramp snapshot source is null");
  }
  LockEdramRequirementsTracking();
  GammaRampCommand command = {};
  command.type = TraceCommandType::kGammaRamp;
  command.rw_component = uint8_t(gamma_ramp_rw_component);
  constexpr size_t kTableLength = sizeof(reg::DC_LUT_30_COLOR) * 256;
  constexpr size_t kPwlLength = sizeof(reg::DC_LUT_PWL_DATA) * 3 * 128;
  constexpr size_t kLength = kTableLength + kPwlLength;
  command.encoding_format = MemoryEncodingFormat::kNone;
  command.encoded_length = uint32_t(kLength);
  const uint64_t worst_payload =
      WorstCasePayloadSize(kLength, compress_output_);
  if (!Reserve(sizeof(command) + kLength, sizeof(command) + worst_payload)) {
    return false;
  }
#if REX_TRACE_USE_SNAPPY
  if (compress_output_) {
    std::unique_ptr<char[]> ramps(new char[kLength]);
    std::memcpy(ramps.get(), gamma_ramp_256_entry_table, kTableLength);
    std::memcpy(ramps.get() + kTableLength, gamma_ramp_pwl_rgb, kPwlLength);
    const long header_position = std::ftell(file_);
    if (header_position < 0) return Fail("trace seek position failed");
    command.encoding_format = MemoryEncodingFormat::kSnappy;
    if (!WriteBytes(&command, sizeof(command))) return false;
    snappy::ByteArraySource source(ramps.get(), kLength);
    CheckedSnappySink sink(file_);
    command.encoded_length = uint32_t(snappy::Compress(&source, &sink));
    if (!sink.ok() || !Seek(header_position, SEEK_SET) ||
        !WriteBytes(&command, sizeof(command)) ||
        !Seek(header_position + long(sizeof(command)) + command.encoded_length,
              SEEK_SET)) {
      return Fail("gamma-ramp compression write failed");
    }
    return true;
  }
#endif
  return WriteBytes(&command, sizeof(command)) &&
         WriteBytes(gamma_ramp_256_entry_table, kTableLength) &&
         WriteBytes(gamma_ramp_pwl_rgb, kPwlLength);
}

}  // namespace rex::graphics
