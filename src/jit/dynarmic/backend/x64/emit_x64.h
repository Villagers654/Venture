// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#include <string>
#include <type_traits>
#include <vector>

#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"
#include <boost/container/stable_vector.hpp>
#include <boost/container/small_vector.hpp>

#include "dynarmic/backend/x64/xbyak.h"
#include "dynarmic/mcl/bit.hpp"
#include "dynarmic/backend/exception_handler.h"
#include "dynarmic/backend/x64/reg_alloc.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/ir/location_descriptor.h"
#include "dynarmic/ir/terminal.h"

namespace Dynarmic::IR {
class Block;
class Inst;
}  // namespace Dynarmic::IR

namespace Dynarmic {
enum class OptimizationFlag : u32;
}  // namespace Dynarmic

namespace Dynarmic::Backend::X64 {

class A64EmitX64;
class BlockOfCode;

using A64FullVectorWidth = std::integral_constant<size_t, 128>;

// Array alias that always sizes itself according to the given type T
// relative to the size of a vector register. e.g. T = u32 would result
// in a std::array<u32, 4>.
template<typename T>
using VectorArray = std::array<T, A64FullVectorWidth::value / mcl::bitsizeof<T>>;

template<typename T>
using HalfVectorArray = std::array<T, A64FullVectorWidth::value / mcl::bitsizeof<T> / 2>;

using SharedLabel = Xbyak::Label*;
struct EmitContext {
    EmitContext(RegAlloc& reg_alloc, IR::Block& block, boost::container::stable_vector<Xbyak::Label>& shared_labels);
    virtual ~EmitContext();
    virtual FP::FPCR FPCR(bool fpcr_controlled = true) const = 0;
    virtual bool HasOptimization(OptimizationFlag flag) const = 0;

    [[nodiscard]] inline Xbyak::Label* GenSharedLabel() noexcept {
        return &shared_labels.emplace_back();
    }

    /// Add64(base, imm32) addresses whose only user is an identity-fastmem
    /// access: the add is not emitted and the access uses [base + imm32].
    [[nodiscard]] inline bool IsFoldedAddress(const IR::Inst* inst) const noexcept {
        return std::find(folded_addresses.begin(), folded_addresses.end(), inst) != folded_addresses.end();
    }
    /// Venture: a lane 1-3 VectorGetElement32 that left its source vector in
    /// place (its value aliases the source); its only user reads the lane.
    [[nodiscard]] inline bool IsLaneAlias(const IR::Value& value) const noexcept {
        return !value.IsImmediate() && std::find(lane_aliases.begin(), lane_aliases.end(), value.GetInst()) != lane_aliases.end();
    }
    /// Venture: a LogicalShiftLeft by 1-3 whose only user is an Add emitted as
    /// LEA [x + y*scale]: its value aliases the unshifted operand.
    [[nodiscard]] inline bool IsScaledIndex(const IR::Inst* inst) const noexcept {
        return std::find(scaled_indices.begin(), scaled_indices.end(), inst) != scaled_indices.end();
    }

