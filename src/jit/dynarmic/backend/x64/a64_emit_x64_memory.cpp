// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <array>
#include <initializer_list>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include "dynarmic/mcl/integer_of_size.hpp"
#include "dynarmic/backend/x64/xbyak.h"

#include "dynarmic/backend/x64/a64_emit_x64.h"
#include "dynarmic/backend/x64/abi.h"
#include "dynarmic/backend/x64/devirtualize.h"
#include "dynarmic/backend/x64/emit_x64_memory.h"
#include "dynarmic/backend/x64/exclusive_monitor_friend.h"
#include "dynarmic/backend/x64/perf_map.h"
#include "dynarmic/backend/x64/stack_layout.h"
#include "dynarmic/common/spin_lock_x64.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/ir/basic_block.h"

namespace Dynarmic::Backend::X64 {

using namespace Xbyak::util;

void A64EmitX64::GenMemory128Accessors() {
    code.align();
    memory_read_128 = code.getCurr<void (*)()>();
    ABIPrologue accessor_prologue_0;
    const auto* accessor_begin_0 = code.getCurr();
#ifdef _WIN32
    Devirtualize<&A64::UserCallbacks::MemoryRead128>(conf.callbacks).EmitCallWithReturnPointer(code, [&](Xbyak::Reg64 return_value_ptr, [[maybe_unused]] RegList args) {
        code.mov(code.ABI_PARAM3, code.ABI_PARAM2);
        code.lea(rsp, ptr[rsp - (8 + 16 + ABI_SHADOW_SPACE)]);
        accessor_prologue_0.operations.push_back({ABIPrologue::Kind::Allocate, static_cast<u8>(code.getCurr() - accessor_begin_0), 0, 8 + 16 + ABI_SHADOW_SPACE});
        code.lea(return_value_ptr, ptr[rsp + ABI_SHADOW_SPACE]);
    });
    code.movups(xmm1, xword[code.ABI_RETURN]);
    code.add(rsp, 8 + 16 + ABI_SHADOW_SPACE);
#else
    code.lea(rsp, ptr[rsp - 8]);
    Devirtualize<&A64::UserCallbacks::MemoryRead128>(conf.callbacks).EmitCall(code);
    if (code.HasHostFeature(HostFeature::SSE41)) {
        code.movq(xmm1, code.ABI_RETURN);
        code.pinsrq(xmm1, code.ABI_RETURN2, 1);
    } else {
        code.movq(xmm1, code.ABI_RETURN);
        code.movq(xmm2, code.ABI_RETURN2);
        code.punpcklqdq(xmm1, xmm2);
    }
    code.add(rsp, 8);
#endif
    code.ret();
    PerfMapRegister(memory_read_128, code.getCurr(), "a64_memory_read_128");
    code.RegisterNativeUnwindFrame(accessor_begin_0, code.getCurr(), accessor_prologue_0);

    code.align();
    memory_write_128 = code.getCurr<void (*)()>();
    ABIPrologue accessor_prologue_1;
    const auto* accessor_begin_1 = code.getCurr();
#ifdef _WIN32
    code.lea(rsp, ptr[rsp - (8 + 16 + ABI_SHADOW_SPACE)]);
        accessor_prologue_1.operations.push_back({ABIPrologue::Kind::Allocate, static_cast<u8>(code.getCurr() - accessor_begin_1), 0, 8 + 16 + ABI_SHADOW_SPACE});
    code.lea(code.ABI_PARAM3, ptr[rsp + ABI_SHADOW_SPACE]);
    code.movaps(xword[code.ABI_PARAM3], xmm1);
    Devirtualize<&A64::UserCallbacks::MemoryWrite128>(conf.callbacks).EmitCall(code);
    code.add(rsp, 8 + 16 + ABI_SHADOW_SPACE);
#else
    code.lea(rsp, ptr[rsp - 8]);
    if (code.HasHostFeature(HostFeature::SSE41)) {
        code.movq(code.ABI_PARAM3, xmm1);
        code.pextrq(code.ABI_PARAM4, xmm1, 1);
    } else {
        code.movq(code.ABI_PARAM3, xmm1);
        code.punpckhqdq(xmm1, xmm1);
        code.movq(code.ABI_PARAM4, xmm1);
    }
    Devirtualize<&A64::UserCallbacks::MemoryWrite128>(conf.callbacks).EmitCall(code);
    code.add(rsp, 8);
#endif
    code.ret();
    PerfMapRegister(memory_write_128, code.getCurr(), "a64_memory_write_128");
    code.RegisterNativeUnwindFrame(accessor_begin_1, code.getCurr(), accessor_prologue_1);

    code.align();
    memory_exclusive_write_128 = code.getCurr<void (*)()>();
    ABIPrologue accessor_prologue_2;
    const auto* accessor_begin_2 = code.getCurr();
#ifdef _WIN32
    code.lea(rsp, ptr[rsp - (8 + 32 + ABI_SHADOW_SPACE)]);
        accessor_prologue_2.operations.push_back({ABIPrologue::Kind::Allocate, static_cast<u8>(code.getCurr() - accessor_begin_2), 0, 8 + 32 + ABI_SHADOW_SPACE});
    code.lea(code.ABI_PARAM3, ptr[rsp + ABI_SHADOW_SPACE]);
    code.lea(code.ABI_PARAM4, ptr[rsp + ABI_SHADOW_SPACE + 16]);
    code.movaps(xword[code.ABI_PARAM3], xmm1);
    code.movaps(xword[code.ABI_PARAM4], xmm2);
    Devirtualize<&A64::UserCallbacks::MemoryWriteExclusive128>(conf.callbacks).EmitCall(code);
    code.add(rsp, 8 + 32 + ABI_SHADOW_SPACE);
#else
    code.lea(rsp, ptr[rsp - 8]);
    if (code.HasHostFeature(HostFeature::SSE41)) {
        code.movq(code.ABI_PARAM3, xmm1);
        code.pextrq(code.ABI_PARAM4, xmm1, 1);
        code.movq(code.ABI_PARAM5, xmm2);
        code.pextrq(code.ABI_PARAM6, xmm2, 1);
    } else {
        code.movq(code.ABI_PARAM3, xmm1);
        code.punpckhqdq(xmm1, xmm1);
        code.movq(code.ABI_PARAM4, xmm1);
        code.movq(code.ABI_PARAM5, xmm2);
        code.punpckhqdq(xmm2, xmm2);
        code.movq(code.ABI_PARAM6, xmm2);
    }
    Devirtualize<&A64::UserCallbacks::MemoryWriteExclusive128>(conf.callbacks).EmitCall(code);
    code.add(rsp, 8);
#endif
    code.ret();
    PerfMapRegister(memory_exclusive_write_128, code.getCurr(), "a64_memory_exclusive_write_128");
    code.RegisterNativeUnwindFrame(accessor_begin_2, code.getCurr(), accessor_prologue_2);
}

void A64EmitX64::GenFastmemFallbacks() {
    const std::initializer_list<int> idxes{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const std::array<std::pair<size_t, ArgCallback>, 4> read_callbacks{{
        {8, Devirtualize<&A64::UserCallbacks::MemoryRead>(conf.callbacks)},
        {16, Devirtualize<&A64::UserCallbacks::MemoryRead>(conf.callbacks)},
        {32, Devirtualize<&A64::UserCallbacks::MemoryRead>(conf.callbacks)},
        {64, Devirtualize<&A64::UserCallbacks::MemoryRead>(conf.callbacks)},
    }};
    const std::array<std::pair<size_t, ArgCallback>, 4> write_callbacks{{
        {8, Devirtualize<&A64::UserCallbacks::MemoryWrite>(conf.callbacks)},
        {16, Devirtualize<&A64::UserCallbacks::MemoryWrite>(conf.callbacks)},
        {32, Devirtualize<&A64::UserCallbacks::MemoryWrite>(conf.callbacks)},
        {64, Devirtualize<&A64::UserCallbacks::MemoryWrite>(conf.callbacks)},
    }};
    const std::array<std::pair<size_t, ArgCallback>, 4> exclusive_write_callbacks{{
        {8, Devirtualize<&A64::UserCallbacks::MemoryWriteExclusive8>(conf.callbacks)},
        {16, Devirtualize<&A64::UserCallbacks::MemoryWriteExclusive16>(conf.callbacks)},
        {32, Devirtualize<&A64::UserCallbacks::MemoryWriteExclusive32>(conf.callbacks)},
        {64, Devirtualize<&A64::UserCallbacks::MemoryWriteExclusive64>(conf.callbacks)},
    }};

    xmm_read_fallbacks = true;
    for (auto const ordered : {false, true}) {
        for (auto const vaddr_idx : idxes) {
            if (vaddr_idx == 4 || vaddr_idx == 15) {
                continue;
            }

            for (int value_idx : idxes) {
                code.EnsureMemoryCommitted();
                if (WantFallback(FallbackKind::Read, std::make_tuple(ordered, 128, vaddr_idx, value_idx))) {
                    code.align();
                    read_fallbacks[std::make_tuple(ordered, 128, vaddr_idx, value_idx)] = code.getCurr<void (*)()>();
                    const void* unwind_begin_0 = code.getCurr();
                    ABIPrologue unwind_prologue_0;
                    ABI_PushCallerSaveRegistersAndAdjustStackExcept(code, HostLocXmmIdx(value_idx), unwind_prologue_0.WindowsOnly());
                    if (ordered) {
                        // Venture: CMPXCHG16B accesses (see FlushA64CachedRbx) reach
                        // here with RBX borrowed; the call synchronizes the cache.
                        code.ReloadA64CachedRbx();
                    }
                    if (vaddr_idx != code.ABI_PARAM2.getIdx()) {
                        code.mov(code.ABI_PARAM2, Xbyak::Reg64{vaddr_idx});
                    }
                    if (ordered) {
                        code.mfence();
                    }
                    code.call(memory_read_128);
                    if (value_idx != 1) {
                        code.movaps(Xbyak::Xmm{value_idx}, xmm1);
                    }
                    ABI_PopCallerSaveRegistersAndAdjustStackExcept(code, HostLocXmmIdx(value_idx));
                    code.ret();
                    PerfMapRegister(read_fallbacks[std::make_tuple(ordered, 128, vaddr_idx, value_idx)], code.getCurr(), "a64_read_fallback_128");
                    code.RegisterNativeUnwindFrame(unwind_begin_0, code.getCurr(), unwind_prologue_0);
                }

                // Venture: FP loads (32/64-bit values in an XMM register); MOVD/MOVQ
                // zero the upper lanes as the fastmem load does.
                for (const size_t bitsize : {size_t(32), size_t(64)}) {
                    if (ordered)
                        break;
                    const auto key = std::make_tuple(false, bitsize | xmm_read_fallback, vaddr_idx, value_idx);
                    if (WantFallback(FallbackKind::Read, key)) {
                        code.align();
                        read_fallbacks[key] = code.getCurr<void (*)()>();
                        const void* unwind_begin = code.getCurr();
                        ABIPrologue unwind_prologue;
                        ABI_PushCallerSaveRegistersAndAdjustStackExcept(code, HostLocXmmIdx(value_idx), unwind_prologue.WindowsOnly());
                        if (vaddr_idx != code.ABI_PARAM2.getIdx()) {
                            code.mov(code.ABI_PARAM2, Xbyak::Reg64{vaddr_idx});
                        }
                        code.mov(code.ABI_PARAM3, bitsize / CHAR_BIT);
                        read_callbacks[bitsize == 32 ? 2 : 3].second.EmitCall(code);
                        if (bitsize == 32) {
                            code.movd(Xbyak::Xmm{value_idx}, code.ABI_RETURN.cvt32());
                        } else {
                            code.movq(Xbyak::Xmm{value_idx}, code.ABI_RETURN);
                        }
                        ABI_PopCallerSaveRegistersAndAdjustStackExcept(code, HostLocXmmIdx(value_idx));
                        code.ret();
                        if (PerfMapActive()) PerfMapRegister(read_fallbacks[key], code.getCurr(), fmt::format("a64_read_fallback_{}_xmm", bitsize));
                        code.RegisterNativeUnwindFrame(unwind_begin, code.getCurr(), unwind_prologue);
                    }
                }

                if (WantFallback(FallbackKind::Write, std::make_tuple(ordered, 128, vaddr_idx, value_idx))) {
                    code.align();
                    write_fallbacks[std::make_tuple(ordered, 128, vaddr_idx, value_idx)] = code.getCurr<void (*)()>();
                    const void* unwind_begin_1 = code.getCurr();
                    ABIPrologue unwind_prologue_1;
                    ABI_PushCallerSaveRegistersAndAdjustStack(code, 0, unwind_prologue_1.WindowsOnly());
                    if (ordered) {
                        code.ReloadA64CachedRbx();
                    }
                    if (vaddr_idx != code.ABI_PARAM2.getIdx()) {
                        code.mov(code.ABI_PARAM2, Xbyak::Reg64{vaddr_idx});
                    }
                    if (value_idx != 1) {
                        code.movaps(xmm1, Xbyak::Xmm{value_idx});
                    }
                    code.call(memory_write_128);
                    if (ordered) {
                        code.mfence();
                    }
                    ABI_PopCallerSaveRegistersAndAdjustStack(code);
                    code.ret();
                    PerfMapRegister(write_fallbacks[std::make_tuple(ordered, 128, vaddr_idx, value_idx)], code.getCurr(), "a64_write_fallback_128");
                    code.RegisterNativeUnwindFrame(unwind_begin_1, code.getCurr(), unwind_prologue_1);
                }

                if (WantFallback(FallbackKind::ExclusiveWrite, std::make_tuple(ordered, 128, vaddr_idx, value_idx))) {
                    code.align();
                    exclusive_write_fallbacks[std::make_tuple(ordered, 128, vaddr_idx, value_idx)] = code.getCurr<void (*)()>();
                    const void* unwind_begin_2 = code.getCurr();
                    ABIPrologue unwind_prologue_2;
                    ABI_PushCallerSaveRegistersAndAdjustStackExcept(code, HostLoc::RAX, unwind_prologue_2.WindowsOnly());
                    code.ReloadA64CachedRbx();
                    if (value_idx != 1) {
                        code.movaps(xmm1, Xbyak::Xmm{value_idx});
                    }
                    if (code.HasHostFeature(HostFeature::SSE41)) {
                        code.movq(xmm2, rax);
                        code.pinsrq(xmm2, rdx, 1);
                    } else {
                        code.movq(xmm2, rax);
                        code.movq(xmm0, rdx);
                        code.punpcklqdq(xmm2, xmm0);
                    }
                    if (vaddr_idx != code.ABI_PARAM2.getIdx()) {
                        code.mov(code.ABI_PARAM2, Xbyak::Reg64{vaddr_idx});
                    }
                    code.call(memory_exclusive_write_128);
                    ABI_PopCallerSaveRegistersAndAdjustStackExcept(code, HostLoc::RAX);
                    code.ret();
                    PerfMapRegister(exclusive_write_fallbacks[std::make_tuple(ordered, 128, vaddr_idx, value_idx)], code.getCurr(), "a64_exclusive_write_fallback_128");
                    code.RegisterNativeUnwindFrame(unwind_begin_2, code.getCurr(), unwind_prologue_2);
                }

                if (value_idx == 4 || value_idx == 15) {
                    continue;
                }

                for (const auto& [bitsize, callback] : read_callbacks) {
                    if (WantFallback(FallbackKind::Read, std::make_tuple(ordered, bitsize, vaddr_idx, value_idx))) {
                        code.align();
                        read_fallbacks[std::make_tuple(ordered, bitsize, vaddr_idx, value_idx)] = code.getCurr<void (*)()>();
                        const void* unwind_begin_3 = code.getCurr();
                        ABIPrologue unwind_prologue_3;
                        ABI_PushCallerSaveRegistersAndAdjustStackExcept(code, HostLocRegIdx(value_idx), unwind_prologue_3.WindowsOnly());
                        // params = { this, vaddr, size }
                        if (vaddr_idx != code.ABI_PARAM2.getIdx()) {
                            code.mov(code.ABI_PARAM2, Xbyak::Reg64{vaddr_idx});
                        }
                        code.mov(code.ABI_PARAM3, bitsize / CHAR_BIT);
                        if (ordered) code.mfence();
                        callback.EmitCall(code);
                        if (value_idx != code.ABI_RETURN.getIdx()) {
                            code.mov(Xbyak::Reg64{value_idx}, code.ABI_RETURN);
                        }
                        code.ZeroExtendFrom(bitsize, Xbyak::Reg64{value_idx});
                        ABI_PopCallerSaveRegistersAndAdjustStackExcept(code, HostLocRegIdx(value_idx));
                        code.ret();
                        if (PerfMapActive()) PerfMapRegister(read_fallbacks[std::make_tuple(ordered, bitsize, vaddr_idx, value_idx)], code.getCurr(), fmt::format("a64_read_fallback_{}", bitsize));
                        code.RegisterNativeUnwindFrame(unwind_begin_3, code.getCurr(), unwind_prologue_3);
                    }
                }

                for (const auto& [bitsize, callback] : write_callbacks) {
                    if (WantFallback(FallbackKind::Write, std::make_tuple(ordered, bitsize, vaddr_idx, value_idx))) {
                        code.align();
                        write_fallbacks[std::make_tuple(ordered, bitsize, vaddr_idx, value_idx)] = code.getCurr<void (*)()>();
                        const void* unwind_begin_4 = code.getCurr();
                        ABIPrologue unwind_prologue_4;
                        ABI_PushCallerSaveRegistersAndAdjustStack(code, 0, unwind_prologue_4.WindowsOnly());
                        // params = { this, vaddr, value, size }
                        if (vaddr_idx == code.ABI_PARAM3.getIdx() && value_idx == code.ABI_PARAM2.getIdx()) {
                            code.xchg(code.ABI_PARAM2, code.ABI_PARAM3);
                        } else if (vaddr_idx == code.ABI_PARAM3.getIdx()) {
                            code.mov(code.ABI_PARAM2, Xbyak::Reg64{vaddr_idx});
                            if (value_idx != code.ABI_PARAM3.getIdx()) {
                                code.mov(code.ABI_PARAM3, Xbyak::Reg64{value_idx});
                            }
                        } else {
                            if (value_idx != code.ABI_PARAM3.getIdx()) {
                                code.mov(code.ABI_PARAM3, Xbyak::Reg64{value_idx});
                            }
                            if (vaddr_idx != code.ABI_PARAM2.getIdx()) {
                                code.mov(code.ABI_PARAM2, Xbyak::Reg64{vaddr_idx});
                            }
                        }
                        code.ZeroExtendFrom(bitsize, code.ABI_PARAM3);
                        code.mov(code.ABI_PARAM4, bitsize / CHAR_BIT);
                        callback.EmitCall(code);
                        if (ordered) code.mfence();
                        ABI_PopCallerSaveRegistersAndAdjustStack(code);
                        code.ret();
                        if (PerfMapActive()) PerfMapRegister(write_fallbacks[std::make_tuple(ordered, bitsize, vaddr_idx, value_idx)], code.getCurr(), fmt::format("a64_write_fallback_{}", bitsize));
                        code.RegisterNativeUnwindFrame(unwind_begin_4, code.getCurr(), unwind_prologue_4);
                    }
                }

                for (const auto& [bitsize, callback] : exclusive_write_callbacks) {
                    if (WantFallback(FallbackKind::ExclusiveWrite, std::make_tuple(ordered, bitsize, vaddr_idx, value_idx))) {
                        code.align();
                        exclusive_write_fallbacks[std::make_tuple(ordered, bitsize, vaddr_idx, value_idx)] = code.getCurr<void (*)()>();
                        const void* unwind_begin_5 = code.getCurr();
                        ABIPrologue unwind_prologue_5;
                        ABI_PushCallerSaveRegistersAndAdjustStackExcept(code, HostLoc::RAX, unwind_prologue_5.WindowsOnly());
                        if (vaddr_idx == code.ABI_PARAM3.getIdx() && value_idx == code.ABI_PARAM2.getIdx()) {
                            code.xchg(code.ABI_PARAM2, code.ABI_PARAM3);
                        } else if (vaddr_idx == code.ABI_PARAM3.getIdx()) {
                            code.mov(code.ABI_PARAM2, Xbyak::Reg64{vaddr_idx});
                            if (value_idx != code.ABI_PARAM3.getIdx()) {
                                code.mov(code.ABI_PARAM3, Xbyak::Reg64{value_idx});
                            }
                        } else {
                            if (value_idx != code.ABI_PARAM3.getIdx()) {
                                code.mov(code.ABI_PARAM3, Xbyak::Reg64{value_idx});
                            }
                            if (vaddr_idx != code.ABI_PARAM2.getIdx()) {
                                code.mov(code.ABI_PARAM2, Xbyak::Reg64{vaddr_idx});
                            }
                        }
                        code.ZeroExtendFrom(bitsize, code.ABI_PARAM3);
                        code.mov(code.ABI_PARAM4, rax);
                        code.ZeroExtendFrom(bitsize, code.ABI_PARAM4);
                        callback.EmitCall(code);
                        ABI_PopCallerSaveRegistersAndAdjustStackExcept(code, HostLoc::RAX);
                        code.ret();
                        if (PerfMapActive()) PerfMapRegister(exclusive_write_fallbacks[std::make_tuple(ordered, bitsize, vaddr_idx, value_idx)], code.getCurr(), fmt::format("a64_exclusive_write_fallback_{}", bitsize));
                        code.RegisterNativeUnwindFrame(unwind_begin_5, code.getCurr(), unwind_prologue_5);
                    }
                }
            }
        }
    }
}

#define Axx A64
#include "dynarmic/backend/x64/emit_x64_memory.cpp.inc"
#undef Axx

void A64EmitX64::FoldMemoryAddresses(A64EmitContext& ctx) const {
    // Identity fastmem (register cache) addresses memory with the guest address
    // itself, so an immediate offset can be the memory operand's displacement.
    // Ordered accesses test the full address for alignment and keep the add.
    if (!conf.HasOptimization(OptimizationFlag::AddressFolding) || !conf.enable_register_cache) {
        return;
    }
    for (auto& inst : ctx.block.instructions) {
        size_t acc_type_arg;
        switch (inst.GetOpcode()) {
        case IR::Opcode::A64ReadMemory8:
        case IR::Opcode::A64ReadMemory16:
        case IR::Opcode::A64ReadMemory32:
        case IR::Opcode::A64ReadMemory64:
        case IR::Opcode::A64ReadMemory128:
            acc_type_arg = 2;
            break;
        case IR::Opcode::A64WriteMemory8:
        case IR::Opcode::A64WriteMemory16:
        case IR::Opcode::A64WriteMemory32:
        case IR::Opcode::A64WriteMemory64:
        case IR::Opcode::A64WriteMemory128:
            acc_type_arg = 3;
            break;
        default:
            continue;
        }
        const IR::Value address = inst.GetArg(1);
        if (address.IsImmediate() || IsOrdered(inst.GetArg(acc_type_arg).GetAccType()) || !ShouldFastmem(ctx, &inst)) {
            continue;
        }
        IR::Inst* const add = address.GetInst();
        if (add->GetOpcode() != IR::Opcode::Add64 || add->UseCount() != 1 || add->GetArg(0).IsImmediate() ||
            !add->GetArg(1).IsImmediate() || !add->GetArg(2).IsImmediate() || add->GetArg(2).GetU1() ||
            add->GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp) ||
            add->GetAssociatedPseudoOperation(IR::Opcode::GetOverflowFromOp) ||
            add->GetAssociatedPseudoOperation(IR::Opcode::GetNZCVFromOp)) {
            continue;
        }
        // The fault path also applies -displacement, so exclude INT32_MIN.
        const s64 displacement = static_cast<s64>(add->GetArg(1).GetImmediateAsU64());
        if (displacement > s64(0x7fffffff) || displacement <= -s64(0x80000000)) {
            continue;
        }
        ctx.folded_addresses.push_back(add);
    }
}

void A64EmitX64::EmitA64ReadMemory8(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryRead<8, &A64::UserCallbacks::MemoryRead>(ctx, inst);
}

void A64EmitX64::EmitA64ReadMemory16(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryRead<16, &A64::UserCallbacks::MemoryRead>(ctx, inst);
}

void A64EmitX64::EmitA64ReadMemory32(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryRead<32, &A64::UserCallbacks::MemoryRead>(ctx, inst);
}

void A64EmitX64::EmitA64ReadMemory64(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryRead<64, &A64::UserCallbacks::MemoryRead>(ctx, inst);
}

void A64EmitX64::EmitA64ReadMemory128(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryRead<128, &A64::UserCallbacks::MemoryRead128>(ctx, inst);
}

void A64EmitX64::EmitA64WriteMemory8(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryWrite<8, &A64::UserCallbacks::MemoryWrite>(ctx, inst);
}

void A64EmitX64::EmitA64WriteMemory16(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryWrite<16, &A64::UserCallbacks::MemoryWrite>(ctx, inst);
}

void A64EmitX64::EmitA64WriteMemory32(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryWrite<32, &A64::UserCallbacks::MemoryWrite>(ctx, inst);
}

void A64EmitX64::EmitA64WriteMemory64(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryWrite<64, &A64::UserCallbacks::MemoryWrite>(ctx, inst);
}

void A64EmitX64::EmitA64WriteMemory128(A64EmitContext& ctx, IR::Inst* inst) {
    EmitMemoryWrite<128, &A64::UserCallbacks::MemoryWrite>(ctx, inst);
}

void A64EmitX64::EmitA64ClearExclusive(A64EmitContext&, IR::Inst*) {
    code.mov(code.byte[code.ABI_JIT_PTR + offsetof(A64JitState, exclusive_state)], u8(0));
}

void A64EmitX64::EmitA64ExclusiveReadMemory8(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveReadMemoryInline<8, &A64::UserCallbacks::MemoryRead>(ctx, inst);
    } else {
        EmitExclusiveReadMemory<8, &A64::UserCallbacks::MemoryRead>(ctx, inst);
    }
}

