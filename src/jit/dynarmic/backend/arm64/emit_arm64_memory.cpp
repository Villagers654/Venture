// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/arm64/emit_arm64_memory.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <cstddef>

#include <bit>
#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/exclusive_monitor_friend.h"
#include "dynarmic/common/spin_lock_arm64.h"
#include "dynarmic/interface/halt_reason.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/emit_context.h"
#include "dynarmic/backend/arm64/fastmem.h"
#include "dynarmic/backend/arm64/fpsr_manager.h"
#include "dynarmic/backend/arm64/reg_alloc.h"
#include "dynarmic/ir/acc_type.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/interface/optimization_flags.h"
#include "dynarmic/interface/A64/config.h"

namespace Dynarmic::Backend::Arm64 {

using namespace oaknut::util;

namespace {

bool IsOrdered(IR::AccType acctype) {
    return acctype == IR::AccType::ORDERED || acctype == IR::AccType::ORDEREDRW || acctype == IR::AccType::LIMITEDORDERED;
}

LinkTarget ReadMemoryLinkTarget(std::size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::ReadMemory8;
    case 16:
        return LinkTarget::ReadMemory16;
    case 32:
        return LinkTarget::ReadMemory32;
    case 64:
        return LinkTarget::ReadMemory64;
    case 128:
        return LinkTarget::ReadMemory128;
    }
    UNREACHABLE();
}

LinkTarget WriteMemoryLinkTarget(std::size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::WriteMemory8;
    case 16:
        return LinkTarget::WriteMemory16;
    case 32:
        return LinkTarget::WriteMemory32;
    case 64:
        return LinkTarget::WriteMemory64;
    case 128:
        return LinkTarget::WriteMemory128;
    }
    UNREACHABLE();
}

LinkTarget WrappedReadMemoryLinkTarget(std::size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::WrappedReadMemory8;
    case 16:
        return LinkTarget::WrappedReadMemory16;
    case 32:
        return LinkTarget::WrappedReadMemory32;
    case 64:
        return LinkTarget::WrappedReadMemory64;
    case 128:
        return LinkTarget::WrappedReadMemory128;
    }
    UNREACHABLE();
}

LinkTarget WrappedWriteMemoryLinkTarget(std::size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::WrappedWriteMemory8;
    case 16:
        return LinkTarget::WrappedWriteMemory16;
    case 32:
        return LinkTarget::WrappedWriteMemory32;
    case 64:
        return LinkTarget::WrappedWriteMemory64;
    case 128:
        return LinkTarget::WrappedWriteMemory128;
    }
    UNREACHABLE();
}

LinkTarget ExclusiveReadMemoryLinkTarget(std::size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::ExclusiveReadMemory8;
    case 16:
        return LinkTarget::ExclusiveReadMemory16;
    case 32:
        return LinkTarget::ExclusiveReadMemory32;
    case 64:
        return LinkTarget::ExclusiveReadMemory64;
    case 128:
        return LinkTarget::ExclusiveReadMemory128;
    }
    UNREACHABLE();
}

LinkTarget ExclusiveWriteMemoryLinkTarget(std::size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::ExclusiveWriteMemory8;
    case 16:
        return LinkTarget::ExclusiveWriteMemory16;
    case 32:
        return LinkTarget::ExclusiveWriteMemory32;
    case 64:
        return LinkTarget::ExclusiveWriteMemory64;
    case 128:
        return LinkTarget::ExclusiveWriteMemory128;
    }
    UNREACHABLE();
}

template<std::size_t bitsize>
void CallbackOnlyEmitReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[1]);
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());

    EmitRelocation(code, ctx, ReadMemoryLinkTarget(bitsize));
    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }

    // Venture: a callback that aborted leaves at the access, as the fastmem and
    // page table fallbacks do.
    oaknut::Label end;
    if constexpr (bitsize == 128) {
        code.MOV(Q8.B16(), Q0.B16());
        ctx.conf.emit_check_memory_abort(code, ctx, inst, end);
        code.l(end);
        ctx.reg_alloc.DefineAsRegister(inst, Q8);
    } else {
        ctx.conf.emit_check_memory_abort(code, ctx, inst, end);
        code.l(end);
        ctx.reg_alloc.DefineAsRegister(inst, X0);
    }
}

template<std::size_t bitsize>
void CallbackOnlyEmitExclusiveReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[1]);
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());

    code.MOV(Wscratch0, 1);
    code.STRB(Wscratch0, Xstate, ctx.conf.state_exclusive_state_offset);
    EmitRelocation(code, ctx, ExclusiveReadMemoryLinkTarget(bitsize));
    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }

    // Venture: a callback that aborted leaves at the access, as the fastmem and
    // page table fallbacks do.
    oaknut::Label end;
    if constexpr (bitsize == 128) {
        code.MOV(Q8.B16(), Q0.B16());
        ctx.conf.emit_check_memory_abort(code, ctx, inst, end);
        code.l(end);
        ctx.reg_alloc.DefineAsRegister(inst, Q8);
    } else {
        ctx.conf.emit_check_memory_abort(code, ctx, inst, end);
        code.l(end);
        ctx.reg_alloc.DefineAsRegister(inst, X0);
    }
}

template<std::size_t bitsize>
void CallbackOnlyEmitWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[1], args[2]);
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());

    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }
    EmitRelocation(code, ctx, WriteMemoryLinkTarget(bitsize));
    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }
    oaknut::Label end;
    ctx.conf.emit_check_memory_abort(code, ctx, inst, end);
    code.l(end);
}

template<std::size_t bitsize>
void CallbackOnlyEmitExclusiveWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[1], args[2]);
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());

    oaknut::Label end;

    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }
    code.MOV(W0, 1);
    code.LDRB(Wscratch0, Xstate, ctx.conf.state_exclusive_state_offset);
    code.CBZ(Wscratch0, end);
    code.STRB(WZR, Xstate, ctx.conf.state_exclusive_state_offset);
    EmitRelocation(code, ctx, ExclusiveWriteMemoryLinkTarget(bitsize));
    if (ordered) {
        code.DMB(oaknut::BarrierOp::ISH);
    }
    ctx.conf.emit_check_memory_abort(code, ctx, inst, end);
    code.l(end);
    ctx.reg_alloc.DefineAsRegister(inst, X0);
}

constexpr std::size_t page_table_const_bits = 12;
constexpr std::size_t page_table_const_size = 1 << page_table_const_bits;
constexpr std::size_t page_table_const_mask = (1 << page_table_const_bits) - 1;

