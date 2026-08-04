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
#include <cmath>
#include <array>
#include <filesystem>
#include <limits>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/input/ios_device_motion.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

#include "sdl_virtual_gamepad_harness.h"

REXCVAR_DEFINE_STRING(hid_mappings_file, "", "Input",
                      "Optional path to an additional SDL gamepad mappings file");
REXCVAR_DEFINE_DOUBLE(controller_look_sensitivity, 1.0, "Input/Controller",
                      "Physical controller right-stick look sensitivity")
    .range(rex::input::kControllerLookSensitivityMin, rex::input::kControllerLookSensitivityMax);
REXCVAR_DEFINE_DOUBLE(controller_move_deadzone, 0.0, "Input/Controller",
                      "Physical controller left-stick radial deadzone")
    .range(rex::input::kControllerDeadzoneMin, rex::input::kControllerDeadzoneMax);
REXCVAR_DEFINE_DOUBLE(controller_aim_deadzone, 0.0, "Input/Controller",
                      "Physical controller right-stick radial deadzone")
    .range(rex::input::kControllerDeadzoneMin, rex::input::kControllerDeadzoneMax);
REXCVAR_DEFINE_BOOL(controller_invert_y, false, "Input/Controller",
                    "Invert physical controller vertical look");
REXCVAR_DEFINE_BOOL(controller_rumble_enabled, true, "Input/Controller",
                    "Enable physical controller rumble");
REXCVAR_DEFINE_DOUBLE(controller_rumble_intensity, 1.0, "Input/Controller",
                      "Physical controller rumble intensity")
    .range(rex::input::kControllerRumbleIntensityMin, rex::input::kControllerRumbleIntensityMax);
REXCVAR_DEFINE_STRING(controller_layout, "modern", "Input/Controller",
                      "Physical controller layout preset")
    .allowed({"modern", "classic", "southpaw"});
REXCVAR_DEFINE_STRING(controller_button_map, "", "Input/Controller",
                      "Physical-to-guest controller button overrides")
    .validator([](std::string_view value) {
      rex::input::controller::ButtonBindings bindings;
      return rex::input::controller::ParseButtonBindings(value, &bindings);
    });
REXCVAR_DEFINE_BOOL(controller_gyro_aim, true, "Input/Controller",
                    "Gyro aiming while the left trigger is held, on controllers with a gyro "
                    "(DualShock 4, DualSense, Switch Pro) or via the device's own gyro");
REXCVAR_DEFINE_STRING(controller_gyro_mode, "hold", "Input/Controller",
                      "Gyro aim response: 'hold' maps the tilt held since aiming began to the "
                      "crosshair offset (GoldenEye's aim mode keeps the crosshair where the "
                      "stick holds it); 'rate' maps rotation speed to stick speed")
    .allowed({"hold", "rate"});
REXCVAR_DEFINE_DOUBLE(controller_gyro_sensitivity, 1.0, "Input/Controller",
                      "Gyro aim sensitivity. hold: 1.0 = ~14 degrees of tilt for full stick "
                      "deflection; rate: 1.0 = 2 rad/s for full deflection")
    .range(0.1, 10.0);
REXCVAR_DEFINE_DOUBLE(controller_gyro_deadzone, 0.02, "Input/Controller",
                      "Rotation rates below this (rad/s) are ignored, absorbing hand tremor "
                      "and sensor drift while holding an aim")
    .range(0.0, 0.5);
REXCVAR_DEFINE_BOOL(controller_gyro_recenter_r3, true, "Input/Controller",
                    "Clicking R3 while aiming re-centers the gyro hold offset");

namespace rex::input::sdl {

namespace {

constexpr std::array<uint16_t, SDL_GAMEPAD_BUTTON_COUNT> kXButtonLookup = {
    X_INPUT_GAMEPAD_A,
    X_INPUT_GAMEPAD_B,
    X_INPUT_GAMEPAD_X,
    X_INPUT_GAMEPAD_Y,
    X_INPUT_GAMEPAD_BACK,
    X_INPUT_GAMEPAD_GUIDE,
    X_INPUT_GAMEPAD_START,
    X_INPUT_GAMEPAD_LEFT_THUMB,
    X_INPUT_GAMEPAD_RIGHT_THUMB,
    X_INPUT_GAMEPAD_LEFT_SHOULDER,
    X_INPUT_GAMEPAD_RIGHT_SHOULDER,
    X_INPUT_GAMEPAD_DPAD_UP,
    X_INPUT_GAMEPAD_DPAD_DOWN,
    X_INPUT_GAMEPAD_DPAD_LEFT,
    X_INPUT_GAMEPAD_DPAD_RIGHT,
    X_INPUT_GAMEPAD_GUIDE,  // Share, microphone, or capture button.
    X_INPUT_GAMEPAD_Y,      // Optional right upper paddle.
    X_INPUT_GAMEPAD_B,      // Optional left upper paddle.
    X_INPUT_GAMEPAD_X,      // Optional right lower paddle.
    X_INPUT_GAMEPAD_A,      // Optional left lower paddle.
    X_INPUT_GAMEPAD_GUIDE,  // PlayStation touchpad click.
    0,
    0,
    0,
    0,
    0,
};

bool ApplyAxis(X_INPUT_GAMEPAD& pad, SDL_GamepadAxis axis, int16_t value) {
  switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
      pad.thumb_lx = value;
      return true;
    case SDL_GAMEPAD_AXIS_LEFTY:
      pad.thumb_ly = ~value;
      return true;
    case SDL_GAMEPAD_AXIS_RIGHTX:
      pad.thumb_rx = value;
      return true;
    case SDL_GAMEPAD_AXIS_RIGHTY:
      pad.thumb_ry = ~value;
      return true;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
      pad.left_trigger = static_cast<uint8_t>(std::max<int16_t>(0, value) >> 7);
      return true;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
      pad.right_trigger = static_cast<uint8_t>(std::max<int16_t>(0, value) >> 7);
      return true;
    default:
      return false;
  }
}

bool ApplyButton(X_INPUT_GAMEPAD& pad, SDL_GamepadButton button, bool down) {
  if (button < 0 || static_cast<size_t>(button) >= kXButtonLookup.size()) {
    return false;
  }
  const uint16_t xbutton = kXButtonLookup.at(static_cast<size_t>(button));
  if (!xbutton) {
    return false;
  }
  if (down) {
    if (xbutton == X_INPUT_GAMEPAD_GUIDE && !REXCVAR_GET(guide_button)) {
      return false;
    }
    pad.buttons = static_cast<uint16_t>(pad.buttons) | xbutton;
  } else {
    pad.buttons = static_cast<uint16_t>(pad.buttons) & ~xbutton;
  }
  return true;
}

}  // namespace

