// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <utility>

#include <fmt/ostream.h>
#include "dynarmic/backend/x64/hostloc.h"
#include "common/assert.h"
#include <bit>
#include "dynarmic/backend/x64/xbyak.h"

#include "dynarmic/backend/x64/abi.h"
#include "dynarmic/backend/x64/reg_alloc.h"
#include "dynarmic/backend/x64/stack_layout.h"
#include "dynarmic/backend/x64/verbose_debugging_output.h"
#include "dynarmic/ir/basic_block.h"

namespace Dynarmic::Backend::X64 {

static inline bool CanExchange(const HostLoc a, const HostLoc b) noexcept {
    return HostLocIsGPR(a) && HostLocIsGPR(b);
}

// Minimum number of bits required to represent a type
static inline size_t GetBitWidth(const IR::Type type) noexcept {
    switch (type) {
    case IR::Type::U1:
        return 8;
    case IR::Type::U8:
        return 8;
    case IR::Type::U16:
        return 16;
    case IR::Type::U32:
        return 32;
    case IR::Type::U64:
        return 64;
    case IR::Type::U128:
        return 128;
    case IR::Type::NZCVFlags:
        return 32;  // TODO: Update to 16 when flags optimization is done
    default:
        // A32REG A32EXTREG A64REG A64VEC COPROCINFO COND VOID TABLE ACCTYPE OPAQUE
        UNREACHABLE();
    }
}

static inline bool IsValuelessType(const IR::Type type) noexcept {
    // Void: an A64ColdSet* record names its (possibly void) memory access.
    return type == IR::Type::Table || type == IR::Type::Void;
}

const bool RegAlloc::check_untouched_slots = [] {
    const char* value = std::getenv("VENTURE_JIT_CHECK_ALLOC_SCOPE");
    return value && value[0] == '1';
}();

void RegAlloc::AssertUntouchedSlotsReleased() const noexcept {
    for (size_t i = 0; i < hostloc_info.size(); ++i)
        ASSERT(((touched[i / 64] >> (i % 64)) & 1) || hostloc_info[i].IsReleased());
}

void HostLocInfo::ReleaseOne() noexcept {
    ASSERT(is_being_used_count > 0);
    --is_being_used_count;
    is_scratch = false;
    if (current_references > 0) {
        ASSERT(size_t(accumulated_uses) + 1 < (std::numeric_limits<decltype(accumulated_uses)>::max)());
        ++accumulated_uses;
        --current_references;
        if (current_references == 0)
            ReleaseAll();
    }
}

void HostLocInfo::ReleaseAll() noexcept {
    ASSERT(size_t(accumulated_uses) + current_references < (std::numeric_limits<decltype(accumulated_uses)>::max)());
    accumulated_uses += current_references;
    current_references = 0;
    is_set_last_use = false;
    if (total_uses == accumulated_uses) {
        values.clear();
        accumulated_uses = 0;
        total_uses = 0;
        max_bit_width = 0;
    }

    is_being_used_count = 0;
    is_scratch = false;
}

void HostLocInfo::AddValue(HostLoc loc, IR::Inst* inst) noexcept {
    if (is_set_last_use) {
        is_set_last_use = false;
        values.clear();
    }
    values.push_back(inst);

    ASSERT(size_t(total_uses) + inst->UseCount() < (std::numeric_limits<decltype(total_uses)>::max)());
    total_uses += inst->UseCount();
    max_bit_width = std::max<uint8_t>(max_bit_width, std::countr_zero(GetBitWidth(inst->GetType())));
}

#ifndef NDEBUG
void HostLocInfo::EmitVerboseDebuggingOutput(BlockOfCode& code, size_t host_loc_index) const noexcept {
    using namespace Xbyak::util;
    for (auto const value : values) {
        code.mov(code.ABI_PARAM1, rsp);
        code.mov(code.ABI_PARAM2, host_loc_index);
        code.mov(code.ABI_PARAM3, value->GetName());
        code.mov(code.ABI_PARAM4, GetBitWidth(value->GetType()));
        code.CallFunction(PrintVerboseDebuggingOutputLine);
    }
}
#endif

bool Argument::FitsInImmediateU32() const noexcept {
    if (!IsImmediate())
        return false;
    const u64 imm = value.GetImmediateAsU64();
    return imm < 0x100000000;
}

bool Argument::FitsInImmediateS32() const noexcept {
    if (!IsImmediate())
        return false;
    const s64 imm = s64(value.GetImmediateAsU64());
    return -s64(0x80000000) <= imm && imm <= s64(0x7FFFFFFF);
}

bool Argument::GetImmediateU1() const noexcept {
    return value.GetU1();
}

u8 Argument::GetImmediateU8() const noexcept {
    const u64 imm = value.GetImmediateAsU64();
    ASSERT(imm <= u64(std::numeric_limits<u8>::max()));
    return u8(imm);
}

u16 Argument::GetImmediateU16() const noexcept {
    const u64 imm = value.GetImmediateAsU64();
    ASSERT(imm <= u64(std::numeric_limits<u16>::max()));
    return u16(imm);
}

u32 Argument::GetImmediateU32() const noexcept {
    const u64 imm = value.GetImmediateAsU64();
    ASSERT(imm <= u64(std::numeric_limits<u32>::max()));
    return u32(imm);
}

u64 Argument::GetImmediateS32() const noexcept {
    ASSERT(FitsInImmediateS32());
    return value.GetImmediateAsU64();
}

u64 Argument::GetImmediateU64() const noexcept {
    return value.GetImmediateAsU64();
}

IR::Cond Argument::GetImmediateCond() const noexcept {
    ASSERT(IsImmediate() && GetType() == IR::Type::Cond);
    return value.GetCond();
}

IR::AccType Argument::GetImmediateAccType() const noexcept {
    ASSERT(IsImmediate() && GetType() == IR::Type::AccType);
    return value.GetAccType();
}

/// Is this value currently in a GPR?
bool Argument::IsInGpr(RegAlloc& reg_alloc) const noexcept {
    if (IsImmediate())
        return false;
    return HostLocIsGPR(*reg_alloc.ValueLocation(value.GetInst()));
}

/// Is this value currently in a XMM?
bool Argument::IsInXmm(RegAlloc& reg_alloc) const noexcept {
    if (IsImmediate())
        return false;
    return HostLocIsXMM(*reg_alloc.ValueLocation(value.GetInst()));
}

/// Is this value currently in memory?
bool Argument::IsInMemory(RegAlloc& reg_alloc) const noexcept {
    if (IsImmediate())
        return false;
    return HostLocIsSpill(*reg_alloc.ValueLocation(value.GetInst()));
}

RegAlloc::RegAlloc(std::bitset<32> gpr_order, std::bitset<32> xmm_order) noexcept
    : gpr_order(gpr_order), xmm_order(xmm_order)
{}

RegAlloc::ArgumentInfo RegAlloc::GetArgumentInfo(const IR::Inst* inst) noexcept {
    ArgumentInfo ret{
        Argument{},
        Argument{},
        Argument{},
        Argument{}
    };
    for (size_t i = 0; i < inst->NumArgs() && i < 4; i++) {
        const auto arg = inst->GetArg(i);
        ret[i].value = arg;
        if (!arg.IsImmediate() && !IsValuelessType(arg.GetType())) {
            auto const loc = ValueLocation(arg.GetInst());
            ASSERT(loc && "argument must already been defined");
            LocInfo(*loc).AddArgReference();
        }
    }
    return ret;
}

void RegAlloc::RegisterPseudoOperation(const IR::Inst* inst) noexcept {
    ASSERT(IsValueLive(inst) || !inst->HasUses());
    for (size_t i = 0; i < inst->NumArgs(); i++) {
        auto const arg = inst->GetArg(i);
        if (!arg.IsImmediate() && !IsValuelessType(arg.GetType())) {
            if (auto const loc = ValueLocation(arg.GetInst())) {
                // May not necessarily have a value (e.g. CMP variant of Sub32).
                LocInfo(*loc).AddArgReference();
            }
        }
    }
}

Xbyak::Reg64 RegAlloc::UseScratchGpr(BlockOfCode& code, Argument& arg) noexcept {
    ASSERT(!arg.allocated);
    arg.allocated = true;
    return HostLocToReg64(UseScratchImpl(code, arg.value, gpr_order));
}

Xbyak::Xmm RegAlloc::UseScratchXmm(BlockOfCode& code, Argument& arg) noexcept {
    ASSERT(!arg.allocated);
    arg.allocated = true;
    return HostLocToXmm(UseScratchImpl(code, arg.value, xmm_order));
}

void RegAlloc::UseScratch(BlockOfCode& code, Argument& arg, HostLoc host_loc) noexcept {
    ASSERT(!arg.allocated);
    arg.allocated = true;
    UseScratchImpl(code, arg.value, BuildRegSet({host_loc}));
}

void RegAlloc::DefineValue(BlockOfCode& code, IR::Inst* inst, const Xbyak::Reg& reg) noexcept {
    ASSERT(reg.getKind() == Xbyak::Operand::XMM || reg.getKind() == Xbyak::Operand::REG);
    const auto hostloc = static_cast<HostLoc>(reg.getIdx() + static_cast<size_t>(reg.getKind() == Xbyak::Operand::XMM ? HostLoc::XMM0 : HostLoc::RAX));
    DefineValueImpl(code, inst, hostloc);
}

void RegAlloc::DefineValue(BlockOfCode& code, IR::Inst* inst, Argument& arg) noexcept {
    ASSERT(!arg.allocated);
    arg.allocated = true;
    DefineValueImpl(code, inst, arg.value);
}

void RegAlloc::Release(const Xbyak::Reg& reg) noexcept {
    ASSERT(reg.getKind() == Xbyak::Operand::XMM || reg.getKind() == Xbyak::Operand::REG);
    const auto hostloc = static_cast<HostLoc>(reg.getIdx() + static_cast<size_t>(reg.getKind() == Xbyak::Operand::XMM ? HostLoc::XMM0 : HostLoc::RAX));
    LocInfo(hostloc).ReleaseOne();
}

HostLoc RegAlloc::UseImpl(BlockOfCode& code, IR::Value use_value, std::bitset<32> desired_locations) noexcept {
    if (use_value.IsImmediate()) {
        return LoadImmediate(code, use_value, ScratchImpl(code, desired_locations));
    }

    auto const* use_inst = use_value.GetInst();
    HostLoc const current_location = *ValueLocation(use_inst);

    // A pinned register (register cache) is read in place by any-GPR uses.
    if (HostLocIsRegister(current_location) && (desired_locations.test(size_t(current_location)) ||
                                                (IsPinned(current_location) && desired_locations == gpr_order))) {
        LocInfo(current_location).ReadLock();
        return current_location;
    }

    if (LocInfo(current_location).IsLocked()) {
        return UseScratchImpl(code, use_value, desired_locations);
    }

    size_t const max_bit_width = LocInfo(current_location).GetMaxBitWidth();
    HostLoc const destination_location = SelectARegister(desired_locations);
    if (max_bit_width > HostLocBitWidth(destination_location)) {
        return UseScratchImpl(code, use_value, desired_locations);
    } else if (CanExchange(destination_location, current_location) && !IsPinned(current_location)) {
        Exchange(code, destination_location, current_location);
    } else {
        MoveOutOfTheWay(code, destination_location);
        Move(code, destination_location, current_location);
    }
    LocInfo(destination_location).ReadLock();
    return destination_location;
}

HostLoc RegAlloc::UseScratchImpl(BlockOfCode& code, IR::Value use_value, std::bitset<32> desired_locations) noexcept {
    if (use_value.IsImmediate()) {
        return LoadImmediate(code, use_value, ScratchImpl(code, desired_locations));
    }

    const auto* use_inst = use_value.GetInst();
    const HostLoc current_location = *ValueLocation(use_inst);
    const size_t bit_width = GetBitWidth(use_inst->GetType());
    if (HostLocIsRegister(current_location) && desired_locations.test(size_t(current_location)) && !LocInfo(current_location).IsLocked()) {
        if (LocInfo(current_location).IsLastUse()) {
            LocInfo(current_location).is_set_last_use = true;
        } else {
            MoveOutOfTheWay(code, current_location);
        }
        LocInfo(current_location).WriteLock();
        return current_location;
    }
    // The hinted pinned register's last aliasing value: the result overwrites
    // it in place, as the following A64Set* would.
    if (result_hint && current_location == *result_hint && desired_locations == gpr_order &&
        !LocInfo(current_location).IsLocked() && LocInfo(current_location).IsLastUse()) {
        result_hint.reset();
        LocInfo(current_location).is_set_last_use = true;
        LocInfo(current_location).WriteLock();
        return current_location;
    }

    const auto hinted = TakeEmptyResultHint(desired_locations);
    const HostLoc destination_location = hinted ? *hinted : SelectARegister(desired_locations);
    MoveOutOfTheWay(code, destination_location);
    // A value aliasing a vector (a VectorGetElement* lane alias, a scalar in the
    // low lane) is narrower than the location it shares: copying it into an XMM
    // by its own width would reload a spilled vector with MOVSS/MOVSD and zero
    // the lanes its user still reads (INSERTPS of lane 3). Copy the whole vector.
    const bool vector_location = LocInfo(current_location).GetMaxBitWidth() == 128;
    CopyToScratch(code, HostLocIsXMM(destination_location) && vector_location ? 128 : bit_width, destination_location, current_location);
    LocInfo(destination_location).WriteLock();
    return destination_location;
}

std::optional<HostLoc> RegAlloc::TakeEmptyResultHint(std::bitset<32> desired_locations) noexcept {
    if (!result_hint || desired_locations != gpr_order || !LocInfo(*result_hint).IsEmpty())
        return std::nullopt;
    return std::exchange(result_hint, std::nullopt);
}

HostLoc RegAlloc::ScratchImpl(BlockOfCode& code, std::bitset<32> desired_locations) noexcept {
    const auto hinted = TakeEmptyResultHint(desired_locations);
    const HostLoc location = hinted ? *hinted : SelectARegister(desired_locations);
    MoveOutOfTheWay(code, location);
    LocInfo(location).WriteLock();
    return location;
}

void RegAlloc::HostCall(
    BlockOfCode& code,
    IR::Inst* result_def,
    const std::optional<Argument::copyable_reference> arg0,
    const std::optional<Argument::copyable_reference> arg1,
    const std::optional<Argument::copyable_reference> arg2,
    const std::optional<Argument::copyable_reference> arg3
) noexcept {
    constexpr size_t args_count = 4;
    constexpr std::array<HostLoc, args_count> args_hostloc = {ABI_PARAM1, ABI_PARAM2, ABI_PARAM3, ABI_PARAM4};
    const std::array<std::optional<Argument::copyable_reference>, args_count> args = {arg0, arg1, arg2, arg3};

    static const std::bitset<32> other_caller_save = [args_hostloc]() noexcept {
        std::bitset<32> ret = ABI_ALL_CALLER_SAVE;
        ret.reset(size_t(ABI_RETURN));
        for (auto const hostloc : args_hostloc)
            ret.reset(size_t(hostloc));
        return ret;
    }();

    // Calls flush and reload the A64 register cache, and supervisor calls may
    // change guest registers: values aliasing pinned registers move out first.
    for (size_t i = 0; i < 16; ++i) {
        const auto loc = HostLoc(i);
        if (IsPinned(loc) && loc != HostLoc::RSP && loc != ABI_JIT_PTR && !LocInfo(loc).IsEmpty() && !LocInfo(loc).IsLocked())
            MoveOutOfTheWay(code, loc);
    }
    ScratchGpr(code, ABI_RETURN);
    if (result_def)
        DefineValueImpl(code, result_def, ABI_RETURN);

    for (size_t i = 0; i < args.size(); i++) {
        if (args[i]) {
            UseScratch(code, *args[i], args_hostloc[i]);
        } else {
            ScratchGpr(code, args_hostloc[i]); // TODO: Force spill
        }
    }
    // Must match with with ScratchImpl
    for (size_t i = 0; i < other_caller_save.size(); ++i) {
        if (other_caller_save[i]) {
            MoveOutOfTheWay(code, HostLoc(i));
            LocInfo(HostLoc(i)).WriteLock();
        }
    }
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] && !args[i]->get().IsVoid()) {
            // LLVM puts the burden of zero-extension of 8 and 16 bit values on the caller instead of the callee
            const Xbyak::Reg64 reg = HostLocToReg64(args_hostloc[i]);
            switch (args[i]->get().GetType()) {
            case IR::Type::U8:
                code.movzx(reg.cvt32(), reg.cvt8());
                break;
            case IR::Type::U16:
                code.movzx(reg.cvt32(), reg.cvt16());
                break;
            case IR::Type::U32:
                code.mov(reg.cvt32(), reg.cvt32());
                break;
            case IR::Type::U64:
                break; //no op
            default:
                UNREACHABLE();
            }
        }
    }
}

