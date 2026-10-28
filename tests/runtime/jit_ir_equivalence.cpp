// Randomized equivalence test for Dynarmic's A64 IR optimization passes.
// Offline only: no game inputs or runtime integration.
//
// Every generated program runs from one initial state under six pass sets:
// the default (all safe optimizations), the default without
// GetSetElimination, the default without ConstProp, the default without
// LowLaneAliasing (scalar-lane IR folds and backend lane aliasing), the default
// without AddressFolding (immediate offsets in identity-fastmem operands) and the
// default without per-site fast dispatch and RSB pops (the default engine enables both;
// some body instructions are preceded by a call/return or indirect branch through
// x30), and the default with deferred handlers and unlinked block exits in the
// cold code region. Two more
// engines run the runtime's Unsafe_InaccurateNaN with and without
// AVX and are compared with each other (x86 NaN results agree between SSE and
// VEX encodings, not with the default's AArch64 NaN rules). All 31 GPRs, SP, all
// 128 bits of every vector register, NZCV, FPCR, FPSR, the 512-byte memory
// fixture and the halt location must agree. Memory traffic includes Q pairs
// and LD1/ST1/LD2/ST2 lists with pre- and post-index writeback, some followed
// by an access through the written-back base. Each program also runs in one of
// two backend memory modes: callbacks (no fastmem, the path the runtime falls
// back to after a fastmem fault such as an EL0 tagged pointer) and the
// runtime's register cache with identity fastmem, and with or without
// check_halt_on_memory_access (the runtime sets it; this Dynarmic then runs
// GetSetElimination with register writes flushed before every memory access).
// --cross-memory-mode also compares the default
// passes between the two memory modes, which finds access-path faults that all
// pass sets share (pass equivalence cannot see those).
//
// A divergence is reduced to a shortest differing program and written with IR
// dumps before and after each pass. The process exits 1 after any divergence,
// 2 on harness errors, and 3 when VENTURE_GPU_DEADLINE stops it early.
#include "jit_ir_equivalence_instructions.h"
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/frontend/A64/a64_location_descriptor.h>
#include <dynarmic/frontend/A64/translate/a64_translate.h>
#include <dynarmic/ir/basic_block.h>
#include <dynarmic/ir/opt_passes.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace jit_ir_equivalence {
using Vector = Dynarmic::A64::Vector;
using Flags = Dynarmic::OptimizationFlag;
constexpr std::uint64_t entry = 0x100000;
alignas(16) std::array<std::uint8_t, 512> active_memory{};
const std::uint64_t memory_base = reinterpret_cast<std::uintptr_t>(active_memory.data());
constexpr std::uint32_t svc = 0xd4000001;
constexpr unsigned loop_counter = 27, memory_register = 28, base_register = 26;

struct State {
    std::array<std::uint64_t, 31> x{};
    std::array<Vector, 32> v{};
    std::uint64_t sp{}, pc{}; // pc is relative to the program entry
    std::uint32_t nzcv{}, fpcr{}, fpsr{};
    std::array<std::uint8_t, 512> memory{};
    // Faults halt the program instead of unwinding through JIT frames, so a
    // miscompiled address or decode shows up as an ordinary state difference.
    bool reached_svc = true, fetched_outside = false, exception_raised = false;
    std::optional<std::uint64_t> wild_access; // first access outside the fixture
};
bool same(const State& a, const State& b) {
    return a.x == b.x && a.v == b.v && a.sp == b.sp && a.pc == b.pc && a.nzcv == b.nzcv &&
           a.fpcr == b.fpcr && a.fpsr == b.fpsr && a.memory == b.memory &&
           a.reached_svc == b.reached_svc && a.fetched_outside == b.fetched_outside &&
           a.exception_raised == b.exception_raised && a.wild_access == b.wild_access;
}
// Nonzero prefix and follow words are emitted around the word and reduced
// together with it. "add x26, x28, #offset" re-establishes the base of
// writeback and multi-register templates, so every access stays inside the
// fixture; a follow accesses memory through the written-back x26.
// control: 0 none, else a position-independent x30 branch sequence before the
// instruction (see control_words) that exercises the RSB and dispatch paths.
struct Instruction { std::uint32_t word{}; std::string name; std::uint32_t prefix = 0, follow = 0; unsigned control = 0; };
constexpr unsigned control_count = 5;
constexpr std::array<const char*, control_count> control_names{"", "bl/ret", "adr x30; blr x30; ret", "adr x30; br x30", "adr x30; ret (RSB mismatch)"};
std::vector<std::uint32_t> control_words(unsigned control) {
    switch (control) {
    case 1: return {0x94000002, 0x14000002, 0xd65f03c0};                 // bl +8; b +8; ret
    case 2: return {0x1000007e, 0xd63f03c0, 0x14000002, 0xd65f03c0};     // adr x30,+12; blr x30; b +8; ret
    case 3: return {0x1000005e, 0xd61f03c0};                             // adr x30,+8; br x30
    case 4: return {0x1000005e, 0xd65f03c0};                             // adr x30,+8; ret
    default: return {};
    }
}
struct Case {
    State initial;
    std::vector<Instruction> body;
    unsigned iterations = 0;
    bool register_cache = false;
    // The runtime sets check_halt_on_memory_access; both settings are tested.
    bool halt_on_memory = true;
};

// mov w27,#N; body; subs w27,w27,#1; b.ne body; svc #0. The body never writes
// w27 or x28, and its only branches are control sequences that continue at the
// next instruction, so every program reaches the SVC.
std::vector<std::uint32_t> code(const Case& c) {
    std::vector<std::uint32_t> words;
    if (c.iterations) words.push_back(0x52800000 | (c.iterations << 5) | loop_counter);
    for (const auto& inst : c.body) {
        for (const auto word : control_words(inst.control)) words.push_back(word);
        if (inst.prefix) words.push_back(inst.prefix);
        words.push_back(inst.word);
        if (inst.follow) words.push_back(inst.follow);
    }
    if (c.iterations) {
        const auto body_words = words.size() - 1;
        words.push_back(0x71000400 | (loop_counter << 5) | loop_counter);
        const auto offset = -static_cast<std::int32_t>(body_words + 1);
        words.push_back(0x54000001 | ((std::uint32_t(offset) & 0x7ffff) << 5));
    }
    words.push_back(svc);
    return words;
}

