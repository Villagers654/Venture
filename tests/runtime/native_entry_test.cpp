// The native entry (cpu/native_entry.h): the slots every host writes, and on
// an AArch64 host the routines themselves, called for real: in place on
// Android, through venture_native_enter with a separate guest thread pointer
// on Linux.
#include "cpu/native_entry.h"

#include <doctest/doctest.h>

#include <cstring>

#if defined(__aarch64__)
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace venture;

namespace {
constexpr NativeRoutines kFakeRoutines = {0x1111000011110000ull, 0x2222000022220000ull, 0x3333000033330000ull,
                                          0x4444000044440000ull, 0x5555000055550000ull, 0x6666000066660000ull};
std::uint64_t literal(const StubCode& code, std::size_t word) {
  return code.words[word] | (std::uint64_t{code.words[word + 1]} << 32);
}
}  // namespace

TEST_CASE("native entry slots load the id, keep the patch area and branch to the kind's routine") {
  const NativeEntry entry(kFakeRoutines);
  CHECK(std::string_view(entry.name()) == "native-a64");
  constexpr std::uint32_t id = 0x00345678;
  const std::pair<std::uint32_t, NativeCall> kinds[] = {
      {kSvcHleCall, NativeCall::hle_call}, {kSvcHleCallFast, NativeCall::hle_call_fast},
      {kSvcHleCallFastFp, NativeCall::hle_call_fast_fp}, {kSvcHleCallFastGuestFp, NativeCall::hle_call_fast_guest_fp},
      {kSvcEntryHook, NativeCall::entry_hook}};
  for (const auto& [svc, call] : kinds) {
    CHECK(native_call_svc(call) == svc);
    CHECK(native_call_for_svc(svc) == call);
    const auto code = entry.stub(GuestIsa::a64, GuestStub::hle_call, id, svc);
    REQUIRE(code.count == 8);
    CHECK(code.words[0] == (0xd2800010u | (0x5678u << 5)));  // movz x16, #0x5678
    CHECK(code.words[1] == (0xf2a00010u | (0x0034u << 5)));  // movk x16, #0x34, lsl #16
    CHECK(code.words[2] == 0xd503201fu);
    CHECK(code.words[3] == 0xd503201fu);
    CHECK(code.words[4] == 0x58000051u);  // ldr x17, #8
    CHECK(code.words[5] == 0xd61f0220u);  // br x17
    CHECK(literal(code, 6) == kFakeRoutines[static_cast<std::size_t>(call)]);
  }
  CHECK(native_call_for_svc(kSvcReturnToHost) == NativeCall::return_to_host);
  CHECK_FALSE(native_call_for_svc(kSvcLinuxSyscall).has_value());
  CHECK_THROWS(entry.stub(GuestIsa::a64, GuestStub::hle_call, id, kSvcReturnToHost));

  const auto back = entry.stub(GuestIsa::a64, GuestStub::return_to_host, 0, kSvcHleCall);
  REQUIRE(back.count == 4);
  CHECK(back.words[0] == 0x58000051u);
  CHECK(literal(back, 2) == kFakeRoutines[5]);
  const auto call_return = entry.stub(GuestIsa::a64, GuestStub::call_return, 3, kSvcHleCall);
  REQUIRE(call_return.count == 6);
  CHECK(call_return.words[0] == (0xd2800010u | (3u << 5)));
  CHECK(call_return.words[2] == 0x58000051u);  // the literal at byte 16, 8-byte aligned
  CHECK(literal(call_return, 4) == kFakeRoutines[5]);

  // Guest code stubs and A32 are the trap entry's.
  for (const auto kind : {GuestStub::sigreturn, GuestStub::tlsdesc, GuestStub::tlsdesc_weak})
    CHECK(entry.stub(GuestIsa::a64, kind, 1, kSvcHleCall).words == guest_stub(GuestIsa::a64, kind, 1).words);
  CHECK(entry.stub(GuestIsa::a32, GuestStub::hle_call, id, kSvcHleCallFast).words ==
        guest_stub(GuestIsa::a32, GuestStub::hle_call, id, kSvcHleCallFast).words);
}

