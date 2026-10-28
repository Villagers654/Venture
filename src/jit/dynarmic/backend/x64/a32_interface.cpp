// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <functional>
#include <mcl/scope_exit.hpp>
#include <memory>
#include <mutex>

#include <boost/icl/interval_set.hpp>
#include <fmt/format.h>
#include "common/assert.h"
#include <bit>
#include "common/common_types.h"
#include "dynarmic/common/llvm_disassemble.h"

#include "dynarmic/backend/x64/a32_emit_x64.h"
#include "dynarmic/backend/x64/a32_jitstate.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/callback.h"
#include "dynarmic/backend/x64/devirtualize.h"
#include "dynarmic/backend/x64/jitstate_info.h"
#include "dynarmic/common/atomic.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A32/translate/a32_translate.h"
#include "dynarmic/interface/A32/a32.h"
#include <xmmintrin.h>
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/location_descriptor.h"
#include "dynarmic/ir/opt_passes.h"
#include "dynarmic/interface/profiling.h"

namespace Dynarmic::A32 {

using namespace Backend::X64;

static RunCodeCallbacks GenRunCodeCallbacks(A32::UserCallbacks* cb, CodePtr (*LookupBlock)(void* lookup_block_arg), void* arg, const A32::UserConfig& conf) {
    return RunCodeCallbacks{
        ArgCallback(LookupBlock, reinterpret_cast<u64>(arg)),
        ArgCallback(Devirtualize<&A32::UserCallbacks::AddTicks>(cb)),
        ArgCallback(Devirtualize<&A32::UserCallbacks::GetTicksRemaining>(cb)),
        conf.enable_cycle_counting,
    };
}

static std::function<void(BlockOfCode&)> GenRCP(const A32::UserConfig& conf) {
    return [conf](BlockOfCode& code) {
        if (conf.page_table) {
            code.mov(code.r14, std::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer) {
            code.mov(code.r13, *conf.fastmem_pointer);
        }
    };
}

static Optimization::PolyfillOptions GenPolyfillOptions(const BlockOfCode& code) {
    return Optimization::PolyfillOptions{
        .sha256 = !code.HasHostFeature(HostFeature::SHA),
        .vector_multiply_widen = true,
    };
}

struct Jit::Impl {
    Impl(Jit* jit, A32::UserConfig conf) noexcept
        : block_of_code(GenRunCodeCallbacks(conf.callbacks, &GetCurrentBlockThunk, this, conf), JitStateInfo{jit_state}, conf.code_cache_size, GenRCP(conf), size_t(-1), conf.disabled_host_features, conf.cold_code_region)
        , emitter(block_of_code, conf, jit)
        , polyfill_options(GenPolyfillOptions(block_of_code))
        , conf(std::move(conf))
        , jit_interface(jit)
    {}

    ~Impl() = default;

    HaltReason Run() {
        if (conf.profile_cookie) Atomic::Exchange(&jit_state.profile_cookie, 0);
        ASSERT(!jit_interface->is_executing);
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        jit_interface->is_executing = true;
        SCOPE_EXIT { jit_interface->is_executing = false; };
        const CodePtr current_codeptr = [this] {
            // RSB optimization
            const u32 new_rsb_ptr = (jit_state.rsb_ptr - 1) & A32JitState::RSB_PTR_MASK;
            if (jit_state.GetUniqueHash() == jit_state.rsb_location_descriptors[new_rsb_ptr]) {
                jit_state.rsb_ptr = new_rsb_ptr;
                return reinterpret_cast<CodePtr>(jit_state.rsb_codeptrs[new_rsb_ptr]);
            }

            return GetCurrentBlock();
        }();
        const u32 host_mxcsr = _mm_getcsr();
        HaltReason hr;
        try {
            hr = block_of_code.RunCode(&jit_state, current_codeptr);
        } catch (...) {
            _mm_setcsr(host_mxcsr);
            throw;
        }
        PerformRequestedCacheInvalidation(hr);
        jit_interface->is_executing = false;
        return hr;
    }

    HaltReason Step() {
        ASSERT(!jit_interface->is_executing);
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        jit_interface->is_executing = true;
        SCOPE_EXIT { jit_interface->is_executing = false; };
        const u32 host_mxcsr = _mm_getcsr();
        HaltReason hr;
        try {
            hr = block_of_code.StepCode(&jit_state, GetCurrentSingleStep());
        } catch (...) {
            _mm_setcsr(host_mxcsr);
            throw;
        }
        PerformRequestedCacheInvalidation(hr);
        jit_interface->is_executing = false;
        return hr;
    }

    void ClearCache() {
        std::unique_lock lock{invalidation_mutex};
        invalidate_entire_cache = true;
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void InvalidateCacheRange(std::uint32_t start_address, std::size_t length) {
        std::unique_lock lock{invalidation_mutex};
        invalid_cache_ranges.add(boost::icl::discrete_interval<u32>::closed(start_address, static_cast<u32>(start_address + length - 1)));
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void Reset() {
        ASSERT(!jit_interface->is_executing);
        jit_state = {};
    }

    void HaltExecution(HaltReason hr) {
        Atomic::Or(&jit_state.halt_reason, static_cast<u32>(hr));
    }

    void ClearHalt(HaltReason hr) {
        Atomic::And(&jit_state.halt_reason, ~static_cast<u32>(hr));
    }

    void ClearExclusiveState() {
        jit_state.exclusive_state = 0;
    }

    std::array<u32, 16>& Regs() {
        return jit_state.Reg;
    }

    const std::array<u32, 16>& Regs() const {
        return jit_state.Reg;
    }

    std::array<u32, 64>& ExtRegs() {
        return jit_state.ExtReg;
    }

    const std::array<u32, 64>& ExtRegs() const {
        return jit_state.ExtReg;
    }

    u32 Cpsr() const {
        return jit_state.Cpsr();
    }

    void SetCpsr(u32 value) {
        return jit_state.SetCpsr(value);
    }

    u32 Fpscr() const {
        return jit_state.Fpscr();
    }

    void SetFpscr(u32 value) {
        return jit_state.SetFpscr(value);
    }

    const void* CodeCacheBegin() const { return block_of_code.getCode(); }
    u32 GetProfileCookie() { return Atomic::Load(&jit_state.profile_cookie); }
    std::size_t CodeCacheSize() const { return conf.code_cache_size; }

    bool Precompile(u64 value) {
        if (jit_interface->is_executing) return false;
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        const IR::LocationDescriptor descriptor{value};
        if (emitter.GetBasicBlock(descriptor)) return true;
        if (block_of_code.SpaceRemaining() < 4 * 1024 * 1024) return false;
        try {
            CompileBlock(descriptor);
        } catch (...) {
            ClearCache();
            throw;
        }
        return true;
    }

    std::string Disassemble() const {
        const size_t size = reinterpret_cast<const char*>(block_of_code.getCurr()) - reinterpret_cast<const char*>(block_of_code.GetCodeBegin());
        auto const* p = reinterpret_cast<const char*>(block_of_code.GetCodeBegin());
        return Common::DisassembleX64(p, p + size);
    }

private:
    static CodePtr GetCurrentBlockThunk(void* this_voidptr) {
        Jit::Impl& this_ = *static_cast<Jit::Impl*>(this_voidptr);
        if (this_.conf.profile_cookie) Atomic::Exchange(&this_.jit_state.profile_cookie, 0);
        return this_.GetCurrentBlock();
    }

    IR::LocationDescriptor GetCurrentLocation() const {
        return IR::LocationDescriptor{jit_state.GetUniqueHash()};
    }

    CodePtr GetCurrentBlock() {
        return GetBasicBlock(GetCurrentLocation()).entrypoint;
    }

    CodePtr GetCurrentSingleStep() {
        return GetBasicBlock(A32::LocationDescriptor{GetCurrentLocation()}.SetSingleStepping(true)).entrypoint;
    }

    A32EmitX64::BlockDescriptor GetBasicBlock(IR::LocationDescriptor descriptor) {
        if (auto block = emitter.GetBasicBlock(descriptor))
            return *block;

        return CompileBlock(descriptor);
    }

    [[gnu::noinline]] A32EmitX64::BlockDescriptor CompileBlock(IR::LocationDescriptor descriptor) {
        // Keep the compiler's host FP environment off the cached lookup path.
        const unsigned saved_mxcsr = _mm_getcsr();
        SCOPE_EXIT { _mm_setcsr(saved_mxcsr); };
        _mm_setcsr(0x1f80);
        CompileTimingScope timing{A32::LocationDescriptor{descriptor}.PC(), true, descriptor.Value()};

        constexpr size_t MINIMUM_REMAINING_CODESIZE = 1 * 1024 * 1024;
        if (block_of_code.SpaceRemaining() < MINIMUM_REMAINING_CODESIZE) {
            invalidate_entire_cache = true;
            PerformRequestedCacheInvalidation(HaltReason::CacheInvalidation);
        }
        block_of_code.EnsureMemoryCommitted(MINIMUM_REMAINING_CODESIZE);

        // LocationDescriptor ctor() does important ops (like tflags) do not skip
        auto const arch_descriptor = A32::LocationDescriptor{descriptor};
        SetCompilePhase(CompilePhase::Decode);
        ir_block.Reset(arch_descriptor);
        A32::Translate(ir_block, arch_descriptor, conf.callbacks, {conf.arch_version, conf.define_unpredictable_behaviour, conf.hook_hint_instructions});
        Optimization::Optimize(ir_block, conf, polyfill_options);
        const auto emitted = emitter.Emit(ir_block);
        SetCompilePhase(CompilePhase::Finalize);
        timing.Complete();
        return emitted;
    }

    void PerformRequestedCacheInvalidation(HaltReason hr) {
        if (Has(hr, HaltReason::CacheInvalidation)) {
            std::unique_lock lock{invalidation_mutex};

            SCOPE_FAIL { HaltExecution(HaltReason::CacheInvalidation); };
            ClearHalt(HaltReason::CacheInvalidation);

            if (!invalidate_entire_cache && invalid_cache_ranges.empty()) {
                return;
            }

            jit_state.ResetRSB();
            if (invalidate_entire_cache) {
                block_of_code.ClearCache();
                emitter.ClearCache();
            } else {
                emitter.InvalidateCacheRanges(invalid_cache_ranges);
            }
            invalid_cache_ranges.clear();
            invalidate_entire_cache = false;
        }
    }

    IR::Block ir_block = {LocationDescriptor(0, PSR(0), FPSCR(0), false)};
    A32JitState jit_state;
    BlockOfCode block_of_code;
    A32EmitX64 emitter;
    Optimization::PolyfillOptions polyfill_options;
    // Keep it here in order to not mess with initializer lists
    const A32::UserConfig conf;
    Jit* jit_interface;

    // Requests made during execution to invalidate the cache are queued up here.
    bool invalidate_entire_cache = false;
    boost::icl::interval_set<u32> invalid_cache_ranges;
    std::mutex invalidation_mutex;
};

Jit::Jit(UserConfig conf)
        : impl(std::make_unique<Impl>(this, std::move(conf))) {}

Jit::~Jit() = default;

HaltReason Jit::Run() {
    return impl->Run();
}

HaltReason Jit::Step() {
    return impl->Step();
}

void Jit::ClearCache() {
    impl->ClearCache();
}

void Jit::InvalidateCacheRange(std::uint32_t start_address, std::size_t length) {
    impl->InvalidateCacheRange(start_address, length);
}

void Jit::Reset() {
    impl->Reset();
}

void Jit::HaltExecution(HaltReason hr) {
    impl->HaltExecution(hr);
}

void Jit::ClearHalt(HaltReason hr) {
    impl->ClearHalt(hr);
}

std::array<std::uint32_t, 16>& Jit::Regs() {
    return impl->Regs();
}

const std::array<std::uint32_t, 16>& Jit::Regs() const {
    return impl->Regs();
}

std::array<std::uint32_t, 64>& Jit::ExtRegs() {
    return impl->ExtRegs();
}

const std::array<std::uint32_t, 64>& Jit::ExtRegs() const {
    return impl->ExtRegs();
}

std::uint32_t Jit::Cpsr() const {
    return impl->Cpsr();
}

void Jit::SetCpsr(std::uint32_t value) {
    impl->SetCpsr(value);
}

std::uint32_t Jit::Fpscr() const {
    return impl->Fpscr();
}

void Jit::SetFpscr(std::uint32_t value) {
    impl->SetFpscr(value);
}

void Jit::ClearExclusiveState() {
    impl->ClearExclusiveState();
}

const void* Jit::CodeCacheBegin() const { return impl->CodeCacheBegin(); }
std::uint32_t Jit::GetProfileCookie() const { return impl->GetProfileCookie(); }
std::size_t Jit::CodeCacheSize() const { return impl->CodeCacheSize(); }
bool Jit::Precompile(std::uint64_t descriptor) { return impl->Precompile(descriptor); }

std::string Jit::Disassemble() const {
    return impl->Disassemble();
}

}  // namespace Dynarmic::A32
