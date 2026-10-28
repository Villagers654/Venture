// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>

#include "common/common_types.h"

#include "dynarmic/backend/x64/nzcv_util.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"

namespace Dynarmic::Backend::X64 {

class BlockOfCode;

#ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning(disable : 4324)  // Structure was padded due to alignment specifier
#endif

struct A64JitState {
    using ProgramCounterType = u64;

    A64JitState() { ResetRSB(); }

    // Exclusive state stuff
    static constexpr u64 RESERVATION_GRANULE_MASK = 0xFFFF'FFFF'FFFF'FFF0ull;
    // Return stack buffer
    // MUST be a power of 2. Venture: 32 entries so returns deeper than eight
    // calls (common in Unity and il2cpp) still predict instead of hashing.
    static constexpr size_t RSB_SIZE = 32;
    static constexpr size_t RSB_PTR_MASK = RSB_SIZE - 1;

    std::array<u64, 31> reg{};
    u64 sp = 0;
    u64 pc = 0;
    u32 cpsr_nzcv = 0;
    u32 fpsr_exc = 0;
    u32 fpsr_qc = 0;
    u32 fpcr = 0;

    alignas(16) std::array<u64, 64> vec{};  // Extension registers.

    // For internal use (See: BlockOfCode::RunCode)
    u32 guest_MXCSR = 0x00001f80;
    u32 asimd_MXCSR = 0x00009fc0;
    volatile u32 halt_reason = 0;

    // Exclusive state
    u8 exclusive_state = 0;
    u32 rsb_ptr = 0;
    std::array<u64, RSB_SIZE> rsb_location_descriptors;
    std::array<u64, RSB_SIZE> rsb_codeptrs;
    // Host return stack (UserConfig::host_return_stack), growing down in
    // 16-byte entries {host landing address, location descriptor}. hrs_base
    // is a sentinel entry whose location matches nothing; a push below
    // hrs_limit restarts from it.
    u64 hrs_sp = 0;
    u64 hrs_base = 0;
    u64 hrs_limit = 0;
    // Separate line from architectural fields: the sampler reads at ~1kHz,
    // while emitted code publishes a block identity with an aligned MOV.
    alignas(64) volatile u32 profile_cookie = 0;

    u32 GetPstate() const {
        return NZCV::FromX64(cpsr_nzcv);
    }

    void SetPstate(u32 new_pstate) {
        cpsr_nzcv = NZCV::ToX64(new_pstate);
    }

    void ResetRSB() {
        rsb_location_descriptors.fill(0xFFFFFFFFFFFFFFFFull);
        rsb_codeptrs.fill(0);
        // Entries name host code that an invalidation may have released.
        hrs_sp = hrs_base;
    }

    u32 GetFpcr() const;
    u32 GetFpsr() const;
    void SetFpcr(u32 value);
    void SetFpsr(u32 value);

    u64 GetUniqueHash() const noexcept {
        const u64 fpcr_u64 = u64(fpcr & A64::LocationDescriptor::fpcr_mask) << A64::LocationDescriptor::fpcr_shift;
        const u64 pc_u64 = pc & A64::LocationDescriptor::pc_mask;
        return pc_u64 | fpcr_u64;
    }
};

#ifdef _MSC_VER
#    pragma warning(pop)
#endif

using CodePtr = const void*;

}  // namespace Dynarmic::Backend::X64
