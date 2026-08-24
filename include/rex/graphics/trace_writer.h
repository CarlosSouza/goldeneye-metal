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

#pragma once

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

#include <rex/graphics/registers.h>
#include <rex/graphics/trace_protocol.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics {

class TraceWriter {
 public:
  static constexpr uint64_t kDefaultByteBudget = 192ull * 1024ull * 1024ull;

  enum class FailurePointForTesting {
    kNone,
    kOpen,
    kWrite,
    kFlush,
    kFinalize,
    kClose,
  };

  explicit TraceWriter(uint8_t* membase);
  ~TraceWriter();

  bool is_open() const { return file_ != nullptr; }
  bool has_error() const { return failed_ || (file_ && std::ferror(file_) != 0); }
  std::string_view failure_reason() const { return failure_reason_; }
  uint64_t decoded_byte_count() const { return decoded_byte_count_; }
  uint64_t worst_case_byte_count() const { return worst_case_byte_count_; }
  uint64_t closed_byte_count() const { return closed_byte_count_; }
  bool last_close_finalized_manifest() const {
    return last_close_finalized_manifest_;
  }

  // Applied to the next Open. A budget covers both decoded trace content and
  // the worst-case encoded file size. The fixed production ceiling is 192 MiB;
  // smaller values are useful for deterministic tests.
  bool SetByteBudget(uint64_t byte_budget);
  void SetFailurePointForTesting(FailurePointForTesting point) {
    failure_point_for_testing_ = point;
  }

  bool Open(const std::filesystem::path& path, uint32_t title_id);
  // Takes ownership of an already-exclusive descriptor. This is used by the
  // private capture slot so path validation and file creation stay race-free.
  bool OpenFileDescriptor(int descriptor, const std::filesystem::path& display_path,
                          uint32_t title_id);
  bool Flush();
  bool Close();
  // Invalidates and removes a trace whose initialization did not complete.
  // Invalidating the header first keeps a removal failure from leaving a file
  // that readers could mistake for a valid capture.
  bool Discard();

  // Enables whole-capture EDRAM requirement collection. This must be called
  // before the initial EDRAM snapshot or first GPU command is written. A clean
  // trace without successful tracking is finalized with an explicitly unknown
  // contract and is never deterministic.
  bool BeginEdramRequirementsTracking();
  bool RequireEdramColorFormat(xenos::ColorRenderTargetFormat format);
  bool RequireEdramDepthFormat(xenos::DepthRenderTargetFormat format);
  bool RequireEdramMsaaSamples(xenos::MsaaSamples msaa_samples);
  void InvalidateEdramRequirementsTracking();

  bool WritePrimaryBufferStart(uint32_t base_ptr, uint32_t count);
  bool WritePrimaryBufferEnd();
  bool WriteIndirectBufferStart(uint32_t base_ptr, uint32_t count);
  bool WriteIndirectBufferEnd();
  bool WritePacketStart(uint32_t base_ptr, uint32_t count);
  bool WritePacketEnd();
  bool WriteMemoryRead(uint32_t base_ptr, size_t length, const void* host_ptr = nullptr);
  bool WriteMemoryReadCached(uint32_t base_ptr, size_t length);
  bool WriteMemoryReadCachedNop(uint32_t base_ptr, size_t length);
  bool WriteMemoryWrite(uint32_t base_ptr, size_t length, const void* host_ptr = nullptr);
  bool WriteEdramSnapshot(const void* snapshot);
  bool WriteEvent(EventCommand::Type event_type);
  bool WriteRegisters(uint32_t first_register, const uint32_t* register_values,
                      uint32_t register_count, bool execute_callbacks_on_play);
  bool WriteGammaRamp(const reg::DC_LUT_30_COLOR* gamma_ramp_256_entry_table,
                      const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl_rgb,
                      uint32_t gamma_ramp_rw_component);

 private:
  bool InitializeOpenFile(const std::filesystem::path& path, uint32_t title_id);
  bool Reserve(uint64_t decoded_bytes, uint64_t worst_case_bytes);
  bool WriteBytes(const void* data, size_t length);
  bool Seek(long offset, int origin);
  bool Fail(std::string reason);
  uint64_t WorstCasePayloadSize(size_t decoded_length,
                                bool use_compression) const;
  void LockEdramRequirementsTracking();
  bool FinalizeEdramRequirementsManifest();
  bool WriteMemoryCommand(TraceCommandType type, uint32_t base_ptr, size_t length,
                          const void* host_ptr = nullptr);

  std::set<uint64_t> cached_memory_reads_;
  uint8_t* membase_;
  FILE* file_ = nullptr;
  std::filesystem::path path_;
  bool remove_path_on_discard_ = true;

  uint64_t byte_budget_ = kDefaultByteBudget;
  uint64_t decoded_byte_count_ = 0;
  uint64_t worst_case_byte_count_ = 0;
  uint64_t closed_byte_count_ = 0;
  bool failed_ = false;
  bool last_close_finalized_manifest_ = false;
  std::string failure_reason_;
  FailurePointForTesting failure_point_for_testing_ = FailurePointForTesting::kNone;

  bool compress_output_ = true;
  size_t compression_threshold_ = 1024;
  bool edram_requirements_tracking_locked_ = false;
  bool edram_requirements_tracking_enabled_ = false;
  bool edram_requirements_tracking_valid_ = false;
  bool allow_manifest_finalization_ = true;
  TraceEdramRequirements edram_requirements_;
};

}  // namespace rex::graphics