struct Callbacks final : Dynarmic::A64::UserCallbacks {
    std::vector<std::uint32_t> words;
    Dynarmic::A64::Jit* jit{};
    std::uint64_t base = entry, ticks = 0, code_reads = 0;
    bool fetched_outside = false, exception_raised = false;
    std::optional<std::uint64_t> wild_access;
    std::uint8_t sink[16]{};
    std::optional<std::uint32_t> MemoryReadCode(std::uint64_t pc) override {
        ++code_reads;
        if (pc < base || pc >= base + words.size() * 4 || (pc & 3)) {
            fetched_outside = true; return svc;
        }
        return words[(pc - base) / 4];
    }
    std::uint8_t* address(std::uint64_t p, std::size_t bytes) {
        if (p >= memory_base && p <= memory_base + active_memory.size() - bytes)
            return active_memory.data() + p - memory_base;
        // Generated addresses are always x28 + an in-fixture offset; anything
        // else is a miscompiled address. Record it and stop at the access.
        if (!wild_access) wild_access = p;
        jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        std::memset(sink, 0, sizeof(sink));
        return sink;
    }
    std::uint64_t MemoryRead(std::uint64_t p, std::size_t bytes) override {
        std::uint64_t value = 0; std::memcpy(&value, address(p, bytes), bytes); return value;
    }
    Vector MemoryRead128(std::uint64_t p) override {
        Vector value{}; std::memcpy(value.data(), address(p, 16), 16); return value;
    }
    void MemoryWrite(std::uint64_t p, std::uint64_t value, std::size_t bytes) override {
        std::memcpy(address(p, bytes), &value, bytes);
    }
    void MemoryWrite128(std::uint64_t p, Vector value) override {
        std::memcpy(address(p, 16), value.data(), 16);
    }
    std::uint64_t MemoryAtomic(std::uint64_t, std::size_t, Dynarmic::A64::AtomicOp,
                              std::uint64_t, std::uint64_t) override { throw std::runtime_error("unexpected atomic"); }
    Vector MemoryAtomicCompareAndSwap128(std::uint64_t, Vector, Vector) override {
        throw std::runtime_error("unexpected atomic128");
    }
    void CallSVC(std::uint32_t) override { jit->HaltExecution(); }
    void ExceptionRaised(std::uint64_t, Dynarmic::A64::Exception) override {
        exception_raised = true;
        jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
    }
    void AddTicks(std::uint64_t amount) override { ticks = amount >= ticks ? 0 : ticks - amount; }
    std::uint64_t GetTicksRemaining() override { return ticks; }
    std::uint64_t GetCNTPCT() override { return 0; }
};

struct Engine {
    Callbacks callbacks;
    Dynarmic::A64::UserConfig config;
    std::unique_ptr<Dynarmic::A64::Jit> jit;
    std::optional<std::uint64_t> last_slot;
    Engine(Flags flags, bool cache, bool halt_on_memory, bool inline_sites = true, bool inaccurate_nan = false,
           std::uint32_t disabled_host_features = 0, bool cold_region = false, bool compact_pop = false) {
        config.callbacks = &callbacks;
        config.unsafe_optimizations = inaccurate_nan;
        if (inaccurate_nan) flags = flags | Flags::Unsafe_InaccurateNaN;
        config.disabled_host_features = disabled_host_features;
        config.inline_fast_dispatch = inline_sites;
        config.inline_rsb_pop = inline_sites;
        config.dispatch_site_cache = inline_sites;
        config.svc_return_link = inline_sites;
        config.compact_rsb_pop = compact_pop;
        config.host_return_stack = inline_sites && !compact_pop;
        config.nzcv_exit_liveness = inline_sites;
        const auto* bounded = std::getenv("VENTURE_JIT_BOUNDED_HALT");
        config.enable_bounded_halt_polling = bounded && std::string_view(bounded) == "1";
        if (config.enable_bounded_halt_polling) config.enable_cycle_counting = false;
        config.optimizations = flags;
        config.enable_register_cache = cache;
        config.check_halt_on_memory_access = halt_on_memory;
        config.code_cache_size = 32 * 1024 * 1024;
        config.cold_code_region = cold_region;
        config.compact_block_links = cold_region;
        if (cold_region) config.block_alignment = 1;
        if (cache) {
            // The register cache ABI requires identity fastmem (as in the runtime).
            config.fastmem_pointer = 0;
            config.fastmem_address_space_bits = 64;
            config.silently_mirror_fastmem = false;
        }
        jit = std::make_unique<Dynarmic::A64::Jit>(config);
        callbacks.jit = jit.get();
    }
    State run(const Case& c, std::uint64_t slot) {
        callbacks.words = code(c);
        active_memory = c.initial.memory;
        callbacks.ticks = 10000;
        callbacks.code_reads = 0;
        callbacks.fetched_outside = false;
        callbacks.exception_raised = false;
        callbacks.wild_access.reset();
        // Every run starts at a fresh entry PC, so no translation of an earlier
        // program can be reused, and a full clear when the slot sequence wraps
        // recycles the PC range (clearing the 16 MiB fast dispatch table per run
        // dominated the run time). All engines of one comparison share the slot:
        // control sequences leave absolute PCs in x30. Reset() clears the halt
        // reasons, including the CacheInvalidation request that ClearCache()
        // posts, so it must come first: the reverse order silently reuses an
        // earlier translation.
        callbacks.base = entry + slot * 0x1000;
        jit->Reset();
        if (last_slot && slot <= *last_slot) jit->ClearCache();
        last_slot = slot;
        jit->SetRegisters(c.initial.x); jit->SetVectors(c.initial.v);
        jit->SetSP(c.initial.sp); jit->SetPC(callbacks.base);
        jit->SetPstate(c.initial.nzcv); jit->SetFpcr(c.initial.fpcr); jit->SetFpsr(c.initial.fpsr);
        const auto halt = jit->Run();
        if (callbacks.code_reads == 0) throw std::runtime_error("program ran without being translated");
        State s;
        s.x = jit->GetRegisters(); s.v = jit->GetVectors(); s.sp = jit->GetSP(); s.pc = jit->GetPC() - callbacks.base;
        s.nzcv = jit->GetPstate(); s.fpcr = jit->GetFpcr(); s.fpsr = jit->GetFpsr();
        s.memory = active_memory;
        s.fetched_outside = callbacks.fetched_outside;
        s.exception_raised = callbacks.exception_raised;
        s.wild_access = callbacks.wild_access;
        s.reached_svc = !callbacks.fetched_outside && !s.exception_raised && !s.wild_access &&
                        Dynarmic::Has(halt, Dynarmic::HaltReason::UserDefined1) &&
                        s.pc == callbacks.words.size() * 4;
        return s;
    }
};