void A64EmitX64::EmitA64ExclusiveReadMemory16(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveReadMemoryInline<16, &A64::UserCallbacks::MemoryRead>(ctx, inst);
    } else {
        EmitExclusiveReadMemory<16, &A64::UserCallbacks::MemoryRead>(ctx, inst);
    }
}

void A64EmitX64::EmitA64ExclusiveReadMemory32(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveReadMemoryInline<32, &A64::UserCallbacks::MemoryRead>(ctx, inst);
    } else {
        EmitExclusiveReadMemory<32, &A64::UserCallbacks::MemoryRead>(ctx, inst);
    }
}

void A64EmitX64::EmitA64ExclusiveReadMemory64(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveReadMemoryInline<64, &A64::UserCallbacks::MemoryRead>(ctx, inst);
    } else {
        EmitExclusiveReadMemory<64, &A64::UserCallbacks::MemoryRead>(ctx, inst);
    }
}

void A64EmitX64::EmitA64ExclusiveReadMemory128(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveReadMemoryInline<128, &A64::UserCallbacks::MemoryRead128>(ctx, inst);
    } else {
        EmitExclusiveReadMemory<128, &A64::UserCallbacks::MemoryRead128>(ctx, inst);
    }
}

void A64EmitX64::EmitA64ExclusiveWriteMemory8(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveWriteMemoryInline<8, &A64::UserCallbacks::MemoryWriteExclusive8>(ctx, inst);
    } else {
        EmitExclusiveWriteMemory<8, &A64::UserCallbacks::MemoryWriteExclusive8>(ctx, inst);
    }
}

