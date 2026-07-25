#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>
#include <utility>

using rex::X_RESULT;
using rex::X_STATUS;

namespace {

class KeystrokeDriver final : public rex::input::InputDriver {
 public:
  KeystrokeDriver(rex::X_RESULT result, uint16_t key = 0,
                  rex::input::MouseMotionMode* observed_mouse_mode = nullptr)
      : InputDriver(nullptr, 0),
        result_(result),
        key_(key),
        observed_mouse_mode_(observed_mouse_mode) {}

  rex::X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  rex::X_RESULT GetCapabilities(uint32_t, uint32_t, rex::input::X_INPUT_CAPABILITIES*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  rex::X_RESULT GetState(uint32_t, rex::input::X_INPUT_STATE*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  rex::X_RESULT SetState(uint32_t, rex::input::X_INPUT_VIBRATION*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  rex::X_RESULT GetKeystroke(uint32_t, uint32_t,
                             rex::input::X_INPUT_KEYSTROKE* keystroke) override {
    if (result_ == X_ERROR_SUCCESS && keystroke) {
      std::memset(keystroke, 0, sizeof(*keystroke));
      keystroke->virtual_key = key_;
    }
    return result_;
  }

  void SetMouseMotionMode(rex::input::MouseMotionMode mode) override {
    if (observed_mouse_mode_) {
      *observed_mouse_mode_ = mode;
    }
  }

  void SetApplicationMouseMotion(rex::input::MouseMotionDelta delta) {
    application_mouse_owned_ = true;
    application_mouse_delta_ = delta;
  }

  void SetControllerSnapshot(rex::input::ControllerSnapshot snapshot) {
    controller_snapshot_ = std::move(snapshot);
    has_controller_snapshot_ = true;
  }

  bool GetControllerSnapshot(uint32_t user_index,
                             rex::input::ControllerSnapshot* out_snapshot) override {
    if (!has_controller_snapshot_ || user_index != controller_snapshot_.user_index) {
      if (out_snapshot) {
        *out_snapshot = {};
        out_snapshot->user_index = user_index;
      }
      return false;
    }
    if (out_snapshot) {
      *out_snapshot = controller_snapshot_;
    }
    return true;
  }

  void SetHostInputSnapshot(rex::input::HostInputSnapshot snapshot) {
    host_input_snapshot_ = snapshot;
    has_host_input_snapshot_ = true;
  }

  bool GetHostInputSnapshot(rex::input::HostInputSnapshot* out_snapshot) const override {
    if (!has_host_input_snapshot_) {
      if (out_snapshot) {
        *out_snapshot = {};
      }
      return false;
    }
    if (out_snapshot) {
      *out_snapshot = host_input_snapshot_;
    }
    return true;
  }

  void SetTestRumbleResult(rex::X_RESULT result) { test_rumble_result_ = result; }

  rex::X_RESULT PlayControllerTestRumble(uint32_t user_index,
                                         uint64_t expected_device_id) override {
    test_rumble_user_ = user_index;
    test_rumble_device_id_ = expected_device_id;
    return test_rumble_result_;
  }

  uint32_t test_rumble_user() const { return test_rumble_user_; }
  uint64_t test_rumble_device_id() const { return test_rumble_device_id_; }

  void SetSwapResult(rex::X_RESULT result) { swap_result_ = result; }

  rex::X_RESULT SwapControllerSlots(uint32_t first_user_index, uint32_t second_user_index,
                                    uint64_t expected_device_id) override {
    swap_called_ = true;
    swap_first_ = first_user_index;
    swap_second_ = second_user_index;
    swap_device_id_ = expected_device_id;
    return swap_result_;
  }

  bool swap_called() const { return swap_called_; }
  uint32_t swap_first() const { return swap_first_; }
  uint32_t swap_second() const { return swap_second_; }
  uint64_t swap_device_id() const { return swap_device_id_; }

  bool ConsumeApplicationMouseMotion(uint32_t user_index,
                                     rex::input::MouseMotionDelta* out_delta) override {
    if (!application_mouse_owned_ || user_index != 0) {
      if (out_delta) {
        *out_delta = {};
      }
      return false;
    }
    if (out_delta) {
      *out_delta = application_mouse_delta_;
    }
    application_mouse_delta_ = {};
    return true;
  }

 private:
  rex::X_RESULT result_;
  uint16_t key_;
  rex::input::MouseMotionMode* observed_mouse_mode_;
  bool application_mouse_owned_ = false;
  rex::input::MouseMotionDelta application_mouse_delta_ = {};
  bool has_controller_snapshot_ = false;
  rex::input::ControllerSnapshot controller_snapshot_ = {};
  bool has_host_input_snapshot_ = false;
  rex::input::HostInputSnapshot host_input_snapshot_ = {};
  rex::X_RESULT test_rumble_result_ = X_ERROR_DEVICE_NOT_CONNECTED;
  uint32_t test_rumble_user_ = 0;
  uint64_t test_rumble_device_id_ = 0;
  rex::X_RESULT swap_result_ = X_ERROR_DEVICE_NOT_CONNECTED;
  bool swap_called_ = false;
  uint32_t swap_first_ = 0;
  uint32_t swap_second_ = 0;
  uint64_t swap_device_id_ = 0;
};

}  // namespace

TEST_CASE("Input system checks MnK keystrokes after an idle controller driver", "[input]") {
  rex::input::InputSystem input(nullptr);
  input.AddDriver(std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY));
  input.AddDriver(std::make_unique<KeystrokeDriver>(X_ERROR_SUCCESS, 0x5810));

  rex::input::X_INPUT_KEYSTROKE keystroke = {};
  REQUIRE(input.GetKeystroke(0, 0, &keystroke) == X_ERROR_SUCCESS);
  CHECK(keystroke.virtual_key == 0x5810);
}

TEST_CASE("Input system forwards application mouse mode to every driver", "[input][mouse]") {
  using rex::input::MouseMotionMode;
  MouseMotionMode first = MouseMotionMode::kRightStick;
  MouseMotionMode second = MouseMotionMode::kRightStick;

  rex::input::InputSystem input(nullptr);
  input.AddDriver(std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY, 0, &first));
  input.AddDriver(std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY, 0, &second));
  input.SetMouseMotionMode(MouseMotionMode::kApplication);

