/**
 * @file        rex/input/ios_touch_gamepad.mm
 * @brief       On-screen touch gamepad implementation (see header)
 *
 * Layout in the spirit of MelonX's digital overlay: two virtual sticks in the
 * lower corners, ABXY diamond on the right, big LT/RT at the edges (aim/fire
 * in GoldenEye's modern layout), LB/RB in the top corners, START/BACK at the
 * top center, and a compact d-pad above the left stick.
 */

#include <rex/input/ios_touch_gamepad.h>

#if REX_PLATFORM_IOS

#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <rex/logging.h>

namespace {

constexpr float kOverlayAlpha = 0.38f;

enum class ControlKind { kStick, kButton, kTrigger };

struct Control {
  ControlKind kind;
  // kButton: SDL_GamepadButton. kTrigger/kStick: SDL_GamepadAxis of the
  // trigger / the stick's X axis (Y is axis + 1).
  int sdl_index;
  const char* label;
  CGPoint center;      // set by layout
  CGFloat radius;      // hit/base radius
  CAShapeLayer* base = nil;
  CAShapeLayer* nub = nil;  // kStick only
};

}  // namespace

@interface RexTouchGamepadView : UIView {
 @private
  SDL_Joystick* joystick_;
  std::vector<Control> controls_;
  NSMapTable<UITouch*, NSNumber*>* touch_to_control_;
}
- (instancetype)initWithJoystick:(SDL_Joystick*)joystick;
- (void)releaseAllInputs;
@end

@implementation RexTouchGamepadView

- (instancetype)initWithJoystick:(SDL_Joystick*)joystick {
  self = [super initWithFrame:CGRectZero];
  if (!self) {
    return nil;
  }
  joystick_ = joystick;
  // Manual retain: this codebase builds Objective-C++ without ARC.
  touch_to_control_ = [[NSMapTable weakToStrongObjectsMapTable] retain];
  self.multipleTouchEnabled = YES;
  self.backgroundColor = [UIColor clearColor];

  controls_ = {
      {ControlKind::kStick, SDL_GAMEPAD_AXIS_LEFTX, "", CGPointZero, 75},
      {ControlKind::kStick, SDL_GAMEPAD_AXIS_RIGHTX, "", CGPointZero, 65},
      {ControlKind::kTrigger, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, "LT", CGPointZero, 38},
      {ControlKind::kTrigger, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, "RT", CGPointZero, 38},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_SOUTH, "A", CGPointZero, 26},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_EAST, "B", CGPointZero, 26},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_WEST, "X", CGPointZero, 26},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_NORTH, "Y", CGPointZero, 26},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "LB", CGPointZero, 27},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "RB", CGPointZero, 27},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_START, "START", CGPointZero, 22},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_BACK, "BACK", CGPointZero, 22},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_DPAD_UP, "▲", CGPointZero, 20},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_DPAD_DOWN, "▼", CGPointZero, 20},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_DPAD_LEFT, "◀", CGPointZero, 20},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "▶", CGPointZero, 20},
  };
  for (auto& control : controls_) {
    control.base = [CAShapeLayer layer];
    control.base.fillColor =
        [UIColor colorWithWhite:1.0 alpha:control.kind == ControlKind::kStick ? 0.06 : 0.10]
            .CGColor;
    control.base.strokeColor = [UIColor colorWithWhite:1.0 alpha:kOverlayAlpha].CGColor;
    control.base.lineWidth = 2.0;
    [self.layer addSublayer:control.base];
    if (control.kind == ControlKind::kStick) {
      control.nub = [CAShapeLayer layer];
      control.nub.fillColor = [UIColor colorWithWhite:1.0 alpha:kOverlayAlpha].CGColor;
      [self.layer addSublayer:control.nub];
    }
    if (control.label[0] != '\0') {
      CATextLayer* text = [CATextLayer layer];
      text.string = [NSString stringWithUTF8String:control.label];
      text.fontSize = control.radius > 25 ? 15 : 11;
      text.alignmentMode = kCAAlignmentCenter;
      text.foregroundColor = [UIColor colorWithWhite:1.0 alpha:kOverlayAlpha + 0.2].CGColor;
      text.contentsScale = [UIScreen mainScreen].scale;
      [control.base addSublayer:text];
    }
  }
  return self;
}

