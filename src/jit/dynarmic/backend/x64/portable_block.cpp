// SPDX-FileCopyrightText: Copyright 2026 Venture contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dynarmic/backend/x64/portable_block.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#    include <psapi.h>
#else
#    include <link.h>
#endif

namespace Dynarmic::Backend::X64 {

namespace {

constexpr u32 magic = 0x32425056;  // "VPB2"

// Integers are LEB128 varints; guest addresses and descriptors are zigzag
// deltas from the block's own descriptor, so a record is mostly its code.
u64 ZigZag(s64 v) { return (u64(v) << 1) ^ u64(v >> 63); }
s64 UnZigZag(u64 v) { return s64(v >> 1) ^ -s64(v & 1); }
bool GuestValued(PortableRefKind kind) { return kind == PortableRefKind::Svc || kind == PortableRefKind::UnlinkedExit; }

class Writer {
public:
    std::vector<u8> bytes;
    u64 base = 0;
    template<typename T>
    void Put(T value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const size_t at = bytes.size();
        bytes.resize(at + sizeof(T));
        std::memcpy(bytes.data() + at, &value, sizeof(T));
    }
    void V(u64 value) {
        while (value >= 0x80) {
            bytes.push_back(u8(value | 0x80));
            value >>= 7;
        }
        bytes.push_back(u8(value));
    }
    void S(s64 value) { V(ZigZag(value)); }
    void Guest(u64 value) { S(s64(value - base)); }
    void PutBytes(std::span<const u8> data) {
        V(data.size());
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
    void PutRef(const PortableRef& ref) {
        Put<u8>(static_cast<u8>(ref.kind));
        V(ref.aux);
        if (GuestValued(ref.kind))
            Guest(ref.value);
        else
            V(ref.value);
    }
};

class Reader {
public:
    explicit Reader(std::span<const u8> bytes)
            : rest(bytes) {}
    std::span<const u8> rest;
    bool ok = true;
    u64 base = 0;
    template<typename T>
    T Get() {
        T value{};
        if (rest.size() < sizeof(T)) {
            ok = false;
            return value;
        }
        std::memcpy(&value, rest.data(), sizeof(T));
        rest = rest.subspan(sizeof(T));
        return value;
    }
    u64 V() {
        u64 value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            if (rest.empty())
                break;
            const u8 b = rest[0];
            rest = rest.subspan(1);
            value |= u64(b & 0x7f) << shift;
            if (!(b & 0x80))
                return value;
        }
        ok = false;
        return 0;
    }
    s64 S() { return UnZigZag(V()); }
    u64 Guest() { return base + u64(S()); }
    std::span<const u8> GetBytes() {
        const u64 n = V();
        if (!ok || rest.size() < n) {
            ok = false;
            return {};
        }
        const auto out = rest.first(n);
        rest = rest.subspan(n);
        return out;
    }
    // A count bounded by the bytes left (every element takes at least one byte).
    u32 GetCount() {
        const u64 n = V();
        if (n > rest.size())
            ok = false;
        return ok ? u32(n) : 0;
    }
    PortableRef GetRef() {
        PortableRef ref;
        const u8 kind = Get<u8>();
        if (kind > static_cast<u8>(PortableRefKind::UnlinkedExit))
            ok = false;
        ref.kind = static_cast<PortableRefKind>(kind);
        ref.aux = u16(V());
        ref.value = GuestValued(ref.kind) ? Guest() : V();
        return ref;
    }
};

}  // namespace

std::vector<u8> SerializePortableBlock(const PortableBlock& block) {
    Writer w;
    w.bytes.reserve(block.hot.size() + block.cold.size() + block.relocs.size() * 8 + 64);
    w.Put<u32>(magic);
    w.Put<u64>(block.descriptor);
    w.base = block.descriptor;
    w.V(block.size);
    w.Put<u64>(block.code_hash);
    w.PutBytes(block.hot);
    w.PutBytes(block.cold);
    w.V(block.relocs.size());
    for (const auto& r : block.relocs) {
        w.V(r.site);
        w.Put<u8>(static_cast<u8>(static_cast<u8>(r.cold) | (r.size << 1)));
        w.S(r.addend);
        w.PutRef(r.target);
    }
    w.V(block.links.size());
    for (const auto& l : block.links) {
        w.V(l.site);
        w.Put<u8>(static_cast<u8>(l.type));
        w.Guest(l.target);
    }
    w.V(block.fastmem.size());
    for (const auto& f : block.fastmem) {
        w.PutRef(f.location);
        w.PutRef(f.resume);
        w.PutRef(f.callback);
        w.Guest(f.marker_descriptor);
        w.V(f.marker_index);
        w.Put<u8>(f.recompile);
    }
    w.V(block.unwind.size());
    for (const auto& u : block.unwind) {
        w.V(u.begin);
        w.V(u.end);
        w.Put<u8>(u.inherits_run_frame);
        w.V(u.operations.size());
        for (const auto& op : u.operations) {
            w.Put<u8>(static_cast<u8>(op.kind));
            w.Put<u8>(op.code_offset);
            w.Put<u8>(op.reg);
            w.V(op.stack_offset);
        }
    }
    w.V(block.ranges.size());
    for (const auto& [begin, end] : block.ranges) {
        w.Guest(begin);
        w.V(end - begin);
    }
    w.V(block.constants.size());
    for (const auto& c : block.constants) {
        w.Put<u64>(c[0]);
        w.Put<u64>(c[1]);
    }
    w.V(block.modules.size());
    for (const auto& m : block.modules)
        w.PutBytes({reinterpret_cast<const u8*>(m.data()), m.size()});
    return std::move(w.bytes);
}

bool DeserializePortableBlock(std::span<const u8> bytes, PortableBlock& block) {
    Reader r{bytes};
    if (r.Get<u32>() != magic || !r.ok)
        return false;
    block.descriptor = r.Get<u64>();
    r.base = block.descriptor;
    block.size = u32(r.V());
    block.code_hash = r.Get<u64>();
    const auto hot = r.GetBytes();
    const auto cold = r.GetBytes();
    block.hot.assign(hot.begin(), hot.end());
    block.cold.assign(cold.begin(), cold.end());
    block.relocs.resize(r.GetCount());
    for (auto& x : block.relocs) {
        x.site = u32(r.V());
        const u8 bits = r.Get<u8>();
        x.cold = bits & 1;
        x.size = bits >> 1;
        x.addend = s32(r.S());
        x.target = r.GetRef();
        const auto& part = x.cold ? block.cold : block.hot;
        if ((x.size != 1 && x.size != 4 && x.size != 8) || x.site > part.size() || x.size > part.size() - x.site)
            r.ok = false;
    }
    block.links.resize(r.GetCount());
    for (auto& x : block.links) {
        x.site = u32(r.V());
        const u8 type = r.Get<u8>();
        x.type = static_cast<PortableLink::Type>(type);
        x.target = r.Guest();
        const size_t n = type == 3 ? 10 : type == 2 ? 5 : 6;
        if (type > static_cast<u8>(PortableLink::Type::MovRcx) || x.site > block.hot.size() || block.hot.size() - x.site < n)
            r.ok = false;
    }
    block.fastmem.resize(r.GetCount());
    for (auto& x : block.fastmem) {
        x.location = r.GetRef();
        x.resume = r.GetRef();
        x.callback = r.GetRef();
        x.marker_descriptor = r.Guest();
        x.marker_index = u32(r.V());
        x.recompile = r.Get<u8>();
    }
    block.unwind.resize(r.GetCount());
    for (auto& x : block.unwind) {
        x.begin = u32(r.V());
        x.end = u32(r.V());
        x.inherits_run_frame = r.Get<u8>();
        x.operations.resize(r.GetCount());
        for (auto& op : x.operations) {
            op.kind = static_cast<ABIPrologue::Kind>(r.Get<u8>());
            op.code_offset = r.Get<u8>();
            op.reg = r.Get<u8>();
            op.stack_offset = static_cast<size_t>(r.V());
        }
        if (x.begin > x.end || x.end > block.hot.size())
            r.ok = false;
    }
    block.ranges.resize(r.GetCount());
    for (auto& [begin, end] : block.ranges) {
        begin = r.Guest();
        end = begin + r.V();
    }
    block.constants.resize(r.GetCount());
    for (auto& c : block.constants) {
        c[0] = r.Get<u64>();
        c[1] = r.Get<u64>();
    }
    block.modules.resize(r.GetCount());
    for (auto& m : block.modules) {
        const auto name = r.GetBytes();
        m.assign(reinterpret_cast<const char*>(name.data()), name.size());
    }
    return r.ok && r.rest.empty() && block.size <= block.hot.size();
}

std::optional<PortableBlock> DeserializePortableBlock(std::span<const u8> bytes) {
    PortableBlock block;
    if (!DeserializePortableBlock(bytes, block))
        return std::nullopt;
    return block;
}

std::vector<u8> NormalizedPortableBytes(const PortableBlock& block, bool cold) {
    std::vector<u8> bytes = cold ? block.cold : block.hot;
    for (const auto& r : block.relocs) {
        if (r.cold == cold)
            std::fill_n(bytes.begin() + r.site, r.size, u8(0));
    }
    if (!cold) {
        for (const auto& l : block.links) {
            std::fill_n(bytes.begin() + l.site, std::min(LinkSiteSize(l.type), bytes.size() - l.site), u8(0));
        }
    }
    return bytes;
}

namespace {
// Removes the unlinked exit stubs a block's translation created in its cold part
// (mov rax, pc; mov [jit+pc], rax; jmp ReturnFromRunCode). They belong to the JIT
// that made them: every reference to one is an UnlinkedExit ref, and which stubs a
// block creates, and where, depends on the stubs that JIT had already. Nothing in
// the block refers to their bytes. Returns whether any were removed.
bool StripUnlinkedExitStubs(PortableBlock& block) {
    std::vector<u32> targeted;
    const auto note = [&](const PortableRef& ref) {
        if (ref.kind == PortableRefKind::Cold)
            targeted.push_back(u32(ref.value));
    };
    for (const auto& r : block.relocs)
        note(r.target);
    for (const auto& f : block.fastmem) {
        note(f.location);
        note(f.resume);
        note(f.callback);
    }
    std::sort(targeted.begin(), targeted.end());
    const auto& c = block.cold;
    std::vector<std::pair<u32, u32>> stubs;
    for (const auto& r : block.relocs) {
        if (!r.cold || r.size != 4 || r.target.kind != PortableRefKind::Symbol || r.target.aux != u16(PortableSymbol::ReturnFromRunCode0) || r.site < 1 || c[r.site - 1] != 0xe9)
            continue;
        // mov [reg+disp8], rax (4 bytes) or [reg+disp32] (7 bytes) between the mov rax, imm64 and the jmp.
        for (const u32 store : {4u, 7u}) {
            if (r.site < 1 + store + 10)
                continue;
            const u32 begin = r.site - 1 - store - 10, mov = r.site - 1 - store;
            const u8 mod = c[mov + 2] >> 6;
            if (c[begin] == 0x48 && c[begin + 1] == 0xb8 && (c[mov] & 0xf8) == 0x48 && c[mov + 1] == 0x89 && (c[mov + 2] & 0x38) == 0 &&
                ((store == 4 && mod == 1) || (store == 7 && mod == 2)) && (c[mov + 2] & 7) != 4 &&
                !std::binary_search(targeted.begin(), targeted.end(), begin)) {
                stubs.emplace_back(begin, r.site + 4);
                break;
            }
        }
    }
    if (stubs.empty())
        return false;
    std::sort(stubs.begin(), stubs.end());
    const auto inside = [&](u32 offset) {
        const auto it = std::upper_bound(stubs.begin(), stubs.end(), std::pair{offset, ~0u});
        return it != stubs.begin() && offset < std::prev(it)->second;
    };
    const auto shifted = [&](u32 offset) {
        u32 removed = 0;
        for (const auto& [begin, end] : stubs)
            if (end <= offset)
                removed += end - begin;
        return offset - removed;
    };
    std::vector<u8> cold;
    for (u32 i = 0; i < c.size(); ++i)
        if (!inside(i))
            cold.push_back(c[i]);
    std::erase_if(block.relocs, [&](const PortableReloc& r) { return r.cold && inside(r.site); });
    const auto move = [&](PortableRef& ref) {
        if (ref.kind == PortableRefKind::Cold)
            ref.value = shifted(u32(ref.value));
    };
    for (auto& r : block.relocs) {
        if (r.cold)
            r.site = shifted(r.site);
        move(r.target);
    }
    for (auto& f : block.fastmem) {
        move(f.location);
        move(f.resume);
        move(f.callback);
    }
    block.cold = std::move(cold);
    return true;
}
}  // namespace

std::string ComparePortableBlocks(std::span<const u8> expected_bytes, std::span<const u8> actual_bytes) {
    auto expected = DeserializePortableBlock(expected_bytes), actual = DeserializePortableBlock(actual_bytes);
    if (!expected || !actual)
        return "unreadable";
    char text[256];
    // An unlinked exit stub lands in the cold part of whichever block first needs
    // it, so cold parts may differ by whole stubs; compare the rest without them.
    const bool stripped_expected = StripUnlinkedExitStubs(*expected), stripped_actual = StripUnlinkedExitStubs(*actual);
    const bool cold_variant = (stripped_expected || stripped_actual) && !std::ranges::equal(expected_bytes, actual_bytes);
    for (const bool cold : {false, true}) {
        const auto a = NormalizedPortableBytes(*expected, cold), b = NormalizedPortableBytes(*actual, cold);
        if (a.size() != b.size()) {
            std::snprintf(text, sizeof(text), "%s size %zu != %zu", cold ? "cold" : "hot", a.size(), b.size());
            return text;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i] != b[i]) {
                std::snprintf(text, sizeof(text), "%s byte %zu: %02x != %02x (context %02x %02x %02x %02x | %02x %02x %02x %02x)", cold ? "cold" : "hot", i, a[i], b[i],
                              i >= 4 ? a[i - 4] : 0, i >= 3 ? a[i - 3] : 0, i >= 2 ? a[i - 2] : 0, i >= 1 ? a[i - 1] : 0,
                              i + 1 < a.size() ? a[i + 1] : 0, i + 2 < a.size() ? a[i + 2] : 0, i + 3 < a.size() ? a[i + 3] : 0, i + 4 < a.size() ? a[i + 4] : 0);
                return text;
            }
        }
    }
    if (expected->relocs.size() != actual->relocs.size()) {
        std::snprintf(text, sizeof(text), "reloc count %zu != %zu", expected->relocs.size(), actual->relocs.size());
        return text;
    }
    // Dispatch site slots are numbered per JIT: compare their first-use order.
    std::map<u64, u64> slots_a, slots_b;
    for (size_t i = 0; i < expected->relocs.size(); ++i) {
        auto a = expected->relocs[i], b = actual->relocs[i];
        if (a.target.kind == PortableRefKind::SiteSlot && b.target.kind == PortableRefKind::SiteSlot) {
            a.target.value = slots_a.try_emplace(a.target.value >> 8, slots_a.size()).first->second << 8 | (a.target.value & 0xff);
            b.target.value = slots_b.try_emplace(b.target.value >> 8, slots_b.size()).first->second << 8 | (b.target.value & 0xff);
        }
        if (!(a == b)) {
            std::snprintf(text, sizeof(text), "reloc %zu at %s+%u: kind %u/%u aux %u/%u value %llx/%llx addend %d/%d", i, a.cold ? "cold" : "hot", a.site,
                          unsigned(a.target.kind), unsigned(b.target.kind), a.target.aux, b.target.aux,
                          static_cast<unsigned long long>(a.target.value), static_cast<unsigned long long>(b.target.value), a.addend, b.addend);
            return text;
        }
    }
    if (expected->links != actual->links)
        return "links differ";
    if (expected->fastmem != actual->fastmem)
        return "fastmem sites differ";
    if (expected->constants != actual->constants)
        return "constants differ";
    if (expected->modules != actual->modules)
        return "modules differ: " + (expected->modules.empty() ? std::string() : expected->modules[0]) + " / " + (actual->modules.empty() ? std::string() : actual->modules[0]);
    if (expected->code_hash != actual->code_hash || expected->ranges != actual->ranges)
        return "guest ranges differ";
    return cold_variant ? "~cold stubs" : std::string{};
}

