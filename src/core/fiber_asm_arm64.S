/**
 * @file        rex/core/fiber_asm_arm64.S
 * @brief       arm64 context switch for rex::thread::Fiber (Apple platforms)
 *
 * ucontext is deprecated on macOS and absent from the iOS SDK, so Apple
 * builds switch fibers with a boost.context-style register swap.
 * See docs/ipad/adr/0001-fibers-asm-arm64.md.
 *
 * Frame layout on the suspended fiber's stack (160 bytes, 16-aligned):
 *   [0x00] x19 x20   [0x10] x21 x22   [0x20] x23 x24   [0x30] x25 x26
 *   [0x40] x27 x28   [0x50] x29 x30   [0x60] d8  d9    [0x70] d10 d11
 *   [0x80] d12 d13   [0x90] d14 d15
 *
 * fiber_asm.cpp seeds a new fiber's frame with x19=entry, x20=arg and
 * x30=rex_fiber_entry_thunk so the first swap "returns" into the thunk.
 */

#if defined(__APPLE__) && defined(__aarch64__)

.text
.p2align 2

// void rex_fiber_swap(void** save_sp /*x0*/, void* target_sp /*x1*/)
.globl _rex_fiber_swap
_rex_fiber_swap:
    sub  sp, sp, #0xA0
    stp  x19, x20, [sp, #0x00]
    stp  x21, x22, [sp, #0x10]
    stp  x23, x24, [sp, #0x20]
    stp  x25, x26, [sp, #0x30]
    stp  x27, x28, [sp, #0x40]
    stp  x29, x30, [sp, #0x50]
    stp  d8,  d9,  [sp, #0x60]
    stp  d10, d11, [sp, #0x70]
    stp  d12, d13, [sp, #0x80]
    stp  d14, d15, [sp, #0x90]
    mov  x9, sp
    str  x9, [x0]
    mov  sp, x1
    ldp  x19, x20, [sp, #0x00]
    ldp  x21, x22, [sp, #0x10]
    ldp  x23, x24, [sp, #0x20]
    ldp  x25, x26, [sp, #0x30]
    ldp  x27, x28, [sp, #0x40]
    ldp  x29, x30, [sp, #0x50]
    ldp  d8,  d9,  [sp, #0x60]
    ldp  d10, d11, [sp, #0x70]
    ldp  d12, d13, [sp, #0x80]
    ldp  d14, d15, [sp, #0x90]
    add  sp, sp, #0xA0
    ret

// First-switch entry point for a new fiber: x19=entry, x20=arg were loaded
// from the seeded frame by rex_fiber_swap.
.globl _rex_fiber_entry_thunk
_rex_fiber_entry_thunk:
    mov  x0, x20
    blr  x19
    // A fiber entry must never return; it exits by switching away.
    brk  #0

#endif
