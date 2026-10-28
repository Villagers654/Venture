/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/frontend/A64/a64_ir_emitter.h"

namespace Dynarmic::A64 {


IR::UAny IREmitter::AtomicMemory(size_t bitsize, const IR::U64& vaddr, const IR::UAny& value, u8 op) {
    switch (bitsize) {
    case 8:
        return Inst<IR::U8>(Opcode::A64AtomicMemory8, ImmCurrentLocationDescriptor(), vaddr, value, Imm8(op));
    case 16:
        return Inst<IR::U16>(Opcode::A64AtomicMemory16, ImmCurrentLocationDescriptor(), vaddr, value, Imm8(op));
    case 32:
        return Inst<IR::U32>(Opcode::A64AtomicMemory32, ImmCurrentLocationDescriptor(), vaddr, value, Imm8(op));
    case 64:
        return Inst<IR::U64>(Opcode::A64AtomicMemory64, ImmCurrentLocationDescriptor(), vaddr, value, Imm8(op));
    default:
        UNREACHABLE();
    }
}

IR::UAnyU128 IREmitter::CompareAndSwapMemory(size_t bitsize, const IR::U64& vaddr, const IR::UAnyU128& expected, const IR::UAnyU128& desired) {
    switch (bitsize) {
    case 8:
        return Inst<IR::U8>(Opcode::A64CompareAndSwapMemory8, ImmCurrentLocationDescriptor(), vaddr, expected, desired);
    case 16:
        return Inst<IR::U16>(Opcode::A64CompareAndSwapMemory16, ImmCurrentLocationDescriptor(), vaddr, expected, desired);
    case 32:
        return Inst<IR::U32>(Opcode::A64CompareAndSwapMemory32, ImmCurrentLocationDescriptor(), vaddr, expected, desired);
    case 64:
        return Inst<IR::U64>(Opcode::A64CompareAndSwapMemory64, ImmCurrentLocationDescriptor(), vaddr, expected, desired);
    case 128:
        return Inst<IR::U128>(Opcode::A64CompareAndSwapMemory128, ImmCurrentLocationDescriptor(), vaddr, expected, desired);
    default:
        UNREACHABLE();
    }
}


}  // namespace Dynarmic::A64
