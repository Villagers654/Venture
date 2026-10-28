// SPDX-License-Identifier: GPL-3.0-or-later
// The x64 register cache holds X20 in RBX, which CMPXCHG16B borrows for the
// 128-bit atomic accesses (LDAXP/STLXP, LDIAPP/STILP). X20 must survive each
// access on every path: inline, a fastmem fault into the fallback, the memory
// callbacks of the recompiled block, and a memory abort out of either, with
// X20 as the base register, written back by the access, loaded by it, or set
// before or between the accesses in the same block.
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace Dynarmic;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u32 ldaxp_x0 = 0xc87f8c02;    // ldaxp x2, x3, [x0]
constexpr u32 ldaxp_x20 = 0xc87f8e82;   // ldaxp x2, x3, [x20]
constexpr u32 ldaxp_to20 = 0xc87fd414;  // ldaxp x20, x21, [x0]
constexpr u32 stlxp_x0 = 0xc8249805;    // stlxp w4, x5, x6, [x0]
constexpr u32 stlxp_x20 = 0xc8249a85;   // stlxp w4, x5, x6, [x20]
constexpr u32 stlxp_of20 = 0xc824d414;  // stlxp w4, x20, x21, [x0]
constexpr u32 ldiapp_x20 = 0xd9430a82;  // ldiapp x2, x3, [x20], #16
constexpr u32 ldiapp_x0 = 0xd9431802;   // ldiapp x2, x3, [x0]
constexpr u32 ldiapp_to20 = 0xd9551814; // ldiapp x20, x21, [x0]
constexpr u32 stilp_x0 = 0xd9061805;    // stilp x5, x6, [x0]
constexpr u32 stilp_of20 = 0xd9151834;  // stilp x20, x21, [x1]
constexpr u32 add_x7 = 0x91000687;      // add x7, x20, #1
constexpr u32 movz_x20 = 0xd2886434;    // movz x20, #0x4321
constexpr u32 svc = 0xd4000001;

constexpr u64 sentinel = 0x20205a5a01234567;
constexpr u64 set = 0x4321;
constexpr u64 junk = 0x8badf00ddeadbeef;
constexpr u64 store_lo = 0x1111222233334444, store_hi = 0x5555666677778888;
constexpr u64 fake = 0x40;  // unmapped: the callbacks serve it
constexpr u64 abort_read = 0x80, abort_write = 0xc0;  // unmapped: the callbacks halt

void require(bool ok, const std::string& what) { if (!ok) throw std::runtime_error(what); }
std::string hex(u64 v) { char b[24]; std::snprintf(b, sizeof(b), "%#llx", static_cast<unsigned long long>(v)); return b; }

struct alignas(16) Cell { u64 lo, hi; };

class Cpu final : private A64::UserCallbacks {
 public:
  Cpu(ExclusiveMonitor& monitor, A64::UserConfig::ColdAbortState cold) {
    A64::UserConfig c;
    c.callbacks = this; c.global_monitor = &monitor; c.processor_id = 0;
    c.fastmem_pointer = 0; c.fastmem_address_space_bits = 64; c.silently_mirror_fastmem = false;
    c.enable_register_cache = true; c.fastmem_exclusive_access = true; c.plain_exclusive_loads = true;
    c.check_halt_on_memory_access = true; c.cold_memory_abort_state = cold;
    c.enable_cycle_counting = false;
    c.unsafe_optimizations = true;
    c.optimizations |= OptimizationFlag::Unsafe_IgnoreGlobalMonitor; // As the runtime.
    jit = std::make_unique<A64::Jit>(c);
  }
  HaltReason run(const std::vector<u32>& code, u64 x0, u64 x20, u64 x1 = junk) {
    jit->ClearHalt(HaltReason::UserDefined1); jit->ClearHalt(HaltReason::MemoryAbort);
    for (const size_t r : {2, 3, 4, 7, 21}) jit->SetRegister(r, junk);
    jit->SetRegister(0, x0); jit->SetRegister(1, x1);
    jit->SetRegister(5, store_lo); jit->SetRegister(6, store_hi);
    jit->SetRegister(20, x20);
    jit->SetPC(reinterpret_cast<std::uintptr_t>(code.data()));
    return jit->Run();
  }
  std::unique_ptr<A64::Jit> jit;
  Cell fake_cell{};

