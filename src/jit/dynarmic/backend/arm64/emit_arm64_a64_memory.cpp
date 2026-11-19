/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/arm64/a64_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/emit_arm64_memory.h"
#include "dynarmic/backend/arm64/emit_context.h"
#include "dynarmic/backend/arm64/reg_alloc.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/ir/acc_type.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Arm64 {

using namespace oaknut::util;

template<>
void EmitIR<IR::Opcode::A64ClearExclusive>(oaknut::CodeGenerator& code, EmitContext&, IR::Inst*) {
    code.STR(WZR, Xstate, offsetof(A64JitState, exclusive_state));
}

template<>
void EmitIR<IR::Opcode::A64ReadMemory8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitReadMemory<8>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ReadMemory16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitReadMemory<16>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ReadMemory32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitReadMemory<32>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ReadMemory64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitReadMemory<64>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ReadMemory128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitReadMemory<128>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ExclusiveReadMemory8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitExclusiveReadMemory<8>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ExclusiveReadMemory16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitExclusiveReadMemory<16>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ExclusiveReadMemory32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitExclusiveReadMemory<32>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ExclusiveReadMemory64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitExclusiveReadMemory<64>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ExclusiveReadMemory128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitExclusiveReadMemory<128>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64WriteMemory8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitWriteMemory<8>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64WriteMemory16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitWriteMemory<16>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64WriteMemory32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitWriteMemory<32>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64WriteMemory64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitWriteMemory<64>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64WriteMemory128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitWriteMemory<128>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ExclusiveWriteMemory8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitExclusiveWriteMemory<8>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ExclusiveWriteMemory16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitExclusiveWriteMemory<16>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ExclusiveWriteMemory32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitExclusiveWriteMemory<32>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ExclusiveWriteMemory64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitExclusiveWriteMemory<64>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ExclusiveWriteMemory128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitExclusiveWriteMemory<128>(code, ctx, inst);
}

// Venture: ARMv8.1 LSE atomics (A64AtomicMemory*, A64CompareAndSwapMemory*), through
// UserCallbacks::MemoryAtomic (a64_address_space.cpp). Each clears this CPU's
// exclusive state, as an exclusive store does. A faulting access leaves at the
// instruction, before its result is written, as a faulting read or write does.
template<std::size_t bitsize, bool compare_and_swap>
static void EmitAtomicMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    if constexpr (bitsize <= 64) {
        if (EmitInlineAtomicMemory<bitsize, compare_and_swap>(code, ctx, inst)) {
            return;
        }
    } else {
        if (EmitInlineCompareAndSwap128(code, ctx, inst)) {
            return;
        }
    }
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    oaknut::Label end;
    if constexpr (bitsize == 128) {
        ctx.reg_alloc.PrepareForCall({}, args[1], args[2], args[3]);
        code.STRB(WZR, Xstate, ctx.conf.state_exclusive_state_offset);
        EmitRelocation(code, ctx, LinkTarget::AtomicCompareAndSwap128);
        code.MOV(Q8.B16(), Q0.B16());
        ctx.conf.emit_check_memory_abort(code, ctx, inst, end);
        code.l(end);
        ctx.reg_alloc.DefineAsRegister(inst, Q8);
    } else {
        u64 op;
        if constexpr (compare_and_swap) {
            ctx.reg_alloc.PrepareForCall({}, args[1], args[2], args[3]);
            op = static_cast<u64>(A64::AtomicOp::CompareAndSwap);
        } else {
            op = args[3].GetImmediateU8();
            ctx.reg_alloc.PrepareForCall({}, args[1], args[2]);
            code.MOV(X3, XZR);
        }
        code.MOV(X4, op | (bitsize / 8) << 8);
        code.STRB(WZR, Xstate, ctx.conf.state_exclusive_state_offset);
        EmitRelocation(code, ctx, LinkTarget::AtomicMemory);
        ctx.conf.emit_check_memory_abort(code, ctx, inst, end);
        code.l(end);
        ctx.reg_alloc.DefineAsRegister(inst, X0);
    }
}

template<>
void EmitIR<IR::Opcode::A64AtomicMemory8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitAtomicMemory<8, false>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64AtomicMemory16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitAtomicMemory<16, false>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64AtomicMemory32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitAtomicMemory<32, false>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64AtomicMemory64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitAtomicMemory<64, false>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64CompareAndSwapMemory8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitAtomicMemory<8, true>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64CompareAndSwapMemory16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitAtomicMemory<16, true>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64CompareAndSwapMemory32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitAtomicMemory<32, true>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64CompareAndSwapMemory64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitAtomicMemory<64, true>(code, ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64CompareAndSwapMemory128>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitAtomicMemory<128, true>(code, ctx, inst);
}

// Venture: cold abort state (as on x64). An A64ColdSet* directly follows its
// access and emits nothing: it records where its value is, and the access's
// abort exit (EmitA64CheckMemoryAbort, out of line) stores it.
static void RecordColdState(EmitContext& ctx, IR::Inst* inst) {
    const auto opcode = inst->GetOpcode();
    const bool has_register = opcode != IR::Opcode::A64ColdSetSP && opcode != IR::Opcode::A64ColdSetNZCV;
    std::size_t index = 0;
    if (opcode == IR::Opcode::A64ColdSetW || opcode == IR::Opcode::A64ColdSetX) {
        index = static_cast<std::size_t>(inst->GetArg(1).GetA64RegRef());
    } else if (has_register) {
        index = static_cast<std::size_t>(inst->GetArg(1).GetA64VecRef());
    }
    IR::Inst* const access = inst->GetArg(0).GetInst();
    // The access's own use, when it has a value (a read).
    ctx.reg_alloc.UseInPlace(inst->GetArg(0));
    const IR::Value value = inst->GetArg(has_register ? 2 : 1);
    EmitContext::ColdStateStore store{opcode, index, std::nullopt, std::nullopt};
    if (value.IsImmediate()) {
        store.immediate = value.GetImmediateAsU64();
    } else {
        store.location = ctx.reg_alloc.UseInPlace(value);
        ASSERT(store.location && store.location->kind != HostLoc::Kind::Flags);
        if (store.location->kind == HostLoc::Kind::Spill) {
            store.remat = ctx.reg_alloc.SpillRematerialization(store.location->index);
        }
    }
    ctx.cold_state[access].push_back(store);
}

template<>
void EmitIR<IR::Opcode::A64ColdSetW>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ColdSetX>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ColdSetS>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ColdSetD>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ColdSetQ>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ColdSetSP>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

template<>
void EmitIR<IR::Opcode::A64ColdSetNZCV>(oaknut::CodeGenerator&, EmitContext& ctx, IR::Inst* inst) {
    RecordColdState(ctx, inst);
}

}  // namespace Dynarmic::Backend::Arm64
