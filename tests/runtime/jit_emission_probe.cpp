// SPDX-License-Identifier: GPL-3.0-or-later
// Compile halted blocks without executing guest input. Raw outputs stay
// private.
#include <cstring>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/profiling.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <dynarmic/backend/x64/exclusive_monitor_friend.h>
#include <dynarmic/ir/basic_block.h>
#include <dynarmic/backend/x64/block_of_code.h>
#include <dynarmic/backend/x64/a64_emit_x64.h>
#include <dynarmic/backend/x64/a32_emit_x64.h>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <vector>
#include <windows.h>
using Json = nlohmann::json;
namespace {
std::vector<std::uint8_t> code;
std::uint64_t start;
Json emitted = Json::array();
Json ranges = Json::array();
void range(std::string name, const void *base, std::size_t size) {
  ranges.push_back({{"name", name}, {"start", reinterpret_cast<std::uintptr_t>(base)}, {"size", size}});
}
void registered(const void *first, const void *last, std::string_view name) {
  range("helper:" + std::string(name), first,
        static_cast<const char *>(last) - static_cast<const char *>(first));
  // Helper names contain words, guest names start with a hexadecimal PC.
  if (!(name.starts_with("a64_") || name.starts_with("a32_"))) return;
  const auto offset = name.starts_with("a32_") ? 5u : 4u;
  const auto separator = name.find('_', offset);
  const auto pc = name.substr(offset, separator - offset);
  if (name.size() < 12 ||
      !(name.starts_with("a64_") || name.starts_with("a32_")) ||
      pc.size() < 8 || pc.find_first_not_of("0123456789abcdefABCDEF") != pc.npos)
    return;
  const auto *bytes = static_cast<const std::uint8_t *>(first);
  const auto length = static_cast<const std::uint8_t *>(last) - bytes;
  constexpr char digits[] = "0123456789abcdef";
  std::string text;
  for (std::ptrdiff_t i = 0; i < length; ++i) {
    text += digits[bytes[i] >> 4];
    text += digits[bytes[i] & 15];
  }
  emitted.push_back({{"name", name}, {"address", reinterpret_cast<std::uintptr_t>(first)}, {"bytes", text}});
}
std::optional<std::uint32_t> read_code(std::uint64_t pc) {
  if (pc < start || pc - start > code.size() || code.size() - (pc - start) < 4)
    return {};
  std::uint32_t word;
  std::memcpy(&word, code.data() + pc - start, 4);
  return word;
}
} // namespace
// Test-only mirrors of the pinned x64 Impl prefixes. They compute addresses,
// never construct or dereference an Impl. Public register/config checks below
// reject layout drift rather than silently normalizing an unrelated object.
using namespace Dynarmic::Backend::X64;
struct Prefix64 {
  Dynarmic::IR::Block block;
  Dynarmic::A64::UserConfig config;
  A64JitState state;
  BlockOfCode code;
  A64EmitX64 emitter;
};
struct Prefix32 {
  Dynarmic::IR::Block block;
  A32JitState state;
  BlockOfCode code;
  A32EmitX64 emitter;
};
template<class Jit, class Prefix, class Callback> void objects(Jit &jit, Callback *cb) {
  // A32 starts with its public IsExecuting flag, then the aligned unique_ptr.
  constexpr auto pointer_offset = std::is_same_v<Prefix, Prefix32> ? sizeof(void *) : 0;
  static_assert(sizeof(Jit) == sizeof(void *) + pointer_offset);
  std::uintptr_t impl;
  std::memcpy(&impl, reinterpret_cast<const char *>(&jit) + pointer_offset, sizeof(impl));
  auto *prefix = reinterpret_cast<Prefix *>(impl);
  auto *conf = &prefix->emitter.conf;
  Callback *actual;
  std::memcpy(&actual, &conf->callbacks, sizeof(actual));
  if (actual != cb) throw std::runtime_error("test Impl/emitter layout changed");
  range("UserConfig", conf, sizeof(*conf));
  range("JitState", &prefix->state, sizeof(prefix->state));
  range("dispatch-table", prefix->emitter.fast_dispatch_table.data(), sizeof(prefix->emitter.fast_dispatch_table));
  range("JitInterface", &jit, sizeof(jit));
  if constexpr (std::is_same_v<Prefix, Prefix64>) {
    jit.SetRegister(0, 0x123456789abcdef0);
    std::uint64_t value;
    std::memcpy(&value, &prefix->state.reg[0], sizeof(value));
    if (value != jit.GetRegister(0)) throw std::runtime_error("test A64 state layout changed");
    jit.SetRegister(0, 0);
  } else if (jit.Regs().data() != prefix->state.Reg.data())
    throw std::runtime_error("test A32 state layout changed");
}
// Keep callback identities stable; the comparison still records all known
// relocations symbolically and checks the remaining bytes exactly.
#pragma clang section text = ".probe"
struct Callbacks64 final : Dynarmic::A64::UserCallbacks {
  std::optional<std::uint32_t> MemoryReadCode(std::uint64_t pc) override {
    return read_code(pc);
  }
  std::uint64_t MemoryRead(std::uint64_t, std::size_t) override { return 0; }
  void MemoryWrite(std::uint64_t, std::uint64_t, std::size_t) override {}
  Dynarmic::A64::Vector MemoryRead128(std::uint64_t) override { return {}; }
  void MemoryWrite128(std::uint64_t, Dynarmic::A64::Vector) override {}
  std::uint64_t MemoryAtomic(std::uint64_t, std::size_t,
                             Dynarmic::A64::AtomicOp, std::uint64_t,
                             std::uint64_t) override {
    return 0;
  }
  Dynarmic::A64::Vector
  MemoryAtomicCompareAndSwap128(std::uint64_t, Dynarmic::A64::Vector,
                                Dynarmic::A64::Vector) override {
    return {};
  }
  void InterpreterFallback(std::uint64_t, std::size_t) {}
  void CallSVC(std::uint32_t) override {}
  void ExceptionRaised(std::uint64_t, Dynarmic::A64::Exception) override {}
  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return 10000; }
  std::uint64_t GetCNTPCT() override { return 0; }
};
struct Callbacks32 final : Dynarmic::A32::UserCallbacks {
  std::optional<std::uint32_t> MemoryReadCode(std::uint32_t pc) override {
    return read_code(pc);
  }
  std::uint64_t MemoryRead(std::uint32_t, std::size_t) override { return 0; }
  void MemoryWrite(std::uint32_t, std::uint64_t, std::size_t) override {}
  void InterpreterFallback(std::uint32_t, std::size_t) {}
  void CallSVC(std::uint32_t) override {}
  void ExceptionRaised(std::uint32_t, Dynarmic::A32::Exception) override {}
  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return 10000; }
};
#pragma clang section text = ""
int main(int argc, char **argv) {
  try {
    if (argc != 3)
      throw std::runtime_error("jit emission probe input.json output.json");
    Json input;
    std::ifstream(argv[1]) >> input;
    Json output = Json::array();
    auto *memory = VirtualAlloc(reinterpret_cast<void *>(0x30000000), 8192,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (memory != reinterpret_cast<void *>(0x30000000))
      throw std::runtime_error("fixed callback allocation unavailable");
    auto *cb64 = new (memory) Callbacks64;
    auto *cb32 = new (static_cast<char *>(memory) + 4096) Callbacks32;
    Dynarmic::ExclusiveMonitor monitor(1);
    for (const auto &row : input) {
      std::ifstream file(row.at("code").get<std::string>(), std::ios::binary);
      code = {std::istreambuf_iterator<char>(file), {}};
      start = row.at("start");
      emitted = Json::array();
      ranges = Json::array();
      range("callback-object:A64", cb64, sizeof(*cb64));
      range("callback-object:A32", cb32, sizeof(*cb32));
      range("global-monitor", &monitor, sizeof(monitor));
      range("monitor:address-slot", GetExclusiveMonitorAddressPointer(&monitor, 0), sizeof(Dynarmic::VAddr));
      range("monitor:value-slot", GetExclusiveMonitorValuePointer(&monitor, 0), sizeof(Dynarmic::Vector));
      Dynarmic::SetCodeRegistrationCallback(registered);
      if (row.at("arch") == "a64") {
        Dynarmic::A64::UserConfig config{};
        config.callbacks = cb64;
        config.global_monitor = &monitor;
        config.unsafe_optimizations = true;
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        config.fastmem_exclusive_access = true;
        config.enable_cycle_counting = false;
        config.fastmem_pointer = 0;
        config.fastmem_address_space_bits = 64;
        config.silently_mirror_fastmem = false;
        config.check_halt_on_memory_access = true;
        config.enable_register_cache = true;
        config.code_cache_size = 32 * 1024 * 1024;
        Dynarmic::A64::Jit jit(config);
        objects<Dynarmic::A64::Jit, Prefix64>(jit, cb64);
        jit.SetPC(start);
        jit.HaltExecution(Dynarmic::HaltReason::UserDefined2);
        jit.Run();
      } else {
        Dynarmic::A32::UserConfig config{};
        config.callbacks = cb32;
        config.global_monitor = &monitor;
        config.unsafe_optimizations = true;
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        config.fastmem_exclusive_access = true;
        config.check_halt_on_memory_access = true;
        config.always_little_endian = true;
        config.define_unpredictable_behaviour = true;
        config.enable_cycle_counting = false;
        config.fastmem_pointer = 0;
        config.code_cache_size = 32 * 1024 * 1024;
        config.enable_block_frame_chain = row.value("frame_chain", false);
        Dynarmic::A32::Jit jit(config);
        objects<Dynarmic::A32::Jit, Prefix32>(jit, cb32);
        jit.Regs()[15] = start;
        jit.SetCpsr(row.value("thumb", false) ? 0x30 : 0x10);
        jit.HaltExecution(Dynarmic::HaltReason::UserDefined2);
        jit.Run();
      }
      Dynarmic::SetCodeRegistrationCallback(nullptr);
      if (emitted.empty())
        throw std::runtime_error("no guest block captured");
      output.push_back({{"id", row.at("id")}, {"blocks", emitted}, {"ranges", ranges}});
    }
    std::ofstream(argv[2]) << output.dump() << '\n';
    cb32->~Callbacks32();
    cb64->~Callbacks64();
    VirtualFree(memory, 0, MEM_RELEASE);
    std::cout << Json{{"compiled_entries", output.size()}}.dump() << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
