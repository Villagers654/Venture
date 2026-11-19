// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <map>
#include <vector>
#include <optional>

#include "common/common_types.h"
#include <oaknut/code_block.hpp>
#include <oaknut/oaknut.hpp>
#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"

#include <string>
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/fastmem.h"
#include "dynarmic/interface/halt_reason.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend::Arm64 {

class AddressSpace {
public:
    explicit AddressSpace(std::size_t code_cache_size);
    virtual ~AddressSpace();

    virtual void GenerateIR(IR::Block& ir_block, IR::LocationDescriptor) const = 0;

    CodePtr Get(IR::LocationDescriptor descriptor);

    // Returns "most likely" LocationDescriptor assocated with the emitted code at that location
    std::optional<IR::LocationDescriptor> ReverseGetLocation(CodePtr host_pc);

    // Returns "most likely" entry_point associated with the emitted code at that location
    CodePtr ReverseGetEntryPoint(CodePtr host_pc);

    CodePtr GetOrEmit(IR::LocationDescriptor descriptor);

    void InvalidateBasicBlocks(const ::Common::unordered_set<IR::LocationDescriptor>& descriptors);

    void ClearCache();

    // Venture: the code cache's extent (profilers, crash attribution).
    const void* CodeBegin() const { return mem.ptr(); }
    std::size_t CodeSize() const { return code_cache_size; }
    // Venture: compile a block without running it. False when it would need
    // more than the space left above min_space (never evicts compiled blocks).
    bool Precompile(IR::LocationDescriptor descriptor, std::size_t min_space);
    // Venture: size the block tables for count more blocks.
    void ReserveBlocks(std::size_t count);

    // Venture: the fast dispatch table (as on x64): indirect branches look up
    // their target in emitted code instead of calling GetOrEmit. The emitted
    // probe computes the same index as FastDispatchIndex.
    struct FastDispatchEntry {
        u64 location = ~u64{0};
        CodePtr code_ptr = nullptr;
    };
    static_assert(sizeof(FastDispatchEntry) == 16);
    // log2 of the table's entry count: 16 for A32 (as on x64), A64's
    // UserConfig::fast_dispatch_table_bits.
    unsigned fast_dispatch_bits = 16;
    u64 FastDispatchIndex(u64 location) const {
        return ((location ^ (location >> 16) ^ (location >> 32)) >> 1) & ((u64{1} << fast_dispatch_bits) - 1);
    }
    // Fills the entry for descriptor, compiling the block if needed.
    CodePtr FastDispatchFill(IR::LocationDescriptor descriptor);
    // Venture: emits the table probe for the location in X0 (the same index as
    // FastDispatchIndex): a hit branches to the block, a miss fills the entry
    // through FastDispatchFill and branches to its result. With `found`, both
    // branch there instead with the block in Xscratch1. census, when set,
    // counts misses in [3].
    void EmitFastDispatchProbe(oaknut::Label& l_return_to_dispatcher, bool reset_rsb, u64* census, oaknut::Label* found = nullptr);

    // Venture: dispatch_site_cache (A64): one slot per indirect-branch site,
    // {guest PC, host code}, assigned round robin in emission order.
    static constexpr std::size_t dispatch_site_count = std::size_t{1} << 13;
    std::vector<FastDispatchEntry> dispatch_sites;
    std::size_t next_dispatch_site = 0;
    void ResetDispatchSites();
protected:
    virtual EmitConfig GetEmitConfig() = 0;
    virtual void RegisterNewBasicBlock(const IR::Block& block, const EmittedBlockInfo& block_info) = 0;
    // Venture: compile timing, the stall marker and perf map names (as on x64).
    virtual u64 DescriptorPC(IR::LocationDescriptor descriptor) const = 0;
    virtual bool IsA32() const = 0;
    virtual std::string FriendlyName(IR::LocationDescriptor descriptor) const = 0;
    // Venture: ClearCache also forgets the guest ranges of the cleared blocks.
    virtual void ClearBlockRanges() = 0;
    // Venture: after a dispatcher's compile call, resets the return stack
    // buffer if that compile cleared the cache. X0 (the target) is preserved.
    void EmitRSBResetAfterCompile(oaknut::Label& l_return_to_dispatcher);
    u8 rsb_reset_pending = 0;

    void ProtectCodeMemory() {
#if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT) || defined(__APPLE__) || defined(__OpenBSD__)
        mem.protect();
#endif
    }

    void UnprotectCodeMemory() {
#if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT) || defined(__APPLE__) || defined(__OpenBSD__)
        mem.unprotect();
#endif
    }

