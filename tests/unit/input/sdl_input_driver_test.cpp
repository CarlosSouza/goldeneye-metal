#include <rex/cvar.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)
#include <unistd.h>
#endif

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::X_INPUT_GAMEPAD_A;
using rex::input::X_INPUT_GAMEPAD_B;
using rex::input::X_INPUT_GAMEPAD_DPAD_UP;
using rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER;
using rex::input::X_INPUT_GAMEPAD_START;
using rex::input::X_INPUT_GAMEPAD_X;
using rex::input::X_INPUT_GAMEPAD_Y;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

REXCVAR_DECLARE(double, controller_look_sensitivity);
REXCVAR_DECLARE(double, controller_move_deadzone);
REXCVAR_DECLARE(double, controller_aim_deadzone);
REXCVAR_DECLARE(bool, controller_invert_y);
REXCVAR_DECLARE(bool, controller_rumble_enabled);
REXCVAR_DECLARE(double, controller_rumble_intensity);
REXCVAR_DECLARE(std::string, controller_layout);
REXCVAR_DECLARE(std::string, controller_button_map);

namespace {

constexpr uint16_t kVirtualVendor = 0xCAFE;
constexpr uint16_t kVirtualProduct = 0x0001;

class TestAppContext final : public rex::ui::WindowedAppContext {
 protected:
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};

class TestWindow final : public rex::ui::Window {
 public:
  explicit TestWindow(TestAppContext& context) : Window(context, "SDL input test", 1280, 720) {}
  ~TestWindow() override { EnterDestructor(); }

 protected:
  bool OpenImpl() override {
    WindowDestructionReceiver receiver(this);
    OnActualSizeUpdate(1280, 720, receiver);
    if (!receiver.IsWindowDestroyed()) {
      OnFocusUpdate(true, receiver);
    }
    return !receiver.IsWindowDestroyed();
  }

  void RequestCloseImpl() override {
    WindowDestructionReceiver receiver(this);
    OnBeforeClose(receiver);
    if (!receiver.IsWindowDestroyed()) {
      OnAfterClose();
    }
  }

  void ApplyNewMouseCapture() override {}
  void ApplyNewMouseRelease() override {}
  std::unique_ptr<rex::ui::Surface> CreateSurfaceImpl(rex::ui::Surface::TypeFlags) override {
    return nullptr;
  }
  void RequestPaintImpl() override {}
};

struct RumbleRecord {
  bool accept = true;
  uint32_t calls = 0;
  uint16_t left = 0;
  uint16_t right = 0;
};

bool SDLCALL RecordRumble(void* userdata, uint16_t left, uint16_t right) {
  auto* record = static_cast<RumbleRecord*>(userdata);
  ++record->calls;
  record->left = left;
  record->right = right;
  return record->accept;
}

class VirtualGamepad {
 public:
  explicit VirtualGamepad(RumbleRecord* rumble = nullptr) {
    SDL_VirtualJoystickDesc description;
    SDL_INIT_INTERFACE(&description);
    description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    description.vendor_id = kVirtualVendor;
    description.product_id = kVirtualProduct;
    description.naxes = SDL_GAMEPAD_AXIS_COUNT;
    description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    description.name = "GoldenEye virtual gamepad";
    description.userdata = rumble;
    description.Rumble = rumble ? RecordRumble : nullptr;

    instance_id_ = SDL_AttachVirtualJoystick(&description);
    if (!instance_id_) {
      throw std::runtime_error(std::string("SDL_AttachVirtualJoystick failed: ") + SDL_GetError());
    }
    joystick_ = SDL_OpenJoystick(instance_id_);
    if (!joystick_) {
      SDL_DetachVirtualJoystick(instance_id_);
      instance_id_ = 0;
      throw std::runtime_error(std::string("SDL_OpenJoystick failed: ") + SDL_GetError());
    }
  }

  ~VirtualGamepad() { Detach(); }

  VirtualGamepad(const VirtualGamepad&) = delete;
  VirtualGamepad& operator=(const VirtualGamepad&) = delete;

  void SetButton(SDL_GamepadButton button, bool down) {
    if (!SDL_SetJoystickVirtualButton(joystick_, static_cast<int>(button), down)) {
      throw std::runtime_error(std::string("SDL_SetJoystickVirtualButton failed: ") +
                               SDL_GetError());
    }
  }

  void SetAxis(SDL_GamepadAxis axis, int16_t value) {
    if (!SDL_SetJoystickVirtualAxis(joystick_, static_cast<int>(axis), value)) {
      throw std::runtime_error(std::string("SDL_SetJoystickVirtualAxis failed: ") + SDL_GetError());
    }
  }

  void Commit() { SDL_UpdateJoysticks(); }

  void Detach() {
    if (joystick_) {
      SDL_CloseJoystick(joystick_);
      joystick_ = nullptr;
    }
    if (instance_id_) {
      SDL_DetachVirtualJoystick(instance_id_);
      instance_id_ = 0;
    }
  }

 private:
  SDL_JoystickID instance_id_ = 0;
  SDL_Joystick* joystick_ = nullptr;
};

class ScopedSDLGamepadSubsystem {
 public:
  ScopedSDLGamepadSubsystem() {
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
      throw std::runtime_error(std::string("SDL gamepad initialization failed: ") + SDL_GetError());
    }
  }
  ~ScopedSDLGamepadSubsystem() { SDL_QuitSubSystem(SDL_INIT_GAMEPAD); }
};

class SDLDriverFixture {
 public:
  SDLDriverFixture() : window(context), driver(nullptr, 0) {
    if (!SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0xCAFE/0x0001")) {
      throw std::runtime_error("Could not isolate SDL virtual gamepads");
    }
    if (!window.Open()) {
      throw std::runtime_error("Could not open the SDL input test window");
    }
    if (driver.Setup() != X_STATUS_SUCCESS) {
      throw std::runtime_error("Could not set up the SDL input driver");
    }
    driver.OnWindowAvailable(&window);
  }

