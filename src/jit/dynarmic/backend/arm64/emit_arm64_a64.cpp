// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <algorithm>
#include <bit>
#include <utility>
#include <variant>
#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/arm64/a64_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/emit_context.h"
#include "dynarmic/backend/arm64/fpsr_manager.h"
#include "dynarmic/backend/arm64/reg_alloc.h"
#include "dynarmic/backend/arm64/stack_layout.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/halt_reason.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Arm64 {

using namespace oaknut::util;

oaknut::Label EmitA64Cond(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Cond cond) {
    oaknut::Label pass;
    // Venture: host NZCV may still hold the guest NZCV (the emit loop).
    if (!ctx.terminal_nzcv_in_flags) {
        code.LDR(Wscratch0, Xstate, offsetof(A64JitState, cpsr_nzcv));
        code.MSR(oaknut::SystemReg::NZCV, Xscratch0);
    }
    code.B(static_cast<oaknut::Cond>(cond), pass);
    return pass;
}

namespace {

enum class NZCVUse { None, Overwrite, Other };

// Venture: an A64 instruction's effect on a later NZCV read, as on x64: None
// (no flag access, falls through), Overwrite (writes N, Z, C and V without
// reading them) or Other (reads flags, transfers control, or is a system or
// unrecognised encoding).
NZCVUse ClassifyNZCV(u32 i) {
    const bool sf = (i >> 31) & 1;
    const bool sets_flags = (i >> 29) & 1;
    if ((i & 0x1F800000) == 0x11000000) {  // add/subtract (immediate)
        return sets_flags ? NZCVUse::Overwrite : NZCVUse::None;
    }
    if ((i & 0x1F200000) == 0x0B000000) {  // add/subtract (shifted register)
        if (((i >> 22) & 3) == 3 || (!sf && (i & 0x8000)))
            return NZCVUse::Other;
        return sets_flags ? NZCVUse::Overwrite : NZCVUse::None;
    }
    if ((i & 0x1F200000) == 0x0B200000) {  // add/subtract (extended register)
        if (((i >> 22) & 3) != 0 || ((i >> 10) & 7) > 4)
            return NZCVUse::Other;
        return sets_flags ? NZCVUse::Overwrite : NZCVUse::None;
    }
    if ((i & 0x1F000000) == 0x0A000000) {  // logical (shifted register): ANDS, BICS set flags
        if (!sf && (i & 0x8000))
            return NZCVUse::Other;
        return ((i >> 29) & 3) == 3 ? NZCVUse::Overwrite : NZCVUse::None;
    }
    if ((i & 0x1F800000) == 0x12000000) {  // logical (immediate): ANDS sets flags
        const u32 n = (i >> 22) & 1;
        const u32 imms = (i >> 10) & 0x3F;
        const u32 combined = n << 6 | (~imms & 0x3F);
        if ((!sf && n) || combined < 2)
            return NZCVUse::Other;
        const u32 levels = (u32(1) << (31 - std::countl_zero(combined))) - 1;
        if ((imms & levels) == levels)
            return NZCVUse::Other;
        return ((i >> 29) & 3) == 3 ? NZCVUse::Overwrite : NZCVUse::None;
    }
    if ((i & 0x1F800000) == 0x12800000) {  // move wide
        return ((i >> 29) & 3) == 1 || (!sf && (i & 0x400000)) ? NZCVUse::Other : NZCVUse::None;
    }
    if ((i & 0x1F000000) == 0x10000000) {  // ADR, ADRP
        return NZCVUse::None;
    }
    if ((i & 0x1F800000) == 0x13000000) {  // bitfield
        const bool n = (i >> 22) & 1;
        return ((i >> 29) & 3) == 3 || n != sf || (!sf && (i & 0x208000)) ? NZCVUse::Other : NZCVUse::None;
    }
    if ((i & 0x1F800000) == 0x13800000) {  // extract
        const bool n = (i >> 22) & 1;
        return (i & 0x60200000) || n != sf || (!sf && (i & 0x8000)) ? NZCVUse::Other : NZCVUse::None;
    }
    if ((i & 0x7FE00000) == 0x1AC00000) {  // data processing (2 source): divides, variable shifts
        const u32 opcode = (i >> 10) & 0x3F;
        return opcode == 2 || opcode == 3 || (opcode >= 8 && opcode <= 11) ? NZCVUse::None : NZCVUse::Other;
    }
    if ((i & 0x7FE00000) == 0x5AC00000) {  // data processing (1 source): RBIT, REV*, CLZ, CLS
        const u32 opcode = (i >> 10) & 0x3F;
        return ((i >> 16) & 0x1F) == 0 && opcode <= 5 && (sf || opcode != 3) ? NZCVUse::None : NZCVUse::Other;
    }
    if ((i & 0x1F000000) == 0x1B000000) {  // data processing (3 source): multiplies
        const u32 op31 = (i >> 21) & 7;
        const bool o0 = (i >> 15) & 1;
        if (((i >> 29) & 3) != 0)
            return NZCVUse::Other;
        if (op31 == 0)
            return NZCVUse::None;
        if (sf && (op31 == 1 || op31 == 5 || ((op31 == 2 || op31 == 6) && !o0)))
            return NZCVUse::None;
        return NZCVUse::Other;
    }
    if ((i & 0x3B200C00) == 0x19000400) {  // memory copy and set (FEAT_MOPS) pass state in NZCV
        return NZCVUse::Other;
    }
    if ((i & 0x0A000000) == 0x08000000) {  // loads and stores
        return NZCVUse::None;
    }
    if ((i & 0x0E000000) == 0x0E000000) {  // SIMD and floating point
        if ((i & 0xFF20FC07) == 0x1E202000)  // FCMP, FCMPE
            return ((i >> 22) & 3) == 2 ? NZCVUse::Other : NZCVUse::Overwrite;
        if ((i & 0xFF200C00) == 0x1E200400 || (i & 0xFF200C00) == 0x1E200C00)  // FCCMP(E), FCSEL
            return NZCVUse::Other;
        return NZCVUse::None;
    }
    if ((i & 0xFFFFF01F) == 0xD503201F) {  // hints
        return NZCVUse::None;
    }
    if ((i & 0xFFFFF01F) == 0xD503301F) {  // CLREX, DSB, DMB, ISB, SB
        const u32 op2 = (i >> 5) & 7;
        return op2 == 2 || op2 >= 4 ? NZCVUse::None : NZCVUse::Other;
    }
    return NZCVUse::Other;
}

// Whether every path from pc overwrites NZCV before any flag read or
// unrecognised encoding, within `budget` instructions in all; B, CBZ/CBNZ and
// TBZ/TBNZ are followed (both ways) up to `depth` branches deep. Every
// instruction relied on is added to `ranges` (as on x64).
bool NZCVDeadAt(const EmitConfig& conf, u64 pc, std::size_t& budget, std::size_t depth, std::vector<std::pair<u64, u64>>& ranges) {
    if (pc & 3)
        return false;
    for (const u64 begin = pc; budget; --budget, pc += 4) {
        const auto word = conf.read_code(pc);
        if (!word)
            return false;
        const u32 i = *word;
        switch (ClassifyNZCV(i)) {
        case NZCVUse::Overwrite:
            ranges.emplace_back(begin, pc + 3);
            return true;
        case NZCVUse::None:
            continue;
        case NZCVUse::Other:
            break;
        }
        std::optional<u64> target;
        bool falls_through = false;
        if ((i & 0xFC000000) == 0x14000000) {  // B
            target = pc + u64(s64(s32(i << 6) >> 6) * 4);
        } else if ((i & 0x7E000000) == 0x34000000) {  // CBZ, CBNZ
            target = pc + u64(s64(s32(i << 8) >> 13) * 4);
            falls_through = true;
        } else if ((i & 0x7E000000) == 0x36000000) {  // TBZ, TBNZ
            target = pc + u64(s64(s32(i << 13) >> 18) * 4);
            falls_through = true;
        }
        if (!target || depth == 0)
            return false;
        ranges.emplace_back(begin, pc + 3);
        --budget;
        return NZCVDeadAt(conf, *target, budget, depth - 1, ranges) && (!falls_through || NZCVDeadAt(conf, pc + 4, budget, depth - 1, ranges));
    }
    return false;
}

// The emit loop keeps host NZCV across these (register moves, loads and stores).
bool KeepsHostNZCV(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::Void:
    case IR::Opcode::A64GetW:
    case IR::Opcode::A64GetX:
    case IR::Opcode::A64SetW:
    case IR::Opcode::A64SetX:
        return true;
    default:
        return false;
    }
}

}  // namespace