void A64EmitX64::EmitA64ExclusiveWriteMemory16(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveWriteMemoryInline<16, &A64::UserCallbacks::MemoryWriteExclusive16>(ctx, inst);
    } else {
        EmitExclusiveWriteMemory<16, &A64::UserCallbacks::MemoryWriteExclusive16>(ctx, inst);
    }
}

void A64EmitX64::EmitA64ExclusiveWriteMemory32(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveWriteMemoryInline<32, &A64::UserCallbacks::MemoryWriteExclusive32>(ctx, inst);
    } else {
        EmitExclusiveWriteMemory<32, &A64::UserCallbacks::MemoryWriteExclusive32>(ctx, inst);
    }
}

void A64EmitX64::EmitA64ExclusiveWriteMemory64(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveWriteMemoryInline<64, &A64::UserCallbacks::MemoryWriteExclusive64>(ctx, inst);
    } else {
        EmitExclusiveWriteMemory<64, &A64::UserCallbacks::MemoryWriteExclusive64>(ctx, inst);
    }
}

void A64EmitX64::EmitA64ExclusiveWriteMemory128(A64EmitContext& ctx, IR::Inst* inst) {
    if (conf.fastmem_exclusive_access) {
        EmitExclusiveWriteMemoryInline<128, &A64::UserCallbacks::MemoryWriteExclusive128>(ctx, inst);
    } else {
        EmitExclusiveWriteMemory<128, &A64::UserCallbacks::MemoryWriteExclusive128>(ctx, inst);
    }
}

