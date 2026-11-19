// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <optional>
#include <functional>
#include <array>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/container/unordered_map.h"

#include "dynarmic/backend/arm64/fastmem.h"
#include "dynarmic/backend/arm64/host_feature.h"
#include "dynarmic/interface/A32/coprocessor.h"
#include "dynarmic/interface/optimization_flags.h"
#include "dynarmic/ir/location_descriptor.h"

namespace oaknut {
struct CodeGenerator;
struct Label;
}  // namespace oaknut

namespace Dynarmic::FP {
class FPCR;
}  // namespace Dynarmic::FP

namespace Dynarmic::IR {
class Block;
class Inst;
enum class Cond;
enum class Opcode;
}  // namespace Dynarmic::IR

namespace Dynarmic {
class ExclusiveMonitor;
}  // namespace Dynarmic

namespace Dynarmic::Backend::Arm64 {

struct EmitContext;

using CodePtr = std::byte*;

enum class LinkTarget {
    ReturnToDispatcher,
    ReturnFromRunCode,
    FastDispatch,
    DispatchSiteMiss,
    ReadMemory8,
    ReadMemory16,
    ReadMemory32,
    ReadMemory64,
    ReadMemory128,
    WrappedReadMemory8,
    WrappedReadMemory16,
    WrappedReadMemory32,
    WrappedReadMemory64,
    WrappedReadMemory128,
    ExclusiveReadMemory8,
    ExclusiveReadMemory16,
    ExclusiveReadMemory32,
    ExclusiveReadMemory64,
    ExclusiveReadMemory128,
    WriteMemory8,
    WriteMemory16,
    WriteMemory32,
    WriteMemory64,
    WriteMemory128,
    WrappedWriteMemory8,
    WrappedWriteMemory16,
    WrappedWriteMemory32,
    WrappedWriteMemory64,
    WrappedWriteMemory128,
    ExclusiveWriteMemory8,
    ExclusiveWriteMemory16,
    ExclusiveWriteMemory32,
    ExclusiveWriteMemory64,
    ExclusiveWriteMemory128,
    InlineExclusiveWriteFallback8,
    InlineExclusiveWriteFallback16,
    InlineExclusiveWriteFallback32,
    InlineExclusiveWriteFallback64,
    InlineExclusiveWriteFallback128,
    AtomicMemory,
    WrappedAtomicMemory,
    AtomicCompareAndSwap128,
    CallSVC,
    CallSVCFast,
    CallSVCFastKnown,
    ExceptionRaised,
    InstructionSynchronizationBarrierRaised,
    InstructionCacheOperationRaised,
    DataCacheOperationRaised,
    GetCNTPCT,
    AddTicks,
    GetTicksRemaining,
};

struct Relocation {
    std::ptrdiff_t code_offset;
    LinkTarget target;
};

enum class BlockRelocationType {
    Branch,
    MoveToScratch1,
};

struct BlockRelocation {
    std::ptrdiff_t code_offset;
    BlockRelocationType type;
    /// Venture: where an unlinked Branch site jumps (offset from the entry
    /// point); 0 for a NOP that falls through to an inline unlinked path.
    std::ptrdiff_t unlinked_offset = 0;
};

struct EmittedBlockInfo {
    CodePtr entry_point;
    std::size_t size;
    std::vector<Relocation> relocations;
    ::Common::unordered_map<IR::LocationDescriptor, std::vector<BlockRelocation>> block_relocations;
    ::Common::unordered_map<std::ptrdiff_t, FastmemPatchInfo> fastmem_patch_info;
    // Venture: guest code [first, second] outside the block that its code
    // depends on (nzcv_exit_liveness), registered with the block's range.
    std::vector<std::pair<u64, u64>> dependent_ranges;
};

struct EmitConfig {
    OptimizationFlag optimizations;
    bool HasOptimization(OptimizationFlag f) const { return (f & optimizations) != no_optimizations; }

    bool hook_isb;

    // System registers
    u64 cntfreq_el0;
    u32 ctr_el0;
    u32 dczid_el0;
    const u64* tpidrro_el0;
    u64* tpidr_el0;

    // Memory
    bool check_halt_on_memory_access;

    // Page table
    u64 page_table_pointer;
    std::size_t page_table_address_space_bits;
    u64 page_table_pointer_mask;
    std::size_t page_table_log2_stride;
    std::optional<std::uint8_t> page_table_marked_bit;
    std::optional<std::uint8_t> page_table_sign_extension;
    bool silently_mirror_page_table;
    bool absolute_offset_page_table;
    u8 detect_misaligned_access_via_page_table;
    bool only_detect_misalignment_via_page_table_on_page_boundary;

    // Fastmem
    std::optional<u64> fastmem_pointer;
    bool recompile_on_fastmem_failure;
    std::size_t fastmem_address_space_bits;
    bool silently_mirror_fastmem;

    // Timing
    bool wall_clock_cntpct;
    bool enable_cycle_counting;

    // Endianness
    bool always_little_endian;

    // Frontend specific callbacks
    FP::FPCR (*descriptor_to_fpcr)(const IR::LocationDescriptor& descriptor);
    oaknut::Label (*emit_cond)(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Cond cond);
    void (*emit_condition_failed_terminal)(oaknut::CodeGenerator& code, EmitContext& ctx);
    void (*emit_terminal)(oaknut::CodeGenerator& code, EmitContext& ctx);
    void (*emit_check_memory_abort)(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, oaknut::Label& end);

