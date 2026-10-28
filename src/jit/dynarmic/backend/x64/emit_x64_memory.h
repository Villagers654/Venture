// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <bit>
#include <utility>
#include "dynarmic/backend/x64/xbyak.h"

#include "dynarmic/backend/x64/a32_emit_x64.h"
#include "dynarmic/backend/x64/a64_emit_x64.h"
#include "dynarmic/backend/x64/exclusive_monitor_friend.h"
#include "dynarmic/common/spin_lock_x64.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/ir/acc_type.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::X64 {

namespace {

using namespace Xbyak::util;

constexpr size_t page_table_const_bits = 12;
/// Venture: read_fallbacks key bit for a 32/64-bit value returned in an XMM register.
constexpr size_t xmm_read_fallback = 0x100;

/// Venture: every user of a 32/64-bit load reads it in an XMM register: scalar
/// FP operations, and zero extension to a vector (a word through
/// ZeroExtendWordToLong used only by ZeroExtendLongToQuad). A64ColdSet* records
/// name the access as their first argument without reading it.
inline bool LoadFeedsXmm(const EmitContext& ctx, IR::Inst* inst, int depth = 0) {
    const size_t uses = inst->UseCount();
    if (uses == 0)
        return false;
    size_t found = 0;
    for (auto it = std::next(IR::Block::iterator{*inst}); it != ctx.block.instructions.end(); ++it) {
        IR::Inst& user = *it;
        for (size_t i = 0; i < user.NumArgs(); ++i) {
            const IR::Value arg = user.GetArg(i);
            if (arg.IsImmediate() || arg.GetInst() != inst)
                continue;
            switch (user.GetOpcode()) {
            case IR::Opcode::A64ColdSetW: case IR::Opcode::A64ColdSetX:
            case IR::Opcode::A64ColdSetS: case IR::Opcode::A64ColdSetD:
            case IR::Opcode::A64ColdSetQ: case IR::Opcode::A64ColdSetSP:
            case IR::Opcode::A64ColdSetNZCV:
                if (i != 0)
                    return false;
                break;
            case IR::Opcode::ZeroExtendLongToQuad:
                break;
            case IR::Opcode::ZeroExtendWordToLong:
                if (depth != 0 || !LoadFeedsXmm(ctx, &user, 1))
                    return false;
                break;
            case IR::Opcode::FPAbs32: case IR::Opcode::FPAbs64:
            case IR::Opcode::FPAdd32: case IR::Opcode::FPAdd64:
            case IR::Opcode::FPCompare32: case IR::Opcode::FPCompare64:
            case IR::Opcode::FPDiv32: case IR::Opcode::FPDiv64:
            case IR::Opcode::FPMax32: case IR::Opcode::FPMax64:
            case IR::Opcode::FPMaxNumeric32: case IR::Opcode::FPMaxNumeric64:
            case IR::Opcode::FPMin32: case IR::Opcode::FPMin64:
            case IR::Opcode::FPMinNumeric32: case IR::Opcode::FPMinNumeric64:
            case IR::Opcode::FPMul32: case IR::Opcode::FPMul64:
            case IR::Opcode::FPMulAdd32: case IR::Opcode::FPMulAdd64:
            case IR::Opcode::FPMulSub32: case IR::Opcode::FPMulSub64:
            case IR::Opcode::FPMulX32: case IR::Opcode::FPMulX64:
            case IR::Opcode::FPNeg32: case IR::Opcode::FPNeg64:
            case IR::Opcode::FPRoundInt32: case IR::Opcode::FPRoundInt64:
            case IR::Opcode::FPSqrt32: case IR::Opcode::FPSqrt64:
            case IR::Opcode::FPSub32: case IR::Opcode::FPSub64:
            case IR::Opcode::FPSingleToDouble: case IR::Opcode::FPDoubleToSingle:
            case IR::Opcode::FPSingleToFixedS32: case IR::Opcode::FPSingleToFixedU32:
            case IR::Opcode::FPSingleToFixedS64: case IR::Opcode::FPSingleToFixedU64:
            case IR::Opcode::FPDoubleToFixedS32: case IR::Opcode::FPDoubleToFixedU32:
            case IR::Opcode::FPDoubleToFixedS64: case IR::Opcode::FPDoubleToFixedU64:
                break;
            default:
                return false;
            }
            if (++found == uses)
                return true;
        }
    }
    return false;
}
constexpr size_t page_table_const_size = 1 << page_table_const_bits;
constexpr size_t page_table_const_mask = (1 << page_table_const_bits) - 1;

template<typename EmitContext>
void EmitDetectMisalignedVAddr(BlockOfCode& code, EmitContext& ctx, size_t bitsize, Xbyak::Label& abort, Xbyak::Reg64 vaddr, Xbyak::Reg64 tmp) {
    if (bitsize == 8 || (ctx.conf.detect_misaligned_access_via_page_table & bitsize) == 0) {
        return;
    }

    const u32 align_mask = [bitsize]() -> u32 {
        switch (bitsize) {
        case 16:
            return 0b1;
        case 32:
            return 0b11;
        case 64:
            return 0b111;
        case 128:
            return 0b1111;
        default:
            UNREACHABLE();
        }
    }();

    code.test(vaddr, align_mask);

    if (ctx.conf.only_detect_misalignment_via_page_table_on_page_boundary) {
        const u32 page_align_mask = static_cast<u32>(page_table_const_size - 1) & ~align_mask;

        SharedLabel detect_boundary = ctx.GenSharedLabel(), resume = ctx.GenSharedLabel();

        code.jnz(*detect_boundary, code.T_NEAR);
        code.L(*resume);

        ctx.deferred_emits.emplace_back([=, &code] {
            code.L(*detect_boundary);
            code.mov(tmp, vaddr);
            code.and_(tmp, page_align_mask);
            code.cmp(tmp, page_align_mask);
            code.jne(*resume, code.T_NEAR);
            // NOTE: We expect to fallthrough into abort code here.
        });
    } else {
        code.jnz(abort, code.T_NEAR);
    }
}

template<typename EmitContext>
Xbyak::RegExp EmitVAddrLookup(BlockOfCode& code, EmitContext& ctx, size_t bitsize, Xbyak::Label& abort, Xbyak::Reg64 vaddr);

template<>
[[maybe_unused]] Xbyak::RegExp EmitVAddrLookup<A32EmitContext>(BlockOfCode& code, A32EmitContext& ctx, size_t bitsize, Xbyak::Label& abort, Xbyak::Reg64 vaddr) {
    const Xbyak::Reg64 page = ctx.reg_alloc.ScratchGpr(code);
    const Xbyak::Reg64 tmp = ctx.conf.absolute_offset_page_table && ctx.conf.page_table_pointer_mask == 0 ? page : ctx.reg_alloc.ScratchGpr(code);

    EmitDetectMisalignedVAddr(code, ctx, bitsize, abort, vaddr, tmp);

    code.mov(tmp, vaddr);
    code.shr(tmp, int(page_table_const_bits));

    if (ctx.conf.page_table_log2_stride > 3) {
        code.shl(tmp, int(ctx.conf.page_table_log2_stride));
        code.mov(page, qword[r14 + tmp.cvt64()]);
    } else {
        code.mov(page, qword[r14 + tmp.cvt64() * int(1 << ctx.conf.page_table_log2_stride)]);
    }

    // check for marked bit, use as unmapped if marked
	if (ctx.conf.page_table_marked_bit) {
        code.bt(page, *ctx.conf.page_table_marked_bit);
        code.jc(abort, code.T_NEAR);
	}
    // mask away attributes
    if (ctx.conf.page_table_pointer_mask == 0) {
        code.test(page, page);
    } else if (std::in_range<s32>(ctx.conf.page_table_pointer_mask)) {
        code.and_(page, ctx.conf.page_table_pointer_mask);
    } else {
        code.mov(tmp, ctx.conf.page_table_pointer_mask);
        code.and_(page, tmp);
    }
    // check for sign bit, apply sign extension as needed
    if (ctx.conf.page_table_sign_extension) {
        code.shl(page, 63 - int(*ctx.conf.page_table_sign_extension));
        code.sar(page, 63 - int(*ctx.conf.page_table_sign_extension));
    }

    code.jz(abort, code.T_NEAR);
    if (ctx.conf.absolute_offset_page_table) {
        return page + vaddr;
    }
    code.mov(tmp, vaddr);
    code.and_(tmp, u32(page_table_const_mask));
    return page + tmp.cvt64();
}

template<>
[[maybe_unused]] Xbyak::RegExp EmitVAddrLookup<A64EmitContext>(BlockOfCode& code, A64EmitContext& ctx, size_t bitsize, Xbyak::Label& abort, Xbyak::Reg64 vaddr) {
    const size_t valid_page_index_bits = ctx.conf.page_table_address_space_bits - page_table_const_bits;
    const size_t unused_top_bits = 64 - ctx.conf.page_table_address_space_bits;

    const Xbyak::Reg64 page = ctx.reg_alloc.ScratchGpr(code);
    const Xbyak::Reg64 tmp = ctx.conf.absolute_offset_page_table && ctx.conf.page_table_pointer_mask == 0 ? page : ctx.reg_alloc.ScratchGpr(code);

    EmitDetectMisalignedVAddr(code, ctx, bitsize, abort, vaddr, tmp);

    if (unused_top_bits == 0) {
        code.mov(tmp, vaddr);
        code.shr(tmp, int(page_table_const_bits));
    } else if (ctx.conf.silently_mirror_page_table) {
        if (valid_page_index_bits >= 32) {
            if (code.HasHostFeature(HostFeature::BMI2)) {
                const Xbyak::Reg64 bit_count = ctx.reg_alloc.ScratchGpr(code);
                code.mov(bit_count, unused_top_bits);
                code.bzhi(tmp, vaddr, bit_count);
                code.shr(tmp, int(page_table_const_bits));
                ctx.reg_alloc.Release(bit_count);
            } else {
                code.mov(tmp, vaddr);
                code.shl(tmp, int(unused_top_bits));
                code.shr(tmp, int(unused_top_bits + page_table_const_bits));
            }
        } else {
            code.mov(tmp, vaddr);
            code.shr(tmp, int(page_table_const_bits));
            code.and_(tmp, u32((1 << valid_page_index_bits) - 1));
        }
    } else {
        ASSERT(valid_page_index_bits < 32);
        code.mov(tmp, vaddr);
        code.shr(tmp, int(page_table_const_bits));
        code.test(tmp, u32(-(1 << valid_page_index_bits)));
        code.jnz(abort, code.T_NEAR);
    }

    if (ctx.conf.page_table_log2_stride > 3) {
        code.shl(tmp, int(ctx.conf.page_table_log2_stride));
        code.mov(page, qword[r14 + tmp.cvt64()]);
    } else {
        code.mov(page, qword[r14 + tmp.cvt64() * int(1 << ctx.conf.page_table_log2_stride)]);
    }

    // check for marked bit, use as unmapped if marked
    if (ctx.conf.page_table_marked_bit) {
        code.bt(page, *ctx.conf.page_table_marked_bit);
        code.jc(abort, code.T_NEAR);
    }
    // mask away attributes
    if (ctx.conf.page_table_pointer_mask == 0) {
        code.test(page, page);
    } else if (std::in_range<s32>(ctx.conf.page_table_pointer_mask)) {
        code.and_(page, ctx.conf.page_table_pointer_mask);
    } else {
        code.mov(tmp, ctx.conf.page_table_pointer_mask);
        code.and_(page, tmp);
    }
    // check for sign bit, apply sign extension as needed
    if (ctx.conf.page_table_sign_extension) {
        code.shl(page, *ctx.conf.page_table_sign_extension);
        code.sar(page, *ctx.conf.page_table_sign_extension);
    }

    code.jz(abort, code.T_NEAR);
    if (ctx.conf.absolute_offset_page_table) {
        return page + vaddr;
    }
    code.mov(tmp, vaddr);
    code.and_(tmp, static_cast<u32>(page_table_const_mask));
    return page + tmp;
}

template<typename EmitContext>
Xbyak::RegExp EmitFastmemVAddr(BlockOfCode& code, EmitContext& ctx, Xbyak::Label& abort, Xbyak::Reg64 vaddr, bool& require_abort_handling, std::optional<Xbyak::Reg64> tmp = std::nullopt);

template<>
[[maybe_unused]] Xbyak::RegExp EmitFastmemVAddr<A32EmitContext>(BlockOfCode&, A32EmitContext&, Xbyak::Label&, Xbyak::Reg64 vaddr, bool&, std::optional<Xbyak::Reg64>) {
    return r13 + vaddr;
}

template<>
[[maybe_unused]] Xbyak::RegExp EmitFastmemVAddr<A64EmitContext>(BlockOfCode& code, A64EmitContext& ctx, Xbyak::Label& abort, Xbyak::Reg64 vaddr, bool& require_abort_handling, std::optional<Xbyak::Reg64> tmp) {
    if (ctx.conf.enable_register_cache) return Xbyak::RegExp{vaddr};
    auto const unused_top_bits = 64 - ctx.conf.fastmem_address_space_bits;
    if (unused_top_bits == 0) {
        return r13 + vaddr;
    } else if (ctx.conf.silently_mirror_fastmem) {
        if (!tmp) {
            tmp = ctx.reg_alloc.ScratchGpr(code);
        }
        if (unused_top_bits < 32) {
            code.mov(*tmp, vaddr);
            code.shl(*tmp, int(unused_top_bits));
            code.shr(*tmp, int(unused_top_bits));
        } else if (unused_top_bits == 32) {
            code.mov(tmp->cvt32(), vaddr.cvt32());
        } else {
            code.mov(tmp->cvt32(), vaddr.cvt32());
            code.and_(*tmp, u32((1 << ctx.conf.fastmem_address_space_bits) - 1));
        }
        return r13 + *tmp;
    } else {
        if (ctx.conf.fastmem_address_space_bits < 32) {
            code.test(vaddr, u32(-(1 << ctx.conf.fastmem_address_space_bits)));
            code.jnz(abort, code.T_NEAR);
            require_abort_handling = true;
        } else {
            // TODO: Consider having TEST as above but coalesce 64-bit constant in register allocator
            if (!tmp) {
                tmp = ctx.reg_alloc.ScratchGpr(code);
            }
            code.mov(*tmp, vaddr);
            code.shr(*tmp, int(ctx.conf.fastmem_address_space_bits));
            code.jnz(abort, code.T_NEAR);
            require_abort_handling = true;
        }
        return r13 + vaddr;
    }
}

template<std::size_t bitsize>
const void* EmitReadMemoryMov(BlockOfCode& code, int value_idx, const Xbyak::RegExp& addr, bool ordered, bool force_locked = false) {
    // x86 TSO supplies acquire ordering for ordinary loads. Ordered stores
    // remain XCHG (or a callback followed by MFENCE), supplying store-load
    // ordering for sequential consistency. Keep exclusive and 128-bit reads
    // on their existing locked paths; explicit guest barriers are unchanged.
    if (ordered && (force_locked || bitsize == 128)) {
        if constexpr (bitsize != 128) {
            code.xor_(Xbyak::Reg32(value_idx), Xbyak::Reg32(value_idx));
        } else {
            code.xor_(eax, eax);
            code.xor_(ebx, ebx);
            code.xor_(ecx, ecx);
            code.xor_(edx, edx);
        }

        const void* fastmem_location = code.getCurr();
        switch (bitsize) {
        case 8:
            code.lock();
            code.xadd(code.byte[addr], Xbyak::Reg32(value_idx).cvt8());
            break;
        case 16:
            code.lock();
            code.xadd(word[addr], Xbyak::Reg64(value_idx).cvt16());
            break;
        case 32:
            code.lock();
            code.xadd(dword[addr], Xbyak::Reg64(value_idx).cvt32());
            break;
        case 64:
            code.lock();
            code.xadd(qword[addr], Xbyak::Reg64(value_idx));
            break;
        case 128:
            code.lock();
            code.cmpxchg16b(xword[addr]);
            if (code.HasHostFeature(HostFeature::SSE41)) {
                code.movq(Xbyak::Xmm(value_idx), rax);
                code.pinsrq(Xbyak::Xmm(value_idx), rdx, 1);
            } else {
                code.movq(Xbyak::Xmm(value_idx), rax);
                code.movq(xmm0, rdx);
                code.punpcklqdq(Xbyak::Xmm(value_idx), xmm0);
            }
            break;
        default:
            UNREACHABLE();
        }
        return fastmem_location;
    } else {
        const void* fastmem_location = code.getCurr();
        switch (bitsize) {
        case 8:
            code.movzx(Xbyak::Reg64(value_idx).cvt32(), code.byte[addr]);
            break;
        case 16:
            code.movzx(Xbyak::Reg64(value_idx).cvt32(), word[addr]);
            break;
        case 32:
            code.mov(Xbyak::Reg64(value_idx).cvt32(), dword[addr]);
            break;
        case 64:
            code.mov(Xbyak::Reg64(value_idx), qword[addr]);
            break;
        case 128:
            code.movups(Xbyak::Xmm(value_idx), xword[addr]);
            break;
        default:
            UNREACHABLE();
        }
        return fastmem_location;
    }
}

// Venture: single-copy atomic 16-byte load for hosts with AVX (aligned VMOVDQA,
// Intel SDM 9.1.1; AMD documents the same). Returns the faulting instruction.
inline const void* EmitAtomicLoad128(BlockOfCode& code, int value_idx, const Xbyak::RegExp& addr) {
    const void* fastmem_location = code.getCurr();
    code.vmovdqa(Xbyak::Xmm(value_idx), xword[addr]);
    return fastmem_location;
}

template<std::size_t bitsize>
const void* EmitWriteMemoryMov(BlockOfCode& code, const Xbyak::RegExp& addr, int value_idx, bool ordered) {
    if (ordered) {
        if constexpr (bitsize == 128) {
            code.xor_(eax, eax);
            code.xor_(edx, edx);
            if (code.HasHostFeature(HostFeature::SSE41)) {
                code.movq(rbx, Xbyak::Xmm(value_idx));
                code.pextrq(rcx, Xbyak::Xmm(value_idx), 1);
            } else {
                code.movaps(xmm0, Xbyak::Xmm(value_idx));
                code.movq(rbx, xmm0);
                code.punpckhqdq(xmm0, xmm0);
                code.movq(rcx, xmm0);
            }
        }

        const void* fastmem_location = code.getCurr();
        switch (bitsize) {
        case 8:
            code.xchg(code.byte[addr], Xbyak::Reg64(value_idx).cvt8());
            break;
        case 16:
            code.xchg(word[addr], Xbyak::Reg64(value_idx).cvt16());
            break;
        case 32:
            code.xchg(dword[addr], Xbyak::Reg64(value_idx).cvt32());
            break;
        case 64:
            code.xchg(qword[addr], Xbyak::Reg64(value_idx));
            break;
        case 128: {
            Xbyak::Label loop;
            code.L(loop);
            code.lock();
            code.cmpxchg16b(xword[addr]);
            code.jnz(loop, code.T_NEAR);
            break;
        }
        default:
            UNREACHABLE();
        }
        return fastmem_location;
    } else {
        const void* fastmem_location = code.getCurr();
        switch (bitsize) {
        case 8:
            code.mov(code.byte[addr], Xbyak::Reg64(value_idx).cvt8());
            break;
        case 16:
            code.mov(word[addr], Xbyak::Reg64(value_idx).cvt16());
            break;
        case 32:
            code.mov(dword[addr], Xbyak::Reg64(value_idx).cvt32());
            break;
        case 64:
            code.mov(qword[addr], Xbyak::Reg64(value_idx));
            break;
        case 128:
            code.movups(xword[addr], Xbyak::Xmm(value_idx));
            break;
        default:
            UNREACHABLE();
        }
        return fastmem_location;
    }
}

template<typename UserConfig>
void EmitExclusiveLock(BlockOfCode& code, const UserConfig& conf, Xbyak::Reg64 pointer, Xbyak::Reg32 tmp) {
    if (conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor)) {
        return;
    }

