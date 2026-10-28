#include <dynarmic/interface/A64/a64.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
constexpr std::uint64_t entry = 0x100000;
constexpr std::uint64_t exit_pc = 0x200000;

struct Callbacks final : Dynarmic::A64::UserCallbacks {
    std::vector<std::uint32_t> words;
    Dynarmic::A64::Jit* jit{};
    std::optional<std::uint32_t> MemoryReadCode(std::uint64_t pc) override {
        if (pc >= entry && pc < entry + words.size() * 4) return words[(pc - entry) / 4];
        return 0xd4000001; // SVC marks the test boundary.
    }
    std::uint64_t MemoryRead(std::uint64_t, std::size_t) override { throw std::runtime_error("unexpected read"); }
    Dynarmic::A64::Vector MemoryRead128(std::uint64_t) override { throw std::runtime_error("unexpected read128"); }
    void MemoryWrite(std::uint64_t, std::uint64_t, std::size_t) override { throw std::runtime_error("unexpected write"); }
    void MemoryWrite128(std::uint64_t, Dynarmic::A64::Vector) override { throw std::runtime_error("unexpected write128"); }
    std::uint64_t MemoryAtomic(std::uint64_t, std::size_t, Dynarmic::A64::AtomicOp, std::uint64_t, std::uint64_t) override {
        throw std::runtime_error("unexpected atomic");
    }
    Dynarmic::A64::Vector MemoryAtomicCompareAndSwap128(std::uint64_t, Dynarmic::A64::Vector, Dynarmic::A64::Vector) override {
        throw std::runtime_error("unexpected atomic128");
    }
    void CallSVC(std::uint32_t) override { jit->HaltExecution(); }
    void ExceptionRaised(std::uint64_t, Dynarmic::A64::Exception) override { throw std::runtime_error("unexpected exception"); }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 10000000; }
    std::uint64_t GetCNTPCT() override { return 0; }
};

std::uint32_t subtract_flags(std::uint64_t a, std::uint64_t b, unsigned width) {
    const auto mask = width == 32 ? std::uint64_t{0xffffffff} : ~std::uint64_t{0};
    const auto sign = std::uint64_t{1} << (width - 1);
    a &= mask;
    b &= mask;
    const auto result = (a - b) & mask;
    const bool n = (result & sign) != 0;
    const bool z = result == 0;
    const bool c = a >= b;
    const bool v = ((a ^ b) & (a ^ result) & sign) != 0;
    return (std::uint32_t(n) << 31) | (std::uint32_t(z) << 30) |
           (std::uint32_t(c) << 29) | (std::uint32_t(v) << 28);
}

bool condition(unsigned cc, std::uint32_t flags) {
    const bool n = (flags >> 31) & 1;
    const bool z = (flags >> 30) & 1;
    const bool c = (flags >> 29) & 1;
    const bool v = (flags >> 28) & 1;
    switch (cc) {
    case 0: return z;
    case 1: return !z;
    case 2: return c;
    case 3: return !c;
    case 4: return n;
    case 5: return !n;
    case 6: return v;
    case 7: return !v;
    case 8: return c && !z;
    case 9: return !c || z;
    case 10: return n == v;
    case 11: return n != v;
    case 12: return !z && n == v;
    case 13: return z || n != v;
    default: return true; // A64 B.AL and B.NV are unconditional.
    }
}
} // namespace

int main() {
    try {
        Callbacks callbacks;
        Dynarmic::A64::UserConfig config;
        config.callbacks = &callbacks;
        config.enable_cycle_counting = false;
        config.enable_register_cache = true;
        config.fastmem_pointer = 0;
        config.fastmem_address_space_bits = 64;
        config.silently_mirror_fastmem = false;
        config.check_halt_on_memory_access = true;
        config.code_cache_size = 32 * 1024 * 1024;
        Dynarmic::A64::Jit jit(config);
        callbacks.jit = &jit;
        std::mt19937_64 random(0x4e5a43564c4541);
        constexpr std::array<std::uint64_t, 11> edges = {
            0, 1, 7, 8, 0x7fffffff, 0x80000000, 0xffffffff,
            0x7fffffffffffffff, 0x8000000000000000, 0xfffffffffffffff8, ~std::uint64_t{0}};
        std::uint64_t cases = 0;
        for (unsigned compare_width : {32u, 64u}) {
            for (unsigned cursor_width : {32u, 64u}) {
                for (bool sub : {false, true}) {
                    for (unsigned cursor : {7u, 8u}) { // uncached and cached guest registers
                        for (bool clobber : {false, true}) {
                            for (unsigned cc = 0; cc != 16; ++cc) {
                                callbacks.words = {
                                    compare_width == 32 ? 0x6b01001fu : 0xeb01001fu, // CMP w/x0, w/x1
                                    (cursor_width == 32 ? 0x11000000u : 0x91000000u) |
                                        (sub ? 0x40000000u : 0u) | (8u << 10) | (cursor << 5) | cursor};
                                if (clobber) callbacks.words.push_back(0x0a0a0129); // AND w9,w9,w10: changes host flags only
                                callbacks.words.insert(callbacks.words.end(), {
                                    0x54000060u | cc, // B.cond +12
                                    0xd2800002, 0xd65f03c0, // MOV x2,0; RET
                                    0xd2800022, 0xd65f03c0}); // MOV x2,1; RET
                                jit.ClearCache();
                                for (unsigned i = 0; i != edges.size() * edges.size() + 256; ++i) {
                                    std::array<std::uint64_t, 31> expected;
                                    for (auto& r : expected) r = random();
                                    if (i < edges.size() * edges.size()) {
                                        expected[0] = edges[i / edges.size()];
                                        expected[1] = edges[i % edges.size()];
                                        expected[cursor] = edges[(i + cc) % edges.size()];
                                    }
                                    expected[30] = exit_pc;
                                    for (unsigned r = 0; r != expected.size(); ++r) jit.SetRegister(r, expected[r]);
                                    jit.SetSP(0x400000);
                                    jit.SetPC(entry);
                                    jit.SetPstate((random() & 15) << 28);
                                    const std::uint32_t fpcr = (i & 3) << 22;
                                    jit.SetFpcr(fpcr);
                                    const auto flags = subtract_flags(expected[0], expected[1], compare_width);
                                    expected[2] = condition(cc, flags);
                                    expected[cursor] = sub ? expected[cursor] - 8 : expected[cursor] + 8;
                                    if (cursor_width == 32) expected[cursor] &= 0xffffffff;
                                    if (clobber) expected[9] = std::uint32_t(expected[9]) & std::uint32_t(expected[10]);
                                    jit.Run();
                                    for (unsigned r = 0; r != expected.size(); ++r) {
                                        if (jit.GetRegister(r) != expected[r]) {
                                            std::cerr << "cc=" << cc << " compare=" << compare_width << " cursor=" << cursor_width
                                                      << " sub=" << sub << " clobber=" << clobber << " r=" << r << " case=" << i << '\n';
                                            throw std::runtime_error("register differs from arithmetic oracle");
                                        }
                                    }
                                    if (jit.GetPstate() != flags || jit.GetSP() != 0x400000 ||
                                        jit.GetPC() != exit_pc + 4 || jit.GetFpcr() != fpcr)
                                        throw std::runtime_error("architectural state differs from arithmetic oracle");
                                    ++cases;
                                }
                            }
                        }
                    }
                }
            }
        }
        std::cout << cases << " cases match independent NZCV and condition oracles\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
