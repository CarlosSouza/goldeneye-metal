#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include <SDL3/SDL.h>

namespace rex::input::sdl {

#if defined(REXGLUE_ENABLE_INPUT_TEST_HARNESS)

// Developer-only controller integration harness. It is compiled out of normal
// and release builds, and still requires an explicit three-part environment
// gate before it will attach any virtual devices.
class SDLVirtualGamepadHarness {
 public:
  static std::unique_ptr<SDLVirtualGamepadHarness> CreateFromEnvironment();

  ~SDLVirtualGamepadHarness();

  SDLVirtualGamepadHarness(const SDLVirtualGamepadHarness&) = delete;
  SDLVirtualGamepadHarness& operator=(const SDLVirtualGamepadHarness&) = delete;

  bool Attach();
  bool ReportDriverReady();
  std::span<const SDL_JoystickID> instance_ids() const;
  bool BeforeSDLPump();
  void AfterSDLPump();
  void Shutdown();

 private:
  struct Impl;

  explicit SDLVirtualGamepadHarness(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

#else

class SDLVirtualGamepadHarness {
 public:
  ~SDLVirtualGamepadHarness() = default;
};

#endif

}  // namespace rex::input::sdl
