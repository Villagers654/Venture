// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>

#include "common/common_types.h"

namespace Dynarmic::Backend::Arm64 {

#ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning(disable : 4324)  // Structure was padded due to alignment specifier
#endif

// Venture: 128, as on x64: cold abort state keeps every guest register's
// written value and its successor live across an access.
constexpr std::size_t SpillCount = 128;

struct alignas(16) RSBEntry {
    u64 target;
    u64 code_ptr;
};

// A32 keeps 8 return stack entries and A64 32, as on x64.
constexpr std::size_t RSBCount = 8;
constexpr u64 RSBIndexMask = (RSBCount - 1) * sizeof(RSBEntry);
constexpr std::size_t A64RSBCount = 32;
constexpr u64 A64RSBIndexMask = (A64RSBCount - 1) * sizeof(RSBEntry);
constexpr std::size_t RSBEntries(bool a64) { return a64 ? A64RSBCount : RSBCount; }
constexpr u64 RSBMask(bool a64) { return a64 ? A64RSBIndexMask : RSBIndexMask; }

struct alignas(16) StackLayout {
    std::array<RSBEntry, A64RSBCount> rsb;

    std::array<std::array<u64, 2>, SpillCount> spill;

    // Venture: the dispatch site slot a site-cache miss refills (A64).
    u64 dispatch_site_slot;

    u32 rsb_ptr;

    s64 cycles_to_run;

    u32 save_host_fpcr;

    bool check_bit;
};

#ifdef _MSC_VER
#    pragma warning(pop)
#endif

static_assert(sizeof(StackLayout) % 16 == 0);

}  // namespace Dynarmic::Backend::Arm64
