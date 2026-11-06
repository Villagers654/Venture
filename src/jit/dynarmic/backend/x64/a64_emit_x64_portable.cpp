// SPDX-FileCopyrightText: Copyright 2026 Venture contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// Venture: capture of compiled A64 blocks as portable blocks and their
// installation into another JIT (portable_block.h).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <boost/icl/interval_set.hpp>
#include <mcl/scope_exit.hpp>

#include "common/assert.h"
#include "dynarmic/backend/x64/a64_emit_x64.h"
#include "dynarmic/backend/x64/a64_jitstate.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/exclusive_monitor_friend.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/interface/exclusive_monitor.h"

namespace Dynarmic::Backend::X64 {

namespace {

u64 PackFallback(EmitX64::FallbackKind kind, const EmitX64::FallbackKey& key) {
    const auto [ordered, bitsize, vaddr, value] = key;
    return u64(kind) | u64(ordered) << 8 | u64(u16(bitsize)) << 16 | u64(u16(s16(vaddr))) << 32 | u64(u16(s16(value))) << 48;
}

std::pair<EmitX64::FallbackKind, EmitX64::FallbackKey> UnpackFallback(u64 packed) {
    return {static_cast<EmitX64::FallbackKind>(packed & 0xff),
            EmitX64::FallbackKey{bool((packed >> 8) & 1), size_t(u16(packed >> 16)), int(s16(u16(packed >> 32))), int(s16(u16(packed >> 48)))}};
}

s64 ReadDisplacement(const u8* p, size_t size) {
    if (size == 1)
        return s8(*p);
    s32 value;
    std::memcpy(&value, p, 4);
    return value;
}

}  // namespace

void EmitX64::RecordLinkSite(u8 type, const IR::LocationDescriptor& target) {
    if (active_capture)
        active_capture->links.push_back({size_t(code.getCurr<const u8*>() - code.getCode()), static_cast<PortableLink::Type>(type), target.Value()});
}

void EmitX64::RecordFastmemSite(u64 location, u64 resume, u64 callback, const std::tuple<IR::LocationDescriptor, unsigned>& marker, bool recompile) {
    if (active_capture)
        active_capture->fastmem.push_back({location, resume, callback, std::get<0>(marker).Value(), std::get<1>(marker), recompile});
}

void A64EmitX64::InitPortableBlocks() {
    if (!conf.portable_block_sink && !conf.portable_block_source)
        return;
    // Instrumented or diagnostic code embeds per-run state; links must be compact.
    if (!CompactBlockLinks() || conf.enable_cycle_counting || conf.very_verbose_debugging_output || conf.profile_cookie ||
        conf.profile_entry_counter || conf.profile_direct_call_counter || conf.profile_call_edge.record || conf.profile_function_links ||
        conf.diagnostic_entry.record || conf.fast_dispatch_census || conf.page_table || !conf.fastmem_pointer)
        return;
    portable_enabled = true;

    using S = PortableSymbol;
    const auto set_symbol = [this](S s, const void* p) { portable_symbols[size_t(s)] = p; };
    set_symbol(S::PopRSBHint, terminal_handler_pop_rsb_hint);
    set_symbol(S::FastDispatchHint, terminal_handler_fast_dispatch_hint);
    set_symbol(S::FastDispatchMiss, terminal_handler_fast_dispatch_miss);
    set_symbol(S::RSBMiss, terminal_handler_rsb_miss);
    set_symbol(S::RSBPopLookup, rsb_pop_lookup);
    set_symbol(S::SiteMiss, terminal_handler_site_miss);
    set_symbol(S::FastDispatchLookup, reinterpret_cast<const void*>(fast_dispatch_table_lookup));
    set_symbol(S::MemoryRead128, reinterpret_cast<const void*>(memory_read_128));
    set_symbol(S::MemoryWrite128, reinterpret_cast<const void*>(memory_write_128));
    set_symbol(S::MemoryExclusiveWrite128, reinterpret_cast<const void*>(memory_exclusive_write_128));
    // The other return_from_run_code variants (MXCSR exited, forced return).
    for (size_t i = 0; i < 4; ++i)
        portable_symbols[size_t(S::ReturnFromRunCode0) + i] = code.GetReturnFromRunCodeVariant(i);

    using O = PortableObject;
    auto& objects = portable_objects;
    objects[size_t(O::Callbacks)] = reinterpret_cast<u64>(conf.callbacks);
    objects[size_t(O::Emitter)] = reinterpret_cast<u64>(this);
    objects[size_t(O::JitInterface)] = reinterpret_cast<u64>(jit_interface);
    objects[size_t(O::FastDispatchTable)] = reinterpret_cast<u64>(fast_dispatch_table.data());
    objects[size_t(O::Tpidr)] = reinterpret_cast<u64>(conf.tpidr_el0);
    objects[size_t(O::Tpidrro)] = reinterpret_cast<u64>(conf.tpidrro_el0);
    if (conf.global_monitor) {
        auto* monitor = conf.global_monitor;
        objects[size_t(O::MonitorLock)] = reinterpret_cast<u64>(GetExclusiveMonitorLockPointer(monitor));
        objects[size_t(O::MonitorActiveCount)] = reinterpret_cast<u64>(GetExclusiveMonitorActiveCountPointer(monitor));
        // Not MonitorAddressBase: emitted code never embeds it, and for processor 0
        // it equals MonitorAddressSelf, which would make the two ambiguous.
        objects[size_t(O::MonitorGenerationBase)] = reinterpret_cast<u64>(GetExclusiveMonitorGenerationBase(monitor));
        // Not GetExclusiveMonitorAddressPointer: that marks the processor active.
        objects[size_t(O::MonitorAddressSelf)] = reinterpret_cast<u64>(GetExclusiveMonitorAddressBase(monitor)) + conf.processor_id * GetExclusiveMonitorProcessorStride();
        objects[size_t(O::MonitorValueSelf)] = reinterpret_cast<u64>(GetExclusiveMonitorValuePointer(monitor, conf.processor_id));
        objects[size_t(O::MonitorGenerationSelf)] = reinterpret_cast<u64>(GetExclusiveMonitorGenerationSlot(monitor, conf.processor_id));
    }

    auto& ranges = capture.ranges;
    const u64 base = reinterpret_cast<u64>(code.getCode());
    // Code-space addresses are classified by ClassifyAddress itself.
    ranges.push_back({base, base + code.GetTotalCodeSize(), PortableRefKind::Hot, 0});
    for (size_t i = 0; i < objects.size(); ++i) {
        if (!objects[i])
            continue;
        const u64 size = i == size_t(O::Emitter) ? sizeof(*this) : 1;
        ranges.push_back({objects[i], objects[i] + size, PortableRefKind::Object, u16(i)});
    }
    if (dispatch_sites) {
        const u64 sites = reinterpret_cast<u64>(dispatch_sites->data());
        ranges.push_back({sites, sites + dispatch_site_count * sizeof(FastDispatchEntry), PortableRefKind::SiteSlot, 0});
    }
    const auto& modules = HostModules();
    for (size_t i = 0; i < modules.size(); ++i)
        ranges.push_back({modules[i].begin, modules[i].end, PortableRefKind::Host, u16(i)});
    capture.SortRanges();
}

void A64EmitX64::BeginCapture() {
    capture.Reset();
    active_capture = &capture;
    code.relocObserver_ = &capture;
    code.unwind_observer = [this](const void* begin, const void* end, const ABIPrologue& prologue, bool inherits) {
        capture.unwind.push_back({begin, end, prologue, inherits});
    };
}

std::optional<PortableRef> A64EmitX64::ClassifyAddress(u64 address, size_t hot_begin, size_t hot_end, size_t cold_begin, size_t cold_end,
                                                       PortableBlock& block) {
    const u64 base = reinterpret_cast<u64>(code.getCode());
    if (address >= base && address < base + code.GetTotalCodeSize()) {
        const size_t offset = size_t(address - base);
        if (const auto it = unlinked_exits.find(address); it != unlinked_exits.end())
            return PortableRef{PortableRefKind::UnlinkedExit, 0, it->second.Value()};
        if (offset >= hot_begin && offset < hot_end)
            return PortableRef{PortableRefKind::Hot, 0, offset - hot_begin};
        if (offset >= cold_begin && offset < cold_end)
            return PortableRef{PortableRefKind::Cold, 0, offset - cold_begin};
        for (size_t i = 0; i < portable_symbols.size(); ++i) {
            if (portable_symbols[i] && reinterpret_cast<u64>(portable_symbols[i]) == address)
                return PortableRef{PortableRefKind::Symbol, u16(i), 0};
        }
        if (const auto [pool_begin, pool_end] = code.ConstantPoolRange();
            address >= reinterpret_cast<u64>(pool_begin) && address < reinterpret_cast<u64>(pool_end)) {
            if ((address - reinterpret_cast<u64>(pool_begin)) % 16)
                return std::nullopt;
            std::array<u64, 2> value;
            std::memcpy(value.data(), reinterpret_cast<const void*>(address), 16);
            size_t index = std::find(block.constants.begin(), block.constants.end(), value) - block.constants.begin();
            if (index == block.constants.size())
                block.constants.push_back(value);
            return PortableRef{PortableRefKind::Constant, 0, index};
        }
        // Off Windows fallbacks are generated on demand, so the index follows the live maps.
        // (A linear search of them took 40% of all translation time: every reference to
        // other code-space addresses reached it.)
        if (const size_t entries = read_fallbacks.size() + write_fallbacks.size() + exclusive_write_fallbacks.size();
            entries != fallback_index_entries) {
            fallback_index.clear();
            fallback_index.reserve(entries);
            for (const auto kind : {FallbackKind::Read, FallbackKind::Write, FallbackKind::ExclusiveWrite}) {
                const auto& map = kind == FallbackKind::Read ? read_fallbacks : kind == FallbackKind::Write ? write_fallbacks : exclusive_write_fallbacks;
                for (const auto& [key, fn] : map)
                    fallback_index.emplace_back(reinterpret_cast<u64>(fn), PackFallback(kind, key));
            }
            // Several keys may share an entry; the first in map order wins, as before.
            std::stable_sort(fallback_index.begin(), fallback_index.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            fallback_index_entries = entries;
        }
        const auto it = std::lower_bound(fallback_index.begin(), fallback_index.end(), address, [](const auto& e, u64 a) { return e.first < a; });
        if (it != fallback_index.end() && it->first == address)
            return PortableRef{PortableRefKind::Fallback, 0, it->second};
        return std::nullopt;
    }
    const auto* range = capture.FindRange(address);
    if (!range)
        return std::nullopt;
    switch (range->kind) {
    case PortableRefKind::Host: {
        const auto& name = HostModules()[range->aux].name;
        size_t index = std::find(block.modules.begin(), block.modules.end(), name) - block.modules.begin();
        if (index == block.modules.size())
            block.modules.push_back(name);
        return PortableRef{PortableRefKind::Host, u16(index), address - range->begin};
    }
    case PortableRefKind::Object:
        return PortableRef{PortableRefKind::Object, range->aux, address - range->begin};
    case PortableRefKind::SiteSlot: {
        const u64 offset = address - range->begin;
        return PortableRef{PortableRefKind::SiteSlot, 0, (offset / sizeof(FastDispatchEntry)) << 8 | (offset % sizeof(FastDispatchEntry))};
    }
    default:
        return std::nullopt;
    }
}

namespace {
// Diagnostics: VENTURE_JIT_DUMP_BLOCK=<descriptor> writes that block's code as emitted
// and as installed to VENTURE_JIT_DUMP_DIR, for comparing the two.
bool DumpWanted(u64 descriptor) {
    static const char* wanted = std::getenv("VENTURE_JIT_DUMP_BLOCK");
    static const bool dir = std::getenv("VENTURE_JIT_DUMP_DIR") != nullptr;
    return wanted && dir && std::strtoull(wanted, nullptr, 0) == descriptor;
}
void DumpBlock(u64 descriptor, const char* tag, const u8* hot, size_t hot_size, const u8* cold, size_t cold_size, const std::string& notes) {
    static const char* dir = std::getenv("VENTURE_JIT_DUMP_DIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%llx-%s-%p.txt", dir, static_cast<unsigned long long>(descriptor), tag, static_cast<const void*>(hot));
    if (std::FILE* f = std::fopen(path, "w")) {
        std::fprintf(f, "hot %p %zu\n", static_cast<const void*>(hot), hot_size);
        for (size_t i = 0; i < hot_size; ++i) std::fprintf(f, "%02x%s", hot[i], i + 1 == hot_size ? "\n" : " ");
        std::fprintf(f, "cold %p %zu\n", static_cast<const void*>(cold), cold_size);
        for (size_t i = 0; i < cold_size; ++i) std::fprintf(f, "%02x%s", cold[i], i + 1 == cold_size ? "\n" : " ");
        std::fputs(notes.c_str(), f);
        std::fclose(f);
    }
}
// Diagnostics: each relocation's site, kind and the 32 bytes at its target, from the code as it stands.
std::string DescribeTargets(const PortableBlock& block, const u8* hot, const u8* cold) {
    std::string notes;
    char line[256];
    for (const auto& r : block.relocs) {
        const u8* site = (r.cold ? cold : hot) + r.site;
        u64 target = 0;
        if (r.size == 8) {
            std::memcpy(&target, site, 8);
        } else if (r.size == 4) {
            s32 disp;
            std::memcpy(&disp, site, 4);
            target = u64(s64(reinterpret_cast<u64>(site)) + disp - r.addend);
        } else {
            target = u64(s64(reinterpret_cast<u64>(site)) + s8(*site) - r.addend);
        }
        int n = std::snprintf(line, sizeof(line), "target %s+%u kind %u aux %u value %llx ->", r.cold ? "cold" : "hot", unsigned(r.site), unsigned(r.target.kind), unsigned(r.target.aux),
                              static_cast<unsigned long long>(r.target.value));
        const bool inside = r.target.kind == PortableRefKind::Hot || r.target.kind == PortableRefKind::Cold;
        if (!inside && r.target.kind != PortableRefKind::UnlinkedExit && r.target.kind != PortableRefKind::SiteSlot && r.target.kind != PortableRefKind::Object) {
            const u8* t = reinterpret_cast<const u8*>(target);
            for (int k = 0; k < 32 && n < int(sizeof(line)) - 4; ++k)
                n += std::snprintf(line + n, sizeof(line) - n, " %02x", t[k]);
        }
        notes += line;
        notes += "\n";
    }
    return notes;
}
}  // namespace

void A64EmitX64::FinishCapture(const IR::LocationDescriptor& descriptor, CodePtr entrypoint, size_t size, size_t cold_begin, size_t cold_end,
                               const std::vector<std::pair<u64, u64>>& ranges) {
    active_capture = nullptr;
    code.relocObserver_ = nullptr;
    code.unwind_observer = nullptr;
    if (capture.failure)
        return PortableStat(capture.failure);
    for (const u64 value : capture.unclassified) {
        char text[48];
        if ((value >> 40) < 0xf)
            std::snprintf(text, sizeof(text), "raw imm %llx", static_cast<unsigned long long>(value));
        else
            std::snprintf(text, sizeof(text), "raw imm %llx TiB", static_cast<unsigned long long>(value >> 40));
        PortableStat(text);
    }
    const u8* const base = code.getCode();
    const size_t hot_begin = static_cast<const u8*>(entrypoint) - base, hot_end = hot_begin + size;
    PortableBlock block;
    block.descriptor = descriptor.Value();
    block.size = static_cast<u32>(size);
    block.hot.assign(base + hot_begin, base + hot_end);
    block.cold.assign(base + cold_begin, base + cold_end);
    block.ranges = ranges;
    {
        PortableCodeHash hash;
        for (const auto& [begin, end] : ranges)
            HashGuestCode(hash, begin, end);
        block.code_hash = hash.state;
    }

    const auto part_of = [&](size_t site, bool& cold) {
        if (site >= hot_begin && site < hot_end) {
            cold = false;
            return true;
        }
        if (site >= cold_begin && site < cold_end) {
            cold = true;
            return true;
        }
        return false;
    };
    std::vector<std::pair<size_t, size_t>> link_ranges;
    for (const auto& link : capture.links) {
        if (link.site < hot_begin || link.site + LinkSiteSize(link.type) > hot_end)
            return PortableStat("capture line 225");
        block.links.push_back({static_cast<u32>(link.site - hot_begin), link.type, link.target});
        link_ranges.emplace_back(link.site, link.site + LinkSiteSize(link.type));
    }
    const auto in_link = [&](size_t site) {
        return std::any_of(link_ranges.begin(), link_ranges.end(), [&](const auto& r) { return site >= r.first && site < r.second; });
    };
    const auto add = [&](size_t site, bool cold, u8 n, s32 addend, const PortableRef& target) {
        block.relocs.push_back({static_cast<u32>(site - (cold ? cold_begin : hot_begin)), cold, n, addend, target});
    };
    for (const auto& e : capture.events) {
        bool cold;
        if (!part_of(e.site, cold))
            return PortableStat("capture line 238");  // a reference outside the block (a patch of an older one)
        if (in_link(e.site))
            continue;
        switch (e.type) {
        case PortableCapture::Event::Type::Label: {
            bool target_cold;
            if (!part_of(e.target, target_cold))
                return PortableStat("capture line 245");
            if (target_cold == cold)
                continue;
            const s64 stored = ReadDisplacement(base + e.site, e.size);
            const s64 addend = stored - (s64(e.target) - s64(e.site));
            if (e.size != 4)
                return PortableStat("capture line 251");
            add(e.site, cold, 4, s32(addend), PortableRef{target_cold ? PortableRefKind::Cold : PortableRefKind::Hot, 0, e.target - (target_cold ? cold_begin : hot_begin)});
            break;
        }
        case PortableCapture::Event::Type::LabelAddress: {
            bool target_cold;
            if (!part_of(e.target, target_cold))
                return PortableStat("capture line 258");
            add(e.site, cold, 8, 0, PortableRef{target_cold ? PortableRefKind::Cold : PortableRefKind::Hot, 0, e.target - (target_cold ? cold_begin : hot_begin)});
            break;
        }
        case PortableCapture::Event::Type::Absolute: {
            const auto ref = ClassifyAddress(e.target, hot_begin, hot_end, cold_begin, cold_end, block);
            if (!ref)
                return PortableStat("capture line 265");
            if ((ref->kind == PortableRefKind::Hot && !cold) || (ref->kind == PortableRefKind::Cold && cold))
                continue;
            if (e.size != 4)
                return PortableStat("capture line 269");
            const s64 stored = ReadDisplacement(base + e.site, e.size);
            const s64 addend = stored - (s64(e.target) - s64(reinterpret_cast<u64>(base + e.site)));
            add(e.site, cold, 4, s32(addend), *ref);
            break;
        }
        case PortableCapture::Event::Type::Imm64: {
            const auto ref = ClassifyAddress(e.target, hot_begin, hot_end, cold_begin, cold_end, block);
            if (!ref)
                return PortableStat("capture line 278");
            add(e.site, cold, 8, 0, *ref);
            break;
        }
        }
    }
    for (const auto& s : capture.svcs) {
        bool cold;
        if (!part_of(s.site, cold) || s.imm > 0xffff)
            return PortableStat("capture line 287");
        add(s.site, cold, 8, 0, PortableRef{PortableRefKind::Svc, u16(s.imm), s.pc});
    }
    for (const auto& f : capture.fastmem) {
        const auto location = ClassifyAddress(f.location, hot_begin, hot_end, cold_begin, cold_end, block);
        const auto resume = ClassifyAddress(f.resume, hot_begin, hot_end, cold_begin, cold_end, block);
        const auto callback = ClassifyAddress(f.callback, hot_begin, hot_end, cold_begin, cold_end, block);
        if (!location || !resume || !callback || (location->kind != PortableRefKind::Hot && location->kind != PortableRefKind::Cold))
            return PortableStat("capture line 295");
        block.fastmem.push_back({*location, *resume, *callback, f.marker_descriptor, f.marker_index, f.recompile});
    }
    for (const auto& u : capture.unwind) {
        const size_t b = static_cast<const u8*>(u.begin) - base, en = static_cast<const u8*>(u.end) - base;
        if (b < hot_begin || en > hot_end || b > en)
            return PortableStat("capture line 301");
        block.unwind.push_back({static_cast<u32>(b - hot_begin), static_cast<u32>(en - hot_begin), u.inherits_run_frame, u.prologue.operations});
    }
    if (DumpWanted(block.descriptor))
        DumpBlock(block.descriptor, "fresh", base + hot_begin, size, base + cold_begin, cold_end - cold_begin, DescribeTargets(block, base + hot_begin, base + cold_begin));
    const auto bytes = SerializePortableBlock(block);
    PortableStat("captured");
    conf.portable_block_sink(block.descriptor, bytes.data(), bytes.size());
}

void A64EmitX64::HashGuestCode(PortableCodeHash& hash, u64 begin, u64 end) {
    u32 words[256];
    for (u64 pc = begin & ~u64(3); pc <= end;) {
        const size_t n = std::min<u64>(std::size(words), (end - pc) / 4 + 1);
        conf.callbacks->MemoryReadCodeWords(pc, words, n);
        for (size_t i = 0; i < n; ++i)
            hash.Add(words[i]);
        pc += 4 * n;
    }
}

std::optional<EmitX64::BlockDescriptor> A64EmitX64::InstallPortableBlock(const PortableBlock& block) {
    if (!portable_enabled)
        return PortableStat("install line 310"), std::nullopt;
    const IR::LocationDescriptor descriptor{block.descriptor};
    if (const auto existing = GetBasicBlock(descriptor))
        return existing;
    for (const auto& f : block.fastmem) {
        if (do_not_fastmem.contains(DoNotFastmemMarker{IR::LocationDescriptor{f.marker_descriptor}, f.marker_index}))
            return PortableStat("install line 316"), std::nullopt;
    }
    // The guest code must be what the block was translated from.
    {
        PortableCodeHash hash;
        for (const auto& [begin, end] : block.ranges) {
            if (end < begin || end - begin > 64 * 1024)
                return PortableStat("install line 323"), std::nullopt;
            HashGuestCode(hash, begin, end);
        }
        if (hash.state != block.code_hash)
            return PortableStat("install line 328"), std::nullopt;
    }
    // Resolve every target before writing; a failure leaves the cache unchanged.
    auto& modules = install_modules;
    modules.assign(block.modules.size(), 0);
    for (size_t i = 0; i < block.modules.size(); ++i) {
        const auto& all = HostModules();
        const auto it = std::find_if(all.begin(), all.end(), [&](const HostModule& m) { return m.name == block.modules[i]; });
        if (it == all.end())
            return PortableStat("install line 336"), std::nullopt;
        modules[i] = it->begin;
    }
    auto& svc = install_svc;
    svc.assign(block.relocs.size(), 0);
    for (size_t i = 0; i < block.relocs.size(); ++i) {
        const auto& r = block.relocs[i];
        if (r.target.kind == PortableRefKind::Svc) {
            const void* meta = conf.svc_lookup ? conf.svc_lookup(r.target.value, r.target.aux) : nullptr;
            if (!meta)
                return PortableStat("install line 345"), std::nullopt;
            svc[i] = reinterpret_cast<u64>(meta);
        }
        if (r.target.kind == PortableRefKind::Symbol && (r.target.aux >= portable_symbols.size() || !portable_symbols[r.target.aux]))
            return PortableStat("install line 349"), std::nullopt;
        if (r.target.kind == PortableRefKind::Object && (r.target.aux >= portable_objects.size() || !portable_objects[r.target.aux]))
            return PortableStat("install line 351"), std::nullopt;
        if (r.target.kind == PortableRefKind::SiteSlot && !dispatch_sites)
            return PortableStat("install line 353"), std::nullopt;
        if (r.target.kind == PortableRefKind::Constant && r.target.value >= block.constants.size())
            return PortableStat("install line 355"), std::nullopt;
    }
    const size_t needed = block.hot.size() + block.cold.size() + conf.block_alignment + 4096;
    if (code.SpaceRemaining() < needed + 1024 * 1024)
        return PortableStat("install line 359"), std::nullopt;

    code.EnableWriting();
    SCOPE_EXIT { code.DisableWriting(); };
    code.align(conf.block_alignment);
    const u8* const hot = code.getCurr<const u8*>();
    code.AppendBytes(block.hot);
    code.SwitchToColdCode();
    const u8* const cold = code.getCurr<const u8*>();
    code.AppendBytes(block.cold);
    code.SwitchToHotCode();

    auto& slots = install_slots;
    slots.clear();
    const auto resolve = [&](const PortableRef& ref, size_t index) -> u64 {
        switch (ref.kind) {
        case PortableRefKind::Hot: return reinterpret_cast<u64>(hot + ref.value);
        case PortableRefKind::Cold: return reinterpret_cast<u64>(cold + ref.value);
        case PortableRefKind::Symbol: return reinterpret_cast<u64>(portable_symbols[ref.aux]);
        case PortableRefKind::Constant: {
            const auto& c = block.constants[ref.value];
            return reinterpret_cast<u64>(code.ConstPointer(c[0], c[1]));
        }
        case PortableRefKind::Fallback: {
            const auto [kind, key] = UnpackFallback(ref.value);
            return reinterpret_cast<u64>(FastmemFallback(kind, key));
        }
        case PortableRefKind::Host: return modules[ref.aux] + ref.value;
        case PortableRefKind::Object:
            if (ref.aux == u16(PortableObject::MonitorAddressSelf) || ref.aux == u16(PortableObject::MonitorValueSelf) ||
                ref.aux == u16(PortableObject::MonitorGenerationSelf))
                GetExclusiveMonitorAddressPointer(conf.global_monitor, conf.processor_id);  // marks this processor active, as emission does
            return portable_objects[ref.aux] + ref.value;
        case PortableRefKind::SiteSlot: {
            auto it = std::find_if(slots.begin(), slots.end(), [&](const auto& s) { return s.first == ref.value >> 8; });
            if (it == slots.end()) {
                slots.emplace_back(ref.value >> 8, u64(next_dispatch_site++ & (dispatch_site_count - 1)));
                it = slots.end() - 1;
            }
            return reinterpret_cast<u64>(dispatch_sites->data() + it->second) + (ref.value & 0xff);
        }
        case PortableRefKind::Svc: return svc[index];
        case PortableRefKind::UnlinkedExit: return reinterpret_cast<u64>(UnlinkedExit(IR::LocationDescriptor{ref.value}));
        }
        return 0;
    };
    for (size_t i = 0; i < block.relocs.size(); ++i) {
        const auto& r = block.relocs[i];
        const u8* site = (r.cold ? cold : hot) + r.site;
        const u64 target = resolve(r.target, i);
        u8* writable = code.GetWritablePointer(site);
        if (r.size == 8) {
            std::memcpy(writable, &target, 8);
            continue;
        }
        const s64 disp = s64(target) - s64(reinterpret_cast<u64>(site)) + r.addend;
        if (r.size == 4 && disp == s32(disp)) {
            const s32 d = s32(disp);
            std::memcpy(writable, &d, 4);
        } else if (r.size == 1 && disp == s8(disp)) {
            *writable = u8(s8(disp));
        } else {
            // Out of range here: the appended bytes stay unreachable.
            return PortableStat("install line 415"), std::nullopt;
        }
    }
    const CodePtr save = code.getCurr();
    for (const auto& link : block.links) {
        const IR::LocationDescriptor target{link.target};
        const CodePtr site = hot + link.site;
        const auto target_block = GetBasicBlock(target);
        auto& info = patch_information[target];
        if (link.type != PortableLink::Type::MovRcx && !info.unlinked_exit)
            UnlinkedExit(target);  // finds the same entry: no insertion, `info` stays valid
        code.SetCodePtr(site);
        switch (link.type) {
        case PortableLink::Type::Jg:
            info.jg.push_back(site);
            EmitPatchJg(target, target_block ? target_block->entrypoint : nullptr);
            break;
        case PortableLink::Type::Jz:
            info.jz.push_back(site);
            EmitPatchJz(target, target_block ? target_block->entrypoint : nullptr);
            break;
        case PortableLink::Type::Jmp:
            info.jmp.push_back(site);
            EmitPatchJmp(target, target_block ? target_block->entrypoint : nullptr);
            break;
        case PortableLink::Type::MovRcx:
            info.mov_rcx.push_back(site);
            EmitPatchMovRcx(target_block ? target_block->entrypoint : nullptr);
            break;
        }
    }
    code.SetCodePtr(save);
    for (const auto& f : block.fastmem) {
        const auto location = resolve(f.location, 0), resume = resolve(f.resume, 0), callback = resolve(f.callback, 0);
        fastmem_patch_info.emplace(location, FastmemPatchInfo{resume, callback, DoNotFastmemMarker{IR::LocationDescriptor{f.marker_descriptor}, f.marker_index}, f.recompile});
    }
    for (const auto& u : block.unwind) {
        ABIPrologue prologue;
        prologue.operations = u.operations;
        code.RegisterNativeUnwindFrame(hot + u.begin, hot + u.end, prologue, u.inherits_run_frame);
    }
    for (const auto& [begin, end] : block.ranges)
        block_ranges.AddRange(boost::icl::discrete_interval<u64>::closed(begin, end), descriptor);
    PortableStat("installed");
    if (DumpWanted(block.descriptor)) {
        std::string notes;
        char line[160];
        for (size_t i = 0; i < block.relocs.size(); ++i) {
            const auto& r = block.relocs[i];
            std::snprintf(line, sizeof(line), "reloc %s+%u size %u kind %u aux %u value %llx addend %lld -> %llx\n", r.cold ? "cold" : "hot", unsigned(r.site), unsigned(r.size),
                          unsigned(r.target.kind), unsigned(r.target.aux), static_cast<unsigned long long>(r.target.value), static_cast<long long>(r.addend), static_cast<unsigned long long>(resolve(r.target, i)));
            notes += line;
        }
        for (const auto& link : block.links) {
            std::snprintf(line, sizeof(line), "link hot+%u type %u target %llx\n", unsigned(link.site), unsigned(link.type), static_cast<unsigned long long>(link.target));
            notes += line;
        }
        DumpBlock(block.descriptor, "installed", hot, block.hot.size(), cold, block.cold.size(), notes);
    }
    const auto registered = RegisterBlock(descriptor, hot, block.size);
    if (DumpWanted(block.descriptor))
        DumpBlock(block.descriptor, "registered", hot, block.hot.size(), cold, block.cold.size(), DescribeTargets(block, hot, cold));
    return registered;
}

}  // namespace Dynarmic::Backend::X64
