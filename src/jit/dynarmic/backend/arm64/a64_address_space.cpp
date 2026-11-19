// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <algorithm>
#include <cstring>
#include <fmt/format.h>
#include "dynarmic/backend/perf_map.h"
#include "dynarmic/backend/arm64/a64_address_space.h"

#include "dynarmic/backend/arm64/a64_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/devirtualize.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/stack_layout.h"
#include "dynarmic/common/cast_util.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/backend/exclusive_monitor_friend.h"
#include "dynarmic/ir/opt_passes.h"

namespace Dynarmic::Backend::Arm64 {

template<auto mfp, typename T>
static void* EmitCallReadTrampoline(oaknut::CodeGenerator& code, T* this_, size_t bitsize) {
    using namespace oaknut::util;
    const auto info = Devirtualize<mfp>(this_);
    oaknut::Label l_addr, l_this;
    void* target = code.xptr<void*>();
    // params = { this, vaddr, bitsize }
    code.LDR(X0, l_this);
    // X1 = vaddr
    code.MOV(X2, bitsize);
    code.LDR(Xscratch0, l_addr);
    code.BR(Xscratch0);
    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);
    return target;
}

template<auto mfp, typename T>
static void* EmitCallWriteTrampoline(oaknut::CodeGenerator& code, T* this_, size_t bitsize) {
    using namespace oaknut::util;
    const auto info = Devirtualize<mfp>(this_);
    oaknut::Label l_addr, l_this;
    void* target = code.xptr<void*>();
    // params = { this, vaddr, value, bitsize }
    code.LDR(X0, l_this);
    // X1 = vaddr
    // X2 = value
    code.MOV(X3, bitsize);
    code.LDR(Xscratch0, l_addr);
    code.BR(Xscratch0);
    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);
    return target;
}

template<auto mfp, typename T>
static void* EmitCallTrampoline(oaknut::CodeGenerator& code, T* this_) {
    using namespace oaknut::util;
    const auto info = Devirtualize<mfp>(this_);
    oaknut::Label l_addr, l_this;
    void* target = code.xptr<void*>();
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BR(Xscratch0);
    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);
    return target;
}

template<auto mfp, typename T>
static void* EmitWrappedReadCallTrampoline(oaknut::CodeGenerator& code, T* this_, size_t bitsize) {
    using namespace oaknut::util;

    const auto info = Devirtualize<mfp>(this_);

    oaknut::Label l_addr, l_this;

    constexpr u64 save_regs = ABI_CALLER_SAVE & ~ToRegList(Xscratch0);

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, save_regs, 0);
    code.LDR(X0, l_this);
    code.MOV(X1, Xscratch0);
    code.MOV(X2, bitsize);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    code.MOV(Xscratch0, X0);
    ABI_PopRegisters(code, save_regs, 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);

    return target;
}

template<auto callback, typename T>
static void* EmitExclusiveReadCallTrampoline(oaknut::CodeGenerator& code, const A64::UserConfig& conf) {
    using namespace oaknut::util;

    oaknut::Label l_addr, l_this;

    auto fn = [](const A64::UserConfig& conf, A64::VAddr vaddr) -> T {
        return conf.global_monitor->ReadAndMark<T>(conf.processor_id, vaddr, [&]() -> T {
            return (conf.callbacks->*callback)(vaddr, sizeof(T));
        });
    };

    void* target = code.xptr<void*>();
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BR(Xscratch0);

    code.align(8);
    code.l(l_this);
    code.dx(std::bit_cast<u64>(&conf));
    code.l(l_addr);
    code.dx(std::bit_cast<u64>(Common::FptrCast(fn)));

    return target;
}

template<auto mfp, typename T>
static void* EmitWrappedWriteCallTrampoline(oaknut::CodeGenerator& code, T* this_, size_t bitsize) {
    using namespace oaknut::util;

    const auto info = Devirtualize<mfp>(this_);

    oaknut::Label l_addr, l_this;

    constexpr u64 save_regs = ABI_CALLER_SAVE;

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, save_regs, 0);
    code.LDR(X0, l_this);
    code.MOV(X1, Xscratch0);
    code.MOV(X2, Xscratch1);
    code.MOV(X3, bitsize);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    ABI_PopRegisters(code, save_regs, 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);

    return target;
}

// Venture: the fallback of an inline exclusive store (emit_arm64_memory.cpp):
// Xscratch0 = vaddr, Xscratch1 = value, compared with the value this CPU's
// exclusive load recorded. Returns Xscratch0 = 0 when stored, else 1, and
// preserves every other caller-saved register.
template<auto callback, typename T>
static void* EmitInlineExclusiveWriteFallbackTrampoline(oaknut::CodeGenerator& code, const A64::UserConfig& conf) {
    using namespace oaknut::util;

    oaknut::Label l_addr, l_this;
    auto fn = [](const A64::UserConfig& conf, A64::VAddr vaddr, T value) -> u64 {
        T expected;
        std::memcpy(&expected, GetExclusiveMonitorValuePointer(conf.global_monitor, conf.processor_id), sizeof(T));
        return (conf.callbacks->*callback)(vaddr, value, expected) ? 0 : 1;
    };

    constexpr u64 save_regs = ABI_CALLER_SAVE & ~ToRegList(Xscratch0);

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, save_regs, 0);
    code.LDR(X0, l_this);
    code.MOV(X1, Xscratch0);
    code.MOV(X2, Xscratch1);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    code.MOV(Xscratch0, X0);
    ABI_PopRegisters(code, save_regs, 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(std::bit_cast<u64>(&conf));
    code.l(l_addr);
    code.dx(std::bit_cast<u64>(Common::FptrCast(fn)));

    return target;
}