namespace {
std::mutex stats_mutex;
std::map<std::string, u64>& Stats() {
    static auto* stats = new std::map<std::string, u64>;
    return *stats;
}
}  // namespace

// Every install and capture counts one of these: no lock on that path.
static std::atomic<u64> installed_count{0}, captured_count{0};

void PortableStat(const char* outcome) {
    if (outcome && !std::strcmp(outcome, "installed"))
        return void(installed_count.fetch_add(1, std::memory_order_relaxed));
    if (outcome && !std::strcmp(outcome, "captured"))
        return void(captured_count.fetch_add(1, std::memory_order_relaxed));
    std::lock_guard lock{stats_mutex};
    ++Stats()[outcome ? outcome : "?"];
}

static std::atomic<long long> install_ns{0}, install_count{0}, translate_ns{0}, translate_count{0};

void PortableTime(bool installed, long long nanoseconds) {
    (installed ? install_ns : translate_ns).fetch_add(nanoseconds, std::memory_order_relaxed);
    (installed ? install_count : translate_count).fetch_add(1, std::memory_order_relaxed);
}

std::string PortableStatsReport() {
    std::lock_guard lock{stats_mutex};
    std::string result;
    char times[160];
    std::snprintf(times, sizeof(times), "install avg %.1f us (%.2f s), translate avg %.1f us (%.2f s)",
                  install_count ? install_ns / 1e3 / install_count : 0.0, install_ns / 1e9, translate_count ? translate_ns / 1e3 / translate_count : 0.0, translate_ns / 1e9);
    result = times;
    auto stats = Stats();
    if (const u64 n = installed_count.load()) stats["installed"] += n;
    if (const u64 n = captured_count.load()) stats["captured"] += n;
    for (const auto& [outcome, count] : stats)
        result += ", " + outcome + " " + std::to_string(count);
    return result;
}

