#pragma once
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

#include <atomic>
#include <string>

#include <rex/graphics/trace_protocol.h>
#include <rex/graphics/trace_reader.h>
#include <rex/thread.h>

namespace rex::graphics {

class GraphicsSystem;

enum class TracePlaybackMode {
  kUntilEnd,
  kBreakOnSwap,
};

class TracePlayer : public TraceReader {
 public:
  TracePlayer(GraphicsSystem* graphics_system);

  GraphicsSystem* graphics_system() const { return graphics_system_; }
  int current_frame_index() const { return current_frame_index_; }
  int current_command_index() const { return current_command_index_; }
  bool is_playing_trace() const { return playing_trace_.load(std::memory_order_acquire); }
  const Frame* current_frame() const;

  // Only valid if playing_trace is true.
  // Scalar from 0-10000
  uint32_t playback_percent() const { return playback_percent_; }

  bool SeekFrame(int target_frame);
  bool SeekCommand(int target_command);

  // Waits for the accepted playback operation and returns whether every trace
  // command was validated and replayed successfully.
  bool WaitOnPlayback();

 private:
  bool PlayTrace(const uint8_t* trace_data, size_t trace_size, TracePlaybackMode playback_mode,
                 bool clear_caches);
  void PlayTraceOnThread(const uint8_t* trace_data, size_t trace_size,
                         TracePlaybackMode playback_mode, bool clear_caches);
  void FinishPlayback(bool succeeded);

  GraphicsSystem* graphics_system_;
  int current_frame_index_;
  int current_command_index_;
  std::atomic<bool> playing_trace_ = false;
  std::atomic<bool> playback_succeeded_ = false;
  std::atomic<uint32_t> playback_percent_ = {0};
  std::unique_ptr<rex::thread::Event> playback_event_;
};

}  // namespace rex::graphics