  ~SDLDriverFixture() {
    driver.OnWindowUnavailable();
    SDL_ResetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT);
  }

  X_RESULT Poll(uint32_t user_index, X_INPUT_STATE& state) {
    for (int attempt = 0; attempt < 8; ++attempt) {
      SDL_PumpEvents();
      context.ExecutePendingFunctionsFromUIThread();
      const X_RESULT result = driver.GetState(user_index, &state);
      if (result == X_ERROR_SUCCESS) {
        return result;
      }
    }
    return driver.GetState(user_index, &state);
  }

  X_RESULT PollDisconnected(uint32_t user_index, X_INPUT_STATE& state) {
    X_RESULT result = X_ERROR_SUCCESS;
    for (int attempt = 0; attempt < 8; ++attempt) {
      SDL_PumpEvents();
      context.ExecutePendingFunctionsFromUIThread();
      result = driver.GetState(user_index, &state);
      if (result == X_ERROR_DEVICE_NOT_CONNECTED) {
        break;
      }
    }
    return result;
  }

  bool Snapshot(uint32_t user_index, rex::input::ControllerSnapshot& snapshot) {
    for (int attempt = 0; attempt < 8; ++attempt) {
      SDL_PumpEvents();
      context.ExecutePendingFunctionsFromUIThread();
      if (driver.GetControllerSnapshot(user_index, &snapshot)) {
        return true;
      }
    }
    return driver.GetControllerSnapshot(user_index, &snapshot);
  }

  TestAppContext context;
  TestWindow window;
  rex::input::sdl::SDLInputDriver driver;
};

#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)

class ScopedHarnessEnvironment {
 public:
  explicit ScopedHarnessEnvironment(int command_fd, uint32_t pad_count = 2) {
    Save("REX_INPUT_TEST_HARNESS");
    Save("REX_TEST_VIRTUAL_GAMEPADS");
    Save("REX_TEST_VIRTUAL_GAMEPAD_FD");
    const std::string descriptor = std::to_string(command_fd);
    setenv("REX_INPUT_TEST_HARNESS", "1", 1);
    const std::string count = std::to_string(pad_count);
    setenv("REX_TEST_VIRTUAL_GAMEPADS", count.c_str(), 1);
    setenv("REX_TEST_VIRTUAL_GAMEPAD_FD", descriptor.c_str(), 1);
  }

  ~ScopedHarnessEnvironment() {
    for (const auto& [name, value] : saved_) {
      if (value) {
        setenv(name.c_str(), value->c_str(), 1);
      } else {
        unsetenv(name.c_str());
      }
    }
  }

 private:
  void Save(const char* name) {
    const char* value = std::getenv(name);
    saved_.emplace_back(name, value ? std::optional<std::string>(value) : std::nullopt);
  }

  std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
};

#endif

}  // namespace

#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)

TEST_CASE("SDL developer harness drives one gameplay pad through the normal input path",
          "[input][sdl][gameplay][harness]") {
  int command_pipe[2] = {-1, -1};
  REQUIRE(pipe(command_pipe) == 0);
  struct ClosePipe {
    int* descriptors;
    ~ClosePipe() {
      for (size_t index = 0; index < 2; ++index) {
        if (descriptors[index] >= 0) {
          close(descriptors[index]);
        }
      }
    }
  } close_pipe{command_pipe};
  ScopedHarnessEnvironment environment(command_pipe[0], 1);

  {
    SDLDriverFixture fixture;
    command_pipe[0] = -1;
    // SDL's vertical axes are negative when the stick is pushed up. The input
    // driver converts that to the positive Xbox Y convention used by the game.
    const std::string command = "SET_AXIS 1 1 LY -24000\n";
    REQUIRE(write(command_pipe[1], command.data(), command.size()) ==
            static_cast<ssize_t>(command.size()));

    X_INPUT_STATE state = {};
    REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
    fixture.context.ExecutePendingFunctionsFromUIThread();
    REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
    CHECK(static_cast<int16_t>(state.gamepad.thumb_ly) == 23999);
    CHECK(fixture.Poll(1, state) == X_ERROR_DEVICE_NOT_CONNECTED);
  }
}

TEST_CASE("SDL developer harness drives two pads through the normal input path",
          "[input][sdl][multiplayer][harness]") {
  int command_pipe[2] = {-1, -1};
  REQUIRE(pipe(command_pipe) == 0);
  struct ClosePipe {
    int* descriptors;
    ~ClosePipe() {
      for (size_t index = 0; index < 2; ++index) {
        if (descriptors[index] >= 0) {
          close(descriptors[index]);
        }
      }
    }
  } close_pipe{command_pipe};
  ScopedHarnessEnvironment environment(command_pipe[0]);

  {
    SDLDriverFixture fixture;
    // The harness owns the inherited read side after successful activation.
    command_pipe[0] = -1;

    const std::string commands =
        "SET_BUTTON 1 1 SOUTH 1\n"
        "SET_BUTTON 2 2 EAST 1\n"
        "SET_AXIS 3 2 LX 12345\n";
    REQUIRE(write(command_pipe[1], commands.data(), commands.size()) ==
            static_cast<ssize_t>(commands.size()));

    X_INPUT_STATE state = {};
    REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
    fixture.context.ExecutePendingFunctionsFromUIThread();
    REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
    CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_A) != 0);
    REQUIRE(fixture.Poll(1, state) == X_ERROR_SUCCESS);
    CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_B) != 0);
    CHECK(static_cast<int16_t>(state.gamepad.thumb_lx) == 12345);

    const std::string reset_and_pulse =
        "RESET 4 ALL\n"
        "PULSE_BUTTON 5 2 START 20\n";
    REQUIRE(write(command_pipe[1], reset_and_pulse.data(), reset_and_pulse.size()) ==
            static_cast<ssize_t>(reset_and_pulse.size()));
    REQUIRE(fixture.Poll(1, state) == X_ERROR_SUCCESS);
    fixture.context.ExecutePendingFunctionsFromUIThread();
    REQUIRE(fixture.Poll(1, state) == X_ERROR_SUCCESS);
    CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_START) != 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    fixture.context.ExecutePendingFunctionsFromUIThread();
    REQUIRE(fixture.Poll(1, state) == X_ERROR_SUCCESS);
    fixture.context.ExecutePendingFunctionsFromUIThread();
    REQUIRE(fixture.Poll(1, state) == X_ERROR_SUCCESS);
    CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_START) == 0);

    // A malformed pulse must not alter input or consume its sequence number.
    const std::string rejected_axis = "PULSE_AXIS 6 2 LX 20000 0\n";
    REQUIRE(write(command_pipe[1], rejected_axis.data(), rejected_axis.size()) ==
            static_cast<ssize_t>(rejected_axis.size()));
    fixture.context.ExecutePendingFunctionsFromUIThread();
    REQUIRE(fixture.Poll(1, state) == X_ERROR_SUCCESS);
    fixture.context.ExecutePendingFunctionsFromUIThread();
    REQUIRE(fixture.Poll(1, state) == X_ERROR_SUCCESS);
    CHECK(static_cast<int16_t>(state.gamepad.thumb_lx) == 0);

    const std::string retry_axis = "SET_AXIS 6 2 LX 22222\n";
    REQUIRE(write(command_pipe[1], retry_axis.data(), retry_axis.size()) ==
            static_cast<ssize_t>(retry_axis.size()));
    fixture.context.ExecutePendingFunctionsFromUIThread();
    REQUIRE(fixture.Poll(1, state) == X_ERROR_SUCCESS);
    fixture.context.ExecutePendingFunctionsFromUIThread();
    REQUIRE(fixture.Poll(1, state) == X_ERROR_SUCCESS);
    CHECK(static_cast<int16_t>(state.gamepad.thumb_lx) == 22222);
  }
}

