// SPDX-FileCopyrightText: Copyright 2026 Venture contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// Venture: blocks captured by one JIT install into another (portable_block.h).

#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/backend/x64/portable_block.h"
#include <cstdio>

namespace {

class PortableEnv final : public Dynarmic::A64::UserCallbacks {
public:
    explicit PortableEnv(std::uint8_t* memory) : memory(memory) {}
    std::uint8_t* memory;
    Dynarmic::A64::Jit* jit = nullptr;
    template<typename T>
    T read(std::uint64_t vaddr) {
        T value;
        std::memcpy(&value, memory + vaddr, sizeof(T));
        return value;
    }
    std::optional<std::uint32_t> MemoryReadCode(std::uint64_t vaddr) override { return read<std::uint32_t>(vaddr); }
    std::uint64_t MemoryRead(std::uint64_t vaddr, std::size_t size) override {
        std::uint64_t value = 0;
        std::memcpy(&value, memory + vaddr, size);
        return value;
    }
    Dynarmic::A64::Vector MemoryRead128(std::uint64_t vaddr) override { return read<Dynarmic::A64::Vector>(vaddr); }
    void MemoryWrite(std::uint64_t vaddr, std::uint64_t value, std::size_t size) override { std::memcpy(memory + vaddr, &value, size); }
    void MemoryWrite128(std::uint64_t vaddr, Dynarmic::A64::Vector value) override { std::memcpy(memory + vaddr, &value, 16); }
    bool MemoryWriteExclusive8(std::uint64_t vaddr, std::uint8_t value, std::uint8_t) override { return MemoryWrite(vaddr, value, 1), true; }
    bool MemoryWriteExclusive16(std::uint64_t vaddr, std::uint16_t value, std::uint16_t) override { return MemoryWrite(vaddr, value, 2), true; }
    bool MemoryWriteExclusive32(std::uint64_t vaddr, std::uint32_t value, std::uint32_t) override { return MemoryWrite(vaddr, value, 4), true; }
    bool MemoryWriteExclusive64(std::uint64_t vaddr, std::uint64_t value, std::uint64_t) override { return MemoryWrite(vaddr, value, 8), true; }
    bool MemoryWriteExclusive128(std::uint64_t vaddr, Dynarmic::A64::Vector value, Dynarmic::A64::Vector) override { return MemoryWrite128(vaddr, value), true; }
    std::uint64_t MemoryAtomic(std::uint64_t, std::size_t, Dynarmic::A64::AtomicOp, std::uint64_t, std::uint64_t) override { std::abort(); }
    Dynarmic::A64::Vector MemoryAtomicCompareAndSwap128(std::uint64_t, Dynarmic::A64::Vector, Dynarmic::A64::Vector) override { std::abort(); }
    void CallSVC(std::uint32_t) override { jit->HaltExecution(); }
    void ExceptionRaised(std::uint64_t, Dynarmic::A64::Exception) override { std::abort(); }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1'000'000; }
    std::uint64_t GetCNTPCT() override { return 0; }
};

constexpr std::uint32_t kProgram[] = {
    0xd2800002,  // 00: MOV X2, #0
    0xd2800c83,  // 04: MOV X3, #100
    0xf8408404,  // 08: LDR X4, [X0], #8
    0x8b040042,  // 0c: ADD X2, X2, X4
    0x9400000c,  // 10: BL 0x40
    0xf1000463,  // 14: SUBS X3, X3, #1
    0x54ffff81,  // 18: B.NE 0x08
    0xf9000022,  // 1c: STR X2, [X1]
    0xd4000001,  // 20: SVC #0
    0x14000000,  // 24: B .
};
constexpr std::uint32_t kCallee[] = {
    0x910004a5,  // 40: ADD X5, X5, #1
    0xd65f03c0,  // 44: RET
};

struct Result {
    std::uint64_t sum, calls, stored;
};

