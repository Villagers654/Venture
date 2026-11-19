// The image form of a native entry slot (cpu/native_entry.h), for shared
// libraries built ahead of time (the Android Meta replacement libraries):
// the slot's first four words, then a PC-relative branch to the routine in the
// same image, so the library needs no text relocation. 32 bytes, x16 = id,
// x17 = scratch, as in the slot NativeEntry writes.
//
//   VENTURE_IMAGE_SLOT ovr_PopMessage, 42, venture_native_hle_call
.macro VENTURE_IMAGE_SLOT name, id, routine
  .globl \name
  .type \name, %function
  .p2align 4
\name:
  movz x16, #((\id) & 0xffff)
  movk x16, #(((\id) >> 16) & 0xffff), lsl #16
  nop
  nop
  adrp x17, \routine
  add x17, x17, :lo12:\routine
  br x17
  nop
  .size \name, 32
.endm