void RegAlloc::AllocStackSpace(BlockOfCode& code, const size_t stack_space) noexcept {
    ASSERT(stack_space < size_t((std::numeric_limits<s32>::max)()));
    ASSERT(reserved_stack_space == 0);
    reserved_stack_space = stack_space;
    code.sub(code.rsp, u32(stack_space));
#ifdef _WIN32
    reserved_stack_begin = code.getCurr();
#endif
}

void RegAlloc::ReleaseStackSpace(BlockOfCode& code, const size_t stack_space) {
    ASSERT(stack_space < size_t((std::numeric_limits<s32>::max)()));
    ASSERT(reserved_stack_space == stack_space);
    reserved_stack_space = 0;
    code.add(code.rsp, u32(stack_space));
#ifdef _WIN32
    ABIPrologue prologue;
    prologue.operations.push_back({ABIPrologue::Kind::Allocate, 0, 0, stack_space});
    const auto* frame_end = code.getCurr();
    Xbyak::Label after_metadata;
    code.jmp(after_metadata, code.T_NEAR);
    code.RegisterNativeUnwindFrame(reserved_stack_begin, frame_end, prologue, true);
    code.L(after_metadata);
#endif
}

HostLoc RegAlloc::SelectARegister(std::bitset<32> desired_locations) const noexcept {
    static_assert(size_t(HostLoc::FirstSpill) >= 32);
    // An empty register first: Intel recommends reusing free registers over
    // spilling. Otherwise evict the register whose values are needed furthest
    // ahead (Belady's rule, the fewest reloads within a block); among equals,
    // one without a REX prefix.
    auto selected = HostLoc::FirstSpill;
    u32 selected_next_use = 0;
    bool selected_rex = true;
    for (size_t index = 0; index < desired_locations.size(); ++index) {
        if (!desired_locations.test(index))
            continue;
        const HostLoc i = HostLoc(index);
        const auto& loc_info = LocInfo(i);
        DEBUG_ASSERT(i != ABI_JIT_PTR);
        // R13 and R14 are technically available but never allocated: skipping
        // them is better than spilling all over the place. RBP must not be trashed.
        if (loc_info.IsLocked() || i == HostLoc::RBP || (i >= HostLoc::R13 && i <= HostLoc::R15))
            continue;
        if (loc_info.IsEmpty())
            return i;
        const u32 next_use = NextUse(loc_info);
        const bool rex = i >= HostLoc::R8 && i <= HostLoc::R15;
        if (selected == HostLoc::FirstSpill || next_use > selected_next_use ||
            (next_use == selected_next_use && selected_rex && !rex)) {
            selected = i;
            selected_next_use = next_use;
            selected_rex = rex;
        }
    }
    ASSERT(selected != HostLoc::FirstSpill && "All candidate registers have already been allocated");
    return selected;
}