// Venture: the fallback of a 128-bit inline exclusive store: Xscratch0 = vaddr,
// Q0 = value, compared with the value this CPU's exclusive load recorded.
// Returns Xscratch0 = 0 when stored, else 1, and preserves every other
// caller-saved register.
static void* EmitInlineExclusiveWrite128FallbackTrampoline(oaknut::CodeGenerator& code, const A64::UserConfig& conf) {
    using namespace oaknut::util;

    oaknut::Label l_addr, l_this;
    auto fn = [](const A64::UserConfig& conf, A64::VAddr vaddr, u64 value_lo, u64 value_hi) -> u64 {
        const Vector expected = *GetExclusiveMonitorValuePointer(conf.global_monitor, conf.processor_id);
        return conf.callbacks->MemoryWriteExclusive128(vaddr, Vector{value_lo, value_hi}, expected) ? 0 : 1;
    };

    constexpr u64 save_regs = ABI_CALLER_SAVE & ~ToRegList(Xscratch0);

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, save_regs, 0);
    code.LDR(X0, l_this);
    code.MOV(X1, Xscratch0);
    code.FMOV(X2, D0);
    code.FMOV(X3, V0.D()[1]);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    code.MOV(Xscratch0, X0);
    ABI_PopRegisters(code, save_regs, 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(std::bit_cast<u64>(&conf));
    code.l(l_addr);
    code.dx(std::bit_cast<u64>(Common::FptrCast(fn)));

    return target;
}

template<auto callback, typename T>
static void* EmitExclusiveWriteCallTrampoline(oaknut::CodeGenerator& code, const A64::UserConfig& conf) {
    using namespace oaknut::util;

    oaknut::Label l_addr, l_this;
    auto fn = [](const A64::UserConfig& conf, A64::VAddr vaddr, T value) -> u32 {
        return conf.global_monitor->DoExclusiveOperation<T>(conf.processor_id, vaddr, [&](T expected) -> bool {
            return (conf.callbacks->*callback)(vaddr, value, expected);
        }) ? 0 : 1;
    };

    void* target = code.xptr<void*>();
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BR(Xscratch0);

    code.align(8);
    code.l(l_this);
    code.dx(std::bit_cast<u64>(&conf));
    code.l(l_addr);
    code.dx(std::bit_cast<u64>(Common::FptrCast(fn)));

    return target;
}

static void* EmitRead128CallTrampoline(oaknut::CodeGenerator& code, A64::UserCallbacks* this_) {
    using namespace oaknut::util;

    const auto info = Devirtualize<&A64::UserCallbacks::MemoryRead128>(this_);

    oaknut::Label l_addr, l_this;

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, (1ull << 29) | (1ull << 30), 0);
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    code.FMOV(D0, X0);
    code.FMOV(V0.D()[1], X1);
    ABI_PopRegisters(code, (1ull << 29) | (1ull << 30), 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);

    return target;
}

static void* EmitWrappedRead128CallTrampoline(oaknut::CodeGenerator& code, A64::UserCallbacks* this_) {
    using namespace oaknut::util;

    const auto info = Devirtualize<&A64::UserCallbacks::MemoryRead128>(this_);

    oaknut::Label l_addr, l_this;

    constexpr u64 save_regs = ABI_CALLER_SAVE & ~ToRegList(Q0);

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, save_regs, 0);
    code.LDR(X0, l_this);
    code.MOV(X1, Xscratch0);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    code.FMOV(D0, X0);
    code.FMOV(V0.D()[1], X1);
    ABI_PopRegisters(code, save_regs, 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);

    return target;
}

static void* EmitExclusiveRead128CallTrampoline(oaknut::CodeGenerator& code, const A64::UserConfig& conf) {
    using namespace oaknut::util;

    oaknut::Label l_addr, l_this;

    auto fn = [](const A64::UserConfig& conf, A64::VAddr vaddr) -> Vector {
        return conf.global_monitor->ReadAndMark<Vector>(conf.processor_id, vaddr, [&]() -> Vector {
            return conf.callbacks->MemoryRead128(vaddr);
        });
    };

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, (1ull << 29) | (1ull << 30), 0);
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    code.FMOV(D0, X0);
    code.FMOV(V0.D()[1], X1);
    ABI_PopRegisters(code, (1ull << 29) | (1ull << 30), 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(std::bit_cast<u64>(&conf));
    code.l(l_addr);
    code.dx(std::bit_cast<u64>(Common::FptrCast(fn)));

    return target;
}

