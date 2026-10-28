#include <optional>
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/x64/a64_emit_x64.h"

#include <algorithm>
#include <bit>
#include <iterator>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include "common/assert.h"
#include "common/common_types.h"
#include "dynarmic/ir/terminal.h"
#include "dynarmic/mcl/integer_of_size.hpp"
#include <boost/container/static_vector.hpp>

#include "dynarmic/backend/x64/a64_jitstate.h"
#include "dynarmic/backend/x64/abi.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/devirtualize.h"
#include "dynarmic/backend/x64/emit_x64.h"
#include "dynarmic/backend/x64/nzcv_util.h"
#include "dynarmic/backend/x64/perf_map.h"
#include "dynarmic/interface/profiling.h"
#include "dynarmic/backend/x64/stack_layout.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/a64_types.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/cond.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

#ifdef _WIN32
#    include <windows.h>
#else
#    include <sys/mman.h>
#endif

// TODO: Have ARM flags in host flags and not have them use up GPR registers unless necessary.
// TODO: Actually implement that proper instruction selector you've always wanted to sweetheart.

namespace Dynarmic::Backend::X64 {

using namespace Xbyak::util;

namespace {

// Add/Sub forms that EmitAdd/EmitSub emit as LEA, which leaves host FLAGS intact.
// Materializing an immediate lhs can use XOR, so only a register lhs qualifies.
bool PreservesFlagsWithLea(IR::Inst& inst) {
    const auto opcode = inst.GetOpcode();
    const bool is_add = opcode == IR::Opcode::Add32 || opcode == IR::Opcode::Add64;
    const bool is_sub = opcode == IR::Opcode::Sub32 || opcode == IR::Opcode::Sub64;
    if (!(is_add || is_sub) || inst.GetArg(0).IsImmediate() || !inst.GetArg(1).IsImmediate() ||
        !inst.GetArg(2).IsUnsignedImmediate(is_sub ? 1 : 0) ||
        inst.GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp) ||
        inst.GetAssociatedPseudoOperation(IR::Opcode::GetOverflowFromOp) ||
        inst.GetAssociatedPseudoOperation(IR::Opcode::GetNZCVFromOp)) {
        return false;
    }
    const s64 displacement = static_cast<s64>(inst.GetArg(1).GetImmediateAsU64());
    return displacement >= -s64(0x80000000) && displacement <= s64(0x7fffffff) &&
           (!is_sub || displacement != -s64(0x80000000));
}

// Instructions the emit loop lets sit between a flag-setting Add/Sub and the
// terminal: they emit only MOV/LEA (register-allocation moves included).
bool PreservesHostFlags(IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::Void:
    case IR::Opcode::A64GetX:
    case IR::Opcode::A64GetW:
    case IR::Opcode::A64SetX:
    case IR::Opcode::A64SetW:
        return true;
    default:
        return PreservesFlagsWithLea(inst);
    }
}

// Venture: values the emit loop may compute directly in the cached register
// the next A64SetX/SetSP (64-bit) or A64SetW (word) writes: register-only
// emitters with no memory access, host call or exit that could observe that
// register early. Every path of a word producer ends in a 32-bit write of its
// result register, which zeroes bits 32-63 as A64SetW must.
bool IsResultHintable(IR::Opcode opcode, bool word) {
    if (word) {
        switch (opcode) {
        case IR::Opcode::A64GetW:
        case IR::Opcode::Add32:
        case IR::Opcode::Sub32:
        case IR::Opcode::Mul32:
        case IR::Opcode::And32:
        case IR::Opcode::AndNot32:
        case IR::Opcode::Or32:
        case IR::Opcode::Eor32:
        case IR::Opcode::Not32:
        case IR::Opcode::ConditionalSelect32:
        case IR::Opcode::SignExtendByteToWord:
        case IR::Opcode::SignExtendHalfToWord:
        case IR::Opcode::ZeroExtendByteToWord:
        case IR::Opcode::ZeroExtendHalfToWord:
            return true;
        default:
            return false;
        }
    }
    switch (opcode) {
    case IR::Opcode::A64GetX:
    case IR::Opcode::Add64:
    case IR::Opcode::Sub64:
    case IR::Opcode::Mul64:
    case IR::Opcode::And64:
    case IR::Opcode::AndNot64:
    case IR::Opcode::Or64:
    case IR::Opcode::Eor64:
    case IR::Opcode::Not64:
    case IR::Opcode::LogicalShiftLeft64:
    case IR::Opcode::LogicalShiftRight64:
    case IR::Opcode::ArithmeticShiftRight64:
    case IR::Opcode::LogicalShiftLeftMasked64:
    case IR::Opcode::LogicalShiftRightMasked64:
    case IR::Opcode::ArithmeticShiftRightMasked64:
    case IR::Opcode::RotateRightMasked64:
    case IR::Opcode::ExtractRegister64:
    case IR::Opcode::ConditionalSelect64:
    case IR::Opcode::SignExtendByteToLong:
    case IR::Opcode::SignExtendHalfToLong:
    case IR::Opcode::SignExtendWordToLong:
    case IR::Opcode::ZeroExtendByteToLong:
    case IR::Opcode::ZeroExtendHalfToLong:
    case IR::Opcode::ZeroExtendWordToLong:
        return true;
    default:
        return false;
    }
}

enum class NZCVUse { None, Overwrite, Other };

// An A64 instruction's effect on a later NZCV read: None (no flag access, falls
// through), Overwrite (writes N, Z, C and V without reading them) or Other
// (reads flags, transfers control, or is a system or unrecognised encoding).
NZCVUse ClassifyNZCV(u32 i) {
    const bool sf = (i >> 31) & 1;
    const bool sets_flags = (i >> 29) & 1;
    if ((i & 0x1F800000) == 0x11000000) {  // add/subtract (immediate)
        return sets_flags ? NZCVUse::Overwrite : NZCVUse::None;
    }
    if ((i & 0x1F200000) == 0x0B000000) {  // add/subtract (shifted register)
        if (((i >> 22) & 3) == 3 || (!sf && (i & 0x8000)))
            return NZCVUse::Other;
        return sets_flags ? NZCVUse::Overwrite : NZCVUse::None;
    }
    if ((i & 0x1F200000) == 0x0B200000) {  // add/subtract (extended register)
        if (((i >> 22) & 3) != 0 || ((i >> 10) & 7) > 4)
            return NZCVUse::Other;
        return sets_flags ? NZCVUse::Overwrite : NZCVUse::None;
    }
    if ((i & 0x1F000000) == 0x0A000000) {  // logical (shifted register): ANDS, BICS set flags
        if (!sf && (i & 0x8000))
            return NZCVUse::Other;
        return ((i >> 29) & 3) == 3 ? NZCVUse::Overwrite : NZCVUse::None;
    }
    if ((i & 0x1F800000) == 0x12000000) {  // logical (immediate): ANDS sets flags
        const u32 n = (i >> 22) & 1;
        const u32 imms = (i >> 10) & 0x3F;
        const u32 combined = n << 6 | (~imms & 0x3F);
        if ((!sf && n) || combined < 2)
            return NZCVUse::Other;
        const u32 levels = (u32(1) << (31 - std::countl_zero(combined))) - 1;
        if ((imms & levels) == levels)
            return NZCVUse::Other;
        return ((i >> 29) & 3) == 3 ? NZCVUse::Overwrite : NZCVUse::None;
    }
    if ((i & 0x1F800000) == 0x12800000) {  // move wide
        return ((i >> 29) & 3) == 1 || (!sf && (i & 0x400000)) ? NZCVUse::Other : NZCVUse::None;
    }
    if ((i & 0x1F000000) == 0x10000000) {  // ADR, ADRP
        return NZCVUse::None;
    }
    if ((i & 0x1F800000) == 0x13000000) {  // bitfield
        const bool n = (i >> 22) & 1;
        return ((i >> 29) & 3) == 3 || n != sf || (!sf && (i & 0x208000)) ? NZCVUse::Other : NZCVUse::None;
    }
    if ((i & 0x1F800000) == 0x13800000) {  // extract
        const bool n = (i >> 22) & 1;
        return (i & 0x60200000) || n != sf || (!sf && (i & 0x8000)) ? NZCVUse::Other : NZCVUse::None;
    }
    if ((i & 0x7FE00000) == 0x1AC00000) {  // data processing (2 source): divides, variable shifts
        const u32 opcode = (i >> 10) & 0x3F;
        return opcode == 2 || opcode == 3 || (opcode >= 8 && opcode <= 11) ? NZCVUse::None : NZCVUse::Other;
    }
    if ((i & 0x7FE00000) == 0x5AC00000) {  // data processing (1 source): RBIT, REV*, CLZ, CLS
        const u32 opcode = (i >> 10) & 0x3F;
        return ((i >> 16) & 0x1F) == 0 && opcode <= 5 && (sf || opcode != 3) ? NZCVUse::None : NZCVUse::Other;
    }
    if ((i & 0x1F000000) == 0x1B000000) {  // data processing (3 source): multiplies
        const u32 op31 = (i >> 21) & 7;
        const bool o0 = (i >> 15) & 1;
        if (((i >> 29) & 3) != 0)
            return NZCVUse::Other;
        if (op31 == 0)
            return NZCVUse::None;
        if (sf && (op31 == 1 || op31 == 5 || ((op31 == 2 || op31 == 6) && !o0)))
            return NZCVUse::None;
        return NZCVUse::Other;
    }
    if ((i & 0x3B200C00) == 0x19000400) {  // memory copy and set (FEAT_MOPS) pass state in NZCV
        return NZCVUse::Other;
    }
    if ((i & 0x0A000000) == 0x08000000) {  // loads and stores
        return NZCVUse::None;
    }
    if ((i & 0x0E000000) == 0x0E000000) {  // SIMD and floating point
        if ((i & 0xFF20FC07) == 0x1E202000)  // FCMP, FCMPE
            return ((i >> 22) & 3) == 2 ? NZCVUse::Other : NZCVUse::Overwrite;
        if ((i & 0xFF200C00) == 0x1E200400 || (i & 0xFF200C00) == 0x1E200C00)  // FCCMP(E), FCSEL
            return NZCVUse::Other;
        return NZCVUse::None;
    }
    if ((i & 0xFFFFF01F) == 0xD503201F) {  // hints
        return NZCVUse::None;
    }
    if ((i & 0xFFFFF01F) == 0xD503301F) {  // CLREX, DSB, DMB, ISB, SB
        const u32 op2 = (i >> 5) & 7;
        return op2 == 2 || op2 >= 4 ? NZCVUse::None : NZCVUse::Other;
    }
    return NZCVUse::Other;
}

}  // namespace

A64EmitContext::A64EmitContext(const A64::UserConfig& conf, RegAlloc& reg_alloc, IR::Block& block, boost::container::stable_vector<Xbyak::Label>& shared_labels)
    : EmitContext(reg_alloc, block, shared_labels)
    , conf(conf)
{}

A64::LocationDescriptor A64EmitContext::Location() const {
    return A64::LocationDescriptor{block.Location()};
}

bool A64EmitContext::IsSingleStep() const {
    return Location().SingleStepping();
}

FP::FPCR A64EmitContext::FPCR(bool fpcr_controlled) const {
    return fpcr_controlled ? Location().FPCR() : Location().FPCR().ASIMDStandardValue();
}

A64EmitX64::FastDispatchTable::FastDispatchTable(size_t bytes, bool pages) : bytes(bytes), pages(pages) {
    if (!pages) {
        entries = new FastDispatchEntry[bytes / sizeof(FastDispatchEntry)];
        return;
    }
#ifdef _WIN32
    entries = static_cast<FastDispatchEntry*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    void* const p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    entries = p == MAP_FAILED ? nullptr : static_cast<FastDispatchEntry*>(p);
#endif
    if (!entries)
        throw std::bad_alloc();
}

A64EmitX64::FastDispatchTable::~FastDispatchTable() {
    if (!pages) {
        delete[] entries;
        return;
    }
#ifdef _WIN32
    VirtualFree(entries, 0, MEM_RELEASE);
#else
    munmap(entries, bytes);
#endif
}

