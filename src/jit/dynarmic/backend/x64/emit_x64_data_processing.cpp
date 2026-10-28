// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <cstddef>
#include <type_traits>

#include "common/assert.h"
#include "common/common_types.h"

#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/emit_x64.h"
#include "dynarmic/interface/optimization_flags.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::X64 {

using namespace Xbyak::util;

void EmitX64::EmitPack2x32To1x64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg64 lo = ctx.reg_alloc.UseScratchGpr(code, args[0]);
    const Xbyak::Reg64 hi = ctx.reg_alloc.UseScratchGpr(code, args[1]);

    code.shl(hi, 32);
    code.mov(lo.cvt32(), lo.cvt32());  // Zero extend to 64-bits
    code.or_(lo, hi);

    ctx.reg_alloc.DefineValue(code, inst, lo);
}

void EmitX64::EmitPack2x64To1x128(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg64 lo = ctx.reg_alloc.UseGpr(code, args[0]);
    const Xbyak::Reg64 hi = ctx.reg_alloc.UseGpr(code, args[1]);
    const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm(code);

    if (code.HasHostFeature(HostFeature::SSE41)) {
        code.movq(result, lo);
        code.pinsrq(result, hi, 1);
    } else {
        const Xbyak::Xmm tmp = ctx.reg_alloc.ScratchXmm(code);
        code.movq(result, lo);
        code.movq(tmp, hi);
        code.punpcklqdq(result, tmp);
    }

    ctx.reg_alloc.DefineValue(code, inst, result);
}

// Venture: an extending move writes a fresh register when its source stays
// live or is a register-cache register, instead of copying it first.
template<typename Op>
static void EmitExtend(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst, Op op) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    if (!args[0].IsImmediate() && !ctx.reg_alloc.IsLastUse(args[0])) {
        const Xbyak::Reg64 source = ctx.reg_alloc.UseGpr(code, args[0]);
        const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code);
        op(result, source);
        ctx.reg_alloc.DefineValue(code, inst, result);
        return;
    }
    const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, args[0]);
    op(result, result);
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitLeastSignificantWord(EmitContext& ctx, IR::Inst* inst) {
    // Venture: the word aliases its source. Word emitters use 32-bit forms (or
    // 32-bit results of LEA) and widen only through explicit IR extensions, so
    // bits 32-63 of a word's register are never read as part of the word.
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.DefineValue(code, inst, args[0]);
}

void EmitX64::EmitMostSignificantWord(EmitContext& ctx, IR::Inst* inst) {
    const auto carry_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp);

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, args[0]);
    code.shr(result, 32);

    if (carry_inst) {
        const Xbyak::Reg64 carry = ctx.reg_alloc.ScratchGpr(code);
        code.setc(carry.cvt8());
        ctx.reg_alloc.DefineValue(code, carry_inst, carry);
    }

    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitLeastSignificantHalf(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    // TODO: DefineValue directly on Argument
    const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code);
    const Xbyak::Reg64 source = ctx.reg_alloc.UseGpr(code, args[0]);
    code.movzx(result.cvt32(), source.cvt16());
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitLeastSignificantByte(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    // TODO: DefineValue directly on Argument
    const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code);
    const Xbyak::Reg64 source = ctx.reg_alloc.UseGpr(code, args[0]);
    code.movzx(result.cvt32(), source.cvt8());
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitMostSignificantBit(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, args[0]).cvt32();
    // TODO: Flag optimization
    code.shr(result, 31);
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitIsZero32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, args[0]).cvt32();
    // TODO: Flag optimization
    code.test(result, result);
    code.sete(result.cvt8());
    code.movzx(result, result.cvt8());
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitIsZero64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, args[0]);
    // TODO: Flag optimization
    code.test(result, result);
    code.sete(result.cvt8());
    code.movzx(result, result.cvt8());
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitTestBit(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, args[0]);
    ASSERT(args[1].IsImmediate());
    // TODO: Flag optimization
    code.bt(result, args[1].GetImmediateU8());
    code.setc(result.cvt8());
    ctx.reg_alloc.DefineValue(code, inst, result);
}

static void EmitConditionalSelect(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst, int bitsize, bool explicit_flags = false) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const IR::Cond cond = args[0].GetImmediateCond();
    // Host FLAGS still hold the NZCV argument: select with them directly.
    // Only MOV and CMOV may run before the select (the register allocator
    // materializes 0 with XOR, so immediates are loaded here with MOV), and
    // HI/LS restore CF afterwards so later selects see the same FLAGS.
    const bool flags_in_host = explicit_flags && ctx.select_flags_in_host;
    ctx.select_flags_in_host = false;
    const bool fp_flags = explicit_flags && ctx.fp_flags_select == inst;
    const bool fp_blend = fp_flags && ctx.fp_select_blend;
    ctx.fp_flags_select = nullptr;
    ctx.fp_select_blend = false;
    if (fp_blend) {
        // Venture: the FPCompare left an all-ones/all-zeros lane 0 mask.
        const Xbyak::Xmm then_ = ctx.reg_alloc.UseXmm(code, args[1]);
        const Xbyak::Xmm else_ = ctx.reg_alloc.UseXmm(code, args[2]);
        const Xbyak::Xmm result = ctx.reg_alloc.UseScratchXmm(code, args[3]);
        if (bitsize == 32) {
            code.vblendvps(result, else_, then_, result);
        } else {
            code.vblendvpd(result, else_, then_, result);
        }
        ctx.reg_alloc.DefineValue(code, inst, result);
        return;
    }
    const auto operand = [&](Argument& arg, bool scratch) -> Xbyak::Reg {
        if (arg.IsImmediate()) {
            const Xbyak::Reg64 reg = ctx.reg_alloc.ScratchGpr(code);
            code.mov(reg, arg.GetImmediateU64());
            return reg.changeBit(bitsize);
        }
        return (scratch ? ctx.reg_alloc.UseScratchGpr(code, arg) : ctx.reg_alloc.UseGpr(code, arg)).changeBit(bitsize);
    };
    if (fp_flags) {
        // Venture: FLAGS hold UCOMIS/COMIS of the FPCompare's operands (b with a
        // for FPFlagsSelectSwapsOperands conditions). ZF:PF:CF is 111 unordered,
        // 000 greater, 001 less and 100 equal. Operands load as in the path below.
        if (cond == IR::Cond::EQ) {
            const Xbyak::Reg result = operand(args[1], true);
            const Xbyak::Reg else_ = operand(args[2], false);
            code.cmovnz(result, else_);
            code.cmovp(result, else_);
            ctx.reg_alloc.DefineValue(code, inst, result);
            return;
        }
        const Xbyak::Reg then_ = operand(args[1], false);
        const Xbyak::Reg else_ = operand(args[2], true);
        switch (cond) {
        case IR::Cond::NE:
            code.cmovnz(else_, then_);
            code.cmovp(else_, then_);
            break;
        case IR::Cond::GT:
        case IR::Cond::MI:
        case IR::Cond::CC: code.cmova(else_, then_); break;
        case IR::Cond::GE:
        case IR::Cond::LS: code.cmovae(else_, then_); break;
        case IR::Cond::LT:
        case IR::Cond::HI: code.cmovb(else_, then_); break;
        case IR::Cond::LE:
        case IR::Cond::PL:
        case IR::Cond::CS: code.cmovbe(else_, then_); break;
        case IR::Cond::VS: code.cmovp(else_, then_); break;
        case IR::Cond::VC: code.cmovnp(else_, then_); break;
        case IR::Cond::AL:
        case IR::Cond::NV: code.mov(else_, then_); break;
        default: UNREACHABLE();
        }
        ctx.reg_alloc.DefineValue(code, inst, else_);
        return;
    }
    if (flags_in_host) {
        const Xbyak::Reg then_ = operand(args[1], false);
        const Xbyak::Reg else_ = operand(args[2], true);
        if (cond == IR::Cond::HI || cond == IR::Cond::LS)
            code.cmc();
        switch (cond) {
        case IR::Cond::EQ: code.cmovz(else_, then_); break;
        case IR::Cond::NE: code.cmovnz(else_, then_); break;
        case IR::Cond::CS: code.cmovc(else_, then_); break;
        case IR::Cond::CC: code.cmovnc(else_, then_); break;
        case IR::Cond::MI: code.cmovs(else_, then_); break;
        case IR::Cond::PL: code.cmovns(else_, then_); break;
        case IR::Cond::VS: code.cmovo(else_, then_); break;
        case IR::Cond::VC: code.cmovno(else_, then_); break;
        case IR::Cond::HI: code.cmova(else_, then_); break;
        case IR::Cond::LS: code.cmovna(else_, then_); break;
        case IR::Cond::GE: code.cmovge(else_, then_); break;
        case IR::Cond::LT: code.cmovl(else_, then_); break;
        case IR::Cond::GT: code.cmovg(else_, then_); break;
        case IR::Cond::LE: code.cmovle(else_, then_); break;
        case IR::Cond::AL:
        case IR::Cond::NV: code.mov(else_, then_); break;
        default: UNREACHABLE();
        }
        if (cond == IR::Cond::HI || cond == IR::Cond::LS)
            code.cmc();
        ctx.reg_alloc.DefineValue(code, inst, else_);
        return;
    }
    // Explicit flags are a GetNZCVFromOp value in the guest-state format.
    if (explicit_flags) {
        ctx.reg_alloc.UseScratch(code, args[3], HostLoc::RAX);
    } else {
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
    }
    const Xbyak::Reg then_ = ctx.reg_alloc.UseGpr(code, args[1]).changeBit(bitsize);
    const Xbyak::Reg else_ = ctx.reg_alloc.UseScratchGpr(code, args[2]).changeBit(bitsize);

    if (!explicit_flags) {
        code.mov(code.eax, dword[code.ABI_JIT_PTR + code.GetJitStateInfo().offsetof_cpsr_nzcv]);
    }

    code.LoadRequiredFlagsForCondFromRax(cond);

    switch (args[0].GetImmediateCond()) {
    case IR::Cond::EQ:
        code.cmovz(else_, then_);
        break;
    case IR::Cond::NE:
        code.cmovnz(else_, then_);
        break;
    case IR::Cond::CS:
        code.cmovc(else_, then_);
        break;
    case IR::Cond::CC:
        code.cmovnc(else_, then_);
        break;
    case IR::Cond::MI:
        code.cmovs(else_, then_);
        break;
    case IR::Cond::PL:
        code.cmovns(else_, then_);
        break;
    case IR::Cond::VS:
        code.cmovo(else_, then_);
        break;
    case IR::Cond::VC:
        code.cmovno(else_, then_);
        break;
    case IR::Cond::HI:
        code.cmova(else_, then_);
        break;
    case IR::Cond::LS:
        code.cmovna(else_, then_);
        break;
    case IR::Cond::GE:
        code.cmovge(else_, then_);
        break;
    case IR::Cond::LT:
        code.cmovl(else_, then_);
        break;
    case IR::Cond::GT:
        code.cmovg(else_, then_);
        break;
    case IR::Cond::LE:
        code.cmovle(else_, then_);
        break;
    case IR::Cond::AL:
    case IR::Cond::NV:
        code.mov(else_, then_);
        break;
    default:
        UNREACHABLE();
    }

    ctx.reg_alloc.DefineValue(code, inst, else_);
}