// This function may use Xscratch0 as a scratch register
// Trashes NZCV
template<std::size_t bitsize>
void EmitDetectMisalignedVAddr(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr, const SharedLabel& fallback) {
    static_assert(bitsize == 8 || bitsize == 16 || bitsize == 32 || bitsize == 64 || bitsize == 128);

    if (bitsize == 8 || (ctx.conf.detect_misaligned_access_via_page_table & bitsize) == 0) {
        return;
    }

    if (!ctx.conf.only_detect_misalignment_via_page_table_on_page_boundary) {
        const u64 align_mask = []() -> u64 {
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

        code.TST(Xaddr, align_mask);
        code.B(NE, *fallback);
    } else {
        // If (addr & page_table_const_mask) > page_table_const_size - byte_size, use fallback.
        code.AND(Xscratch0, Xaddr, page_table_const_mask);
        code.CMP(Xscratch0, page_table_const_size - bitsize / 8);
        code.B(HI, *fallback);
    }
}

// Outputs Xscratch0 = page_table[addr >> page_table_const_bits]
// May use Xscratch1 as scratch register
// Address to read/write = [ret0 + ret1], ret0 is always Xscratch0 and ret1 is either Xaddr or Xscratch1
// Trashes NZCV
template<std::size_t bitsize>
std::pair<oaknut::XReg, oaknut::XReg> InlinePageTableEmitVAddrLookup(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr, const SharedLabel& fallback) {
    const std::size_t valid_page_index_bits = ctx.conf.page_table_address_space_bits - page_table_const_bits;
    const std::size_t unused_top_bits = 64 - ctx.conf.page_table_address_space_bits;

    EmitDetectMisalignedVAddr<bitsize>(code, ctx, Xaddr, fallback);

    if (ctx.conf.silently_mirror_page_table || unused_top_bits == 0) {
        code.UBFX(Xscratch0, Xaddr, page_table_const_bits, valid_page_index_bits);
    } else {
        code.LSR(Xscratch0, Xaddr, page_table_const_bits);
        code.TST(Xscratch0, u64(~u64(0)) << valid_page_index_bits);
        code.B(NE, *fallback);
    }

    // index = index << log2
    code.LSL(Xscratch0, Xscratch0, ctx.conf.page_table_log2_stride);
    // load x0 = *<(u8*)pagetable + index>
    code.LDR(Xscratch0, Xpagetable, Xscratch0);

    if (ctx.conf.page_table_marked_bit) {
        code.TST(Xscratch0, 1ULL << *ctx.conf.page_table_marked_bit);
        code.B(NE, *fallback);
    }

    if (ctx.conf.page_table_pointer_mask != 0) {
        code.AND(Xscratch0, Xscratch0, ctx.conf.page_table_pointer_mask);
    }

    // TODO: combine this with page_table_pointer_mask
    if (ctx.conf.page_table_sign_extension) {
        code.SBFM(Xscratch0, Xscratch0, 0, *ctx.conf.page_table_sign_extension);
    }

    code.CBZ(Xscratch0, *fallback);

    if (ctx.conf.absolute_offset_page_table) {
        return std::make_pair(Xscratch0, Xaddr);
    }
    code.AND(Xscratch1, Xaddr, page_table_const_mask);
    return std::make_pair(Xscratch0, Xscratch1);
}

template<std::size_t bitsize>
CodePtr EmitMemoryLdr(oaknut::CodeGenerator& code, int value_idx, oaknut::XReg Xbase, oaknut::XReg Xoffset, bool ordered, bool extend32 = false) {
    const auto index_ext = extend32 ? oaknut::IndexExt::UXTW : oaknut::IndexExt::LSL;
    const auto add_ext = extend32 ? oaknut::AddSubExt::UXTW : oaknut::AddSubExt::LSL;
    const auto Roffset = extend32 ? oaknut::RReg{Xoffset.toW()} : oaknut::RReg{Xoffset};

    CodePtr fastmem_location = code.xptr<CodePtr>();

    if (ordered) {
        code.ADD(Xscratch0, Xbase, Roffset, add_ext);

        fastmem_location = code.xptr<CodePtr>();

        switch (bitsize) {
        case 8:
            code.LDARB(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 16:
            code.LDARH(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 32:
            code.LDAR(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 64:
            code.LDAR(oaknut::XReg{value_idx}, Xscratch0);
            break;
        case 128:
            code.LDR(oaknut::QReg{value_idx}, Xscratch0);
            code.DMB(oaknut::BarrierOp::ISH);
            break;
        default:
            UNREACHABLE();
        }
    } else {
        fastmem_location = code.xptr<CodePtr>();

        switch (bitsize) {
        case 8:
            code.LDRB(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 16:
            code.LDRH(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 32:
            code.LDR(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 64:
            code.LDR(oaknut::XReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 128:
            code.LDR(oaknut::QReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        default:
            UNREACHABLE();
        }
    }

    return fastmem_location;
}

template<std::size_t bitsize>
CodePtr EmitMemoryStr(oaknut::CodeGenerator& code, int value_idx, oaknut::XReg Xbase, oaknut::XReg Xoffset, bool ordered, bool extend32 = false) {
    const auto index_ext = extend32 ? oaknut::IndexExt::UXTW : oaknut::IndexExt::LSL;
    const auto add_ext = extend32 ? oaknut::AddSubExt::UXTW : oaknut::AddSubExt::LSL;
    const auto Roffset = extend32 ? oaknut::RReg{Xoffset.toW()} : oaknut::RReg{Xoffset};

    CodePtr fastmem_location;

    if (ordered) {
        code.ADD(Xscratch0, Xbase, Roffset, add_ext);

        fastmem_location = code.xptr<CodePtr>();

        switch (bitsize) {
        case 8:
            code.STLRB(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 16:
            code.STLRH(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 32:
            code.STLR(oaknut::WReg{value_idx}, Xscratch0);
            break;
        case 64:
            code.STLR(oaknut::XReg{value_idx}, Xscratch0);
            break;
        case 128:
            code.DMB(oaknut::BarrierOp::ISH);
            // Venture: the store is the access that faults, not the barrier.
            fastmem_location = code.xptr<CodePtr>();
            code.STR(oaknut::QReg{value_idx}, Xscratch0);
            code.DMB(oaknut::BarrierOp::ISH);
            break;
        default:
            UNREACHABLE();
        }
    } else {
        fastmem_location = code.xptr<CodePtr>();

        switch (bitsize) {
        case 8:
            code.STRB(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 16:
            code.STRH(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 32:
            code.STR(oaknut::WReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 64:
            code.STR(oaknut::XReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        case 128:
            code.STR(oaknut::QReg{value_idx}, Xbase, Roffset, index_ext);
            break;
        default:
            UNREACHABLE();
        }
    }

    return fastmem_location;
}

template<std::size_t bitsize>
void InlinePageTableEmitReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = [&] {
        if constexpr (bitsize == 128) {
            return ctx.reg_alloc.WriteQ(inst);
        } else {
            return ctx.reg_alloc.WriteReg<std::max<std::size_t>(bitsize, 32)>(inst);
        }
    }();
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());
    // Venture: the host FPSR stays live across the access; only the fallback folds it.
    const bool fold_fpsr = ctx.fpsr.IsLoaded();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Rvalue);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();

    const auto [Xbase, Xoffset] = InlinePageTableEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback);
    EmitMemoryLdr<bitsize>(code, Rvalue->index(), Xbase, Xoffset, ordered);

    ctx.deferred_emits.emplace_back([&code, &ctx, inst, Xaddr = *Xaddr, Rvalue = *Rvalue, ordered, fold_fpsr, fallback, end] {
        code.l(*fallback);
        if (fold_fpsr) {
            ctx.fpsr.EmitFold();
        }
        code.MOV(Xscratch0, Xaddr);
        EmitRelocation(code, ctx, WrappedReadMemoryLinkTarget(bitsize));
        if (fold_fpsr) {
            ctx.fpsr.EmitClearHost();
        }
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        if constexpr (bitsize == 128) {
            code.MOV(Rvalue.B16(), Q0.B16());
        } else {
            code.MOV(Rvalue.toX(), Xscratch0);
        }
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
        code.B(*end);
    });

    code.l(*end);
}

template<std::size_t bitsize>
void InlinePageTableEmitWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = [&] {
        if constexpr (bitsize == 128) {
            return ctx.reg_alloc.ReadQ(args[2]);
        } else {
            return ctx.reg_alloc.ReadReg<std::max<std::size_t>(bitsize, 32)>(args[2]);
        }
    }();
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());
    // Venture: the host FPSR stays live across the access; only the fallback folds it.
    const bool fold_fpsr = ctx.fpsr.IsLoaded();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Rvalue);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();

    const auto [Xbase, Xoffset] = InlinePageTableEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback);
    EmitMemoryStr<bitsize>(code, Rvalue->index(), Xbase, Xoffset, ordered);

    ctx.deferred_emits.emplace_back([&code, &ctx, inst, Xaddr = *Xaddr, Rvalue = *Rvalue, ordered, fold_fpsr, fallback, end] {
        code.l(*fallback);
        if (fold_fpsr) {
            ctx.fpsr.EmitFold();
        }
        if constexpr (bitsize == 128) {
            code.MOV(Xscratch0, Xaddr);
            code.MOV(Q0.B16(), Rvalue.B16());
        } else {
            code.MOV(Xscratch0, Xaddr);
            code.MOV(Xscratch1, Rvalue.toX());
        }
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        EmitRelocation(code, ctx, WrappedWriteMemoryLinkTarget(bitsize));
        if (fold_fpsr) {
            ctx.fpsr.EmitClearHost();
        }
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
        code.B(*end);
    });

    code.l(*end);
}

std::optional<DoNotFastmemMarker> ShouldFastmem(EmitContext& ctx, IR::Inst* inst) {
    if (!ctx.conf.fastmem_pointer || !ctx.fastmem.SupportsFastmem()) {
        return std::nullopt;
    }

    const auto marker = std::make_tuple(ctx.block.Location(), inst->GetName());
    if (ctx.fastmem.ShouldFastmem(marker)) {
        return marker;
    }
    return std::nullopt;
}

inline bool ShouldExt32(EmitContext& ctx) {
    return ctx.conf.fastmem_address_space_bits == 32 && ctx.conf.silently_mirror_fastmem;
}

// May use Xscratch0 as scratch register
// Address to read/write = [ret0 + ret1], ret0 is always Xfastmem and ret1 is either Xaddr or Xscratch0
// Trashes NZCV
template<std::size_t bitsize>
std::pair<oaknut::XReg, oaknut::XReg> FastmemEmitVAddrLookup(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr, const SharedLabel& fallback) {
    if (ctx.conf.fastmem_address_space_bits == 64 || ShouldExt32(ctx)) {
        return std::make_pair(Xfastmem, Xaddr);
    }

    if (ctx.conf.silently_mirror_fastmem) {
        code.UBFX(Xscratch0, Xaddr, 0, ctx.conf.fastmem_address_space_bits);
        return std::make_pair(Xfastmem, Xscratch0);
    }

    code.LSR(Xscratch0, Xaddr, ctx.conf.fastmem_address_space_bits);
    code.CBNZ(Xscratch0, *fallback);
    return std::make_pair(Xfastmem, Xaddr);
}

}  // namespace

// Venture: address folding (OptimizationFlag::AddressFolding), as the x64
// backend's FoldMemoryAddresses. With identity fastmem (fastmem_pointer 0 and
// 64-bit addresses) the guest address is the host address, so an unordered A64
// access whose address is a single-use Add64 addresses memory as [base, #imm]
// (a scaled unsigned 12-bit or a signed 9-bit offset) or as
// [base, index, LSL #shift], the index a single-use LogicalShiftLeft64 by the
// access size (shift) or the add's other operand (no shift). The add and that
// shift emit nothing: each aliases its first operand, and the access reads the
// index itself. The fallback computes the address into Xscratch0.
void FoldA64Addresses(EmitContext& ctx) {
    const auto& conf = ctx.conf;
    if (!conf.is_a64 || !conf.HasOptimization(OptimizationFlag::AddressFolding) || conf.very_verbose_debugging_output ||
        !conf.fastmem_pointer || *conf.fastmem_pointer != 0 || conf.fastmem_address_space_bits != 64) {
        return;
    }
    for (auto& inst : ctx.block.instructions) {
        std::size_t bytes;
        std::size_t acc_type_arg = 2;
        switch (inst.GetOpcode()) {
        case IR::Opcode::A64WriteMemory8:
            acc_type_arg = 3;
            [[fallthrough]];
        case IR::Opcode::A64ReadMemory8:
            bytes = 1;
            break;
        case IR::Opcode::A64WriteMemory16:
            acc_type_arg = 3;
            [[fallthrough]];
        case IR::Opcode::A64ReadMemory16:
            bytes = 2;
            break;
        case IR::Opcode::A64WriteMemory32:
            acc_type_arg = 3;
            [[fallthrough]];
        case IR::Opcode::A64ReadMemory32:
            bytes = 4;
            break;
        case IR::Opcode::A64WriteMemory64:
            acc_type_arg = 3;
            [[fallthrough]];
        case IR::Opcode::A64ReadMemory64:
            bytes = 8;
            break;
        case IR::Opcode::A64WriteMemory128:
            acc_type_arg = 3;
            [[fallthrough]];
        case IR::Opcode::A64ReadMemory128:
            bytes = 16;
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
            !add->GetArg(2).IsImmediate() || add->GetArg(2).GetU1() || add->HasAssociatedPseudoOperation()) {
            continue;
        }
        const u8 size_log2 = static_cast<u8>(std::countr_zero(bytes));
        EmitContext::FoldedAddress folded;
        if (add->GetArg(1).IsImmediate()) {
            const s64 imm = static_cast<s64>(add->GetArg(1).GetU64());
            const bool scaled = imm >= 0 && imm % s64(bytes) == 0 && imm / s64(bytes) < 4096;
            if (!scaled && (imm < -256 || imm > 255)) {
                continue;
            }
            folded.imm = imm;
        } else {
            if (add->GetArg(0).GetInst() == add->GetArg(1).GetInst()) {
                continue;
            }
            folded.index = add->GetArg(1);
            for (const std::size_t k : {std::size_t(1), std::size_t(0)}) {
                IR::Inst* const lsl = add->GetArg(k).GetInst();
                if (size_log2 != 0 && lsl->GetOpcode() == IR::Opcode::LogicalShiftLeft64 && lsl->UseCount() == 1 &&
                    !lsl->GetArg(0).IsImmediate() && lsl->GetArg(1).IsImmediate() && lsl->GetArg(1).GetU8() == size_log2 &&
                    !lsl->HasAssociatedPseudoOperation()) {
                    folded.base_arg = 1 - k;
                    folded.index = add->GetArg(k);
                    folded.shift = size_log2;
                    folded.shift_inst = lsl;
                    break;
                }
            }
        }
        ctx.folded_accesses.emplace(&inst, folded);
        ctx.folded_adds.emplace(add, &inst);
        if (folded.shift_inst) {
            ctx.folded_shifts.insert(folded.shift_inst);
        }
        if (folded.index) {
            ctx.reg_alloc.AddUse(folded.index->GetInst(), &inst);
        }
    }
}

bool EmitFoldedAddressAlias(EmitContext& ctx, IR::Inst* inst) {
    if (ctx.folded_shifts.contains(inst)) {
        auto args = ctx.reg_alloc.GetArgumentInfo(inst);
        ctx.reg_alloc.DefineAsExisting(inst, args[0]);
        return true;
    }
    const auto it = ctx.folded_adds.find(inst);
    if (it == ctx.folded_adds.end()) {
        return false;
    }
    auto base = ctx.reg_alloc.UseArgument(inst->GetArg(ctx.folded_accesses.at(it->second).base_arg));
    ctx.reg_alloc.DefineAsExisting(inst, base);
    return true;
}

namespace {

const EmitContext::FoldedAddress* FoldedAddressOf(EmitContext& ctx, const IR::Inst* inst) {
    const auto it = ctx.folded_accesses.find(inst);
    return it == ctx.folded_accesses.end() ? nullptr : &it->second;
}

// A folded access's address, from its base register (and index register).
struct FoldedOperand {
    std::optional<s64> imm;
    int index = -1;
    u8 shift = 0;

    template<std::size_t bitsize>
    bool ScaledImm() const {
        constexpr s64 bytes = bitsize / 8;
        return *imm >= 0 && *imm % bytes == 0 && *imm / bytes < 4096;
    }

    void EmitAddress(oaknut::CodeGenerator& code, oaknut::XReg Xbase, oaknut::XReg Xout) const {
        if (imm) {
            code.MOV(Xout, static_cast<u64>(*imm));
            code.ADD(Xout, Xbase, Xout);
        } else {
            code.ADD(Xout, Xbase, oaknut::XReg{index}, oaknut::AddSubShift::LSL, shift);
        }
    }
};

template<std::size_t bitsize>
CodePtr EmitFoldedLdr(oaknut::CodeGenerator& code, int value_idx, oaknut::XReg Xbase, const FoldedOperand& op) {
    const CodePtr location = code.xptr<CodePtr>();
    if (!op.imm) {
        const oaknut::XReg Xindex{op.index};
        switch (bitsize) {
        case 8:
            code.LDRB(oaknut::WReg{value_idx}, Xbase, Xindex, oaknut::IndexExt::LSL, op.shift);
            break;
        case 16:
            code.LDRH(oaknut::WReg{value_idx}, Xbase, Xindex, oaknut::IndexExt::LSL, op.shift);
            break;
        case 32:
            code.LDR(oaknut::WReg{value_idx}, Xbase, Xindex, oaknut::IndexExt::LSL, op.shift);
            break;
        case 64:
            code.LDR(oaknut::XReg{value_idx}, Xbase, Xindex, oaknut::IndexExt::LSL, op.shift);
            break;
        case 128:
            code.LDR(oaknut::QReg{value_idx}, Xbase, Xindex, oaknut::IndexExt::LSL, op.shift);
            break;
        default:
            UNREACHABLE();
        }
    } else if (op.ScaledImm<bitsize>()) {
        switch (bitsize) {
        case 8:
            code.LDRB(oaknut::WReg{value_idx}, Xbase, *op.imm);
            break;
        case 16:
            code.LDRH(oaknut::WReg{value_idx}, Xbase, *op.imm);
            break;
        case 32:
            code.LDR(oaknut::WReg{value_idx}, Xbase, *op.imm);
            break;
        case 64:
            code.LDR(oaknut::XReg{value_idx}, Xbase, *op.imm);
            break;
        case 128:
            code.LDR(oaknut::QReg{value_idx}, Xbase, *op.imm);
            break;
        default:
            UNREACHABLE();
        }
    } else {
        switch (bitsize) {
        case 8:
            code.LDURB(oaknut::WReg{value_idx}, Xbase, *op.imm);
            break;
        case 16:
            code.LDURH(oaknut::WReg{value_idx}, Xbase, *op.imm);
            break;
        case 32:
            code.LDUR(oaknut::WReg{value_idx}, Xbase, *op.imm);
            break;
        case 64:
            code.LDUR(oaknut::XReg{value_idx}, Xbase, *op.imm);
            break;
        case 128:
            code.LDUR(oaknut::QReg{value_idx}, Xbase, *op.imm);
            break;
        default:
            UNREACHABLE();
        }
    }
    return location;
}

template<std::size_t bitsize>
CodePtr EmitFoldedStr(oaknut::CodeGenerator& code, int value_idx, oaknut::XReg Xbase, const FoldedOperand& op) {
    const CodePtr location = code.xptr<CodePtr>();
    if (!op.imm) {
        const oaknut::XReg Xindex{op.index};
        switch (bitsize) {
        case 8:
            code.STRB(oaknut::WReg{value_idx}, Xbase, Xindex, oaknut::IndexExt::LSL, op.shift);
            break;
        case 16:
            code.STRH(oaknut::WReg{value_idx}, Xbase, Xindex, oaknut::IndexExt::LSL, op.shift);
            break;
        case 32:
            code.STR(oaknut::WReg{value_idx}, Xbase, Xindex, oaknut::IndexExt::LSL, op.shift);
            break;
        case 64:
            code.STR(oaknut::XReg{value_idx}, Xbase, Xindex, oaknut::IndexExt::LSL, op.shift);
            break;
        case 128:
            code.STR(oaknut::QReg{value_idx}, Xbase, Xindex, oaknut::IndexExt::LSL, op.shift);
            break;
        default:
            UNREACHABLE();
        }
    } else if (op.ScaledImm<bitsize>()) {
        switch (bitsize) {
        case 8:
            code.STRB(oaknut::WReg{value_idx}, Xbase, *op.imm);
            break;
        case 16:
            code.STRH(oaknut::WReg{value_idx}, Xbase, *op.imm);
            break;
        case 32:
            code.STR(oaknut::WReg{value_idx}, Xbase, *op.imm);
            break;
        case 64:
            code.STR(oaknut::XReg{value_idx}, Xbase, *op.imm);
            break;
        case 128:
            code.STR(oaknut::QReg{value_idx}, Xbase, *op.imm);
            break;
        default:
            UNREACHABLE();
        }
    } else {
        switch (bitsize) {
        case 8:
            code.STURB(oaknut::WReg{value_idx}, Xbase, *op.imm);
            break;
        case 16:
            code.STURH(oaknut::WReg{value_idx}, Xbase, *op.imm);
            break;
        case 32:
            code.STUR(oaknut::WReg{value_idx}, Xbase, *op.imm);
            break;
        case 64:
            code.STUR(oaknut::XReg{value_idx}, Xbase, *op.imm);
            break;
        case 128:
            code.STUR(oaknut::QReg{value_idx}, Xbase, *op.imm);
            break;
        default:
            UNREACHABLE();
        }
    }
    return location;
}

template<std::size_t bitsize>
void FastmemEmitReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, DoNotFastmemMarker marker) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const auto* const folded = FoldedAddressOf(ctx, inst);
    Argument index_arg = folded && folded->index ? ctx.reg_alloc.UseArgument(*folded->index) : Argument{};
    std::optional<RAReg<oaknut::XReg>> Xindex;
    if (folded && folded->index) {
        Xindex.emplace(ctx.reg_alloc.ReadX(index_arg));
        RegAlloc::Realize(*Xindex);
    }
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = [&] {
        if constexpr (bitsize == 128) {
            return ctx.reg_alloc.WriteQ(inst);
        } else {
            return ctx.reg_alloc.WriteReg<std::max<std::size_t>(bitsize, 32)>(inst);
        }
    }();
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());
    // Venture: the host FPSR stays live across the access; only the fallback folds it.
    const bool fold_fpsr = ctx.fpsr.IsLoaded();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Rvalue);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();

    std::optional<FoldedOperand> folded_op;
    if (folded) {
        folded_op = FoldedOperand{folded->imm, Xindex ? (*Xindex)->index() : -1, folded->shift};
    }
    const auto fastmem_location = [&] {
        if (folded_op) {
            return EmitFoldedLdr<bitsize>(code, Rvalue->index(), *Xaddr, *folded_op);
        }
        const auto [Xbase, Xoffset] = FastmemEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback);
        return EmitMemoryLdr<bitsize>(code, Rvalue->index(), Xbase, Xoffset, ordered, ShouldExt32(ctx));
    }();

    ctx.deferred_emits.emplace_back([&code, &ctx, inst, marker, Xaddr = *Xaddr, Rvalue = *Rvalue, ordered, fold_fpsr, fallback, end, fastmem_location, folded_op] {
        ctx.ebi.fastmem_patch_info.emplace(
            fastmem_location - ctx.ebi.entry_point,
            FastmemPatchInfo{
                .marker = marker,
                .fc = FakeCall{
                    .call_pc = std::bit_cast<u64>(code.xptr<void*>()),
                },
                .recompile = ctx.conf.recompile_on_fastmem_failure,
            });

        code.l(*fallback);
        if (fold_fpsr) {
            ctx.fpsr.EmitFold();
        }
        if (folded_op) {
            folded_op->EmitAddress(code, Xaddr, Xscratch0);
        } else {
            code.MOV(Xscratch0, Xaddr);
        }
        EmitRelocation(code, ctx, WrappedReadMemoryLinkTarget(bitsize));
        if (fold_fpsr) {
            ctx.fpsr.EmitClearHost();
        }
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        if constexpr (bitsize == 128) {
            code.MOV(Rvalue.B16(), Q0.B16());
        } else {
            code.MOV(Rvalue.toX(), Xscratch0);
        }
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
        code.B(*end);
    });

    code.l(*end);
}

template<std::size_t bitsize>
void FastmemEmitWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, DoNotFastmemMarker marker) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const auto* const folded = FoldedAddressOf(ctx, inst);
    Argument index_arg = folded && folded->index ? ctx.reg_alloc.UseArgument(*folded->index) : Argument{};
    std::optional<RAReg<oaknut::XReg>> Xindex;
    if (folded && folded->index) {
        Xindex.emplace(ctx.reg_alloc.ReadX(index_arg));
        RegAlloc::Realize(*Xindex);
    }
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = [&] {
        if constexpr (bitsize == 128) {
            return ctx.reg_alloc.ReadQ(args[2]);
        } else {
            return ctx.reg_alloc.ReadReg<std::max<std::size_t>(bitsize, 32)>(args[2]);
        }
    }();
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());
    // Venture: the host FPSR stays live across the access; only the fallback folds it.
    const bool fold_fpsr = ctx.fpsr.IsLoaded();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Rvalue);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();

    std::optional<FoldedOperand> folded_op;
    if (folded) {
        folded_op = FoldedOperand{folded->imm, Xindex ? (*Xindex)->index() : -1, folded->shift};
    }
    const auto fastmem_location = [&] {
        if (folded_op) {
            return EmitFoldedStr<bitsize>(code, Rvalue->index(), *Xaddr, *folded_op);
        }
        const auto [Xbase, Xoffset] = FastmemEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback);
        return EmitMemoryStr<bitsize>(code, Rvalue->index(), Xbase, Xoffset, ordered, ShouldExt32(ctx));
    }();

    ctx.deferred_emits.emplace_back([&code, &ctx, inst, marker, Xaddr = *Xaddr, Rvalue = *Rvalue, ordered, fold_fpsr, fallback, end, fastmem_location, folded_op] {
        ctx.ebi.fastmem_patch_info.emplace(
            fastmem_location - ctx.ebi.entry_point,
            FastmemPatchInfo{
                .marker = marker,
                .fc = FakeCall{
                    .call_pc = std::bit_cast<u64>(code.xptr<void*>()),
                },
                .recompile = ctx.conf.recompile_on_fastmem_failure,
            });

        code.l(*fallback);
        if (fold_fpsr) {
            ctx.fpsr.EmitFold();
        }
        if (folded_op) {
            folded_op->EmitAddress(code, Xaddr, Xscratch0);
        } else {
            code.MOV(Xscratch0, Xaddr);
        }
        if constexpr (bitsize == 128) {
            code.MOV(Q0.B16(), Rvalue.B16());
        } else {
            code.MOV(Xscratch1, Rvalue.toX());
        }
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        EmitRelocation(code, ctx, WrappedWriteMemoryLinkTarget(bitsize));
        if (fold_fpsr) {
            ctx.fpsr.EmitClearHost();
        }
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
        code.B(*end);
    });

    code.l(*end);
}


