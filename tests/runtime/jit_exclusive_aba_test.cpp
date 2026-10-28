// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic A64 linked-list pop/push with an ABA on the head between one CPU's
// LDAXR and STLXR. With reservation generations (and on the callback path) the
// stale STLXR must fail; plain value compare accepts it and installs a node
// that is in use.
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace Dynarmic;
// Pop: LDAXR x2,[x0]; CBZ x2,+5; LDR x3,[x2]; STLXR w4,x3,[x0]; CBNZ w4,-4; SVC.
constexpr std::array<std::uint32_t, 6> pop{0xc85ffc02, 0xb40000a2, 0xf9400043, 0xc804fc03, 0x35ffff84, 0xd4000001};
// Push x1: LDAXR x2,[x0]; STR x2,[x1]; STLXR w4,x1,[x0]; CBNZ w4,-3; SVC.
constexpr std::array<std::uint32_t, 5> push{0xc85ffc02, 0xf9000022, 0xc804fc01, 0x35ffffa4, 0xd4000001};
// x3 times: LDAXR x2,[x0]; ADD x2,x2,#1; STLXR w4,x2,[x0]; CBNZ w4,-3; then SVC.
constexpr std::array<std::uint32_t, 7> increment{0xc85ffc02, 0x91000442, 0xc804fc02, 0x35ffffa4, 0xd1000463, 0xb5ffff63, 0xd4000001};
// SWPAL x1,x2,[x0]; SVC.
constexpr std::array<std::uint32_t, 2> swap{0xf8e18002, 0xd4000001};
// LDXR x2,[x0]; STXR (or STLXR) w4,x2,[x0]; CBNZ w4,-2; SVC: store the loaded value back.
constexpr std::array<std::uint32_t, 4> touch{0xc85f7c02, 0xc8047c02, 0x35ffffc4, 0xd4000001};
constexpr std::array<std::uint32_t, 4> touch_release{0xc85f7c02, 0xc804fc02, 0x35ffffc4, 0xd4000001};
// x3 times: LDXR x2,[x0]; STXR w4,x2,[x0] in one block, counting failed stores in x5; then SVC.
constexpr std::array<std::uint32_t, 8> touch_count{0xc85f7c02, 0xc8047c02, 0x34000064, 0x910004a5,
                                                   0x17fffffc, 0xd1000463, 0xb5ffff43, 0xd4000001};
// x3 times: plain LL/SC add 16, then plain LL/SC OR x1 (mostly already set); then SVC.
// x5 times: LDXP x2,x3,[x0]; ADD x2,#1; ADD x3,#1; STXP w4,x2,x3,[x0]; CBNZ w4,-4; then SVC.
constexpr std::array<std::uint32_t, 8> increment_pair{0xc87f0c02, 0x91000442, 0x91000463, 0xc8240c02,
                                                      0x35ffff84, 0xd10004a5, 0xb5ffff45, 0xd4000001};
// x5 times: the LDXP/STXP atomic 128-bit read idiom (store back what was loaded),
// counting failed stores in x6 and torn pairs (x2 != x3) in x7; then SVC.
constexpr std::array<std::uint32_t, 10> read_pair{0xc87f0c02, 0xc8240c02, 0x34000064, 0x910004c6, 0x17fffffc,
                                                  0xeb03005f, 0x9a8704e7, 0xd10004a5, 0xb5ffff05, 0xd4000001};
constexpr std::array<std::uint32_t, 11> add_or{0xc85f7c02, 0x91004042, 0xc8047c02, 0x35ffffa4, 0xc85f7c02, 0xaa010042,
                                               0xc8047c02, 0x35ffffa4, 0xd1000463, 0xb5fffee3, 0xd4000001};

struct alignas(64) Word { std::uint64_t value = 0; };
struct alignas(64) Pair { std::uint64_t low = 0, high = 0; };
std::uint64_t address(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }
void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }

class Cpu final : private A64::UserCallbacks {
 public:
  Cpu(ExclusiveMonitor& monitor, std::size_t slot, bool fastmem, bool elide = false) {
    A64::UserConfig c;
    c.callbacks = this; c.global_monitor = &monitor; c.processor_id = slot;
    if (fastmem) c.fastmem_pointer = 0;
    c.fastmem_address_space_bits = 64; c.silently_mirror_fastmem = false;
    c.fastmem_exclusive_access = fastmem; c.plain_exclusive_loads = true;
    c.elide_unchanged_exclusive_stores = elide;
    c.enable_cycle_counting = false; c.check_halt_on_memory_access = true;
    c.unsafe_optimizations = true;
    c.optimizations |= OptimizationFlag::Unsafe_IgnoreGlobalMonitor; // As the runtime.
    jit = std::make_unique<A64::Jit>(c);
  }
  template<std::size_t N, class Target> void start(const std::array<std::uint32_t, N>& code, Target& target) {
    jit->ClearHalt(HaltReason::UserDefined1); jit->ClearHalt(HaltReason::Step);
    jit->SetPC(address(code.data())); jit->SetRegister(0, address(&target)); jit->SetRegister(4, 99);
  }
  std::unique_ptr<A64::Jit> jit;
  unsigned atomic_callbacks = 0;

