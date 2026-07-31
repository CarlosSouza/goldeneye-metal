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

#include <cstdint>

namespace rex::graphics {

// Trace file extension.
static const char kTraceExtension[] = "xtr";

// Any byte changes to the files should bump this version.
// Only builds with matching versions will work.
// Other changes besides the file format may require bumps, such as
// anything that changes what is recorded into the files (new GPU
// command processor commands, etc).
constexpr uint32_t kTraceFormatVersion = 2;

// EDRAM state fidelity requirements accumulated over the entire trace. Each
// bit is the underlying numeric value of the corresponding Xenos enum. Keeping
// this representation independent of a host backend lets trace replay reject
// unsupported contracts before creating a device or producing output.
struct TraceEdramRequirements {
  uint32_t color_format_mask = 0;
  uint32_t depth_format_mask = 0;
  uint32_t msaa_samples_mask = 0;

  constexpr bool operator==(const TraceEdramRequirements&) const = default;
};

constexpr uint32_t kTraceEdramKnownColorFormatMask =
    (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) |
    (1u << 6) | (1u << 7) | (1u << 10) | (1u << 12) | (1u << 14) | (1u << 15);
constexpr uint32_t kTraceEdramKnownDepthFormatMask = (1u << 0) | (1u << 1);
constexpr uint32_t kTraceEdramKnownMsaaSamplesMask = (1u << 0) | (1u << 1) | (1u << 2);

constexpr uint32_t kTraceEdramManifestMagic = 0x4D524445u;  // "EDRM" in LE.
constexpr uint32_t kTraceEdramManifestVersion = 1;
constexpr uint32_t kTraceEdramManifestFlagRequirementsTracked = 1u << 0;
constexpr uint32_t kTraceEdramManifestFlagFinalized = 1u << 1;
constexpr uint32_t kTraceEdramManifestKnownFlags =
    kTraceEdramManifestFlagRequirementsTracked | kTraceEdramManifestFlagFinalized;
constexpr uint32_t kTraceEdramManifestFinalizationCookie = 0xC14EED12u;

// Stored in the fixed file header so an interrupted capture can't leave a
// valid-looking trailer behind. The checksum and finalization cookie make a
// torn header update fail closed.
struct TraceEdramRequirementsManifest {
  uint32_t magic = kTraceEdramManifestMagic;
  uint32_t version = kTraceEdramManifestVersion;
  uint32_t size = 0;
  uint32_t flags = 0;
  TraceEdramRequirements requirements;
  uint32_t reserved = 0;
  uint32_t checksum = 0;
  uint32_t finalization_cookie = 0;
};

constexpr uint32_t TraceManifestChecksumWord(uint32_t checksum, uint32_t value) {
  for (uint32_t byte_index = 0; byte_index < 4; ++byte_index) {
    checksum ^= uint8_t(value >> (byte_index * 8));
    checksum *= 16777619u;
  }
  return checksum;
}

constexpr uint32_t ComputeTraceEdramManifestChecksum(
    const TraceEdramRequirementsManifest& manifest) {
  uint32_t checksum = 2166136261u;
  checksum = TraceManifestChecksumWord(checksum, manifest.magic);
  checksum = TraceManifestChecksumWord(checksum, manifest.version);
  checksum = TraceManifestChecksumWord(checksum, manifest.size);
  checksum = TraceManifestChecksumWord(checksum, manifest.flags);
  checksum = TraceManifestChecksumWord(checksum, manifest.requirements.color_format_mask);
  checksum = TraceManifestChecksumWord(checksum, manifest.requirements.depth_format_mask);
  checksum = TraceManifestChecksumWord(checksum, manifest.requirements.msaa_samples_mask);
  checksum = TraceManifestChecksumWord(checksum, manifest.reserved);
  checksum = TraceManifestChecksumWord(checksum, manifest.finalization_cookie);
  return checksum;
}

constexpr TraceEdramRequirementsManifest MakeTraceEdramRequirementsManifest(
    TraceEdramRequirements requirements = {}, bool requirements_tracked = false,
    bool finalized = false) {
  TraceEdramRequirementsManifest manifest;
  manifest.size = sizeof(TraceEdramRequirementsManifest);
  if (requirements_tracked) {
    manifest.flags |= kTraceEdramManifestFlagRequirementsTracked;
    manifest.requirements = requirements;
  }
  if (finalized) {
    manifest.flags |= kTraceEdramManifestFlagFinalized;
    manifest.finalization_cookie = kTraceEdramManifestFinalizationCookie;
  }
  manifest.checksum = ComputeTraceEdramManifestChecksum(manifest);
  return manifest;
}

constexpr bool IsTraceEdramRequirementsManifestValid(
    const TraceEdramRequirementsManifest& manifest) {
  if (manifest.magic != kTraceEdramManifestMagic ||
      manifest.version != kTraceEdramManifestVersion ||
      manifest.size != sizeof(TraceEdramRequirementsManifest) ||
      (manifest.flags & ~kTraceEdramManifestKnownFlags) || manifest.reserved ||
      (manifest.requirements.color_format_mask & ~kTraceEdramKnownColorFormatMask) ||
      (manifest.requirements.depth_format_mask & ~kTraceEdramKnownDepthFormatMask) ||
      (manifest.requirements.msaa_samples_mask & ~kTraceEdramKnownMsaaSamplesMask) ||
      manifest.checksum != ComputeTraceEdramManifestChecksum(manifest)) {
    return false;
  }
  const bool requirements_tracked =
      (manifest.flags & kTraceEdramManifestFlagRequirementsTracked) != 0;
  const bool finalized = (manifest.flags & kTraceEdramManifestFlagFinalized) != 0;
  if (!requirements_tracked &&
      !(manifest.requirements == TraceEdramRequirements{})) {
    return false;
  }
  if (finalized) {
    return manifest.finalization_cookie == kTraceEdramManifestFinalizationCookie;
  }
  // The writer never publishes requirements before finalization. This also
  // prevents a partially patched manifest from being treated as intentional.
  return !requirements_tracked && manifest.finalization_cookie == 0;
}

// Trace file header identifying information about the trace.
// This must be positioned at the start of the file and must only occur once.
struct TraceHeader {
  // Must be the first 4 bytes of the file.
  // Set to kTraceFormatVersion.
  uint32_t version;