// Venture: inline exclusives, as the x64 backend's EmitExclusive*MemoryInline,
// for UserConfig::fastmem_exclusive_access. Without Unsafe_IgnoreGlobalMonitor
// each access holds the monitor's lock, as x64's EmitExclusiveLock, and a
// store clears every CPU's reservation of its address (the callbacks' rules);
// with it the lock is never taken. An exclusive load records the address, the
// granule's generation (ExclusiveMonitor::EnableGenerations) and the loaded
// value in this CPU's monitor slot. An exclusive store checks the address,
// claims the generation unless its load is earlier in the block with no memory
// read between (ExclusivePairIsLocal), and compare-exchanges the recorded value
// with a host exclusive pair. A faulting access, or one in a block recompiled
// without fastmem, goes through the memory callbacks with the same records.
// 128-bit pairs (A64 LDXP/STXP) work the same way on exclusive pair loops.
bool InlineExclusives(EmitContext& ctx, std::size_t bitsize) {
    return bitsize <= 128 && ctx.conf.fastmem_exclusive_access && ctx.conf.fastmem_pointer && ctx.conf.global_monitor
        && ctx.fastmem.SupportsFastmem();
}

bool LockedExclusives(EmitContext& ctx) {
    return !ctx.conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor);
}

// Takes and releases the monitor's lock; both clobber Xscratch2, the lock also
// Xscratch0 and Xscratch1.
void EmitExclusiveMonitorLock(oaknut::CodeGenerator& code, EmitContext& ctx) {
    if (LockedExclusives(ctx)) {
        code.MOV(Xscratch2, std::bit_cast<u64>(GetExclusiveMonitorLockPointer(ctx.conf.global_monitor)));
        EmitSpinLockLock(code, Xscratch2);
    }
}

