/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    2026 - Rewired onto the current Runtime/RuntimeConfig API and
 *                     made fully headless + dependency-free (BMP/RAW dump),
 *                     for deterministic backend comparison when the trace and
 *                     selected backend pass the state-fidelity preflight.
 */

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

#include <rex/filesystem.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/trace_dump.h>
#include <rex/graphics/trace_player.h>
#include <rex/graphics/trace_reader.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/string.h>
#include <rex/system/xtypes.h>
#include <rex/ui/presenter.h>

namespace rex::graphics {

namespace {

bool WriteFile(const std::filesystem::path& path, const void* data, size_t size) {
  if (size && !data) {
    return false;
  }
  FILE* file = rex::filesystem::OpenFile(path, "wb");
  if (!file) {
    return false;
  }
  bool succeeded = !size || std::fwrite(data, 1, size, file) == size;
  if (std::fflush(file) != 0) {
    succeeded = false;
  }
  if (std::fclose(file) != 0) {
    succeeded = false;
  }
  return succeeded;
}

// Write a 24-bit, bottom-up BMP from RGBX (R8 G8 B8 X8) pixel data. BMP is the
// simplest broadly-viewable format with zero dependencies; macOS Preview/sips
// open it directly. We also dump raw RGBA alongside for byte-exact diffing.
bool WriteBmp(const std::filesystem::path& path, const ui::RawImage& image) {
  const uint32_t w = image.width;
  const uint32_t h = image.height;
  if (!w || !h || image.data.empty()) {
    return false;
  }
  const size_t src_stride = image.stride ? image.stride : size_t(w) * 4;
  if (size_t(w) > std::numeric_limits<size_t>::max() / 4 || src_stride < size_t(w) * 4 ||
      size_t(h) > std::numeric_limits<size_t>::max() / src_stride ||
      image.data.size() < src_stride * size_t(h)) {
    return false;
  }
  uint64_t row_bytes_64 = uint64_t(w) * 3;
  uint64_t padded_row_64 = (row_bytes_64 + 3u) & ~uint64_t(3);
  uint64_t pixel_array_64 = padded_row_64 * h;
  uint64_t file_size_64 = 54 + pixel_array_64;
  if (row_bytes_64 > UINT32_MAX || padded_row_64 > UINT32_MAX || pixel_array_64 > UINT32_MAX ||
      file_size_64 > UINT32_MAX || file_size_64 > std::numeric_limits<size_t>::max()) {
    return false;
  }
  const uint32_t padded_row = uint32_t(padded_row_64);
  const uint32_t pixel_array = uint32_t(pixel_array_64);
  const uint32_t file_size = uint32_t(file_size_64);

  std::vector<uint8_t> buf(file_size, 0);
  auto put16 = [&](size_t off, uint16_t v) {
    buf[off] = uint8_t(v & 0xff);
    buf[off + 1] = uint8_t((v >> 8) & 0xff);
  };
  auto put32 = [&](size_t off, uint32_t v) {
    buf[off] = uint8_t(v & 0xff);
    buf[off + 1] = uint8_t((v >> 8) & 0xff);
    buf[off + 2] = uint8_t((v >> 16) & 0xff);
    buf[off + 3] = uint8_t((v >> 24) & 0xff);
  };
  buf[0] = 'B';
  buf[1] = 'M';
  put32(2, file_size);
  put32(10, 54);  // pixel data offset
  put32(14, 40);  // DIB header size (BITMAPINFOHEADER)
  put32(18, w);
  put32(22, h);
  put16(26, 1);   // planes
  put16(28, 24);  // bits per pixel
  put32(34, pixel_array);

  for (uint32_t y = 0; y < h; ++y) {
    // BMP scanlines are bottom-up.
    const uint8_t* src = image.data.data() + size_t(h - 1 - y) * src_stride;
    uint8_t* dst = buf.data() + 54 + size_t(y) * padded_row;
    for (uint32_t x = 0; x < w; ++x) {
      // src is R,G,B,X; BMP wants B,G,R.
      dst[x * 3 + 0] = src[x * 4 + 2];
      dst[x * 3 + 1] = src[x * 4 + 1];
      dst[x * 3 + 2] = src[x * 4 + 0];
    }
  }

  return WriteFile(path, buf.data(), buf.size());
}

}  // namespace

TraceDump::TraceDump() = default;

TraceDump::~TraceDump() = default;

int TraceDump::Main(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    REXGPU_ERROR(
        "usage: trace_dump <trace_file> [output_base] [frame_index] "
        "[--best-effort]");
    return 5;
  }

