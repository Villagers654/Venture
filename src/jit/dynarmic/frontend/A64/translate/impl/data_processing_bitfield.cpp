/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/frontend/A64/translate/impl/impl.h"

namespace Dynarmic::A64 {

bool TranslatorVisitor::SBFM(bool sf, bool N, Imm<6> immr, Imm<6> imms, Reg Rn, Reg Rd) {
    if (sf && !N) {
        return ReservedValue();
    }

    if (!sf && (N || immr.Bit<5>() || imms.Bit<5>())) {
        return ReservedValue();
    }

    const u8 R = immr.ZeroExtend<u8>();
    const u8 S = imms.ZeroExtend<u8>();
    const auto masks = DecodeBitMasks(N, imms, immr, false);
    if (!masks) {
        return ReservedValue();
    }

    const size_t datasize = sf ? 64 : 32;
    const auto src = X(datasize, Rn);

    // Venture: every SBFM is a signed field extract (S >= R: SBFX, ASR, SXT*) or
    // a signed field insert into zeros (S < R: SBFIZ). Shifting the field's sign
    // bit to the top and back is two shift instructions instead of a rotate,
    // a bit replication and three masks.
    const size_t width = S >= R ? S - R + 1 : S + 1;
    const size_t up = S >= R ? datasize - R - width : datasize - width;
    IR::U32U64 result = src;
    if (up)
        result = ir.LogicalShiftLeft(result, ir.Imm8(u8(up)));
    if (datasize - width)
        result = ir.ArithmeticShiftRight(result, ir.Imm8(u8(datasize - width)));
    if (S < R)
        result = ir.LogicalShiftLeft(result, ir.Imm8(u8(datasize - R)));

    X(datasize, Rd, result);
    return true;
}

bool TranslatorVisitor::BFM(bool sf, bool N, Imm<6> immr, Imm<6> imms, Reg Rn, Reg Rd) {
    if (sf && !N) {
        return ReservedValue();
    }

    if (!sf && (N || immr.Bit<5>() || imms.Bit<5>())) {
        return ReservedValue();
    }

    const u8 R = immr.ZeroExtend<u8>();
    const auto masks = DecodeBitMasks(N, imms, immr, false);
    if (!masks) {
        return ReservedValue();
    }

    const size_t datasize = sf ? 64 : 32;
    const auto dst = X(datasize, Rd);
    const auto src = X(datasize, Rn);

    // Venture: one masked insert. BFXIL (imms >= immr) copies source bits immr..imms
    // to the bottom; BFI copies the bottom imms + 1 bits to bit datasize - immr. A
    // shift brings the field into place, so no rotate or second masking round.
    const u8 S = imms.ZeroExtend<u8>();
    const bool extract = S >= R;
    const size_t width = extract ? S - R + 1 : S + 1;
    const size_t lsb = extract ? 0 : datasize - R;
    const u64 field = (width == 64 ? ~u64(0) : (u64(1) << width) - 1) << lsb;
    const u64 keep = ~field & (datasize == 64 ? ~u64(0) : u64(0xffffffff));
    IR::U32U64 moved = src;
    if (extract && R != 0)
        moved = ir.LogicalShiftRight(src, ir.Imm8(R));
    else if (!extract)
        moved = ir.LogicalShiftLeft(src, ir.Imm8(u8(lsb)));
    // A field that reaches the top bit needs no mask: the shift cleared everything else.
    const bool shifted_clean = (extract && R != 0 && R + width == datasize) || (!extract && lsb + width == datasize);
    X(datasize, Rd, ir.Or(ir.And(dst, I(datasize, keep)), shifted_clean ? moved : ir.And(moved, I(datasize, field))));
    return true;
}

bool TranslatorVisitor::UBFM(bool sf, bool N, Imm<6> immr, Imm<6> imms, Reg Rn, Reg Rd) {
    if (sf && !N) {
        return ReservedValue();
    }

    if (!sf && (N || immr.Bit<5>() || imms.Bit<5>())) {
        return ReservedValue();
    }

    const u8 R = immr.ZeroExtend<u8>();
    const auto masks = DecodeBitMasks(N, imms, immr, false);
    if (!masks) {
        return ReservedValue();
    }

    const size_t datasize = sf ? 64 : 32;
    const auto src = X(datasize, Rn);

    // Venture: every UBFM is an unsigned field extract (S >= R: UBFX, LSR,
    // UXT*) or a field insert into zeros (S < R: UBFIZ, LSL). A shift plus a
    // mask that the shift does not already imply replaces the rotate and two
    // masks.
    const u8 S = imms.ZeroExtend<u8>();
    const size_t width = S >= R ? S - R + 1 : S + 1;
    const u64 field = width == 64 ? ~u64(0) : (u64(1) << width) - 1;
    IR::U32U64 result = src;
    if (S >= R) {
        if (R)
            result = ir.LogicalShiftRight(result, ir.Imm8(R));
        if (R + width < datasize)
            result = ir.And(result, I(datasize, field));
    } else {
        const size_t shift = datasize - R;
        if (shift + width < datasize)
            result = ir.And(result, I(datasize, field));
        result = ir.LogicalShiftLeft(result, ir.Imm8(u8(shift)));
    }

    X(datasize, Rd, result);
    return true;
}

bool TranslatorVisitor::ASR_1(Imm<5> immr, Reg Rn, Reg Rd) {
    const auto src = X(32, Rn);
    const auto result = ir.ArithmeticShiftRightMasked(src, ir.Imm32(immr.ZeroExtend<u32>()));
    X(32, Rd, result);
    return true;
}

bool TranslatorVisitor::ASR_2(Imm<6> immr, Reg Rn, Reg Rd) {
    const auto src = X(64, Rn);
    const auto result = ir.ArithmeticShiftRightMasked(src, ir.Imm64(immr.ZeroExtend<u64>()));
    X(64, Rd, result);
    return true;
}

bool TranslatorVisitor::SXTB_1(Reg Rn, Reg Rd) {
    const auto src = X(32, Rn);
    const auto result = ir.SignExtendToWord(ir.LeastSignificantByte(src));
    X(32, Rd, result);
    return true;
}

bool TranslatorVisitor::SXTB_2(Reg Rn, Reg Rd) {
    const auto src = X(64, Rn);
    const auto result = ir.SignExtendToLong(ir.LeastSignificantByte(src));
    X(64, Rd, result);
    return true;
}

bool TranslatorVisitor::SXTH_1(Reg Rn, Reg Rd) {
    const auto src = X(32, Rn);
    const auto result = ir.SignExtendToWord(ir.LeastSignificantHalf(src));
    X(32, Rd, result);
    return true;
}

bool TranslatorVisitor::SXTH_2(Reg Rn, Reg Rd) {
    const auto src = X(64, Rn);
    const auto result = ir.SignExtendToLong(ir.LeastSignificantHalf(src));
    X(64, Rd, result);
    return true;
}

bool TranslatorVisitor::SXTW(Reg Rn, Reg Rd) {
    const auto src = X(64, Rn);
    const auto result = ir.SignExtendToLong(ir.LeastSignificantWord(src));
    X(64, Rd, result);
    return true;
}

bool TranslatorVisitor::EXTR(bool sf, bool N, Reg Rm, Imm<6> imms, Reg Rn, Reg Rd) {
    if (N != sf) {
        return UnallocatedEncoding();
    }

    if (!sf && imms.Bit<5>()) {
        return ReservedValue();
    }

    const size_t datasize = sf ? 64 : 32;

    const IR::U32U64 m = X(datasize, Rm);
    const IR::U32U64 n = X(datasize, Rn);
    const IR::U32U64 result = ir.ExtractRegister(m, n, ir.Imm8(imms.ZeroExtend<u8>()));

    X(datasize, Rd, result);
    return true;
}

}  // namespace Dynarmic::A64