void EmitExclusiveMonitorUnlock(oaknut::CodeGenerator& code, EmitContext& ctx) {
    if (LockedExclusives(ctx)) {
        code.MOV(Xscratch2, std::bit_cast<u64>(GetExclusiveMonitorLockPointer(ctx.conf.global_monitor)));
        EmitSpinLockUnlock(code, Xscratch2);
    }
}

// The memory abort check of a fallback taken under the lock: an abort releases
// it before leaving; otherwise execution continues after the check.
void EmitExclusiveCheckMemoryAbort(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, oaknut::Label& end) {
    if (!LockedExclusives(ctx) || !ctx.conf.check_halt_on_memory_access) {
        ctx.conf.emit_check_memory_abort(code, ctx, inst, end);
        return;
    }
    oaknut::Label resume;
    code.LDAR(Xscratch0, Xhalt);
    code.TST(Xscratch0, static_cast<u32>(HaltReason::MemoryAbort));
    code.B(EQ, resume);
    EmitExclusiveMonitorUnlock(code, ctx);
    ctx.conf.emit_check_memory_abort(code, ctx, inst, resume);
    code.l(resume);
}

// Under the lock: every CPU's reservation of Xaddr ends (x64's
// EmitExclusiveTestAndClear). Clobbers Xtmp, Xscratch1 and Xscratch2.
void EmitExclusiveTestAndClear(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr, oaknut::XReg Xtmp) {
    if (!LockedExclusives(ctx)) {
        return;
    }
    constexpr std::size_t stride = 64;
    ASSERT(GetExclusiveMonitorProcessorStride() == stride);
    oaknut::Label loop, next, done;
    code.MOV(Xscratch1, std::bit_cast<u64>(GetExclusiveMonitorActiveCountPointer(ctx.conf.global_monitor)));
    code.LDR(Xscratch1, Xscratch1);
    code.MOV(Xtmp, std::bit_cast<u64>(GetExclusiveMonitorAddressBase(ctx.conf.global_monitor)));
    code.ADD(Xscratch1, Xtmp, Xscratch1, LSL, 6);
    code.l(loop);
    code.CMP(Xtmp, Xscratch1);
    code.B(EQ, done);
    code.LDR(Xscratch2, Xtmp);
    code.CMP(Xscratch2, Xaddr);
    code.B(NE, next);
    code.MOV(Xscratch2, u64(0xDEAD'DEAD'DEAD'DEAD));  // ExclusiveMonitor::INVALID_EXCLUSIVE_ADDRESS
    code.STR(Xscratch2, Xtmp);
    code.l(next);
    code.ADD(Xtmp, Xtmp, stride);
    code.B(loop);
    code.l(done);
}

// The store's exclusive load is earlier in this block and nothing between them
// reads memory (x64's ExclusivePairIsLocal).
bool ExclusivePairIsLocal(const IR::Block& block, const IR::Inst* store) {
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

// The store follows its exclusive load with no other memory access,
// clear-exclusive, supervisor call or exception between (x64's ExclusivePairIsAdjacent).
bool ExclusivePairIsAdjacent(const IR::Block& block, const IR::Inst* store) {
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

// Xout = &generation bucket of Xaddr's 64-byte granule, from bits 6-47 so a
// tagged pointer reserves the same granule (x64's EmitExclusiveGenerationBucket).
void EmitExclusiveGenerationBucket(oaknut::CodeGenerator& code, std::uint64_t* base, oaknut::XReg Xaddr, oaknut::XReg Xout, oaknut::XReg Xtmp) {
    static_assert((ExclusiveMonitor::GENERATION_BUCKETS & (ExclusiveMonitor::GENERATION_BUCKETS - 1)) == 0);
    code.UBFX(Xout, Xaddr, 6, 42);
    code.EOR(Xout, Xout, Xout, LSR, 12);
    code.AND(Xout, Xout, ExclusiveMonitor::GENERATION_BUCKETS - 1);
    code.MOV(Xtmp, std::bit_cast<u64>(base));
    code.ADD(Xout, Xtmp, Xout, LSL, 6);
}

struct ExclusiveSlot {
    u64 address;  // &this CPU's reserved address
    u32 generation_offset;
    u32 value_offset;
};

ExclusiveSlot GetExclusiveSlot(EmitContext& ctx) {
    const auto address = std::bit_cast<u64>(GetExclusiveMonitorAddressPointer(ctx.conf.global_monitor, ctx.conf.processor_id));
    const auto generation = std::bit_cast<u64>(GetExclusiveMonitorGenerationSlot(ctx.conf.global_monitor, ctx.conf.processor_id));
    const auto value = std::bit_cast<u64>(GetExclusiveMonitorValuePointer(ctx.conf.global_monitor, ctx.conf.processor_id));
    return {address, static_cast<u32>(generation - address), static_cast<u32>(value - address)};
}

// Zero-extending load of a recorded value.
template<std::size_t bitsize>
void EmitSlotLoad(oaknut::CodeGenerator& code, oaknut::XReg Xt, oaknut::XReg Xbase, u32 offset) {
    if constexpr (bitsize == 8) {
        code.LDRB(Xt.toW(), Xbase, offset);
    } else if constexpr (bitsize == 16) {
        code.LDRH(Xt.toW(), Xbase, offset);
    } else if constexpr (bitsize == 32) {
        code.LDR(Xt.toW(), Xbase, offset);
    } else {
        code.LDR(Xt, Xbase, offset);
    }
}

template<std::size_t bitsize>
void EmitSlotStore(oaknut::CodeGenerator& code, oaknut::XReg Xt, oaknut::XReg Xbase, u32 offset) {
    if constexpr (bitsize == 8) {
        code.STRB(Xt.toW(), Xbase, offset);
    } else if constexpr (bitsize == 16) {
        code.STRH(Xt.toW(), Xbase, offset);
    } else if constexpr (bitsize == 32) {
        code.STR(Xt.toW(), Xbase, offset);
    } else {
        code.STR(Xt, Xbase, offset);
    }
}

LinkTarget InlineExclusiveWriteFallbackLinkTarget(std::size_t bitsize) {
    switch (bitsize) {
    case 8:
        return LinkTarget::InlineExclusiveWriteFallback8;
    case 16:
        return LinkTarget::InlineExclusiveWriteFallback16;
    case 32:
        return LinkTarget::InlineExclusiveWriteFallback32;
    case 64:
        return LinkTarget::InlineExclusiveWriteFallback64;
    }
    UNREACHABLE();
}

template<std::size_t bitsize>
void InlineEmitExclusiveReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = ctx.reg_alloc.WriteReg<std::max<std::size_t>(bitsize, 32)>(inst);
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());
    ctx.fpsr.Spill();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Rvalue);

    EmitExclusiveMonitorLock(code, ctx);
    const auto slot = GetExclusiveSlot(ctx);
    code.MOV(Wscratch1, 1);
    code.STRB(Wscratch1, Xstate, ctx.conf.state_exclusive_state_offset);
    code.MOV(Xscratch0, slot.address);
    code.STR(Xaddr, Xscratch0);
    if (const auto generations = GetExclusiveMonitorGenerationBase(ctx.conf.global_monitor)) {
        // Record the granule's generation before loading its value: the acquire
        // keeps the value's load after it.
        EmitExclusiveGenerationBucket(code, generations, Xaddr, Xscratch2, Xscratch1);
        code.LDAR(Xscratch1, Xscratch2);
        code.STR(Xscratch1, Xscratch0, slot.generation_offset);
    }

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();
    if (const auto marker = ShouldFastmem(ctx, inst)) {
        const auto [Xbase, Xoffset] = FastmemEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback);
        const auto fastmem_location = EmitMemoryLdr<bitsize>(code, Rvalue->index(), Xbase, Xoffset, ordered, ShouldExt32(ctx));

        ctx.deferred_emits.emplace_back([&code, &ctx, inst, marker = *marker, Xaddr = *Xaddr, Rvalue = *Rvalue, ordered, fallback, end, fastmem_location] {
            ctx.ebi.fastmem_patch_info.emplace(
                fastmem_location - ctx.ebi.entry_point,
                FastmemPatchInfo{
                    .marker = marker,
                    .fc = FakeCall{
                        .call_pc = std::bit_cast<u64>(code.xptr<void*>()),
                    },
                    .recompile = ctx.conf.recompile_on_exclusive_fastmem_failure,
                });

            code.l(*fallback);
            code.MOV(Xscratch0, Xaddr);
            EmitRelocation(code, ctx, WrappedReadMemoryLinkTarget(bitsize));
            if (ordered) {
                code.DMB(oaknut::BarrierOp::ISH);
            }
            code.MOV(Rvalue.toX(), Xscratch0);
            EmitExclusiveCheckMemoryAbort(code, ctx, inst, *end);
            code.B(*end);
        });
    } else {
        code.MOV(Xscratch0, Xaddr);
        EmitRelocation(code, ctx, WrappedReadMemoryLinkTarget(bitsize));
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        code.MOV(Rvalue->toX(), Xscratch0);
        EmitExclusiveCheckMemoryAbort(code, ctx, inst, *end);
    }
    code.l(*end);

    code.MOV(Xscratch0, slot.address);
    EmitSlotStore<bitsize>(code, Rvalue->toX(), Xscratch0, slot.value_offset);
    EmitExclusiveMonitorUnlock(code, ctx);
}

