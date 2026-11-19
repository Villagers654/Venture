// Host entry routines of the native entry (cpu/native_entry.h,
// docs/design/native-headsets.md 4.1). One routine per call kind; a slot
// branches here with x16 = function id, x17 = scratch, x30 = the guest return
// address, sp = the guest stack, TPIDR_EL0 = the guest thread pointer.
//
// The host is the build's OS: __ANDROID__ selects the in-place routines
// (guest and host share the bionic thread), anything else the Linux routines
// (separate guest and host thread state, exits to run()).
#include "cpu/native_context_offsets.h"

#define SVC_HLE_CALL 0x5150
#define SVC_RETURN_TO_HOST 0x5151
#define SVC_HLE_CALL_FAST 0x5154
#define SVC_HLE_CALL_FAST_FP 0x5155
#define SVC_HLE_CALL_FAST_GUEST_FP 0x5156
#define SVC_ENTRY_HOOK 0x5157

  .text
  .p2align 4

.macro FUNCTION name
  .globl \name
  .hidden \name
  .type \name, %function
  .p2align 4
\name:
.endm

.macro END name
  .size \name, . - \name
.endm

// Stores the argument and caller-visible registers: x0..x16, x18, x29, x30
// and q0..q7 (base \c; uses x0 as scratch after x0 is stored). The x17 slot
// is left alone: x17 is the slot's scratch register.
.macro SAVE_CALL c
  stp x0, x1, [\c, #VENTURE_NCTX_X + 0]
  stp x2, x3, [\c, #VENTURE_NCTX_X + 16]
  stp x4, x5, [\c, #VENTURE_NCTX_X + 32]
  stp x6, x7, [\c, #VENTURE_NCTX_X + 48]
  stp x8, x9, [\c, #VENTURE_NCTX_X + 64]
  stp x10, x11, [\c, #VENTURE_NCTX_X + 80]
  stp x12, x13, [\c, #VENTURE_NCTX_X + 96]
  stp x14, x15, [\c, #VENTURE_NCTX_X + 112]
  str x16, [\c, #VENTURE_NCTX_X16]
  str x18, [\c, #VENTURE_NCTX_X18]
  str x19, [\c, #VENTURE_NCTX_X19]
  stp x29, x30, [\c, #VENTURE_NCTX_X29]
  str x30, [\c, #VENTURE_NCTX_PC]
  mrs x0, tpidr_el0
  str x0, [\c, #VENTURE_NCTX_TPIDR]
  mrs x0, nzcv
  str w0, [\c, #VENTURE_NCTX_NZCV]
  mrs x0, fpcr
  str w0, [\c, #VENTURE_NCTX_FPCR]
  mrs x0, fpsr
  str w0, [\c, #VENTURE_NCTX_FPSR]
  add x0, \c, #VENTURE_NCTX_Q
  stp q0, q1, [x0, #0]
  stp q2, q3, [x0, #32]
  stp q4, q5, [x0, #64]
  stp q6, q7, [x0, #96]
.endm

#if defined(__ANDROID__)
// ---------------------------------------------------------------------------
// Android: the context is a frame on the caller's stack; every kind runs its
// handler in place and returns to the caller. x19 holds the frame across the
// dispatcher call (saved in the frame first).

.macro IN_PLACE name, svc
FUNCTION \name
  .cfi_startproc
  sub sp, sp, #VENTURE_NCTX_SIZE
  .cfi_def_cfa_offset VENTURE_NCTX_SIZE
  SAVE_CALL sp
  .cfi_offset x19, VENTURE_NCTX_X19 - VENTURE_NCTX_SIZE
  .cfi_offset x29, VENTURE_NCTX_X29 - VENTURE_NCTX_SIZE
  .cfi_offset x30, VENTURE_NCTX_X29 + 8 - VENTURE_NCTX_SIZE
  mov x19, sp
  add x0, sp, #VENTURE_NCTX_SIZE
  str x0, [x19, #VENTURE_NCTX_SP]
  mov w0, #\svc
  str w0, [x19, #VENTURE_NCTX_KIND]
  str w0, [x19, #VENTURE_NCTX_SVC_IMMEDIATE]
  str wzr, [x19, #VENTURE_NCTX_EXIT_REASON]
  str wzr, [x19, #VENTURE_NCTX_HALT]
  mov w0, #VENTURE_NATIVE_FLAG_SHARED_TP
  str w0, [x19, #VENTURE_NCTX_FLAGS]
  str xzr, [x19, #VENTURE_NCTX_OWNER]
  mov x0, x19
  bl venture_native_dispatch
  // Results: x0, x1, q0, q1. The return address is the context's pc.
  add x0, x19, #VENTURE_NCTX_Q
  ldp q0, q1, [x0]
  ldp x0, x1, [x19, #VENTURE_NCTX_X]
  ldr x29, [x19, #VENTURE_NCTX_X29]
  ldr x30, [x19, #VENTURE_NCTX_PC]
  mov x17, x19
  ldr x19, [x17, #VENTURE_NCTX_X19]
  add sp, x17, #VENTURE_NCTX_SIZE
  .cfi_def_cfa_offset 0
  ret
  .cfi_endproc
END \name
.endm

IN_PLACE venture_native_hle_call, SVC_HLE_CALL
IN_PLACE venture_native_hle_call_fast, SVC_HLE_CALL_FAST
IN_PLACE venture_native_hle_call_fast_fp, SVC_HLE_CALL_FAST_FP
IN_PLACE venture_native_hle_call_fast_guest_fp, SVC_HLE_CALL_FAST_GUEST_FP
IN_PLACE venture_native_entry_hook, SVC_ENTRY_HOOK

// A host->guest call on Android is an ordinary native call that returns by
// itself; nothing ever returns through here.
FUNCTION venture_native_return_to_host
  brk #0x5151
END venture_native_return_to_host

#else
// ---------------------------------------------------------------------------
// Linux: the context is at [TPIDR_EL0 - 16]. No host TLS, libc call or
// allocation happens before TPIDR_EL0 and x18 are the host's.

// x17 := the thread's context.
.macro LOAD_CONTEXT
  mrs x17, tpidr_el0
  ldur x17, [x17, #-VENTURE_NATIVE_TP_CONTEXT_OFFSET]
.endm

// Marks a pc the engine's halt signal handler must recognise (a window in
// which a halt signal would otherwise be lost, see venture_native_resume_call).
.macro LABEL name
  .globl \name
  .hidden \name
\name:
.endm

// Context in x19: switch to the host thread pointer, x18, stack and FPCR.
.macro TO_HOST
  ldr x0, [x19, #VENTURE_NCTX_HOST_SP]
  mov sp, x0
  ldr w0, [x19, #VENTURE_NCTX_FLAGS]
  tbnz w0, #0, 1f
  ldr x0, [x19, #VENTURE_NCTX_HOST_TPIDR]
  msr tpidr_el0, x0
  ldr x18, [x19, #VENTURE_NCTX_HOST_X18]
1:
  // A guest-FP handler keeps the guest FPCR (rounding, flush to zero).
  ldr w0, [x19, #VENTURE_NCTX_KIND]
  mov w1, #SVC_HLE_CALL_FAST_GUEST_FP
  cmp w0, w1
  b.eq 2f
  ldr x0, [x19, #VENTURE_NCTX_HOST_FPCR]
  msr fpcr, x0
2:
.endm

// Saves the call (context in x17), marks the kind, moves the context to x19.
.macro ENTER svc
  LOAD_CONTEXT
  SAVE_CALL x17
  mov x19, x17
  mov x0, sp
  str x0, [x19, #VENTURE_NCTX_SP]
  mov w0, #\svc
  str w0, [x19, #VENTURE_NCTX_KIND]
  str w0, [x19, #VENTURE_NCTX_SVC_IMMEDIATE]
  TO_HOST
.endm

// RunLoop kinds: the thread loop runs the handler. Leave run() with Svc.
.macro RUN_LOOP name, svc
FUNCTION \name
  ENTER \svc
  mov w0, #VENTURE_NATIVE_EXIT_SVC
  b venture_native_exit
END \name
.endm

// InJit kinds: run the handler here; return to guest code when it completed
// and no halt is pending, else leave run().
.macro IN_JIT name, svc
FUNCTION \name
  ENTER \svc
  mov x0, x19
  bl venture_native_dispatch
  cbnz w0, venture_native_exit
  b venture_native_resume_call
END \name
.endm

RUN_LOOP venture_native_hle_call, SVC_HLE_CALL
IN_JIT venture_native_hle_call_fast, SVC_HLE_CALL_FAST
IN_JIT venture_native_hle_call_fast_fp, SVC_HLE_CALL_FAST_FP
IN_JIT venture_native_hle_call_fast_guest_fp, SVC_HLE_CALL_FAST_GUEST_FP
RUN_LOOP venture_native_entry_hook, SVC_ENTRY_HOOK
RUN_LOOP venture_native_return_to_host, SVC_RETURN_TO_HOST

// Back to the guest after a completed handler (context in x19) unless a halt
// is pending: guest FPCR, thread pointer, x18 and stack, then the results (x0,
// x1, q0, q1) and the callee-visible registers the routine used. A halt signal
// that lands after the check and before the final ret would find no guest pc
// and be lost, so the engine's handler treats this routine from its start to
// venture_native_resume_call_return as a halt exit through venture_native_exit
// (x19 is still the context, x20..x28 and q8..q31 the guest's), and the ret as
// guest code at x30. venture_native_enter_window..venture_native_enter_branch
// is the same window on the way in (context in x0, nothing of the guest's in
// registers yet), and venture_native_enter_branch is guest code at x17.
FUNCTION venture_native_resume_call
  ldr w0, [x19, #VENTURE_NCTX_HALT]
  cbz w0, 3f
  mov w0, #VENTURE_NATIVE_EXIT_HALT
  b venture_native_exit
3:
  ldr w0, [x19, #VENTURE_NCTX_FPCR]
  msr fpcr, x0
  // The guest's own flags; a host-FP handler's flags stay out. A guest-FP
  // handler raised guest flags directly in FPSR, which stays.
  ldr w0, [x19, #VENTURE_NCTX_KIND]
  mov w1, #SVC_HLE_CALL_FAST_GUEST_FP
  cmp w0, w1
  b.eq 2f
  ldr w0, [x19, #VENTURE_NCTX_FPSR]
  msr fpsr, x0
2:
  ldr w0, [x19, #VENTURE_NCTX_FLAGS]
  tbnz w0, #0, 1f
  ldr x0, [x19, #VENTURE_NCTX_TPIDR]
  msr tpidr_el0, x0
1:
  ldr x18, [x19, #VENTURE_NCTX_X18]
  ldr x0, [x19, #VENTURE_NCTX_SP]
  mov sp, x0
  add x0, x19, #VENTURE_NCTX_Q
  ldp q0, q1, [x0]
  ldp x0, x1, [x19, #VENTURE_NCTX_X]
  ldp x29, x30, [x19, #VENTURE_NCTX_X29]
  ldr x30, [x19, #VENTURE_NCTX_PC]
  ldr x19, [x19, #VENTURE_NCTX_X19]
LABEL venture_native_resume_call_return
  ret
END venture_native_resume_call

// Leaves venture_native_enter with the reason in w0 (context in x19, already
// on the host thread pointer and stack). Completes the full guest context:
// x20..x28 and q8..q31 still hold the guest's values (callee-saved by the
// handler, or untouched by a RunLoop kind).
FUNCTION venture_native_exit
  str w0, [x19, #VENTURE_NCTX_EXIT_REASON]
  stp x20, x21, [x19, #VENTURE_NCTX_X20 + 0]
  stp x22, x23, [x19, #VENTURE_NCTX_X20 + 16]
  stp x24, x25, [x19, #VENTURE_NCTX_X20 + 32]
  stp x26, x27, [x19, #VENTURE_NCTX_X20 + 48]
  str x28, [x19, #VENTURE_NCTX_X20 + 64]
  add x1, x19, #VENTURE_NCTX_Q8
  stp q8, q9, [x1, #0]
  stp q10, q11, [x1, #32]
  stp q12, q13, [x1, #64]
  stp q14, q15, [x1, #96]
  stp q16, q17, [x1, #128]
  stp q18, q19, [x1, #160]
  stp q20, q21, [x1, #192]
  stp q22, q23, [x1, #224]
  stp q24, q25, [x1, #256]
  stp q26, q27, [x1, #288]
  stp q28, q29, [x1, #320]
  stp q30, q31, [x1, #352]
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
END venture_native_exit

// uint32_t venture_native_enter(NativeContext* context): records the host
// side, then runs guest code from context->pc with the full guest context.
// Returns the exit reason when guest code leaves through a routine above.
  .globl venture_native_enter
  .type venture_native_enter, %function
  .p2align 4
venture_native_enter:
  add x1, x0, #VENTURE_NCTX_HOST_X19
  stp x19, x20, [x1, #0]
  stp x21, x22, [x1, #16]
  stp x23, x24, [x1, #32]
  stp x25, x26, [x1, #48]
  stp x27, x28, [x1, #64]
  stp x29, x30, [x1, #80]
  add x1, x0, #VENTURE_NCTX_HOST_D8
  stp d8, d9, [x1, #0]
  stp d10, d11, [x1, #16]
  stp d12, d13, [x1, #32]
  stp d14, d15, [x1, #48]
  mov x1, sp
  str x1, [x0, #VENTURE_NCTX_HOST_SP]
  mrs x1, tpidr_el0
  str x1, [x0, #VENTURE_NCTX_HOST_TPIDR]
  str x18, [x0, #VENTURE_NCTX_HOST_X18]
  mrs x1, fpcr
  str x1, [x0, #VENTURE_NCTX_HOST_FPCR]
  str wzr, [x0, #VENTURE_NCTX_EXIT_REASON]
LABEL venture_native_enter_window
  ldr w1, [x0, #VENTURE_NCTX_HALT]
  cbz w1, 1f
  mov w1, #VENTURE_NATIVE_EXIT_HALT
  str w1, [x0, #VENTURE_NCTX_EXIT_REASON]
  mov w0, w1
  ret
1:
  ldr w1, [x0, #VENTURE_NCTX_FPCR]
  msr fpcr, x1
  ldr w1, [x0, #VENTURE_NCTX_FPSR]
  msr fpsr, x1
  ldr w1, [x0, #VENTURE_NCTX_NZCV]
  msr nzcv, x1
  ldr w1, [x0, #VENTURE_NCTX_FLAGS]
  tbnz w1, #0, 2f
  ldr x1, [x0, #VENTURE_NCTX_TPIDR]
  msr tpidr_el0, x1
2:
  ldr x1, [x0, #VENTURE_NCTX_SP]
  mov sp, x1
  add x1, x0, #VENTURE_NCTX_Q
  ldp q0, q1, [x1, #0]
  ldp q2, q3, [x1, #32]
  ldp q4, q5, [x1, #64]
  ldp q6, q7, [x1, #96]
  ldp q8, q9, [x1, #128]
  ldp q10, q11, [x1, #160]
  ldp q12, q13, [x1, #192]
  ldp q14, q15, [x1, #224]
  ldp q16, q17, [x1, #256]
  ldp q18, q19, [x1, #288]
  ldp q20, q21, [x1, #320]
  ldp q22, q23, [x1, #352]
  ldp q24, q25, [x1, #384]
  ldp q26, q27, [x1, #416]
  ldp q28, q29, [x1, #448]
  ldp q30, q31, [x1, #480]
  ldr x17, [x0, #VENTURE_NCTX_PC]
  ldp x2, x3, [x0, #VENTURE_NCTX_X + 16]
  ldp x4, x5, [x0, #VENTURE_NCTX_X + 32]
  ldp x6, x7, [x0, #VENTURE_NCTX_X + 48]
  ldp x8, x9, [x0, #VENTURE_NCTX_X + 64]
  ldp x10, x11, [x0, #VENTURE_NCTX_X + 80]
  ldp x12, x13, [x0, #VENTURE_NCTX_X + 96]
  ldp x14, x15, [x0, #VENTURE_NCTX_X + 112]
  ldr x16, [x0, #VENTURE_NCTX_X16]
  ldr x18, [x0, #VENTURE_NCTX_X18]
  ldp x19, x20, [x0, #VENTURE_NCTX_X19]
  ldp x21, x22, [x0, #VENTURE_NCTX_X19 + 16]
  ldp x23, x24, [x0, #VENTURE_NCTX_X19 + 32]
  ldp x25, x26, [x0, #VENTURE_NCTX_X19 + 48]
  ldp x27, x28, [x0, #VENTURE_NCTX_X19 + 64]
  ldp x29, x30, [x0, #VENTURE_NCTX_X29]
  ldp x0, x1, [x0, #VENTURE_NCTX_X]
LABEL venture_native_enter_branch
  br x17
  .size venture_native_enter, . - venture_native_enter

#endif

// A repeated HLE call (Cpu::rewind_hle_call), run as guest code with x17 = the
// call's stub. A signal handler that steps over the faulting call (pc + 4)
// reaches the ret, so the call returns to its caller as from the trap stub.
FUNCTION venture_native_repeat_call
  br x17
  ret
END venture_native_repeat_call

#if defined(__linux__)
  .section .note.GNU-stack, "", %progbits
#endif