static void* EmitWrite128CallTrampoline(oaknut::CodeGenerator& code, A64::UserCallbacks* this_) {
    using namespace oaknut::util;

    const auto info = Devirtualize<&A64::UserCallbacks::MemoryWrite128>(this_);

    oaknut::Label l_addr, l_this;

    void* target = code.xptr<void*>();
    code.LDR(X0, l_this);
    code.FMOV(X2, D0);
    code.FMOV(X3, V0.D()[1]);
    code.LDR(Xscratch0, l_addr);
    code.BR(Xscratch0);

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);

    return target;
}

static void* EmitWrappedWrite128CallTrampoline(oaknut::CodeGenerator& code, A64::UserCallbacks* this_) {
    using namespace oaknut::util;

    const auto info = Devirtualize<&A64::UserCallbacks::MemoryWrite128>(this_);

    oaknut::Label l_addr, l_this;

    constexpr u64 save_regs = ABI_CALLER_SAVE;

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, save_regs, 0);
    code.LDR(X0, l_this);
    code.MOV(X1, Xscratch0);
    code.FMOV(X2, D0);
    code.FMOV(X3, V0.D()[1]);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    ABI_PopRegisters(code, save_regs, 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);

    return target;
}

static void* EmitExclusiveWrite128CallTrampoline(oaknut::CodeGenerator& code, const A64::UserConfig& conf) {
    using namespace oaknut::util;

    oaknut::Label l_addr, l_this;

    auto fn = [](const A64::UserConfig& conf, A64::VAddr vaddr, Vector value) -> u32 {
        return conf.global_monitor->DoExclusiveOperation<Vector>(conf.processor_id, vaddr,
                                                                 [&](Vector expected) -> bool {
                                                                     return conf.callbacks->MemoryWriteExclusive128(vaddr, value, expected);
                                                                 })
                 ? 0
                 : 1;
    };

    void* target = code.xptr<void*>();
    code.LDR(X0, l_this);
    code.FMOV(X2, D0);
    code.FMOV(X3, V0.D()[1]);
    code.LDR(Xscratch0, l_addr);
    code.BR(Xscratch0);

    code.align(8);
    code.l(l_this);
    code.dx(std::bit_cast<u64>(&conf));
    code.l(l_addr);
    code.dx(std::bit_cast<u64>(Common::FptrCast(fn)));

    return target;
}

// Venture: LSE atomics through UserCallbacks::MemoryAtomic. X1 = vaddr, X2 = value
// (the expected value for CAS), X3 = the desired value (CAS), X4 = op | bytes << 8.
// With the global monitor the access also breaks every reservation, as on x64.
static u64 AtomicCall(const A64::UserConfig& conf, A64::VAddr vaddr, u64 value, u64 desired, u64 op_and_bytes) {
    const auto op = static_cast<A64::AtomicOp>(op_and_bytes & 0xff);
    const std::size_t bytes = op_and_bytes >> 8;
    const u64 mask = bytes == 8 ? ~u64{0} : (u64{1} << (bytes * 8)) - 1;
    const auto call = [&]() -> u64 {
        return conf.callbacks->MemoryAtomic(vaddr, bytes, op, value & mask, desired & mask) & mask;
    };
    if (!conf.global_monitor || conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor)) {
        return call();
    }
    return conf.global_monitor->DoAtomicOperation(call);
}

static void* EmitAtomicCallTrampoline(oaknut::CodeGenerator& code, const A64::UserConfig& conf) {
    using namespace oaknut::util;

    oaknut::Label l_addr, l_this;
    auto fn = &AtomicCall;

    void* target = code.xptr<void*>();
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BR(Xscratch0);

    code.align(8);
    code.l(l_this);
    code.dx(std::bit_cast<u64>(&conf));
    code.l(l_addr);
    code.dx(std::bit_cast<u64>(Common::FptrCast(fn)));

    return target;
}

// Venture: the fallback of an inline atomic (emit_arm64_memory.cpp): Xscratch0 =
// vaddr, Xscratch1 = a stack block of {value or expected, desired, op | bytes << 8}.
// Returns the old value in Xscratch0 and preserves every other caller-saved register.
static void* EmitWrappedAtomicCallTrampoline(oaknut::CodeGenerator& code, const A64::UserConfig& conf) {
    using namespace oaknut::util;

    oaknut::Label l_addr, l_this;
    constexpr u64 save_regs = ABI_CALLER_SAVE & ~ToRegList(Xscratch0);

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, save_regs, 0);
    code.LDR(X0, l_this);
    code.MOV(X1, Xscratch0);
    code.LDP(X2, X3, Xscratch1);
    code.LDR(X4, Xscratch1, 16);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    code.MOV(Xscratch0, X0);
    ABI_PopRegisters(code, save_regs, 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(std::bit_cast<u64>(&conf));
    code.l(l_addr);
    code.dx(std::bit_cast<u64>(Common::FptrCast(&AtomicCall)));

    return target;
}