- (void)layoutSubviews {
  [super layoutSubviews];
  const CGFloat W = self.bounds.size.width;
  const CGFloat H = self.bounds.size.height;
  auto place = [&](int sdl_index, ControlKind kind, CGPoint p) {
    for (auto& control : controls_) {
      if (control.kind == kind && control.sdl_index == sdl_index) {
        control.center = p;
        const CGFloat r = control.radius;
        control.base.path =
            [UIBezierPath bezierPathWithOvalInRect:CGRectMake(p.x - r, p.y - r, 2 * r, 2 * r)]
                .CGPath;
        if (control.nub) {
          [self setStick:control offset:CGPointZero];
        }
        for (CALayer* sub in control.base.sublayers) {
          sub.frame = CGRectMake(p.x - r, p.y - 8, 2 * r, 16);
        }
        return;
      }
    }
  };
  place(SDL_GAMEPAD_AXIS_LEFTX, ControlKind::kStick, {160, H - 175});
  place(SDL_GAMEPAD_AXIS_RIGHTX, ControlKind::kStick, {W - 170, H - 165});
  place(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, ControlKind::kTrigger, {80, H - 330});
  place(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, ControlKind::kTrigger, {W - 80, H - 320});
  const CGPoint face = {W - 210, H - 340};
  place(SDL_GAMEPAD_BUTTON_SOUTH, ControlKind::kButton, {face.x, face.y + 52});
  place(SDL_GAMEPAD_BUTTON_EAST, ControlKind::kButton, {face.x + 52, face.y});
  place(SDL_GAMEPAD_BUTTON_WEST, ControlKind::kButton, {face.x - 52, face.y});
  place(SDL_GAMEPAD_BUTTON_NORTH, ControlKind::kButton, {face.x, face.y - 52});
  place(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ControlKind::kButton, {90, 60});
  place(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ControlKind::kButton, {W - 90, 60});
  place(SDL_GAMEPAD_BUTTON_START, ControlKind::kButton, {W / 2 + 70, 44});
  place(SDL_GAMEPAD_BUTTON_BACK, ControlKind::kButton, {W / 2 - 70, 44});
  const CGPoint dpad = {170, H - 430};
  place(SDL_GAMEPAD_BUTTON_DPAD_UP, ControlKind::kButton, {dpad.x, dpad.y - 44});
  place(SDL_GAMEPAD_BUTTON_DPAD_DOWN, ControlKind::kButton, {dpad.x, dpad.y + 44});
  place(SDL_GAMEPAD_BUTTON_DPAD_LEFT, ControlKind::kButton, {dpad.x - 44, dpad.y});
  place(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, ControlKind::kButton, {dpad.x + 44, dpad.y});
}

- (void)setStick:(Control&)control offset:(CGPoint)offset {
  const CGFloat nub_radius = control.radius * 0.45;
  const CGPoint p = {control.center.x + offset.x, control.center.y + offset.y};
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  control.nub.path = [UIBezierPath
      bezierPathWithOvalInRect:CGRectMake(p.x - nub_radius, p.y - nub_radius, 2 * nub_radius,
                                          2 * nub_radius)]
                         .CGPath;
  [CATransaction commit];
}

- (int)controlIndexAt:(CGPoint)point {
  int best = -1;
  CGFloat best_distance = CGFLOAT_MAX;
  for (size_t i = 0; i < controls_.size(); ++i) {
    const auto& control = controls_[i];
    const CGFloat dx = point.x - control.center.x;
    const CGFloat dy = point.y - control.center.y;
    const CGFloat distance = std::sqrt(dx * dx + dy * dy);
    // Sticks accept grabs a little outside their base.
    const CGFloat reach =
        control.kind == ControlKind::kStick ? control.radius * 1.5 : control.radius * 1.25;
    if (distance <= reach && distance < best_distance) {
      best = static_cast<int>(i);
      best_distance = distance;
    }
  }
  return best;
}

- (void)applyControl:(Control&)control down:(BOOL)down at:(CGPoint)point {
  switch (control.kind) {
    case ControlKind::kStick: {
      CGPoint offset = {0, 0};
      float nx = 0.0f;
      float ny = 0.0f;
      if (down) {
        const CGFloat dx = point.x - control.center.x;
        const CGFloat dy = point.y - control.center.y;
        const CGFloat length = std::max<CGFloat>(1.0, std::sqrt(dx * dx + dy * dy));
        const CGFloat clamped = std::min<CGFloat>(length, control.radius);
        offset = {dx / length * clamped, dy / length * clamped};
        nx = static_cast<float>(std::clamp<CGFloat>(dx / control.radius, -1.0, 1.0));
        ny = static_cast<float>(std::clamp<CGFloat>(dy / control.radius, -1.0, 1.0));
      }
      [self setStick:control offset:offset];
      // UIKit y grows downward, matching SDL's stick convention.
      SDL_SetJoystickVirtualAxis(joystick_, control.sdl_index,
                                 static_cast<Sint16>(nx * 32767.0f));
      SDL_SetJoystickVirtualAxis(joystick_, control.sdl_index + 1,
                                 static_cast<Sint16>(ny * 32767.0f));
      break;
    }
    case ControlKind::kTrigger:
      SDL_SetJoystickVirtualAxis(joystick_, control.sdl_index,
                                 down ? SDL_MAX_SINT16 : SDL_MIN_SINT16);
      control.base.fillColor =
          [UIColor colorWithWhite:1.0 alpha:down ? 0.30 : 0.10].CGColor;
      break;
    case ControlKind::kButton:
      SDL_SetJoystickVirtualButton(joystick_, control.sdl_index, down);
      control.base.fillColor =
          [UIColor colorWithWhite:1.0 alpha:down ? 0.30 : 0.10].CGColor;
      break;
  }
}

