// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/x64/block_of_code.h"

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#else
#    include <sys/mman.h>
#    include <unistd.h>
#endif

#if defined(__linux__) && !defined(MFD_NOEXEC_SEAL)
#    define MFD_NOEXEC_SEAL 0x0008U
#endif

#ifdef __APPLE__
#    include <errno.h>
#    include <fmt/format.h>
#    include <sys/sysctl.h>
#endif

#include <algorithm>
#include <cerrno>
#include <exception>
#include <stdexcept>
#include <array>
#include <cstring>

#include "common/assert.h"
#include "dynarmic/mcl/bit.hpp"
#include "dynarmic/backend/x64/xbyak.h"

#include "dynarmic/backend/x64/a32_jitstate.h"
#include "dynarmic/backend/x64/abi.h"
#include "dynarmic/backend/x64/hostloc.h"
#include "dynarmic/backend/x64/perf_map.h"
#include "dynarmic/backend/x64/stack_layout.h"

namespace Dynarmic::Backend::X64 {

const Xbyak::Reg64 BlockOfCode::ABI_JIT_PTR = HostLocToReg64(Dynarmic::Backend::X64::ABI_JIT_PTR);
#ifdef _WIN32
const Xbyak::Reg64 BlockOfCode::ABI_RETURN = HostLocToReg64(Dynarmic::Backend::X64::ABI_RETURN);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM1 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM1);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM2 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM2);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM3 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM3);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM4 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM4);
const std::array<Xbyak::Reg64, ABI_PARAM_COUNT> BlockOfCode::ABI_PARAMS = {BlockOfCode::ABI_PARAM1, BlockOfCode::ABI_PARAM2, BlockOfCode::ABI_PARAM3, BlockOfCode::ABI_PARAM4};
#else
const Xbyak::Reg64 BlockOfCode::ABI_RETURN = HostLocToReg64(Dynarmic::Backend::X64::ABI_RETURN);
const Xbyak::Reg64 BlockOfCode::ABI_RETURN2 = HostLocToReg64(Dynarmic::Backend::X64::ABI_RETURN2);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM1 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM1);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM2 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM2);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM3 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM3);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM4 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM4);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM5 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM5);
const Xbyak::Reg64 BlockOfCode::ABI_PARAM6 = HostLocToReg64(Dynarmic::Backend::X64::ABI_PARAM6);
const std::array<Xbyak::Reg64, ABI_PARAM_COUNT> BlockOfCode::ABI_PARAMS = {BlockOfCode::ABI_PARAM1, BlockOfCode::ABI_PARAM2, BlockOfCode::ABI_PARAM3, BlockOfCode::ABI_PARAM4, BlockOfCode::ABI_PARAM5, BlockOfCode::ABI_PARAM6};
#endif

namespace {

constexpr size_t CONSTANT_POOL_SIZE = 2 * 1024 * 1024;
constexpr size_t PRELUDE_COMMIT_SIZE = 1024 * 1024;

#ifdef __linux__
#    ifndef MADV_POPULATE_WRITE
#        define MADV_POPULATE_WRITE 23
#    endif
// memfd pages are charged when first written. Fault them in through the writable
// view first, so that a refused charge (strict overcommit) is an allocation
// failure rather than SIGBUS at the store that first touches the page. Kernels
// before 5.14 reject MADV_POPULATE_WRITE with EINVAL and populate on first touch.
bool PopulateWritable(void* p, size_t size) {
    const uintptr_t begin = reinterpret_cast<uintptr_t>(p) & ~uintptr_t{4095};
    const uintptr_t end = (reinterpret_cast<uintptr_t>(p) + size + 4095) & ~uintptr_t{4095};
    return madvise(reinterpret_cast<void*>(begin), end - begin, MADV_POPULATE_WRITE) == 0 || errno == EINVAL;
}
#endif

class CustomXbyakAllocator : public Xbyak::Allocator {
public:
#ifdef VENTURE_DUAL_MAPPED_CODE
    // One page before the executable view records both views for getWritable and free.
    static constexpr size_t HEADER_SIZE = 4096;
    struct Header {
        uint8_t* writable;
        size_t size;
    };

#    ifdef _WIN32
    uint8_t* alloc(size_t size) override {
        // Per-cache bound and immutable 4KiB alignment.
        if (size < 16 * 1024 * 1024 || size > 256 * 1024 * 1024 || size % 4096) {
            throw Xbyak::Error(Xbyak::ERR_CANT_ALLOC);
        }
        const size_t total = size + HEADER_SIZE;
        // Pagefile-backed and reserved: pages are committed as emission reaches them.
        HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_RESERVE,
                                            0, static_cast<DWORD>(total), nullptr);
        if (!section) {
            throw Xbyak::Error(Xbyak::ERR_CANT_ALLOC);
        }
        void* executable = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_EXECUTE, 0, 0, total);
        void* writable = executable ? MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, total) : nullptr;
        CloseHandle(section);  // The views keep the section alive.
        if (!writable || !VirtualAlloc(writable, HEADER_SIZE, MEM_COMMIT, PAGE_READWRITE)) {
            if (writable) UnmapViewOfFile(writable);
            if (executable) UnmapViewOfFile(executable);
            throw Xbyak::Error(Xbyak::ERR_CANT_ALLOC);
        }
        const Header header{static_cast<uint8_t*>(writable) + HEADER_SIZE, total};
        std::memcpy(writable, &header, sizeof(header));
        return static_cast<uint8_t*>(executable) + HEADER_SIZE;
    }

    void free(uint8_t* p) override {
        const Header header = ReadHeader(p);
        if (!UnmapViewOfFile(header.writable - HEADER_SIZE) || !UnmapViewOfFile(p - HEADER_SIZE)) {
            // Cleanup failure must not continue execution or throw while unwinding.
            RaiseFailFastException(nullptr, nullptr, 0);
            std::terminate();
        }
    }