    std::vector<std::function<void()>> deferred_emits;
    /// Venture: deferred handlers that register native unwind frames. They are
    /// emitted after the block in the hot region, never in the cold region.
    std::vector<std::function<void()>> framed_deferred_emits;
    boost::container::small_vector<const IR::Inst*, 16> folded_addresses;
    boost::container::small_vector<const IR::Inst*, 8> lane_aliases;
    boost::container::small_vector<const IR::Inst*, 8> scaled_indices;
    /// Set by the A64 emit loop for the ConditionalSelectFlags* being emitted
    /// when host FLAGS still hold its NZCV argument (CF holds ARM C).
    bool select_flags_in_host = false;
    /// Venture: the Add/Sub whose result FLAGS a conditional terminal uses
    /// directly (no NZCV materialization or LEA form); its emitter records
    /// whether x64 CF is the inverse of ARM C.
    const IR::Inst* host_flags_inst = nullptr;
    bool host_flags_carry_inverted = false;
    /// Venture: set by the A64 emit loop for an FPCompare and then for its only
    /// user, the ConditionalSelectFlags32/64 right after it: the compare leaves
    /// its UCOMIS/COMIS FLAGS and the select tests them with CMOVs.
    const IR::Inst* fp_flags_select = nullptr;
    /// Venture: the FPCompare for fp_flags_select left a VCMPSS/VCMPSD mask as
    /// its value (no FLAGS) for the select to blend with.
    bool fp_select_blend = false;
    RegAlloc& reg_alloc;
    IR::Block& block;
    boost::container::stable_vector<Xbyak::Label>& shared_labels;
};

/// Venture: after UCOMIS/COMIS each ARM condition on FPCompare(a, b) NZCV is one
/// x64 condition (EQ and NE also test PF). These conditions compare b with a.
/// Venture: the AVX compare predicate that is true when an A64 condition holds
/// after FCMP a, b (quiet) or FCMPE a, b (signaling: raises Invalid on QNaN).
constexpr u8 FPSelectPredicate(IR::Cond cond, bool signaling) noexcept {
    switch (cond) {
    case IR::Cond::EQ: return signaling ? 0x10 : 0x00;  // EQ_OS / EQ_OQ
    case IR::Cond::NE: return signaling ? 0x14 : 0x04;  // NEQ_US / NEQ_UQ
    case IR::Cond::CS:
    case IR::Cond::PL: return signaling ? 0x05 : 0x15;  // NLT_US / NLT_UQ
    case IR::Cond::CC:
    case IR::Cond::MI: return signaling ? 0x01 : 0x11;  // LT_OS / LT_OQ
    case IR::Cond::VS: return signaling ? 0x13 : 0x03;  // UNORD_S / UNORD_Q
    case IR::Cond::VC: return signaling ? 0x17 : 0x07;  // ORD_S / ORD_Q
    case IR::Cond::HI: return signaling ? 0x06 : 0x16;  // NLE_US / NLE_UQ
    case IR::Cond::LS: return signaling ? 0x02 : 0x12;  // LE_OS / LE_OQ
    case IR::Cond::GE: return signaling ? 0x0D : 0x1D;  // GE_OS / GE_OQ
    case IR::Cond::LT: return signaling ? 0x09 : 0x19;  // NGE_US / NGE_UQ
    case IR::Cond::GT: return signaling ? 0x0E : 0x1E;  // GT_OS / GT_OQ
    case IR::Cond::LE: return signaling ? 0x0A : 0x1A;  // NGT_US / NGT_UQ
    default: return signaling ? 0x1F : 0x0F;  // TRUE_US / TRUE_UQ
    }
}

/// Venture: word producers whose emitters always end by writing a 32-bit GPR
/// (or an XMM register), so a GPR holding the value has bits 32-63 clear. Moves
/// and spill reloads of a word use 32-bit (or the location's widest, equally
/// clear) forms. A shift by 0 may leave the register unwritten, so only
/// immediate shifts by 1-31 qualify; LeastSignificantWord aliases its source.
inline bool WritesZeroExtendedWord(const IR::Inst* inst) noexcept {
    switch (inst->GetOpcode()) {
    case IR::Opcode::Add32:
    case IR::Opcode::Sub32:
    case IR::Opcode::And32:
    case IR::Opcode::AndNot32:
    case IR::Opcode::Or32:
    case IR::Opcode::Eor32:
    case IR::Opcode::Mul32:
    case IR::Opcode::ZeroExtendByteToWord:
    case IR::Opcode::ZeroExtendHalfToWord:
    case IR::Opcode::ConditionalSelect32:
    case IR::Opcode::ConditionalSelectFlags32:
    case IR::Opcode::A64GetW:
    case IR::Opcode::A64ReadMemory32:
        return true;
    case IR::Opcode::LogicalShiftLeft32:
        return inst->GetArg(1).IsImmediate() && inst->GetArg(1).GetU8() >= 1 && inst->GetArg(1).GetU8() <= 31;
    default:
        return false;
    }
}

constexpr bool FPFlagsSelectSwapsOperands(IR::Cond cond) noexcept {
    switch (cond) {
    case IR::Cond::CS:
    case IR::Cond::CC:
    case IR::Cond::MI:
    case IR::Cond::PL:
    case IR::Cond::HI:
    case IR::Cond::LS:
        return true;
    default:
        return false;
    }
}

/// Venture: true when the code this backend emits for `value` (a U128) leaves
/// its upper 64 bits zero, so a VectorZeroUpper or SetD of it needs no MOVQ.
bool IsUpperZeroVector(BlockOfCode& code, const EmitContext& ctx, const IR::Value& value, int depth = 0);
/// Venture: true when an XMM register holding `value` (a U32 or U64) is zero
/// above the value's width, so a ZeroExtendWordToLong or ZeroExtendLongToQuad
/// of it needs no INSERTPS or MOVQ.
bool IsZeroAboveScalar(BlockOfCode& code, const EmitContext& ctx, const IR::Value& value, int depth = 0);
/// Venture: an FPAdd/FPMul that the emitter computes as b + a (b * a) because
/// only its second operand is zero above the value's width.
bool SwapsScalarFPOperands(BlockOfCode& code, const EmitContext& ctx, const IR::Inst* inst);

class EmitX64 {
public:
    struct BlockDescriptor {
        CodePtr entrypoint;  // Entrypoint of emitted code
        size_t size;         // Length in bytes of emitted code
    };
    static_assert(sizeof(BlockDescriptor) == 16);

