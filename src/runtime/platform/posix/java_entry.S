// The host JVM's native call into venture_java_native_dispatch(record, HostArguments*).
// The per-method stub (java/native_bridge.cpp make_stub) leaves the record in a
// scratch register and tail-jumps here with the JVM's arguments untouched.
// HostArguments (java/abi.h): gp[8] at 0, fp[8] at 64, stack pointer at 128.
#if defined(__x86_64__)
// System V AMD64 ABI: independent GP/SSE allocation; overflow in source order.
// The record is in r10.
.text
.globl venture_java_native_entry
.type venture_java_native_entry,@function
venture_java_native_entry:
  .cfi_startproc
  subq $152, %rsp
  .cfi_def_cfa_offset 160
  movq %rdi, 0(%rsp)
  movq %rsi, 8(%rsp)
  movq %rdx, 16(%rsp)
  movq %rcx, 24(%rsp)
  movq %r8, 32(%rsp)
  movq %r9, 40(%rsp)
  movq %xmm0, 64(%rsp)
  movq %xmm1, 72(%rsp)
  movq %xmm2, 80(%rsp)
  movq %xmm3, 88(%rsp)
  movq %xmm4, 96(%rsp)
  movq %xmm5, 104(%rsp)
  movq %xmm6, 112(%rsp)
  movq %xmm7, 120(%rsp)
  leaq 160(%rsp), %rax
  movq %rax, 128(%rsp)
  movq %r10, %rdi
  movq %rsp, %rsi
  call venture_java_native_dispatch
  movq %rax, %xmm0
  addq $152, %rsp
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc
.size venture_java_native_entry, .-venture_java_native_entry
#elif defined(__aarch64__)
// AAPCS64: x0-x7 and v0-v7 allocated independently; each stack argument in an
// 8-byte slot from the caller's sp. The record is in x16. The result goes back
// in both x0 and d0, as the x64 entries return it in rax and xmm0.
.text
.globl venture_java_native_entry
.type venture_java_native_entry,%function
.p2align 2
venture_java_native_entry:
  .cfi_startproc
  hint #34  // BTI c
  stp x29, x30, [sp, #-160]!
  .cfi_def_cfa_offset 160
  .cfi_offset x29, -160
  .cfi_offset x30, -152
  mov x29, sp
  stp x0, x1, [sp, #16]
  stp x2, x3, [sp, #32]
  stp x4, x5, [sp, #48]
  stp x6, x7, [sp, #64]
  stp d0, d1, [sp, #80]
  stp d2, d3, [sp, #96]
  stp d4, d5, [sp, #112]
  stp d6, d7, [sp, #128]
  add x9, sp, #160
  str x9, [sp, #144]
  mov x0, x16
  add x1, sp, #16
  bl venture_java_native_dispatch
  fmov d0, x0
  ldp x29, x30, [sp], #160
  .cfi_def_cfa_offset 0
  .cfi_restore x29
  .cfi_restore x30
  ret
  .cfi_endproc
.size venture_java_native_entry, .-venture_java_native_entry
#else
#error "no JNI entry for this host architecture"
#endif
.section .note.GNU-stack,"",%progbits