void EmitX64::EmitConditionalSelect32(EmitContext& ctx, IR::Inst* inst) {
    EmitConditionalSelect(code, ctx, inst, 32);
}

void EmitX64::EmitConditionalSelect64(EmitContext& ctx, IR::Inst* inst) {
    EmitConditionalSelect(code, ctx, inst, 64);
}

void EmitX64::EmitConditionalSelectNZCV(EmitContext& ctx, IR::Inst* inst) {
    EmitConditionalSelect(code, ctx, inst, 32);
}

void EmitX64::EmitConditionalSelectFlags32(EmitContext& ctx, IR::Inst* inst) {
    EmitConditionalSelect(code, ctx, inst, 32, true);
}

void EmitX64::EmitConditionalSelectFlags64(EmitContext& ctx, IR::Inst* inst) {
    EmitConditionalSelect(code, ctx, inst, 64, true);
}

void EmitX64::EmitConditionalSelectFlagsNZCV(EmitContext& ctx, IR::Inst* inst) {
    EmitConditionalSelect(code, ctx, inst, 32, true);
}

static void EmitExtractRegister(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst, int bit_size) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Reg result = ctx.reg_alloc.UseScratchGpr(code, args[0]).changeBit(bit_size);
    const Xbyak::Reg operand = ctx.reg_alloc.UseGpr(code, args[1]).changeBit(bit_size);
    const u8 lsb = args[2].GetImmediateU8();

    code.shrd(result, operand, lsb);

    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitExtractRegister32(Dynarmic::Backend::X64::EmitContext& ctx, IR::Inst* inst) {
    EmitExtractRegister(code, ctx, inst, 32);
}

void EmitX64::EmitExtractRegister64(Dynarmic::Backend::X64::EmitContext& ctx, IR::Inst* inst) {
    EmitExtractRegister(code, ctx, inst, 64);
}

static void EmitReplicateBit(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst, int bit_size) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const u8 bit = args[1].GetImmediateU8();

    if (bit == bit_size - 1) {
        const Xbyak::Reg result = ctx.reg_alloc.UseScratchGpr(code, args[0]).changeBit(bit_size);

        code.sar(result, bit_size - 1);

        ctx.reg_alloc.DefineValue(code, inst, result);
        return;
    }

    const Xbyak::Reg value = ctx.reg_alloc.UseGpr(code, args[0]).changeBit(bit_size);
    const Xbyak::Reg result = ctx.reg_alloc.ScratchGpr(code).changeBit(bit_size);

    code.xor_(result, result);
    code.bt(value, bit);
    code.sbb(result, result);

    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitReplicateBit32(Dynarmic::Backend::X64::EmitContext& ctx, IR::Inst* inst) {
    EmitReplicateBit(code, ctx, inst, 32);
}

void EmitX64::EmitReplicateBit64(Dynarmic::Backend::X64::EmitContext& ctx, IR::Inst* inst) {
    EmitReplicateBit(code, ctx, inst, 64);
}

void EmitX64::EmitLogicalShiftLeft32(EmitContext& ctx, IR::Inst* inst) {
    const auto carry_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp);

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto& operand_arg = args[0];
    auto& shift_arg = args[1];
    auto& carry_arg = args[2];

    if (!carry_inst) {
        if (shift_arg.IsImmediate()) {
            const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg).cvt32();
            const u8 shift = shift_arg.GetImmediateU8();

            if (shift <= 31) {
                code.shl(result, shift);
            } else {
                code.xor_(result, result);
            }

            ctx.reg_alloc.DefineValue(code, inst, result);
        } else if (code.HasHostFeature(HostFeature::BMI2)) {
            const Xbyak::Reg32 shift = ctx.reg_alloc.UseGpr(code, shift_arg).cvt32();
            const Xbyak::Reg32 operand = ctx.reg_alloc.UseGpr(code, operand_arg).cvt32();
            const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr(code).cvt32();
            const Xbyak::Reg32 zero = ctx.reg_alloc.ScratchGpr(code).cvt32();

            code.shlx(result, operand, shift);
            code.xor_(zero, zero);
            code.cmp(shift.cvt8(), 32);
            code.cmovnb(result, zero);

            ctx.reg_alloc.DefineValue(code, inst, result);
        } else {
            ctx.reg_alloc.Use(code, shift_arg, HostLoc::RCX);
            const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg).cvt32();
            const Xbyak::Reg32 zero = ctx.reg_alloc.ScratchGpr(code).cvt32();

            // The 32-bit x64 SHL instruction masks the shift count by 0x1F before performing the shift.
            // ARM differs from the behaviour: It does not mask the count, so shifts above 31 result in zeros.

            code.shl(result, code.cl);
            code.xor_(zero, zero);
            code.cmp(code.cl, 32);
            code.cmovnb(result, zero);

            ctx.reg_alloc.DefineValue(code, inst, result);
        }
    } else {
        if (shift_arg.IsImmediate()) {
            const u8 shift = shift_arg.GetImmediateU8();
            const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg).cvt32();
            const Xbyak::Reg32 carry = ctx.reg_alloc.UseScratchGpr(code, carry_arg).cvt32();

            if (shift == 0) {
                // There is nothing more to do.
            } else if (shift < 32) {
                code.bt(carry.cvt32(), 0);
                code.shl(result, shift);
                code.setc(carry.cvt8());
            } else if (shift > 32) {
                code.xor_(result, result);
                code.xor_(carry, carry);
            } else {
                code.mov(carry, result);
                code.xor_(result, result);
                code.and_(carry, 1);
            }

            ctx.reg_alloc.DefineValue(code, carry_inst, carry);
            ctx.reg_alloc.DefineValue(code, inst, result);
        } else {
            ctx.reg_alloc.UseScratch(code, shift_arg, HostLoc::RCX);
            const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg).cvt32();
            const Xbyak::Reg32 tmp = ctx.reg_alloc.ScratchGpr(code).cvt32();
            const Xbyak::Reg32 carry = ctx.reg_alloc.UseScratchGpr(code, carry_arg).cvt32();

            code.mov(tmp, 63);
            code.cmp(code.cl, 63);
            code.cmova(code.ecx, tmp);
            code.shl(result.cvt64(), 32);
            code.bt(carry.cvt32(), 0);
            code.shl(result.cvt64(), code.cl);
            code.setc(carry.cvt8());
            code.shr(result.cvt64(), 32);

            ctx.reg_alloc.DefineValue(code, carry_inst, carry);
            ctx.reg_alloc.DefineValue(code, inst, result);
        }
    }
}

void EmitX64::EmitLogicalShiftLeft64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto& operand_arg = args[0];
    auto& shift_arg = args[1];

    if (shift_arg.IsImmediate()) {
        const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg);
        const u8 shift = shift_arg.GetImmediateU8();

        if (shift < 64) {
            code.shl(result, shift);
        } else {
            code.xor_(result.cvt32(), result.cvt32());
        }

        ctx.reg_alloc.DefineValue(code, inst, result);
    } else if (code.HasHostFeature(HostFeature::BMI2)) {
        const Xbyak::Reg64 shift = ctx.reg_alloc.UseGpr(code, shift_arg);
        const Xbyak::Reg64 operand = ctx.reg_alloc.UseGpr(code, operand_arg);
        const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code);
        const Xbyak::Reg64 zero = ctx.reg_alloc.ScratchGpr(code);

        code.shlx(result, operand, shift);
        code.xor_(zero.cvt32(), zero.cvt32());
        code.cmp(shift.cvt8(), 64);
        code.cmovnb(result, zero);

        ctx.reg_alloc.DefineValue(code, inst, result);
    } else {
        ctx.reg_alloc.Use(code, shift_arg, HostLoc::RCX);
        const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg);
        const Xbyak::Reg64 zero = ctx.reg_alloc.ScratchGpr(code);

        // The x64 SHL instruction masks the shift count by 0x1F before performing the shift.
        // ARM differs from the behaviour: It does not mask the count, so shifts above 63 result in zeros.

        code.shl(result, code.cl);
        code.xor_(zero.cvt32(), zero.cvt32());
        code.cmp(code.cl, 64);
        code.cmovnb(result, zero);

        ctx.reg_alloc.DefineValue(code, inst, result);
    }
}

