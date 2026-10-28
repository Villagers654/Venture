// Windows x64 ABI: positional GP/FP registers, 32-byte caller home area.
// https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention
.text
.globl venture_java_native_entry
.def venture_java_native_entry; .scl 2; .type 32; .endef
.seh_proc venture_java_native_entry
venture_java_native_entry:
  subq $168, %rsp
  .seh_stackalloc 168
  .seh_endprologue
  movq %rcx, 32(%rsp)
  movq %rdx, 40(%rsp)
  movq %r8, 48(%rsp)
  movq %r9, 56(%rsp)
  movq %xmm0, 80(%rsp)
  movq %xmm1, 88(%rsp)
  movq %xmm2, 96(%rsp)
  movq %xmm3, 104(%rsp)
  leaq 208(%rsp), %rax
  movq %rax, 144(%rsp)
  movq %r10, %rcx
  leaq 32(%rsp), %rdx
  call venture_java_native_dispatch
  movq %rax, %xmm0
  addq $168, %rsp
  ret
.seh_endproc
