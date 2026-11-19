// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <cstddef>

namespace oaknut {
struct CodeGenerator;
struct WReg;
}  // namespace oaknut

namespace Dynarmic::Backend::Arm64 {

class FpsrManager {
public:
    explicit FpsrManager(oaknut::CodeGenerator& code, std::size_t state_fpsr_offset);

    void Spill();
    void Load();
    void Overwrite() { fpsr_loaded = false; }

    void GetFpsr(oaknut::WReg);

    // Venture: for slow paths that rejoin the block (memory fallbacks). The
    // fast path keeps the host FPSR live; the slow path folds it into the state
    // before its call and clears it after, so the tracked state stays valid.
    bool IsLoaded() const { return fpsr_loaded; }
    void EmitFold();
    void EmitClearHost();

private:
    oaknut::CodeGenerator& code;
    std::size_t state_fpsr_offset;
    bool fpsr_loaded = false;
};

}  // namespace Dynarmic::Backend::Arm64
