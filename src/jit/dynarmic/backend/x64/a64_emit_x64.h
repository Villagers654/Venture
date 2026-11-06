// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <map>
#include <optional>
#include <tuple>
#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"
#include <boost/container/static_vector.hpp>

#include "dynarmic/backend/block_range_information.h"
#include "dynarmic/backend/x64/a64_jitstate.h"
#include "dynarmic/backend/x64/emit_x64.h"
#include "dynarmic/backend/x64/portable_block.h"
#include "dynarmic/backend/x64/reg_alloc.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/ir/terminal.h"

namespace Dynarmic::Backend::X64 {

struct A64EmitContext final : public EmitContext {
    A64EmitContext(const A64::UserConfig& conf, RegAlloc& reg_alloc, IR::Block& block, boost::container::stable_vector<Xbyak::Label>& shared_labels);

    A64::LocationDescriptor Location() const;
    bool IsSingleStep() const;
    FP::FPCR FPCR(bool fpcr_controlled = true) const override;

    bool HasOptimization(OptimizationFlag flag) const override {
        return conf.HasOptimization(flag);
    }

    const A64::UserConfig& conf;

    // Register values a memory access's abort exit stores (A64ColdSet*), keyed by
    // the access. Locations are those after the access, as at its abort exit.
    struct ColdStateStore {
        IR::Opcode opcode;
        size_t index;
        std::optional<HostLoc> location;  // nullopt: immediate
        u64 immediate;
        // Venture: a spilled value that was never stored is read from JitState here.
        std::optional<NextUseTable::Remat> remat = std::nullopt;
    };
    ::Common::unordered_map<const IR::Inst*, boost::container::small_vector<ColdStateStore, 4>> cold_state;
    bool emitting_deferred = false;
};

// UserConfig::host_return_stack, where its requirements hold.
inline bool UsesHostReturnStack(const A64::UserConfig& conf) {
    return conf.host_return_stack && conf.HasOptimization(OptimizationFlag::ReturnStackBuffer) &&
           conf.HasOptimization(OptimizationFlag::BlockLinking) && conf.HasOptimization(OptimizationFlag::FastDispatch) &&
           !conf.fast_dispatch_census;
}

class A64EmitX64 final : public EmitX64 {
public:
    A64EmitX64(BlockOfCode& code, A64::UserConfig conf, A64::Jit* jit_interface);
    ~A64EmitX64() override;

    /// Emit host machine code for a basic block with intermediate representation `block`.
    /// @note block is modified.
    BlockDescriptor Emit(IR::Block& block);

    void ClearCache() override;
    void EmitCallEdge(A64EmitContext& ctx, u64 target, u32 kind, bool dynamic = false);
    void MarkProfileLink(IR::LocationDescriptor source, IR::LocationDescriptor target);
    ::Common::unordered_set<CodePtr> profile_internal_links;
    size_t profile_prefix_size = 0;

    void InvalidateCacheRanges(const boost::icl::interval_set<u64>& ranges);

    // Venture: portable blocks (portable_block.h)
    bool PortableBlocksEnabled() const { return portable_enabled; }
    void ReserveBlocks(size_t count);
    /// Installs a portable block captured by a compatible JIT; nullopt when it
    /// cannot be installed here (the caller compiles instead).
    std::optional<BlockDescriptor> InstallPortableBlock(const PortableBlock& block);
    void HashGuestCode(PortableCodeHash& hash, u64 begin, u64 end);

//protected:
    struct FastDispatchEntry {
        u64 location_descriptor = 0xFFFF'FFFF'FFFF'FFFFull;
        const void* code_ptr = nullptr;
    };
    static_assert(sizeof(FastDispatchEntry) == 0x10);
    /// With OS pages, a zero tag is empty, so construction and clearing touch no
    /// entry pages; the one entry location 0 hashes to holds ~0 instead. Without,
    /// one heap array of empty (~0-tag) entries, cleared by filling as upstream.
    class FastDispatchTable {
    public:
        FastDispatchTable(size_t bytes, bool pages);
        ~FastDispatchTable();
        FastDispatchTable(const FastDispatchTable&) = delete;
        FastDispatchTable& operator=(const FastDispatchTable&) = delete;
        /// Empties every entry at the same address.
        void Reset();
        FastDispatchEntry* data() const noexcept { return entries; }
    private:
        FastDispatchEntry* entries = nullptr;
        size_t bytes = 0;
        bool pages = false;
    };

