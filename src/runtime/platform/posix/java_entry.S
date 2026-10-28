// System V AMD64 ABI: independent GP/SSE allocation; overflow in source order.
.text
.globl venture_java_native_entry
.type venture_java_native_entry,@function
venture_java_native_entry:
  .cfi_startproc
  subq $136, %rsp
  .cfi_def_cfa_offset 144
  movq %rdi, 0(%rsp)
  movq %rsi, 8(%rsp)
  movq %rdx, 16(%rsp)
  movq %rcx, 24(%rsp)
  movq %r8, 32(%rsp)
  movq %r9, 40(%rsp)
  movq %xmm0, 48(%rsp)
  movq %xmm1, 56(%rsp)
  movq %xmm2, 64(%rsp)
  movq %xmm3, 72(%rsp)
  movq %xmm4, 80(%rsp)
  movq %xmm5, 88(%rsp)
  movq %xmm6, 96(%rsp)
  movq %xmm7, 104(%rsp)
  leaq 144(%rsp), %rax
  movq %rax, 112(%rsp)
  movq %r10, %rdi
  movq %rsp, %rsi
  call venture_java_native_dispatch
  movq %rax, %xmm0
  addq $136, %rsp
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc
.size venture_java_native_entry, .-venture_java_native_entry
.section .note.GNU-stack,"",@progbits