SDLInputDriver::SDLInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order),
      sdl_events_initialized_(false),
      SDL_Gamepad_initialized_(false),
      sdl_events_unflushed_(0),
      sdl_pumpevents_queued_(false),
      controllers_(),
      controllers_mutex_(),
      keystroke_states_() {}

SDLInputDriver::~SDLInputDriver() {
  assert_null(attached_window_);
}

X_STATUS SDLInputDriver::Setup() {
  if (!TestSDLVersion()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

void SDLInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  std::lock_guard lifecycle_guard(lifecycle_mutex_);
  if (!window || attached_window_) {
    return;
  }
  attached_window_ = window;
  has_focus_.store(window->HasFocus(), std::memory_order_release);
  window->AddListener(this);
  window->app_context().CallInUIThreadSynchronous([this]() {
    // Register the watch before gamepad initialization so already-connected
    // devices are observed when SDL emits its initial added events.
    if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
      REXLOG_ERROR("SDL: Failed to init events subsystem: {}", SDL_GetError());
      return;
    }
    sdl_events_initialized_ = true;
    pending_events_.reserve(64);
    accepting_events_.store(true, std::memory_order_release);
    if (!SDL_AddEventWatch(EventWatch, this)) {
      REXLOG_ERROR("SDL: Failed to register gamepad event watch: {}", SDL_GetError());
      accepting_events_.store(false, std::memory_order_release);
      SDL_QuitSubSystem(SDL_INIT_EVENTS);
      sdl_events_initialized_ = false;
      return;
    }
    sdl_event_watch_registered_ = true;

#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)
    test_virtual_gamepads_supply_input_.store(false, std::memory_order_release);
    test_virtual_gamepads_ = SDLVirtualGamepadHarness::CreateFromEnvironment();
#endif
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
      REXLOG_ERROR("SDL: Failed to init gamepad subsystem: {}", SDL_GetError());
#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)
      test_virtual_gamepads_.reset();
#endif
      accepting_events_.store(false, std::memory_order_release);
      SDL_RemoveEventWatch(EventWatch, this);
      sdl_event_watch_registered_ = false;
      SDL_QuitSubSystem(SDL_INIT_EVENTS);
      sdl_events_initialized_ = false;
      return;
    }
    SDL_Gamepad_initialized_ = true;

#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)
    if (test_virtual_gamepads_ && !test_virtual_gamepads_->Attach()) {
      REXLOG_ERROR("[vpad] virtual controller harness could not start");
      test_virtual_gamepads_.reset();
    }
#endif

    // Load custom controller mappings if available
    if (!REXCVAR_GET(hid_mappings_file).empty()) {
      std::filesystem::path mappings_path(REXCVAR_GET(hid_mappings_file));
      if (!std::filesystem::exists(mappings_path)) {
        REXLOG_WARN("SDL GameControllerDB: file '{}' does not exist.",
                    REXCVAR_GET(hid_mappings_file));
      } else {
        auto mappings_result =
            SDL_AddGamepadMappingsFromFile(REXCVAR_GET(hid_mappings_file).c_str());
        if (mappings_result < 0) {
          REXLOG_ERROR("SDL GameControllerDB: error loading file '{}': {}.",
                       REXCVAR_GET(hid_mappings_file), mappings_result);
        } else {
          REXLOG_INFO("SDL GameControllerDB: loaded {} mappings.", mappings_result);
        }
      }
    }

    // Do not depend exclusively on queue timing for startup discovery.
    // Added events that are already pending are harmless because opening an
    // instance is idempotent below.
    {
      std::lock_guard guard(controllers_mutex_);
#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)
      if (test_virtual_gamepads_) {
        for (SDL_JoystickID instance_id : test_virtual_gamepads_->instance_ids()) {
          OpenControllerLocked(instance_id);
        }
      }
#endif
      OpenUnassignedControllersLocked();
#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)
      if (test_virtual_gamepads_) {
        const bool all_assigned = std::all_of(
            test_virtual_gamepads_->instance_ids().begin(),
            test_virtual_gamepads_->instance_ids().end(), [this](SDL_JoystickID instance_id) {
              return GetControllerIndexFromInstanceID(instance_id).has_value();
            });
        if (all_assigned && test_virtual_gamepads_->ReportDriverReady()) {
          test_virtual_gamepads_supply_input_.store(true, std::memory_order_release);
        } else {
          REXLOG_ERROR("[vpad] FAILED virtual controllers did not occupy every guest port");
        }
      }
#endif
    }
    ready_.store(true, std::memory_order_release);
    REXLOG_INFO("SDL gamepad input initialized successfully");
  });
}

void SDLInputDriver::OnClosing(rex::ui::UIEvent&) {
  OnWindowUnavailable();
}

void SDLInputDriver::OnWindowUnavailable() {
  std::lock_guard lifecycle_guard(lifecycle_mutex_);
  auto* window = attached_window_;
  if (!window) {
    return;
  }
  ready_.store(false, std::memory_order_release);
  has_focus_.store(false, std::memory_order_release);
  attached_window_ = nullptr;
  window->RemoveListener(this);
  accepting_events_.store(false, std::memory_order_release);
  if (sdl_event_watch_registered_) {
    SDL_RemoveEventWatch(EventWatch, this);
    sdl_event_watch_registered_ = false;
  }
  if (sdl_pumpevents_queued_.load(std::memory_order_acquire)) {
    window->app_context().CallInUIThreadSynchronous(
        [window]() { window->app_context().ExecutePendingFunctionsFromUIThread(); });
  }
  sdl_pumpevents_queued_.store(false, std::memory_order_release);
  {
    std::lock_guard guard(controllers_mutex_);
    for (auto& controller : controllers_) {
      if (controller.sdl) {
        SDL_RumbleGamepad(controller.sdl, 0, 0, 0);
        SDL_CloseGamepad(controller.sdl);
        controller = {};
      }
    }
    keystroke_states_ = {};
  }
#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)
  test_virtual_gamepads_supply_input_.store(false, std::memory_order_release);
  test_virtual_gamepads_.reset();
#endif
  {
    std::lock_guard guard(event_queue_mutex_);
    pending_events_.clear();
  }
  sdl_events_unflushed_.store(0, std::memory_order_release);
  if (SDL_Gamepad_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    SDL_Gamepad_initialized_ = false;
  }
  if (sdl_events_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
    sdl_events_initialized_ = false;
  }
}

void SDLInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  has_focus_.store(false, std::memory_order_release);
  StopRumble();
}

void SDLInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {
  has_focus_.store(true, std::memory_order_release);
}

void SDLInputDriver::OnInputActiveChanged(bool active) {
  host_input_active_.store(active, std::memory_order_release);
  if (!active) {
    StopRumble();
  }
}

X_RESULT SDLInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  (void)flags;
  if (user_index >= HID_SDL_USER_COUNT || !out_caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!ready_.load(std::memory_order_acquire)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Unfortunately drivers can't present all information immediately (e.g.
  // battery information) so this needs to be refreshed every time.
  UpdateXCapabilities(*controller);

  std::memcpy(out_caps, &controller->caps, sizeof(*out_caps));

  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  if (user_index >= HID_SDL_USER_COUNT || !out_state) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!ready_.load(std::memory_order_acquire)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  auto is_active = this->is_active();
#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)
  // Automated controller integration must not depend on whichever desktop app
  // happens to be frontmost. Release builds compile this override out.
  if (test_virtual_gamepads_supply_input_.load(std::memory_order_acquire)) {
    is_active = true;
  }
#endif

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_INPUT_GAMEPAD configured_gamepad = {};
  if (is_active) {
    configured_gamepad = ApplyControllerTuning(controller->state.gamepad);
    ApplyGyroAim(*controller, configured_gamepad);
  }

  // Count changes in the guest-visible state, including hot-reloaded layouts
  // and remaps. Physical changes hidden by a deadzone or unbound control do not
  // need to wake a guest that uses packet_number to skip unchanged samples.
  const bool configured_changed =
      !controller->configured_state_valid ||
      std::memcmp(&configured_gamepad, &controller->last_configured_gamepad,
                  sizeof(configured_gamepad)) != 0;
  if ((is_active != controller->is_active) || configured_changed) {
    controller->state.packet_number++;
  }
  controller->last_configured_gamepad = configured_gamepad;
  controller->configured_state_valid = true;
  controller->is_active = is_active;
  controller->state_changed = false;

  std::memcpy(out_state, &controller->state, sizeof(*out_state));
  // When input is inactive this is the zeroed sample built above. The raw
  // physical state is retained and becomes visible again after reactivation.
  out_state->gamepad = configured_gamepad;
  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  if (user_index >= HID_SDL_USER_COUNT || !vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!ready_.load(std::memory_order_acquire)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  const bool active = is_active();
  const bool enabled = REXCVAR_GET(controller_rumble_enabled);
  const double intensity = REXCVAR_GET(controller_rumble_intensity);
  const uint16_t left = active && enabled
                            ? rex::input::controller::ScaleRumble(
                                  static_cast<uint16_t>(vibration->left_motor_speed), intensity)
                            : 0;
  const uint16_t right = active && enabled
                             ? rex::input::controller::ScaleRumble(
                                   static_cast<uint16_t>(vibration->right_motor_speed), intensity)
                             : 0;
  // SDL clamps longer requests to this duration. XInput vibration updates
  // refresh it during normal play.
  const uint32_t duration = (left || right) ? std::numeric_limits<uint16_t>::max() : 0;
  // Disabling rumble is an intentional successful suppression, even for a pad
  // that has no rumble hardware. Still issue a stop in case the setting was
  // changed while a previous effect was active.
  const X_RESULT result = SetRumbleLocked(*controller, left, right, duration, false);
  return (!enabled || !active) ? X_ERROR_SUCCESS : result;
}

bool SDLInputDriver::GetControllerSnapshot(uint32_t user_index, ControllerSnapshot* out_snapshot) {
  if (!out_snapshot || user_index >= HID_SDL_USER_COUNT ||
      !ready_.load(std::memory_order_acquire)) {
    if (out_snapshot) {
      *out_snapshot = {};
      out_snapshot->user_index = user_index;
    }
    return false;
  }

  QueueControllerUpdate();
  auto guard = DrainAndLock();
  auto* controller = GetControllerState(user_index);
  if (!controller) {
    *out_snapshot = {};
    out_snapshot->user_index = user_index;
    return false;
  }

  out_snapshot->connected = true;
  // Diagnostics may request this from the guest thread. Do not invoke the
  // application active callback here: it may inspect UI-owned overlay state.
  out_snapshot->input_active = host_input_active_.load(std::memory_order_acquire) &&
                               has_focus_.load(std::memory_order_acquire);
  out_snapshot->rumble_supported = controller->rumble_supported;
  out_snapshot->user_index = user_index;
  out_snapshot->device_id = static_cast<uint64_t>(SDL_GetGamepadID(controller->sdl));
  const char* name = SDL_GetGamepadName(controller->sdl);
  out_snapshot->name = name ? name : "Controller";
  out_snapshot->raw_gamepad = controller->state.gamepad;
  out_snapshot->gamepad = ApplyControllerTuning(controller->state.gamepad);
  return true;
}

X_RESULT SDLInputDriver::PlayControllerTestRumble(uint32_t user_index,
                                                  uint64_t expected_device_id) {
  if (user_index >= HID_SDL_USER_COUNT || !ready_.load(std::memory_order_acquire)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!PumpControllerTopologyFromUIThread()) {
    return X_ERROR_FUNCTION_FAILED;
  }
  auto guard = DrainAndLock();
  auto* controller = GetControllerState(user_index);
  if (!controller || !SDL_GamepadConnected(controller->sdl) ||
      (expected_device_id &&
       static_cast<uint64_t>(SDL_GetGamepadID(controller->sdl)) != expected_device_id)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!REXCVAR_GET(controller_rumble_enabled) || !controller->rumble_supported) {
    return X_ERROR_FUNCTION_FAILED;
  }
  constexpr uint16_t kTestStrength = 0x9000;
  constexpr uint32_t kTestDurationMs = 220;
  const uint16_t strength =
      rex::input::controller::ScaleRumble(kTestStrength, REXCVAR_GET(controller_rumble_intensity));
  if (!strength) {
    return X_ERROR_FUNCTION_FAILED;
  }
  return SetRumbleLocked(*controller, strength, strength, kTestDurationMs, true);
}

bool SDLInputDriver::PumpControllerTopologyFromUIThread() {
  std::lock_guard lifecycle_guard(lifecycle_mutex_);
  auto* window = attached_window_;
  if (!window || !ready_.load(std::memory_order_acquire) || !window->app_context().IsInUIThread()) {
    return false;
  }
#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)
  if (test_virtual_gamepads_) {
    test_virtual_gamepads_supply_input_.store(test_virtual_gamepads_->BeforeSDLPump(),
                                              std::memory_order_release);
  }
#endif
  SDL_PumpEvents();
#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)
  if (test_virtual_gamepads_) {
    test_virtual_gamepads_->AfterSDLPump();
  }
#endif
  return true;
}

void SDLInputDriver::SwapSlotsLocked(uint32_t first_user_index, uint32_t second_user_index) {
  auto& first = controllers_.at(first_user_index);
  auto& second = controllers_.at(second_user_index);
  const uint32_t first_packet = static_cast<uint32_t>(first.state.packet_number);
  const uint32_t second_packet = static_cast<uint32_t>(second.state.packet_number);
  std::swap(first, second);
  // Packet numbers belong to guest ports, not physical devices (see
  // SwapControllerSlots).
  first.state.packet_number = first_packet;
  second.state.packet_number = second_packet;
  first.configured_state_valid = false;
  second.configured_state_valid = false;
  for (uint32_t user_index : {first_user_index, second_user_index}) {
    auto& keystroke = keystroke_states_.at(user_index);
    keystroke.repeat_state = RepeatState::Idle;
    keystroke.repeat_butt_idx = 0;
    keystroke.repeat_time = 0;
  }
  if (first.sdl) {
    SDL_SetGamepadPlayerIndex(first.sdl, static_cast<int>(first_user_index));
  }
  if (second.sdl) {
    SDL_SetGamepadPlayerIndex(second.sdl, static_cast<int>(second_user_index));
  }
}

void SDLInputDriver::EnforceVirtualSlotOrderLocked() {
  // A virtual (touch overlay) gamepad must never shadow a physical
  // controller's port - the guest consumes player 1 first - but it must also
  // slide down into vacated slots (the guest treats an empty player 1 as a
  // disconnected controller and waits for it). Slot priority: physical, then
  // virtual, then empty. Physical pads never move down relative to each
  // other (player identity, see OnControllerDeviceRemovedLocked).
  auto is_virtual = [](const ControllerState& c) {
    return c.sdl && SDL_IsJoystickVirtual(SDL_GetGamepadID(c.sdl));
  };
  bool swapped = true;
  while (swapped) {
    swapped = false;
    for (uint32_t i = 0; i + 1 < controllers_.size(); ++i) {
      auto& low = controllers_.at(i);
      auto& high = controllers_.at(i + 1);
      const bool low_empty = !low.sdl;
      const bool low_yields_to_physical = low_empty || is_virtual(low);
      const bool high_physical = high.sdl && !is_virtual(high);
      const bool high_virtual = high.sdl && is_virtual(high);
      if ((low_yields_to_physical && high_physical) || (low_empty && high_virtual)) {
        SwapSlotsLocked(i, i + 1);
        REXLOG_INFO("SDL: {} controller promoted to player {}",
                    high_physical ? "physical" : "virtual", i + 1);
        swapped = true;
      }
    }
  }
}

X_RESULT SDLInputDriver::SwapControllerSlots(uint32_t first_user_index, uint32_t second_user_index,
                                             uint64_t expected_device_id) {
  if (first_user_index >= HID_SDL_USER_COUNT || second_user_index >= HID_SDL_USER_COUNT) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!ready_.load(std::memory_order_acquire)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Port reassignment is a Host Settings UI action. Refresh the topology now,
  // on that same UI thread, rather than relying on the coalesced asynchronous
  // pump used by normal guest polling. This ensures an unplug queued just
  // before the click is applied before expected_device_id is checked.
  if (!PumpControllerTopologyFromUIThread()) {
    return X_ERROR_FUNCTION_FAILED;
  }

  auto guard = DrainAndLock();
  auto& first = controllers_.at(first_user_index);
  auto& second = controllers_.at(second_user_index);
  // The first slot identifies the controller selected by the host UI. If it
  // disconnected while the click was queued, do nothing rather than moving an
  // unrelated destination controller into the stale source slot.
  if (!first.sdl || !SDL_GamepadConnected(first.sdl)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (second.sdl && !SDL_GamepadConnected(second.sdl)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (expected_device_id &&
      static_cast<uint64_t>(SDL_GetGamepadID(first.sdl)) != expected_device_id) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (first_user_index == second_user_index) {
    return X_ERROR_SUCCESS;
  }

  const uint32_t first_packet = static_cast<uint32_t>(first.state.packet_number);
  const uint32_t second_packet = static_cast<uint32_t>(second.state.packet_number);
  SDL_RumbleGamepad(first.sdl, 0, 0, 0);
  if (second.sdl) {
    SDL_RumbleGamepad(second.sdl, 0, 0, 0);
  }
  std::swap(first, second);
  // Packet numbers belong to guest ports, not physical devices. Preserve each
  // port's monotonic sequence and force its next GetState call to publish the
  // newly assigned input as a change.
  first.state.packet_number = first_packet;
  second.state.packet_number = second_packet;
  first.configured_state_valid = false;
  second.configured_state_valid = false;
  for (uint32_t user_index : {first_user_index, second_user_index}) {
    auto& keystroke = keystroke_states_.at(user_index);
    // Preserve prior buttons so GetKeystroke can emit the necessary KEYUP
    // transitions for the newly assigned device, but never carry a repeat
    // timer across physical controllers.
    keystroke.repeat_state = RepeatState::Idle;
    keystroke.repeat_butt_idx = 0;
    keystroke.repeat_time = 0;
  }
  if (first.sdl) {
    SDL_SetGamepadPlayerIndex(first.sdl, static_cast<int>(first_user_index));
  }
  if (second.sdl) {
    SDL_SetGamepadPlayerIndex(second.sdl, static_cast<int>(second_user_index));
  }
  REXLOG_INFO("SDL controller ports swapped: player {} <-> player {}", first_user_index + 1,
              second_user_index + 1);
  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::GetKeystroke(uint32_t users, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  // TODO(JoelLinn): Figure out the flags
  // https://github.com/evilC/UCR/blob/0489929e2a8e39caa3484c67f3993d3fba39e46f/Libraries/XInput.ahk#L85-L98
  (void)flags;
  bool user_any = users == 0xFF;
  if (users >= HID_SDL_USER_COUNT && !user_any) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!out_keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!ready_.load(std::memory_order_acquire)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // The order of this list is also the order in which events are send if
  // multiple buttons change at once.
  static_assert(sizeof(X_INPUT_GAMEPAD::buttons) == 2);
  static constexpr std::array<rex::ui::VirtualKey, 34> kVkLookup = {
      // 00 - True buttons from xinput button field
      rex::ui::VirtualKey::kXInputPadDpadUp,
      rex::ui::VirtualKey::kXInputPadDpadDown,
      rex::ui::VirtualKey::kXInputPadDpadLeft,
      rex::ui::VirtualKey::kXInputPadDpadRight,
      rex::ui::VirtualKey::kXInputPadStart,
      rex::ui::VirtualKey::kXInputPadBack,
      rex::ui::VirtualKey::kXInputPadLThumbPress,
      rex::ui::VirtualKey::kXInputPadRThumbPress,
      rex::ui::VirtualKey::kXInputPadLShoulder,
      rex::ui::VirtualKey::kXInputPadRShoulder,
      rex::ui::VirtualKey::kNone, /* Guide has no VK */
      rex::ui::VirtualKey::kNone, /* Unknown */
      rex::ui::VirtualKey::kXInputPadA,
      rex::ui::VirtualKey::kXInputPadB,
      rex::ui::VirtualKey::kXInputPadX,
      rex::ui::VirtualKey::kXInputPadY,
      // 16 - Fake buttons generated from analog inputs
      rex::ui::VirtualKey::kXInputPadLTrigger,
      rex::ui::VirtualKey::kXInputPadRTrigger,
      // 18
      rex::ui::VirtualKey::kXInputPadLThumbUp,
      rex::ui::VirtualKey::kXInputPadLThumbDown,
      rex::ui::VirtualKey::kXInputPadLThumbRight,
      rex::ui::VirtualKey::kXInputPadLThumbLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownLeft,
      // 26
      rex::ui::VirtualKey::kXInputPadRThumbUp,
      rex::ui::VirtualKey::kXInputPadRThumbDown,
      rex::ui::VirtualKey::kXInputPadRThumbRight,
      rex::ui::VirtualKey::kXInputPadRThumbLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownLeft,
  };

  auto is_active = this->is_active();

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  for (uint32_t user_index = (user_any ? 0 : users);
       user_index < (user_any ? HID_SDL_USER_COUNT : users + 1); user_index++) {
    auto controller = GetControllerState(user_index);
    if (!controller) {
      if (user_any) {
        continue;
      } else {
        return X_ERROR_DEVICE_NOT_CONNECTED;
      }
    }

    // If input is not active (e.g. due to a dialog overlay), force buttons to
    // "unpressed". The algorithm will automatically send UP events when
    // `is_active()` goes low and DOWN events when it goes high again.
    X_INPUT_GAMEPAD guest_gamepad = {};
    if (is_active) {
      guest_gamepad = ApplyControllerTuning(controller->state.gamepad);
    }
    const uint64_t curr_butts =
        is_active ? (static_cast<uint16_t>(guest_gamepad.buttons) | AnalogToKeyfield(guest_gamepad))
                  : uint64_t(0);
    KeystrokeState& last = keystroke_states_.at(user_index);

    // Handle repeating
    auto guest_now = rex::chrono::Clock::QueryGuestUptimeMillis();
    static_assert(HID_SDL_REPEAT_DELAY >= HID_SDL_REPEAT_RATE);
    if (last.repeat_state == RepeatState::Waiting &&
        (last.repeat_time + HID_SDL_REPEAT_DELAY < guest_now)) {
      last.repeat_state = RepeatState::Repeating;
    }
    if (last.repeat_state == RepeatState::Repeating &&
        (last.repeat_time + HID_SDL_REPEAT_RATE < guest_now)) {
      last.repeat_time = guest_now;
      rex::ui::VirtualKey vk = kVkLookup.at(last.repeat_butt_idx);
      assert_true(vk != rex::ui::VirtualKey::kNone);
      out_keystroke->virtual_key = uint16_t(vk);
      out_keystroke->unicode = 0;
      out_keystroke->user_index = user_index;
      out_keystroke->hid_code = 0;
      out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT;
      return X_ERROR_SUCCESS;
    }

    auto butts_changed = curr_butts ^ last.buttons;
    if (!butts_changed) {
      continue;
    }

    // First try to clear buttons with up events. This is to match xinput
    // behaviour when transitioning thumb sticks, e.g. so that THUMB_UPLEFT is
    // up before THUMB_LEFT is down.
    for (auto [clear_pass, i] = std::tuple{true, 0}; i < 2; clear_pass = false, i++) {
      for (uint8_t i = 0; i < uint8_t(std::size(kVkLookup)); i++) {
        auto fbutton = uint64_t(1) << i;
        if (!(butts_changed & fbutton)) {
          continue;
        }
        rex::ui::VirtualKey vk = kVkLookup.at(i);
        if (vk == rex::ui::VirtualKey::kNone) {
          continue;
        }

        out_keystroke->virtual_key = uint16_t(vk);
        out_keystroke->unicode = 0;
        out_keystroke->user_index = user_index;
        out_keystroke->hid_code = 0;

        bool is_pressed = curr_butts & fbutton;
        if (clear_pass && !is_pressed) {
          // up
          out_keystroke->flags = X_INPUT_KEYSTROKE_KEYUP;
          last.buttons &= ~fbutton;
          last.repeat_state = RepeatState::Idle;
          return X_ERROR_SUCCESS;
        }
        if (!clear_pass && is_pressed) {
          // down
          out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN;
          last.buttons |= fbutton;
          last.repeat_state = RepeatState::Waiting;
          last.repeat_butt_idx = i;
          last.repeat_time = guest_now;
          return X_ERROR_SUCCESS;
        }
      }
    }
  }
  return X_ERROR_EMPTY;
}

bool SDLCALL SDLInputDriver::EventWatch(void* userdata, SDL_Event* event) {
  if (!userdata || !event) {
    return false;
  }
  auto* driver = static_cast<SDLInputDriver*>(userdata);
  if (!driver->accepting_events_.load(std::memory_order_acquire)) {
    return false;
  }
  switch (event->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_GAMEPAD_REMAPPED:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      driver->HandleEvent(*event);
      break;
    default:
      break;
  }
  return false;
}

void SDLInputDriver::HandleEvent(const SDL_Event& event) {
  // This callback will likely run on the thread that posts the event, which
  // may be a dedicated thread SDL has created for the joystick subsystem.

  // Event queue should never be (this) full
  assert(SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) < 0xFFFF);

  // The queue could grow up to 3.5MB since it is never polled.
  if (++sdl_events_unflushed_ > 64) {
    SDL_FlushEvents(SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_FINGER_DOWN - 1);
    sdl_events_unflushed_ = 0;
  }

  // Buffer only - no controllers_mutex_ acquisition here.
  // This breaks the lock ordering inversion between controllers_mutex_ and
  // SDL's internal joystick lock that caused deadlocks.
  std::lock_guard<std::mutex> guard(event_queue_mutex_);
  pending_events_.push_back(event);
}

std::unique_lock<std::mutex> SDLInputDriver::DrainAndLock() {
  std::vector<SDL_Event> events;
  {
    std::lock_guard<std::mutex> guard(event_queue_mutex_);
    events.swap(pending_events_);
  }
  std::unique_lock<std::mutex> guard(controllers_mutex_);
  for (const auto& event : events) {
    ProcessEventLocked(event);
  }
  return guard;
}

void SDLInputDriver::ProcessEventLocked(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
      OnControllerDeviceAddedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      OnControllerDeviceRemovedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_REMAPPED:
      OnControllerDeviceRemappedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      OnControllerDeviceAxisMotionLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      OnControllerDeviceButtonChangedLocked(event);
      break;
    default:
      break;
  }
}

void SDLInputDriver::OnControllerDeviceAddedLocked(const SDL_Event& event) {
  OpenControllerLocked(event.gdevice.which);
  EnforceVirtualSlotOrderLocked();
}

bool SDLInputDriver::OpenControllerLocked(SDL_JoystickID instance_id) {
  if (auto existing = GetControllerIndexFromInstanceID(instance_id)) {
    RefreshControllerStateLocked(controllers_.at(*existing));
    UpdateXCapabilities(controllers_.at(*existing));
    return true;
  }

  const auto controller = SDL_OpenGamepad(instance_id);
  if (!controller) {
    REXLOG_WARN("SDL: Could not open gamepad {}: {}", instance_id, SDL_GetError());
    return false;
  }
  REXLOG_INFO(
      "SDL OnControllerDeviceAdded: \"{}\", "
      "JoystickType({}), "
      "GameControllerType({}), "
      "VendorID(0x{:04X}), "
      "ProductID(0x{:04X})",
      SDL_GetGamepadName(controller),
      static_cast<int>(SDL_GetJoystickType(SDL_GetGamepadJoystick(controller))),
      static_cast<int>(SDL_GetGamepadType(controller)), SDL_GetGamepadVendor(controller),
      SDL_GetGamepadProduct(controller));

  if (SDL_GamepadHasSensor(controller, SDL_SENSOR_GYRO)) {
    if (SDL_SetGamepadSensorEnabled(controller, SDL_SENSOR_GYRO, true)) {
      REXLOG_INFO("SDL: gyro available on \"{}\" - gyro aim ready",
                  SDL_GetGamepadName(controller));
    } else {
      REXLOG_WARN("SDL: could not enable gyro on \"{}\": {}", SDL_GetGamepadName(controller),
                  SDL_GetError());
    }
  }

  int user_id = -1;
  // GoldenEye consumes player 1. Always fill slots from zero instead of
  // trusting a remembered host player index that may start at another slot.
  for (size_t i = 0; i < controllers_.size(); i++) {
    if (!controllers_.at(i).sdl) {
      user_id = static_cast<int>(i);
      SDL_SetGamepadPlayerIndex(controller, user_id);
      break;
    }
  }
  if (user_id >= 0) {
    auto& state = controllers_.at(user_id);
    const uint32_t slot_packet = static_cast<uint32_t>(state.state.packet_number);
    state = {};
    state.state.packet_number = slot_packet;
    state.sdl = controller;
    state.state_changed = true;
    RefreshControllerStateLocked(state);
    UpdateXCapabilities(state);

    REXLOG_INFO("SDL OnControllerDeviceAdded: Added at index {}.", user_id);
    return true;
  } else {
    SDL_CloseGamepad(controller);
    REXLOG_WARN("SDL OnControllerDeviceAdded: Ignored. No free slots.");
    return false;
  }
}

void SDLInputDriver::OpenUnassignedControllersLocked() {
  int count = 0;
  SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
  if (!gamepads) {
    return;
  }
  for (int i = 0; i < count; ++i) {
    OpenControllerLocked(gamepads[i]);
  }
  SDL_free(gamepads);
}

void SDLInputDriver::OnControllerDeviceRemovedLocked(const SDL_Event& event) {
  // Find the disconnected gamecontroller and close it.
  auto idx = GetControllerIndexFromInstanceID(event.gdevice.which);
  if (idx) {
    auto& controller = controllers_.at(*idx);
    const uint32_t slot_packet = static_cast<uint32_t>(controller.state.packet_number);
    SDL_CloseGamepad(controller.sdl);
    controller = {};
    controller.state.packet_number = slot_packet;
    keystroke_states_.at(*idx) = {};
    REXLOG_INFO("SDL OnControllerDeviceRemoved: Removed at player index {}.", *idx);
    // Never compact surviving controllers: changing their guest user index
    // mid-match makes physical players take control of different characters.
    // A waiting or newly reconnected pad may claim only the vacated slot.
    OpenUnassignedControllersLocked();
    // Exception: virtual (touch) pads yield to physical ones and may slide
    // down into a vacated lower slot - they have no player identity to keep.
    EnforceVirtualSlotOrderLocked();
  } else {
    REXLOG_DEBUG("SDL OnControllerDeviceRemoved: Ignored unused device.");
  }
}

void SDLInputDriver::OnControllerDeviceRemappedLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gdevice.which);
  if (!idx) {
    OpenControllerLocked(event.gdevice.which);
    return;
  }
  auto& controller = controllers_.at(*idx);
  RefreshControllerStateLocked(controller);
  UpdateXCapabilities(controller);
  keystroke_states_.at(*idx) = {};
  REXLOG_INFO("SDL gamepad mapping refreshed at player index {}.", *idx);
}

