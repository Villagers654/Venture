/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/ir/opcodes.h"

#include <array>

namespace Dynarmic::IR {

/// @brief Get the name of an opcode.
std::string_view GetNameOf(Opcode op) noexcept {
    static const std::string_view opcode_names[] = {
#define OPCODE(name, type, ...) #name,
#define A32OPC(name, type, ...) #name,
#define A64OPC(name, type, ...) #name,
#include "./opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC
    };
    return opcode_names[size_t(op)];
}

}  // namespace Dynarmic::IR