u32 RegAlloc::NextUse(const HostLocInfo& info) const noexcept {
    u32 next = NextUseTable::no_use;
    if (next_uses)
        for (const IR::Inst* value : info.values)
            next = std::min(next, next_uses->NextUse(value, current_position));
    return next;
}

void NextUseTable::Build(IR::Block& block) {
    insts.clear();
    u32 position = 0;
    for (auto& inst : block.instructions) {
        inst.SetEmitPosition(position++);
        insts.push_back(&inst);
    }
    // Only this block's positions are read (Position checks insts), so clear just those: the table keeps the
    // largest block's size for the emitter's lifetime, and clearing all of it made every compile pay for it.
    if (uses.size() < position)
        uses.resize(position);
    for (u32 i = 0; i < position; ++i)
        uses[i].clear();
    position = 0;
    for (const auto& inst : block.instructions) {
        // An A64ColdSet* record names the location an abort exit reads, which may
        // be a spill slot: it needs no register, so it is not a use here.
        switch (inst.GetOpcode()) {
        case IR::Opcode::A64ColdSetW:
        case IR::Opcode::A64ColdSetX:
        case IR::Opcode::A64ColdSetS:
        case IR::Opcode::A64ColdSetD:
        case IR::Opcode::A64ColdSetQ:
        case IR::Opcode::A64ColdSetSP:
        case IR::Opcode::A64ColdSetNZCV:
            ++position;
            continue;
        default:
            break;
        }
        for (size_t i = 0; i < inst.NumArgs(); ++i) {
            const IR::Value arg = inst.GetArg(i);
            if (arg.IsImmediate())
                continue;
            if (const auto found = Position(arg.GetInst()))
                uses[*found].push_back(position);
        }
        ++position;
    }
}

