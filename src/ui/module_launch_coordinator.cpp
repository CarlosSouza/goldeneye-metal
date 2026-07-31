#include <rex/ui/module_launch_coordinator.h>

#include <utility>

namespace rex::ui {

bool ModuleLaunchCoordinator::RunPreLaunch(const Callback& callback) {
  std::lock_guard lock(mutex_);
  if (state_ != State::kIdle) {
    return false;
  }
  callback();
  state_ = State::kPreLaunchComplete;
  return true;
}

ModuleLaunchCoordinator::PrepareResult ModuleLaunchCoordinator::PrepareLaunch(
    const std::function<bool()>& prepare, LaunchThreadCleanup launch_thread_cleanup) {
  std::lock_guard lock(mutex_);
  if (state_ == State::kShuttingDown || state_ == State::kTerminated) {
    return PrepareResult::kCancelled;
  }
  if (state_ != State::kPreLaunchComplete || !launch_thread_cleanup) {
    return PrepareResult::kFailed;
  }
  if (!prepare()) {
    state_ = State::kLaunchFailed;
    return PrepareResult::kFailed;
  }
  launch_thread_cleanup_ = std::move(launch_thread_cleanup);
  state_ = State::kPrepared;
  return PrepareResult::kPrepared;
}

ModuleLaunchCoordinator::CommitResult ModuleLaunchCoordinator::CommitLaunch(
    const Callback& post_launch, const std::function<bool()>& resume,
    const Callback& failure_cleanup) {
  LaunchThreadCleanup launch_thread_cleanup;
  {
    std::unique_lock lock(mutex_);
    if (state_ == State::kShuttingDown || state_ == State::kTerminated) {
      return CommitResult::kCancelled;
    }
    if (state_ != State::kPrepared) {
      return CommitResult::kInvalidState;
    }

    post_launch();
    if (resume()) {
      state_ = State::kRunning;
      return CommitResult::kRunning;
    }

    launch_thread_cleanup = std::exchange(launch_thread_cleanup_, {});
    state_ = State::kCleaningResumeFailure;
  }

  failure_cleanup();
  if (launch_thread_cleanup) {
    launch_thread_cleanup(true);
  }
  {
    std::lock_guard lock(mutex_);
    state_ = State::kLaunchFailed;
  }
  state_changed_.notify_all();
  return CommitResult::kResumeFailed;
}

bool ModuleLaunchCoordinator::BeginShutdown(const Callback& before_terminate,
                                            const Callback& terminate_title,
                                            bool terminate_running_launch_thread) {
  LaunchThreadCleanup launch_thread_cleanup;
  bool launch_was_running = false;
  {
    std::unique_lock lock(mutex_);
    state_changed_.wait(lock, [this]() {
      return state_ != State::kCleaningResumeFailure && state_ != State::kCleaningGuestExit;
    });
    if (state_ == State::kShuttingDown || state_ == State::kTerminated) {
      return false;
    }
    launch_was_running = state_ == State::kRunning;
    state_ = State::kShuttingDown;
    launch_thread_cleanup = std::exchange(launch_thread_cleanup_, {});
  }

  before_terminate();
  terminate_title();
  if (launch_thread_cleanup) {
    launch_thread_cleanup(!launch_was_running || terminate_running_launch_thread);
  }

  {
    std::lock_guard lock(mutex_);
    state_ = State::kTerminated;
  }
  state_changed_.notify_all();
  return true;
}

void ModuleLaunchCoordinator::GuestExited() {
  LaunchThreadCleanup launch_thread_cleanup;
  {
    std::lock_guard lock(mutex_);
    if (state_ != State::kRunning) {
      return;
    }
    state_ = State::kCleaningGuestExit;
    launch_thread_cleanup = std::exchange(launch_thread_cleanup_, {});
  }
  if (launch_thread_cleanup) {
    launch_thread_cleanup(true);
  }
  {
    std::lock_guard lock(mutex_);
    state_ = State::kLaunchFinished;
  }
  state_changed_.notify_all();
}

bool ModuleLaunchCoordinator::is_shutting_down() const {
  std::lock_guard lock(mutex_);
  return state_ == State::kShuttingDown || state_ == State::kTerminated;
}

}  // namespace rex::ui
