// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <bit>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "boost/container/small_vector.hpp"
#include "common/common_types.h"
#include "common/container/unordered_map.h"
#include "dynarmic/backend/x64/xbyak.h"
#include <boost/container/static_vector.hpp>
#include <boost/container/flat_set.hpp>
#include <boost/pool/pool_alloc.hpp>

#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/hostloc.h"
#include "dynarmic/backend/x64/stack_layout.h"
#include "dynarmic/backend/x64/oparg.h"
#include "dynarmic/backend/x64/abi.h"
#include "dynarmic/ir/cond.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/value.h"

namespace Dynarmic::IR {
enum class AccType;
class Block;
}  // namespace Dynarmic::IR

namespace Dynarmic::Backend::X64 {

class RegAlloc;

struct HostLocInfo {
public:
    HostLocInfo() {}
    inline bool IsLocked() const {
        return is_being_used_count > 0;
    }
    inline bool IsEmpty() const {
        return is_being_used_count == 0 && values.empty();
    }
    inline bool IsLastUse() const {
        return is_being_used_count == 0 && current_references == 1 && size_t(accumulated_uses) + 1 == size_t(total_uses);
    }
    inline void ReadLock() noexcept {
        ASSERT(size_t(is_being_used_count) + 1 < (std::numeric_limits<decltype(is_being_used_count)>::max)());
        ASSERT(!bool(is_scratch));
        is_being_used_count++;
    }
    inline void WriteLock() noexcept {
        ASSERT(is_being_used_count == 0);
        is_being_used_count++;
        is_scratch = true;
    }
    inline void AddArgReference() noexcept {
        ASSERT(size_t(current_references) + 1 < (std::numeric_limits<decltype(current_references)>::max)());
        ++current_references;
        ASSERT(size_t(accumulated_uses) + current_references <= size_t(total_uses));
    }
    void ReleaseOne() noexcept;
    void ReleaseAll() noexcept;
    /// True when ReleaseAll would change nothing.
    inline bool IsReleased() const noexcept {
        return current_references == 0 && is_being_used_count == 0 && !is_scratch && !is_set_last_use
            && (total_uses != accumulated_uses || (total_uses == 0 && max_bit_width == 0 && values.empty()));
    }
    constexpr size_t GetMaxBitWidth() const noexcept { return 1 << max_bit_width; }
    void AddValue(HostLoc loc, IR::Inst* inst) noexcept;
    /// Checks if the given instruction is in our values set
    /// SAFETY: Const is casted away, irrelevant since this is only used for checking
    [[nodiscard]] bool ContainsValue(const IR::Inst* inst) const noexcept {
        return std::find(values.begin(), values.end(), inst) != values.end();
    }
#ifndef NDEBUG
    void EmitVerboseDebuggingOutput(BlockOfCode& code, size_t host_loc_index) const noexcept;
#endif
private:
    boost::container::small_vector<IR::Inst*, 3> values; //24
//non trivial
    // Block state, the total amount of uses for this particular arg
    uint16_t total_uses = 0; //2
    // Sometimes zeroed, accumulated (non referenced) uses
    uint16_t accumulated_uses = 0; //2
//always zeroed
    // Current instruction state
    uint8_t current_references = 0; //1
    uint8_t is_being_used_count = 0; //1
    // Log 2 of bit width, valid values: log2(1,2,4,8,16,32,128) = (0, 1, 2, 3, 4, 5, 6)
    uint8_t max_bit_width : 4 = 0;
    bool is_scratch : 1 = false; //1
    bool is_set_last_use : 1 = false; //1
    friend class RegAlloc;
};
//static_assert(sizeof(HostLocInfo) == 64);

struct Argument {
public:
    using copyable_reference = std::reference_wrapper<Argument>;

    inline IR::Type GetType() const noexcept {
        return value.GetType();
    }
    inline bool IsImmediate() const noexcept {
        return value.IsImmediate();
    }
    inline bool IsVoid() const noexcept {
        return GetType() == IR::Type::Void;
    }

    bool FitsInImmediateU32() const noexcept;
    bool FitsInImmediateS32() const noexcept;

