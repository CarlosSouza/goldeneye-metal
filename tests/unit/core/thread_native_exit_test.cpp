#include <rex/platform.h>
#include <rex/thread.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <pthread.h>
#endif

using namespace std::chrono_literals;

TEST_CASE("A suspended native thread can be terminated without executing",
          "[thread][lifecycle]") {
  std::atomic<bool> executed = false;
  rex::thread::Thread::CreationParameters parameters;
  parameters.create_suspended = true;
  auto thread =
      rex::thread::Thread::Create(parameters, [&]() { executed.store(true); });
  REQUIRE(thread);

  thread->Terminate(0);
  CHECK(thread->WaitForExitUntil(std::chrono::steady_clock::now() + 2s) ==
        rex::thread::WaitResult::kSuccess);
  CHECK_FALSE(executed.load());
}

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC

TEST_CASE("A cancellation-disabled native thread respects an absolute exit deadline",
          "[thread][lifecycle]") {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;
  std::atomic<int> cancel_disable_result = -1;

  rex::thread::Thread::CreationParameters parameters;
  auto thread = rex::thread::Thread::Create(parameters, [&]() {
    cancel_disable_result.store(
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, nullptr));
    {
      std::lock_guard lock(mutex);
      entered = true;
    }
    condition.notify_all();
    std::unique_lock lock(mutex);
    condition.wait(lock, [&]() { return release; });
    // Return normally with cancellation still disabled. This exercises the
    // cleanup-handler signal and the reaper's stronger joined completion.
  });
  REQUIRE(thread);

  {
    std::unique_lock lock(mutex);
    REQUIRE(condition.wait_for(lock, 2s, [&]() { return entered; }));
  }
  REQUIRE(cancel_disable_result.load() == 0);
  thread->Terminate(0);

  const auto started = std::chrono::steady_clock::now();
  CHECK(thread->WaitForExitUntil(started + 40ms) ==
        rex::thread::WaitResult::kTimeout);
  CHECK(std::chrono::steady_clock::now() - started < 500ms);
  CHECK(rex::thread::Wait(thread.get(), false, 20ms) ==
        rex::thread::WaitResult::kTimeout);

  {
    std::lock_guard lock(mutex);
    release = true;
  }
  condition.notify_all();
  CHECK(thread->WaitForExitUntil(std::chrono::steady_clock::now() + 2s) ==
        rex::thread::WaitResult::kSuccess);
}

TEST_CASE("A cancellation-disabled target can self-exit after an external request",
          "[thread][lifecycle]") {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool self_exit = false;
  std::atomic<int> cancel_disable_result = -1;

  rex::thread::Thread::CreationParameters parameters;
  auto thread = rex::thread::Thread::Create(parameters, [&]() {
    cancel_disable_result.store(
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, nullptr));
    {
      std::unique_lock lock(mutex);
      entered = true;
      condition.notify_all();
      condition.wait(lock, [&]() { return self_exit; });
    }
    rex::thread::Thread::Exit(0);
  });
  REQUIRE(thread);
  {
    std::unique_lock lock(mutex);
    REQUIRE(condition.wait_for(lock, 2s, [&]() { return entered; }));
  }
  REQUIRE(cancel_disable_result.load() == 0);

  thread->Terminate(0);
  {
    std::lock_guard lock(mutex);
    self_exit = true;
  }
  condition.notify_all();
  CHECK(thread->WaitForExitUntil(std::chrono::steady_clock::now() + 2s) ==
        rex::thread::WaitResult::kSuccess);
}

namespace {

struct TlsDestructorBarrier {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;
};

void BlockingTlsDestructor(void* value) {
  auto* barrier = static_cast<TlsDestructorBarrier*>(value);
  std::unique_lock lock(barrier->mutex);
  barrier->entered = true;
  barrier->condition.notify_all();
  barrier->condition.wait(lock, [&]() { return barrier->release; });
}

}  // namespace