std::string difference(const State& a, const State& b) {
    std::ostringstream out; out << std::hex;
    if (a.reached_svc != b.reached_svc) out << "reached SVC: " << a.reached_svc << " != " << b.reached_svc << '\n';
    if (a.fetched_outside != b.fetched_outside) out << "fetched outside program: " << a.fetched_outside << " != " << b.fetched_outside << '\n';
    if (a.exception_raised != b.exception_raised) out << "exception raised: " << a.exception_raised << " != " << b.exception_raised << '\n';
    if (a.wild_access != b.wild_access) {
        const auto show = [&](const std::optional<std::uint64_t>& w) {
            if (!w) return std::string("none");
            std::ostringstream s; s << "0x" << std::hex << *w << " (fixture" << (*w >= memory_base ? "+0x" : "-0x") << std::hex
                                    << (*w >= memory_base ? *w - memory_base : memory_base - *w) << ')';
            return s.str();
        };
        out << "access outside fixture: " << show(a.wild_access) << " != " << show(b.wild_access) << '\n';
    }
    for (unsigned i = 0; i != a.x.size(); ++i)
        if (a.x[i] != b.x[i]) out << "x" << std::dec << i << std::hex << ": " << a.x[i] << " != " << b.x[i] << '\n';
    for (unsigned i = 0; i != a.v.size(); ++i)
        if (a.v[i] != b.v[i]) out << "v" << std::dec << i << std::hex << ": " << std::setfill('0') << std::setw(16) << a.v[i][1] << ':'
                                 << std::setw(16) << a.v[i][0] << " != " << std::setw(16) << b.v[i][1] << ':' << std::setw(16) << b.v[i][0]
                                 << std::setfill(' ') << '\n';
    if (a.sp != b.sp) out << "SP: " << a.sp << " != " << b.sp << '\n';
    if (a.pc != b.pc) out << "PC (entry offset): " << a.pc << " != " << b.pc << '\n';
    if (a.nzcv != b.nzcv) out << "NZCV: " << a.nzcv << " != " << b.nzcv << '\n';
    if (a.fpcr != b.fpcr) out << "FPCR: " << a.fpcr << " != " << b.fpcr << '\n';
    if (a.fpsr != b.fpsr) out << "FPSR: " << a.fpsr << " != " << b.fpsr << '\n';
    for (unsigned i = 0; i < a.memory.size(); i += 8) {
        std::uint64_t left, right;
        std::memcpy(&left, a.memory.data() + i, 8); std::memcpy(&right, b.memory.data() + i, 8);
        if (left != right) out << "memory+" << std::setw(3) << i << " (u64 LE): " << std::setfill('0') << std::setw(16) << left
                               << " != " << std::setw(16) << right << std::setfill(' ') << '\n';
    }
    return out.str();
}

constexpr unsigned config_count = 10;
constexpr std::array<const char*, config_count> config_names{"default", "no-GetSetElimination", "no-ConstProp", "no-LowLaneAliasing",
                                                             "no-AddressFolding", "no-InlineSites", "inaccurate-nan", "inaccurate-nan-no-AVX",
                                                             "cold-region", "compact-rsb-pop"};
// Backend::X64::HostFeature AVX, AVX2 and AVX512F-AVX512VBMI: the SSE encodings.
constexpr std::uint32_t vex_features = 0x0ff8;
constexpr unsigned inaccurate_nan = 6;
// Each configuration is compared with its base; the inaccurate-NaN pair with each other.
constexpr unsigned base_of(unsigned peer) { return peer == inaccurate_nan + 1 ? inaccurate_nan : 0; }
constexpr bool is_peer(unsigned config) { return config != 0 && config != inaccurate_nan; }
// Fused multiply-add and reciprocal steps take other host paths (or a fallback)
// without AVX, with their own rounding and NaN results; the pair skips them.
bool compared(const Case& c, unsigned peer) {
    if (!is_peer(peer)) return false;
    if (peer != inaccurate_nan + 1) return true;
    static constexpr const char* fused[]{"fmla", "fmls", "fmadd", "fmsub", "fnmadd", "fnmsub", "frecps", "frsqrts"};
    return std::none_of(c.body.begin(), c.body.end(), [](const Instruction& i) {
        return std::any_of(std::begin(fused), std::end(fused), [&](const char* f) { return i.name.find(f) != std::string::npos; });
    });
}
struct Harness {
    std::array<std::array<std::array<std::unique_ptr<Engine>, config_count>, 2>, 2> engines; // [cache][halt][config]
    Harness() {
        for (unsigned cache = 0; cache != 2; ++cache) {
            for (unsigned halt = 0; halt != 2; ++halt) {
                engines[cache][halt][0] = std::make_unique<Engine>(Dynarmic::all_safe_optimizations, cache, halt);
                engines[cache][halt][1] = std::make_unique<Engine>(Dynarmic::all_safe_optimizations & ~Flags::GetSetElimination, cache, halt);
                engines[cache][halt][2] = std::make_unique<Engine>(Dynarmic::all_safe_optimizations & ~Flags::ConstProp, cache, halt);
                // The scalar-lane lowering (IR fold plus backend aliasing) against the plain lowering.
                engines[cache][halt][3] = std::make_unique<Engine>(Dynarmic::all_safe_optimizations & ~Flags::LowLaneAliasing, cache, halt);
                // Folded address operands, including their fault paths, against the separate add.
                engines[cache][halt][4] = std::make_unique<Engine>(Dynarmic::all_safe_optimizations & ~Flags::AddressFolding, cache, halt);
                // Shared dispatch and RSB-pop handlers against per-site copies.
                engines[cache][halt][5] = std::make_unique<Engine>(Dynarmic::all_safe_optimizations, cache, halt, false);
                // VEX three-operand FP forms against SSE, both with the runtime's NaN handling.
                engines[cache][halt][6] = std::make_unique<Engine>(Dynarmic::all_safe_optimizations, cache, halt, true, true);
                engines[cache][halt][7] = std::make_unique<Engine>(Dynarmic::all_safe_optimizations, cache, halt, true, true, vex_features);
                // Deferred handlers (fastmem fallbacks, NaN and abort paths) and unlinked
                // block exits in the cold region; hot links keep only the patchable jump
                // and blocks are packed without entry padding.
                engines[cache][halt][8] = std::make_unique<Engine>(Dynarmic::all_safe_optimizations, cache, halt, true, false, 0, true);
                // Returns through the shared lookup that each site calls and then jumps on.
                engines[cache][halt][9] = std::make_unique<Engine>(Dynarmic::all_safe_optimizations, cache, halt, true, false, 0, false, true);
            }
        }
    }
    // One fresh entry slot per comparison, shared by every engine in it.
    std::uint64_t runs = 0, slot = 0;
    std::uint64_t next_slot() { return slot = runs++ % 65536; }
    std::array<State, config_count> run(const Case& c) {
        auto& e = engines[c.register_cache][c.halt_on_memory];
        const auto s = next_slot();
        return {e[0]->run(c, s), e[1]->run(c, s), e[2]->run(c, s), e[3]->run(c, s), e[4]->run(c, s), e[5]->run(c, s),
                e[6]->run(c, s), e[7]->run(c, s), e[8]->run(c, s), e[9]->run(c, s)};
    }
    // Peer cross_mode compares the default passes between the two memory modes:
    // a backend fault on one access path, even when every pass set shares it.
    static constexpr unsigned cross_mode = config_count;
    std::array<State, 2> run_both_modes(const Case& c) {
        const auto s = next_slot();
        return {engines[0][c.halt_on_memory][0]->run(c, s), engines[1][c.halt_on_memory][0]->run(c, s)};
    }
    bool differs(const Case& c, unsigned peer) {
        if (peer == cross_mode) { const auto r = run_both_modes(c); return !same(r[0], r[1]); }
        auto& e = engines[c.register_cache][c.halt_on_memory];
        const auto s = next_slot();
        return !same(e[base_of(peer)]->run(c, s), e[peer]->run(c, s));
    }
    Case reduce(Case c, unsigned peer) {
        if (c.iterations) {
            auto once = c; once.iterations = 0;
            if (differs(once, peer)) c = std::move(once);
        }
        // Chunk deletion followed by single-instruction deletion. Loop control
        // is rebuilt for every trial, so deletions cannot corrupt branch targets.
        for (std::size_t chunk = c.body.size(); chunk != 0; chunk /= 2) {
            for (std::size_t i = 0; i + chunk <= c.body.size();) {
                auto trial = c;
                trial.body.erase(trial.body.begin() + i, trial.body.begin() + i + chunk);
                if (differs(trial, peer)) { c = std::move(trial); i = 0; }
                else ++i;
            }
        }
        // Search every smaller subsequence of a small reduced program. This
        // establishes a shortest subsequence, not just a greedy local minimum.
        if (c.body.size() <= 12) {
            const auto original = c;
            for (std::size_t length = 0; length < original.body.size(); ++length) {
                for (unsigned mask = 0; mask < (1u << original.body.size()); ++mask) {
                    if (std::size_t(std::popcount(mask)) != length) continue;
                    auto trial = original; trial.body.clear();
                    for (unsigned i = 0; i != original.body.size(); ++i)
                        if (mask & (1u << i)) trial.body.push_back(original.body[i]);
                    if (differs(trial, peer)) return trial;
                }
            }
        }
        return c;
    }
};