void SDLInputDriver::OnControllerDeviceAxisMotionLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gaxis.which);
  if (!idx) {
    return;
  }
  auto& controller = controllers_.at(*idx);
  if (ApplyAxis(controller.state.gamepad, static_cast<SDL_GamepadAxis>(event.gaxis.axis),
                event.gaxis.value)) {
    controller.state_changed = true;
  }
}

void SDLInputDriver::OnControllerDeviceButtonChangedLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gbutton.which);
  if (!idx) {
    return;
  }
  auto& controller = controllers_.at(*idx);
  if (ApplyButton(controller.state.gamepad, static_cast<SDL_GamepadButton>(event.gbutton.button),
                  event.gbutton.down)) {
    controller.state_changed = true;
  }
}

void SDLInputDriver::RefreshControllerStateLocked(ControllerState& controller) {
  assert_not_null(controller.sdl);
  auto& pad = controller.state.gamepad;
  std::memset(&pad, 0, sizeof(pad));
  for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
    ApplyAxis(pad, static_cast<SDL_GamepadAxis>(axis),
              SDL_GetGamepadAxis(controller.sdl, static_cast<SDL_GamepadAxis>(axis)));
  }
  for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
    if (SDL_GetGamepadButton(controller.sdl, static_cast<SDL_GamepadButton>(button))) {
      ApplyButton(pad, static_cast<SDL_GamepadButton>(button), true);
    }
  }
  controller.state_changed = true;
}