#    else
    uint8_t* alloc(size_t size) override {
        const size_t total = size + HEADER_SIZE;
        // A memfd sealed non-executable can still be mapped executable. Hardened
        // 6.3-6.5 kernels (vm.memfd_noexec=2) refuse memfds without the seal flag;
        // kernels before 6.3 do not know the flag and fail with EINVAL.
        int fd = memfd_create("dynarmic-code", MFD_CLOEXEC | MFD_NOEXEC_SEAL);
        if (fd < 0 && errno == EINVAL) {
            fd = memfd_create("dynarmic-code", MFD_CLOEXEC);
        }
        if (fd < 0) {
            throw Xbyak::Error(Xbyak::ERR_CANT_ALLOC);
        }
        void* executable = MAP_FAILED;
        void* writable = MAP_FAILED;
        if (ftruncate(fd, static_cast<off_t>(total)) == 0) {
            executable = mmap(nullptr, total, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
            writable = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        }
        close(fd);  // The mappings keep the memory object alive.
        if (executable == MAP_FAILED || writable == MAP_FAILED || !PopulateWritable(writable, HEADER_SIZE)) {
            if (writable != MAP_FAILED) munmap(writable, total);
            if (executable != MAP_FAILED) munmap(executable, total);
            throw Xbyak::Error(Xbyak::ERR_CANT_ALLOC);
        }
        const Header header{static_cast<uint8_t*>(writable) + HEADER_SIZE, total};
        std::memcpy(writable, &header, sizeof(header));
        return static_cast<uint8_t*>(executable) + HEADER_SIZE;
    }

    void free(uint8_t* p) override {
        const Header header = ReadHeader(p);
        munmap(header.writable - HEADER_SIZE, header.size);
        munmap(p - HEADER_SIZE, header.size);
    }
#    endif

    uint8_t* getWritable(uint8_t* p) const override { return ReadHeader(p).writable; }

    bool useProtect() const override { return false; }

private:
    static Header ReadHeader(const uint8_t* p) {
        Header header;
        std::memcpy(&header, p - HEADER_SIZE, sizeof(header));
        return header;
    }
#else
    static constexpr size_t DYNARMIC_PAGE_SIZE = 4096;

    // Can't subclass Xbyak::MmapAllocator because it is not a pure interface
    // and doesn't expose its construtor
    uint8_t* alloc(size_t size) override {
        // Waste a page to store the size
        size += DYNARMIC_PAGE_SIZE;

#    if defined(MAP_ANONYMOUS)
        int mode = MAP_PRIVATE | MAP_ANONYMOUS;
#    elif defined(MAP_ANON)
        int mode = MAP_PRIVATE | MAP_ANON;
#    else
#        error "not supported"
#    endif
#    ifdef MAP_JIT
        mode |= MAP_JIT;
#    endif

        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, mode, -1, 0);
        if (p == MAP_FAILED) {
            throw Xbyak::Error(Xbyak::ERR_CANT_ALLOC);
        }
        std::memcpy(p, &size, sizeof(size_t));
        return static_cast<uint8_t*>(p) + DYNARMIC_PAGE_SIZE;
    }

    void free(uint8_t* p) override {
        size_t size;
        std::memcpy(&size, p - DYNARMIC_PAGE_SIZE, sizeof(size_t));
        munmap(p - DYNARMIC_PAGE_SIZE, size);
    }

