// The native-a64 engine on a Linux AArch64 host (cpu/native_a64_backend.cpp):
// guest code runs directly, raw system calls trap through the seccomp filter,
// and a halt from another thread leaves a tight loop and resumes it with every
// register, x17 included. The other core CPU tests run on the same engine when
// the host picks it; on any other engine these cases do nothing.
#include <doctest/doctest.h>

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string_view>
#include <thread>

#include "core/address_space.h"
#include "cpu/cpu.h"
#include "cpu/native_a64.h"

using namespace venture;
namespace {
struct Code {
  GuestAddr address;
  explicit Code(std::initializer_list<std::uint32_t> words) {
    address = AddressSpace::get().allocate(4096, kProtRead | kProtWrite | kProtExec, "[native-a64 test]");
    if (!address) throw std::bad_alloc();
    std::memcpy(g2h(address), words.begin(), words.size() * 4);
    cpu_backend().invalidate(address, 4096, InvalidationCause::code_patch);
  }
  ~Code() { AddressSpace::get().free(address); }
};
constexpr std::uint32_t kReturn = 0xd4000001u | (kSvcReturnToHost << 5);
bool native() { return std::string_view(cpu_backend().info().name) == "native-a64"; }
}  // namespace

TEST_CASE("native-a64 traps raw guest system calls and Venture's trap SVCs") {
  if (!native()) return;
  auto cpu = cpu_backend().create_cpu();
  Code code{0xd2801588u /* mov x8, #172 */, 0xd4000001u /* svc #0 */, kReturn};
  cpu->set_pc(code.address);
  cpu->set_x(0, 5);
  REQUIRE(cpu->run() == CpuExit::Svc);
  CHECK(cpu->svc_immediate() == kSvcLinuxSyscall);
  CHECK(cpu->x(8) == 172);
  CHECK(cpu->x(0) == 5);
  CHECK(cpu->pc() == code.address + 8);
  cpu->set_x(0, 1234);  // the system call's result, as the thread loop writes it
  REQUIRE(cpu->run() == CpuExit::Svc);
  CHECK(cpu->svc_immediate() == kSvcReturnToHost);
  CHECK(cpu->x(0) == 1234);
  CHECK(cpu->x(8) == 172);
}

TEST_CASE("native-a64 traps system calls from guest code mapped at a fixed address outside the arenas") {
  if (!native()) return;
  const auto [arena_begin, arena_end] = AddressSpace::get().arena64();
  // A free address below the arenas, executable from the start or made so by mprotect.
  for (const std::uint32_t initial : {kProtRead | kProtWrite | kProtExec, kProtRead | kProtWrite}) {
    CAPTURE(initial);
    std::int64_t mapped = -1;
    for (GuestAddr candidate = 0x4000000000ull; candidate < 0x8000000000ull && mapped < 0; candidate += 0x100000000ull) {
      if (candidate >= arena_begin && candidate < arena_end) continue;
      mapped = AddressSpace::get().map(candidate, 65536, initial,
                                       kMapPrivate | kMapAnonymous | kMapFixed | kMapFixedNoReplace, nullptr, 0, "[fixed code]");
    }
    REQUIRE(mapped > 0);
    const auto address = static_cast<GuestAddr>(mapped);
    const std::uint32_t words[] = {0xd2801588u /* mov x8, #172 */, 0xd4000001u /* svc #0 */, kReturn};
    std::memcpy(g2h(address + 0x8000), words, sizeof words);
    if (!(initial & kProtExec)) REQUIRE(AddressSpace::get().protect(address, 65536, kProtRead | kProtExec) == 0);
    cpu_backend().invalidate(address, 65536, InvalidationCause::code_patch);
    auto cpu = cpu_backend().create_cpu();
    cpu->set_pc(address + 0x8000);
    REQUIRE(cpu->run() == CpuExit::Svc);
    CHECK(cpu->svc_immediate() == kSvcLinuxSyscall);
    CHECK(cpu->pc() == address + 0x8008);
    REQUIRE(cpu->run() == CpuExit::Svc);
    CHECK(cpu->svc_immediate() == kSvcReturnToHost);
    // Kept mapped, so the second round takes a fresh address.
  }
}

TEST_CASE("native-a64 halts a guest loop from another thread and resumes it with every register") {
  if (!native()) return;
  auto cpu = cpu_backend().create_cpu();
  Code loop{0x91000421u /* add x1, x1, #1 */, 0x17ffffffu /* b .-4 */};
  cpu->set_pc(loop.address);
  cpu->set_x(1, 0);
  cpu->set_x(17, 0x1717171717171717ull);
  cpu->set_v(31, {0x3131, 0x3232});
  std::uint64_t last = 0;
  for (int round = 0; round < 3; ++round) {
    CAPTURE(round);
    std::thread halter([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      cpu->request_halt();
    });
    const auto exit = cpu->run();
    halter.join();
    REQUIRE(exit == CpuExit::HaltRequested);
    CHECK(cpu->x(1) > last);
    last = cpu->x(1);
    CHECK(cpu->x(17) == 0x1717171717171717ull);
    CHECK(cpu->v(31).lo == 0x3131);
    CHECK(cpu->v(31).hi == 0x3232);
    CHECK((cpu->pc() == loop.address || cpu->pc() == loop.address + 4));
  }
}