Result RunProgram(std::map<std::uint64_t, std::vector<std::uint8_t>>& blocks, bool capture, bool install, std::size_t& stores,
                  std::size_t& lookups, bool patched = false, std::span<const std::uint32_t> program = kProgram) {
    constexpr std::size_t size = 1 << 20;
    auto backing = std::make_unique<std::uint8_t[]>(size + 4096);
    auto* memory = reinterpret_cast<std::uint8_t*>((reinterpret_cast<std::uintptr_t>(backing.get()) + 4095) & ~std::uintptr_t(4095));
    std::memset(memory, 0, size);
    std::memcpy(memory, program.data(), program.size_bytes());
    if (program.data() == kProgram)
        std::memcpy(memory + 0x40, kCallee, sizeof(kCallee));
    if (patched) {
        const std::uint32_t add2 = 0x910008a5;  // ADD X5, X5, #2
        std::memcpy(memory + 0x40, &add2, 4);
    }
    for (std::uint64_t i = 0; i < 100; ++i) {
        const std::uint64_t value = i * 3 + 1;
        std::memcpy(memory + 0x1000 + i * 8, &value, 8);
    }

    PortableEnv env{memory};
    Dynarmic::A64::UserConfig config{};
    config.callbacks = &env;
    config.fastmem_pointer = reinterpret_cast<std::uintptr_t>(memory);
    config.fastmem_address_space_bits = 20;
    config.silently_mirror_fastmem = true;
    config.recompile_on_fastmem_failure = false;
    config.enable_cycle_counting = false;
    config.cold_code_region = true;
    config.compact_block_links = true;
    if (capture)
        config.portable_block_sink = [&](std::uint64_t descriptor, const std::uint8_t* data, std::size_t n) {
            ++stores;
            blocks[descriptor].assign(data, data + n);
        };
    if (install)
        config.portable_block_source = [&](std::uint64_t descriptor) -> std::pair<const std::uint8_t*, std::size_t> {
            const auto it = blocks.find(descriptor);
            if (it == blocks.end())
                return {nullptr, 0};
            ++lookups;
            return {it->second.data(), it->second.size()};
        };
    Dynarmic::A64::Jit jit{config};
    env.jit = &jit;
    jit.SetRegister(0, 0x1000);
    jit.SetRegister(1, 0x2000);
    jit.SetSP(0x8000);
    jit.SetPC(0);
    jit.Run();
    std::uint64_t stored;
    std::memcpy(&stored, memory + 0x2000, 8);
    return {jit.GetRegister(2), jit.GetRegister(5), stored};
}

}  // namespace

TEST_CASE("A64: portable blocks captured by one JIT run in another", "[a64]") {
    std::map<std::uint64_t, std::vector<std::uint8_t>> blocks;
    std::size_t stores = 0, lookups = 0;
    const auto reference = RunProgram(blocks, true, false, stores, lookups);
    REQUIRE(reference.sum == 14950);
    REQUIRE(reference.calls == 100);
    REQUIRE(reference.stored == 14950);
    std::printf("portable: %s\n", Dynarmic::Backend::X64::PortableStatsReport().c_str());
    REQUIRE(stores >= 3);

    std::size_t second_stores = 0;
    const auto installed = RunProgram(blocks, true, true, second_stores, lookups);
    REQUIRE(installed.sum == reference.sum);
    REQUIRE(installed.calls == reference.calls);
    REQUIRE(installed.stored == reference.stored);
    // Every block came from the first JIT; none was translated again.
    REQUIRE(lookups == stores);
    REQUIRE(second_stores == 0);
}

TEST_CASE("A64: a portable block does not install over changed guest code", "[a64]") {
    std::map<std::uint64_t, std::vector<std::uint8_t>> blocks;
    std::size_t stores = 0, lookups = 0;
    RunProgram(blocks, true, false, stores, lookups);
    std::size_t second_stores = 0;
    const auto patched = RunProgram(blocks, true, true, second_stores, lookups, true);
    REQUIRE(patched.calls == 200);
    REQUIRE(patched.sum == 14950);
    REQUIRE(second_stores >= 1);  // the callee was translated again
}

// Each block makes the first use of a fastmem fallback. Off Windows the JIT emits
// fallbacks on demand, in the middle of the block that needs one; that thunk is not
// part of the block, and the block must still be captured (it refers to the thunk by key).
TEST_CASE("A64: blocks that first need a fastmem fallback are captured", "[a64]") {
    constexpr std::uint32_t program[] = {
        0x39400006,  // 00: LDRB W6, [X0]
        0x14000001,  // 04: B 0x08
        0x79000027,  // 08: STRH W7, [X1]
        0x14000001,  // 0c: B 0x10
        0xb9400408,  // 10: LDR W8, [X0, #4]
        0x14000001,  // 14: B 0x18
        0xf9000428,  // 18: STR X8, [X1, #8]
        0xd4000001,  // 1c: SVC #0
        0x14000000,  // 20: B .
    };
    std::map<std::uint64_t, std::vector<std::uint8_t>> blocks;
    std::size_t stores = 0, lookups = 0;
    RunProgram(blocks, true, false, stores, lookups, false, program);
    // A64 descriptors are the PC with the default FPCR mode bits (zero).
    for (const std::uint64_t pc : {0x00, 0x08, 0x10, 0x18})
        CHECK(blocks.contains(pc));

    std::size_t second_stores = 0, second_lookups = 0;
    RunProgram(blocks, true, true, second_stores, second_lookups, false, program);
    CHECK(second_lookups == 4);
    CHECK(second_stores == 0);
}
