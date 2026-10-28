// SPDX-License-Identifier: GPL-3.0-or-later
// The x64 register cache holds X9 and X10 in R10 and R11, which every host
// call may clobber. Each callback here clobbers both, so X9 and X10 must come
// back from the cache's store and reload around supervisor calls, memory
// callbacks (fastmem faults into the fallbacks and the recompiled block's
// callback path), exclusive and atomic accesses, and memory aborts; a
// supervisor call that changes them through the interface must be seen too.
// Returns and indirect branches cover the dispatch paths, which use other
// temporaries.
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <atomic>
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

// Clobbers the two registers the way any host function may.
void clobber_r10_r11() {
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
  asm volatile("movabs $0x0badc0de0badc0de, %%r10\n\tmov %%r10, %%r11" ::: "r10", "r11");
#endif
}

constexpr u32 movz_x9 = 0xd2824689;   // movz x9, #0x1234
constexpr u32 movz_x10 = 0xd28acf0a;  // movz x10, #0x5678
constexpr u32 movz_x9b = 0xd2886429;  // movz x9, #0x4321
constexpr u32 movz_x10b = 0xd290ecaa; // movz x10, #0x8765
constexpr u32 ldr_x0 = 0xf9400002;    // ldr x2, [x0]
constexpr u32 str_x0 = 0xf9000005;    // str x5, [x0]
constexpr u32 ldaxr_x0 = 0xc85ffc02;  // ldaxr x2, [x0]
constexpr u32 stlxr_x0 = 0xc804fc05;  // stlxr w4, x5, [x0]
constexpr u32 mov_x2 = 0xaa0003e2;    // mov x2, x0
constexpr u32 ldaxr_x2 = 0xc85ffc43;  // ldaxr x3, [x2]
constexpr u32 stlxr_x2 = 0xc804fc45;  // stlxr w4, x5, [x2]
constexpr u32 ldxr_x2 = 0xc85f7c43;   // ldxr x3, [x2]
constexpr u32 stxr_same = 0xc8047c43; // stxr w4, x3, [x2]: stores the loaded value
constexpr u32 stxr_x2 = 0xc8047c45;   // stxr w4, x5, [x2]
constexpr u32 b4 = 0x14000001;        // b .+4: ends the block
constexpr u32 ldaddal = 0xf8e50002;   // ldaddal x5, x2, [x0]
constexpr u32 add_x7 = 0x8b0a0127;    // add x7, x9, x10
constexpr u32 svc0 = 0xd4000001;      // svc #0: halt
constexpr u32 svc1 = 0xd4000021;      // svc #1: clobber, keep going
constexpr u32 svc2 = 0xd4000041;      // svc #2: set X9/X10 through the interface
constexpr u32 bl8 = 0x94000002;       // bl .+8
constexpr u32 b12 = 0x14000003;       // b .+12
constexpr u32 ret = 0xd65f03c0;       // ret
constexpr u32 adr_x16 = 0x10000070;   // adr x16, .+12
constexpr u32 br_x16 = 0xd61f0200;    // br x16

constexpr u64 x9_in = 0x0909090909090909, x10_in = 0x1010101010101010;
constexpr u64 fake = 0x40, abort_at = 0x80;  // unmapped
constexpr u64 junk = 0x8badf00ddeadbeef;

void require(bool ok, const std::string& what) { if (!ok) throw std::runtime_error(what); }
std::string hex(u64 v) { char b[24]; std::snprintf(b, sizeof(b), "%#llx", static_cast<unsigned long long>(v)); return b; }

class Cpu final : private A64::UserCallbacks {
 public:
  // The runtime's code layout and dispatch, or the per-site inline dispatch candidates.
  Cpu(ExclusiveMonitor& monitor, A64::UserConfig::ColdAbortState cold, bool inline_dispatch) {
    A64::UserConfig c;
    c.callbacks = this; c.global_monitor = &monitor; c.processor_id = 0;
    c.fastmem_pointer = 0; c.fastmem_address_space_bits = 64; c.silently_mirror_fastmem = false;
    c.enable_register_cache = true; c.fastmem_exclusive_access = true; c.plain_exclusive_loads = true;
    c.check_halt_on_memory_access = true; c.cold_memory_abort_state = cold;
    c.elide_unchanged_exclusive_stores = true;
    c.enable_cycle_counting = false;
    c.cold_code_region = true; c.compact_block_links = true; c.enable_bounded_halt_polling = true;
    c.fast_dispatch_table_bits = 16; c.fast_dispatch_table_pages = true; c.nzcv_exit_liveness = true;
    c.dispatch_site_cache = !inline_dispatch;
    c.inline_fast_dispatch = inline_dispatch; c.inline_rsb_pop = inline_dispatch;
    c.unsafe_optimizations = true;
    c.optimizations |= OptimizationFlag::Unsafe_IgnoreGlobalMonitor; // As the runtime.
    jit = std::make_unique<A64::Jit>(c);
  }
  HaltReason run(const std::vector<u32>& code, u64 x0) {
    jit->ClearHalt(HaltReason::UserDefined1); jit->ClearHalt(HaltReason::MemoryAbort);
    for (const size_t r : {2, 4, 7}) jit->SetRegister(r, junk);
    jit->SetRegister(0, x0); jit->SetRegister(5, 5);
    jit->SetRegister(9, x9_in); jit->SetRegister(10, x10_in);
    jit->SetPC(reinterpret_cast<std::uintptr_t>(code.data()));
    return jit->Run();
  }
  std::unique_ptr<A64::Jit> jit;
  u64 fake_word = 0;
  unsigned callbacks = 0;