#    ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
    bool useProtect() const override { return false; }
#    endif
#endif
};

// This is threadsafe as Xbyak::Allocator does not contain any state; it is a pure interface.
CustomXbyakAllocator s_allocator;

#if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT) && !defined(VENTURE_DUAL_MAPPED_CODE)
void ProtectMemory(const void* base, size_t size, bool is_executable) {
    static const size_t pageSize = sysconf(_SC_PAGESIZE);
    const size_t iaddr = reinterpret_cast<size_t>(base);
    const size_t roundAddr = iaddr & ~(pageSize - static_cast<size_t>(1));
    const int mode = is_executable ? (PROT_READ | PROT_EXEC) : (PROT_READ | PROT_WRITE);
    mprotect(reinterpret_cast<void*>(roundAddr), size + (iaddr - roundAddr), mode);
}
#endif

static const HostFeature features = []() {
    HostFeature f{};
#ifdef DYNARMIC_ENABLE_CPU_FEATURE_DETECTION
    using Cpu = Xbyak::util::Cpu;
    Xbyak::util::Cpu cpu_info{};
    if (cpu_info.has(Cpu::tSSSE3)) f |= HostFeature::SSSE3;
    if (cpu_info.has(Cpu::tSSE41)) f |= HostFeature::SSE41;
    if (cpu_info.has(Cpu::tSSE42)) f |= HostFeature::SSE42;
    if (cpu_info.has(Cpu::tAVX)) f |= HostFeature::AVX;
    if (cpu_info.has(Cpu::tAVX2)) f |= HostFeature::AVX2;
    if (cpu_info.has(Cpu::tAVX512F)) f |= HostFeature::AVX512F;
    if (cpu_info.has(Cpu::tAVX512CD)) f |= HostFeature::AVX512CD;
    if (cpu_info.has(Cpu::tAVX512VL)) f |= HostFeature::AVX512VL;
    if (cpu_info.has(Cpu::tAVX512BW)) f |= HostFeature::AVX512BW;
    if (cpu_info.has(Cpu::tAVX512DQ)) f |= HostFeature::AVX512DQ;
    if (cpu_info.has(Cpu::tAVX512_BITALG)) f |= HostFeature::AVX512BITALG;
    if (cpu_info.has(Cpu::tAVX512VBMI)) f |= HostFeature::AVX512VBMI;
    if (cpu_info.has(Cpu::tPCLMULQDQ)) f |= HostFeature::PCLMULQDQ;
    // Venture: VCVTPH2PS/VCVTPS2PH are VEX-encoded and need the OS-enabled AVX state (XCR0), which xbyak
    // folds into tAVX but not into tF16C.
    if (cpu_info.has(Cpu::tF16C) && cpu_info.has(Cpu::tAVX)) f |= HostFeature::F16C;
    if (cpu_info.has(Cpu::tFMA)) f |= HostFeature::FMA;
    if (cpu_info.has(Cpu::tAESNI)) f |= HostFeature::AES;
    if (cpu_info.has(Cpu::tSHA)) f |= HostFeature::SHA;
    if (cpu_info.has(Cpu::tPOPCNT)) f |= HostFeature::POPCNT;
    if (cpu_info.has(Cpu::tBMI1)) f |= HostFeature::BMI1;
    if (cpu_info.has(Cpu::tBMI2)) f |= HostFeature::BMI2;
    if (cpu_info.has(Cpu::tLZCNT)) f |= HostFeature::LZCNT;
    if (cpu_info.has(Cpu::tGFNI)) f |= HostFeature::GFNI;
    if (cpu_info.has(Cpu::tWAITPKG)) f |= HostFeature::WAITPKG;
    if (cpu_info.has(Cpu::tBMI2)) {
        // BMI2 instructions such as pdep and pext have been very slow up until Zen 3.
        // Check for Zen 3 or newer by its family (0x19).
        // See also: https://en.wikichip.org/wiki/amd/cpuid
        if (cpu_info.has(Cpu::tAMD)) {
            std::array<u32, 4> data{};
            cpu_info.getCpuid(1, data.data());
            const u32 family_base = mcl::bit::get_bits<8, 11>(data[0]);
            const u32 family_extended = mcl::bit::get_bits<20, 27>(data[0]);
            const u32 family = family_base + family_extended;
            if (family >= 0x19)
                f |= HostFeature::FastBMI2;
        } else {
            f |= HostFeature::FastBMI2;
        }
    }
    return f;
#endif
}();
HostFeature GetHostFeatures() {
    return features;
}