// Venture: CASP (128-bit). X1 = vaddr, Q0 = expected, Q1 = desired; the old value returns in Q0.
static void* EmitAtomicCompareAndSwap128CallTrampoline(oaknut::CodeGenerator& code, const A64::UserConfig& conf) {
    using namespace oaknut::util;

    oaknut::Label l_addr, l_this;
    auto fn = [](const A64::UserConfig& conf, A64::VAddr vaddr, u64 expected_lo, u64 expected_hi, u64 desired_lo, u64 desired_hi) -> Vector {
        const auto call = [&]() -> Vector {
            return conf.callbacks->MemoryAtomicCompareAndSwap128(vaddr, Vector{expected_lo, expected_hi}, Vector{desired_lo, desired_hi});
        };
        if (!conf.global_monitor || conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor)) {
            return call();
        }
        return conf.global_monitor->DoAtomicOperation(call);
    };

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, (1ull << 29) | (1ull << 30), 0);
    code.LDR(X0, l_this);
    code.FMOV(X2, D0);
    code.FMOV(X3, V0.D()[1]);
    code.FMOV(X4, D1);
    code.FMOV(X5, V1.D()[1]);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    code.FMOV(D0, X0);
    code.FMOV(V0.D()[1], X1);
    ABI_PopRegisters(code, (1ull << 29) | (1ull << 30), 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(std::bit_cast<u64>(&conf));
    code.l(l_addr);
    code.dx(std::bit_cast<u64>(Common::FptrCast(fn)));

    return target;
}

A64AddressSpace::A64AddressSpace(const A64::UserConfig& conf)
        : AddressSpace(conf.code_cache_size)
        , conf(conf) {
    EmitPrelude();
}

void A64AddressSpace::GenerateIR(IR::Block& ir_block, IR::LocationDescriptor descriptor) const {
    ir_block.Reset(descriptor);
    const auto get_code = [this](u64 vaddr) { return conf.callbacks->MemoryReadCode(vaddr); };
    inlined_code.clear();
    A64::Translate(ir_block, A64::LocationDescriptor{descriptor}, get_code,
                   {.define_unpredictable_behaviour = conf.define_unpredictable_behaviour,
                    .wall_clock_cntpct = conf.wall_clock_cntpct,
                    .inline_outlined_atomics = conf.inline_outlined_atomics && !conf.diagnostic_entry.record,
                    .inlined_code = &inlined_code,
                    .diagnostic_boundary = conf.diagnostic_entry.record ? conf.diagnostic_entry.selected : std::function<bool(u64)>{}});
    // Venture: without the SHA2 instructions the SHA256 opcodes become plain
    // IR, as on x64 without SHA-NI.
    Optimization::Optimize(ir_block, conf, {.sha256 = !HasHostFeature(HostFeatures(conf.disabled_host_features), HostFeature::SHA256)});
}

void A64AddressSpace::InvalidateCacheRanges(const boost::icl::interval_set<u64>& ranges) {
    InvalidateBasicBlocks(block_ranges.InvalidateRanges(ranges));
}

