/**
 * @file        rex/core/fiber_asm.cpp
 * @brief       Apple arm64 backend for rex::thread::Fiber (asm context switch)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#if REX_PLATFORM_MAC && REX_ARCH_ARM64

#include <rex/thread/fiber.h>

#include <cassert>
#include <cstdint>
#include <cstring>

extern "C" {
void rex_fiber_swap(void** save_sp, void* target_sp);
void rex_fiber_entry_thunk();
}

namespace rex::thread {

namespace {
// Must match the frame layout in fiber_asm_arm64.S.
constexpr size_t kFrameBytes = 0xA0;
constexpr size_t kFrameX19 = 0;    // entry
constexpr size_t kFrameX20 = 1;    // arg
constexpr size_t kFrameX30 = 11;   // resume address
}  // namespace

thread_local Fiber* Fiber::tls_current_ = nullptr;

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  f->is_thread_fiber_ = true;
  // sp_ is captured by the first SwitchTo away from this fiber.
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);

  // Seed the initial frame so the first rex_fiber_swap into this fiber
  // "returns" into rex_fiber_entry_thunk with x19=entry, x20=arg.
  auto top = reinterpret_cast<uintptr_t>(f->stack_.data() + f->stack_.size());
  top &= ~uintptr_t{15};
  auto* frame = reinterpret_cast<uint64_t*>(top - kFrameBytes);
  std::memset(frame, 0, kFrameBytes);
  frame[kFrameX19] = reinterpret_cast<uint64_t>(entry);
  frame[kFrameX20] = reinterpret_cast<uint64_t>(arg);
  frame[kFrameX30] = reinterpret_cast<uint64_t>(&rex_fiber_entry_thunk);
  f->sp_ = frame;
  return f;
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  rex_fiber_swap(&from->sp_, target->sp_);
}

void Fiber::Destroy() {
  // Thread fibers are destroyed from the owning thread itself.
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
  }
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_MAC && REX_ARCH_ARM64