void A64EmitX64::FastDispatchTable::Reset() {
    if (!pages) {
        std::fill_n(entries, bytes / sizeof(FastDispatchEntry), FastDispatchEntry{});
        return;
    }
#ifdef _WIN32
    // A decommitted range recommits as zero pages at the same address.
    const bool ok = VirtualFree(entries, bytes, MEM_DECOMMIT) && VirtualAlloc(entries, bytes, MEM_COMMIT, PAGE_READWRITE);
#else
    const bool ok = mmap(entries, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED;
#endif
    ASSERT_MSG(ok, "fast dispatch table reset failed");
}

A64EmitX64::A64EmitX64(BlockOfCode& code, A64::UserConfig conf, A64::Jit* jit_interface)
        : EmitX64(code), conf(conf)
        , fast_dispatch_table_mask{u32(((u64(1) << std::clamp<unsigned>(conf.fast_dispatch_table_bits, 10, 24)) - 1) << 4)}
        , fast_dispatch_table{size_t(fast_dispatch_table_mask) + sizeof(FastDispatchEntry), conf.fast_dispatch_table_pages}
        , jit_interface{jit_interface} {
    if (conf.fast_dispatch_census)
        census_shadow.emplace((size_t(1) << 18) * sizeof(u64) + (size_t(1) << 16) * sizeof(u64), true);
    if (conf.dispatch_site_cache && conf.HasOptimization(OptimizationFlag::FastDispatch) && !conf.fast_dispatch_census)
        dispatch_sites.emplace(dispatch_site_count * sizeof(FastDispatchEntry), false);
    host_return_stack = UsesHostReturnStack(conf);
    GenMemory128Accessors();
#ifdef _WIN32
    GenFastmemFallbacks();
#else
    // On demand (FastmemFallback); the full set is 2,378,138 bytes.
    xmm_read_fallbacks = true;
    code.ReserveThunkArena(2560 * 1024);
#endif
    GenTerminalHandlers();
    ResetDispatchSites();
    exception_handler.Register(code);
    code.PreludeComplete();
    if (conf.HasOptimization(OptimizationFlag::FastDispatch) && conf.fast_dispatch_table_pages)
        SeedFastDispatchTable();

    if (conf.fastmem_pointer.has_value()) {
        exception_handler.SetFastmemCallback([this](u64 rip_) {
            return FastmemCallback(rip_);
        });
    }
}

A64EmitX64::~A64EmitX64() = default;

// Venture: `select` is a ConditionalSelectFlags32/64 and the only user of
// `compare`, an FPCompare32/64 with a non-immediate operand.
static bool IsFPFlagsSelect(const IR::Inst& select, const IR::Inst& compare) {
    const auto op = select.GetOpcode();
    return (op == IR::Opcode::ConditionalSelectFlags32 || op == IR::Opcode::ConditionalSelectFlags64) &&
        !select.GetArg(3).IsImmediate() && select.GetArg(3).GetInst() == &compare && compare.UseCount() == 1 &&
        (compare.GetOpcode() == IR::Opcode::FPCompare32 || compare.GetOpcode() == IR::Opcode::FPCompare64) &&
        !(compare.GetArg(0).IsImmediate() && compare.GetArg(1).IsImmediate());
}

// Venture: an FPCompare may move later past `inst`: no memory access or exit,
// no FPCR write and no FPSR access other than accumulating cumulative
// exception flags (which commutes with the compare's own); guest register
// writes are the only other side effects allowed.
static bool FPCompareMayMovePast(const IR::Inst& inst) {
    const auto op = inst.GetOpcode();
    switch (op) {
    case IR::Opcode::A64SetW:
    case IR::Opcode::A64SetX:
    case IR::Opcode::A64SetS:
    case IR::Opcode::A64SetD:
    case IR::Opcode::A64SetQ:
    case IR::Opcode::A64SetSP:
        return true;
    default:
        break;
    }
    if (IR::IsMemoryReadOrWrite(op) || (IR::ReadsFromFPSR(op) && !IR::ReadsFromFPSRCumulativeExceptionBits(op)))
        return false;
    // FP arithmetic's only side effect is OR-ing cumulative exception flags.
    return !IR::MayHaveSideEffects(op) || IR::WritesToFPSRCumulativeExceptionBits(op);
}

// Venture: move each FPCompare whose only user is a ConditionalSelectFlags32/64
// (FCMP + FCSEL/CSEL with NZCV dead afterwards) to just before that select, so
// the select tests the compare's host FLAGS.
/// Venture: the scaled operand (0 or 1) of an Add32/Add64 that one LEA can
/// emit as [x + y*scale]: y is a LogicalShiftLeft of the same width by 1-3 with
/// no other user, and the Add has no carry in and no flag outputs.
static std::optional<size_t> ScaledAddOperand(IR::Inst& add, const IR::Inst* host_flags_inst) {
    const bool is64 = add.GetOpcode() == IR::Opcode::Add64;
    if ((!is64 && add.GetOpcode() != IR::Opcode::Add32) || &add == host_flags_inst ||
        !add.GetArg(2).IsImmediate() || add.GetArg(2).GetU1() ||
        add.GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp) ||
        add.GetAssociatedPseudoOperation(IR::Opcode::GetOverflowFromOp) ||
        add.GetAssociatedPseudoOperation(IR::Opcode::GetNZCVFromOp)) {
        return std::nullopt;
    }
    for (const size_t k : {size_t(1), size_t(0)}) {
        const IR::Value scaled = add.GetArg(k), other = add.GetArg(1 - k);
        if (scaled.IsImmediate() || other.IsImmediate())
            continue;
        IR::Inst* const shift = scaled.GetInst();
        if (shift->GetOpcode() != (is64 ? IR::Opcode::LogicalShiftLeft64 : IR::Opcode::LogicalShiftLeft32) ||
            shift->UseCount() != 1 || !shift->GetArg(1).IsImmediate() || shift->GetArg(1).GetU8() < 1 || shift->GetArg(1).GetU8() > 3 ||
            shift->GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp)) {
            continue;
        }
        return k;
    }
    return std::nullopt;
}

void A64EmitX64::FoldScaledIndices(A64EmitContext& ctx) const {
    for (auto& inst : ctx.block.instructions) {
        if (const auto k = ScaledAddOperand(inst, ctx.host_flags_inst))
            ctx.scaled_indices.push_back(inst.GetArg(*k).GetInst());
    }
}

static void MoveFPComparesToSelects(IR::Block& block) {
    for (auto iter = block.instructions.begin(); iter != block.instructions.end();) {
        IR::Inst& compare = *iter++;
        const auto op = compare.GetOpcode();
        if ((op != IR::Opcode::FPCompare32 && op != IR::Opcode::FPCompare64) || compare.UseCount() != 1)
            continue;
        size_t distance = 0;
        for (auto user = iter; user != block.instructions.end() && distance < 32; ++user, ++distance) {
            if (IsFPFlagsSelect(*user, compare)) {
                if (user != iter) {
                    block.instructions.remove(compare);
                    block.instructions.insert(user, &compare);
                }
                break;
            }
            if (!FPCompareMayMovePast(*user))
                break;
        }
    }
}

/// Venture: the widest bit of a GetQ value that `user` reads through argument `index`.
static size_t QuadBitsRead(const IR::Inst& user, size_t index) {
    size_t element = 0;
    switch (user.GetOpcode()) {
    case IR::Opcode::VectorGetElement8: element = 8; break;
    case IR::Opcode::VectorGetElement16: element = 16; break;
    case IR::Opcode::VectorGetElement32: element = 32; break;
    case IR::Opcode::VectorGetElement64: element = 64; break;
    case IR::Opcode::VectorZeroUpper: return 64;
    default: return 128;
    }
    return index == 0 && user.GetArg(1).IsImmediate() ? (user.GetArg(1).GetU8() + 1) * element : 128;
}

/// Venture: a GetQ whose users read only its low 32 or 64 bits loads as GetS
/// or GetD (MOVD/MOVQ), the same bits with zeros above. Scalar S/D reads come
/// from the frontend as lane 0 of GetQ; this keeps the Q register's other lanes
/// out of scalar results, which then need no INSERTPS when they are written back
/// (IsZeroAboveScalar). After get/set elimination, so only real loads change.
static void NarrowQuadGets(IR::Block& block) {
    struct QuadGet { IR::Inst* get; size_t bits; size_t uses; };
    boost::container::small_vector<QuadGet, 16> gets;
    for (auto& inst : block.instructions) {
        for (size_t index = 0; index < inst.NumArgs(); ++index) {
            const IR::Value arg = inst.GetArg(index);
            if (arg.IsEmpty() || arg.IsImmediate())
                continue;
            for (QuadGet& get : gets) {
                if (get.get == arg.GetInst()) {
                    get.bits = std::max(get.bits, QuadBitsRead(inst, index));
                    get.uses++;
                }
            }
        }
        if (inst.GetOpcode() == IR::Opcode::A64GetQ)
            gets.push_back({&inst, 0, 0});
    }
    for (const QuadGet& get : gets) {
        if (get.uses == get.get->UseCount() && get.uses != 0 && get.bits <= 64)
            get.get->ReplaceOpcode(get.bits <= 32 ? IR::Opcode::A64GetS : IR::Opcode::A64GetD);
    }
}

