/**
 * @file        rex/input/ios_device_motion.mm
 * @brief       CoreMotion-backed device gyro for gyro aim (see header)
 */

#include <rex/input/ios_device_motion.h>

#if REX_PLATFORM_IOS

#import <CoreMotion/CoreMotion.h>

#include <cmath>

namespace rex::input::ios {

bool GetDeviceGyroLook(float* yaw_left_out, float* pitch_up_out) {
  static CMMotionManager* manager = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    manager = [[CMMotionManager alloc] init];
    if ([manager isDeviceMotionAvailable]) {
      manager.deviceMotionUpdateInterval = 1.0 / 100.0;
      [manager startDeviceMotionUpdates];
    }
  });

  CMDeviceMotion* motion = [manager deviceMotion];
  if (!motion) {
    return false;
  }

  // Device axes (portrait): +X right, +Y top, +Z out of the screen; rotation
  // rates are right-hand rule and rotationRate is gyro-bias corrected.
  //
  // The app runs in landscape, either side up. Gravity tells which: with the
  // portrait +X edge pointing at the ceiling gravity.x is about -1, with it
  // pointing at the floor about +1. World-up is then (+/-)X and the player's
  // left is (+/-)Y, which resolves both look axes without touching UIKit.
  const CMAcceleration gravity = [motion gravity];
  const CMRotationRate rate = [motion rotationRate];
  const float up_sign = gravity.x < 0.0 ? 1.0f : -1.0f;

  *yaw_left_out = up_sign * static_cast<float>(rate.x);
  *pitch_up_out = -up_sign * static_cast<float>(rate.y);
  return true;
}

}  // namespace rex::input::ios

#endif  // REX_PLATFORM_IOS