TEST_CASE("a NativeContextCpu reads and writes the context for handlers") {
  NativeContext context{};
  NativeContextCpu cpu(context);
  cpu.set_x(3, 42);
  cpu.set_sp(0x1000);
  cpu.set_pc(0x2000);
  cpu.set_v(1, Vec128{7, 9});
  cpu.set_pstate(0xf000000fu);
  CHECK(context.x[3] == 42);
  CHECK(context.sp == 0x1000);
  CHECK(context.pc == 0x2000);
  CHECK(context.q[1][0] == 7);
  CHECK(context.q[1][1] == 9);
  CHECK(cpu.pstate() == 0xf0000000u);
  CHECK(cpu.hle_call_id() == context.x[16]);
  cpu.request_halt();
  CHECK(context.halt == 1);
  CHECK_THROWS(cpu.run());
}

TEST_CASE("without a dispatcher a native call reports an HLE exception") {
  set_native_dispatcher(nullptr);
  NativeContext context{};
  CHECK(venture_native_dispatch(&context) == VENTURE_NATIVE_EXIT_HLE_EXCEPTION);
}

#if defined(__aarch64__)
namespace {
constexpr std::uint32_t kAddId = 0x00010007;
thread_local int host_tls_marker = 0;
int dispatcher_saw_marker = -1;
NativeContext seen{};

// x0 + ... + x7 + the first stack argument, and d0 * 2 in d0.
std::uint32_t test_dispatcher(NativeContext& context) {
  dispatcher_saw_marker = host_tls_marker;  // host TLS works: the routine switched to the host
  seen = context;
  NativeContextCpu cpu(context);
  std::uint64_t sum = 0;
  for (int i = 0; i < 8; ++i) sum += cpu.x(i);
  std::uint64_t stack0;
  std::memcpy(&stack0, reinterpret_cast<const void*>(cpu.sp()), sizeof(stack0));
  cpu.set_x(0, sum + stack0);
  cpu.set_x(1, cpu.x(16));
  double d0;
  std::memcpy(&d0, &context.q[0][0], sizeof(d0));
  d0 *= 2;
  std::uint64_t bits;
  std::memcpy(&bits, &d0, sizeof(bits));
  cpu.set_v(0, Vec128{bits, 0});
  return VENTURE_NATIVE_EXIT_NONE;
}

// Executable copies of slots written by the native entry.
class SlotPage {
 public:
  SlotPage() {
    size_ = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    page_ = static_cast<std::uint32_t*>(mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(page_ != MAP_FAILED);
  }
  ~SlotPage() { munmap(page_, size_); }
  void* add(const StubCode& code) {
    auto* at = page_ + used_;
    std::memcpy(at, code.words.data(), code.count * 4);
    used_ += 8;
    return at;
  }
  void seal() {
    REQUIRE(mprotect(page_, size_, PROT_READ | PROT_EXEC) == 0);
    __builtin___clear_cache(reinterpret_cast<char*>(page_), reinterpret_cast<char*>(page_ + used_));
  }

 private:
  std::uint32_t* page_;
  std::size_t size_;
  std::size_t used_ = 0;
};
}  // namespace

#if defined(__ANDROID__)
using AddFunction = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t, std::uint64_t, std::uint64_t, double, std::uint64_t);

TEST_CASE("Android: a native call runs its handler in place on the caller's stack, for every kind") {
  set_native_dispatcher(test_dispatcher);
  host_tls_marker = 77;
  SlotPage page;
  void* slots[5];
  const std::uint32_t svcs[] = {kSvcHleCall, kSvcHleCallFast, kSvcHleCallFastFp, kSvcHleCallFastGuestFp, kSvcEntryHook};
  for (int i = 0; i < 5; ++i) slots[i] = page.add(native_entry().stub(GuestIsa::a64, GuestStub::hle_call, kAddId, svcs[i]));
  page.seal();
  for (int i = 0; i < 5; ++i) {
    auto add = reinterpret_cast<AddFunction>(slots[i]);
    const auto result = add(1, 2, 3, 4, 5, 6, 7, 8, 1.25, 100);
    CHECK(result == 136);
    CHECK(seen.x[16] == kAddId);
    CHECK(seen.kind == svcs[i]);
    CHECK(seen.flags == VENTURE_NATIVE_FLAG_SHARED_TP);
    CHECK(dispatcher_saw_marker == 77);
  }
  set_native_dispatcher(nullptr);
}
#else
namespace {
// What a guest thread's TLS block looks like to the routines: the context
// pointer at [TPIDR_EL0 - 16].
struct alignas(16) FakeGuestTls {
  NativeContext* context;
  std::uint64_t pad;
  std::uint64_t slots[8];
};
}  // namespace