void SDLInputDriver::ApplyGyroAim(ControllerState& controller, X_INPUT_GAMEPAD& gamepad) const {
  if (!REXCVAR_GET(controller_gyro_aim) || !controller.sdl) {
    return;
  }
  // Aim gate: GoldenEye's modern layout aims with the left trigger or the
  // left shoulder - either engages the gyro. Checked post-tuning, so custom
  // remaps see the guest-visible controls.
  const bool aiming =
      gamepad.left_trigger >= 32 ||
      (static_cast<uint16_t>(gamepad.buttons) & X_INPUT_GAMEPAD_LEFT_SHOULDER) != 0;
  if (!aiming) {
    controller.gyro_aiming = false;
    controller.gyro_yaw_angle = 0.0;
    controller.gyro_pitch_angle = 0.0;
    return;
  }
  // Look rates in rad/s: positive yaw looks left, positive pitch looks up.
  float yaw_left = 0.0f;
  float pitch_up = 0.0f;
  float rate[3];  // rad/s, right-hand rule, controller held level (SDL convention)
  if (SDL_GetGamepadSensorData(controller.sdl, SDL_SENSOR_GYRO, rate, 3)) {
    yaw_left = rate[1];
    pitch_up = rate[0];
  } else if (!rex::input::ios::GetDeviceGyroLook(&yaw_left, &pitch_up)) {
    // No controller gyro and no device gyro (grip controllers such as the
    // GameSir G8 rely on the attached iPad's own sensor).
    return;
  }

  // Absorb hand tremor and sensor drift.
  const float rate_deadzone = static_cast<float>(REXCVAR_GET(controller_gyro_deadzone));
  if (std::abs(yaw_left) < rate_deadzone) {
    yaw_left = 0.0f;
  }
  if (std::abs(pitch_up) < rate_deadzone) {
    pitch_up = 0.0f;
  }

  const double sensitivity = REXCVAR_GET(controller_gyro_sensitivity);
  const double invert = REXCVAR_GET(controller_invert_y) ? -1.0 : 1.0;
  auto inject = [](int32_t current, double add) {
    return static_cast<int16_t>(
        std::clamp<int32_t>(current + static_cast<int32_t>(std::lround(add)), INT16_MIN,
                            INT16_MAX));
  };

  double yaw_deflection;
  double pitch_deflection;
  if (REXCVAR_GET(controller_gyro_mode) == "rate") {
    // Rotation speed -> stick speed. 2 rad/s = full deflection at 1.0.
    const double scale = sensitivity * (32767.0 / 2.0);
    yaw_deflection = double(yaw_left) * scale;
    pitch_deflection = double(pitch_up) * scale;
  } else {
    // Hold: tilt held since the aim began -> crosshair offset. GoldenEye's
    // aim mode maps stick deflection to crosshair position, so a held tilt
    // must produce a held deflection. ~14 degrees = full deflection at 1.0.
    const uint64_t now_ns = SDL_GetTicksNS();
    const bool recenter =
        REXCVAR_GET(controller_gyro_recenter_r3) &&
        (static_cast<uint16_t>(gamepad.buttons) & X_INPUT_GAMEPAD_RIGHT_THUMB) != 0;
    if (!controller.gyro_aiming || recenter) {
      controller.gyro_aiming = true;
      controller.gyro_yaw_angle = 0.0;
      controller.gyro_pitch_angle = 0.0;
    } else {
      double dt = double(now_ns - controller.gyro_last_time_ns) * 1e-9;
      dt = std::clamp(dt, 0.0, 0.1);
      controller.gyro_yaw_angle += double(yaw_left) * dt;
      controller.gyro_pitch_angle += double(pitch_up) * dt;
    }
    controller.gyro_last_time_ns = now_ns;
    constexpr double kFullDeflectionRad = 0.25;
    const double scale = sensitivity * (32767.0 / kFullDeflectionRad);
    yaw_deflection = controller.gyro_yaw_angle * scale;
    pitch_deflection = controller.gyro_pitch_angle * scale;
  }
  gamepad.thumb_rx = inject(int16_t(gamepad.thumb_rx), -yaw_deflection);
  gamepad.thumb_ry = inject(int16_t(gamepad.thumb_ry), invert * pitch_deflection);
}

