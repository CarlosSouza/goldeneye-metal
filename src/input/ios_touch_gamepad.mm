/**
 * @file        rex/input/ios_touch_gamepad.mm
 * @brief       On-screen touch gamepad implementation (see header)
 *
 * Layout replicated from MeloNX's Melo-Controller package (landscape):
 * bottom-anchored columns with the trigger/shoulder pill row directly above
 * each stick, the d-pad overlapping the left stick's footprint and the face
 * buttons overlapping the right stick's (Joy-Con style), BACK/START at the
 * bottom center. Face buttons use the Xbox arrangement (Y top, X left,
 * B right, A bottom) since the guest is an Xbox 360 title.
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

constexpr float kStrokeAlpha = 0.42f;
constexpr float kFillAlpha = 0.10f;
constexpr float kFillPressedAlpha = 0.32f;

enum class ControlKind { kStick, kButton, kTrigger, kMenuChord };
enum class Icon { kNone, kUp, kDown, kLeft, kRight };

struct Control {
  ControlKind kind;
  // kButton: SDL_GamepadButton. kTrigger/kStick: SDL_GamepadAxis of the
  // trigger / the stick's X axis (Y is axis + 1).
  int sdl_index;
  const char* label;
  Icon icon;
  CGSize size;     // pills use w != h; circles use w == h (diameter)
  CGPoint center;  // set by layout
  CAShapeLayer* base = nil;
  CAShapeLayer* nub = nil;  // kStick only

  CGFloat HitRadius() const { return std::max(size.width, size.height) * 0.5; }
};

UIBezierPath* TrianglePath(Icon icon, CGPoint c, CGFloat s) {
  UIBezierPath* path = [UIBezierPath bezierPath];
  switch (icon) {
    case Icon::kUp:
      [path moveToPoint:CGPointMake(c.x, c.y - s)];
      [path addLineToPoint:CGPointMake(c.x - s, c.y + s * 0.7)];
      [path addLineToPoint:CGPointMake(c.x + s, c.y + s * 0.7)];
      break;
    case Icon::kDown:
      [path moveToPoint:CGPointMake(c.x, c.y + s)];
      [path addLineToPoint:CGPointMake(c.x - s, c.y - s * 0.7)];
      [path addLineToPoint:CGPointMake(c.x + s, c.y - s * 0.7)];
      break;
    case Icon::kLeft:
      [path moveToPoint:CGPointMake(c.x - s, c.y)];
      [path addLineToPoint:CGPointMake(c.x + s * 0.7, c.y - s)];
      [path addLineToPoint:CGPointMake(c.x + s * 0.7, c.y + s)];
      break;
    case Icon::kRight:
      [path moveToPoint:CGPointMake(c.x + s, c.y)];
      [path addLineToPoint:CGPointMake(c.x - s * 0.7, c.y - s)];
      [path addLineToPoint:CGPointMake(c.x - s * 0.7, c.y + s)];
      break;
    case Icon::kNone:
      break;
  }
  [path closePath];
  return path;
}

}  // namespace

@interface RexTouchGamepadView : UIView {
 @private
  SDL_Joystick* joystick_;
  std::vector<Control> controls_;
  NSMapTable<UITouch*, NSNumber*>* touch_to_control_;
  std::function<void()> menu_callback_;
}
- (instancetype)initWithJoystick:(SDL_Joystick*)joystick
                    menuCallback:(std::function<void()>)menuCallback;
- (void)releaseAllInputs;
@end

@implementation RexTouchGamepadView

- (instancetype)initWithJoystick:(SDL_Joystick*)joystick
                    menuCallback:(std::function<void()>)menuCallback {
  self = [super initWithFrame:CGRectZero];
  if (!self) {
    return nil;
  }
  joystick_ = joystick;
  menu_callback_ = std::move(menuCallback);
  // Manual retain: this codebase builds Objective-C++ without ARC.
  touch_to_control_ = [[NSMapTable weakToStrongObjectsMapTable] retain];
  self.multipleTouchEnabled = YES;
  self.backgroundColor = [UIColor clearColor];

  // Sizes from Melo-Controller (iPad multiplier 1.2 applied): sticks 160,
  // face buttons 54, small buttons 42, trigger pills 84x48.
  controls_ = {
      {ControlKind::kStick, SDL_GAMEPAD_AXIS_LEFTX, "", Icon::kNone, {160, 160}},
      {ControlKind::kStick, SDL_GAMEPAD_AXIS_RIGHTX, "", Icon::kNone, {160, 160}},
      {ControlKind::kTrigger, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, "LT", Icon::kNone, {84, 48}},
      {ControlKind::kTrigger, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, "RT", Icon::kNone, {84, 48}},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "LB", Icon::kNone, {84, 48}},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "RB", Icon::kNone, {84, 48}},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_SOUTH, "A", Icon::kNone, {54, 54}},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_EAST, "B", Icon::kNone, {54, 54}},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_WEST, "X", Icon::kNone, {54, 54}},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_NORTH, "Y", Icon::kNone, {54, 54}},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_START, "START", Icon::kNone, {42, 42}},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_BACK, "BACK", Icon::kNone, {42, 42}},
      // Holds L3+R3 while touched: the runtime's Host Settings chord, which
      // is otherwise unreachable without stick-click buttons.
      {ControlKind::kMenuChord, 0, "MENU", Icon::kNone, {64, 30}},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_DPAD_UP, "", Icon::kUp, {42, 42}},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_DPAD_DOWN, "", Icon::kDown, {42, 42}},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_DPAD_LEFT, "", Icon::kLeft, {42, 42}},
      {ControlKind::kButton, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "", Icon::kRight, {42, 42}},
  };
  for (auto& control : controls_) {
    control.base = [CAShapeLayer layer];
    control.base.fillColor =
        [UIColor colorWithWhite:1.0
                          alpha:control.kind == ControlKind::kStick ? 0.05 : kFillAlpha]
            .CGColor;
    control.base.strokeColor = [UIColor colorWithWhite:1.0 alpha:kStrokeAlpha].CGColor;
    control.base.lineWidth = 2.0;
    [self.layer addSublayer:control.base];
    if (control.kind == ControlKind::kStick) {
      control.nub = [CAShapeLayer layer];
      control.nub.fillColor = [UIColor colorWithWhite:1.0 alpha:kStrokeAlpha].CGColor;
      [self.layer addSublayer:control.nub];
    }
    if (control.label[0] != '\0') {
      CATextLayer* text = [CATextLayer layer];
      text.string = [NSString stringWithUTF8String:control.label];
      text.fontSize = std::strlen(control.label) > 2 ? 11 : 16;
      text.alignmentMode = kCAAlignmentCenter;
      text.foregroundColor = [UIColor colorWithWhite:1.0 alpha:kStrokeAlpha + 0.25].CGColor;
      text.contentsScale = [UIScreen mainScreen].scale;
      [control.base addSublayer:text];
    }
  }
  return self;
}

- (void)dealloc {
  [touch_to_control_ release];
  [super dealloc];
}

- (Control*)findControl:(ControlKind)kind index:(int)sdl_index {
  for (auto& control : controls_) {
    if (control.kind == kind && control.sdl_index == sdl_index) {
      return &control;
    }
  }
  return nullptr;
}

- (void)placeControl:(ControlKind)kind index:(int)sdl_index at:(CGPoint)p {
  Control* control = [self findControl:kind index:sdl_index];
  if (!control) {
    return;
  }
  control->center = p;
  const CGFloat w = control->size.width;
  const CGFloat h = control->size.height;
  const CGRect rect = CGRectMake(p.x - w / 2, p.y - h / 2, w, h);
  if (control->icon != Icon::kNone) {
    // D-pad: circular base with a vector triangle (text glyphs render
    // unreliably in CATextLayer).
    control->base.path = [UIBezierPath bezierPathWithOvalInRect:rect].CGPath;
    CAShapeLayer* arrow = nil;
    for (CALayer* sub in control->base.sublayers) {
      if ([sub isKindOfClass:[CAShapeLayer class]]) {
        arrow = (CAShapeLayer*)sub;
        break;
      }
    }
    if (!arrow) {
      arrow = [CAShapeLayer layer];
      arrow.fillColor = [UIColor colorWithWhite:1.0 alpha:kStrokeAlpha + 0.25].CGColor;
      [control->base addSublayer:arrow];
    }
    arrow.path = TrianglePath(control->icon, p, w * 0.22).CGPath;
  } else {
    control->base.path =
        [UIBezierPath bezierPathWithRoundedRect:rect cornerRadius:std::min(w, h) / 2].CGPath;
  }
  if (control->nub) {
    [self setStick:*control offset:CGPointZero];
  }
  for (CALayer* sub in control->base.sublayers) {
    if ([sub isKindOfClass:[CATextLayer class]]) {
      sub.frame = CGRectMake(p.x - w / 2, p.y - 9, w, 18);
    }
  }
}

- (void)layoutSubviews {
  [super layoutSubviews];
  const CGFloat W = self.bounds.size.width;
  const CGFloat H = self.bounds.size.height;
  const UIEdgeInsets safe = self.safeAreaInsets;
  const CGFloat left = std::max<CGFloat>(safe.left, 20);
  const CGFloat right = std::max<CGFloat>(safe.right, 20);
  const CGFloat bottom = std::max<CGFloat>(safe.bottom, 16);

  // Left column: [LT LB] pill row above the stick; d-pad on the stick's rim.
  const CGPoint lstick = {left + 90, H - bottom - 90};
  [self placeControl:ControlKind::kStick index:SDL_GAMEPAD_AXIS_LEFTX at:lstick];
  [self placeControl:ControlKind::kButton index:SDL_GAMEPAD_BUTTON_DPAD_UP
                  at:{lstick.x, lstick.y - 58}];
  [self placeControl:ControlKind::kButton index:SDL_GAMEPAD_BUTTON_DPAD_DOWN
                  at:{lstick.x, lstick.y + 58}];
  [self placeControl:ControlKind::kButton index:SDL_GAMEPAD_BUTTON_DPAD_LEFT
                  at:{lstick.x - 58, lstick.y}];
  [self placeControl:ControlKind::kButton index:SDL_GAMEPAD_BUTTON_DPAD_RIGHT
                  at:{lstick.x + 58, lstick.y}];
  const CGFloat lrow_y = lstick.y - 80 - 20 - 24;
  [self placeControl:ControlKind::kTrigger index:SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                  at:{lstick.x - 52, lrow_y}];
  [self placeControl:ControlKind::kButton index:SDL_GAMEPAD_BUTTON_LEFT_SHOULDER
                  at:{lstick.x + 52, lrow_y}];

  // Right column mirrored: [RB RT] pills; ABXY on the stick's rim in the
  // Xbox arrangement.
  const CGPoint rstick = {W - right - 90, H - bottom - 90};
  [self placeControl:ControlKind::kStick index:SDL_GAMEPAD_AXIS_RIGHTX at:rstick];
  [self placeControl:ControlKind::kButton index:SDL_GAMEPAD_BUTTON_NORTH
                  at:{rstick.x, rstick.y - 58}];
  [self placeControl:ControlKind::kButton index:SDL_GAMEPAD_BUTTON_SOUTH
                  at:{rstick.x, rstick.y + 58}];
  [self placeControl:ControlKind::kButton index:SDL_GAMEPAD_BUTTON_WEST
                  at:{rstick.x - 58, rstick.y}];
  [self placeControl:ControlKind::kButton index:SDL_GAMEPAD_BUTTON_EAST
                  at:{rstick.x + 58, rstick.y}];
  const CGFloat rrow_y = rstick.y - 80 - 20 - 24;
  [self placeControl:ControlKind::kButton index:SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER
                  at:{rstick.x - 52, rrow_y}];
  [self placeControl:ControlKind::kTrigger index:SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
                  at:{rstick.x + 52, rrow_y}];

  // Bottom center: BACK ... MENU ... START.
  [self placeControl:ControlKind::kButton index:SDL_GAMEPAD_BUTTON_BACK
                  at:{W / 2 - 90, H - bottom - 24}];
  [self placeControl:ControlKind::kMenuChord index:0 at:{W / 2, H - bottom - 24}];
  [self placeControl:ControlKind::kButton index:SDL_GAMEPAD_BUTTON_START
                  at:{W / 2 + 90, H - bottom - 24}];
}

- (void)setStick:(Control&)control offset:(CGPoint)offset {
  const CGFloat nub_radius = control.HitRadius() * 0.42;
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
  // Nearest control whose reach contains the point. Buttons sit on the
  // sticks' rims (MeloNX layout), so nearest-wins keeps taps on buttons and
  // center grabs on the stick.
  int best = -1;
  CGFloat best_distance = CGFLOAT_MAX;
  for (size_t i = 0; i < controls_.size(); ++i) {
    const auto& control = controls_[i];
    const CGFloat dx = point.x - control.center.x;
    const CGFloat dy = point.y - control.center.y;
    const CGFloat distance = std::sqrt(dx * dx + dy * dy);
    const CGFloat reach = control.kind == ControlKind::kStick ? control.HitRadius() * 1.25
                                                              : control.HitRadius() * 1.15;
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
      const CGFloat radius = control.HitRadius();
      CGPoint offset = {0, 0};
      float nx = 0.0f;
      float ny = 0.0f;
      if (down) {
        const CGFloat dx = point.x - control.center.x;
        const CGFloat dy = point.y - control.center.y;
        const CGFloat length = std::max<CGFloat>(1.0, std::sqrt(dx * dx + dy * dy));
        const CGFloat clamped = std::min<CGFloat>(length, radius);
        offset = {dx / length * clamped, dy / length * clamped};
        nx = static_cast<float>(std::clamp<CGFloat>(dx / radius, -1.0, 1.0));
        ny = static_cast<float>(std::clamp<CGFloat>(dy / radius, -1.0, 1.0));
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
          [UIColor colorWithWhite:1.0 alpha:down ? kFillPressedAlpha : kFillAlpha].CGColor;
      break;
    case ControlKind::kButton:
      SDL_SetJoystickVirtualButton(joystick_, control.sdl_index, down);
      control.base.fillColor =
          [UIColor colorWithWhite:1.0 alpha:down ? kFillPressedAlpha : kFillAlpha].CGColor;
      break;
    case ControlKind::kMenuChord:
      // Opens Host Settings immediately through the app hook - no L3+R3
      // hold delay. Fires on press; release only restores the visual.
      if (down && menu_callback_) {
        menu_callback_();
      }
      control.base.fillColor =
          [UIColor colorWithWhite:1.0 alpha:down ? kFillPressedAlpha : kFillAlpha].CGColor;
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
// through to the game view (which drives the ImGui host UI).
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

void InstallTouchGamepad(std::function<void()> menu_callback) {
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

  g_overlay = [[RexTouchGamepadView alloc] initWithJoystick:g_virtual_joystick
                                               menuCallback:std::move(menu_callback)];
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