    std::size_t GetRemainingSize();
    EmittedBlockInfo Emit(IR::Block ir_block);
    void Link(EmittedBlockInfo& block);
    void LinkBlockLinks(const CodePtr entry_point, const CodePtr target_ptr, const std::vector<BlockRelocation>& block_relocations_list);
    void RelinkForDescriptor(IR::LocationDescriptor target_descriptor, CodePtr target_ptr);

    FakeCall FastmemCallback(u64 host_pc);

    // Venture: the table lives in zero-filled pages that the OS commits on first
    // touch (as x64's fast_dispatch_table_pages), so a large A64 table costs
    // nothing until used. An entry's tag is the complement of its location: a
    // zero tag is empty and matches only location ~0, which no location is.
    // The address never changes: the prelude embeds it.
    class FastDispatchTable {
    public:
        FastDispatchTable() = default;
        FastDispatchTable(const FastDispatchTable&) = delete;
        FastDispatchTable& operator=(const FastDispatchTable&) = delete;
        ~FastDispatchTable();
        void Allocate(std::size_t entries);
        // Empties every entry, returning the pages to the OS.
        void Clear();
        bool empty() const { return entries == nullptr; }
        FastDispatchEntry* data() { return entries; }
        FastDispatchEntry& operator[](std::size_t index) { return entries[index]; }

    private:
        FastDispatchEntry* entries = nullptr;
        std::size_t count = 0;
    };
    // Empty when the FastDispatch optimization is off.
    FastDispatchTable fast_dispatch_table;

    IR::Block ir_block;
    const std::size_t code_cache_size;
    oaknut::CodeBlock mem;
    oaknut::CodeGenerator code;

    // A IR::LocationDescriptor will have one current CodePtr.
    // However, there can be multiple other CodePtrs which are older, previously invalidated blocks.
    std::map<CodePtr, IR::LocationDescriptor> reverse_block_entries;
    ::Common::unordered_map<IR::LocationDescriptor, CodePtr> block_entries;
    ::Common::unordered_map<CodePtr, EmittedBlockInfo> block_infos;
    ::Common::unordered_map<IR::LocationDescriptor, ::Common::unordered_set<CodePtr>> block_references;

    ExceptionHandler exception_handler;
    FastmemManager fastmem_manager;

    struct PreludeInfo {
        std::ptrdiff_t end_of_prelude;

        using RunCodeFuncType = HaltReason (*)(CodePtr entry_point, void* jit_state, volatile u32* halt_reason);
        RunCodeFuncType run_code;
        RunCodeFuncType step_code;
        void* return_to_dispatcher;
        void* fast_dispatch = nullptr;
        void* dispatch_site_miss = nullptr;
        void* return_from_run_code;

        void* read_memory_8;
        void* read_memory_16;
        void* read_memory_32;
        void* read_memory_64;
        void* read_memory_128;
        void* wrapped_read_memory_8;
        void* wrapped_read_memory_16;
        void* wrapped_read_memory_32;
        void* wrapped_read_memory_64;
        void* wrapped_read_memory_128;
        void* exclusive_read_memory_8;
        void* exclusive_read_memory_16;
        void* exclusive_read_memory_32;
        void* exclusive_read_memory_64;
        void* exclusive_read_memory_128;
        void* write_memory_8;
        void* write_memory_16;
        void* write_memory_32;
        void* write_memory_64;
        void* write_memory_128;
        void* wrapped_write_memory_8;
        void* wrapped_write_memory_16;
        void* wrapped_write_memory_32;
        void* wrapped_write_memory_64;
        void* wrapped_write_memory_128;
        void* exclusive_write_memory_8;
        void* exclusive_write_memory_16;
        void* exclusive_write_memory_32;
        void* exclusive_write_memory_64;
        void* exclusive_write_memory_128;
        void* inline_exclusive_write_fallback_8 = nullptr;
        void* inline_exclusive_write_fallback_16 = nullptr;
        void* inline_exclusive_write_fallback_32 = nullptr;
        void* inline_exclusive_write_fallback_64 = nullptr;
        void* inline_exclusive_write_fallback_128 = nullptr;
        void* atomic_memory;
        void* wrapped_atomic_memory = nullptr;
        void* atomic_compare_and_swap_128;

        void* call_svc;
        void* call_svc_fast = nullptr;
        void* call_svc_fast_known = nullptr;
        void* exception_raised;
        void* dc_raised;
        void* ic_raised;
        void* isb_raised;

        void* get_cntpct;
        void* add_ticks;
        void* get_ticks_remaining;
    } prelude_info;
};

}  // namespace Dynarmic::Backend::Arm64