void A64AddressSpace::EmitPrelude() {
    using namespace oaknut::util;

    UnprotectCodeMemory();

    if (conf.HasOptimization(OptimizationFlag::FastDispatch)) {
        fast_dispatch_bits = std::clamp<unsigned>(conf.fast_dispatch_table_bits, 10, 24);
        fast_dispatch_table.Allocate(std::size_t{1} << fast_dispatch_bits);
        if (conf.dispatch_site_cache && !conf.fast_dispatch_census) {
            dispatch_sites.resize(dispatch_site_count);
        }
    }

    prelude_info.read_memory_8 = EmitCallReadTrampoline<&A64::UserCallbacks::MemoryRead>(code, conf.callbacks, sizeof(u8));
    prelude_info.read_memory_16 = EmitCallReadTrampoline<&A64::UserCallbacks::MemoryRead>(code, conf.callbacks, sizeof(u16));
    prelude_info.read_memory_32 = EmitCallReadTrampoline<&A64::UserCallbacks::MemoryRead>(code, conf.callbacks, sizeof(u32));
    prelude_info.read_memory_64 = EmitCallReadTrampoline<&A64::UserCallbacks::MemoryRead>(code, conf.callbacks, sizeof(u64));
    prelude_info.read_memory_128 = EmitRead128CallTrampoline(code, conf.callbacks);
    prelude_info.wrapped_read_memory_8 = EmitWrappedReadCallTrampoline<&A64::UserCallbacks::MemoryRead>(code, conf.callbacks, sizeof(u8));
    prelude_info.wrapped_read_memory_16 = EmitWrappedReadCallTrampoline<&A64::UserCallbacks::MemoryRead>(code, conf.callbacks, sizeof(u16));
    prelude_info.wrapped_read_memory_32 = EmitWrappedReadCallTrampoline<&A64::UserCallbacks::MemoryRead>(code, conf.callbacks, sizeof(u32));
    prelude_info.wrapped_read_memory_64 = EmitWrappedReadCallTrampoline<&A64::UserCallbacks::MemoryRead>(code, conf.callbacks, sizeof(u64));
    prelude_info.wrapped_read_memory_128 = EmitWrappedRead128CallTrampoline(code, conf.callbacks);
    prelude_info.exclusive_read_memory_8 = EmitExclusiveReadCallTrampoline<&A64::UserCallbacks::MemoryRead, u8>(code, conf);
    prelude_info.exclusive_read_memory_16 = EmitExclusiveReadCallTrampoline<&A64::UserCallbacks::MemoryRead, u16>(code, conf);
    prelude_info.exclusive_read_memory_32 = EmitExclusiveReadCallTrampoline<&A64::UserCallbacks::MemoryRead, u32>(code, conf);
    prelude_info.exclusive_read_memory_64 = EmitExclusiveReadCallTrampoline<&A64::UserCallbacks::MemoryRead, u64>(code, conf);
    prelude_info.exclusive_read_memory_128 = EmitExclusiveRead128CallTrampoline(code, conf);
    prelude_info.write_memory_8 = EmitCallWriteTrampoline<&A64::UserCallbacks::MemoryWrite>(code, conf.callbacks, sizeof(u8));
    prelude_info.write_memory_16 = EmitCallWriteTrampoline<&A64::UserCallbacks::MemoryWrite>(code, conf.callbacks, sizeof(u16));
    prelude_info.write_memory_32 = EmitCallWriteTrampoline<&A64::UserCallbacks::MemoryWrite>(code, conf.callbacks, sizeof(u32));
    prelude_info.write_memory_64 = EmitCallWriteTrampoline<&A64::UserCallbacks::MemoryWrite>(code, conf.callbacks, sizeof(u64));
    prelude_info.write_memory_128 = EmitWrite128CallTrampoline(code, conf.callbacks);
    prelude_info.wrapped_write_memory_8 = EmitWrappedWriteCallTrampoline<&A64::UserCallbacks::MemoryWrite>(code, conf.callbacks, sizeof(u8));
    prelude_info.wrapped_write_memory_16 = EmitWrappedWriteCallTrampoline<&A64::UserCallbacks::MemoryWrite>(code, conf.callbacks, sizeof(u16));
    prelude_info.wrapped_write_memory_32 = EmitWrappedWriteCallTrampoline<&A64::UserCallbacks::MemoryWrite>(code, conf.callbacks, sizeof(u32));
    prelude_info.wrapped_write_memory_64 = EmitWrappedWriteCallTrampoline<&A64::UserCallbacks::MemoryWrite>(code, conf.callbacks, sizeof(u64));
    prelude_info.wrapped_write_memory_128 = EmitWrappedWrite128CallTrampoline(code, conf.callbacks);
    prelude_info.exclusive_write_memory_8 = EmitExclusiveWriteCallTrampoline<&A64::UserCallbacks::MemoryWriteExclusive8, u8>(code, conf);
    prelude_info.exclusive_write_memory_16 = EmitExclusiveWriteCallTrampoline<&A64::UserCallbacks::MemoryWriteExclusive16, u16>(code, conf);
    prelude_info.exclusive_write_memory_32 = EmitExclusiveWriteCallTrampoline<&A64::UserCallbacks::MemoryWriteExclusive32, u32>(code, conf);
    prelude_info.exclusive_write_memory_64 = EmitExclusiveWriteCallTrampoline<&A64::UserCallbacks::MemoryWriteExclusive64, u64>(code, conf);
    if (conf.global_monitor) {
        prelude_info.inline_exclusive_write_fallback_8 = EmitInlineExclusiveWriteFallbackTrampoline<&A64::UserCallbacks::MemoryWriteExclusive8, u8>(code, conf);
        prelude_info.inline_exclusive_write_fallback_16 = EmitInlineExclusiveWriteFallbackTrampoline<&A64::UserCallbacks::MemoryWriteExclusive16, u16>(code, conf);
        prelude_info.inline_exclusive_write_fallback_32 = EmitInlineExclusiveWriteFallbackTrampoline<&A64::UserCallbacks::MemoryWriteExclusive32, u32>(code, conf);
        prelude_info.inline_exclusive_write_fallback_64 = EmitInlineExclusiveWriteFallbackTrampoline<&A64::UserCallbacks::MemoryWriteExclusive64, u64>(code, conf);
        prelude_info.inline_exclusive_write_fallback_128 = EmitInlineExclusiveWrite128FallbackTrampoline(code, conf);
    }
    prelude_info.exclusive_write_memory_128 = EmitExclusiveWrite128CallTrampoline(code, conf);
    prelude_info.atomic_memory = EmitAtomicCallTrampoline(code, conf);
    prelude_info.wrapped_atomic_memory = EmitWrappedAtomicCallTrampoline(code, conf);
    prelude_info.atomic_compare_and_swap_128 = EmitAtomicCompareAndSwap128CallTrampoline(code, conf);
    prelude_info.call_svc = EmitCallTrampoline<&A64::UserCallbacks::CallSVC>(code, conf.callbacks);
    prelude_info.call_svc_fast = EmitCallTrampoline<&A64::UserCallbacks::CallSVCFast>(code, conf.callbacks);
    prelude_info.call_svc_fast_known = EmitCallTrampoline<&A64::UserCallbacks::CallSVCFastKnown>(code, conf.callbacks);
    prelude_info.exception_raised = EmitCallTrampoline<&A64::UserCallbacks::ExceptionRaised>(code, conf.callbacks);
    prelude_info.isb_raised = EmitCallTrampoline<&A64::UserCallbacks::InstructionSynchronizationBarrierRaised>(code, conf.callbacks);
    prelude_info.ic_raised = EmitCallTrampoline<&A64::UserCallbacks::InstructionCacheOperationRaised>(code, conf.callbacks);
    prelude_info.dc_raised = EmitCallTrampoline<&A64::UserCallbacks::DataCacheOperationRaised>(code, conf.callbacks);
    prelude_info.get_cntpct = EmitCallTrampoline<&A64::UserCallbacks::GetCNTPCT>(code, conf.callbacks);
    prelude_info.add_ticks = EmitCallTrampoline<&A64::UserCallbacks::AddTicks>(code, conf.callbacks);
    prelude_info.get_ticks_remaining = EmitCallTrampoline<&A64::UserCallbacks::GetTicksRemaining>(code, conf.callbacks);

    oaknut::Label return_from_run_code, l_return_to_dispatcher;

    prelude_info.run_code = code.xptr<PreludeInfo::RunCodeFuncType>();
    {
        ABI_PushRegisters(code, ABI_CALLEE_SAVE | (1 << 30), sizeof(StackLayout));

        code.MOV(X19, X0);
        code.MOV(Xstate, X1);
        code.MOV(Xhalt, X2);
        if (conf.page_table) {
            code.MOV(Xpagetable, std::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer) {
            code.MOV(Xfastmem, *conf.fastmem_pointer);
        }

        if (conf.HasOptimization(OptimizationFlag::ReturnStackBuffer)) {
            code.LDR(Xscratch0, l_return_to_dispatcher);
            for (std::size_t i = 0; i < A64RSBCount; i++) {
                code.STR(Xscratch0, SP, offsetof(StackLayout, rsb) + offsetof(RSBEntry, code_ptr) + i * sizeof(RSBEntry));
            }
        }

        if (conf.enable_cycle_counting) {
            code.BL(prelude_info.get_ticks_remaining);
            code.MOV(Xticks, X0);
            code.STR(Xticks, SP, offsetof(StackLayout, cycles_to_run));
        }

        code.MRS(Xscratch1, oaknut::SystemReg::FPCR);
        code.STR(Wscratch1, SP, offsetof(StackLayout, save_host_fpcr));
        code.LDR(Wscratch0, Xstate, offsetof(A64JitState, fpcr));
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);

        code.MOV(Xscratch2, X19);
        EmitRegisterCacheLoad();
        code.LDAR(Wscratch0, Xhalt);
        code.CBNZ(Wscratch0, return_from_run_code);

        code.BR(Xscratch2);
    }

    prelude_info.step_code = code.xptr<PreludeInfo::RunCodeFuncType>();
    {
        ABI_PushRegisters(code, ABI_CALLEE_SAVE | (1 << 30), sizeof(StackLayout));

        code.MOV(X19, X0);
        code.MOV(Xstate, X1);
        code.MOV(Xhalt, X2);
        if (conf.page_table) {
            code.MOV(Xpagetable, std::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer) {
            code.MOV(Xfastmem, *conf.fastmem_pointer);
        }

        if (conf.HasOptimization(OptimizationFlag::ReturnStackBuffer)) {
            code.LDR(Xscratch0, l_return_to_dispatcher);
            for (std::size_t i = 0; i < A64RSBCount; i++) {
                code.STR(Xscratch0, SP, offsetof(StackLayout, rsb) + offsetof(RSBEntry, code_ptr) + i * sizeof(RSBEntry));
            }
        }

        if (conf.enable_cycle_counting) {
            code.MOV(Xticks, 1);
            code.STR(Xticks, SP, offsetof(StackLayout, cycles_to_run));
        }

        code.MRS(Xscratch1, oaknut::SystemReg::FPCR);
        code.STR(Wscratch1, SP, offsetof(StackLayout, save_host_fpcr));
        code.LDR(Wscratch0, Xstate, offsetof(A64JitState, fpcr));
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);

        code.MOV(Xscratch2, X19);
        EmitRegisterCacheLoad();

        oaknut::Label step_hr_loop;
        code.l(step_hr_loop);
        code.LDAXR(Wscratch0, Xhalt);
        code.CBNZ(Wscratch0, return_from_run_code);
        code.ORR(Wscratch0, Wscratch0, static_cast<u32>(HaltReason::Step));
        code.STLXR(Wscratch1, Wscratch0, Xhalt);
        code.CBNZ(Wscratch1, step_hr_loop);

        code.BR(Xscratch2);
    }

    prelude_info.return_to_dispatcher = code.xptr<void*>();
    {
        oaknut::Label l_this, l_addr;

        code.LDAR(Wscratch0, Xhalt);
        code.CBNZ(Wscratch0, return_from_run_code);

        if (conf.enable_cycle_counting) {
            code.CMP(Xticks, 0);
            code.B(LE, return_from_run_code);
        }

        code.LDR(X0, l_this);
        code.MOV(X1, Xstate);
        code.LDR(Xscratch0, l_addr);
        code.BLR(Xscratch0);
        if (conf.HasOptimization(OptimizationFlag::ReturnStackBuffer)) {
            EmitRSBResetAfterCompile(l_return_to_dispatcher);
        }
        code.BR(X0);

        const auto fn = [](A64AddressSpace& self, A64JitState& context) -> CodePtr {
            return self.GetOrEmit(context.GetLocationDescriptor());
        };

        code.align(8);
        code.l(l_this);
        code.dx(std::bit_cast<u64>(this));
        code.l(l_addr);
        code.dx(std::bit_cast<u64>(Common::FptrCast(fn)));
    }

    // Venture: X0 = the location descriptor of the JIT state's PC and FPCR.
    const auto emit_current_location = [&] {
        code.MOV(Wscratch0, A64::LocationDescriptor::fpcr_mask);
        code.LDR(W0, Xstate, offsetof(A64JitState, fpcr));
        code.LDR(X1, Xstate, offsetof(A64JitState, pc));
        code.AND(W0, W0, Wscratch0);
        code.AND(X1, X1, A64::LocationDescriptor::pc_mask);
        code.LSL(X0, X0, A64::LocationDescriptor::fpcr_shift);
        code.ORR(X0, X0, X1);
    };

    if (conf.HasOptimization(OptimizationFlag::FastDispatch)) {
        // Venture: the fast dispatch table probed in emitted code (as on x64
        // and for A32); a miss fills the entry through GetOrEmit.
        prelude_info.fast_dispatch = code.xptr<void*>();
        code.LDAR(Wscratch0, Xhalt);
        code.CBNZ(Wscratch0, return_from_run_code);
        if (conf.enable_cycle_counting) {
            code.CMP(Xticks, 0);
            code.B(LE, return_from_run_code);
        }
        emit_current_location();
        EmitFastDispatchProbe(l_return_to_dispatcher, conf.HasOptimization(OptimizationFlag::ReturnStackBuffer), conf.fast_dispatch_census);

        if (!dispatch_sites.empty()) {
            // Venture: dispatch_site_cache (as on x64). A site's slot missed (X0
            // guest PC, X1 slot): the table lookup, then the slot takes the PC
            // and code. A PC with bits above the location's 56 is never cached,
            // so every slot tag is a plain location of the sites' zero FPCR.
            oaknut::Label found, done;
            prelude_info.dispatch_site_miss = code.xptr<void*>();
            code.STR(X1, SP, offsetof(StackLayout, dispatch_site_slot));
            emit_current_location();
            EmitFastDispatchProbe(l_return_to_dispatcher, conf.HasOptimization(OptimizationFlag::ReturnStackBuffer), nullptr, &found);
            code.l(found);
            code.LDR(X0, Xstate, offsetof(A64JitState, pc));
            code.LSR(X2, X0, A64::LocationDescriptor::pc_bit_count);
            code.CBNZ(X2, done);
            code.LDR(X1, SP, offsetof(StackLayout, dispatch_site_slot));
            code.STP(X0, Xscratch1, X1);
            code.l(done);
            code.BR(Xscratch1);
        }
    }

    prelude_info.return_from_run_code = code.xptr<void*>();
    {
        code.l(return_from_run_code);
        EmitRegisterCacheStore();

        if (conf.enable_cycle_counting) {
            code.LDR(X1, SP, offsetof(StackLayout, cycles_to_run));
            code.SUB(X1, X1, Xticks);
            code.BL(prelude_info.add_ticks);
        }

        code.LDR(Wscratch0, SP, offsetof(StackLayout, save_host_fpcr));
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);

        oaknut::Label exit_hr_loop;
        code.l(exit_hr_loop);
        code.LDAXR(W0, Xhalt);
        code.STLXR(Wscratch0, WZR, Xhalt);
        code.CBNZ(Wscratch0, exit_hr_loop);

        ABI_PopRegisters(code, ABI_CALLEE_SAVE | (1 << 30), sizeof(StackLayout));
        code.RET();
    }

    code.align(8);
    code.l(l_return_to_dispatcher);
    code.dx(std::bit_cast<u64>(prelude_info.return_to_dispatcher));

    prelude_info.end_of_prelude = code.offset();
    ResetDispatchSites();
    PerfMapRegister(mem.ptr(), reinterpret_cast<const std::byte*>(mem.ptr()) + prelude_info.end_of_prelude, "a64_prelude");

    // Venture: only the prelude holds code. Cleaning the whole cache walked every
    // line of it (128 MiB, 80 ms in an emulated arm64 machine) for each JIT.
    mem.invalidate(mem.ptr(), prelude_info.end_of_prelude);
    ProtectCodeMemory();
}

