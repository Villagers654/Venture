// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <string_view>

#include <bit>

namespace Dynarmic::Backend::X64 {

namespace detail {
void PerfMapRegister(const void* start, const void* end, std::string_view friendly_name);
}  // namespace detail
template<typename T>
void PerfMapRegister(T start, const void* end, std::string_view friendly_name) noexcept {
    detail::PerfMapRegister(std::bit_cast<const void*>(start), end, friendly_name);
}
void PerfMapClear();
/// Venture: whether a registration has any consumer (a perf map file or a code registration callback). Callers that
/// build a name per block skip both the name and the call when it has none.
bool PerfMapActive() noexcept;


}  // namespace Dynarmic::Backend::X64
