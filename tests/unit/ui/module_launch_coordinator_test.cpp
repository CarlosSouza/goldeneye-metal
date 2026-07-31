#include <rex/ui/module_launch_coordinator.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct FakePreparedThread {
  std::atomic<int> prepare_count = 0;
  std::atomic<int> post_launch_count = 0;
  std::atomic<int> resume_count = 0;
  std::atomic<int> terminate_count = 0;
  std::atomic<int> wait_count = 0;
  std::atomic<int> disarm_count = 0;
  std::atomic<int> waiter_count = 0;
  bool resume_succeeds = true;

  rex::ui::ModuleLaunchCoordinator::LaunchThreadCleanup MakeCleanup() {
    return [this](bool terminate) {
      if (terminate) {
        ++terminate_count;
        ++wait_count;
      } else {
        ++disarm_count;
      }
    };
  }
};

}  // namespace

TEST_CASE("Shutdown before deferred launch drain prevents every launch phase", "[ui][lifecycle]") {
  rex::ui::ModuleLaunchCoordinator coordinator;
  FakePreparedThread thread;
  int pre_launch_count = 0;
  int observer_stop_count = 0;
  int title_terminate_count = 0;

  std::vector<std::function<void()>> ui_queue;
  ui_queue.emplace_back([&]() {
    if (!coordinator.RunPreLaunch([&]() { ++pre_launch_count; })) {
      return;
    }
    const auto prepared = coordinator.PrepareLaunch(
        [&]() {
          ++thread.prepare_count;
          return true;
        },
        thread.MakeCleanup());
    if (prepared != rex::ui::ModuleLaunchCoordinator::PrepareResult::kPrepared) {
      return;
    }
    const auto committed = coordinator.CommitLaunch([&]() { ++thread.post_launch_count; },
                                                    [&]() {
                                                      ++thread.resume_count;
                                                      return true;
                                                    },
                                                    []() {});
    if (committed == rex::ui::ModuleLaunchCoordinator::CommitResult::kRunning) {
      ++thread.waiter_count;
    }
  });

  CHECK(coordinator.BeginShutdown([&]() { ++observer_stop_count; },
                                  [&]() { ++title_terminate_count; }, true));
  ui_queue.front()();

  CHECK(pre_launch_count == 0);
  CHECK(thread.prepare_count == 0);
  CHECK(thread.post_launch_count == 0);
  CHECK(thread.resume_count == 0);
  CHECK(thread.waiter_count == 0);
  CHECK(observer_stop_count == 1);
  CHECK(title_terminate_count == 1);
}

TEST_CASE("Shutdown after suspended prepare cancels and waits exactly once", "[ui][lifecycle]") {
  rex::ui::ModuleLaunchCoordinator coordinator;
  FakePreparedThread thread;
  std::mutex barrier_mutex;
  std::condition_variable barrier;
  bool prepared = false;
  bool release_launch = false;
  bool pre_launch_succeeded = false;
  rex::ui::ModuleLaunchCoordinator::PrepareResult prepare_result =
      rex::ui::ModuleLaunchCoordinator::PrepareResult::kFailed;
  rex::ui::ModuleLaunchCoordinator::CommitResult commit_result =
      rex::ui::ModuleLaunchCoordinator::CommitResult::kInvalidState;

  std::thread launch([&]() {
    pre_launch_succeeded = coordinator.RunPreLaunch([]() {});
    prepare_result = coordinator.PrepareLaunch(
        [&]() {
          ++thread.prepare_count;
          return true;
        },
        thread.MakeCleanup());
    {
      std::unique_lock lock(barrier_mutex);
      prepared = true;
      barrier.notify_all();
      barrier.wait(lock, [&]() { return release_launch; });
    }
    commit_result = coordinator.CommitLaunch([&]() { ++thread.post_launch_count; },
                                             [&]() {
                                               ++thread.resume_count;
                                               return true;
                                             },
                                             []() {});
  });

  {
    std::unique_lock lock(barrier_mutex);
    barrier.wait(lock, [&]() { return prepared; });
  }
  CHECK(coordinator.BeginShutdown([]() {}, []() {}, true));
  {
    std::lock_guard lock(barrier_mutex);
    release_launch = true;
  }
  barrier.notify_all();
  launch.join();

  CHECK(pre_launch_succeeded);
  CHECK(prepare_result == rex::ui::ModuleLaunchCoordinator::PrepareResult::kPrepared);
  CHECK(commit_result == rex::ui::ModuleLaunchCoordinator::CommitResult::kCancelled);
  CHECK(thread.terminate_count == 1);
  CHECK(thread.wait_count == 1);
  CHECK(thread.post_launch_count == 0);
  CHECK(thread.resume_count == 0);
}