 private:
  void touch(u64 p) {
    ++callbacks; clobber_r10_r11();
    if (p == abort_at) jit->HaltExecution(HaltReason::MemoryAbort);
    else require(p == fake, "access at " + hex(p));
  }
  std::optional<u32> MemoryReadCode(u64 p) override {
    u32 word; std::memcpy(&word, reinterpret_cast<void*>(p), sizeof(word)); return word;
  }
  u64 MemoryRead(u64 p, size_t) override { touch(p); return fake_word; }
  void MemoryWrite(u64 p, u64 v, size_t) override { touch(p); if (p == fake) fake_word = v; }
  bool MemoryWriteExclusive64(u64 p, u64 v, u64 expected) override {
    touch(p);
    if (p != fake || fake_word != expected) return false;
    fake_word = v; return true;
  }
  u64 MemoryAtomic(u64 p, size_t, A64::AtomicOp, u64 v, u64) override {
    touch(p); const u64 old = fake_word; if (p == fake) fake_word += v; return old;
  }
  A64::Vector MemoryRead128(u64 p) override { throw std::runtime_error("unexpected read128 at " + hex(p)); }
  void MemoryWrite128(u64 p, A64::Vector) override { throw std::runtime_error("unexpected write128 at " + hex(p)); }
  A64::Vector MemoryAtomicCompareAndSwap128(u64 p, A64::Vector, A64::Vector) override { throw std::runtime_error("unexpected CASP at " + hex(p)); }
  void CallSVC(u32 imm) override {
    ++callbacks; clobber_r10_r11();
    if (imm == 0) jit->HaltExecution(HaltReason::UserDefined1);
    if (imm == 2) { jit->SetRegister(9, jit->GetRegister(9) + 1); jit->SetRegister(10, jit->GetRegister(10) + 2); }
  }
  void ExceptionRaised(u64 pc, A64::Exception) override { throw std::runtime_error("unexpected exception at " + hex(pc)); }
  void AddTicks(u64) override {}
  u64 GetTicksRemaining() override { return 1000000; }
  u64 GetCNTPCT() override { return 0; }
};

// One access (or none) between X9/X10 writes and their use in X7 = X9 + X10.
struct Program {
  const char* name;
  std::vector<u32> code;
  size_t access_at;  // index of the access, or SIZE_MAX
  bool sets;         // MOVZ X9/X10 before the access
  u64 bump9, bump10; // SVC #2 changes after the access
  bool exclusive_status = false;  // the store-exclusive's W4 status reads 0
};

