// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>

#include "common/common_types.h"
#include "common/x64/xbyak.h"
#include "dynarmic/mcl/bit.hpp"
#include "dynarmic/backend/x64/xbyak.h"
#include "dynarmic/backend/x64/abi.h"
#include "dynarmic/backend/x64/callback.h"
#include "dynarmic/backend/x64/constant_pool.h"
#include "dynarmic/backend/x64/host_feature.h"
#include "dynarmic/backend/x64/jitstate_info.h"
#include "dynarmic/common/cast_util.h"
#include "dynarmic/interface/halt_reason.h"
#include "dynarmic/ir/cond.h"

// Venture: hosts whose code cache is one memory object mapped executable and
// writable (VENTURE_DUAL_MAPPED_CODE_CACHE_V1 in block_of_code.cpp).
#if defined(_WIN32) || defined(__linux__)
#    define VENTURE_DUAL_MAPPED_CODE 1
#endif

namespace Dynarmic::Backend::X64 {

using CodePtr = const void*;

struct RunCodeCallbacks {
    ArgCallback LookupBlock;
    ArgCallback AddTicks;
    ArgCallback GetTicksRemaining;
    bool enable_cycle_counting;
};

class BlockOfCode final : public Xbyak::CodeGenerator {
public:
    BlockOfCode(RunCodeCallbacks cb, JitStateInfo jsi, size_t total_code_size, std::function<void(BlockOfCode&)> rcp, size_t a64_register_cache_offset = size_t(-1), u32 disabled_host_features = 0, bool cold_region = false);
    BlockOfCode(const BlockOfCode&) = delete;

    /// Call when external emitters have finished emitting their preludes.
    void PreludeComplete();

    /// Change permissions to RW. This is required to support systems with W^X enforced.
    void EnableWriting();
    /// Change permissions to RX. This is required to support systems with W^X enforced.
    void DisableWriting();
    /// The executable view is never writable: code-space stores go through this
    /// alias of a code-space address in the writable view of the same memory.
    template<typename T>
    T* GetWritablePointer(const T* p) {
        return reinterpret_cast<T*>(writableTop_ + (reinterpret_cast<const u8*>(p) - top_));
    }

    /// Clears this block of code and resets code pointer to beginning.
    void ClearCache();
    /// Calculates how much space is remaining to use.
    size_t SpaceRemaining() const;
    /// Ensure at least codesize bytes of code cache memory are committed at the current code_ptr.
    void EnsureMemoryCommitted(size_t codesize);
    // Maintain the append window throughout large blocks and deferred handlers.
    void EnsureMemoryCommitted();
    // Commit code-space pages outside the append window (constant pool slots).
    void CommitCodeSpace(const void* begin, size_t size);

    /// Runs emulated code from code_ptr.
    HaltReason RunCode(void* jit_state, CodePtr code_ptr) const;
    /// Runs emulated code from code_ptr for a single cycle.
    HaltReason StepCode(void* jit_state, CodePtr code_ptr) const;
    /// Code emitter: Returns to dispatcher
    void ReturnFromRunCode(bool mxcsr_already_exited = false);
    /// Code emitter: Returns to dispatcher, forces return to host
    void ForceReturnFromRunCode(bool mxcsr_already_exited = false);
    /// Code emitter: Makes guest MXCSR the current MXCSR
    void SwitchMxcsrOnEntry();
    /// Code emitter: Makes saved host MXCSR the current MXCSR
    void SwitchMxcsrOnExit();
    /// Code emitter: Enter standard ASIMD MXCSR region
    void EnterStandardASIMD();
    /// Code emitter: Leave standard ASIMD MXCSR region
    void LeaveStandardASIMD();
    /// Code emitter: Updates cycles remaining my calling cb.AddTicks and cb.GetTicksRemaining
    /// @note this clobbers ABI caller-save registers
    void UpdateTicks();
    /// Code emitter: Performs a block lookup based on current state
    /// @note this clobbers ABI caller-save registers
    void LookupBlock();

    /// Code emitter: Load required flags for conditional cond from rax into host rflags
    void LoadRequiredFlagsForCondFromRax(IR::Cond cond);

