/**
 * @file        rex/input/ios_device_motion.h
 * @brief       iPad/iPhone built-in gyro as a gyro-aim source (CoreMotion)
 *
 * For grip controllers without a gyro (GameSir G8 and similar) the device is
 * physically attached to the controller, so the device's own rotation is the
 * aiming motion.
 */

#pragma once

#include <rex/platform.h>

namespace rex::input::ios {

#if REX_PLATFORM_IOS
/// Returns the device rotation mapped to look axes, in rad/s:
/// yaw_left_out > 0 when the rig turns left, pitch_up_out > 0 when it tilts
/// up. Landscape orientation (either side) is resolved via gravity, so no
/// UIKit call is needed. Starts CoreMotion updates lazily on first use.
/// Returns false when motion data is not (yet) available.
bool GetDeviceGyroLook(float* yaw_left_out, float* pitch_up_out);
#else
inline bool GetDeviceGyroLook(float*, float*) { return false; }
#endif

}  // namespace rex::input::ios