    code.mov(pointer, std::bit_cast<u64>(GetExclusiveMonitorLockPointer(conf.global_monitor)));
    EmitSpinLockLock(code, pointer, tmp, code.HasHostFeature(HostFeature::WAITPKG));
}

template<typename UserConfig>
void EmitExclusiveUnlock(BlockOfCode& code, const UserConfig& conf, Xbyak::Reg64 pointer, Xbyak::Reg32 tmp) {
    if (conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor)) {
        return;
    }

    code.mov(pointer, std::bit_cast<u64>(GetExclusiveMonitorLockPointer(conf.global_monitor)));
    EmitSpinLockUnlock(code, pointer, tmp);
}

// Venture: the store-exclusive's exclusive load is earlier in this block and
// nothing between them reads memory, so the stored value derives only from
// that load and registers. Value compare is then a linearizable
// compare-and-swap, the generation claim is unnecessary, and the store does not
// advance the generation (a dependent pair, such as a list pop, still sees
// every claiming store).
inline bool ExclusivePairIsLocal(const IR::Block& block, const IR::Inst* store) {
    bool local = false;
    for (const IR::Inst& inst : block.Instructions()) {
        if (&inst == store)
            return local;
        const auto op = inst.GetOpcode();
        if (IR::IsExclusiveMemoryRead(op)) {
            local = true;
        } else if (IR::IsMemoryRead(op) || IR::IsExclusiveMemoryWrite(op)) {
            local = false;
        } else {
            switch (op) {
            case IR::Opcode::A64AtomicMemory8:
            case IR::Opcode::A64AtomicMemory16:
            case IR::Opcode::A64AtomicMemory32:
            case IR::Opcode::A64AtomicMemory64:
            case IR::Opcode::A64CompareAndSwapMemory8:
            case IR::Opcode::A64CompareAndSwapMemory16:
            case IR::Opcode::A64CompareAndSwapMemory32:
            case IR::Opcode::A64CompareAndSwapMemory64:
            case IR::Opcode::A64CompareAndSwapMemory128:
                local = false;
                break;
            default:
                break;
            }
        }
    }
    return false;
}