    /// Guest GPRs held in host registers by the A64 register cache. Index 31 is SP,
    /// which A64JitState stores directly after reg[30].
#ifdef _WIN32
    static constexpr std::array<size_t, 9> a64_cached_indices{0, 1, 30, 8, 31, 19, 20, 9, 10};
#else
    static constexpr std::array<size_t, 7> a64_cached_indices{0, 1, 30, 8, 20, 9, 10};
#endif
    std::optional<Xbyak::Reg64> A64CachedRegister(size_t index) const;
    void FlushA64RegisterCache();
    void ReloadA64RegisterCache();
    /// Venture: CMPXCHG16B takes RBX, which the register cache may hold. The
    /// 128-bit atomic accesses store it before taking RBX and reload it where
    /// their paths join; their fallbacks reload it before their callbacks
    /// synchronize the cache. Both are no-ops when RBX is not cached.
    bool A64CacheHoldsRbx() const;
    void FlushA64CachedRbx();
    void ReloadA64CachedRbx();

    /// @brief Code emitter: Calls the function
    template<typename F>
        requires std::is_pointer_v<F> && std::is_function_v<std::remove_pointer_t<F>>
    void CallFunction(F fn, bool synchronize_a64_register_cache = true) {
        if (synchronize_a64_register_cache) FlushA64RegisterCache();
        ::Common::X64::CallFarFunction(*this, fn);
        if (synchronize_a64_register_cache) ReloadA64RegisterCache();
    }

    /// @brief Code emitter: Calls the lambda. Lambda must not have any captures.
    template<typename Lambda>
    void CallLambda(Lambda l, bool synchronize_a64_register_cache = true) {
        CallFunction(Common::FptrCast(l), synchronize_a64_register_cache);
    }

    void ZeroExtendFrom(size_t bitsize, Xbyak::Reg64 reg) {
        switch (bitsize) {
        case 8:
            movzx(reg.cvt32(), reg.cvt8());
            return;
        case 16:
            movzx(reg.cvt32(), reg.cvt16());
            return;
        case 32:
            mov(reg.cvt32(), reg.cvt32());
            return;
        case 64:
            return;
        default:
            UNREACHABLE();
        }
    }

    Xbyak::Address Const(const Xbyak::AddressFrame& frame, u64 lower, u64 upper = 0);

    template<size_t esize>
    Xbyak::Address BConst(const Xbyak::AddressFrame& frame, u64 value) {
        return Const(frame, mcl::bit::replicate_element<u64>(esize, value),
                     mcl::bit::replicate_element<u64>(esize, value));
    }

    CodePtr GetCodeBegin() const;
    size_t GetTotalCodeSize() const;

    const void* GetReturnFromRunCodeAddress() const {
        return return_from_run_code[0];
    }

    const void* GetForceReturnFromRunCodeAddress() const {
        return return_from_run_code[FORCE_RETURN];
    }

    void int3() { db(0xCC); }

    /// Allocate memory of `size` bytes from the same block of memory the code is in.
    /// This is useful for objects that need to be placed close to or within code.
    /// The lifetime of this memory is the same as the code around it.
    void* AllocateFromCodeSpace(size_t size);
    // Leave pool pages uncommitted until constants first use them.
    void* ReserveFromCodeSpace(size_t alloc_size);

    void SetCodePtr(CodePtr code_ptr);
    /// Venture: with a cold region, blocks emit their deferred handlers into the
    /// upper part of the code space, so the hot code of consecutively compiled
    /// blocks is contiguous. Both parts grow upward; either filling up clears the cache.
    bool HasColdRegion() const { return cold_begin != 0; }
    /// Code-space offset where the cold region begins (0 without one).
    size_t ColdRegionOffset() const { return cold_begin; }
    void SwitchToColdCode();
    void SwitchToHotCode();
    /// Venture: a bounded region inside the prelude for code emitted on demand
    /// after it (fastmem fallbacks). Cache clears keep it; pages commit as it fills.
    void ReserveThunkArena(size_t size);
    /// Emits `emit` at the arena's insertion point, then restores the current one.
    template<typename EmitFn>
    void EmitInThunkArena(EmitFn&& emit) {
        BeginThunkArena();
        try {
            emit();
        } catch (...) {
            EndThunkArena();
            throw;
        }
        EndThunkArena();
    }
    void EnsurePatchLocationSize(CodePtr begin, size_t size);