X_INPUT_GAMEPAD SDLInputDriver::ApplyControllerTuning(const X_INPUT_GAMEPAD& gamepad) const {
  rex::input::controller::Layout layout = rex::input::controller::Layout::kModern;
  rex::input::controller::ParseLayout(rex::cvar::GetFlagByName(rex::input::kControllerLayoutCvar),
                                      &layout);

  rex::input::controller::ButtonBindings bindings;
  rex::input::controller::ParseButtonBindings(
      rex::cvar::GetFlagByName(rex::input::kControllerButtonMapCvar), &bindings);

  rex::input::ControllerTuning tuning;
  tuning.look_sensitivity = REXCVAR_GET(controller_look_sensitivity);
  tuning.move_deadzone = REXCVAR_GET(controller_move_deadzone);
  tuning.aim_deadzone = REXCVAR_GET(controller_aim_deadzone);
  tuning.invert_y = REXCVAR_GET(controller_invert_y);
  return rex::input::controller::ApplyTuning(
      rex::input::controller::ApplyMapping(gamepad, layout, bindings), tuning);
}

X_RESULT SDLInputDriver::SetRumbleLocked(ControllerState& controller, uint16_t left, uint16_t right,
                                         uint32_t duration_ms, bool host_test) {
  if (!controller.rumble_supported && !left && !right) {
    return X_ERROR_SUCCESS;
  }
  if (SDL_RumbleGamepad(controller.sdl, left, right, duration_ms)) {
    return X_ERROR_SUCCESS;
  }
  if (!controller.rumble_failure_logged) {
    controller.rumble_failure_logged = true;
    const char* name = SDL_GetGamepadName(controller.sdl);
    const char* error = SDL_GetError();
    REXLOG_WARN("SDL: Controller '{}' rejected {} rumble: {}", name ? name : "Controller",
                host_test ? "test" : "game", error && *error ? error : "no device detail");
  }
  return X_ERROR_FUNCTION_FAILED;
}