A64EmitX64::BlockDescriptor A64EmitX64::Emit(IR::Block& block) {
    SetCompilePhase(CompilePhase::Emit);
    // Supervisor-call regions below may change FPCR; their terminals use the
    // shared handlers even after an emission that ended early.
    static_fpcr_terminal = false;
    known_return.reset();
    host_call_return.reset();
    if (conf.very_verbose_debugging_output) [[unlikely]] {
        std::puts(IR::DumpBlock(block).c_str());
    }

    code.EnableWriting();
    new (&this->reg_alloc) RegAlloc{[this] {
        std::bitset<32> gprs = any_gpr;
        if (conf.fastmem_pointer)
            gprs.reset(size_t(HostLoc::R13));
        if (conf.page_table)
            gprs.reset(size_t(HostLoc::R14));
        if (conf.enable_register_cache)
            for (const size_t index : BlockOfCode::a64_cached_indices)
                if (const auto reg = code.A64CachedRegister(index))
                    gprs.reset(size_t(reg->getIdx()));  // HostLoc GPRs follow x64 encoding order.
        return gprs;
    }(), any_xmm};

    A64EmitContext ctx{conf, reg_alloc, block, shared_labels};
    FoldMemoryAddresses(ctx);
    if (!conf.very_verbose_debugging_output) {
        MoveFPComparesToSelects(block);
        NarrowQuadGets(block);
    }
    // Before next-use analysis: it drops the final NZCV store and its pseudo-op.
    terminal_host_flags = false;
    exit_nzcv_ranges.clear();
    IR::Inst* const host_flags_producer = !conf.diagnostic_entry.record && conf.nzcv_exit_liveness && !conf.enable_cycle_counting &&
        !conf.very_verbose_debugging_output && !ctx.IsSingleStep() ? PrepareHostFlagsExit(ctx) : nullptr;
    ctx.host_flags_inst = host_flags_producer;
    if (!conf.very_verbose_debugging_output)
        FoldScaledIndices(ctx);
    bool host_flags_live = false;
    next_uses.Build(block);
    reg_alloc.SetNextUses(&next_uses);
    // Raw LinkBlock terminals (mode changes, callbacks and translation caps)
    // remain checkpoints. Only ordinary conditional edges can skip a poll.
    elide_forward_halt_polls = conf.enable_bounded_halt_polling &&
        !conf.enable_cycle_counting && !ctx.IsSingleStep() &&
        (std::holds_alternative<IR::Term::If>(block.GetTerminal()) ||
         std::holds_alternative<IR::Term::CheckBit>(block.GetTerminal()));

    // Start emitting.
    code.align(conf.block_alignment);
    const auto* const entrypoint = code.getCurr();
    if (conf.profile_cookie && (!conf.profile_cookie_site || conf.profile_cookie_site(ctx.Location().PC())))
        code.mov(code.dword[code.ABI_JIT_PTR + offsetof(A64JitState, profile_cookie)], conf.profile_cookie(ctx.Location().PC()));
    if (conf.profile_function_links) {
        ASSERT(conf.profile_cookie && !conf.profile_cookie_site);
        // Keep the internal body aligned like an uninstrumented block.
        code.align(conf.block_alignment);
        profile_prefix_size = size_t(code.getCurr() - entrypoint);
    }
    if (conf.profile_entry_counter) {
        // Entry scratch GPRs and host flags are dead; cached X19/X20/X30
        // occupy R12/R14/RBP. Counter is owner-only and read after Run stops.
        if (const auto counter = conf.profile_entry_counter(ctx.Location().PC())) {
            code.mov(rax, reinterpret_cast<std::uintptr_t>(counter));
            code.inc(code.qword[rax]);
        }
    }
    if (conf.diagnostic_entry.record && conf.diagnostic_entry.selected &&
        conf.diagnostic_entry.selected(ctx.Location().PC())) {
        // Entry has no allocated IR temporaries. CallFunction synchronizes all
        // cached guest registers; MXCSR returns to the unchanged guest mode.
        code.SwitchMxcsrOnExit();
        code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(conf.diagnostic_entry.user));
        code.mov(code.ABI_PARAM2, ctx.Location().PC());
        code.mov(code.ABI_PARAM3, code.ABI_JIT_PTR);
        code.CallFunction(conf.diagnostic_entry.record);
        code.SwitchMxcsrOnEntry();
    }
    // Cached X30 occupies RBP, as in Venture's original cache patch. The
    // dispatcher still saves the host RBP; emitted Windows unwind records use
    // RSP and its recorded stack slots rather than this optional frame chain.
    const bool frame_chain = !conf.enable_register_cache &&
        (conf.enable_block_frame_chain || conf.very_verbose_debugging_output);
    if (frame_chain) {
        code.mov(code.qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, abi_base_pointer)], rbp);
        code.lea(rbp, code.ptr[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, abi_base_pointer) - 8]);
    }

    DEBUG_ASSERT(block.GetCondition() == IR::Cond::AL);
    typedef void (EmitX64::*EmitHandlerFn)(EmitContext& context, IR::Inst* inst);
    constexpr EmitHandlerFn opcode_handlers[] = {
#define OPCODE(name, type, ...) &EmitX64::Emit##name,
#define A32OPC(name, type, ...)
#define A64OPC(name, type, ...)
#include "dynarmic/ir/opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC
    };
    typedef void (A64EmitX64::*A64EmitHandlerFn)(A64EmitContext& context, IR::Inst* inst);
    constexpr A64EmitHandlerFn a64_handlers[] = {
#define OPCODE(...)
#define A32OPC(...)
#define A64OPC(name, type, ...) &A64EmitX64::EmitA64##name,
#include "dynarmic/ir/opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC
    };

    bool terminal_emitted = false;
    IR::Inst* flags_inst = nullptr;
    static_fpcr_terminal = (conf.inline_fast_dispatch || conf.inline_rsb_pop || dispatch_sites) && !conf.fast_dispatch_census &&
        std::none_of(block.instructions.begin(), block.instructions.end(), [](const IR::Inst& inst) {
            const auto op = inst.GetOpcode();
            return op == IR::Opcode::A64SetFPCR || op == IR::Opcode::A64CallSupervisor;
        });
    if ((conf.svc_return_link || host_return_stack) && conf.HasOptimization(OptimizationFlag::ReturnStackBuffer) &&
        conf.HasOptimization(OptimizationFlag::BlockLinking) && !ctx.IsSingleStep()) {
        const auto terminal = block.GetTerminal();
        const auto halt = std::get_if<IR::Term::CheckHalt>(&terminal);
        const auto leaf = halt ? &halt->else_ : std::get_if<IR::Term::LeafTerminal>(&terminal);
        if (leaf && std::holds_alternative<IR::Term::PopRSBHint>(*leaf)) {
            size_t pushes = 0;
            for (const auto& inst : block.instructions) {
                if (inst.GetOpcode() == IR::Opcode::PushRSB) {
                    ++pushes;
                    known_return = IR::LocationDescriptor{inst.GetArg(0).GetU64()};
                }
            }
            if (pushes != 1)
                known_return.reset();
        }
    }
    bool flags_stored = false;

    // Venture: a value whose only user is the next A64SetX/SetSP of a cached
    // register (after its pseudo-operations and their NZCV store) is computed
    // in that register; the set then emits nothing.
    const auto result_hint = [this, &block](auto at) -> std::optional<HostLoc> {
        IR::Inst& value = *at;
        const bool word = value.GetType() == IR::Type::U32;
        if (!IsResultHintable(value.GetOpcode(), word))
            return std::nullopt;
        size_t pseudo_uses = 0;
        auto user = std::next(at);
        for (; user != block.instructions.end(); ++user) {
            const auto op = user->GetOpcode();
            if (op == IR::Opcode::Void || op == IR::Opcode::A64SetNZCV)
                continue;
            if (IR::IsAPseudoOperation(op) && !user->GetArg(0).IsImmediate() && user->GetArg(0).GetInst() == &value) {
                ++pseudo_uses;
                continue;
            }
            break;
        }
        if (user == block.instructions.end())
            return std::nullopt;
        const auto set_op = user->GetOpcode();
        const bool is_reg = set_op == (word ? IR::Opcode::A64SetW : IR::Opcode::A64SetX);
        if (!is_reg && (word || set_op != IR::Opcode::A64SetSP))
            return std::nullopt;
        const IR::Value set = user->GetArg(is_reg ? 1 : 0);
        if (set.IsImmediate() || set.GetInst() != &value || value.UseCount() != 1 + pseudo_uses)
            return std::nullopt;
        const auto cached = code.A64CachedRegister(is_reg ? static_cast<size_t>(user->GetArg(0).GetA64RegRef()) : 31);
        if (!cached)
            return std::nullopt;
        return static_cast<HostLoc>(cached->getIdx() + static_cast<size_t>(HostLoc::RAX));
    };

    for (auto iter = block.instructions.begin(); iter != block.instructions.end(); ++iter) {
        code.EnsureMemoryCommitted();
        auto& inst = *iter;
        reg_alloc.SetCurrentInstruction(&inst);
        auto* instruction = &inst;
        // Fuse the final boolean producer with CheckBit. No guest state is
        // retained across blocks; all earlier stores and fault exits stay intact.
        const auto next = std::next(iter);
        const auto terminal = block.GetTerminal();
        const auto check = std::get_if<IR::Term::CheckBit>(&terminal);
        const auto opcode = instruction->GetOpcode();
        if (!conf.enable_cycle_counting && !conf.very_verbose_debugging_output && check &&
            next != block.instructions.end() && std::next(next) == block.instructions.end() &&
            next->GetOpcode() == IR::Opcode::A64SetCheckBit &&
            !next->GetArg(0).IsImmediate() && next->GetArg(0).GetInst() == instruction &&
            instruction->UseCount() == 1 &&
            (opcode == IR::Opcode::IsZero32 || opcode == IR::Opcode::IsZero64 ||
             opcode == IR::Opcode::TestBit)) {
            next->Invalidate();
            auto args = reg_alloc.GetArgumentInfo(instruction);
            const auto operand = reg_alloc.UseGpr(code, args[0]);
            Xbyak::Label fail;
            if (opcode == IR::Opcode::TestBit) {
                ASSERT(args[1].IsImmediate());
                code.bt(operand, args[1].GetImmediateU8());
                code.jnc(fail);
            } else {
                const auto value = operand.changeBit(opcode == IR::Opcode::IsZero32 ? 32 : 64);
                code.test(value, value);
                code.jnz(fail);
            }
            reg_alloc.EndOfAllocScope();
            if (frame_chain)
                code.mov(rbp, code.qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, abi_base_pointer)]);
            EmitLeafTerminal(check->then_, ctx.Location().SetSingleStepping(false), ctx.IsSingleStep());
            code.L(fail);
            if (frame_chain)
                code.mov(rbp, code.qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, abi_base_pointer)]);
            EmitLeafTerminal(check->else_, ctx.Location().SetSingleStepping(false), ctx.IsSingleStep());
            terminal_emitted = true;
            ++iter;
            continue;
        }

        // Match the immediate LEA paths in EmitAdd/EmitSub. The terminal's
        // host-FLAGS producer is never emitted as LEA.
        const bool preserves_flags_with_lea = instruction != host_flags_producer && PreservesFlagsWithLea(*instruction);

        // Memory operations, calls and unrelated arithmetic invalidate FLAGS.
        // GetW/GetX, SetW/SetX and the proven LEA paths only emit MOV/LEA,
        // including register-allocation moves. Architectural NZCV stores remain.
        ctx.select_flags_in_host = false;
        // An FPCompare right before its only user, a ConditionalSelectFlags32/64,
        // leaves its FLAGS for the select (MoveFPComparesToSelects).
        if (ctx.fp_flags_select != instruction)
            ctx.fp_flags_select = nullptr;
        if ((opcode == IR::Opcode::FPCompare32 || opcode == IR::Opcode::FPCompare64) && !conf.very_verbose_debugging_output &&
            next != block.instructions.end() && IsFPFlagsSelect(*next, *instruction))
            ctx.fp_flags_select = &*next;
        switch (opcode) {
        case IR::Opcode::ConditionalSelectFlags32:
        case IR::Opcode::ConditionalSelectFlags64:
        case IR::Opcode::ConditionalSelectFlagsNZCV:
            // Selecting on the FLAGS-resident NZCV emits only MOV/CMOV/CMC
            // pairs and keeps FLAGS; any other select reloads them.
            ctx.select_flags_in_host = flags_inst && !conf.very_verbose_debugging_output &&
                !instruction->GetArg(3).IsImmediate() && instruction->GetArg(3).GetInst() == flags_inst;
            if (!ctx.select_flags_in_host) {
                flags_inst = nullptr;
                flags_stored = false;
                host_flags_live = false;
            }
            break;
        case IR::Opcode::Add32:
        case IR::Opcode::Add64:
        case IR::Opcode::Sub32:
        case IR::Opcode::Sub64:
            if (preserves_flags_with_lea) break;
            flags_inst = instruction->GetAssociatedPseudoOperation(IR::Opcode::GetNZCVFromOp);
            flags_stored = false;
            host_flags_live = instruction == host_flags_producer;
            break;
        case IR::Opcode::GetNZCVFromOp:
            if (instruction != flags_inst) {
                flags_inst = nullptr;
                flags_stored = false;
                host_flags_live = false;
            }
            break;
        case IR::Opcode::Void:  // invalidated: emits nothing
        case IR::Opcode::A64SetX:
        case IR::Opcode::A64SetW:
        case IR::Opcode::A64GetX:
        case IR::Opcode::A64GetW:
            break;
        case IR::Opcode::A64SetNZCV:
            flags_stored = !instruction->GetArg(0).IsImmediate() && instruction->GetArg(0).GetInst() == flags_inst;
            if (!flags_stored) flags_inst = nullptr;
            host_flags_live = false;
            break;
        default:
            flags_inst = nullptr;
            flags_stored = false;
            host_flags_live = false;
            break;
        }
        if ((opcode == IR::Opcode::LogicalShiftLeft32 || opcode == IR::Opcode::LogicalShiftLeft64) && ctx.IsScaledIndex(instruction)) {
            // Its only user, an Add, applies the shift as the LEA scale.
            auto args = reg_alloc.GetArgumentInfo(instruction);
            reg_alloc.DefineValue(code, instruction, args[0]);
            goto finish_this_inst;
        }
        if (opcode == IR::Opcode::Add64 && ctx.IsFoldedAddress(instruction)) {
            // Its only user addresses memory as [base + imm32]; the value stays in base's location.
            auto args = reg_alloc.GetArgumentInfo(instruction);
            reg_alloc.DefineValue(code, instruction, args[0]);
            goto finish_this_inst;
        }
        reg_alloc.SetResultHint(result_hint(iter));
        // Call the relevant Emit* member function.
        switch (opcode) {
#define OPCODE(name, type, ...) case IR::Opcode::name: goto opcode_branch;
#define A32OPC(name, type, ...)
#define A64OPC(name, type, ...) case IR::Opcode::A64##name: goto a64_branch;
#include "dynarmic/ir/opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC
        default:
            UNREACHABLE();
        }
opcode_branch:
        if (opcode == IR::Opcode::PushRSB && (conf.profile_call_edge.record || conf.profile_direct_call_counter || known_return || host_return_stack))
            EmitPushRSB(ctx, &inst);
        else
            (this->*opcode_handlers[size_t(opcode)])(ctx, &inst);
        goto finish_this_inst;
a64_branch:
        (this->*a64_handlers[size_t(opcode) - std::size(opcode_handlers)])(ctx, &inst);
finish_this_inst:
        ctx.reg_alloc.EndOfAllocScope();
        ctx.reg_alloc.SetResultHint(std::nullopt);
#ifndef NDEBUG
        if (conf.very_verbose_debugging_output)
            EmitVerboseDebuggingOutput(reg_alloc);
#endif
    }

    reg_alloc.AssertNoMoreUses();
    code.EnsureMemoryCommitted();
    if (conf.enable_cycle_counting)
        EmitAddCycles(block.CycleCount());
    if (frame_chain)
        code.mov(rbp, code.qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, abi_base_pointer)]);
    terminal_nzcv_in_flags = flags_inst && flags_stored &&
        !conf.enable_cycle_counting && !conf.very_verbose_debugging_output &&
        std::holds_alternative<IR::Term::If>(block.terminal);
    if (host_flags_producer) {
        // PrepareHostFlagsExit admitted only FLAGS-preserving instructions after it.
        ASSERT(host_flags_live && !terminal_emitted);
        terminal_host_flags = true;
        terminal_carry_inverted = ctx.host_flags_carry_inverted;
    }
    if (!terminal_emitted) {
        if (host_call_return)
            EmitHostCall(*host_call_return, ctx.Location().SetSingleStepping(false), ctx.IsSingleStep(), block.terminal);
        else
            EmitTerminal(block.terminal, ctx.Location().SetSingleStepping(false), ctx.IsSingleStep());
    }
    terminal_nzcv_in_flags = false;
    terminal_host_flags = false;
    static_fpcr_terminal = false;
    code.int3();
    ctx.emitting_deferred = true;
    // Handlers that register unwind frames stay in the hot region: unwind
    // entries are appended in address order and the cold region lies beyond them.
    for (auto& deferred_emit : ctx.framed_deferred_emits) {
        code.EnsureMemoryCommitted();
        deferred_emit();
    }
    // Venture: deferred handlers go to the cold region when there is one.
    const bool cold = code.HasColdRegion() && !ctx.deferred_emits.empty();
    if (cold)
        code.SwitchToColdCode();
    for (auto& deferred_emit : ctx.deferred_emits) {
        code.EnsureMemoryCommitted();
        deferred_emit();
    }
    code.EnsureMemoryCommitted();
    code.int3();
    if (cold)
        code.SwitchToHotCode();

    const size_t size = size_t(code.getCurr() - entrypoint);

    const A64::LocationDescriptor descriptor{block.Location()};
    const A64::LocationDescriptor end_location{block.EndLocation()};

    const auto range = boost::icl::discrete_interval<u64>::closed(descriptor.PC(), end_location.PC() - 1);
    SetCompilePhase(CompilePhase::CacheInsert);
    block_ranges.AddRange(range, descriptor);
    // An exit without its NZCV store depends on the successor code that overwrites it.
    for (const auto& successor : exit_nzcv_ranges)
        block_ranges.AddRange(successor, descriptor);
    for (const auto& [begin, end] : inlined_code)
        block_ranges.AddRange(boost::icl::discrete_interval<u64>::closed(begin, end - 1), descriptor);

    auto bdesc = RegisterBlock(descriptor, entrypoint, size);
    SetCompilePhase(CompilePhase::Finalize);
    code.DisableWriting();
    shared_labels.clear();
    return bdesc;
}

