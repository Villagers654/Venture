/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <optional>

#include "dynarmic/frontend/A64/translate/impl/impl.h"

namespace Dynarmic::A64 {

static bool ExclusiveSharedDecodeAndOperation(TranslatorVisitor& v, bool pair, size_t size, bool L, bool o0, std::optional<Reg> Rs, std::optional<Reg> Rt2, Reg Rn, Reg Rt) {
    // Shared Decode

    const auto acctype = o0 ? IR::AccType::ORDERED : IR::AccType::ATOMIC;
    const auto memop = L ? IR::MemOp::LOAD : IR::MemOp::STORE;
    const size_t elsize = 8 << size;
    const size_t regsize = elsize == 64 ? 64 : 32;
    const size_t datasize = pair ? elsize * 2 : elsize;

    // Operation

    const size_t dbytes = datasize / 8;

    if (memop == IR::MemOp::LOAD && pair && Rt == *Rt2) {
        return v.UnpredictableInstruction();
    } else if (memop == IR::MemOp::STORE && (*Rs == Rt || (pair && *Rs == *Rt2))) {
        if (!v.options.define_unpredictable_behaviour) {
            return v.UnpredictableInstruction();
        }
        // UNPREDICTABLE: The Constraint_NONE case is executed.
    } else if (memop == IR::MemOp::STORE && *Rs == Rn && Rn != Reg::R31) {
        return v.UnpredictableInstruction();
    }

    IR::U64 address;
    if (Rn == Reg::SP) {
        // TODO: Check SP Alignment
        address = v.SP(64);
    } else {
        address = v.X(64, Rn);
    }

    switch (memop) {
    case IR::MemOp::STORE: {
        IR::UAnyU128 data;
        if (pair && elsize == 64) {
            data = v.ir.Pack2x64To1x128(v.X(64, Rt), v.X(64, *Rt2));
        } else if (pair && elsize == 32) {
            data = v.ir.Pack2x32To1x64(v.X(32, Rt), v.X(32, *Rt2));
        } else {
            data = v.X(elsize, Rt);
        }
        const IR::U32 status = v.ExclusiveMem(address, dbytes, acctype, data);
        v.X(32, *Rs, status);
        break;
    }
    case IR::MemOp::LOAD: {
        const IR::UAnyU128 data = v.ExclusiveMem(address, dbytes, acctype);
        if (pair && elsize == 64) {
            v.X(64, Rt, v.ir.VectorGetElement(64, data, 0));
            v.X(64, *Rt2, v.ir.VectorGetElement(64, data, 1));
        } else if (pair && elsize == 32) {
            v.X(32, Rt, v.ir.LeastSignificantWord(data));
            v.X(32, *Rt2, v.ir.MostSignificantWord(data).result);
        } else {
            v.X(regsize, Rt, v.ZeroExtend(data, regsize));
        }
        break;
    }
    default:
        UNREACHABLE();
    }

    return true;
}

bool TranslatorVisitor::STXR(Imm<2> sz, Reg Rs, Reg Rn, Reg Rt) {
    const bool pair = false;
    const size_t size = sz.ZeroExtend<size_t>();
    const bool L = 0;
    const bool o0 = 0;
    return ExclusiveSharedDecodeAndOperation(*this, pair, size, L, o0, Rs, {}, Rn, Rt);
}

bool TranslatorVisitor::STLXR(Imm<2> sz, Reg Rs, Reg Rn, Reg Rt) {
    const bool pair = false;
    const size_t size = sz.ZeroExtend<size_t>();
    const bool L = 0;
    const bool o0 = 1;
    return ExclusiveSharedDecodeAndOperation(*this, pair, size, L, o0, Rs, {}, Rn, Rt);
}

bool TranslatorVisitor::STXP(Imm<1> sz, Reg Rs, Reg Rt2, Reg Rn, Reg Rt) {
    const bool pair = true;
    const size_t size = concatenate(Imm<1>{1}, sz).ZeroExtend<size_t>();
    const bool L = 0;
    const bool o0 = 0;
    return ExclusiveSharedDecodeAndOperation(*this, pair, size, L, o0, Rs, Rt2, Rn, Rt);
}

bool TranslatorVisitor::STLXP(Imm<1> sz, Reg Rs, Reg Rt2, Reg Rn, Reg Rt) {
    const bool pair = true;
    const size_t size = concatenate(Imm<1>{1}, sz).ZeroExtend<size_t>();
    const bool L = 0;
    const bool o0 = 1;
    return ExclusiveSharedDecodeAndOperation(*this, pair, size, L, o0, Rs, Rt2, Rn, Rt);
}

bool TranslatorVisitor::LDXR(Imm<2> sz, Reg Rn, Reg Rt) {
    const bool pair = false;
    const size_t size = sz.ZeroExtend<size_t>();
    const bool L = 1;
    const bool o0 = 0;
    return ExclusiveSharedDecodeAndOperation(*this, pair, size, L, o0, {}, {}, Rn, Rt);
}

bool TranslatorVisitor::LDAXR(Imm<2> sz, Reg Rn, Reg Rt) {
    const bool pair = false;
    const size_t size = sz.ZeroExtend<size_t>();
    const bool L = 1;
    const bool o0 = 1;
    return ExclusiveSharedDecodeAndOperation(*this, pair, size, L, o0, {}, {}, Rn, Rt);
}

bool TranslatorVisitor::LDXP(Imm<1> sz, Reg Rt2, Reg Rn, Reg Rt) {
    const bool pair = true;
    const size_t size = concatenate(Imm<1>{1}, sz).ZeroExtend<size_t>();
    const bool L = 1;
    const bool o0 = 0;
    return ExclusiveSharedDecodeAndOperation(*this, pair, size, L, o0, {}, Rt2, Rn, Rt);
}

bool TranslatorVisitor::LDAXP(Imm<1> sz, Reg Rt2, Reg Rn, Reg Rt) {
    const bool pair = true;
    const size_t size = concatenate(Imm<1>{1}, sz).ZeroExtend<size_t>();
    const bool L = 1;
    const bool o0 = 1;
    return ExclusiveSharedDecodeAndOperation(*this, pair, size, L, o0, {}, Rt2, Rn, Rt);
}

static bool OrderedSharedDecodeAndOperation(TranslatorVisitor& v, size_t size, bool L, bool o0, Reg Rn, Reg Rt) {
    // Shared Decode

    const auto acctype = !o0 ? IR::AccType::LIMITEDORDERED : IR::AccType::ORDERED;
    const auto memop = L ? IR::MemOp::LOAD : IR::MemOp::STORE;
    const size_t elsize = 8 << size;
    const size_t regsize = elsize == 64 ? 64 : 32;
    const size_t datasize = elsize;

    // Operation

    const size_t dbytes = datasize / 8;

    IR::U64 address;
    if (Rn == Reg::SP) {
        // TODO: Check SP Alignment
        address = v.SP(64);
    } else {
        address = v.X(64, Rn);
    }

    switch (memop) {
    case IR::MemOp::STORE: {
        const IR::UAny data = v.X(datasize, Rt);
        v.Mem(address, dbytes, acctype, data);
        break;
    }
    case IR::MemOp::LOAD: {
        const IR::UAny data = v.Mem(address, dbytes, acctype);
        v.X(regsize, Rt, v.ZeroExtend(data, regsize));
        break;
    }
    default:
        UNREACHABLE();
    }

    return true;
}

bool TranslatorVisitor::STLLR(Imm<2> sz, Reg Rn, Reg Rt) {
    const size_t size = sz.ZeroExtend<size_t>();
    const bool L = 0;
    const bool o0 = 0;
    return OrderedSharedDecodeAndOperation(*this, size, L, o0, Rn, Rt);
}

bool TranslatorVisitor::STLR(Imm<2> sz, Reg Rn, Reg Rt) {
    const size_t size = sz.ZeroExtend<size_t>();
    const bool L = 0;
    const bool o0 = 1;
    return OrderedSharedDecodeAndOperation(*this, size, L, o0, Rn, Rt);
}

bool TranslatorVisitor::LDLAR(Imm<2> sz, Reg Rn, Reg Rt) {
    const size_t size = sz.ZeroExtend<size_t>();
    const bool L = 1;
    const bool o0 = 0;
    return OrderedSharedDecodeAndOperation(*this, size, L, o0, Rn, Rt);
}

bool TranslatorVisitor::LDAR(Imm<2> sz, Reg Rn, Reg Rt) {
    const size_t size = sz.ZeroExtend<size_t>();
    const bool L = 1;
    const bool o0 = 1;
    return OrderedSharedDecodeAndOperation(*this, size, L, o0, Rn, Rt);
}

bool TranslatorVisitor::RCPC_LOAD(Imm<2> sz, Reg Rn, Reg Rt) {
    // RCpc permits weaker store-release/load-acquire ordering than LDAR.
    // The existing LIMITEDORDERED path is a conservative implementation on
    // x64: aligned MOV, locked misaligned read. Reuse its address/width rules.
    return OrderedSharedDecodeAndOperation(*this, sz.ZeroExtend<size_t>(), true, false, Rn, Rt);
}

static bool OrderedPair(TranslatorVisitor& v, bool sz, Reg Rt2, bool no_writeback, Reg Rn, Reg Rt, bool load) {
    const auto bits = sz ? 64u : 32u;
    const auto bytes = bits / 4u;
    if ((load && Rt == Rt2) || (!no_writeback && Rn != Reg::SP && (Rn == Rt || Rn == Rt2))) {
        return v.UnpredictableInstruction();
    }
    const IR::U64 original = Rn == Reg::SP ? IR::U64(v.SP(64)) : IR::U64(v.X(64, Rn));
    const IR::U64 address = !load && !no_writeback ? IR::U64(v.ir.Sub(original, v.ir.Imm64(bytes))) : original;
    // A single combined access preserves pair atomicity. The existing ordered
    // backend supplies MOV/XCHG for 64 bits and CMPXCHG16B for 128 bits.
    if (load) {
        if (sz) {
            const IR::U128 data = v.Mem(address, bytes, IR::AccType::LIMITEDORDERED);
            v.X(64, Rt, v.ir.VectorGetElement(64, data, 0));
            v.X(64, Rt2, v.ir.VectorGetElement(64, data, 1));
        } else {
            const IR::U64 data = v.Mem(address, bytes, IR::AccType::LIMITEDORDERED);
            v.X(32, Rt, v.ir.LeastSignificantWord(data));
            v.X(32, Rt2, v.ir.MostSignificantWord(data).result);
        }
    } else if (sz) {
        v.Mem(address, bytes, IR::AccType::LIMITEDORDERED, v.ir.Pack2x64To1x128(v.X(64, Rt), v.X(64, Rt2)));
    } else {
        v.Mem(address, bytes, IR::AccType::LIMITEDORDERED, v.ir.Pack2x32To1x64(v.X(32, Rt), v.X(32, Rt2)));
    }
    if (!no_writeback) {
        const IR::U64 next = load ? IR::U64(v.ir.Add(original, v.ir.Imm64(bytes))) : address;
        if (Rn == Reg::SP) v.SP(64, next);
        else v.X(64, Rn, next);
    }
    return true;
}

bool TranslatorVisitor::RCPC_LOAD_PAIR(bool sz, Reg Rt2, bool no_writeback, Reg Rn, Reg Rt) {
    return OrderedPair(*this, sz, Rt2, no_writeback, Rn, Rt, true);
}

bool TranslatorVisitor::RCPC_STORE_PAIR(bool sz, Reg Rt2, bool no_writeback, Reg Rn, Reg Rt) {
    return OrderedPair(*this, sz, Rt2, no_writeback, Rn, Rt, false);
}

// ARMv8.1 LSE atomics. Each is one atomic access; the backend's locked host
// instructions are full barriers, so the acquire and release forms need no fences.
static IR::U64 AtomicAddress(TranslatorVisitor& v, Reg Rn) {
    if (Rn == Reg::SP) {
        return v.SP(64);
    }
    return v.X(64, Rn);
}

bool TranslatorVisitor::LSE_RMW(Imm<2> size, bool, bool, Reg Rs, Imm<4> op, Reg Rn, Reg Rt) {
    // o3:opc is A64::AtomicOp: LDADD..LDUMIN, then SWP. ST64BV* stays undefined.
    if (op.ZeroExtend() > 8) {
        return UnallocatedEncoding();
    }
    const size_t datasize = 8 << size.ZeroExtend<size_t>();
    const size_t regsize = datasize == 64 ? 64 : 32;
    const IR::UAny old = ir.AtomicMemory(datasize, AtomicAddress(*this, Rn), X(datasize, Rs), static_cast<u8>(op.ZeroExtend()));
    X(regsize, Rt, ZeroExtend(old, regsize));
    return true;
}

static bool CompareAndSwap(TranslatorVisitor& v, size_t datasize, Reg Rs, Reg Rn, Reg Rt) {
    const size_t regsize = datasize == 64 ? 64 : 32;
    const IR::UAny old = v.ir.CompareAndSwapMemory(datasize, AtomicAddress(v, Rn), v.X(datasize, Rs), v.X(datasize, Rt));
    v.X(regsize, Rs, v.ZeroExtend(old, regsize));
    return true;
}

bool TranslatorVisitor::CASB(bool, Reg Rs, bool, Reg Rn, Reg Rt) {
    return CompareAndSwap(*this, 8, Rs, Rn, Rt);
}

bool TranslatorVisitor::CASH(bool, Reg Rs, bool, Reg Rn, Reg Rt) {
    return CompareAndSwap(*this, 16, Rs, Rn, Rt);
}

bool TranslatorVisitor::CAS(bool sz, bool, Reg Rs, bool, Reg Rn, Reg Rt) {
    return CompareAndSwap(*this, sz ? 64 : 32, Rs, Rn, Rt);
}

bool TranslatorVisitor::CASP(bool sz, bool, Reg Rs, bool, Reg Rn, Reg Rt) {
    if ((static_cast<size_t>(Rs) & 1) || (static_cast<size_t>(Rt) & 1)) {
        return UnallocatedEncoding();
    }
    const IR::U64 address = AtomicAddress(*this, Rn);
    if (sz) {
        const IR::U128 old = ir.CompareAndSwapMemory(128, address, ir.Pack2x64To1x128(X(64, Rs), X(64, Rs + 1)), ir.Pack2x64To1x128(X(64, Rt), X(64, Rt + 1)));
        X(64, Rs, ir.VectorGetElement(64, old, 0));
        X(64, Rs + 1, ir.VectorGetElement(64, old, 1));
    } else {
        const IR::U64 old = ir.CompareAndSwapMemory(64, address, ir.Pack2x32To1x64(X(32, Rs), X(32, Rs + 1)), ir.Pack2x32To1x64(X(32, Rt), X(32, Rt + 1)));
        X(32, Rs, ir.LeastSignificantWord(old));
        X(32, Rs + 1, ir.MostSignificantWord(old).result);
    }
    return true;
}

}  // namespace Dynarmic::A64