#ifdef __APPLE__
bool IsUnderRosetta() {
    int result = 0;
    size_t result_size = sizeof(result);
    if (sysctlbyname("sysctl.proc_translated", &result, &result_size, nullptr, 0) == -1) {
        if (errno != ENOENT)
            fmt::print("IsUnderRosetta: Failed to detect Rosetta state, assuming not under Rosetta");
        return false;
    }
    return result != 0;
}
#endif

}  // anonymous namespace

BlockOfCode::BlockOfCode(RunCodeCallbacks cb, JitStateInfo jsi, size_t total_code_size, std::function<void(BlockOfCode&)> rcp, size_t a64_register_cache_offset, u32 disabled_host_features, bool cold_region)
    : Xbyak::CodeGenerator(total_code_size
#ifdef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
        , Xbyak::DontSetProtectRWE
#else
        , nullptr //Allow RWE
#endif
        , &s_allocator)
    , jsi(jsi)
    , a64_register_cache_offset(a64_register_cache_offset)
    , disabled_host_features(disabled_host_features)
    , cb(std::move(cb))
    , constant_pool(*this, CONSTANT_POOL_SIZE)
{
    // Venture: guest 128-bit exclusives and atomics are emitted as LOCK CMPXCHG16B without a fallback, so the
    // instruction is part of the supported CPU baseline (Windows 10/11 x64 requires it as well). Check it once
    // here, for both front ends, instead of faulting on the first 128-bit atomic.
    {
        std::array<u32, 4> leaf1{};
        Xbyak::util::Cpu{}.getCpuid(1, leaf1.data());
        if (!(leaf1[2] & (1u << 13)))
            throw std::runtime_error("This CPU lacks CMPXCHG16B (CX16), which Venture's ARM translation requires.");
    }
    if (cold_region) {
        // Deferred handlers are slightly larger in total than the hot code they serve.
        cold_begin = (maxSize_ * 9 / 20) & ~size_t{0xffff};
        cold_size = cold_committed = cold_flush_begin = cold_begin;
    }
    EnableWriting();
    EnsureMemoryCommitted(PRELUDE_COMMIT_SIZE);
    GenRunCode(rcp);
}

bool BlockOfCode::HasHostFeature(HostFeature feature) const noexcept {
    HostFeature available = GetHostFeatures() & ~static_cast<HostFeature>(disabled_host_features);
    // Venture: a diagnostic mask that removes AVX also removes everything encoded with VEX/EVEX on it.
    if ((available & HostFeature::AVX) != HostFeature::AVX)
        available &= ~(HostFeature::AVX2 | HostFeature::FMA | HostFeature::F16C | HostFeature::AVX512F | HostFeature::AVX512CD
                       | HostFeature::AVX512VL | HostFeature::AVX512BW | HostFeature::AVX512DQ | HostFeature::AVX512BITALG
                       | HostFeature::AVX512VBMI);
    return (available & feature) == feature;
}

// These registers are never allocated to IR values or dispatcher temporaries.
// MOV preserves the live host flags and callback return registers.
std::optional<Xbyak::Reg64> BlockOfCode::A64CachedRegister(size_t index) const {
    if (a64_register_cache_offset == size_t(-1)) return std::nullopt;
    switch (index) {
    case 0: return r14;
    case 1: return r12;
    // RBP is reserved by the register allocator and preserved by both host ABIs.
    case 30: return rbp;
    case 8: return r13;
    // Venture: both host ABIs preserve RBX. X20 has the most block-boundary
    // traffic of the remaining registers; CMPXCHG16B borrows RBX (see
    // FlushA64CachedRbx).
    case 20: return rbx;
    // Venture: caller-saved, which holds because every call stores and reloads
    // the cache and the memory fallbacks preserve caller-saved registers.
    // X9 and X10 carry the most block-boundary traffic after the above.
    case 9: return r10;
    case 10: return r11;
#ifdef _WIN32
    // The Windows x64 ABI preserves RSI/RDI across calls; SysV passes arguments in them.
    // SP and X19 carry the most block-boundary traffic after the registers above.
    case 31: return rsi;
    case 19: return rdi;
#endif
    default: return std::nullopt;
    }
}

bool BlockOfCode::A64CacheHoldsRbx() const {
    const auto reg = A64CachedRegister(20);
    return reg && reg->getIdx() == rbx.getIdx();
}

void BlockOfCode::FlushA64CachedRbx() {
    if (A64CacheHoldsRbx())
        mov(qword[ABI_JIT_PTR + a64_register_cache_offset + sizeof(u64) * 20], rbx);
}

