// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <arm_neon.h>
#include <array>
#include <bit>

#include <oaknut/oaknut.hpp>

#include "dynarmic/backend/arm64/a32_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/emit_context.h"
#include "dynarmic/backend/arm64/reg_alloc.h"
#include "dynarmic/common/crypto/aes.h"
#include "dynarmic/common/crypto/crc32.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Arm64 {

using namespace oaknut::util;

template<std::size_t bitsize, int length, bool castagnoli, typename EmitFn>
static void EmitCRC(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, EmitFn emit_fn) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    if (!ctx.conf.HasHostFeature(HostFeature::CRC32)) {
        // Venture: the CRC32 instructions are optional before ARMv8.1; without
        // them the table implementation runs, as on x64 without SSE4.2/CLMUL.
        ctx.reg_alloc.PrepareForCall(args[0], args[1]);
        code.MOV(W2, length);
        u32 (*fn)(u32, u64, int) = castagnoli ? &Common::Crypto::CRC32::ComputeCRC32Castagnoli : &Common::Crypto::CRC32::ComputeCRC32ISO;
        code.MOV(Xscratch0, std::bit_cast<u64>(fn));
        code.BLR(Xscratch0);
        ctx.reg_alloc.DefineAsRegister(inst, X0);
        return;
    }

    auto Woutput = ctx.reg_alloc.WriteW(inst);
    auto Winput = ctx.reg_alloc.ReadW(args[0]);
    auto Rdata = ctx.reg_alloc.ReadReg<bitsize>(args[1]);
    RegAlloc::Realize(Woutput, Winput, Rdata);

    emit_fn(Woutput, Winput, Rdata);
}

template<>
void EmitIR<IR::Opcode::CRC32Castagnoli8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitCRC<32, 1, true>(code, ctx, inst, [&](auto& Woutput, auto& Winput, auto& Wdata) { code.CRC32CB(Woutput, Winput, Wdata); });
}

template<>
void EmitIR<IR::Opcode::CRC32Castagnoli16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitCRC<32, 2, true>(code, ctx, inst, [&](auto& Woutput, auto& Winput, auto& Wdata) { code.CRC32CH(Woutput, Winput, Wdata); });
}

template<>
void EmitIR<IR::Opcode::CRC32Castagnoli32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitCRC<32, 4, true>(code, ctx, inst, [&](auto& Woutput, auto& Winput, auto& Wdata) { code.CRC32CW(Woutput, Winput, Wdata); });
}

template<>
void EmitIR<IR::Opcode::CRC32Castagnoli64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitCRC<64, 8, true>(code, ctx, inst, [&](auto& Woutput, auto& Winput, auto& Xdata) { code.CRC32CX(Woutput, Winput, Xdata); });
}

template<>
void EmitIR<IR::Opcode::CRC32ISO8>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitCRC<32, 1, false>(code, ctx, inst, [&](auto& Woutput, auto& Winput, auto& Wdata) { code.CRC32B(Woutput, Winput, Wdata); });
}

template<>
void EmitIR<IR::Opcode::CRC32ISO16>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitCRC<32, 2, false>(code, ctx, inst, [&](auto& Woutput, auto& Winput, auto& Wdata) { code.CRC32H(Woutput, Winput, Wdata); });
}

template<>
void EmitIR<IR::Opcode::CRC32ISO32>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitCRC<32, 4, false>(code, ctx, inst, [&](auto& Woutput, auto& Winput, auto& Wdata) { code.CRC32W(Woutput, Winput, Wdata); });
}

template<>
void EmitIR<IR::Opcode::CRC32ISO64>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitCRC<64, 8, false>(code, ctx, inst, [&](auto& Woutput, auto& Winput, auto& Xdata) { code.CRC32X(Woutput, Winput, Xdata); });
}

// Venture: the AES instructions are optional (FEAT_AES); without them the
// table implementation runs, as on x64 without AES-NI.
template<void (*op)(Common::Crypto::AES::State&, const Common::Crypto::AES::State&)>
static uint8x16_t AESFallback(uint8x16_t input) {
    Common::Crypto::AES::State in, out;
    vst1q_u8(in.data(), input);
    op(out, in);
    return vld1q_u8(out.data());
}

