// SPDX-FileCopyrightText: Copyright 2026 Venture contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// Venture: background translation compiles guest code the guest may never run
// (cpu/pretranslate.cpp), so compiling any instruction word must not stop the
// process; an encoding the JIT does not implement raises an exception when run.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <random>

#include <catch2/catch_test_macros.hpp>

#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

namespace {

class CodeEnv final : public Dynarmic::A64::UserCallbacks {
public:
    std::uint32_t words[4]{};
    std::optional<std::uint32_t> MemoryReadCode(std::uint64_t vaddr) override { return words[(vaddr >> 2) & 3]; }
    std::uint64_t MemoryRead(std::uint64_t, std::size_t) override { return 0; }
    Dynarmic::A64::Vector MemoryRead128(std::uint64_t) override { return {}; }
    void MemoryWrite(std::uint64_t, std::uint64_t, std::size_t) override {}
    void MemoryWrite128(std::uint64_t, Dynarmic::A64::Vector) override {}
    bool MemoryWriteExclusive8(std::uint64_t, std::uint8_t, std::uint8_t) override { return true; }
    bool MemoryWriteExclusive16(std::uint64_t, std::uint16_t, std::uint16_t) override { return true; }
    bool MemoryWriteExclusive32(std::uint64_t, std::uint32_t, std::uint32_t) override { return true; }
    bool MemoryWriteExclusive64(std::uint64_t, std::uint64_t, std::uint64_t) override { return true; }
    bool MemoryWriteExclusive128(std::uint64_t, Dynarmic::A64::Vector, Dynarmic::A64::Vector) override { return true; }
    std::uint64_t MemoryAtomic(std::uint64_t, std::size_t, Dynarmic::A64::AtomicOp, std::uint64_t, std::uint64_t) override { return 0; }
    Dynarmic::A64::Vector MemoryAtomicCompareAndSwap128(std::uint64_t, Dynarmic::A64::Vector, Dynarmic::A64::Vector) override { return {}; }
    void CallSVC(std::uint32_t) override {}
    void ExceptionRaised(std::uint64_t, Dynarmic::A64::Exception) override {}
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 0; }
    std::uint64_t GetCNTPCT() override { return 0; }
};

// Compiles a block that starts with `word` (then B .) at a fresh address.
struct Compiler {
    CodeEnv env;
    Dynarmic::ExclusiveMonitor monitor{1};
    Dynarmic::A64::UserConfig config = [this] {
        Dynarmic::A64::UserConfig c{};
        c.callbacks = &env;
        c.global_monitor = &monitor;
        c.enable_cycle_counting = false;
        return c;
    }();
    Dynarmic::A64::Jit jit{config};
    std::uint64_t pc = 0x10000;
    std::size_t count = 0, thrown = 0;
    void compile(std::uint32_t word) {
        env.words[(pc >> 2) & 3] = word;
        env.words[((pc >> 2) + 1) & 3] = 0x14000000;  // B .
        // The descriptor of PC `pc` with the default FPCR, as the guest's first blocks use.
        // A failed emission throws, which callers handle (DynarmicCpu::pretranslate); only
        // terminating the process is a failure.
        try {
            if (!jit.Precompile(pc))
                jit.ClearCache();
        } catch (...) {
            ++thrown;
        }
        pc += 8;
        if (++count % 4096 == 0)
            jit.ClearCache();
    }
};

}  // namespace

TEST_CASE("A64: every MRS and MSR encoding compiles", "[a64]") {
    Compiler compiler;
    // MRS/MSR (register): 1101 0101 001L 1o0 op1 CRn CRm op2 Rt.
    for (std::uint32_t read = 0; read < 2; ++read) {
        for (std::uint32_t sysreg = 0; sysreg < (1u << 15); ++sysreg)
            compiler.compile(0xd5100000u | read << 21 | sysreg << 5 | 9);
    }
}

// Not part of the default run: a search for further encodings that stop compilation.
// VENTURE_TEST_WORDS and VENTURE_TEST_SEED widen the search (about 0.6 M words per second).
TEST_CASE("A64: random instruction words compile", "[.][a64-random]") {
    const char* words = std::getenv("VENTURE_TEST_WORDS");
    const char* seed = std::getenv("VENTURE_TEST_SEED");
    const unsigned long long count = words ? std::strtoull(words, nullptr, 10) : 2'000'000;
    Compiler compiler;
    std::mt19937 random{seed ? static_cast<std::uint32_t>(std::strtoul(seed, nullptr, 10)) : 12345u};
    for (unsigned long long i = 0; i < count; ++i)
        compiler.compile(random());
}