template<std::size_t bitsize, bool compare_and_swap>
void A64EmitX64::EmitAtomicMemory(A64EmitContext& ctx, IR::Inst* inst) {
    using T = mcl::unsigned_integer_of_size<bitsize == 128 ? 64 : bitsize>;
    using Op = A64::AtomicOp;
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const auto op = compare_and_swap ? Op::CompareAndSwap : static_cast<Op>(args[3].GetImmediateU8());
    const bool ignore_monitor = conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor);

    // The callback path. With the global monitor it also breaks every reservation, as
    // an exclusive store does; IgnoreGlobalMonitor exclusives compare values instead.
    static constexpr auto fallback = [](A64::UserConfig& conf, A64::VAddr vaddr, u64 value, u64 extra) -> u64 {
        const auto call = [&]() -> u64 {
            if constexpr (compare_and_swap) {
                return conf.callbacks->MemoryAtomic(vaddr, bitsize / 8, Op::CompareAndSwap, T(value), T(extra));
            } else {
                return conf.callbacks->MemoryAtomic(vaddr, bitsize / 8, static_cast<Op>(extra), T(value), 0);
            }
        };
        if (!conf.global_monitor || conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor)) {
            return call();
        }
        return conf.global_monitor->DoAtomicOperation(call);
    };

    if constexpr (bitsize == 128) {
        static constexpr auto fallback128 = [](A64::UserConfig& conf, A64::VAddr vaddr, Vector* io) {
            const auto call = [&]() { return conf.callbacks->MemoryAtomicCompareAndSwap128(vaddr, io[0], io[1]); };
            io[0] = !conf.global_monitor || conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor)
                      ? call()
                      : conf.global_monitor->DoAtomicOperation(call);
        };
        const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm(code);
        ctx.reg_alloc.Use(code, args[1], ABI_PARAM2);
        ctx.reg_alloc.Use(code, args[2], HostLoc::XMM1);
        ctx.reg_alloc.Use(code, args[3], HostLoc::XMM2);
        ctx.reg_alloc.EndOfAllocScope();
        ctx.reg_alloc.HostCall(code, nullptr);
        code.mov(code.byte[r15 + offsetof(A64JitState, exclusive_state)], u8(0));
        ctx.reg_alloc.AllocStackSpace(code, 32 + ABI_SHADOW_SPACE);
        code.lea(code.ABI_PARAM3, ptr[rsp + ABI_SHADOW_SPACE]);
        code.movups(xword[code.ABI_PARAM3], xmm1);
        code.movups(xword[code.ABI_PARAM3 + 16], xmm2);
        code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(&conf));
        code.CallLambda(fallback128);
        code.movups(result, xword[rsp + ABI_SHADOW_SPACE]);
        ctx.reg_alloc.ReleaseStackSpace(code, 32 + ABI_SHADOW_SPACE);
        ctx.reg_alloc.DefineValue(code, inst, result);
        EmitCheckMemoryAbort(ctx, inst);
        return;
    } else {
        const auto fastmem_marker = ignore_monitor ? ShouldFastmem(ctx, inst) : std::nullopt;
        if (!fastmem_marker) {
            if constexpr (compare_and_swap) {
                ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2], args[3]);
            } else {
                ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
                code.mov(code.ABI_PARAM4.cvt32(), static_cast<u32>(op));
            }
            code.mov(code.byte[r15 + offsetof(A64JitState, exclusive_state)], u8(0));
            code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(&conf));
            code.CallLambda(fallback);
            code.ZeroExtendFrom(bitsize, code.ABI_RETURN);
            EmitCheckMemoryAbort(ctx, inst);
            return;
        }

        const auto sized = [](Xbyak::Reg64 reg) -> Xbyak::Reg {
            switch (bitsize) {
            case 8:
                return reg.cvt8();
            case 16:
                return reg.cvt16();
            case 32:
                return reg.cvt32();
            default:
                return reg;
            }
        };
        const auto memory = [&](const Xbyak::RegExp& address) -> Xbyak::Address {
            switch (bitsize) {
            case 8:
                return code.byte[address];
            case 16:
                return code.word[address];
            case 32:
                return code.dword[address];
            default:
                return code.qword[address];
            }
        };

        // Add and Swap are one locked instruction on the operand register. CAS and the
        // other operations leave the old value in RAX; those but CAS retry a CMPXCHG loop.
        const bool single = op == Op::Add || op == Op::Swap;
        const bool loop = !single && op != Op::CompareAndSwap;
        const bool narrow_compare = bitsize < 32 && (op == Op::SMax || op == Op::SMin || op == Op::UMax || op == Op::UMin);
        Xbyak::Reg64 result, value, extra, tmp, aux;
        if (single) {
            result = value = ctx.reg_alloc.UseScratchGpr(code, args[2]);
        } else if (compare_and_swap) {
            ctx.reg_alloc.UseScratch(code, args[2], HostLoc::RAX);
            result = value = rax;
            extra = ctx.reg_alloc.UseGpr(code, args[3]);
        } else {
            result = ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
            value = ctx.reg_alloc.UseGpr(code, args[2]);
            tmp = ctx.reg_alloc.ScratchGpr(code);
            if (op == Op::Clear || narrow_compare) {
                aux = ctx.reg_alloc.ScratchGpr(code);
            }
        }
        const Xbyak::Reg64 vaddr = ctx.reg_alloc.UseGpr(code, args[1]);

        SharedLabel abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();
        if constexpr (bitsize > 8) {
            // LSE requires natural alignment; the callback reports the fault.
            code.test(vaddr.cvt32(), u32(bitsize / 8 - 1));
            code.jnz(*abort, code.T_NEAR);
        }
        bool require_abort_handling = false;
        const auto address = EmitFastmemVAddr(code, ctx, *abort, vaddr, require_abort_handling);
        code.mov(code.byte[r15 + offsetof(A64JitState, exclusive_state)], u8(0));

        std::vector<const void*> sites;
        if (single) {
            sites.push_back(code.getCurr());
            if (op == Op::Add) {
                code.lock();
                code.xadd(memory(address), sized(value));
            } else {
                code.xchg(memory(address), sized(value));
            }
        } else if (compare_and_swap) {
            sites.push_back(code.getCurr());
            code.lock();
            code.cmpxchg(memory(address), sized(extra));
        } else {
            if (op == Op::Clear) {
                code.mov(aux, value);
                code.not_(aux);
            } else if (narrow_compare) {
                if (op == Op::SMax || op == Op::SMin) {
                    code.movsx(aux.cvt32(), sized(value));
                } else {
                    code.movzx(aux.cvt32(), sized(value));
                }
            }
            sites.push_back(code.getCurr());
            if constexpr (bitsize < 32) {
                code.movzx(eax, memory(address));
            } else {
                code.mov(sized(rax), memory(address));
            }
            Xbyak::Label retry;
            code.L(retry);
            const Xbyak::Reg64 operand = narrow_compare ? aux : value;
            const auto compare = [&](auto cmov) {
                if constexpr (bitsize < 32) {
                    if (op == Op::SMax || op == Op::SMin) {
                        code.movsx(tmp.cvt32(), sized(rax));
                    } else {
                        code.movzx(tmp.cvt32(), sized(rax));
                    }
                    code.cmp(tmp.cvt32(), operand.cvt32());
                    (code.*cmov)(tmp.cvt32(), operand.cvt32());
                } else {
                    code.mov(sized(tmp), sized(rax));
                    code.cmp(sized(tmp), sized(operand));
                    (code.*cmov)(sized(tmp), sized(operand));
                }
            };
            using Cmov = void (Xbyak::CodeGenerator::*)(const Xbyak::Reg&, const Xbyak::Operand&);
            switch (op) {
            case Op::Clear:
                code.mov(tmp, rax);
                code.and_(tmp, aux);
                break;
            case Op::Eor:
                code.mov(tmp, rax);
                code.xor_(tmp, value);
                break;
            case Op::Set:
                code.mov(tmp, rax);
                code.or_(tmp, value);
                break;
            case Op::SMax:
                compare(static_cast<Cmov>(&Xbyak::CodeGenerator::cmovl));
                break;
            case Op::SMin:
                compare(static_cast<Cmov>(&Xbyak::CodeGenerator::cmovg));
                break;
            case Op::UMax:
                compare(static_cast<Cmov>(&Xbyak::CodeGenerator::cmovb));
                break;
            case Op::UMin:
                compare(static_cast<Cmov>(&Xbyak::CodeGenerator::cmova));
                break;
            default:
                UNREACHABLE();
            }
            sites.push_back(code.getCurr());
            code.lock();
            code.cmpxchg(memory(address), sized(tmp));
            code.jnz(retry);
        }
        code.L(*end);

        // Entered by the abort branch, or by the fault handler from a site above with
        // the registers as they were before that instruction.
        ctx.framed_deferred_emits.emplace_back([=, this, &ctx] {
            const void* stub = code.getCurr();
            ABIPrologue unwind_prologue;
            ABI_PushCallerSaveRegistersAndAdjustStackExcept(code, HostLocRegIdx(result.getIdx()), unwind_prologue.WindowsOnly());
#ifdef _WIN32
            code.mov(code.qword[rsp], vaddr);
            code.mov(code.qword[rsp + 8], value);
            if (compare_and_swap) {
                code.mov(code.qword[rsp + 16], extra);
                code.mov(code.ABI_PARAM4, code.qword[rsp + 16]);
            }
            code.mov(code.ABI_PARAM3, code.qword[rsp + 8]);
            code.mov(code.ABI_PARAM2, code.qword[rsp]);
#else
            code.push(vaddr);
            code.push(value);
            if (compare_and_swap) {
                code.push(extra);
                code.pop(code.ABI_PARAM4);
            }
            code.pop(code.ABI_PARAM3);
            code.pop(code.ABI_PARAM2);
#endif
            if (!compare_and_swap) {
                code.mov(code.ABI_PARAM4.cvt32(), static_cast<u32>(op));
            }
            code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(&conf));
            code.CallLambda(fallback);
            if (result.getIdx() != code.ABI_RETURN.getIdx()) {
                code.mov(result, code.ABI_RETURN);
            }
            code.ZeroExtendFrom(bitsize, result);
            ABI_PopCallerSaveRegistersAndAdjustStackExcept(code, HostLocRegIdx(result.getIdx()));
            code.ret();
            code.RegisterNativeUnwindFrame(stub, code.getCurr(), unwind_prologue);

            code.L(*abort);
            code.call(stub);
            for (const void* site : sites) {
                fastmem_patch_info.emplace(
                    std::bit_cast<u64>(site),
                    FastmemPatchInfo{
                        std::bit_cast<u64>(code.getCurr()),
                        std::bit_cast<u64>(stub),
                        *fastmem_marker,
                        conf.recompile_on_fastmem_failure,
                    });
            }
            EmitCheckMemoryAbort(ctx, inst, end);
            code.jmp(*end, code.T_NEAR);
        });

        if constexpr (bitsize < 64) {
            code.ZeroExtendFrom(bitsize, result);
        }
        ctx.reg_alloc.DefineValue(code, inst, result);
    }
}