std::optional<size_t> SDLInputDriver::GetControllerIndexFromInstanceID(SDL_JoystickID instance_id) {
  // Loop through our controllers and try to match the given ID.
  for (size_t i = 0; i < controllers_.size(); i++) {
    auto controller = controllers_.at(i).sdl;
    if (!controller) {
      continue;
    }
    auto joystick = SDL_GetGamepadJoystick(controller);
    if (!joystick) {
      continue;
    }
    auto joy_instance_id = SDL_GetJoystickID(joystick);
    if (joy_instance_id == instance_id) {
      return i;
    }
  }
  return std::nullopt;
}

SDLInputDriver::ControllerState* SDLInputDriver::GetControllerState(uint32_t user_index) {
  if (user_index >= controllers_.size()) {
    return nullptr;
  }
  auto controller = &controllers_.at(user_index);
  if (!controller->sdl) {
    return nullptr;
  }
  return controller;
}

bool SDLInputDriver::TestSDLVersion() const {
  REXLOG_INFO("SDL: Using version {}.{}.{}", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
              SDL_MICRO_VERSION);
  return true;
}

void SDLInputDriver::UpdateXCapabilities(ControllerState& state) {
  assert(state.sdl);
  uint16_t cap_flags = 0x0;

  // The RAWINPUT driver combines and enhances input from different APIs. For
  // details, see `SDL_rawinputjoystick.c`. This correlation however has latency
  // which might confuse games calling `GetCapabilities()` (The power level is
  // only available after the controller has been "touched"). Generally that
  // should not be a problem, when in doubt disable the RAWINPUT driver via hint
  // (env var).

  if (SDL_GetJoystickConnectionState(SDL_GetGamepadJoystick(state.sdl)) ==
      SDL_JOYSTICK_CONNECTION_WIRELESS) {
    cap_flags |= X_INPUT_CAPS_WIRELESS;
  }

  const SDL_PropertiesID properties = SDL_GetGamepadProperties(state.sdl);
  state.rumble_supported =
      properties && SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
  if (state.rumble_supported) {
    cap_flags |= X_INPUT_CAPS_FFB_SUPPORTED;
  }

  // Check if all navigational buttons are present
  static constexpr std::array<SDL_GamepadButton, 6> nav_buttons = {
      SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,      SDL_GAMEPAD_BUTTON_DPAD_UP,
      SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
  };
  for (auto it = nav_buttons.begin(); it < nav_buttons.end(); it++) {
    if (!SDL_GamepadHasButton(state.sdl, *it)) {
      cap_flags |= X_INPUT_CAPS_NO_NAVIGATION;
      break;
    }
  }

  auto& c = state.caps;
  c.type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
  c.sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
  c.flags = cap_flags;
  c.gamepad.buttons = 0xF3FF | (REXCVAR_GET(guide_button) ? X_INPUT_GAMEPAD_GUIDE : 0x0);
  c.gamepad.left_trigger = 0xFF;
  c.gamepad.right_trigger = 0xFF;
  c.gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
  c.vibration.left_motor_speed = 0xFFFFu;
  c.vibration.right_motor_speed = 0xFFFFu;
}