const std::vector<HostModule>& HostModules() {
    static const std::vector<HostModule> modules = [] {
        std::vector<HostModule> result;
#ifdef _WIN32
        HANDLE process = GetCurrentProcess();
        std::vector<HMODULE> handles(1024);
        DWORD needed = 0;
        if (EnumProcessModules(process, handles.data(), DWORD(handles.size() * sizeof(HMODULE)), &needed)) {
            handles.resize(std::min<size_t>(handles.size(), needed / sizeof(HMODULE)));
            for (HMODULE handle : handles) {
                MODULEINFO info{};
                wchar_t name[MAX_PATH]{};
                if (!GetModuleInformation(process, handle, &info, sizeof(info)) || !GetModuleBaseNameW(process, handle, name, MAX_PATH))
                    continue;
                std::string narrow;
                for (const wchar_t* c = name; *c; ++c)
                    narrow.push_back(*c < 128 ? static_cast<char>(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c) : '?');
                const u64 base = reinterpret_cast<u64>(info.lpBaseOfDll);
                // The image's link timestamp and size identify its build: a system
                // update moves offsets, and must not match a block captured before it.
                const auto* dos = static_cast<const IMAGE_DOS_HEADER*>(info.lpBaseOfDll);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(static_cast<const u8*>(info.lpBaseOfDll) + dos->e_lfanew);
                char identity[32];
                std::snprintf(identity, sizeof(identity), "#%08lx%08lx", static_cast<unsigned long>(nt->FileHeader.TimeDateStamp), static_cast<unsigned long>(info.SizeOfImage));
                result.push_back({std::move(narrow) + identity, base, base + info.SizeOfImage});
            }
        }
#else
        dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) {
            auto& out = *static_cast<std::vector<HostModule>*>(data);
            u64 low = ~u64(0), high = 0, identity = 0xcbf29ce484222325ull;
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                if (info->dlpi_phdr[i].p_type != PT_LOAD)
                    continue;
                for (const u64 v : {u64(info->dlpi_phdr[i].p_vaddr), u64(info->dlpi_phdr[i].p_filesz), u64(info->dlpi_phdr[i].p_memsz)})
                    identity = (identity ^ v) * 0x100000001b3ull;
                low = std::min<u64>(low, info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
                high = std::max<u64>(high, info->dlpi_addr + info->dlpi_phdr[i].p_vaddr + info->dlpi_phdr[i].p_memsz);
            }
            if (low < high) {
                std::string name = info->dlpi_name ? info->dlpi_name : "";
                if (const auto slash = name.rfind('/'); slash != std::string::npos)
                    name = name.substr(slash + 1);
                // Offsets are from the load bias, which is what module identity keeps stable.
                char suffix[24];
                std::snprintf(suffix, sizeof(suffix), "#%016llx", static_cast<unsigned long long>(identity));
                out.push_back({(name.empty() ? std::string("main") : std::move(name)) + suffix, u64(info->dlpi_addr), high});
            }
            return 0;
        },
                        &result);