void A64EmitX64::EmitA64AtomicMemory8(A64EmitContext& ctx, IR::Inst* inst) {
    EmitAtomicMemory<8, false>(ctx, inst);
}

void A64EmitX64::EmitA64AtomicMemory16(A64EmitContext& ctx, IR::Inst* inst) {
    EmitAtomicMemory<16, false>(ctx, inst);
}

void A64EmitX64::EmitA64AtomicMemory32(A64EmitContext& ctx, IR::Inst* inst) {
    EmitAtomicMemory<32, false>(ctx, inst);
}

void A64EmitX64::EmitA64AtomicMemory64(A64EmitContext& ctx, IR::Inst* inst) {
    EmitAtomicMemory<64, false>(ctx, inst);
}

void A64EmitX64::EmitA64CompareAndSwapMemory8(A64EmitContext& ctx, IR::Inst* inst) {
    EmitAtomicMemory<8, true>(ctx, inst);
}

void A64EmitX64::EmitA64CompareAndSwapMemory16(A64EmitContext& ctx, IR::Inst* inst) {
    EmitAtomicMemory<16, true>(ctx, inst);
}

void A64EmitX64::EmitA64CompareAndSwapMemory32(A64EmitContext& ctx, IR::Inst* inst) {
    EmitAtomicMemory<32, true>(ctx, inst);
}