void SDLInputDriver::StopRumble() {
  if (!ready_.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard guard(controllers_mutex_);
  for (auto& controller : controllers_) {
    if (controller.sdl) {
      SDL_RumbleGamepad(controller.sdl, 0, 0, 0);
    }
  }
}

void SDLInputDriver::QueueControllerUpdate() {
  // Pump SDL events to ensure controller state is up to date.
  if (!ready_.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard lifecycle_guard(lifecycle_mutex_);
  auto* window = attached_window_;
  if (!window || !ready_.load(std::memory_order_acquire)) {
    return;
  }
  bool is_queued = false;
  sdl_pumpevents_queued_.compare_exchange_strong(is_queued, true);
  if (!is_queued) {
    if (!window->app_context().CallInUIThread([this]() {
          if (ready_.load(std::memory_order_acquire)) {
#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)
            if (test_virtual_gamepads_) {
              test_virtual_gamepads_supply_input_.store(test_virtual_gamepads_->BeforeSDLPump(),
                                                        std::memory_order_release);
            }
#endif
            SDL_PumpEvents();
#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)
            if (test_virtual_gamepads_) {
              test_virtual_gamepads_->AfterSDLPump();
            }
#endif
          }
          sdl_pumpevents_queued_.store(false, std::memory_order_release);
        })) {
      sdl_pumpevents_queued_.store(false, std::memory_order_release);
    }
  }
}

// Check if the analog inputs exceed their thresholds to become a button press
// and build the bitfield.
inline uint64_t SDLInputDriver::AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const {
  uint64_t f = 0;

  f |= static_cast<uint64_t>(gamepad.left_trigger > HID_SDL_TRIGG_THRES) << 16;
  f |= static_cast<uint64_t>(gamepad.right_trigger > HID_SDL_TRIGG_THRES) << 17;

  auto thumb_x = static_cast<int16_t>(gamepad.thumb_lx);
  auto thumb_y = static_cast<int16_t>(gamepad.thumb_ly);
  for (size_t i = 0; i <= 8; i = i + 8) {
    uint64_t u = thumb_y > HID_SDL_THUMB_THRES;
    uint64_t d = thumb_y < ~HID_SDL_THUMB_THRES;
    uint64_t r = thumb_x > HID_SDL_THUMB_THRES;
    uint64_t l = thumb_x < ~HID_SDL_THUMB_THRES;
    if (u && l) {
      u = l = 0;
      f |= uint64_t(1) << (22 + i);
    }
    if (u && r) {
      u = r = 0;
      f |= uint64_t(1) << (23 + i);
    }
    if (d && r) {
      d = r = 0;
      f |= uint64_t(1) << (24 + i);
    }
    if (d && l) {
      d = l = 0;
      f |= uint64_t(1) << (25 + i);
    }
    f |= u << (18 + i);
    f |= d << (19 + i);
    f |= r << (20 + i);
    f |= l << (21 + i);

    thumb_x = static_cast<int16_t>(gamepad.thumb_rx);
    thumb_y = static_cast<int16_t>(gamepad.thumb_ry);
  }
  return f;
}

}  // namespace rex::input::sdl
