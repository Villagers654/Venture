// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Venture's extensions of the arm64-host backend: LSE atomics (fastmem and callbacks),
// fast supervisor calls, the host FP environment around callbacks and precise memory
// aborts on the callback-only memory paths.

#include <array>
#include <cstring>
#include <functional>
#include <optional>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "dynarmic/backend/arm64/a32_jitstate.h"
#include "dynarmic/backend/arm64/a64_jitstate.h"
#include "dynarmic/backend/arm64/host_feature.h"
#include "dynarmic/backend/arm64/stack_layout.h"
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

using namespace Dynarmic;

namespace {

u64 HostFpcr() {
    u64 value;
    asm volatile("mrs %0, fpcr" : "=r"(value));
    return value;
}
u64 HostFpsr() {
    u64 value;
    asm volatile("mrs %0, fpsr" : "=r"(value));
    return value;
}
void SetHostFpcr(u64 value) { asm volatile("msr fpcr, %0" : : "r"(value)); }
void SetHostFpsr(u64 value) { asm volatile("msr fpsr, %0" : : "r"(value)); }

// Restores the test thread's FP environment.
struct HostFpGuard {
    u64 fpcr = HostFpcr(), fpsr = HostFpsr();
    ~HostFpGuard() {
        SetHostFpcr(fpcr);
        SetHostFpsr(fpsr);
    }
};

// fdiv under `fpcr`, as hardware computes it.
u64 Divide(double a, double b, u64 fpcr) {
    HostFpGuard guard;
    SetHostFpcr(fpcr);
    double result;
    asm volatile("fdiv %d0, %d1, %d2" : "=w"(result) : "w"(a), "w"(b));
    u64 bits;
    std::memcpy(&bits, &result, sizeof(bits));
    return bits;
}

constexpr u32 fpcr_round_toward_zero = 0b11 << 22;
constexpr u32 fpcr_flush_to_zero = 1 << 24;
constexpr u32 fpsr_inexact = 1 << 4;

// Encodings.
constexpr u32 Svc(u32 imm) { return 0xd4000001 | imm << 5; }
constexpr u32 Lse(u32 size, bool acquire, bool release, u32 rs, u32 op, u32 rn, u32 rt) {
    return 0x38200000 | size << 30 | u32(acquire) << 23 | u32(release) << 22 | rs << 16 | (op >> 3) << 15 | (op & 7) << 12 | rn << 5 | rt;
}
constexpr u32 Cas(u32 size, bool acquire, bool release, u32 rs, u32 rn, u32 rt) {
    return 0x08a07c00 | size << 30 | u32(acquire) << 22 | rs << 16 | u32(release) << 15 | rn << 5 | rt;
}
constexpr u32 Casp(bool sz, bool acquire, bool release, u32 rs, u32 rn, u32 rt) {
    return 0x08207c00 | u32(sz) << 30 | u32(acquire) << 22 | rs << 16 | u32(release) << 15 | rn << 5 | rt;
}
static_assert(Lse(0, true, true, 1, 0, 0, 2) == 0x38e10002);  // ldaddalb w1, w2, [x0]
static_assert(Lse(3, true, false, 1, 8, 0, 2) == 0xf8a18002);  // swpa x1, x2, [x0]
static_assert(Cas(0, true, true, 1, 0, 2) == 0x08e1fc02);      // casalb w1, w2, [x0]
static_assert(Casp(true, true, true, 2, 0, 4) == 0x4862fc04);  // caspal x2, x3, x4, x5, [x0]

constexpr u32 stop_svc = 0xff;
constexpr u32 STOP = Svc(stop_svc);
constexpr u32 ADD_X1_X0_1 = 0x91000401;
constexpr u32 ADD_X1_X4_1 = 0x91000481;
constexpr u32 ADD_X1_X1_1 = 0x91000421;
constexpr u32 MOVZ_X5_7 = 0xd28000e5;
constexpr u32 FDIV_D2_D0_D1 = 0x1e611802;
constexpr u32 FDIV_D3_D0_D1 = 0x1e611803;
constexpr u32 LDR_X2_X3 = 0xf9400062;
constexpr u32 STR_X1_X3 = 0xf9000061;
constexpr u32 LDXR_X2_X3 = 0xc85f7c62;
constexpr u32 LDADD_X1_X2_X3 = 0xf8210062;
constexpr std::array<u32, 4> LDAXR_W2_X0{0x085ffc02, 0x485ffc02, 0x885ffc02, 0xc85ffc02};  // by size: b, h, w, x
constexpr std::array<u32, 4> STLXR_W4_W2_X0{0x0804fc02, 0x4804fc02, 0x8804fc02, 0xc804fc02};
constexpr u32 ADD_X2_X2_1 = 0x91000442;
constexpr u32 LDXR_X2_X0 = 0xc85f7c02;
constexpr u32 STXR_W4_X2_X0 = 0xc8047c02;
constexpr u32 LDXP_X2_X3_X0 = 0xc87f0c02;
constexpr u32 STXP_W4_X5_X6_X0 = 0xc8241805;
constexpr u32 CLREX = 0xd5033f5f;

// Guest memory: code page, two data pages and an inaccessible page whose contents the
// callbacks keep (fastmem faults there).
struct Arena {
    const std::size_t page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    const u64 data = page;
    const u64 device = 3 * page;
    u8* base = nullptr;