  // SHA1 of the commit used to record the trace.
  char build_commit_sha[40];

  // Title ID of game that was being recorded.
  // May be 0 if not generated from a game or the ID could not be retrieved.
  uint32_t title_id;

  // Whole-capture EDRAM fidelity contract. A cleanly closed trace may still
  // have tracking disabled; that is an explicitly unknown contract, not an
  // empty one, and must not be called deterministic.
  TraceEdramRequirementsManifest edram_manifest;
};

static_assert(sizeof(TraceEdramRequirements) == 12);
static_assert(sizeof(TraceEdramRequirementsManifest) == 40);
static_assert(sizeof(TraceHeader) == 88);

// Tags each command in the trace file stream as one of the *Command types.
// Each command has this value as its first dword.
enum class TraceCommandType : uint32_t {
  kPrimaryBufferStart,
  kPrimaryBufferEnd,
  kIndirectBufferStart,
  kIndirectBufferEnd,
  kPacketStart,
  kPacketEnd,
  kMemoryRead,
  kMemoryWrite,
  kEdramSnapshot,
  kEvent,
  kRegisters,
  kGammaRamp,
};

struct PrimaryBufferStartCommand {
  TraceCommandType type;
  uint32_t base_ptr;
  uint32_t count;
};

struct PrimaryBufferEndCommand {
  TraceCommandType type;
};

struct IndirectBufferStartCommand {
  TraceCommandType type;
  uint32_t base_ptr;
  uint32_t count;
};

struct IndirectBufferEndCommand {
  TraceCommandType type;
};

struct PacketStartCommand {
  TraceCommandType type;
  uint32_t base_ptr;
  uint32_t count;
};

struct PacketEndCommand {
  TraceCommandType type;
};

// The compression format used for memory read/write buffers.
// Note that not every memory read/write will have compressed data
// (as it's silly to compress 4 byte buffers).
enum class MemoryEncodingFormat {
  // Data is in its raw form. encoded_length == decoded_length.
  kNone,
  // Data is compressed with third_party/snappy.
  kSnappy,
};

// Represents the GPU reading or writing data from or to memory.
// Used for both TraceCommandType::kMemoryRead and kMemoryWrite.
struct MemoryCommand {
  TraceCommandType type;

  // Base physical memory pointer this read starts at.
  uint32_t base_ptr;
  // Encoding format of the data in the trace file.
  MemoryEncodingFormat encoding_format;
  // Number of bytes the data occupies in the trace file in its encoded form.
  uint32_t encoded_length;
  // Number of bytes the data occupies in memory after decoding.
  // Note that if no encoding is used this will equal encoded_length.
  uint32_t decoded_length;
};

// Represents a full 10 MB snapshot of EDRAM contents, for trace initialization
// (since replaying the trace will reconstruct its state at any point later) as
// a sequence of tiles with row-major samples (2x multisampling as 1x2 samples,
// 4x as 2x2 samples).
struct EdramSnapshotCommand {
  TraceCommandType type;
  // Encoding format of the data in the trace file.
  MemoryEncodingFormat encoding_format;
  // Number of bytes the data occupies in the trace file in its encoded form.
  uint32_t encoded_length;
};

// Represents a GPU event of EventCommand::Type.
struct EventCommand {
  TraceCommandType type;

  // Identifies the event that occurred.
  enum class Type {
    kSwap,
  };
  Type event_type;
};

// Represents a range of registers.
struct RegistersCommand {
  TraceCommandType type;

  uint32_t first_register;
  uint32_t register_count;
  // Whether to set the registers via WriteRegister, which may have side
  // effects, rather than by copying them directly to the register file.
  bool execute_callbacks;

  // Encoding format of the values in the trace file.
  MemoryEncodingFormat encoding_format;
  // Number of bytes the values occupy in the trace file in their encoded form.
  // If no encoding is used, this will be sizeof(uint32_t) * register_count.
  uint32_t encoded_length;
};

// Represents a gamma ramp - encoded 256 DC_LUT_30_COLOR values and 128
// interleaved RGB DC_LUT_PWL_DATA values.
// Assuming that all other gamma ramp state is saved as plain registers.
struct GammaRampCommand {
  TraceCommandType type;

  // The component index (0 = red, 1 = green, 2 = blue) for the next
  // DC_LUT_SEQ_COLOR or DC_LUT_PWL_DATA read or write.
  uint8_t rw_component;

  // Encoding format of the ramps in the trace file.
  MemoryEncodingFormat encoding_format;
  // Number of bytes the ramps occupy in the trace file in their encoded form.
  // If no encoding is used, this will be sizeof(uint32_t) * (256 + 3 * 128).
  uint32_t encoded_length;
};

}  // namespace rex::graphics