// Venture: nzcv_exit_liveness, as on x64. An If terminal after an ADDS, SUBS,
// ANDS or BICS whose NZCV only the block's final A64SetNZCV stores, with only
// register moves after it: the terminal branches on host NZCV, and NZCV
// reaches the JIT state only on the exits whose successor may read it before
// overwriting it. The successor code scanned joins the block's range.
void PrepareA64NZCVExit(EmitContext& ctx) {
    const auto& conf = ctx.conf;
    ctx.deferred_nzcv_set = nullptr;
    ctx.exit_nzcv_store = {};
    IR::Block& block = ctx.block;
    if (!conf.nzcv_exit_liveness || !conf.read_code || conf.enable_cycle_counting || conf.very_verbose_debugging_output ||
        conf.diagnostic_record || A64::LocationDescriptor{block.Location()}.SingleStepping()) {
        return;
    }
    const auto block_terminal = block.GetTerminal();
    const auto* const terminal = std::get_if<IR::Term::If>(&block_terminal);
    if (!terminal || terminal->if_ == IR::Cond::AL || terminal->if_ == IR::Cond::NV) {
        return;
    }

    auto iter = block.instructions.end();
    IR::Inst* set_nzcv = nullptr;
    while (iter != block.instructions.begin()) {
        auto& inst = *--iter;
        if (inst.GetOpcode() == IR::Opcode::A64SetNZCV) {
            set_nzcv = &inst;
            break;
        }
        if (!KeepsHostNZCV(inst)) {
            return;
        }
    }
    if (!set_nzcv || set_nzcv->GetArg(0).IsImmediate()) {
        return;
    }
    const IR::Inst* const nzcv = set_nzcv->GetArg(0).GetInst();
    if (nzcv->GetOpcode() != IR::Opcode::GetNZCVFromOp || nzcv->UseCount() != 1) {
        return;
    }
    const IR::Inst* const producer = nzcv->GetArg(0).GetInst();
    switch (producer->GetOpcode()) {
    case IR::Opcode::Add32:
    case IR::Opcode::Add64:
    case IR::Opcode::Sub32:
    case IR::Opcode::Sub64:
    case IR::Opcode::And32:
    case IR::Opcode::And64:
    case IR::Opcode::AndNot32:
    case IR::Opcode::AndNot64:
        break;
    default:
        return;
    }
    while (true) {
        if (iter == block.instructions.begin()) {
            return;
        }
        auto& inst = *--iter;
        if (&inst == producer) {
            break;
        }
        if (&inst != nzcv && !KeepsHostNZCV(inst)) {
            return;
        }
    }

    const std::array<const IR::Term::LeafTerminal*, 2> exits{&terminal->else_, &terminal->then_};
    std::array<bool, 2> live{true, true};
    std::vector<std::pair<u64, u64>> dead_ranges;
    for (std::size_t i = 0; i < exits.size(); ++i) {
        std::optional<IR::LocationDescriptor> next;
        if (const auto* link = std::get_if<IR::Term::LinkBlock>(exits[i])) {
            next = link->next;
        } else if (const auto* fast = std::get_if<IR::Term::LinkBlockFast>(exits[i])) {
            next = fast->next;
        }
        if (!next) {
            continue;
        }
        std::vector<std::pair<u64, u64>> ranges;
        std::size_t budget = 24;
        if (NZCVDeadAt(conf, A64::LocationDescriptor{*next}.PC(), budget, 3, ranges)) {
            live[i] = false;
            dead_ranges.insert(dead_ranges.end(), ranges.begin(), ranges.end());
        }
    }
    if (live[0] && live[1]) {
        return;
    }
    ctx.deferred_nzcv_set = set_nzcv;
    ctx.exit_nzcv_store = live;
    ctx.ebi.dependent_ranges.insert(ctx.ebi.dependent_ranges.end(), dead_ranges.begin(), dead_ranges.end());
}

static void EmitExitNZCV(oaknut::CodeGenerator& code) {
    code.MRS(Xscratch0, oaknut::SystemReg::NZCV);
    code.STR(Wscratch0, Xstate, offsetof(A64JitState, cpsr_nzcv));
}

void EmitA64LeafTerminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::LeafTerminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step);
void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::Terminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step);

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::ReturnToDispatch, IR::LocationDescriptor, bool) {
    EmitRelocation(code, ctx, LinkTarget::ReturnToDispatcher);
}