void A64EmitX64::ClearCache() {
    EmitX64::ClearCache();
    profile_internal_links.clear();
    block_ranges.ClearCache();
    ClearFastDispatchTable();
    next_dispatch_site = 0;
    ResetDispatchSites();
    fastmem_patch_info.clear();
}

void A64EmitX64::InvalidateCacheRanges(const boost::icl::interval_set<u64>& ranges) {
    const auto invalidated = block_ranges.InvalidateRanges(ranges);
    InvalidateBasicBlocks(invalidated);
    // Slots may hold an invalidated block's code.
    if (!invalidated.empty())
        ResetDispatchSites();
}

std::optional<u64> A64EmitX64::NZCVOverwriteEnd(u64 pc) const {
    // The last byte of the first instruction at pc that overwrites NZCV before
    // any flag read, branch or unrecognised encoding, within 16 instructions.
    if (pc & 3)
        return std::nullopt;
    for (u64 i = 0; i < 16; ++i) {
        const auto word = conf.callbacks->MemoryReadCode(pc + i * 4);
        if (!word)
            return std::nullopt;
        switch (ClassifyNZCV(*word)) {
        case NZCVUse::Overwrite:
            return pc + i * 4 + 3;
        case NZCVUse::None:
            continue;
        case NZCVUse::Other:
            return std::nullopt;
        }
    }
    return std::nullopt;
}

IR::Inst* A64EmitX64::PrepareHostFlagsExit(A64EmitContext& ctx) {
    // An If terminal after a plain ADDS/SUBS whose NZCV only the final SetNZCV
    // stores: the terminal can branch on the operation's own FLAGS and store
    // NZCV just on the exits that may read it.
    exit_nzcv_live = {true, true};
    auto& block = ctx.block;
    const auto block_terminal = block.GetTerminal();
    const auto* const terminal = std::get_if<IR::Term::If>(&block_terminal);
    if (!terminal || terminal->if_ == IR::Cond::AL || terminal->if_ == IR::Cond::NV)
        return nullptr;

    // Everything after the final SetNZCV, and between its producer and it, must keep FLAGS.
    auto iter = block.instructions.end();
    IR::Inst* set_nzcv = nullptr;
    while (iter != block.instructions.begin()) {
        auto& inst = *--iter;
        if (inst.GetOpcode() == IR::Opcode::A64SetNZCV) {
            set_nzcv = &inst;
            break;
        }
        if (!PreservesHostFlags(inst))
            return nullptr;
    }
    if (!set_nzcv || set_nzcv->GetArg(0).IsImmediate())
        return nullptr;
    IR::Inst* const nzcv = set_nzcv->GetArg(0).GetInst();
    if (nzcv->GetOpcode() != IR::Opcode::GetNZCVFromOp || nzcv->UseCount() != 1)
        return nullptr;
    IR::Inst* const producer = nzcv->GetArg(0).GetInst();
    // Plain ADDS (carry in 0) or SUBS (carry in 1): emitted as ADD or CMP/SUB.
    bool carry_in_set;
    switch (producer->GetOpcode()) {
    case IR::Opcode::Add32:
    case IR::Opcode::Add64:
        carry_in_set = false;
        break;
    case IR::Opcode::Sub32:
    case IR::Opcode::Sub64:
        carry_in_set = true;
        break;
    default:
        return nullptr;
    }
    if (!producer->GetArg(2).IsImmediate() || producer->GetArg(2).GetU1() != carry_in_set)
        return nullptr;
    if (producer->GetAssociatedPseudoOperation(IR::Opcode::GetCarryFromOp) ||
        producer->GetAssociatedPseudoOperation(IR::Opcode::GetOverflowFromOp))
        return nullptr;
    while (true) {
        if (iter == block.instructions.begin())
            return nullptr;
        auto& inst = *--iter;
        if (&inst == producer)
            break;
        if (&inst != nzcv && !PreservesHostFlags(inst))
            return nullptr;
    }

    const std::array<const IR::Term::LeafTerminal*, 2> exits{&terminal->else_, &terminal->then_};
    bool any_dead = false;
    for (size_t i = 0; i < exits.size(); ++i) {
        std::optional<IR::LocationDescriptor> next;
        if (const auto* link = std::get_if<IR::Term::LinkBlock>(exits[i]))
            next = link->next;
        else if (const auto* fast = std::get_if<IR::Term::LinkBlockFast>(exits[i]))
            next = fast->next;
        if (!next)
            continue;
        const u64 pc = A64::LocationDescriptor{*next}.PC();
        if (const auto end = NZCVOverwriteEnd(pc)) {
            exit_nzcv_live[i] = false;
            exit_nzcv_ranges.push_back(boost::icl::discrete_interval<u64>::closed(pc, *end));
            any_dead = true;
        }
    }
    if (!any_dead) {
        exit_nzcv_ranges.clear();
        return nullptr;
    }
    set_nzcv->Invalidate();
    nzcv->Invalidate();
    return producer;
}

void A64EmitX64::EmitExitNZCV() {
    // FLAGS still hold the final Add/Sub result. Store NZCV as the flag-setting
    // emitters do: AH from LAHF with CF as ARM C, AL = OF, upper bits zero.
    code.mov(eax, 0);  // MOV, not XOR: FLAGS must survive
    if (terminal_carry_inverted)
        code.cmc();
    code.lahf();
    code.seto(code.al);
    code.mov(dword[code.ABI_JIT_PTR + offsetof(A64JitState, cpsr_nzcv)], eax);
}

void A64EmitX64::ResetDispatchSites() {
    // An empty slot's ~0 tag matches only a branch to PC ~0, which its handler
    // dispatches without caching.
    if (dispatch_sites)
        std::fill_n(dispatch_sites->data(), dispatch_site_count, FastDispatchEntry{~u64(0), terminal_handler_site_miss});
}

void A64EmitX64::ClearFastDispatchTable() {
    if (conf.HasOptimization(OptimizationFlag::FastDispatch)) {
        fast_dispatch_table.Reset();
        if (conf.fast_dispatch_table_pages)
            SeedFastDispatchTable();
    }
}

void A64EmitX64::SeedFastDispatchTable() {
    // Location 0 would match a zero (empty) tag. Its one possible entry gets the
    // ~0 empty tag, which no A64 location matches (the PC would be unaligned).
    // The hash is the handlers' SSE4.2 CRC32C of the table address seeded with
    // the location, here 0.
    u32 hash = 0;
    if (code.HasHostFeature(HostFeature::SSE42)) {
        u64 data = u64(fast_dispatch_table.data());
        for (int bit = 0; bit < 64; ++bit, data >>= 1)
            hash = (hash >> 1) ^ (0x82F63B78u & (0u - ((hash ^ u32(data)) & 1)));
    }
    fast_dispatch_table.data()[(hash & fast_dispatch_table_mask) / sizeof(FastDispatchEntry)] = {};
}