void A64EmitX64::EmitA64CompareAndSwapMemory64(A64EmitContext& ctx, IR::Inst* inst) {
    EmitAtomicMemory<64, true>(ctx, inst);
}

void A64EmitX64::EmitA64CompareAndSwapMemory128(A64EmitContext& ctx, IR::Inst* inst) {
    EmitAtomicMemory<128, true>(ctx, inst);
}

void A64EmitX64::EmitCheckMemoryAbort(A64EmitContext& ctx, IR::Inst* inst, Xbyak::Label* end) {
    if (!conf.check_halt_on_memory_access) {
        return;
    }

    Xbyak::Label skip;

    const A64::LocationDescriptor current_location{IR::LocationDescriptor{inst->GetArg(0).GetU64()}};
    const auto exit = [this, &ctx, inst, current_location] {
        EmitColdStateStores(ctx, inst);
        code.mov(rax, current_location.PC());
        code.mov(qword[code.ABI_JIT_PTR + offsetof(A64JitState, pc)], rax);
        code.ForceReturnFromRunCode();
    };

    code.test(dword[code.ABI_JIT_PTR + offsetof(A64JitState, halt_reason)], static_cast<u32>(HaltReason::MemoryAbort));
    if (!end && !ctx.emitting_deferred && conf.cold_memory_abort_state != A64::UserConfig::ColdAbortState::Off) {
        // An inline check precedes the access's A64ColdSet* records: exit out of line.
        // Nothing is emitted between the check and those records.
        SharedLabel abort = ctx.GenSharedLabel();
        code.jnz(*abort, code.T_NEAR);
        ctx.deferred_emits.emplace_back([=, this] {
            code.L(*abort);
            exit();
        });
        return;
    }
    if (end) {
        code.jz(*end, code.T_NEAR);
    } else {
        code.jz(skip, code.T_NEAR);
    }
    exit();
    code.L(skip);
}