 private:
  std::optional<u32> MemoryReadCode(u64 p) override {
    u32 word; std::memcpy(&word, reinterpret_cast<void*>(p), sizeof(word)); return word;
  }
  A64::Vector MemoryRead128(u64 p) override {
    if (p == abort_read) { jit->HaltExecution(HaltReason::MemoryAbort); return {junk, junk}; }
    if (p == abort_write) return {7, 8};
    require(p == fake, "read128 at " + hex(p));
    return {fake_cell.lo, fake_cell.hi};
  }
  void MemoryWrite128(u64 p, A64::Vector v) override {
    if (p == abort_write) { jit->HaltExecution(HaltReason::MemoryAbort); return; }
    require(p == fake, "write128 at " + hex(p));
    fake_cell = {v[0], v[1]};
  }
  bool MemoryWriteExclusive128(u64 p, A64::Vector v, A64::Vector expected) override {
    if (p == abort_write) { jit->HaltExecution(HaltReason::MemoryAbort); return false; }
    require(p == fake, "exclusive write128 at " + hex(p));
    if (fake_cell.lo != expected[0] || fake_cell.hi != expected[1]) return false;
    fake_cell = {v[0], v[1]};
    return true;
  }
  u64 MemoryRead(u64 p, size_t) override { throw std::runtime_error("unexpected read at " + hex(p)); }
  void MemoryWrite(u64 p, u64, size_t) override { throw std::runtime_error("unexpected write at " + hex(p)); }
  u64 MemoryAtomic(u64 p, size_t, A64::AtomicOp, u64, u64) override { throw std::runtime_error("unexpected atomic at " + hex(p)); }
  A64::Vector MemoryAtomicCompareAndSwap128(u64 p, A64::Vector, A64::Vector) override { throw std::runtime_error("unexpected CASP at " + hex(p)); }
  void CallSVC(u32) override { jit->HaltExecution(HaltReason::UserDefined1); }
  void ExceptionRaised(u64 pc, A64::Exception) override { throw std::runtime_error("unexpected exception at " + hex(pc)); }
  void AddTicks(u64) override {}
  u64 GetTicksRemaining() override { return 1000000; }
  u64 GetCNTPCT() override { return 0; }
};

// A load of X2:X3 at load_at, a store of X5:X6 at store_at, then X7 = X20 + 1.
// X20 holds x20_load before the load, x20_store before the store and x20_end
// at the end.
struct Program {
  const char* name;
  std::vector<u32> code;
  bool exclusive;
  size_t load_at, store_at;
  u64 x20_load, x20_store, x20_end;
};

void check_completed(Cpu& cpu, const Program& p, HaltReason halt, const Cell& before, const Cell& after, const std::string& what) {
  auto& jit = *cpu.jit;
  require(Has(halt, HaltReason::UserDefined1) && !Has(halt, HaltReason::MemoryAbort), what + ": halt");
  require(jit.GetRegister(20) == p.x20_end, what + ": x20 " + hex(jit.GetRegister(20)) + " expected " + hex(p.x20_end));
  require(jit.GetRegister(7) == p.x20_store + 1, what + ": x7 " + hex(jit.GetRegister(7)));
  require(jit.GetRegister(2) == before.lo && jit.GetRegister(3) == before.hi, what + ": loaded pair");
  require(after.lo == store_lo && after.hi == store_hi, what + ": stored pair");
  if (p.exclusive) require(jit.GetRegister(4) == 0, what + ": store-exclusive status");
}