    explicit EmitX64(BlockOfCode& code);
    virtual ~EmitX64();

    /// Looks up an emitted host block in the cache.
    std::optional<BlockDescriptor> GetBasicBlock(IR::LocationDescriptor descriptor) const;

    /// Empties the entire cache.
    virtual void ClearCache();

    /// Invalidates a selection of basic blocks.
    void InvalidateBasicBlocks(const ::Common::unordered_set<IR::LocationDescriptor>& locations);

//protected:
    // Microinstruction emitters
#define OPCODE(name, type, ...) void Emit##name(EmitContext& ctx, IR::Inst* inst);
#define A32OPC(...)
#define A64OPC(...)
#include "dynarmic/ir/opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC
    void EmitInvalid(EmitContext& ctx, IR::Inst* inst);

    // Helpers
    virtual std::string LocationDescriptorToFriendlyName(const IR::LocationDescriptor&) const = 0;
    void EmitAddCycles(size_t cycles);
    /// flags_available: host FLAGS hold the condition's NZCV, CF as ARM C or,
    /// with carry_inverted, as its inverse (an x64 subtraction's borrow).
    Xbyak::Label EmitCond(IR::Cond cond, bool flags_available = false, bool carry_inverted = false);
    BlockDescriptor RegisterBlock(const IR::LocationDescriptor& location_descriptor, CodePtr entrypoint, size_t size);
    void PushRSBHelper(Xbyak::Reg64 loc_desc_reg, Xbyak::Reg64 index_reg, IR::LocationDescriptor target);

#ifndef NDEBUG
    void EmitVerboseDebuggingOutput(RegAlloc& reg_alloc);
#endif
    virtual bool EmitLeafTerminal(IR::Term::LeafTerminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step) noexcept = 0;
    virtual bool EmitTerminal(IR::Term::Terminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step) noexcept = 0;

    // Patching
    struct PatchInformation {
        boost::container::small_vector<CodePtr, 4> jg; //4*8=32
        boost::container::small_vector<CodePtr, 4> jz; //4*8=32
        boost::container::small_vector<CodePtr, 4> jmp; //4*8=32
        boost::container::small_vector<CodePtr, 4> mov_rcx; //4*8=32
        // Compact links: cold-region stub that stores the target PC and
        // returns to the dispatcher; unlinked jg/jz/jmp sites jump here.
        CodePtr unlinked_exit = nullptr;
    };
    void Patch(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr);
    virtual void Unpatch(const IR::LocationDescriptor& target_desc);
    virtual void EmitPatchJg(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) = 0;
    virtual void EmitPatchJz(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) = 0;
    virtual void EmitPatchJmp(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) = 0;
    virtual void EmitPatchMovRcx(CodePtr target_code_ptr = nullptr) = 0;

    // State
    BlockOfCode& code;
    ExceptionHandler exception_handler;
    /// Venture: read fallbacks exist for 32/64-bit values in XMM registers.
    bool xmm_read_fallbacks = false;
    /// Venture: fastmem fallback thunks, keyed (ordered, bitsize, vaddr, value).
    /// Off Windows, FastmemFallback emits each into the prelude's thunk arena on
    /// first use instead of all 6,496 at construction (12 ms and 2.4 MB per JIT).
    /// Windows keeps them in the prelude: its unwind table is address-ordered.
    enum class FallbackKind { Read, Write, ExclusiveWrite };
    using FallbackKey = std::tuple<bool, size_t, int, int>;
    struct FallbackRequest {
        FallbackKind kind;
        FallbackKey key;
    };
    /// Set while FastmemFallback generates one thunk; null generates all.
    const FallbackRequest* fallback_request = nullptr;
    template<typename Key>
    bool WantFallback(FallbackKind kind, const Key& key) const {
        return !fallback_request || (fallback_request->kind == kind && fallback_request->key == FallbackKey(key));
    }
    ::Common::unordered_map<IR::LocationDescriptor, BlockDescriptor> block_descriptors;
    ::Common::unordered_map<IR::LocationDescriptor, PatchInformation> patch_information;

    // We need materialized protected members
    friend class A64EmitX64;
};

}  // namespace Dynarmic::Backend::X64
