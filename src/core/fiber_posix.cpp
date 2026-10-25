/**
 * @file        rex/core/fiber_posix.cpp
 * @brief       POSIX backend for rex::thread::Fiber (makecontext/swapcontext)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
// Darwin hides the deprecated ucontext API unless a POSIX/XSI feature level
// is requested before <ucontext.h> is included. fiber.h includes ucontext.h,
// so the feature macro must be set in this translation unit first.
#define _XOPEN_SOURCE 700
#endif

#include <rex/platform.h>
#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC

#include <rex/thread/fiber.h>

#include <cassert>

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

#if REX_PLATFORM_ANDROID

#if !defined(__aarch64__)
#error Android fibers are implemented for arm64 only.
#endif

// void rex_fiber_switch(void** save_stack_pointer, void* load_stack_pointer)
// Pushes the AAPCS64 callee-saved registers (x19-x30, d8-d15), stores sp,
// switches to the other stack and pops its registers. Returning through the
// restored x30 resumes the other fiber.
extern "C" void rex_fiber_switch(void** save_stack_pointer, void* load_stack_pointer);
asm(R"(
  .text
  .p2align 4
  .globl rex_fiber_switch
  .hidden rex_fiber_switch
  .type rex_fiber_switch, %function
rex_fiber_switch:
  sub sp, sp, #0xa0
  stp x19, x20, [sp, #0x00]
  stp x21, x22, [sp, #0x10]
  stp x23, x24, [sp, #0x20]
  stp x25, x26, [sp, #0x30]
  stp x27, x28, [sp, #0x40]
  stp x29, x30, [sp, #0x50]
  stp d8, d9, [sp, #0x60]
  stp d10, d11, [sp, #0x70]
  stp d12, d13, [sp, #0x80]
  stp d14, d15, [sp, #0x90]
  mov x2, sp
  str x2, [x0]
  mov sp, x1
  ldp x19, x20, [sp, #0x00]
  ldp x21, x22, [sp, #0x10]
  ldp x23, x24, [sp, #0x20]
  ldp x25, x26, [sp, #0x30]
  ldp x27, x28, [sp, #0x40]
  ldp x29, x30, [sp, #0x50]
  ldp d8, d9, [sp, #0x60]
  ldp d10, d11, [sp, #0x70]
  ldp d12, d13, [sp, #0x80]
  ldp d14, d15, [sp, #0x90]
  add sp, sp, #0xa0
  ret
  .size rex_fiber_switch, .-rex_fiber_switch
)");

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);
  // Initial frame popped by the first rex_fiber_switch into this fiber: all
  // registers zero except x30, so that switch "returns" into the trampoline.
  uintptr_t top = (uintptr_t(f->stack_.data()) + f->stack_.size()) & ~uintptr_t(15);
  auto* frame = reinterpret_cast<uint64_t*>(top - 0xa0);
  for (size_t i = 0; i < 0xa0 / sizeof(uint64_t); ++i) {
    frame[i] = 0;
  }
  frame[0x58 / sizeof(uint64_t)] = uint64_t(reinterpret_cast<uintptr_t>(&Fiber::Trampoline));
  f->stack_pointer_ = frame;
  return f;
}

/*static*/ void Fiber::Trampoline() {
  // tls_current_ was updated by SwitchTo before the switch landed here.
  Fiber* f = tls_current_;
  f->entry_(f->arg_);
  // Fiber entries must switch away instead of returning: there is no frame
  // to return to.
  __builtin_trap();
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  rex_fiber_switch(&from->stack_pointer_, target->stack_pointer_);
}

#else  // REX_PLATFORM_ANDROID

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  if (getcontext(&f->context_) == -1) {
    delete f;
    return nullptr;
  }
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);

  if (getcontext(&f->context_) == -1) {
    delete f;
    return nullptr;
  }
  f->context_.uc_stack.ss_sp = f->stack_.data();
  f->context_.uc_stack.ss_size = f->stack_.size();
  f->context_.uc_link = nullptr;
  // Trampoline reads entry_/arg_ from tls_current_ — no pointer splitting needed.
  makecontext(&f->context_, &Fiber::Trampoline, 0);
  return f;
}

/*static*/ void Fiber::Trampoline() {
  // tls_current_ was updated by SwitchTo before swapcontext returned here.
  Fiber* f = tls_current_;
  f->entry_(f->arg_);
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  swapcontext(&from->context_, &target->context_);
}

#endif  // REX_PLATFORM_ANDROID

void Fiber::Destroy() {
  // Thread fibers are destroyed from the owning thread itself.
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
  }
  // No POSIX equivalent of ConvertFiberToThread; stack_ is freed by the vector destructor.
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_LINUX || REX_PLATFORM_MAC