std::uint64_t mix(std::uint64_t v) {
    v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9;
    v = (v ^ (v >> 27)) * 0x94d049bb133111eb;
    return v ^ (v >> 31);
}
struct Coverage {
    std::array<std::uint64_t, instruction_count> templates{};
    std::array<std::uint64_t, categories.size()> category_counts{};
    std::array<std::uint64_t, 8> shapes{};
    std::array<std::uint64_t, 32> fpcr{};
    std::array<std::uint64_t, 2> cache{}, halt{};
    std::uint64_t joint[32][2][2][8]{}; // FPCR, cache, halt, shape
    std::array<std::array<std::uint64_t, 2>, config_count> mismatches{}; // [peer][register cache]
    std::map<std::string, std::uint64_t> classes; // "peer mode: differing components"
    std::uint64_t loops = 0, programs = 0, instructions = 0, follows = 0;
    std::array<std::uint64_t, control_count> controls{};
};
// Which state components differ, e.g. "memory" or "gpr+fpsr".
std::string difference_class(const State& a, const State& b) {
    std::string k;
    const auto add = [&](bool differs, const char* name) { if (differs) { if (!k.empty()) k += '+'; k += name; } };
    add(a.reached_svc != b.reached_svc || a.fetched_outside != b.fetched_outside ||
        a.exception_raised != b.exception_raised || a.wild_access != b.wild_access, "control");
    add(a.pc != b.pc, "pc"); add(a.x != b.x, "gpr"); add(a.sp != b.sp, "sp"); add(a.v != b.v, "vector");
    add(a.memory != b.memory, "memory"); add(a.nzcv != b.nzcv, "nzcv"); add(a.fpcr != b.fpcr, "fpcr");
    add(a.fpsr != b.fpsr, "fpsr");
    return k;
}
// Templates the generator may pick (--exclude-template narrows a hunt).
std::vector<bool>& excluded_templates() { static std::vector<bool> v(instruction_count); return v; }
const std::array<std::vector<std::size_t>, categories.size()>& by_category() {
    static const auto table = [] {
        std::array<std::vector<std::size_t>, categories.size()> t;
        for (std::size_t i = 0; i != instruction_count; ++i) t[instructions[i].category].push_back(i);
        for (const auto& list : t) if (list.empty()) throw std::runtime_error("empty instruction category");
        return t;
    }();
    return table;
}
static_assert([] { unsigned sum = 0; for (auto w : category_weights) sum += w; return sum; }() == 100);

