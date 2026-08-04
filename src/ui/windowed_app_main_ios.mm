/**
 ******************************************************************************
 * ReXGlue iOS/iPadOS windowed app entry point                                 *
 ******************************************************************************
 *
 * UIApplicationMain owns the run loop on iOS, so unlike the macOS entry point
 * there is no custom C++ main loop: the app delegate initializes the
 * WindowedApp after launch and a repeating main-queue timer executes the
 * context's pending cross-thread functions each tick.
 *
 * There is no native launcher on the iPad. Game data imported by the macOS
 * launcher is dropped into the app's Documents folder (UIFileSharingEnabled)
 * and --game_data_root defaults to it unless given explicitly.
 */

#include <rex/platform.h>
#if REX_PLATFORM_IOS

#import <UIKit/UIKit.h>

// SDL on iOS refuses SDL_Init unless the app either uses SDL_main or declares
// its own entry point as ready (audio/gamepad drivers depend on this).
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/input/ios_touch_gamepad.h>
#include <rex/logging.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context.h>

namespace {

// Boot breadcrumbs into Documents/goldeneye-boot.txt: cheap, file-based and
// readable from the Files app, so early aborts can be located on-device even
// where stderr is invisible (LiveContainer, detached launches).
void BootMark(const char* stage) {
  NSArray<NSString*>* paths =
      NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
  NSString* documents = [paths firstObject];
  if (!documents) {
    return;
  }
  NSString* path = [documents stringByAppendingPathComponent:@"goldeneye-boot.txt"];
  FILE* f = fopen([path fileSystemRepresentation], "a");
  if (f) {
    fprintf(f, "%s\n", stage);
    fclose(f);
  }
  NSLog(@"[goldeneye-boot] %s", stage);
}

class IOSWindowedAppContext final : public rex::ui::WindowedAppContext {
 private:
  void NotifyUILoopOfPendingFunctions() override {
    dispatch_async(dispatch_get_main_queue(), ^{
      ExecutePendingFunctionsFromUIThread();
    });
  }

  void PlatformQuitFromUIThread() override {
    // iOS apps have no orderly programmatic exit; the guest teardown path on
    // Apple already ends at the process boundary (see the macOS entry point).
    std::_Exit(EXIT_SUCCESS);
  }
};

// Command-line remainder captured in main() before UIApplicationMain.
std::vector<std::string> g_positional_arguments;

std::string DefaultGameDataRoot() {
  NSArray<NSString*>* paths =
      NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
  NSString* documents = [paths firstObject];
  if (!documents) {
    return {};
  }
  // Accept both the macOS launcher's folder name and a space-less variant.
  NSFileManager* fm = [NSFileManager defaultManager];
  for (NSString* candidate in @[ @"Game Data", @"GameData" ]) {
    NSString* path = [documents stringByAppendingPathComponent:candidate];
    BOOL is_directory = NO;
    if ([fm fileExistsAtPath:path isDirectory:&is_directory] && is_directory) {
      return std::string([path UTF8String]);
    }
  }
  // Nothing imported yet: report the canonical location so the setup screen
  // and the runtime's error mention where to drop the data.
  return std::string(
      [[documents stringByAppendingPathComponent:@"Game Data"] UTF8String]);
}

// The boot is gated on this instead of letting the runtime abort: a fresh
// native install has no game data yet, and exiting at launch looks like a
// crash. default.xex is enough of a fingerprint for the gate; full
// validation still happens in the runtime.
bool GameDataPresent() {
  const std::string root = DefaultGameDataRoot();
  if (root.empty()) {
    return false;
  }
  NSString* path = [NSString stringWithUTF8String:root.c_str()];
  NSFileManager* fm = [NSFileManager defaultManager];
  BOOL is_directory = NO;
  if (![fm fileExistsAtPath:path isDirectory:&is_directory] || !is_directory) {
    return false;
  }
  return [fm fileExistsAtPath:[path stringByAppendingPathComponent:@"default.xex"]];
}

}  // namespace

@interface RexIOSAppDelegate : UIResponder <UIApplicationDelegate> {
 @private
  std::unique_ptr<IOSWindowedAppContext> app_context_;
  std::unique_ptr<rex::ui::WindowedApp> app_;
  NSTimer* pending_functions_timer_;
  UIWindow* setup_window_;
  UILabel* setup_status_label_;
}
@end

@implementation RexIOSAppDelegate

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
  (void)application;
  (void)launchOptions;

  BootMark("didFinishLaunching");
  if (GameDataPresent()) {
    [self bootGame];
  } else {
    BootMark("game data missing - setup screen");
    [self showSetupScreen];
  }
  return YES;
}