bool A64AddressSpace::RegisterCacheEnabled() const {
    return conf.enable_register_cache && !conf.page_table && !conf.very_verbose_debugging_output;
}

void A64AddressSpace::EmitRegisterCacheLoad() {
    EmitA64RegisterCacheLoad(code, RegisterCacheEnabled());
}

void A64AddressSpace::EmitRegisterCacheStore() {
    EmitA64RegisterCacheStore(code, RegisterCacheEnabled());
}

EmitConfig A64AddressSpace::GetEmitConfig() {
    return EmitConfig{
        .optimizations = conf.unsafe_optimizations ? conf.optimizations : conf.optimizations & all_safe_optimizations,

        .hook_isb = conf.hook_isb,

        .cntfreq_el0 = conf.cntfrq_el0,
        .ctr_el0 = conf.ctr_el0,
        .dczid_el0 = conf.dczid_el0,
        .tpidrro_el0 = conf.tpidrro_el0,
        .tpidr_el0 = conf.tpidr_el0,

        .check_halt_on_memory_access = conf.check_halt_on_memory_access,

        .page_table_pointer = std::bit_cast<u64>(conf.page_table),
        .page_table_address_space_bits = conf.page_table_address_space_bits,
        .page_table_pointer_mask = conf.page_table_pointer_mask,
        .page_table_log2_stride = conf.page_table_log2_stride,
        .page_table_marked_bit = conf.page_table_marked_bit,
        .page_table_sign_extension = conf.page_table_sign_extension,
        .silently_mirror_page_table = conf.silently_mirror_page_table,
        .absolute_offset_page_table = conf.absolute_offset_page_table,
        .detect_misaligned_access_via_page_table = conf.detect_misaligned_access_via_page_table,
        .only_detect_misalignment_via_page_table_on_page_boundary = conf.only_detect_misalignment_via_page_table_on_page_boundary,

        .fastmem_pointer = conf.fastmem_pointer,
        .recompile_on_fastmem_failure = conf.recompile_on_fastmem_failure,
        .fastmem_address_space_bits = conf.fastmem_address_space_bits,
        .silently_mirror_fastmem = conf.silently_mirror_fastmem,

        .wall_clock_cntpct = conf.wall_clock_cntpct,
        .enable_cycle_counting = conf.enable_cycle_counting,

        .always_little_endian = true,

        .descriptor_to_fpcr = [](const IR::LocationDescriptor& location) { return A64::LocationDescriptor{location}.FPCR(); },
        .emit_cond = EmitA64Cond,
        .emit_condition_failed_terminal = EmitA64ConditionFailedTerminal,
        .emit_terminal = EmitA64Terminal,
        .emit_check_memory_abort = EmitA64CheckMemoryAbort,

        .state_nzcv_offset = offsetof(A64JitState, cpsr_nzcv),
        .state_fpsr_offset = offsetof(A64JitState, fpsr),
        .state_exclusive_state_offset = offsetof(A64JitState, exclusive_state),

        .coprocessors{},

        .very_verbose_debugging_output = conf.very_verbose_debugging_output,

        .svc_fast = conf.svc_fast,
        .svc_fp_transfer = conf.svc_fp_transfer,
        .svc_fp_guest = conf.svc_fp_guest,
        .svc_lookup = conf.svc_lookup,

        .global_monitor = conf.global_monitor,
        .processor_id = conf.processor_id,
        .fastmem_exclusive_access = conf.fastmem_exclusive_access,
        .recompile_on_exclusive_fastmem_failure = conf.recompile_on_exclusive_fastmem_failure,
        .elide_unchanged_exclusive_stores = conf.elide_unchanged_exclusive_stores,
        .is_a64 = true,
        .compact_block_links = conf.compact_block_links,
        .diagnostic_selected = conf.diagnostic_entry.record ? conf.diagnostic_entry.selected : std::function<bool(u64)>{},
        .diagnostic_record = conf.diagnostic_entry.record,
        .diagnostic_user = conf.diagnostic_entry.user,
        .host_features = HostFeatures(conf.disabled_host_features),

        .svc_return_link = conf.svc_return_link,
        .enable_bounded_halt_polling = conf.enable_bounded_halt_polling,
        .fast_dispatch_census = conf.fast_dispatch_census,
        .next_dispatch_site = dispatch_sites.empty() ? std::function<void*()>{} : std::function<void*()>{[this]() -> void* {
            return &dispatch_sites[next_dispatch_site++ & (dispatch_site_count - 1)];
        }},
        .cold_memory_abort_state = conf.check_halt_on_memory_access && conf.cold_memory_abort_state != A64::UserConfig::ColdAbortState::Off,
        .a64_register_cache = RegisterCacheEnabled(),
        .nzcv_exit_liveness = conf.nzcv_exit_liveness,
        .read_code = [this](u64 vaddr) { return conf.callbacks->MemoryReadCode(vaddr); },
    };
}