void A64EmitX64::GenTerminalHandlers() {
    // PC ends up in rcx, location_descriptor ends up in r9
    //static_assert(ABI_ALL_CALLEE_SAVE.test(size_t(HostLoc::R12)));
    const auto calculate_location_descriptor = [this] {
        // This calculation has to match up with A64::LocationDescriptor::UniqueHash
        // TODO: Optimization is available here based on known state of fpcr.
        code.mov(rdx, qword[code.ABI_JIT_PTR + offsetof(A64JitState, pc)]);
        code.mov(rcx, A64::LocationDescriptor::pc_mask);
        code.and_(rcx, rdx);
        code.mov(r9d, dword[code.ABI_JIT_PTR + offsetof(A64JitState, fpcr)]);
        code.and_(r9d, A64::LocationDescriptor::fpcr_mask);
        code.shl(r9, A64::LocationDescriptor::fpcr_shift);
        code.or_(r9, rcx);
    };

    Xbyak::Label fast_dispatch_cache_miss;
    Xbyak::Label rsb_cache_miss;
    Xbyak::Label census_rsb_miss;
    // Diagnostic census: RCX and RAX are free after the location calculation
    // (RAX holds only the RSB index); host flags are dead in these handlers.
    u64* const census = conf.fast_dispatch_census;
    const auto count = [&](size_t index) {
        code.mov(rcx, u64(census));
        code.inc(qword[rcx + index * sizeof(u64)]);
    };

    code.align();
    terminal_handler_pop_rsb_hint = code.getCurr<const void*>();
    if (conf.enable_bounded_halt_polling && !conf.enable_cycle_counting) {
        code.cmp(dword[code.ABI_JIT_PTR + offsetof(A64JitState, halt_reason)], 0);
        code.jne(code.GetForceReturnFromRunCodeAddress());
    }
    calculate_location_descriptor();
    if (host_return_stack) {
        // The top entry's location matches: pop it and RET to its caller's
        // landing, which the CPU predicts from the CALL there. Otherwise empty
        // the stack, since its entries are out of step with the guest's calls.
        Xbyak::Label mismatch;
        code.mov(rax, qword[code.ABI_JIT_PTR + offsetof(A64JitState, hrs_sp)]);
        code.cmp(r9, qword[rax + 8]);
        code.jne(mismatch, code.T_NEAR);
        code.lea(rcx, ptr[rax + 16]);
        code.mov(qword[code.ABI_JIT_PTR + offsetof(A64JitState, hrs_sp)], rcx);
        code.push(qword[rax]);
        code.ret();
        code.L(mismatch);
        code.mov(rax, qword[code.ABI_JIT_PTR + offsetof(A64JitState, hrs_base)]);
        code.mov(qword[code.ABI_JIT_PTR + offsetof(A64JitState, hrs_sp)], rax);
        code.jmp(rsb_cache_miss, code.T_NEAR);
        PerfMapRegister(terminal_handler_pop_rsb_hint, code.getCurr(), "a64_terminal_handler_pop_host_return");
    } else {
    if (census)
        count(0);
    code.mov(eax, dword[code.ABI_JIT_PTR + offsetof(A64JitState, rsb_ptr)]);
    code.sub(eax, 1);
    code.and_(eax, u32(A64JitState::RSB_PTR_MASK));
    code.mov(dword[code.ABI_JIT_PTR + offsetof(A64JitState, rsb_ptr)], eax);
    code.cmp(r9, qword[code.ABI_JIT_PTR + offsetof(A64JitState, rsb_location_descriptors) + rax * sizeof(u64)]);
    if (conf.HasOptimization(OptimizationFlag::FastDispatch)) {
        code.jne(census ? census_rsb_miss : rsb_cache_miss, code.T_NEAR);
    } else {
        code.jne(code.GetReturnFromRunCodeAddress());
    }
    code.mov(rax, qword[code.ABI_JIT_PTR + offsetof(A64JitState, rsb_codeptrs) + rax * sizeof(u64)]);
    code.jmp(rax);
    if (census && conf.HasOptimization(OptimizationFlag::FastDispatch)) {
        code.L(census_rsb_miss);
        count(1);
        code.jmp(rsb_cache_miss, code.T_NEAR);
    }
    PerfMapRegister(terminal_handler_pop_rsb_hint, code.getCurr(), "a64_terminal_handler_pop_rsb_hint");
    }

    if (conf.HasOptimization(OptimizationFlag::FastDispatch)) {
        code.align();
        terminal_handler_fast_dispatch_hint = code.getCurr<const void*>();
        if (conf.enable_bounded_halt_polling && !conf.enable_cycle_counting) {
            code.cmp(dword[code.ABI_JIT_PTR + offsetof(A64JitState, halt_reason)], 0);
            code.jne(code.GetForceReturnFromRunCodeAddress());
        }
        calculate_location_descriptor();
        if (census)
            count(2);
        code.L(rsb_cache_miss);
        terminal_handler_rsb_miss = code.getCurr<const void*>();
        code.mov(r8, u64(fast_dispatch_table.data()));
        //code.mov(rdx, qword[code.ABI_JIT_PTR + offsetof(A64JitState, pc)]);
        code.mov(rdx, r9);
        if (code.HasHostFeature(HostFeature::SSE42)) {
            code.crc32(rdx, r8);
        }
        if (census) {
            // Same hash, smaller masks: what 2^18- and 2^16-entry tables would see.
            u64* const shadow = reinterpret_cast<u64*>(census_shadow->data());
            const std::pair<u64*, unsigned> sizes[] = {{shadow, 18}, {shadow + (size_t(1) << 18), 16}};
            for (size_t i = 0; i < 2; ++i) {
                Xbyak::Label hit;
                code.mov(eax, edx);
                code.and_(eax, u32(((u64(1) << sizes[i].second) - 1) << 4));
                code.shr(eax, 1);
                code.mov(rcx, u64(sizes[i].first));
                code.cmp(r9, qword[rcx + rax]);
                code.je(hit);
                code.mov(qword[rcx + rax], r9);
                count(4 + i);
                code.L(hit);
            }
        }
        code.and_(edx, fast_dispatch_table_mask);
        code.lea(rdx, ptr[r8 + rdx]);
        code.cmp(r9, qword[rdx + offsetof(FastDispatchEntry, location_descriptor)]);
        code.jne(fast_dispatch_cache_miss, code.T_NEAR);
        code.jmp(ptr[rdx + offsetof(FastDispatchEntry, code_ptr)]);
        code.L(fast_dispatch_cache_miss);
        terminal_handler_fast_dispatch_miss = code.getCurr<const void*>();
        if (census)
            count(3);
        code.mov(qword[rdx + offsetof(FastDispatchEntry, location_descriptor)], r9);
        code.mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, dispatch_scratch)], rdx);
        code.LookupBlock();
        code.mov(rdx, qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, dispatch_scratch)]);
        code.mov(ptr[rdx + offsetof(FastDispatchEntry, code_ptr)], rax);
        code.jmp(rax);
        PerfMapRegister(terminal_handler_fast_dispatch_hint, code.getCurr(), "a64_terminal_handler_fast_dispatch_hint");

        code.align();
        fast_dispatch_table_lookup = code.getCurr<FastDispatchEntry& (*)(u64)>();
        code.mov(code.ABI_PARAM2, u64(fast_dispatch_table.data()));
        if (code.HasHostFeature(HostFeature::SSE42)) {
            code.crc32(code.ABI_PARAM1, code.ABI_PARAM2);
        }
        code.and_(code.ABI_PARAM1.cvt32(), fast_dispatch_table_mask);
        code.lea(code.ABI_RETURN, code.ptr[code.ABI_PARAM2 + code.ABI_PARAM1]);
        code.ret();
        PerfMapRegister(fast_dispatch_table_lookup, code.getCurr(), "a64_fast_dispatch_table_lookup");

        if (conf.dispatch_site_cache) {
            // A site's slot missed (RAX guest PC, R8 slot): the fast-dispatch
            // lookup, then the slot takes the PC and code. A PC with bits above
            // the location's 56 is never cached, so every slot tag is a plain
            // location of the sites' zero FPCR.
            Xbyak::Label table_miss, fill, done;
            code.align();
            terminal_handler_site_miss = code.getCurr<const void*>();
            code.mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, dispatch_site_scratch)], r8);
            calculate_location_descriptor();
            code.mov(r8, u64(fast_dispatch_table.data()));
            code.mov(rdx, r9);
            if (code.HasHostFeature(HostFeature::SSE42)) {
                code.crc32(rdx, r8);
            }
            code.and_(edx, fast_dispatch_table_mask);
            code.lea(rdx, ptr[r8 + rdx]);
            code.cmp(r9, qword[rdx + offsetof(FastDispatchEntry, location_descriptor)]);
            code.jne(table_miss, code.T_NEAR);
            code.mov(rax, qword[rdx + offsetof(FastDispatchEntry, code_ptr)]);
            code.jmp(fill, code.T_NEAR);
            code.L(table_miss);
            code.mov(qword[rdx + offsetof(FastDispatchEntry, location_descriptor)], r9);
            code.mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, dispatch_scratch)], rdx);
            code.LookupBlock();
            code.mov(rdx, qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, dispatch_scratch)]);
            code.mov(ptr[rdx + offsetof(FastDispatchEntry, code_ptr)], rax);
            code.L(fill);
            code.mov(rcx, qword[code.ABI_JIT_PTR + offsetof(A64JitState, pc)]);
            code.mov(r8, rcx);
            code.shr(r8, A64::LocationDescriptor::pc_bit_count);
            code.jnz(done, code.T_NEAR);
            code.mov(r8, qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, dispatch_site_scratch)]);
            code.mov(qword[r8 + offsetof(FastDispatchEntry, location_descriptor)], rcx);
            code.mov(qword[r8 + offsetof(FastDispatchEntry, code_ptr)], rax);
            code.L(done);
            code.jmp(rax);
            PerfMapRegister(terminal_handler_site_miss, code.getCurr(), "a64_terminal_handler_site_miss");
        }
    }

    if (conf.compact_rsb_pop && conf.HasOptimization(OptimizationFlag::ReturnStackBuffer) && !host_return_stack && !census) {
        // Compact return sites: `call rsb_pop_lookup; jmp rax`. The lookup is the
        // shared pop handler ending in RET with the target in RAX, so each site
        // keeps its own indirect jump while CALL and RET stay paired. A miss
        // returns the shared miss path with R9 set, as the pop handler leaves it.
        Xbyak::Label miss, halt;
        code.align();
        rsb_pop_lookup = code.getCurr<const void*>();
        if (conf.enable_bounded_halt_polling && !conf.enable_cycle_counting) {
            code.cmp(dword[code.ABI_JIT_PTR + offsetof(A64JitState, halt_reason)], 0);
            code.jne(halt, code.T_NEAR);
        }
        calculate_location_descriptor();
        code.mov(eax, dword[code.ABI_JIT_PTR + offsetof(A64JitState, rsb_ptr)]);
        code.sub(eax, 1);
        code.and_(eax, u32(A64JitState::RSB_PTR_MASK));
        code.mov(dword[code.ABI_JIT_PTR + offsetof(A64JitState, rsb_ptr)], eax);
        code.cmp(r9, qword[code.ABI_JIT_PTR + offsetof(A64JitState, rsb_location_descriptors) + rax * sizeof(u64)]);
        code.jne(miss, code.T_NEAR);
        code.mov(rax, qword[code.ABI_JIT_PTR + offsetof(A64JitState, rsb_codeptrs) + rax * sizeof(u64)]);
        code.ret();
        code.L(miss);
        code.mov(rax, u64(conf.HasOptimization(OptimizationFlag::FastDispatch) ? terminal_handler_rsb_miss : code.GetReturnFromRunCodeAddress()));
        code.ret();
        code.L(halt);
        code.mov(rax, u64(code.GetForceReturnFromRunCodeAddress()));
        code.ret();
        PerfMapRegister(rsb_pop_lookup, code.getCurr(), "a64_rsb_pop_lookup");
    }
}

void A64EmitX64::EmitStaticLocationDescriptor(IR::LocationDescriptor initial_location) {
    // The shared handlers' halt poll and location calculation (R9), with this
    // block's FPCR as a constant.
    if (conf.enable_bounded_halt_polling && !conf.enable_cycle_counting) {
        code.cmp(dword[code.ABI_JIT_PTR + offsetof(A64JitState, halt_reason)], 0);
        code.jne(code.GetForceReturnFromRunCodeAddress());
    }
    const u64 fpcr = u64(A64::LocationDescriptor{initial_location}.FPCR().Value() & A64::LocationDescriptor::fpcr_mask)
                     << A64::LocationDescriptor::fpcr_shift;
    code.mov(r9, qword[code.ABI_JIT_PTR + offsetof(A64JitState, pc)]);
    code.shl(r9, 64 - A64::LocationDescriptor::pc_bit_count);
    code.shr(r9, 64 - A64::LocationDescriptor::pc_bit_count);
    if (fpcr) {
        code.mov(rcx, fpcr);
        code.or_(r9, rcx);
    }
}

bool A64EmitX64::UsesCompactPopRSB() const {
    return rsb_pop_lookup != nullptr;
}

void A64EmitX64::EmitInlinePopRSB(IR::LocationDescriptor initial_location) {
    // The shared pop handler at this return site, so each site has its own host
    // indirect jump. A mismatch joins the fast-dispatch lookup with R9 set.
    EmitStaticLocationDescriptor(initial_location);
    code.mov(eax, dword[code.ABI_JIT_PTR + offsetof(A64JitState, rsb_ptr)]);
    code.sub(eax, 1);
    code.and_(eax, u32(A64JitState::RSB_PTR_MASK));
    code.mov(dword[code.ABI_JIT_PTR + offsetof(A64JitState, rsb_ptr)], eax);
    code.cmp(r9, qword[code.ABI_JIT_PTR + offsetof(A64JitState, rsb_location_descriptors) + rax * sizeof(u64)]);
    if (conf.HasOptimization(OptimizationFlag::FastDispatch)) {
        code.jne(terminal_handler_rsb_miss);
    } else {
        code.jne(code.GetReturnFromRunCodeAddress());
    }
    code.jmp(qword[code.ABI_JIT_PTR + offsetof(A64JitState, rsb_codeptrs) + rax * sizeof(u64)]);
}

void A64EmitX64::EmitKnownReturn(IR::LocationDescriptor initial_location, bool is_single_step) {
    // The shared pop handler's location calculation (R9). FPCR is loaded, not
    // assumed: the supervisor call may have changed it, or PC.
    code.mov(rdx, qword[code.ABI_JIT_PTR + offsetof(A64JitState, pc)]);
    code.mov(rcx, A64::LocationDescriptor::pc_mask);
    code.and_(rcx, rdx);
    code.mov(r9d, dword[code.ABI_JIT_PTR + offsetof(A64JitState, fpcr)]);
    code.and_(r9d, A64::LocationDescriptor::fpcr_mask);
    code.shl(r9, A64::LocationDescriptor::fpcr_shift);
    code.or_(r9, rcx);
    code.mov(rcx, known_return->Value());
    code.cmp(r9, rcx);
    if (conf.HasOptimization(OptimizationFlag::FastDispatch)) {
        code.jne(terminal_handler_rsb_miss);
    } else {
        code.jne(code.GetReturnFromRunCodeAddress());
    }
    EmitLeafTerminal(IR::Term::LinkBlockFast{*known_return}, initial_location, is_single_step);
}

void A64EmitX64::EmitHostCall(IR::LocationDescriptor return_location, IR::LocationDescriptor initial_location,
                              bool is_single_step, const IR::Term::Terminal& terminal) {
    // A CALL gives the CPU's return predictor this site's landing; the callee
    // POPs it back off the frame (the predictor keeps it) and records
    // {landing, return location} on the host return stack before leaving
    // through the block's own exit. A full stack restarts from its sentinel;
    // the dropped entries only stop predicting.
    Xbyak::Label room, enter;
    code.call(enter);
    // Landing: the pop handler's RET arrives here with the entry's location
    // matched and popped, and PC set.
    EmitLeafTerminal(IR::Term::LinkBlockFast{return_location}, initial_location, is_single_step);
    code.L(enter);
    code.pop(rcx);
    code.mov(rax, qword[code.ABI_JIT_PTR + offsetof(A64JitState, hrs_sp)]);
    code.sub(rax, 16);
    code.cmp(rax, qword[code.ABI_JIT_PTR + offsetof(A64JitState, hrs_limit)]);
    code.jae(room);
    code.mov(rax, qword[code.ABI_JIT_PTR + offsetof(A64JitState, hrs_base)]);
    code.sub(rax, 16);
    code.L(room);
    code.mov(qword[rax], rcx);
    code.mov(rcx, return_location.Value());
    code.mov(qword[rax + 8], rcx);
    code.mov(qword[code.ABI_JIT_PTR + offsetof(A64JitState, hrs_sp)], rax);
    EmitTerminal(terminal, initial_location, is_single_step);
}

void A64EmitX64::EmitInlineFastDispatch(IR::LocationDescriptor initial_location) {
    // The shared handler's lookup with this block's FPCR as a constant; misses
    // join the handler with the same registers (R9 location, RDX entry).
    EmitStaticLocationDescriptor(initial_location);
    code.mov(r8, u64(fast_dispatch_table.data()));
    code.mov(rdx, r9);
    if (code.HasHostFeature(HostFeature::SSE42)) {
        code.crc32(rdx, r8);
    }
    code.and_(edx, fast_dispatch_table_mask);
    code.lea(rdx, ptr[r8 + rdx]);
    code.cmp(r9, qword[rdx + offsetof(FastDispatchEntry, location_descriptor)]);
    code.jne(terminal_handler_fast_dispatch_miss);
    code.jmp(ptr[rdx + offsetof(FastDispatchEntry, code_ptr)]);
}

void A64EmitX64::EmitSiteDispatch() {
    // The shared handlers' halt poll, then this site's slot: a matching PC jumps
    // to its cached code, a mismatch refills the slot (RAX PC, R8 slot).
    if (conf.enable_bounded_halt_polling && !conf.enable_cycle_counting) {
        code.cmp(dword[code.ABI_JIT_PTR + offsetof(A64JitState, halt_reason)], 0);
        code.jne(code.GetForceReturnFromRunCodeAddress());
    }
    FastDispatchEntry* const slot = dispatch_sites->data() + (next_dispatch_site++ & (dispatch_site_count - 1));
    code.mov(rax, qword[code.ABI_JIT_PTR + offsetof(A64JitState, pc)]);
    code.mov(r8, u64(slot));
    code.cmp(rax, qword[r8 + offsetof(FastDispatchEntry, location_descriptor)]);
    code.jne(terminal_handler_site_miss);
    code.jmp(qword[r8 + offsetof(FastDispatchEntry, code_ptr)]);
}