    bool GetImmediateU1() const noexcept;
    u8 GetImmediateU8() const noexcept;
    u16 GetImmediateU16() const noexcept;
    u32 GetImmediateU32() const noexcept;
    u64 GetImmediateS32() const noexcept;
    u64 GetImmediateU64() const noexcept;
    IR::Cond GetImmediateCond() const noexcept;
    IR::AccType GetImmediateAccType() const noexcept;

    /// Is this value currently in a GPR?
    bool IsInGpr(RegAlloc& reg_alloc) const noexcept;
    bool IsInXmm(RegAlloc& reg_alloc) const noexcept;
    bool IsInMemory(RegAlloc& reg_alloc) const noexcept;
private:
    friend class RegAlloc;
    explicit Argument() {}

//data
    IR::Value value; //8
    bool allocated = false; //1
};

/// Where each value of a block is used in a register, by instruction position.
/// The emitter owns one and rebuilds it per block.
class NextUseTable final {
public:
    static constexpr u32 no_use = UINT32_MAX;
    void Build(IR::Block& block);
    /// The instruction's position, if it was in the block when the table was built.
    std::optional<u32> Position(const IR::Inst* inst) const noexcept;
    /// The first use of the value at or after the position, or no_use.
    u32 NextUse(const IR::Inst* value, u32 position) const noexcept;
private:
    // Instructions by position; each instruction carries its own position (Inst::emit_position), so lookups need no
    // hash map. An instruction is in the table only if insts[its position] is itself.
    std::vector<const IR::Inst*> insts;
    std::vector<boost::container::small_vector<u32, 4>> uses;
};

class RegAlloc final {
public:
    using ArgumentInfo = std::array<Argument, IR::max_arg_count>;
    RegAlloc() noexcept = default;
    RegAlloc(std::bitset<32> gpr_order, std::bitset<32> xmm_order) noexcept;

    /// Lets evictions pick the register whose values are needed furthest ahead.
    inline void SetNextUses(const NextUseTable* table) noexcept {
        next_uses = table;
        current_position = 0;
    }
    /// Call before emitting each instruction of the block.
    inline void SetCurrentInstruction(const IR::Inst* inst) noexcept {
        if (next_uses)
            if (const auto position = next_uses->Position(inst))
                current_position = *position;
    }

    ArgumentInfo GetArgumentInfo(const IR::Inst* inst) noexcept;
    void RegisterPseudoOperation(const IR::Inst* inst) noexcept;
    inline bool IsValueLive(const IR::Inst* inst) const noexcept {
        return !!ValueLocation(inst);
    }
    /// Venture: whether a defined value currently lives in an XMM register.
    inline bool IsValueInXmm(const IR::Inst* inst) const noexcept {
        const auto location = ValueLocation(inst);
        return location && HostLocIsXMM(*location);
    }
    /// Where a non-immediate argument currently is, without using or moving it.
    inline std::optional<HostLoc> ArgumentLocation(const Argument& arg) const noexcept {
        return arg.IsImmediate() ? std::nullopt : ValueLocation(arg.value.GetInst());
    }
    /// Whether this instruction holds the only remaining use of the argument's
    /// value, so a scratch use takes its location instead of copying it (never
    /// a pinned register's: a three-operand form reads that in place).
    inline bool IsLastUse(const Argument& arg) const noexcept {
        if (arg.IsImmediate())
            return false;
        const auto location = ValueLocation(arg.value.GetInst());
        return location && (!IsPinned(*location) || location == result_hint) && LocInfo(*location).IsLastUse();
    }
    /// Venture: the current instruction's value is set to this pinned register
    /// directly afterwards: the first any-GPR scratch takes it when it holds
    /// nothing, or a scratch use of its last aliasing value writes it in place.
    inline void SetResultHint(std::optional<HostLoc> loc) noexcept {
        result_hint = loc;
    }
    /// Whether the argument's value is (only) in this register already.
    inline bool IsInRegister(const Argument& arg, const Xbyak::Reg64& reg) const noexcept {
        return !arg.IsImmediate() && ValueLocation(arg.value.GetInst()) == static_cast<HostLoc>(reg.getIdx() + static_cast<size_t>(HostLoc::RAX));
    }
    inline Xbyak::Reg64 UseGpr(BlockOfCode& code, Argument& arg) noexcept {
        ASSERT(!arg.allocated);
        arg.allocated = true;
        return HostLocToReg64(UseImpl(code, arg.value, gpr_order));
    }
    inline Xbyak::Xmm UseXmm(BlockOfCode& code, Argument& arg) noexcept {
        ASSERT(!arg.allocated);
        arg.allocated = true;
        return HostLocToXmm(UseImpl(code, arg.value, xmm_order));
    }
    inline OpArg UseOpArg(BlockOfCode& code, Argument& arg) noexcept {
        return UseGpr(code, arg);
    }
    inline void Use(BlockOfCode& code, Argument& arg, const HostLoc host_loc) noexcept {
        ASSERT(!arg.allocated);
        arg.allocated = true;
        UseImpl(code, arg.value, BuildRegSet({host_loc}));
    }