u64 A64AddressSpace::DescriptorPC(IR::LocationDescriptor descriptor) const {
    return A64::LocationDescriptor{descriptor}.PC();
}

std::string A64AddressSpace::FriendlyName(IR::LocationDescriptor ir_descriptor) const {
    const A64::LocationDescriptor descriptor{ir_descriptor};
    return fmt::format("a64_{:016X}_fpcr{:08X}", descriptor.PC(), descriptor.FPCR().Value());
}

void A64AddressSpace::RegisterNewBasicBlock(const IR::Block& block, const EmittedBlockInfo& block_info) {
    const A64::LocationDescriptor descriptor{block.Location()};
    const A64::LocationDescriptor end_location{block.EndLocation()};
    const auto range = boost::icl::discrete_interval<u64>::closed(descriptor.PC(), end_location.PC() - 1);
    block_ranges.AddRange(range, descriptor);
    // Venture: rewriting an outlined atomic helper translated in place invalidates its callers.
    for (const auto& [begin, end] : inlined_code)
        block_ranges.AddRange(boost::icl::discrete_interval<u64>::closed(begin, end - 1), descriptor);
    // Venture: successor code an nzcv_exit_liveness exit relies on.
    for (const auto& [first, last] : block_info.dependent_ranges)
        block_ranges.AddRange(boost::icl::discrete_interval<u64>::closed(first, last), descriptor);
}

}  // namespace Dynarmic::Backend::Arm64