namespace {
// A JVM-style handler: SA_SIGINFO, reads host TLS and the interrupted context.
thread_local std::uint64_t host_marker = 0;
std::atomic<std::uint64_t> handler_tp{0}, handler_marker{0}, handler_pc{0};
std::atomic<int> handler_calls{0};
void jvm_style_handler(int, siginfo_t*, void* raw) {
  std::uint64_t tp;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  handler_tp.store(tp);
  handler_marker.store(host_marker);
  handler_pc.store(static_cast<ucontext_t*>(raw)->uc_mcontext.pc);
  handler_calls.fetch_add(1);
}
}  // namespace

TEST_CASE("native-a64 runs other host signal handlers on the host thread pointer") {
  if (!native()) return;
  // Libraries (the JVM) bind to the executable's sigaction and signal, not libc's.
  using Sigaction = int (*)(int, const struct sigaction*, struct sigaction*);
  CHECK(reinterpret_cast<Sigaction>(dlsym(RTLD_DEFAULT, "sigaction")) == static_cast<Sigaction>(&sigaction));
  CHECK(dlsym(RTLD_DEFAULT, "signal") == reinterpret_cast<void*>(&signal));
  struct sigaction action{}, old{}, seen{};
  action.sa_sigaction = jvm_style_handler;
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&action.sa_mask);
  REQUIRE(sigaction(SIGUSR2, &action, &old) == 0);
  // Host code sees its own action, not the engine's guard.
  REQUIRE(sigaction(SIGUSR2, nullptr, &seen) == 0);
  CHECK(seen.sa_sigaction == jvm_style_handler);
  CHECK((seen.sa_flags & SA_SIGINFO));

  Code loop{0x91000421u /* add x1, x1, #1 */, 0x17ffffffu /* b .-4 */};
  auto cpu = cpu_backend().create_cpu();
  cpu->set_pc(loop.address);
  cpu->set_x(1, 0);
  std::atomic<std::uint64_t> worker_tp{0};
  std::atomic<bool> started{false};
  CpuExit exit{};
  std::thread worker([&] {
    host_marker = 0x5157'5157;
    std::uint64_t tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    worker_tp.store(tp);
    started.store(true);
    exit = cpu->run();
  });
  while (!started.load()) std::this_thread::yield();
  const auto handle = worker.native_handle();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  // Signal until one lands in guest code (the first may arrive before it starts).
  while (std::chrono::steady_clock::now() < deadline) {
    pthread_kill(handle, SIGUSR2);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto pc = handler_pc.load();
    if (handler_calls.load() && pc >= loop.address && pc < loop.address + 8) break;
  }
  const bool landed = handler_calls.load() > 0;
  cpu->request_halt();
  worker.join();
  REQUIRE(exit == CpuExit::HaltRequested);
  REQUIRE(landed);
  CHECK(handler_pc.load() >= loop.address);
  CHECK(handler_pc.load() < loop.address + 8);
  CHECK(handler_tp.load() == worker_tp.load());
  CHECK(handler_marker.load() == 0x5157'5157);
  // The guest ran on after the handler, on its own thread pointer.
  CHECK(cpu->x(1) > 0);
  CHECK((cpu->pc() == loop.address || cpu->pc() == loop.address + 4));
  REQUIRE(sigaction(SIGUSR2, &old, nullptr) == 0);
}

TEST_CASE("native-a64 reports faults precisely and keeps the host thread pointer") {
  if (!native()) return;
  std::uint64_t host_tp;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(host_tp));
  auto cpu = cpu_backend().create_cpu();
  Code fault{0xd2800541u /* mov x1, #42 */, 0xf9400002u /* ldr x2, [x0] */, kReturn};
  cpu->set_pc(fault.address);
  cpu->set_x(0, 0);
  REQUIRE(cpu->run() == CpuExit::MemoryFault);
  CHECK(cpu->pc() == fault.address + 4);
  CHECK(cpu->fault().address == 0);
  CHECK_FALSE(cpu->fault().write);
  CHECK(cpu->x(1) == 42);
  std::uint64_t after;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(after));
  CHECK(after == host_tp);
  // Resumed at the load with a valid address, it runs on.
  std::uint64_t word = 7;
  cpu->set_x(0, h2g(&word));
  REQUIRE(cpu->run() == CpuExit::Svc);
  CHECK(cpu->x(2) == 7);
}