void A64EmitX64::RecordColdState(A64EmitContext& ctx, IR::Inst* inst) {
    // Emits nothing: the argument references keep the value where it is until this
    // instruction, directly after the access.
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const auto opcode = inst->GetOpcode();
    const bool has_register = opcode != IR::Opcode::A64ColdSetSP && opcode != IR::Opcode::A64ColdSetNZCV;
    A64EmitContext::ColdStateStore store{opcode, 0, std::nullopt, 0};
    if (opcode == IR::Opcode::A64ColdSetW || opcode == IR::Opcode::A64ColdSetX) {
        store.index = static_cast<size_t>(inst->GetArg(1).GetA64RegRef());
    } else if (has_register) {
        store.index = static_cast<size_t>(inst->GetArg(1).GetA64VecRef());
    }
    const auto& value = args[has_register ? 2 : 1];
    if (value.IsImmediate()) {
        store.immediate = value.GetImmediateU64();
    } else {
        store.location = ctx.reg_alloc.ArgumentLocation(value);
        ASSERT(store.location);
    }
    ctx.cold_state[inst->GetArg(0).GetInst()].push_back(store);
}

void A64EmitX64::EmitA64ColdSetW(A64EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

void A64EmitX64::EmitA64ColdSetX(A64EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

void A64EmitX64::EmitA64ColdSetS(A64EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

void A64EmitX64::EmitA64ColdSetD(A64EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

void A64EmitX64::EmitA64ColdSetQ(A64EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

void A64EmitX64::EmitA64ColdSetSP(A64EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

void A64EmitX64::EmitA64ColdSetNZCV(A64EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

void A64EmitX64::EmitColdStateStores(A64EmitContext& ctx, IR::Inst* access) {
    const auto found = ctx.cold_state.find(access);
    if (found == ctx.cold_state.end()) {
        return;
    }
    using Op = IR::Opcode;
    const auto state = [this](const A64EmitContext::ColdStateStore& store) -> Xbyak::RegExp {
        switch (store.opcode) {
        case Op::A64ColdSetW:
        case Op::A64ColdSetX:
            return code.ABI_JIT_PTR + offsetof(A64JitState, reg) + sizeof(u64) * store.index;
        case Op::A64ColdSetS:
        case Op::A64ColdSetD:
        case Op::A64ColdSetQ:
            return code.ABI_JIT_PTR + offsetof(A64JitState, vec) + sizeof(u64) * 2 * store.index;
        case Op::A64ColdSetSP:
            return code.ABI_JIT_PTR + offsetof(A64JitState, sp);
        default:
            return code.ABI_JIT_PTR + offsetof(A64JitState, cpsr_nzcv);
        }
    };
    const auto spill = [](HostLoc loc) -> Xbyak::RegExp {
        ASSERT(HostLocIsSpill(loc));
        return rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, spill) + (size_t(loc) - size_t(HostLoc::FirstSpill)) * sizeof(StackLayout::spill[0]);
    };
    // Register-held values first. Afterwards no host register holds anything this
    // exit needs, and RAX carries spilled and immediate values.
    for (const bool registers : {true, false}) {
        for (const auto& store : found->second) {
            const auto loc = store.location;
            if ((loc && HostLocIsRegister(*loc)) != registers) {
                continue;
            }
            const auto at = state(store);
            switch (store.opcode) {
            case Op::A64ColdSetX:
            case Op::A64ColdSetSP:
                if (!loc) {
                    code.mov(rax, store.immediate);
                    code.mov(qword[at], rax);
                } else if (HostLocIsGPR(*loc)) {
                    code.mov(qword[at], HostLocToReg64(*loc));
                } else if (HostLocIsXMM(*loc)) {
                    code.movq(qword[at], HostLocToXmm(*loc));
                } else {
                    code.mov(rax, qword[spill(*loc)]);
                    code.mov(qword[at], rax);
                }
                break;
            case Op::A64ColdSetW:
            case Op::A64ColdSetNZCV:
                if (!loc) {
                    code.mov(dword[at], u32(store.immediate));
                } else if (HostLocIsGPR(*loc)) {
                    code.mov(dword[at], HostLocToReg64(*loc).cvt32());
                } else if (HostLocIsXMM(*loc)) {
                    code.movd(dword[at], HostLocToXmm(*loc));
                } else {
                    code.mov(eax, dword[spill(*loc)]);
                    code.mov(dword[at], eax);
                }
                if (store.opcode == Op::A64ColdSetW) {
                    code.mov(dword[at + 4], 0);  // W writes zero-extend
                }
                break;
            default: {
                // S/D/Q: a U128 value whose low 4/8/16 bytes become the vector
                // register; the rest of it is zeroed, as the A64Set* emitters do.
                ASSERT(loc && !HostLocIsGPR(*loc));
                const size_t bytes = store.opcode == Op::A64ColdSetS ? 4 : store.opcode == Op::A64ColdSetD ? 8 : 16;
                if (HostLocIsXMM(*loc)) {
                    const Xbyak::Xmm xmm = HostLocToXmm(*loc);
                    if (bytes == 16) {
                        code.movups(xword[at], xmm);
                    } else if (bytes == 8) {
                        code.movq(qword[at], xmm);
                    } else {
                        code.movd(dword[at], xmm);
                    }
                } else if (bytes == 4) {
                    code.mov(eax, dword[spill(*loc)]);
                    code.mov(dword[at], eax);
                } else {
                    code.mov(rax, qword[spill(*loc)]);
                    code.mov(qword[at], rax);
                    if (bytes == 16) {
                        code.mov(rax, qword[spill(*loc) + 8]);
                        code.mov(qword[at + 8], rax);
                    }
                }
                if (bytes == 4) {
                    code.mov(dword[at + 4], 0);
                }
                if (bytes != 16) {
                    code.mov(qword[at + 8], 0);
                }
                break;
            }
            }
        }
    }
}

}  // namespace Dynarmic::Backend::X64