// Venture: the store-exclusive follows an exclusive read in this block with no
// other memory access, clear-exclusive, supervisor call or exception between
// them: a register-only read-modify-write of the reserved location.
inline bool ExclusivePairIsAdjacent(const IR::Block& block, const IR::Inst* store) {
    bool adjacent = false;
    for (const IR::Inst& inst : block.Instructions()) {
        if (&inst == store)
            return adjacent;
        const auto op = inst.GetOpcode();
        if (IR::IsExclusiveMemoryRead(op)) {
            adjacent = true;
        } else if (IR::IsMemoryReadOrWrite(op)) {
            adjacent = false;
        } else {
            switch (op) {
            case IR::Opcode::A32ClearExclusive:
            case IR::Opcode::A64ClearExclusive:
            case IR::Opcode::A32CallSupervisor:
            case IR::Opcode::A64CallSupervisor:
            case IR::Opcode::A32ExceptionRaised:
            case IR::Opcode::A64ExceptionRaised:
                adjacent = false;
                break;
            default:
                break;
            }
        }
    }
    return false;
}

// Venture: out = &generation bucket of vaddr's 64-byte granule. Bits 6-47
// only, so a tagged pointer reserves the same granule as an untagged one.
inline void EmitExclusiveGenerationBucket(BlockOfCode& code, std::uint64_t* base, Xbyak::Reg64 vaddr, Xbyak::Reg64 out, Xbyak::Reg64 scratch) {
    static_assert((ExclusiveMonitor::GENERATION_BUCKETS & (ExclusiveMonitor::GENERATION_BUCKETS - 1)) == 0);
    code.mov(out, vaddr);
    code.shl(out, 16);
    code.shr(out, 22);
    code.mov(scratch, out);
    code.shr(scratch, 12);
    code.xor_(out, scratch);
    code.and_(out.cvt32(), u32(ExclusiveMonitor::GENERATION_BUCKETS - 1));
    code.shl(out.cvt32(), 6);
    code.mov(scratch, std::bit_cast<u64>(base));
    code.add(out, scratch);
}

