// SPDX-License-Identifier: GPL-3.0-or-later
// Store-buffering litmus on two JIT threads: STR; DMB ISH; LDR must never let
// both loads miss the other thread's store. DMB ISHST emits nothing on x64
// (TSO orders store-store), so its count of both-zero outcomes is reported as
// a sensitivity control, not checked.
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {
using namespace Dynarmic;
// STR x1,[x0]; DMB <option>; LDR x3,[x2]; SVC.
constexpr std::uint32_t dmb_ish = 0xd5033bbf, dmb_ishst = 0xd5033abf;

struct alignas(64) Word { std::uint64_t value = 0; };
std::uint64_t address(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }

class Cpu final : private A64::UserCallbacks {
 public:
  Cpu(ExclusiveMonitor& monitor, std::size_t slot, std::uint32_t barrier) : code{0xf9000001, barrier, 0xf9400043, 0xd4000001} {
    A64::UserConfig c;
    c.callbacks = this; c.global_monitor = &monitor; c.processor_id = slot;
    c.fastmem_pointer = 0; c.fastmem_address_space_bits = 64; c.silently_mirror_fastmem = false;
    c.enable_cycle_counting = false; c.check_halt_on_memory_access = true;
    jit = std::make_unique<A64::Jit>(c);
  }
  // Stores 1 to mine, then returns the value loaded from theirs.
  std::uint64_t run(Word& mine, Word& theirs) {
    jit->ClearHalt(HaltReason::UserDefined1);
    jit->SetPC(address(code.data())); jit->SetRegister(0, address(&mine));
    jit->SetRegister(1, 1); jit->SetRegister(2, address(&theirs));
    jit->Run();
    return jit->GetRegister(3);
  }

 private:
  std::array<std::uint32_t, 4> code;
  std::unique_ptr<A64::Jit> jit;
  std::optional<std::uint32_t> MemoryReadCode(std::uint64_t p) override {
    std::uint32_t word; std::memcpy(&word, reinterpret_cast<void*>(p), sizeof(word)); return word;
  }
  std::uint64_t MemoryRead(std::uint64_t, std::size_t) override { throw std::runtime_error("unexpected read callback"); }
  void MemoryWrite(std::uint64_t, std::uint64_t, std::size_t) override { throw std::runtime_error("unexpected write callback"); }
  A64::Vector MemoryRead128(std::uint64_t) override { throw std::runtime_error("unexpected read128"); }
  void MemoryWrite128(std::uint64_t, A64::Vector) override { throw std::runtime_error("unexpected write128"); }
  std::uint64_t MemoryAtomic(std::uint64_t, std::size_t, A64::AtomicOp, std::uint64_t, std::uint64_t) override { throw std::runtime_error("unexpected atomic"); }
  A64::Vector MemoryAtomicCompareAndSwap128(std::uint64_t, A64::Vector, A64::Vector) override { throw std::runtime_error("unexpected CASP"); }
  void CallSVC(std::uint32_t) override { jit->HaltExecution(HaltReason::UserDefined1); }
  void ExceptionRaised(std::uint64_t, A64::Exception) override { throw std::runtime_error("unexpected exception"); }
  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return 1000000; }
  std::uint64_t GetCNTPCT() override { return 0; }
};

// Both-zero outcomes over the given number of rounds.
unsigned both_zero(std::uint32_t barrier, unsigned rounds) {
  ExclusiveMonitor monitor(2);
  Cpu a(monitor, 0, barrier), b(monitor, 1, barrier);
  Word x, y;
  std::atomic<unsigned> start{0}, done{0};
  std::uint64_t ra = 0;
  unsigned zeros = 0;
  std::thread other([&] {
    for (unsigned r = 1; r <= rounds; ++r) {
      while (start.load(std::memory_order_acquire) != r) {}
      ra = a.run(x, y);
      done.store(r, std::memory_order_release);
    }
  });
  for (unsigned r = 1; r <= rounds; ++r) {
    x.value = y.value = 0;
    start.store(r, std::memory_order_release);
    const auto rb = b.run(y, x);
    while (done.load(std::memory_order_acquire) != r) {}
    zeros += ra == 0 && rb == 0;
  }
  other.join();
  return zeros;
}
}  // namespace

int main() {
  try {
    constexpr unsigned rounds = 200000;
    const unsigned control = both_zero(dmb_ishst, rounds);
    const unsigned full = both_zero(dmb_ish, rounds);
    std::printf("jit barrier: DMB ISH both-zero %u/%u, DMB ISHST control %u/%u\n", full, rounds, control, rounds);
    if (full) throw std::runtime_error("DMB ISH let a load pass an earlier store");
    std::puts("jit barrier: ok");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "jit barrier: %s\n", e.what());
    return 1;
  }
}
