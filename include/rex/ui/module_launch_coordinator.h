#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>

namespace rex::ui {

// Serializes the narrow guest-thread lifecycle boundary where a deferred
// launch may race an application shutdown. Pre-launch setup, preparing,
// publishing, resuming and forced termination have a single, testable ordering
// contract.
class ModuleLaunchCoordinator {
 public:
  using Callback = std::function<void()>;
  // true terminates/waits/disposes the launch thread; false only relinquishes
  // launch ownership (used by macOS immediately before process-boundary exit).
  using LaunchThreadCleanup = std::function<void(bool)>;

  enum class PrepareResult {
    kPrepared,
    kCancelled,
    kFailed,
  };

  enum class CommitResult {
    kRunning,
    kCancelled,
    kResumeFailed,
    kInvalidState,
  };

  // Runs setup that must finish before a guest thread may be prepared. The
  // callback is serialized against BeginShutdown and must not call it
  // reentrantly.
  bool RunPreLaunch(const Callback& callback);

  // Creates a suspended guest thread and atomically arms its cancellation
  // callback before shutdown can observe the prepared state.
  PrepareResult PrepareLaunch(const std::function<bool()>& prepare,
                              LaunchThreadCleanup launch_thread_cleanup);

  // Runs the post-prepare hook and Resume as one commit against shutdown.
  // If Resume fails, failure_cleanup runs before the armed cancellation.
  CommitResult CommitLaunch(const Callback& post_launch, const std::function<bool()>& resume,
                            const Callback& failure_cleanup);

  // Marks shutdown first so no later launch phase can commit, then stops
  // observers, cancels any suspended prepared thread, and terminates the title
  // in that order. A running launch thread is explicitly cleaned after title
  // termination when terminate_running_launch_thread is true.
  // Returns false when shutdown was already started.
  bool BeginShutdown(const Callback& before_terminate, const Callback& terminate_title,
                     bool terminate_running_launch_thread);

  // Releases the retained launch-thread cleanup after a normal guest exit.
  // This may perform the final wait/disposal, so shutdown waits for it.
  void GuestExited();

  bool is_shutting_down() const;

 private:
  enum class State {
    kIdle,
    kPreLaunchComplete,
    kPrepared,
    kRunning,
    kCleaningResumeFailure,
    kCleaningGuestExit,
    kLaunchFailed,
    kLaunchFinished,
    kShuttingDown,
    kTerminated,
  };

  mutable std::mutex mutex_;
  std::condition_variable state_changed_;
  State state_ = State::kIdle;
  LaunchThreadCleanup launch_thread_cleanup_;
};

}  // namespace rex::ui