Case generate(std::uint64_t seed, std::uint64_t index, Coverage& coverage, int memory_mode, int halt_mode) {
    std::mt19937_64 rng(mix(seed + index * 0x9e3779b97f4a7c15));
    Case c;
    c.register_cache = memory_mode < 0 ? (index >> 8) & 1 : memory_mode;
    // Shape uses bits 0-2, FPCR 3-7 and cache 8. Keep halt independent
    // so AHP-enabled programs also exercise the shipped memory halt checks.
    c.halt_on_memory = halt_mode < 0 ? !((index >> 9) & 1) : halt_mode;
    c.initial.pc = entry; c.initial.sp = 0x800000;
    c.initial.nzcv = (rng() & 15) << 28;
    // FPCR RMode (22-23), FZ (24), DN (25) and AHP (26): all 32 combinations.
    const unsigned fpcr_index = (index / 8) % 32;
    c.initial.fpcr = fpcr_index << 22;
    c.initial.fpsr = rng() & 0x0800009f;
    for (auto& x : c.initial.x) x = rng();
    c.initial.x[memory_register] = memory_base;
    constexpr std::array<std::uint32_t, 16> float_edges{
        0, 0x80000000, 1, 0x007fffff, 0x00800000, 0x3f800000, 0xbf800000,
        0x3f000000, 0x7f7fffff, 0x7f800000, 0xff800000, 0x7fc00001,
        0x7f800001, 0xffc01234, 0x4b000001, 0x33800000};
    constexpr std::array<std::uint64_t, 12> double_edges{
        0, 0x8000000000000000, 1, 0x000fffffffffffff, 0x0010000000000000,
        0x3ff0000000000000, 0xbff0000000000000, 0x7fefffffffffffff,
        0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000001, 0x7ff0000000000001};
    for (auto& v : c.initial.v) for (auto& lane : v) {
        switch (rng() % 3) {
        case 0: lane = rng(); break;
        case 1: lane = (std::uint64_t(float_edges[rng() % float_edges.size()]) << 32) |
                      float_edges[rng() % float_edges.size()]; break;
        default: lane = double_edges[rng() % double_edges.size()]; break;
        }
    }
    for (auto& byte : c.initial.memory) byte = std::uint8_t(rng());
    // Three of four register choices fall in a window of four "hot" registers
    // so values are written and re-read at mixed widths within one block.
    const unsigned hot = rng() % 24;
    const auto reg = [&](bool general) {
        if (rng() % 4) return unsigned((hot + rng() % 4) % 24);
        const unsigned r = rng() % (general ? 29 : 32);
        return general && r >= loop_counter ? r + 2 : r; // skip w27/x28, never 31
    };
    const auto append = [&](std::size_t t, bool force_destination = false) {
        const auto& item = instructions[t]; auto word = item.word;
        const unsigned d = force_destination ? hot : reg(item.gpr_fields & kRd);
        const unsigned n = reg(item.gpr_fields & kRn);
        const unsigned m = reg(item.gpr_fields & kRm);
        unsigned a = reg(item.gpr_fields & kRa);
        if (item.fields & kDistinctRtRt2) while (a == d) a = reg(item.gpr_fields & kRa);
        for (auto [bit, shift, r] : {std::array<unsigned, 3>{kRd, 0, d}, {kRn, 5, n}, {kRm, 16, m}, {kRa, 10, a}})
            if (item.fields & bit) word = (word & ~(31u << shift)) | (r << shift);
        if (item.fields & kCond) word |= unsigned(rng() % 16) << 12;
        if (item.fields & kImm12Small) word = (word & ~(4095u << 10)) | (unsigned(rng() % 32) << 10);
        if (item.fields & kMemImm12) word |= ((unsigned(rng() % 30) * 16 / item.memory_size) << 10);
        if (item.fields & kMovVectorAlias) word |= n << 16;
        if (item.fields & kImm16Small) word = (word & ~(65535u << 5)) | (unsigned(rng() % 8) << 5);
        if (item.fields & kPairImm7) {
            const unsigned size = item.memory_size;
            word |= unsigned(rng() % std::min(64u, (512 - 2 * size) / size + 1)) << 15;
        }
        if (item.fields & kImm9) word |= unsigned(rng() % 256) << 12;
        if (item.fields & kNzcvImm) word |= unsigned(rng() % 16);
        Instruction inst{word, item.assembly};
        if (item.fields & kBasePrefix) {
            // Half of the immediate-writeback forms are followed by an access
            // through the written-back base. The base offset keeps both
            // accesses inside the fixture.
            struct Follow { std::uint32_t word; int size; bool pair; const char* name; };
            static constexpr std::array<Follow, 4> follows{{{0x3dc00340, 16, false, "ldr q0, [x26]"},
                {0x3d800340, 16, false, "str q0, [x26]"}, {0xad400740, 32, true, "ldp q0, q1, [x26]"},
                {0x4c00af40, 32, false, "st1 {v0.2d, v1.2d}, [x26]"}}};
            const Follow* follow = item.writeback && rng() % 2 ? &follows[rng() % follows.size()] : nullptr;
            const int size = int(item.memory_size), adj = item.access_offset, wb = item.writeback;
            int lo = std::max(0, -adj), hi = 512 - size - adj;
            if (follow) { lo = std::max(lo, -wb); hi = std::min(hi, 512 - follow->size - wb); }
            const int offset = lo + 16 * int(rng() % unsigned((hi - lo) / 16 + 1));
            inst.prefix = 0x91000000 | (unsigned(offset) << 10) | (memory_register << 5) | base_register;
            if (follow) {
                const unsigned t1 = reg(false);
                unsigned t2 = reg(false);
                while (follow->pair && t2 == t1) t2 = reg(false);
                inst.follow = (follow->word & ~31u) | t1;
                if (follow->pair) inst.follow = (inst.follow & ~(31u << 10)) | (t2 << 10);
                inst.name += std::string(" ; ") + follow->name;
                ++coverage.follows;
            }
        }
        // One in eight instructions follows a call/return or indirect branch.
        if (rng() % 8 == 0) {
            inst.control = 1 + unsigned(rng() % (control_count - 1));
            inst.name = std::string(control_names[inst.control]) + " ; " + inst.name;
            ++coverage.controls[inst.control];
        }
        c.body.push_back(inst);
        ++coverage.templates[t]; ++coverage.category_counts[item.category];
    };
    const auto find = [](const char* name) {
        for (std::size_t t = 0; t != instruction_count; ++t)
            if (std::strcmp(instructions[t].assembly, name) == 0) return t;
        throw std::runtime_error("missing instruction template");
    };
    const auto pick = [&] {
        unsigned r = rng() % 100, category = 0;
        while (r >= category_weights[category]) r -= category_weights[category++];
        const auto& list = by_category()[category];
        for (;;) if (const auto t = list[rng() % list.size()]; !excluded_templates()[t]) return t;
    };
    // Shapes 1-6 start with a narrow write of the hot register followed by a
    // wider read, then continue randomly; shape 7 wraps the body in a loop.
    const unsigned shape = index % 8;
    switch (shape) {
    case 1: append(find("fmov s0, s0"), true); append(find("str q0, [x28]"), true); break;
    case 2: append(find("fmov d0, d0"), true); append(find("fmov s0, s0"), true); append(find("str d0, [x28]"), true); break;
    case 3: append(find("mov v0.16b, v0.16b"), true); append(find("fmov s0, w0"), true); append(find("str q0, [x28]"), true); break;
    case 4: append(find("ins v0.d[1], x0"), true); append(find("fmov d0, d0"), true); append(find("str q0, [x28]"), true); break;
    case 5: append(find("ldr s0, [x28]"), true); append(find("str q0, [x28]"), true); break;
    case 6: append(find("fadd s0, s0, s0"), true); append(find("fmul s0, s0, s0"), true); append(find("fcmp s0, s0")); append(find("fcsel s0, s0, s0, eq"), true); break;
    default: break;
    }
    const auto count = 8 + rng() % 25;
    while (c.body.size() < count) append(pick());
    if (shape == 7) { c.iterations = 2 + rng() % 3; ++coverage.loops; }
    ++coverage.shapes[shape]; ++coverage.fpcr[fpcr_index]; ++coverage.cache[c.register_cache]; ++coverage.halt[c.halt_on_memory]; ++coverage.programs;
    ++coverage.joint[fpcr_index][c.register_cache][c.halt_on_memory][shape];
    coverage.instructions += c.body.size();
    return c;
}

