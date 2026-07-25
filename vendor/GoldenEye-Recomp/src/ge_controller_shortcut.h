#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

namespace ge::controller_shortcut {

inline constexpr uint16_t kLeftStickButton = 0x0040;
inline constexpr uint16_t kRightStickButton = 0x0080;
inline constexpr uint16_t kButtonChord =
    kLeftStickButton | kRightStickButton;
inline constexpr uint64_t kHoldDurationMs = 750;
inline constexpr size_t kControllerSlotCount = 4;

struct ControllerSample {
  uint64_t device_id = 0;
  uint16_t buttons = 0;
};

// Recognizes one deliberate L3+R3 hold without consuming either button. A new
// toggle isn't armed until every controller has released the chord, preventing
// one long hold (or two simultaneous holds) from opening and immediately
// closing Host Settings.
class HoldTracker {
 public:
  std::optional<uint32_t> Observe(
      const std::array<ControllerSample, kControllerSlotCount>& controllers,
      uint64_t now_ms) noexcept {
    bool any_chord_held = false;
    std::optional<uint32_t> completed_slot;

    for (size_t slot = 0; slot < controllers.size(); ++slot) {
      if (controllers[slot].device_id != device_ids_[slot]) {
        device_ids_[slot] = controllers[slot].device_id;
        holding_[slot] = false;
        hold_started_ms_[slot] = 0;
      }
      const bool chord_held =
          (controllers[slot].buttons & kButtonChord) == kButtonChord;
      any_chord_held |= chord_held;
      if (!chord_held) {
        holding_[slot] = false;
        continue;
      }

      if (!holding_[slot] || now_ms < hold_started_ms_[slot]) {
        holding_[slot] = true;
        hold_started_ms_[slot] = now_ms;
        continue;
      }
      if (armed_ && !completed_slot &&
          now_ms - hold_started_ms_[slot] >= kHoldDurationMs) {
        completed_slot = static_cast<uint32_t>(slot);
      }
    }

    if (!any_chord_held) {
      armed_ = true;
    } else if (completed_slot) {
      armed_ = false;
    }
    return completed_slot;
  }

  void Reset() noexcept {
    device_ids_.fill(0);
    holding_.fill(false);
    hold_started_ms_.fill(0);
    armed_ = true;
  }

  bool armed() const noexcept { return armed_; }

 private:
  std::array<uint64_t, kControllerSlotCount> device_ids_{};
  std::array<bool, kControllerSlotCount> holding_{};
  std::array<uint64_t, kControllerSlotCount> hold_started_ms_{};
  bool armed_ = true;
};

using ToggleHandler = std::function<void(uint32_t controller_slot)>;

// The game thread detects the chord, while Host Settings must be mutated on the
// UI thread. Holding the registry lock through the short handler call lets app
// shutdown unregister the handler and know no later producer can enqueue work.
class ToggleHandlerRegistry {
 public:
  void Set(ToggleHandler handler) {
    std::lock_guard lock(mutex_);
    handler_ = std::move(handler);
  }

  void Clear() {
    std::lock_guard lock(mutex_);
    handler_ = {};
  }

  bool Request(uint32_t controller_slot) {
    std::lock_guard lock(mutex_);
    if (!handler_) {
      return false;
    }
    handler_(controller_slot);
    return true;
  }

 private:
  std::mutex mutex_;
  ToggleHandler handler_;
};

inline ToggleHandlerRegistry& HandlerRegistry() {
  static ToggleHandlerRegistry registry;
  return registry;
}

inline void SetToggleHandler(ToggleHandler handler) {
  HandlerRegistry().Set(std::move(handler));
}

inline void ClearToggleHandler() { HandlerRegistry().Clear(); }

inline bool RequestToggle(uint32_t controller_slot) {
  return HandlerRegistry().Request(controller_slot);
}

}  // namespace ge::controller_shortcut