void A64EmitX64::EmitCallEdge(A64EmitContext& ctx, u64 target, u32 kind, bool dynamic) {
    if (!conf.profile_call_edge.record) return;
    // Full normal ABI/cache/FP synchronization. Diagnostics cannot change the
    // guest's rounding mode or clobber cached state, including BLR X30.
    // Target is already committed; release its temporary register locks.
    ctx.reg_alloc.EndOfAllocScope();
    code.SwitchMxcsrOnExit();
    ctx.reg_alloc.HostCall(code, nullptr);
    code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(conf.profile_call_edge.user));
    code.mov(code.ABI_PARAM2, A64::LocationDescriptor{ctx.block.EndLocation()}.PC() - 4);
    if (dynamic) code.mov(code.ABI_PARAM3, code.qword[code.ABI_JIT_PTR + offsetof(A64JitState, pc)]);
    else code.mov(code.ABI_PARAM3, target);
    code.mov(code.ABI_PARAM4.cvt32(), kind);
    code.CallFunction(conf.profile_call_edge.record);
    code.SwitchMxcsrOnEntry();
    ctx.reg_alloc.EndOfAllocScope();
}

void A64EmitX64::EmitPushRSB(EmitContext& ctx, IR::Inst* inst) {
    if (conf.profile_direct_call_counter) {
        const auto terminal = ctx.block.GetTerminal();
        const auto direct = std::get_if<IR::Term::LeafTerminal>(&terminal);
        const auto halt = std::get_if<IR::Term::CheckHalt>(&terminal);
        const auto leaf = direct ? direct : halt ? &halt->else_ : nullptr;
        std::optional<u64> target;
        if (const auto link = leaf ? std::get_if<IR::Term::LinkBlockFast>(leaf) : nullptr)
            target = A64::LocationDescriptor{link->next}.PC();
        else if (const auto link = leaf ? std::get_if<IR::Term::LinkBlock>(leaf) : nullptr)
            target = A64::LocationDescriptor{link->next}.PC();
        if (target) {
            const auto from = A64::LocationDescriptor{ctx.block.EndLocation()}.PC() - 4;
            if (const auto counter = conf.profile_direct_call_counter(from, *target)) {
                code.pushfq();
                code.push(rax);
                code.mov(rax, reinterpret_cast<std::uintptr_t>(counter));
                code.inc(code.qword[rax]);
                code.pop(rax);
                code.popfq();
            }
        }
    }
    if (conf.profile_call_edge.record) {
        const auto terminal = ctx.block.GetTerminal();
        const auto direct = std::get_if<IR::Term::LeafTerminal>(&terminal);
        const auto halt = std::get_if<IR::Term::CheckHalt>(&terminal);
        const auto leaf = direct ? direct : halt ? &halt->else_ : nullptr;
        if (const auto link = leaf ? std::get_if<IR::Term::LinkBlockFast>(leaf) : nullptr)
            EmitCallEdge(static_cast<A64EmitContext&>(ctx), A64::LocationDescriptor{link->next}.PC(), 1);
        else if (const auto link = leaf ? std::get_if<IR::Term::LinkBlock>(leaf) : nullptr)
            EmitCallEdge(static_cast<A64EmitContext&>(ctx), A64::LocationDescriptor{link->next}.PC(), 1);
    }
    // A known return is popped by this block's own exit: no entry to push.
    // With the host return stack the block's exit pushes it (EmitHostCall).
    if (known_return)
        return;
    if (host_return_stack) {
        if (!static_cast<A64EmitContext&>(ctx).IsSingleStep())
            host_call_return = IR::LocationDescriptor{inst->GetArg(0).GetU64()};
        return;
    }
    if (conf.HasOptimization(OptimizationFlag::ReturnStackBuffer))
        EmitX64::EmitPushRSB(ctx, inst);
}

void A64EmitX64::EmitA64SetCheckBit(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg8 to_store = ctx.reg_alloc.UseGpr(code, args[0]).cvt8();
    code.mov(code.byte[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, check_bit)], to_store);
}

void A64EmitX64::EmitA64GetCFlag(A64EmitContext& ctx, IR::Inst* inst) {
    const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr(code).cvt32();
    code.mov(result, dword[code.ABI_JIT_PTR + offsetof(A64JitState, cpsr_nzcv)]);
    code.shr(result, NZCV::x64_c_flag_bit);
    code.and_(result, 1);
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void A64EmitX64::EmitA64GetNZCVRaw(A64EmitContext& ctx, IR::Inst* inst) {
    const Xbyak::Reg32 nzcv_raw = ctx.reg_alloc.ScratchGpr(code).cvt32();

    code.mov(nzcv_raw, dword[code.ABI_JIT_PTR + offsetof(A64JitState, cpsr_nzcv)]);

    if (code.HasHostFeature(HostFeature::FastBMI2)) {
        const Xbyak::Reg32 tmp = ctx.reg_alloc.ScratchGpr(code).cvt32();
        code.mov(tmp, NZCV::x64_mask);
        code.pext(nzcv_raw, nzcv_raw, tmp);
        code.shl(nzcv_raw, 28);
    } else {
        code.and_(nzcv_raw, NZCV::x64_mask);
        code.imul(nzcv_raw, nzcv_raw, NZCV::from_x64_multiplier);
        code.and_(nzcv_raw, NZCV::arm_mask);
    }

    ctx.reg_alloc.DefineValue(code, inst, nzcv_raw);
}

void A64EmitX64::EmitA64SetNZCVRaw(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg32 nzcv_raw = ctx.reg_alloc.UseScratchGpr(code, args[0]).cvt32();

    code.shr(nzcv_raw, 28);
    if (code.HasHostFeature(HostFeature::FastBMI2)) {
        const Xbyak::Reg32 tmp = ctx.reg_alloc.ScratchGpr(code).cvt32();
        code.mov(tmp, NZCV::x64_mask);
        code.pdep(nzcv_raw, nzcv_raw, tmp);
    } else {
        code.imul(nzcv_raw, nzcv_raw, NZCV::to_x64_multiplier);
        code.and_(nzcv_raw, NZCV::x64_mask);
    }
    code.mov(dword[code.ABI_JIT_PTR + offsetof(A64JitState, cpsr_nzcv)], nzcv_raw);
}

void A64EmitX64::EmitA64SetNZCV(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    // A plain store: no scratch copy of a value that selects or the terminal still use.
    const Xbyak::Reg32 to_store = ctx.reg_alloc.UseGpr(code, args[0]).cvt32();
    code.mov(dword[code.ABI_JIT_PTR + offsetof(A64JitState, cpsr_nzcv)], to_store);
}

void A64EmitX64::EmitA64GetW(A64EmitContext& ctx, IR::Inst* inst) {
    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();
    const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr(code).cvt32();

    if (const auto cached = code.A64CachedRegister(static_cast<size_t>(reg)))
        code.mov(result, cached->cvt32());
    else
        code.mov(result, dword[code.ABI_JIT_PTR + offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg)]);
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void A64EmitX64::EmitA64GetX(A64EmitContext& ctx, IR::Inst* inst) {
    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();
    if (const auto cached = code.A64CachedRegister(static_cast<size_t>(reg))) {
        // Venture: the value aliases its cache register, read in place; writes
        // to the register and host calls move live aliases out first.
        ctx.reg_alloc.DefineValue(code, inst, *cached);
        return;
    }
    const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code);
    code.mov(result, qword[code.ABI_JIT_PTR + offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg)]);
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void A64EmitX64::EmitA64GetS(A64EmitContext& ctx, IR::Inst* inst) {
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    const auto addr = qword[code.ABI_JIT_PTR + offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec)];

    const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm(code);
    code.movd(result, addr);
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void A64EmitX64::EmitA64GetD(A64EmitContext& ctx, IR::Inst* inst) {
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    const auto addr = qword[code.ABI_JIT_PTR + offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec)];

    const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm(code);
    code.movq(result, addr);
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void A64EmitX64::EmitA64GetQ(A64EmitContext& ctx, IR::Inst* inst) {
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    const auto addr = xword[code.ABI_JIT_PTR + offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec)];

    const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm(code);
    code.movaps(result, addr);
    ctx.reg_alloc.DefineValue(code, inst, result);
}

// The register cache addresses SP as GPR index 31.
static_assert(offsetof(A64JitState, sp) == offsetof(A64JitState, reg) + sizeof(u64) * 31);

void A64EmitX64::EmitA64GetSP(A64EmitContext& ctx, IR::Inst* inst) {
    if (const auto cached = code.A64CachedRegister(31)) {
        ctx.reg_alloc.DefineValue(code, inst, *cached);  // as GetX
        return;
    }
    const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code);
    code.mov(result, qword[code.ABI_JIT_PTR + offsetof(A64JitState, sp)]);
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void A64EmitX64::EmitA64GetFPCR(A64EmitContext& ctx, IR::Inst* inst) {
    const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr(code).cvt32();
    code.mov(result, dword[code.ABI_JIT_PTR + offsetof(A64JitState, fpcr)]);
    ctx.reg_alloc.DefineValue(code, inst, result);
}

static u32 GetFPSRImpl(A64JitState* jit_state) {
    return jit_state->GetFpsr();
}

void A64EmitX64::EmitA64GetFPSR(A64EmitContext& ctx, IR::Inst* inst) {
    ctx.reg_alloc.HostCall(code, inst);
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.stmxcsr(code.dword[code.ABI_JIT_PTR + offsetof(A64JitState, guest_MXCSR)]);
    code.CallFunction(GetFPSRImpl);
}

void A64EmitX64::EmitA64SetW(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();
    if (const auto cached = code.A64CachedRegister(static_cast<size_t>(reg))) {
        // A word producer's value is only ever here when computed in place (result hint).
        if (ctx.reg_alloc.IsInRegister(args[1], *cached) && IsResultHintable(inst->GetArg(1).GetInst()->GetOpcode(), true))
            return;
        ctx.reg_alloc.MovePinnedValuesOut(code, *cached);
        if (args[1].IsImmediate()) {
            code.mov(cached->cvt32(), args[1].GetImmediateU32());
        } else if (args[1].IsInXmm(ctx.reg_alloc)) {
            const auto value = ctx.reg_alloc.UseXmm(code, args[1]);
            code.movd(cached->cvt32(), value);
        } else {
            const auto value = ctx.reg_alloc.UseGpr(code, args[1]);
            code.mov(cached->cvt32(), value.cvt32());
        }
        return;
    }
    const auto addr = qword[code.ABI_JIT_PTR + offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg)];
    if (args[1].FitsInImmediateS32()) {
        code.mov(addr, args[1].GetImmediateS32());
    } else if (args[1].IsInXmm(ctx.reg_alloc)) {
        // Venture: MOVD zero-extends into the register that is stored.
        const Xbyak::Xmm value = ctx.reg_alloc.UseXmm(code, args[1]);
        const Xbyak::Reg64 to_store = ctx.reg_alloc.ScratchGpr(code);
        code.movd(to_store.cvt32(), value);
        code.mov(addr, to_store);
    } else if (!args[1].IsImmediate() && WritesZeroExtendedWord(inst->GetArg(1).GetInst())) {
        // Venture: the producer wrote a 32-bit register; bits 32-63 are clear.
        code.mov(addr, ctx.reg_alloc.UseGpr(code, args[1]));
    } else {
        // The zero-extending MOV into a fresh register is also the copy, so a
        // value with later uses costs one MOV rather than a copy plus a MOV.
        const Xbyak::Reg32 value = ctx.reg_alloc.UseGpr(code, args[1]).cvt32();
        const Xbyak::Reg64 to_store = ctx.reg_alloc.ScratchGpr(code);
        code.mov(to_store.cvt32(), value);
        code.mov(addr, to_store);
    }
}

void A64EmitX64::EmitA64SetX(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();
    if (const auto cached = code.A64CachedRegister(static_cast<size_t>(reg))) {
        if (ctx.reg_alloc.IsInRegister(args[1], *cached))
            return;  // computed in place (result hint), or a no-op move
        ctx.reg_alloc.MovePinnedValuesOut(code, *cached);
        if (args[1].IsImmediate()) {
            code.mov(*cached, args[1].GetImmediateU64());
        } else if (args[1].IsInXmm(ctx.reg_alloc)) {
            const auto value = ctx.reg_alloc.UseXmm(code, args[1]);
            code.movq(*cached, value);
        } else {
            const auto value = ctx.reg_alloc.UseGpr(code, args[1]);
            code.mov(*cached, value);
        }
        return;
    }
    const auto addr = qword[code.ABI_JIT_PTR + offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg)];
    if (args[1].FitsInImmediateS32()) {
        code.mov(addr, args[1].GetImmediateS32());
    } else if (args[1].IsInXmm(ctx.reg_alloc)) {
        const Xbyak::Xmm to_store = ctx.reg_alloc.UseXmm(code, args[1]);
        code.movq(addr, to_store);
    } else {
        const Xbyak::Reg64 to_store = ctx.reg_alloc.UseGpr(code, args[1]);
        code.mov(addr, to_store);
    }
}

