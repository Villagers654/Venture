#include "core/hle_runtime.h"
#include "core/linker.h"
#include "core/process.h"
#include "cpu/stubs.h"
#include "platform/low_memory.h"
#include <array>
#include <cstring>
#include <span>
#include <vector>

namespace venture::hle::libc {
namespace {
/* Copyright (c) 2012-2013, Linaro Limited
   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:
       * Redistributions of source code must retain the above copyright
         notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above copyright
         notice, this list of conditions and the following disclaimer in the
         documentation and/or other materials provided with the distribution.
       * Neither the name of the Linaro nor the
         names of its contributors may be used to endorse or promote products
         derived from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

/*
 * Copyright (c) 2015 ARM Ltd
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the company may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY ARM LTD ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL ARM LTD BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Assumptions:
 *
 * ARMv8-a, AArch64, unaligned accesses.
 *
 */

// Bionic 050b1b17b292418ca009ff0e07f25d1c036db246/libc/arch-arm64/generic/bionic/memcpy_base.S
constexpr std::array<std::uint32_t, 91> kMemcpy64{
    0xf9800020, // prfm pldl1keep, [x1]
    0x8b020024, // add x4, x1, x2
    0x8b020005, // add x5, x0, x2
    0xf100405f, // cmp x2, #0x10
    0x54000209, // b.ls 0x50 <__memcpy+0x50>
    0xf101805f, // cmp x2, #0x60
    0x54000648, // b.hi 0xe0 <__memcpy+0xe0>
    0xd1000449, // sub x9, x2, #0x1
    0xa9401c26, // ldp x6, x7, [x1]
    0x37300469, // tbnz w9, #0x6, 0xb0 <__memcpy+0xb0>
    0xa97f348c, // ldp x12, x13, [x4, #-0x10]
    0x362800a9, // tbz w9, #0x5, 0x40 <__memcpy+0x40>
    0xa9412428, // ldp x8, x9, [x1, #0x10]
    0xa97e2c8a, // ldp x10, x11, [x4, #-0x20]
    0xa9012408, // stp x8, x9, [x0, #0x10]
    0xa93e2caa, // stp x10, x11, [x5, #-0x20]
    0xa9001c06, // stp x6, x7, [x0]
    0xa93f34ac, // stp x12, x13, [x5, #-0x10]
    0xd65f03c0, // ret
    0xd503201f, // nop
    0xf100205f, // cmp x2, #0x8
    0x540000e3, // b.lo 0x70 <__memcpy+0x70>
    0xf9400026, // ldr x6, [x1]
    0xf85f8087, // ldur x7, [x4, #-0x8]
    0xf9000006, // str x6, [x0]
    0xf81f80a7, // stur x7, [x5, #-0x8]
    0xd65f03c0, // ret
    0xd503201f, // nop
    0x361000c2, // tbz w2, #0x2, 0x88 <__memcpy+0x88>
    0xb9400026, // ldr w6, [x1]
    0xb85fc087, // ldur w7, [x4, #-0x4]
    0xb9000006, // str w6, [x0]
    0xb81fc0a7, // stur w7, [x5, #-0x4]
    0xd65f03c0, // ret
    0xb4000102, // cbz x2, 0xa8 <__memcpy+0xa8>
    0xd341fc49, // lsr x9, x2, #1
    0x39400026, // ldrb w6, [x1]
    0x385ff087, // ldurb w7, [x4, #-0x1]
    0x38696828, // ldrb w8, [x1, x9]
    0x39000006, // strb w6, [x0]
    0x38296808, // strb w8, [x0, x9]
    0x381ff0a7, // sturb w7, [x5, #-0x1]
    0xd65f03c0, // ret
    0xd503201f, // nop
    0xa9412428, // ldp x8, x9, [x1, #0x10]
    0xa9422c2a, // ldp x10, x11, [x1, #0x20]
    0xa943342c, // ldp x12, x13, [x1, #0x30]
    0xa97e0881, // ldp x1, x2, [x4, #-0x20]
    0xa97f0c84, // ldp x4, x3, [x4, #-0x10]
    0xa9001c06, // stp x6, x7, [x0]
    0xa9012408, // stp x8, x9, [x0, #0x10]
    0xa9022c0a, // stp x10, x11, [x0, #0x20]
    0xa903340c, // stp x12, x13, [x0, #0x30]
    0xa93e08a1, // stp x1, x2, [x5, #-0x20]
    0xa93f0ca4, // stp x4, x3, [x5, #-0x10]
    0xd65f03c0, // ret
    0x92400c09, // and x9, x0, #0xf
    0x927cec03, // and x3, x0, #0xfffffffffffffff0
    0xa940342c, // ldp x12, x13, [x1]
    0xcb090021, // sub x1, x1, x9
    0x8b090042, // add x2, x2, x9
    0xa9411c26, // ldp x6, x7, [x1, #0x10]
    0xa900340c, // stp x12, x13, [x0]
    0xa9422428, // ldp x8, x9, [x1, #0x20]
    0xa9432c2a, // ldp x10, x11, [x1, #0x30]
    0xa9c4342c, // ldp x12, x13, [x1, #0x40]!
    0xf1024042, // subs x2, x2, #0x90
    0x54000169, // b.ls 0x138 <__memcpy+0x138>
    0xa9011c66, // stp x6, x7, [x3, #0x10]
    0xa9411c26, // ldp x6, x7, [x1, #0x10]
    0xa9022468, // stp x8, x9, [x3, #0x20]
    0xa9422428, // ldp x8, x9, [x1, #0x20]
    0xa9032c6a, // stp x10, x11, [x3, #0x30]
    0xa9432c2a, // ldp x10, x11, [x1, #0x30]
    0xa984346c, // stp x12, x13, [x3, #0x40]!
    0xa9c4342c, // ldp x12, x13, [x1, #0x40]!
    0xf1010042, // subs x2, x2, #0x40
    0x54fffee8, // b.hi 0x110 <__memcpy+0x110>
    0xa97c0881, // ldp x1, x2, [x4, #-0x40]
    0xa9011c66, // stp x6, x7, [x3, #0x10]
    0xa97d1c86, // ldp x6, x7, [x4, #-0x30]
    0xa9022468, // stp x8, x9, [x3, #0x20]
    0xa97e2488, // ldp x8, x9, [x4, #-0x20]
    0xa9032c6a, // stp x10, x11, [x3, #0x30]
    0xa97f2c8a, // ldp x10, x11, [x4, #-0x10]
    0xa904346c, // stp x12, x13, [x3, #0x40]
    0xa93c08a1, // stp x1, x2, [x5, #-0x40]
    0xa93d1ca6, // stp x6, x7, [x5, #-0x30]
    0xa93e24a8, // stp x8, x9, [x5, #-0x20]
    0xa93f2caa, // stp x10, x11, [x5, #-0x10]
    0xd65f03c0, // ret
};

/* Copyright (c) 2013-2015, Linaro Limited
   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:
       * Redistributions of source code must retain the above copyright
	 notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above copyright
	 notice, this list of conditions and the following disclaimer in the
	 documentation and/or other materials provided with the distribution.
       * Neither the name of the Linaro nor the
	 names of its contributors may be used to endorse or promote products
	 derived from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

/* Assumptions:
 *
 * ARMv8-a, AArch64, unaligned accesses, min page size 4k.
 */

// Bionic 050b1b17b292418ca009ff0e07f25d1c036db246/libc/arch-arm64/default/bionic/strlen.S
constexpr std::array<std::uint32_t, 80> kStrlen64{
    0x92402c04, // and x4, x0, #0xfff
    0xb200c3e8, // mov x8, #0x101010101010101  // =72340172838076673
    0xf13fc09f, // cmp x4, #0xff0
    0x5400082c, // b.gt 0x110 <strlen_default+0x110>
    0xa9400c02, // ldp x2, x3, [x0]
    0xcb080044, // sub x4, x2, x8
    0xb200d845, // orr x5, x2, #0x7f7f7f7f7f7f7f7f
    0xcb080066, // sub x6, x3, x8
    0xb200d867, // orr x7, x3, #0x7f7f7f7f7f7f7f7f
    0xea250084, // bics x4, x4, x5
    0x8a2700c5, // bic x5, x6, x7
    0xfa4008a0, // ccmp x5, #0x0, #0x0, eq
    0x54000100, // b.eq 0x50 <strlen_default+0x50>
    0x9a853084, // csel x4, x4, x5, lo
    0xd2800100, // mov x0, #0x8                // =8
    0xdac00c84, // rev x4, x4
    0xdac01084, // clz x4, x4
    0x9a8033e0, // csel x0, xzr, x0, lo
    0x8b440c00, // add x0, x0, x4, lsr #3
    0xd65f03c0, // ret
    0x927cec01, // and x1, x0, #0xfffffffffffffff0
    0xd1004021, // sub x1, x1, #0x10
    0xa9c20c22, // ldp x2, x3, [x1, #0x20]!
    0xcb080044, // sub x4, x2, x8
    0xcb080066, // sub x6, x3, x8
    0xaa060085, // orr x5, x4, x6
    0xea081cbf, // tst x5, x8, lsl #7
    0x54000101, // b.ne 0x8c <strlen_default+0x8c>
    0xa9410c22, // ldp x2, x3, [x1, #0x10]
    0xcb080044, // sub x4, x2, x8
    0xcb080066, // sub x6, x3, x8
    0xaa060085, // orr x5, x4, x6
    0xea081cbf, // tst x5, x8, lsl #7
    0x54fffea0, // b.eq 0x58 <strlen_default+0x58>
    0x91004021, // add x1, x1, #0x10
    0xb200d845, // orr x5, x2, #0x7f7f7f7f7f7f7f7f
    0xb200d867, // orr x7, x3, #0x7f7f7f7f7f7f7f7f
    0xea250084, // bics x4, x4, x5
    0x8a2700c5, // bic x5, x6, x7
    0xfa4008a0, // ccmp x5, #0x0, #0x0, eq
    0x54000120, // b.eq 0xc4 <strlen_default+0xc4>
    0x9a853084, // csel x4, x4, x5, lo
    0xcb000020, // sub x0, x1, x0
    0xdac00c84, // rev x4, x4
    0x91002005, // add x5, x0, #0x8
    0xdac01084, // clz x4, x4
    0x9a853000, // csel x0, x0, x5, lo
    0x8b440c00, // add x0, x0, x4, lsr #3
    0xd65f03c0, // ret
    0xa9c10c22, // ldp x2, x3, [x1, #0x10]!
    0xcb080044, // sub x4, x2, x8
    0xb200d845, // orr x5, x2, #0x7f7f7f7f7f7f7f7f
    0xcb080066, // sub x6, x3, x8
    0xb200d867, // orr x7, x3, #0x7f7f7f7f7f7f7f7f
    0xea250084, // bics x4, x4, x5
    0x8a2700c5, // bic x5, x6, x7
    0xfa4008a0, // ccmp x5, #0x0, #0x0, eq
    0x54fffe01, // b.ne 0xa4 <strlen_default+0xa4>
    0xa9c10c22, // ldp x2, x3, [x1, #0x10]!
    0xcb080044, // sub x4, x2, x8
    0xb200d845, // orr x5, x2, #0x7f7f7f7f7f7f7f7f
    0xcb080066, // sub x6, x3, x8
    0xb200d867, // orr x7, x3, #0x7f7f7f7f7f7f7f7f
    0xea250084, // bics x4, x4, x5
    0x8a2700c5, // bic x5, x6, x7
    0xfa4008a0, // ccmp x5, #0x0, #0x0, eq
    0x54fffde0, // b.eq 0xc4 <strlen_default+0xc4>
    0x17ffffe6, // b 0xa4 <strlen_default+0xa4>
    0x927cec01, // and x1, x0, #0xfffffffffffffff0
    0xa9400c22, // ldp x2, x3, [x1]
    0xd37df004, // lsl x4, x0, #3
    0x92800007, // mov x7, #-0x1               // =-1
    0x9ac420e4, // lsl x4, x7, x4
    0xb201c084, // orr x4, x4, #0x8080808080808080
    0xaa240042, // orn x2, x2, x4
    0xaa240065, // orn x5, x3, x4
    0xf27d001f, // tst x0, #0x8
    0x9a870042, // csel x2, x2, x7, eq
    0x9a850063, // csel x3, x3, x5, eq
    0x17ffffc8, // b 0x5c <strlen_default+0x5c>
};

/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/*
 * Copyright (c) 2013 ARM Ltd
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the company may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY ARM LTD ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL ARM LTD BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// Bionic 050b1b17b292418ca009ff0e07f25d1c036db246/libc/arch-arm/cortex-a15/bionic/memcpy.S
constexpr std::array<std::uint32_t, 56> kMemcpy32{
    0xf5d1f040, // pld [r1, #0x40]
    0xe92d4001, // push {r0, lr}
    0xe3520010, // cmp r2, #16
    0x3a000024, // blo 0xa4 <__memcpy_a15+0xa4> @ imm = #0x90
    0xe2603000, // rsb r3, r0, #0
    0xe213300f, // ands r3, r3, #15
    0x0a00000e, // beq 0x58 <__memcpy_a15+0x58> @ imm = #0x38
    0xe0422003, // sub r2, r2, r3
    0xe1b0cf83, // lsls r12, r3, #31
    0x44d1e001, // ldrbmi lr, [r1], #1
    0x44c0e001, // strbmi lr, [r0], #1
    0x24d1c001, // ldrbhs r12, [r1], #1
    0x24d1e001, // ldrbhs lr, [r1], #1
    0x24c0c001, // strbhs r12, [r0], #1
    0x24c0e001, // strbhs lr, [r0], #1
    0xe1b0ce83, // lsls r12, r3, #29
    0xaa000001, // bge 0x4c <__memcpy_a15+0x4c> @ imm = #0x4
    0xf4a1030d, // vld4.8 {d0[0], d1[0], d2[0], d3[0]}, [r1]!
    0xf480031d, // vst4.8 {d0[0], d1[0], d2[0], d3[0]}, [r0:32]!
    0x3a000001, // blo 0x58 <__memcpy_a15+0x58> @ imm = #0x4
    0xf421070d, // vld1.8 {d0}, [r1]!
    0xf400071d, // vst1.8 {d0}, [r0:64]!
    0xe2522040, // subs r2, r2, #64
    0x3a000006, // blo 0x7c <__memcpy_a15+0x7c> @ imm = #0x18
    0xf421020d, // vld1.8 {d0, d1, d2, d3}, [r1]!
    0xf421420d, // vld1.8 {d4, d5, d6, d7}, [r1]!
    0xf5d1f100, // pld [r1, #0x100]
    0xe2522040, // subs r2, r2, #64
    0xf400022d, // vst1.8 {d0, d1, d2, d3}, [r0:128]!
    0xf400422d, // vst1.8 {d4, d5, d6, d7}, [r0:128]!
    0x2afffff8, // bhs 0x60 <__memcpy_a15+0x60> @ imm = #-0x20
    0xe2922020, // adds r2, r2, #32
    0x3a000002, // blo 0x90 <__memcpy_a15+0x90> @ imm = #0x8
    0xf421020d, // vld1.8 {d0, d1, d2, d3}, [r1]!
    0xe2422020, // sub r2, r2, #32
    0xf400022d, // vst1.8 {d0, d1, d2, d3}, [r0:128]!
    0xe2822020, // add r2, r2, #32
    0xe3120010, // tst r2, #16
    0x0a000001, // beq 0xa4 <__memcpy_a15+0xa4> @ imm = #0x4
    0xf4210a0d, // vld1.8 {d0, d1}, [r1]!
    0xf4000a2d, // vst1.8 {d0, d1}, [r0:128]!
    0xe1b0ce82, // lsls r12, r2, #29
    0x3a000001, // blo 0xb4 <__memcpy_a15+0xb4> @ imm = #0x4
    0xf421070d, // vld1.8 {d0}, [r1]!
    0xf400070d, // vst1.8 {d0}, [r0]!
    0xaa000001, // bge 0xc0 <__memcpy_a15+0xc0> @ imm = #0x4
    0xf4a1030d, // vld4.8 {d0[0], d1[0], d2[0], d3[0]}, [r1]!
    0xf480030d, // vst4.8 {d0[0], d1[0], d2[0], d3[0]}, [r0]!
    0xe1b02f82, // lsls r2, r2, #31
    0x14d1e001, // ldrbne lr, [r1], #1
    0x14c0e001, // strbne lr, [r0], #1
    0x24d1c001, // ldrbhs r12, [r1], #1
    0x25d1e000, // ldrbhs lr, [r1]
    0x24c0c001, // strbhs r12, [r0], #1
    0x25c0e000, // strbhs lr, [r0]
    0xe8bd8001, // pop {r0, pc}
};

/*
 * Copyright (C) 2013 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/*
 * Copyright (c) 2013 ARM Ltd
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the company may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY ARM LTD ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL ARM LTD BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// Bionic 050b1b17b292418ca009ff0e07f25d1c036db246/libc/arch-arm/cortex-a15/bionic/strlen.S
// Added a 16-byte prefix limit; final word is the host fallback address.
constexpr std::array<std::uint32_t, 56> kStrlen32{
    0xf000f890, // pld [r0]
    0xf0104601, // mov r1, r0
    0xd01d0307, // packed Thumb instructions/data
    0x0308f1c3, // rsb.w r3, r3, #0x8
    0x7cc3ea5f, // lsls.w r12, r3, #0x1f
    0xf811d002, // beq 0x1c <strlen_a15+0x1c>  @ imm = #0x4
    0xb36a2b01, // packed Thumb instructions/data
    0xf013d308, // blo 0x30 <strlen_a15+0x30>  @ imm = #0x10
    0xd0050c02, // packed Thumb instructions/data
    0x2b01f811, // ldrb r2, [r1], #1
    0xf811b332, // cbz r2, 0x78 <strlen_a15+0x78> @ imm = #0x4c
    0xb31a2b01, // packed Thumb instructions/data
    0x0f04f013, // tst.w r3, #0x4
    0xf851d008, // beq 0x48 <strlen_a15+0x48>  @ imm = #0x10
    0xf1a33b04, // packed Thumb instructions/data
    0xea2c3c01, // packed Thumb instructions/data
    0xf01c0c03, // packed Thumb instructions/data
    0xd1313c80, // packed Thumb instructions/data
    0x0c00eba1, // sub.w r12, r1, r0
    0x0f10f1bc, // cmp.w r12, #0x10
    0xe8f1d241, // bhs 0xd6 <strlen_a15+0xd6>  @ imm = #0x82
    0xf8912302, // packed Thumb instructions/data
    0xf1a2f040, // packed Thumb instructions/data
    0xea2c3c01, // packed Thumb instructions/data
    0xf01c0c02, // packed Thumb instructions/data
    0xd10c3c80, // packed Thumb instructions/data
    0x3c01f1a3, // sub.w r12, r3, #0x1010101
    0x0c03ea2c, // bic.w r12, r12, r3
    0x3c80f01c, // ands r12, r12, #0x80808080
    0xe7e7d11a, // bne 0xac <strlen_a15+0xac>  @ imm = #0x34
    0x0000eba1, // sub.w r0, r1, r0
    0x0001f1a0, // sub.w r0, r0, #0x1
    0xeba14770, // bx lr
    0xea5f0000, // packed Thumb instructions/data
    0xd106434c, // packed Thumb instructions/data
    0xea5fd208, // bhs 0xa0 <strlen_a15+0xa0>  @ imm = #0x10
    0xd1080c4c, // packed Thumb instructions/data
    0x0005f1a0, // sub.w r0, r0, #0x5
    0xf1a04770, // bx lr
    0x47700008, // packed Thumb instructions/data
    0x0007f1a0, // sub.w r0, r0, #0x7
    0xf1a04770, // bx lr
    0x47700006, // packed Thumb instructions/data
    0x0000eba1, // sub.w r0, r1, r0
    0x434cea5f, // lsls.w r3, r12, #0x11
    0xd208d106, // bne 0xc4 <strlen_a15+0xc4>  @ imm = #0xc
    0x0c4cea5f, // lsls.w r12, r12, #0x1
    0xf1a0d108, // bne 0xd0 <strlen_a15+0xd0>  @ imm = #0x10
    0x47700001, // packed Thumb instructions/data
    0x0004f1a0, // sub.w r0, r0, #0x4
    0xf1a04770, // bx lr
    0x47700003, // packed Thumb instructions/data
    0x0002f1a0, // sub.w r0, r0, #0x2
    0xf8df4770, // bx lr
    0x4760c004, // packed Thumb instructions/data
    0x00000000, // .word 0x00000000
};

/* Copyright (c) 2012-2013, Linaro Limited
   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:
       * Redistributions of source code must retain the above copyright
         notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above copyright
         notice, this list of conditions and the following disclaimer in the
         documentation and/or other materials provided with the distribution.
       * Neither the name of the Linaro nor the
         names of its contributors may be used to endorse or promote products
         derived from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

/*
 * Copyright (c) 2015 ARM Ltd
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the company may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY ARM LTD ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL ARM LTD BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Assumptions:
 *
 * ARMv8-a, AArch64, unaligned accesses
 *
 */

// Bionic 050b1b17b292418ca009ff0e07f25d1c036db246/libc/arch-arm64/generic/bionic/memset.S
constexpr std::array<std::uint32_t, 118> kMemset64{
    0x4e010c20, // dup v0.16b, w1
    0x8b020004, // add x4, x0, x2
    0xf101805f, // cmp x2, #0x60
    0x540003c8, // b.hi 0x84 <memset+0x84>
    0xf100405f, // cmp x2, #0x10
    0x54000202, // b.hs 0x54 <memset+0x54>
    0x4e083c01, // mov x1, v0.d[0]
    0x361800a2, // tbz w2, #0x3, 0x30 <memset+0x30>
    0xf9000001, // str x1, [x0]
    0xf81f8081, // stur x1, [x4, #-0x8]
    0xd65f03c0, // ret
    0xd503201f, // nop
    0x36100082, // tbz w2, #0x2, 0x40 <memset+0x40>
    0xb9000001, // str w1, [x0]
    0xb81fc081, // stur w1, [x4, #-0x4]
    0xd65f03c0, // ret
    0xb4000082, // cbz x2, 0x50 <memset+0x50>
    0x39000001, // strb w1, [x0]
    0x36080042, // tbz w2, #0x1, 0x50 <memset+0x50>
    0x781fe081, // sturh w1, [x4, #-0x2]
    0xd65f03c0, // ret
    0x3d800000, // str q0, [x0]
    0x373000c2, // tbnz w2, #0x6, 0x70 <memset+0x70>
    0x3c9f0080, // stur q0, [x4, #-0x10]
    0x36280062, // tbz w2, #0x5, 0x6c <memset+0x6c>
    0x3d800400, // str q0, [x0, #0x10]
    0x3c9e0080, // stur q0, [x4, #-0x20]
    0xd65f03c0, // ret
    0x3d800400, // str q0, [x0, #0x10]
    0xad010000, // stp q0, q0, [x0, #0x20]
    0xad3f0080, // stp q0, q0, [x4, #-0x20]
    0xd65f03c0, // ret
    0xd503201f, // nop
    0x12001c21, // and w1, w1, #0xff
    0x927cec03, // and x3, x0, #0xfffffffffffffff0
    0x3d800000, // str q0, [x0]
    0xf104005f, // cmp x2, #0x100
    0x7a402820, // ccmp w1, #0x0, #0x0, hs
    0x54000180, // b.eq 0xc8 <memset+0xc8>
    0xcb030082, // sub x2, x4, x3
    0x91004063, // add x3, x3, #0x10
    0xd1014042, // sub x2, x2, #0x50
    0xac820060, // stp q0, q0, [x3], #0x40
    0xad3f0060, // stp q0, q0, [x3, #-0x20]
    0xf1010042, // subs x2, x2, #0x40
    0x54ffffa8, // b.hi 0xa8 <memset+0xa8>
    0xad3e0080, // stp q0, q0, [x4, #-0x40]
    0xad3f0080, // stp q0, q0, [x4, #-0x20]
    0xd65f03c0, // ret
    0xd503201f, // nop
    0xd53b00e5, // mrs x5, DCZID_EL0
    0x3727fe85, // tbnz w5, #0x4, 0x9c <memset+0x9c>
    0x12000ca5, // and w5, w5, #0xf
    0x710010bf, // cmp w5, #0x4
    0x54000281, // b.ne 0x128 <memset+0x128>
    0x3d800460, // str q0, [x3, #0x10]
    0xad010060, // stp q0, q0, [x3, #0x20]
    0x927ae463, // and x3, x3, #0xffffffffffffffc0
    0xad020060, // stp q0, q0, [x3, #0x40]
    0xad030060, // stp q0, q0, [x3, #0x60]
    0xcb030082, // sub x2, x4, x3
    0xd1040042, // sub x2, x2, #0x100
    0x91020063, // add x3, x3, #0x80
    0xd503201f, // nop
    0xd50b7423, // dc zva, x3
    0x91010063, // add x3, x3, #0x40
    0xf1010042, // subs x2, x2, #0x40
    0x54ffffa8, // b.hi 0x100 <memset+0x100>
    0xad000060, // stp q0, q0, [x3]
    0xad010060, // stp q0, q0, [x3, #0x20]
    0xad3e0080, // stp q0, q0, [x4, #-0x40]
    0xad3f0080, // stp q0, q0, [x4, #-0x20]
    0xd65f03c0, // ret
    0xd503201f, // nop
    0x710014bf, // cmp w5, #0x5
    0x54000241, // b.ne 0x174 <memset+0x174>
    0x3d800460, // str q0, [x3, #0x10]
    0xad010060, // stp q0, q0, [x3, #0x20]
    0xad020060, // stp q0, q0, [x3, #0x40]
    0xad030060, // stp q0, q0, [x3, #0x60]
    0x9279e063, // and x3, x3, #0xffffffffffffff80
    0xcb030082, // sub x2, x4, x3
    0xd1040042, // sub x2, x2, #0x100
    0x91020063, // add x3, x3, #0x80
    0xd50b7423, // dc zva, x3
    0x91020063, // add x3, x3, #0x80
    0xf1020042, // subs x2, x2, #0x80
    0x54ffffa8, // b.hi 0x150 <memset+0x150>
    0xad3c0080, // stp q0, q0, [x4, #-0x80]
    0xad3d0080, // stp q0, q0, [x4, #-0x60]
    0xad3e0080, // stp q0, q0, [x4, #-0x40]
    0xad3f0080, // stp q0, q0, [x4, #-0x20]
    0xd65f03c0, // ret
    0x52800086, // mov w6, #0x4                // =4
    0x1ac520c7, // lsl w7, w6, w5
    0x910100e5, // add x5, x7, #0x40
    0xeb05005f, // cmp x2, x5
    0x54fff8c3, // b.lo 0x9c <memset+0x9c>
    0xd10004e6, // sub x6, x7, #0x1
    0x8b070065, // add x5, x3, x7
    0x91004063, // add x3, x3, #0x10
    0xeb0300a2, // subs x2, x5, x3
    0x8a2600a5, // bic x5, x5, x6
    0x540000a0, // b.eq 0x1b0 <memset+0x1b0>
    0xac820060, // stp q0, q0, [x3], #0x40
    0xad3f0060, // stp q0, q0, [x3, #-0x20]
    0xf1010042, // subs x2, x2, #0x40
    0x54ffffa8, // b.hi 0x1a0 <memset+0x1a0>
    0xaa0503e3, // mov x3, x5
    0xcb050082, // sub x2, x4, x5
    0xeb070042, // subs x2, x2, x7
    0x540000a3, // b.lo 0x1d0 <memset+0x1d0>
    0xd50b7423, // dc zva, x3
    0x8b070063, // add x3, x3, x7
    0xeb070042, // subs x2, x2, x7
    0x54ffffa2, // b.hs 0x1c0 <memset+0x1c0>
    0x8b070042, // add x2, x2, x7
    0x17ffffb7, // b 0xb0 <memset+0xb0>
};

/*
 * Copyright (c) 2017 ARM Ltd
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the company may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY ARM LTD ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL ARM LTD BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Assumptions:
 *
 * ARMv8-a, AArch64, unaligned accesses.
 */

// Bionic 050b1b17b292418ca009ff0e07f25d1c036db246/libc/arch-arm64/generic/bionic/memcmp.S
constexpr std::array<std::uint32_t, 68> kMemcmp64{
    0xf1002042, // subs x2, x2, #0x8
    0x54000663, // b.lo 0xd0 <memcmp+0xd0>
    0xf8408403, // ldr x3, [x0], #0x8
    0xf8408425, // ldr x5, [x1], #0x8
    0xeb05007f, // cmp x3, x5
    0x54000501, // b.ne 0xb4 <memcmp+0xb4>
    0xf1002042, // subs x2, x2, #0x8
    0x5400008c, // b.gt 0x2c <memcmp+0x2c>
    0xf8626803, // ldr x3, [x0, x2]
    0xf8626825, // ldr x5, [x1, x2]
    0x14000023, // b 0xb4 <memcmp+0xb4>
    0xf8408403, // ldr x3, [x0], #0x8
    0xf8408425, // ldr x5, [x1], #0x8
    0xeb05007f, // cmp x3, x5
    0x540003e1, // b.ne 0xb4 <memcmp+0xb4>
    0xf1004042, // subs x2, x2, #0x10
    0x54000289, // b.ls 0x90 <memcmp+0x90>
    0xf101805f, // cmp x2, #0x60
    0x540000c9, // b.ls 0x60 <memcmp+0x60>
    0x92400c07, // and x7, x0, #0xf
    0x8b070042, // add x2, x2, x7
    0xcb070000, // sub x0, x0, x7
    0xcb070021, // sub x1, x1, x7
    0xd503201f, // nop
    0xa8c11003, // ldp x3, x4, [x0], #0x10
    0xa8c11825, // ldp x5, x6, [x1], #0x10
    0xf1004042, // subs x2, x2, #0x10
    0xfa458060, // ccmp x3, x5, #0x0, hi
    0xfa460080, // ccmp x4, x6, #0x0, eq
    0x54ffff60, // b.eq 0x60 <memcmp+0x60>
    0xeb05007f, // cmp x3, x5
    0x540001c1, // b.ne 0xb4 <memcmp+0xb4>
    0xaa0403e3, // mov x3, x4
    0xaa0603e5, // mov x5, x6
    0xeb05007f, // cmp x3, x5
    0x54000141, // b.ne 0xb4 <memcmp+0xb4>
    0x8b020000, // add x0, x0, x2
    0x8b020021, // add x1, x1, x2
    0xa9401003, // ldp x3, x4, [x0]
    0xa9401825, // ldp x5, x6, [x1]
    0xeb05007f, // cmp x3, x5
    0x54000081, // b.ne 0xb4 <memcmp+0xb4>
    0xaa0403e3, // mov x3, x4
    0xaa0603e5, // mov x5, x6
    0xeb05007f, // cmp x3, x5
    0xdac00c63, // rev x3, x3
    0xdac00ca5, // rev x5, x5
    0xeb05007f, // cmp x3, x5
    0x1a9f07e0, // cset w0, ne
    0x5a802400, // cneg w0, w0, lo
    0xd65f03c0, // ret
    0xd503201f, // nop
    0xb1001042, // adds x2, x2, #0x4
    0x540000c3, // b.lo 0xec <memcmp+0xec>
    0xb8404403, // ldr w3, [x0], #0x4
    0xb8404425, // ldr w5, [x1], #0x4
    0x6b05007f, // cmp w3, w5
    0x54fffe81, // b.ne 0xb4 <memcmp+0xb4>
    0xd1001042, // sub x2, x2, #0x4
    0xb1001042, // adds x2, x2, #0x4
    0x54fffe80, // b.eq 0xc0 <memcmp+0xc0>
    0x38401403, // ldrb w3, [x0], #0x1
    0x38401425, // ldrb w5, [x1], #0x1
    0xf1000442, // subs x2, x2, #0x1
    0x7a451060, // ccmp w3, w5, #0x0, ne
    0x54ffff80, // b.eq 0xf4 <memcmp+0xf4>
    0x4b050060, // sub w0, w3, w5
    0xd65f03c0, // ret
};

/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

// Bionic 050b1b17b292418ca009ff0e07f25d1c036db246/libc/arch-arm/generic/bionic/memcmp.S
constexpr std::array<std::uint32_t, 191> kMemcmp32{
    0xf5d0f000, // pld [r0]
    0xf5d0f040, // pld [r0, #0x40]
    0xe1500001, // cmp r0, r1
    0x03a00000, // moveq r0, #0
    0x012fff1e, // bxeq lr
    0xf5d1f000, // pld [r1]
    0xf5d1f040, // pld [r1, #0x40]
    0xe352000c, // cmp r2, #12
    0x4a000061, // bmi 0x1ac <memcmp+0x1ac>    @ imm = #0x184
    0xe2522020, // subs r2, r2, #32
    0x3a000017, // blo 0x8c <memcmp+0x8c>      @ imm = #0x5c
    0xf5d0f080, // pld [r0, #0x80]
    0xf5d1f080, // pld [r1, #0x80]
    0xf420020d, // vld1.8 {d0, d1, d2, d3}, [r0]!
    0xf5d0f080, // pld [r0, #0x80]
    0xf421420d, // vld1.8 {d4, d5, d6, d7}, [r1]!
    0xf5d1f080, // pld [r1, #0x80]
    0xf3000844, // vsub.i8 q0, q0, q2
    0xf3022846, // vsub.i8 q1, q1, q3
    0xf2204152, // vorr q2, q0, q1
    0xf2244115, // vorr d4, d4, d5
    0xec5c3b14, // vmov r3, r12, d4
    0xe193300c, // orrs r3, r3, r12
    0x1a000002, // bne 0x6c <memcmp+0x6c>      @ imm = #0x8
    0xe2522020, // subs r2, r2, #32
    0x2afffff2, // bhs 0x34 <memcmp+0x34>      @ imm = #-0x38
    0xea000007, // b 0x8c <memcmp+0x8c>      @ imm = #0x1c
    0xe2400020, // sub r0, r0, #32
    0xf2200111, // vorr d0, d0, d1
    0xe2411020, // sub r1, r1, #32
    0xec5c3b10, // vmov r3, r12, d0
    0xe193300c, // orrs r3, r3, r12
    0x02422010, // subeq r2, r2, #16
    0x02800010, // addeq r0, r0, #16
    0x02811010, // addeq r1, r1, #16
    0xe2822020, // add r2, r2, #32
    0xe352000c, // cmp r2, #12
    0x4a000044, // bmi 0x1ac <memcmp+0x1ac>    @ imm = #0x110
    0xe92d4010, // push {r4, lr}
    0xe1a04000, // mov r4, r0
    0xe2643000, // rsb r3, r4, #0
    0xe2133003, // ands r3, r3, #3
    0x0a000006, // beq 0xc8 <memcmp+0xc8>      @ imm = #0x18
    0xe0422003, // sub r2, r2, r3
    0xe4d40001, // ldrb r0, [r4], #1
    0xe4d1c001, // ldrb r12, [r1], #1
    0xe050000c, // subs r0, r0, r12
    0x1a000039, // bne 0x1a8 <memcmp+0x1a8>    @ imm = #0xe4
    0xe2533001, // subs r3, r3, #1
    0x1afffff9, // bne 0xb0 <memcmp+0xb0>      @ imm = #-0x1c
    0xe0240001, // eor r0, r4, r1
    0xe2100003, // ands r0, r0, #3
    0x1a000040, // bne 0x1d8 <memcmp+0x1d8>    @ imm = #0x100
    0xe591c000, // ldr r12, [r1]
    0xe2522024, // subs r2, r2, #36
    0x4a00001c, // bmi 0x154 <memcmp+0x154>    @ imm = #0x70
    0xf5d4f080, // pld [r4, #0x80]
    0xf5d1f080, // pld [r1, #0x80]
    0xe4940004, // ldr r0, [r4], #4
    0xe5b1e004, // ldr lr, [r1, #0x4]!
    0xe030000c, // eors r0, r0, r12
    0x04940004, // ldreq r0, [r4], #4
    0x05b1c004, // ldreq r12, [r1, #0x4]!
    0x0030000e, // eorseq r0, r0, lr
    0x04940004, // ldreq r0, [r4], #4
    0x05b1e004, // ldreq lr, [r1, #0x4]!
    0x0030000c, // eorseq r0, r0, r12
    0x04940004, // ldreq r0, [r4], #4
    0x05b1c004, // ldreq r12, [r1, #0x4]!
    0x0030000e, // eorseq r0, r0, lr
    0x04940004, // ldreq r0, [r4], #4
    0x05b1e004, // ldreq lr, [r1, #0x4]!
    0x0030000c, // eorseq r0, r0, r12
    0x04940004, // ldreq r0, [r4], #4
    0x05b1c004, // ldreq r12, [r1, #0x4]!
    0x0030000e, // eorseq r0, r0, lr
    0x04940004, // ldreq r0, [r4], #4
    0x05b1e004, // ldreq lr, [r1, #0x4]!
    0x0030000c, // eorseq r0, r0, r12
    0x04940004, // ldreq r0, [r4], #4
    0x05b1c004, // ldreq r12, [r1, #0x4]!
    0x0030000e, // eorseq r0, r0, lr
    0x1a00000d, // bne 0x184 <memcmp+0x184>    @ imm = #0x34
    0xe2522020, // subs r2, r2, #32
    0x2affffe2, // bhs 0xe0 <memcmp+0xe0>      @ imm = #-0x78
    0xe2922020, // adds r2, r2, #32
    0x4a000005, // bmi 0x174 <memcmp+0x174>    @ imm = #0x14
    0xe4940004, // ldr r0, [r4], #4
    0xe491c004, // ldr r12, [r1], #4
    0xe030000c, // eors r0, r0, r12
    0x1a000005, // bne 0x184 <memcmp+0x184>    @ imm = #0x14
    0xe2522004, // subs r2, r2, #4
    0x2afffff9, // bhs 0x15c <memcmp+0x15c>    @ imm = #-0x1c
    0xe2922004, // adds r2, r2, #4
    0x03a00000, // moveq r0, #0
    0x0a000009, // beq 0x1a8 <memcmp+0x1a8>    @ imm = #0x24
    0xea000002, // b 0x190 <memcmp+0x190>    @ imm = #0x8
    0xe2444004, // sub r4, r4, #4
    0xe2411004, // sub r1, r1, #4
    0xe3a02004, // mov r2, #4
    0xe4d40001, // ldrb r0, [r4], #1
    0xe4d1c001, // ldrb r12, [r1], #1
    0xe050000c, // subs r0, r0, r12
    0x1a000001, // bne 0x1a8 <memcmp+0x1a8>    @ imm = #0x4
    0xe2522001, // subs r2, r2, #1
    0x1afffff9, // bne 0x190 <memcmp+0x190>    @ imm = #-0x1c
    0xe8bd8010, // pop {r4, pc}
    0xe3520000, // cmp r2, #0
    0x03a00000, // moveq r0, #0
    0x012fff1e, // bxeq lr
    0xe1a03000, // mov r3, r0
    0xe4d30001, // ldrb r0, [r3], #1
    0xe4d1c001, // ldrb r12, [r1], #1
    0xe050000c, // subs r0, r0, r12
    0x112fff1e, // bxne lr
    0xe2522001, // subs r2, r2, #1
    0x1afffff9, // bne 0x1bc <memcmp+0x1bc>    @ imm = #-0x1c
    0xe12fff1e, // bx lr
    0xe2010003, // and r0, r1, #3
    0xe3500002, // cmp r0, #2
    0x1a000026, // bne 0x280 <memcmp+0x280>    @ imm = #0x98
    0xe2522010, // subs r2, r2, #16
    0x42822010, // addmi r2, r2, #16
    0x4affffe7, // bmi 0x190 <memcmp+0x190>    @ imm = #-0x64
    0xe3c11003, // bic r1, r1, #3
    0xe491e004, // ldr lr, [r1], #4
    0xf5d1f080, // pld [r1, #0x80]
    0xf5d4f080, // pld [r4, #0x80]
    0xe1a0c82e, // lsr r12, lr, #16
    0xe491e004, // ldr lr, [r1], #4
    0xe4940004, // ldr r0, [r4], #4
    0xe18cc80e, // orr r12, r12, lr, lsl #16
    0xe030000c, // eors r0, r0, r12
    0x01a0c82e, // lsreq r12, lr, #16
    0x0491e004, // ldreq lr, [r1], #4
    0x04940004, // ldreq r0, [r4], #4
    0x018cc80e, // orreq r12, r12, lr, lsl #16
    0x0030000c, // eorseq r0, r0, r12
    0x01a0c82e, // lsreq r12, lr, #16
    0x0491e004, // ldreq lr, [r1], #4
    0x04940004, // ldreq r0, [r4], #4
    0x018cc80e, // orreq r12, r12, lr, lsl #16
    0x0030000c, // eorseq r0, r0, r12
    0x01a0c82e, // lsreq r12, lr, #16
    0x0491e004, // ldreq lr, [r1], #4
    0x04940004, // ldreq r0, [r4], #4
    0x018cc80e, // orreq r12, r12, lr, lsl #16
    0x0030000c, // eorseq r0, r0, r12
    0x1a000006, // bne 0x270 <memcmp+0x270>    @ imm = #0x18
    0xe2522010, // subs r2, r2, #16
    0x2affffe6, // bhs 0x1f8 <memcmp+0x1f8>    @ imm = #-0x68
    0xe2411002, // sub r1, r1, #2
    0xe2922010, // adds r2, r2, #16
    0x03a00000, // moveq r0, #0
    0x0affffce, // beq 0x1a8 <memcmp+0x1a8>    @ imm = #-0xc8
    0xeaffffc7, // b 0x190 <memcmp+0x190>    @ imm = #-0xe4
    0xe2411006, // sub r1, r1, #6
    0xe2444004, // sub r4, r4, #4
    0xe3a02004, // mov r2, #4
    0xeaffffc3, // b 0x190 <memcmp+0x190>    @ imm = #-0xf4
    0xe92d00e0, // push {r5, r6, r7}
    0xe1a05180, // lsl r5, r0, #3
    0xe2656020, // rsb r6, r5, #32
    0xe3c11003, // bic r1, r1, #3
    0xe4917004, // ldr r7, [r1], #4
    0xe2422008, // sub r2, r2, #8
    0xe1a0c537, // lsr r12, r7, r5
    0xe4917004, // ldr r7, [r1], #4
    0xe4940004, // ldr r0, [r4], #4
    0xe18cc617, // orr r12, r12, r7, lsl r6
    0xe030000c, // eors r0, r0, r12
    0x01a0c537, // lsreq r12, r7, r5
    0x04917004, // ldreq r7, [r1], #4
    0x04940004, // ldreq r0, [r4], #4
    0x018cc617, // orreq r12, r12, r7, lsl r6
    0x0030000c, // eorseq r0, r0, r12
    0x1a000007, // bne 0x2e4 <memcmp+0x2e4>    @ imm = #0x1c
    0xe2522008, // subs r2, r2, #8
    0x2afffff2, // bhs 0x298 <memcmp+0x298>    @ imm = #-0x38
    0xe04111a6, // sub r1, r1, r6, lsr #3
    0xe8bd00e0, // pop {r5, r6, r7}
    0xe2922008, // adds r2, r2, #8
    0x03a00000, // moveq r0, #0
    0x0affffb1, // beq 0x1a8 <memcmp+0x1a8>    @ imm = #-0x13c
    0xeaffffaa, // b 0x190 <memcmp+0x190>    @ imm = #-0x158
    0xe2411004, // sub r1, r1, #4
    0xe04111a6, // sub r1, r1, r6, lsr #3
    0xe2444004, // sub r4, r4, #4
    0xe3a02004, // mov r2, #4
    0xe8bd00e0, // pop {r5, r6, r7}
    0xeaffffa4, // b 0x190 <memcmp+0x190>    @ imm = #-0x170
};

// Compiled with clang -O2 --target=aarch64-linux-gnu -ffreestanding
// -fno-builtin -fno-strict-aliasing -fno-math-errno from C: scalbn/scalbnf and
// bsearch adapted from musl (Copyright (c) 2005-2020 Rich Felker, et al.; MIT
// license, see LICENSES/MIT.txt), strcmp written for Venture. Each call of a
// host handler costs a JIT exit and an FP environment switch; these run inside
// translated code. In Batman: Arkham Shadow's level loads, UnityGfxDeviceWorker
// spent 55% of its stalled time entering ldexp and Loading.Preload a third in
// bsearch, whose every comparison was a host-to-guest callback, and the strcmp
// that comparison called.
// scalbn: musl's algorithm, word for word. Bionic's libm reports range errors
// through FP exceptions only (math_errhandling is MATH_ERREXCEPT), never errno.
constexpr std::array<std::uint32_t, 36> kScalbn64{
    0x7110001f, // 0x000: cmp w0, #0x400
    0x540001ab, // 0x004: b.lt 0x38 <g_scalbn+0x38>
    0xd2effc08, // 0x008: mov x8, #0x7fe0000000000000
    0x711ffc1f, // 0x00c: cmp w0, #0x7ff
    0x9e670101, // 0x010: fmov d1, x8
    0x1e610800, // 0x014: fmul d0, d0, d1
    0x540002c3, // 0x018: b.lo 0x70 <g_scalbn+0x70>
    0x9e670101, // 0x01c: fmov d1, x8
    0x712ff41f, // 0x020: cmp w0, #0xbfd
    0x52817fa8, // 0x024: mov w8, #0xbfd
    0x1a883008, // 0x028: csel w8, w0, w8, lo
    0x511ff900, // 0x02c: sub w0, w8, #0x7fe
    0x1e610800, // 0x030: fmul d0, d0, d1
    0x14000012, // 0x034: b 0x7c <g_scalbn+0x7c>
    0x310ffc1f, // 0x038: cmn w0, #0x3ff
    0x5400020c, // 0x03c: b.gt 0x7c <g_scalbn+0x7c>
    0xd2e06c08, // 0x040: mov x8, #0x360000000000000
    0x311f201f, // 0x044: cmn w0, #0x7c8
    0x9e670101, // 0x048: fmov d1, x8
    0x1e610800, // 0x04c: fmul d0, d0, d1
    0x54000148, // 0x050: b.hi 0x78 <g_scalbn+0x78>
    0x9e670101, // 0x054: fmov d1, x8
    0x312e401f, // 0x058: cmn w0, #0xb90
    0x128171e8, // 0x05c: mov w8, #-0xb90
    0x1a888008, // 0x060: csel w8, w0, w8, hi
    0x111e4900, // 0x064: add w0, w8, #0x792
    0x1e610800, // 0x068: fmul d0, d0, d1
    0x14000004, // 0x06c: b 0x7c <g_scalbn+0x7c>
    0x510ffc00, // 0x070: sub w0, w0, #0x3ff
    0x14000002, // 0x074: b 0x7c <g_scalbn+0x7c>
    0x110f2400, // 0x078: add w0, w0, #0x3c9
    0x110ffc08, // 0x07c: add w8, w0, #0x3ff
    0xd34c2d08, // 0x080: lsl x8, x8, #52
    0x9e670101, // 0x084: fmov d1, x8
    0x1e610800, // 0x088: fmul d0, d0, d1
    0xd65f03c0, // 0x08c: ret
};
constexpr std::array<std::uint32_t, 34> kScalbnf64{
    0x7102001f, // 0x000: cmp w0, #0x80
    0x5400016b, // 0x004: b.lt 0xc0 <g_scalbnf+0x30>
    0x0f0367e1, // 0x008: movi v1.2s, #0x7f, lsl #24
    0x7103fc1f, // 0x00c: cmp w0, #0xff
    0x1e210800, // 0x010: fmul s0, s0, s1
    0x540002a3, // 0x014: b.lo 0xf8 <g_scalbnf+0x68>
    0x7105f41f, // 0x018: cmp w0, #0x17d
    0x52802fa8, // 0x01c: mov w8, #0x17d
    0x1e210800, // 0x020: fmul s0, s0, s1
    0x1a883008, // 0x024: csel w8, w0, w8, lo
    0x5103f900, // 0x028: sub w0, w8, #0xfe
    0x14000012, // 0x02c: b 0x104 <g_scalbnf+0x74>
    0x3101fc1f, // 0x030: cmn w0, #0x7f
    0x5400020c, // 0x034: b.gt 0x104 <g_scalbnf+0x74>
    0x52a19008, // 0x038: mov w8, #0xc800000
    0x3103941f, // 0x03c: cmn w0, #0xe5
    0x1e270101, // 0x040: fmov s1, w8
    0x1e210800, // 0x044: fmul s0, s0, s1
    0x54000148, // 0x048: b.hi 0x100 <g_scalbnf+0x70>
    0x1e270101, // 0x04c: fmov s1, w8
    0x3105281f, // 0x050: cmn w0, #0x14a
    0x12802928, // 0x054: mov w8, #-0x14a
    0x1a888008, // 0x058: csel w8, w0, w8, hi
    0x11033100, // 0x05c: add w0, w8, #0xcc
    0x1e210800, // 0x060: fmul s0, s0, s1
    0x14000004, // 0x064: b 0x104 <g_scalbnf+0x74>
    0x5101fc00, // 0x068: sub w0, w0, #0x7f
    0x14000002, // 0x06c: b 0x104 <g_scalbnf+0x74>
    0x11019800, // 0x070: add w0, w0, #0x66
    0x52a7f008, // 0x074: mov w8, #0x3f800000
    0x0b005d08, // 0x078: add w8, w8, w0, lsl #23
    0x1e270101, // 0x07c: fmov s1, w8
    0x1e210800, // 0x080: fmul s0, s0, s1
    0xd65f03c0, // 0x084: ret
};
constexpr std::array<std::uint32_t, 34> kBsearch64{
    0xa9bb7bfd, // 0x000: stp x29, x30, [sp, #-0x50]!
    0xf9000bf9, // 0x004: str x25, [sp, #0x10]
    0x910003fd, // 0x008: mov x29, sp
    0xa9025ff8, // 0x00c: stp x24, x23, [sp, #0x20]
    0xa90357f6, // 0x010: stp x22, x21, [sp, #0x30]
    0xa9044ff4, // 0x014: stp x20, x19, [sp, #0x40]
    0xb4000282, // 0x018: cbz x2, 0x180 <g_bsearch+0x68>
    0xaa0403f3, // 0x01c: mov x19, x4
    0xaa0303f4, // 0x020: mov x20, x3
    0xaa0203f6, // 0x024: mov x22, x2
    0xaa0103f7, // 0x028: mov x23, x1
    0xaa0003f5, // 0x02c: mov x21, x0
    0x14000003, // 0x030: b 0x154 <g_bsearch+0x3c>
    0xaa1903f6, // 0x034: mov x22, x25
    0xb4000196, // 0x038: cbz x22, 0x180 <g_bsearch+0x68>
    0xd341fed9, // 0x03c: lsr x25, x22, #1
    0xaa1503e0, // 0x040: mov x0, x21
    0x9b145f38, // 0x044: madd x24, x25, x20, x23
    0xaa1803e1, // 0x048: mov x1, x24
    0xd63f0260, // 0x04c: blr x19
    0x37ffff20, // 0x050: tbnz w0, #0x1f, 0x14c <g_bsearch+0x34>
    0x340000c0, // 0x054: cbz w0, 0x184 <g_bsearch+0x6c>
    0xaa3903e8, // 0x058: mvn x8, x25
    0x8b140317, // 0x05c: add x23, x24, x20
    0x8b0802d6, // 0x060: add x22, x22, x8
    0xb5fffed6, // 0x064: cbnz x22, 0x154 <g_bsearch+0x3c>
    0xaa1f03f8, // 0x068: mov x24, xzr
    0xaa1803e0, // 0x06c: mov x0, x24
    0xa9444ff4, // 0x070: ldp x20, x19, [sp, #0x40]
    0xa94357f6, // 0x074: ldp x22, x21, [sp, #0x30]
    0xf9400bf9, // 0x078: ldr x25, [sp, #0x10]
    0xa9425ff8, // 0x07c: ldp x24, x23, [sp, #0x20]
    0xa8c57bfd, // 0x080: ldp x29, x30, [sp], #0x50
    0xd65f03c0, // 0x084: ret
};

// qsort_r: introsort written for Venture (median-of-three Hoare partition,
// insertion sort below 13 elements, heapsort past 2*log2(n) levels), compiled
// as above plus -fno-jump-tables. The comparator only ever sees addresses
// inside the array. As a host handler every comparison was a JIT exit and
// re-entry, about 180 ns: Robo Recall's level load spent it in Vorbis codebook
// setup on every audio decode worker.
constexpr std::array<std::uint32_t, 351> kQsortR64{
    0xf100005f, // 0x000: cmp x2, #0x0
    0xfa421820, // 0x004: ccmp x1, #0x2, #0x0, ne
    0x54000103, // 0x008: b.lo 0x28 <g_qsort_r+0x28>
    0x2a1f03e5, // 0x00c: mov w5, wzr
    0xaa0103e8, // 0x010: mov x8, x1
    0xf1000d1f, // 0x014: cmp x8, #0x3
    0xd341fd08, // 0x018: lsr x8, x8, #1
    0x110008a5, // 0x01c: add w5, w5, #0x2
    0x54ffffa8, // 0x020: b.hi 0x14 <g_qsort_r+0x14>
    0x14000002, // 0x024: b 0x2c <introsort_r>
    0xd65f03c0, // 0x028: ret
    0xd10203ff, // 0x02c: sub sp, sp, #0x80
    0xa9036ffc, // 0x030: stp x28, x27, [sp, #0x30]
    0xaa0103fc, // 0x034: mov x28, x1
    0xf100343f, // 0x038: cmp x1, #0xd
    0xa90657f6, // 0x03c: stp x22, x21, [sp, #0x60]
    0xaa0203f5, // 0x040: mov x21, x2
    0xaa0003f6, // 0x044: mov x22, x0
    0xa9074ff4, // 0x048: stp x20, x19, [sp, #0x70]
    0xaa0403f3, // 0x04c: mov x19, x4
    0xaa0303f4, // 0x050: mov x20, x3
    0xa9027bfd, // 0x054: stp x29, x30, [sp, #0x20]
    0x910083fd, // 0x058: add x29, sp, #0x20
    0xa90467fa, // 0x05c: stp x26, x25, [sp, #0x40]
    0xa9055ff8, // 0x060: stp x24, x23, [sp, #0x50]
    0x54001363, // 0x064: b.lo 0x2d0 <introsort_r+0x2a4>
    0x2a0503f7, // 0x068: mov w23, w5
    0x34000d65, // 0x06c: cbz w5, 0x218 <introsort_r+0x1ec>
    0xcb1503fb, // 0x070: neg x27, x21
    0xd341ff88, // 0x074: lsr x8, x28, #1
    0xaa1603e1, // 0x078: mov x1, x22
    0xaa1303e2, // 0x07c: mov x2, x19
    0x9b155919, // 0x080: madd x25, x8, x21, x22
    0xaa1903e0, // 0x084: mov x0, x25
    0xd63f0280, // 0x088: blr x20
    0x36f800a0, // 0x08c: tbz w0, #0x1f, 0xa0 <introsort_r+0x74>
    0xaa1903e0, // 0x090: mov x0, x25
    0xaa1603e1, // 0x094: mov x1, x22
    0xaa1503e2, // 0x098: mov x2, x21
    0x940000b0, // 0x09c: bl 0x35c <swap>
    0xd1000788, // 0x0a0: sub x8, x28, #0x1
    0xaa1903e1, // 0x0a4: mov x1, x25
    0xaa1303e2, // 0x0a8: mov x2, x19
    0x9b15591a, // 0x0ac: madd x26, x8, x21, x22
    0xf9000bfc, // 0x0b0: str x28, [sp, #0x10]
    0xaa1a03e0, // 0x0b4: mov x0, x26
    0xd63f0280, // 0x0b8: blr x20
    0x36f801c0, // 0x0bc: tbz w0, #0x1f, 0xf4 <introsort_r+0xc8>
    0xaa1a03e0, // 0x0c0: mov x0, x26
    0xaa1903e1, // 0x0c4: mov x1, x25
    0xaa1503e2, // 0x0c8: mov x2, x21
    0x940000a4, // 0x0cc: bl 0x35c <swap>
    0xaa1903e0, // 0x0d0: mov x0, x25
    0xaa1603e1, // 0x0d4: mov x1, x22
    0xaa1303e2, // 0x0d8: mov x2, x19
    0xd63f0280, // 0x0dc: blr x20
    0x36f800a0, // 0x0e0: tbz w0, #0x1f, 0xf4 <introsort_r+0xc8>
    0xaa1903e0, // 0x0e4: mov x0, x25
    0xaa1603e1, // 0x0e8: mov x1, x22
    0xaa1503e2, // 0x0ec: mov x2, x21
    0x9400009b, // 0x0f0: bl 0x35c <swap>
    0x510006f7, // 0x0f4: sub w23, w23, #0x1
    0xaa1603e0, // 0x0f8: mov x0, x22
    0xaa1903e1, // 0x0fc: mov x1, x25
    0xaa1503e2, // 0x100: mov x2, x21
    0xb9000ff7, // 0x104: str w23, [sp, #0xc]
    0x94000095, // 0x108: bl 0x35c <swap>
    0xf9400bf7, // 0x10c: ldr x23, [sp, #0x10]
    0xaa1f03fc, // 0x110: mov x28, xzr
    0x9b1c7ea8, // 0x114: mul x8, x21, x28
    0x8b1502c9, // 0x118: add x9, x22, x21
    0x8b0802da, // 0x11c: add x26, x22, x8
    0x8b080139, // 0x120: add x25, x9, x8
    0xaa1903e0, // 0x124: mov x0, x25
    0xaa1603e1, // 0x128: mov x1, x22
    0xaa1303e2, // 0x12c: mov x2, x19
    0x9100079c, // 0x130: add x28, x28, #0x1
    0xd63f0280, // 0x134: blr x20
    0x8b15035a, // 0x138: add x26, x26, x21
    0x8b150339, // 0x13c: add x25, x25, x21
    0x37ffff20, // 0x140: tbnz w0, #0x1f, 0x124 <introsort_r+0xf8>
    0xd10006e8, // 0x144: sub x8, x23, #0x1
    0xaa1703f9, // 0x148: mov x25, x23
    0xaa1603f7, // 0x14c: mov x23, x22
    0x9b087eb8, // 0x150: mul x24, x21, x8
    0xf81f83b9, // 0x154: stur x25, [x29, #-0x8]
    0x8b1802e1, // 0x158: add x1, x23, x24
    0xaa1603e0, // 0x15c: mov x0, x22
    0xaa1303e2, // 0x160: mov x2, x19
    0xd1000739, // 0x164: sub x25, x25, #0x1
    0xd63f0280, // 0x168: blr x20
    0x8b1b02f7, // 0x16c: add x23, x23, x27
    0x37ffff40, // 0x170: tbnz w0, #0x1f, 0x158 <introsort_r+0x12c>
    0xf85f83b8, // 0x174: ldur x24, [x29, #-0x8]
    0xeb19039f, // 0x178: cmp x28, x25
    0x9b185ea1, // 0x17c: madd x1, x21, x24, x23
    0x540000c2, // 0x180: b.hs 0x198 <introsort_r+0x16c>
    0xaa1a03e0, // 0x184: mov x0, x26
    0xaa1503e2, // 0x188: mov x2, x21
    0x94000074, // 0x18c: bl 0x35c <swap>
    0xaa1903f7, // 0x190: mov x23, x25
    0x17ffffe0, // 0x194: b 0x114 <introsort_r+0xe8>
    0xaa1603e0, // 0x198: mov x0, x22
    0xaa1503e2, // 0x19c: mov x2, x21
    0x9400006f, // 0x1a0: bl 0x35c <swap>
    0x9b1856b8, // 0x1a4: madd x24, x21, x24, x21
    0xf9400bfc, // 0x1a8: ldr x28, [sp, #0x10]
    0xaa3903e8, // 0x1ac: mvn x8, x25
    0x8b1c011c, // 0x1b0: add x28, x8, x28
    0xeb1c033f, // 0x1b4: cmp x25, x28
    0x54000182, // 0x1b8: b.hs 0x1e8 <introsort_r+0x1bc>
    0xaa1903e1, // 0x1bc: mov x1, x25
    0xb9400ff9, // 0x1c0: ldr w25, [sp, #0xc]
    0xaa1603e0, // 0x1c4: mov x0, x22
    0xaa1503e2, // 0x1c8: mov x2, x21
    0xaa1403e3, // 0x1cc: mov x3, x20
    0xaa1303e4, // 0x1d0: mov x4, x19
    0x2a1903e5, // 0x1d4: mov w5, w25
    0x97ffff95, // 0x1d8: bl 0x2c <introsort_r>
    0x8b1802f6, // 0x1dc: add x22, x23, x24
    0x2a1903f7, // 0x1e0: mov w23, w25
    0x1400000a, // 0x1e4: b 0x20c <introsort_r+0x1e0>
    0x8b1802e0, // 0x1e8: add x0, x23, x24
    0xb9400ff7, // 0x1ec: ldr w23, [sp, #0xc]
    0xaa1c03e1, // 0x1f0: mov x1, x28
    0xaa1503e2, // 0x1f4: mov x2, x21
    0xaa1403e3, // 0x1f8: mov x3, x20
    0xaa1303e4, // 0x1fc: mov x4, x19
    0x2a1703e5, // 0x200: mov w5, w23
    0x97ffff8a, // 0x204: bl 0x2c <introsort_r>
    0xaa1903fc, // 0x208: mov x28, x25
    0xf100339f, // 0x20c: cmp x28, #0xc
    0x54000609, // 0x210: b.ls 0x2d0 <introsort_r+0x2a4>
    0x35fff317, // 0x214: cbnz w23, 0x74 <introsort_r+0x48>
    0xd341ff9a, // 0x218: lsr x26, x28, #1
    0xb400007a, // 0x21c: cbz x26, 0x228 <introsort_r+0x1fc>
    0xd100075a, // 0x220: sub x26, x26, #0x1
    0x14000008, // 0x224: b 0x244 <introsort_r+0x218>
    0xf100079c, // 0x228: subs x28, x28, #0x1
    0x54000880, // 0x22c: b.eq 0x33c <introsort_r+0x310>
    0x9b155b81, // 0x230: madd x1, x28, x21, x22
    0xaa1603e0, // 0x234: mov x0, x22
    0xaa1503e2, // 0x238: mov x2, x21
    0x94000048, // 0x23c: bl 0x35c <swap>
    0xaa1f03fa, // 0x240: mov x26, xzr
    0x52800037, // 0x244: mov w23, #0x1 // =1
    0xb37ffb57, // 0x248: bfi x23, x26, #1, #63
    0xeb1c02ff, // 0x24c: cmp x23, x28
    0x54fffe62, // 0x250: b.hs 0x21c <introsort_r+0x1f0>
    0xd37ffb48, // 0x254: lsl x8, x26, #1
    0xaa1a03f8, // 0x258: mov x24, x26
    0x91000919, // 0x25c: add x25, x8, #0x2
    0xeb1c033f, // 0x260: cmp x25, x28
    0x54000102, // 0x264: b.hs 0x284 <introsort_r+0x258>
    0x9b155ae0, // 0x268: madd x0, x23, x21, x22
    0xaa1303e2, // 0x26c: mov x2, x19
    0x9b155b21, // 0x270: madd x1, x25, x21, x22
    0xd63f0280, // 0x274: blr x20
    0x7100001f, // 0x278: cmp w0, #0x0
    0x9a97433b, // 0x27c: csel x27, x25, x23, mi
    0x14000002, // 0x280: b 0x288 <introsort_r+0x25c>
    0xaa1703fb, // 0x284: mov x27, x23
    0x9b155b18, // 0x288: madd x24, x24, x21, x22
    0xaa1303e2, // 0x28c: mov x2, x19
    0x9b155b79, // 0x290: madd x25, x27, x21, x22
    0xaa1803e0, // 0x294: mov x0, x24
    0xaa1903e1, // 0x298: mov x1, x25
    0xd63f0280, // 0x29c: blr x20
    0x36fffbe0, // 0x2a0: tbz w0, #0x1f, 0x21c <introsort_r+0x1f0>
    0xaa1803e0, // 0x2a4: mov x0, x24
    0xaa1903e1, // 0x2a8: mov x1, x25
    0xaa1503e2, // 0x2ac: mov x2, x21
    0x9400002b, // 0x2b0: bl 0x35c <swap>
    0x52800037, // 0x2b4: mov w23, #0x1 // =1
    0xd37ffb68, // 0x2b8: lsl x8, x27, #1
    0xaa1b03f8, // 0x2bc: mov x24, x27
    0xb37ffb77, // 0x2c0: bfi x23, x27, #1, #63
    0xeb1c02ff, // 0x2c4: cmp x23, x28
    0x54fffca3, // 0x2c8: b.lo 0x25c <introsort_r+0x230>
    0x17ffffd4, // 0x2cc: b 0x21c <introsort_r+0x1f0>
    0xf1000b9f, // 0x2d0: cmp x28, #0x2
    0x54000343, // 0x2d4: b.lo 0x33c <introsort_r+0x310>
    0xcb1503f7, // 0x2d8: neg x23, x21
    0x92800019, // 0x2dc: mov x25, #-0x1 // =-1
    0x5280003a, // 0x2e0: mov w26, #0x1 // =1
    0x14000006, // 0x2e4: b 0x2fc <introsort_r+0x2d0>
    0x9100075a, // 0x2e8: add x26, x26, #0x1
    0x8b1502d6, // 0x2ec: add x22, x22, x21
    0xd1000739, // 0x2f0: sub x25, x25, #0x1
    0xeb1c035f, // 0x2f4: cmp x26, x28
    0x54000220, // 0x2f8: b.eq 0x33c <introsort_r+0x310>
    0xaa1903fb, // 0x2fc: mov x27, x25
    0xaa1603f8, // 0x300: mov x24, x22
    0x8b150301, // 0x304: add x1, x24, x21
    0xaa1803e0, // 0x308: mov x0, x24
    0xaa1303e2, // 0x30c: mov x2, x19
    0xd63f0280, // 0x310: blr x20
    0x7100001f, // 0x314: cmp w0, #0x0
    0x54fffe8d, // 0x318: b.le 0x2e8 <introsort_r+0x2bc>
    0x8b150301, // 0x31c: add x1, x24, x21
    0xaa1803e0, // 0x320: mov x0, x24
    0xaa1503e2, // 0x324: mov x2, x21
    0x9400000d, // 0x328: bl 0x35c <swap>
    0xb100077b, // 0x32c: adds x27, x27, #0x1
    0x8b170318, // 0x330: add x24, x24, x23
    0x54fffe83, // 0x334: b.lo 0x304 <introsort_r+0x2d8>
    0x17ffffec, // 0x338: b 0x2e8 <introsort_r+0x2bc>
    0xa9474ff4, // 0x33c: ldp x20, x19, [sp, #0x70]
    0xa94657f6, // 0x340: ldp x22, x21, [sp, #0x60]
    0xa9455ff8, // 0x344: ldp x24, x23, [sp, #0x50]
    0xa94467fa, // 0x348: ldp x26, x25, [sp, #0x40]
    0xa9436ffc, // 0x34c: ldp x28, x27, [sp, #0x30]
    0xa9427bfd, // 0x350: ldp x29, x30, [sp, #0x20]
    0x910203ff, // 0x354: add sp, sp, #0x80
    0xd65f03c0, // 0x358: ret
    0xaa000028, // 0x35c: orr x8, x1, x0
    0xaa020108, // 0x360: orr x8, x8, x2
    0xf240091f, // 0x364: tst x8, #0x7
    0x540002a0, // 0x368: b.eq 0x3bc <swap+0x60>
    0xf240051f, // 0x36c: tst x8, #0x3
    0x540002e0, // 0x370: b.eq 0x3cc <swap+0x70>
    0xf100205f, // 0x374: cmp x2, #0x8
    0x540000c3, // 0x378: b.lo 0x390 <swap+0x34>
    0x8b020008, // 0x37c: add x8, x0, x2
    0x8b020029, // 0x380: add x9, x1, x2
    0xeb08003f, // 0x384: cmp x1, x8
    0xfa493002, // 0x388: ccmp x0, x9, #0x2, lo
    0x54000502, // 0x38c: b.hs 0x42c <swap+0xd0>
    0xaa1f03e8, // 0x390: mov x8, xzr
    0xcb080049, // 0x394: sub x9, x2, x8
    0x8b08002a, // 0x398: add x10, x1, x8
    0x8b080008, // 0x39c: add x8, x0, x8
    0x3940014b, // 0x3a0: ldrb w11, [x10]
    0x3940010c, // 0x3a4: ldrb w12, [x8]
    0xf1000529, // 0x3a8: subs x9, x9, #0x1
    0x3800150b, // 0x3ac: strb w11, [x8], #0x1
    0x3800154c, // 0x3b0: strb w12, [x10], #0x1
    0x54ffff61, // 0x3b4: b.ne 0x3a0 <swap+0x44>
    0x14000052, // 0x3b8: b 0x500 <swap+0x1a4>
    0xf100e45f, // 0x3bc: cmp x2, #0x39
    0x540000e2, // 0x3c0: b.hs 0x3dc <swap+0x80>
    0xaa1f03e8, // 0x3c4: mov x8, xzr
    0x1400002e, // 0x3c8: b 0x480 <swap+0x124>
    0xf100745f, // 0x3cc: cmp x2, #0x1d
    0x540001a2, // 0x3d0: b.hs 0x404 <swap+0xa8>
    0xaa1f03e8, // 0x3d4: mov x8, xzr
    0x14000043, // 0x3d8: b 0x4e4 <swap+0x188>
    0xd1000448, // 0x3dc: sub x8, x2, #0x1
    0x927df109, // 0x3e0: and x9, x8, #0xfffffffffffffff8
    0x91002129, // 0x3e4: add x9, x9, #0x8
    0x8b09000a, // 0x3e8: add x10, x0, x9
    0x8b090029, // 0x3ec: add x9, x1, x9
    0xeb0a003f, // 0x3f0: cmp x1, x10
    0xfa493002, // 0x3f4: ccmp x0, x9, #0x2, lo
    0x54000222, // 0x3f8: b.hs 0x43c <swap+0xe0>
    0xaa1f03e8, // 0x3fc: mov x8, xzr
    0x14000020, // 0x400: b 0x480 <swap+0x124>
    0xd1000448, // 0x404: sub x8, x2, #0x1
    0x927ef509, // 0x408: and x9, x8, #0xfffffffffffffffc
    0x91001129, // 0x40c: add x9, x9, #0x4
    0x8b09000a, // 0x410: add x10, x0, x9
    0x8b090029, // 0x414: add x9, x1, x9
    0xeb0a003f, // 0x418: cmp x1, x10
    0xfa493002, // 0x41c: ccmp x0, x9, #0x2, lo
    0x54000402, // 0x420: b.hs 0x4a0 <swap+0x144>
    0xaa1f03e8, // 0x424: mov x8, xzr
    0x1400002f, // 0x428: b 0x4e4 <swap+0x188>
    0xf100805f, // 0x42c: cmp x2, #0x20
    0x540006a2, // 0x430: b.hs 0x504 <swap+0x1a8>
    0xaa1f03e8, // 0x434: mov x8, xzr
    0x14000043, // 0x438: b 0x544 <swap+0x1e8>
    0xd343fd08, // 0x43c: lsr x8, x8, #3
    0x9100402b, // 0x440: add x11, x1, #0x10
    0x9100400c, // 0x444: add x12, x0, #0x10
    0x91000509, // 0x448: add x9, x8, #0x1
    0x927eed2a, // 0x44c: and x10, x9, #0x3ffffffffffffffc
    0x927eed2d, // 0x450: and x13, x9, #0x3ffffffffffffffc
    0xd37df148, // 0x454: lsl x8, x10, #3
    0xad7f8981, // 0x458: ldp q1, q2, [x12, #-0x10]
    0xf10011ad, // 0x45c: subs x13, x13, #0x4
    0xad7f8d60, // 0x460: ldp q0, q3, [x11, #-0x10]
    0xad3f8961, // 0x464: stp q1, q2, [x11, #-0x10]
    0x9100816b, // 0x468: add x11, x11, #0x20
    0x3c9f0180, // 0x46c: stur q0, [x12, #-0x10]
    0x3c820583, // 0x470: str q3, [x12], #0x20
    0x54ffff21, // 0x474: b.ne 0x458 <swap+0xfc>
    0xeb0a013f, // 0x478: cmp x9, x10
    0x54000420, // 0x47c: b.eq 0x500 <swap+0x1a4>
    0xf8686829, // 0x480: ldr x9, [x1, x8]
    0xf868680a, // 0x484: ldr x10, [x0, x8]
    0xf8286809, // 0x488: str x9, [x0, x8]
    0xf828682a, // 0x48c: str x10, [x1, x8]
    0x91002108, // 0x490: add x8, x8, #0x8
    0xeb02011f, // 0x494: cmp x8, x2
    0x54ffff43, // 0x498: b.lo 0x480 <swap+0x124>
    0x14000019, // 0x49c: b 0x500 <swap+0x1a4>
    0xd342fd08, // 0x4a0: lsr x8, x8, #2
    0x9100402b, // 0x4a4: add x11, x1, #0x10
    0x9100400c, // 0x4a8: add x12, x0, #0x10
    0x91000509, // 0x4ac: add x9, x8, #0x1
    0x927ded2a, // 0x4b0: and x10, x9, #0x7ffffffffffffff8
    0x927ded2d, // 0x4b4: and x13, x9, #0x7ffffffffffffff8
    0xd37ef548, // 0x4b8: lsl x8, x10, #2
    0xad7f8981, // 0x4bc: ldp q1, q2, [x12, #-0x10]
    0xf10021ad, // 0x4c0: subs x13, x13, #0x8
    0xad7f8d60, // 0x4c4: ldp q0, q3, [x11, #-0x10]
    0xad3f8961, // 0x4c8: stp q1, q2, [x11, #-0x10]
    0x9100816b, // 0x4cc: add x11, x11, #0x20
    0x3c9f0180, // 0x4d0: stur q0, [x12, #-0x10]
    0x3c820583, // 0x4d4: str q3, [x12], #0x20
    0x54ffff21, // 0x4d8: b.ne 0x4bc <swap+0x160>
    0xeb0a013f, // 0x4dc: cmp x9, x10
    0x54000100, // 0x4e0: b.eq 0x500 <swap+0x1a4>
    0xb8686829, // 0x4e4: ldr w9, [x1, x8]
    0xb868680a, // 0x4e8: ldr w10, [x0, x8]
    0xb8286809, // 0x4ec: str w9, [x0, x8]
    0xb828682a, // 0x4f0: str w10, [x1, x8]
    0x91001108, // 0x4f4: add x8, x8, #0x4
    0xeb02011f, // 0x4f8: cmp x8, x2
    0x54ffff43, // 0x4fc: b.lo 0x4e4 <swap+0x188>
    0xd65f03c0, // 0x500: ret
    0x927d0449, // 0x504: and x9, x2, #0x18
    0x927be848, // 0x508: and x8, x2, #0xffffffffffffffe0
    0x9100402a, // 0x50c: add x10, x1, #0x10
    0x9100400b, // 0x510: add x11, x0, #0x10
    0x927be84c, // 0x514: and x12, x2, #0xffffffffffffffe0
    0xad7f8961, // 0x518: ldp q1, q2, [x11, #-0x10]
    0xf100818c, // 0x51c: subs x12, x12, #0x20
    0xad7f8d40, // 0x520: ldp q0, q3, [x10, #-0x10]
    0xad3f8941, // 0x524: stp q1, q2, [x10, #-0x10]
    0x9100814a, // 0x528: add x10, x10, #0x20
    0x3c9f0160, // 0x52c: stur q0, [x11, #-0x10]
    0x3c820563, // 0x530: str q3, [x11], #0x20
    0x54ffff21, // 0x534: b.ne 0x518 <swap+0x1bc>
    0xeb08005f, // 0x538: cmp x2, x8
    0x54fffe20, // 0x53c: b.eq 0x500 <swap+0x1a4>
    0xb4fff2a9, // 0x540: cbz x9, 0x394 <swap+0x38>
    0xaa0803eb, // 0x544: mov x11, x8
    0x927df048, // 0x548: and x8, x2, #0xfffffffffffffff8
    0xcb080169, // 0x54c: sub x9, x11, x8
    0x8b0b002a, // 0x550: add x10, x1, x11
    0x8b0b000b, // 0x554: add x11, x0, x11
    0xfd400140, // 0x558: ldr d0, [x10]
    0xfd400161, // 0x55c: ldr d1, [x11]
    0xb1002129, // 0x560: adds x9, x9, #0x8
    0xfc008560, // 0x564: str d0, [x11], #0x8
    0xfc008541, // 0x568: str d1, [x10], #0x8
    0x54ffff61, // 0x56c: b.ne 0x558 <swap+0x1fc>
    0xeb08005f, // 0x570: cmp x2, x8
    0x54fffc60, // 0x574: b.eq 0x500 <swap+0x1a4>
    0x17ffff87, // 0x578: b 0x394 <swap+0x38>
};
constexpr std::array<std::uint32_t, 121> kStrcmp64{
    0x4a000028, // 0x000: eor w8, w1, w0
    0xf240091f, // 0x004: tst x8, #0x7
    0x540001c0, // 0x008: b.eq 0x1e0 <g_strcmp+0x40>
    0x39400008, // 0x00c: ldrb w8, [x0]
    0x3940002b, // 0x010: ldrb w11, [x1]
    0x340009e8, // 0x014: cbz w8, 0x2f0 <g_strcmp+0x150>
    0x6b0b011f, // 0x018: cmp w8, w11
    0x540009a1, // 0x01c: b.ne 0x2f0 <g_strcmp+0x150>
    0x91000409, // 0x020: add x9, x0, #0x1
    0x9100042a, // 0x024: add x10, x1, #0x1
    0x38401528, // 0x028: ldrb w8, [x9], #0x1
    0x3840154b, // 0x02c: ldrb w11, [x10], #0x1
    0x34000908, // 0x030: cbz w8, 0x2f0 <g_strcmp+0x150>
    0x6b0b011f, // 0x034: cmp w8, w11
    0x54ffff80, // 0x038: b.eq 0x1c8 <g_strcmp+0x28>
    0x14000045, // 0x03c: b 0x2f0 <g_strcmp+0x150>
    0xf240081f, // 0x040: tst x0, #0x7
    0x540008a0, // 0x044: b.eq 0x2f8 <g_strcmp+0x158>
    0x39400008, // 0x048: ldrb w8, [x0]
    0x3940002b, // 0x04c: ldrb w11, [x1]
    0x34000808, // 0x050: cbz w8, 0x2f0 <g_strcmp+0x150>
    0x6b0b011f, // 0x054: cmp w8, w11
    0x540007c1, // 0x058: b.ne 0x2f0 <g_strcmp+0x150>
    0x92400809, // 0x05c: and x9, x0, #0x7
    0x5280010a, // 0x060: mov w10, #0x8
    0x91000408, // 0x064: add x8, x0, #0x1
    0xcb09014a, // 0x068: sub x10, x10, x9
    0xf240091f, // 0x06c: tst x8, #0x7
    0x8b0a0009, // 0x070: add x9, x0, x10
    0x8b0a002a, // 0x074: add x10, x1, x10
    0x54000740, // 0x078: b.eq 0x300 <g_strcmp+0x160>
    0x39400108, // 0x07c: ldrb w8, [x8]
    0x3940042b, // 0x080: ldrb w11, [x1, #0x1]
    0x34000668, // 0x084: cbz w8, 0x2f0 <g_strcmp+0x150>
    0x6b0b011f, // 0x088: cmp w8, w11
    0x54000621, // 0x08c: b.ne 0x2f0 <g_strcmp+0x150>
    0x91000808, // 0x090: add x8, x0, #0x2
    0xf240091f, // 0x094: tst x8, #0x7
    0x54000640, // 0x098: b.eq 0x300 <g_strcmp+0x160>
    0x39400108, // 0x09c: ldrb w8, [x8]
    0x3940082b, // 0x0a0: ldrb w11, [x1, #0x2]
    0x34000568, // 0x0a4: cbz w8, 0x2f0 <g_strcmp+0x150>
    0x6b0b011f, // 0x0a8: cmp w8, w11
    0x54000521, // 0x0ac: b.ne 0x2f0 <g_strcmp+0x150>
    0x91000c08, // 0x0b0: add x8, x0, #0x3
    0xf240091f, // 0x0b4: tst x8, #0x7
    0x54000540, // 0x0b8: b.eq 0x300 <g_strcmp+0x160>
    0x39400108, // 0x0bc: ldrb w8, [x8]
    0x39400c2b, // 0x0c0: ldrb w11, [x1, #0x3]
    0x34000468, // 0x0c4: cbz w8, 0x2f0 <g_strcmp+0x150>
    0x6b0b011f, // 0x0c8: cmp w8, w11
    0x54000421, // 0x0cc: b.ne 0x2f0 <g_strcmp+0x150>
    0x91001008, // 0x0d0: add x8, x0, #0x4
    0xf240091f, // 0x0d4: tst x8, #0x7
    0x54000440, // 0x0d8: b.eq 0x300 <g_strcmp+0x160>
    0x39400108, // 0x0dc: ldrb w8, [x8]
    0x3940102b, // 0x0e0: ldrb w11, [x1, #0x4]
    0x34000368, // 0x0e4: cbz w8, 0x2f0 <g_strcmp+0x150>
    0x6b0b011f, // 0x0e8: cmp w8, w11
    0x54000321, // 0x0ec: b.ne 0x2f0 <g_strcmp+0x150>
    0x91001408, // 0x0f0: add x8, x0, #0x5
    0xf240091f, // 0x0f4: tst x8, #0x7
    0x54000340, // 0x0f8: b.eq 0x300 <g_strcmp+0x160>
    0x39400108, // 0x0fc: ldrb w8, [x8]
    0x3940142b, // 0x100: ldrb w11, [x1, #0x5]
    0x34000268, // 0x104: cbz w8, 0x2f0 <g_strcmp+0x150>
    0x6b0b011f, // 0x108: cmp w8, w11
    0x54000221, // 0x10c: b.ne 0x2f0 <g_strcmp+0x150>
    0x91001808, // 0x110: add x8, x0, #0x6
    0xf240091f, // 0x114: tst x8, #0x7
    0x54000240, // 0x118: b.eq 0x300 <g_strcmp+0x160>
    0x39400108, // 0x11c: ldrb w8, [x8]
    0x3940182b, // 0x120: ldrb w11, [x1, #0x6]
    0x34000168, // 0x124: cbz w8, 0x2f0 <g_strcmp+0x150>
    0x6b0b011f, // 0x128: cmp w8, w11
    0x54000121, // 0x12c: b.ne 0x2f0 <g_strcmp+0x150>
    0x91001c08, // 0x130: add x8, x0, #0x7
    0xf240091f, // 0x134: tst x8, #0x7
    0x54000140, // 0x138: b.eq 0x300 <g_strcmp+0x160>
    0x39400108, // 0x13c: ldrb w8, [x8]
    0x39401c2b, // 0x140: ldrb w11, [x1, #0x7]
    0x34000068, // 0x144: cbz w8, 0x2f0 <g_strcmp+0x150>
    0x6b0b011f, // 0x148: cmp w8, w11
    0x540000a0, // 0x14c: b.eq 0x300 <g_strcmp+0x160>
    0x4b0b0100, // 0x150: sub w0, w8, w11
    0xd65f03c0, // 0x154: ret
    0xaa0103ea, // 0x158: mov x10, x1
    0xaa0003e9, // 0x15c: mov x9, x0
    0xf940012d, // 0x160: ldr x13, [x9]
    0xf940014b, // 0x164: ldr x11, [x10]
    0xeb0b01bf, // 0x168: cmp x13, x11
    0x12001da8, // 0x16c: and w8, w13, #0xff
    0x12001d6b, // 0x170: and w11, w11, #0xff
    0x54000321, // 0x174: b.ne 0x378 <g_strcmp+0x1d8>
    0xb200c3ec, // 0x178: mov x12, #0x101010101010101
    0xf280200c, // 0x17c: movk x12, #0x100
    0xcb0d018e, // 0x180: sub x14, x12, x13
    0xaa0d01ce, // 0x184: orr x14, x14, x13
    0xb201c3ed, // 0x188: mov x13, #-0x7f7f7f7f7f7f7f80
    0xea2e01bf, // 0x18c: bics xzr, x13, x14
    0x54000241, // 0x190: b.ne 0x378 <g_strcmp+0x1d8>
    0x9100214a, // 0x194: add x10, x10, #0x8
    0x9100212b, // 0x198: add x11, x9, #0x8
    0xf9400168, // 0x19c: ldr x8, [x11]
    0xf9400149, // 0x1a0: ldr x9, [x10]
    0xaa0a03e1, // 0x1a4: mov x1, x10
    0xaa0b03e0, // 0x1a8: mov x0, x11
    0xeb09011f, // 0x1ac: cmp x8, x9
    0x540000e1, // 0x1b0: b.ne 0x36c <g_strcmp+0x1cc>
    0xcb08018a, // 0x1b4: sub x10, x12, x8
    0x9100200b, // 0x1b8: add x11, x0, #0x8
    0xaa08014a, // 0x1bc: orr x10, x10, x8
    0xea2a01bf, // 0x1c0: bics xzr, x13, x10
    0x9100202a, // 0x1c4: add x10, x1, #0x8
    0x54fffea0, // 0x1c8: b.eq 0x33c <g_strcmp+0x19c>
    0x12001d08, // 0x1cc: and w8, w8, #0xff
    0x12001d2b, // 0x1d0: and w11, w9, #0xff
    0x17ffff90, // 0x1d4: b 0x1b4 <g_strcmp+0x14>
    0xaa0a03e1, // 0x1d8: mov x1, x10
    0xaa0903e0, // 0x1dc: mov x0, x9
    0x17ffff8d, // 0x1e0: b 0x1b4 <g_strcmp+0x14>
};

template<std::size_t N>
GuestAddr bounded_routine(const std::array<std::uint32_t, N>& body, GuestAddr host, unsigned limit, bool arm) {
  std::vector<std::uint32_t> code;
  if (arm) {
    code = {0xe3520000u | limit, 0x8a000000u | static_cast<unsigned>(N - 1)}; // cmp r2; bhi host
  } else {
    // Match the local-exclusive clearing performed by the former A64 SVC.
    code = {0xd5033f5fu, 0xf100005fu | (limit << 10), 0x54000008u | (static_cast<unsigned>(N + 1) << 5)};
  }
  code.insert(code.end(), body.begin(), body.end());
  const auto* thunk = g2h<const std::uint32_t>(host);
  code.insert(code.end(), thunk, thunk + guest_stub(arm ? GuestIsa::a32 : GuestIsa::a64, GuestStub::hle_call).count);
  return code_stub(code, "bounded guest libc routine");
}
} // namespace

void register_guest_strings(HleLibrary& lib) {
  if (process_config().isa() == GuestIsa::a32) {
    // Checked memory callbacks cost more than the in-JIT host handler even
    // for eight-byte copies. Select guest routines only with identity fastmem.
    if (!platform::a32_identity_fastmem_safe()) return;
    lib.add_function("memcpy", bounded_routine(kMemcpy32, lib.find_symbol("memcpy"), 8, true));
    lib.add_function("memcmp", bounded_routine(kMemcmp32, lib.find_symbol("memcmp"), 8, true));
    std::vector<std::uint32_t> code(kStrlen32.begin(), kStrlen32.end());
    code.back() = static_cast<std::uint32_t>(lib.find_symbol("strlen"));
    lib.add_function("strlen", code_stub(code, "bounded guest strlen") | 1u);
  } else {
    lib.add_function("memcpy", bounded_routine(kMemcpy64, lib.find_symbol("memcpy"), 128, false));
    // Staying below 256 also excludes memset's DC ZVA path for zero fills.
    lib.add_function("memset", bounded_routine(kMemset64, lib.find_symbol("memset"), 128, false));
    lib.add_function("memcmp", bounded_routine(kMemcmp64, lib.find_symbol("memcmp"), 32, false));
    // Bionic's <=96-byte memcpy path reads every source byte before writing,
    // so it also implements overlapping memmove. Larger copies retain host memmove.
    lib.add_function("memmove", bounded_routine(kMemcpy64, lib.find_symbol("memmove"), 96, false));
    std::vector<std::uint32_t> code{0xd5033f5fu}; // preserve A64 SVC's exclusive clearing
    code.insert(code.end(), kStrlen64.begin(), kStrlen64.end());
    // No strlen crossover was observed through 64 KiB: use the whole routine.
    lib.add_function("strlen", code_stub(code, "guest strlen"));
    // Word-at-a-time when both strings share 8-byte alignment, else bytewise.
    // Returns the difference of the first differing bytes, as bionic's does.
    code = {0xd5033f5fu};
    code.insert(code.end(), kStrcmp64.begin(), kStrcmp64.end());
    lib.add_function("strcmp", code_stub(code, "guest strcmp"));
  }
}

// After register_stdlib: the comparator runs as an ordinary guest call.
void register_guest_search(HleLibrary& lib) {
  if (process_config().isa() != GuestIsa::a64) return;
  std::vector<std::uint32_t> code{0xd5033f5fu};
  code.insert(code.end(), kBsearch64.begin(), kBsearch64.end());
  lib.add_function("bsearch", code_stub(code, "guest bsearch"));
  code = {0xd5033f5fu};
  code.insert(code.end(), kQsortR64.begin(), kQsortR64.end());
  lib.add_function("qsort_r", code_stub(code, "guest qsort_r"));
  code = {0xd5033f5fu, 0xaa1f03e4u}; // qsort: mov x4, xzr (no comparator argument)
  code.insert(code.end(), kQsortR64.begin(), kQsortR64.end());
  lib.add_function("qsort", code_stub(code, "guest qsort"));
}

// After register_libm. ldexp and scalbn share a signature; scalbln keeps the
// host handler.
void register_guest_math(HleLibrary& lib) {
  if (process_config().isa() != GuestIsa::a64) return;
  const auto routine = [](std::span<const std::uint32_t> body, const char* name) {
    std::vector<std::uint32_t> code{0xd5033f5fu};
    code.insert(code.end(), body.begin(), body.end());
    return code_stub(code, name);
  };
  const auto scalbn = routine(kScalbn64, "guest scalbn"), scalbnf = routine(kScalbnf64, "guest scalbnf");
  lib.add_function("ldexp", scalbn);
  lib.add_function("scalbn", scalbn);
  lib.add_function("ldexpf", scalbnf);
  lib.add_function("scalbnf", scalbnf);
}
} // namespace venture::hle::libc