TEST_CASE("Native exit completion includes host TLS destructors", "[thread][lifecycle]") {
  pthread_key_t key;
  REQUIRE(pthread_key_create(&key, BlockingTlsDestructor) == 0);
  TlsDestructorBarrier barrier;
  std::atomic<int> set_specific_result = -1;

  rex::thread::Thread::CreationParameters parameters;
  auto thread = rex::thread::Thread::Create(parameters, [&]() {
    set_specific_result.store(pthread_setspecific(key, &barrier));
  });
  REQUIRE(thread);

  {
    std::unique_lock lock(barrier.mutex);
    REQUIRE(barrier.condition.wait_for(lock, 2s, [&]() { return barrier.entered; }));
  }
  REQUIRE(set_specific_result.load() == 0);
  CHECK(rex::thread::Wait(thread.get(), false, 40ms) ==
        rex::thread::WaitResult::kTimeout);
  CHECK(thread->WaitForExitUntil(std::chrono::steady_clock::now() + 40ms) ==
        rex::thread::WaitResult::kTimeout);

  {
    std::lock_guard lock(barrier.mutex);
    barrier.release = true;
  }
  barrier.condition.notify_all();
  CHECK(thread->WaitForExitUntil(std::chrono::steady_clock::now() + 2s) ==
        rex::thread::WaitResult::kSuccess);
  CHECK(pthread_key_delete(key) == 0);
}

#endif

TEST_CASE("Concurrent native-exit waiters share one completion", "[thread][lifecycle]") {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;

  rex::thread::Thread::CreationParameters parameters;
  auto thread = rex::thread::Thread::Create(parameters, [&]() {
    {
      std::lock_guard lock(mutex);
      entered = true;
    }
    condition.notify_all();
    std::unique_lock lock(mutex);
    condition.wait(lock, [&]() { return release; });
  });
  REQUIRE(thread);
  {
    std::unique_lock lock(mutex);
    REQUIRE(condition.wait_for(lock, 2s, [&]() { return entered; }));
  }

  std::atomic<int> successes = 0;
  auto wait = [&]() {
    if (thread->WaitForExitUntil(std::chrono::steady_clock::now() + 2s) ==
        rex::thread::WaitResult::kSuccess) {
      successes.fetch_add(1);
    }
  };
  std::thread first(wait);
  std::thread second(wait);
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  condition.notify_all();
  first.join();
  second.join();
  CHECK(successes.load() == 2);
}

TEST_CASE("A bounded thread wait includes exit callbacks without overrunning its timeout",
          "[thread][lifecycle]") {
  std::mutex mutex;
  std::condition_variable condition;
  bool callback_entered = false;
  bool release_callback = false;

  rex::thread::Thread::CreationParameters parameters;
  parameters.create_suspended = true;
  auto thread = rex::thread::Thread::Create(parameters, []() {});
  REQUIRE(thread);
  thread->SetExitCallback([&]() {
    std::unique_lock lock(mutex);
    callback_entered = true;
    condition.notify_all();
    condition.wait(lock, [&]() { return release_callback; });
  });
  REQUIRE(thread->Resume());

  {
    std::unique_lock lock(mutex);
    REQUIRE(condition.wait_for(lock, 2s, [&]() { return callback_entered; }));
  }
  const auto started = std::chrono::steady_clock::now();
  CHECK(rex::thread::Wait(thread.get(), false, 40ms) ==
        rex::thread::WaitResult::kTimeout);
  CHECK(std::chrono::steady_clock::now() - started < 500ms);

  {
    std::lock_guard lock(mutex);
    release_callback = true;
  }
  condition.notify_all();
  CHECK(thread->WaitForExitUntil(std::chrono::steady_clock::now() + 2s) ==
        rex::thread::WaitResult::kSuccess);
}

namespace {

struct CallbackCaptureReleaseBarrier {
  std::mutex mutex;
  std::condition_variable condition;
  bool destructor_entered = false;
  bool release_destructor = false;
};

struct BlockingCallbackCapture {
  explicit BlockingCallbackCapture(
      std::shared_ptr<CallbackCaptureReleaseBarrier> barrier)
      : barrier(std::move(barrier)) {}

  ~BlockingCallbackCapture() {
    std::unique_lock lock(barrier->mutex);
    barrier->destructor_entered = true;
    barrier->condition.notify_all();
    barrier->condition.wait(lock, [&]() { return barrier->release_destructor; });
  }

