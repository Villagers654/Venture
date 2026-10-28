// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <cstring>
#include <memory>
#include <mutex>

#include <boost/icl/interval_set.hpp>
#include "common/assert.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/llvm_disassemble.h"
#include <bit>

#include "dynarmic/backend/x64/a64_emit_x64.h"
#include "dynarmic/backend/x64/a64_jitstate.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/devirtualize.h"
#include "dynarmic/backend/x64/jitstate_info.h"
#include "dynarmic/common/atomic.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/interface/A64/a64.h"
#include <xmmintrin.h>
#include <mcl/scope_exit.hpp>
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opt_passes.h"
#include "dynarmic/interface/profiling.h"

namespace Dynarmic::A64 {

using namespace Backend::X64;

static RunCodeCallbacks GenRunCodeCallbacks(A64::UserCallbacks* cb, CodePtr (*LookupBlock)(void* lookup_block_arg), void* arg, const A64::UserConfig& conf) {
    return RunCodeCallbacks{
        ArgCallback(LookupBlock, reinterpret_cast<u64>(arg)),
        ArgCallback(Devirtualize<&A64::UserCallbacks::AddTicks>(cb)),
        ArgCallback(Devirtualize<&A64::UserCallbacks::GetTicksRemaining>(cb)),
        conf.enable_cycle_counting,
    };
}

static std::function<void(BlockOfCode&)> GenRCP(const A64::UserConfig& conf) {
    return [conf](BlockOfCode& code) {
        if (conf.page_table) {
            code.mov(code.r14, std::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer && !conf.enable_register_cache) {
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

struct Jit::Impl final {
public:
    static UserConfig DiagnosticConfig(UserConfig config) {
        if (config.enable_block_frame_chain || config.very_verbose_debugging_output)
            config.enable_register_cache = false; // RBP cannot hold both a chain and X30.
        return config;
    }
    Impl(Jit* jit, UserConfig user_config)
        : conf(DiagnosticConfig(std::move(user_config)))
        , block_of_code(GenRunCodeCallbacks(conf.callbacks, &GetCurrentBlockThunk, this, conf), JitStateInfo{jit_state}, conf.code_cache_size, GenRCP(conf), conf.enable_register_cache ? offsetof(A64JitState, reg) : size_t(-1), conf.disabled_host_features, conf.cold_code_region)
        , emitter(block_of_code, conf, jit)
        , polyfill_options(GenPolyfillOptions(block_of_code))
    {
        ASSERT(!conf.enable_register_cache || (!conf.page_table && conf.fastmem_pointer && *conf.fastmem_pointer == 0 && conf.fastmem_address_space_bits == 64 && !conf.silently_mirror_fastmem));
        ASSERT(conf.page_table_address_space_bits >= 12 && conf.page_table_address_space_bits <= 64);
        if (UsesHostReturnStack(conf)) {
            // 1024 entries; the top pair is the sentinel entry.
            host_return_stack = std::make_unique<u64[]>(host_return_stack_words);
            u64* const base = &host_return_stack[host_return_stack_words - 2];
            base[0] = 0;
            base[1] = ~u64(0);
        }
        BindHostReturnStack();
    }

    ~Impl() = default;

    HaltReason Run() {
        if (conf.profile_cookie) Atomic::Exchange(&jit_state.profile_cookie, 0);
        ASSERT(!is_executing);
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        is_executing = true;
        SCOPE_EXIT { this->is_executing = false; };
        const CodePtr current_code_ptr = [this] {
            // RSB optimization
            const u32 new_rsb_ptr = (jit_state.rsb_ptr - 1) & A64JitState::RSB_PTR_MASK;
            if (jit_state.GetUniqueHash() == jit_state.rsb_location_descriptors[new_rsb_ptr]) {
                jit_state.rsb_ptr = new_rsb_ptr;
                return CodePtr(jit_state.rsb_codeptrs[new_rsb_ptr]);
            }
            // Block entry points are exact (Venture: block_alignment may pack them).
            return GetCurrentBlock();
        }();
        const u32 host_mxcsr = _mm_getcsr();
        HaltReason hr;
        try {
            hr = block_of_code.RunCode(&jit_state, current_code_ptr);
        } catch (...) {
            _mm_setcsr(host_mxcsr);
            throw;
        }
        PerformRequestedCacheInvalidation(hr);
        is_executing = false;
        return hr;
    }

    HaltReason Step() {
        ASSERT(!is_executing);
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        is_executing = true;
        SCOPE_EXIT { this->is_executing = false; };
        const u32 host_mxcsr = _mm_getcsr();
        HaltReason hr;
        try {
            hr = block_of_code.StepCode(&jit_state, GetCurrentSingleStep());
        } catch (...) {
            _mm_setcsr(host_mxcsr);
            throw;
        }
        PerformRequestedCacheInvalidation(hr);
        is_executing = false;
        return hr;
    }

    void ClearCache() {
        std::unique_lock lock{invalidation_mutex};
        invalidate_entire_cache = true;
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void InvalidateCacheRange(u64 start_address, size_t length) {
        std::unique_lock lock{invalidation_mutex};
        const auto end_address = static_cast<u64>(start_address + length - 1);
        const auto range = boost::icl::discrete_interval<u64>::closed(start_address, end_address);
        invalid_cache_ranges.add(range);
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void Reset() {
        ASSERT(!is_executing);
        jit_state = {};
        BindHostReturnStack();
    }

    void BindHostReturnStack() {
        if (!host_return_stack)
            return;
        jit_state.hrs_base = reinterpret_cast<u64>(&host_return_stack[host_return_stack_words - 2]);
        jit_state.hrs_limit = reinterpret_cast<u64>(&host_return_stack[0]);
        jit_state.hrs_sp = jit_state.hrs_base;
    }

    void HaltExecution(HaltReason hr) {
        Atomic::Or(&jit_state.halt_reason, static_cast<u32>(hr));
    }

    void ClearHalt(HaltReason hr) {
        Atomic::And(&jit_state.halt_reason, ~static_cast<u32>(hr));
    }

    u64 GetSP() const {
        return jit_state.sp;
    }

    void SetSP(u64 value) {
        jit_state.sp = value;
    }

    u64 GetPC() const {
        return jit_state.pc;
    }

    void SetPC(u64 value) {
        jit_state.pc = value;
    }

    u64 GetRegister(size_t index) const {
        if (index == 31)
            return GetSP();
        return jit_state.reg.at(index);
    }

    void SetRegister(size_t index, u64 value) {
        if (index == 31)
            return SetSP(value);
        jit_state.reg.at(index) = value;
    }

    std::array<u64, 31> GetRegisters() const {
        return jit_state.reg;
    }

    void SetRegisters(const std::array<u64, 31>& value) {
        jit_state.reg = value;
    }

    Vector GetVector(size_t index) const {
        return {jit_state.vec.at(index * 2), jit_state.vec.at(index * 2 + 1)};
    }

    void SetVector(size_t index, Vector value) {
        jit_state.vec.at(index * 2) = value[0];
        jit_state.vec.at(index * 2 + 1) = value[1];
    }

    std::array<Vector, 32> GetVectors() const {
        std::array<Vector, 32> ret;
        static_assert(sizeof(ret) == sizeof(jit_state.vec));
        std::memcpy(ret.data(), jit_state.vec.data(), sizeof(jit_state.vec));
        return ret;
    }

    void SetVectors(const std::array<Vector, 32>& value) {
        static_assert(sizeof(value) == sizeof(jit_state.vec));
        std::memcpy(jit_state.vec.data(), value.data(), sizeof(jit_state.vec));
    }

    u32 GetFpcr() const {
        return jit_state.GetFpcr();
    }

    void SetFpcr(u32 value) {
        jit_state.SetFpcr(value);
    }

    u32 GetFpsr() const {
        return jit_state.GetFpsr();
    }

    void SetFpsr(u32 value) {
        jit_state.SetFpsr(value);
    }

    u32 GetPstate() const {
        return jit_state.GetPstate();
    }

    void SetPstate(u32 value) {
        jit_state.SetPstate(value);
    }

    void ClearExclusiveState() {
        jit_state.exclusive_state = 0;
    }

    bool IsExecuting() const {
        return is_executing;
    }

    const void* CodeCacheBegin() const { return block_of_code.getCode(); }
    u32 GetProfileCookie() { return Atomic::Load(&jit_state.profile_cookie); }
    std::size_t CodeCacheSize() const { return conf.code_cache_size; }

    bool Precompile(u64 value) {
        if (is_executing) return false;
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        const IR::LocationDescriptor descriptor{value};
        if (emitter.GetBasicBlock(descriptor)) return true;
        // Do not evict demand-compiled blocks to warm a speculative descriptor.
        if (block_of_code.SpaceRemaining() < 4 * 1024 * 1024) return false;
        try {
            CompileBlock(descriptor);
        } catch (...) {
            // A failed emission must not leave a partially published link live.
            // Run services this invalidation before using the RSB or any block.
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
    static CodePtr GetCurrentBlockThunk(void* thisptr) {
        Jit::Impl* this_ = static_cast<Jit::Impl*>(thisptr);
        if (this_->conf.profile_cookie) Atomic::Exchange(&this_->jit_state.profile_cookie, 0);
        return this_->GetCurrentBlock();
    }

    IR::LocationDescriptor GetCurrentLocation() const {
        return IR::LocationDescriptor{jit_state.GetUniqueHash()};
    }

    CodePtr GetCurrentBlock() {
        return GetBlock(GetCurrentLocation());
    }

    CodePtr GetCurrentSingleStep() {
        return GetBlock(A64::LocationDescriptor{GetCurrentLocation()}.SetSingleStepping(true));
    }

    CodePtr GetBlock(IR::LocationDescriptor descriptor) {
        if (auto block = emitter.GetBasicBlock(descriptor))
            return block->entrypoint;

        return CompileBlock(descriptor);
    }

    [[gnu::noinline]] CodePtr CompileBlock(IR::LocationDescriptor descriptor) {
        // Compilation must use host FP state without altering the suspended
        // guest rounding mode, flush mode or accumulated exception flags.
        const unsigned saved_mxcsr = _mm_getcsr();
        SCOPE_EXIT { _mm_setcsr(saved_mxcsr); };
        _mm_setcsr(0x1f80);
        CompileTimingScope timing{A64::LocationDescriptor{descriptor}.PC(), false, descriptor.Value()};

        constexpr size_t MINIMUM_REMAINING_CODESIZE = 1 * 1024 * 1024;
        if (block_of_code.SpaceRemaining() < MINIMUM_REMAINING_CODESIZE) {
            // Immediately evacuate cache
            invalidate_entire_cache = true;
            PerformRequestedCacheInvalidation(HaltReason::CacheInvalidation);
        }
        block_of_code.EnsureMemoryCommitted(MINIMUM_REMAINING_CODESIZE);

        // JIT Compile
        const auto get_code = [this](u64 vaddr) { return conf.callbacks->MemoryReadCode(vaddr); };
        // LocationDescriptor ctor() does important ops (like tflags) do not skip
        auto const arch_descriptor = A64::LocationDescriptor{descriptor};
        SetCompilePhase(CompilePhase::Decode);
        ir_block.Reset(arch_descriptor);
        emitter.inlined_code.clear();
        A64::Translate(ir_block, arch_descriptor, get_code,
            {conf.define_unpredictable_behaviour, conf.wall_clock_cntpct, true,
             conf.enable_bounded_halt_polling && !conf.enable_cycle_counting,
             conf.inline_outlined_atomics && !conf.diagnostic_entry.record, &emitter.inlined_code,
             conf.diagnostic_entry.record ? conf.diagnostic_entry.selected : std::function<bool(u64)>{}});
        Optimization::Optimize(ir_block, conf, polyfill_options);
        const auto emitted = emitter.Emit(ir_block);
        SetCompilePhase(CompilePhase::Finalize);
        timing.Complete();
        return emitted.entrypoint;
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

    IR::Block ir_block = {LocationDescriptor(0, FP::FPCR(0), false)};
    const UserConfig conf;
    A64JitState jit_state;
    static constexpr size_t host_return_stack_words = (1024 * 16 + 16) / sizeof(u64);
    std::unique_ptr<u64[]> host_return_stack;
    BlockOfCode block_of_code;
    A64EmitX64 emitter;
    Optimization::PolyfillOptions polyfill_options;
    bool is_executing = false;
    bool invalidate_entire_cache = false;
    boost::icl::interval_set<u64> invalid_cache_ranges;
    std::mutex invalidation_mutex;
};

Jit::Jit(UserConfig conf)
        : impl(std::make_unique<Jit::Impl>(this, conf)) {}

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

void Jit::InvalidateCacheRange(u64 start_address, size_t length) {
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

u64 Jit::GetSP() const {
    return impl->GetSP();
}

void Jit::SetSP(u64 value) {
    impl->SetSP(value);
}

// VENTURE_PROFILE_CACHE_RANGE_V1: no mutation or profiling on the hot path.
const void* Jit::CodeCacheBegin() const {
    return impl->CodeCacheBegin();
}
std::uint32_t Jit::GetProfileCookie() const { return impl->GetProfileCookie(); }

std::size_t Jit::CodeCacheSize() const {
    return impl->CodeCacheSize();
}

bool Jit::Precompile(std::uint64_t descriptor) {
    return impl->Precompile(descriptor);
}

u64 Jit::GetPC() const {
    return impl->GetPC();
}

void Jit::SetPC(u64 value) {
    impl->SetPC(value);
}

u64 Jit::GetRegister(size_t index) const {
    return impl->GetRegister(index);
}

void Jit::SetRegister(size_t index, u64 value) {
    impl->SetRegister(index, value);
}

std::array<u64, 31> Jit::GetRegisters() const {
    return impl->GetRegisters();
}

void Jit::SetRegisters(const std::array<u64, 31>& value) {
    impl->SetRegisters(value);
}

Vector Jit::GetVector(size_t index) const {
    return impl->GetVector(index);
}

void Jit::SetVector(size_t index, Vector value) {
    impl->SetVector(index, value);
}

std::array<Vector, 32> Jit::GetVectors() const {
    return impl->GetVectors();
}

void Jit::SetVectors(const std::array<Vector, 32>& value) {
    impl->SetVectors(value);
}

u32 Jit::GetFpcr() const {
    return impl->GetFpcr();
}

void Jit::SetFpcr(u32 value) {
    impl->SetFpcr(value);
}

u32 Jit::GetFpsr() const {
    return impl->GetFpsr();
}

void Jit::SetFpsr(u32 value) {
    impl->SetFpsr(value);
}

u32 Jit::GetPstate() const {
    return impl->GetPstate();
}

void Jit::SetPstate(u32 value) {
    impl->SetPstate(value);
}

void Jit::ClearExclusiveState() {
    impl->ClearExclusiveState();
}

bool Jit::IsExecuting() const {
    return impl->IsExecuting();
}

std::string Jit::Disassemble() const {
    return impl->Disassemble();
}

}  // namespace Dynarmic::A64