// V4 adds a control sequence per instruction, V3 prefix and follow words, V2 the
// halt setting; V1 always halted on memory access.
constexpr const char* case_header = "VENTURE_JIT_IR_EQUIVALENCE_V4";
constexpr const char* case_header_v3 = "VENTURE_JIT_IR_EQUIVALENCE_V3";
constexpr const char* case_header_v2 = "VENTURE_JIT_IR_EQUIVALENCE_V2";
constexpr const char* case_header_v1 = "VENTURE_JIT_IR_EQUIVALENCE_V1";
void save(const Case& c, const std::filesystem::path& path) {
    std::ofstream out(path);
    out << case_header << '\n' << c.iterations << ' ' << c.register_cache << ' ' << c.halt_on_memory << ' ' << c.body.size() << '\n' << std::hex;
    for (const auto& inst : c.body)
        out << inst.word << ' ' << inst.prefix << ' ' << inst.follow << ' ' << inst.control << ' ' << std::quoted(inst.name) << '\n';
    for (auto x : c.initial.x) out << x << '\n';
    for (auto v : c.initial.v) out << v[0] << ' ' << v[1] << '\n';
    out << c.initial.sp << ' ' << c.initial.pc << ' ' << c.initial.nzcv << ' ' << c.initial.fpcr << ' ' << c.initial.fpsr << '\n';
    for (auto byte : c.initial.memory) out << unsigned(byte) << ' ';
    out << '\n';
    if (!out) throw std::runtime_error("cannot save case");
}
Case load(const std::filesystem::path& path) {
    Case c; std::ifstream in(path); std::string header; unsigned count = 0;
    in >> header >> c.iterations >> c.register_cache;
    const bool v4 = header == case_header, v3 = v4 || header == case_header_v3;
    if (v3 || header == case_header_v2) in >> c.halt_on_memory;
    else if (header != case_header_v1) throw std::runtime_error("invalid case file");
    in >> count;
    if (count > 128 || c.iterations > 4) throw std::runtime_error("invalid case file");
    in >> std::hex;
    for (unsigned i = 0; i != count; ++i) {
        Instruction inst; in >> inst.word;
        if (v3) in >> inst.prefix >> inst.follow;
        if (v4) in >> inst.control;
        if (inst.control >= control_count) throw std::runtime_error("invalid case file");
        in >> std::quoted(inst.name); c.body.push_back(inst);
    }
    for (auto& x : c.initial.x) in >> x;
    for (auto& v : c.initial.v) in >> v[0] >> v[1];
    in >> c.initial.sp >> c.initial.pc >> c.initial.nzcv >> c.initial.fpcr >> c.initial.fpsr;
    for (auto& byte : c.initial.memory) { unsigned value; in >> value; byte = std::uint8_t(value); }
    if (!in) throw std::runtime_error("invalid case state");
    // Only x28 holds a fixture address and no generated instruction writes it.
    // Relocate that pointer for replay in a fresh process.
    c.initial.x[memory_register] = memory_base;
    return c;
}
// Translates each block the program executes and dumps its IR with the given
// passes, through the same Optimize entry point the JIT uses.
void dump_ir(const Case& c, Flags flags, const std::filesystem::path& path, bool optimize) {
    Callbacks callbacks; callbacks.words = code(c);
    Dynarmic::A64::UserConfig config; config.callbacks = &callbacks; config.optimizations = flags;
    config.check_halt_on_memory_access = c.halt_on_memory;
    std::vector<std::uint64_t> blocks{entry};
    if (c.iterations) blocks.insert(blocks.end(), {entry + 4, entry + (callbacks.words.size() - 1) * 4});
    std::ofstream out(path);
    out << "# passes: " << (optimize ? "Optimize() with flags 0x" : "none (translation only), flags 0x")
        << std::hex << static_cast<std::uint32_t>(flags) << std::dec
        << " check_halt_on_memory_access=" << c.halt_on_memory << '\n';
    for (const std::uint64_t pc : blocks) {
        Dynarmic::A64::LocationDescriptor location(pc, Dynarmic::FP::FPCR(c.initial.fpcr));
        Dynarmic::IR::Block ir(location);
        Dynarmic::A64::Translate(ir, location, [&](auto p) { return callbacks.MemoryReadCode(p); }, {});
        if (optimize) Dynarmic::Optimization::Optimize(ir, config, {.vector_multiply_widen = true});
        out << Dynarmic::IR::DumpBlock(ir) << '\n';
    }
}
void write_assembly(const Case& c, std::ostream& out) {
    out << ".text\n";
    const auto words = code(c);
    // Name each body word; loop control and the SVC stay unnamed.
    std::vector<std::string> names(words.size());
    std::size_t at = c.iterations ? 1 : 0;
    for (const auto& inst : c.body) {
        for (std::size_t i = 0; i != control_words(inst.control).size(); ++i) names[at++] = "control for: " + inst.name;
        if (inst.prefix) names[at++] = "base for: " + inst.name;
        names[at++] = "template: " + inst.name;
        if (inst.follow) names[at++] = "follow of: " + inst.name;
    }
    for (std::size_t i = 0; i != words.size(); ++i) {
        out << ".inst 0x" << std::hex << std::setfill('0') << std::setw(8) << words[i] << std::dec << std::setfill(' ');
        if (!names[i].empty()) out << "  // " << names[i];
        out << '\n';
    }
}
std::string signature(const Case& c, unsigned peer) {
    std::string s = std::to_string(peer) + '/' + std::to_string(c.register_cache) + '/' + std::to_string(c.halt_on_memory) + '/' + std::to_string(c.iterations != 0);
    for (const auto& inst : c.body) s += '|' + inst.name;
    return s;
}
Case report(Harness& h, const Case& c, unsigned peer, const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    save(c, directory / "original.case");
    const auto original_outputs = h.run(c);
    const auto reduced = h.reduce(c, peer);
    save(reduced, directory / "reduced.case");
    const auto outputs = h.run(reduced);
    const unsigned base = base_of(peer);
    std::ofstream summary(directory / "difference.txt");
    summary << config_names[base] << " vs " << config_names[peer] << '\n'
            << "memory mode: " << (reduced.register_cache ? "register cache + identity fastmem" : "callbacks (no fastmem)") << '\n'
            << "check_halt_on_memory_access: " << reduced.halt_on_memory << '\n'
            << "original body=" << c.body.size() << " reduced body=" << reduced.body.size()
            << " loop iterations=" << reduced.iterations << '\n'
            << "reduced program difference:\n" << difference(outputs[base], outputs[peer])
            << "original program difference:\n" << difference(original_outputs[base], original_outputs[peer]);
    // Group every pass combination by final state to show which pass alone
    // reproduces the divergence.
    std::vector<std::pair<std::string, State>> results{
        {"default", outputs[0]}, {"no-GetSetElimination", outputs[1]}, {"no-ConstProp", outputs[2]}, {"no-LowLaneAliasing", outputs[3]},
        {"no-AddressFolding", outputs[4]}, {"no-InlineSites", outputs[5]}, {"inaccurate-nan", outputs[6]},
        {"inaccurate-nan-no-AVX", outputs[7]}, {"cold-region", outputs[8]}, {"compact-rsb-pop", outputs[9]}};
    for (auto [flags, name] : {std::pair{Dynarmic::no_optimizations, "no-passes"},
                              {Flags::GetSetElimination, "GetSetElimination-only"}, {Flags::ConstProp, "ConstProp-only"},
                              {Dynarmic::all_safe_optimizations & ~(Flags::GetSetElimination | Flags::ConstProp), "default-without-both"}}) {
        Engine engine(flags, reduced.register_cache, reduced.halt_on_memory);
        results.emplace_back(name, engine.run(reduced, h.slot)); // the slot of outputs
    }
    summary << "final-state groups:\n";
    std::vector<const State*> groups;
    for (const auto& [name, state] : results) {
        auto it = std::find_if(groups.begin(), groups.end(), [&](const State* g) { return same(*g, state); });
        if (it == groups.end()) { groups.push_back(&state); it = groups.end() - 1; }
        summary << "  " << char('A' + (it - groups.begin())) << ' ' << name << '\n';
    }
    std::ofstream assembly(directory / "reduced.s"); write_assembly(reduced, assembly);
    dump_ir(reduced, Dynarmic::no_optimizations, directory / "before.ir", false);
    dump_ir(reduced, Dynarmic::no_optimizations, directory / "no-passes.ir", true);
    dump_ir(reduced, Flags::GetSetElimination, directory / "getset-only.ir", true);
    dump_ir(reduced, Flags::ConstProp, directory / "constprop-only.ir", true);
    dump_ir(reduced, Dynarmic::all_safe_optimizations, directory / "default.ir", true);
    dump_ir(reduced, Dynarmic::all_safe_optimizations & ~Flags::GetSetElimination, directory / "no-getset.ir", true);
    dump_ir(reduced, Dynarmic::all_safe_optimizations & ~Flags::ConstProp, directory / "no-constprop.ir", true);
    dump_ir(reduced, Dynarmic::all_safe_optimizations & ~Flags::LowLaneAliasing, directory / "no-low-lane.ir", true);
    dump_ir(reduced, Dynarmic::all_safe_optimizations & ~Flags::AddressFolding, directory / "no-address-folding.ir", true);
    std::cout << "DIVERGENCE " << config_names[base] << " vs " << config_names[peer] << " (memory mode "
              << reduced.register_cache << ", halt on memory " << reduced.halt_on_memory << "), reduced " << c.body.size() << " -> " << reduced.body.size()
              << " body instructions, written to " << directory.string() << '\n'
              << difference(outputs[base], outputs[peer]) << std::flush;
    return reduced;
}
unsigned coverage_report(const Coverage& c, std::uint64_t seed, std::uint64_t start, std::uint64_t completed,
                     const std::filesystem::path& output) {
    std::ofstream out(output / "coverage.txt");
    out << "seed=0x" << std::hex << seed << std::dec << " start=" << start << " completed=" << completed
        << " programs=" << c.programs << " runs-per-configuration=" << c.programs
        << " body-instructions=" << c.instructions << " loops=" << c.loops
        << " memory-mode callbacks/register-cache=" << c.cache[0] << '/' << c.cache[1]
        << " halt-on-memory off/on=" << c.halt[0] << '/' << c.halt[1]
        << " writeback-follows=" << c.follows << " controls=" << c.controls[1] << '/' << c.controls[2] << '/'
        << c.controls[3] << '/' << c.controls[4] << '\n';
    for (unsigned peer = 1; peer != config_count; ++peer)
        if (is_peer(peer))
            out << "mismatches " << config_names[base_of(peer)] << " vs " << config_names[peer] << " callbacks/register-cache="
                << c.mismatches[peer][0] << '/' << c.mismatches[peer][1] << '\n';
    for (const auto& [key, n] : c.classes) out << "divergence-class " << key << '=' << n << '\n';
    for (unsigned i = 0; i != categories.size(); ++i)
        out << "category " << categories[i] << '=' << c.category_counts[i] << '\n';
    for (unsigned i = 0; i != c.shapes.size(); ++i) out << "shape " << i << '=' << c.shapes[i] << '\n';
    for (unsigned i = 0; i != c.fpcr.size(); ++i) out << "FPCR-combination " << i << '=' << c.fpcr[i] << '\n';
    unsigned covered = 0;
    for (unsigned fpcr = 0; fpcr != 32; ++fpcr)
        for (unsigned cache = 0; cache != 2; ++cache)
            for (unsigned halt = 0; halt != 2; ++halt)
                for (unsigned shape = 0; shape != 8; ++shape) {
                    const auto count = c.joint[fpcr][cache][halt][shape];
                    covered += count != 0;
                    out << "joint FPCR/cache/halt/shape " << fpcr << '/' << cache << '/' << halt << '/' << shape << '=' << count << '\n';
                }
    out << "joint-covered=" << covered << "/1024\n";
    for (unsigned i = 0; i != instruction_count; ++i)
        out << "template " << instructions[i].assembly << '=' << c.templates[i] << '\n';
    return covered;
}
} // namespace jit_ir_equivalence