 private:
  std::optional<std::uint32_t> MemoryReadCode(std::uint64_t p) override {
    std::uint32_t word; std::memcpy(&word, reinterpret_cast<void*>(p), sizeof(word)); return word;
  }
  std::uint64_t MemoryRead(std::uint64_t p, std::size_t size) override {
    std::uint64_t v = 0; std::memcpy(&v, reinterpret_cast<void*>(p), size); return v;
  }
  void MemoryWrite(std::uint64_t p, std::uint64_t v, std::size_t size) override { std::memcpy(reinterpret_cast<void*>(p), &v, size); }
  bool MemoryWriteExclusive64(std::uint64_t p, std::uint64_t v, std::uint64_t expected) override {
    return std::atomic_ref(*reinterpret_cast<std::uint64_t*>(p)).compare_exchange_strong(expected, v);
  }
  A64::Vector MemoryRead128(std::uint64_t) override { throw std::runtime_error("unexpected read128"); }
  bool MemoryWriteExclusive128(std::uint64_t, A64::Vector, A64::Vector) override { throw std::runtime_error("unexpected exclusive write128"); }
  void MemoryWrite128(std::uint64_t, A64::Vector) override { throw std::runtime_error("unexpected write128"); }
  std::uint64_t MemoryAtomic(std::uint64_t p, std::size_t bytes, A64::AtomicOp op, std::uint64_t v, std::uint64_t) override {
    require(bytes == 8 && op == A64::AtomicOp::Swap, "unexpected LSE");
    ++atomic_callbacks;
    return std::atomic_ref(*reinterpret_cast<std::uint64_t*>(p)).exchange(v);
  }
  A64::Vector MemoryAtomicCompareAndSwap128(std::uint64_t, A64::Vector, A64::Vector) override { throw std::runtime_error("unexpected CASP"); }
  void CallSVC(std::uint32_t) override { jit->HaltExecution(HaltReason::UserDefined1); }
  void ExceptionRaised(std::uint64_t, A64::Exception) override { throw std::runtime_error("unexpected exception"); }
  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return 1000000; }
  std::uint64_t GetCNTPCT() override { return 0; }
};

// Returns the reader's first STLXR status after another CPU popped X and Y and
// pushed X back over Z, restoring the head value.
std::uint64_t aba(bool generations, bool fastmem) {
  ExclusiveMonitor monitor(2);
  if (generations) monitor.EnableGenerations();
  Cpu reader(monitor, 0, fastmem), writer(monitor, 1, fastmem);
  Word head;
  std::array<Word, 3> nodes;
  head.value = address(&nodes[0]);
  nodes[0].value = address(&nodes[1]); nodes[1].value = address(&nodes[2]); nodes[2].value = 0;
  reader.start(pop, head);
  for (int i = 0; i < 3; ++i) reader.jit->Step(), reader.jit->ClearHalt(HaltReason::Step);
  require(reader.jit->GetRegister(3) == address(&nodes[1]) && reader.jit->GetPC() == address(pop.data() + 3), "reader pause");
  for (unsigned n = 0; n < 2; ++n) {
    writer.start(pop, head); writer.jit->Run();
    require(writer.jit->GetRegister(4) == 0 && head.value == address(&nodes[n + 1]), "writer pop");
  }
  writer.start(push, head); writer.jit->SetRegister(1, address(&nodes[0])); writer.jit->Run();
  require(writer.jit->GetRegister(4) == 0 && head.value == address(&nodes[0]) && nodes[0].value == address(&nodes[2]), "writer push");
  reader.jit->Step();
  const auto status = reader.jit->GetRegister(4);
  reader.jit->ClearHalt(HaltReason::Step); reader.jit->Run();
  // A failed store retries and pops X correctly, leaving Z.
  require(reader.jit->GetRegister(4) == 0, "reader retry");
  require(head.value == (status ? address(&nodes[2]) : address(&nodes[1])), "reader result");
  return status;
}