TEST_CASE("Resume commit and shutdown have a single serialized order", "[ui][lifecycle]") {
  rex::ui::ModuleLaunchCoordinator coordinator;
  FakePreparedThread thread;
  std::mutex barrier_mutex;
  std::condition_variable barrier;
  bool commit_entered = false;
  bool release_commit = false;
  std::mutex events_mutex;
  std::vector<std::string> events;
  rex::ui::ModuleLaunchCoordinator::CommitResult commit_result =
      rex::ui::ModuleLaunchCoordinator::CommitResult::kInvalidState;
  bool shutdown_succeeded = false;

  REQUIRE(coordinator.RunPreLaunch([]() {}));
  REQUIRE(coordinator.PrepareLaunch(
              [&]() {
                ++thread.prepare_count;
                return true;
              },
              thread.MakeCleanup()) == rex::ui::ModuleLaunchCoordinator::PrepareResult::kPrepared);

  std::thread launch([&]() {
    commit_result = coordinator.CommitLaunch(
        [&]() {
          ++thread.post_launch_count;
          {
            std::lock_guard events_lock(events_mutex);
            events.emplace_back("post");
          }
          std::unique_lock lock(barrier_mutex);
          commit_entered = true;
          barrier.notify_all();
          barrier.wait(lock, [&]() { return release_commit; });
        },
        [&]() {
          ++thread.resume_count;
          std::lock_guard lock(events_mutex);
          events.emplace_back("resume");
          return true;
        },
        []() {});
  });

  {
    std::unique_lock lock(barrier_mutex);
    barrier.wait(lock, [&]() { return commit_entered; });
  }
  std::thread shutdown([&]() {
    shutdown_succeeded = coordinator.BeginShutdown(
        [&]() {
          std::lock_guard lock(events_mutex);
          events.emplace_back("stop");
        },
        [&]() {
          std::lock_guard lock(events_mutex);
          events.emplace_back("terminate");
        },
        true);
  });
  {
    std::lock_guard lock(barrier_mutex);
    release_commit = true;
  }
  barrier.notify_all();
  launch.join();
  shutdown.join();

  CHECK(commit_result == rex::ui::ModuleLaunchCoordinator::CommitResult::kRunning);
  CHECK(shutdown_succeeded);
  CHECK(thread.resume_count == 1);
  CHECK(thread.terminate_count == 1);
  CHECK(thread.wait_count == 1);
  CHECK(thread.disarm_count == 0);
  REQUIRE(events.size() == 4);
  CHECK(events[0] == "post");
  CHECK(events[1] == "resume");
  CHECK(events[2] == "stop");
  CHECK(events[3] == "terminate");
}

TEST_CASE("Resume failure stops observers and disposes without spawning a waiter",
          "[ui][lifecycle]") {
  rex::ui::ModuleLaunchCoordinator coordinator;
  FakePreparedThread thread;
  thread.resume_succeeds = false;
  int observer_stop_count = 0;

  REQUIRE(coordinator.RunPreLaunch([]() {}));
  REQUIRE(coordinator.PrepareLaunch(
              [&]() {
                ++thread.prepare_count;
                return true;
              },
              thread.MakeCleanup()) == rex::ui::ModuleLaunchCoordinator::PrepareResult::kPrepared);

  const auto result = coordinator.CommitLaunch([&]() { ++thread.post_launch_count; },
                                               [&]() {
                                                 ++thread.resume_count;
                                                 return thread.resume_succeeds;
                                               },
                                               [&]() { ++observer_stop_count; });
  if (result == rex::ui::ModuleLaunchCoordinator::CommitResult::kRunning) {
    ++thread.waiter_count;
  }

  CHECK(result == rex::ui::ModuleLaunchCoordinator::CommitResult::kResumeFailed);
  CHECK(thread.post_launch_count == 1);
  CHECK(thread.resume_count == 1);
  CHECK(observer_stop_count == 1);
  CHECK(thread.terminate_count == 1);
  CHECK(thread.wait_count == 1);
  CHECK(thread.waiter_count == 0);
}

TEST_CASE("Observer stop always precedes title termination", "[ui][lifecycle]") {
  rex::ui::ModuleLaunchCoordinator coordinator;
  std::vector<std::string> order;
  REQUIRE(coordinator.BeginShutdown([&]() { order.emplace_back("stop"); },
                                    [&]() { order.emplace_back("terminate"); }, true));
  REQUIRE(order.size() == 2);
  CHECK(order[0] == "stop");
  CHECK(order[1] == "terminate");
}