void BlockOfCode::ReloadA64CachedRbx() {
    if (A64CacheHoldsRbx())
        mov(rbx, qword[ABI_JIT_PTR + a64_register_cache_offset + sizeof(u64) * 20]);
}

void BlockOfCode::FlushA64RegisterCache() {
    for (const size_t index : a64_cached_indices) {
        if (const auto reg = A64CachedRegister(index))
            mov(qword[ABI_JIT_PTR + a64_register_cache_offset + sizeof(u64) * index], *reg);
    }
}

void BlockOfCode::ReloadA64RegisterCache() {
    for (const size_t index : a64_cached_indices) {
        if (const auto reg = A64CachedRegister(index))
            mov(*reg, qword[ABI_JIT_PTR + a64_register_cache_offset + sizeof(u64) * index]);
    }
}

void BlockOfCode::PreludeComplete() {
    prelude_complete = true;
    code_begin = getCurr();
    ClearCache();
    DisableWriting();
}

void BlockOfCode::EnableWriting() {
#ifdef VENTURE_DUAL_MAPPED_CODE
    ASSERT(!in_cold);
    writing = true;
    flush_begin = flush_end = size_;
    cold_flush_begin = cold_size;
#elif defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
    ProtectMemory(getCode(), maxSize_, false);
#endif
}

void BlockOfCode::DisableWriting() {
#ifdef VENTURE_DUAL_MAPPED_CODE
    // Instructions were stored through the writable alias: the appended code
    // and any links patched in older blocks. Synchronize them with instruction
    // fetch from the executable view before they run.
    flush_end = std::max(flush_end, size_);
    const auto flush = [this](size_t begin, size_t end) {
        if (begin >= end)
            return;
#    ifdef _WIN32
        if (!FlushInstructionCache(GetCurrentProcess(), getCode() + begin, end - begin)) {
            // DisableWriting also runs during scope-exit cleanup; do not throw.
            RaiseFailFastException(nullptr, nullptr, 0);
            std::terminate();
        }
#    else
        __builtin___clear_cache(reinterpret_cast<char*>(top_ + begin), reinterpret_cast<char*>(top_ + end));
#    endif
    };
    flush(flush_begin, flush_end);
    if (cold_begin)
        flush(cold_flush_begin, cold_size);
    flush(arena_flush_begin, arena_flush_end);
    arena_flush_begin = arena_flush_end = 0;
    flush_begin = flush_end = 0;
    cold_flush_begin = cold_size;
    writing = false;
#elif defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT)
    ProtectMemory(getCode(), maxSize_, true);
#endif
}

void BlockOfCode::ClearCache() {
    ASSERT(prelude_complete);
    if (reset_native_unwind)
        reset_native_unwind(static_cast<const u8*>(code_begin) - getCode());
    // A block whose emission threw may have left the cold region current.
    in_cold = false;
    SetCodePtr(code_begin);
    if (cold_begin)
        cold_size = cold_flush_begin = cold_begin;
}

void BlockOfCode::RegisterNativeUnwindFrame([[maybe_unused]] const void* begin,
        [[maybe_unused]] const void* end, [[maybe_unused]] const ABIPrologue& prologue, [[maybe_unused]] bool inherits_run_frame) {
    // Unwind entries are appended in address order; the cold region lies above them all.
    ASSERT(!in_cold);
#ifdef _WIN32
    if (native_unwind_registrar) native_unwind_registrar(begin, end, prologue, inherits_run_frame);
    else native_unwind_frames.push_back({begin, end, prologue, inherits_run_frame});
#endif
}

void BlockOfCode::SetNativeUnwindCallbacks(NativeUnwindRegistrar registrar, std::function<void(size_t)> reset) {
    native_unwind_registrar = std::move(registrar);
    reset_native_unwind = std::move(reset);
    std::vector<NativeUnwindFrame>().swap(native_unwind_frames);
}

size_t BlockOfCode::SpaceRemaining() const {
    ASSERT(prelude_complete);
    if (cold_begin) {
        const size_t hot = in_cold ? hot_size : size_, cold = in_cold ? size_ : cold_size;
        return std::min(hot < cold_begin ? cold_begin - hot : 0, cold < maxSize_ ? maxSize_ - cold : 0);
    }
    const u8* current_ptr = getCurr<const u8*>();
    if (current_ptr >= &top_[maxSize_])
        return 0;
    return &top_[maxSize_] - current_ptr;
}

