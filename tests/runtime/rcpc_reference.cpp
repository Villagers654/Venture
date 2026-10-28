// SPDX-License-Identifier: GPL-3.0-or-later
// Independent aligned, little-endian EL0 load/store state model. It consumes
// assembly-fixture descriptors, not JIT IR or instruction-bit decoding.
#include <array>
#include <cstring>
#include <dynarmic/interface/A64/a64.h>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <random>
using Json = nlohmann::json;
struct Callbacks final : Dynarmic::A64::UserCallbacks {
  std::uint32_t word{};
  bool exception{};
  std::uint64_t exception_pc{};
  Dynarmic::A64::Exception exception_kind{};
  unsigned reads{}, writes{};
  alignas(16) std::array<std::uint8_t, 256> memory{};
  std::optional<std::uint32_t> MemoryReadCode(std::uint64_t p) override {
    return p == 0x1000 ? std::optional(word) : std::nullopt;
  }
  void check(std::uint64_t p, std::size_t n) {
    const auto first = reinterpret_cast<std::uint64_t>(memory.data());
    if (p < first || p - first > memory.size() ||
        n > memory.size() - (p - first))
      throw std::runtime_error("reference fixture wild access");
  }
  std::uint64_t MemoryRead(std::uint64_t p, std::size_t n) override {
    check(p, n);
    ++reads;
    std::uint64_t v = 0;
    std::memcpy(&v, reinterpret_cast<void *>(p), n);
    return v;
  }
  void MemoryWrite(std::uint64_t p, std::uint64_t v, std::size_t n) override {
    check(p, n);
    ++writes;
    std::memcpy(reinterpret_cast<void *>(p), &v, n);
  }
  Dynarmic::A64::Vector MemoryRead128(std::uint64_t p) override {
    check(p, 16);
    ++reads;
    Dynarmic::A64::Vector v;
    std::memcpy(v.data(), reinterpret_cast<void *>(p), 16);
    return v;
  }
  void MemoryWrite128(std::uint64_t p, Dynarmic::A64::Vector v) override {
    check(p, 16);
    ++writes;
    std::memcpy(reinterpret_cast<void *>(p), v.data(), 16);
  }
  std::uint64_t MemoryAtomic(std::uint64_t, std::size_t,
                             Dynarmic::A64::AtomicOp, std::uint64_t,
                             std::uint64_t) override {
    throw std::runtime_error("unexpected atomic");
  }
  Dynarmic::A64::Vector
  MemoryAtomicCompareAndSwap128(std::uint64_t, Dynarmic::A64::Vector,
                                Dynarmic::A64::Vector) override {
    throw std::runtime_error("unexpected CAS");
  }
  void InterpreterFallback(std::uint64_t, std::size_t) {
    throw std::runtime_error("unexpected interpreter fallback");
  }
  void CallSVC(std::uint32_t) override {
    throw std::runtime_error("unexpected SVC");
  }
  void ExceptionRaised(std::uint64_t pc, Dynarmic::A64::Exception kind) override {
    exception = true;
    exception_pc = pc;
    exception_kind = kind;
  }
  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return 10000; }
  std::uint64_t GetCNTPCT() override { return 0; }
};
int main(int argc, char **argv) {
  try {
    if (argc != 2 && argc != 3)
      throw std::runtime_error("reference fixture manifest required");
    const bool expect_undefined = argc == 3 && std::string_view(argv[2]) == "--expect-undefined";
    Json cases;
    std::ifstream(argv[1]) >> cases;
    std::mt19937_64 random(0x63637072);
    unsigned checked = 0, undefined = 0;
    for (const auto &c : cases)
      for (bool fast : {false, true}) {
        Callbacks cb;
        std::ifstream file(c.at("code").get<std::string>(), std::ios::binary);
        file.read(reinterpret_cast<char *>(&cb.word), 4);
        Dynarmic::A64::UserConfig config{};
        config.callbacks = &cb;
        config.enable_cycle_counting = false;
        config.enable_register_cache = fast;
        config.check_halt_on_memory_access = true;
        config.code_cache_size = 32 * 1024 * 1024;
        if (fast) {
          config.fastmem_pointer = 0;
          config.fastmem_address_space_bits = 64;
          config.silently_mirror_fastmem = false;
        }
        Dynarmic::A64::Jit jit(config);
        for (unsigned round = 0; round < 256; ++round) {
          std::array<std::uint64_t, 31> regs;
          for (auto &x : regs)
            x = random();
          for (auto &x : cb.memory)
            x = random();
          auto expected_memory = cb.memory;
          const auto base =
              reinterpret_cast<std::uint64_t>(cb.memory.data()) + 64;
          auto sp = base;
          if (!c.at("sp").get<bool>())
            regs[0] = base;
          const auto flags = static_cast<std::uint32_t>(random() & 15) << 28;
          jit.SetRegisters(regs);
          jit.SetSP(sp);
          jit.SetPstate(flags);
          jit.SetPC(0x1000);
          cb.exception = false;
          cb.reads = cb.writes = 0;
          const bool load = c.at("load"), pair = c.at("pair"),
                     writeback = c.at("writeback"),
                   trap = c.value("trap", false) || expect_undefined;
          const unsigned bytes = c.at("width").get<unsigned>() / 8,
                         total = bytes * (pair ? 2 : 1), rt = c.at("rt"),
                         rt2 = c.value("rt2", 3u);
          const auto offset = writeback && !load ? 64 - total : 64;
          if (!trap) {
            if (load) {
              auto get = [&](unsigned reg, unsigned at) {
                std::uint64_t v = 0;
                std::memcpy(&v, expected_memory.data() + at, bytes);
                if (reg != 31)
                  regs[reg] = v;
              };
              get(rt, offset);
              if (pair)
                get(rt2, offset + bytes);
            } else {
              auto put = [&](unsigned reg, unsigned at) {
                auto v = reg == 31 ? 0 : regs[reg];
                std::memcpy(expected_memory.data() + at, &v, bytes);
              };
              put(rt, offset);
              if (pair)
                put(rt2, offset + bytes);
            }
            if (writeback) {
              const auto next = load ? base + total : base - total;
              if (c.at("sp").get<bool>())
                sp = next;
              else
                regs[0] = next;
            }
          }
          jit.Step();
          if (trap) {
            if (!cb.exception || cb.exception_pc != 0x1000 ||
                cb.exception_kind != (expect_undefined ? Dynarmic::A64::Exception::UnallocatedEncoding : Dynarmic::A64::Exception::UnpredictableInstruction) ||
                jit.GetRegisters() != regs || jit.GetSP() != sp ||
                jit.GetPstate() != flags || cb.memory != expected_memory || cb.reads || cb.writes)
              throw std::runtime_error("trap changed architectural state or memory");
            ++undefined;
          } else if (cb.exception || jit.GetRegisters() != regs ||
                     jit.GetSP() != sp || jit.GetPstate() != flags ||
                     cb.memory != expected_memory || jit.GetPC() != 0x1004)
            throw std::runtime_error("reference mismatch in " +
                                     c.at("name").get<std::string>());
          ++checked;
        }
      }
    std::cout << Json{{"checked", checked},
                      {"overlap_traps", undefined},
                      {"matched", true},
                      {"scope", "aligned little-endian EL0 state and memory"}}
                     .dump()
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
