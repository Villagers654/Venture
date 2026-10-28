#include <doctest/doctest.h>
#include "common/environment.h"
#include "core/address_space.h"
#include "core/hle_runtime.h"
#include "core/thread_runtime.h"
#include "core/wait.h"
#include "cpu/state.h"
#include "platform/memory.h"
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "../async_wait.h"

using namespace venture;
namespace {
struct Code {
  GuestAddr address;
  explicit Code(std::initializer_list<std::uint32_t> words) {
    address = AddressSpace::get().allocate(4096, kProtRead | kProtWrite | kProtExec, "[CPU test]");
    if (!address) throw std::bad_alloc();
    std::memcpy(g2h(address), words.begin(), words.size() * 4);
  }
  ~Code() { AddressSpace::get().free(address); }
};
constexpr auto svc = 0xd4000001u | (kSvcReturnToHost << 5);

// Reservation invalidation is an explicit accuracy mode, independent of the
// runtime's value-based exclusive-store policy. Exercise the compiled fastmem
// paths with that mode enabled, including CPUs created after store compilation.
class MonitoredTestCpu final : private Dynarmic::A64::UserCallbacks {
 public:
  MonitoredTestCpu(Dynarmic::ExclusiveMonitor& monitor, std::size_t slot) {
    Dynarmic::A64::UserConfig config{};
    config.callbacks = this;
    config.processor_id = slot;
    config.global_monitor = &monitor;
    config.unsafe_optimizations = false;
    config.optimizations &= ~Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
    config.fastmem_pointer = 0;
    config.fastmem_address_space_bits = 64;
    config.silently_mirror_fastmem = false;
    config.fastmem_exclusive_access = true;
    config.check_halt_on_memory_access = true;
    config.enable_register_cache = true;
    config.enable_cycle_counting = false;
    jit_ = std::make_unique<Dynarmic::A64::Jit>(config);
  }
  void set_pc(GuestAddr p) { jit_->SetPC(p); }
  void set_x(int r, std::uint64_t v) { jit_->SetRegister(r, v); }
  std::uint64_t x(int r) const { return jit_->GetRegister(r); }
  CpuExit run() { exit_ = CpuExit::UndefinedInstruction; jit_->Run(); return exit_; }