- (void)releaseAllInputs {
  [touch_to_control_ removeAllObjects];
  for (auto& control : controls_) {
    [self applyControl:control down:NO at:CGPointZero];
  }
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  for (UITouch* touch in touches) {
    const CGPoint point = [touch locationInView:self];
    const int index = [self controlIndexAt:point];
    if (index >= 0) {
      [touch_to_control_ setObject:@(index) forKey:touch];
      [self applyControl:controls_[index] down:YES at:point];
    }
  }
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  for (UITouch* touch in touches) {
    NSNumber* index = [touch_to_control_ objectForKey:touch];
    if (index && controls_[index.intValue].kind == ControlKind::kStick) {
      [self applyControl:controls_[index.intValue] down:YES at:[touch locationInView:self]];
    }
  }
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  for (UITouch* touch in touches) {
    NSNumber* index = [touch_to_control_ objectForKey:touch];
    if (index) {
      [self applyControl:controls_[index.intValue] down:NO at:CGPointZero];
      [touch_to_control_ removeObjectForKey:touch];
    }
  }
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  [self touchesEnded:touches withEvent:event];
}

// Only touches on a control belong to the overlay; everything else falls
// through to the game view (and the ImGui overlay's future touch path).
- (BOOL)pointInside:(CGPoint)point withEvent:(UIEvent*)event {
  return self.hidden ? NO : [self controlIndexAt:point] >= 0;
}

@end

namespace rex::input::ios {

namespace {

RexTouchGamepadView* g_overlay = nil;
SDL_JoystickID g_virtual_id = 0;
SDL_Joystick* g_virtual_joystick = nullptr;

void UpdateOverlayVisibility() {
  const bool physical_present = [[GCController controllers] count] > 0;
  if (g_overlay) {
    g_overlay.hidden = physical_present;
    if (physical_present) {
      [g_overlay releaseAllInputs];
    }
  }
  REXLOG_INFO("Touch gamepad overlay {}", physical_present ? "hidden (physical controller)"
                                                           : "visible (no controller)");
}

}  // namespace

void InstallTouchGamepad() {
  if (g_overlay) {
    return;
  }

  UIWindow* window = nil;
  for (UIScene* scene in [[UIApplication sharedApplication] connectedScenes]) {
    if ([scene isKindOfClass:[UIWindowScene class]]) {
      for (UIWindow* candidate in [(UIWindowScene*)scene windows]) {
        if (candidate.isKeyWindow || !window) {
          window = candidate;
        }
      }
    }
  }
  if (!window) {
    REXLOG_WARN("Touch gamepad: no window to attach to");
    return;
  }

  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
  desc.name = "Touch Gamepad";
  g_virtual_id = SDL_AttachVirtualJoystick(&desc);
  if (!g_virtual_id) {
    REXLOG_ERROR("Touch gamepad: SDL_AttachVirtualJoystick failed: {}", SDL_GetError());
    return;
  }
  g_virtual_joystick = SDL_OpenJoystick(g_virtual_id);
  if (!g_virtual_joystick) {
    REXLOG_ERROR("Touch gamepad: SDL_OpenJoystick failed: {}", SDL_GetError());
    SDL_DetachVirtualJoystick(g_virtual_id);
    g_virtual_id = 0;
    return;
  }
  // Triggers rest at the negative end of their axis.
  SDL_SetJoystickVirtualAxis(g_virtual_joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_MIN_SINT16);
  SDL_SetJoystickVirtualAxis(g_virtual_joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_MIN_SINT16);

  g_overlay = [[RexTouchGamepadView alloc] initWithJoystick:g_virtual_joystick];
  g_overlay.frame = window.bounds;
  g_overlay.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [window addSubview:g_overlay];

  NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
  for (NSNotificationName name :
       @[ GCControllerDidConnectNotification, GCControllerDidDisconnectNotification ]) {
    [center addObserverForName:name
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification* notification) {
                      (void)notification;
                      UpdateOverlayVisibility();
                    }];
  }
  UpdateOverlayVisibility();
  REXLOG_INFO("Touch gamepad installed (virtual joystick id {})",
              static_cast<uint32_t>(g_virtual_id));
}

}  // namespace rex::input::ios

#endif  // REX_PLATFORM_IOS
