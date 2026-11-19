// uint32_t venture_native_resume_frame(NativeContext* context, void* frame)
//
// The native-a64 engine's second way into guest code, for a context whose every
// register matters, x17 included: one left by a signal (an arbitrary interrupted
// pc), or edited by the thread loop. Records the host side exactly as
// venture_native_enter does, switches to the guest thread pointer, and restores
// the whole guest state with rt_sigreturn from frame (an rt_sigframe the engine
// filled from the context, on the host stack). Guest code leaves through the
// same routines as after venture_native_enter, so this returns the exit reason.
#include "cpu/native_context_offsets.h"

  .text
  .globl venture_native_resume_frame
  .hidden venture_native_resume_frame
  .type venture_native_resume_frame, %function
  .p2align 4
venture_native_resume_frame:
  add x2, x0, #VENTURE_NCTX_HOST_X19
  stp x19, x20, [x2, #0]
  stp x21, x22, [x2, #16]
  stp x23, x24, [x2, #32]
  stp x25, x26, [x2, #48]
  stp x27, x28, [x2, #64]
  stp x29, x30, [x2, #80]
  add x2, x0, #VENTURE_NCTX_HOST_D8
  stp d8, d9, [x2, #0]
  stp d10, d11, [x2, #16]
  stp d12, d13, [x2, #32]
  stp d14, d15, [x2, #48]
  mov x2, sp
  str x2, [x0, #VENTURE_NCTX_HOST_SP]
  mrs x2, tpidr_el0
  str x2, [x0, #VENTURE_NCTX_HOST_TPIDR]
  str x18, [x0, #VENTURE_NCTX_HOST_X18]
  mrs x2, fpcr
  str x2, [x0, #VENTURE_NCTX_HOST_FPCR]
  str wzr, [x0, #VENTURE_NCTX_EXIT_REASON]
  // From here to the svc a halt signal finds no guest pc: the handler leaves
  // through venture_native_leave_unentered instead (context in x0).
  .globl venture_native_resume_frame_window
  .hidden venture_native_resume_frame_window
venture_native_resume_frame_window:
  ldr w2, [x0, #VENTURE_NCTX_HALT]
  cbz w2, 1f
  mov w0, #VENTURE_NATIVE_EXIT_HALT
  ret
1:
  ldr w2, [x0, #VENTURE_NCTX_FLAGS]
  tbnz w2, #0, 2f
  ldr x2, [x0, #VENTURE_NCTX_TPIDR]
  msr tpidr_el0, x2
2:
  mov sp, x1
  mov x8, #139  // rt_sigreturn, from host text: the seccomp filter lets it through
  .globl venture_native_resume_frame_svc
  .hidden venture_native_resume_frame_svc
venture_native_resume_frame_svc:
  svc #0
  brk #0x5152
  .size venture_native_resume_frame, . - venture_native_resume_frame

// The halt signal handler's way out of venture_native_enter or
// venture_native_resume_frame before guest code ran (x19 = context, w0 =
// reason; host thread pointer, x18, stack and FPCR already restored by the
// handler). The context's guest state is untouched, so only the host side is
// restored, as venture_native_exit's tail does.
  .globl venture_native_leave_unentered
  .hidden venture_native_leave_unentered
  .type venture_native_leave_unentered, %function
  .p2align 4
venture_native_leave_unentered:
  str w0, [x19, #VENTURE_NCTX_EXIT_REASON]
  mov x1, x19
  add x2, x1, #VENTURE_NCTX_HOST_D8
  ldp d8, d9, [x2, #0]
  ldp d10, d11, [x2, #16]
  ldp d12, d13, [x2, #32]
  ldp d14, d15, [x2, #48]
  add x2, x1, #VENTURE_NCTX_HOST_X19
  ldp x19, x20, [x2, #0]
  ldp x21, x22, [x2, #16]
  ldp x23, x24, [x2, #32]
  ldp x25, x26, [x2, #48]
  ldp x27, x28, [x2, #64]
  ldp x29, x30, [x2, #80]
  ret
  .size venture_native_leave_unentered, . - venture_native_leave_unentered

  .section .note.GNU-stack, "", %progbits
