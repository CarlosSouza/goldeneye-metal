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
#include <filesystem>
#include <set>
#include <string>

#include <rex/graphics/registers.h>
#include <rex/graphics/trace_protocol.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics {

class TraceWriter {
 public:
  explicit TraceWriter(uint8_t* membase);
  ~TraceWriter();

  bool is_open() const { return file_ != nullptr; }
  bool has_error() const { return file_ && std::ferror(file_) != 0; }

  bool Open(const std::filesystem::path& path, uint32_t title_id);
  void Flush();
  void Close();
  // Invalidates and removes a trace whose initialization did not complete.
  // Invalidating the header first keeps a removal failure from leaving a file
  // that readers could mistake for a valid capture.
  void Discard();

  // Enables whole-capture EDRAM requirement collection. This must be called
  // before the initial EDRAM snapshot or first GPU command is written. A clean
  // trace without successful tracking is finalized with an explicitly unknown
  // contract and is never deterministic.
  bool BeginEdramRequirementsTracking();
  bool RequireEdramColorFormat(xenos::ColorRenderTargetFormat format);
  bool RequireEdramDepthFormat(xenos::DepthRenderTargetFormat format);
  bool RequireEdramMsaaSamples(xenos::MsaaSamples msaa_samples);
  void InvalidateEdramRequirementsTracking();

  void WritePrimaryBufferStart(uint32_t base_ptr, uint32_t count);
  void WritePrimaryBufferEnd();
  void WriteIndirectBufferStart(uint32_t base_ptr, uint32_t count);
  void WriteIndirectBufferEnd();
  void WritePacketStart(uint32_t base_ptr, uint32_t count);
  void WritePacketEnd();
  void WriteMemoryRead(uint32_t base_ptr, size_t length, const void* host_ptr = nullptr);
  void WriteMemoryReadCached(uint32_t base_ptr, size_t length);
  void WriteMemoryReadCachedNop(uint32_t base_ptr, size_t length);
  void WriteMemoryWrite(uint32_t base_ptr, size_t length, const void* host_ptr = nullptr);
  void WriteEdramSnapshot(const void* snapshot);
  void WriteEvent(EventCommand::Type event_type);
  void WriteRegisters(uint32_t first_register, const uint32_t* register_values,
                      uint32_t register_count, bool execute_callbacks_on_play);
  void WriteGammaRamp(const reg::DC_LUT_30_COLOR* gamma_ramp_256_entry_table,
                      const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl_rgb,
                      uint32_t gamma_ramp_rw_component);

 private:
  void LockEdramRequirementsTracking();
  bool FinalizeEdramRequirementsManifest();
  void WriteMemoryCommand(TraceCommandType type, uint32_t base_ptr, size_t length,
                          const void* host_ptr = nullptr);

  std::set<uint64_t> cached_memory_reads_;
  uint8_t* membase_;
  FILE* file_ = nullptr;
  std::filesystem::path path_;

  bool compress_output_ = true;
  size_t compression_threshold_ = 1024;
  bool edram_requirements_tracking_locked_ = false;
  bool edram_requirements_tracking_enabled_ = false;
  bool edram_requirements_tracking_valid_ = false;
  bool allow_manifest_finalization_ = true;
  TraceEdramRequirements edram_requirements_;
};

}  // namespace rex::graphics
