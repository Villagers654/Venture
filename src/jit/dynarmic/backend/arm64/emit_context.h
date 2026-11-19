/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/reg_alloc.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/ir/basic_block.h"

namespace Dynarmic::IR {
class Block;
}  // namespace Dynarmic::IR

namespace Dynarmic::Backend::Arm64 {

struct EmitConfig;
class FastmemManager;
class FpsrManager;

using SharedLabel = std::shared_ptr<oaknut::Label>;

inline SharedLabel GenSharedLabel() {
    return std::make_shared<oaknut::Label>();
}

struct EmitContext {
    IR::Block& block;
    RegAlloc& reg_alloc;
    const EmitConfig& conf;
    EmittedBlockInfo& ebi;
    FpsrManager& fpsr;
    FastmemManager& fastmem;

    std::vector<std::function<void()>> deferred_emits;

    // Venture: A64 terminal choices (PrepareA64Terminal). known_return is the
    // single pushed return location a PopRSBHint exit links to directly.
    std::optional<IR::LocationDescriptor> known_return{};
    bool elide_forward_halt_polls = false;
    // No FPCR write or supervisor call: the exit's FPCR is the block's.
    bool static_fpcr_terminal = false;
    // Venture: host NZCV still holds this ConditionalSelectFlags*'s NZCV
    // argument (set by the emit loop, as on x64), so it skips the MSR.
    bool select_flags_in_host = false;
    // Venture: host NZCV holds the guest NZCV at the If terminal (the emit
    // loop), so its condition reads them directly. With nzcv_exit_liveness the
    // block's final A64SetNZCV stores nothing and the terminal stores NZCV only
    // on the exits whose successor may read it ([0] else, [1] then).
    bool terminal_nzcv_in_flags = false;
    // Venture: the block's final IsZero32/IsZero64/TestBit, fused with its
    // CheckBit terminal (FuseA64CheckBit): the terminal tests this register.
    struct FusedCheckBit {
        int reg;
        IR::Opcode opcode;
        u8 bit = 0;
    };
    std::optional<FusedCheckBit> fused_check_bit{};
    const IR::Inst* deferred_nzcv_set = nullptr;
    std::array<bool, 2> exit_nzcv_store{};

    // Venture: cold abort state (A64 UserConfig::cold_memory_abort_state). The
    // A64ColdSet* records after an access: register writes stored only on that
    // access's abort exit, from where the value is at the access.
    struct ColdStateStore {
        IR::Opcode opcode;
        std::size_t index;
        std::optional<u64> immediate;
        std::optional<HostLoc> location;
        // A spilled value re-read from the JIT state (guest register reads).
        std::optional<RegAlloc::Remat> remat;
    };
    std::map<const IR::Inst*, std::vector<ColdStateStore>> cold_state{};

    // Venture: address folding (FoldA64Addresses). An access addressed as
    // [base, #imm] or [base, index, LSL #shift]; the add and a folded index
    // shift alias their first operand and emit nothing.
    struct FoldedAddress {
        std::size_t base_arg = 0;  // the add's base operand
        std::optional<s64> imm;
        std::optional<IR::Value> index;
        u8 shift = 0;
        const IR::Inst* shift_inst = nullptr;
    };
    std::map<const IR::Inst*, FoldedAddress> folded_accesses{};  // by access
    std::map<const IR::Inst*, const IR::Inst*> folded_adds{};     // add -> access
    std::set<const IR::Inst*> folded_shifts{};

    // Venture: LowLaneAliasing lane-0 VectorGetElement32/64 values that share
    // their dead source vector's location: the bits above them are other lanes.
    std::set<const IR::Inst*> lane_aliases{};

    FP::FPCR FPCR(bool fpcr_controlled = true) const {
        const FP::FPCR fpcr = conf.descriptor_to_fpcr(block.Location());
        return fpcr_controlled ? fpcr : fpcr.ASIMDStandardValue();
    }
};

}  // namespace Dynarmic::Backend::Arm64