TEST_CASE("SDL developer harness hotplugs a pad into its vacant guest slot",
          "[input][sdl][multiplayer][harness][hotplug]") {
  int command_pipe[2] = {-1, -1};
  REQUIRE(pipe(command_pipe) == 0);
  struct ClosePipe {
    int* descriptors;
    ~ClosePipe() {
      for (size_t index = 0; index < 2; ++index) {
        if (descriptors[index] >= 0) {
          close(descriptors[index]);
        }
      }
    }
  } close_pipe{command_pipe};
  ScopedHarnessEnvironment environment(command_pipe[0]);

  {
    SDLDriverFixture fixture;
    command_pipe[0] = -1;

    const std::string initial_input =
        "SET_BUTTON 1 1 SOUTH 1\n"
        "SET_BUTTON 2 2 EAST 1\n";
    REQUIRE(write(command_pipe[1], initial_input.data(), initial_input.size()) ==
            static_cast<ssize_t>(initial_input.size()));

    X_INPUT_STATE state = {};
    REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
    fixture.context.ExecutePendingFunctionsFromUIThread();
    REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
    CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_A) != 0);
    REQUIRE(fixture.Poll(1, state) == X_ERROR_SUCCESS);
    CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_B) != 0);

    const std::string disconnect = "DISCONNECT 3 1\n";
    REQUIRE(write(command_pipe[1], disconnect.data(), disconnect.size()) ==
            static_cast<ssize_t>(disconnect.size()));
    REQUIRE(fixture.PollDisconnected(0, state) == X_ERROR_DEVICE_NOT_CONNECTED);
    REQUIRE(fixture.Poll(1, state) == X_ERROR_SUCCESS);
    CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_B) != 0);

    // Input cannot be applied to a detached pad, and a rejected command must
    // not consume its sequence number.
    const std::string rejected_input = "SET_AXIS 4 1 LX 18000\n";
    REQUIRE(write(command_pipe[1], rejected_input.data(), rejected_input.size()) ==
            static_cast<ssize_t>(rejected_input.size()));
    REQUIRE(fixture.PollDisconnected(0, state) == X_ERROR_DEVICE_NOT_CONNECTED);

    const std::string reconnect = "CONNECT 4 1\n";
    REQUIRE(write(command_pipe[1], reconnect.data(), reconnect.size()) ==
            static_cast<ssize_t>(reconnect.size()));
    REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
    fixture.context.ExecutePendingFunctionsFromUIThread();
    REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
    CHECK(state.gamepad.buttons == 0);
    CHECK(state.gamepad.left_trigger == 0);
    CHECK(state.gamepad.right_trigger == 0);
    CHECK(state.gamepad.thumb_lx == 0);
    CHECK(state.gamepad.thumb_ly >= -1);
    CHECK(state.gamepad.thumb_ly <= 1);
    CHECK(state.gamepad.thumb_rx == 0);
    CHECK(state.gamepad.thumb_ry >= -1);
    CHECK(state.gamepad.thumb_ry <= 1);

    // The reattached logical player reclaims the original vacancy without
    // renumbering the surviving player, then accepts fresh input normally.
    REQUIRE(fixture.Poll(1, state) == X_ERROR_SUCCESS);
    CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_B) != 0);
    const std::string reconnected_input =
        "SET_AXIS 5 1 LX 18000\n"
        "SET_BUTTON 6 1 NORTH 1\n";
    REQUIRE(write(command_pipe[1], reconnected_input.data(), reconnected_input.size()) ==
            static_cast<ssize_t>(reconnected_input.size()));
    fixture.context.ExecutePendingFunctionsFromUIThread();
    REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
    fixture.context.ExecutePendingFunctionsFromUIThread();
    REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
    CHECK(static_cast<int16_t>(state.gamepad.thumb_lx) == 18000);
    CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_Y) != 0);
    REQUIRE(fixture.Poll(1, state) == X_ERROR_SUCCESS);
    CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_B) != 0);
  }
}

#endif