  CHECK(first == MouseMotionMode::kApplication);
  CHECK(second == MouseMotionMode::kApplication);
}

TEST_CASE("Input system consumes and combines paired application mouse motion", "[input][mouse]") {
  rex::input::InputSystem input(nullptr);
  input.AddDriver(std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY));

  auto first = std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY);
  first->SetApplicationMouseMotion({4, -3});
  input.AddDriver(std::move(first));

  auto second = std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY);
  second->SetApplicationMouseMotion({-1, 5});
  input.AddDriver(std::move(second));

  rex::input::MouseMotionDelta delta{99, 99};
  REQUIRE(input.ConsumeApplicationMouseMotion(0, &delta));
  CHECK(delta.x == 3);
  CHECK(delta.y == 2);

  // Drivers still own application input on an idle frame, but the sample is
  // consumed exactly once and the output is always initialized.
  REQUIRE(input.ConsumeApplicationMouseMotion(0, &delta));
  CHECK(delta.x == 0);
  CHECK(delta.y == 0);

  delta = {99, 99};
  CHECK_FALSE(input.ConsumeApplicationMouseMotion(1, &delta));
  CHECK(delta.x == 0);
  CHECK(delta.y == 0);
}

TEST_CASE("Input system exposes the first physical controller snapshot", "[input][controller]") {
  rex::input::InputSystem input(nullptr);
  input.AddDriver(std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY));

  auto controller = std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY);
  rex::input::ControllerSnapshot expected;
  expected.connected = true;
  expected.user_index = 0;
  expected.name = "Test pad";
  expected.raw_gamepad.thumb_lx = 1234;
  expected.gamepad.thumb_lx = 1000;
  controller->SetControllerSnapshot(expected);
  input.AddDriver(std::move(controller));

  rex::input::ControllerSnapshot actual;
  REQUIRE(input.GetControllerSnapshot(0, &actual));
  CHECK(actual.connected);
  CHECK(actual.name == "Test pad");
  CHECK(actual.raw_gamepad.thumb_lx == 1234);
  CHECK(actual.gamepad.thumb_lx == 1000);

  actual.name = "stale";
  CHECK_FALSE(input.GetControllerSnapshot(1, &actual));
  CHECK_FALSE(actual.connected);
  CHECK(actual.user_index == 1);
  CHECK(actual.name.empty());
}

TEST_CASE("Input system exposes a driver-published host input snapshot", "[input][mouse]") {
  rex::input::InputSystem input(nullptr);
  input.AddDriver(std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY));

  auto host = std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY);
  host->SetHostInputSnapshot({
      .focused = true,
      .input_active = true,
      .mouse_capture_active = true,
  });
  input.AddDriver(std::move(host));

  rex::input::HostInputSnapshot snapshot;
  REQUIRE(input.GetHostInputSnapshot(&snapshot));
  CHECK(snapshot.focused);
  CHECK(snapshot.input_active);
  CHECK(snapshot.mouse_capture_active);

  rex::input::InputSystem empty(nullptr);
  snapshot = {.focused = true, .input_active = true, .mouse_capture_active = true};
  CHECK_FALSE(empty.GetHostInputSnapshot(&snapshot));
  CHECK_FALSE(snapshot.focused);
  CHECK_FALSE(snapshot.input_active);
  CHECK_FALSE(snapshot.mouse_capture_active);
}

TEST_CASE("Input system routes host rumble tests to a connected driver", "[input][controller]") {
  rex::input::InputSystem input(nullptr);
  auto disconnected = std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY);
  disconnected->SetTestRumbleResult(X_ERROR_DEVICE_NOT_CONNECTED);
  input.AddDriver(std::move(disconnected));
  auto connected = std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY);
  connected->SetTestRumbleResult(X_ERROR_SUCCESS);
  auto* connected_ptr = connected.get();
  input.AddDriver(std::move(connected));
  REQUIRE(input.PlayControllerTestRumble(2, 0xABCD) == X_ERROR_SUCCESS);
  CHECK(connected_ptr->test_rumble_user() == 2);
  CHECK(connected_ptr->test_rumble_device_id() == 0xABCD);
}

TEST_CASE("Input system routes intentional controller port swaps",
          "[input][controller][multiplayer]") {
  rex::input::InputSystem input(nullptr);
  input.AddDriver(std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY));

  auto controller = std::make_unique<KeystrokeDriver>(X_ERROR_EMPTY);
  controller->SetSwapResult(X_ERROR_SUCCESS);
  auto* controller_ptr = controller.get();
  input.AddDriver(std::move(controller));

  REQUIRE(input.SwapControllerSlots(1, 3, 0x1234) == X_ERROR_SUCCESS);
  CHECK(controller_ptr->swap_called());
  CHECK(controller_ptr->swap_first() == 1);
  CHECK(controller_ptr->swap_second() == 3);
  CHECK(controller_ptr->swap_device_id() == 0x1234);

  controller_ptr->SetSwapResult(X_ERROR_BAD_ARGUMENTS);
  CHECK(input.SwapControllerSlots(4, 0) == X_ERROR_BAD_ARGUMENTS);
}
