// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2020 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <cstdint>

#include <catch2/catch_test_macros.hpp>
#include "common/common_types.h"
#include "common/x64/xbyak.h"
#include "dynarmic/backend/x64/xbyak.h"

TEST_CASE("x64: rel32 signed displacement boundaries", "[x64][codegen]") {
    constexpr uintptr_t reference = 0x400000000ULL;
    constexpr uintptr_t next_instruction = reference + 5;
    CHECK(Common::X64::IsWithin2G(reference, next_instruction - 0x80000000ULL));
    CHECK_FALSE(Common::X64::IsWithin2G(reference, next_instruction - 0x80000001ULL));
    CHECK(Common::X64::IsWithin2G(reference, next_instruction + 0x7fffffffULL));
    CHECK_FALSE(Common::X64::IsWithin2G(reference, next_instruction + 0x80000000ULL));
    CHECK_FALSE(Common::X64::IsWithin2G(reference, next_instruction + 0xffffffffULL));
    CHECK_FALSE(Common::X64::IsWithin2G(reference, next_instruction + 0x100000000ULL));
    CHECK(Common::X64::IsWithin2G(reference, next_instruction - 1));
    CHECK(Common::X64::IsWithin2G(reference, next_instruction));
    CHECK(Common::X64::IsWithin2G(reference, reference));
}