TEST_CASE("A resumed thread remains cancellable before its host callback runs", "[ui][lifecycle]") {
  rex::ui::ModuleLaunchCoordinator coordinator;
  FakePreparedThread thread;

  REQUIRE(coordinator.RunPreLaunch([]() {}));
  REQUIRE(coordinator.PrepareLaunch([]() { return true; }, thread.MakeCleanup()) ==
          rex::ui::ModuleLaunchCoordinator::PrepareResult::kPrepared);
  REQUIRE(coordinator.CommitLaunch([]() {}, []() { return true; }, []() {}) ==
          rex::ui::ModuleLaunchCoordinator::CommitResult::kRunning);

  CHECK(coordinator.BeginShutdown([]() {}, []() {}, true));
  CHECK(thread.terminate_count == 1);
  CHECK(thread.wait_count == 1);
  CHECK(thread.disarm_count == 0);
}

TEST_CASE("macOS running shutdown relinquishes launch ownership for process exit",
          "[ui][lifecycle]") {
  rex::ui::ModuleLaunchCoordinator coordinator;
  FakePreparedThread thread;

  REQUIRE(coordinator.RunPreLaunch([]() {}));
  REQUIRE(coordinator.PrepareLaunch([]() { return true; }, thread.MakeCleanup()) ==
          rex::ui::ModuleLaunchCoordinator::PrepareResult::kPrepared);
  REQUIRE(coordinator.CommitLaunch([]() {}, []() { return true; }, []() {}) ==
          rex::ui::ModuleLaunchCoordinator::CommitResult::kRunning);

  CHECK(coordinator.BeginShutdown([]() {}, []() {}, false));
  CHECK(thread.terminate_count == 0);
  CHECK(thread.wait_count == 0);
  CHECK(thread.disarm_count == 1);
}

TEST_CASE("Shutdown waits until resume-failure cleanup is complete", "[ui][lifecycle]") {
  using namespace std::chrono_literals;

  rex::ui::ModuleLaunchCoordinator coordinator;
  FakePreparedThread thread;
  std::mutex barrier_mutex;
  std::condition_variable barrier;
  bool cleanup_entered = false;
  bool release_cleanup = false;
  std::atomic<bool> shutdown_entered = false;
  std::atomic<bool> shutdown_returned = false;
  std::atomic<int> observer_stop_count = 0;
  rex::ui::ModuleLaunchCoordinator::CommitResult commit_result =
      rex::ui::ModuleLaunchCoordinator::CommitResult::kInvalidState;

  REQUIRE(coordinator.RunPreLaunch([]() {}));
  REQUIRE(coordinator.PrepareLaunch([]() { return true; },
                                    [&](bool terminate) {
                                      if (terminate) {
                                        ++thread.terminate_count;
                                        ++thread.wait_count;
                                      }
                                      std::unique_lock lock(barrier_mutex);
                                      cleanup_entered = true;
                                      barrier.notify_all();
                                      barrier.wait(lock, [&]() { return release_cleanup; });
                                    }) ==
          rex::ui::ModuleLaunchCoordinator::PrepareResult::kPrepared);

  std::thread launch([&]() {
    commit_result = coordinator.CommitLaunch([]() {}, []() { return false; }, []() {});
  });
  {
    std::unique_lock lock(barrier_mutex);
    barrier.wait(lock, [&]() { return cleanup_entered; });
  }

  std::thread shutdown([&]() {
    shutdown_entered.store(true, std::memory_order_release);
    const bool result = coordinator.BeginShutdown([&]() { ++observer_stop_count; }, []() {}, true);
    shutdown_returned.store(result, std::memory_order_release);
  });
  while (!shutdown_entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  {
    std::unique_lock lock(barrier_mutex);
    CHECK_FALSE(barrier.wait_for(
        lock, 50ms, [&]() { return shutdown_returned.load(std::memory_order_acquire); }));
    release_cleanup = true;
  }
  barrier.notify_all();
  launch.join();
  shutdown.join();

  CHECK(commit_result == rex::ui::ModuleLaunchCoordinator::CommitResult::kResumeFailed);
  CHECK(shutdown_returned.load(std::memory_order_acquire));
  CHECK(observer_stop_count == 1);
  CHECK(thread.terminate_count == 1);
  CHECK(thread.wait_count == 1);
}

TEST_CASE("Normal guest exit disposes the retained launch thread once", "[ui][lifecycle]") {
  rex::ui::ModuleLaunchCoordinator coordinator;
  FakePreparedThread thread;

  REQUIRE(coordinator.RunPreLaunch([]() {}));
  REQUIRE(coordinator.PrepareLaunch([]() { return true; }, thread.MakeCleanup()) ==
          rex::ui::ModuleLaunchCoordinator::PrepareResult::kPrepared);
  REQUIRE(coordinator.CommitLaunch([]() {}, []() { return true; }, []() {}) ==
          rex::ui::ModuleLaunchCoordinator::CommitResult::kRunning);

  coordinator.GuestExited();
  CHECK(thread.terminate_count == 1);
  CHECK(thread.wait_count == 1);
  CHECK(coordinator.BeginShutdown([]() {}, []() {}, true));
  CHECK(thread.terminate_count == 1);
  CHECK(thread.wait_count == 1);
}
