// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/arm64/emit_arm64.h"

#include <algorithm>
#include <array>

#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/emit_context.h"
#include "dynarmic/backend/arm64/fpsr_manager.h"
#include "dynarmic/backend/arm64/reg_alloc.h"
#include "dynarmic/backend/arm64/a32_jitstate.h"
#include "dynarmic/backend/arm64/a64_jitstate.h"
#include "dynarmic/backend/arm64/stack_layout.h"
#include "dynarmic/backend/arm64/verbose_debugging_output.h"
#include "dynarmic/backend/code_stats.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Arm64 {

using namespace oaknut::util;

template<>
void EmitIR<IR::Opcode::Void>(oaknut::CodeGenerator&, EmitContext&, IR::Inst*) {}

template<>
void EmitIR<IR::Opcode::Identity>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    ctx.reg_alloc.DefineAsExisting(inst, args[0]);
}

template<>
void EmitIR<IR::Opcode::Breakpoint>(oaknut::CodeGenerator& code, EmitContext&, IR::Inst*) {
    code.BRK(0);
}

template<>
void EmitIR<IR::Opcode::CallHostFunction>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    ctx.reg_alloc.PrepareForCall(args[1], args[2], args[3]);
    code.MOV(Xscratch0, args[0].GetImmediateU64());
    code.BLR(Xscratch0);
}

template<>
void EmitIR<IR::Opcode::PushRSB>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    if (!ctx.conf.HasOptimization(OptimizationFlag::ReturnStackBuffer)) {
        return;
    }

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[0].IsImmediate());
    const IR::LocationDescriptor target{args[0].GetImmediateU64()};
    // Venture: a known return is popped by the block's own exit (as on x64).
    if (ctx.known_return) {
        return;
    }

    code.LDR(Wscratch2, SP, offsetof(StackLayout, rsb_ptr));
    code.ADD(Wscratch2, Wscratch2, sizeof(RSBEntry));
    code.AND(Wscratch2, Wscratch2, RSBMask(ctx.conf.is_a64));
    code.STR(Wscratch2, SP, offsetof(StackLayout, rsb_ptr));
    code.ADD(Xscratch2, SP, Xscratch2);

    code.MOV(Xscratch0, target.Value());
    EmitBlockLinkRelocation(code, ctx, target, BlockRelocationType::MoveToScratch1);
    code.STP(Xscratch0, Xscratch1, Xscratch2, offsetof(StackLayout, rsb));
}

template<>
void EmitIR<IR::Opcode::GetCarryFromOp>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    [[maybe_unused]] auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(ctx.reg_alloc.WasValueDefined(inst));
}

template<>
void EmitIR<IR::Opcode::GetOverflowFromOp>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    [[maybe_unused]] auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(ctx.reg_alloc.WasValueDefined(inst));
}

template<>
void EmitIR<IR::Opcode::GetGEFromOp>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    [[maybe_unused]] auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(ctx.reg_alloc.WasValueDefined(inst));
}

template<>
void EmitIR<IR::Opcode::GetNZCVFromOp>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    if (ctx.reg_alloc.WasValueDefined(inst)) {
        return;
    }

    switch (args[0].GetType()) {
    case IR::Type::U32: {
        auto Wvalue = ctx.reg_alloc.ReadW(args[0]);
        auto flags = ctx.reg_alloc.WriteFlags(inst);
        RegAlloc::Realize(Wvalue, flags);

        code.TST(*Wvalue, Wvalue);
        break;
    }
    case IR::Type::U64: {
        auto Xvalue = ctx.reg_alloc.ReadX(args[0]);
        auto flags = ctx.reg_alloc.WriteFlags(inst);
        RegAlloc::Realize(Xvalue, flags);

        code.TST(*Xvalue, Xvalue);
        break;
    }
    default:
        UNREACHABLE();
    }
}

template<>
void EmitIR<IR::Opcode::GetNZFromOp>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    if (ctx.reg_alloc.WasValueDefined(inst)) {
        return;
    }

    switch (args[0].GetType()) {
    case IR::Type::U32: {
        auto Wvalue = ctx.reg_alloc.ReadW(args[0]);
        auto flags = ctx.reg_alloc.WriteFlags(inst);
        RegAlloc::Realize(Wvalue, flags);

        code.TST(*Wvalue, *Wvalue);
        break;
    }
    case IR::Type::U64: {
        auto Xvalue = ctx.reg_alloc.ReadX(args[0]);
        auto flags = ctx.reg_alloc.WriteFlags(inst);
        RegAlloc::Realize(Xvalue, flags);

        code.TST(*Xvalue, *Xvalue);
        break;
    }
    default:
        UNREACHABLE();
    }
}

template<>
void EmitIR<IR::Opcode::GetUpperFromOp>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    [[maybe_unused]] auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(ctx.reg_alloc.WasValueDefined(inst));
}