template<std::size_t bitsize>
void InlineEmitExclusiveWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Rvalue = ctx.reg_alloc.ReadReg<std::max<std::size_t>(bitsize, 32)>(args[2]);
    auto Wstatus = ctx.reg_alloc.WriteW(inst);
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());
    ctx.fpsr.Spill();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Rvalue, Wstatus);

    const auto slot = GetExclusiveSlot(ctx);
    const bool elide = ctx.conf.elide_unchanged_exclusive_stores && !ordered && ExclusivePairIsAdjacent(ctx.block, inst);
    const auto Xvalue = Rvalue->toX();
    const auto Xstatus = Wstatus->toX();
    SharedLabel end = GenSharedLabel();

    EmitExclusiveMonitorLock(code, ctx);
    code.MOV(Wstatus, 1);
    code.LDRB(Wscratch0, Xstate, ctx.conf.state_exclusive_state_offset);
    code.CBZ(Wscratch0, *end);
    code.MOV(Xscratch0, slot.address);
    code.LDR(Xscratch1, Xscratch0);
    code.CMP(Xscratch1, Xaddr);
    code.B(NE, *end);

    // A plain store-exclusive of the value its load returned, with only
    // register operations since, succeeds without writing: the pair
    // linearizes at the single-copy atomic load (x64's elision).
    if (elide) {
        oaknut::Label store;
        EmitSlotLoad<bitsize>(code, Xscratch1, Xscratch0, slot.value_offset);
        if constexpr (bitsize == 8) {
            code.CMP(Wscratch1, Xvalue.toW(), oaknut::AddSubExt::UXTB);
        } else if constexpr (bitsize == 16) {
            code.CMP(Wscratch1, Xvalue.toW(), oaknut::AddSubExt::UXTH);
        } else if constexpr (bitsize == 32) {
            code.CMP(Wscratch1, Xvalue.toW());
        } else {
            code.CMP(Xscratch1, Xvalue);
        }
        code.B(NE, store);
        code.STRB(WZR, Xstate, ctx.conf.state_exclusive_state_offset);
        code.MOV(Wstatus, WZR);
        code.B(*end);
        code.l(store);
    }

    if (const auto generations = GetExclusiveMonitorGenerationBase(ctx.conf.global_monitor); generations && !ExclusivePairIsLocal(ctx.block, inst)) {
        // Claim the granule: advance its generation from the value the load
        // recorded, or fail because another exclusive store succeeded since.
        oaknut::Label retry, claimed, failed;
        EmitExclusiveGenerationBucket(code, generations, Xaddr, Xscratch2, Xscratch1);
        code.LDR(Xscratch1, Xscratch0, slot.generation_offset);
        code.l(retry);
        code.LDAXR(Xstatus, Xscratch2);
        code.CMP(Xstatus, Xscratch1);
        code.B(NE, failed);
        code.ADD(Xscratch1, Xscratch1, 2);
        code.STLXR(Wstatus, Xscratch1, Xscratch2);
        code.SUB(Xscratch1, Xscratch1, 2);
        code.CBNZ(Wstatus, retry);
        code.B(claimed);
        code.l(failed);
        code.CLREX();
        code.MOV(Wstatus, 1);
        code.B(*end);
        code.l(claimed);
    }

    EmitExclusiveTestAndClear(code, ctx, *Xaddr, Xstatus);
    code.STRB(WZR, Xstate, ctx.conf.state_exclusive_state_offset);

    const auto emit_fallback_call = [&code, &ctx, inst, Xaddr = *Xaddr, Xvalue, Wstatus = *Wstatus, ordered, end] {
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        code.MOV(Xscratch0, Xaddr);
        code.MOV(Xscratch1, Xvalue);
        EmitRelocation(code, ctx, InlineExclusiveWriteFallbackLinkTarget(bitsize));
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        code.MOV(Wstatus, Wscratch0);
        EmitExclusiveCheckMemoryAbort(code, ctx, inst, *end);
    };

    if (const auto marker = ShouldFastmem(ctx, inst)) {
        // Compare-exchange the recorded value: Xscratch1 = expected, Xscratch2 = host address.
        SharedLabel fallback = GenSharedLabel();
        oaknut::Label retry, failed;
        EmitSlotLoad<bitsize>(code, Xscratch1, Xscratch0, slot.value_offset);
        const auto [Xbase, Xoffset] = FastmemEmitVAddrLookup<bitsize>(code, ctx, Xaddr, fallback);
        if (ShouldExt32(ctx)) {
            code.ADD(Xscratch2, Xbase, oaknut::RReg{Xoffset.toW()}, oaknut::AddSubExt::UXTW);
        } else {
            code.ADD(Xscratch2, Xbase, Xoffset);
        }
        code.l(retry);
        const auto load_location = code.xptr<CodePtr>();
        if constexpr (bitsize == 8) {
            code.LDXRB(Wscratch0, Xscratch2);
        } else if constexpr (bitsize == 16) {
            code.LDXRH(Wscratch0, Xscratch2);
        } else if constexpr (bitsize == 32) {
            code.LDXR(Wscratch0, Xscratch2);
        } else {
            code.LDXR(Xscratch0, Xscratch2);
        }
        if constexpr (bitsize == 64) {
            code.CMP(Xscratch0, Xscratch1);
        } else {
            code.CMP(Wscratch0, Wscratch1);
        }
        code.B(NE, failed);
        const auto store_location = code.xptr<CodePtr>();
        if constexpr (bitsize == 8) {
            ordered ? code.STLXRB(Wstatus, Xvalue.toW(), Xscratch2) : code.STXRB(Wstatus, Xvalue.toW(), Xscratch2);
        } else if constexpr (bitsize == 16) {
            ordered ? code.STLXRH(Wstatus, Xvalue.toW(), Xscratch2) : code.STXRH(Wstatus, Xvalue.toW(), Xscratch2);
        } else if constexpr (bitsize == 32) {
            ordered ? code.STLXR(Wstatus, Xvalue.toW(), Xscratch2) : code.STXR(Wstatus, Xvalue.toW(), Xscratch2);
        } else {
            ordered ? code.STLXR(Wstatus, Xvalue, Xscratch2) : code.STXR(Wstatus, Xvalue, Xscratch2);
        }
        code.CBNZ(Wstatus, retry);
        code.B(*end);
        code.l(failed);
        code.CLREX();
        code.MOV(Wstatus, 1);

        ctx.deferred_emits.emplace_back([&code, &ctx, marker = *marker, fallback, end, load_location, store_location, emit_fallback_call] {
            const auto call_pc = std::bit_cast<u64>(code.xptr<void*>());
            for (const auto location : {load_location, store_location}) {
                ctx.ebi.fastmem_patch_info.emplace(
                    location - ctx.ebi.entry_point,
                    FastmemPatchInfo{
                        .marker = marker,
                        .fc = FakeCall{
                            .call_pc = call_pc,
                        },
                        .recompile = ctx.conf.recompile_on_exclusive_fastmem_failure,
                    });
            }
            code.l(*fallback);
            emit_fallback_call();
            code.B(*end);
        });
    } else {
        emit_fallback_call();
    }

    code.l(*end);
    EmitExclusiveMonitorUnlock(code, ctx);
}

