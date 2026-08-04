/**
 * @file        rex/input/ios_touch_gamepad.h
 * @brief       On-screen touch gamepad for iPad play without a controller
 *
 * A semi-transparent digital control overlay (sticks, face buttons, triggers)
 * that feeds an SDL virtual gamepad, so the input driver consumes it exactly
 * like a physical controller. The overlay shows only while no physical
 * controller is connected; the driver keeps physical pads on the lower guest
 * ports (EnforceVirtualSlotOrderLocked).
 */

#pragma once

#include <functional>

#include <rex/platform.h>

namespace rex::input::ios {

#if REX_PLATFORM_IOS
/// Installs the overlay into the key window and attaches the SDL virtual
/// gamepad. Must be called on the main thread after the game window exists.
/// menu_callback runs on the main thread when the overlay's MENU button is
/// tapped (opens the host settings UI without the L3+R3 hold delay).
void InstallTouchGamepad(std::function<void()> menu_callback);
#else
inline void InstallTouchGamepad(std::function<void()>) {}
#endif

}  // namespace rex::input::ios