int main(int argc, char** argv) {
    using namespace jit_ir_equivalence;
    std::filesystem::path output = "jit-ir-equivalence";
    std::optional<Case> current;
    std::uint64_t current_index = 0;
    try {
        std::uint64_t seed = 0x495246555a5a, count = 128, start = 0, max_reports = 4;
        int memory_mode = -1; // alternate every 256 programs
        int halt_mode = -1;   // alternate every 512 programs, independently of FPCR/cache/shape
        bool cross = false;   // also compare the default passes across memory modes
        std::filesystem::path replay, assembly_path;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--long") { count = 1000000; continue; }
            if (arg == "--cross-memory-mode") { cross = true; continue; }
            if (++i == argc) throw std::runtime_error("missing option value");
            if (arg == "--seed") seed = std::stoull(argv[i], nullptr, 0);
            else if (arg == "--count") count = std::stoull(argv[i], nullptr, 0);
            else if (arg == "--start") start = std::stoull(argv[i], nullptr, 0);
            else if (arg == "--output") output = argv[i];
            else if (arg == "--replay") replay = argv[i];
            else if (arg == "--max-reports") max_reports = std::stoull(argv[i], nullptr, 0);
            else if (arg == "--memory-mode") {
                const std::string mode = argv[i];
                if (mode == "callbacks") memory_mode = 0;
                else if (mode == "register-cache") memory_mode = 1;
                else if (mode != "both") throw std::runtime_error("--memory-mode is callbacks, register-cache or both");
            }
            else if (arg == "--halt-on-memory") {
                const std::string mode = argv[i];
                if (mode == "on") halt_mode = 1;
                else if (mode == "off") halt_mode = 0;
                else if (mode != "both") throw std::runtime_error("--halt-on-memory is on, off or both");
            }
            else if (arg == "--assembly") assembly_path = argv[i]; // generated programs, for an external disassembler
            else if (arg == "--exclude-template") {
                const auto it = std::find_if(std::begin(instructions), std::end(instructions),
                                             [&](const Template& t) { return t.assembly == std::string(argv[i]); });
                if (it == std::end(instructions)) throw std::runtime_error(std::string("unknown template ") + argv[i]);
                excluded_templates()[it - std::begin(instructions)] = true;
            }
            else throw std::runtime_error("unknown option " + arg);
        }
        for (const auto& list : by_category())
            if (std::all_of(list.begin(), list.end(), [](std::size_t t) { return excluded_templates()[t]; }))
                throw std::runtime_error("--exclude-template removed a whole category");
        // gpu_policy.py --cpu passes its deadline; stop cleanly before it.
        std::optional<std::time_t> deadline;
        if (const char* value = std::getenv("VENTURE_GPU_DEADLINE")) deadline = std::time_t(std::stoll(value)) - 30;
        std::filesystem::create_directories(output);
        std::ofstream assembly;
        if (!assembly_path.empty()) assembly.open(assembly_path);
        Harness h; Coverage coverage;
        std::set<std::string> reported, reported_classes;
        std::uint64_t reports = 0, mismatched_programs = 0, cross_mismatches = 0, cross_reports = 0;
        const auto total = replay.empty() ? count : 1;
        const auto began = std::chrono::steady_clock::now();
        for (std::uint64_t i = start; i != start + total; ++i) {
            if (deadline && (i - start) % 64 == 0 && std::time(nullptr) >= *deadline) {
                coverage_report(coverage, seed, start, i - start, output);
                std::cout << "deadline: stopped after " << (i - start) << " programs; resume with --start " << i
                          << " --count " << (start + total - i) << '\n';
                return 3;
            }
            current_index = i;
            current = replay.empty() ? generate(seed, i, coverage, memory_mode, halt_mode) : load(replay);
            if (!replay.empty() && memory_mode >= 0) current->register_cache = memory_mode;
            if (!replay.empty() && halt_mode >= 0) current->halt_on_memory = halt_mode;
            const auto& c = *current;
            if (assembly) { assembly << "// seed=0x" << std::hex << seed << std::dec << " index=" << i << '\n'; write_assembly(c, assembly); }
            const auto results = h.run(c);
            // A replay also shows absolute results, for faults every pass set shares.
            if (!replay.empty()) std::cout << "replay: default-pass changes from the initial state\n" << difference(c.initial, results[0]);
            // The generator guarantees termination, so a program that stops early
            // under every pass set is a generator error or a backend fault shared by
            // all of them. Neither may be skipped silently.
            if (std::none_of(results.begin(), results.end(), [](const State& r) { return r.reached_svc; })) {
                std::ostringstream m;
                m << "no pass configuration reached the SVC boundary: pc offset 0x" << std::hex << results[0].pc
                  << " fetched-outside=" << results[0].fetched_outside << " exception=" << results[0].exception_raised
                  << " wild-access=" << (results[0].wild_access ? *results[0].wild_access : 0);
                throw std::runtime_error(m.str());
            }
            bool mismatch = false;
            for (unsigned peer = 1; peer != config_count; ++peer) {
                if (!compared(c, peer) || same(results[base_of(peer)], results[peer])) continue;
                mismatch = true;
                ++coverage.mismatches[peer][c.register_cache];
                const auto key = std::string(config_names[peer]) + (c.register_cache ? " register-cache" : " callbacks") +
                                 (c.halt_on_memory ? " halt: " : " no-halt: ") + difference_class(results[base_of(peer)], results[peer]);
                ++coverage.classes[key];
                // Reduce the first divergence of each class, up to --max-reports.
                if (reports == max_reports || !reported_classes.insert(key).second) continue;
                std::cout << "seed=0x" << std::hex << seed << std::dec << " index=" << i << '\n';
                const auto directory = output / ("failure-" + std::to_string(i) + "-" + config_names[peer]);
                const auto reduced = report(h, c, peer, directory);
                if (!reported.insert(signature(reduced, peer)).second)
                    std::cout << "(same reduced template sequence as an earlier report)\n";
                ++reports;
            }
            if (cross && h.differs(c, Harness::cross_mode)) {
                mismatch = true;
                ++cross_mismatches;
                if (cross_reports++ < max_reports) {
                    const auto reduced = h.reduce(c, Harness::cross_mode);
                    const auto directory = output / ("cross-memory-mode-" + std::to_string(i));
                    std::filesystem::create_directories(directory);
                    save(c, directory / "original.case");
                    save(reduced, directory / "reduced.case");
                    std::ofstream assembly_out(directory / "reduced.s"); write_assembly(reduced, assembly_out);
                    const auto r = h.run_both_modes(reduced);
                    std::ofstream(directory / "difference.txt")
                        << "default passes: callbacks vs register cache + identity fastmem\ncheck_halt_on_memory_access: "
                        << reduced.halt_on_memory << '\n' << difference(r[0], r[1]);
                    std::cout << "CROSS-MODE DIVERGENCE (halt on memory " << reduced.halt_on_memory << "), reduced "
                              << c.body.size() << " -> " << reduced.body.size() << " body instructions, written to "
                              << directory.string() << '\n' << difference(r[0], r[1]);
                }
            }
            mismatched_programs += mismatch;
            if ((i - start + 1) % 50000 == 0) {
                coverage_report(coverage, seed, start, i - start + 1, output);
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
                std::cout << (i - start + 1) << " programs, " << mismatched_programs << " with divergences, "
                          << int((i - start + 1) / seconds) << " programs/s\n" << std::flush;
            }
        }
        const auto joint_covered = coverage_report(coverage, seed, start, total, output);
        if (replay.empty() && memory_mode < 0 && halt_mode < 0 && total >= 1024 && joint_covered != 1024)
            throw std::runtime_error("incomplete FPCR/cache/halt/shape coverage");
        std::cout << "seed=0x" << std::hex << seed << std::dec << " start=" << start << ": " << total
                  << " programs x " << config_count << " pass configurations; " << mismatched_programs << " with divergences (no-GetSetElimination "
                  << coverage.mismatches[1][0] + coverage.mismatches[1][1] << ", no-ConstProp "
                  << coverage.mismatches[2][0] + coverage.mismatches[2][1] << ", no-LowLaneAliasing "
                  << coverage.mismatches[3][0] + coverage.mismatches[3][1] << ", no-AddressFolding "
                  << coverage.mismatches[4][0] + coverage.mismatches[4][1] << ", no-InlineSites "
                  << coverage.mismatches[5][0] + coverage.mismatches[5][1] << ", inaccurate-nan no-AVX "
                  << coverage.mismatches[7][0] + coverage.mismatches[7][1] << ", cold-region "
                  << coverage.mismatches[8][0] + coverage.mismatches[8][1] << ", compact-rsb-pop "
                  << coverage.mismatches[9][0] + coverage.mismatches[9][1] << (cross ? ", across memory modes " + std::to_string(cross_mismatches) : std::string())
                  << "); compared all GPR/SP/vector/NZCV/FPCR/FPSR/memory bits\n";
        return mismatched_programs ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        if (current) {
            std::cerr << "while running index " << current_index << '\n';
            try { save(*current, output / "abort.case"); std::cerr << "case saved to " << (output / "abort.case").string() << '\n'; }
            catch (...) {}
        }
        return 2;
    }
}
