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

#include <rex/platform.h>

namespace rex::input::ios {

#if REX_PLATFORM_IOS
/// Installs the overlay into the key window and attaches the SDL virtual
/// gamepad. Must be called on the main thread after the game window exists.
void InstallTouchGamepad();
#else
inline void InstallTouchGamepad() {}
#endif

}  // namespace rex::input::ios