    Xbyak::Reg64 UseScratchGpr(BlockOfCode& code, Argument& arg) noexcept;
    Xbyak::Xmm UseScratchXmm(BlockOfCode& code, Argument& arg) noexcept;
    void UseScratch(BlockOfCode& code, Argument& arg, HostLoc host_loc) noexcept;

    void DefineValue(BlockOfCode& code, IR::Inst* inst, const Xbyak::Reg& reg) noexcept;
    void DefineValue(BlockOfCode& code, IR::Inst* inst, Argument& arg) noexcept;

    void Release(const Xbyak::Reg& reg) noexcept;

    /// Venture: a GPR outside the allocation order (an A64 register-cache
    /// register) may hold IR values that alias it read-only; uses read it in
    /// place. Call before writing such a register: live values there move out.
    inline void MovePinnedValuesOut(BlockOfCode& code, const Xbyak::Reg64& reg) noexcept {
        const auto loc = static_cast<HostLoc>(reg.getIdx() + static_cast<size_t>(HostLoc::RAX));
        if (!LocInfo(loc).IsEmpty())
            MoveOutOfTheWay(code, loc);
    }

    inline Xbyak::Reg64 ScratchGpr(BlockOfCode& code) noexcept {
        return HostLocToReg64(ScratchImpl(code, gpr_order));
    }
    inline Xbyak::Reg64 ScratchGpr(BlockOfCode& code, const HostLoc desired_location) noexcept {
        return HostLocToReg64(ScratchImpl(code, BuildRegSet({desired_location})));
    }
    inline Xbyak::Xmm ScratchXmm(BlockOfCode& code) noexcept {
        return HostLocToXmm(ScratchImpl(code, xmm_order));
    }
    inline Xbyak::Xmm ScratchXmm(BlockOfCode& code, HostLoc desired_location) noexcept {
        return HostLocToXmm(ScratchImpl(code, BuildRegSet({desired_location})));
    }

    void HostCall(
        BlockOfCode& code,
        IR::Inst* result_def = nullptr,
        const std::optional<Argument::copyable_reference> arg0 = {},
        const std::optional<Argument::copyable_reference> arg1 = {},
        const std::optional<Argument::copyable_reference> arg2 = {},
        const std::optional<Argument::copyable_reference> arg3 = {}
    ) noexcept;

    // TODO: Values in host flags
    void AllocStackSpace(BlockOfCode& code, const size_t stack_space) noexcept;
    void ReleaseStackSpace(BlockOfCode& code, const size_t stack_space);

    // ReleaseAll is idempotent, so only slots reached through LocInfo() since the last scope end
    // can need it; walking all register and spill slots per IR instruction dominated compile time.
    inline void EndOfAllocScope() noexcept {
        if (check_untouched_slots)
            AssertUntouchedSlotsReleased();
        for (size_t word = 0; word < touched.size(); ++word) {
            for (u64 bits = std::exchange(touched[word], 0); bits; bits &= bits - 1)
                hostloc_info[word * 64 + std::countr_zero(bits)].ReleaseAll();
        }
    }
    inline void AssertNoMoreUses() noexcept {
        ASSERT(std::all_of(hostloc_info.begin(), hostloc_info.end(), [](const auto& i) noexcept { return i.IsEmpty(); }));
    }
#ifndef NDEBUG
    inline void EmitVerboseDebuggingOutput(BlockOfCode& code) noexcept {
        for (size_t i = 0; i < hostloc_info.size(); i++)
            hostloc_info[i].EmitVerboseDebuggingOutput(code, i);
    }
#endif
private:
    friend struct Argument;