 private:
  std::uint64_t MemoryRead(std::uint64_t p, std::size_t size) override {
    std::uint64_t value = 0;
    std::memcpy(&value, g2h(p), size);
    return value;
  }
  void MemoryWrite(std::uint64_t p, std::uint64_t value, std::size_t size) override {
    std::memcpy(g2h(p), &value, size);
  }
  Dynarmic::A64::Vector MemoryRead128(std::uint64_t p) override {
    Dynarmic::A64::Vector value;
    std::memcpy(&value, g2h(p), sizeof(value));
    return value;
  }
  void MemoryWrite128(std::uint64_t p, Dynarmic::A64::Vector value) override {
    std::memcpy(g2h(p), &value, sizeof(value));
  }
  bool MemoryWriteExclusive64(std::uint64_t p, std::uint64_t value, std::uint64_t expected) override {
    return std::atomic_ref(*g2h<std::uint64_t>(p)).compare_exchange_strong(expected, value);
  }
  std::uint64_t MemoryAtomic(std::uint64_t p, std::size_t size, Dynarmic::A64::AtomicOp op,
                            std::uint64_t value, std::uint64_t) override {
    REQUIRE(op == Dynarmic::A64::AtomicOp::Swap);
    if (size == 4) return std::atomic_ref(*g2h<std::uint32_t>(p)).exchange(static_cast<std::uint32_t>(value));
    REQUIRE(size == 8);
    return std::atomic_ref(*g2h<std::uint64_t>(p)).exchange(value);
  }
  Dynarmic::A64::Vector MemoryAtomicCompareAndSwap128(std::uint64_t,
      Dynarmic::A64::Vector, Dynarmic::A64::Vector) override { FAIL("unexpected paired atomic"); return {}; }
  void CallSVC(std::uint32_t) override { exit_ = CpuExit::Svc; jit_->HaltExecution(); }
  void ExceptionRaised(std::uint64_t, Dynarmic::A64::Exception exception) override {
    REQUIRE(exception == Dynarmic::A64::Exception::Breakpoint);
    exit_ = CpuExit::Breakpoint;
    jit_->HaltExecution();
  }
  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return 1000000; }
  std::uint64_t GetCNTPCT() override { return 0; }
  std::unique_ptr<Dynarmic::A64::Jit> jit_;
  CpuExit exit_ = CpuExit::UndefinedInstruction;
};
}
TEST_CASE("core CPU arithmetic, identity loads/stores and NEON") {
  GuestThread thread(20); thread.attach_to_current_host_thread();
  Code add{0x8b010000, 0xd65f03c0}; // add x0,x0,x1; ret
  CHECK(thread.call(add.address, {40, 2}) == 42);
  std::uint64_t word = 9;
  Code memory{0xf9400001, 0x91000421, 0xf9000001, 0xaa0103e0, 0xd65f03c0};
  CHECK(thread.call(memory.address, {h2g(&word)}) == 10);
  CHECK(word == 10);
  Code neon{0x4e208400, 0xd65f03c0}; // add v0.16b,v0.16b,v0.16b
  thread.cpu().set_v(0, {0x0102030405060708, 0x090a0b0c0d0e0f10});
  auto result = thread.call(neon.address, GuestCallArgs{});
  CHECK(result.v0.lo == 0x020406080a0c0e10);
  CHECK(result.v0.hi == 0x121416181a1c1e20);
}
TEST_CASE("core CPU register pressure preserves GPR and SIMD values across spills") {
  auto cpu = cpu_backend().create_cpu();
  Code code{svc};
  std::vector<std::uint32_t> words;
  // Load all 32 vectors before writing any result: their values must remain live
  // across more guest operations than there are host XMM registers.
  for (unsigned i = 0; i < 32; ++i) words.push_back(0x3dc00000u | (i << 10) | i); // ldr qI,[x0,#I*16]
  for (unsigned i = 0; i < 32; ++i) words.push_back(0x4e208400u | (i << 16) | (i << 5) | i); // add vI.16b,vI.16b,vI.16b
  for (unsigned i = 0; i < 32; ++i) words.push_back(0x3d800020u | (i << 10) | i); // str qI,[x1,#I*16]
  // Also keep 26 independent integer results live until the final store group.
  for (unsigned i = 2; i < 28; ++i) words.push_back(0xf9400000u | ((i - 2) << 10) | i); // ldr xI,[x0,#(I-2)*8]
  for (unsigned i = 2; i < 28; ++i) words.push_back(0x91000000u | (i << 10) | (i << 5) | i); // add xI,xI,#I
  for (unsigned i = 2; i < 28; ++i) words.push_back(0xf9000020u | ((i - 2) << 10) | i); // str xI,[x1,#(I-2)*8]
  words.push_back(svc);
  std::memcpy(g2h(code.address), words.data(), words.size() * sizeof(words[0]));
  alignas(16) std::array<std::uint64_t, 64> input{}, output{};
  for (unsigned sample = 0; sample < 16; ++sample) {
    for (unsigned i = 0; i < input.size(); ++i) input[i] = 0x0101010101010101ull * ((i + sample) & 0x7f);
    output.fill(0);
    cpu->set_x(0, h2g(input.data())); cpu->set_x(1, h2g(output.data())); cpu->set_pc(code.address);
    REQUIRE(cpu->run() == CpuExit::Svc);
    for (unsigned i = 0; i < output.size(); ++i) {
      CHECK(output[i] == (i < 26 ? input[i] + i + 2 : input[i] * 2));
    }
  }
}
TEST_CASE("core CPU SVC and nested HLE callback preserve full context") {
  GuestThread thread(21); thread.attach_to_current_host_thread();
  Code add{0x8b010000, 0xd65f03c0};
  auto thunk = hle::make_thunk("test", "nested", [](hle::CallContext& ctx) {
    auto entry = h2g(ctx.user());
    auto value = ctx.thread().call(entry, {ctx.x(0), 7});
    ctx.return_x(value + 1);
  }, g2h(add.address));
  thread.cpu().set_v(31, {0xdead, 0xbeef});
  thread.cpu().set_fpcr(0x400000);
  thread.cpu().set_fpsr(1);
  thread.cpu().set_x(19, 0x12345678);
  auto before_sp = thread.cpu().sp();
  CHECK(thread.call(thunk, {34}) == 42);
  CHECK(thread.cpu().x(19) == 0x12345678);
  CHECK(thread.cpu().v(31).hi == 0xbeef);
  CHECK(thread.cpu().fpcr() == 0x400000);
  CHECK(thread.cpu().fpsr() == 1);
  CHECK(thread.cpu().sp() == before_sp);
}
TEST_CASE("core CPU precise read/write/execute faults and undefined instruction") {
  auto cpu = cpu_backend().create_cpu();
  Code fault{0xd2800541, 0xf9400002, 0x91000421, svc}; // mov x1,42; ldr x2,[x0]; add x1,x1,1
  cpu->set_pc(fault.address); cpu->set_x(0, 0);
  cpu->set_x(2, 0x12345678);
  CHECK(cpu->run() == CpuExit::MemoryFault);
  CHECK(cpu->pc() == fault.address + 4);
  CHECK(cpu->fault().address == 0);
  CHECK(cpu->x(1) == 42);
  CHECK(cpu->x(2) == 0x12345678);
  Code write{0xf9000001, svc};
  auto page = AddressSpace::get().allocate(4096, kProtRead, "[read-only]");
  cpu->set_pc(write.address); cpu->set_x(0, page); cpu->set_x(1, 99);
  CHECK(cpu->run() == CpuExit::MemoryFault);
  CHECK(cpu->fault().write);
  CHECK(cpu->pc() == write.address);
  CHECK(*g2h<std::uint64_t>(page) == 0);
  cpu->set_pc(page);
  CHECK(cpu->run() == CpuExit::MemoryFault);
  CHECK(cpu->fault().execute);
  AddressSpace::get().free(page);
  // Instruction fetch is a guarded host load: code whose host mapping vanished
  // under the bookkeeping (a racing unmap) is a precise fetch fault, not a crash.
  Code vanished{0xd2800541, svc};
  REQUIRE(platform::protect(vanished.address, 4096, kProtNone));
  cpu->set_pc(vanished.address);
  CHECK(cpu->run() == CpuExit::MemoryFault);
  CHECK(cpu->fault().execute);
  CHECK(cpu->pc() == vanished.address);
  REQUIRE(platform::protect(vanished.address, 4096, kProtRead | kProtWrite | kProtExec));
  Code undef{0, svc}; cpu->set_pc(undef.address);
  CHECK(cpu->run() == CpuExit::UndefinedInstruction);
  CHECK(cpu->pc() == undef.address);
  Code breakpoint{0xd4400000, svc}; cpu->set_pc(breakpoint.address);
  CHECK(cpu->run() == CpuExit::Breakpoint);
  CHECK(cpu->pc() == breakpoint.address);
}
// Runs the body with VENTURE_JIT_ADDRESS_FOLD off and on; each CPU reads it when created.
template<class Body> void with_address_folding(Body body) {
  const char* name = "VENTURE_JIT_ADDRESS_FOLD";
  const char* previous = std::getenv(name);
  const std::string saved = previous ? previous : "";
  const auto set = [&](const char* value) {
#ifdef _WIN32
    SetEnvironmentVariableA(name, value);
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
    environment::refresh();
  };
  for (const char* arm : {"0", "1"}) {
    CAPTURE(arm);
    set(arm);
    body();
  }
  set(previous ? saved.c_str() : nullptr);
}
TEST_CASE("core CPU invalidation replaces every block in a range") {
  GuestThread thread(24); thread.attach_to_current_host_thread();
  Code code{};
  for (unsigned i = 0; i < 6; ++i) {
    auto* words = g2h<std::uint32_t>(code.address + i * 32);
    words[0] = 0xd2800020; words[1] = 0xd65f03c0; // mov x0,#1; ret
    REQUIRE(thread.call(code.address + i * 32, {}) == 1);
    words[0] = 0xd2800040; // mov x0,#2
  }
  cpu_backend().invalidate(code.address, 4096);
  for (unsigned i = 0; i < 6; ++i) CHECK(thread.call(code.address + i * 32, {}) == 2);
  auto start = code.address + 0x500;
  auto* words = g2h<std::uint32_t>(start);
  for (unsigned i = 0; i < 14; ++i) words[i] = 0xd503201f; // nop
  words[14] = 0xd2800020; words[15] = 0xd65f03c0;
  for (unsigned i = 1; i <= 6; ++i) REQUIRE(thread.call(start + i * 4, {}) == 1);
  REQUIRE(thread.call(start, {}) == 1);
  words[14] = 0xd2800040;
  cpu_backend().invalidate(start, 4);
  CHECK(thread.call(start, {}) == 2);
  for (unsigned i = 1; i <= 6; ++i) CHECK(thread.call(start + i * 4, {}) == 1);
  cpu_backend().invalidate(start + 56, 4);
  for (unsigned i = 1; i <= 6; ++i) CHECK(thread.call(start + i * 4, {}) == 2);
}
TEST_CASE("core CPU keeps exclusives working beyond 64 live guest CPUs") {
  // Unity titles keep more than 64 native threads alive; the monitor used to cap at 64.
  std::vector<std::unique_ptr<Cpu>> live;
  for (int i = 0; i < 200; ++i) live.push_back(cpu_backend().create_cpu());
  Code increment{0xd2800004, 0xc85f7c02, 0x91000442, 0xc8037c02, 0x35ffffa3,
                 0x91000484, 0xeb01009f, 0x54ffff41, 0xd65f03c0};
  alignas(16) std::uint64_t counter = 0;
  std::array<std::thread, 4> workers;
  for (int i = 0; i < 4; ++i) workers[i] = std::thread([&, i] {
    GuestThread thread(40 + i); thread.attach_to_current_host_thread();
    thread.call(increment.address, {h2g(&counter), 100000});
  });
  for (auto& worker : workers) worker.join();
  CHECK(counter == 400000);
}
TEST_CASE("core CPU CASP pairs and CAS with one register for both operands") {
  auto cpu = cpu_backend().create_cpu();
  alignas(16) std::array<std::uint64_t, 4> memory{1, 2, 0, 0};
  Code pair{0x48227c04, svc}; // casp x2,x3,x4,x5,[x0]
  Code words{0x0862fc04, svc}; // caspal w2,w3,w4,w5,[x0]
  Code same{0xc8a17c01, svc}; // cas x1,x1,[x0]
  cpu->set_pc(pair.address); cpu->set_x(0, h2g(memory.data()));
  cpu->set_x(2, 1); cpu->set_x(3, 2); cpu->set_x(4, 30); cpu->set_x(5, 40);
  REQUIRE(cpu->run() == CpuExit::Svc);
  CHECK(memory[0] == 30); CHECK(memory[1] == 40);
  CHECK(cpu->x(2) == 1); CHECK(cpu->x(3) == 2);
  cpu->set_pc(pair.address); cpu->set_x(2, 30); cpu->set_x(3, 41); cpu->set_x(4, 7); cpu->set_x(5, 8);
  REQUIRE(cpu->run() == CpuExit::Svc);
  CHECK(memory[0] == 30); CHECK(memory[1] == 40);
  CHECK(cpu->x(2) == 30); CHECK(cpu->x(3) == 40);
  memory[2] = 0x0000000200000001ull;
  cpu->set_pc(words.address); cpu->set_x(0, h2g(&memory[2]));
  cpu->set_x(2, 1); cpu->set_x(3, 2); cpu->set_x(4, 0xffffffff00000003ull); cpu->set_x(5, 4);
  REQUIRE(cpu->run() == CpuExit::Svc);
  CHECK(memory[2] == 0x0000000400000003ull);
  CHECK(cpu->x(2) == 1); CHECK(cpu->x(3) == 2);
  cpu->set_pc(words.address); cpu->set_x(2, 3); cpu->set_x(3, 5);
  REQUIRE(cpu->run() == CpuExit::Svc);
  CHECK(memory[2] == 0x0000000400000003ull);
  CHECK(cpu->x(2) == 3); CHECK(cpu->x(3) == 4);
  memory[3] = 9;
  cpu->set_pc(same.address); cpu->set_x(0, h2g(&memory[3])); cpu->set_x(1, 9);
  REQUIRE(cpu->run() == CpuExit::Svc);
  CHECK(memory[3] == 9); CHECK(cpu->x(1) == 9);
  cpu->set_pc(same.address); cpu->set_x(1, 4);
  REQUIRE(cpu->run() == CpuExit::Svc);
  CHECK(memory[3] == 9); CHECK(cpu->x(1) == 9);
  // Unmapped and unaligned addresses raise a precise write fault at the instruction
  // and leave the result register alone: ldsmaxal, ldclral, casal, swpal x1,x2,[x0].
  // The code pages come first so none of them reuses the freed page.
  Code ops[]{Code{0xf8e14002u, svc}, Code{0xb8e11002u, svc}, Code{0xc8e1fc02u, svc}, Code{0xf8e18002u, svc}};
  const auto gone = AddressSpace::get().allocate(4096, kProtRead | kProtWrite, "[LSE unmapped]");
  AddressSpace::get().free(gone);
  for (const Code& code : ops) {
    const bool word = (*g2h<std::uint32_t>(code.address) >> 30) == 2;
    INFO("instruction ", std::hex, *g2h<std::uint32_t>(code.address));
    for (GuestAddr address : {gone + 64, h2g(&memory[0]) + (word ? 2 : 4)}) {
      cpu->set_pc(code.address); cpu->set_x(0, address); cpu->set_x(1, 1); cpu->set_x(2, 77);
      REQUIRE(cpu->run() == CpuExit::MemoryFault);
      CHECK(cpu->pc() == code.address);
      CHECK(cpu->fault().address == address);
      CHECK(cpu->fault().write);
      CHECK(cpu->x(1) == 1);
      CHECK(cpu->x(2) == 77);
    }
  }
  // A doubleword pair needs 16-byte alignment, a word pair 8.
  for (auto [code, address] : {std::pair{pair.address, h2g(&memory[1])}, std::pair{words.address, h2g(&memory[2]) + 4}}) {
    cpu->set_pc(code); cpu->set_x(0, address); cpu->set_x(2, 77);
    REQUIRE(cpu->run() == CpuExit::MemoryFault);
    CHECK(cpu->pc() == code);
    CHECK(cpu->fault().address == address);
    CHECK(cpu->x(2) == 77);
  }
}
TEST_CASE("core CPU LSE and exclusive increments are atomic across threads") {
  Code lse{0xf8e20003, 0xf1000421, 0x54ffffc1, 0xd65f03c0}; // ldaddal; subs; b.ne; ret
  Code exclusive{0xd2800004, 0xc85f7c02, 0x91000442, 0xc8037c02, 0x35ffffa3,
                 0x91000484, 0xeb01009f, 0x54ffff41, 0xd65f03c0};
  alignas(16) std::uint64_t counter = 0;
  std::array<std::thread, 4> workers;
  for (int i = 0; i < 4; ++i) workers[i] = std::thread([&, i] {
    GuestThread thread(60 + i); thread.attach_to_current_host_thread();
    thread.call(i % 2 ? lse.address : exclusive.address, {h2g(&counter), 4000, 1});
  });
  for (auto& worker : workers) worker.join();
  CHECK(counter == 16000);
}
TEST_CASE("core CPU guarded host atomics contain faults and store once") {
  using platform::AtomicRmw;
  alignas(8) std::uint64_t memory = 0xa5a5a5a5a5a5a5feull;
  std::uint64_t old = 0;
  // One access of the given size: the sum wraps inside it and old zero-extends.
  REQUIRE(platform::atomic_rmw(h2g(&memory), 1, AtomicRmw::add, 3, 0, old));
  CHECK(old == 0xfe);
  CHECK(memory == 0xa5a5a5a5a5a5a501ull);
  REQUIRE(platform::atomic_rmw(h2g(&memory) + 2, 2, AtomicRmw::exchange, 0x1234, 0, old));
  CHECK(old == 0xa5a5);
  CHECK(memory == 0xa5a5a5a51234a501ull);
  REQUIRE(platform::atomic_rmw(h2g(&memory), 8, AtomicRmw::compare_exchange, 1, 2, old));
  CHECK(old == 0xa5a5a5a51234a501ull);
  CHECK(memory == 0xa5a5a5a51234a501ull);
  REQUIRE(platform::atomic_rmw(h2g(&memory) + 4, 4, AtomicRmw::compare_exchange, 0xa5a5a5a5, 7, old));
  CHECK(old == 0xa5a5a5a5);
  CHECK(memory == 0x000000071234a501ull);
  CHECK_FALSE(platform::atomic_rmw(h2g(&memory) + 1, 2, AtomicRmw::add, 1, 0, old));
  // Read-only, inaccessible and null pages fault inside the locked instruction;
  // the fault is contained and nothing is stored, even by a matching CAS.
  const auto page = AddressSpace::get().allocate(8192, kProtRead | kProtWrite, "[guarded atomics]");
  *g2h<std::uint32_t>(page) = 5;
  REQUIRE(AddressSpace::get().protect(page, 4096, kProtRead) == 0);
  REQUIRE(AddressSpace::get().protect(page + 4096, 4096, kProtNone) == 0);
  for (auto address : {page, page + 4096, GuestAddr{0}})
    for (auto op : {AtomicRmw::add, AtomicRmw::exchange, AtomicRmw::compare_exchange}) {
      old = 77;
      CHECK_FALSE(platform::atomic_rmw(address, 4, op, 5, 6, old));
      CHECK(old == 77);
    }
  CHECK(*g2h<std::uint32_t>(page) == 5);
  std::uint32_t word = 0;
  CHECK(platform::guarded_load32(page, word));
  CHECK(word == 5);
  CHECK_FALSE(platform::guarded_load32(page + 4096, word));
  // A guest LDADD loop whose page loses access while it runs stops with a
  // precise fault; the monitor lock is released for other CPUs.
  REQUIRE(AddressSpace::get().protect(page, 4096, kProtRead | kProtWrite) == 0);
  Code loop{0xf8e10002, 0x17ffffff}; // ldaddal x1,x2,[x0]; b .-4
  auto cpu = cpu_backend().create_cpu();
  cpu->set_pc(loop.address); cpu->set_x(0, page); cpu->set_x(1, 1);
  std::atomic<bool> finished{false};
  CpuExit result{};
  std::thread worker([&] { result = cpu->run(); finished.store(true); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (__atomic_load_n(g2h<std::uint32_t>(page), __ATOMIC_RELAXED) < 1005 &&
         std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  REQUIRE(AddressSpace::get().protect(page, 4096, kProtNone) == 0);
  while (!finished.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  while (!finished.load()) { cpu->request_halt(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
  worker.join();
  CHECK(result == CpuExit::MemoryFault);
  CHECK(cpu->pc() == loop.address);
  CHECK(cpu->fault().address == page);
  REQUIRE(AddressSpace::get().protect(page, 4096, kProtRead | kProtWrite) == 0);
  CHECK(*g2h<std::uint32_t>(page) >= 1005);
  alignas(16) std::uint64_t counter = 0;
  Code exclusive{0xc85f7c02, 0x91000442, 0xc8037c02, svc}; // ldxr; add; stxr w3
  auto other = cpu_backend().create_cpu();
  other->set_pc(exclusive.address); other->set_x(0, h2g(&counter));
  REQUIRE(other->run() == CpuExit::Svc);
  CHECK(other->x(3) == 0);
  CHECK(counter == 1);
  AddressSpace::get().free(page);
}
TEST_CASE("core CPU LSE loops keep concurrent halt requests") {
  // A request_halt must stop this loop, which never reaches an SVC.
  Code loop{0xf8e10002, 0x17ffffff}; // ldaddal x1,x2,[x0]; b .-4
  alignas(8) std::uint64_t counter = 0;
  auto cpu = cpu_backend().create_cpu();
  cpu->set_pc(loop.address); cpu->set_x(0, h2g(&counter)); cpu->set_x(1, 1);
  std::atomic<unsigned> halts{0};
  std::atomic<bool> done{false}, finished{false};
  std::thread worker([&] {
    while (!done.load()) if (cpu->run() == CpuExit::HaltRequested) halts.fetch_add(1);
    finished.store(true);
  });
  unsigned lost = 0;
  for (int i = 0; i < 3000 && !lost; ++i) {
    const auto before = halts.load();
    cpu->request_halt();
    const auto deadline = std::chrono::steady_clock::now() + venture::test::hang_timeout;
    while (halts.load() == before && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    if (halts.load() == before) {
      FAIL_CHECK("halt acknowledgement missing: iteration=", i, " acknowledgements=", halts.load(),
                 " worker finished=", finished.load());
      ++lost;
    }
  }
  done.store(true);
  while (!finished.load()) { cpu->request_halt(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
  worker.join();
  CHECK(lost == 0);
  CHECK(counter > 0);
}
TEST_CASE("A64 arithmetic conditions use exact NZCV and survive host flag clobbers") {
  auto cpu = cpu_backend().create_cpu();
  auto condition = [](unsigned cond, unsigned flags) {
    const bool n = flags & 8, z = flags & 4, c = flags & 2, v = flags & 1;
    const std::array outcomes{z, !z, c, !c, n, !n, v, !v, c && !z, !c || z,
                              n == v, n != v, !z && n == v, z || n != v};
    return outcomes[cond];
  };
  for (bool wide : {false, true}) for (bool subtract : {false, true}) {
    const unsigned bits = wide ? 64 : 32;
    const std::uint64_t mask = wide ? ~0ull : 0xffffffffull;
    const auto sign = 1ull << (bits - 1);
    const std::array<std::uint64_t, 5> values{0, 1, sign - 1, sign, mask};
    const auto opcode = (subtract ? 0x6b010003u : 0x2b010003u) | (wide ? 0x80000000u : 0);
    for (unsigned cond = 0; cond < 14; ++cond) for (bool clobber : {false, true}) {
      // An unrelated ADD changes host flags but must preserve guest NZCV.
      Code code{opcode, clobber ? 0x91000484u : 0xd503201fu, 0x54000060u | cond,
                0xd2800002, svc, 0xd2800022, svc};
      for (auto a : values) for (auto b : values) {
        const auto result = (subtract ? a - b : a + b) & mask;
        const bool carry = subtract ? a >= b : result < a;
        const bool overflow = ((subtract ? a ^ b : ~(a ^ b)) & (a ^ result) & sign) != 0;
        const unsigned flags = ((result & sign) ? 8 : 0) | (result == 0 ? 4 : 0) |
                               (carry ? 2 : 0) | (overflow ? 1 : 0);
        cpu->set_x(0, a); cpu->set_x(1, b); cpu->set_x(4, ~0ull);
        cpu->set_pstate(0); cpu->set_pc(code.address);
        REQUIRE(cpu->run() == CpuExit::Svc);
        CHECK(cpu->x(2) == static_cast<unsigned>(condition(cond, flags)));
        CHECK(cpu->x(3) == result);
        CHECK(cpu->pstate() == (flags << 28));
      }
    }
  }
  // Conditions with no producer in the block must reload the saved guest flags.
  for (unsigned cond = 0; cond < 14; ++cond) {
    Code code{0x54000060u | cond, 0xd2800002, svc, 0xd2800022, svc};
    for (unsigned flags = 0; flags < 16; ++flags) {
      cpu->set_pstate(flags << 28); cpu->set_pc(code.address);
      REQUIRE(cpu->run() == CpuExit::Svc);
      CHECK(cpu->x(2) == static_cast<unsigned>(condition(cond, flags)));
      CHECK(cpu->pstate() == (flags << 28));
    }
  }
}

TEST_CASE("core CPU fault keeps register, SP, vector and flag writes made earlier in its block") {
  // Get/set elimination with precise aborts: a write that a later instruction in the
  // same block overwrites must still be visible when an access between them faults.
  auto cpu = cpu_backend().create_cpu();
  Code block{0xd2800541,  // mov x1,#42
             0x9e670020,  // fmov d0,x1
             0xd10043ff,  // sub sp,sp,#16
             0xf100a83f,  // cmp x1,#42 (Z,C)
             0xf9400002,  // ldr x2,[x0]
             0x91000421,  // add x1,x1,#1
             0x9e6703e0,  // fmov d0,xzr
             0x910043ff,  // add sp,sp,#16
             0xf101903f,  // cmp x1,#100 (N)
             svc};
  cpu->set_sp(0x10000); cpu->set_pstate(0); cpu->set_v(0, {7, 7});
  cpu->set_x(0, 0); cpu->set_x(1, 0); cpu->set_x(2, 9);
  cpu->set_pc(block.address);
  REQUIRE(cpu->run() == CpuExit::MemoryFault);
  CHECK(cpu->pc() == block.address + 16);
  CHECK(cpu->fault().address == 0);
  CHECK(cpu->x(1) == 42);
  CHECK(cpu->x(2) == 9);
  CHECK(cpu->v(0).lo == 42);
  CHECK(cpu->v(0).hi == 0);
  CHECK(cpu->sp() == 0x10000 - 16);
  CHECK((cpu->pstate() >> 28) == 0b0110);
  std::uint64_t word = 5;
  cpu->set_sp(0x10000); cpu->set_x(0, h2g(&word)); cpu->set_pc(block.address);
  REQUIRE(cpu->run() == CpuExit::Svc);
  CHECK(cpu->x(1) == 43);
  CHECK(cpu->x(2) == 5);
  CHECK(cpu->v(0).lo == 0);
  CHECK(cpu->sp() == 0x10000);
  CHECK((cpu->pstate() >> 28) == 0b1000);
}

#include "core/pc_trace.h"