template<typename EmitFn>
static void EmitAES(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, uint8x16_t (*fallback)(uint8x16_t), EmitFn emit_fn) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    if (!ctx.conf.HasHostFeature(HostFeature::AES)) {
        ctx.reg_alloc.PrepareForCall(args[0]);
        code.MOV(Xscratch0, std::bit_cast<u64>(fallback));
        code.BLR(Xscratch0);
        ctx.reg_alloc.DefineAsRegister(inst, Q0);
        return;
    }

    auto Qoutput = ctx.reg_alloc.WriteQ(inst);
    auto Qinput = ctx.reg_alloc.ReadQ(args[0]);
    RegAlloc::Realize(Qoutput, Qinput);

    emit_fn(Qoutput, Qinput);
}

template<>
void EmitIR<IR::Opcode::AESDecryptSingleRound>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitAES(code, ctx, inst, &AESFallback<&Common::Crypto::AES::DecryptSingleRound>, [&](auto& Qoutput, auto& Qinput) {
        code.MOVI(Qoutput->toD(), oaknut::RepImm{0});
        code.AESD(Qoutput->B16(), Qinput->B16());
    });
}

template<>
void EmitIR<IR::Opcode::AESEncryptSingleRound>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitAES(code, ctx, inst, &AESFallback<&Common::Crypto::AES::EncryptSingleRound>, [&](auto& Qoutput, auto& Qinput) {
        code.MOVI(Qoutput->toD(), oaknut::RepImm{0});
        code.AESE(Qoutput->B16(), Qinput->B16());
    });
}

template<>
void EmitIR<IR::Opcode::AESInverseMixColumns>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitAES(code, ctx, inst, &AESFallback<&Common::Crypto::AES::InverseMixColumns>, [&](auto& Qoutput, auto& Qinput) {
        code.AESIMC(Qoutput->B16(), Qinput->B16());
    });
}

template<>
void EmitIR<IR::Opcode::AESMixColumns>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    EmitAES(code, ctx, inst, &AESFallback<&Common::Crypto::AES::MixColumns>, [&](auto& Qoutput, auto& Qinput) {
        code.AESMC(Qoutput->B16(), Qinput->B16());
    });
}

// Venture: the SM4 S-box (as in emit_x64_sm4.cpp), addressed through a 64-bit
// immediate so a position-independent executable may place it anywhere.
alignas(64) static constexpr std::array<u8, 256> substitution_box = {
    0xD6, 0x90, 0xE9, 0xFE, 0xCC, 0xE1, 0x3D, 0xB7,
    0x16, 0xB6, 0x14, 0xC2, 0x28, 0xFB, 0x2C, 0x05,
    0x2B, 0x67, 0x9A, 0x76, 0x2A, 0xBE, 0x04, 0xC3,
    0xAA, 0x44, 0x13, 0x26, 0x49, 0x86, 0x06, 0x99,
    0x9C, 0x42, 0x50, 0xF4, 0x91, 0xEF, 0x98, 0x7A,
    0x33, 0x54, 0x0B, 0x43, 0xED, 0xCF, 0xAC, 0x62,
    0xE4, 0xB3, 0x1C, 0xA9, 0xC9, 0x08, 0xE8, 0x95,
    0x80, 0xDF, 0x94, 0xFA, 0x75, 0x8F, 0x3F, 0xA6,
    0x47, 0x07, 0xA7, 0xFC, 0xF3, 0x73, 0x17, 0xBA,
    0x83, 0x59, 0x3C, 0x19, 0xE6, 0x85, 0x4F, 0xA8,
    0x68, 0x6B, 0x81, 0xB2, 0x71, 0x64, 0xDA, 0x8B,
    0xF8, 0xEB, 0x0F, 0x4B, 0x70, 0x56, 0x9D, 0x35,
    0x1E, 0x24, 0x0E, 0x5E, 0x63, 0x58, 0xD1, 0xA2,
    0x25, 0x22, 0x7C, 0x3B, 0x01, 0x21, 0x78, 0x87,
    0xD4, 0x00, 0x46, 0x57, 0x9F, 0xD3, 0x27, 0x52,
    0x4C, 0x36, 0x02, 0xE7, 0xA0, 0xC4, 0xC8, 0x9E,
    0xEA, 0xBF, 0x8A, 0xD2, 0x40, 0xC7, 0x38, 0xB5,
    0xA3, 0xF7, 0xF2, 0xCE, 0xF9, 0x61, 0x15, 0xA1,
    0xE0, 0xAE, 0x5D, 0xA4, 0x9B, 0x34, 0x1A, 0x55,
    0xAD, 0x93, 0x32, 0x30, 0xF5, 0x8C, 0xB1, 0xE3,
    0x1D, 0xF6, 0xE2, 0x2E, 0x82, 0x66, 0xCA, 0x60,
    0xC0, 0x29, 0x23, 0xAB, 0x0D, 0x53, 0x4E, 0x6F,
    0xD5, 0xDB, 0x37, 0x45, 0xDE, 0xFD, 0x8E, 0x2F,
    0x03, 0xFF, 0x6A, 0x72, 0x6D, 0x6C, 0x5B, 0x51,
    0x8D, 0x1B, 0xAF, 0x92, 0xBB, 0xDD, 0xBC, 0x7F,
    0x11, 0xD9, 0x5C, 0x41, 0x1F, 0x10, 0x5A, 0xD8,
    0x0A, 0xC1, 0x31, 0x88, 0xA5, 0xCD, 0x7B, 0xBD,
    0x2D, 0x74, 0xD0, 0x12, 0xB8, 0xE5, 0xB4, 0xB0,
    0x89, 0x69, 0x97, 0x4A, 0x0C, 0x96, 0x77, 0x7E,
    0x65, 0xB9, 0xF1, 0x09, 0xC5, 0x6E, 0xC6, 0x84,
    0x18, 0xF0, 0x7D, 0xEC, 0x3A, 0xDC, 0x4D, 0x20,
    0x79, 0xEE, 0x5F, 0x3E, 0xD7, 0xCB, 0x39, 0x48
};