std::optional<u32> NextUseTable::Position(const IR::Inst* inst) const noexcept {
    const u32 position = inst->GetEmitPosition();
    return position < insts.size() && insts[position] == inst ? std::optional<u32>{position} : std::nullopt;
}

u32 NextUseTable::NextUse(const IR::Inst* value, u32 position) const noexcept {
    const auto found = Position(value);
    if (!found)
        return no_use;
    const auto& list = uses[*found];
    const auto use = std::lower_bound(list.begin(), list.end(), position);
    return use == list.end() ? no_use : *use;
}

std::optional<HostLoc> RegAlloc::ValueLocation(const IR::Inst* value) const noexcept {
    for (size_t i = 0; i < hostloc_info.size(); i++)
        if (hostloc_info[i].ContainsValue(value)) {
            //for (size_t j = 0; j < hostloc_info.size(); ++j)
            //    ASSERT((i == j || !hostloc_info[j].ContainsValue(value)) && "duplicate defs");
            return HostLoc(i);
        }
    return std::nullopt;
}

// The single-definition checks scan every register and spill slot, twice per defined value; they run with
// VENTURE_JIT_CHECK_ALLOC_SCOPE=1 (tests) rather than on every cold compile.
void RegAlloc::DefineValueImpl(BlockOfCode& code, IR::Inst* def_inst, HostLoc host_loc) noexcept {
    if (check_untouched_slots)
        ASSERT(!ValueLocation(def_inst) && "def_inst has already been defined");
    LocInfo(host_loc).AddValue(host_loc, def_inst);
    if (check_untouched_slots)
        ASSERT(*ValueLocation(def_inst) == host_loc);
}