void PrepareA64Terminal(EmitContext& ctx) {
    const auto& conf = ctx.conf;
    const IR::Block& block = ctx.block;
    const bool is_single_step = A64::LocationDescriptor{block.Location()}.SingleStepping();
    const auto terminal = block.GetTerminal();

    // Raw LinkBlock terminals (mode changes, callbacks and translation caps)
    // stay checkpoints: only ordinary conditional edges skip a poll (x64).
    ctx.elide_forward_halt_polls = conf.enable_bounded_halt_polling && !conf.enable_cycle_counting && !is_single_step &&
        (std::holds_alternative<IR::Term::If>(terminal) || std::holds_alternative<IR::Term::CheckBit>(terminal));

    ctx.static_fpcr_terminal = conf.next_dispatch_site && !conf.fast_dispatch_census &&
        std::none_of(block.instructions.begin(), block.instructions.end(), [](const IR::Inst& inst) {
            const auto op = inst.GetOpcode();
            return op == IR::Opcode::A64SetFPCR || op == IR::Opcode::A64CallSupervisor;
        });

    ctx.known_return.reset();
    if (conf.svc_return_link && conf.HasOptimization(OptimizationFlag::ReturnStackBuffer) &&
        conf.HasOptimization(OptimizationFlag::BlockLinking) && !is_single_step) {
        const auto halt = std::get_if<IR::Term::CheckHalt>(&terminal);
        const auto leaf = halt ? &halt->else_ : std::get_if<IR::Term::LeafTerminal>(&terminal);
        if (leaf && std::holds_alternative<IR::Term::PopRSBHint>(*leaf)) {
            std::size_t pushes = 0;
            for (const auto& inst : block.instructions) {
                if (inst.GetOpcode() == IR::Opcode::PushRSB) {
                    ++pushes;
                    ctx.known_return = IR::LocationDescriptor{inst.GetArg(0).GetU64()};
                }
            }
            if (pushes != 1) {
                ctx.known_return.reset();
            }
        }
    }
}

// Venture: counts a dispatch census event. Every caller-saved register is free
// at a terminal.
static void EmitCensusCount(oaknut::CodeGenerator& code, EmitContext& ctx, std::size_t index) {
    if (!ctx.conf.fast_dispatch_census) {
        return;
    }
    code.MOV(X3, std::bit_cast<u64>(ctx.conf.fast_dispatch_census + index));
    code.LDR(X4, X3);
    code.ADD(X4, X4, 1);
    code.STR(X4, X3);
}

// X0 = the location descriptor of the JIT state's PC and FPCR.
static void EmitCurrentLocation(oaknut::CodeGenerator& code) {
    code.MOV(Wscratch0, A64::LocationDescriptor::fpcr_mask);
    code.LDR(W0, Xstate, offsetof(A64JitState, fpcr));
    code.LDR(X1, Xstate, offsetof(A64JitState, pc));
    code.AND(W0, W0, Wscratch0);
    code.AND(X1, X1, A64::LocationDescriptor::pc_mask);
    code.LSL(X0, X0, A64::LocationDescriptor::fpcr_shift);
    code.ORR(X0, X0, X1);
}

