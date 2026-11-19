// SPDX-License-Identifier: GPL-3.0-or-later
// The A32 entry of the 32-bit Platform loader: every export slot
// (a32_slot.inc) branches here with its id in r12. The argument registers
// r0-r3 are stored below the caller's stack arguments, so the frame is the
// AAPCS32 argument area in one piece; venture_platform_a32_call (a32_platform.cpp)
// converts the call, runs the handler and writes the result to frame[0..1],
// which return in r0:r1. Android's armeabi-v7a is softfp: floating-point
// arguments and results travel in core registers too.
  .syntax unified
  .arm
  .text
  .globl venture_platform_a32_entry
  .hidden venture_platform_a32_entry
  .type venture_platform_a32_entry, %function
  .p2align 2
venture_platform_a32_entry:
  push {r0-r3}            // frame[0..3]; the stack arguments follow at frame + 16
  push {r4, lr}           // 24 bytes in all: sp stays 8-byte aligned
  add r0, sp, #8          // frame
  mov r1, r12             // id
  bl venture_platform_a32_call
  pop {r4, lr}
  pop {r0-r3}
  bx lr
  .size venture_platform_a32_entry, . - venture_platform_a32_entry
  .section .note.GNU-stack, "", %progbits