// LSE stays inline (no callback) with generations, and the swapped value lands.
void inline_swap(bool fastmem) {
  ExclusiveMonitor monitor(1);
  monitor.EnableGenerations();
  Cpu cpu(monitor, 0, fastmem);
  Word word; word.value = 7;
  cpu.start(swap, word); cpu.jit->SetRegister(1, 9); cpu.jit->Run();
  require(cpu.jit->GetRegister(2) == 7 && word.value == 9, "swap result");
  require(fastmem ? cpu.atomic_callbacks == 0 : cpu.atomic_callbacks == 1, "swap path");
}
// Contended LL/SC increments stay exact and keep making progress.
void contended() {
  constexpr unsigned cpus = 4, per_cpu = 100000;
  ExclusiveMonitor monitor(cpus);
  monitor.EnableGenerations();
  Word counter;
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < cpus; ++i) threads.emplace_back([&, i] {
    Cpu cpu(monitor, i, true);
    cpu.start(increment, counter); cpu.jit->SetRegister(3, per_cpu); cpu.jit->Run();
  });
  for (auto& thread : threads) thread.join();
  require(counter.value == cpus * per_cpu, "contended increments lost an update");
}
// The reader loads 5, another CPU adds 16, then the reader stores 5 back. Single
// stepping puts the STXR in its own block, so it is never elided: plain or
// release, it fails and the retry stores 21 back. Returns the first status.
std::uint64_t unchanged_store(bool elide, bool release) {
  ExclusiveMonitor monitor(2);
  monitor.EnableGenerations();
  Cpu reader(monitor, 0, true, elide), writer(monitor, 1, true, elide);
  Word word; word.value = 5;
  if (release) reader.start(touch_release, word); else reader.start(touch, word);
  reader.jit->Step(); reader.jit->ClearHalt(HaltReason::Step);
  require(reader.jit->GetRegister(2) == 5, "touch load");
  writer.start(add_or, word); writer.jit->SetRegister(1, 0); writer.jit->SetRegister(3, 1); writer.jit->Run();
  require(word.value == 21, "writer add");
  reader.jit->Step();
  const auto status = reader.jit->GetRegister(4);
  reader.jit->ClearHalt(HaltReason::Step); reader.jit->Run();
  require(reader.jit->GetRegister(4) == 0 && word.value == 21, "touch result");
  return status;
}
// Contended plain LL/SC adds stay exact next to ORs that mostly store the loaded
// value back and a CPU that only stores back what it loaded, and every bit lands.
// Elided, the store-back pairs (no access between LDXR and STXR) never fail.
void contended_add_or(bool elide) {
  constexpr unsigned cpus = 4, per_cpu = 50000;
  ExclusiveMonitor monitor(cpus + 1);
  monitor.EnableGenerations();
  Word word;
  std::uint64_t touch_failures = 0;
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < cpus; ++i) threads.emplace_back([&, i] {
    Cpu cpu(monitor, i, true, elide);
    cpu.start(add_or, word); cpu.jit->SetRegister(1, 1u << i); cpu.jit->SetRegister(3, per_cpu); cpu.jit->Run();
  });
  threads.emplace_back([&] {
    Cpu cpu(monitor, cpus, true, elide);
    cpu.start(touch_count, word); cpu.jit->SetRegister(3, per_cpu); cpu.jit->SetRegister(5, 0); cpu.jit->Run();
    touch_failures = cpu.jit->GetRegister(5);
  });
  for (auto& thread : threads) thread.join();
  require(word.value == (16ull * cpus * per_cpu | 0xf), "contended add/or lost an update");
  require(!elide || touch_failures == 0, "an adjacent unchanged STXR failed with elision");
}
// Contended LDXP/STXP increments of both halves stay exact next to a CPU running
// the LDXP/STXP atomic read idiom: it never sees a torn pair, and elided it never
// fails (its load is single-copy atomic: VMOVDQA with AVX, else LOCK CMPXCHG16B).
void contended_pair(bool elide) {
  constexpr unsigned cpus = 4, per_cpu = 50000;
  ExclusiveMonitor monitor(cpus + 1);
  monitor.EnableGenerations();
  Pair pair;
  std::uint64_t read_failures = 0, torn = 0;
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < cpus; ++i) threads.emplace_back([&, i] {
    Cpu cpu(monitor, i, true, elide);
    cpu.start(increment_pair, pair); cpu.jit->SetRegister(5, per_cpu); cpu.jit->Run();
  });
  threads.emplace_back([&] {
    Cpu cpu(monitor, cpus, true, elide);
    cpu.start(read_pair, pair); cpu.jit->SetRegister(5, per_cpu);
    cpu.jit->SetRegister(6, 0); cpu.jit->SetRegister(7, 0); cpu.jit->Run();
    read_failures = cpu.jit->GetRegister(6); torn = cpu.jit->GetRegister(7);
  });
  for (auto& thread : threads) thread.join();
  require(pair.low == cpus * per_cpu && pair.high == cpus * per_cpu, "contended pair increments lost an update");
  require(torn == 0, "the LDXP/STXP read idiom saw a torn pair");
  require(!elide || read_failures == 0, "an adjacent unchanged STXP failed with elision");
}
} // namespace

int main() {
  try {
    require(aba(true, true) == 1, "reservation generations accepted the ABA store");
    require(aba(true, false) == 1, "callback exclusives accepted the ABA store");
    for (bool fastmem : {true, false}) inline_swap(fastmem);
    contended();
    require(unchanged_store(true, false) == 1, "a separate-block plain STXR was elided");
    require(unchanged_store(true, true) == 1, "release STLXR was elided");
    require(unchanged_store(false, false) == 1, "plain STXR without elision accepted a changed word");
    for (bool elide : {true, false}) contended_add_or(elide);
    for (bool elide : {true, false}) contended_pair(elide);
    // Negative control: inline value compare without generations accepts it.
    require(aba(false, true) == 0, "value-compare control rejected the ABA store");
    std::puts("jit exclusive ABA: ok");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "jit exclusive ABA: %s\n", e.what());
    return 1;
  }
}
