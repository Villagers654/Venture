// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/arm64/reg_alloc.h"

#include <algorithm>
#include <array>
#include <iterator>

#include "common/assert.h"
#include "dynarmic/mcl/bit.hpp"
#include <bit>
#include "common/common_types.h"

#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/emit_context.h"
#include "dynarmic/backend/arm64/fpsr_manager.h"
#include "dynarmic/backend/arm64/verbose_debugging_output.h"
#include "dynarmic/common/always_false.h"

namespace Dynarmic::Backend::Arm64 {

using namespace oaknut::util;

constexpr std::size_t spill_offset = offsetof(StackLayout, spill);
constexpr std::size_t spill_slot_size = sizeof(decltype(StackLayout::spill)::value_type);

static bool IsValuelessType(IR::Type type) {
    switch (type) {
    case IR::Type::Table:
        return true;
    default:
        return false;
    }
}

IR::Type Argument::GetType() const {
    return value.GetType();
}

bool Argument::IsImmediate() const {
    return value.IsImmediate();
}

bool Argument::GetImmediateU1() const {
    return value.GetU1();
}

u8 Argument::GetImmediateU8() const {
    const u64 imm = value.GetImmediateAsU64();
    ASSERT(imm < 0x100);
    return u8(imm);
}

u16 Argument::GetImmediateU16() const {
    const u64 imm = value.GetImmediateAsU64();
    ASSERT(imm < 0x10000);
    return u16(imm);
}

u32 Argument::GetImmediateU32() const {
    const u64 imm = value.GetImmediateAsU64();
    ASSERT(imm < 0x100000000);
    return u32(imm);
}

u64 Argument::GetImmediateU64() const {
    return value.GetImmediateAsU64();
}

IR::Cond Argument::GetImmediateCond() const {
    ASSERT(IsImmediate() && GetType() == IR::Type::Cond);
    return value.GetCond();
}

IR::AccType Argument::GetImmediateAccType() const {
    ASSERT(IsImmediate() && GetType() == IR::Type::AccType);
    return value.GetAccType();
}

HostLoc::Kind Argument::CurrentLocationKind(RegAlloc& reg_alloc) const {
    return reg_alloc.ValueLocation(value.GetInst())->kind;
}

bool HostLocInfo::Contains(const IR::Inst* value) const {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void HostLocInfo::SetupScratchLocation() {
    ASSERT(IsCompletelyEmpty());
    realized = true;
}

void HostLocInfo::SetupLocation(const IR::Inst* value) {
    ASSERT(IsCompletelyEmpty());
    values.clear();
    values.push_back(value);
    realized = true;
    uses_this_inst = 0;
    accumulated_uses = 0;
    expected_uses = value->UseCount();
}

bool HostLocInfo::IsCompletelyEmpty() const {
    return values.empty() && !locked && !realized && !accumulated_uses && !expected_uses && !uses_this_inst;
}

bool HostLocInfo::MaybeAllocatable() const {
    return !locked && !realized;
}

bool HostLocInfo::IsOneRemainingUse() const {
    return accumulated_uses + 1 == expected_uses && uses_this_inst == 1;
}

void HostLocInfo::UpdateUses() {
    accumulated_uses += uses_this_inst;
    uses_this_inst = 0;

    if (accumulated_uses == expected_uses) {
        values.clear();
        accumulated_uses = 0;
        expected_uses = 0;
    }
}

RegAlloc::ArgumentInfo RegAlloc::GetArgumentInfo(IR::Inst* inst) {
    ArgumentInfo ret = {Argument{}, Argument{}, Argument{}, Argument{}};
    for (std::size_t i = 0; i < inst->NumArgs(); i++) {
        const IR::Value arg = inst->GetArg(i);
        ret[i].value = arg;
        if (!arg.IsImmediate() && !IsValuelessType(arg.GetType())) {
            ASSERT(ValueLocation(arg.GetInst()) && "argument must already been defined");
            ValueInfo(arg.GetInst()).uses_this_inst++;
        }
    }
    return ret;
}

std::optional<HostLoc> RegAlloc::UseInPlace(const IR::Value& value) {
    if (value.IsImmediate()) {
        return std::nullopt;
    }
    const auto location = ValueLocation(value.GetInst());
    if (location) {
        ValueInfo(value.GetInst()).uses_this_inst++;
    }
    return location;
}

void RegAlloc::MarkRematerializable(const IR::Inst* inst, u32 offset, bool word) {
    if (InTable(inst)) {
        remat_by_position[inst->GetEmitPosition()] = (offset + 1) | (word ? u32(1) << 31 : 0);
    }
}

u32 RegAlloc::RematCode(const HostLocInfo& info) const {
    if (info.values.size() != 1 || !InTable(info.values[0])) {
        return 0;
    }
    return remat_by_position[info.values[0]->GetEmitPosition()];
}

std::optional<RegAlloc::Remat> RegAlloc::SpillRematerialization(int spill_index) const {
    const u32 remat = spill_remat[static_cast<std::size_t>(spill_index)];
    if (!remat) {
        return std::nullopt;
    }
    return Remat{(remat & ~(u32(1) << 31)) - 1, (remat >> 31) != 0};
}

void RegAlloc::LoadSpill(oaknut::XReg reg, int spill_index, bool word) {
    if (const auto remat = SpillRematerialization(spill_index)) {
        if (remat->word) {
            code.LDR(reg.toW(), Xstate, remat->offset);
        } else {
            code.LDR(reg, Xstate, remat->offset);
        }
        return;
    }
    if (word) {
        code.LDR(reg.toW(), SP, spill_offset + spill_index * spill_slot_size);
        return;
    }
    code.LDR(reg, SP, spill_offset + spill_index * spill_slot_size);
}

std::optional<u32> RegAlloc::LastUse(const IR::Inst* inst) const {
    if (!InTable(inst) || next_use_positions[inst->GetEmitPosition()].empty()) {
        return std::nullopt;
    }
    return next_use_positions[inst->GetEmitPosition()].back();
}

bool RegAlloc::WasValueDefined(IR::Inst* inst) const {
    if (InTable(inst)) {
        return defined_by_position[inst->GetEmitPosition()] != 0;
    }
    return defined_insts.count(inst) > 0;
}

void RegAlloc::MarkDefined(const IR::Inst* inst) {
    if (InTable(inst)) {
        defined_by_position[inst->GetEmitPosition()] = 1;
    } else {
        defined_insts.insert(inst);
    }
}

void RegAlloc::PrepareForCall(std::optional<Argument::copyable_reference> arg0, std::optional<Argument::copyable_reference> arg1, std::optional<Argument::copyable_reference> arg2, std::optional<Argument::copyable_reference> arg3) {
    fpsr_manager.Spill();
    SpillFlags();

    // TODO: Spill into callee-save registers

    for (int i = 0; i < 32; i++) {
        if (mcl::bit::get_bit(i, static_cast<u32>(ABI_CALLER_SAVE))) {
            SpillGpr(i);
        }
    }

    for (int i = 0; i < 32; i++) {
        if (mcl::bit::get_bit(i, static_cast<u32>(ABI_CALLER_SAVE >> 32))) {
            SpillFpr(i);
        }
    }

    const std::array<std::optional<Argument::copyable_reference>, 4> args{arg0, arg1, arg2, arg3};

    // AAPCS64 Next General-purpose Register Number
    int ngrn = 0;
    // AAPCS64 Next SIMD and Floating-point Register Number
    int nsrn = 0;

    for (int i = 0; i < 4; i++) {
        if (args[i]) {
            if (args[i]->get().GetType() == IR::Type::U128) {
                ASSERT(fprs[nsrn].IsCompletelyEmpty());
                LoadCopyInto(args[i]->get().value, oaknut::QReg{nsrn});
                nsrn++;
            } else {
                ASSERT(gprs[ngrn].IsCompletelyEmpty());
                LoadCopyInto(args[i]->get().value, oaknut::XReg{ngrn});
                ngrn++;
            }
        } else {
            // Gaps are assumed to be in general-purpose registers
            // TODO: should there be a separate list passed for FPRs instead?
            ngrn++;
        }
    }
}

void RegAlloc::DefineAsExisting(IR::Inst* inst, Argument& arg) {
    MarkDefined(inst);
    ASSERT(!ValueLocation(inst));

    if (arg.value.IsImmediate()) {
        inst->ReplaceUsesWith(arg.value);
        return;
    }

    auto& info = ValueInfo(arg.value.GetInst());
    info.values.push_back(inst);
    info.expected_uses += inst->UseCount();
}

Argument RegAlloc::UseArgument(const IR::Value& value) {
    Argument arg;
    arg.value = value;
    if (!value.IsImmediate() && !IsValuelessType(value.GetType())) {
        ASSERT(ValueLocation(value.GetInst()) && "argument must already been defined");
        ValueInfo(value.GetInst()).uses_this_inst++;
    }
    return arg;
}

bool RegAlloc::IsLastUse(const Argument& arg) {
    if (arg.value.IsImmediate()) {
        return false;
    }
    const auto location = ValueLocation(arg.value.GetInst());
    if (!location) {
        return false;
    }
    const HostLocInfo& info = ValueInfo(*location);
    return info.accumulated_uses + info.uses_this_inst == info.expected_uses;
}

void RegAlloc::DefineAsRegister(IR::Inst* inst, oaknut::Reg reg) {
    MarkDefined(inst);
    ASSERT(!ValueLocation(inst));
    auto& info = reg.is_vector() ? fprs[reg.index()] : gprs[reg.index()];
    ASSERT(info.IsCompletelyEmpty());
    info.values.push_back(inst);
    info.expected_uses += inst->UseCount();
}

void RegAlloc::UpdateAllUses() {
    for (auto& gpr : gprs) {
        gpr.UpdateUses();
    }
    for (auto& fpr : fprs) {
        fpr.UpdateUses();
    }
    flags.UpdateUses();
    for (std::size_t i = 0; i < spill_high; i++) {
        spills[i].UpdateUses();
    }
}

void RegAlloc::AssertAllUnlocked() const {
    const auto is_unlocked = [](const auto& i) { return !i.locked && !i.realized; };
    ASSERT(std::all_of(gprs.begin(), gprs.end(), is_unlocked));
    ASSERT(std::all_of(fprs.begin(), fprs.end(), is_unlocked));
    ASSERT(is_unlocked(flags));
    ASSERT(std::all_of(spills.begin(), spills.begin() + spill_high, is_unlocked));
}

void RegAlloc::AssertNoMoreUses() const {
    const auto is_empty = [](const auto& i) { return i.IsCompletelyEmpty(); };
    ASSERT(std::all_of(gprs.begin(), gprs.end(), is_empty));
    ASSERT(std::all_of(fprs.begin(), fprs.end(), is_empty));
    ASSERT(is_empty(flags));
    ASSERT(std::all_of(spills.begin(), spills.begin() + spill_high, is_empty));
}

void RegAlloc::EmitVerboseDebuggingOutput() {
    code.MOV(X19, std::bit_cast<u64>(&PrintVerboseDebuggingOutputLine));  // Non-volatile register

    const auto do_location = [&](HostLocInfo& info, HostLocType type, std::size_t index) {
        using namespace oaknut::util;
        for (const IR::Inst* value : info.values) {
            code.MOV(X0, SP);
            code.MOV(X1, static_cast<u64>(type));
            code.MOV(X2, index);
            code.MOV(X3, value->GetName());
            code.MOV(X4, static_cast<u64>(value->GetType()));
            code.BLR(X19);
        }
    };

    for (std::size_t i = 0; i < gprs.size(); i++) {
        do_location(gprs[i], HostLocType::X, i);
    }
    for (std::size_t i = 0; i < fprs.size(); i++) {
        do_location(fprs[i], HostLocType::Q, i);
    }
    do_location(flags, HostLocType::Nzcv, 0);
    for (std::size_t i = 0; i < spills.size(); i++) {
        do_location(spills[i], HostLocType::Spill, i);
    }
}

template<HostLoc::Kind kind>
int RegAlloc::GenerateImmediate(const IR::Value& value) {
    ASSERT(value.GetType() != IR::Type::U1);
    if constexpr (kind == HostLoc::Kind::Gpr) {
        const int new_location_index = AllocateRegister(gprs, gpr_order);
        SpillGpr(new_location_index);
        gprs[new_location_index].SetupScratchLocation();

        code.MOV(oaknut::XReg{new_location_index}, value.GetImmediateAsU64());

        return new_location_index;
    } else if constexpr (kind == HostLoc::Kind::Fpr) {
        const int new_location_index = AllocateRegister(fprs, fpr_order);
        SpillFpr(new_location_index);
        fprs[new_location_index].SetupScratchLocation();

        code.MOV(Xscratch0, value.GetImmediateAsU64());
        code.FMOV(oaknut::DReg{new_location_index}, Xscratch0);

        return new_location_index;
    } else if constexpr (kind == HostLoc::Kind::Flags) {
        SpillFlags();
        flags.SetupScratchLocation();

        code.MOV(Xscratch0, value.GetImmediateAsU64());
        code.MSR(oaknut::SystemReg::NZCV, Xscratch0);

        return 0;
    } else {
        UNREACHABLE();
    }
}

template<HostLoc::Kind required_kind>
int RegAlloc::RealizeReadImpl(const IR::Value& value) {
    if (value.IsImmediate()) {
        return GenerateImmediate<required_kind>(value);
    }

    const auto current_location = ValueLocation(value.GetInst());
    ASSERT(current_location);

    if (current_location->kind == required_kind) {
        ValueInfo(*current_location).realized = true;
        return current_location->index;
    }

    ASSERT(!bool(ValueInfo(*current_location).realized));
    ASSERT(bool(ValueInfo(*current_location).locked));

    if constexpr (required_kind == HostLoc::Kind::Gpr) {
        const int new_location_index = AllocateRegister(gprs, gpr_order);
        SpillGpr(new_location_index);

        switch (current_location->kind) {
        case HostLoc::Kind::Gpr:
            UNREACHABLE(); //logic error
        case HostLoc::Kind::Fpr:
            // Venture: only the low 32 bits of a U32 in an FPR are defined (a
            // LowLaneAliasing lane 0 keeps the vector's other lanes); a W move
            // zero-extends it, as every U32 in a GPR is.
            if (value.GetType() == IR::Type::U32) {
                code.FMOV(oaknut::WReg{new_location_index}, oaknut::SReg{current_location->index});
            } else {
                code.FMOV(oaknut::XReg{new_location_index}, oaknut::DReg{current_location->index});
            }
            break;
        case HostLoc::Kind::Spill:
            LoadSpill(oaknut::XReg{new_location_index}, current_location->index, value.GetType() == IR::Type::U32);
            break;
        case HostLoc::Kind::Flags:
            code.MRS(oaknut::XReg{new_location_index}, oaknut::SystemReg::NZCV);
            break;
        }

        gprs[new_location_index] = std::exchange(ValueInfo(*current_location), {});
        gprs[new_location_index].realized = true;
        return new_location_index;
    } else if constexpr (required_kind == HostLoc::Kind::Fpr) {
        const int new_location_index = AllocateRegister(fprs, fpr_order);
        SpillFpr(new_location_index);

        switch (current_location->kind) {
        case HostLoc::Kind::Gpr:
            code.FMOV(oaknut::DReg{new_location_index}, oaknut::XReg{current_location->index});
            break;
        case HostLoc::Kind::Fpr:
            UNREACHABLE(); //logic error
        case HostLoc::Kind::Spill:
            if (const auto remat = SpillRematerialization(current_location->index)) {
                if (remat->word) {
                    code.LDR(oaknut::SReg{new_location_index}, Xstate, remat->offset);
                } else {
                    code.LDR(oaknut::DReg{new_location_index}, Xstate, remat->offset);
                }
            } else {
                code.LDR(oaknut::QReg{new_location_index}, SP, spill_offset + current_location->index * spill_slot_size);
            }
            break;
        case HostLoc::Kind::Flags:
            ASSERT(false && "Moving from flags into fprs is not currently supported");
            break;
        }

        fprs[new_location_index] = std::exchange(ValueInfo(*current_location), {});
        fprs[new_location_index].realized = true;
        return new_location_index;
    } else if constexpr (required_kind == HostLoc::Kind::Flags) {
        UNREACHABLE(); //A simple read from flags is likely a logic error
    } else {
        UNREACHABLE();
    }
}

template<HostLoc::Kind kind>
int RegAlloc::RealizeWriteImpl(const IR::Inst* value) {
    MarkDefined(value);
    ASSERT(!ValueLocation(value));

    if constexpr (kind == HostLoc::Kind::Gpr) {
        const int new_location_index = AllocateRegister(gprs, gpr_order);
        SpillGpr(new_location_index);
        gprs[new_location_index].SetupLocation(value);
        return new_location_index;
    } else if constexpr (kind == HostLoc::Kind::Fpr) {
        const int new_location_index = AllocateRegister(fprs, fpr_order);
        SpillFpr(new_location_index);
        fprs[new_location_index].SetupLocation(value);
        return new_location_index;
    } else if constexpr (kind == HostLoc::Kind::Flags) {
        SpillFlags();
        flags.SetupLocation(value);
        return 0;
    } else {
        UNREACHABLE();
    }
}

template<HostLoc::Kind kind>
int RegAlloc::RealizeReadWriteImpl(const IR::Value& read_value, const IR::Inst* write_value) {
    MarkDefined(write_value);
    // TODO: Move elimination

    const int write_loc = RealizeWriteImpl<kind>(write_value);

    if constexpr (kind == HostLoc::Kind::Gpr) {
        LoadCopyInto(read_value, oaknut::XReg{write_loc});
        return write_loc;
    } else if constexpr (kind == HostLoc::Kind::Fpr) {
        LoadCopyInto(read_value, oaknut::QReg{write_loc});
        return write_loc;
    } else if constexpr (kind == HostLoc::Kind::Flags) {
        ASSERT(false && "Incorrect function for ReadWrite of flags");
    } else {
        UNREACHABLE();
    }
}

template int RegAlloc::RealizeReadImpl<HostLoc::Kind::Gpr>(const IR::Value& value);
template int RegAlloc::RealizeReadImpl<HostLoc::Kind::Fpr>(const IR::Value& value);
template int RegAlloc::RealizeReadImpl<HostLoc::Kind::Flags>(const IR::Value& value);
template int RegAlloc::RealizeWriteImpl<HostLoc::Kind::Gpr>(const IR::Inst* value);
template int RegAlloc::RealizeWriteImpl<HostLoc::Kind::Fpr>(const IR::Inst* value);
template int RegAlloc::RealizeWriteImpl<HostLoc::Kind::Flags>(const IR::Inst* value);
template int RegAlloc::RealizeReadWriteImpl<HostLoc::Kind::Gpr>(const IR::Value&, const IR::Inst*);
template int RegAlloc::RealizeReadWriteImpl<HostLoc::Kind::Fpr>(const IR::Value&, const IR::Inst*);
template int RegAlloc::RealizeReadWriteImpl<HostLoc::Kind::Flags>(const IR::Value&, const IR::Inst*);

int RegAlloc::AllocateRegister(const std::array<HostLocInfo, 32>& regs, const std::vector<int>& order) const {
    const auto empty = std::find_if(order.begin(), order.end(), [&](int i) { return regs[i].IsCompletelyEmpty(); });
    if (empty != order.end()) {
        return *empty;
    }

    std::vector<int> candidates;
    std::copy_if(order.begin(), order.end(), std::back_inserter(candidates), [&](int i) { return regs[i].MaybeAllocatable(); });
    ASSERT(!candidates.empty());

    if (next_use_insts.empty()) {
        return candidates[alloc_candidate_index++ % candidates.size()];
    }
    // Venture: evict the register needed furthest ahead (Belady), as on x64.
    int selected = candidates[0];
    u32 selected_next_use = NextUse(regs[selected]);
    for (std::size_t i = 1; i < candidates.size() && selected_next_use != no_use; i++) {
        const u32 next_use = NextUse(regs[candidates[i]]);
        if (next_use > selected_next_use) {
            selected = candidates[i];
            selected_next_use = next_use;
        }
    }
    return selected;
}

void RegAlloc::BuildNextUses(IR::Block& block) {
    next_use_insts.clear();
    u32 position = 0;
    for (auto& inst : block.instructions) {
        inst.SetEmitPosition(position++);
        next_use_insts.push_back(&inst);
    }
    defined_by_position.assign(position, 0);
    remat_by_position.assign(position, 0);
    if (next_use_positions.size() < position) {
        next_use_positions.resize(position);
    }
    for (u32 i = 0; i < position; i++) {
        next_use_positions[i].clear();
    }
    for (auto& inst : block.instructions) {
        for (std::size_t i = 0; i < inst.NumArgs(); i++) {
            const IR::Value arg = inst.GetArg(i);
            if (arg.IsImmediate()) {
                continue;
            }
            const IR::Inst* const value = arg.GetInstRecursive();
            const u32 at = value->GetEmitPosition();
            if (at < next_use_insts.size() && next_use_insts[at] == value) {
                next_use_positions[at].push_back(inst.GetEmitPosition());
            }
        }
    }
    current_position = 0;
}

void RegAlloc::AddUse(const IR::Inst* value, const IR::Inst* user) {
    const u32 at = value->GetEmitPosition();
    if (at >= next_use_insts.size() || next_use_insts[at] != value) {
        return;
    }
    auto& uses = next_use_positions[at];
    uses.insert(std::upper_bound(uses.begin(), uses.end(), user->GetEmitPosition()), user->GetEmitPosition());
}

u32 RegAlloc::NextUse(const HostLocInfo& info) const {
    u32 next = no_use;
    for (const IR::Inst* value : info.values) {
        const u32 at = value->GetEmitPosition();
        if (at >= next_use_insts.size() || next_use_insts[at] != value) {
            return 0;  // unknown: keep it
        }
        const auto& uses = next_use_positions[at];
        const auto use = std::lower_bound(uses.begin(), uses.end(), current_position);
        if (use != uses.end()) {
            next = std::min(next, *use);
        }
    }
    return next;
}

void RegAlloc::SpillGpr(int index) {
    ASSERT(!gprs[index].locked && !gprs[index].realized);
    if (gprs[index].values.empty()) {
        return;
    }
    const int new_location_index = FindFreeSpill();
    spill_remat[new_location_index] = RematCode(gprs[index]);
    if (!spill_remat[new_location_index]) {
        code.STR(oaknut::XReg{index}, SP, spill_offset + new_location_index * spill_slot_size);
    }
    spills[new_location_index] = std::exchange(gprs[index], {});
    spill_high = std::max<std::size_t>(spill_high, new_location_index + 1);
}

void RegAlloc::SpillFpr(int index) {
    ASSERT(!fprs[index].locked && !fprs[index].realized);
    if (fprs[index].values.empty()) {
        return;
    }
    const int new_location_index = FindFreeSpill();
    spill_remat[new_location_index] = RematCode(fprs[index]);
    if (!spill_remat[new_location_index]) {
        code.STR(oaknut::QReg{index}, SP, spill_offset + new_location_index * spill_slot_size);
    }
    spills[new_location_index] = std::exchange(fprs[index], {});
    spill_high = std::max<std::size_t>(spill_high, new_location_index + 1);
}

void RegAlloc::ReadWriteFlags(Argument& read, IR::Inst* write) {
    if (write) {
        MarkDefined(write);
    }
    const auto current_location = ValueLocation(read.value.GetInst());
    ASSERT(current_location);

    if (current_location->kind == HostLoc::Kind::Flags) {
        if (!flags.IsOneRemainingUse()) {
            SpillFlags();
        }
    } else if (current_location->kind == HostLoc::Kind::Gpr) {
        if (!flags.values.empty()) {
            SpillFlags();
        }
        code.MSR(oaknut::SystemReg::NZCV, oaknut::XReg{current_location->index});
    } else if (current_location->kind == HostLoc::Kind::Spill) {
        if (!flags.values.empty()) {
            SpillFlags();
        }
        LoadSpill(Xscratch0, current_location->index);
        code.MSR(oaknut::SystemReg::NZCV, Xscratch0);
    } else {
        UNREACHABLE(); //ASSERT(false && "Invalid current location for flags");
    }

    if (write) {
        flags.SetupLocation(write);
        flags.realized = false;
    }
}

void RegAlloc::SpillFlags() {
    ASSERT(!flags.locked && !flags.realized);
    if (flags.values.empty()) {
        return;
    }
    const int new_location_index = AllocateRegister(gprs, gpr_order);
    SpillGpr(new_location_index);
    code.MRS(oaknut::XReg{new_location_index}, oaknut::SystemReg::NZCV);
    gprs[new_location_index] = std::exchange(flags, {});
}

int RegAlloc::FindFreeSpill() const {
    const auto iter = std::find_if(spills.begin(), spills.end(), [](const HostLocInfo& info) { return info.values.empty(); });
    ASSERT(iter != spills.end() && "All spill locations are full");
    return static_cast<int>(iter - spills.begin());
}

void RegAlloc::LoadCopyInto(const IR::Value& value, oaknut::XReg reg) {
    if (value.IsImmediate()) {
        code.MOV(reg, value.GetImmediateAsU64());
        return;
    }

    const auto current_location = ValueLocation(value.GetInst());
    ASSERT(current_location);
    switch (current_location->kind) {
    case HostLoc::Kind::Gpr:
        code.MOV(reg, oaknut::XReg{current_location->index});
        break;
    case HostLoc::Kind::Fpr:
        if (value.GetType() == IR::Type::U32) {  // as in RealizeReadImpl
            code.FMOV(reg.toW(), oaknut::SReg{current_location->index});
        } else {
            code.FMOV(reg, oaknut::DReg{current_location->index});
        }
        break;
    case HostLoc::Kind::Spill:
        LoadSpill(reg, current_location->index, value.GetType() == IR::Type::U32);
        break;
    case HostLoc::Kind::Flags:
        code.MRS(reg, oaknut::SystemReg::NZCV);
        break;
    }
}

void RegAlloc::LoadCopyInto(const IR::Value& value, oaknut::QReg reg) {
    if (value.IsImmediate()) {
        code.MOV(Xscratch0, value.GetImmediateAsU64());
        code.FMOV(reg.toD(), Xscratch0);
        return;
    }

    const auto current_location = ValueLocation(value.GetInst());
    ASSERT(current_location);
    switch (current_location->kind) {
    case HostLoc::Kind::Gpr:
        code.FMOV(reg.toD(), oaknut::XReg{current_location->index});
        break;
    case HostLoc::Kind::Fpr:
        code.MOV(reg.B16(), oaknut::QReg{current_location->index}.B16());
        break;
    case HostLoc::Kind::Spill:
        // TODO: Minimize move size to max value width
        if (const auto remat = SpillRematerialization(current_location->index)) {
            if (remat->word) {
                code.LDR(reg.toS(), Xstate, remat->offset);
            } else {
                code.LDR(reg.toD(), Xstate, remat->offset);
            }
        } else {
            code.LDR(reg, SP, spill_offset + current_location->index * spill_slot_size);
        }
        break;
    case HostLoc::Kind::Flags:
        UNREACHABLE(); //ASSERT(false && "Moving from flags into fprs is not currently supported");
    }
}

std::optional<HostLoc> RegAlloc::ValueLocation(const IR::Inst* value) const {
    const auto contains_value = [value](const HostLocInfo& info) { return info.Contains(value); };

    if (const auto iter = std::find_if(gprs.begin(), gprs.end(), contains_value); iter != gprs.end()) {
        return HostLoc{HostLoc::Kind::Gpr, static_cast<int>(iter - gprs.begin())};
    }
    if (const auto iter = std::find_if(fprs.begin(), fprs.end(), contains_value); iter != fprs.end()) {
        return HostLoc{HostLoc::Kind::Fpr, static_cast<int>(iter - fprs.begin())};
    }
    if (contains_value(flags)) {
        return HostLoc{HostLoc::Kind::Flags, 0};
    }
    if (const auto iter = std::find_if(spills.begin(), spills.begin() + spill_high, contains_value); iter != spills.begin() + spill_high) {
        return HostLoc{HostLoc::Kind::Spill, static_cast<int>(iter - spills.begin())};
    }
    return std::nullopt;
}

HostLocInfo& RegAlloc::ValueInfo(HostLoc host_loc) {
    switch (host_loc.kind) {
    case HostLoc::Kind::Gpr:
        return gprs[static_cast<std::size_t>(host_loc.index)];
    case HostLoc::Kind::Fpr:
        return fprs[static_cast<std::size_t>(host_loc.index)];
    case HostLoc::Kind::Flags:
        return flags;
    case HostLoc::Kind::Spill:
        return spills[static_cast<std::size_t>(host_loc.index)];
    }
    UNREACHABLE();
}

HostLocInfo& RegAlloc::ValueInfo(const IR::Inst* value) {
    const auto contains_value = [value](const HostLocInfo& info) { return info.Contains(value); };

    if (const auto iter = std::find_if(gprs.begin(), gprs.end(), contains_value); iter != gprs.end()) {
        return *iter;
    } else if (const auto iter = std::find_if(fprs.begin(), fprs.end(), contains_value); iter != fprs.end()) {
        return *iter;
    } else if (contains_value(flags)) {
        return flags;
    } else if (const auto iter = std::find_if(spills.begin(), spills.begin() + spill_high, contains_value); iter != spills.begin() + spill_high) {
        return *iter;
    }
    UNREACHABLE();
}

}  // namespace Dynarmic::Backend::Arm64