  std::vector<std::string_view> positional_args;
  bool allow_best_effort = false;
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--best-effort") {
      allow_best_effort = true;
    } else if (args[i].starts_with("--")) {
      REXGPU_ERROR("Unknown trace dump option '{}'", args[i]);
      return 5;
    } else {
      positional_args.emplace_back(args[i]);
    }
  }
  if (positional_args.empty() || positional_args.size() > 3) {
    REXGPU_ERROR(
        "usage: trace_dump <trace_file> [output_base] [frame_index] "
        "[--best-effort]");
    return 5;
  }

  std::filesystem::path path = rex::to_path(positional_args[0]);
  std::filesystem::path output_path;
  if (positional_args.size() >= 2) {
    output_path = rex::to_path(positional_args[1]);
  }
  if (positional_args.size() >= 3) {
    std::string_view frame_index_arg = positional_args[2];
    int parsed_frame_index = 0;
    auto [end_ptr, error] =
        std::from_chars(frame_index_arg.data(), frame_index_arg.data() + frame_index_arg.size(),
                        parsed_frame_index);
    if (frame_index_arg.empty() || error != std::errc() ||
        end_ptr != frame_index_arg.data() + frame_index_arg.size() || parsed_frame_index < 0) {
      REXGPU_ERROR("Invalid frame index '{}'; expected a non-negative integer",
                   std::string(frame_index_arg));
      return 5;
    }
    frame_index_ = parsed_frame_index;
  }

  auto abs_path = std::filesystem::absolute(path);
  REXGPU_INFO("Loading trace file {} (frame {})...", rex::path_to_utf8(abs_path), frame_index_);

  // Validate the trace before reserving guest memory or creating the graphics
  // backend. This keeps malformed, incomplete and out-of-range trace errors
  // visible even on hosts where runtime setup itself isn't available.
  {
    TraceReader preflight_reader;
    if (!preflight_reader.Open(rex::path_to_utf8(abs_path))) {
      REXGPU_ERROR("Unable to load or validate trace file");
      return 5;
    }
    if (!preflight_reader.frame_count()) {
      REXGPU_ERROR("Trace contains no completed swap frames");
      return 6;
    }
    if (frame_index_ < 0 || frame_index_ >= preflight_reader.frame_count()) {
      REXGPU_ERROR("Frame {} is out of range (trace has {} frames)", frame_index_,
                   preflight_reader.frame_count());
      return 6;
    }
    std::string backend_edram_limitation;
    bool backend_supports_edram_requirements = false;
    if (preflight_reader.has_finalized_edram_requirements()) {
      backend_supports_edram_requirements = SupportsCanonicalEdramRequirements(
          preflight_reader.edram_requirements(), backend_edram_limitation);
    }
    const TraceReplayPreflight replay_preflight = {
        preflight_reader.has_initial_edram_snapshot(),
        preflight_reader.edram_requirements_finalized(),
        preflight_reader.edram_requirements_tracked(),
        backend_supports_edram_requirements,
    };
    if (!replay_preflight.HasDeterministicEdramState()) {
      if (!replay_preflight.PermitsReplay(allow_best_effort)) {
        if (!replay_preflight.has_initial_edram_snapshot) {
          REXGPU_ERROR(
              "Trace has no initial 10 MiB EDRAM snapshot. Standalone replay "
              "would start with undefined color, depth and stencil state.");
        }
        if (!replay_preflight.edram_requirements_finalized) {
          REXGPU_ERROR(
              "Trace ended without a finalized whole-capture EDRAM requirements manifest.");
        } else if (!replay_preflight.edram_requirements_tracked) {
          REXGPU_ERROR(
              "Trace was finalized without EDRAM requirements tracking; its "
              "render-target contract is unknown.");
        } else if (!replay_preflight.backend_supports_edram_requirements) {
          REXGPU_ERROR("Selected backend cannot satisfy this trace's canonical EDRAM contract: {}",
                       backend_edram_limitation);
        }
        REXGPU_ERROR(
            "Refusing to label this replay deterministic. Pass --best-effort "
            "only for diagnostic output.");
        return 6;
      }
      REXGPU_WARN(
          "BEST-EFFORT TRACE REPLAY: output is diagnostic and must not be used "
          "as a deterministic backend oracle.");
      if (!replay_preflight.has_initial_edram_snapshot) {
        REXGPU_WARN("The trace has no initial canonical EDRAM snapshot.");
      }
      if (!replay_preflight.edram_requirements_finalized) {
        REXGPU_WARN("The trace EDRAM requirements manifest is unfinalized.");
      } else if (!replay_preflight.edram_requirements_tracked) {
        REXGPU_WARN("The trace EDRAM requirements contract is unknown.");
      } else if (!replay_preflight.backend_supports_edram_requirements) {
        REXGPU_WARN("Backend EDRAM limitation: {}", backend_edram_limitation);
      }
    }
  }

  if (!Setup()) {
    REXGPU_ERROR("Unable to setup trace dump tool");
    return 4;
  }
  if (!Load(abs_path)) {
    REXGPU_ERROR("Unable to load trace file; not found?");
    return 5;
  }

  if (output_path.empty()) {
    output_path = path;
    output_path.replace_extension();
  }
  base_output_path_ = output_path;
  if (!rex::filesystem::CreateParentFolder(base_output_path_)) {
    REXGPU_ERROR("Unable to create output directory for {}", rex::path_to_utf8(base_output_path_));
    return 5;
  }

  return Run();
}