// Venture: a compact link site (as for A32). The patchable branch stays in
// line; unlinked, it (and `exit`, for a halt or cycle check) jumps to a PC
// store and dispatcher return emitted with the block's deferred code.
static void EmitCompactBlockLink(oaknut::CodeGenerator& code, EmitContext& ctx, IR::LocationDescriptor next, SharedLabel exit) {
    const std::size_t index = ctx.ebi.block_relocations[next].size();
    EmitBlockLinkRelocation(code, ctx, next, BlockRelocationType::Branch);
    ctx.deferred_emits.emplace_back([&code, &ctx, next, index, exit] {
        code.l(*exit);
        ctx.ebi.block_relocations[next][index].unlinked_offset = code.xptr<CodePtr>() - ctx.ebi.entry_point;
        code.MOV(Xscratch0, A64::LocationDescriptor{next}.PC());
        code.STR(Xscratch0, Xstate, offsetof(A64JitState, pc));
        EmitRelocation(code, ctx, LinkTarget::ReturnToDispatcher);
    });
}

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::LinkBlockFast terminal, IR::LocationDescriptor initial_location, bool is_single_step);

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::LinkBlock terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    // Venture: bounded halt polling (as on x64). A forward conditional edge
    // within one window needs no poll: every cycle has a backward or
    // window-crossing edge, and those poll.
    const u64 from = A64::LocationDescriptor{initial_location}.PC();
    const u64 to = A64::LocationDescriptor{terminal.next}.PC();
    if (ctx.elide_forward_halt_polls && to > from && to / A64::halt_polling_window_size == from / A64::halt_polling_window_size) {
        return EmitA64Terminal(code, ctx, IR::Term::LinkBlockFast{terminal.next}, initial_location, is_single_step);
    }

    if (ctx.conf.compact_block_links && ctx.conf.HasOptimization(OptimizationFlag::BlockLinking) && !is_single_step) {
        const SharedLabel exit = GenSharedLabel();
        if (ctx.conf.enable_cycle_counting) {
            code.CMP(Xticks, 0);
            code.B(LE, *exit);
        } else {
            code.LDAR(Wscratch0, Xhalt);
            code.CBNZ(Wscratch0, *exit);
        }
        EmitCompactBlockLink(code, ctx, terminal.next, exit);
        return;
    }

    oaknut::Label fail;

    if (ctx.conf.HasOptimization(OptimizationFlag::BlockLinking) && !is_single_step) {
        if (ctx.conf.enable_cycle_counting) {
            code.CMP(Xticks, 0);
            code.B(LE, fail);
            EmitBlockLinkRelocation(code, ctx, terminal.next, BlockRelocationType::Branch);
        } else {
            code.LDAR(Wscratch0, Xhalt);
            code.CBNZ(Wscratch0, fail);
            EmitBlockLinkRelocation(code, ctx, terminal.next, BlockRelocationType::Branch);
        }
    }

    code.l(fail);
    code.MOV(Xscratch0, A64::LocationDescriptor{terminal.next}.PC());
    code.STR(Xscratch0, Xstate, offsetof(A64JitState, pc));
    EmitRelocation(code, ctx, LinkTarget::ReturnToDispatcher);
}

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::LinkBlockFast terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    // Every cycle of linked blocks has a backward edge. Keep forward links fast
    // and make backward and self links observe halt requests, as on x64. With
    // bounded halt polling, a link across a window polls too.
    const u64 from = A64::LocationDescriptor{initial_location}.PC();
    const u64 to = A64::LocationDescriptor{terminal.next}.PC();
    if (to <= from ||
        (ctx.conf.enable_bounded_halt_polling && !ctx.conf.enable_cycle_counting &&
         to / A64::halt_polling_window_size != from / A64::halt_polling_window_size)) {
        const bool elide = ctx.elide_forward_halt_polls;
        ctx.elide_forward_halt_polls = false;
        EmitA64Terminal(code, ctx, IR::Term::LinkBlock{terminal.next}, initial_location, is_single_step);
        ctx.elide_forward_halt_polls = elide;
        return;
    }

    if (ctx.conf.compact_block_links && ctx.conf.HasOptimization(OptimizationFlag::BlockLinking) && !is_single_step) {
        EmitCompactBlockLink(code, ctx, terminal.next, GenSharedLabel());
        return;
    }

    if (ctx.conf.HasOptimization(OptimizationFlag::BlockLinking) && !is_single_step) {
        EmitBlockLinkRelocation(code, ctx, terminal.next, BlockRelocationType::Branch);
    }

    code.MOV(Xscratch0, A64::LocationDescriptor{terminal.next}.PC());
    code.STR(Xscratch0, Xstate, offsetof(A64JitState, pc));
    EmitRelocation(code, ctx, LinkTarget::ReturnToDispatcher);
}

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::PopRSBHint, IR::LocationDescriptor initial_location, bool is_single_step) {
    if (ctx.known_return && !is_single_step) {
        // Venture: svc_return_link (as on x64). The block pushed no entry: its
        // exit links to the pushed location, and a supervisor call that
        // changed PC or FPCR takes the fast-dispatch lookup.
        oaknut::Label fail;
        EmitCurrentLocation(code);
        code.MOV(Xscratch0, ctx.known_return->Value());
        code.CMP(X0, Xscratch0);
        code.B(NE, fail);
        EmitA64Terminal(code, ctx, IR::Term::LinkBlockFast{*ctx.known_return}, initial_location, is_single_step);
        code.l(fail);
        EmitRelocation(code, ctx, ctx.conf.HasOptimization(OptimizationFlag::FastDispatch) ? LinkTarget::FastDispatch : LinkTarget::ReturnToDispatcher);
        return;
    }

    if (ctx.conf.HasOptimization(OptimizationFlag::ReturnStackBuffer) && !is_single_step) {
        oaknut::Label fail;

        EmitCensusCount(code, ctx, 0);
        EmitCurrentLocation(code);

        code.LDR(Wscratch2, SP, offsetof(StackLayout, rsb_ptr));
        code.AND(Wscratch2, Wscratch2, A64RSBIndexMask);
        code.ADD(X2, SP, Xscratch2);
        code.SUB(Wscratch2, Wscratch2, sizeof(RSBEntry));
        code.STR(Wscratch2, SP, offsetof(StackLayout, rsb_ptr));

        code.LDP(Xscratch0, Xscratch1, X2, offsetof(StackLayout, rsb));

        code.CMP(X0, Xscratch0);
        code.B(NE, fail);
        code.BR(Xscratch1);

        code.l(fail);
        if (ctx.conf.HasOptimization(OptimizationFlag::FastDispatch)) {
            // Venture: a miss takes the table lookup, as on x64.
            EmitCensusCount(code, ctx, 1);
            EmitRelocation(code, ctx, LinkTarget::FastDispatch);
            return;
        }
    }

    EmitRelocation(code, ctx, LinkTarget::ReturnToDispatcher);
}

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::FastDispatchHint, IR::LocationDescriptor initial_location, bool is_single_step) {
    if (!ctx.conf.HasOptimization(OptimizationFlag::FastDispatch) || is_single_step) {
        EmitRelocation(code, ctx, LinkTarget::ReturnToDispatcher);
        return;
    }

    if (ctx.static_fpcr_terminal && A64::LocationDescriptor{initial_location}.FPCR().Value() == 0) {
        // Venture: dispatch_site_cache (as on x64). This site's slot holds its
        // last target's PC and code; a mismatch refills it (X0 PC, X1 slot).
        // The block's FPCR is zero and unchanged, so the location is the PC.
        void* const slot = ctx.conf.next_dispatch_site();
        oaknut::Label run, match;
        if (ctx.conf.enable_cycle_counting) {
            oaknut::Label ticks_left;
            code.CMP(Xticks, 0);
            code.B(GT, ticks_left);
            EmitRelocation(code, ctx, LinkTarget::ReturnToDispatcher);
            code.l(ticks_left);
        }
        code.LDAR(W1, Xhalt);
        code.CBZ(W1, run);
        EmitRelocation(code, ctx, LinkTarget::ReturnToDispatcher);
        code.l(run);
        code.LDR(X0, Xstate, offsetof(A64JitState, pc));
        code.MOV(X1, std::bit_cast<u64>(slot));
        code.LDP(X2, X3, X1);
        code.CMP(X0, X2);
        code.B(EQ, match);
        EmitRelocation(code, ctx, LinkTarget::DispatchSiteMiss);
        code.l(match);
        code.BR(X3);
        return;
    }

    // Venture: the table lookup in the prelude (as on x64).
    EmitCensusCount(code, ctx, 2);
    EmitRelocation(code, ctx, LinkTarget::FastDispatch);
}

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::If terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    oaknut::Label pass = EmitA64Cond(code, ctx, terminal.if_);
    ctx.terminal_nzcv_in_flags = false;
    // nzcv_exit_liveness: NZCV is stored only where a successor may read it.
    const bool deferred = ctx.deferred_nzcv_set != nullptr;
    ctx.deferred_nzcv_set = nullptr;
    if (deferred && ctx.exit_nzcv_store[0]) {
        EmitExitNZCV(code);
    }
    EmitA64LeafTerminal(code, ctx, terminal.else_, initial_location, is_single_step);
    code.l(pass);
    if (deferred && ctx.exit_nzcv_store[1]) {
        EmitExitNZCV(code);
    }
    EmitA64LeafTerminal(code, ctx, terminal.then_, initial_location, is_single_step);
}