TEST_CASE("SDL virtual gamepad maps modern controller controls and activity", "[input][sdl]") {
  SDLDriverFixture fixture;
  bool active = true;
  fixture.driver.set_is_active_callback([&active] { return active; });

  VirtualGamepad gamepad;
  X_INPUT_STATE state = {};
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);

  gamepad.SetButton(SDL_GAMEPAD_BUTTON_SOUTH, true);
  gamepad.SetButton(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
  gamepad.SetButton(SDL_GAMEPAD_BUTTON_DPAD_UP, true);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_LEFTX, 12345);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_LEFTY, -20000);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_RIGHTX, -16000);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_RIGHTY, 7000);
  // Virtual joystick triggers use the raw -32768..32767 range; zero maps to
  // the midpoint of SDL's normalized 0..32767 gamepad-trigger range.
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
  gamepad.Commit();

  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  CHECK((state.gamepad.buttons & X_INPUT_GAMEPAD_A) != 0);
  CHECK((state.gamepad.buttons & X_INPUT_GAMEPAD_DPAD_UP) != 0);
  CHECK((state.gamepad.buttons & X_INPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
  CHECK(state.gamepad.thumb_lx == 12345);
  CHECK(state.gamepad.thumb_ly == 19999);
  CHECK(state.gamepad.thumb_rx == -16000);
  CHECK(state.gamepad.thumb_ry == -7001);
  CHECK(state.gamepad.left_trigger == 127);
  CHECK(state.gamepad.right_trigger == 255);

  gamepad.SetButton(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false);
  gamepad.Commit();
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  CHECK((state.gamepad.buttons & X_INPUT_GAMEPAD_RIGHT_SHOULDER) == 0);

  active = false;
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  CHECK(state.gamepad.buttons == 0);
  CHECK(state.gamepad.thumb_lx == 0);
  CHECK(state.gamepad.left_trigger == 0);

  active = true;
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  CHECK((state.gamepad.buttons & X_INPUT_GAMEPAD_A) != 0);
  CHECK(state.gamepad.thumb_lx == 12345);
}

TEST_CASE("SDL gamepad discovery includes a controller connected before driver startup",
          "[input][sdl]") {
  REQUIRE(SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0xCAFE/0x0001"));
  ScopedSDLGamepadSubsystem subsystem;
  VirtualGamepad gamepad;
  gamepad.SetButton(SDL_GAMEPAD_BUTTON_SOUTH, true);
  gamepad.Commit();

  SDLDriverFixture fixture;
  X_INPUT_STATE state = {};
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  CHECK((state.gamepad.buttons & X_INPUT_GAMEPAD_A) != 0);
}

TEST_CASE("SDL keeps four controller ports isolated across disconnect and reconnect",
          "[input][sdl][multiplayer]") {
  const bool old_rumble_enabled = REXCVAR_GET(controller_rumble_enabled);
  const double old_rumble_intensity = REXCVAR_GET(controller_rumble_intensity);
  struct RestoreRumbleCvars {
    bool enabled;
    double intensity;
    ~RestoreRumbleCvars() {
      REXCVAR_SET(controller_rumble_enabled, enabled);
      REXCVAR_SET(controller_rumble_intensity, intensity);
    }
  } restore{old_rumble_enabled, old_rumble_intensity};
  REXCVAR_SET(controller_rumble_enabled, true);
  REXCVAR_SET(controller_rumble_intensity, 1.0);

  SDLDriverFixture fixture;
  std::array<RumbleRecord, 4> rumble_records = {};
  std::vector<std::unique_ptr<VirtualGamepad>> gamepads;
  gamepads.reserve(rumble_records.size());
  for (auto& record : rumble_records) {
    gamepads.push_back(std::make_unique<VirtualGamepad>(&record));
  }

  constexpr std::array<SDL_GamepadButton, 4> physical_buttons = {
      SDL_GAMEPAD_BUTTON_SOUTH,
      SDL_GAMEPAD_BUTTON_EAST,
      SDL_GAMEPAD_BUTTON_WEST,
      SDL_GAMEPAD_BUTTON_NORTH,
  };
  constexpr std::array<uint16_t, 4> guest_buttons = {
      X_INPUT_GAMEPAD_A,
      X_INPUT_GAMEPAD_B,
      X_INPUT_GAMEPAD_X,
      X_INPUT_GAMEPAD_Y,
  };
  for (size_t i = 0; i < gamepads.size(); ++i) {
    gamepads[i]->SetButton(physical_buttons[i], true);
    gamepads[i]->Commit();
  }

  X_INPUT_STATE state = {};
  std::array<uint32_t, 4> initial_packets = {};
  std::array<uint64_t, 4> device_ids = {};
  for (uint32_t user = 0; user < 4; ++user) {
    REQUIRE(fixture.Poll(user, state) == X_ERROR_SUCCESS);
    initial_packets[user] = static_cast<uint32_t>(state.packet_number);
    rex::input::ControllerSnapshot snapshot;
    REQUIRE(fixture.Snapshot(user, snapshot));
    device_ids[user] = snapshot.device_id;
    CHECK(device_ids[user] != 0);
    for (uint32_t earlier = 0; earlier < user; ++earlier) {
      CHECK(device_ids[user] != device_ids[earlier]);
    }
    for (uint32_t button = 0; button < 4; ++button) {
      CHECK(((static_cast<uint16_t>(state.gamepad.buttons) & guest_buttons[button]) != 0) ==
            (button == user));
    }
  }

  rex::input::X_INPUT_KEYSTROKE keystroke = {};
  REQUIRE(fixture.driver.GetKeystroke(0, 0, &keystroke) == X_ERROR_SUCCESS);
  CHECK(keystroke.virtual_key == static_cast<uint16_t>(rex::ui::VirtualKey::kXInputPadA));
  CHECK((keystroke.flags & rex::input::X_INPUT_KEYSTROKE_KEYDOWN) != 0);

  // Rumble must follow the same guest port without touching any other pad.
  for (uint32_t user = 0; user < 4; ++user) {
    std::array<uint32_t, 4> calls_before = {};
    for (size_t i = 0; i < rumble_records.size(); ++i) {
      calls_before[i] = rumble_records[i].calls;
    }
    X_INPUT_VIBRATION vibration = {};
    vibration.left_motor_speed = static_cast<uint16_t>(0x1100 + user);
    vibration.right_motor_speed = static_cast<uint16_t>(0x2200 + user);
    REQUIRE(fixture.driver.SetState(user, &vibration) == X_ERROR_SUCCESS);
    for (uint32_t controller = 0; controller < 4; ++controller) {
      CHECK(rumble_records[controller].calls ==
            calls_before[controller] + (controller == user ? 1u : 0u));
    }
    CHECK(rumble_records[user].left == vibration.left_motor_speed);
    CHECK(rumble_records[user].right == vibration.right_motor_speed);
  }

  // An intentional assignment swaps the complete physical devices, while all
  // other ports remain unchanged.
  std::array<uint32_t, 4> calls_before_swap = {};
  for (size_t i = 0; i < rumble_records.size(); ++i) {
    calls_before_swap[i] = rumble_records[i].calls;
  }
  REQUIRE(fixture.driver.SwapControllerSlots(0, 2, device_ids[0]) == X_ERROR_SUCCESS);
  for (size_t controller = 0; controller < rumble_records.size(); ++controller) {
    const bool swapped_controller = controller == 0 || controller == 2;
    CHECK(rumble_records[controller].calls ==
          calls_before_swap[controller] + (swapped_controller ? 1u : 0u));
    if (swapped_controller) {
      CHECK(rumble_records[controller].left == 0);
      CHECK(rumble_records[controller].right == 0);
    }
  }
  REQUIRE(fixture.driver.GetKeystroke(0, 0, &keystroke) == X_ERROR_SUCCESS);
  CHECK(keystroke.virtual_key == static_cast<uint16_t>(rex::ui::VirtualKey::kXInputPadA));
  CHECK((keystroke.flags & rex::input::X_INPUT_KEYSTROKE_KEYUP) != 0);
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  CHECK(static_cast<uint32_t>(state.packet_number) > initial_packets[0]);
  const uint32_t first_swapped_packet = static_cast<uint32_t>(state.packet_number);
  CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_X) != 0);
  REQUIRE(fixture.Poll(2, state) == X_ERROR_SUCCESS);
  CHECK(static_cast<uint32_t>(state.packet_number) > initial_packets[2]);
  const uint32_t third_swapped_packet = static_cast<uint32_t>(state.packet_number);
  CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_A) != 0);
  REQUIRE(fixture.Poll(1, state) == X_ERROR_SUCCESS);
  CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_B) != 0);
  REQUIRE(fixture.Poll(3, state) == X_ERROR_SUCCESS);
  CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_Y) != 0);

  // A stale host snapshot cannot reassign the controller that replaced it.
  CHECK(fixture.driver.SwapControllerSlots(0, 1, device_ids[0]) == X_ERROR_DEVICE_NOT_CONNECTED);
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_X) != 0);

  std::array<uint32_t, 4> swapped_rumble_calls = {};
  for (size_t i = 0; i < rumble_records.size(); ++i) {
    swapped_rumble_calls[i] = rumble_records[i].calls;
  }
  X_INPUT_VIBRATION swapped_vibration = {};
  swapped_vibration.left_motor_speed = 0x3456;
  REQUIRE(fixture.driver.SetState(0, &swapped_vibration) == X_ERROR_SUCCESS);
  CHECK(rumble_records[2].calls == swapped_rumble_calls[2] + 1);
  CHECK(rumble_records[2].left == swapped_vibration.left_motor_speed);
  CHECK(rumble_records[0].calls == swapped_rumble_calls[0]);

  REQUIRE(fixture.driver.SwapControllerSlots(0, 2, device_ids[2]) == X_ERROR_SUCCESS);
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  CHECK(static_cast<uint32_t>(state.packet_number) > first_swapped_packet);
  REQUIRE(fixture.Poll(2, state) == X_ERROR_SUCCESS);
  CHECK(static_cast<uint32_t>(state.packet_number) > third_swapped_packet);
  CHECK(fixture.driver.SwapControllerSlots(0, 4) == X_ERROR_BAD_ARGUMENTS);

  // Removing player 2 must leave players 1, 3 and 4 on their original ports.
  gamepads[1]->Detach();
  REQUIRE(fixture.PollDisconnected(1, state) == X_ERROR_DEVICE_NOT_CONNECTED);
  for (uint32_t user : {0u, 2u, 3u}) {
    REQUIRE(fixture.Poll(user, state) == X_ERROR_SUCCESS);
    CHECK((static_cast<uint16_t>(state.gamepad.buttons) & guest_buttons[user]) != 0);
  }

  // A deliberate move into an empty port preserves the physical controller,
  // input and rumble association. An empty source is never allowed to pull a
  // different destination controller backward.
  CHECK(fixture.driver.SwapControllerSlots(1, 0) == X_ERROR_DEVICE_NOT_CONNECTED);
  REQUIRE(fixture.driver.SwapControllerSlots(0, 1, device_ids[0]) == X_ERROR_SUCCESS);
  REQUIRE(fixture.PollDisconnected(0, state) == X_ERROR_DEVICE_NOT_CONNECTED);
  REQUIRE(fixture.Poll(1, state) == X_ERROR_SUCCESS);
  CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_A) != 0);
  const uint32_t moved_rumble_calls = rumble_records[0].calls;
  X_INPUT_VIBRATION moved_vibration = {};
  moved_vibration.right_motor_speed = 0x4567;
  REQUIRE(fixture.driver.SetState(1, &moved_vibration) == X_ERROR_SUCCESS);
  CHECK(rumble_records[0].calls == moved_rumble_calls + 1);
  CHECK(rumble_records[0].right == moved_vibration.right_motor_speed);
  REQUIRE(fixture.driver.SwapControllerSlots(1, 0, device_ids[0]) == X_ERROR_SUCCESS);
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_A) != 0);
  REQUIRE(fixture.PollDisconnected(1, state) == X_ERROR_DEVICE_NOT_CONNECTED);

  RumbleRecord replacement_rumble;
  VirtualGamepad replacement(&replacement_rumble);
  replacement.SetButton(SDL_GAMEPAD_BUTTON_START, true);
  replacement.Commit();
  REQUIRE(fixture.Poll(1, state) == X_ERROR_SUCCESS);
  CHECK(static_cast<uint32_t>(state.packet_number) > initial_packets[1]);
  CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_START) != 0);

  // Filling player 2 still must not renumber established players.
  for (uint32_t user : {0u, 2u, 3u}) {
    REQUIRE(fixture.Poll(user, state) == X_ERROR_SUCCESS);
    CHECK((static_cast<uint16_t>(state.gamepad.buttons) & guest_buttons[user]) != 0);
  }
}