void BlockOfCode::EnsureMemoryCommitted() {
#ifdef VENTURE_DUAL_MAPPED_CODE
    // GetBlock's 1 MiB reserve bounds an individual emitter, not a whole block.
    // Refill between IR operations, terminal emission and deferred callbacks.
    // Also used while the fallback prelude is being generated, before
    // PreludeComplete makes SpaceRemaining available.
    const size_t end = RegionEnd();
    EnsureMemoryCommitted(size_ <= end ? std::min<size_t>(1024 * 1024, end - size_) : 0);
#endif
}

void BlockOfCode::EnsureMemoryCommitted([[maybe_unused]] size_t codesize) {
#ifdef VENTURE_DUAL_MAPPED_CODE
    if (in_arena)
        return;  // BeginThunkArena committed this thunk's pages.
    const size_t end = RegionEnd();
    if (size_ > end || codesize > end - size_) {
        throw Xbyak::Error(Xbyak::ERR_CODE_IS_TOO_BIG);
    }
    const size_t needed = size_ + codesize;
    size_t& committed = in_cold ? cold_committed : committed_size;
    if (committed < needed) {
#    ifndef DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
#        error Venture requires DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT
#    endif
        const size_t proposed = std::min<size_t>(end, (needed + 4095) & ~size_t{4095});
        // Skip the reserved constant-pool hole; its pages commit as constants are added.
        const size_t begin = std::max(committed, size_ & ~size_t{4095});
        CommitCodeSpace(getCode() + begin, proposed - begin);
        committed = proposed;
    }
#endif
}

void BlockOfCode::CommitCodeSpace([[maybe_unused]] const void* begin, [[maybe_unused]] size_t size) {
#ifdef _WIN32
    // Committing a page of the section through the writable view commits it for
    // both views; the executable view sees it as PAGE_EXECUTE_READ. Repeating a
    // commit is harmless.
    if (size && !VirtualAlloc(GetWritablePointer(begin), size, MEM_COMMIT, PAGE_READWRITE)) {
        throw Xbyak::Error(Xbyak::ERR_CANT_ALLOC);
    }
#elif defined(__linux__)
    if (size && !PopulateWritable(GetWritablePointer(begin), size)) {
        throw Xbyak::Error(Xbyak::ERR_CANT_ALLOC);
    }
#endif
}

HaltReason BlockOfCode::RunCode(void* jit_state, CodePtr code_ptr) const {
    return run_code(jit_state, code_ptr);
}

HaltReason BlockOfCode::StepCode(void* jit_state, CodePtr code_ptr) const {
    return step_code(jit_state, code_ptr);
}

void BlockOfCode::ReturnFromRunCode(bool mxcsr_already_exited) {
    size_t index = 0;
    if (mxcsr_already_exited)
        index |= MXCSR_ALREADY_EXITED;
    jmp(return_from_run_code[index]);
}

void BlockOfCode::ForceReturnFromRunCode(bool mxcsr_already_exited) {
    size_t index = FORCE_RETURN;
    if (mxcsr_already_exited)
        index |= MXCSR_ALREADY_EXITED;
    jmp(return_from_run_code[index]);
}

