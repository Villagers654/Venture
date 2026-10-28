#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/frontend/A64/a64_location_descriptor.h>
#include <dynarmic/frontend/A64/translate/a64_translate.h>
#include <dynarmic/ir/basic_block.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace Dynarmic;
constexpr std::uint64_t base = 0x10000;
struct Callbacks : A64::UserCallbacks {
  std::vector<std::uint32_t> words;
  A64::Jit* jit = nullptr;
  std::optional<std::uint32_t> MemoryReadCode(std::uint64_t p) override {
    if (p < base || (p - base) / 4 >= words.size() || p % 4) return {};
    return words[(p - base) / 4];
  }
  std::uint64_t MemoryRead(std::uint64_t, std::size_t) override { throw std::runtime_error("unexpected read"); }
  A64::Vector MemoryRead128(std::uint64_t) override { throw std::runtime_error("unexpected read128"); }
  void MemoryWrite(std::uint64_t, std::uint64_t, std::size_t) override { throw std::runtime_error("unexpected write"); }
  void MemoryWrite128(std::uint64_t, A64::Vector) override { throw std::runtime_error("unexpected write128"); }
  void InterpreterFallback(std::uint64_t, std::size_t) { throw std::runtime_error("unexpected fallback"); }
  void ExceptionRaised(std::uint64_t, A64::Exception) override { throw std::runtime_error("unexpected exception"); }
  void CallSVC(std::uint32_t) override { jit->HaltExecution(HaltReason::UserDefined1); }
  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return 1000; }
  std::uint64_t MemoryAtomic(std::uint64_t, std::size_t, A64::AtomicOp, std::uint64_t, std::uint64_t) override { throw std::runtime_error("unexpected atomic"); }
  A64::Vector MemoryAtomicCompareAndSwap128(std::uint64_t, A64::Vector, A64::Vector) override { throw std::runtime_error("unexpected atomic128"); }
  std::uint64_t GetCNTPCT() override { return 0; }
};
void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
A64::UserConfig config(Callbacks& cb, bool bounded, bool cycles = false) {
  A64::UserConfig c; c.callbacks = &cb; c.enable_cycle_counting = cycles;
  c.enable_bounded_halt_polling = bounded; c.enable_register_cache = true;
  c.fastmem_pointer = 0; c.fastmem_address_space_bits = 64;
  c.silently_mirror_fastmem = false;
  c.code_cache_size = 16 * 1024 * 1024; return c;
}
void semantics(bool bounded, bool cycles) {
  Callbacks cb; cb.words.assign(1024, 0x91000421); // add x1,x1,#1
  cb.words.push_back(0xd4000021); // svc #1
  A64::Jit jit(config(cb, bounded, cycles)); cb.jit = &jit;
  jit.SetPC(base); jit.SetRegister(1, 0); jit.SetPstate(0xa0000000);
  // Cycle-counted execution may return at a cap and must resume precisely.
  for (unsigned i = 0; i < 10 && jit.GetPC() < base + 4096; ++i) jit.Run();
  if (jit.GetPC() == base + 4096) jit.Run();
  require(jit.GetRegister(1) == 1024, "straight-line register state");
  require(jit.GetPstate() == 0xa0000000, "straight-line NZCV state");
  require(jit.GetPC() == base + 4100, "straight-line exit PC");
  jit.ClearHalt(HaltReason::UserDefined1); jit.SetPC(base); jit.SetRegister(1, 9);
  jit.Step(); require(jit.GetPC() == base + 4 && jit.GetRegister(1) == 10, "single step");
  // Mutate inside a capped block, not just its entry, and invalidate it.
  jit.ClearHalt(HaltReason::Step); cb.words[700] = 0x91000821; // add x1,x1,#2
  jit.InvalidateCacheRange(base + 700 * 4, 4); jit.SetPC(base); jit.SetRegister(1, 0);
  for (unsigned i = 0; i < 10 && jit.GetPC() < base + 4096; ++i) jit.Run();
  if (jit.GetPC() == base + 4096) jit.Run();
  require(jit.GetRegister(1) == 1025, "interior invalidation");
}
void halted(const char* name, std::vector<std::uint32_t> words, bool bounded) {
  Callbacks cb; cb.words = std::move(words); A64::Jit jit(config(cb, bounded)); cb.jit = &jit;
  // Precompile every possible entry; the timed halt excludes first translation.
  for (std::size_t i = 0; i < cb.words.size(); ++i) require(jit.Precompile(base + i * 4), "precompile");
  jit.SetPC(base); jit.SetRegister(0, base);
  std::promise<HaltReason> result; auto ready = result.get_future();
  std::atomic<bool> running{false};
  std::thread worker([&] { running.store(true); result.set_value(jit.Run()); });
  while (!running.load()) std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const auto start = std::chrono::steady_clock::now(); jit.HaltExecution(HaltReason::UserDefined2);
  if (ready.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    // A failed halt must terminate the test rather than hang in join/destruction.
    std::fprintf(stderr, "halt timeout: %s bounded=%d\n", name, bounded); std::fflush(stderr); std::terminate();
  }
  const auto reason = ready.get(); worker.join();
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
  require(Has(reason, HaltReason::UserDefined2), "halt reason");
  require(jit.GetPC() >= base && jit.GetPC() < base + cb.words.size() * 4, "halt PC");
  std::printf("halt name=%s bounded=%d latency_us=%.3f\n", name, bounded, us);
}
}
int main() {
  try {
    // A cap is part of the static latency bound even without a branch.
    Callbacks cb; cb.words.assign(A64::halt_polling_window_size / 4, 0xd503201f); cb.words.push_back(0xd4000021);
    IR::Block block{A64::LocationDescriptor{base, FP::FPCR{0}}};
    A64::Translate(block, A64::LocationDescriptor{base, FP::FPCR{0}}, [&](auto pc) { return cb.MemoryReadCode(pc); }, {false,false,true,true});
    require(A64::LocationDescriptor{block.EndLocation()}.PC() == base + A64::halt_polling_window_size, "translation cap");
    for (bool bounded : {false, true}) for (bool cycles : {false, true}) semantics(bounded, cycles);
    for (bool bounded : {false, true}) {
      halted("backedge", {0x14000000}, bounded);
      // Legacy indirect hits do not poll; this experiment adds the checks.
      if (bounded) for (auto [name, word] : {std::pair{"BR",0xd61f0000u}, {"RET",0xd65f0000u}, {"BLR",0xd63f0000u}})
        halted(name, {word}, true);
      for (auto [name, word] : {std::pair{"conditional-forward",0xb4000020u}, {"direct-forward",0x14000001u}, {"straight-line",0x91000421u}}) {
        std::vector<std::uint32_t> words(256, word);
        words.push_back(0x14000000u | ((-256) & 0x03ffffff));
        halted(name, std::move(words), bounded);
      }
    }
    return 0;
  } catch (const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
