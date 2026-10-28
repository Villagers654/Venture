// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <optional>
#include <xmmintrin.h>

#include <catch2/catch_test_macros.hpp>

#include "tests/A32/testenv.h"
#include "tests/native/testenv.h"

using namespace Dynarmic;

class ArmSvcTestEnv : public ArmTestEnv {
public:
    std::optional<u32> svc_called = std::nullopt;
    void CallSVC(u32 swi) override {
        svc_called = swi;
    }
};

TEST_CASE("arm: svc", "[arm][A32]") {
    ArmSvcTestEnv test_env;
    A32::Jit jit{A32::UserConfig{&test_env}};
    test_env.code_mem = {
        0xef0001ee,  // svc #0x1ee
        0xe30a0071,  // mov r0, #41073
        0xeafffffe,  // b +#0
    };

    jit.SetCpsr(0x000001d0);  // User-mode

    test_env.ticks_left = 3;
    CheckedRun([&]() { jit.Run(); });

    REQUIRE(test_env.svc_called == 0x1ee);
    REQUIRE(jit.Regs()[15] == 0x00000008);
    REQUIRE(jit.Regs()[0] == 41073);
}

TEST_CASE("arm/thumb: optional frame chain preserves interworking and conditional callback state", "[arm][thumb][A32]") {
    class StopEnv final : public ArmSvcTestEnv {
    public:
        A32::Jit* jit = nullptr;
        void CallSVC(u32 swi) override {
            ArmSvcTestEnv::CallSVC(swi);
            // Host callback arithmetic may set sticky flags. They must not
            // enter the guest FPSCR when the JIT resumes or halts.
            _mm_setcsr(_mm_getcsr() | 0x04); // masked host divide-by-zero status
            jit->HaltExecution(HaltReason::UserDefined1);
        }
    };
    // Fixed program, also comparing the existing A32 IR passes on/off. No
    // generated instruction stream: the oracle is its explicit architectural state.
    for (bool chain : {false, true}) {
        for (auto opts : {no_optimizations, all_safe_optimizations}) {
          for (unsigned mode = 0; mode != 4; ++mode) {
            StopEnv env;
            A32::UserConfig config{&env};
            config.enable_block_frame_chain = chain;
            config.optimizations = opts;
            config.enable_cycle_counting = false;
            config.check_halt_on_memory_access = (mode & 1) != 0;
            A32::Jit jit{config};
            const u32 fpscr = (mode >> 1) << 26; // AHP varies independently of halt checks.
            jit.SetFpscr(fpscr);
            jit.ExtRegs()[6] = 0x12345678;
            env.jit = &jit;
            env.code_mem = {
                0xe3a03021, // mov r3,#0x21
                0xe12fff33, // blx r3
                0xe350002a, // cmp r0,#42
                0x13a01063, // movne r1,#99
                0x03a01007, // moveq r1,#7
                0xe5821000, // str r1,[r2]
                0xef0001ee, // svc #0x1ee
                0xeafffffe, // b .
                0x47703001, // Thumb adds r0,#1; bx lr
            };
            for (unsigned iteration = 0; iteration < 3; ++iteration) {
                jit.Regs().fill(0);
                jit.Regs()[0] = 41;
                jit.Regs()[2] = 0x100;
                jit.SetCpsr(0x10);
                const auto host_mxcsr = _mm_getcsr();
                _mm_setcsr(host_mxcsr | 0x20); // independent host inexact status
                CheckedRun([&] { jit.Run(); });
                const auto returned_mxcsr = _mm_getcsr();
                _mm_setcsr(host_mxcsr);
                CHECK((returned_mxcsr & 0x24) == 0x24);
                CHECK(env.svc_called == 0x1ee);
                CHECK(jit.Regs()[0] == 42);
                CHECK(jit.Regs()[1] == 7);
                CHECK(jit.Regs()[14] == 8);
                CHECK(jit.Regs()[15] == 28);
                CHECK((jit.Cpsr() & 0xf0000020) == 0x60000000);
                CHECK(jit.Fpscr() == fpscr);
                CHECK(jit.ExtRegs()[6] == 0x12345678);
                CHECK(env.MemoryRead(0x100, 4) == 7);
                jit.ClearHalt(HaltReason::UserDefined1);
            }
            env.code_mem[8] = 0x47703002; // Changed Thumb callee; retire linked/RSB targets.
            jit.InvalidateCacheRange(32, 4);
            jit.Regs()[0] = 40;
            jit.Regs()[15] = 0;
            CheckedRun([&] { jit.Run(); });
            CHECK(jit.Regs()[0] == 42);
            CHECK(jit.Regs()[1] == 7);
            CHECK(jit.Regs()[15] == 28);
          }
        }
    }
}
