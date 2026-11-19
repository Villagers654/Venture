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
struct Label;
}  // namespace oaknut

namespace Dynarmic::IR {
enum class AccType;
class Inst;
}  // namespace Dynarmic::IR

namespace Dynarmic::Backend::Arm64 {

struct EmitContext;
enum class LinkTarget;

template<std::size_t bitsize>
void EmitReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template<std::size_t bitsize>
void EmitExclusiveReadMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template<std::size_t bitsize>
void EmitWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
template<std::size_t bitsize>
void EmitExclusiveWriteMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
// Venture: an A64 LSE atomic inline on guest memory (fastmem with
// Unsafe_IgnoreGlobalMonitor, as the x64 backend); false when it cannot be.
template<std::size_t bitsize, bool compare_and_swap>
bool EmitInlineAtomicMemory(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
// Venture: an A64 128-bit compare-and-swap as one host CASPAL (FEAT_LSE, the
// same conditions); false when it cannot be.
bool EmitInlineCompareAndSwap128(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);

}  // namespace Dynarmic::Backend::Arm64