void EmitX64::EmitLogicalShiftRight32(EmitContext& ctx, IR::Inst* inst) {
    const auto carry_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp);

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto& operand_arg = args[0];
    auto& shift_arg = args[1];
    auto& carry_arg = args[2];

    if (!carry_inst) {
        if (shift_arg.IsImmediate()) {
            const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg).cvt32();
            const u8 shift = shift_arg.GetImmediateU8();

            if (shift <= 31) {
                code.shr(result, shift);
            } else {
                code.xor_(result, result);
            }

            ctx.reg_alloc.DefineValue(code, inst, result);
        } else if (code.HasHostFeature(HostFeature::BMI2)) {
            const Xbyak::Reg32 shift = ctx.reg_alloc.UseGpr(code, shift_arg).cvt32();
            const Xbyak::Reg32 operand = ctx.reg_alloc.UseGpr(code, operand_arg).cvt32();
            const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr(code).cvt32();
            const Xbyak::Reg32 zero = ctx.reg_alloc.ScratchGpr(code).cvt32();

            code.shrx(result, operand, shift);
            code.xor_(zero, zero);
            code.cmp(shift.cvt8(), 32);
            code.cmovnb(result, zero);

            ctx.reg_alloc.DefineValue(code, inst, result);
        } else {
            ctx.reg_alloc.Use(code, shift_arg, HostLoc::RCX);
            const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg).cvt32();
            const Xbyak::Reg32 zero = ctx.reg_alloc.ScratchGpr(code).cvt32();

            // The 32-bit x64 SHR instruction masks the shift count by 0x1F before performing the shift.
            // ARM differs from the behaviour: It does not mask the count, so shifts above 31 result in zeros.

            code.shr(result, code.cl);
            code.xor_(zero, zero);
            code.cmp(code.cl, 32);
            code.cmovnb(result, zero);

            ctx.reg_alloc.DefineValue(code, inst, result);
        }
    } else {
        if (shift_arg.IsImmediate()) {
            const u8 shift = shift_arg.GetImmediateU8();
            const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg).cvt32();
            const Xbyak::Reg32 carry = ctx.reg_alloc.UseScratchGpr(code, carry_arg).cvt32();

            if (shift == 0) {
                // There is nothing more to do.
            } else if (shift < 32) {
                code.shr(result, shift);
                code.setc(carry.cvt8());
            } else if (shift == 32) {
                code.bt(result, 31);
                code.setc(carry.cvt8());
                code.mov(result, 0);
            } else {
                code.xor_(result, result);
                code.xor_(carry, carry);
            }

            ctx.reg_alloc.DefineValue(code, carry_inst, carry);
            ctx.reg_alloc.DefineValue(code, inst, result);
        } else {
            ctx.reg_alloc.UseScratch(code, shift_arg, HostLoc::RCX);
            const Xbyak::Reg32 operand = ctx.reg_alloc.UseGpr(code, operand_arg).cvt32();
            const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr(code).cvt32();
            const Xbyak::Reg32 carry = ctx.reg_alloc.UseScratchGpr(code, carry_arg).cvt32();

            code.mov(result, 63);
            code.cmp(code.cl, 63);
            code.cmovnb(code.ecx, result);
            code.mov(result, operand);
            code.bt(carry.cvt32(), 0);
            code.shr(result.cvt64(), code.cl);
            code.setc(carry.cvt8());

            ctx.reg_alloc.DefineValue(code, carry_inst, carry);
            ctx.reg_alloc.DefineValue(code, inst, result);
        }
    }
}

void EmitX64::EmitLogicalShiftRight64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto& operand_arg = args[0];
    auto& shift_arg = args[1];

    if (shift_arg.IsImmediate()) {
        const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg);
        const u8 shift = shift_arg.GetImmediateU8();

        if (shift < 64) {
            code.shr(result, shift);
        } else {
            code.xor_(result.cvt32(), result.cvt32());
        }

        ctx.reg_alloc.DefineValue(code, inst, result);
    } else if (code.HasHostFeature(HostFeature::BMI2)) {
        const Xbyak::Reg64 shift = ctx.reg_alloc.UseGpr(code, shift_arg);
        const Xbyak::Reg64 operand = ctx.reg_alloc.UseGpr(code, operand_arg);
        const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code);
        const Xbyak::Reg64 zero = ctx.reg_alloc.ScratchGpr(code);

        code.shrx(result, operand, shift);
        code.xor_(zero.cvt32(), zero.cvt32());
        code.cmp(shift.cvt8(), 64);
        code.cmovnb(result, zero);

        ctx.reg_alloc.DefineValue(code, inst, result);
    } else {
        ctx.reg_alloc.Use(code, shift_arg, HostLoc::RCX);
        const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg);
        const Xbyak::Reg64 zero = ctx.reg_alloc.ScratchGpr(code);

        // The x64 SHR instruction masks the shift count by 0x1F before performing the shift.
        // ARM differs from the behaviour: It does not mask the count, so shifts above 63 result in zeros.

        code.shr(result, code.cl);
        code.xor_(zero.cvt32(), zero.cvt32());
        code.cmp(code.cl, 64);
        code.cmovnb(result, zero);

        ctx.reg_alloc.DefineValue(code, inst, result);
    }
}

void EmitX64::EmitArithmeticShiftRight32(EmitContext& ctx, IR::Inst* inst) {
    const auto carry_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp);

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto& operand_arg = args[0];
    auto& shift_arg = args[1];
    auto& carry_arg = args[2];

    if (!carry_inst) {
        if (shift_arg.IsImmediate()) {
            const u8 shift = shift_arg.GetImmediateU8();
            const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg).cvt32();

            code.sar(result, u8(shift < 31 ? shift : 31));

            ctx.reg_alloc.DefineValue(code, inst, result);
        } else if (code.HasHostFeature(HostFeature::BMI2)) {
            const Xbyak::Reg32 shift = ctx.reg_alloc.UseScratchGpr(code, shift_arg).cvt32();
            const Xbyak::Reg32 operand = ctx.reg_alloc.UseGpr(code, operand_arg).cvt32();
            const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr(code).cvt32();
            const Xbyak::Reg32 const31 = ctx.reg_alloc.ScratchGpr(code).cvt32();

            // The 32-bit x64 SAR instruction masks the shift count by 0x1F before performing the shift.
            // ARM differs from the behaviour: It does not mask the count.

            // We note that all shift values above 31 have the same behaviour as 31 does, so we saturate `shift` to 31.
            code.mov(const31, 31);
            code.cmp(shift.cvt8(), 31);
            code.cmovnb(shift, const31);
            code.sarx(result, operand, shift);

            ctx.reg_alloc.DefineValue(code, inst, result);
        } else {
            ctx.reg_alloc.UseScratch(code, shift_arg, HostLoc::RCX);
            const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg).cvt32();
            const Xbyak::Reg32 const31 = ctx.reg_alloc.ScratchGpr(code).cvt32();

            // The 32-bit x64 SAR instruction masks the shift count by 0x1F before performing the shift.
            // ARM differs from the behaviour: It does not mask the count.

            // We note that all shift values above 31 have the same behaviour as 31 does, so we saturate `shift` to 31.
            code.mov(const31, 31);
            code.cmp(code.cl, u32(31));
            code.cmova(code.ecx, const31);
            code.sar(result, code.cl);

            ctx.reg_alloc.DefineValue(code, inst, result);
        }
    } else {
        if (shift_arg.IsImmediate()) {
            const u8 shift = shift_arg.GetImmediateU8();
            const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg).cvt32();
            const Xbyak::Reg8 carry = ctx.reg_alloc.UseScratchGpr(code, carry_arg).cvt8();

            if (shift == 0) {
                // There is nothing more to do.
            } else if (shift <= 31) {
                code.sar(result, shift);
                code.setc(carry);
            } else {
                code.sar(result, 31);
                code.bt(result, 31);
                code.setc(carry);
            }

            ctx.reg_alloc.DefineValue(code, carry_inst, carry);
            ctx.reg_alloc.DefineValue(code, inst, result);
        } else {
            ctx.reg_alloc.UseScratch(code, shift_arg, HostLoc::RCX);
            const Xbyak::Reg32 operand = ctx.reg_alloc.UseGpr(code, operand_arg).cvt32();
            const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr(code).cvt32();
            const Xbyak::Reg32 carry = ctx.reg_alloc.UseScratchGpr(code, carry_arg).cvt32();

            code.mov(result, 63);
            code.cmp(code.cl, 63);
            code.cmovnb(code.ecx, result);
            code.movsxd(result.cvt64(), operand);
            code.bt(carry.cvt32(), 0);
            code.sar(result.cvt64(), code.cl);
            code.setc(carry.cvt8());

            ctx.reg_alloc.DefineValue(code, carry_inst, carry);
            ctx.reg_alloc.DefineValue(code, inst, result);
        }
    }
}