template<typename UserConfig>
void EmitExclusiveTestAndClear(BlockOfCode& code, const UserConfig& conf, Xbyak::Reg64 vaddr, Xbyak::Reg64 pointer, Xbyak::Reg64 tmp) {
    if (conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor)) {
        return;
    }

    // A block may run after more guest CPUs acquire reservations. Load the
    // active bound at execution time instead of baking in a compilation-time
    // count or unrolling the entire 1024-slot capacity at every store.
    constexpr size_t stride = 64;
    ASSERT(GetExclusiveMonitorProcessorStride() == stride);
    code.mov(tmp, std::bit_cast<u64>(GetExclusiveMonitorActiveCountPointer(conf.global_monitor)));
    code.mov(tmp, qword[tmp]);
    code.shl(tmp, 6);
    code.mov(pointer, std::bit_cast<u64>(GetExclusiveMonitorAddressBase(conf.global_monitor)));
    code.add(tmp, pointer);
    Xbyak::Label loop, next, end;
    code.L(loop);
    code.cmp(pointer, tmp);
    code.je(end, code.T_NEAR);
    code.cmp(qword[pointer], vaddr);
    code.jne(next, code.T_NEAR);
    // The lock serializes all address readers. Two immediate stores avoid
    // borrowing another GPR from the 128-bit CMPXCHG16B operand set.
    code.mov(dword[pointer], u32(0xDEAD'DEAD));
    code.mov(dword[pointer + 4], u32(0xDEAD'DEAD));
    code.L(next);
    code.add(pointer, stride);
    code.jmp(loop, code.T_NEAR);
    code.L(end);
}

inline bool IsOrdered(IR::AccType acctype) {
    return acctype == IR::AccType::ORDERED || acctype == IR::AccType::ORDEREDRW || acctype == IR::AccType::LIMITEDORDERED;
}

}  // namespace

}  // namespace Dynarmic::Backend::X64