    HostLoc SelectARegister(std::bitset<32> desired_locations) const noexcept;
    inline bool IsPinned(HostLoc loc) const noexcept {
        return HostLocIsGPR(loc) && !gpr_order.test(size_t(loc));
    }
    std::optional<HostLoc> TakeEmptyResultHint(std::bitset<32> desired_locations) noexcept;
    u32 NextUse(const HostLocInfo& info) const noexcept;
    std::optional<HostLoc> ValueLocation(const IR::Inst* value) const noexcept;
    HostLoc UseImpl(BlockOfCode& code, IR::Value use_value, std::bitset<32> desired_locations) noexcept;
    HostLoc UseScratchImpl(BlockOfCode& code, IR::Value use_value, std::bitset<32> desired_locations) noexcept;
    HostLoc ScratchImpl(BlockOfCode& code, std::bitset<32> desired_locations) noexcept;
    void DefineValueImpl(BlockOfCode& code, IR::Inst* def_inst, HostLoc host_loc) noexcept;
    void DefineValueImpl(BlockOfCode& code, IR::Inst* def_inst, const IR::Value& use_inst) noexcept;

    HostLoc LoadImmediate(BlockOfCode& code, IR::Value imm, HostLoc host_loc) noexcept;
    void Move(BlockOfCode& code, HostLoc to, HostLoc from) noexcept;
    void CopyToScratch(BlockOfCode& code, size_t bit_width, HostLoc to, HostLoc from) noexcept;
    void Exchange(BlockOfCode& code, HostLoc a, HostLoc b) noexcept;
    void MoveOutOfTheWay(BlockOfCode& code, HostLoc reg) noexcept;

    void SpillRegister(BlockOfCode& code, HostLoc loc) noexcept;
    HostLoc FindFreeSpill(bool is_xmm) const noexcept;

    inline HostLocInfo& LocInfo(const HostLoc loc) noexcept {
        DEBUG_ASSERT(loc != HostLoc::RSP && loc != ABI_JIT_PTR);
        touched[size_t(loc) / 64] |= u64(1) << (size_t(loc) % 64);
        return hostloc_info[size_t(loc)];
    }
    inline const HostLocInfo& LocInfo(const HostLoc loc) const noexcept {
        DEBUG_ASSERT(loc != HostLoc::RSP && loc != ABI_JIT_PTR);
        return hostloc_info[size_t(loc)];
    }

    void EmitMove(BlockOfCode& code, const size_t bit_width, const HostLoc to, const HostLoc from) noexcept;
    // VENTURE_JIT_CHECK_ALLOC_SCOPE=1 (tests): every slot EndOfAllocScope skips must already be released, and each
    // defined value must be in no slot before its definition and in exactly its slot after.
    static const bool check_untouched_slots;
    void AssertUntouchedSlotsReleased() const noexcept;
    void EmitExchange(BlockOfCode& code, const HostLoc a, const HostLoc b) noexcept;

//data
    alignas(64) std::array<HostLocInfo, NonSpillHostLocCount + SpillCount> hostloc_info;
    // Slots handed out mutably since the last EndOfAllocScope.
    std::array<u64, (NonSpillHostLocCount + SpillCount + 63) / 64> touched{};
    std::bitset<32> gpr_order;
    std::bitset<32> xmm_order;
    size_t reserved_stack_space = 0;
#ifdef _WIN32
    const void* reserved_stack_begin = nullptr;
#endif
    const NextUseTable* next_uses = nullptr;
    u32 current_position = 0;
    std::optional<HostLoc> result_hint;
};

}  // namespace Dynarmic::Backend::X64