void EmitX64::EmitArithmeticShiftRight64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto& operand_arg = args[0];
    auto& shift_arg = args[1];

    if (shift_arg.IsImmediate()) {
        const u8 shift = shift_arg.GetImmediateU8();
        const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg);

        code.sar(result, u8(shift < 63 ? shift : 63));

        ctx.reg_alloc.DefineValue(code, inst, result);
    } else if (code.HasHostFeature(HostFeature::BMI2)) {
        const Xbyak::Reg64 shift = ctx.reg_alloc.UseScratchGpr(code, shift_arg);
        const Xbyak::Reg64 operand = ctx.reg_alloc.UseGpr(code, operand_arg);
        const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code);
        const Xbyak::Reg64 const63 = ctx.reg_alloc.ScratchGpr(code);

        code.mov(const63.cvt32(), 63);
        code.cmp(shift.cvt8(), 63);
        code.cmovnb(shift, const63);
        code.sarx(result, operand, shift);

        ctx.reg_alloc.DefineValue(code, inst, result);
    } else {
        ctx.reg_alloc.UseScratch(code, shift_arg, HostLoc::RCX);
        const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg);
        const Xbyak::Reg64 const63 = ctx.reg_alloc.ScratchGpr(code);

        // The 64-bit x64 SAR instruction masks the shift count by 0x3F before performing the shift.
        // ARM differs from the behaviour: It does not mask the count.

        // We note that all shift values above 63 have the same behaviour as 63 does, so we saturate `shift` to 63.
        code.mov(const63, 63);
        code.cmp(code.cl, u32(63));
        code.cmovnb(code.ecx, const63);
        code.sar(result, code.cl);

        ctx.reg_alloc.DefineValue(code, inst, result);
    }
}

void EmitX64::EmitBitRotateRight32(EmitContext& ctx, IR::Inst* inst) {
    const auto carry_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp);

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto& operand_arg = args[0];
    auto& shift_arg = args[1];
    auto& carry_arg = args[2];

    if (!carry_inst) {
        if (shift_arg.IsImmediate() && code.HasHostFeature(HostFeature::BMI2)) {
            const u8 shift = shift_arg.GetImmediateU8();
            const Xbyak::Reg32 operand = ctx.reg_alloc.UseGpr(code, operand_arg).cvt32();
            const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr(code).cvt32();

            code.rorx(result, operand, shift);

            ctx.reg_alloc.DefineValue(code, inst, result);
        } else if (shift_arg.IsImmediate()) {
            const u8 shift = shift_arg.GetImmediateU8();
            const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg).cvt32();

            code.ror(result, u8(shift & 0x1F));

            ctx.reg_alloc.DefineValue(code, inst, result);
        } else {
            ctx.reg_alloc.Use(code, shift_arg, HostLoc::RCX);
            const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg).cvt32();

            // x64 ROR instruction does (shift & 0x1F) for us.
            code.ror(result, code.cl);

            ctx.reg_alloc.DefineValue(code, inst, result);
        }
    } else {
        if (shift_arg.IsImmediate()) {
            const u8 shift = shift_arg.GetImmediateU8();
            const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg).cvt32();
            const Xbyak::Reg8 carry = ctx.reg_alloc.UseScratchGpr(code, carry_arg).cvt8();

            if (shift == 0) {
                // There is nothing more to do.
            } else if ((shift & 0x1F) == 0) {
                code.bt(result, u8(31));
                code.setc(carry);
            } else {
                code.ror(result, shift);
                code.setc(carry);
            }

            ctx.reg_alloc.DefineValue(code, carry_inst, carry);
            ctx.reg_alloc.DefineValue(code, inst, result);
        } else {
            ctx.reg_alloc.UseScratch(code, shift_arg, HostLoc::RCX);
            const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg).cvt32();
            const Xbyak::Reg8 carry = ctx.reg_alloc.UseScratchGpr(code, carry_arg).cvt8();

            Xbyak::Label end;

            code.test(code.cl, code.cl);
            code.jz(end);

            code.ror(result, code.cl);
            code.bt(result, u8(31));
            code.setc(carry);

            code.L(end);

            ctx.reg_alloc.DefineValue(code, carry_inst, carry);
            ctx.reg_alloc.DefineValue(code, inst, result);
        }
    }
}

void EmitX64::EmitBitRotateRight64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto& operand_arg = args[0];
    auto& shift_arg = args[1];

    if (shift_arg.IsImmediate() && code.HasHostFeature(HostFeature::BMI2)) {
        const u8 shift = shift_arg.GetImmediateU8();
        const Xbyak::Reg64 operand = ctx.reg_alloc.UseGpr(code, operand_arg);
        const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code);

        code.rorx(result, operand, shift);

        ctx.reg_alloc.DefineValue(code, inst, result);
    } else if (shift_arg.IsImmediate()) {
        const u8 shift = shift_arg.GetImmediateU8();
        const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg);

        code.ror(result, u8(shift & 0x3F));

        ctx.reg_alloc.DefineValue(code, inst, result);
    } else {
        ctx.reg_alloc.Use(code, shift_arg, HostLoc::RCX);
        const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg);

        // x64 ROR instruction does (shift & 0x3F) for us.
        code.ror(result, code.cl);

        ctx.reg_alloc.DefineValue(code, inst, result);
    }
}

void EmitX64::EmitRotateRightExtended(EmitContext& ctx, IR::Inst* inst) {
    const auto carry_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp);

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, args[0]).cvt32();
    const Xbyak::Reg8 carry = ctx.reg_alloc.UseScratchGpr(code, args[1]).cvt8();

    code.bt(carry.cvt32(), 0);
    code.rcr(result, 1);

    if (carry_inst) {
        code.setc(carry);

        ctx.reg_alloc.DefineValue(code, carry_inst, carry);
    }

    ctx.reg_alloc.DefineValue(code, inst, result);
}

template<typename ShfitFT, typename BMI2FT>
static void EmitMaskedShift32(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst, ShfitFT shift_fn, [[maybe_unused]] BMI2FT bmi2_shift) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto& operand_arg = args[0];
    auto& shift_arg = args[1];

    if (shift_arg.IsImmediate()) {
        const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg).cvt32();
        const u32 shift = shift_arg.GetImmediateU32();

        shift_fn(result, static_cast<int>(shift & 0x1F));

        ctx.reg_alloc.DefineValue(code, inst, result);
        return;
    }

    if constexpr (!std::is_same_v<BMI2FT, std::nullptr_t>) {
        if (code.HasHostFeature(HostFeature::BMI2)) {
            const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr(code).cvt32();
            const Xbyak::Reg32 operand = ctx.reg_alloc.UseGpr(code, operand_arg).cvt32();
            const Xbyak::Reg32 shift = ctx.reg_alloc.UseGpr(code, shift_arg).cvt32();

            (code.*bmi2_shift)(result, operand, shift);

            ctx.reg_alloc.DefineValue(code, inst, result);
            return;
        }
    }

    ctx.reg_alloc.Use(code, shift_arg, HostLoc::RCX);
    const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg).cvt32();

    shift_fn(result, code.cl);

    ctx.reg_alloc.DefineValue(code, inst, result);
}

template<typename ShfitFT, typename BMI2FT>
static void EmitMaskedShift64(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst, ShfitFT shift_fn, [[maybe_unused]] BMI2FT bmi2_shift) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto& operand_arg = args[0];
    auto& shift_arg = args[1];

    if (shift_arg.IsImmediate()) {
        const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg);
        const u64 shift = shift_arg.GetImmediateU64();

        shift_fn(result, static_cast<int>(shift & 0x3F));

        ctx.reg_alloc.DefineValue(code, inst, result);
        return;
    }

    if constexpr (!std::is_same_v<BMI2FT, std::nullptr_t>) {
        if (code.HasHostFeature(HostFeature::BMI2)) {
            const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code);
            const Xbyak::Reg64 operand = ctx.reg_alloc.UseGpr(code, operand_arg);
            const Xbyak::Reg64 shift = ctx.reg_alloc.UseGpr(code, shift_arg);

            (code.*bmi2_shift)(result, operand, shift);

            ctx.reg_alloc.DefineValue(code, inst, result);
            return;
        }
    }

    ctx.reg_alloc.Use(code, shift_arg, HostLoc::RCX);
    const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, operand_arg);

    shift_fn(result, code.cl);

    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitLogicalShiftLeftMasked32(EmitContext& ctx, IR::Inst* inst) {
    EmitMaskedShift32(
        code, ctx, inst, [&](auto result, auto shift) { code.shl(result, shift); }, &Xbyak::CodeGenerator::shlx);
}

void EmitX64::EmitLogicalShiftLeftMasked64(EmitContext& ctx, IR::Inst* inst) {
    EmitMaskedShift64(
        code, ctx, inst, [&](auto result, auto shift) { code.shl(result, shift); }, &Xbyak::CodeGenerator::shlx);
}

void EmitX64::EmitLogicalShiftRightMasked32(EmitContext& ctx, IR::Inst* inst) {
    EmitMaskedShift32(
        code, ctx, inst, [&](auto result, auto shift) { code.shr(result, shift); }, &Xbyak::CodeGenerator::shrx);
}

void EmitX64::EmitLogicalShiftRightMasked64(EmitContext& ctx, IR::Inst* inst) {
    EmitMaskedShift64(
        code, ctx, inst, [&](auto result, auto shift) { code.shr(result, shift); }, &Xbyak::CodeGenerator::shrx);
}

void EmitX64::EmitArithmeticShiftRightMasked32(EmitContext& ctx, IR::Inst* inst) {
    EmitMaskedShift32(
        code, ctx, inst, [&](auto result, auto shift) { code.sar(result, shift); }, &Xbyak::CodeGenerator::sarx);
}

void EmitX64::EmitArithmeticShiftRightMasked64(EmitContext& ctx, IR::Inst* inst) {
    EmitMaskedShift64(
        code, ctx, inst, [&](auto result, auto shift) { code.sar(result, shift); }, &Xbyak::CodeGenerator::sarx);
}

void EmitX64::EmitRotateRightMasked32(EmitContext& ctx, IR::Inst* inst) {
    EmitMaskedShift32(
        code, ctx, inst, [&](auto result, auto shift) { code.ror(result, shift); }, nullptr);
}

