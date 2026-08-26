#include "sdl_virtual_gamepad_harness.h"

#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <rex/logging.h>

namespace rex::input::sdl {
namespace {

constexpr uint16_t kVirtualVendorId = 0x4758;
constexpr uint16_t kVirtualProductId = 0x0001;
constexpr size_t kMaximumBufferedBytes = 16 * 1024;
constexpr size_t kMaximumLineBytes = 256;
constexpr size_t kMaximumCommandsPerPump = 64;
constexpr uint32_t kMaximumPulseMilliseconds = 10'000;

template <typename T>
bool ParseInteger(std::string_view text, T* out) {
  if (!out || text.empty()) {
    return false;
  }
  T value{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size()) {
    return false;
  }
  *out = value;
  return true;
}

std::string Uppercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::toupper(character));
                 });
  return value;
}

std::optional<SDL_GamepadButton> ParseButton(std::string_view name) {
  constexpr std::array<std::pair<std::string_view, SDL_GamepadButton>, 15>
      kButtons = {{
          {"SOUTH", SDL_GAMEPAD_BUTTON_SOUTH},
          {"EAST", SDL_GAMEPAD_BUTTON_EAST},
          {"WEST", SDL_GAMEPAD_BUTTON_WEST},
          {"NORTH", SDL_GAMEPAD_BUTTON_NORTH},
          {"BACK", SDL_GAMEPAD_BUTTON_BACK},
          {"GUIDE", SDL_GAMEPAD_BUTTON_GUIDE},
          {"START", SDL_GAMEPAD_BUTTON_START},
          {"LEFT_STICK", SDL_GAMEPAD_BUTTON_LEFT_STICK},
          {"RIGHT_STICK", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
          {"LEFT_SHOULDER", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
          {"RIGHT_SHOULDER", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
          {"DPAD_UP", SDL_GAMEPAD_BUTTON_DPAD_UP},
          {"DPAD_DOWN", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
          {"DPAD_LEFT", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
          {"DPAD_RIGHT", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
      }};
  for (const auto& [candidate, button] : kButtons) {
    if (candidate == name) {
      return button;
    }
  }
  return std::nullopt;
}

std::optional<SDL_GamepadAxis> ParseAxis(std::string_view name) {
  constexpr std::array<std::pair<std::string_view, SDL_GamepadAxis>, 6> kAxes = {{
      {"LX", SDL_GAMEPAD_AXIS_LEFTX},
      {"LY", SDL_GAMEPAD_AXIS_LEFTY},
      {"RX", SDL_GAMEPAD_AXIS_RIGHTX},
      {"RY", SDL_GAMEPAD_AXIS_RIGHTY},
      {"LT", SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
      {"RT", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
  }};
  for (const auto& [candidate, axis] : kAxes) {
    if (candidate == name) {
      return axis;
    }
  }
  return std::nullopt;
}

int16_t NeutralAxis(SDL_GamepadAxis axis) {
  return axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                 axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
             ? SDL_JOYSTICK_AXIS_MIN
             : 0;
}

bool IsExactEnvironmentValue(const char* name, const char* expected) {
  const char* value = std::getenv(name);
  return value && std::strcmp(value, expected) == 0;
}

}  // namespace

struct SDLVirtualGamepadHarness::Impl {
  struct Pad {
    SDL_JoystickID instance_id = 0;
    SDL_Joystick* joystick = nullptr;
    std::array<uint64_t, SDL_GAMEPAD_BUTTON_COUNT> button_generation{};
    std::array<uint64_t, SDL_GAMEPAD_AXIS_COUNT> axis_generation{};
  };

  struct Pulse {
    enum class Kind {
      kButton,
      kAxis,
    };

    Kind kind = Kind::kButton;
    size_t player = 0;
    int control = 0;
    uint64_t generation = 0;
    uint64_t earliest_pump = 0;
    std::chrono::steady_clock::time_point release_at;
  };

  uint32_t pad_count = 0;
  int command_fd = -1;
  std::optional<std::string> previous_isolation_hint;
  bool isolation_hint_changed = false;
  bool attached = false;
  bool driver_ready = false;
  bool shutdown = false;
  bool pipe_eof = false;
  uint64_t last_sequence = 0;
  uint64_t pump_count = 0;
  std::string command_buffer;
  std::vector<Pad> pads;
  std::vector<SDL_JoystickID> instance_ids;
  std::vector<Pulse> pulses;

  bool IsConnected(size_t player) const {
    return player < pads.size() && pads[player].instance_id && pads[player].joystick;
  }

  void CloseCommandPipe() {
    if (command_fd >= 0) {
      close(command_fd);
      command_fd = -1;
    }
  }

  bool SetButton(size_t player, SDL_GamepadButton button, bool down) {
    if (!IsConnected(player)) {
      return false;
    }
    return SDL_SetJoystickVirtualButton(
        pads[player].joystick, static_cast<int>(button), down);
  }

  bool SetAxis(size_t player, SDL_GamepadAxis axis, int16_t value) {
    if (!IsConnected(player)) {
      return false;
    }
    return SDL_SetJoystickVirtualAxis(
        pads[player].joystick, static_cast<int>(axis), value);
  }

  bool ResetPlayer(size_t player) {
    if (!IsConnected(player)) {
      return false;
    }
    bool success = true;
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
      ++pads[player].button_generation[button];
      success =
          SDL_SetJoystickVirtualButton(pads[player].joystick, button, false) &&
          success;
    }
    for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
      ++pads[player].axis_generation[axis];
      success = SDL_SetJoystickVirtualAxis(
                    pads[player].joystick, axis,
                    NeutralAxis(static_cast<SDL_GamepadAxis>(axis))) &&
                success;
    }
    return success;
  }

  bool AttachPlayer(size_t player) {
    if (player >= pads.size() || IsConnected(player)) {
      return false;
    }

    SDL_VirtualJoystickDesc description{};
    SDL_INIT_INTERFACE(&description);
    description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    description.vendor_id = kVirtualVendorId;
    description.product_id = kVirtualProductId;
    description.naxes = SDL_GAMEPAD_AXIS_COUNT;
    description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    description.name = "GoldenEye integration-test gamepad";

    auto& pad = pads[player];
    pad.instance_id = SDL_AttachVirtualJoystick(&description);
    if (!pad.instance_id) {
      return false;
    }
    pad.joystick = SDL_OpenJoystick(pad.instance_id);
    if (!pad.joystick) {
      SDL_DetachVirtualJoystick(pad.instance_id);
      pad.instance_id = 0;
      return false;
    }
    instance_ids[player] = pad.instance_id;
    if (!ResetPlayer(player)) {
      SDL_CloseJoystick(pad.joystick);
      pad.joystick = nullptr;
      SDL_DetachVirtualJoystick(pad.instance_id);
      pad.instance_id = 0;
      instance_ids[player] = 0;
      return false;
    }
    return true;
  }

  bool DisconnectPlayer(size_t player) {
    if (!IsConnected(player) || !ResetPlayer(player)) {
      return false;
    }

    // Publish neutral state before removal so neither SDL nor the guest can
    // retain a pressed control while the device is disappearing.
    SDL_UpdateJoysticks();
    pulses.erase(std::remove_if(pulses.begin(), pulses.end(),
                                [player](const Pulse& pulse) { return pulse.player == player; }),
                 pulses.end());

    auto& pad = pads[player];
    const SDL_JoystickID instance_id = pad.instance_id;
    SDL_CloseJoystick(pad.joystick);
    pad.joystick = nullptr;
    if (!SDL_DetachVirtualJoystick(instance_id)) {
      // Keep the harness internally usable if SDL refuses the detach.
      pad.joystick = SDL_OpenJoystick(instance_id);
      if (pad.joystick) {
        ResetPlayer(player);
        SDL_UpdateJoysticks();
      }
      return false;
    }
    pad.instance_id = 0;
    instance_ids[player] = 0;
    return true;
  }

  void Reject(std::string_view reason, std::string_view line) {
    REXLOG_WARN("[vpad] REJECT reason={} command={}", reason, line);
  }

  bool ParsePlayer(std::string_view text, size_t* player) const {
    uint32_t one_based = 0;
    if (!ParseInteger(text, &one_based) || one_based < 1 ||
        one_based > pads.size()) {
      return false;
    }
    *player = one_based - 1;
    return true;
  }

  bool ValidateSequence(std::string_view text, uint64_t* sequence) const {
    if (!ParseInteger(text, sequence) || *sequence == 0 ||
        *sequence <= last_sequence) {
      return false;
    }
    return true;
  }

  bool Execute(std::string_view line) {
    std::istringstream stream{std::string(line)};
    std::vector<std::string> tokens;
    for (std::string token; stream >> token;) {
      tokens.push_back(Uppercase(std::move(token)));
    }
    if (tokens.empty()) {
      return false;
    }

    const std::string& operation = tokens[0];
    const bool player_only_operation =
        operation == "RESET" || operation == "DISCONNECT" || operation == "CONNECT";
    const size_t expected_tokens = player_only_operation ? 3 : (operation == "PULSE_AXIS" ? 6 : 5);
    if (tokens.size() != expected_tokens) {
      Reject("invalid argument count", line);
      return false;
    }

    uint64_t sequence = 0;
    if (!ValidateSequence(tokens[1], &sequence)) {
      Reject("sequence must increase", line);
      return false;
    }

    bool changed = false;
    if (operation == "DISCONNECT" || operation == "CONNECT") {
      size_t player = 0;
      if (!ParsePlayer(tokens[2], &player)) {
        Reject("invalid player", line);
        return false;
      }
      if (operation == "DISCONNECT") {
        if (!IsConnected(player)) {
          Reject("player is already disconnected", line);
          return false;
        }
        changed = DisconnectPlayer(player);
      } else {
        if (IsConnected(player)) {
          Reject("player is already connected", line);
          return false;
        }
        changed = AttachPlayer(player);
      }
    } else if (operation == "RESET") {
      if (tokens[2] == "ALL") {
        changed = true;
        for (size_t player = 0; player < pads.size(); ++player) {
          if (IsConnected(player)) {
            changed = ResetPlayer(player) && changed;
          }
        }
      } else {
        size_t player = 0;
        if (!ParsePlayer(tokens[2], &player)) {
          Reject("invalid player", line);
          return false;
        }
        if (!IsConnected(player)) {
          Reject("player is disconnected", line);
          return false;
        }
        changed = ResetPlayer(player);
      }
    } else {
      size_t player = 0;
      if (!ParsePlayer(tokens[2], &player)) {
        Reject("invalid player", line);
        return false;
      }
      if (!IsConnected(player)) {
        Reject("player is disconnected", line);
        return false;
      }

      if (operation == "SET_BUTTON" || operation == "PULSE_BUTTON") {
        const auto button = ParseButton(tokens[3]);
        if (!button) {
          Reject("unknown button", line);
          return false;
        }
        uint32_t button_value = 0;
        if (!ParseInteger(tokens[4], &button_value) ||
            (operation == "SET_BUTTON" && button_value > 1) ||
            (operation == "PULSE_BUTTON" &&
             (button_value == 0 ||
              button_value > kMaximumPulseMilliseconds))) {
          Reject(operation == "SET_BUTTON"
                     ? "button value must be 0 or 1"
                     : "invalid pulse duration",
                 line);
          return false;
        }
        const bool pulse = operation == "PULSE_BUTTON";
        changed = SetButton(player, *button,
                            pulse || button_value != 0);
        if (!changed) {
          Reject(SDL_GetError(), line);
          return false;
        }
        const size_t button_index = static_cast<size_t>(*button);
        const uint64_t generation =
            ++pads[player].button_generation[button_index];
        if (pulse) {
          pulses.push_back({
              .kind = Pulse::Kind::kButton,
              .player = player,
              .control = static_cast<int>(*button),
              .generation = generation,
              .earliest_pump = pump_count + 1,
              .release_at = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(button_value),
          });
        }
      } else if (operation == "SET_AXIS" ||
                 operation == "PULSE_AXIS") {
        const auto axis = ParseAxis(tokens[3]);
        int32_t value = 0;
        if (!axis || !ParseInteger(tokens[4], &value) ||
            value < std::numeric_limits<int16_t>::min() ||
            value > std::numeric_limits<int16_t>::max()) {
          Reject("invalid axis or value", line);
          return false;
        }
        uint32_t hold_ms = 0;
        if (operation == "PULSE_AXIS") {
          if (!ParseInteger(tokens[5], &hold_ms) || hold_ms == 0 ||
              hold_ms > kMaximumPulseMilliseconds) {
            Reject("invalid pulse duration", line);
            return false;
          }
        }
        changed = SetAxis(player, *axis, static_cast<int16_t>(value));
        if (!changed) {
          Reject(SDL_GetError(), line);
          return false;
        }
        const size_t axis_index = static_cast<size_t>(*axis);
        const uint64_t generation =
            ++pads[player].axis_generation[axis_index];
        if (operation == "PULSE_AXIS") {
          pulses.push_back({
              .kind = Pulse::Kind::kAxis,
              .player = player,
              .control = static_cast<int>(*axis),
              .generation = generation,
              .earliest_pump = pump_count + 1,
              .release_at = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(hold_ms),
          });
        }
      } else {
        Reject("unknown operation", line);
        return false;
      }
    }

    if (!changed) {
      Reject(SDL_GetError(), line);
      return false;
    }
    last_sequence = sequence;
    REXLOG_INFO("[vpad] ACK seq={}", sequence);
    return true;
  }

  bool ReleaseExpiredPulses() {
    const auto now = std::chrono::steady_clock::now();
    bool changed = false;
    auto pulse = pulses.begin();
    while (pulse != pulses.end()) {
      if (pump_count < pulse->earliest_pump || now < pulse->release_at) {
        ++pulse;
        continue;
      }

      bool current = false;
      if (pulse->player < pads.size()) {
        auto& pad = pads[pulse->player];
        if (pulse->kind == Pulse::Kind::kButton) {
          const size_t button = static_cast<size_t>(pulse->control);
          current = button < pad.button_generation.size() &&
                    pad.button_generation[button] == pulse->generation;
          if (current) {
            changed =
                SetButton(pulse->player,
                          static_cast<SDL_GamepadButton>(pulse->control),
                          false) ||
                changed;
          }
        } else {
          const size_t axis = static_cast<size_t>(pulse->control);
          current = axis < pad.axis_generation.size() &&
                    pad.axis_generation[axis] == pulse->generation;
          if (current) {
            changed =
                SetAxis(pulse->player,
                        static_cast<SDL_GamepadAxis>(pulse->control),
                        NeutralAxis(
                            static_cast<SDL_GamepadAxis>(pulse->control))) ||
                changed;
          }
        }
      }
      pulse = pulses.erase(pulse);
    }
    return changed;
  }

  bool DrainCommands() {
    if (command_fd < 0 || pipe_eof) {
      return false;
    }

    std::array<char, 4096> incoming{};
    for (size_t read_count = 0; read_count < 4; ++read_count) {
      const ssize_t bytes = read(command_fd, incoming.data(), incoming.size());
      if (bytes > 0) {
        command_buffer.append(incoming.data(), static_cast<size_t>(bytes));
        if (command_buffer.size() > kMaximumBufferedBytes) {
          REXLOG_ERROR("[vpad] command buffer exceeded {} bytes; resetting",
                       kMaximumBufferedBytes);
          command_buffer.clear();
        }
        continue;
      }
      if (bytes == 0) {
        pipe_eof = true;
        CloseCommandPipe();
      } else if (errno != EAGAIN && errno != EWOULDBLOCK &&
                 errno != EINTR) {
        REXLOG_ERROR("[vpad] command pipe read failed: {}",
                     std::strerror(errno));
        pipe_eof = true;
        CloseCommandPipe();
      }
      break;
    }

    bool changed = false;
    size_t processed = 0;
    while (processed < kMaximumCommandsPerPump) {
      const size_t newline = command_buffer.find('\n');
      if (newline == std::string::npos) {
        break;
      }
      std::string line = command_buffer.substr(0, newline);
      command_buffer.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.empty() || line.front() == '#') {
        continue;
      }
      ++processed;
      if (line.size() > kMaximumLineBytes) {
        Reject("command line too long", line.substr(0, kMaximumLineBytes));
        continue;
      }
      changed = Execute(line) || changed;
    }
    return changed;
  }
};

std::unique_ptr<SDLVirtualGamepadHarness>
SDLVirtualGamepadHarness::CreateFromEnvironment() {
  const char* enabled = std::getenv("REX_INPUT_TEST_HARNESS");
  const char* pad_count_text = std::getenv("REX_TEST_VIRTUAL_GAMEPADS");
  const char* command_fd_text =
      std::getenv("REX_TEST_VIRTUAL_GAMEPAD_FD");
  if (!enabled && !pad_count_text && !command_fd_text) {
    return nullptr;
  }
  if (!IsExactEnvironmentValue("REX_INPUT_TEST_HARNESS", "1") ||
      !pad_count_text || !command_fd_text) {
    REXLOG_ERROR(
        "[vpad] disabled: activation requires REX_INPUT_TEST_HARNESS=1, "
        "REX_TEST_VIRTUAL_GAMEPADS and REX_TEST_VIRTUAL_GAMEPAD_FD");
    return nullptr;
  }

  uint32_t pad_count = 0;
  if (!ParseInteger(pad_count_text, &pad_count) || pad_count < 1 || pad_count > 4) {
    REXLOG_ERROR("[vpad] disabled: invalid pad count");
    return nullptr;
  }
  int command_fd = -1;
  if (!ParseInteger(command_fd_text, &command_fd) || command_fd < 0 ||
      fcntl(command_fd, F_GETFD) < 0) {
    REXLOG_ERROR("[vpad] disabled: invalid command descriptor");
    return nullptr;
  }

  const int flags = fcntl(command_fd, F_GETFL);
  struct stat descriptor_info {};
  if (flags < 0 || (flags & O_ACCMODE) == O_WRONLY ||
      fstat(command_fd, &descriptor_info) < 0 ||
      !S_ISFIFO(descriptor_info.st_mode)) {
    REXLOG_ERROR("[vpad] disabled: command descriptor must be a readable pipe");
    close(command_fd);
    return nullptr;
  }
  if (fcntl(command_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    REXLOG_ERROR(
        "[vpad] disabled: could not make command descriptor nonblocking");
    close(command_fd);
    return nullptr;
  }

  auto impl = std::make_unique<Impl>();
  impl->pad_count = pad_count;
  impl->command_fd = command_fd;
  if (const char* previous =
          SDL_GetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT)) {
    impl->previous_isolation_hint = previous;
  }
  if (!SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,
                   "0x4758/0x0001")) {
    REXLOG_ERROR("[vpad] disabled: could not isolate virtual test gamepads");
    close(command_fd);
    return nullptr;
  }
  impl->isolation_hint_changed = true;
  return std::unique_ptr<SDLVirtualGamepadHarness>(
      new SDLVirtualGamepadHarness(std::move(impl)));
}

SDLVirtualGamepadHarness::SDLVirtualGamepadHarness(
    std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

SDLVirtualGamepadHarness::~SDLVirtualGamepadHarness() { Shutdown(); }

bool SDLVirtualGamepadHarness::Attach() {
  if (!impl_ || impl_->shutdown || impl_->attached) {
    return false;
  }

  impl_->pads.resize(impl_->pad_count);
  impl_->instance_ids.resize(impl_->pad_count);
  for (uint32_t player = 0; player < impl_->pad_count; ++player) {
    if (!impl_->AttachPlayer(player)) {
      REXLOG_ERROR("[vpad] attach failed for player {}: {}", player + 1,
                   SDL_GetError());
      Shutdown();
      return false;
    }
  }
  SDL_UpdateJoysticks();
  impl_->attached = true;
  return true;
}

bool SDLVirtualGamepadHarness::ReportDriverReady() {
  if (!impl_ || !impl_->attached || impl_->driver_ready ||
      impl_->shutdown) {
    return false;
  }
  impl_->driver_ready = true;
  REXLOG_INFO("[vpad] READY pads={}", impl_->pads.size());
  return true;
}

std::span<const SDL_JoystickID>
SDLVirtualGamepadHarness::instance_ids() const {
  if (!impl_) {
    return {};
  }
  return impl_->instance_ids;
}

bool SDLVirtualGamepadHarness::BeforeSDLPump() {
  if (!impl_ || !impl_->attached || impl_->shutdown) {
    return false;
  }
  const bool released_pulses = impl_->ReleaseExpiredPulses();
  const bool received_commands = impl_->DrainCommands();
  const bool changed = released_pulses || received_commands;
  if (changed) {
    SDL_UpdateJoysticks();
  }
  return impl_->driver_ready && !impl_->pipe_eof &&
         impl_->command_fd >= 0;
}

void SDLVirtualGamepadHarness::AfterSDLPump() {
  if (impl_ && impl_->attached && !impl_->shutdown) {
    ++impl_->pump_count;
  }
}

void SDLVirtualGamepadHarness::Shutdown() {
  if (!impl_ || impl_->shutdown) {
    return;
  }
  impl_->shutdown = true;
  if (!impl_->pads.empty()) {
    for (size_t player = 0; player < impl_->pads.size(); ++player) {
      impl_->ResetPlayer(player);
    }
    SDL_UpdateJoysticks();
  }
  impl_->pulses.clear();
  impl_->CloseCommandPipe();
  for (auto& pad : impl_->pads) {
    if (pad.joystick) {
      SDL_CloseJoystick(pad.joystick);
      pad.joystick = nullptr;
    }
  }
  for (auto pad = impl_->pads.rbegin(); pad != impl_->pads.rend(); ++pad) {
    if (pad->instance_id) {
      SDL_DetachVirtualJoystick(pad->instance_id);
      pad->instance_id = 0;
    }
  }
  impl_->pads.clear();
  impl_->instance_ids.clear();
  if (impl_->isolation_hint_changed) {
    if (impl_->previous_isolation_hint) {
      SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,
                  impl_->previous_isolation_hint->c_str());
    } else {
      SDL_ResetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT);
    }
    impl_->isolation_hint_changed = false;
  }
}

}  // namespace rex::input::sdl

#endif