    Arena() {
        base = static_cast<u8*>(mmap(nullptr, 4 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        REQUIRE(base != MAP_FAILED);
        REQUIRE(mprotect(base + device, page, PROT_NONE) == 0);
    }
    ~Arena() { munmap(base, 4 * page); }
};

u64 Apply(A64::AtomicOp op, std::size_t bits, u64 old, u64 value, u64 desired) {
    const u64 mask = bits == 64 ? ~u64(0) : (u64(1) << bits) - 1;
    old &= mask;
    value &= mask;
    const auto sext = [bits](u64 v) { return bits == 64 ? s64(v) : s64(v << (64 - bits)) >> (64 - bits); };
    using Op = A64::AtomicOp;
    switch (op) {
    case Op::Add: return (old + value) & mask;
    case Op::Clear: return old & ~value;
    case Op::Eor: return old ^ value;
    case Op::Set: return old | value;
    case Op::SMax: return sext(old) > sext(value) ? old : value;
    case Op::SMin: return sext(old) < sext(value) ? old : value;
    case Op::UMax: return old > value ? old : value;
    case Op::UMin: return old < value ? old : value;
    case Op::Swap: return value;
    case Op::CompareAndSwap: return old == value ? desired & mask : old;
    }
    return old;
}

class A64Env final : public A64::UserCallbacks {
public:
    explicit A64Env(Arena& arena) : arena(arena), device(arena.page) {}

    Arena& arena;
    std::vector<u8> device;
    A64::Jit* jit = nullptr;

    std::size_t atomic_calls = 0, cas128_calls = 0, svc_calls = 0, fast_calls = 0, known_calls = 0, memory_calls = 0;
    std::optional<u64> abort_address;  // a callback access here raises a memory abort
    std::function<void(u32)> on_svc, on_fast;
    std::function<void(u32, const void*, void*)> on_known;
    std::function<void()> on_memory;
    std::vector<u64> exceptions;

    void Load(const std::vector<u32>& code, u64 at = 0) {
        std::memcpy(arena.base + at, code.data(), code.size() * sizeof(u32));
        if (jit) {
            jit->ClearCache();
        }
    }

    u8* Host(u64 vaddr) {
        if (vaddr >= arena.device && vaddr < arena.device + arena.page) {
            return device.data() + (vaddr - arena.device);
        }
        return arena.base + vaddr;
    }
    bool Abort(u64 vaddr) {
        ++memory_calls;
        if (on_memory) {
            on_memory();
        }
        if (abort_address && vaddr == *abort_address) {
            jit->HaltExecution(HaltReason::MemoryAbort);
            return true;
        }
        return false;
    }
    template<typename T>
    bool WriteExclusive(u64 vaddr, T value, T expected) {
        if (Abort(vaddr)) {
            return false;
        }
        T current;
        std::memcpy(&current, Host(vaddr), sizeof(T));
        if (std::memcmp(&current, &expected, sizeof(T)) != 0) {
            return false;
        }
        std::memcpy(Host(vaddr), &value, sizeof(T));
        return true;
    }

    std::optional<u32> MemoryReadCode(u64 vaddr) override {
        u32 value;
        std::memcpy(&value, arena.base + vaddr, sizeof(value));
        return value;
    }
    u64 MemoryRead(u64 vaddr, std::size_t size) override {
        u64 value = 0;
        if (!Abort(vaddr)) {
            std::memcpy(&value, Host(vaddr), size);
        }
        return value;
    }
    Vector MemoryRead128(u64 vaddr) override {
        Vector value{};
        if (!Abort(vaddr)) {
            std::memcpy(&value, Host(vaddr), sizeof(value));
        }
        return value;
    }
    void MemoryWrite(u64 vaddr, u64 value, std::size_t size) override {
        if (!Abort(vaddr)) {
            std::memcpy(Host(vaddr), &value, size);
        }
    }
    void MemoryWrite128(u64 vaddr, Vector value) override {
        if (!Abort(vaddr)) {
            std::memcpy(Host(vaddr), &value, sizeof(value));
        }
    }
    bool MemoryWriteExclusive8(u64 vaddr, u8 value, u8 expected) override { return WriteExclusive(vaddr, value, expected); }
    bool MemoryWriteExclusive16(u64 vaddr, u16 value, u16 expected) override { return WriteExclusive(vaddr, value, expected); }
    bool MemoryWriteExclusive32(u64 vaddr, u32 value, u32 expected) override { return WriteExclusive(vaddr, value, expected); }
    bool MemoryWriteExclusive64(u64 vaddr, u64 value, u64 expected) override { return WriteExclusive(vaddr, value, expected); }
    bool MemoryWriteExclusive128(u64 vaddr, Vector value, Vector expected) override { return WriteExclusive(vaddr, value, expected); }

    u64 MemoryAtomic(u64 vaddr, std::size_t bytes, A64::AtomicOp op, u64 value, u64 desired) override {
        ++atomic_calls;
        if (Abort(vaddr)) {
            return 0;
        }
        u64 old = 0;
        std::memcpy(&old, Host(vaddr), bytes);
        const u64 result = Apply(op, bytes * 8, old, value, desired);
        std::memcpy(Host(vaddr), &result, bytes);
        return old;
    }
    Vector MemoryAtomicCompareAndSwap128(u64 vaddr, Vector expected, Vector desired) override {
        ++cas128_calls;
        Vector old{};
        if (Abort(vaddr)) {
            return old;
        }
        std::memcpy(&old, Host(vaddr), sizeof(old));
        if (old == expected) {
            std::memcpy(Host(vaddr), &desired, sizeof(desired));
        }
        return old;
    }

    void CallSVC(u32 swi) override {
        ++svc_calls;
        if (on_svc) {
            on_svc(swi);
        }
        if (swi == stop_svc) {
            jit->HaltExecution(HaltReason::UserDefined2);
        }
    }
    void CallSVCFast(u32 swi) override {
        ++fast_calls;
        if (on_fast) {
            on_fast(swi);
        }
    }
    void CallSVCFastKnown(u32 swi, const void* metadata, void* state) override {
        ++known_calls;
        if (on_known) {
            on_known(swi, metadata, state);
        }
    }
    void ExceptionRaised(u64 pc, A64::Exception) override {
        exceptions.push_back(pc);
        jit->HaltExecution(HaltReason::UserDefined3);
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1'000'000; }
    u64 GetCNTPCT() override { return 0; }
};

A64::UserConfig MakeConfig(A64Env& env, bool fastmem) {
    A64::UserConfig conf{};
    conf.callbacks = &env;
    conf.enable_cycle_counting = false;
    conf.code_cache_size = 16 * 1024 * 1024;
    conf.optimizations |= OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
    conf.unsafe_optimizations = true;
    if (fastmem) {
        conf.fastmem_pointer = reinterpret_cast<std::uintptr_t>(env.arena.base);
        conf.fastmem_address_space_bits = 64;
        conf.silently_mirror_fastmem = false;
        conf.recompile_on_fastmem_failure = true;
    }
    return conf;
}

HaltReason RunFrom(A64::Jit& jit, u64 pc) {
    jit.SetPC(pc);
    return jit.Run();
}

constexpr std::array<u64, 2> atomic_values{0xF0E1D2C3B4A59687, 0x0123456789ABCDEF};

bool HostHasLse(u32 disabled) {
    return Backend::Arm64::HasHostFeature(Backend::Arm64::HostFeatures(disabled), Backend::Arm64::HostFeature::LSE);
}

// Whether the code cache holds a host LSE read-modify-write, SWP or CAS on
// Xscratch2 (X30), the inline atomics' address register.
bool CodeCacheHasLse(A64::Jit& jit) {
    const auto* words = static_cast<const u32*>(jit.CodeCacheBegin());
    for (std::size_t i = 0; i < jit.CodeCacheSize() / sizeof(u32); i++) {
        const u32 w = words[i];
        if ((w & 0x3f200fe0) == (0x38200000 | 30 << 5) || (w & 0x3fa07fe0) == (0x08a07c00 | 30 << 5) ||
            (w & 0xbfa07fe0) == (0x08207c00 | 30 << 5)) {
            return true;
        }
    }
    return false;
}

A64::UserConfig MakeConfig(A64Env& env, bool fastmem, u32 disabled_host_features) {
    auto conf = MakeConfig(env, fastmem);
    conf.disabled_host_features = disabled_host_features;
    return conf;
}

void CheckAtomics(bool fastmem, u32 disabled_host_features = 0) {
    Arena arena;
    A64Env env{arena};
    A64::Jit jit{MakeConfig(env, fastmem, disabled_host_features)};
    env.jit = &jit;
    // Venture: with FEAT_LSE each inline atomic is one host LSE instruction.
    const bool lse = fastmem && HostHasLse(disabled_host_features);

    for (u32 size = 0; size < 4; ++size) {
        const std::size_t bits = 8u << size;
        const u64 mask = bits == 64 ? ~u64(0) : (u64(1) << bits) - 1;
        for (u32 op = 0; op <= 8; ++op) {
            for (u32 order = 0; order < 4; ++order) {
                for (std::size_t swap = 0; swap < 2; ++swap) {
                    const bool acquire = order & 1, release = order & 2;
                    const u64 old = atomic_values[swap], value = atomic_values[1 - swap];
                    const u64 address = arena.data + 8 * (op + 1);
                    std::memcpy(arena.base + address, &old, sizeof(old));
                    env.Load({Lse(size, acquire, release, 1, op, 0, 2), STOP});
                    jit.SetRegister(0, address);
                    jit.SetRegister(1, value);
                    jit.SetRegister(2, 0xdeadbeefdeadbeef);
                    const std::size_t calls = env.atomic_calls;

                    INFO("size " << bits << " op " << op << " order " << order << " swap " << swap);
                    REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
                    REQUIRE(jit.GetRegister(2) == (old & mask));
                    u64 memory;
                    std::memcpy(&memory, arena.base + address, sizeof(memory));
                    const u64 expected = (old & ~mask) | Apply(static_cast<A64::AtomicOp>(op), bits, old, value, 0);
                    REQUIRE(memory == expected);
                    REQUIRE(env.atomic_calls == calls + (fastmem ? 0 : 1));
                    REQUIRE(CodeCacheHasLse(jit) == lse);
                }
            }
        }
    }
}

void CheckCompareAndSwap(bool fastmem, u32 disabled_host_features = 0) {
    Arena arena;
    A64Env env{arena};
    A64::Jit jit{MakeConfig(env, fastmem, disabled_host_features)};
    env.jit = &jit;
    const bool lse = fastmem && HostHasLse(disabled_host_features);
    const u64 address = arena.data + 0x40;

    for (u32 size = 0; size < 4; ++size) {
        const std::size_t bits = 8u << size;
        const u64 mask = bits == 64 ? ~u64(0) : (u64(1) << bits) - 1;
        for (u32 order = 0; order < 4; ++order) {
            for (const bool match : {true, false}) {
                const u64 old = 0x8877665544332211, desired = 0x0102030405060708;
                const u64 expected = match ? old : old ^ 1;
                std::memcpy(arena.base + address, &old, sizeof(old));
                env.Load({Cas(size, order & 1, order & 2, 1, 0, 2), STOP});
                jit.SetRegister(0, address);
                jit.SetRegister(1, expected | (bits == 64 ? 0 : 0xabcd'0000'0000'0000));
                jit.SetRegister(2, desired);
                const std::size_t calls = env.atomic_calls;

                INFO("size " << bits << " order " << order << " match " << match);
                REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
                REQUIRE(jit.GetRegister(1) == (old & mask));
                u64 memory;
                std::memcpy(&memory, arena.base + address, sizeof(memory));
                REQUIRE(memory == (match ? (old & ~mask) | (desired & mask) : old));
                REQUIRE(env.atomic_calls == calls + (fastmem ? 0 : 1));
                REQUIRE(CodeCacheHasLse(jit) == lse);
            }
        }
    }

    // CASP: X2:X3 expected and old, X4:X5 desired.
    for (const bool sz : {false, true}) {
        for (const bool match : {true, false}) {
            const std::array<u64, 2> old{0x1111222233334444, 0x5555666677778888};
            const std::array<u64, 2> desired{0x9999aaaabbbbcccc, 0xddddeeeeffff0000};
            std::memcpy(arena.base + address, old.data(), sizeof(old));
            env.Load({Casp(sz, true, true, 2, 0, 4), STOP});
            jit.SetRegister(0, address);
            if (sz) {
                jit.SetRegister(2, old[0] ^ (match ? 0 : 1));
                jit.SetRegister(3, old[1]);
                jit.SetRegister(4, desired[0]);
                jit.SetRegister(5, desired[1]);
            } else {
                jit.SetRegister(2, u32(old[0]) ^ (match ? 0 : 1));
                jit.SetRegister(3, u32(old[0] >> 32));
                jit.SetRegister(4, u32(desired[0]));
                jit.SetRegister(5, u32(desired[0] >> 32));
            }
            const std::size_t calls = env.atomic_calls + env.cas128_calls;

            INFO("casp sz " << sz << " match " << match);
            REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
            std::array<u64, 2> memory;
            std::memcpy(memory.data(), arena.base + address, sizeof(memory));
            if (sz) {
                REQUIRE(jit.GetRegister(2) == old[0]);
                REQUIRE(jit.GetRegister(3) == old[1]);
                REQUIRE(memory == (match ? desired : old));
            } else {
                REQUIRE(jit.GetRegister(2) == u32(old[0]));
                REQUIRE(jit.GetRegister(3) == u32(old[0] >> 32));
                REQUIRE(memory[0] == (match ? desired[0] : old[0]));
                REQUIRE(memory[1] == old[1]);
            }
            // Venture: a 64-bit CASP pair is inline on fastmem; a 128-bit one
            // is a host CASPAL with FEAT_LSE and calls
            // MemoryAtomicCompareAndSwap128 without it.
            REQUIRE(env.atomic_calls + env.cas128_calls == calls + (fastmem && (!sz || lse) ? 0 : 1));
            REQUIRE(CodeCacheHasLse(jit) == lse);
        }
    }
}

}  // namespace

TEST_CASE("arm64: LSE atomics through the MemoryAtomic callback", "[a64][arm64][atomics]") {
    CheckAtomics(false);
}

TEST_CASE("arm64: LSE atomics on fastmem", "[a64][arm64][atomics]") {
    CheckAtomics(true);
}

TEST_CASE("arm64: CAS and CASP through the callbacks", "[a64][arm64][atomics]") {
    CheckCompareAndSwap(false);
}

TEST_CASE("arm64: LSE atomics on fastmem without FEAT_LSE", "[a64][arm64][atomics]") {
    CheckAtomics(true, static_cast<u32>(Backend::Arm64::HostFeature::LSE));
}

TEST_CASE("arm64: CAS and CASP on fastmem without FEAT_LSE", "[a64][arm64][atomics]") {
    CheckCompareAndSwap(true, static_cast<u32>(Backend::Arm64::HostFeature::LSE));
}

TEST_CASE("arm64: CAS and CASP on fastmem", "[a64][arm64][atomics]") {
    CheckCompareAndSwap(true);
}

TEST_CASE("arm64: fastmem atomics fall back on faults and misalignment", "[a64][arm64][atomics]") {
    Arena arena;
    A64Env env{arena};
    A64::Jit jit{MakeConfig(env, true)};
    env.jit = &jit;

    // A fault at the LSE instruction: the callback completes it, the recompiled block calls it directly.
    const u64 device_address = arena.device + 0x10;
    u64 old = 40;
    std::memcpy(env.Host(device_address), &old, sizeof(old));
    env.Load({Lse(3, true, true, 1, 0, 0, 2), STOP});
    for (int run = 0; run < 2; ++run) {
        jit.SetRegister(0, device_address);
        jit.SetRegister(1, 2);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(jit.GetRegister(2) == old);
        old += 2;
        u64 memory;
        std::memcpy(&memory, env.Host(device_address), sizeof(memory));
        REQUIRE(memory == old);
        REQUIRE(env.atomic_calls == std::size_t(run + 1));
    }

    // CASP faulting.
    const std::array<u64, 2> pair_old{1, 2}, pair_new{3, 4};
    std::memcpy(env.Host(device_address), pair_old.data(), sizeof(pair_old));
    env.Load({Casp(true, false, false, 2, 0, 4), STOP});
    jit.SetRegister(0, device_address);
    jit.SetRegister(2, 1);
    jit.SetRegister(3, 2);
    jit.SetRegister(4, 3);
    jit.SetRegister(5, 4);
    REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
    REQUIRE(env.cas128_calls == 1);
    REQUIRE(jit.GetRegister(2) == 1);
    REQUIRE(jit.GetRegister(3) == 2);
    std::array<u64, 2> pair_memory;
    std::memcpy(pair_memory.data(), env.Host(device_address), sizeof(pair_memory));
    REQUIRE(pair_memory == pair_new);

    // Misaligned: LSE requires natural alignment, the callback does not.
    const u64 misaligned = arena.data + 2;
    const u32 word = 0x10000000;
    std::memcpy(arena.base + misaligned, &word, sizeof(word));
    env.Load({Lse(2, false, false, 1, 0, 0, 2), STOP});
    jit.SetRegister(0, misaligned);
    jit.SetRegister(1, 5);
    const std::size_t calls = env.atomic_calls;
    REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
    REQUIRE(env.atomic_calls == calls + 1);
    REQUIRE(jit.GetRegister(2) == word);
    u32 memory;
    std::memcpy(&memory, arena.base + misaligned, sizeof(memory));
    REQUIRE(memory == word + 5);
}

TEST_CASE("arm64: fast supervisor calls stay in Run", "[a64][arm64][svc]") {
    Arena arena;
    A64Env env{arena};
    static constexpr int token = 0;
    auto conf = MakeConfig(env, false);
    conf.svc_fast = 0x10;
    conf.svc_lookup = [](u64 pc, u32 imm) -> const void* { return pc == 0x40 && imm == 0x10 ? &token : nullptr; };
    A64::Jit jit{conf};
    env.jit = &jit;

    SECTION("register round trip through the Jit interface") {
        env.on_fast = [&](u32 swi) {
            REQUIRE(swi == 0x10);
            jit.SetRegister(0, jit.GetRegister(0) * 2);
        };
        env.Load({Svc(0x10), ADD_X1_X0_1, STOP});
        jit.SetRegister(0, 21);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(env.fast_calls == 1);
        REQUIRE(env.svc_calls == 1);  // the stop
        REQUIRE(jit.GetRegister(0) == 42);
        REQUIRE(jit.GetRegister(1) == 43);
        REQUIRE(jit.GetPC() == 12);
    }

    SECTION("known thunk gets its metadata and the JitState") {
        env.on_known = [&](u32 swi, const void* metadata, void* state) {
            REQUIRE(swi == 0x10);
            REQUIRE(metadata == &token);
            auto& registers = static_cast<Backend::Arm64::A64JitState*>(state)->reg;
            registers[0] += 5;
        };
        // The lookup names a thunk by its block's PC.
        env.Load({Svc(0x10), ADD_X1_X0_1, STOP}, 0x40);
        jit.SetRegister(0, 1);
        REQUIRE(RunFrom(jit, 0x40) == HaltReason::UserDefined2);
        REQUIRE(env.fast_calls == 0);
        REQUIRE(env.known_calls == 1);
        REQUIRE(jit.GetRegister(0) == 6);
        REQUIRE(jit.GetRegister(1) == 7);
    }

    SECTION("a halt inside the callback stops after the call") {
        env.on_fast = [&](u32) { jit.HaltExecution(HaltReason::UserDefined1); };
        env.Load({Svc(0x10), ADD_X1_X0_1, STOP});
        jit.SetRegister(0, 1);
        jit.SetRegister(1, 99);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined1);
        REQUIRE(env.fast_calls == 1);
        REQUIRE(jit.GetRegister(1) == 99);
        REQUIRE(jit.GetPC() == 4);
    }

    SECTION("a callback may redirect the PC") {
        env.on_fast = [&](u32) { jit.SetPC(8); };
        env.Load({Svc(0x10), ADD_X1_X0_1, STOP});
        jit.SetRegister(1, 99);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(jit.GetRegister(1) == 99);
    }
}

// Venture: as on x64, callbacks run in the guest FP environment except the
// svc_fp_transfer call, which runs in the host's. The host FPSR is not part of
// the contract (tests/A32/test_svc.cpp: host sticky flags pass through Run).
TEST_CASE("arm64: svc_fp_transfer runs in the host FP environment", "[a64][arm64][fp]") {
    HostFpGuard guard;
    Arena arena;
    A64Env env{arena};
    auto conf = MakeConfig(env, false);
    conf.svc_fp_transfer = 0x12;
    A64::Jit jit{conf};
    env.jit = &jit;

    const double a = 1.0, b = 10.0;
    const u64 guest_result = Divide(a, b, fpcr_round_toward_zero);
    const u64 host_result = Divide(a, b, fpcr_flush_to_zero);
    REQUIRE(guest_result != host_result);

    for (const u32 imm : {0x12u}) {
        INFO("svc " << imm);
        std::optional<u64> fpcr_inside, fpsr_inside, divide_inside;
        std::optional<u32> guest_fpsr_inside;
        const auto record = [&](u32 swi) {
            if (swi != imm) {
                return;
            }
            fpcr_inside = HostFpcr();
            fpsr_inside = HostFpsr();
            guest_fpsr_inside = jit.GetFpsr();
            volatile double x = a, y = b;
            const double r = x / y;
            std::memcpy(&divide_inside.emplace(), &r, sizeof(r));
        };
        env.on_svc = record;
        env.on_fast = record;
        env.Load({FDIV_D2_D0_D1, Svc(imm), FDIV_D3_D0_D1, STOP});
        jit.SetVector(0, {std::bit_cast<u64>(a), 0});
        jit.SetVector(1, {std::bit_cast<u64>(b), 0});
        jit.SetFpcr(fpcr_round_toward_zero);
        // The first Run services the cache clear, whose host code may raise host flags.
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        jit.SetFpsr(0);

        SetHostFpcr(fpcr_flush_to_zero);
        SetHostFpsr(0);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        const u64 host_fpcr_after = HostFpcr(), host_fpsr_after = HostFpsr();

        REQUIRE(fpcr_inside == fpcr_flush_to_zero);
        REQUIRE(guest_fpsr_inside.has_value());
        REQUIRE((*guest_fpsr_inside & fpsr_inexact) != 0);  // the first FDIV's flag, merged
        REQUIRE(divide_inside == host_result);
        REQUIRE(jit.GetVector(2)[0] == guest_result);
        REQUIRE(jit.GetVector(3)[0] == guest_result);  // guest rounding mode reinstalled
        REQUIRE((jit.GetFpsr() & fpsr_inexact) != 0);
        REQUIRE(host_fpcr_after == fpcr_flush_to_zero);
    }

    SECTION("a callback's SetFpcr applies after it") {
        env.on_svc = [&](u32 swi) {
            if (swi == 0x20) {
                jit.SetFpcr(0);
            }
        };
        env.Load({FDIV_D2_D0_D1, Svc(0x20), FDIV_D3_D0_D1, STOP});
        jit.SetFpcr(fpcr_round_toward_zero);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(jit.GetVector(2)[0] == guest_result);
        REQUIRE(jit.GetVector(3)[0] == Divide(a, b, 0));
    }

}

TEST_CASE("arm64: svc_fast and svc_fp_guest keep the guest FP environment", "[a64][arm64][fp]") {
    HostFpGuard guard;
    Arena arena;
    A64Env env{arena};
    auto conf = MakeConfig(env, false);
    conf.svc_fast = 0x10;
    conf.svc_fp_guest = 0x13;
    A64::Jit jit{conf};
    env.jit = &jit;

    for (const u32 imm : {0x10u, 0x13u}) {
        INFO("svc " << imm);
        std::optional<u64> fpcr_inside;
        env.on_fast = [&](u32) {
            fpcr_inside = HostFpcr();
            // Host math in the callback rounds in the guest mode; its flags are the guest's.
            volatile double x = 2.0, y = 3.0;
            volatile double r = x / y;
            (void)r;
        };
        env.Load({Svc(imm), STOP});
        jit.SetFpcr(fpcr_round_toward_zero);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        jit.SetFpsr(0);
        SetHostFpcr(0);
        SetHostFpsr(0);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        const u64 host_fpcr_after = HostFpcr(), host_fpsr_after = HostFpsr();
        REQUIRE(fpcr_inside == fpcr_round_toward_zero);
        REQUIRE((jit.GetFpsr() & fpsr_inexact) != 0);
        REQUIRE(host_fpcr_after == 0);
    }
}

TEST_CASE("arm64: memory aborts on callback paths are precise", "[a64][arm64][abort]") {
    Arena arena;
    A64Env env{arena};
    ExclusiveMonitor monitor{1};
    auto conf = MakeConfig(env, false);
    conf.global_monitor = &monitor;
    conf.check_halt_on_memory_access = true;
    conf.cold_memory_abort_state = A64::UserConfig::ColdAbortState::Live;

    for (const bool fastmem : {false, true}) {
        auto run_conf = conf;
        if (fastmem) {
            run_conf.fastmem_pointer = reinterpret_cast<std::uintptr_t>(arena.base);
            run_conf.fastmem_address_space_bits = 64;
            run_conf.silently_mirror_fastmem = false;
        }
        A64::Jit jit{run_conf};
        env.jit = &jit;
        // Fastmem faults in the inaccessible page and its fallback callback aborts.
        const u64 address = fastmem ? arena.device + 0x20 : arena.data + 0x20;
        env.abort_address = address;

        for (const u32 access : {LDR_X2_X3, STR_X1_X3, LDXR_X2_X3, LDADD_X1_X2_X3}) {
            INFO("fastmem " << fastmem << " access " << std::hex << access);
            // X1's first value is overwritten after the access: it reaches the JitState
            // only on the abort exit (A64ColdSetX).
            env.Load({ADD_X1_X4_1, access, ADD_X1_X1_1, MOVZ_X5_7, STOP});
            jit.SetRegister(1, 0);
            jit.SetRegister(3, address);
            jit.SetRegister(4, 100);
            jit.SetRegister(5, 0);
            const HaltReason hr = RunFrom(jit, 0);
            REQUIRE(Has(hr, HaltReason::MemoryAbort));
            REQUIRE(jit.GetPC() == 4);
            REQUIRE(jit.GetRegister(1) == 101);
            REQUIRE(jit.GetRegister(5) == 0);
        }
    }
}

TEST_CASE("arm64: fastmem exclusives keep the global monitor", "[a64][arm64][exclusive]") {
    for (const bool ignore_monitor : {false, true}) {
        INFO("ignore monitor " << ignore_monitor);
        Arena arena;
        A64Env env{arena};
        ExclusiveMonitor monitor{1};
        if (ignore_monitor) {
            monitor.EnableGenerations();
        }
        auto conf = MakeConfig(env, true);
        conf.unsafe_optimizations = ignore_monitor;
        conf.global_monitor = &monitor;
        conf.fastmem_exclusive_access = true;
        conf.elide_unchanged_exclusive_stores = true;
        A64::Jit jit{conf};
        env.jit = &jit;
        const u64 address = arena.data + 0x80;
        const auto memory = [&](u64 at) {
            u64 value;
            std::memcpy(&value, env.Host(at), sizeof(value));
            return value;
        };
        const auto set_memory = [&](u64 at, u64 value) { std::memcpy(env.Host(at), &value, sizeof(value)); };

        // An exclusive increment of each size, on fastmem and through a fault's callbacks.
        for (const u64 at : {address, arena.device + 0x80}) {
            for (u32 size = 0; size < 4; ++size) {
                INFO("at " << std::hex << at << " size " << size);
                const std::size_t bits = 8u << size;
                const u64 mask = bits == 64 ? ~u64(0) : (u64(1) << bits) - 1;
                const u64 old = 0x8877665544332211 | mask;  // the low `bits` wrap
                set_memory(at, old);
                env.Load({LDAXR_W2_X0[size], ADD_X2_X2_1, STLXR_W4_W2_X0[size], STOP});
                jit.SetRegister(0, at);
                jit.SetRegister(4, 7);
                REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
                REQUIRE(jit.GetRegister(4) == 0);
                REQUIRE(jit.GetRegister(2) == (old & mask) + 1);
                REQUIRE(memory(at) == (old & ~mask));
            }
        }

        // CLREX between the pair fails the store.
        set_memory(address, 5);
        env.Load({LDXR_X2_X0, CLREX, STXR_W4_X2_X0, STOP});
        jit.SetRegister(0, address);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(jit.GetRegister(4) == 1);
        REQUIRE(memory(address) == 5);

        // An unchanged value (elided store) succeeds.
        env.Load({LDXR_X2_X0, STXR_W4_X2_X0, STOP});
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(jit.GetRegister(4) == 0);
        REQUIRE(memory(address) == 5);

        // 128-bit pair.
        set_memory(address, 0x1111);
        set_memory(address + 8, 0x2222);
        env.Load({LDXP_X2_X3_X0, STXP_W4_X5_X6_X0, STOP});
        jit.SetRegister(5, 0x3333);
        jit.SetRegister(6, 0x4444);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(jit.GetRegister(2) == 0x1111);
        REQUIRE(jit.GetRegister(3) == 0x2222);
        REQUIRE(jit.GetRegister(4) == 0);
        REQUIRE(memory(address) == 0x3333);
        REQUIRE(memory(address + 8) == 0x4444);

        // Breaking the reservation between the pair fails the store: another CPU's
        // exclusive store clears it in the monitor; value-compared exclusives
        // (IgnoreGlobalMonitor) see the changed value.
        set_memory(address, 9);
        env.Load({LDXR_X2_X0, ADD_X2_X2_1, STXR_W4_X2_X0, STOP});
        jit.SetPC(0);
        REQUIRE(Has(jit.Step(), HaltReason::Step));
        if (ignore_monitor) {
            set_memory(address, 10);
        } else {
            monitor.ClearProcessor(0);
        }
        REQUIRE(jit.Run() == HaltReason::UserDefined2);
        REQUIRE(jit.GetRegister(4) == 1);
    }
}

namespace {

class A32Env final : public A32::UserCallbacks {
public:
    explicit A32Env(Arena& arena) : arena(arena) {}

    Arena& arena;
    A32::Jit* jit = nullptr;
    std::size_t fast_calls = 0, known_calls = 0;
    std::optional<u64> fpcr_inside;

    u64 MemoryRead(u32 vaddr, std::size_t size) override {
        u64 value = 0;
        std::memcpy(&value, arena.base + vaddr, size);
        return value;
    }
    void MemoryWrite(u32 vaddr, u64 value, std::size_t size) override { std::memcpy(arena.base + vaddr, &value, size); }
    void CallSVC(u32 swi) override {
        if (swi == stop_svc) {
            jit->HaltExecution(HaltReason::UserDefined2);
        }
    }
    void CallSVCFast(u32) override {
        ++fast_calls;
        fpcr_inside = HostFpcr();
        jit->Regs()[0] *= 2;
    }
    void CallSVCFastKnown(u32, const void*, void* state) override {
        ++known_calls;
        static_cast<Backend::Arm64::A32JitState*>(state)->regs[0] += 5;
    }
    void ExceptionRaised(u32, A32::Exception) override { jit->HaltExecution(HaltReason::UserDefined3); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1'000'000; }
};

}  // namespace

TEST_CASE("arm64: A32 fast supervisor calls", "[a32][arm64][svc]") {
    HostFpGuard guard;
    Arena arena;
    A32Env env{arena};
    static constexpr int token = 0;
    A32::UserConfig conf{};
    conf.callbacks = &env;
    conf.enable_cycle_counting = false;
    conf.code_cache_size = 16 * 1024 * 1024;
    conf.svc_fast = 0x10;
    conf.svc_fp_transfer = 0x12;
    conf.svc_lookup = [](u32 pc, u32 imm) -> const void* { return pc == 0x40 && imm == 0x10 ? &token : nullptr; };
    A32::Jit jit{conf};
    env.jit = &jit;

    constexpr std::array<u32, 3> program{0xef000010, 0xe2801001, 0xef0000ff};  // svc #0x10; add r1, r0, #1; svc #0xff
    std::memcpy(arena.base, program.data(), sizeof(program));
    jit.Regs()[0] = 21;
    jit.Regs()[15] = 0;
    jit.SetCpsr(0x000001d0);
    jit.SetFpscr(fpcr_round_toward_zero);
    SetHostFpcr(0);
    REQUIRE(jit.Run() == HaltReason::UserDefined2);
    REQUIRE(env.fast_calls == 1);
    REQUIRE(jit.Regs()[0] == 42);
    REQUIRE(jit.Regs()[1] == 43);
    REQUIRE(env.fpcr_inside == fpcr_round_toward_zero);  // svc_fast keeps the guest's

    constexpr std::array<u32, 3> transfer{0xef000012, 0xe2801001, 0xef0000ff};
    std::memcpy(arena.base, transfer.data(), sizeof(transfer));
    jit.ClearCache();
    jit.Regs()[15] = 0;
    REQUIRE(jit.Run() == HaltReason::UserDefined2);
    REQUIRE(env.fpcr_inside == 0);  // svc_fp_transfer switches to the host's

    std::memcpy(arena.base + 0x40, program.data(), sizeof(program));
    jit.ClearCache();
    jit.Regs()[0] = 1;
    jit.Regs()[15] = 0x40;
    REQUIRE(jit.Run() == HaltReason::UserDefined2);
    REQUIRE(env.known_calls == 1);
    REQUIRE(jit.Regs()[1] == 7);
}

namespace {

// Whether the code cache holds an instruction word matching (word & mask) == match.
bool CodeCacheHas(const void* begin, std::size_t size, u32 mask, u32 match) {
    const auto* words = static_cast<const u32*>(begin);
    for (std::size_t i = 0; i < size / sizeof(u32); i++) {
        if ((words[i] & mask) == match) {
            return true;
        }
    }
    return false;
}

}  // namespace

// Venture: AES, SHA256, 64-bit PMULL and CRC32 are optional ARMv8 instructions.
// The emitters use them only when the host reports them (HWCAP), as x64 checks
// CPUID; without them the same results come from the fallbacks.
TEST_CASE("arm64: optional crypto instructions follow the host features", "[a64][arm64][features]") {
    const std::vector<u32> program{
        0x4e284820,  // aese v0.16b, v1.16b
        0x4e286802,  // aesmc v2.16b, v0.16b
        0x4e285823,  // aesd v3.16b, v1.16b
        0x4e287864,  // aesimc v4.16b, v3.16b
        0x5e0740c5,  // sha256h q5, q6, v7.4s
        0x5e282928,  // sha256su0 v8.4s, v9.4s
        0x5e0c516a,  // sha256h2 q10, q11, v12.4s
        0x5e0f61cd,  // sha256su1 v13.4s, v14.4s, v15.4s
        0x0ef2e230,  // pmull v16.1q, v17.1d, v18.1d
        0x4ef2e233,  // pmull2 v19.1q, v17.2d, v18.2d
        0x1ac740c5,  // crc32b w5, w6, w7
        0x1ac744ca,  // crc32h w10, w6, w7
        0x1ac758cb,  // crc32cw w11, w6, w7
        0x9ac95cc8,  // crc32cx w8, w6, x9
        STOP,
    };
    const auto run = [&](u32 disabled, std::array<A64::Vector, 32>& vectors, std::array<u64, 31>& registers) {
        Arena arena;
        A64Env env{arena};
        auto conf = MakeConfig(env, false);
        conf.disabled_host_features = disabled;
        A64::Jit jit{conf};
        env.jit = &jit;
        env.Load(program);
        for (std::size_t i = 0; i < 32; i++) {
            jit.SetVector(i, {0x0123456789abcdefULL * (i + 1), 0xfedcba9876543210ULL ^ (i * 0x1111)});
        }
        jit.SetRegister(6, 0xdeadbeef);
        jit.SetRegister(7, 0x12345678);
        jit.SetRegister(9, 0x0f1e2d3c4b5a6978ULL);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        vectors = jit.GetVectors();
        registers = jit.GetRegisters();
        if (disabled == ~u32{0}) {
            const auto has = [&](u32 mask, u32 match) { return CodeCacheHas(jit.CodeCacheBegin(), jit.CodeCacheSize(), mask, match); };
            CHECK_FALSE(has(0xffffcc00, 0x4e284800));  // AESE, AESD, AESMC, AESIMC
            CHECK_FALSE(has(0xffe0cc00, 0x5e004000));  // SHA256H, SHA256H2, SHA256SU1
            CHECK_FALSE(has(0xfffffc00, 0x5e282800));  // SHA256SU0
            CHECK_FALSE(has(0xbfe0fc00, 0x0ee0e000));  // PMULL{2} .1q
            CHECK_FALSE(has(0x7fe0e000, 0x1ac04000));  // CRC32*, CRC32C*
        }
    };
    std::array<A64::Vector, 32> native_vectors, fallback_vectors;
    std::array<u64, 31> native_registers, fallback_registers;
    run(0, native_vectors, native_registers);
    run(~u32{0}, fallback_vectors, fallback_registers);
    for (std::size_t i = 0; i < 32; i++) {
        INFO("v" << i);
        REQUIRE(fallback_vectors[i] == native_vectors[i]);
    }
    REQUIRE(fallback_registers == native_registers);
}

namespace {

// Code memory (and a data table) for A32 tests that need more than a page.
class A32BigEnv final : public A32::UserCallbacks {
public:
    std::vector<u32> memory;
    A32::Jit* jit = nullptr;
    bool halted = false;

    u64 MemoryRead(u32 vaddr, std::size_t size) override {
        u64 value = 0;
        if (vaddr + size <= memory.size() * sizeof(u32)) {
            std::memcpy(&value, reinterpret_cast<const u8*>(memory.data()) + vaddr, size);
        }
        return value;
    }
    std::optional<u32> MemoryReadCode(u32 vaddr) override { return static_cast<u32>(MemoryRead(vaddr, 4)); }
    void MemoryWrite(u32, u64, std::size_t) override {}
    void CallSVC(u32) override {
        halted = true;
        jit->HaltExecution(HaltReason::UserDefined2);
    }
    void ExceptionRaised(u32, A32::Exception) override { jit->HaltExecution(HaltReason::UserDefined3); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1'000'000; }
};

}  // namespace

// Venture: a compile that finds the code cache full clears it inside Run. The
// return stack buffer, kept on the run's stack, then pointed at a return block
// whose code the next compile overwrote, so the guest return ran from the
// middle of another block (a hang or wrong registers). The dispatcher now
// resets it after such a compile, as x64 resets its RSB on every clear.
TEST_CASE("arm64: the return stack buffer survives a cache clear inside Run", "[a32][arm64][rsb]") {
    constexpr u32 table = 0x200;
    constexpr u32 tiny = 0x100;
    constexpr u32 functions = 0x1000;
    constexpr u32 adds = 256;
    constexpr u32 count = 400;
    constexpr u32 stride = (adds + 1) * 4;
    A32BigEnv env;
    env.memory.assign((functions + count * stride) / 4, 0);
    const std::array<u32, 6> main{
        0xe4932004,  // 0x00: ldr r2, [r3], #4
        0xe3520000,  // 0x04: cmp r2, #0
        0x0a000001,  // 0x08: beq 0x14
        0xe12fff32,  // 0x0c: blx r2
        0xeafffffa,  // 0x10: b 0x00 (the return block)
        0xef0000ff,  // 0x14: svc #0xff
    };
    std::copy(main.begin(), main.end(), env.memory.begin());
    env.memory[tiny / 4] = 0xe12fff1e;  // bx lr
    // Two calls of a small function compile and link the return block early in
    // the cache; then large, distinct functions fill it until it clears.
    std::vector<u32> targets{tiny, tiny};
    for (u32 i = 0; i < count; i++) {
        const u32 at = functions + i * stride;
        for (u32 j = 0; j < adds; j++) {
            env.memory[at / 4 + j] = 0xe0888009;  // add r8, r8, r9
        }
        env.memory[at / 4 + adds] = 0xe12fff1e;  // bx lr
        targets.push_back(at);
    }
    targets.push_back(0);
    std::copy(targets.begin(), targets.end(), env.memory.begin() + table / 4);

    A32::UserConfig conf{};
    conf.callbacks = &env;
    conf.enable_cycle_counting = false;
    // A compile clears the cache when less than 1 MiB is left: this one clears a
    // few times during the run.
    conf.code_cache_size = 1024 * 1024 + 128 * 1024;
    A32::Jit jit{conf};
    env.jit = &jit;
    jit.Regs()[3] = table;
    jit.Regs()[8] = 0;
    jit.Regs()[9] = 1;
    jit.Regs()[15] = 0;
    jit.SetCpsr(0x000001d0);
    REQUIRE(jit.Run() == HaltReason::UserDefined2);
    REQUIRE(env.halted);
    REQUIRE(jit.Regs()[8] == adds * count);
    REQUIRE(jit.Regs()[2] == 0);
    REQUIRE(jit.Regs()[3] == table + 4 * (count + 3));
}

namespace {

constexpr u32 MOVZ_X11_10 = 0xd280014b;
constexpr u32 MOVZ_X11_100 = 0xd2800c8b;
constexpr u32 BLR_X10 = 0xd63f0140;
constexpr u32 RET = 0xd65f03c0;
constexpr u32 SUBS_X11_X11_1 = 0xf100056b;
constexpr u32 EOR_X10_X10_X12 = 0xca0c014a;
constexpr u32 ADD_X3_X3_1 = 0x91000463;
constexpr u32 CMP_X0_0 = 0xf100001f;
constexpr u32 LDAR_W16_X27 = 0x88dfff70;  // the emitted halt poll
constexpr u32 BR_X3 = 0xd61f0060;         // a dispatch site's jump
constexpr u32 BRK_0 = 0xd4200000;

constexpr u32 BCond(s32 offset, u32 cond) { return 0x54000000 | (static_cast<u32>(offset / 4) & 0x7ffff) << 5 | cond; }
constexpr u32 B(s32 offset) { return 0x14000000 | (static_cast<u32>(offset / 4) & 0x3ffffff); }
static_assert(BCond(-8, 1) == 0x54ffffc1);  // b.ne .-8
static_assert(B(-12) == 0x17fffffd);

std::size_t CodeCacheCount(const void* begin, std::size_t size, u32 mask, u32 match) {
    const auto* words = static_cast<const u32*>(begin);
    std::size_t count = 0;
    for (std::size_t i = 0; i < size / sizeof(u32); i++) {
        count += (words[i] & mask) == match;
    }
    return count;
}

// A loop of `iterations` calls through BLR X10 that alternate between X10 and
// X10 ^ X12. The function at 0x100 increments X1, the one at 0x200 X2.
void LoadCallLoop(A64Env& env, u32 movz_iterations) {
    env.Load({movz_iterations, BLR_X10, EOR_X10_X10_X12, SUBS_X11_X11_1, BCond(-12, 1), STOP});
    env.Load({ADD_X1_X1_1, RET}, 0x100);
    env.Load({ADD_X2_X2_1, RET}, 0x200);
}

}  // namespace

// Venture: A64 fast dispatch on arm64, as on x64. Calls push the return stack
// buffer, returns pop it, and indirect branches look their target up in the
// fast dispatch table in emitted code; the census counts each event.
TEST_CASE("arm64: A64 fast dispatch and the return stack buffer", "[a64][arm64][dispatch]") {
    Arena arena;
    A64Env env{arena};
    std::array<u64, 6> census{};
    auto conf = MakeConfig(env, false);
    conf.fast_dispatch_census = census.data();
    A64::Jit jit{conf};
    env.jit = &jit;

    LoadCallLoop(env, MOVZ_X11_100);
    jit.SetRegister(10, 0x100);
    jit.SetRegister(12, 0x300);
    REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
    REQUIRE(jit.GetRegister(1) == 50);
    REQUIRE(jit.GetRegister(2) == 50);
    CHECK(census[0] == 100);  // every RET pops
    CHECK(census[1] == 0);    // and finds its BLR's entry
    CHECK(census[2] == 100);  // every BLR takes the table lookup
    CHECK(census[3] == 2);    // which misses once per target
}

// Venture: svc_return_link (as on x64). A supervisor call's block pushes no
// return entry and its exit links to the next instruction directly.
TEST_CASE("arm64: svc_return_link links a supervisor call to its return", "[a64][arm64][dispatch]") {
    for (const bool link : {false, true}) {
        Arena arena;
        A64Env env{arena};
        std::array<u64, 6> census{};
        auto conf = MakeConfig(env, false);
        conf.svc_fast = 0x10;
        conf.svc_return_link = link;
        conf.fast_dispatch_census = census.data();
        A64::Jit jit{conf};
        env.jit = &jit;

        env.Load({MOVZ_X11_100, Svc(0x10), SUBS_X11_X11_1, BCond(-8, 1), STOP});
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(env.fast_calls == 100);
        CHECK(census[0] == (link ? 0 : 100));

        // A call that moves the PC leaves through the table lookup.
        env.fast_calls = 0;
        env.on_fast = [&](u32) { jit.SetPC(16); };
        jit.SetRegister(11, 5);
        REQUIRE(RunFrom(jit, 4) == HaltReason::UserDefined2);
        REQUIRE(env.fast_calls == 1);
        REQUIRE(jit.GetRegister(11) == 5);
    }
}

// Venture: dispatch_site_cache (as on x64). Each indirect-branch site keeps
// its last target; a different target refills the slot, and invalidation
// empties every slot.
TEST_CASE("arm64: dispatch_site_cache follows targets and invalidation", "[a64][arm64][dispatch]") {
    for (const bool sites : {false, true}) {
        Arena arena;
        A64Env env{arena};
        auto conf = MakeConfig(env, false);
        conf.dispatch_site_cache = sites;
        A64::Jit jit{conf};
        env.jit = &jit;

        LoadCallLoop(env, MOVZ_X11_100);
        jit.SetRegister(10, 0x100);
        jit.SetRegister(12, 0x300);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(jit.GetRegister(1) == 50);
        REQUIRE(jit.GetRegister(2) == 50);
        CHECK((CodeCacheCount(jit.CodeCacheBegin(), jit.CodeCacheSize(), ~u32(0), BR_X3) != 0) == sites);

        // One target: the slot holds it. Rewriting the target must not run
        // the old code through the slot.
        jit.SetRegister(1, 0);
        jit.SetRegister(2, 0);
        jit.SetRegister(10, 0x100);
        jit.SetRegister(11, 10);
        jit.SetRegister(12, 0);
        REQUIRE(RunFrom(jit, 4) == HaltReason::UserDefined2);
        REQUIRE(jit.GetRegister(1) == 10);
        std::memcpy(arena.base + 0x100, &ADD_X3_X3_1, sizeof(u32));
        jit.InvalidateCacheRange(0x100, 4);
        jit.SetRegister(11, 10);
        REQUIRE(RunFrom(jit, 4) == HaltReason::UserDefined2);
        REQUIRE(jit.GetRegister(1) == 10);
        REQUIRE(jit.GetRegister(3) == 10);
    }
}

// Venture: enable_bounded_halt_polling (as on x64). Forward conditional links
// within one window skip the halt poll; the results are the same.
TEST_CASE("arm64: bounded halt polling elides forward conditional polls", "[a64][arm64][dispatch]") {
    std::array<std::size_t, 2> polls{};
    for (const bool bounded : {false, true}) {
        for (const u64 x0 : {0, 1}) {
            Arena arena;
            A64Env env{arena};
            auto conf = MakeConfig(env, false);
            conf.enable_bounded_halt_polling = bounded;
            A64::Jit jit{conf};
            env.jit = &jit;

            env.Load({CMP_X0_0, BCond(8, 0), ADD_X1_X1_1,
                      CMP_X0_0, BCond(8, 1), ADD_X1_X1_1,
                      CMP_X0_0, BCond(8, 0), ADD_X2_X2_1, STOP});
            jit.SetRegister(0, x0);
            REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
            REQUIRE(jit.GetRegister(1) == 1);
            REQUIRE(jit.GetRegister(2) == x0);
            if (x0 == 0) {
                polls[bounded] = CodeCacheCount(jit.CodeCacheBegin(), jit.CodeCacheSize(), ~u32(0), LDAR_W16_X27);
            }
        }
    }
    CHECK(polls[1] < polls[0]);
}

// Venture: compact_block_links for A64 (as on x64 and for A32). A polled link
// keeps only the poll and the patchable branch in line; unlinked, the branch
// reaches the PC store in the block's deferred code.
TEST_CASE("arm64: A64 compact block links", "[a64][arm64][dispatch]") {
    for (const bool compact : {false, true}) {
        Arena arena;
        A64Env env{arena};
        auto conf = MakeConfig(env, false);
        conf.compact_block_links = compact;
        A64::Jit jit{conf};
        env.jit = &jit;

        // x11 = 10; loop: if (--x11 == 0) stop; x1++; goto loop.
        env.Load({MOVZ_X11_10, SUBS_X11_X11_1, BCond(12, 0), ADD_X1_X1_1, B(-12), STOP});
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(jit.GetRegister(1) == 9);

        // The backward link: poll, branch on halt, link branch, end of block.
        const auto* words = static_cast<const u32*>(jit.CodeCacheBegin());
        bool found = false;
        for (std::size_t i = 0; i + 3 < jit.CodeCacheSize() / sizeof(u32); i++) {
            found |= words[i] == LDAR_W16_X27 && (words[i + 1] & 0xff00001f) == 0x35000010 &&
                     (words[i + 2] & 0xfc000000) == 0x14000000 && words[i + 3] == BRK_0;
        }
        CHECK(found == compact);

        // Unlink and relink: replace the loop body.
        std::memcpy(arena.base + 12, &ADD_X2_X2_1, sizeof(u32));
        jit.InvalidateCacheRange(12, 4);
        jit.SetRegister(1, 0);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(jit.GetRegister(1) == 0);
        REQUIRE(jit.GetRegister(2) == 9);
    }
}

// Venture: cold abort state on arm64 (as on x64). X1's first value is
// overwritten after two loads: with Live it is stored only on each load's
// abort exit, out of line, instead of once before the first load.
TEST_CASE("arm64: cold abort state stores on the abort exits", "[a64][arm64][abort]") {
    constexpr u32 str_x_x28 = 0xf9000000 | 28 << 5;
    const u32 x1_slot = static_cast<u32>((offsetof(Backend::Arm64::A64JitState, reg) + sizeof(u64)) / 8) << 10;
    std::array<std::size_t, 2> stores{};
    for (const bool live : {false, true}) {
        Arena arena;
        A64Env env{arena};
        auto conf = MakeConfig(env, false);
        conf.check_halt_on_memory_access = true;
        conf.cold_memory_abort_state = live ? A64::UserConfig::ColdAbortState::Live : A64::UserConfig::ColdAbortState::Off;
        A64::Jit jit{conf};
        env.jit = &jit;

        const u64 address = arena.data + 0x20;
        const u64 value = 0x1234;
        std::memcpy(arena.base + address, &value, sizeof(value));
        env.Load({ADD_X1_X4_1, LDR_X2_X3, LDR_X2_X3, ADD_X1_X1_1, STOP});
        jit.SetRegister(3, address);
        jit.SetRegister(4, 10);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        REQUIRE(jit.GetRegister(1) == 12);
        REQUIRE(jit.GetRegister(2) == value);
        stores[live] = CodeCacheCount(jit.CodeCacheBegin(), jit.CodeCacheSize(), 0xffffffe0, str_x_x28 | x1_slot);

        // An abort at the first load: X1 holds its first value.
        env.abort_address = address;
        jit.SetRegister(1, 0);
        REQUIRE(RunFrom(jit, 0) == HaltReason::MemoryAbort);
        REQUIRE(jit.GetPC() == 4);
        REQUIRE(jit.GetRegister(1) == 11);
    }
    CHECK(stores[0] == 2);
    CHECK(stores[1] == 3);
}

// Venture: next-use eviction (as on x64). Twenty-five values stay live across
// the block in 21 allocatable GPRs and are read back in definition order: the
// allocator evicts those read last, so each spilled value is reloaded once,
// late, instead of the round robin spilling values read next.
TEST_CASE("arm64: register eviction prefers the value needed furthest ahead", "[a64][arm64][regalloc]") {
    Arena arena;
    A64Env env{arena};
    A64::Jit jit{MakeConfig(env, false)};
    env.jit = &jit;

    std::vector<u32> code;
    for (u32 i = 1; i <= 25; i++) {
        code.push_back(0x8b000000 | i << 5 | i);  // add xI, xI, x0
    }
    for (u32 i = 1; i <= 25; i++) {
        code.push_back(0x8b000000 | i << 16 | 28 << 5 | 28);  // add x28, x28, xI
    }
    code.push_back(STOP);
    env.Load(code);
    u64 expected = 0;
    for (u32 i = 1; i <= 25; i++) {
        jit.SetRegister(i, i * 3);
        expected += i * 3 + 7;
    }
    jit.SetRegister(0, 7);
    jit.SetRegister(28, 0);
    REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
    REQUIRE(jit.GetRegister(28) == expected);

    // Reloads: LDR Xt, [SP, #spill slot].
    constexpr std::size_t spill = offsetof(Backend::Arm64::StackLayout, spill);
    std::size_t reloads = 0;
    const auto* words = static_cast<const u32*>(jit.CodeCacheBegin());
    for (std::size_t i = 0; i < jit.CodeCacheSize() / sizeof(u32); i++) {
        const u32 w = words[i];
        const std::size_t offset = ((w >> 10) & 0xfff) * 8;
        reloads += (w & 0xffc003e0) == 0xf94003e0 && offset >= spill && offset < spill + 16 * Backend::Arm64::SpillCount;
    }
    CHECK(reloads <= 6);
}

namespace {
// Stores to the stack spill slots: STR Xt, [SP, #spill slot] and STR Qt.
std::size_t CountSpillStores(A64::Jit& jit) {
    constexpr std::size_t spill = offsetof(Backend::Arm64::StackLayout, spill);
    constexpr std::size_t end = spill + 16 * Backend::Arm64::SpillCount;
    std::size_t stores = 0;
    const auto* words = static_cast<const u32*>(jit.CodeCacheBegin());
    for (std::size_t i = 0; i < jit.CodeCacheSize() / sizeof(u32); i++) {
        const u32 w = words[i];
        const std::size_t x = ((w >> 10) & 0xfff) * 8;
        const std::size_t q = ((w >> 10) & 0xfff) * 16;
        stores += (w & 0xffc003e0) == 0xf90003e0 && x >= spill && x < end;
        stores += (w & 0xffc003e0) == 0x3d8003e0 && q >= spill && q < end;
    }
    return stores;
}

// The same count for a JIT that ran only STOP: the prelude's callee-saved
// register pushes can match the pattern.
std::size_t BaselineSpillStores() {
    Arena arena;
    A64Env env{arena};
    A64::Jit jit{MakeConfig(env, false)};
    env.jit = &jit;
    env.Load({STOP});
    REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
    return CountSpillStores(jit);
}
}  // namespace

// Venture: guest register reads are re-read from the JIT state instead of
// spilled, as on x64. Twenty-five registers are read, used, and read again
// at the end of the block: with 21 allocatable GPRs some must leave their
// registers, and an unwritten guest register needs no spill store.
TEST_CASE("arm64: guest register reads are re-read rather than spilled", "[a64][arm64][regalloc]") {
    Arena arena;
    A64Env env{arena};
    A64::Jit jit{MakeConfig(env, false)};
    env.jit = &jit;

    std::vector<u32> code;
    for (u32 i = 1; i <= 25; i++) {
        code.push_back(0x8b000000 | i << 16 | 26 << 5 | 26);  // add x26, x26, xI
    }
    for (u32 i = 1; i <= 25; i++) {
        code.push_back(0x8b000000 | i << 16 | 27 << 5 | 27);  // add x27, x27, xI
    }
    code.push_back(STOP);
    env.Load(code);
    u64 expected = 0;
    for (u32 i = 1; i <= 25; i++) {
        jit.SetRegister(i, i * 5 + 1);
        expected += i * 5 + 1;
    }
    jit.SetRegister(26, 0);
    jit.SetRegister(27, 0);
    REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
    REQUIRE(jit.GetRegister(26) == expected);
    REQUIRE(jit.GetRegister(27) == expected);
    CHECK(CountSpillStores(jit) == BaselineSpillStores());
}

// A read whose register is written before its last use keeps a spill slot:
// fifteen reads are copied to other registers, their own registers are
// cleared, and a memory callback (which spills the caller-saved registers)
// comes before the copies are summed.
TEST_CASE("arm64: re-read guest registers stop at a write to the register", "[a64][arm64][regalloc]") {
    Arena arena;
    A64Env env{arena};
    A64::Jit jit{MakeConfig(env, false)};
    env.jit = &jit;

    std::vector<u32> code;
    for (u32 i = 1; i <= 15; i++) {
        code.push_back(0xaa0003e0 | i << 16 | (15 + i));  // mov x(15+I), xI
    }
    for (u32 i = 1; i <= 15; i++) {
        code.push_back(0xd2800000 | i);  // movz xI, #0
    }
    code.push_back(0xf9400000);  // ldr x0, [x0]
    for (u32 i = 2; i <= 15; i++) {
        code.push_back(0x8b000000 | (15 + i) << 16 | 16 << 5 | 16);  // add x16, x16, x(15+I)
    }
    code.push_back(STOP);
    env.Load(code);
    u64 expected = 0;
    for (u32 i = 1; i <= 15; i++) {
        jit.SetRegister(i, i * 7 + 3);
        expected += i * 7 + 3;
    }
    jit.SetRegister(0, 0);
    REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
    REQUIRE(jit.GetRegister(16) == expected);
    for (u32 i = 1; i <= 15; i++) {
        REQUIRE(jit.GetRegister(i) == 0);
        REQUIRE(jit.GetRegister(15 + i) == (i == 1 ? expected : i * 7 + 3));
    }
    CHECK(CountSpillStores(jit) > BaselineSpillStores());
}

namespace {
// MSR NZCV, Xt in the code cache.
std::size_t CountMsrNzcv(A64::Jit& jit) {
    std::size_t count = 0;
    const auto* words = static_cast<const u32*>(jit.CodeCacheBegin());
    for (std::size_t i = 0; i < jit.CodeCacheSize() / sizeof(u32); i++) {
        count += (words[i] & 0xffffffe0) == 0xd51b4200;
    }
    return count;
}
}  // namespace

// Venture: selects read live host flags, as on x64. SUBS leaves guest NZCV in
// host NZCV, and the selects after it (with guest register reads and writes
// in between) use it without moving it back through MSR.
TEST_CASE("arm64: selects read the flags a compare left in host NZCV", "[a64][arm64][flags]") {
    const auto run = [](std::vector<u32> code, u64 x1, u64 x2, std::size_t& msr) {
        Arena arena;
        A64Env env{arena};
        A64::Jit jit{MakeConfig(env, false)};
        env.jit = &jit;
        code.push_back(STOP);
        env.Load(code);
        jit.SetRegister(1, x1);
        jit.SetRegister(2, x2);
        for (u32 i = 4; i <= 9; i++) {
            jit.SetRegister(i, i * 100);
        }
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        msr = CountMsrNzcv(jit);
        return std::array<u64, 3>{jit.GetRegister(3), jit.GetRegister(10), jit.GetPstate()};
    };
    std::size_t baseline = 0;
    run({}, 0, 0, baseline);

    const std::vector<u32> code{
        0xeb020020,  // subs x0, x1, x2
        0x9a850083,  // csel x3, x4, x5, eq
        0x9a87b0ca,  // csel x10, x6, x7, lt
    };
    for (const auto& [x1, x2] : {std::pair<u64, u64>{5, 5}, {3, 9}, {9, 3}, {u64(1) << 63, 1}}) {
        std::size_t msr = 0;
        const auto [x3, x10, pstate] = run(code, x1, x2, msr);
        const bool eq = x1 == x2;
        CHECK(x3 == (eq ? 400 : 500));
        const std::int64_t a = static_cast<std::int64_t>(x1), b = static_cast<std::int64_t>(x2);
        const bool n = static_cast<std::int64_t>(x1 - x2) < 0;
        const bool v = ((a ^ b) & (a ^ static_cast<std::int64_t>(x1 - x2))) < 0;
        CHECK(x10 == (n != v ? 600 : 700));
        CHECK((pstate >> 28) == ((n ? 8 : 0) | (eq ? 4 : 0) | (x1 >= x2 ? 2 : 0) | (v ? 1 : 0)));
        CHECK(msr == baseline);
    }
}

// Venture: inline exclusives keep the global monitor (no
// Unsafe_IgnoreGlobalMonitor), as x64's locked inline path: the pairs run on
// fastmem under the monitor's lock without the callbacks, and another CPU's
// exclusive stores clear this CPU's reservation even when they put back the
// value it loaded.
TEST_CASE("arm64: inline exclusives under the global monitor's lock", "[a64][arm64][exclusive]") {
    Arena arena;
    A64Env env_a{arena}, env_b{arena};
    ExclusiveMonitor monitor{2};
    const auto config = [&](A64Env& env, std::size_t id) {
        auto conf = MakeConfig(env, true);
        conf.unsafe_optimizations = false;
        conf.global_monitor = &monitor;
        conf.processor_id = id;
        conf.fastmem_exclusive_access = true;
        return conf;
    };
    A64::Jit a{config(env_a, 0)};
    A64::Jit b{config(env_b, 1)};
    env_a.jit = &a;
    env_b.jit = &b;
    const u64 address = arena.data + 0x80;
    const auto memory = [&] {
        u64 value;
        std::memcpy(&value, env_a.Host(address), sizeof(value));
        return value;
    };
    const u64 five = 5;
    std::memcpy(env_a.Host(address), &five, sizeof(five));

    constexpr u32 SUB_X2_X2_1 = 0xd1000442;
    env_a.Load({LDXR_X2_X0, ADD_X2_X2_1, STXR_W4_X2_X0, STOP});
    env_b.Load({LDXR_X2_X0, ADD_X2_X2_1, STXR_W4_X2_X0, LDXR_X2_X0, SUB_X2_X2_1, STXR_W4_X2_X0, STOP}, 0x100);
    a.SetRegister(0, address);
    b.SetRegister(0, address);

    a.SetRegister(4, 7);
    REQUIRE(RunFrom(a, 0) == HaltReason::UserDefined2);
    REQUIRE(a.GetRegister(4) == 0);
    REQUIRE(memory() == 6);

    // A loads 6; B stores 7 and then 6; A's store fails.
    a.SetPC(0);
    REQUIRE(Has(a.Step(), HaltReason::Step));
    b.SetRegister(4, 7);
    REQUIRE(RunFrom(b, 0x100) == HaltReason::UserDefined2);
    REQUIRE(b.GetRegister(4) == 0);
    REQUIRE(memory() == 6);
    REQUIRE(a.Run() == HaltReason::UserDefined2);
    CHECK(a.GetRegister(4) == 1);
    CHECK(memory() == 6);

    CHECK(env_a.memory_calls == 0);
    CHECK(env_b.memory_calls == 0);
}

namespace {
// LDR/STR Xt, [X28, #reg + 8 * {0, 1, 8, 30}]: the cached guest registers in the JIT state.
std::size_t CountCachedStateAccesses(A64::Jit& jit) {
    std::size_t count = 0;
    const auto* words = static_cast<const u32*>(jit.CodeCacheBegin());
    for (std::size_t i = 0; i < jit.CodeCacheSize() / sizeof(u32); i++) {
        const u32 w = words[i];
        if ((w & 0xffc003e0) != (0xf9400000 | 28 << 5) && (w & 0xffc003e0) != (0xf9000000 | 28 << 5)) {
            continue;
        }
        const std::size_t offset = ((w >> 10) & 0xfff) * 8;
        for (const std::size_t guest : {0, 1, 8, 30}) {
            count += offset == offsetof(Backend::Arm64::A64JitState, reg) + 8 * guest;
        }
    }
    return count;
}
}  // namespace

// Venture: the A64 register cache (as x64's): guest X0, X1, X8 and X30 stay in
// host registers across blocks and runs' edges load and store them, so block
// code never touches their JIT state slots. Supervisor calls see and change
// them in the JIT state, and a memory abort leaves them there.
TEST_CASE("arm64: A64 register cache", "[a64][arm64][regcache]") {
    for (const bool cache : {false, true}) {
        INFO("cache " << cache);
        Arena arena;
        A64Env env{arena};
        auto conf = MakeConfig(env, false);
        conf.enable_register_cache = cache;
        conf.check_halt_on_memory_access = true;
        A64::Jit jit{conf};
        env.jit = &jit;

        // Baseline: only the stop block (its supervisor call stores and reloads the cache).
        env.Load({STOP});
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        const std::size_t baseline = CountCachedStateAccesses(jit);

        env.Load({
            0x8b010000,  // add x0, x0, x1
            0x8b0803de,  // add x30, x30, x8
            0x8b000021,  // add x1, x1, x0
            0x14000001,  // b next block
            0x8b1e0108,  // add x8, x8, x30
            STOP,
        });
        jit.SetRegister(0, 1);
        jit.SetRegister(1, 2);
        jit.SetRegister(8, 3);
        jit.SetRegister(30, 4);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        CHECK(jit.GetRegister(0) == 3);
        CHECK(jit.GetRegister(1) == 5);
        CHECK(jit.GetRegister(30) == 7);
        CHECK(jit.GetRegister(8) == 10);
        if (cache) {
            CHECK(CountCachedStateAccesses(jit) == baseline);
        } else {
            CHECK(CountCachedStateAccesses(jit) > baseline);
        }

        // A supervisor call's handler reads and writes the registers in the JIT state.
        u64 seen = 0;
        env.on_svc = [&](u32 swi) {
            if (swi == 1) {
                seen = jit.GetRegister(30);
                jit.SetRegister(0, 100);
            }
        };
        env.Load({
            0x910007de,  // add x30, x30, #1
            Svc(1),
            0x91000400,  // add x0, x0, #1
            STOP,
        });
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        CHECK(seen == 8);
        CHECK(jit.GetRegister(0) == 101);
        env.on_svc = nullptr;

        // A memory abort leaves the earlier register writes in the JIT state.
        const u64 address = arena.data + 0x20;
        env.abort_address = address;
        env.Load({
            0xd28000a0,  // movz x0, #5
            0xf9400062,  // ldr x2, [x3]
            0xd2800000,  // movz x0, #0
            STOP,
        });
        jit.SetRegister(3, address);
        REQUIRE(Has(RunFrom(jit, 0), HaltReason::MemoryAbort));
        CHECK(jit.GetPC() == 4);
        CHECK(jit.GetRegister(0) == 5);
        env.abort_address.reset();
    }
}

// Venture: a conditional exit after SUBS branches on host NZCV instead of
// reloading the NZCV the block just stored, as on x64.
TEST_CASE("arm64: an If terminal branches on the NZCV left in host NZCV", "[a64][arm64][nzcv]") {
    const auto count_nzcv_loads = [](A64::Jit& jit) {
        constexpr u32 offset = offsetof(Backend::Arm64::A64JitState, cpsr_nzcv);
        std::size_t count = 0;
        const auto* words = static_cast<const u32*>(jit.CodeCacheBegin());
        for (std::size_t i = 0; i < jit.CodeCacheSize() / sizeof(u32); i++) {
            count += (words[i] & 0xffffffe0) == (0xb9400000 | (offset / 4) << 10 | 28 << 5);  // LDR Wt, [X28, #cpsr_nzcv]
        }
        return count;
    };
    const auto run = [&](std::vector<u32> code, u64 x1, u64 x2, std::size_t& loads) {
        Arena arena;
        A64Env env{arena};
        A64::Jit jit{MakeConfig(env, false)};
        env.jit = &jit;
        env.Load(code);
        jit.SetRegister(1, x1);
        jit.SetRegister(2, x2);
        jit.SetRegister(9, 0);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        loads = count_nzcv_loads(jit);
        return std::pair<u64, u32>{jit.GetRegister(9), jit.GetPstate() >> 28};
    };
    std::size_t baseline = 0;
    run({STOP}, 0, 0, baseline);
    const std::vector<u32> code{
        0xeb020020,  // subs x0, x1, x2
        0x54000060,  // b.eq 12
        0x91000529,  // add x9, x9, #1
        STOP,
        0x91002129,  // add x9, x9, #8
        STOP,
    };
    for (const auto& [x1, x2] : {std::pair<u64, u64>{4, 4}, {3, 4}}) {
        std::size_t loads = 0;
        const auto [x9, nzcv] = run(code, x1, x2, loads);
        CHECK(x9 == (x1 == x2 ? 8 : 1));
        CHECK(nzcv == (x1 == x2 ? 0b0110u : 0b1000u));
        CHECK(loads == baseline);
    }
}

// Venture: nzcv_exit_liveness (as on x64): NZCV reaches the JIT state only on
// the exits whose successor may read it before overwriting it. Here the taken
// successor overwrites NZCV after a load, so at that load's memory abort the JIT
// state still holds the NZCV from before the SUBS; the other successor
// reads the flags and gets them.
TEST_CASE("arm64: nzcv_exit_liveness stores NZCV only on exits that read it", "[a64][arm64][nzcv]") {
    for (const bool liveness : {false, true}) {
        INFO("liveness " << liveness);
        Arena arena;
        A64Env env{arena};
        auto conf = MakeConfig(env, false);
        conf.nzcv_exit_liveness = liveness;
        conf.check_halt_on_memory_access = true;
        A64::Jit jit{conf};
        env.jit = &jit;
        env.Load({
            0xeb020020,  // subs x0, x1, x2
            0x54000060,  // b.eq 12
            0x9a8710c5,  // csel x5, x6, x7, ne
            STOP,
            0xf9400149,  // ldr x9, [x10]
            0xeb04007f,  // cmp x3, x4
            STOP,
        });
        jit.SetRegister(6, 60);
        jit.SetRegister(7, 70);
        jit.SetRegister(10, arena.data + 0x40);

        // Not equal: the successor reads the flags.
        jit.SetPstate(0);
        jit.SetRegister(1, 3);
        jit.SetRegister(2, 4);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        CHECK(jit.GetRegister(5) == 60);
        CHECK((jit.GetPstate() >> 28) == 0b1000u);

        // Equal: a halt between the exit and the overwrite.
        env.abort_address = arena.data + 0x40;
        jit.SetPstate(0);
        jit.SetRegister(1, 4);
        jit.SetRegister(2, 4);
        REQUIRE(Has(RunFrom(jit, 0), HaltReason::MemoryAbort));
        CHECK(jit.GetPC() == 16);
        CHECK((jit.GetPstate() >> 28) == (liveness ? 0u : 0b0110u));
        env.abort_address.reset();

        // Running on, the overwrite sets NZCV.
        jit.SetRegister(3, 1);
        jit.SetRegister(4, 2);
        REQUIRE(jit.Run() == HaltReason::UserDefined2);
        CHECK((jit.GetPstate() >> 28) == 0b1000u);
    }
}

// Venture: address folding (as x64's FoldMemoryAddresses): with identity
// fastmem an immediate or register offset becomes the host access's own
// addressing mode ([Xn, #imm], [Xn, #simm9] or [Xn, Xm, LSL #2]) instead of an
// ADD and [X25, Xaddr]. A fault on a folded access calls back with the full
// address.
TEST_CASE("arm64: address folding", "[a64][arm64][fold]") {
    const auto count = [](A64::Jit& jit, u32 mask, u32 pattern) {
        std::size_t n = 0;
        const auto* words = static_cast<const u32*>(jit.CodeCacheBegin());
        for (std::size_t i = 0; i < jit.CodeCacheSize() / sizeof(u32); i++) {
            const u32 rn = (words[i] >> 5) & 31;
            n += (words[i] & mask) == pattern && rn != 28 && rn != 31;
        }
        return n;
    };
    std::array<std::array<std::size_t, 4>, 2> counts{};
    for (const bool fold : {false, true}) {
        INFO("fold " << fold);
        Arena arena;
        A64Env env{arena};
        auto conf = MakeConfig(env, false);
        conf.fastmem_pointer = 0;  // identity: guest addresses are host addresses
        conf.fastmem_address_space_bits = 64;
        conf.silently_mirror_fastmem = false;
        conf.recompile_on_fastmem_failure = false;
        conf.check_halt_on_memory_access = true;
        if (fold) {
            conf.optimizations |= OptimizationFlag::AddressFolding;
        } else {
            conf.optimizations &= ~OptimizationFlag::AddressFolding;
        }
        A64::Jit jit{conf};
        env.jit = &jit;
        const std::vector<u32> code{
            0xf9400801,  // ldr x1, [x0, #16]
            0xf9000c01,  // str x1, [x0, #24]
            0xb8627803,  // ldr w3, [x0, x2, lsl #2]
            0xf85f8004,  // ldur x4, [x0, #-8]
            STOP,
        };
        env.Load(code);
        u8* const at = arena.base + arena.data + 0x40;
        const u64 x0 = reinterpret_cast<u64>(at);
        const u64 a = 0x1122334455667788, b = 0x99aabbccddeeff00;
        const u32 c = 0xcafef00d;
        std::memcpy(at + 16, &a, sizeof(a));
        std::memcpy(at - 8, &b, sizeof(b));
        std::memcpy(at + 12, &c, sizeof(c));
        jit.SetRegister(0, x0);
        jit.SetRegister(2, 3);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        CHECK(jit.GetRegister(1) == a);
        u64 stored = 0;
        std::memcpy(&stored, at + 24, sizeof(stored));
        CHECK(stored == a);
        CHECK(jit.GetRegister(3) == c);
        CHECK(jit.GetRegister(4) == b);
        CHECK(env.memory_calls == 0);
        counts[fold] = {
            count(jit, 0xfffffc00, 0xf9400800),  // ldr xt, [xn, #16]
            count(jit, 0xfffffc00, 0xf9000c00),  // str xt, [xn, #24]
            count(jit, 0xffe0fc00, 0xb8607800),  // ldr wt, [xn, xm, lsl #2]
            count(jit, 0xfffffc00, 0xf85f8000),  // ldur xt, [xn, #-8]
        };

        // Each access faulting in the device page calls back with its full address.
        const u64 device = reinterpret_cast<u64>(arena.base + arena.device) + 0x40;
        const std::array<std::pair<std::size_t, u64>, 4> faults{{{0, device + 16}, {1, device + 24}, {2, device + 12}, {3, device - 8}}};
        for (const auto& [index, address] : faults) {
            INFO("access " << index);
            env.Load({code[index], STOP});
            env.abort_address = address;
            jit.SetRegister(0, device);
            REQUIRE(Has(RunFrom(jit, 0), HaltReason::MemoryAbort));
            CHECK(jit.GetPC() == 0);
            CHECK(jit.GetRegister(0) == device);
            CHECK(jit.GetRegister(2) == 3);
        }
        env.abort_address.reset();
    }
    for (std::size_t i = 0; i < 4; i++) {
        INFO("pattern " << i);
        CHECK(counts[true][i] > counts[false][i]);
    }
}

// Venture: LowLaneAliasing (as x64's): a scalar FP operand stays in its vector
// register (aliasing a vector that dies there, an in-register FMOV otherwise)
// instead of a UMOV to a GPR and a move back. Only the low lane is defined:
// integer reads and zero extensions still see a zero-extended value.
TEST_CASE("arm64: LowLaneAliasing", "[a64][arm64][lanes]") {
    const auto count = [](A64::Jit& jit, u32 pattern) {
        std::size_t n = 0;
        const auto* words = static_cast<const u32*>(jit.CodeCacheBegin());
        for (std::size_t i = 0; i < jit.CodeCacheSize() / sizeof(u32); i++) {
            n += (words[i] & 0xfffffc00) == pattern;
        }
        return n;
    };
    std::array<std::array<std::size_t, 2>, 2> umovs{};
    for (const bool lanes : {false, true}) {
        INFO("lanes " << lanes);
        Arena arena;
        A64Env env{arena};
        auto conf = MakeConfig(env, false);
        if (lanes) {
            conf.optimizations |= OptimizationFlag::LowLaneAliasing;
        } else {
            conf.optimizations &= ~OptimizationFlag::LowLaneAliasing;
        }
        A64::Jit jit{conf};
        env.jit = &jit;
        env.Load({
            0x1e222820,  // fadd s0, s1, s2
            0x1e204083,  // fmov s3, s4
            0x1e2600c5,  // fmov w5, s6
            0x9e660207,  // fmov x7, d16
            0x1e322a51,  // fadd s17, s18, s18
            0x1e260248,  // fmov w8, s18
            STOP,
        });
        const u64 junk = 0x5555aaaa5555aaaa;
        jit.SetVector(1, {0xdeadbeef'3fc00000, junk});  // 1.5f
        jit.SetVector(2, {0xfeedface'40100000, junk});  // 2.25f
        jit.SetVector(4, {0x12345678'aabbccdd, junk});
        jit.SetVector(6, {0xffffffff'01020304, junk});
        jit.SetVector(16, {0x01234567'89abcdef, junk});
        jit.SetVector(18, {0xabcdef01'3f400000, junk});  // 0.75f
        for (const std::size_t r : {5, 7, 8}) {
            jit.SetRegister(r, ~u64(0));
        }
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        CHECK(jit.GetVector(0) == Vector{0x40700000, 0});  // 3.75f
        CHECK(jit.GetVector(3) == Vector{0xaabbccdd, 0});
        CHECK(jit.GetRegister(5) == 0x01020304);
        CHECK(jit.GetRegister(7) == 0x0123456789abcdef);
        CHECK(jit.GetVector(17) == Vector{0x3fc00000, 0});  // 1.5f
        CHECK(jit.GetRegister(8) == 0x3f400000);
        umovs[lanes] = {count(jit, 0x0e043c00), count(jit, 0x4e083c00)};  // umov wd, vn.s[0]; umov xd, vn.d[0]
    }
    CHECK(umovs[true][0] < umovs[false][0]);
    CHECK(umovs[true][1] < umovs[false][1]);
}

// Venture: scalars known zero above their width (as x64's IsZeroAboveScalar):
// a scalar FP result written back to a vector register stays in its FPR (no
// FMOV to a GPR and back), aliased when nothing else needs its register and
// an in-register FMOV D otherwise.
TEST_CASE("arm64: scalar FP results stay in FPRs", "[a64][arm64][upperzero]") {
    const auto count = [](A64::Jit& jit) {
        std::size_t n = 0;
        const auto* words = static_cast<const u32*>(jit.CodeCacheBegin());
        for (std::size_t i = 0; i < jit.CodeCacheSize() / sizeof(u32); i++) {
            const u32 w = words[i] & 0xfffffc00;
            n += w == 0x9e660000 || w == 0x9e670000 || w == 0x1e260000 || w == 0x1e270000;  // fmov x/w <-> d/s
        }
        return n;
    };
    Arena arena;
    A64Env env{arena};
    A64::Jit jit{MakeConfig(env, false)};
    env.jit = &jit;
    env.Load({STOP});
    REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
    const std::size_t baseline = count(jit);

    env.Load({
        0x1e222820,  // fadd s0, s1, s2
        0x1e252883,  // fadd s3, s4, s5
        0x1e230866,  // fmul s6, s3, s3
        0x1e712a07,  // fadd d7, d16, d17
        STOP,
    });
    const u64 junk = 0x5555aaaa5555aaaa;
    jit.SetVector(1, {0xdeadbeef'3fc00000, junk});  // 1.5f
    jit.SetVector(2, {0xfeedface'40100000, junk});  // 2.25f
    jit.SetVector(4, {0x12345678'3f800000, junk});  // 1.0f
    jit.SetVector(5, {0x9abcdef0'40000000, junk});  // 2.0f
    jit.SetVector(16, {0x3ff8000000000000, junk});  // 1.5
    jit.SetVector(17, {0x4002000000000000, junk});  // 2.25
    for (const std::size_t v : {0, 3, 6, 7}) {
        jit.SetVector(v, {~u64(0), ~u64(0)});
    }
    REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
    CHECK(jit.GetVector(0) == Vector{0x40700000, 0});          // 3.75f
    CHECK(jit.GetVector(3) == Vector{0x40400000, 0});          // 3.0f
    CHECK(jit.GetVector(6) == Vector{0x41100000, 0});          // 9.0f
    CHECK(jit.GetVector(7) == Vector{0x400e000000000000, 0});  // 3.75
    CHECK(count(jit) == baseline);
}

// Venture: FP select (x64's blend): a guest FCSEL whose operands are in FPRs
// is a host FCSEL, not a CSEL between GPR copies.
TEST_CASE("arm64: FP selects are FCSEL", "[a64][arm64][fcsel]") {
    const auto count = [](A64::Jit& jit, u32 mask, u32 pattern) {
        std::size_t n = 0;
        const auto* words = static_cast<const u32*>(jit.CodeCacheBegin());
        for (std::size_t i = 0; i < jit.CodeCacheSize() / sizeof(u32); i++) {
            n += (words[i] & mask) == pattern;
        }
        return n;
    };
    for (const bool greater : {false, true}) {
        INFO("greater " << greater);
        Arena arena;
        A64Env env{arena};
        A64::Jit jit{MakeConfig(env, false)};
        env.jit = &jit;
        env.Load({
            0x1e222020,  // fcmp s1, s2
            0x1e24cc60,  // fcsel s0, s3, s4, gt
            0x1e67bcc5,  // fcsel d5, d6, d7, lt
            STOP,
        });
        const u64 junk = 0x5555aaaa5555aaaa;
        jit.SetVector(1, {greater ? 0x40000000u : 0x3f800000u, junk});  // 2.0f or 1.0f
        jit.SetVector(2, {0x3fc00000, junk});                            // 1.5f
        jit.SetVector(3, {0x11111111'33333333, junk});
        jit.SetVector(4, {0x22222222'44444444, junk});
        jit.SetVector(6, {0x6666666666666666, junk});
        jit.SetVector(7, {0x7777777777777777, junk});
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        CHECK(jit.GetVector(0) == Vector{greater ? 0x33333333u : 0x44444444u, 0});
        CHECK(jit.GetVector(5) == Vector{greater ? u64(0x7777777777777777) : u64(0x6666666666666666), 0});
        CHECK(count(jit, 0xffe00c00, 0x1e200c00) >= 1);  // fcsel s
        CHECK(count(jit, 0xffe00c00, 0x1e600c00) >= 1);  // fcsel d
    }
}

// Venture: a CBZ/CBNZ/TBZ/TBNZ block branches on its operand register (as
// x64 fuses the boolean with its CheckBit terminal) instead of storing a check
// bit on the stack and reloading it.
TEST_CASE("arm64: CheckBit terminals test the operand", "[a64][arm64][checkbit]") {
    const auto count = [](A64::Jit& jit) {
        std::size_t n = 0;
        const auto* words = static_cast<const u32*>(jit.CodeCacheBegin());
        for (std::size_t i = 0; i < jit.CodeCacheSize() / sizeof(u32); i++) {
            const u32 w = words[i] & 0xffc003e0;
            n += w == 0x394003e0 || w == 0x390003e0;  // ldrb/strb wt, [sp, #imm]
        }
        return n;
    };
    std::size_t baseline = 0;
    {
        Arena arena;
        A64Env env{arena};
        A64::Jit jit{MakeConfig(env, false)};
        env.jit = &jit;
        env.Load({STOP});
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        baseline = count(jit);
    }
    struct Case {
        u32 branch;
        std::size_t reg;
        u64 value;
        bool taken;
    };
    const std::array<Case, 9> cases{{
        {0x34000061, 1, 0, true},                     // cbz w1
        {0x34000061, 1, 0x100000000, true},           // cbz w1: upper half ignored
        {0x34000061, 1, 5, false},                    // cbz w1
        {0xb5000063, 3, 0, false},                    // cbnz x3
        {0xb5000063, 3, u64(1) << 63, true},          // cbnz x3
        {0xb6400062, 2, ~(u64(1) << 40), true},       // tbz x2, #40
        {0xb6400062, 2, u64(1) << 40, false},         // tbz x2, #40
        {0x37180062, 2, 8, true},                     // tbnz w2, #3
        {0x37180062, 2, ~u64(8), false},              // tbnz w2, #3
    }};
    for (const auto& c : cases) {
        INFO("branch " << std::hex << c.branch << " value " << c.value);
        Arena arena;
        A64Env env{arena};
        A64::Jit jit{MakeConfig(env, false)};
        env.jit = &jit;
        env.Load({
            c.branch,    // to 12
            0xd2800029,  // movz x9, #1
            STOP,
            0xd2800049,  // movz x9, #2
            STOP,
        });
        jit.SetRegister(c.reg, c.value);
        REQUIRE(RunFrom(jit, 0) == HaltReason::UserDefined2);
        CHECK(jit.GetRegister(9) == (c.taken ? 2 : 1));
        CHECK(count(jit) == baseline);
    }
}