// Venture: a free general register for a 128-bit exclusive pair loop (its store
// status), saved in D0: V0-V7 are never allocated. Not `avoid`.
oaknut::XReg PairTemp(int avoid) {
    return oaknut::XReg{avoid == 0 ? 1 : 0};
}

// Record the reservation as InlineEmitExclusiveReadMemory does.
void EmitRecordExclusive(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr, const ExclusiveSlot& slot) {
    code.MOV(Wscratch1, 1);
    code.STRB(Wscratch1, Xstate, ctx.conf.state_exclusive_state_offset);
    code.MOV(Xscratch0, slot.address);
    code.STR(Xaddr, Xscratch0);
    if (const auto generations = GetExclusiveMonitorGenerationBase(ctx.conf.global_monitor)) {
        EmitExclusiveGenerationBucket(code, generations, Xaddr, Xscratch2, Xscratch1);
        code.LDAR(Xscratch1, Xscratch2);
        code.STR(Xscratch1, Xscratch0, slot.generation_offset);
    }
}

// Xscratch2 = host address of a 16-byte aligned pair, else to `fallback`.
void EmitPairAddress(oaknut::CodeGenerator& code, EmitContext& ctx, oaknut::XReg Xaddr, const SharedLabel& fallback) {
    code.TST(Xaddr, 15);
    code.B(NE, *fallback);
    const auto [Xbase, Xoffset] = FastmemEmitVAddrLookup<128>(code, ctx, Xaddr, fallback);
    if (ShouldExt32(ctx)) {
        code.ADD(Xscratch2, Xbase, oaknut::RReg{Xoffset.toW()}, oaknut::AddSubExt::UXTW);
    } else {
        code.ADD(Xscratch2, Xbase, Xoffset);
    }
}

// LDXP alone is not single-copy atomic: the pair is read by an exclusive pair
// loop that stores back what it loaded (x64: VMOVDQA or LOCK CMPXCHG16B).
void InlineEmitExclusiveReadMemory128(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Qvalue = ctx.reg_alloc.WriteQ(inst);
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());
    ctx.fpsr.Spill();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Qvalue);

    EmitExclusiveMonitorLock(code, ctx);
    const auto slot = GetExclusiveSlot(ctx);
    EmitRecordExclusive(code, ctx, *Xaddr, slot);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();
    const auto emit_fallback_call = [&code, &ctx, inst, Xaddr = *Xaddr, Qvalue = *Qvalue, ordered, end] {
        code.MOV(Xscratch0, Xaddr);
        EmitRelocation(code, ctx, LinkTarget::WrappedReadMemory128);
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        code.MOV(Qvalue.B16(), Q0.B16());
        EmitExclusiveCheckMemoryAbort(code, ctx, inst, *end);
    };
    if (const auto marker = ShouldFastmem(ctx, inst)) {
        const oaknut::XReg Xtemp = PairTemp(Xaddr->index());
        EmitPairAddress(code, ctx, *Xaddr, fallback);
        code.FMOV(D0, Xtemp);
        oaknut::Label retry;
        code.l(retry);
        const auto load_location = code.xptr<CodePtr>();
        ordered ? code.LDAXP(Xscratch0, Xscratch1, Xscratch2) : code.LDXP(Xscratch0, Xscratch1, Xscratch2);
        const auto store_location = code.xptr<CodePtr>();
        code.STXP(Xtemp.toW(), Xscratch0, Xscratch1, Xscratch2);
        code.CBNZ(Xtemp.toW(), retry);
        code.FMOV(Xtemp, D0);
        code.FMOV(Qvalue->toD(), Xscratch0);
        code.FMOV(oaknut::VRegSelector{Qvalue->index()}.D()[1], Xscratch1);

        ctx.deferred_emits.emplace_back([&code, &ctx, marker = *marker, fallback, end, load_location, store_location, Xtemp, emit_fallback_call] {
            const auto call_pc = std::bit_cast<u64>(code.xptr<void*>());
            for (const auto location : {load_location, store_location}) {
                ctx.ebi.fastmem_patch_info.emplace(
                    location - ctx.ebi.entry_point,
                    FastmemPatchInfo{
                        .marker = marker,
                        .fc = FakeCall{
                            .call_pc = call_pc,
                        },
                        .recompile = ctx.conf.recompile_on_exclusive_fastmem_failure,
                    });
            }
            // A fault in the loop arrives with the temporary in use.
            code.FMOV(Xtemp, D0);
            code.l(*fallback);
            emit_fallback_call();
            code.B(*end);
        });
    } else {
        emit_fallback_call();
    }
    code.l(*end);

    code.MOV(Xscratch0, slot.address + slot.value_offset);
    code.STR(*Qvalue, Xscratch0);
    EmitExclusiveMonitorUnlock(code, ctx);
}