TEST_CASE("Linux: guest code enters through the slots and leaves run() with the host thread pointer back") {
  set_native_dispatcher(test_dispatcher);
  host_tls_marker = 55;
  SlotPage page;
  void* fast = page.add(native_entry().stub(GuestIsa::a64, GuestStub::hle_call, kAddId, kSvcHleCallFast));
  void* run_loop = page.add(native_entry().stub(GuestIsa::a64, GuestStub::hle_call, kAddId, kSvcHleCall));
  void* back = page.add(native_entry().stub(GuestIsa::a64, GuestStub::return_to_host, 0, kSvcHleCall));
  page.seal();

  static NativeContext context{};
  static FakeGuestTls tls{};
  alignas(16) static std::uint64_t stack[512];
  tls.context = &context;
  const auto guest_tp = reinterpret_cast<std::uint64_t>(&tls.slots[0]);
  const auto host_tp = [] { std::uint64_t v; asm volatile("mrs %0, tpidr_el0" : "=r"(v)); return v; };
  const auto reset = [&](void* pc) {
    context = {};
    for (int i = 0; i < 8; ++i) context.x[i] = static_cast<std::uint64_t>(i + 1);
    for (int i = 19; i <= 28; ++i) context.x[i] = 0x1900 + static_cast<std::uint64_t>(i);
    context.x[30] = reinterpret_cast<std::uint64_t>(back);
    context.sp = reinterpret_cast<std::uint64_t>(&stack[256]);
    stack[256] = 100;
    const double d0 = 1.25;
    std::memcpy(&context.q[0][0], &d0, sizeof(d0));
    context.pc = reinterpret_cast<std::uint64_t>(pc);
    context.tpidr = guest_tp;
    asm volatile("mrs %0, fpcr" : "=r"(context.host_fpcr));
    context.fpcr = static_cast<std::uint32_t>(context.host_fpcr);
  };

  // An InJit call completes in place, then the guest returns to the host.
  reset(fast);
  const auto before = host_tp();
  CHECK(venture_native_enter(&context) == VENTURE_NATIVE_EXIT_SVC);
  CHECK(host_tp() == before);
  CHECK(dispatcher_saw_marker == 55);
  CHECK(seen.tpidr == guest_tp);
  CHECK(seen.x[16] == kAddId);
  CHECK(context.svc_immediate == kSvcReturnToHost);
  CHECK(context.x[0] == 136);
  CHECK(context.x[1] == kAddId);
  double d0;
  std::memcpy(&d0, &context.q[0][0], sizeof(d0));
  CHECK(d0 == 2.5);
  for (int i = 19; i <= 28; ++i) CHECK(context.x[i] == 0x1900 + static_cast<std::uint64_t>(i));
  CHECK(context.sp == reinterpret_cast<std::uint64_t>(&stack[256]));

  // A RunLoop call leaves run() at once, resumable at the return address.
  reset(run_loop);
  dispatcher_saw_marker = -1;
  CHECK(venture_native_enter(&context) == VENTURE_NATIVE_EXIT_SVC);
  CHECK(context.svc_immediate == kSvcHleCall);
  CHECK(context.x[16] == kAddId);
  CHECK(context.pc == reinterpret_cast<std::uint64_t>(back));
  CHECK(dispatcher_saw_marker == -1);
  CHECK(host_tp() == before);

  // A pending halt returns before any guest code runs.
  reset(fast);
  context.halt = 1;
  CHECK(venture_native_enter(&context) == VENTURE_NATIVE_EXIT_HALT);
  set_native_dispatcher(nullptr);
}
#endif
#endif