void RegAlloc::DefineValueImpl(BlockOfCode& code, IR::Inst* def_inst, const IR::Value& use_inst) noexcept {
    if (check_untouched_slots)
        ASSERT(!ValueLocation(def_inst) && "def_inst has already been defined");
    if (use_inst.IsImmediate()) {
        const HostLoc location = ScratchImpl(code, gpr_order);
        DefineValueImpl(code, def_inst, location);
        LoadImmediate(code, use_inst, location);
    } else {
        const auto location = ValueLocation(use_inst.GetInst());
        ASSERT(location && "use_inst must already be defined");
        DefineValueImpl(code, def_inst, *location);
    }
}

void RegAlloc::Move(BlockOfCode& code, HostLoc to, HostLoc from) noexcept {
    const size_t bit_width = LocInfo(from).GetMaxBitWidth();
    ASSERT(LocInfo(to).IsEmpty() && !LocInfo(from).IsLocked());
    ASSERT(bit_width <= HostLocBitWidth(to));
    ASSERT(!LocInfo(from).IsEmpty() && "Mov eliminated");
    EmitMove(code, bit_width, to, from);
    LocInfo(to) = std::exchange(LocInfo(from), {});
}

void RegAlloc::CopyToScratch(BlockOfCode& code, size_t bit_width, HostLoc to, HostLoc from) noexcept {
    ASSERT(LocInfo(to).IsEmpty() && !LocInfo(from).IsEmpty());
    EmitMove(code, bit_width, to, from);
}