void InlineEmitExclusiveWriteMemory128(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Qvalue = ctx.reg_alloc.ReadQ(args[2]);
    auto Wstatus = ctx.reg_alloc.WriteW(inst);
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());
    ctx.fpsr.Spill();
    ctx.reg_alloc.SpillFlags();
    RegAlloc::Realize(Xaddr, Qvalue, Wstatus);

    const auto slot = GetExclusiveSlot(ctx);
    const bool elide = ctx.conf.elide_unchanged_exclusive_stores && !ordered && ExclusivePairIsAdjacent(ctx.block, inst);
    const auto Xstatus = Wstatus->toX();
    const auto Vhigh = oaknut::VRegSelector{Qvalue->index()}.D()[1];
    SharedLabel end = GenSharedLabel();

    EmitExclusiveMonitorLock(code, ctx);
    code.MOV(Wstatus, 1);
    code.LDRB(Wscratch0, Xstate, ctx.conf.state_exclusive_state_offset);
    code.CBZ(Wscratch0, *end);
    code.MOV(Xscratch0, slot.address);
    code.LDR(Xscratch1, Xscratch0);
    code.CMP(Xscratch1, *Xaddr);
    code.B(NE, *end);

    // Xscratch0/1 = value, Z set when it equals the recorded value.
    const auto compare_recorded = [&](oaknut::XReg Xlo, oaknut::XReg Xhi) {
        code.MOV(Xstatus, slot.address + slot.value_offset);
        code.LDR(Xstatus, Xstatus);
        code.CMP(Xlo, Xstatus);
        code.MOV(Xstatus, slot.address + slot.value_offset + 8);
        code.LDR(Xstatus, Xstatus);
        code.CCMP(Xhi, Xstatus, 0, EQ);
    };

    if (elide) {
        oaknut::Label store;
        code.FMOV(Xscratch0, Qvalue->toD());
        code.FMOV(Xscratch1, Vhigh);
        compare_recorded(Xscratch0, Xscratch1);
        code.B(NE, store);
        code.STRB(WZR, Xstate, ctx.conf.state_exclusive_state_offset);
        code.MOV(Wstatus, WZR);
        code.B(*end);
        code.l(store);
        code.MOV(Xscratch0, slot.address);
    }

    if (const auto generations = GetExclusiveMonitorGenerationBase(ctx.conf.global_monitor); generations && !ExclusivePairIsLocal(ctx.block, inst)) {
        oaknut::Label retry, claimed, failed;
        EmitExclusiveGenerationBucket(code, generations, *Xaddr, Xscratch2, Xscratch1);
        code.LDR(Xscratch1, Xscratch0, slot.generation_offset);
        code.l(retry);
        code.LDAXR(Xstatus, Xscratch2);
        code.CMP(Xstatus, Xscratch1);
        code.B(NE, failed);
        code.ADD(Xscratch1, Xscratch1, 2);
        code.STLXR(Wstatus, Xscratch1, Xscratch2);
        code.SUB(Xscratch1, Xscratch1, 2);
        code.CBNZ(Wstatus, retry);
        code.B(claimed);
        code.l(failed);
        code.CLREX();
        code.MOV(Wstatus, 1);
        code.B(*end);
        code.l(claimed);
    }

    EmitExclusiveTestAndClear(code, ctx, *Xaddr, Xstatus);
    code.STRB(WZR, Xstate, ctx.conf.state_exclusive_state_offset);

    const auto emit_fallback_call = [&code, &ctx, inst, Xaddr = *Xaddr, Qvalue = *Qvalue, Wstatus = *Wstatus, ordered, end] {
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        code.MOV(Xscratch0, Xaddr);
        code.MOV(Q0.B16(), Qvalue.B16());
        EmitRelocation(code, ctx, LinkTarget::InlineExclusiveWriteFallback128);
        if (ordered) {
            code.DMB(oaknut::BarrierOp::ISH);
        }
        code.MOV(Wstatus, Wscratch0);
        EmitExclusiveCheckMemoryAbort(code, ctx, inst, *end);
    };

    if (const auto marker = ShouldFastmem(ctx, inst)) {
        // Compare-exchange the recorded pair.
        SharedLabel fallback = GenSharedLabel();
        oaknut::Label retry, failed;
        EmitPairAddress(code, ctx, *Xaddr, fallback);
        code.l(retry);
        const auto load_location = code.xptr<CodePtr>();
        code.LDXP(Xscratch0, Xscratch1, Xscratch2);
        compare_recorded(Xscratch0, Xscratch1);
        code.B(NE, failed);
        code.FMOV(Xscratch0, Qvalue->toD());
        code.FMOV(Xscratch1, Vhigh);
        const auto store_location = code.xptr<CodePtr>();
        ordered ? code.STLXP(Wstatus, Xscratch0, Xscratch1, Xscratch2) : code.STXP(Wstatus, Xscratch0, Xscratch1, Xscratch2);
        code.CBNZ(Wstatus, retry);
        code.B(*end);
        code.l(failed);
        code.CLREX();
        code.MOV(Wstatus, 1);

        ctx.deferred_emits.emplace_back([&code, &ctx, marker = *marker, fallback, end, load_location, store_location, emit_fallback_call] {
            const auto call_pc = std::bit_cast<u64>(code.xptr<void*>());
            for (const auto location : {load_location, store_location}) {
                ctx.ebi.fastmem_patch_info.emplace(
                    location - ctx.ebi.entry_point,
                    FastmemPatchInfo{
                        .marker = marker,
                        .fc = FakeCall{
                            .call_pc = call_pc,
                        },
                        .recompile = ctx.conf.recompile_on_exclusive_fastmem_failure,
                    });
            }
            code.l(*fallback);
            emit_fallback_call();
            code.B(*end);
        });
    } else {
        emit_fallback_call();
    }

    code.l(*end);
    EmitExclusiveMonitorUnlock(code, ctx);
}

}  // namespace

// Venture: an LSE atomic as an acquire-release exclusive pair loop on guest
// memory (base ARMv8, as a compiler lowers a seq_cst read-modify-write without
// LSE): Wscratch0 = old, Wscratch1 = new, Xscratch2 = host address, and the
// result register takes the store status. A fault at either access, a
// misaligned address (LSE requires alignment) or an address outside fastmem
// goes through UserCallbacks::MemoryAtomic. Clears this CPU's exclusive state,
// as an exclusive store does.
template<std::size_t bitsize, bool compare_and_swap>
static void EmitInlineAtomicFallback(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, DoNotFastmemMarker marker, A64::AtomicOp op,
                                     oaknut::XReg addr, oaknut::XReg value, oaknut::XReg desired, oaknut::XReg result,
                                     SharedLabel fallback, SharedLabel end, CodePtr load_location, CodePtr store_location);

template<std::size_t bitsize, bool compare_and_swap>
bool EmitInlineAtomicMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    using Op = A64::AtomicOp;
    static_assert(bitsize <= 64);
    if (!ctx.conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor)) {
        return false;
    }
    const auto marker = ShouldFastmem(ctx, inst);
    if (!marker) {
        return false;
    }

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Op op = compare_and_swap ? Op::CompareAndSwap : static_cast<Op>(args[3].GetImmediateU8());
    auto Xaddr = ctx.reg_alloc.ReadX(args[1]);
    auto Xvalue = ctx.reg_alloc.ReadX(args[2]);
    auto Xresult = ctx.reg_alloc.WriteX(inst);
    std::optional<RAReg<oaknut::XReg>> Xdesired;
    if constexpr (compare_and_swap) {
        Xdesired.emplace(ctx.reg_alloc.ReadX(args[3]));
    }
    ctx.fpsr.Spill();
    ctx.reg_alloc.SpillFlags();
    if constexpr (compare_and_swap) {
        RegAlloc::Realize(Xaddr, Xvalue, Xresult, *Xdesired);
    } else {
        RegAlloc::Realize(Xaddr, Xvalue, Xresult);
    }
    const oaknut::XReg value = *Xvalue, addr = *Xaddr, result = *Xresult;
    const oaknut::XReg desired = compare_and_swap ? **Xdesired : value;  // value: unused

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();
    if constexpr (bitsize > 8) {
        code.TST(addr, bitsize / 8 - 1);
        code.B(NE, *fallback);
    }
    const auto [Xbase, Xoffset] = FastmemEmitVAddrLookup<bitsize>(code, ctx, addr, fallback);
    if (ShouldExt32(ctx)) {
        code.ADD(Xscratch2, Xbase, oaknut::RReg{Xoffset.toW()}, oaknut::AddSubExt::UXTW);
    } else {
        code.ADD(Xscratch2, Xbase, Xoffset);
    }
    code.STRB(WZR, Xstate, ctx.conf.state_exclusive_state_offset);

    const auto ext = bitsize == 8 ? oaknut::AddSubExt::UXTB : oaknut::AddSubExt::UXTH;
    const auto sext = bitsize == 8 ? oaknut::AddSubExt::SXTB : oaknut::AddSubExt::SXTH;
    const auto compare = [&](bool is_signed) {
        if constexpr (bitsize == 64) {
            code.CMP(Xscratch0, value);
        } else if constexpr (bitsize == 32) {
            code.CMP(Wscratch0, value.toW());
        } else if (is_signed) {
            if constexpr (bitsize == 8) {
                code.SXTB(Wscratch1, Wscratch0);
            } else {
                code.SXTH(Wscratch1, Wscratch0);
            }
            code.CMP(Wscratch1, value.toW(), sext);
        } else {
            code.CMP(Wscratch0, value.toW(), ext);
        }
    };
    const auto select = [&](oaknut::Cond keep_old) {
        if constexpr (bitsize == 64) {
            code.CSEL(Xscratch1, Xscratch0, value, keep_old);
        } else {
            code.CSEL(Wscratch1, Wscratch0, value.toW(), keep_old);
        }
    };

    if (ctx.conf.HasHostFeature(HostFeature::LSE)) {
        // Venture: with FEAT_LSE the guest's atomic is the same host instruction
        // (acquire-release, as the loop), with the old value in Wscratch0.
        const auto location = code.xptr<CodePtr>();
#define VENTURE_LSE(NAME)                                       \
    if constexpr (bitsize == 8) {                               \
        code.NAME##B(value.toW(), Wscratch0, Xscratch2);        \
    } else if constexpr (bitsize == 16) {                       \
        code.NAME##H(value.toW(), Wscratch0, Xscratch2);        \
    } else if constexpr (bitsize == 32) {                       \
        code.NAME(value.toW(), Wscratch0, Xscratch2);           \
    } else {                                                    \
        code.NAME(value, Xscratch0, Xscratch2);                 \
    }
        switch (op) {
        case Op::Add:
            VENTURE_LSE(LDADDAL);
            break;
        case Op::Clear:
            VENTURE_LSE(LDCLRAL);
            break;
        case Op::Eor:
            VENTURE_LSE(LDEORAL);
            break;
        case Op::Set:
            VENTURE_LSE(LDSETAL);
            break;
        case Op::SMax:
            VENTURE_LSE(LDSMAXAL);
            break;
        case Op::SMin:
            VENTURE_LSE(LDSMINAL);
            break;
        case Op::UMax:
            VENTURE_LSE(LDUMAXAL);
            break;
        case Op::UMin:
            VENTURE_LSE(LDUMINAL);
            break;
        case Op::Swap:
            VENTURE_LSE(SWPAL);
            break;
        case Op::CompareAndSwap:
            // CASAL compares and returns in its first register.
            code.MOV(Xscratch0, value);
            if constexpr (bitsize == 8) {
                code.CASALB(Wscratch0, desired.toW(), Xscratch2);
            } else if constexpr (bitsize == 16) {
                code.CASALH(Wscratch0, desired.toW(), Xscratch2);
            } else if constexpr (bitsize == 32) {
                code.CASAL(Wscratch0, desired.toW(), Xscratch2);
            } else {
                code.CASAL(Xscratch0, desired, Xscratch2);
            }
            break;
        default:
            UNREACHABLE();
        }
#undef VENTURE_LSE
        code.MOV(result, Xscratch0);
        code.l(*end);
        EmitInlineAtomicFallback<bitsize, compare_and_swap>(code, ctx, inst, *marker, op, addr, value, desired, result, fallback, end, location, location);
        return true;
    }

    oaknut::Label retry, done;
    code.l(retry);
    const auto load_location = code.xptr<CodePtr>();
    if constexpr (bitsize == 8) {
        code.LDAXRB(Wscratch0, Xscratch2);
    } else if constexpr (bitsize == 16) {
        code.LDAXRH(Wscratch0, Xscratch2);
    } else if constexpr (bitsize == 32) {
        code.LDAXR(Wscratch0, Xscratch2);
    } else {
        code.LDAXR(Xscratch0, Xscratch2);
    }
    oaknut::XReg store = Xscratch1;
    switch (op) {
    case Op::Add:
        code.ADD(Xscratch1, Xscratch0, value);
        break;
    case Op::Clear:
        code.BIC(Xscratch1, Xscratch0, value);
        break;
    case Op::Eor:
        code.EOR(Xscratch1, Xscratch0, value);
        break;
    case Op::Set:
        code.ORR(Xscratch1, Xscratch0, value);
        break;
    case Op::SMax:
        compare(true);
        select(GT);
        break;
    case Op::SMin:
        compare(true);
        select(LT);
        break;
    case Op::UMax:
        compare(false);
        select(HI);
        break;
    case Op::UMin:
        compare(false);
        select(LO);
        break;
    case Op::Swap:
        store = value;
        break;
    case Op::CompareAndSwap: {
        oaknut::Label match;
        compare(false);
        code.B(EQ, match);
        code.CLREX();
        code.B(done);
        code.l(match);
        store = desired;
        break;
    }
    default:
        UNREACHABLE();
    }
    const auto store_location = code.xptr<CodePtr>();
    if constexpr (bitsize == 8) {
        code.STLXRB(result.toW(), store.toW(), Xscratch2);
    } else if constexpr (bitsize == 16) {
        code.STLXRH(result.toW(), store.toW(), Xscratch2);
    } else if constexpr (bitsize == 32) {
        code.STLXR(result.toW(), store.toW(), Xscratch2);
    } else {
        code.STLXR(result.toW(), store, Xscratch2);
    }
    code.CBNZ(result.toW(), retry);
    code.l(done);
    code.MOV(result, Xscratch0);
    code.l(*end);

    EmitInlineAtomicFallback<bitsize, compare_and_swap>(code, ctx, inst, *marker, op, addr, value, desired, result, fallback, end, load_location, store_location);
    return true;
}