// Venture: as the x64 backend fuses the final boolean producer with CheckBit:
// a block ending in IsZero32, IsZero64 or TestBit whose only use is the
// A64SetCheckBit before its CheckBit terminal stores no check bit; the
// terminal branches on the operand (CBNZ/TBZ). Nothing allocates registers
// between the producer and the terminal, so the operand's register holds.
bool FuseA64CheckBit(EmitContext& ctx, IR::Inst* inst, IR::Inst* next, bool next_is_last) {
    const auto opcode = inst->GetOpcode();
    if ((opcode != IR::Opcode::IsZero32 && opcode != IR::Opcode::IsZero64 && opcode != IR::Opcode::TestBit) ||
        !next || !next_is_last || next->GetOpcode() != IR::Opcode::A64SetCheckBit || next->GetArg(0).IsImmediate() ||
        next->GetArg(0).GetInst() != inst || inst->UseCount() != 1 || inst->GetArg(0).IsImmediate() ||
        !std::holds_alternative<IR::Term::CheckBit>(ctx.block.GetTerminal())) {
        return false;
    }
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xoperand = ctx.reg_alloc.ReadX(args[0]);
    RegAlloc::Realize(Xoperand);
    ctx.fused_check_bit = EmitContext::FusedCheckBit{Xoperand->index(), opcode, opcode == IR::Opcode::TestBit ? args[1].GetImmediateU8() : u8(0)};
    next->Invalidate();
    return true;
}

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::CheckBit terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    oaknut::Label fail;
    if (const auto fused = std::exchange(ctx.fused_check_bit, std::nullopt)) {
        switch (fused->opcode) {
        case IR::Opcode::IsZero32:
            code.CBNZ(oaknut::WReg{fused->reg}, fail);
            break;
        case IR::Opcode::IsZero64:
            code.CBNZ(oaknut::XReg{fused->reg}, fail);
            break;
        default:
            code.TBZ(oaknut::XReg{fused->reg}, fused->bit, fail);
            break;
        }
    } else {
        code.LDRB(Wscratch0, SP, offsetof(StackLayout, check_bit));
        code.CBZ(Wscratch0, fail);
    }
    EmitA64LeafTerminal(code, ctx, terminal.then_, initial_location, is_single_step);
    code.l(fail);
    EmitA64LeafTerminal(code, ctx, terminal.else_, initial_location, is_single_step);
}

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::CheckHalt terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    oaknut::Label fail;
    code.LDAR(Wscratch0, Xhalt);
    code.CBNZ(Wscratch0, fail);
    EmitA64LeafTerminal(code, ctx, terminal.else_, initial_location, is_single_step);
    code.l(fail);
    EmitRelocation(code, ctx, LinkTarget::ReturnToDispatcher);
}

void EmitA64LeafTerminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::LeafTerminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    if (auto const x = std::get_if<IR::Term::ReturnToDispatch>(&terminal))
        return EmitA64Terminal(code, ctx, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::LinkBlock>(&terminal))
        return EmitA64Terminal(code, ctx, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::LinkBlockFast>(&terminal))
        return EmitA64Terminal(code, ctx, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::PopRSBHint>(&terminal))
        return EmitA64Terminal(code, ctx, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::FastDispatchHint>(&terminal))
        return EmitA64Terminal(code, ctx, *x, initial_location, is_single_step);
    UNREACHABLE();
}

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Term::Terminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    if (auto const x = std::get_if<IR::Term::LeafTerminal>(&terminal))
        return EmitA64LeafTerminal(code, ctx, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::If>(&terminal))
        return EmitA64Terminal(code, ctx, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::CheckBit>(&terminal))
        return EmitA64Terminal(code, ctx, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::CheckHalt>(&terminal))
        return EmitA64Terminal(code, ctx, *x, initial_location, is_single_step);
    UNREACHABLE();
}

void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx) {
    const A64::LocationDescriptor location{ctx.block.Location()};
    EmitA64Terminal(code, ctx, ctx.block.GetTerminal(), location.SetSingleStepping(false), location.SingleStepping());
}

void EmitA64ConditionFailedTerminal(oaknut::CodeGenerator& code, EmitContext& ctx) {
    const A64::LocationDescriptor location{ctx.block.Location()};
    EmitA64Terminal(code, ctx, IR::Term::LinkBlock{ctx.block.ConditionFailedLocation()}, location.SetSingleStepping(false), location.SingleStepping());
}

// Venture: stores an access's A64ColdSet* records (cold abort state) from
// where each value was at the access. The exit leaves the run, so the scratch
// registers are free.
static void EmitColdStateStores(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* access) {
    const auto found = ctx.cold_state.find(access);
    if (found == ctx.cold_state.end()) {
        return;
    }
    using Op = IR::Opcode;
    constexpr std::size_t spill_offset = offsetof(StackLayout, spill);
    constexpr std::size_t spill_slot_size = sizeof(decltype(StackLayout::spill)::value_type);
    for (const auto& store : found->second) {
        std::size_t offset = 0, bytes = 8;
        switch (store.opcode) {
        case Op::A64ColdSetW:
            offset = offsetof(A64JitState, reg) + sizeof(u64) * store.index;
            bytes = 4;
            break;
        case Op::A64ColdSetX:
            offset = offsetof(A64JitState, reg) + sizeof(u64) * store.index;
            break;
        case Op::A64ColdSetS:
            offset = offsetof(A64JitState, vec) + 2 * sizeof(u64) * store.index;
            bytes = 4;
            break;
        case Op::A64ColdSetD:
            offset = offsetof(A64JitState, vec) + 2 * sizeof(u64) * store.index;
            break;
        case Op::A64ColdSetQ:
            offset = offsetof(A64JitState, vec) + 2 * sizeof(u64) * store.index;
            bytes = 16;
            break;
        case Op::A64ColdSetSP:
            offset = offsetof(A64JitState, sp);
            break;
        case Op::A64ColdSetNZCV:
            offset = offsetof(A64JitState, cpsr_nzcv);
            bytes = 4;
            break;
        default:
            UNREACHABLE();
        }
        const bool vector = store.opcode == Op::A64ColdSetS || store.opcode == Op::A64ColdSetD || store.opcode == Op::A64ColdSetQ;

        // The low 64 bits in a GPR (and the high ones in Xscratch1 for Q),
        // or a whole Q register stored directly.
        oaknut::XReg low = Xscratch0;
        if (store.immediate) {
            code.MOV(Xscratch0, *store.immediate);
        } else {
            const HostLoc location = *store.location;
            switch (location.kind) {
            case HostLoc::Kind::Gpr:
                ASSERT(bytes != 16);
                low = oaknut::XReg{location.index};
                break;
            case HostLoc::Kind::Fpr:
                if (bytes == 16) {
                    code.STR(oaknut::QReg{location.index}, Xstate, offset);
                    continue;
                }
                code.FMOV(Xscratch0, oaknut::DReg{location.index});
                break;
            case HostLoc::Kind::Spill:
                if (store.remat) {
                    ASSERT(bytes != 16);
                    if (store.remat->word) {
                        code.LDR(Wscratch0, Xstate, store.remat->offset);
                    } else {
                        code.LDR(Xscratch0, Xstate, store.remat->offset);
                    }
                    break;
                }
                code.LDR(Xscratch0, SP, spill_offset + location.index * spill_slot_size);
                if (bytes == 16) {
                    code.LDR(Xscratch1, SP, spill_offset + location.index * spill_slot_size + 8);
                }
                break;
            default:
                UNREACHABLE();
            }
        }

        if (bytes == 4) {
            code.STR(low.toW(), Xstate, offset);
        } else {
            code.STR(low, Xstate, offset);
        }
        if (bytes == 16) {
            code.STR(Xscratch1, Xstate, offset + 8);
        }
        // A W write zero-extends into X; S and D writes clear the rest of Q.
        if (store.opcode == Op::A64ColdSetW || store.opcode == Op::A64ColdSetS) {
            code.STR(WZR, Xstate, offset + 4);
        }
        if (vector && bytes < 16) {
            code.STR(XZR, Xstate, offset + 8);
        }
    }
}