void RegAlloc::Exchange(BlockOfCode& code, HostLoc a, HostLoc b) noexcept {
    ASSERT(!LocInfo(a).IsLocked() && !LocInfo(b).IsLocked());
    ASSERT(LocInfo(a).GetMaxBitWidth() <= HostLocBitWidth(b));
    ASSERT(LocInfo(b).GetMaxBitWidth() <= HostLocBitWidth(a));

    if (LocInfo(a).IsEmpty()) {
        Move(code, a, b);
    } else if (LocInfo(b).IsEmpty()) {
        Move(code, b, a);
    } else {
        EmitExchange(code, a, b);
        std::swap(LocInfo(a), LocInfo(b));
    }
}

void RegAlloc::MoveOutOfTheWay(BlockOfCode& code, HostLoc reg) noexcept {
    ASSERT(!LocInfo(reg).IsLocked());
    if (!LocInfo(reg).IsEmpty()) {
        const auto& order = HostLocIsGPR(reg) ? gpr_order : xmm_order;
        for (size_t index = 0; index < order.size(); ++index) {
            if (!order[index]) continue;
            const auto free = HostLoc(index);
            if (LocInfo(free).IsEmpty() && !LocInfo(free).IsLocked()) {
                Move(code, free, reg);
                return;
            }
        }
        SpillRegister(code, reg);
    }
}