    void ClearFastDispatchTable();
    void SeedFastDispatchTable();
    void GenMemory128Accessors();
    void GenFastmemFallbacks();
    void (*FastmemFallback(FallbackKind kind, const FallbackKey& key))();
    void GenTerminalHandlers();

    // Microinstruction emitters
    void EmitPushRSB(EmitContext& ctx, IR::Inst* inst);
    void EmitInlineFastDispatch(IR::LocationDescriptor initial_location);
    void EmitSiteDispatch();
    void ResetDispatchSites();
    void EmitInlinePopRSB(IR::LocationDescriptor initial_location);
    bool UsesCompactPopRSB() const;
    void EmitStaticLocationDescriptor(IR::LocationDescriptor initial_location);
    void EmitKnownReturn(IR::LocationDescriptor initial_location, bool is_single_step);
    void EmitHostCall(IR::LocationDescriptor return_location, IR::LocationDescriptor initial_location, bool is_single_step,
                      const IR::Term::Terminal& terminal);
#define OPCODE(...)
#define A32OPC(...)
#define A64OPC(name, type, ...) void EmitA64##name(A64EmitContext& ctx, IR::Inst* inst);
#include "dynarmic/ir/opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC

    // Helpers
    std::string LocationDescriptorToFriendlyName(const IR::LocationDescriptor&) const override;

    // Fastmem information
    using DoNotFastmemMarker = std::tuple<IR::LocationDescriptor, unsigned>;
    struct FastmemPatchInfo {
        u64 resume_rip;
        u64 callback;
        DoNotFastmemMarker marker;
        bool recompile;
    };
    std::optional<DoNotFastmemMarker> ShouldFastmem(A64EmitContext& ctx, IR::Inst* inst) const;
    void EmitFastmemFallback(A64EmitContext& ctx, IR::Inst* inst, Xbyak::Label& abort, Xbyak::Label& end, Xbyak::Reg64 vaddr,
                             std::optional<s32> folded, std::optional<FoldedIndex> index, void (*wrapped_fn)(), const void* location, DoNotFastmemMarker marker);
    void FoldMemoryAddresses(A64EmitContext& ctx) const;
    void FoldScaledIndices(A64EmitContext& ctx) const;
    FakeCall FastmemCallback(u64 rip);

    // nzcv_exit_liveness
    void MarkRematerializableGets(IR::Block& block);
    bool NZCVDeadAt(u64 pc, size_t& budget, size_t depth, std::vector<boost::icl::discrete_interval<u64>>& ranges) const;
    IR::Inst* PrepareHostFlagsExit(A64EmitContext& ctx);
    void EmitExitNZCV();