void EmitA64CheckMemoryAbort(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, oaknut::Label& end) {
    if (!ctx.conf.check_halt_on_memory_access) {
        return;
    }

    const A64::LocationDescriptor current_location{IR::LocationDescriptor{inst->GetArg(0).GetU64()}};

    code.LDAR(Xscratch0, Xhalt);
    code.TST(Xscratch0, static_cast<u32>(HaltReason::MemoryAbort));
    code.B(EQ, end);
    if (ctx.conf.cold_memory_abort_state) {
        // Venture: the access's A64ColdSet* records follow it, so its exit is
        // emitted after the block's code, when they are known (as on x64).
        SharedLabel abort = GenSharedLabel();
        code.B(*abort);
        ctx.deferred_emits.emplace_back([&code, &ctx, inst, abort, current_location] {
            code.l(*abort);
            EmitColdStateStores(code, ctx, inst);
            code.MOV(Xscratch0, current_location.PC());
            code.STR(Xscratch0, Xstate, offsetof(A64JitState, pc));
            EmitRelocation(code, ctx, LinkTarget::ReturnFromRunCode);
        });
        return;
    }
    code.MOV(Xscratch0, current_location.PC());
    code.STR(Xscratch0, Xstate, offsetof(A64JitState, pc));
    EmitRelocation(code, ctx, LinkTarget::ReturnFromRunCode);
}

template<>
void EmitIR<IR::Opcode::A64SetCheckBit>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    if (args[0].IsImmediate()) {
        if (args[0].GetImmediateU1()) {
            code.MOV(Wscratch0, 1);
            code.STRB(Wscratch0, SP, offsetof(StackLayout, check_bit));
        } else {
            code.STRB(WZR, SP, offsetof(StackLayout, check_bit));
        }
    } else {
        auto Wbit = ctx.reg_alloc.ReadW(args[0]);
        RegAlloc::Realize(Wbit);
        code.STRB(Wbit, SP, offsetof(StackLayout, check_bit));
    }
}

template<>
void EmitIR<IR::Opcode::A64GetCFlag>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto Wflag = ctx.reg_alloc.WriteW(inst);
    RegAlloc::Realize(Wflag);
    code.LDR(Wflag, Xstate, offsetof(A64JitState, cpsr_nzcv));
    code.AND(Wflag, Wflag, 1 << 29);
}

template<>
void EmitIR<IR::Opcode::A64GetNZCVRaw>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto Wnzcv = ctx.reg_alloc.WriteW(inst);
    RegAlloc::Realize(Wnzcv);

    code.LDR(Wnzcv, Xstate, offsetof(A64JitState, cpsr_nzcv));
}

template<>
void EmitIR<IR::Opcode::A64SetNZCVRaw>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Wnzcv = ctx.reg_alloc.ReadW(args[0]);
    RegAlloc::Realize(Wnzcv);

    // Venture: MSR NZCV keeps only N, Z, C and V; the other bits are RES0.
    code.AND(Wscratch0, Wnzcv, 0xf0000000);
    code.STR(Wscratch0, Xstate, offsetof(A64JitState, cpsr_nzcv));
}

template<>
void EmitIR<IR::Opcode::A64SetNZCV>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    if (ctx.deferred_nzcv_set == inst) {
        return;  // the If terminal stores it on the exits that need it
    }
    auto Wnzcv = ctx.reg_alloc.ReadW(args[0]);
    RegAlloc::Realize(Wnzcv);

    code.STR(Wnzcv, Xstate, offsetof(A64JitState, cpsr_nzcv));
}

// Venture: the A64 register cache's guest registers to and from the JIT state,
// around code that reads or writes them there.
void EmitA64RegisterCacheStore(oaknut::CodeGenerator& code, bool enabled) {
    if (enabled) {
        for (const auto& pair : A64_REGISTER_CACHE) {
            code.STR(oaknut::XReg{pair.host}, Xstate, offsetof(A64JitState, reg) + sizeof(u64) * pair.guest);
        }
    }
}

void EmitA64RegisterCacheLoad(oaknut::CodeGenerator& code, bool enabled) {
    if (enabled) {
        for (const auto& pair : A64_REGISTER_CACHE) {
            code.LDR(oaknut::XReg{pair.host}, Xstate, offsetof(A64JitState, reg) + sizeof(u64) * pair.guest);
        }
    }
}

template<>
void EmitIR<IR::Opcode::A64GetW>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();

    auto Wresult = ctx.reg_alloc.WriteW(inst);
    RegAlloc::Realize(Wresult);

    // TODO: Detect if Gpr vs Fpr is more appropriate

    if (const auto cached = A64CachedRegister(ctx.conf.a64_register_cache, static_cast<size_t>(reg))) {
        code.MOV(Wresult, cached->toW());
        return;
    }
    code.LDR(Wresult, Xstate, offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg));
}

template<>
void EmitIR<IR::Opcode::A64GetX>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();

    auto Xresult = ctx.reg_alloc.WriteX(inst);
    RegAlloc::Realize(Xresult);

    // TODO: Detect if Gpr vs Fpr is more appropriate

    if (const auto cached = A64CachedRegister(ctx.conf.a64_register_cache, static_cast<size_t>(reg))) {
        code.MOV(Xresult, *cached);
        return;
    }
    code.LDR(Xresult, Xstate, offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg));
}

template<>
void EmitIR<IR::Opcode::A64GetS>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    auto Sresult = ctx.reg_alloc.WriteS(inst);
    RegAlloc::Realize(Sresult);
    code.LDR(Sresult, Xstate, offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec));
}