TEST_CASE("SDL waiting controller fills only a vacated multiplayer port",
          "[input][sdl][multiplayer]") {
  SDLDriverFixture fixture;
  std::vector<std::unique_ptr<VirtualGamepad>> gamepads;
  gamepads.reserve(5);
  for (size_t i = 0; i < 5; ++i) {
    gamepads.push_back(std::make_unique<VirtualGamepad>());
  }

  gamepads[0]->SetButton(SDL_GAMEPAD_BUTTON_SOUTH, true);
  gamepads[1]->SetButton(SDL_GAMEPAD_BUTTON_EAST, true);
  gamepads[2]->SetButton(SDL_GAMEPAD_BUTTON_WEST, true);
  gamepads[3]->SetButton(SDL_GAMEPAD_BUTTON_NORTH, true);
  gamepads[4]->SetButton(SDL_GAMEPAD_BUTTON_START, true);
  for (auto& gamepad : gamepads) {
    gamepad->Commit();
  }

  constexpr std::array<uint16_t, 4> original_buttons = {
      X_INPUT_GAMEPAD_A,
      X_INPUT_GAMEPAD_B,
      X_INPUT_GAMEPAD_X,
      X_INPUT_GAMEPAD_Y,
  };
  X_INPUT_STATE state = {};
  uint64_t old_player_two_device = 0;
  uint32_t old_player_two_packet = 0;
  for (uint32_t user = 0; user < 4; ++user) {
    REQUIRE(fixture.Poll(user, state) == X_ERROR_SUCCESS);
    CHECK((static_cast<uint16_t>(state.gamepad.buttons) & original_buttons[user]) != 0);
    if (user == 1) {
      old_player_two_packet = static_cast<uint32_t>(state.packet_number);
      rex::input::ControllerSnapshot snapshot;
      REQUIRE(fixture.Snapshot(user, snapshot));
      old_player_two_device = snapshot.device_id;
    }
  }

  // The fifth controller is connected but initially has no guest port. When
  // player 2 disconnects it claims only that vacancy; players 1, 3 and 4 keep
  // their established identities.
  gamepads[1]->Detach();
  REQUIRE(fixture.Poll(1, state) == X_ERROR_SUCCESS);
  CHECK(static_cast<uint32_t>(state.packet_number) > old_player_two_packet);
  CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_START) != 0);
  rex::input::ControllerSnapshot replacement_snapshot;
  REQUIRE(fixture.Snapshot(1, replacement_snapshot));
  CHECK(replacement_snapshot.device_id != old_player_two_device);
  CHECK(fixture.driver.SwapControllerSlots(1, 0, old_player_two_device) ==
        X_ERROR_DEVICE_NOT_CONNECTED);
  REQUIRE(fixture.Poll(1, state) == X_ERROR_SUCCESS);
  CHECK((static_cast<uint16_t>(state.gamepad.buttons) & X_INPUT_GAMEPAD_START) != 0);
  for (uint32_t user : {0u, 2u, 3u}) {
    REQUIRE(fixture.Poll(user, state) == X_ERROR_SUCCESS);
    CHECK((static_cast<uint16_t>(state.gamepad.buttons) & original_buttons[user]) != 0);
  }
}