void EmitX64::EmitRotateRightMasked64(EmitContext& ctx, IR::Inst* inst) {
    EmitMaskedShift64(
        code, ctx, inst, [&](auto result, auto shift) { code.ror(result, shift); }, nullptr);
}

// Venture: a commutative operation writes over whichever register operand
// dies here, so a still-live or register-cache operand is not copied.
static void PreferDyingFirstOperand(EmitContext& ctx, RegAlloc::ArgumentInfo& args) {
    if (!args[0].IsImmediate() && !args[1].IsImmediate() && !ctx.reg_alloc.IsLastUse(args[0]) && ctx.reg_alloc.IsLastUse(args[1]))
        std::swap(args[0], args[1]);
}

static Xbyak::Reg8 DoCarry(BlockOfCode& code, RegAlloc& reg_alloc, Argument& carry_in, IR::Inst* carry_out) {
    if (carry_in.IsImmediate()) {
        return carry_out ? reg_alloc.ScratchGpr(code).cvt8() : Xbyak::Reg8{-1};
    } else {
        return carry_out ? reg_alloc.UseScratchGpr(code, carry_in).cvt8() : reg_alloc.UseGpr(code, carry_in).cvt8();
    }
}

// AL contains flags (after LAHF + SETO sequence)
static Xbyak::Reg64 DoNZCV(BlockOfCode& code, RegAlloc& reg_alloc, IR::Inst* nzcv_out) {
    if (nzcv_out) {
        const Xbyak::Reg64 nzcv = reg_alloc.ScratchGpr(code, HostLoc::RAX);
        code.xor_(nzcv.cvt32(), nzcv.cvt32());
        return nzcv;
    }
    return Xbyak::Reg64{-1};
}

static void EmitAdd(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst, size_t bitsize) {
    const auto carry_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp);
    const auto overflow_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetOverflowFromOp);
    const auto nzcv_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetNZCVFromOp);

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto& carry_in = args[2];

    // Venture: x + (y << 1..3), the shift folded by the A64 emit loop.
    for (const size_t k : {size_t(1), size_t(0)}) {
        if (args[k].IsImmediate() || !ctx.IsScaledIndex(inst->GetArg(k).GetInst()))
            continue;
        const int scale = 1 << inst->GetArg(k).GetInst()->GetArg(1).GetU8();
        auto& other = args[1 - k];
        if (ctx.reg_alloc.IsLastUse(other)) {
            const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, other);
            const Xbyak::Reg64 index = ctx.reg_alloc.UseGpr(code, args[k]);
            code.lea(result.changeBit(bitsize), code.ptr[result + index * scale]);
            ctx.reg_alloc.DefineValue(code, inst, result);
        } else if (ctx.reg_alloc.IsLastUse(args[k])) {
            const Xbyak::Reg64 base = ctx.reg_alloc.UseGpr(code, other);
            const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, args[k]);
            code.lea(result.changeBit(bitsize), code.ptr[base + result * scale]);
            ctx.reg_alloc.DefineValue(code, inst, result);
        } else {
            const Xbyak::Reg64 base = ctx.reg_alloc.UseGpr(code, other);
            const Xbyak::Reg64 index = ctx.reg_alloc.UseGpr(code, args[k]);
            const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code);
            code.lea(result.changeBit(bitsize), code.ptr[base + index * scale]);
            ctx.reg_alloc.DefineValue(code, inst, result);
        }
        return;
    }

    // Consider using LEA (not when a terminal reads this result's FLAGS).
    if (!carry_inst && !overflow_inst && !nzcv_inst && carry_in.IsImmediate() && !carry_in.GetImmediateU1() && inst != ctx.host_flags_inst) {
        // Venture: 64-bit addressing (no address-size prefix; the low word of the
        // sum is the same), and a still-live or register-cache operand is read in
        // place by a three-operand LEA instead of being copied first.
        const bool in_place = !args[0].IsImmediate() && ctx.reg_alloc.IsLastUse(args[0]);
        if (args[1].IsImmediate() && args[1].FitsInImmediateS32()) {
            Xbyak::Reg64 const op1 = in_place ? ctx.reg_alloc.UseScratchGpr(code, args[0]) : ctx.reg_alloc.UseGpr(code, args[0]);
            Xbyak::Reg64 const result = in_place ? op1 : ctx.reg_alloc.ScratchGpr(code);
            code.lea(result.changeBit(bitsize), code.ptr[op1 + args[1].GetImmediateS32()]);
            ctx.reg_alloc.DefineValue(code, inst, result);
        } else if (in_place || args[0].IsImmediate()) {
            Xbyak::Reg64 const result = ctx.reg_alloc.UseScratchGpr(code, args[0]);
            Xbyak::Reg64 const op2 = ctx.reg_alloc.UseGpr(code, args[1]);
            code.lea(result.changeBit(bitsize), code.ptr[result + op2]);
            ctx.reg_alloc.DefineValue(code, inst, result);
        } else if (!args[1].IsImmediate() && ctx.reg_alloc.IsLastUse(args[1])) {
            Xbyak::Reg64 const op1 = ctx.reg_alloc.UseGpr(code, args[0]);
            Xbyak::Reg64 const result = ctx.reg_alloc.UseScratchGpr(code, args[1]);
            code.lea(result.changeBit(bitsize), code.ptr[op1 + result]);
            ctx.reg_alloc.DefineValue(code, inst, result);
        } else {
            Xbyak::Reg64 const op1 = ctx.reg_alloc.UseGpr(code, args[0]);
            Xbyak::Reg64 const op2 = ctx.reg_alloc.UseGpr(code, args[1]);
            Xbyak::Reg64 const result = ctx.reg_alloc.ScratchGpr(code);
            code.lea(result.changeBit(bitsize), code.ptr[op1 + op2]);
            ctx.reg_alloc.DefineValue(code, inst, result);
        }
        return;
    }

    PreferDyingFirstOperand(ctx, args);
    const Xbyak::Reg64 nzcv = DoNZCV(code, ctx.reg_alloc, nzcv_inst);
    const Xbyak::Reg result = ctx.reg_alloc.UseScratchGpr(code, args[0]).changeBit(bitsize);
    const Xbyak::Reg8 carry = DoCarry(code, ctx.reg_alloc, carry_in, carry_inst);
    const Xbyak::Reg8 overflow = overflow_inst ? ctx.reg_alloc.ScratchGpr(code).cvt8() : Xbyak::Reg8{-1};

    if (args[1].IsImmediate() && args[1].GetType() == IR::Type::U32) {
        const u64 op_arg = args[1].GetImmediateU64();
        if (carry_in.IsImmediate()) {
            if (carry_in.GetImmediateU1()) {
                // In range for a valid LEA materialisation
                auto const in_range = s32(op_arg) >= -0x7ffffffe && s32(op_arg) <= 0x7ffffffe;
                if (in_range && (carry_inst || nzcv_inst || overflow_inst)) {
                    code.stc();
                    code.adc(result, op_arg);
                } else {
                    code.lea(result, code.ptr[result + op_arg + 1]);
                }
            } else {
                code.add(result, op_arg);
            }
        } else {
            code.bt(carry.cvt32(), 0);
            code.adc(result, op_arg);
        }
    } else {
        OpArg op_arg = ctx.reg_alloc.UseOpArg(code, args[1]);
        op_arg.setBit(bitsize);
        if (carry_in.IsImmediate()) {
            if (carry_in.GetImmediateU1()) {
                code.stc();
                code.adc(result, *op_arg);
            } else {
                code.add(result, *op_arg);
            }
        } else {
            code.bt(carry.cvt32(), 0);
            code.adc(result, *op_arg);
        }
    }

    if (inst == ctx.host_flags_inst) {
        // Only plain ADDS (carry in 0) is chosen, emitted as ADD: CF is ARM C.
        ctx.host_flags_carry_inverted = false;
    }
    if (nzcv_inst) {
        code.lahf();
        code.seto(code.al);
        ctx.reg_alloc.DefineValue(code, nzcv_inst, nzcv);
    }
    if (carry_inst) {
        code.setc(carry);
        ctx.reg_alloc.DefineValue(code, carry_inst, carry);
    }
    if (overflow_inst) {
        code.seto(overflow);
        ctx.reg_alloc.DefineValue(code, overflow_inst, overflow);
    }
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitAdd32(EmitContext& ctx, IR::Inst* inst) {
    EmitAdd(code, ctx, inst, 32);
}

void EmitX64::EmitAdd64(EmitContext& ctx, IR::Inst* inst) {
    EmitAdd(code, ctx, inst, 64);
}

