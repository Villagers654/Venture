// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <algorithm>
#include <cstdio>

#include <bit>

#include "dynarmic/backend/perf_map.h"
#include "dynarmic/interface/profiling.h"
#include "dynarmic/backend/arm64/a64_address_space.h"
#include "dynarmic/backend/arm64/a64_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/devirtualize.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/stack_layout.h"
#include "dynarmic/common/cast_util.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/llvm_disassemble.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/ir/location_descriptor.h"

#ifdef _WIN32
#    include <cstdlib>
#    include <cstring>
#else
#    include <sys/mman.h>
#endif

namespace Dynarmic::Backend::Arm64 {

AddressSpace::FastDispatchTable::~FastDispatchTable() {
    if (!entries) {
        return;
    }
#ifdef _WIN32
    std::free(entries);
#else
    munmap(entries, count * sizeof(FastDispatchEntry));
#endif
}

void AddressSpace::FastDispatchTable::Allocate(std::size_t entry_count) {
    ASSERT(!entries && entry_count > 0);
    count = entry_count;
#ifdef _WIN32
    entries = static_cast<FastDispatchEntry*>(std::calloc(count, sizeof(FastDispatchEntry)));
    ASSERT(entries);
#else
    void* const pages = mmap(nullptr, count * sizeof(FastDispatchEntry), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT(pages != MAP_FAILED);
    entries = static_cast<FastDispatchEntry*>(pages);
#endif
}

void AddressSpace::FastDispatchTable::Clear() {
    if (!entries) {
        return;
    }
#ifdef _WIN32
    std::memset(entries, 0, count * sizeof(FastDispatchEntry));
#else
    // Private anonymous pages read as zero again after MADV_DONTNEED.
    if (madvise(entries, count * sizeof(FastDispatchEntry), MADV_DONTNEED) != 0) {
        std::fill_n(entries, count, FastDispatchEntry{0, nullptr});
    }
#endif
}

AddressSpace::AddressSpace(std::size_t code_cache_size)
    : ir_block{IR::LocationDescriptor{0}}
    , code_cache_size(code_cache_size)
    , mem(code_cache_size)
    , code(mem.ptr(), mem.ptr())
    , fastmem_manager(exception_handler)
{
    ASSERT(code_cache_size <= 128 * 1024 * 1024 && "code_cache_size > 128 MiB not currently supported");

    exception_handler.Register(mem, code_cache_size);
    exception_handler.SetFastmemCallback([this](u64 host_pc) {
        return FastmemCallback(host_pc);
    });
}

AddressSpace::~AddressSpace() = default;

CodePtr AddressSpace::Get(IR::LocationDescriptor descriptor) {
    if (const auto iter = block_entries.find(descriptor); iter != block_entries.end()) {
        return iter->second;
    }
    return nullptr;
}

std::optional<IR::LocationDescriptor> AddressSpace::ReverseGetLocation(CodePtr host_pc) {
    if (auto iter = reverse_block_entries.upper_bound(host_pc); iter != reverse_block_entries.begin()) {
        // upper_bound locates the first value greater than host_pc, so we need to decrement
        --iter;
        return iter->second;
    }
    return std::nullopt;
}

CodePtr AddressSpace::ReverseGetEntryPoint(CodePtr host_pc) {
    if (auto iter = reverse_block_entries.upper_bound(host_pc); iter != reverse_block_entries.begin()) {
        // upper_bound locates the first value greater than host_pc, so we need to decrement
        --iter;
        return iter->first;
    }
    return nullptr;
}

CodePtr AddressSpace::GetOrEmit(IR::LocationDescriptor descriptor) {
    if (CodePtr block_entry = Get(descriptor)) {
        return block_entry;
    }
    CompileTimingScope timing{DescriptorPC(descriptor), IsA32(), descriptor.Value()};
    SetCompilePhase(CompilePhase::Decode);
    GenerateIR(ir_block, descriptor);
    SetCompilePhase(CompilePhase::Emit);
    const EmittedBlockInfo block_info = Emit(std::move(ir_block));
    SetCompilePhase(CompilePhase::Finalize);
    timing.Complete();
    return block_info.entry_point;
}

void AddressSpace::InvalidateBasicBlocks(const ::Common::unordered_set<IR::LocationDescriptor>& descriptors) {
    UnprotectCodeMemory();

    for (const auto& descriptor : descriptors) {
        const auto iter = block_entries.find(descriptor);
        if (iter == block_entries.end()) {
            continue;
        }

        // Unlink before removal because InvalidateBasicBlocks can be called within a fastmem callback,
        // and the currently executing block may have references to itself which need to be unlinked.
        RelinkForDescriptor(descriptor, nullptr);

        if (!fast_dispatch_table.empty()) {
            FastDispatchEntry& entry = fast_dispatch_table[FastDispatchIndex(descriptor.Value())];
            if (entry.location == ~descriptor.Value()) {
                entry = {0, nullptr};
            }
        }

        block_entries.erase(iter);
    }
    // Sites keep only a PC and code: any invalidation empties them all (x64).
    ResetDispatchSites();

    ProtectCodeMemory();
}

void AddressSpace::ResetDispatchSites() {
    // An empty slot's ~0 tag matches only a branch to PC ~0, which the miss
    // handler dispatches without caching.
    std::fill(dispatch_sites.begin(), dispatch_sites.end(),
              FastDispatchEntry{~u64{0}, static_cast<CodePtr>(prelude_info.dispatch_site_miss)});
}

void AddressSpace::ClearCache() {
    fast_dispatch_table.Clear();
    ResetDispatchSites();
    next_dispatch_site = 0;
    block_entries.clear();
    reverse_block_entries.clear();
    block_infos.clear();
    block_references.clear();
    ClearBlockRanges();
    code.set_offset(prelude_info.end_of_prelude);
    // Venture: a clear inside Run (a compile that found the cache full) leaves
    // the return stack buffer, which lives on the run's stack, pointing into
    // code that is about to be overwritten; the dispatcher resets it (as x64
    // resets its RSB on every cache clear).
    rsb_reset_pending = 1;
    PerfMapClear();
}

void AddressSpace::EmitRSBResetAfterCompile(oaknut::Label& l_return_to_dispatcher) {
    using namespace oaknut::util;

    oaknut::Label done;
    code.MOV(Xscratch0, std::bit_cast<u64>(&rsb_reset_pending));
    code.LDRB(Wscratch1, Xscratch0);
    code.CBZ(Wscratch1, done);
    code.STRB(WZR, Xscratch0);
    code.LDR(Xscratch0, l_return_to_dispatcher);
    for (std::size_t i = 0; i < RSBEntries(!IsA32()); i++) {
        code.STR(Xscratch0, SP, offsetof(StackLayout, rsb) + offsetof(RSBEntry, code_ptr) + i * sizeof(RSBEntry));
    }
    code.l(done);
}

CodePtr AddressSpace::FastDispatchFill(IR::LocationDescriptor descriptor) {
    const CodePtr code_ptr = GetOrEmit(descriptor);
    // GetOrEmit may have cleared the cache; code_ptr is valid either way.
    fast_dispatch_table[FastDispatchIndex(descriptor.Value())] = {~descriptor.Value(), code_ptr};
    return code_ptr;
}

void AddressSpace::EmitFastDispatchProbe(oaknut::Label& l_return_to_dispatcher, bool reset_rsb, u64* census, oaknut::Label* found) {
    using namespace oaknut::util;

    oaknut::Label miss, l_this, l_fill, l_table;
    // FastDispatchIndex(X0) * sizeof(FastDispatchEntry)
    code.EOR(Xscratch0, X0, X0, LSR, 16);
    code.EOR(Xscratch0, Xscratch0, X0, LSR, 32);
    code.UBFX(Xscratch0, Xscratch0, 1, fast_dispatch_bits);
    code.LDR(Xscratch1, l_table);
    code.ADD(Xscratch1, Xscratch1, Xscratch0, LSL, 4);
    code.LDP(Xscratch0, Xscratch1, Xscratch1);
    // The tag is ~location: tag XNOR location is zero on a hit.
    code.EON(Xscratch0, Xscratch0, X0);
    code.CBNZ(Xscratch0, miss);
    if (found) {
        code.B(*found);
    } else {
        code.BR(Xscratch1);
    }

    code.l(miss);
    if (census) {
        code.MOV(Xscratch0, std::bit_cast<u64>(census + 3));
        code.LDR(Xscratch1, Xscratch0);
        code.ADD(Xscratch1, Xscratch1, 1);
        code.STR(Xscratch1, Xscratch0);
    }
    code.MOV(X1, X0);
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_fill);
    code.BLR(Xscratch0);
    if (reset_rsb) {
        EmitRSBResetAfterCompile(l_return_to_dispatcher);
    }
    if (found) {
        code.MOV(Xscratch1, X0);
        code.B(*found);
    } else {
        code.BR(X0);
    }

    const auto fill = [](AddressSpace& self, u64 location) -> CodePtr {
        return self.FastDispatchFill(IR::LocationDescriptor{location});
    };
    code.align(8);
    code.l(l_this);
    code.dx(std::bit_cast<u64>(this));
    code.l(l_fill);
    code.dx(std::bit_cast<u64>(static_cast<CodePtr (*)(AddressSpace&, u64)>(fill)));
    code.l(l_table);
    code.dx(std::bit_cast<u64>(fast_dispatch_table.data()));
}

bool AddressSpace::Precompile(IR::LocationDescriptor descriptor, std::size_t min_space) {
    if (Get(descriptor)) {
        return true;
    }
    if (GetRemainingSize() < min_space) {
        return false;
    }
    GetOrEmit(descriptor);
    return true;
}

void AddressSpace::ReserveBlocks(std::size_t count) {
    block_entries.reserve(block_entries.size() + count);
    block_infos.reserve(block_infos.size() + count);
}

std::size_t AddressSpace::GetRemainingSize() {
    return code_cache_size - static_cast<std::size_t>(code.offset());
}

EmittedBlockInfo AddressSpace::Emit(IR::Block block) {
    if (GetRemainingSize() < 1024 * 1024) {
        ClearCache();
    }

    UnprotectCodeMemory();

    EmittedBlockInfo block_info = EmitArm64(code, std::move(block), GetEmitConfig(), fastmem_manager);

    ASSERT(block_entries.insert({block.Location(), block_info.entry_point}).second);
    ASSERT(reverse_block_entries.insert({block_info.entry_point, block.Location()}).second);
    ASSERT(block_infos.insert({block_info.entry_point, block_info}).second);

    Link(block_info);
    RelinkForDescriptor(block.Location(), block_info.entry_point);

    mem.invalidate(reinterpret_cast<u32*>(block_info.entry_point), block_info.size);
    ProtectCodeMemory();

    RegisterNewBasicBlock(block, block_info);
    SetCompilePhase(CompilePhase::Registration);
    if (PerfMapActive())
        PerfMapRegister(block_info.entry_point, block_info.entry_point + block_info.size, FriendlyName(block.Location()));

    return block_info;
}

void AddressSpace::Link(EmittedBlockInfo& block_info) {
    using namespace oaknut;
    using namespace oaknut::util;

    for (auto [ptr_offset, target] : block_info.relocations) {
        CodeGenerator c{mem.ptr(), mem.ptr()};
        c.set_xptr(reinterpret_cast<u32*>(block_info.entry_point + ptr_offset));

        switch (target) {
        case LinkTarget::ReturnToDispatcher:
            c.B(prelude_info.return_to_dispatcher);
            break;
        case LinkTarget::ReturnFromRunCode:
            c.B(prelude_info.return_from_run_code);
            break;
        case LinkTarget::FastDispatch:
            c.B(prelude_info.fast_dispatch);
            break;
        case LinkTarget::DispatchSiteMiss:
            c.B(prelude_info.dispatch_site_miss);
            break;
        // { this, vaddr, size }
        case LinkTarget::ReadMemory8:
            c.BL(prelude_info.read_memory_8);
            break;
        case LinkTarget::ReadMemory16:
            c.BL(prelude_info.read_memory_16);
            break;
        case LinkTarget::ReadMemory32:
            c.BL(prelude_info.read_memory_32);
            break;
        case LinkTarget::ReadMemory64:
            c.BL(prelude_info.read_memory_64);
            break;
        case LinkTarget::ReadMemory128:
            c.BL(prelude_info.read_memory_128);
            break;
        // { this, vaddr, size }
        case LinkTarget::WrappedReadMemory8:
            c.BL(prelude_info.wrapped_read_memory_8);
            break;
        case LinkTarget::WrappedReadMemory16:
            c.BL(prelude_info.wrapped_read_memory_16);
            break;
        case LinkTarget::WrappedReadMemory32:
            c.BL(prelude_info.wrapped_read_memory_32);
            break;
        case LinkTarget::WrappedReadMemory64:
            c.BL(prelude_info.wrapped_read_memory_64);
            break;
        case LinkTarget::WrappedReadMemory128:
            c.BL(prelude_info.wrapped_read_memory_128);
            break;
        //
        case LinkTarget::ExclusiveReadMemory8:
            c.BL(prelude_info.exclusive_read_memory_8);
            break;
        case LinkTarget::ExclusiveReadMemory16:
            c.BL(prelude_info.exclusive_read_memory_16);
            break;
        case LinkTarget::ExclusiveReadMemory32:
            c.BL(prelude_info.exclusive_read_memory_32);
            break;
        case LinkTarget::ExclusiveReadMemory64:
            c.BL(prelude_info.exclusive_read_memory_64);
            break;
        case LinkTarget::ExclusiveReadMemory128:
            c.BL(prelude_info.exclusive_read_memory_128);
            break;
        // { this, vaddr, value, size }
        case LinkTarget::WriteMemory8:
            c.BL(prelude_info.write_memory_8);
            break;
        case LinkTarget::WriteMemory16:
            c.BL(prelude_info.write_memory_16);
            break;
        case LinkTarget::WriteMemory32:
            c.BL(prelude_info.write_memory_32);
            break;
        case LinkTarget::WriteMemory64:
            c.BL(prelude_info.write_memory_64);
            break;
        case LinkTarget::WriteMemory128:
            c.BL(prelude_info.write_memory_128);
            break;
        //
        case LinkTarget::WrappedWriteMemory8:
            c.BL(prelude_info.wrapped_write_memory_8);
            break;
        case LinkTarget::WrappedWriteMemory16:
            c.BL(prelude_info.wrapped_write_memory_16);
            break;
        case LinkTarget::WrappedWriteMemory32:
            c.BL(prelude_info.wrapped_write_memory_32);
            break;
        case LinkTarget::WrappedWriteMemory64:
            c.BL(prelude_info.wrapped_write_memory_64);
            break;
        case LinkTarget::WrappedWriteMemory128:
            c.BL(prelude_info.wrapped_write_memory_128);
            break;
        //
        case LinkTarget::ExclusiveWriteMemory8:
            c.BL(prelude_info.exclusive_write_memory_8);
            break;
        case LinkTarget::ExclusiveWriteMemory16:
            c.BL(prelude_info.exclusive_write_memory_16);
            break;
        case LinkTarget::ExclusiveWriteMemory32:
            c.BL(prelude_info.exclusive_write_memory_32);
            break;
        case LinkTarget::ExclusiveWriteMemory64:
            c.BL(prelude_info.exclusive_write_memory_64);
            break;
        case LinkTarget::ExclusiveWriteMemory128:
            c.BL(prelude_info.exclusive_write_memory_128);
            break;
        case LinkTarget::AtomicMemory:
            c.BL(prelude_info.atomic_memory);
            break;
        case LinkTarget::WrappedAtomicMemory:
            c.BL(prelude_info.wrapped_atomic_memory);
            break;
        case LinkTarget::AtomicCompareAndSwap128:
            c.BL(prelude_info.atomic_compare_and_swap_128);
            break;
        case LinkTarget::InlineExclusiveWriteFallback8:
            c.BL(prelude_info.inline_exclusive_write_fallback_8);
            break;
        case LinkTarget::InlineExclusiveWriteFallback16:
            c.BL(prelude_info.inline_exclusive_write_fallback_16);
            break;
        case LinkTarget::InlineExclusiveWriteFallback32:
            c.BL(prelude_info.inline_exclusive_write_fallback_32);
            break;
        case LinkTarget::InlineExclusiveWriteFallback64:
            c.BL(prelude_info.inline_exclusive_write_fallback_64);
            break;
        case LinkTarget::InlineExclusiveWriteFallback128:
            c.BL(prelude_info.inline_exclusive_write_fallback_128);
            break;
        case LinkTarget::CallSVC:
            c.BL(prelude_info.call_svc);
            break;
        case LinkTarget::CallSVCFast:
            c.BL(prelude_info.call_svc_fast);
            break;
        case LinkTarget::CallSVCFastKnown:
            c.BL(prelude_info.call_svc_fast_known);
            break;
        case LinkTarget::ExceptionRaised:
            c.BL(prelude_info.exception_raised);
            break;
        case LinkTarget::InstructionSynchronizationBarrierRaised:
            c.BL(prelude_info.isb_raised);
            break;
        case LinkTarget::InstructionCacheOperationRaised:
            c.BL(prelude_info.ic_raised);
            break;
        case LinkTarget::DataCacheOperationRaised:
            c.BL(prelude_info.dc_raised);
            break;
        case LinkTarget::GetCNTPCT:
            c.BL(prelude_info.get_cntpct);
            break;
        case LinkTarget::AddTicks:
            c.BL(prelude_info.add_ticks);
            break;
        case LinkTarget::GetTicksRemaining:
            c.BL(prelude_info.get_ticks_remaining);
            break;
        default:
            UNREACHABLE();
        }
    }

    for (auto [target_descriptor, list] : block_info.block_relocations) {
        block_references[target_descriptor].insert(block_info.entry_point);
        LinkBlockLinks(block_info.entry_point, Get(target_descriptor), list);
    }
}

void AddressSpace::LinkBlockLinks(const CodePtr entry_point, const CodePtr target_ptr, const std::vector<BlockRelocation>& block_relocations_list) {
    using namespace oaknut;
    using namespace oaknut::util;

    for (auto [ptr_offset, type, unlinked_offset] : block_relocations_list) {
        CodeGenerator c{mem.ptr(), mem.ptr()};
        c.set_xptr(reinterpret_cast<u32*>(entry_point + ptr_offset));

        switch (type) {
        case BlockRelocationType::Branch:
            if (target_ptr) {
                c.B((void*)target_ptr);
            } else if (unlinked_offset) {
                c.B((void*)(entry_point + unlinked_offset));
            } else {
                c.NOP();
            }
            break;
        case BlockRelocationType::MoveToScratch1:
            if (target_ptr) {
                c.ADRL(Xscratch1, (void*)target_ptr);
            } else {
                c.ADRL(Xscratch1, prelude_info.return_to_dispatcher);
            }
            break;
        default:
            UNREACHABLE();
        }
    }
}

void AddressSpace::RelinkForDescriptor(IR::LocationDescriptor target_descriptor, CodePtr target_ptr) {
    for (auto code_ptr : block_references[target_descriptor]) {
        if (auto block_iter = block_infos.find(code_ptr); block_iter != block_infos.end()) {
            const EmittedBlockInfo& block_info = block_iter->second;

            if (auto relocation_iter = block_info.block_relocations.find(target_descriptor); relocation_iter != block_info.block_relocations.end()) {
                LinkBlockLinks(block_info.entry_point, target_ptr, relocation_iter->second);
            }

            mem.invalidate(reinterpret_cast<u32*>(block_info.entry_point), block_info.size);
        }
    }
}

FakeCall AddressSpace::FastmemCallback(u64 host_pc) {
    {
        const auto host_ptr = std::bit_cast<CodePtr>(host_pc);

        const auto entry_point = ReverseGetEntryPoint(host_ptr);
        if (!entry_point) {
            goto fail;
        }

        const auto block_info = block_infos.find(entry_point);
        if (block_info == block_infos.end()) {
            goto fail;
        }

        const auto patch_entry = block_info->second.fastmem_patch_info.find(host_ptr - entry_point);
        if (patch_entry == block_info->second.fastmem_patch_info.end()) {
            goto fail;
        }

        const auto fc = patch_entry->second.fc;

        if (patch_entry->second.recompile) {
            const auto marker = patch_entry->second.marker;
            fastmem_manager.MarkDoNotFastmem(marker);
            InvalidateBasicBlocks({std::get<0>(marker)});
        }

        return fc;
    }

fail:
    fmt::print("dynarmic: Segfault happened within JITted code at host_pc = {:016x}\n"
        "Segfault wasn't at a fastmem patch location!\n", host_pc);
    UNREACHABLE();
}

}  // namespace Dynarmic::Backend::Arm64