unsigned run_all(A64::UserConfig::ColdAbortState cold, bool inline_dispatch) {
  const std::string mode = std::string(cold == A64::UserConfig::ColdAbortState::Off ? "cold off" : "cold live") +
                           (inline_dispatch ? " inline dispatch" : "");
  constexpr size_t none = SIZE_MAX;
  const std::vector<Program> programs{
      {"svc", {svc1, add_x7, svc0}, none, false, 0, 0},
      {"svc set", {movz_x9, movz_x10, svc1, add_x7, svc0}, none, true, 0, 0},
      {"svc changes", {movz_x9, movz_x10, svc2, add_x7, svc0}, none, true, 1, 2},
      {"ret", {movz_x9, movz_x10, bl8, b12, ret, svc0, add_x7, svc0}, none, true, 0, 0},
      {"br", {movz_x9, movz_x10, adr_x16, br_x16, svc0, add_x7, svc0}, none, true, 0, 0},
      {"load", {movz_x9, movz_x10, ldr_x0, add_x7, svc0}, 2, true, 0, 0},
      // Set again after the access: the first values are needed only by the
      // abort exit, which must still see them with the cold abort state live.
      {"load set twice", {movz_x9, movz_x10, ldr_x0, movz_x9b, movz_x10b, add_x7, svc0}, 2, true, 0x30ed, 0x30ed},
      {"store", {movz_x9, movz_x10, str_x0, add_x7, svc0}, 2, true, 0, 0},
      {"exclusive", {movz_x9, ldaxr_x0, movz_x10, stlxr_x0, add_x7, svc0}, 1, false, 0, 0},
      // Address and value both outside the cache: the store-exclusive's largest register demand.
      {"exclusive uncached", {movz_x9, movz_x10, mov_x2, ldaxr_x2, stlxr_x2, add_x7, svc0}, 3, true, 0, 0, true},
      // The unchanged-value store is skipped; a changed one is not; a pair split
      // across blocks claims the reservation's generation.
      {"exclusive same", {movz_x9, movz_x10, mov_x2, ldxr_x2, stxr_same, add_x7, svc0}, 3, true, 0, 0, true},
      {"exclusive changed", {movz_x9, movz_x10, mov_x2, ldxr_x2, stxr_x2, add_x7, svc0}, 3, true, 0, 0, true},
      {"exclusive split", {movz_x9, movz_x10, mov_x2, ldaxr_x2, b4, stlxr_x2, add_x7, svc0}, 3, true, 0, 0, true},
      {"atomic", {movz_x9, movz_x10, ldaddal, add_x7, svc0}, 2, true, 0, 0},
  };
  unsigned checks = 0;
  for (const auto& p : programs) {
    const u64 x9 = (p.sets || p.access_at == 1 ? 0x1234 : x9_in) + p.bump9;
    const u64 x10 = (p.sets || p.access_at == 1 ? 0x5678 : x10_in) + p.bump10;
    ExclusiveMonitor monitor(1);
    monitor.EnableGenerations();
    Cpu cpu(monitor, cold, inline_dispatch);
    // Inline (mapped memory), then a fastmem fault and the recompiled callbacks.
    for (int pass = 0; pass < 4; ++pass) {
      const std::string what = mode + " " + p.name + " pass " + std::to_string(pass);
      u64 word = 0x7700;  // bits above the low byte: a status left in the loaded value shows
      cpu.fake_word = 0x7700;
      const auto halt = cpu.run(p.code, pass < 2 ? reinterpret_cast<std::uintptr_t>(&word) : fake);
      auto& jit = *cpu.jit;
      require(Has(halt, HaltReason::UserDefined1) && !Has(halt, HaltReason::MemoryAbort), what + ": halt");
      require(jit.GetRegister(9) == x9 && jit.GetRegister(10) == x10,
              what + ": x9 " + hex(jit.GetRegister(9)) + " x10 " + hex(jit.GetRegister(10)));
      require(jit.GetRegister(7) == x9 + x10, what + ": x7 " + hex(jit.GetRegister(7)));
      require(!p.exclusive_status || jit.GetRegister(4) == 0, what + ": status " + hex(jit.GetRegister(4)));
      ++checks;
    }
    if (p.access_at == none) continue;
    // A memory abort at the access stops there with X9/X10 as before it.
    for (int pass = 0; pass < 3; ++pass) {
      const std::string what = mode + " " + p.name + " abort pass " + std::to_string(pass);
      const auto halt = cpu.run(p.code, abort_at);
      auto& jit = *cpu.jit;
      require(Has(halt, HaltReason::MemoryAbort), what + ": halt");
      require(jit.GetPC() == reinterpret_cast<std::uintptr_t>(p.code.data() + p.access_at), what + ": pc");
      const u64 a9 = p.sets || p.access_at == 1 ? 0x1234 : x9_in, a10 = p.sets ? 0x5678 : x10_in;
      require(jit.GetRegister(9) == a9 && jit.GetRegister(10) == a10,
              what + ": x9 " + hex(jit.GetRegister(9)) + " x10 " + hex(jit.GetRegister(10)));
      ++checks;
    }
    require(cpu.callbacks > 0, mode + " " + p.name + ": no callback ran");
  }
  return checks;
}
}  // namespace

int main() {
  try {
    unsigned checks = 0;
    for (const auto cold : {A64::UserConfig::ColdAbortState::Off, A64::UserConfig::ColdAbortState::Live})
      for (const bool inline_dispatch : {false, true})
        checks += run_all(cold, inline_dispatch);
    std::printf("jit_cached_volatile: %u runs passed\n", checks);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "jit_cached_volatile: %s\n", e.what());
    return 1;
  }
}