static void EmitSub(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst, int bitsize) {
    const auto carry_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp);
    const auto overflow_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetOverflowFromOp);
    const auto nzcv_inst = inst->GetAssociatedPseudoOperation(IR::Opcode::GetNZCVFromOp);

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto& carry_in = args[2];
    const bool is_cmp = inst->UseCount() == size_t(!!carry_inst + !!overflow_inst + !!nzcv_inst) && carry_in.IsImmediate() && carry_in.GetImmediateU1();

    // Consider using LEA (not when a terminal reads this result's FLAGS).
    if (!carry_inst && !overflow_inst && !nzcv_inst && carry_in.IsImmediate() && carry_in.GetImmediateU1() && args[1].IsImmediate() && args[1].FitsInImmediateS32() && args[1].GetImmediateS32() != 0xffff'ffff'8000'0000 && inst != ctx.host_flags_inst) {
        // Venture: as EmitAdd, 64-bit addressing, and in place when the operand dies here.
        const bool in_place = ctx.reg_alloc.IsLastUse(args[0]);
        const Xbyak::Reg64 op1 = in_place ? ctx.reg_alloc.UseScratchGpr(code, args[0]) : ctx.reg_alloc.UseGpr(code, args[0]);
        const Xbyak::Reg64 result = in_place ? op1 : ctx.reg_alloc.ScratchGpr(code);

        code.lea(result.changeBit(bitsize), code.ptr[op1 - args[1].GetImmediateS32()]);

        ctx.reg_alloc.DefineValue(code, inst, result);
        return;
    }

    const Xbyak::Reg64 nzcv = DoNZCV(code, ctx.reg_alloc, nzcv_inst);
    const Xbyak::Reg result = (is_cmp ? ctx.reg_alloc.UseGpr(code, args[0]) : ctx.reg_alloc.UseScratchGpr(code, args[0])).changeBit(bitsize);
    const Xbyak::Reg8 carry = DoCarry(code, ctx.reg_alloc, carry_in, carry_inst);
    const Xbyak::Reg8 overflow = overflow_inst ? ctx.reg_alloc.ScratchGpr(code).cvt8() : Xbyak::Reg8{-1};

    // Note that x64 CF is inverse of what the ARM carry flag is here.

    bool invert_output_carry = true;

    if (is_cmp) {
        if (args[1].IsImmediate() && args[1].GetType() == IR::Type::U32) {
            const u32 op_arg = args[1].GetImmediateU32();
            code.cmp(result, op_arg);
        } else {
            OpArg op_arg = ctx.reg_alloc.UseOpArg(code, args[1]);
            op_arg.setBit(bitsize);
            code.cmp(result, *op_arg);
        }
    } else if (args[1].IsImmediate() && args[1].GetType() == IR::Type::U32) {
        const u32 op_arg = args[1].GetImmediateU32();
        if (carry_in.IsImmediate()) {
            if (carry_in.GetImmediateU1()) {
                code.sub(result, op_arg);
            } else {
                code.add(result, ~op_arg);
                invert_output_carry = false;
            }
        } else {
            code.bt(carry.cvt32(), 0);
            code.adc(result, ~op_arg);
            invert_output_carry = false;
        }
    } else {
        OpArg op_arg = ctx.reg_alloc.UseOpArg(code, args[1]);
        op_arg.setBit(bitsize);
        if (carry_in.IsImmediate()) {
            if (carry_in.GetImmediateU1()) {
                code.sub(result, *op_arg);
            } else {
                code.stc();
                code.sbb(result, *op_arg);
            }
        } else {
            code.bt(carry.cvt32(), 0);
            code.cmc();
            code.sbb(result, *op_arg);
        }
    }

    if (inst == ctx.host_flags_inst) {
        ctx.host_flags_carry_inverted = invert_output_carry;
    }
    if (nzcv_inst) {
        if (invert_output_carry) {
            code.cmc();
        }
        code.lahf();
        code.seto(code.al);
        ctx.reg_alloc.DefineValue(code, nzcv_inst, nzcv);
    }
    if (carry_inst) {
        if (invert_output_carry) {
            code.setnc(carry);
        } else {
            code.setc(carry);
        }
        ctx.reg_alloc.DefineValue(code, carry_inst, carry);
    }
    if (overflow_inst) {
        code.seto(overflow);
        ctx.reg_alloc.DefineValue(code, overflow_inst, overflow);
    }
    if (!is_cmp) {
        ctx.reg_alloc.DefineValue(code, inst, result);
    }
}

void EmitX64::EmitSub32(EmitContext& ctx, IR::Inst* inst) {
    EmitSub(code, ctx, inst, 32);
}

void EmitX64::EmitSub64(EmitContext& ctx, IR::Inst* inst) {
    EmitSub(code, ctx, inst, 64);
}

void EmitX64::EmitMul32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, args[0]).cvt32();
    if (args[1].IsImmediate()) {
        code.imul(result, result, args[1].GetImmediateU32());
    } else {
        OpArg op_arg = ctx.reg_alloc.UseOpArg(code, args[1]);
        op_arg.setBit(32);

        code.imul(result, *op_arg);
    }
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitMul64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, args[0]);
    OpArg op_arg = ctx.reg_alloc.UseOpArg(code, args[1]);

    code.imul(result, *op_arg);

    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitUnsignedMultiplyHigh64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    ctx.reg_alloc.ScratchGpr(code, HostLoc::RDX);
    ctx.reg_alloc.UseScratch(code, args[0], HostLoc::RAX);
    OpArg op_arg = ctx.reg_alloc.UseOpArg(code, args[1]);
    code.mul(*op_arg);

    ctx.reg_alloc.DefineValue(code, inst, rdx);
}

void EmitX64::EmitSignedMultiplyHigh64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    ctx.reg_alloc.ScratchGpr(code, HostLoc::RDX);
    ctx.reg_alloc.UseScratch(code, args[0], HostLoc::RAX);
    OpArg op_arg = ctx.reg_alloc.UseOpArg(code, args[1]);
    code.imul(*op_arg);

    ctx.reg_alloc.DefineValue(code, inst, rdx);
}

void EmitX64::EmitUnsignedDiv32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
    ctx.reg_alloc.ScratchGpr(code, HostLoc::RDX);
    const Xbyak::Reg32 dividend = ctx.reg_alloc.UseGpr(code, args[0]).cvt32();
    const Xbyak::Reg32 divisor = ctx.reg_alloc.UseGpr(code, args[1]).cvt32();

    Xbyak::Label end;

    code.xor_(eax, eax);
    code.test(divisor, divisor);
    code.jz(end, code.T_NEAR);
    code.mov(eax, dividend);
    code.xor_(edx, edx);
    code.div(divisor);
    code.L(end);

    ctx.reg_alloc.DefineValue(code, inst, eax);
}

void EmitX64::EmitUnsignedDiv64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
    ctx.reg_alloc.ScratchGpr(code, HostLoc::RDX);
    const Xbyak::Reg64 dividend = ctx.reg_alloc.UseGpr(code, args[0]);
    const Xbyak::Reg64 divisor = ctx.reg_alloc.UseGpr(code, args[1]);

    Xbyak::Label end;

    code.xor_(eax, eax);
    code.test(divisor, divisor);
    code.jz(end, code.T_NEAR);
    code.mov(rax, dividend);
    code.xor_(edx, edx);
    code.div(divisor);
    code.L(end);

    ctx.reg_alloc.DefineValue(code, inst, rax);
}

void EmitX64::EmitSignedDiv32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
    ctx.reg_alloc.ScratchGpr(code, HostLoc::RDX);
    auto const dividend = ctx.reg_alloc.UseGpr(code, args[0]).cvt32();
    auto const divisor = ctx.reg_alloc.UseGpr(code, args[1]).cvt32();
    Xbyak::Label end, ok;
    code.xor_(eax, eax);
    code.test(divisor, divisor);
    code.jz(end, code.T_NEAR); // eax = 0, if divisor == 0
    code.mov(eax, dividend);
    code.cmp(divisor, u32(-1)); // is sign extended
    code.jne(ok, code.T_NEAR);
    code.cmp(eax, u32(1ULL << 31));
    code.je(end, code.T_NEAR);
    code.L(ok);
    code.cdq();
    code.idiv(divisor);
    code.L(end);
    ctx.reg_alloc.DefineValue(code, inst, eax);
}

void EmitX64::EmitSignedDiv64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
    ctx.reg_alloc.ScratchGpr(code, HostLoc::RDX);
    auto const dividend = ctx.reg_alloc.UseGpr(code, args[0]);
    auto const divisor = ctx.reg_alloc.UseGpr(code, args[1]);
    Xbyak::Label end, ok;
    code.xor_(eax, eax);
    code.test(divisor, divisor);
    code.jz(end, code.T_NEAR); // rax = 0, if divisor == 0
    code.mov(rdx, u64(-1));
    code.cmp(divisor, rdx); // is sign extended
    code.jne(ok, code.T_NEAR);
    code.mov(rax, u64(1ULL << 63));
    code.cmp(dividend, rax);
    code.je(end, code.T_NEAR); // rax = 0x8000_0000 if dividend is same
    code.L(ok);
    code.mov(rax, dividend);
    code.cqo();
    code.idiv(divisor);
    code.L(end);
    ctx.reg_alloc.DefineValue(code, inst, rax);
}