- (void)bootGame {
  app_context_ = std::make_unique<IOSWindowedAppContext>();
  app_ = rex::ui::GetWindowedAppCreator()(*app_context_);
  BootMark("app created");

  const auto& option_names = app_->GetPositionalOptions();
  std::map<std::string, std::string> parsed;
  size_t count = std::min(g_positional_arguments.size(), option_names.size());
  for (size_t i = 0; i < count; ++i) {
    parsed[option_names[i]] = g_positional_arguments[i];
  }
  app_->SetParsedArguments(std::move(parsed));

  if (!app_->OnInitialize()) {
    BootMark("OnInitialize FAILED");
    std::_Exit(EXIT_FAILURE);
  }
  BootMark("OnInitialize ok");

  // On-screen controls when no physical controller is connected; the game
  // window exists once OnInitialize returns. The app pointer outlives the
  // overlay (both live until process exit).
  rex::ui::WindowedApp* app = app_.get();
  rex::input::ios::InstallTouchGamepad([app] { app->OnHostMenuRequested(); });

  // Safety net alongside NotifyUILoopOfPendingFunctions: matches the 1 ms
  // cadence of the macOS manual loop.
  pending_functions_timer_ =
      [NSTimer scheduledTimerWithTimeInterval:0.001
                                      repeats:YES
                                        block:^(NSTimer* timer) {
                                          (void)timer;
                                          if (app_context_) {
                                            app_context_->ExecutePendingFunctionsFromUIThread();
                                          }
                                        }];
}

// First-run screen shown instead of aborting when no game data has been
// dropped into Documents yet. "Rescan" re-checks and boots in place, so the
// folder can be copied in via Files split view without relaunching.
- (void)showSetupScreen {
  setup_window_ = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
  for (UIScene* scene in [[UIApplication sharedApplication] connectedScenes]) {
    if ([scene isKindOfClass:[UIWindowScene class]]) {
      [setup_window_ setWindowScene:(UIWindowScene*)scene];
      break;
    }
  }

  UIViewController* controller = [[UIViewController alloc] init];
  UIView* root = controller.view;
  root.backgroundColor = [UIColor colorWithWhite:0.07 alpha:1.0];

  UILabel* title = [[UILabel alloc] init];
  title.text = @"Game data not found";
  title.font = [UIFont boldSystemFontOfSize:28];
  title.textColor = [UIColor whiteColor];
  title.textAlignment = NSTextAlignmentCenter;

  UILabel* body = [[UILabel alloc] init];
  body.text = [NSString
      stringWithFormat:
          @"Import your legally owned game backup with the GoldenEye Metal launcher on a Mac, "
          @"then copy the resulting “Game Data” folder (default.xex, files, music.xwb, "
          @"sfx.xwb) into this app's Documents folder using the Files app:\n\n"
          @"On My iPad ▸ GoldenEye ▸ Game Data\n\n"
          @"You can keep this screen open, copy the folder in split view, and tap Rescan."];
  body.font = [UIFont systemFontOfSize:17];
  body.textColor = [UIColor colorWithWhite:0.75 alpha:1.0];
  body.textAlignment = NSTextAlignmentCenter;
  body.numberOfLines = 0;

  setup_status_label_ = [[UILabel alloc] init];
  setup_status_label_.text = @"";
  setup_status_label_.font = [UIFont systemFontOfSize:15];
  setup_status_label_.textColor = [UIColor colorWithRed:0.9 green:0.6 blue:0.3 alpha:1.0];
  setup_status_label_.textAlignment = NSTextAlignmentCenter;
  setup_status_label_.numberOfLines = 0;

  UIButton* rescan = [UIButton buttonWithType:UIButtonTypeSystem];
  [rescan setTitle:@"Rescan" forState:UIControlStateNormal];
  rescan.titleLabel.font = [UIFont boldSystemFontOfSize:20];
  rescan.backgroundColor = [UIColor colorWithRed:0.83 green:0.68 blue:0.28 alpha:1.0];
  [rescan setTitleColor:[UIColor blackColor] forState:UIControlStateNormal];
  rescan.layer.cornerRadius = 12;
  [rescan addTarget:self
                action:@selector(rescanTapped)
      forControlEvents:UIControlEventTouchUpInside];

  UIStackView* stack = [[UIStackView alloc]
      initWithArrangedSubviews:@[ title, body, rescan, setup_status_label_ ]];
  stack.axis = UILayoutConstraintAxisVertical;
  stack.spacing = 24;
  stack.alignment = UIStackViewAlignmentCenter;
  stack.translatesAutoresizingMaskIntoConstraints = NO;
  [root addSubview:stack];
  [NSLayoutConstraint activateConstraints:@[
    [stack.centerXAnchor constraintEqualToAnchor:root.centerXAnchor],
    [stack.centerYAnchor constraintEqualToAnchor:root.centerYAnchor],
    [stack.widthAnchor constraintLessThanOrEqualToAnchor:root.widthAnchor multiplier:0.7],
    [body.widthAnchor constraintLessThanOrEqualToConstant:560],
    [rescan.widthAnchor constraintEqualToConstant:220],
    [rescan.heightAnchor constraintEqualToConstant:52],
  ]];

  setup_window_.rootViewController = controller;
  [controller release];
  [title release];
  [body release];
  [setup_window_ makeKeyAndVisible];
}