void A64EmitX64::EmitA64SetS(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    const auto addr = xword[code.ABI_JIT_PTR + offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec)];

    if (conf.HasOptimization(OptimizationFlag::LowLaneAliasing)) {
        // ZeroExtendLongToQuad(ZeroExtendWordToLong(x)) already has zero lanes 1-3.
        const IR::Value value = inst->GetArg(1);
        const IR::Inst* const quad = value.IsImmediate() ? nullptr : value.GetInstRecursive();
        if (quad && quad->GetOpcode() == IR::Opcode::ZeroExtendLongToQuad && !quad->GetArg(0).IsImmediate() &&
            quad->GetArg(0).GetInstRecursive()->GetOpcode() == IR::Opcode::ZeroExtendWordToLong) {
            code.movaps(addr, ctx.reg_alloc.UseXmm(code, args[1]));
            return;
        }
    }
    const Xbyak::Xmm to_store = ctx.reg_alloc.UseXmm(code, args[1]);
    const Xbyak::Xmm tmp = ctx.reg_alloc.ScratchXmm(code);
    // TODO: Optimize
    code.pxor(tmp, tmp);
    code.movss(tmp, to_store);
    code.movaps(addr, tmp);
}

void A64EmitX64::EmitA64SetD(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    const auto addr = xword[code.ABI_JIT_PTR + offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec)];

    if (IsUpperZeroVector(code, ctx, inst->GetArg(1))) {
        code.movaps(addr, ctx.reg_alloc.UseXmm(code, args[1]));
        return;
    }
    const Xbyak::Xmm to_store = ctx.reg_alloc.UseScratchXmm(code, args[1]);
    code.movq(to_store, to_store);  // TODO: Remove when able
    code.movaps(addr, to_store);
}

void A64EmitX64::EmitA64SetQ(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    const auto addr = xword[code.ABI_JIT_PTR + offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec)];

    const Xbyak::Xmm to_store = ctx.reg_alloc.UseXmm(code, args[1]);
    code.movaps(addr, to_store);
}

void A64EmitX64::EmitA64SetSP(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    if (const auto cached = code.A64CachedRegister(31)) {
        if (ctx.reg_alloc.IsInRegister(args[0], *cached))
            return;  // as SetX
        ctx.reg_alloc.MovePinnedValuesOut(code, *cached);
        if (args[0].IsImmediate()) {
            code.mov(*cached, args[0].GetImmediateU64());
        } else if (args[0].IsInXmm(ctx.reg_alloc)) {
            const auto value = ctx.reg_alloc.UseXmm(code, args[0]);
            code.movq(*cached, value);
        } else {
            const auto value = ctx.reg_alloc.UseGpr(code, args[0]);
            code.mov(*cached, value);
        }
        return;
    }
    const auto addr = qword[code.ABI_JIT_PTR + offsetof(A64JitState, sp)];
    if (args[0].FitsInImmediateS32()) {
        code.mov(addr, args[0].GetImmediateS32());
    } else if (args[0].IsInXmm(ctx.reg_alloc)) {
        const Xbyak::Xmm to_store = ctx.reg_alloc.UseXmm(code, args[0]);
        code.movq(addr, to_store);
    } else {
        const Xbyak::Reg64 to_store = ctx.reg_alloc.UseGpr(code, args[0]);
        code.mov(addr, to_store);
    }
}

static void SetFPCRImpl(A64JitState* jit_state, u32 value) {
    jit_state->SetFpcr(value);
}

void A64EmitX64::EmitA64SetFPCR(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, nullptr, {}, args[0]);
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallFunction(SetFPCRImpl);
    code.ldmxcsr(code.dword[code.ABI_JIT_PTR + offsetof(A64JitState, guest_MXCSR)]);
}

static void SetFPSRImpl(A64JitState* jit_state, u32 value) {
    jit_state->SetFpsr(value);
}

void A64EmitX64::EmitA64SetFPSR(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, nullptr, {}, args[0]);
    code.mov(code.ABI_PARAM1, code.ABI_JIT_PTR);
    code.CallFunction(SetFPSRImpl);
    code.ldmxcsr(code.dword[code.ABI_JIT_PTR + offsetof(A64JitState, guest_MXCSR)]);
}

void A64EmitX64::EmitA64SetPC(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const auto addr = qword[code.ABI_JIT_PTR + offsetof(A64JitState, pc)];
    if (args[0].FitsInImmediateS32()) {
        code.mov(addr, args[0].GetImmediateS32());
    } else if (args[0].IsInXmm(ctx.reg_alloc)) {
        const Xbyak::Xmm to_store = ctx.reg_alloc.UseXmm(code, args[0]);
        code.movq(addr, to_store);
    } else {
        const Xbyak::Reg64 to_store = ctx.reg_alloc.UseGpr(code, args[0]);
        code.mov(addr, to_store);
    }
    const auto terminal = ctx.block.GetTerminal();
    const auto leaf = std::get_if<IR::Term::LeafTerminal>(&terminal);
    const auto halt = std::get_if<IR::Term::CheckHalt>(&terminal);
    const auto target_terminal = leaf ? leaf : halt ? &halt->else_ : nullptr;
    if (conf.profile_call_edge.record && target_terminal && std::holds_alternative<IR::Term::FastDispatchHint>(*target_terminal)) {
        bool call = false;
        for (const auto& instruction : ctx.block.instructions)
            call |= instruction.GetOpcode() == IR::Opcode::PushRSB;
        EmitCallEdge(ctx, 0, call ? 2 : 3, true);
    }
    if (conf.profile_call_edge.record && conf.profile_call_edge.record_returns && target_terminal &&
        std::holds_alternative<IR::Term::PopRSBHint>(*target_terminal))
        EmitCallEdge(ctx, 0, 4, true);
}

void A64EmitX64::EmitA64CallSupervisor(A64EmitContext& ctx, IR::Inst* inst) {
    ctx.reg_alloc.HostCall(code, nullptr);
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[0].IsImmediate());
    const u32 imm = args[0].GetImmediateU32();
    const bool transfer_fp = conf.svc_fp_transfer && imm == *conf.svc_fp_transfer;
    const bool guest_fp = conf.svc_fp_guest && imm == *conf.svc_fp_guest;
    const bool fast_svc = transfer_fp || guest_fp || (conf.svc_fast && imm == *conf.svc_fast);
    if (transfer_fp) code.SwitchMxcsrOnExit();
    const auto known = fast_svc && conf.svc_lookup ? conf.svc_lookup(ctx.Location().PC(), imm) : nullptr;
    if (known) {
        Devirtualize<&A64::UserCallbacks::CallSVCFastKnown>(conf.callbacks).EmitCall(code, [&](RegList param) {
            code.mov(param[0], imm);
            code.mov(param[1], reinterpret_cast<u64>(known));
            code.mov(param[2], code.ABI_JIT_PTR);
        });
    } else if (fast_svc) {
        Devirtualize<&A64::UserCallbacks::CallSVCFast>(conf.callbacks).EmitCall(code, [&](RegList param) {
            code.mov(param[0], imm);
        });
    } else {
        Devirtualize<&A64::UserCallbacks::CallSVC>(conf.callbacks).EmitCall(code, [&](RegList param) {
            code.mov(param[0], imm);
        });
    }
    if (transfer_fp) code.SwitchMxcsrOnEntry();
    // The kernel would have to execute ERET to get here, which would clear exclusive state.
    code.mov(code.byte[code.ABI_JIT_PTR + offsetof(A64JitState, exclusive_state)], u8(0));
}

void A64EmitX64::EmitA64ExceptionRaised(A64EmitContext& ctx, IR::Inst* inst) {
    ctx.reg_alloc.HostCall(code, nullptr);
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[0].IsImmediate() && args[1].IsImmediate());
    const u64 pc = args[0].GetImmediateU64();
    const u64 exception = args[1].GetImmediateU64();
    Devirtualize<&A64::UserCallbacks::ExceptionRaised>(conf.callbacks).EmitCall(code, [&](RegList param) {
        code.mov(param[0], pc);
        code.mov(param[1], exception);
    });
}

void A64EmitX64::EmitA64DataCacheOperationRaised(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, nullptr, {}, args[1], args[2]);
    Devirtualize<&A64::UserCallbacks::DataCacheOperationRaised>(conf.callbacks).EmitCall(code);
}

void A64EmitX64::EmitA64InstructionCacheOperationRaised(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(code, nullptr, {}, args[0], args[1]);
    Devirtualize<&A64::UserCallbacks::InstructionCacheOperationRaised>(conf.callbacks).EmitCall(code);
}

void A64EmitX64::EmitA64DataSynchronizationBarrier(A64EmitContext&, IR::Inst*) {
    code.mfence();
    code.lfence();
}

void A64EmitX64::EmitA64DataMemoryBarrier(A64EmitContext&, IR::Inst* inst) {
    // x86-TSO already orders load-load, load-store and store-store, so DMB LD
    // and DMB ST (option types 0b01 and 0b10) need no instruction. Ordering
    // stores before later loads takes a locked operation on the private JIT
    // stack: a full barrier for write-back memory, cheaper than MFENCE.
    if ((inst->GetArg(0).GetU8() & 0b11) != 0b11) {
        return;
    }
    code.lock();
    code.or_(code.dword[code.rsp], 0);
}

void A64EmitX64::EmitA64InstructionSynchronizationBarrier(A64EmitContext& ctx, IR::Inst*) {
    if (!conf.hook_isb) {
        return;
    }

    ctx.reg_alloc.HostCall(code, nullptr);
    Devirtualize<&A64::UserCallbacks::InstructionSynchronizationBarrierRaised>(conf.callbacks).EmitCall(code);
}