TEST_CASE("SDL gamepad lifecycle clears and rediscovers controller ports", "[input][sdl]") {
  SDLDriverFixture fixture;
  std::vector<std::unique_ptr<VirtualGamepad>> gamepads;
  gamepads.reserve(4);
  for (size_t i = 0; i < 4; ++i) {
    gamepads.push_back(std::make_unique<VirtualGamepad>());
  }
  gamepads.back()->SetButton(SDL_GAMEPAD_BUTTON_NORTH, true);
  gamepads.back()->Commit();

  X_INPUT_STATE state = {};
  for (uint32_t user = 0; user < 4; ++user) {
    REQUIRE(fixture.Poll(user, state) == X_ERROR_SUCCESS);
  }

  for (auto& gamepad : gamepads) {
    gamepad->Detach();
  }
  REQUIRE(fixture.PollDisconnected(0, state) == X_ERROR_DEVICE_NOT_CONNECTED);

  // Reattaching the same driver must clear slot and keystroke state.
  fixture.driver.OnWindowUnavailable();
  fixture.driver.OnWindowAvailable(&fixture.window);

  VirtualGamepad replacement;
  replacement.SetButton(SDL_GAMEPAD_BUTTON_SOUTH, true);
  replacement.Commit();
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  CHECK((state.gamepad.buttons & X_INPUT_GAMEPAD_A) != 0);
}

TEST_CASE("SDL gamepad rumble follows XInput state and stops when inactive", "[input][sdl]") {
  SDLDriverFixture fixture;
  bool active = true;
  fixture.driver.set_is_active_callback([&active] { return active; });
  RumbleRecord rumble;
  VirtualGamepad gamepad(&rumble);

  X_INPUT_STATE state = {};
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);

  X_INPUT_VIBRATION vibration = {};
  vibration.left_motor_speed = 0x1234;
  vibration.right_motor_speed = 0xABCD;
  REQUIRE(fixture.driver.SetState(0, &vibration) == X_ERROR_SUCCESS);
  CHECK(rumble.calls >= 1);
  CHECK(rumble.left == 0x1234);
  CHECK(rumble.right == 0xABCD);

  active = false;
  fixture.driver.OnInputActiveChanged(active);
  CHECK(rumble.left == 0);
  CHECK(rumble.right == 0);

  REQUIRE(fixture.driver.SetState(0, &vibration) == X_ERROR_SUCCESS);
  CHECK(rumble.left == 0);
  CHECK(rumble.right == 0);

  active = true;
  rumble.accept = false;
  REQUIRE(fixture.driver.SetState(0, &vibration) == X_ERROR_FUNCTION_FAILED);
}

TEST_CASE("SDL controller snapshot remains live while guest input is suppressed",
          "[input][sdl][controller]") {
  const double old_sensitivity = REXCVAR_GET(controller_look_sensitivity);
  const double old_move_deadzone = REXCVAR_GET(controller_move_deadzone);
  const double old_aim_deadzone = REXCVAR_GET(controller_aim_deadzone);
  const bool old_invert = REXCVAR_GET(controller_invert_y);
  struct RestoreCvars {
    double sensitivity;
    double move_deadzone;
    double aim_deadzone;
    bool invert;
    ~RestoreCvars() {
      REXCVAR_SET(controller_look_sensitivity, sensitivity);
      REXCVAR_SET(controller_move_deadzone, move_deadzone);
      REXCVAR_SET(controller_aim_deadzone, aim_deadzone);
      REXCVAR_SET(controller_invert_y, invert);
    }
  } restore{old_sensitivity, old_move_deadzone, old_aim_deadzone, old_invert};

  REXCVAR_SET(controller_look_sensitivity, 2.0);
  REXCVAR_SET(controller_move_deadzone, 0.15);
  REXCVAR_SET(controller_aim_deadzone, 0.0);
  REXCVAR_SET(controller_invert_y, true);

  SDLDriverFixture fixture;
  std::atomic<uint32_t> active_callback_calls{0};
  fixture.driver.set_is_active_callback([&active_callback_calls] {
    active_callback_calls.fetch_add(1, std::memory_order_relaxed);
    return false;
  });
  fixture.driver.OnInputActiveChanged(false);
  VirtualGamepad gamepad;
  gamepad.SetButton(SDL_GAMEPAD_BUTTON_SOUTH, true);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_LEFTX, 3000);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_RIGHTX, 12000);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_RIGHTY, 8000);
  gamepad.Commit();

  rex::input::ControllerSnapshot snapshot;
  REQUIRE(fixture.Snapshot(0, snapshot));
  CHECK(snapshot.connected);
  CHECK_FALSE(snapshot.input_active);
  CHECK(snapshot.name == "GoldenEye virtual gamepad");
  CHECK((snapshot.raw_gamepad.buttons & X_INPUT_GAMEPAD_A) != 0);
  CHECK(snapshot.raw_gamepad.thumb_lx == 3000);
  CHECK(snapshot.gamepad.thumb_lx == 0);
  CHECK(snapshot.gamepad.thumb_rx == 24000);
  CHECK(snapshot.gamepad.thumb_ry > 0);
  CHECK(active_callback_calls.load(std::memory_order_relaxed) == 0);

  X_INPUT_STATE guest_state = {};
  REQUIRE(fixture.Poll(0, guest_state) == X_ERROR_SUCCESS);
  CHECK(active_callback_calls.load(std::memory_order_relaxed) != 0);
  CHECK(guest_state.gamepad.buttons == 0);
  CHECK(guest_state.gamepad.thumb_rx == 0);
}

TEST_CASE("SDL snapshot samples focus when attaching to an unfocused window",
          "[input][sdl][controller]") {
  REQUIRE(SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0xCAFE/0x0001"));

  TestAppContext context;
  TestWindow window(context);
  CHECK_FALSE(window.HasFocus());

  rex::input::sdl::SDLInputDriver driver(nullptr, 0);
  REQUIRE(driver.Setup() == X_STATUS_SUCCESS);
  driver.OnWindowAvailable(&window);
  struct Cleanup {
    rex::input::sdl::SDLInputDriver& driver;
    ~Cleanup() {
      driver.OnWindowUnavailable();
      SDL_ResetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT);
    }
  } cleanup{driver};

  VirtualGamepad gamepad;
  gamepad.Commit();

  rex::input::ControllerSnapshot snapshot;
  bool found = false;
  for (int attempt = 0; attempt < 8 && !found; ++attempt) {
    SDL_PumpEvents();
    context.ExecutePendingFunctionsFromUIThread();
    found = driver.GetControllerSnapshot(0, &snapshot);
  }
  REQUIRE(found);
  CHECK(snapshot.connected);
  CHECK_FALSE(snapshot.input_active);
}