template<>
void EmitIR<IR::Opcode::A64GetD>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    auto Dresult = ctx.reg_alloc.WriteD(inst);
    RegAlloc::Realize(Dresult);
    code.LDR(Dresult, Xstate, offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec));
}

template<>
void EmitIR<IR::Opcode::A64GetQ>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    auto Qresult = ctx.reg_alloc.WriteQ(inst);
    RegAlloc::Realize(Qresult);
    code.LDR(Qresult, Xstate, offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec));
}

template<>
void EmitIR<IR::Opcode::A64GetSP>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto Xresult = ctx.reg_alloc.WriteX(inst);
    RegAlloc::Realize(Xresult);

    code.LDR(Xresult, Xstate, offsetof(A64JitState, sp));
}

template<>
void EmitIR<IR::Opcode::A64GetFPCR>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto Wresult = ctx.reg_alloc.WriteW(inst);
    RegAlloc::Realize(Wresult);

    code.LDR(Wresult, Xstate, offsetof(A64JitState, fpcr));
}

template<>
void EmitIR<IR::Opcode::A64GetFPSR>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    auto Wresult = ctx.reg_alloc.WriteW(inst);
    RegAlloc::Realize(Wresult);

    ctx.fpsr.GetFpsr(Wresult);
}

template<>
void EmitIR<IR::Opcode::A64SetW>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    auto Wvalue = ctx.reg_alloc.ReadW(args[1]);
    RegAlloc::Realize(Wvalue);

    // TODO: Detect if Gpr vs Fpr is more appropriate
    if (const auto cached = A64CachedRegister(ctx.conf.a64_register_cache, static_cast<size_t>(reg))) {
        code.MOV(cached->toW(), Wvalue);
        return;
    }
    code.MOV(*Wvalue, Wvalue);
    code.STR(Wvalue->toX(), Xstate, offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg));
}

template<>
void EmitIR<IR::Opcode::A64SetX>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    auto Xvalue = ctx.reg_alloc.ReadX(args[1]);
    RegAlloc::Realize(Xvalue);

    // TODO: Detect if Gpr vs Fpr is more appropriate

    if (const auto cached = A64CachedRegister(ctx.conf.a64_register_cache, static_cast<size_t>(reg))) {
        code.MOV(*cached, Xvalue);
        return;
    }
    code.STR(Xvalue, Xstate, offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg));
}

template<>
void EmitIR<IR::Opcode::A64SetS>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    auto Svalue = ctx.reg_alloc.ReadS(args[1]);
    RegAlloc::Realize(Svalue);

    code.FMOV(Svalue, Svalue);
    code.STR(Svalue->toQ(), Xstate, offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec));
}

template<>
void EmitIR<IR::Opcode::A64SetD>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    auto Dvalue = ctx.reg_alloc.ReadD(args[1]);
    RegAlloc::Realize(Dvalue);

    code.FMOV(Dvalue, Dvalue);
    code.STR(Dvalue->toQ(), Xstate, offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec));
}

template<>
void EmitIR<IR::Opcode::A64SetQ>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    auto Qvalue = ctx.reg_alloc.ReadQ(args[1]);
    RegAlloc::Realize(Qvalue);
    code.STR(Qvalue, Xstate, offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec));
}

template<>
void EmitIR<IR::Opcode::A64SetSP>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xvalue = ctx.reg_alloc.ReadX(args[0]);
    RegAlloc::Realize(Xvalue);
    code.STR(Xvalue, Xstate, offsetof(A64JitState, sp));
}

template<>
void EmitIR<IR::Opcode::A64SetFPCR>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Wvalue = ctx.reg_alloc.ReadW(args[0]);
    RegAlloc::Realize(Wvalue);
    code.STR(Wvalue, Xstate, offsetof(A64JitState, fpcr));
    code.MSR(oaknut::SystemReg::FPCR, Wvalue->toX());
}

template<>
void EmitIR<IR::Opcode::A64SetFPSR>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Wvalue = ctx.reg_alloc.ReadW(args[0]);
    RegAlloc::Realize(Wvalue);
    code.STR(Wvalue, Xstate, offsetof(A64JitState, fpsr));
    code.MSR(oaknut::SystemReg::FPSR, Wvalue->toX());
}

template<>
void EmitIR<IR::Opcode::A64SetPC>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xvalue = ctx.reg_alloc.ReadX(args[0]);
    RegAlloc::Realize(Xvalue);
    code.STR(Xvalue, Xstate, offsetof(A64JitState, pc));
}

template<>
void EmitIR<IR::Opcode::A64CallSupervisor>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall();

    if (ctx.conf.enable_cycle_counting) {
        code.LDR(X1, SP, offsetof(StackLayout, cycles_to_run));
        code.SUB(X1, X1, Xticks);
        EmitRelocation(code, ctx, LinkTarget::AddTicks);
    }

    // Venture: fast supervisor calls run their handler inside the JIT, which halts
    // only when the handler declines (as on x64). A transferring call runs with
    // the host FPCR, the guest FPSR already in the JIT state (PrepareForCall
    // spilled it) and the handler's own flags dropped; every other call keeps the
    // guest FPCR and its flags become guest FPSR flags, as with x64's guest MXCSR.
    const u32 imm = args[0].GetImmediateU32();
    const bool transfer_fp = ctx.conf.svc_fp_transfer && imm == *ctx.conf.svc_fp_transfer;
    const bool guest_fp = ctx.conf.svc_fp_guest && imm == *ctx.conf.svc_fp_guest;
    const bool fast = transfer_fp || guest_fp || (ctx.conf.svc_fast && imm == *ctx.conf.svc_fast);
    const void* known = fast && ctx.conf.svc_lookup ? ctx.conf.svc_lookup(A64::LocationDescriptor{ctx.block.Location()}.PC(), imm) : nullptr;
    if (transfer_fp) {
        code.LDR(Wscratch0, SP, offsetof(StackLayout, save_host_fpcr));
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);
    } else {
        code.MSR(oaknut::SystemReg::FPSR, XZR);
    }
    EmitA64RegisterCacheStore(code, ctx.conf.a64_register_cache);
    code.MOV(W1, imm);
    if (known) {
        code.MOV(X2, std::bit_cast<u64>(known));
        code.MOV(X3, Xstate);
        EmitRelocation(code, ctx, LinkTarget::CallSVCFastKnown);
    } else {
        EmitRelocation(code, ctx, fast ? LinkTarget::CallSVCFast : LinkTarget::CallSVC);
    }
    EmitA64RegisterCacheLoad(code, ctx.conf.a64_register_cache);
    if (transfer_fp) {
        code.LDR(Wscratch0, Xstate, offsetof(A64JitState, fpcr));
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);
    } else {
        code.LDR(Wscratch0, Xstate, offsetof(A64JitState, fpsr));
        code.MRS(Xscratch1, oaknut::SystemReg::FPSR);
        code.ORR(Wscratch0, Wscratch0, Wscratch1);
        code.STR(Wscratch0, Xstate, offsetof(A64JitState, fpsr));
    }
    if (!fast) {
        // A halting call's handler may have set the guest FPCR (Jit::SetFpcr).
        code.LDR(Wscratch0, Xstate, offsetof(A64JitState, fpcr));
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);
    }

    if (ctx.conf.enable_cycle_counting) {
        EmitRelocation(code, ctx, LinkTarget::GetTicksRemaining);
        code.STR(X0, SP, offsetof(StackLayout, cycles_to_run));
        code.MOV(Xticks, X0);
    }
}

