// SPDX-License-Identifier: GPL-3.0-or-later
// An export of the 32-bit Platform loader (armeabi-v7a, for ARM32 games on
// CPUs that run AArch32): r12 = the export's id, then the A32 entry
// (a32_entry.S), which keeps r0-r3 and the stack arguments for the
// conversion to the A64 handler (a32_call.h). ARM state; callers interwork.
//
//   VENTURE_A32_SLOT ovr_PopMessage, 42
  .syntax unified
.macro VENTURE_A32_SLOT name, id
  .globl \name
  .type \name, %function
  .p2align 3
  .arm
\name:
  movw r12, #(\id)
  b venture_platform_a32_entry
  .size \name, 8
.endm