TEST_CASE("SDL host rumble test respects enable and intensity settings",
          "[input][sdl][controller]") {
  const bool old_enabled = REXCVAR_GET(controller_rumble_enabled);
  const double old_intensity = REXCVAR_GET(controller_rumble_intensity);
  struct RestoreRumbleCvars {
    bool enabled;
    double intensity;
    ~RestoreRumbleCvars() {
      REXCVAR_SET(controller_rumble_enabled, enabled);
      REXCVAR_SET(controller_rumble_intensity, intensity);
    }
  } restore{old_enabled, old_intensity};

  REXCVAR_SET(controller_rumble_enabled, true);
  REXCVAR_SET(controller_rumble_intensity, 0.5);
  SDLDriverFixture fixture;
  RumbleRecord rumble;
  VirtualGamepad gamepad(&rumble);
  rex::input::ControllerSnapshot snapshot;
  REQUIRE(fixture.Snapshot(0, snapshot));
  REQUIRE(snapshot.rumble_supported);

  REQUIRE(fixture.driver.PlayControllerTestRumble(0, snapshot.device_id) == X_ERROR_SUCCESS);
  CHECK(rumble.left == 0x4800);
  CHECK(rumble.right == 0x4800);

  gamepad.Detach();
  RumbleRecord replacement_rumble;
  VirtualGamepad replacement(&replacement_rumble);
  CHECK(fixture.driver.PlayControllerTestRumble(0, snapshot.device_id) ==
        X_ERROR_DEVICE_NOT_CONNECTED);
  rex::input::ControllerSnapshot replacement_snapshot;
  REQUIRE(fixture.Snapshot(0, replacement_snapshot));
  REQUIRE(replacement_snapshot.device_id != snapshot.device_id);
  REQUIRE(fixture.driver.PlayControllerTestRumble(0, replacement_snapshot.device_id) ==
          X_ERROR_SUCCESS);
  CHECK(replacement_rumble.left == 0x4800);
  CHECK(replacement_rumble.right == 0x4800);

  REXCVAR_SET(controller_rumble_enabled, false);
  REQUIRE(fixture.driver.PlayControllerTestRumble(0, replacement_snapshot.device_id) ==
          X_ERROR_FUNCTION_FAILED);
}

TEST_CASE("Controller tuning uses independent radial deadzones and safe inversion",
          "[input][sdl][controller]") {
  rex::input::X_INPUT_GAMEPAD source = {};
  source.thumb_lx = 3000;
  source.thumb_ly = 0;
  source.thumb_rx = 12000;
  source.thumb_ry = static_cast<int16_t>(-32768);

  rex::input::ControllerTuning tuning;
  tuning.move_deadzone = 0.15;
  tuning.aim_deadzone = 0.0;
  tuning.look_sensitivity = 2.0;
  tuning.invert_y = true;
  const auto tuned = rex::input::controller::ApplyTuning(source, tuning);

  CHECK(tuned.thumb_lx == 0);
  CHECK(tuned.thumb_ly == 0);
  CHECK(tuned.thumb_rx == 24000);
  // Inverting -32768 must saturate to +32767, not overflow back to -32768.
  CHECK(tuned.thumb_ry == 32767);
}

TEST_CASE("Controller radial deadzone remaps its remaining range", "[input][sdl][controller]") {
  int16_t x = 0;
  int16_t y = 0;
  rex::input::controller::ApplyRadialDeadzone(16384, 0, 0.25, &x, &y);
  CHECK(x == 10923);
  CHECK(y == 0);

  rex::input::controller::ApplyRadialDeadzone(32767, 32767, 0.25, &x, &y);
  CHECK(x == 23170);
  CHECK(y == 23170);
}

TEST_CASE("Controller rumble intensity scales both motors", "[input][sdl][controller]") {
  CHECK(rex::input::controller::ScaleRumble(0xFFFF, 0.0) == 0);
  CHECK(rex::input::controller::ScaleRumble(0xFFFF, 0.5) == 0x8000);
  CHECK(rex::input::controller::ScaleRumble(0xFFFF, 1.0) == 0xFFFF);
  CHECK(rex::input::controller::ScaleRumble(0xFFFF, std::numeric_limits<double>::quiet_NaN()) ==
        0xFFFF);
}

TEST_CASE("Controller layout presets route axes and southpaw stick clicks",
          "[input][sdl][controller][mapping]") {
  using rex::input::controller::ApplyMapping;
  using rex::input::controller::ButtonBindings;
  using rex::input::controller::Layout;

  rex::input::X_INPUT_GAMEPAD source = {};
  source.thumb_lx = 1111;
  source.thumb_ly = 2222;
  source.thumb_rx = 3333;
  source.thumb_ry = 4444;
  source.buttons = rex::input::X_INPUT_GAMEPAD_LEFT_THUMB;
  const ButtonBindings bindings;

  const auto modern = ApplyMapping(source, Layout::kModern, bindings);
  CHECK(modern.thumb_lx == 1111);
  CHECK(modern.thumb_ly == 2222);
  CHECK(modern.thumb_rx == 3333);
  CHECK(modern.thumb_ry == 4444);

  const auto classic = ApplyMapping(source, Layout::kClassic, bindings);
  CHECK(classic.thumb_lx == 3333);
  CHECK(classic.thumb_ly == 2222);
  CHECK(classic.thumb_rx == 1111);
  CHECK(classic.thumb_ry == 4444);

  const auto southpaw = ApplyMapping(source, Layout::kSouthpaw, bindings);
  CHECK(southpaw.thumb_lx == 3333);
  CHECK(southpaw.thumb_ly == 4444);
  CHECK(southpaw.thumb_rx == 1111);
  CHECK(southpaw.thumb_ry == 2222);
  CHECK((southpaw.buttons & rex::input::X_INPUT_GAMEPAD_LEFT_THUMB) == 0);
  CHECK((southpaw.buttons & rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB) != 0);
}