    static const Xbyak::Reg64 ABI_JIT_PTR;
    // ABI registers
#ifdef _WIN32
    static const Xbyak::Reg64 ABI_RETURN;
    static const Xbyak::Reg64 ABI_PARAM1;
    static const Xbyak::Reg64 ABI_PARAM2;
    static const Xbyak::Reg64 ABI_PARAM3;
    static const Xbyak::Reg64 ABI_PARAM4;
    static const std::array<Xbyak::Reg64, ABI_PARAM_COUNT> ABI_PARAMS;
#else
    static const Xbyak::Reg64 ABI_RETURN;
    static const Xbyak::Reg64 ABI_RETURN2;
    static const Xbyak::Reg64 ABI_PARAM1;
    static const Xbyak::Reg64 ABI_PARAM2;
    static const Xbyak::Reg64 ABI_PARAM3;
    static const Xbyak::Reg64 ABI_PARAM4;
    static const Xbyak::Reg64 ABI_PARAM5;
    static const Xbyak::Reg64 ABI_PARAM6;
    static const std::array<Xbyak::Reg64, ABI_PARAM_COUNT> ABI_PARAMS;
#endif

    JitStateInfo GetJitStateInfo() const { return jsi; }

    const ABIPrologue& GetRunCodePrologue() const { return run_prologue; }
    std::array<size_t, 3> GetRunCodeRanges() const {
        return {static_cast<size_t>(reinterpret_cast<const u8*>(run_code) - getCode()),
                static_cast<size_t>(reinterpret_cast<const u8*>(step_code) - getCode()),
                static_cast<size_t>(static_cast<const u8*>(return_from_run_code[0]) - getCode())};
    }

    struct NativeUnwindFrame { const void* begin; const void* end; ABIPrologue prologue; bool inherits_run_frame; };
    using NativeUnwindRegistrar = std::function<void(const void*, const void*, const ABIPrologue&, bool)>;
    void RegisterNativeUnwindFrame(const void* begin, const void* end, const ABIPrologue& prologue, bool inherits_run_frame = false);
    const std::vector<NativeUnwindFrame>& GetPendingNativeUnwindFrames() const { return native_unwind_frames; }
    void SetNativeUnwindCallbacks(NativeUnwindRegistrar registrar, std::function<void(size_t)> reset);
    bool HasHostFeature(HostFeature feature) const noexcept;

private:
    ABIPrologue run_prologue;
    std::vector<NativeUnwindFrame> native_unwind_frames;
    NativeUnwindRegistrar native_unwind_registrar;
    std::function<void(size_t)> reset_native_unwind;
    using RunCodeFuncType = HaltReason (*)(void*, CodePtr);
    static constexpr size_t MXCSR_ALREADY_EXITED = 1 << 0;
    static constexpr size_t FORCE_RETURN = 1 << 1;

    JitStateInfo jsi;
    const size_t a64_register_cache_offset;
    const u32 disabled_host_features;
    std::array<const void*, 4> return_from_run_code;
    RunCodeFuncType run_code = nullptr;
    RunCodeFuncType step_code = nullptr;
    RunCodeCallbacks cb;
    CodePtr code_begin = nullptr;
#ifdef VENTURE_DUAL_MAPPED_CODE
    size_t committed_size = 0;
#endif
    bool writing = false;
    size_t flush_begin = 0, flush_end = 0;
    // Cold region: its insertion point while hot code is emitted, the hot one
    // while cold code is, its commit watermark and its part written since EnableWriting.
    size_t cold_begin = 0, cold_size = 0, hot_size = 0, cold_committed = 0, cold_flush_begin = 0;
    bool in_cold = false;
    size_t RegionEnd() const { return in_arena ? arena_committed : in_cold || !cold_begin ? maxSize_ : cold_begin; }
    // Thunk arena: [arena_begin, arena_end), insertion point, commit watermark,
    // the code pointer and cold state it interrupted, and its unflushed part.
    size_t arena_begin = 0, arena_end = 0, arena_size = 0, arena_committed = 0, arena_saved_size = 0;
    size_t arena_flush_begin = 0, arena_flush_end = 0;
    bool in_arena = false, arena_saved_cold = false;
    void BeginThunkArena();
    void EndThunkArena();
    bool prelude_complete = false;
    // The pool emits bytes and commits pages during its constructor. All
    // commitment and publication state must already have been initialized.
    ConstantPool constant_pool;

    void GenRunCode(std::function<void(BlockOfCode&)> rcp);
};

}  // namespace Dynarmic::Backend::X64