template<>
void EmitIR<IR::Opcode::A64ExceptionRaised>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall();

    if (ctx.conf.enable_cycle_counting) {
        code.LDR(X1, SP, offsetof(StackLayout, cycles_to_run));
        code.SUB(X1, X1, Xticks);
        EmitRelocation(code, ctx, LinkTarget::AddTicks);
    }

    code.MOV(X1, args[0].GetImmediateU64());
    code.MOV(X2, args[1].GetImmediateU64());
    EmitA64RegisterCacheStore(code, ctx.conf.a64_register_cache);
    EmitRelocation(code, ctx, LinkTarget::ExceptionRaised);
    EmitA64RegisterCacheLoad(code, ctx.conf.a64_register_cache);

    if (ctx.conf.enable_cycle_counting) {
        EmitRelocation(code, ctx, LinkTarget::GetTicksRemaining);
        code.STR(X0, SP, offsetof(StackLayout, cycles_to_run));
        code.MOV(Xticks, X0);
    }
}

template<>
void EmitIR<IR::Opcode::A64DataCacheOperationRaised>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[1], args[2]);
    EmitA64RegisterCacheStore(code, ctx.conf.a64_register_cache);
    EmitRelocation(code, ctx, LinkTarget::DataCacheOperationRaised);
    EmitA64RegisterCacheLoad(code, ctx.conf.a64_register_cache);
}

template<>
void EmitIR<IR::Opcode::A64InstructionCacheOperationRaised>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.PrepareForCall({}, args[0], args[1]);
    EmitA64RegisterCacheStore(code, ctx.conf.a64_register_cache);
    EmitRelocation(code, ctx, LinkTarget::InstructionCacheOperationRaised);
    EmitA64RegisterCacheLoad(code, ctx.conf.a64_register_cache);
}

template<>
void EmitIR<IR::Opcode::A64DataSynchronizationBarrier>(oaknut::CodeGenerator& code, EmitContext&, IR::Inst*) {
    code.DSB(oaknut::BarrierOp::SY);
}

template<>
void EmitIR<IR::Opcode::A64DataMemoryBarrier>(oaknut::CodeGenerator& code, EmitContext&, IR::Inst*) {
    code.DMB(oaknut::BarrierOp::SY);
}

template<>
void EmitIR<IR::Opcode::A64InstructionSynchronizationBarrier>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst*) {
    if (!ctx.conf.hook_isb) {
        return;
    }

    ctx.reg_alloc.PrepareForCall();
    EmitA64RegisterCacheStore(code, ctx.conf.a64_register_cache);
    EmitRelocation(code, ctx, LinkTarget::InstructionSynchronizationBarrierRaised);
    EmitA64RegisterCacheLoad(code, ctx.conf.a64_register_cache);
}

template<>
void EmitIR<IR::Opcode::A64GetCNTFRQ>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto Xvalue = ctx.reg_alloc.WriteX(inst);
    RegAlloc::Realize(Xvalue);
    code.MOV(Xvalue, ctx.conf.cntfreq_el0);
}

template<>
void EmitIR<IR::Opcode::A64GetCNTPCT>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    ctx.reg_alloc.PrepareForCall();
    if (!ctx.conf.wall_clock_cntpct && ctx.conf.enable_cycle_counting) {
        code.LDR(X1, SP, offsetof(StackLayout, cycles_to_run));
        code.SUB(X1, X1, Xticks);
        EmitRelocation(code, ctx, LinkTarget::AddTicks);
        EmitRelocation(code, ctx, LinkTarget::GetTicksRemaining);
        code.STR(X0, SP, offsetof(StackLayout, cycles_to_run));
        code.MOV(Xticks, X0);
    }
    EmitRelocation(code, ctx, LinkTarget::GetCNTPCT);
    ctx.reg_alloc.DefineAsRegister(inst, X0);
}

template<>
void EmitIR<IR::Opcode::A64GetCTR>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto Wvalue = ctx.reg_alloc.WriteW(inst);
    RegAlloc::Realize(Wvalue);
    code.MOV(Wvalue, ctx.conf.ctr_el0);
}

template<>
void EmitIR<IR::Opcode::A64GetDCZID>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto Wvalue = ctx.reg_alloc.WriteW(inst);
    RegAlloc::Realize(Wvalue);
    code.MOV(Wvalue, ctx.conf.dczid_el0);
}

template<>
void EmitIR<IR::Opcode::A64GetTPIDR>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto Xvalue = ctx.reg_alloc.WriteX(inst);
    RegAlloc::Realize(Xvalue);
    code.MOV(Xscratch0, std::bit_cast<u64>(ctx.conf.tpidr_el0));
    code.LDR(Xvalue, Xscratch0);
}

template<>
void EmitIR<IR::Opcode::A64GetTPIDRRO>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto Xvalue = ctx.reg_alloc.WriteX(inst);
    RegAlloc::Realize(Xvalue);
    code.MOV(Xscratch0, std::bit_cast<u64>(ctx.conf.tpidrro_el0));
    code.LDR(Xvalue, Xscratch0);
}

template<>
void EmitIR<IR::Opcode::A64SetTPIDR>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Xvalue = ctx.reg_alloc.ReadX(args[0]);
    RegAlloc::Realize(Xvalue);
    code.MOV(Xscratch0, std::bit_cast<u64>(ctx.conf.tpidr_el0));
    code.STR(Xvalue, Xscratch0);
}

}  // namespace Dynarmic::Backend::Arm64