template<>
void EmitIR<IR::Opcode::GetLowerFromOp>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    [[maybe_unused]] auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(ctx.reg_alloc.WasValueDefined(inst));
}

template<>
void EmitIR<IR::Opcode::GetCFlagFromNZCV>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    auto Wc = ctx.reg_alloc.WriteW(inst);
    auto Wnzcv = ctx.reg_alloc.ReadW(args[0]);
    RegAlloc::Realize(Wc, Wnzcv);

    code.AND(Wc, Wnzcv, 1 << 29);
}

template<>
void EmitIR<IR::Opcode::NZCVFromPackedFlags>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    ctx.reg_alloc.DefineAsExisting(inst, args[0]);
}

static void EmitAddCycles(oaknut::CodeGenerator& code, EmitContext& ctx, std::size_t cycles_to_add) {
    if (!ctx.conf.enable_cycle_counting) {
        return;
    }
    if (cycles_to_add == 0) {
        return;
    }

    if (oaknut::AddSubImm::is_valid(cycles_to_add)) {
        code.SUB(Xticks, Xticks, cycles_to_add);
    } else {
        code.MOV(Xscratch1, cycles_to_add);
        code.SUB(Xticks, Xticks, Xscratch1);
    }
}

// Venture: guest register reads are re-read from the JIT state instead of
// spilled (as on x64) while no write to that register comes before their last
// use. A callback that may change registers (a supervisor call, an exception,
// a coprocessor or cache operation) counts as a write to every register; A64
// registers with cold abort state, whose deferred stores run together on the
// abort exit, keep spilling.
static void MarkRematerializableReads(RegAlloc& reg_alloc, IR::Block& block, bool is_a64, bool register_cache) {
    using Op = IR::Opcode;
    std::array<std::vector<u32>, 32> writes;
    std::array<bool, 32> cold{};
    std::vector<u32> clobbers;
    for (auto& inst : block.instructions) {
        switch (inst.GetOpcode()) {
        case Op::A32CallSupervisor:
        case Op::A32ExceptionRaised:
        case Op::A32InstructionSynchronizationBarrier:
        case Op::A32CoprocInternalOperation:
        case Op::A32CoprocSendOneWord:
        case Op::A32CoprocSendTwoWords:
        case Op::A32CoprocGetOneWord:
        case Op::A32CoprocGetTwoWords:
        case Op::A32CoprocLoadWords:
        case Op::A32CoprocStoreWords:
        case Op::A64CallSupervisor:
        case Op::A64ExceptionRaised:
        case Op::A64DataCacheOperationRaised:
        case Op::A64InstructionCacheOperationRaised:
        case Op::A64InstructionSynchronizationBarrier:
            clobbers.push_back(inst.GetEmitPosition());
            break;
        case Op::A32SetRegister:
            writes[static_cast<std::size_t>(inst.GetArg(0).GetA32RegRef())].push_back(inst.GetEmitPosition());
            break;
        case Op::A64SetW:
        case Op::A64SetX:
            writes[static_cast<std::size_t>(inst.GetArg(0).GetA64RegRef())].push_back(inst.GetEmitPosition());
            break;
        case Op::A64ColdSetW:
        case Op::A64ColdSetX:
            cold[static_cast<std::size_t>(inst.GetArg(1).GetA64RegRef())] = true;
            break;
        default:
            break;
        }
    }
    for (auto& inst : block.instructions) {
        std::size_t reg;
        u32 offset;
        bool word;
        switch (inst.GetOpcode()) {
        case Op::A32GetRegister:
            reg = static_cast<std::size_t>(inst.GetArg(0).GetA32RegRef());
            if (is_a64 || reg == 15) {
                continue;
            }
            offset = static_cast<u32>(offsetof(A32JitState, regs) + sizeof(u32) * reg);
            word = true;
            break;
        case Op::A64GetW:
        case Op::A64GetX:
            reg = static_cast<std::size_t>(inst.GetArg(0).GetA64RegRef());
            // A cached register's JIT state slot is stale.
            if (!is_a64 || cold[reg] || A64CachedRegister(register_cache, reg)) {
                continue;
            }
            offset = static_cast<u32>(offsetof(A64JitState, reg) + sizeof(u64) * reg);
            word = inst.GetOpcode() == Op::A64GetW;
            break;
        default:
            continue;
        }
        const auto last = reg_alloc.LastUse(&inst);
        if (!last) {
            continue;
        }
        // A write at the last use reads this value first.
        const u32 at = inst.GetEmitPosition();
        const auto between = [&](u32 w) { return w > at && w < *last; };
        const bool overwritten = std::any_of(writes[reg].begin(), writes[reg].end(), between) || std::any_of(clobbers.begin(), clobbers.end(), between);
        if (!overwritten) {
            reg_alloc.MarkRematerializable(&inst, offset, word);
        }
    }
}