#endif
        return result;
    }();
    return modules;
}

std::optional<std::pair<size_t, u64>> FindHostModule(u64 address) {
    const auto& modules = HostModules();
    for (size_t i = 0; i < modules.size(); ++i) {
        if (address >= modules[i].begin && address < modules[i].end)
            return std::pair{i, address - modules[i].begin};
    }
    return std::nullopt;
}

void PortableCapture::Reset() {
    events.clear();
    svcs.clear();
    links.clear();
    fastmem.clear();
    unwind.clear();
    unclassified.clear();
    failure = nullptr;
}

void PortableCapture::SortRanges() {
    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) {
        return a.begin != b.begin ? a.begin < b.begin : a.end < b.end;
    });
}

const PortableCapture::Range* PortableCapture::FindRange(u64 address) const {
    // Few entries; exact (one-byte) entries win over the ranges containing them.
    const Range* found = nullptr;
    for (const auto& r : ranges) {
        if (address >= r.begin && address < r.end && (!found || r.end - r.begin < found->end - found->begin))
            found = &r;
    }
    return found;
}

void PortableCapture::onAbsolute(size_t site, int size, const uint8_t* target) {
    events.push_back({Event::Type::Absolute, static_cast<u8>(size), site, reinterpret_cast<u64>(target)});
}

void PortableCapture::onLabel(size_t site, int size, size_t target) {
    events.push_back({Event::Type::Label, static_cast<u8>(size), site, target});
}

void PortableCapture::onLabelAddress(size_t site, int size, size_t target) {
    if (size != 8) {
        failure = "label address narrower than 64 bits";
        return;
    }
    events.push_back({Event::Type::LabelAddress, 8, site, target});
}

void PortableCapture::onRipDisplacement(size_t) {
    failure = "RIP-relative displacement without a target";
}

bool PortableCapture::isAddress(uint64_t value) {
    if (FindRange(value))
        return true;
    if (value > 0xffffffffull)
        unclassified.push_back(value);
    return false;
}

void PortableCapture::onImm64(size_t site, uint64_t value) {
    events.push_back({Event::Type::Imm64, 8, site, value});
}

}  // namespace Dynarmic::Backend::X64