bool TraceDump::Setup() {
  // Headless runtime: empty game_data_root => VFS setup is skipped. No audio,
  // no input, no kernel game-init: trace replay only needs guest memory + the
  // GPU command processor + an offscreen presenter to capture from.
  emulator_ = std::make_unique<Runtime>(std::filesystem::path{});

  RuntimeConfig config;
  config.graphics = CreateGraphicsSystem();
  config.force_presentation = true;  // create an offscreen presenter (no window)

  X_STATUS result = emulator_->Setup(std::move(config));
  if (XFAILED(result)) {
    REXGPU_ERROR("Failed to setup runtime: {:08X}", uint32_t(result));
    return false;
  }

  graphics_system_ = static_cast<GraphicsSystem*>(emulator_->graphics_system());
  if (!graphics_system_) {
    REXGPU_ERROR("Runtime has no graphics system");
    return false;
  }
  player_ = std::make_unique<TracePlayer>(graphics_system_);
  return true;
}

bool TraceDump::Load(const std::filesystem::path& trace_file_path) {
  trace_file_path_ = trace_file_path;
  if (!player_->Open(rex::path_to_utf8(trace_file_path_))) {
    REXGPU_ERROR("Could not load trace file");
    return false;
  }
  return true;
}

int TraceDump::Run() {
  if (!player_->frame_count()) {
    REXGPU_ERROR("Trace contains no completed swap frames");
    return 6;
  }
  if (frame_index_ < 0 || frame_index_ >= player_->frame_count()) {
    REXGPU_ERROR("Frame {} is out of range (trace has {} frames)", frame_index_,
                 player_->frame_count());
    return 6;
  }

  BeginHostCapture();
  if (!player_->SeekFrame(frame_index_)) {
    REXGPU_ERROR("Unable to start playback for frame {}", frame_index_);
    EndHostCapture();
    return 7;
  }
  bool playback_succeeded = player_->WaitOnPlayback();
  EndHostCapture();
  if (!playback_succeeded) {
    REXGPU_ERROR("Playback failed while replaying frame {}", frame_index_);
    return 8;
  }

  int result = 0;
  ui::Presenter* presenter = graphics_system_->presenter();
  ui::RawImage image;
  if (presenter && presenter->CaptureGuestOutput(image)) {
    std::filesystem::path bmp_path = base_output_path_;
    bmp_path += ".bmp";
    if (WriteBmp(bmp_path, image)) {
      REXGPU_INFO("Wrote {} ({}x{})", rex::path_to_utf8(bmp_path), image.width, image.height);
    } else {
      REXGPU_ERROR("Failed to write BMP output");
      result = 1;
    }
    // Raw RGBX for byte-exact backend diffing.
    std::filesystem::path raw_path = base_output_path_;
    raw_path += ".rgba";
    if (!WriteFile(raw_path, image.data.data(), image.data.size())) {
      REXGPU_ERROR("Failed to write raw output {}", rex::path_to_utf8(raw_path));
      result = 1;
    }
  } else {
    REXGPU_ERROR("CaptureGuestOutput failed (no presenter, or backend capture unimplemented)");
    result = 1;
  }

  player_.reset();
  emulator_.reset();
  return result;
}

}  // namespace rex::graphics