void EmitX64::EmitAnd32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    if (args[1].IsImmediate() && !args[0].IsImmediate() && !inst->HasAssociatedPseudoOperation() && !ctx.reg_alloc.IsLastUse(args[0])) {
        // Venture: as And64, a zero-extending mask reads its source in place.
        const u32 mask = args[1].GetImmediateU32();
        if (mask == 0xff || mask == 0xffff) {
            const Xbyak::Reg64 source = ctx.reg_alloc.UseGpr(code, args[0]);
            const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr(code).cvt32();
            if (mask == 0xff) {
                code.movzx(result, source.cvt8());
            } else {
                code.movzx(result, source.cvt16());
            }
            ctx.reg_alloc.DefineValue(code, inst, result);
            return;
        }
    }

    PreferDyingFirstOperand(ctx, args);
    const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, args[0]).cvt32();

    if (args[1].IsImmediate()) {
        const u32 op_arg = args[1].GetImmediateU32();

        code.and_(result, op_arg);
    } else {
        OpArg op_arg = ctx.reg_alloc.UseOpArg(code, args[1]);
        op_arg.setBit(32);

        code.and_(result, *op_arg);
    }

    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitAnd64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    if (args[1].IsImmediate() && !args[0].IsImmediate() && !inst->HasAssociatedPseudoOperation() && !ctx.reg_alloc.IsLastUse(args[0])) {
        // Venture: a still-live or register-cache source is read in place when
        // the mask is a zero-extending move.
        const u64 mask = args[1].GetImmediateU64();
        if (mask == 0xff || mask == 0xffff || mask == 0xffffffff) {
            const Xbyak::Reg64 source = ctx.reg_alloc.UseGpr(code, args[0]);
            const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code);
            if (mask == 0xff) {
                code.movzx(result.cvt32(), source.cvt8());
            } else if (mask == 0xffff) {
                code.movzx(result.cvt32(), source.cvt16());
            } else {
                code.mov(result.cvt32(), source.cvt32());
            }
            ctx.reg_alloc.DefineValue(code, inst, result);
            return;
        }
    }

    PreferDyingFirstOperand(ctx, args);
    const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, args[0]);

    if (args[1].IsImmediate() && args[1].GetImmediateU64() <= 0xffffffff && !inst->HasAssociatedPseudoOperation()) {
        // Venture: a mask within the low word is a 32-bit AND, which clears the
        // upper half; the all-ones low word is the zero-extending MOV.
        const u32 mask = u32(args[1].GetImmediateU64());
        if (mask == 0xffffffff) {
            code.mov(result.cvt32(), result.cvt32());
        } else {
            code.and_(result.cvt32(), mask);
        }
    } else if (args[1].FitsInImmediateS32()) {
        const u32 op_arg = u32(args[1].GetImmediateS32());

        code.and_(result, op_arg);
    } else {
        OpArg op_arg = ctx.reg_alloc.UseOpArg(code, args[1]);
        op_arg.setBit(64);

        code.and_(result, *op_arg);
    }

    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitAndNot32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    if (!args[0].IsImmediate() && !args[1].IsImmediate() && code.HasHostFeature(HostFeature::BMI1)) {
        Xbyak::Reg32 op_a = ctx.reg_alloc.UseGpr(code, args[0]).cvt32();
        Xbyak::Reg32 op_b = ctx.reg_alloc.UseGpr(code, args[1]).cvt32();
        Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr(code).cvt32();
        code.andn(result, op_b, op_a);
        ctx.reg_alloc.DefineValue(code, inst, result);
        return;
    }

    Xbyak::Reg32 result;
    if (args[1].IsImmediate()) {
        result = ctx.reg_alloc.ScratchGpr(code).cvt32();
        code.mov(result, u32(~args[1].GetImmediateU32()));
    } else {
        result = ctx.reg_alloc.UseScratchGpr(code, args[1]).cvt32();
        code.not_(result);
    }

    if (args[0].IsImmediate()) {
        const u32 op_arg = args[0].GetImmediateU32();
        code.and_(result, op_arg);
    } else {
        OpArg op_arg = ctx.reg_alloc.UseOpArg(code, args[0]);
        op_arg.setBit(32);
        code.and_(result, *op_arg);
    }

    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitAndNot64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    if (!args[0].IsImmediate() && !args[1].IsImmediate() && code.HasHostFeature(HostFeature::BMI1)) {
        Xbyak::Reg64 op_a = ctx.reg_alloc.UseGpr(code, args[0]);
        Xbyak::Reg64 op_b = ctx.reg_alloc.UseGpr(code, args[1]);
        Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code);
        code.andn(result, op_b, op_a);
        ctx.reg_alloc.DefineValue(code, inst, result);
        return;
    }

    Xbyak::Reg64 result;
    if (args[1].IsImmediate()) {
        result = ctx.reg_alloc.ScratchGpr(code);
        code.mov(result, ~args[1].GetImmediateU64());
    } else {
        result = ctx.reg_alloc.UseScratchGpr(code, args[1]);
        code.not_(result);
    }

    if (args[0].FitsInImmediateS32()) {
        const u32 op_arg = u32(args[0].GetImmediateS32());
        code.and_(result, op_arg);
    } else {
        OpArg op_arg = ctx.reg_alloc.UseOpArg(code, args[0]);
        op_arg.setBit(64);
        code.and_(result, *op_arg);
    }

    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitEor32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    PreferDyingFirstOperand(ctx, args);
    const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, args[0]).cvt32();

    if (args[1].IsImmediate()) {
        const u32 op_arg = args[1].GetImmediateU32();

        code.xor_(result, op_arg);
    } else {
        OpArg op_arg = ctx.reg_alloc.UseOpArg(code, args[1]);
        op_arg.setBit(32);

        code.xor_(result, *op_arg);
    }

    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitEor64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    PreferDyingFirstOperand(ctx, args);
    const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, args[0]);

    if (args[1].FitsInImmediateS32()) {
        const u32 op_arg = u32(args[1].GetImmediateS32());

        code.xor_(result, op_arg);
    } else {
        OpArg op_arg = ctx.reg_alloc.UseOpArg(code, args[1]);
        op_arg.setBit(64);

        code.xor_(result, *op_arg);
    }

    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitOr32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    PreferDyingFirstOperand(ctx, args);
    const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, args[0]).cvt32();

    if (args[1].IsImmediate()) {
        const u32 op_arg = args[1].GetImmediateU32();

        code.or_(result, op_arg);
    } else {
        OpArg op_arg = ctx.reg_alloc.UseOpArg(code, args[1]);
        op_arg.setBit(32);

        code.or_(result, *op_arg);
    }

    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitOr64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    PreferDyingFirstOperand(ctx, args);
    const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, args[0]);

    if (args[1].FitsInImmediateS32()) {
        const u32 op_arg = u32(args[1].GetImmediateS32());

        code.or_(result, op_arg);
    } else {
        OpArg op_arg = ctx.reg_alloc.UseOpArg(code, args[1]);
        op_arg.setBit(64);

        code.or_(result, *op_arg);
    }

    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitNot32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    Xbyak::Reg32 result;
    if (args[0].IsImmediate()) {
        result = ctx.reg_alloc.ScratchGpr(code).cvt32();
        code.mov(result, u32(~args[0].GetImmediateU32()));
    } else {
        result = ctx.reg_alloc.UseScratchGpr(code, args[0]).cvt32();
        code.not_(result);
    }
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitNot64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    Xbyak::Reg64 result;
    if (args[0].IsImmediate()) {
        result = ctx.reg_alloc.ScratchGpr(code);
        code.mov(result, ~args[0].GetImmediateU64());
    } else {
        result = ctx.reg_alloc.UseScratchGpr(code, args[0]);
        code.not_(result);
    }
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitSignExtendByteToWord(EmitContext& ctx, IR::Inst* inst) {
    EmitExtend(code, ctx, inst, [&](Xbyak::Reg64 result, Xbyak::Reg64 source) { code.movsx(result.cvt32(), source.cvt8()); });
}

void EmitX64::EmitSignExtendHalfToWord(EmitContext& ctx, IR::Inst* inst) {
    EmitExtend(code, ctx, inst, [&](Xbyak::Reg64 result, Xbyak::Reg64 source) { code.movsx(result.cvt32(), source.cvt16()); });
}

void EmitX64::EmitSignExtendByteToLong(EmitContext& ctx, IR::Inst* inst) {
    EmitExtend(code, ctx, inst, [&](Xbyak::Reg64 result, Xbyak::Reg64 source) { code.movsx(result.cvt64(), source.cvt8()); });
}

void EmitX64::EmitSignExtendHalfToLong(EmitContext& ctx, IR::Inst* inst) {
    EmitExtend(code, ctx, inst, [&](Xbyak::Reg64 result, Xbyak::Reg64 source) { code.movsx(result.cvt64(), source.cvt16()); });
}

void EmitX64::EmitSignExtendWordToLong(EmitContext& ctx, IR::Inst* inst) {
    EmitExtend(code, ctx, inst, [&](Xbyak::Reg64 result, Xbyak::Reg64 source) { code.movsxd(result.cvt64(), source.cvt32()); });
}

void EmitX64::EmitZeroExtendByteToWord(EmitContext& ctx, IR::Inst* inst) {
    EmitExtend(code, ctx, inst, [&](Xbyak::Reg64 result, Xbyak::Reg64 source) { code.movzx(result.cvt32(), source.cvt8()); });
}

void EmitX64::EmitZeroExtendHalfToWord(EmitContext& ctx, IR::Inst* inst) {
    EmitExtend(code, ctx, inst, [&](Xbyak::Reg64 result, Xbyak::Reg64 source) { code.movzx(result.cvt32(), source.cvt16()); });
}

void EmitX64::EmitZeroExtendByteToLong(EmitContext& ctx, IR::Inst* inst) {
    // x64 zeros upper 32 bits on a 32-bit move
    EmitZeroExtendByteToWord(ctx, inst);
}

void EmitX64::EmitZeroExtendHalfToLong(EmitContext& ctx, IR::Inst* inst) {
    // x64 zeros upper 32 bits on a 32-bit move
    EmitZeroExtendHalfToWord(ctx, inst);
}