void A64EmitX64::EmitA64GetCNTFRQ(A64EmitContext& ctx, IR::Inst* inst) {
    const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr(code).cvt32();
    code.mov(result, conf.cntfrq_el0);
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void A64EmitX64::EmitA64GetCNTPCT(A64EmitContext& ctx, IR::Inst* inst) {
    ctx.reg_alloc.HostCall(code, inst);
    if (!conf.wall_clock_cntpct) {
        code.UpdateTicks();
    }
    Devirtualize<&A64::UserCallbacks::GetCNTPCT>(conf.callbacks).EmitCall(code);
}

void A64EmitX64::EmitA64GetCTR(A64EmitContext& ctx, IR::Inst* inst) {
    const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr(code).cvt32();
    code.mov(result, conf.ctr_el0);
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void A64EmitX64::EmitA64GetDCZID(A64EmitContext& ctx, IR::Inst* inst) {
    const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr(code).cvt32();
    code.mov(result, conf.dczid_el0);
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void A64EmitX64::EmitA64GetTPIDR(A64EmitContext& ctx, IR::Inst* inst) {
    const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code);
    if (conf.tpidr_el0) {
        code.mov(result, u64(conf.tpidr_el0));
        code.mov(result, qword[result]);
    } else {
        code.xor_(result.cvt32(), result.cvt32());
    }
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void A64EmitX64::EmitA64GetTPIDRRO(A64EmitContext& ctx, IR::Inst* inst) {
    const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr(code);
    if (conf.tpidrro_el0) {
        code.mov(result, u64(conf.tpidrro_el0));
        code.mov(result, qword[result]);
    } else {
        code.xor_(result.cvt32(), result.cvt32());
    }
    ctx.reg_alloc.DefineValue(code, inst, result);
}

void A64EmitX64::EmitA64SetTPIDR(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg64 value = ctx.reg_alloc.UseGpr(code, args[0]);
    const Xbyak::Reg64 addr = ctx.reg_alloc.ScratchGpr(code);
    if (conf.tpidr_el0) {
        code.mov(addr, u64(conf.tpidr_el0));
        code.mov(qword[addr], value);
    }
}

std::string A64EmitX64::LocationDescriptorToFriendlyName(const IR::LocationDescriptor& ir_descriptor) const {
    const A64::LocationDescriptor descriptor{ir_descriptor};
    return fmt::format("a64_{:016X}_fpcr{:08X}",
                       descriptor.PC(),
                       descriptor.FPCR().Value());
}

namespace {
bool EmitTerminalImpl(A64EmitX64& e, IR::Term::ReturnToDispatch, IR::LocationDescriptor, bool) {
    e.code.ReturnFromRunCode();
    return true;
}

bool EmitTerminalImpl(A64EmitX64& e, IR::Term::LinkBlockFast terminal, IR::LocationDescriptor initial_location, bool is_single_step);

bool EmitTerminalImpl(A64EmitX64& e, IR::Term::LinkBlock terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    const u64 from = A64::LocationDescriptor{initial_location}.PC();
    const u64 to = A64::LocationDescriptor{terminal.next}.PC();
    if (e.elide_forward_halt_polls && to > from &&
        to / A64::halt_polling_window_size == from / A64::halt_polling_window_size)
        return EmitTerminalImpl(e, IR::Term::LinkBlockFast{terminal.next}, initial_location, is_single_step);
    // Used for patches and linking
    if (e.conf.HasOptimization(OptimizationFlag::BlockLinking) && !is_single_step) {
        const bool compact = e.CompactBlockLinks();
        const CodePtr unlinked_exit = compact ? e.UnlinkedExit(terminal.next) : nullptr;
        if (e.conf.enable_cycle_counting) {
            e.code.cmp(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], 0);
            e.MarkProfileLink(initial_location, terminal.next);
            e.patch_information[terminal.next].jg.push_back(e.code.getCurr());
            if (const auto next_bb = e.GetBasicBlock(terminal.next)) {
                e.EmitPatchJg(terminal.next, next_bb->entrypoint);
            } else {
                e.EmitPatchJg(terminal.next);
            }
        } else {
            e.code.cmp(dword[e.code.ABI_JIT_PTR + offsetof(A64JitState, halt_reason)], 0);
            e.MarkProfileLink(initial_location, terminal.next);
            e.patch_information[terminal.next].jz.push_back(e.code.getCurr());
            if (const auto next_bb = e.GetBasicBlock(terminal.next)) {
                e.EmitPatchJz(terminal.next, next_bb->entrypoint);
            } else {
                e.EmitPatchJz(terminal.next);
            }
        }
        if (compact) {
            // Halted or out of cycles: the dispatcher entry re-checks both.
            e.code.jmp(unlinked_exit, Xbyak::CodeGenerator::T_NEAR);
            return true;
        }
        e.code.mov(rax, A64::LocationDescriptor{terminal.next}.PC());
        e.code.mov(qword[e.code.ABI_JIT_PTR + offsetof(A64JitState, pc)], rax);
        e.code.ForceReturnFromRunCode();
    } else {
        e.code.mov(rax, A64::LocationDescriptor{terminal.next}.PC());
        e.code.mov(qword[e.code.ABI_JIT_PTR + offsetof(A64JitState, pc)], rax);
        e.code.ReturnFromRunCode();
    }
    return true;
}

bool EmitTerminalImpl(A64EmitX64& e, IR::Term::LinkBlockFast terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    // Every cycle of linked blocks has a backward edge. Preserve fast forward
    // links while making backward and self links observe halt/invalidation.
    if (A64::LocationDescriptor{terminal.next}.PC() <= A64::LocationDescriptor{initial_location}.PC() ||
        (e.conf.enable_bounded_halt_polling && !e.conf.enable_cycle_counting &&
         A64::LocationDescriptor{terminal.next}.PC() / A64::halt_polling_window_size !=
         A64::LocationDescriptor{initial_location}.PC() / A64::halt_polling_window_size))
        return EmitTerminalImpl(e, IR::Term::LinkBlock{terminal.next}, initial_location, is_single_step);
    if (e.conf.HasOptimization(OptimizationFlag::BlockLinking) && !is_single_step) {
        if (e.CompactBlockLinks())
            e.UnlinkedExit(terminal.next);
        e.MarkProfileLink(initial_location, terminal.next);
        e.patch_information[terminal.next].jmp.push_back(e.code.getCurr());
        if (auto next_bb = e.GetBasicBlock(terminal.next)) {
            e.EmitPatchJmp(terminal.next, next_bb->entrypoint);
        } else {
            e.EmitPatchJmp(terminal.next);
        }
    } else {
        e.code.mov(rax, A64::LocationDescriptor{terminal.next}.PC());
        e.code.mov(qword[e.code.ABI_JIT_PTR + offsetof(A64JitState, pc)], rax);
        e.code.ReturnFromRunCode();
    }
    return true;
}

bool EmitTerminalImpl(A64EmitX64& e, IR::Term::PopRSBHint, IR::LocationDescriptor initial_location, bool is_single_step) {
    if (e.known_return && !is_single_step) {
        e.EmitKnownReturn(initial_location, is_single_step);
    } else if (e.UsesCompactPopRSB() && !is_single_step) {
        e.code.call(e.rsb_pop_lookup);
        e.code.jmp(rax);
    } else if (e.conf.HasOptimization(OptimizationFlag::ReturnStackBuffer) && !is_single_step && e.static_fpcr_terminal && e.conf.inline_rsb_pop && !e.host_return_stack) {
        e.EmitInlinePopRSB(initial_location);
    } else if (e.conf.HasOptimization(OptimizationFlag::ReturnStackBuffer) && !is_single_step) {
        e.code.jmp(e.terminal_handler_pop_rsb_hint);
    } else {
        e.code.ReturnFromRunCode();
    }
    return true;
}

bool EmitTerminalImpl(A64EmitX64& e, IR::Term::FastDispatchHint, IR::LocationDescriptor initial_location, bool is_single_step) {
    if (!e.conf.HasOptimization(OptimizationFlag::FastDispatch) || is_single_step) {
        e.code.ReturnFromRunCode();
    } else if (e.static_fpcr_terminal && e.dispatch_sites && A64::LocationDescriptor{initial_location}.FPCR().Value() == 0) {
        e.EmitSiteDispatch();
    } else if (e.static_fpcr_terminal && e.conf.inline_fast_dispatch) {
        e.EmitInlineFastDispatch(initial_location);
    } else {
        e.code.jmp(e.terminal_handler_fast_dispatch_hint);
    }
    return true;
}

bool EmitTerminalImpl(A64EmitX64& e, IR::Term::If terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    switch (terminal.if_) {
    case IR::Cond::AL:
    case IR::Cond::NV:
        e.EmitLeafTerminal(terminal.then_, initial_location, is_single_step);
        break;
    default:
        if (e.terminal_host_flags) {
            // Raw FLAGS of the final Add/Sub; NZCV is stored only where a successor may read it.
            e.terminal_host_flags = false;
            Xbyak::Label pass = e.EmitCond(terminal.if_, true, e.terminal_carry_inverted);
            // For ARM-sense CF, EmitCond complements CF (CMC) before testing HI/LS.
            if (!e.terminal_carry_inverted && (terminal.if_ == IR::Cond::HI || terminal.if_ == IR::Cond::LS))
                e.terminal_carry_inverted = true;
            if (e.exit_nzcv_live[0])
                e.EmitExitNZCV();
            e.EmitLeafTerminal(terminal.else_, initial_location, is_single_step);
            e.code.L(pass);
            if (e.exit_nzcv_live[1])
                e.EmitExitNZCV();
            e.EmitLeafTerminal(terminal.then_, initial_location, is_single_step);
            break;
        }
        const bool flags_available = e.terminal_nzcv_in_flags;
        e.terminal_nzcv_in_flags = false;
        Xbyak::Label pass = e.EmitCond(terminal.if_, flags_available);
        e.EmitLeafTerminal(terminal.else_, initial_location, is_single_step);
        e.code.L(pass);
        e.EmitLeafTerminal(terminal.then_, initial_location, is_single_step);
        break;
    }
    return true;
}

bool EmitTerminalImpl(A64EmitX64& e, IR::Term::CheckBit terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    Xbyak::Label fail;
    e.code.cmp(e.code.byte[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, check_bit)], u8(0));
    e.code.jz(fail);
    e.EmitLeafTerminal(terminal.then_, initial_location, is_single_step);
    e.code.L(fail);
    e.EmitLeafTerminal(terminal.else_, initial_location, is_single_step);
    return true;
}

bool EmitTerminalImpl(A64EmitX64& e, IR::Term::CheckHalt terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    e.code.cmp(dword[e.code.ABI_JIT_PTR + offsetof(A64JitState, halt_reason)], 0);
    e.code.jne(e.code.GetForceReturnFromRunCodeAddress());
    e.EmitLeafTerminal(terminal.else_, initial_location, is_single_step);
    return true;
}

}

bool A64EmitX64::EmitLeafTerminal(IR::Term::LeafTerminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step) noexcept {
    if (auto const x = std::get_if<IR::Term::ReturnToDispatch>(&terminal))
        return EmitTerminalImpl(*this, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::LinkBlock>(&terminal))
        return EmitTerminalImpl(*this, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::LinkBlockFast>(&terminal))
        return EmitTerminalImpl(*this, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::PopRSBHint>(&terminal))
        return EmitTerminalImpl(*this, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::FastDispatchHint>(&terminal))
        return EmitTerminalImpl(*this, *x, initial_location, is_single_step);
    UNREACHABLE();
}

bool A64EmitX64::EmitTerminal(IR::Term::Terminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step) noexcept {
    if (auto const x = std::get_if<IR::Term::LeafTerminal>(&terminal))
        return EmitLeafTerminal(*x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::If>(&terminal))
        return EmitTerminalImpl(*this, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::CheckBit>(&terminal))
        return EmitTerminalImpl(*this, *x, initial_location, is_single_step);
    if (auto const x = std::get_if<IR::Term::CheckHalt>(&terminal))
        return EmitTerminalImpl(*this, *x, initial_location, is_single_step);
    UNREACHABLE();
}

void A64EmitX64::MarkProfileLink(IR::LocationDescriptor source, IR::LocationDescriptor target) {
    if (!conf.profile_function_links) return;
    const auto from = conf.profile_cookie(A64::LocationDescriptor{source}.PC());
    const auto to = conf.profile_cookie(A64::LocationDescriptor{target}.PC());
    if (from && from == to) profile_internal_links.insert(code.getCurr());
    else profile_internal_links.erase(code.getCurr());
}

CodePtr A64EmitX64::UnlinkedExit(const IR::LocationDescriptor& target_desc) {
    CodePtr& exit = patch_information[target_desc].unlinked_exit;
    if (!exit) {
        code.SwitchToColdCode();
        code.EnsureMemoryCommitted();
        exit = code.getCurr();
        code.mov(rax, A64::LocationDescriptor{target_desc}.PC());
        code.mov(qword[code.ABI_JIT_PTR + offsetof(A64JitState, pc)], rax);
        code.jmp(code.GetReturnFromRunCodeAddress(), Xbyak::CodeGenerator::T_NEAR);
        code.SwitchToHotCode();
    }
    return exit;
}

CodePtr A64EmitX64::CompactLinkTarget(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr, CodePtr patch_location) {
    if (!target_code_ptr)
        return patch_information[target_desc].unlinked_exit;
    if (profile_internal_links.contains(patch_location))
        return static_cast<const u8*>(target_code_ptr) + profile_prefix_size;
    return target_code_ptr;
}

void A64EmitX64::EmitPatchJg(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr) {
    const CodePtr patch_location = code.getCurr();
    if (CompactBlockLinks()) {
        code.jg(CompactLinkTarget(target_desc, target_code_ptr, patch_location));
        code.EnsurePatchLocationSize(patch_location, 6);
        return;
    }
    if (target_code_ptr) {
        if (profile_internal_links.contains(patch_location)) target_code_ptr = static_cast<const u8*>(target_code_ptr) + profile_prefix_size;
        code.jg(target_code_ptr);
    } else {
        code.mov(rax, A64::LocationDescriptor{target_desc}.PC());
        code.mov(qword[code.ABI_JIT_PTR + offsetof(A64JitState, pc)], rax);
        code.jg(code.GetReturnFromRunCodeAddress());
    }
    code.EnsurePatchLocationSize(patch_location, 23);
}

void A64EmitX64::EmitPatchJz(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr) {
    const CodePtr patch_location = code.getCurr();
    if (CompactBlockLinks()) {
        code.jz(CompactLinkTarget(target_desc, target_code_ptr, patch_location));
        code.EnsurePatchLocationSize(patch_location, 6);
        return;
    }
    if (target_code_ptr) {
        if (profile_internal_links.contains(patch_location)) target_code_ptr = static_cast<const u8*>(target_code_ptr) + profile_prefix_size;
        code.jz(target_code_ptr);
    } else {
        code.mov(rax, A64::LocationDescriptor{target_desc}.PC());
        code.mov(qword[code.ABI_JIT_PTR + offsetof(A64JitState, pc)], rax);
        code.jz(code.GetReturnFromRunCodeAddress());
    }
    code.EnsurePatchLocationSize(patch_location, 23);
}

void A64EmitX64::EmitPatchJmp(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr) {
    const CodePtr patch_location = code.getCurr();
    if (CompactBlockLinks()) {
        code.jmp(CompactLinkTarget(target_desc, target_code_ptr, patch_location), Xbyak::CodeGenerator::T_NEAR);
        code.EnsurePatchLocationSize(patch_location, 5);
        return;
    }
    if (target_code_ptr) {
        if (profile_internal_links.contains(patch_location)) target_code_ptr = static_cast<const u8*>(target_code_ptr) + profile_prefix_size;
        code.jmp(target_code_ptr);
    } else {
        code.mov(rax, A64::LocationDescriptor{target_desc}.PC());
        code.mov(qword[code.ABI_JIT_PTR + offsetof(A64JitState, pc)], rax);
        code.jmp(code.GetReturnFromRunCodeAddress());
    }
    code.EnsurePatchLocationSize(patch_location, 22);
}

void A64EmitX64::EmitPatchMovRcx(CodePtr target_code_ptr) {
    if (!target_code_ptr) {
        target_code_ptr = code.GetReturnFromRunCodeAddress();
    }
    const CodePtr patch_location = code.getCurr();
    code.mov(code.rcx, u64(target_code_ptr));
    code.EnsurePatchLocationSize(patch_location, 10);
}

void A64EmitX64::Unpatch(const IR::LocationDescriptor& location) {
    EmitX64::Unpatch(location);
    if (conf.HasOptimization(OptimizationFlag::FastDispatch)) {
        code.DisableWriting();
        (*fast_dispatch_table_lookup)(location.Value()) = {};
        code.EnableWriting();
    }
}

}  // namespace Dynarmic::Backend::X64