EmittedBlockInfo EmitArm64(oaknut::CodeGenerator& code, IR::Block block, const EmitConfig& conf, FastmemManager& fastmem_manager) {
    if (conf.very_verbose_debugging_output) {
        std::puts(IR::DumpBlock(block).c_str());
    }

    EmittedBlockInfo ebi;

    FpsrManager fpsr_manager{code, conf.state_fpsr_offset};
    RegAlloc reg_alloc{code, fpsr_manager, conf.a64_register_cache ? GPR_ORDER_A64_REGISTER_CACHE : GPR_ORDER, FPR_ORDER};
    EmitContext ctx{block, reg_alloc, conf, ebi, fpsr_manager, fastmem_manager, {}};
    if (conf.is_a64) {
        PrepareA64Terminal(ctx);
        PrepareA64NZCVExit(ctx);
    }
    reg_alloc.BuildNextUses(block);
    FoldA64Addresses(ctx);
    if (!conf.very_verbose_debugging_output) {
        MarkRematerializableReads(reg_alloc, block, conf.is_a64, conf.a64_register_cache);
    }

    ebi.entry_point = code.xptr<CodePtr>();
    Backend::CodeStats::Recorder stats{conf.is_a64,
                                       conf.is_a64 ? A64::LocationDescriptor{block.Location()}.PC() : A32::LocationDescriptor{block.Location()}.PC(),
                                       static_cast<std::uint32_t>(block.CycleCount())};

    if (ctx.block.GetCondition() == IR::Cond::AL) {
        ASSERT(!ctx.block.HasConditionFailedLocation());
    } else {
        ASSERT(ctx.block.HasConditionFailedLocation());
        oaknut::Label pass;

        pass = conf.emit_cond(code, ctx, ctx.block.GetCondition());
        EmitAddCycles(code, ctx, ctx.block.ConditionFailedCycleCount());
        conf.emit_condition_failed_terminal(code, ctx);

        code.l(pass);
    }
    if (conf.is_a64 && conf.diagnostic_record && conf.diagnostic_selected &&
        conf.diagnostic_selected(A64::LocationDescriptor{block.Location()}.PC())) {
        // Venture: at entry every guest register is in the state block and no
        // caller-saved host register is live; the pinned registers are
        // callee-saved. The callback runs with the host FPCR, as on x64.
        code.LDR(Wscratch0, SP, offsetof(StackLayout, save_host_fpcr));
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);
        EmitA64RegisterCacheStore(code, conf.a64_register_cache);
        code.MOV(X0, std::bit_cast<u64>(conf.diagnostic_user));
        code.MOV(X1, A64::LocationDescriptor{block.Location()}.PC());
        code.MOV(X2, Xstate);
        code.MOV(Xscratch0, std::bit_cast<u64>(conf.diagnostic_record));
        code.BLR(Xscratch0);
        code.LDR(Wscratch0, Xstate, offsetof(A64JitState, fpcr));
        code.MSR(oaknut::SystemReg::FPCR, Xscratch0);
    }
    stats.Add(Backend::CodeStats::Chunk::Prologue, ebi.entry_point, code.xptr<CodePtr>());

    // Venture: the NZCV value whose bits host NZCV holds, as x64's
    // select_flags_in_host. ADDS/SUBS/ANDS/BICS leave guest NZCV in host NZCV;
    // guest register reads and writes, storing that NZCV, and selects on it
    // emit only loads, stores, moves, MRS and CSEL (register allocation
    // included), which keep it.
    const IR::Inst* host_nzcv = nullptr;
    bool host_nzcv_stored = false;  // and the guest NZCV is that value
    for (auto iter = block.instructions.begin(); iter != block.instructions.end(); ++iter) {
        IR::Inst* inst = &*iter;
        const auto op_start = code.xptr<CodePtr>();
        reg_alloc.SetCurrentInstruction(inst);

        ctx.select_flags_in_host = false;
        if (conf.is_a64 && !conf.very_verbose_debugging_output) {
            const auto holds = [&](const IR::Value& arg) { return host_nzcv && !arg.IsImmediate() && arg.GetInst() == host_nzcv; };
            switch (inst->GetOpcode()) {
            case IR::Opcode::ConditionalSelectFlags32:
            case IR::Opcode::ConditionalSelectFlags64:
            case IR::Opcode::ConditionalSelectFlagsNZCV:
                ctx.select_flags_in_host = holds(inst->GetArg(3));
                if (!ctx.select_flags_in_host) {
                    host_nzcv = nullptr;
                    host_nzcv_stored = false;
                }
                break;
            case IR::Opcode::Add32:
            case IR::Opcode::Add64:
            case IR::Opcode::Sub32:
            case IR::Opcode::Sub64:
            case IR::Opcode::And32:
            case IR::Opcode::And64:
            case IR::Opcode::AndNot32:
            case IR::Opcode::AndNot64:
                host_nzcv = inst->GetAssociatedPseudoOperation(IR::Opcode::GetNZCVFromOp);
                host_nzcv_stored = false;
                break;
            case IR::Opcode::GetNZCVFromOp:
                if (inst != host_nzcv) {
                    host_nzcv = nullptr;
                    host_nzcv_stored = false;
                }
                break;
            case IR::Opcode::A64SetNZCV:
                host_nzcv_stored = holds(inst->GetArg(0));
                if (!host_nzcv_stored) {
                    host_nzcv = nullptr;
                }
                break;
            case IR::Opcode::Void:
            case IR::Opcode::A64GetW:
            case IR::Opcode::A64GetX:
            case IR::Opcode::A64SetW:
            case IR::Opcode::A64SetX:
                break;
            default:
                host_nzcv = nullptr;
                host_nzcv_stored = false;
                break;
            }
        }

        // Venture: a folded address's add or index shift emits nothing, nor
        // does a boolean its CheckBit terminal tests directly.
        const auto next = std::next(iter);
        const bool fused = conf.is_a64 && !conf.enable_cycle_counting && !conf.very_verbose_debugging_output &&
                           FuseA64CheckBit(ctx, inst, next != block.instructions.end() ? &*next : nullptr,
                                           next != block.instructions.end() && std::next(next) == block.instructions.end());
        if (!fused && !EmitFoldedAddressAlias(ctx, inst))
        switch (inst->GetOpcode()) {
#define OPCODE(name, type, ...)                    \
    case IR::Opcode::name:                         \
        EmitIR<IR::Opcode::name>(code, ctx, inst); \
        break;
#define A32OPC(name, type, ...)                         \
    case IR::Opcode::A32##name:                         \
        EmitIR<IR::Opcode::A32##name>(code, ctx, inst); \
        break;
#define A64OPC(name, type, ...)                         \
    case IR::Opcode::A64##name:                         \
        EmitIR<IR::Opcode::A64##name>(code, ctx, inst); \
        break;
#include "dynarmic/ir/opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC
        default:
            UNREACHABLE();
        }

        reg_alloc.UpdateAllUses();
        reg_alloc.AssertAllUnlocked();
        stats.Add(Backend::CodeStats::Chunk::Op, inst->GetOpcode(), op_start, code.xptr<CodePtr>());

        if (conf.very_verbose_debugging_output) {
            EmitVerboseDebuggingOutput(code, ctx);
        }
    }

    const auto epilogue_start = code.xptr<CodePtr>();
    fpsr_manager.Spill();

    reg_alloc.AssertNoMoreUses();

    EmitAddCycles(code, ctx, block.CycleCount());
    // Venture: the epilogue so far (FPSR spill, cycle count) keeps host NZCV.
    ctx.terminal_nzcv_in_flags = conf.is_a64 && host_nzcv && host_nzcv_stored && !conf.enable_cycle_counting;
    ASSERT(!ctx.deferred_nzcv_set || ctx.terminal_nzcv_in_flags);
    conf.emit_terminal(code, ctx);
    code.BRK(0);
    stats.Add(Backend::CodeStats::Chunk::Epilogue, epilogue_start, code.xptr<CodePtr>());

    const auto deferred_start = code.xptr<CodePtr>();
    // Venture: a deferred emit may add more (an out-of-line abort exit).
    for (std::size_t i = 0; i < ctx.deferred_emits.size(); i++) {
        const auto deferred_emit = std::move(ctx.deferred_emits[i]);
        deferred_emit();
    }
    code.BRK(0);
    stats.Add(Backend::CodeStats::Chunk::Deferred, deferred_start, code.xptr<CodePtr>());

    ebi.size = code.xptr<CodePtr>() - ebi.entry_point;
    return ebi;
}

void EmitRelocation(oaknut::CodeGenerator& code, EmitContext& ctx, LinkTarget link_target) {
    ctx.ebi.relocations.emplace_back(Relocation{code.xptr<CodePtr>() - ctx.ebi.entry_point, link_target});
    code.NOP();
}

void EmitBlockLinkRelocation(oaknut::CodeGenerator& code, EmitContext& ctx, const IR::LocationDescriptor& descriptor, BlockRelocationType type) {
    ctx.ebi.block_relocations[descriptor].emplace_back(BlockRelocation{code.xptr<CodePtr>() - ctx.ebi.entry_point, type});
    switch (type) {
    case BlockRelocationType::Branch:
        code.NOP();
        break;
    case BlockRelocationType::MoveToScratch1:
        code.BRK(0);
        code.NOP();
        break;
    default:
        UNREACHABLE();
    }
}

}  // namespace Dynarmic::Backend::Arm64