void EmitX64::EmitZeroExtendWordToLong(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    if (!args[0].IsImmediate() && args[0].IsInXmm(ctx.reg_alloc) && IsZeroAboveScalar(code, ctx, inst->GetArg(0))) {
        // Venture: lanes 1-3 of the word's register are already clear.
        ctx.reg_alloc.DefineValue(code, inst, args[0]);
        return;
    }
    if (ctx.IsLaneAlias(inst->GetArg(0))) {
        // A VectorGetElement32 left its source in place: copy its lane to lane
        // 0 and zero lanes 1-3 in one INSERTPS.
        const u8 imm = u8((inst->GetArg(0).GetInst()->GetArg(1).GetU8() << 6) | 0b00001110);
        if (code.HasHostFeature(HostFeature::AVX) && !ctx.reg_alloc.IsLastUse(args[0])) {
            const Xbyak::Xmm source = ctx.reg_alloc.UseXmm(code, args[0]);
            const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm(code);
            code.vinsertps(result, source, source, imm);
            ctx.reg_alloc.DefineValue(code, inst, result);
            return;
        }
        const Xbyak::Xmm result = ctx.reg_alloc.UseScratchXmm(code, args[0]);
        code.insertps(result, result, imm);
        ctx.reg_alloc.DefineValue(code, inst, result);
        return;
    }
    if (ctx.HasOptimization(OptimizationFlag::LowLaneAliasing) && code.HasHostFeature(HostFeature::SSE41) && args[0].IsInXmm(ctx.reg_alloc)) {
        // Scalar FP results stay in the vector domain: copy lane 0, zero lanes 1-3.
        // With AVX a still-live source is read in place instead of copied first.
        if (code.HasHostFeature(HostFeature::AVX) && !ctx.reg_alloc.IsLastUse(args[0])) {
            const Xbyak::Xmm source = ctx.reg_alloc.UseXmm(code, args[0]);
            const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm(code);
            code.vinsertps(result, source, source, 0b00001110);
            ctx.reg_alloc.DefineValue(code, inst, result);
            return;
        }
        const Xbyak::Xmm result = ctx.reg_alloc.UseScratchXmm(code, args[0]);
        if (code.HasHostFeature(HostFeature::AVX)) {
            code.vinsertps(result, result, result, 0b00001110);
        } else {
            code.insertps(result, result, 0b00001110);
        }
        ctx.reg_alloc.DefineValue(code, inst, result);
        return;
    }
    if (!args[0].IsImmediate() && args[0].IsInGpr(ctx.reg_alloc)) {
        // A 32-bit load or GetW wrote a 32-bit register, which zeroed bits 32-63
        // (fastmem MOV, locked XADD, fallbacks and callbacks all zero-extend).
        // A GPR copy of it is zero-extended too: only spill slots of a 32-bit
        // value may hold stale upper bits, and moves of the shared location use
        // its widest value from here on.
        if (WritesZeroExtendedWord(inst->GetArg(0).GetInst())) {
            ctx.reg_alloc.DefineValue(code, inst, args[0]);
            return;
        }
        if (!ctx.reg_alloc.IsLastUse(args[0])) {
            // One move into a fresh register instead of a copy then an in-place MOV.
            const Xbyak::Reg32 source = ctx.reg_alloc.UseGpr(code, args[0]).cvt32();
            const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code);
            code.mov(result.cvt32(), source);
            ctx.reg_alloc.DefineValue(code, inst, result);
            return;
        }
    }
    const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, args[0]);
    code.mov(result.cvt32(), result.cvt32());  // x64 zeros upper 32 bits on a 32-bit move
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitZeroExtendLongToQuad(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    if (args[0].IsImmediate()) {
        // FP and vector immediates: load {value, 0} directly (no MOVQ after).
        const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm(code);
        if (const u64 imm = args[0].GetImmediateU64(); imm == 0) {
            code.xorps(result, result);
        } else {
            code.movaps(result, code.Const(xword, imm, 0));
        }
        ctx.reg_alloc.DefineValue(code, inst, result);
        return;
    }
    if (!args[0].IsImmediate() && args[0].IsInXmm(ctx.reg_alloc) && args[0].GetType() == IR::Type::U64 &&
        IsZeroAboveScalar(code, ctx, inst->GetArg(0))) {
        // Lanes 2-3 are already clear: a zero-extended word, a MOVQ load, an FP
        // result of such values (see IsZeroAboveScalar).
        ctx.reg_alloc.DefineValue(code, inst, args[0]);
        return;
    }
    if (args[0].IsInGpr(ctx.reg_alloc)) {
        const Xbyak::Reg64 source = ctx.reg_alloc.UseGpr(code, args[0]);
        const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm(code);
        code.movq(result, source);
        ctx.reg_alloc.DefineValue(code, inst, result);
    } else {
        const Xbyak::Xmm result = ctx.reg_alloc.UseScratchXmm(code, args[0]);
        code.movq(result, result);
        ctx.reg_alloc.DefineValue(code, inst, result);
    }
}

void EmitX64::EmitByteReverseWord(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg32 result = ctx.reg_alloc.UseScratchGpr(code, args[0]).cvt32();
    code.bswap(result);
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitByteReverseHalf(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg16 result = ctx.reg_alloc.UseScratchGpr(code, args[0]).cvt16();
    code.rol(result, 8);
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitByteReverseDual(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg64 result = ctx.reg_alloc.UseScratchGpr(code, args[0]);
    code.bswap(result);
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void EmitX64::EmitCountLeadingZeros32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    if (code.HasHostFeature(HostFeature::LZCNT)) {
        const Xbyak::Reg32 source = ctx.reg_alloc.UseGpr(code, args[0]).cvt32();
        const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr(code).cvt32();

        code.lzcnt(result, source);

        ctx.reg_alloc.DefineValue(code, inst, result);
    } else {
        const Xbyak::Reg32 source = ctx.reg_alloc.UseScratchGpr(code, args[0]).cvt32();
        const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr(code).cvt32();
        const Xbyak::Reg32 temp = ctx.reg_alloc.ScratchGpr(code).cvt32();

        // The result of a bsr of zero is undefined, but zf is set after it.
        code.bsr(result, source);
        code.mov(temp, 32);
        code.xor_(result, 31);
        code.test(source, source);
        code.cmove(result, temp);
        ctx.reg_alloc.DefineValue(code, inst, result);
    }
}

void EmitX64::EmitCountLeadingZeros64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    if (code.HasHostFeature(HostFeature::LZCNT)) {
        const Xbyak::Reg64 source = ctx.reg_alloc.UseGpr(code, args[0]).cvt64();
        const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code).cvt64();

        code.lzcnt(result, source);

        ctx.reg_alloc.DefineValue(code, inst, result);
    } else {
        const Xbyak::Reg64 source = ctx.reg_alloc.UseScratchGpr(code, args[0]).cvt64();
        const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code).cvt64();
        const Xbyak::Reg64 temp = ctx.reg_alloc.ScratchGpr(code).cvt64();

        // The result of a bsr of zero is undefined, but zf is set after it.
        code.bsr(result, source);
        code.mov(temp.cvt32(), 64);
        code.xor_(result.cvt32(), 63);
        code.test(source, source);
        code.cmove(result.cvt32(), temp.cvt32());
        ctx.reg_alloc.DefineValue(code, inst, result);
    }
}

void EmitX64::EmitMaxSigned32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Reg32 x = ctx.reg_alloc.UseGpr(code, args[0]).cvt32();
    const Xbyak::Reg32 y = ctx.reg_alloc.UseScratchGpr(code, args[1]).cvt32();

    code.cmp(x, y);
    code.cmovge(y, x);

    ctx.reg_alloc.DefineValue(code, inst, y);
}

void EmitX64::EmitMaxSigned64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Reg64 x = ctx.reg_alloc.UseGpr(code, args[0]);
    const Xbyak::Reg64 y = ctx.reg_alloc.UseScratchGpr(code, args[1]);

    code.cmp(x, y);
    code.cmovge(y, x);

    ctx.reg_alloc.DefineValue(code, inst, y);
}

void EmitX64::EmitMaxUnsigned32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Reg32 x = ctx.reg_alloc.UseGpr(code, args[0]).cvt32();
    const Xbyak::Reg32 y = ctx.reg_alloc.UseScratchGpr(code, args[1]).cvt32();

    code.cmp(x, y);
    code.cmova(y, x);

    ctx.reg_alloc.DefineValue(code, inst, y);
}

void EmitX64::EmitMaxUnsigned64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Reg64 x = ctx.reg_alloc.UseGpr(code, args[0]);
    const Xbyak::Reg64 y = ctx.reg_alloc.UseScratchGpr(code, args[1]);

    code.cmp(x, y);
    code.cmova(y, x);

    ctx.reg_alloc.DefineValue(code, inst, y);
}

void EmitX64::EmitMinSigned32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Reg32 x = ctx.reg_alloc.UseGpr(code, args[0]).cvt32();
    const Xbyak::Reg32 y = ctx.reg_alloc.UseScratchGpr(code, args[1]).cvt32();

    code.cmp(x, y);
    code.cmovle(y, x);

    ctx.reg_alloc.DefineValue(code, inst, y);
}

void EmitX64::EmitMinSigned64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Reg64 x = ctx.reg_alloc.UseGpr(code, args[0]);
    const Xbyak::Reg64 y = ctx.reg_alloc.UseScratchGpr(code, args[1]);

    code.cmp(x, y);
    code.cmovle(y, x);

    ctx.reg_alloc.DefineValue(code, inst, y);
}

void EmitX64::EmitMinUnsigned32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Reg32 x = ctx.reg_alloc.UseGpr(code, args[0]).cvt32();
    const Xbyak::Reg32 y = ctx.reg_alloc.UseScratchGpr(code, args[1]).cvt32();

    code.cmp(x, y);
    code.cmovb(y, x);

    ctx.reg_alloc.DefineValue(code, inst, y);
}

void EmitX64::EmitMinUnsigned64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Reg64 x = ctx.reg_alloc.UseGpr(code, args[0]);
    const Xbyak::Reg64 y = ctx.reg_alloc.UseScratchGpr(code, args[1]);

    code.cmp(x, y);
    code.cmovb(y, x);

    ctx.reg_alloc.DefineValue(code, inst, y);
}

}  // namespace Dynarmic::Backend::X64