unsigned run_all(A64::UserConfig::ColdAbortState cold) {
  const std::string mode = cold == A64::UserConfig::ColdAbortState::Off ? "cold off" : "cold live";
  const std::vector<Program> programs{
      {"exclusive", {ldaxp_x0, stlxp_x0, add_x7, svc}, true, 0, 1, sentinel, sentinel, sentinel},
      {"exclusive set", {movz_x20, ldaxp_x0, stlxp_x0, add_x7, svc}, true, 1, 2, set, set, set},
      {"exclusive set between", {ldaxp_x0, movz_x20, stlxp_x0, add_x7, svc}, true, 0, 2, sentinel, set, set},
      {"ordered set", {movz_x20, ldiapp_x0, stilp_x0, add_x7, svc}, false, 1, 2, set, set, set},
      {"ordered set between", {ldiapp_x0, movz_x20, stilp_x0, add_x7, svc}, false, 0, 2, sentinel, set, set},
      // The first setter is overwritten later in the block, but a cached
      // register's setter is never left to the abort exit alone.
      {"ordered set twice", {movz_x20, ldiapp_x0, stilp_x0, add_x7, movz_x20, svc}, false, 1, 2, set, set, set},
      {"exclusive set twice", {movz_x20, ldaxp_x0, stlxp_x0, add_x7, movz_x20, svc}, true, 1, 2, set, set, set},
  };
  unsigned checks = 0;
  for (const auto& p : programs) {
    const std::string what = mode + " " + p.name;
    // Inline, then through a fastmem fault and the recompiled block's callbacks.
    {
      ExclusiveMonitor monitor(1);
      monitor.EnableGenerations();
      Cpu cpu(monitor, cold);
      for (int pass = 0; pass < 3; ++pass) {
        Cell cell{0x0123456789abcdef, 0xfedcba9876543210};
        const Cell before = cell;
        const auto halt = cpu.run(p.code, reinterpret_cast<std::uintptr_t>(&cell), sentinel);
        check_completed(cpu, p, halt, before, cell, what + " inline pass " + std::to_string(pass));
        ++checks;
      }
      for (int pass = 0; pass < 3; ++pass) {
        cpu.fake_cell = {0xa0a0a0a0a0a0a0a0 + static_cast<u64>(pass), 0xb0b0b0b0b0b0b0b0};
        const Cell before = cpu.fake_cell;
        const auto halt = cpu.run(p.code, fake, sentinel);
        check_completed(cpu, p, halt, before, cpu.fake_cell, what + " fault pass " + std::to_string(pass));
        ++checks;
      }
    }
    // A memory abort at the load, then at the store: the run stops at that
    // instruction with X20 as it was before it.
    for (const u64 address : {abort_read, abort_write}) {
      ExclusiveMonitor monitor(1);
      monitor.EnableGenerations();
      Cpu cpu(monitor, cold);
      const bool at_store = address == abort_write;
      const size_t at = at_store ? p.store_at : p.load_at;
      const u64 x20 = at_store ? p.x20_store : p.x20_load;
      for (int pass = 0; pass < 3; ++pass) {
        const std::string where = what + (at_store ? " store" : " load") + " abort pass " + std::to_string(pass);
        const auto halt = cpu.run(p.code, address, sentinel);
        auto& jit = *cpu.jit;
        require(Has(halt, HaltReason::MemoryAbort), where + ": halt");
        require(jit.GetPC() == reinterpret_cast<std::uintptr_t>(p.code.data() + at), where + ": pc");
        require(jit.GetRegister(20) == x20, where + ": x20 " + hex(jit.GetRegister(20)) + " expected " + hex(x20));
        if (at_store) require(jit.GetRegister(2) == 7 && jit.GetRegister(3) == 8, where + ": loaded pair");
        ++checks;
      }
    }
  }
  // X20 as the base of an exclusive pair, written back by an ordered load, and
  // loaded by each kind of pair then stored back from X20:X21.
  {
    ExclusiveMonitor monitor(1);
    monitor.EnableGenerations();
    Cpu cpu(monitor, cold);
    // Each program keeps its code at one address: the JIT caches blocks by PC.
    Program through{"x20 base", {ldaxp_x20, stlxp_x20, add_x7, svc}, true, 0, 1, 0, 0, 0};
    Program writeback{"x20 writeback", {ldiapp_x20, stilp_x0, add_x7, svc}, false, 0, 1, 0, 0, 0};
    const std::vector<u32> exclusive_to20{ldaxp_to20, stlxp_of20, add_x7, svc};
    const std::vector<u32> ordered_to20{ldiapp_to20, stilp_of20, add_x7, svc};
    for (int pass = 0; pass < 3; ++pass) {
      const std::string suffix = " pass " + std::to_string(pass);
      Cell cell{0x0123456789abcdef + static_cast<u64>(pass), 0xfedcba9876543210}, out{};
      const Cell before = cell;
      const u64 base = reinterpret_cast<std::uintptr_t>(&cell);
      through.x20_load = through.x20_store = through.x20_end = base;
      auto halt = cpu.run(through.code, junk, base);
      check_completed(cpu, through, halt, before, cell, mode + " " + through.name + suffix);
      cell = before;
      writeback.x20_load = base;
      writeback.x20_store = writeback.x20_end = base + 16;
      halt = cpu.run(writeback.code, reinterpret_cast<std::uintptr_t>(&out), base);
      check_completed(cpu, writeback, halt, before, out, mode + " " + writeback.name + suffix);
      for (const bool exclusive : {true, false}) {
        const std::string what = mode + (exclusive ? " exclusive" : " ordered") + " load to x20" + suffix;
        cell = before; out = {};
        halt = cpu.run(exclusive ? exclusive_to20 : ordered_to20, base, sentinel, reinterpret_cast<std::uintptr_t>(&out));
        auto& jit = *cpu.jit;
        require(Has(halt, HaltReason::UserDefined1), what + ": halt");
        require(jit.GetRegister(20) == before.lo && jit.GetRegister(21) == before.hi, what + ": loaded pair");
        require(jit.GetRegister(7) == before.lo + 1, what + ": x7");
        const Cell& stored = exclusive ? cell : out;
        require(stored.lo == before.lo && stored.hi == before.hi, what + ": stored pair");
        if (exclusive) require(jit.GetRegister(4) == 0, what + ": store-exclusive status");
      }
      checks += 4;
    }
  }
  return checks;
}
}  // namespace

int main() {
  try {
    unsigned checks = 0;
    for (const auto cold : {A64::UserConfig::ColdAbortState::Off, A64::UserConfig::ColdAbortState::Live})
      checks += run_all(cold);
    std::printf("jit_cached_rbx: %u runs passed\n", checks);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "jit_cached_rbx: %s\n", e.what());
    return 1;
  }
}
