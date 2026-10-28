// NDK clang --target=armv7a-linux-androideabi23 -c a32_unwind_fixture.S
// readelf --unwind yields compact word 0x8002abb0 (vsp += 12; pop r4-r7,lr).
.syntax unified
.arm
.text
.global venture_unwind_fixture
.type venture_unwind_fixture,%function
venture_unwind_fixture:
  .fnstart
  push {r4-r7,lr}
  .save {r4-r7,lr}
  sub sp,sp,#12
  .pad #12
  bl venture_unwind_target
  add sp,sp,#12
  pop {r4-r7,pc}
  .fnend
.size venture_unwind_fixture,.-venture_unwind_fixture
