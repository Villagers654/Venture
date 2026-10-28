// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/frontend/A64/translate/impl/impl.h"

namespace Dynarmic::A64 {

bool TranslatorVisitor::B_cond(Imm<19> imm19, Cond cond) {
    const s64 offset = concatenate(imm19, Imm<2>{0}).SignExtend<s64>();
    const u64 target = ir.PC() + offset;

    const auto cond_pass = IR::Term::LinkBlock{ir.current_location->SetPC(target)};
    const auto cond_fail = IR::Term::LinkBlock{ir.current_location->AdvancePC(4)};
    ir.SetTerm(IR::Term::If{cond, cond_pass, cond_fail});
    return false;
}

bool TranslatorVisitor::B_uncond(Imm<26> imm26) {
    const s64 offset = concatenate(imm26, Imm<2>{0}).SignExtend<s64>();
    const u64 target = ir.PC() + offset;
    // Pattern to halt execution (B .)
    if (target == ir.PC()) {
        ir.SetTerm(IR::Term::LinkBlock{ir.current_location->SetPC(target)});
        return false;
    }
    ir.SetTerm(IR::Term::LinkBlockFast{ir.current_location->SetPC(target)});
    return false;
}

std::optional<std::pair<u64, u32>> TranslatorVisitor::OutlinedAtomic(u64 target) const {
    // compiler-rt lse.S: [BTI c] ADRP x16; LDRB w16, [x16, #lo12]; CBZ w16, +12;
    // <LSE instruction>; RET; then the LL/SC fallback, which starts by copying
    // the first argument into x16/w16 (MOV or UXTB/UXTH). The fallback is an
    // equivalent implementation, so the LSE instruction is the helper.
    const auto read = [this](u64 vaddr) { return (*read_code)(vaddr); };
    u64 pc = target;
    auto word = read(pc);
    // BTI c, or the NOP a code reader may present it as (both are HINTs).
    if (word == std::optional<u32>{0xd503245f} || word == std::optional<u32>{0xd503201f}) {
        pc += 4;
        word = read(pc);
    }
    if (!word || (*word & 0x9f00001f) != 0x90000010)
        return std::nullopt;
    const auto load = read(pc + 4);
    if (!load || (*load & 0xffc003ff) != 0x39400210 || read(pc + 8) != std::optional<u32>{0x34000070})
        return std::nullopt;
    const auto lse = read(pc + 12);
    const auto fallback = read(pc + 20);
    if (!lse || read(pc + 16) != std::optional<u32>{0xd65f03c0} || !fallback ||
        (*fallback != 0xaa0003f0 && *fallback != 0x2a0003f0 && *fallback != 0x53001c10 && *fallback != 0x53003c10))
        return std::nullopt;
    // CAS{,A,L,AL}{B,H,,} s0, s1, [x2]
    const bool cas = (*lse & 0x3fa07c00) == 0x08a07c00 && (*lse & 0x001f03ff) == 0x041;
    // LD{ADD,CLR,EOR,SET}{,A,L,AL}{B,H,,} and SWP{,A,L,AL}{B,H,,} s0, s0, [x1]
    const u32 op = (*lse >> 12) & 0xf;
    const bool rmw = (*lse & 0x3f200c00) == 0x38200000 && (*lse & 0x001f03ff) == 0x020 && (op <= 3 || op == 8);
    if (!cas && !rmw)
        return std::nullopt;
    return std::pair{pc + 12, *lse};
}

bool TranslatorVisitor::BL(Imm<26> imm26) {
    const s64 offset = concatenate(imm26, Imm<2>{0}).SignExtend<s64>();

    if (options.inline_outlined_atomics && read_code) {
        if (const auto helper = OutlinedAtomic(ir.PC() + offset)) {
            // Venture: the helper's LSE instruction runs here, at its own address
            // (a fault reports it with X30 as the BL left it), then the block
            // continues at the return address.
            X(64, Reg::R30, ir.Imm64(ir.PC() + 4));
            const auto call_site = *ir.current_location;
            ir.current_location = call_site.SetPC(helper->first);
            const auto decoded = Decode<TranslatorVisitor, bool>(*this, helper->second);
            ir.current_location = call_site;
            // Through the fallback's first instruction, which recognition read.
            if (options.inlined_code)
                options.inlined_code->emplace_back(ir.PC() + offset, helper->first + 12);
            return decoded && *decoded;
        }
    }

    X(64, Reg::R30, ir.Imm64(ir.PC() + 4));
    ir.PushRSB(ir.current_location->AdvancePC(4));

    const u64 target = ir.PC() + offset;
    ir.SetTerm(IR::Term::LinkBlockFast{ir.current_location->SetPC(target)});
    return false;
}

bool TranslatorVisitor::BLR(Reg Rn) {
    const auto target = X(64, Rn);

    X(64, Reg::R30, ir.Imm64(ir.PC() + 4));
    ir.PushRSB(ir.current_location->AdvancePC(4));

    ir.SetPC(target);
    ir.SetTerm(IR::Term::FastDispatchHint{});
    return false;
}

bool TranslatorVisitor::BR(Reg Rn) {
    const auto target = X(64, Rn);

    ir.SetPC(target);
    ir.SetTerm(IR::Term::FastDispatchHint{});
    return false;
}

bool TranslatorVisitor::RET(Reg Rn) {
    const auto target = X(64, Rn);

    ir.SetPC(target);
    ir.SetTerm(IR::Term::PopRSBHint{});
    return false;
}

bool TranslatorVisitor::CBZ(bool sf, Imm<19> imm19, Reg Rt) {
    const size_t datasize = sf ? 64 : 32;
    const s64 offset = concatenate(imm19, Imm<2>{0}).SignExtend<s64>();

    const IR::U32U64 operand1 = X(datasize, Rt);

    ir.SetCheckBit(ir.IsZero(operand1));

    const u64 target = ir.PC() + offset;
    const auto cond_pass = IR::Term::LinkBlock{ir.current_location->SetPC(target)};
    const auto cond_fail = IR::Term::LinkBlock{ir.current_location->AdvancePC(4)};
    ir.SetTerm(IR::Term::CheckBit{cond_pass, cond_fail});
    return false;
}

bool TranslatorVisitor::CBNZ(bool sf, Imm<19> imm19, Reg Rt) {
    const size_t datasize = sf ? 64 : 32;
    const s64 offset = concatenate(imm19, Imm<2>{0}).SignExtend<s64>();

    const IR::U32U64 operand1 = X(datasize, Rt);

    ir.SetCheckBit(ir.IsZero(operand1));

    const u64 target = ir.PC() + offset;
    const auto cond_pass = IR::Term::LinkBlock{ir.current_location->AdvancePC(4)};
    const auto cond_fail = IR::Term::LinkBlock{ir.current_location->SetPC(target)};
    ir.SetTerm(IR::Term::CheckBit{cond_pass, cond_fail});
    return false;
}

bool TranslatorVisitor::TBZ(Imm<1> b5, Imm<5> b40, Imm<14> imm14, Reg Rt) {
    const size_t datasize = b5 == 1 ? 64 : 32;
    const u8 bit_pos = concatenate(b5, b40).ZeroExtend<u8>();
    const s64 offset = concatenate(imm14, Imm<2>{0}).SignExtend<s64>();

    const auto operand = X(datasize, Rt);

    ir.SetCheckBit(ir.TestBit(operand, ir.Imm8(bit_pos)));

    const u64 target = ir.PC() + offset;
    const auto cond_1 = IR::Term::LinkBlock{ir.current_location->AdvancePC(4)};
    const auto cond_0 = IR::Term::LinkBlock{ir.current_location->SetPC(target)};
    ir.SetTerm(IR::Term::CheckBit{cond_1, cond_0});
    return false;
}

bool TranslatorVisitor::TBNZ(Imm<1> b5, Imm<5> b40, Imm<14> imm14, Reg Rt) {
    const size_t datasize = b5 == 1 ? 64 : 32;
    const u8 bit_pos = concatenate(b5, b40).ZeroExtend<u8>();
    const s64 offset = concatenate(imm14, Imm<2>{0}).SignExtend<s64>();

    const auto operand = X(datasize, Rt);

    ir.SetCheckBit(ir.TestBit(operand, ir.Imm8(bit_pos)));

    const u64 target = ir.PC() + offset;
    const auto cond_1 = IR::Term::LinkBlock{ir.current_location->SetPC(target)};
    const auto cond_0 = IR::Term::LinkBlock{ir.current_location->AdvancePC(4)};
    ir.SetTerm(IR::Term::CheckBit{cond_1, cond_0});
    return false;
}

}  // namespace Dynarmic::A64