    // Memory access helpers
    void EmitCheckMemoryAbort(A64EmitContext& ctx, IR::Inst* inst, Xbyak::Label* end = nullptr);
    void RecordColdState(A64EmitContext& ctx, IR::Inst* inst);
    void EmitColdStateStores(A64EmitContext& ctx, IR::Inst* access);
    template<std::size_t bitsize, auto callback>
    void EmitMemoryRead(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitMemoryWrite(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveReadMemory(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveWriteMemory(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveReadMemoryInline(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveWriteMemoryInline(A64EmitContext& ctx, IR::Inst* inst);

    template<std::size_t bitsize, bool compare_and_swap>
    void EmitAtomicMemory(A64EmitContext& ctx, IR::Inst* inst);

    // Terminal instruction emitters
    bool EmitLeafTerminal(IR::Term::LeafTerminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step) noexcept override;
    bool EmitTerminal(IR::Term::Terminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step) noexcept override;

    // Patching
    void Unpatch(const IR::LocationDescriptor& target_desc) override;
    void EmitPatchJg(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) override;
    void EmitPatchJz(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) override;
    void EmitPatchJmp(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) override;
    void EmitPatchMovRcx(CodePtr target_code_ptr = nullptr) override;
    bool CompactBlockLinks() const { return conf.compact_block_links && code.HasColdRegion(); }
    CodePtr UnlinkedExit(const IR::LocationDescriptor& target_desc);
    CodePtr CompactLinkTarget(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr, CodePtr patch_location);

//data
    bool terminal_nzcv_in_flags = false;
    // nzcv_exit_liveness: the If terminal reads the raw FLAGS of the block's
    // final Add/Sub (NZCV not yet stored) and stores NZCV only on the exits
    // marked live ([0] else, [1] then).
    bool terminal_host_flags = false;
    bool terminal_carry_inverted = false;
    std::array<bool, 2> exit_nzcv_live{};
    // Successor code each dead exit depends on (registered with the block's range).
    boost::container::small_vector<boost::icl::discrete_interval<u64>, 2> exit_nzcv_ranges;
    // Guest code [begin, end) the translator copied into the next block (outlined
    // atomic helpers), registered with its range.
    std::vector<std::pair<u64, u64>> inlined_code;
    // The current block's terminal sees its entry FPCR (no FPCR write or SVC).
    bool static_fpcr_terminal = false;
    // svc_return_link: the current block's pushed return location, when its
    // own PopRSBHint exit pops it.
    std::optional<IR::LocationDescriptor> known_return;
    // host_return_stack: the current block's call return location (BL, BLR).
    std::optional<IR::LocationDescriptor> host_call_return;
    bool host_return_stack = false;
    bool elide_forward_halt_polls = false;
    const A64::UserConfig conf;
    RegAlloc reg_alloc; //reusable reg alloc
    NextUseTable next_uses;
    BlockRangeInformation<u64> block_ranges;
    u32 fast_dispatch_table_mask = 0;
    FastDispatchTable fast_dispatch_table;
    // Diagnostic census only: simulated 2^18- and 2^16-entry tag arrays.
    std::optional<FastDispatchTable> census_shadow;
    // dispatch_site_cache: one slot (last target PC, host code) per indirect-branch
    // site, assigned in emission order and reused round robin once all are taken.
    static constexpr size_t dispatch_site_count = size_t(1) << 13;
    std::optional<FastDispatchTable> dispatch_sites;
    size_t next_dispatch_site = 0;
    ::Common::unordered_map<u64, FastmemPatchInfo> fastmem_patch_info;
    void EmplaceFastmemPatch(u64 location, const FastmemPatchInfo& info) {
        fastmem_patch_info.emplace(location, info);
        if (active_capture)
            RecordFastmemSite(location, info.resume_rip, info.callback, info.marker, info.recompile);
    }
    ::Common::unordered_map<std::tuple<bool, size_t, int, int>, void (*)()> read_fallbacks;
    ::Common::unordered_map<std::tuple<bool, size_t, int, int>, void (*)()> write_fallbacks;
    ::Common::unordered_map<std::tuple<bool, size_t, int, int>, void (*)()> exclusive_write_fallbacks;
    // Venture: fallback entry addresses -> packed (kind, key), sorted, for ClassifyAddress;
    // rebuilt when the maps (which only grow) change size.
    std::vector<std::pair<u64, u64>> fallback_index;
    size_t fallback_index_entries = 0;
    ::Common::unordered_set<DoNotFastmemMarker> do_not_fastmem;
    boost::container::stable_vector<Xbyak::Label> shared_labels;
    const void* terminal_handler_pop_rsb_hint = nullptr;
    const void* terminal_handler_fast_dispatch_hint = nullptr;
    const void* terminal_handler_fast_dispatch_miss = nullptr;
    const void* terminal_handler_rsb_miss = nullptr;
    const void* rsb_pop_lookup = nullptr;
    const void* terminal_handler_site_miss = nullptr;
    FastDispatchEntry& (*fast_dispatch_table_lookup)(u64) = nullptr;
    A64::Jit* jit_interface = nullptr;
    void (*memory_read_128)() = nullptr;
    void (*memory_write_128)() = nullptr;
    void (*memory_exclusive_write_128)() = nullptr;

    // Venture: portable blocks
    bool portable_enabled = false;
    PortableCapture capture;
    std::array<const void*, size_t(PortableSymbol::Count)> portable_symbols{};
    std::array<u64, size_t(PortableObject::Count)> portable_objects{};
    ::Common::unordered_map<u64, IR::LocationDescriptor> unlinked_exits;
    // InstallPortableBlock scratch, kept to avoid allocating per block.
    std::vector<u64> install_modules, install_svc;
    std::vector<std::pair<u64, u64>> install_slots;
    void InitPortableBlocks();
    void BeginCapture();
    void FinishCapture(const IR::LocationDescriptor& descriptor, CodePtr entrypoint, size_t size, size_t cold_begin, size_t cold_end,
                       const std::vector<std::pair<u64, u64>>& ranges);
    std::optional<PortableRef> ClassifyAddress(u64 address, size_t hot_begin, size_t hot_end, size_t cold_begin, size_t cold_end,
                                               PortableBlock& block);
};

}  // namespace Dynarmic::Backend::X64