void BlockOfCode::GenRunCode(std::function<void(BlockOfCode&)> rcp) {
    Xbyak::Label return_to_caller, return_to_caller_mxcsr_already_exited;

    align();
    run_code = getCurr<RunCodeFuncType>();

    // This serves two purposes:
    // 1. It saves all the registers we as a callee need to save.
    // 2. It aligns the stack so that the code the JIT emits can assume
    //    that the stack is appropriately aligned for CALLs.
    ABI_PushCalleeSaveRegistersAndAdjustStack(*this, sizeof(StackLayout), &run_prologue);

    mov(ABI_JIT_PTR, ABI_PARAM1);
    ReloadA64RegisterCache();
    mov(r8, ABI_PARAM2);

    if (cb.enable_cycle_counting) {
        mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, dispatch_scratch)], r8);
        cb.GetTicksRemaining.EmitCall(*this);
        mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)], ABI_RETURN);
        mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], ABI_RETURN);
        mov(r8, qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, dispatch_scratch)]);
    }

    // r14 = page table
    // r13 = fastmem pointer
    rcp(*this);

    cmp(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], 0);
    jne(return_to_caller_mxcsr_already_exited, T_NEAR);

    SwitchMxcsrOnEntry();
    jmp(r8);

    align();
    step_code = getCurr<RunCodeFuncType>();

    ABI_PushCalleeSaveRegistersAndAdjustStack(*this, sizeof(StackLayout));

    mov(ABI_JIT_PTR, ABI_PARAM1);
    ReloadA64RegisterCache();

    if (cb.enable_cycle_counting) {
        mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)], 1);
        mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], 1);
    }

    rcp(*this);

    cmp(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], 0);
    jne(return_to_caller_mxcsr_already_exited, T_NEAR);
    lock(); or_(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], u32(HaltReason::Step));

    SwitchMxcsrOnEntry();
    jmp(ABI_PARAM2);

    // Dispatcher loop

    align();
    return_from_run_code[0] = getCurr<const void*>();

    cmp(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], 0);
    jne(return_to_caller, T_NEAR);
    if (cb.enable_cycle_counting) {
        cmp(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], 0);
        jng(return_to_caller, T_NEAR);
    }
    cb.LookupBlock.EmitCall(*this);
    jmp(ABI_RETURN);

    align();
    return_from_run_code[MXCSR_ALREADY_EXITED] = getCurr<const void*>();

    cmp(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], 0);
    jne(return_to_caller_mxcsr_already_exited, T_NEAR);
    if (cb.enable_cycle_counting) {
        cmp(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], 0);
        jng(return_to_caller_mxcsr_already_exited, T_NEAR);
    }
    SwitchMxcsrOnEntry();
    cb.LookupBlock.EmitCall(*this);
    jmp(ABI_RETURN);

    align();
    return_from_run_code[FORCE_RETURN] = getCurr<const void*>();
    L(return_to_caller);

    SwitchMxcsrOnExit();
    // fallthrough

    return_from_run_code[MXCSR_ALREADY_EXITED | FORCE_RETURN] = getCurr<const void*>();
    L(return_to_caller_mxcsr_already_exited);
    FlushA64RegisterCache();

    if (cb.enable_cycle_counting) {
        cb.AddTicks.EmitCall(*this, [this](RegList param) {
            mov(param[0], qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)]);
            sub(param[0], qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)]);
        });
    }

    xor_(eax, eax);
    xchg(dword[ABI_JIT_PTR + jsi.offsetof_halt_reason], eax);

    ABI_PopCalleeSaveRegistersAndAdjustStack(*this, sizeof(StackLayout));
    ret();

    PerfMapRegister(run_code, getCurr(), "dynarmic_dispatcher");
}

void BlockOfCode::SwitchMxcsrOnEntry() {
    stmxcsr(dword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, save_host_MXCSR)]);
    ldmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_guest_MXCSR]);
}

void BlockOfCode::SwitchMxcsrOnExit() {
    stmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_guest_MXCSR]);
    ldmxcsr(dword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, save_host_MXCSR)]);
}

void BlockOfCode::EnterStandardASIMD() {
    stmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_guest_MXCSR]);
    ldmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_asimd_MXCSR]);
}

void BlockOfCode::LeaveStandardASIMD() {
    stmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_asimd_MXCSR]);
    ldmxcsr(dword[ABI_JIT_PTR + jsi.offsetof_guest_MXCSR]);
}

void BlockOfCode::UpdateTicks() {
    if (!cb.enable_cycle_counting) {
        return;
    }

    cb.AddTicks.EmitCall(*this, [this](RegList param) {
        mov(param[0], qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)]);
        sub(param[0], qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)]);
    });

    cb.GetTicksRemaining.EmitCall(*this);
    mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_to_run)], ABI_RETURN);
    mov(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], ABI_RETURN);
}

void BlockOfCode::LookupBlock() {
    cb.LookupBlock.EmitCall(*this);
}

void BlockOfCode::LoadRequiredFlagsForCondFromRax(IR::Cond cond) {
#ifdef __APPLE__
    static const bool is_rosetta = IsUnderRosetta();
#endif

    // sahf restores SF, ZF, CF
    // add al, 0x7F restores OF

    switch (cond) {
    case IR::Cond::EQ:  // z
    case IR::Cond::NE:  // !z
    case IR::Cond::CS:  // c
    case IR::Cond::CC:  // !c
    case IR::Cond::MI:  // n
    case IR::Cond::PL:  // !n
        sahf();
        break;
    case IR::Cond::VS:  // v
    case IR::Cond::VC:  // !v
        cmp(al, 0x81);
        break;
    case IR::Cond::HI:  // c & !z
    case IR::Cond::LS:  // !c | z
        sahf();
        cmc();
        break;
    case IR::Cond::GE:  // n == v
    case IR::Cond::LT:  // n != v
    case IR::Cond::GT:  // !z & (n == v)
    case IR::Cond::LE:  // z | (n != v)
#ifdef __APPLE__
        if (is_rosetta) {
            shl(al, 3);
            xchg(al, ah);
            push(rax);
            popf();
            break;
        }
#endif
        cmp(al, 0x81);
        sahf();
        break;
    case IR::Cond::AL:
    case IR::Cond::NV:
        break;
    default:
        UNREACHABLE();
    }
}