// The inline atomic's fault records and its MemoryAtomic call.
template<std::size_t bitsize, bool compare_and_swap>
static void EmitInlineAtomicFallback(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, DoNotFastmemMarker marker, A64::AtomicOp op,
                                     oaknut::XReg addr, oaknut::XReg value, oaknut::XReg desired, oaknut::XReg result,
                                     SharedLabel fallback, SharedLabel end, CodePtr load_location, CodePtr store_location) {
    ctx.deferred_emits.emplace_back([&code, &ctx, inst, marker, op, addr, value, desired, result, fallback, end, load_location, store_location] {
        const auto call_pc = std::bit_cast<u64>(code.xptr<void*>());
        for (const auto location : {load_location, store_location}) {
            ctx.ebi.fastmem_patch_info.emplace(
                location - ctx.ebi.entry_point,
                FastmemPatchInfo{
                    .marker = marker,
                    .fc = FakeCall{
                        .call_pc = call_pc,
                    },
                    .recompile = ctx.conf.recompile_on_fastmem_failure,
                });
        }
        code.l(*fallback);
        code.SUB(SP, SP, 32);
        code.STR(value, SP);
        if constexpr (compare_and_swap) {
            code.STR(desired, SP, 8);
        }
        code.MOV(Xscratch1, static_cast<u64>(op) | (bitsize / 8) << 8);
        code.STR(Xscratch1, SP, 16);
        code.MOV(Xscratch1, SP);
        code.MOV(Xscratch0, addr);
        EmitRelocation(code, ctx, LinkTarget::WrappedAtomicMemory);
        code.ADD(SP, SP, 32);
        code.MOV(result, Xscratch0);
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
        code.B(*end);
    });
}

bool EmitInlineCompareAndSwap128(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    if (!ctx.conf.HasHostFeature(HostFeature::LSE) || !ctx.conf.HasOptimization(OptimizationFlag::Unsafe_IgnoreGlobalMonitor)) {
        return false;
    }
    const auto marker = ShouldFastmem(ctx, inst);
    if (!marker) {
        return false;
    }

    // The callback's registers: X1 address, Q0 expected, Q1 desired. CASPAL
    // takes even-numbered pairs; X4..X7 are free after PrepareForCall. A fault
    // or misalignment (CASP requires 16-byte alignment) calls the callback
    // with those registers unchanged.
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[1], args[2], args[3]);

    SharedLabel fallback = GenSharedLabel(), end = GenSharedLabel();
    code.TST(X1, 15);
    code.B(NE, *fallback);
    const auto [Xbase, Xoffset] = FastmemEmitVAddrLookup<128>(code, ctx, X1, fallback);
    if (ShouldExt32(ctx)) {
        code.ADD(Xscratch2, Xbase, oaknut::RReg{Xoffset.toW()}, oaknut::AddSubExt::UXTW);
    } else {
        code.ADD(Xscratch2, Xbase, Xoffset);
    }
    code.STRB(WZR, Xstate, ctx.conf.state_exclusive_state_offset);
    code.FMOV(X4, D0);
    code.FMOV(X5, V0.D()[1]);
    code.FMOV(X6, D1);
    code.FMOV(X7, V1.D()[1]);
    const auto location = code.xptr<CodePtr>();
    code.CASPAL(X4, X5, X6, X7, Xscratch2);
    code.FMOV(D8, X4);
    code.FMOV(V8.D()[1], X5);
    code.l(*end);

    ctx.deferred_emits.emplace_back([&code, &ctx, inst, marker = *marker, fallback, end, location] {
        ctx.ebi.fastmem_patch_info.emplace(
            location - ctx.ebi.entry_point,
            FastmemPatchInfo{
                .marker = marker,
                .fc = FakeCall{
                    .call_pc = std::bit_cast<u64>(code.xptr<void*>()),
                },
                .recompile = ctx.conf.recompile_on_fastmem_failure,
            });
        code.l(*fallback);
        code.STRB(WZR, Xstate, ctx.conf.state_exclusive_state_offset);
        EmitRelocation(code, ctx, LinkTarget::AtomicCompareAndSwap128);
        code.MOV(Q8.B16(), Q0.B16());
        ctx.conf.emit_check_memory_abort(code, ctx, inst, *end);
        code.B(*end);
    });
    ctx.reg_alloc.DefineAsRegister(inst, Q8);
    return true;
}

template bool EmitInlineAtomicMemory<8, false>(oaknut::CodeGenerator&, EmitContext&, IR::Inst*);
template bool EmitInlineAtomicMemory<16, false>(oaknut::CodeGenerator&, EmitContext&, IR::Inst*);
template bool EmitInlineAtomicMemory<32, false>(oaknut::CodeGenerator&, EmitContext&, IR::Inst*);
template bool EmitInlineAtomicMemory<64, false>(oaknut::CodeGenerator&, EmitContext&, IR::Inst*);
template bool EmitInlineAtomicMemory<8, true>(oaknut::CodeGenerator&, EmitContext&, IR::Inst*);
template bool EmitInlineAtomicMemory<16, true>(oaknut::CodeGenerator&, EmitContext&, IR::Inst*);
template bool EmitInlineAtomicMemory<32, true>(oaknut::CodeGenerator&, EmitContext&, IR::Inst*);
template bool EmitInlineAtomicMemory<64, true>(oaknut::CodeGenerator&, EmitContext&, IR::Inst*);

template<std::size_t bitsize>
void EmitReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    if (const auto marker = ShouldFastmem(ctx, inst)) {
        FastmemEmitReadMemory<bitsize>(code, ctx, inst, *marker);
    } else if (ctx.conf.page_table_pointer != 0) {
        InlinePageTableEmitReadMemory<bitsize>(code, ctx, inst);
    } else {
        CallbackOnlyEmitReadMemory<bitsize>(code, ctx, inst);
    }
}

template<std::size_t bitsize>
void EmitExclusiveReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    if (InlineExclusives(ctx, bitsize)) {
        if constexpr (bitsize <= 64) {
            InlineEmitExclusiveReadMemory<bitsize>(code, ctx, inst);
        } else {
            InlineEmitExclusiveReadMemory128(code, ctx, inst);
        }
        return;
    }
    CallbackOnlyEmitExclusiveReadMemory<bitsize>(code, ctx, inst);
}

template<std::size_t bitsize>
void EmitWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    if (const auto marker = ShouldFastmem(ctx, inst)) {
        FastmemEmitWriteMemory<bitsize>(code, ctx, inst, *marker);
    } else if (ctx.conf.page_table_pointer != 0) {
        InlinePageTableEmitWriteMemory<bitsize>(code, ctx, inst);
    } else {
        CallbackOnlyEmitWriteMemory<bitsize>(code, ctx, inst);
    }
}

template<std::size_t bitsize>
void EmitExclusiveWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    if (InlineExclusives(ctx, bitsize)) {
        if constexpr (bitsize <= 64) {
            InlineEmitExclusiveWriteMemory<bitsize>(code, ctx, inst);
        } else {
            InlineEmitExclusiveWriteMemory128(code, ctx, inst);
        }
        return;
    }
    CallbackOnlyEmitExclusiveWriteMemory<bitsize>(code, ctx, inst);
}

template void EmitReadMemory<8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitReadMemory<16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitReadMemory<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitReadMemory<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitReadMemory<128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveReadMemory<128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitWriteMemory<128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template void EmitExclusiveWriteMemory<128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);

}  // namespace Dynarmic::Backend::Arm64