- (void)rescanTapped {
  if (!GameDataPresent()) {
    setup_status_label_.text = [NSString
        stringWithFormat:@"Still not found at:\n%s", DefaultGameDataRoot().c_str()];
    return;
  }
  BootMark("rescan found game data");
  [setup_window_ setHidden:YES];
  [setup_window_ release];
  setup_window_ = nil;
  setup_status_label_ = nil;
  [self bootGame];
}

- (void)applicationWillResignActive:(UIApplication*)application {
  (void)application;
  // The process may be suspended right after backgrounding; park the game in
  // its real paused state so it survives (and resumes from) the freeze.
  if (app_) {
    app_->OnEnterBackground();
  }
}

- (void)applicationDidEnterBackground:(UIApplication*)application {
  (void)application;
  // The process is about to be frozen while host clocks keep running; stop
  // guest-visible time so the game's hang watchdogs never see the gap.
  rex::chrono::Clock::BeginHostSuspend();
}

- (void)applicationWillEnterForeground:(UIApplication*)application {
  (void)application;
  rex::chrono::Clock::EndHostSuspend();
}

- (void)applicationDidBecomeActive:(UIApplication*)application {
  (void)application;
  // iOS interrupts the audio session while backgrounded. Without this the
  // guest waits forever on audio buffer completions after closing the pause
  // menu - frozen world, live UI.
  rex::audio::sdl::ResumeAllDevicesForForeground();
}

- (void)applicationWillTerminate:(UIApplication*)application {
  (void)application;
  if (pending_functions_timer_) {
    [pending_functions_timer_ invalidate];
    pending_functions_timer_ = nil;
  }
  if (app_) {
    app_->InvokeOnDestroy();
    app_.reset();
  }
  rex::ShutdownLogging();
}

@end

int main(int argc, char** argv) {
  @autoreleasepool {
    BootMark("main");
    SDL_SetMainReady();
    // iPad defaults; explicit arguments and environment still win.
    setenv("REX_INPUT_BACKEND", "sdl", /*overwrite=*/0);

    // Keep the runtime log next to the game data, readable from the Files
    // app; the default per-user Logs dir is invisible on device.
    NSArray<NSString*>* doc_paths =
        NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString* documents = [doc_paths firstObject];
    if (documents) {
      NSString* log_path = [documents stringByAppendingPathComponent:@"goldeneye.log"];
      setenv("REX_LOG_FILE", [log_path fileSystemRepresentation], /*overwrite=*/0);
    }

    std::vector<char*> args(argv, argv + argc);
    std::string game_data_flag = "--game_data_root";
    bool has_game_data_root = false;
    for (int i = 1; i < argc; ++i) {
      if (game_data_flag == argv[i] ||
          std::string(argv[i]).rfind(game_data_flag + "=", 0) == 0) {
        has_game_data_root = true;
        break;
      }
    }
    std::string default_root;
    if (!has_game_data_root) {
      default_root = DefaultGameDataRoot();
      if (!default_root.empty()) {
        args.push_back(game_data_flag.data());
        args.push_back(default_root.data());
      }
    }

    auto remaining = rex::cvar::Init(static_cast<int>(args.size()), args.data());
    rex::cvar::ApplyEnvironment();
    rex::InitLoggingEarly();
    g_positional_arguments.assign(remaining.begin(), remaining.end());

    BootMark("pre UIApplicationMain");
    return UIApplicationMain(argc, argv, nil, @"RexIOSAppDelegate");
  }
}

#endif  // REX_PLATFORM_IOS