Xbyak::Address BlockOfCode::Const(const Xbyak::AddressFrame& frame, u64 lower, u64 upper) {
    return constant_pool.GetConstant(*this, frame, lower, upper);
}

CodePtr BlockOfCode::GetCodeBegin() const {
    return code_begin;
}

size_t BlockOfCode::GetTotalCodeSize() const {
    return maxSize_;
}

// VENTURE_DEMAND_CONSTANT_POOL_V1: preserve its address and full capacity.
// Fresh shared memory is zero; pool pages are committed as constants arrive.
void* BlockOfCode::ReserveFromCodeSpace(size_t alloc_size) {
#ifdef VENTURE_DUAL_MAPPED_CODE
    if (size_ > maxSize_ || alloc_size > maxSize_ - size_)
        throw Xbyak::Error(Xbyak::ERR_CODE_IS_TOO_BIG);
    void* ret = getCurr<void*>();
    size_ += alloc_size;
    return ret;
#else
    return AllocateFromCodeSpace(alloc_size);
#endif
}

void* BlockOfCode::AllocateFromCodeSpace(size_t alloc_size) {
    if (size_ + alloc_size >= RegionEnd()) {
        using Xbyak::Error;
        XBYAK_THROW(Xbyak::ERR_CODE_IS_TOO_BIG);
    }

    EnsureMemoryCommitted(alloc_size);

    void* ret = getCurr<void*>();
    size_ += alloc_size;
    memset(GetWritablePointer(ret), 0, alloc_size);
    return ret;
}

void BlockOfCode::SetCodePtr(CodePtr code_ptr) {
    // The "size" defines where top_, the insertion point, is.
    size_t required_size = reinterpret_cast<const u8*>(code_ptr) - getCode();
    setSize(required_size);
#ifdef VENTURE_DUAL_MAPPED_CODE
    // Link/unlink patches rewrite older blocks; DisableWriting flushes them.
    // All x64 patches are at most 23 bytes (A32/A64 EmitPatch*).
    if (writing) {
        flush_begin = std::min(flush_begin, required_size);
        flush_end = std::max(flush_end, std::min(required_size + 64, maxSize_));
    }
#endif
}

void BlockOfCode::ReserveThunkArena(size_t size) {
    ASSERT(!prelude_complete && !arena_end && size % 4096 == 0);
    align(4096);
    arena_begin = arena_size = arena_committed = size_;
    // Uncommitted like the constant pool's hole: later prelude commits skip it.
    ReserveFromCodeSpace(size);
    arena_end = arena_begin + size;
    // The rest of the prelude (terminal handlers) continues above the hole; the
    // first compile commits more on the JIT's own thread.
    EnsureMemoryCommitted(std::min<size_t>(64 * 1024, RegionEnd() - size_));
}

void BlockOfCode::BeginThunkArena() {
    ASSERT(prelude_complete && arena_end && !in_arena);
    arena_saved_size = size_;
    arena_saved_cold = in_cold;
    // Thunks may register unwind frames, which the cold region never has.
    in_cold = false;
    in_arena = true;
    setSize(arena_size);
    // One thunk is well under 4 KiB; commit a page beyond the insertion point.
    const size_t want = std::min(arena_end, ((arena_size + 4096) + 4095) & ~size_t{4095});
    if (arena_committed < want) {
        CommitCodeSpace(getCode() + arena_committed, want - arena_committed);
        arena_committed = want;
    }
}

void BlockOfCode::EndThunkArena() {
    ASSERT(in_arena);
    if (size_ > arena_committed) {
        // Xbyak wrote past the committed part; never continue with that code.
        std::terminate();
    }
    arena_flush_begin = arena_flush_end == 0 ? arena_size : std::min(arena_flush_begin, arena_size);
    arena_flush_end = std::max(arena_flush_end, size_);
    arena_size = size_;
    in_arena = false;
    in_cold = arena_saved_cold;
    setSize(arena_saved_size);
}

void BlockOfCode::SwitchToColdCode() {
    ASSERT(cold_begin && !in_cold);
    hot_size = size_;
    in_cold = true;
    setSize(cold_size);
}

void BlockOfCode::SwitchToHotCode() {
    ASSERT(in_cold);
    cold_size = size_;
    in_cold = false;
    setSize(hot_size);
}

void BlockOfCode::EnsurePatchLocationSize(CodePtr begin, size_t size) {
    size_t current_size = getCurr<const u8*>() - reinterpret_cast<const u8*>(begin);
    ASSERT(current_size <= size);
    nop(size - current_size);
}

}  // namespace Dynarmic::Backend::X64