  std::shared_ptr<CallbackCaptureReleaseBarrier> barrier;
};

}  // namespace

TEST_CASE("Strong native exit completion releases callback captures before returning",
          "[thread][lifecycle]") {
  rex::thread::Thread::CreationParameters parameters;
  parameters.create_suspended = true;
  auto thread = rex::thread::Thread::Create(parameters, []() {});
  REQUIRE(thread);

  auto barrier = std::make_shared<CallbackCaptureReleaseBarrier>();
  auto capture = std::make_shared<BlockingCallbackCapture>(barrier);
  thread->SetExitCallback([capture]() {});
  capture.reset();
  REQUIRE(thread->Resume());

  std::atomic<bool> waiter_finished = false;
  rex::thread::WaitResult wait_result = rex::thread::WaitResult::kFailed;
  std::thread waiter([&]() {
    wait_result = thread->WaitForExitUntil(std::chrono::steady_clock::now() + 2s);
    waiter_finished.store(true, std::memory_order_release);
  });

  bool destructor_entered = false;
  {
    std::unique_lock lock(barrier->mutex);
    destructor_entered = barrier->condition.wait_for(
        lock, 2s, [&]() { return barrier->destructor_entered; });
  }
  if (destructor_entered) {
    std::this_thread::sleep_for(40ms);
    CHECK_FALSE(waiter_finished.load(std::memory_order_acquire));
  }

  {
    std::lock_guard lock(barrier->mutex);
    barrier->release_destructor = true;
  }
  barrier->condition.notify_all();
  waiter.join();

  REQUIRE(destructor_entered);
  CHECK(wait_result == rex::thread::WaitResult::kSuccess);
}

TEST_CASE("Strong native exit completion includes callbacks registered during capture release",
          "[thread][lifecycle]") {
  rex::thread::Thread::CreationParameters parameters;
  parameters.create_suspended = true;
  auto thread = rex::thread::Thread::Create(parameters, []() {});
  REQUIRE(thread);

  auto initial_barrier = std::make_shared<CallbackCaptureReleaseBarrier>();
  auto initial_capture = std::make_shared<BlockingCallbackCapture>(initial_barrier);
  thread->SetExitCallback([initial_capture]() {});
  initial_capture.reset();
  REQUIRE(thread->Resume());

  std::atomic<bool> waiter_finished = false;
  rex::thread::WaitResult wait_result = rex::thread::WaitResult::kFailed;
  std::thread waiter([&]() {
    wait_result = thread->WaitForExitUntil(std::chrono::steady_clock::now() + 2s);
    waiter_finished.store(true, std::memory_order_release);
  });

  bool initial_destructor_entered = false;
  {
    std::unique_lock lock(initial_barrier->mutex);
    initial_destructor_entered = initial_barrier->condition.wait_for(
        lock, 2s, [&]() { return initial_barrier->destructor_entered; });
  }

  auto late_barrier = std::make_shared<CallbackCaptureReleaseBarrier>();
  std::thread registrar([&thread, late_barrier]() {
    auto late_capture = std::make_shared<BlockingCallbackCapture>(late_barrier);
    thread->SetExitCallback([capture = std::move(late_capture)]() {});
  });

  bool late_destructor_entered = false;
  {
    std::unique_lock lock(late_barrier->mutex);
    late_destructor_entered = late_barrier->condition.wait_for(
        lock, 2s, [&]() { return late_barrier->destructor_entered; });
  }

  {
    std::lock_guard lock(initial_barrier->mutex);
    initial_barrier->release_destructor = true;
  }
  initial_barrier->condition.notify_all();

  if (initial_destructor_entered && late_destructor_entered) {
    std::this_thread::sleep_for(40ms);
    CHECK_FALSE(waiter_finished.load(std::memory_order_acquire));
  }

  {
    std::lock_guard lock(late_barrier->mutex);
    late_barrier->release_destructor = true;
  }
  late_barrier->condition.notify_all();
  registrar.join();
  waiter.join();

  REQUIRE(initial_destructor_entered);
  REQUIRE(late_destructor_entered);
  CHECK(wait_result == rex::thread::WaitResult::kSuccess);
}