TEST_CASE("Controller button map parsing is strict and canonical",
          "[input][sdl][controller][mapping]") {
  rex::input::controller::ButtonBindings bindings;
  REQUIRE(rex::input::controller::ParseButtonBindings(" a=b, rt=lb, x=none ", &bindings));
  CHECK(rex::input::controller::SerializeButtonBindings(bindings) == "a=b,x=none,rt=lb");

  rex::input::controller::ButtonBindings round_trip;
  REQUIRE(rex::input::controller::ParseButtonBindings(
      rex::input::controller::SerializeButtonBindings(bindings), &round_trip));
  CHECK(round_trip.sources == bindings.sources);

  CHECK_FALSE(rex::input::controller::ParseButtonBindings("a=b,a=x", &round_trip));
  CHECK_FALSE(rex::input::controller::ParseButtonBindings("guide=a", &round_trip));
  CHECK_FALSE(rex::input::controller::ParseButtonBindings("a=guide", &round_trip));
  CHECK_FALSE(rex::input::controller::ParseButtonBindings("a=b,", &round_trip));
}

TEST_CASE("Controller remapping supports buttons triggers and unbound inputs",
          "[input][sdl][controller][mapping]") {
  rex::input::controller::ButtonBindings bindings;
  REQUIRE(rex::input::controller::ParseButtonBindings("a=b,b=a,x=lt,y=none,lt=rb", &bindings));

  rex::input::X_INPUT_GAMEPAD source = {};
  source.buttons = rex::input::X_INPUT_GAMEPAD_B | rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER |
                   rex::input::X_INPUT_GAMEPAD_GUIDE;
  source.left_trigger = 31;
  const auto mapped = rex::input::controller::ApplyMapping(
      source, rex::input::controller::Layout::kModern, bindings);

  CHECK((mapped.buttons & rex::input::X_INPUT_GAMEPAD_A) != 0);
  CHECK((mapped.buttons & rex::input::X_INPUT_GAMEPAD_B) == 0);
  CHECK((mapped.buttons & rex::input::X_INPUT_GAMEPAD_X) != 0);
  CHECK((mapped.buttons & rex::input::X_INPUT_GAMEPAD_Y) == 0);
  CHECK((mapped.buttons & rex::input::X_INPUT_GAMEPAD_GUIDE) != 0);
  CHECK(mapped.left_trigger == 255);
}

TEST_CASE("Controller UI assignment swaps conflicting physical sources",
          "[input][sdl][controller][mapping]") {
  using rex::input::controller::AssignSourceWithSwap;
  using rex::input::controller::ButtonBindings;
  using rex::input::controller::Control;
  using rex::input::controller::Layout;
  using rex::input::controller::ResolveSource;

  ButtonBindings bindings;
  REQUIRE(AssignSourceWithSwap(Layout::kModern, &bindings, Control::kA, Control::kB));
  CHECK(ResolveSource(Layout::kModern, bindings, Control::kA) == Control::kB);
  CHECK(ResolveSource(Layout::kModern, bindings, Control::kB) == Control::kA);
  CHECK(rex::input::controller::SerializeButtonBindings(bindings) == "a=b,b=a");

  rex::input::X_INPUT_GAMEPAD source = {};
  source.buttons = rex::input::X_INPUT_GAMEPAD_B;
  const auto mapped = rex::input::controller::ApplyMapping(source, Layout::kModern, bindings);
  CHECK((mapped.buttons & rex::input::X_INPUT_GAMEPAD_A) != 0);
  CHECK((mapped.buttons & rex::input::X_INPUT_GAMEPAD_B) == 0);
}

TEST_CASE("Controller tuning follows logical axes after layout mapping",
          "[input][sdl][controller][mapping]") {
  rex::input::X_INPUT_GAMEPAD source = {};
  source.thumb_lx = 12000;
  source.thumb_rx = 3000;
  const rex::input::controller::ButtonBindings bindings;
  const auto classic = rex::input::controller::ApplyMapping(
      source, rex::input::controller::Layout::kClassic, bindings);

  rex::input::ControllerTuning tuning;
  tuning.move_deadzone = 0.15;
  tuning.aim_deadzone = 0.0;
  tuning.look_sensitivity = 2.0;
  const auto tuned = rex::input::controller::ApplyTuning(classic, tuning);
  CHECK(tuned.thumb_lx == 0);
  CHECK(tuned.thumb_rx == 24000);
}

TEST_CASE("SDL hot-reloads mapped state consistently for state snapshot and keystroke",
          "[input][sdl][controller][mapping]") {
  const std::string old_layout = REXCVAR_GET(controller_layout);
  const std::string old_button_map = REXCVAR_GET(controller_button_map);
  struct RestoreMappingCvars {
    std::string layout;
    std::string button_map;
    ~RestoreMappingCvars() {
      REXCVAR_SET(controller_layout, layout);
      REXCVAR_SET(controller_button_map, button_map);
    }
  } restore{old_layout, old_button_map};

  REXCVAR_SET(controller_layout, std::string("modern"));
  REXCVAR_SET(controller_button_map, std::string());

  SDLDriverFixture fixture;
  VirtualGamepad gamepad;
  gamepad.SetButton(SDL_GAMEPAD_BUTTON_EAST, true);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_LEFTX, 12000);
  gamepad.SetAxis(SDL_GAMEPAD_AXIS_RIGHTX, 3000);
  gamepad.Commit();

  X_INPUT_STATE state = {};
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  const uint32_t modern_packet = static_cast<uint32_t>(state.packet_number);
  CHECK((state.gamepad.buttons & rex::input::X_INPUT_GAMEPAD_B) != 0);
  CHECK(state.gamepad.thumb_lx == 12000);

  REXCVAR_SET(controller_layout, std::string("classic"));
  REXCVAR_SET(controller_button_map, std::string("a=b,b=a"));
  REQUIRE(fixture.Poll(0, state) == X_ERROR_SUCCESS);
  CHECK(static_cast<uint32_t>(state.packet_number) == modern_packet + 1);
  CHECK((state.gamepad.buttons & rex::input::X_INPUT_GAMEPAD_A) != 0);
  CHECK((state.gamepad.buttons & rex::input::X_INPUT_GAMEPAD_B) == 0);
  CHECK(state.gamepad.thumb_lx == 3000);
  CHECK(state.gamepad.thumb_rx == 12000);

  rex::input::ControllerSnapshot snapshot;
  REQUIRE(fixture.Snapshot(0, snapshot));
  CHECK((snapshot.raw_gamepad.buttons & rex::input::X_INPUT_GAMEPAD_B) != 0);
  CHECK((snapshot.raw_gamepad.buttons & rex::input::X_INPUT_GAMEPAD_A) == 0);
  CHECK((snapshot.gamepad.buttons & rex::input::X_INPUT_GAMEPAD_A) != 0);
  CHECK(snapshot.gamepad.thumb_lx == 3000);

  rex::input::X_INPUT_KEYSTROKE keystroke = {};
  REQUIRE(fixture.driver.GetKeystroke(0, 0, &keystroke) == X_ERROR_SUCCESS);
  CHECK(keystroke.virtual_key == static_cast<uint16_t>(rex::ui::VirtualKey::kXInputPadA));
}