template<>
void EmitIR<IR::Opcode::SM4AccessSubstitutionBox>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Woutput = ctx.reg_alloc.WriteW(inst);
    auto Windex = ctx.reg_alloc.ReadW(args[0]);
    RegAlloc::Realize(Woutput, Windex);

    code.MOV(Xscratch0, reinterpret_cast<u64>(substitution_box.data()));
    code.UXTB(Wscratch1, Windex);
    code.LDRB(Woutput, Xscratch0, Xscratch1);
}

template<>
void EmitIR<IR::Opcode::SHA256Hash>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool part1 = args[3].GetImmediateU1();

    if (part1) {
        auto Qx = ctx.reg_alloc.ReadWriteQ(args[0], inst);
        auto Qy = ctx.reg_alloc.ReadQ(args[1]);
        auto Qz = ctx.reg_alloc.ReadQ(args[2]);
        RegAlloc::Realize(Qx, Qy, Qz);

        code.SHA256H(Qx, Qy, Qz->S4());
    } else {
        auto Qx = ctx.reg_alloc.ReadQ(args[0]);
        auto Qy = ctx.reg_alloc.ReadWriteQ(args[1], inst);
        auto Qz = ctx.reg_alloc.ReadQ(args[2]);
        RegAlloc::Realize(Qx, Qy, Qz);

        code.SHA256H2(Qy, Qx, Qz->S4());  // Yes x and y are swapped
    }
}

template<>
void EmitIR<IR::Opcode::SHA256MessageSchedule0>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Qa = ctx.reg_alloc.ReadWriteQ(args[0], inst);
    auto Qb = ctx.reg_alloc.ReadQ(args[1]);
    RegAlloc::Realize(Qa, Qb);

    code.SHA256SU0(Qa->S4(), Qb->S4());
}

template<>
void EmitIR<IR::Opcode::SHA256MessageSchedule1>(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto Qa = ctx.reg_alloc.ReadWriteQ(args[0], inst);
    auto Qb = ctx.reg_alloc.ReadQ(args[1]);
    auto Qc = ctx.reg_alloc.ReadQ(args[2]);
    RegAlloc::Realize(Qa, Qb, Qc);

    code.SHA256SU1(Qa->S4(), Qb->S4(), Qc->S4());
}

}  // namespace Dynarmic::Backend::Arm64