    // State offsets
    std::size_t state_nzcv_offset;
    std::size_t state_fpsr_offset;
    std::size_t state_exclusive_state_offset;

    // A32 specific
    std::array<std::shared_ptr<A32::Coprocessor>, 16> coprocessors{};

    // Debugging
    bool very_verbose_debugging_output;

    // Venture: supervisor calls dispatched inside the JIT (UserConfig::svc_fast,
    // svc_fp_transfer, svc_fp_guest (A64 only) and svc_lookup).
    std::optional<u32> svc_fast{};
    std::optional<u32> svc_fp_transfer{};
    std::optional<u32> svc_fp_guest{};
    std::function<const void*(u64, u32)> svc_lookup{};

    // Venture: inline exclusives (emit_arm64_memory.cpp), from UserConfig's
    // global_monitor, processor_id, fastmem_exclusive_access,
    // recompile_on_exclusive_fastmem_failure and elide_unchanged_exclusive_stores.
    ExclusiveMonitor* global_monitor = nullptr;
    std::size_t processor_id = 0;
    bool fastmem_exclusive_access = false;
    bool recompile_on_exclusive_fastmem_failure = true;
    bool elide_unchanged_exclusive_stores = false;
    // Venture: the guest ISA, for diagnostics (dynarmic/interface/code_stats.h).
    bool is_a64 = false;
    // Venture: block links keep only the patchable branch in the block's
    // straight-line code; the unlinked exit is emitted with the deferred code.
    bool compact_block_links = false;
    // Venture: A64 UserConfig::diagnostic_entry. A selected block entry calls
    // diagnostic_record(user, pc, state) with the host FPCR.
    std::function<bool(u64)> diagnostic_selected;
    void (*diagnostic_record)(void* user, u64 pc, const void* state) noexcept = nullptr;
    void* diagnostic_user = nullptr;
    // Venture: the host's optional instructions (host_feature.h), less
    // UserConfig::disabled_host_features.
    u32 host_features = 0;
    bool HasHostFeature(HostFeature feature) const { return Arm64::HasHostFeature(host_features, feature); }

    // Venture: A64 dispatch, as on x64 (A64::UserConfig of the same names).
    // svc_return_link: a block with one RSB push that ends in PopRSBHint
    // links to the pushed location instead of pushing and popping.
    bool svc_return_link = false;
    // Forward conditional links within one halt_polling_window_size skip
    // the halt poll; LinkBlockFast across a window polls.
    bool enable_bounded_halt_polling = false;
    // Diagnostic counters: [0] RSB pops, [1] RSB misses, [2] fast-dispatch
    // hints, [3] table misses.
    u64* fast_dispatch_census = nullptr;
    // dispatch_site_cache: the next indirect-branch site's {pc, code} slot,
    // empty when the cache is off.
    std::function<void*()> next_dispatch_site{};
    // A64 cold_memory_abort_state is Live or All: abort exits are out of line
    // and store the access's A64ColdSet* records.
    bool cold_memory_abort_state = false;

    // Venture: the A64 register cache (abi.h A64CachedRegister).
    bool a64_register_cache = false;

    // Venture: A64 UserConfig::nzcv_exit_liveness, reading successor code.
    bool nzcv_exit_liveness = false;
    std::function<std::optional<u32>(u64)> read_code;
};

EmittedBlockInfo EmitArm64(oaknut::CodeGenerator& code, IR::Block block, const EmitConfig& emit_conf, FastmemManager& fastmem_manager);

template<IR::Opcode op>
void EmitIR(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst);
void EmitRelocation(oaknut::CodeGenerator& code, EmitContext& ctx, LinkTarget link_target);
void EmitBlockLinkRelocation(oaknut::CodeGenerator& code, EmitContext& ctx, const IR::LocationDescriptor& descriptor, BlockRelocationType type);
oaknut::Label EmitA32Cond(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Cond cond);
oaknut::Label EmitA64Cond(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Cond cond);
void EmitA32Terminal(oaknut::CodeGenerator& code, EmitContext& ctx);
void EmitA64Terminal(oaknut::CodeGenerator& code, EmitContext& ctx);
// Venture: the A64 terminal's per-block choices, made before the block's code.
void PrepareA64Terminal(EmitContext& ctx);
void PrepareA64NZCVExit(EmitContext& ctx);
bool FuseA64CheckBit(EmitContext& ctx, IR::Inst* inst, IR::Inst* next, bool next_is_last);
void FoldA64Addresses(EmitContext& ctx);
bool EmitFoldedAddressAlias(EmitContext& ctx, IR::Inst* inst);
void EmitA64RegisterCacheStore(oaknut::CodeGenerator& code, bool enabled);
void EmitA64RegisterCacheLoad(oaknut::CodeGenerator& code, bool enabled);
void EmitA32ConditionFailedTerminal(oaknut::CodeGenerator& code, EmitContext& ctx);
void EmitA64ConditionFailedTerminal(oaknut::CodeGenerator& code, EmitContext& ctx);
void EmitA32CheckMemoryAbort(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, oaknut::Label& end);
void EmitA64CheckMemoryAbort(oaknut::CodeGenerator& code, EmitContext& ctx, IR::Inst* inst, oaknut::Label& end);

}  // namespace Dynarmic::Backend::Arm64