void RegAlloc::SpillRegister(BlockOfCode& code, HostLoc loc) noexcept {
    ASSERT(HostLocIsRegister(loc) && "Only registers can be spilled");
    ASSERT(!LocInfo(loc).IsEmpty() && "There is no need to spill unoccupied registers");
    ASSERT(!LocInfo(loc).IsLocked() && "Registers that have been allocated must not be spilt");
    auto const new_loc = FindFreeSpill(HostLocIsXMM(loc));
    Move(code, new_loc, loc);
}

HostLoc RegAlloc::FindFreeSpill(bool is_xmm) const noexcept {
    // TODO(lizzie): Ok, Windows hates XMM spills, this means less perf for windows
    // but it's fine anyways. We can find other ways to cheat it later - but which?!?!
    // we should NOT save xmm each block entering... MAYBE xbyak has a bug on start/end?
    // TODO(lizzie): This needs to be investigated further later.
    // Do not spill XMM into other XMM silly
    /*if (!is_xmm) {
        // TODO(lizzie): Using lower (xmm0 and such) registers results in issues/crashes - INVESTIGATE WHY
        // Intel recommends to spill GPR onto XMM registers IF POSSIBLE
        // TODO(lizzie): Issues on DBZ, theory: Scratch XMM not properly restored after a function call?
        // Must sync with ABI registers (except XMM0, XMM1 and XMM2)
        for (size_t i = size_t(HostLoc::XMM15); i >= size_t(HostLoc::XMM3); --i)
            if (const auto loc = HostLoc(i); LocInfo(loc).IsEmpty())
                return loc;
    }*/
    // TODO: Doing this would mean saving XMM on each call... need to benchmark the benefits
    // of spilling on XMM versus the potential cost of using XMM registers.....
    // Otherwise go to stack spilling
    for (size_t i = size_t(HostLoc::FirstSpill); i < hostloc_info.size(); ++i)
        if (const auto loc = HostLoc(i); LocInfo(loc).IsEmpty())
            return loc;
    UNREACHABLE();
}

#define MAYBE_AVX(OPCODE, ...) \
    [&] { \
        if (code.HasHostFeature(HostFeature::AVX)) code.v##OPCODE(__VA_ARGS__); \
        else code.OPCODE(__VA_ARGS__); \
    }()

HostLoc RegAlloc::LoadImmediate(BlockOfCode& code, IR::Value imm, HostLoc host_loc) noexcept {
    ASSERT(imm.IsImmediate() && "imm is not an immediate");
    if (HostLocIsGPR(host_loc)) {
        const Xbyak::Reg64 reg = HostLocToReg64(host_loc);
        const u64 imm_value = imm.GetImmediateAsU64();
        if (imm_value == 0) {
            code.xor_(reg.cvt32(), reg.cvt32());
        } else {
            code.mov(reg, imm_value);
        }
    } else if (HostLocIsXMM(host_loc)) {
        const Xbyak::Xmm reg = HostLocToXmm(host_loc);
        const u64 imm_value = imm.GetImmediateAsU64();
        if (imm_value == 0) {
            MAYBE_AVX(xorps, reg, reg);
        } else {
            MAYBE_AVX(movaps, reg, code.Const(code.xword, imm_value));
        }
    } else {
        UNREACHABLE();
    }
    return host_loc;
}

void RegAlloc::EmitMove(BlockOfCode& code, const size_t bit_width, const HostLoc to, const HostLoc from) noexcept {
    auto const spill_to_op_arg_helper = [&](HostLoc loc, size_t reserved_stack_space) {
        ASSERT(HostLocIsSpill(loc));
        size_t i = size_t(loc) - size_t(HostLoc::FirstSpill);
        ASSERT(i < SpillCount && "Spill index greater than number of available spill locations");
        return Xbyak::util::rsp + reserved_stack_space + ABI_SHADOW_SPACE + offsetof(StackLayout, spill) + i * sizeof(StackLayout::spill[0]);
    };
    auto const spill_xmm_to_op = [&](const HostLoc loc) {
        return Xbyak::util::xword[spill_to_op_arg_helper(loc, reserved_stack_space)];
    };
    if (HostLocIsXMM(to) && HostLocIsXMM(from)) {
        MAYBE_AVX(movaps, HostLocToXmm(to), HostLocToXmm(from));
    } else if (HostLocIsGPR(to) && HostLocIsGPR(from)) {
        ASSERT(bit_width != 128);
        if (bit_width == 64) {
            code.mov(HostLocToReg64(to), HostLocToReg64(from));
        } else {
            code.mov(HostLocToReg64(to).cvt32(), HostLocToReg64(from).cvt32());
        }
    } else if (HostLocIsXMM(to) && HostLocIsGPR(from)) {
        ASSERT(bit_width != 128);
        if (bit_width == 64) {
            MAYBE_AVX(movq, HostLocToXmm(to), HostLocToReg64(from));
        } else {
            MAYBE_AVX(movd, HostLocToXmm(to), HostLocToReg64(from).cvt32());
        }
    } else if (HostLocIsGPR(to) && HostLocIsXMM(from)) {
        ASSERT(bit_width != 128);
        if (bit_width == 64) {
            MAYBE_AVX(movq, HostLocToReg64(to), HostLocToXmm(from));
        } else {
            MAYBE_AVX(movd, HostLocToReg64(to).cvt32(), HostLocToXmm(from));
        }
    } else if (HostLocIsXMM(to) && HostLocIsSpill(from)) {
        const Xbyak::Address spill_addr = spill_xmm_to_op(from);
        ASSERT(spill_addr.getBit() >= bit_width);
        switch (bit_width) {
        case 128:
            MAYBE_AVX(movaps, HostLocToXmm(to), spill_addr);
            break;
        case 64:
            MAYBE_AVX(movsd, HostLocToXmm(to), spill_addr);
            break;
        case 32:
        case 16:
        case 8:
            MAYBE_AVX(movss, HostLocToXmm(to), spill_addr);
            break;
        default:
            UNREACHABLE();
        }
    } else if (HostLocIsSpill(to) && HostLocIsXMM(from)) {
        const Xbyak::Address spill_addr = spill_xmm_to_op(to);
        ASSERT(spill_addr.getBit() >= bit_width);
        switch (bit_width) {
        case 128:
            MAYBE_AVX(movaps, spill_addr, HostLocToXmm(from));
            break;
        case 64:
            MAYBE_AVX(movsd, spill_addr, HostLocToXmm(from));
            break;
        case 32:
        case 16:
        case 8:
            MAYBE_AVX(movss, spill_addr, HostLocToXmm(from));
            break;
        default:
            UNREACHABLE();
        }
    } else if (HostLocIsGPR(to) && HostLocIsSpill(from)) {
        ASSERT(bit_width != 128);
        if (bit_width == 64) {
            code.mov(HostLocToReg64(to), Xbyak::util::qword[spill_to_op_arg_helper(from, reserved_stack_space)]);
        } else {
            code.mov(HostLocToReg64(to).cvt32(), Xbyak::util::dword[spill_to_op_arg_helper(from, reserved_stack_space)]);
        }
    } else if (HostLocIsSpill(to) && HostLocIsGPR(from)) {
        ASSERT(bit_width != 128);
        if (bit_width == 64) {
            code.mov(Xbyak::util::qword[spill_to_op_arg_helper(to, reserved_stack_space)], HostLocToReg64(from));
        } else {
            code.mov(Xbyak::util::dword[spill_to_op_arg_helper(to, reserved_stack_space)], HostLocToReg64(from).cvt32());
        }
    } else {
        UNREACHABLE();
    }
}
#undef MAYBE_AVX

void RegAlloc::EmitExchange(BlockOfCode& code, const HostLoc a, const HostLoc b) noexcept {
    ASSERT(HostLocIsGPR(a) && HostLocIsGPR(b) && "Exchanging XMM registers is uneeded OR invalid emit");
    code.xchg(HostLocToReg64(a), HostLocToReg64(b));
}

}  // namespace Dynarmic::Backend::X64
