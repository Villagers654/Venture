// A32 shares the debuggerd disposition installed by the libc registration.
// A crash reporter can call the saved old action; direct deliveries of the
// runtime's debuggerd disposition run the same reporter host-side.
#include <doctest/doctest.h>
#include "fake_cpu32.h"
#include "core/hle_runtime.h"
#include "core/linker.h"
#include "core/signals.h"
#include "core/thread.h"
#include "core/thread_runtime.h"
#include "hle/registry.h"
#include "core/signal_frame.h"
#include "platform/memory.h"
#include "core/hle_unwind.h"
#include <cstring>
#include <vector>
#include <array>

using namespace venture;
namespace {
constexpr std::uint32_t debuggerd_flags = 0x10000000 | 4 | 0x08000000 | 0x800;
int exit_status(GuestThread& thread, GuestAddr fn, std::initializer_list<std::uint64_t> arguments) {
  try { thread.call(fn, arguments); } catch (const ProcessExit& exit) { return exit.status; }
  return -1;
}
#ifdef VENTURE_A32_PERSONALITY_FIXTURE
TEST_CASE("A32 EHABI condition cleanup uses opaque VRS APIs and only cleans during phase two") {
  register_all_hle_libraries();
  GuestThread thread(62903); thread.attach_to_current_host_thread();
  auto* library = Linker::get().open(VENTURE_A32_PERSONALITY_FIXTURE, kRtldNow, nullptr);
  REQUIRE(library);
  auto entry = library->find_symbol("condition_personality_fixture"); REQUIRE(entry);
  auto* libc = Linker::get().open("libc.so", kRtldNow, nullptr); REQUIRE(libc);
  const auto wait = libc->find_symbol("pthread_cond_wait"); REQUIRE(wait);
  test::LowMemory32 memory;
  auto frame = memory.address + 256;
  memory.at<std::uint32_t>(256)[0] = 0x11223344;
  memory.at<std::uint32_t>(256)[1] = 0x100033; // Thumb return address
  unsigned acquisitions = 0;
  for (unsigned phase : {0u, 1u, 2u, 8u, 9u, 3u}) {
    GuestThread::ConditionWaitCleanup cleanup{nullptr, [](void* p) { ++*static_cast<unsigned*>(p); }, &acquisitions, frame+8};
    thread.condition_wait_cleanup = &cleanup;
    const auto before = acquisitions;
    auto status = thread.call(entry, {wait+12, frame, phase, memory.address});
    thread.condition_wait_cleanup = nullptr;
    REQUIRE(status == 0);
    auto* result = memory.at<std::uint32_t>();
    const bool invalid = phase == 3;
    CHECK(result[0] == (invalid ? 9 : 8));
    CHECK(acquisitions - before == (phase == 1 || phase == 9 ? 1 : 0));
    CHECK(cleanup.pending == (phase != 1 && phase != 9));
    CHECK(cleanup.instructions == 0);
    CHECK(result[6] == 0x12345678); // no private-context layout assumptions
    if (!invalid) {
      CHECK(result[1] == 0x11223344); CHECK(result[2] == frame+8); CHECK(result[3] == 0x100033);
      CHECK(result[4] == 1); CHECK(result[5] == 3);
    }
  }
  Linker::get().close(library); Linker::get().close(libc);
}
#endif
TEST_CASE("A32 nested guest calls use alternate stack bounds including the empty top") {
  GuestThread thread(62904); thread.attach_to_current_host_thread();
  test::LowMemory32 alt(true), code(false, true);
  const std::uint32_t instructions[]{0xe59d0000, 0xe12fff1e}; // fifth argument, bx lr
  std::memcpy(code.at<void>(), instructions, sizeof(instructions));
  auto& cpu = thread.cpu();
  const auto original_sp = cpu.sp();
  signals::AltStack stack{alt.address, 65536, 0};
  REQUIRE(signals::set_alt_stack(thread, &stack, nullptr) == 0);
  cpu.set_sp(alt.address + 65536);
  CHECK(thread.call(code.address, {1, 2, 3, 4, 77}) == 77);
  CHECK(cpu.sp() == alt.address + 65536);
  cpu.set_sp(alt.address);
  CHECK_THROWS_AS(thread.call(code.address, {1, 2, 3, 4, 77}), std::runtime_error);
  cpu.set_sp(alt.address + 65536 + 16);
  CHECK_THROWS_AS(thread.call(code.address, {1, 2, 3, 4, 77}), std::runtime_error);
  cpu.set_sp(original_sp);
}
TEST_CASE("A32 ARM and Thumb signal handlers restore edited contexts and realtime alternate stacks") {
  GuestThread thread(62901); thread.attach_to_current_host_thread();
  test::LowMemory32 data, alt(true), code(false, true);
  auto& cpu = thread.cpu();
  REQUIRE(alt.address > cpu.sp());
  for (bool realtime : {false, true}) for (bool thumb : {false, true}) {
    const unsigned code_offset = (unsigned(realtime) * 2 + unsigned(thumb)) * 256;
    std::memset(data.at<void>(), 0, 64);
    signals::AltStack stack{alt.address, 65536, 0};
    REQUIRE(signals::set_alt_stack(thread, &stack, nullptr) == 0);
    for (int i = 0; i < 13; ++i) cpu.set_x(i, 0x500 + i);
    cpu.set_x(4, data.address); cpu.set_x(5, UINT32_MAX); cpu.set_x(6, UINT32_MAX);
    cpu.set_pc(code.address + 0x182); cpu.set_pstate(0x40000830); // Thumb + interrupted IT
    for (int i = 0; i < 32; ++i) cpu.set_v(i, {0x1122334400000000ull + unsigned(i), 0});
    cpu.set_fpcr(0x01000000);
    std::array<std::uint32_t, 16> saved;
    for (int i = 0; i < 16; ++i) saved[i] = cpu.x(i);
    std::vector<std::uint32_t> instructions;
    if (thumb) {
      const std::uint16_t entry[]{0x6020, 0x6061, 0x60a2, 0x4668, 0x60e0, 0xbf00, 0x4778, 0xbf00};
      std::memcpy(code.at<void>(code_offset), entry, sizeof(entry)); // record args/SP, interwork to ARM
    } else {
      // r4 is the observation buffer; preserve the signal arguments before edits.
      instructions = {0xe8840007, 0xe1a0000d, 0xe584000c, 0xe10f0000, 0xe5840010};
    }
    if (!realtime) instructions.push_back(0xe1a0200d); // legacy ucontext starts at SP
    const std::uint32_t tail[]{0xe3a0004d, 0xe5820020, 0xe58200f0, 0xe3a00066, 0xe58200f4,
        0xe3a00000, 0xe58201f0, 0xe5825068, 0xe582606c, 0xe3a00002, 0xe582000c, 0xe12fff1e};
    instructions.insert(instructions.end(), std::begin(tail), std::end(tail));
    std::memcpy(code.at<void>(code_offset + (thumb ? 16 : 0)), instructions.data(), instructions.size()*4);
    signals::Action action{(code.address + code_offset) | unsigned(thumb), 1ull << 22, 0x08000000u | (realtime ? 4u : 0u)}, previous;
    REQUIRE(signals::set_action(12, &action, &previous) == 0);
    thread.blocked_signals = 1ull << 20; thread.signal_mask_snapshot.store(thread.blocked_signals);
    signals::Info info{12, 1, 0x1234}; info.syndrome = thumb ? 1ull << 6 : 0;
    signals::deliver_synchronous(thread, info);
    auto* observed = data.at<std::uint32_t>();
    CHECK(observed[0] == 12);
    CHECK((observed[3] & 7) == 0);
    CHECK(observed[3] >= alt.address);
    CHECK(observed[3] + (realtime ? sizeof(signals::RtSigFrame32) : sizeof(signals::SigFrame32)) <= alt.address + 65536);
    if (!thumb) CHECK((observed[4] & 0x0600fc20u) == 0); // ARM entry clears Thumb/IT
    CHECK(observed[1] == (realtime ? observed[3] : 0));
    CHECK(observed[2] == (realtime ? observed[3] + 128 : 0));
    auto* uc = g2h<signals::UContext32>(realtime ? observed[2] : observed[3]);
    CHECK(uc->flags == (realtime ? 0u : 0x5ac3c35au));
    CHECK(uc->mcontext.oldmask == (1u << 20));
    CHECK(uc->mcontext.fault_address == 0x1234);
    CHECK(uc->mcontext.error == (thumb ? 1u << 11 : 0));
    for (int i = 0; i < 16; ++i) CHECK(cpu.x(i) == (i == 0 ? 77u : saved[i]));
    CHECK(cpu.pstate() == 0x40000830);
    CHECK(cpu.v(0).lo == 0x660000004dull);
    for (int i = 1; i < 32; ++i) CHECK(cpu.v(i).lo == 0x1122334400000000ull + unsigned(i));
    CHECK(cpu.fpcr() == 0);
    CHECK(thread.blocked_signals == (~0ull & ~(1ull << 8) & ~(1ull << 18)));
    signals::AltStack restored;
    CHECK(signals::set_alt_stack(thread, nullptr, &restored) == 0);
    CHECK(restored.flags == (realtime ? 2 : 0));
    CHECK(restored.base == (realtime ? 0 : alt.address));
    CHECK(signals::set_action(12, &previous, nullptr) == 0);
    thread.blocked_signals = 0; thread.signal_mask_snapshot.store(0);
  }
  // Linux requires alignment of the signal frame, not of the restored user SP.
  signals::RtSigFrame32 frame{};
  frame.sig.context.stack.flags = 2;
  frame.sig.context.mcontext.regs[13] = thread.stack_base + thread.stack_size - 4;
  frame.sig.context.mcontext.regs[15] = code.address + 0x182;
  frame.sig.context.mcontext.cpsr = 0x30;
  signals::VfpContext32 fp;
  std::memcpy(frame.sig.context.regspace.data(), &fp, sizeof(fp));
  std::memcpy(data.at<void>(), &frame, sizeof(frame));
  cpu.set_sp(data.address);
  signals::return_from_signal(thread, true);
  CHECK(cpu.sp() == thread.stack_base + thread.stack_size - 4);
}
}
// Each case ends its process (and registers the whole libc), so CTest runs
// each alone with --no-skip.
TEST_CASE("A32 sigaction reports debuggerd from startup and a fault takes the default action" * doctest::skip()) {
  register_all_hle_libraries();
  const auto debuggerd = signals::debuggerd_handler();
  REQUIRE(debuggerd > 1);
  CHECK(debuggerd <= UINT32_MAX);
  CHECK((debuggerd & 1) == 0);  // an ARM-state thunk
  test::LowMemory32 memory;
  auto* words = memory.at<std::uint32_t>();
  GuestThread thread(62002); thread.attach_to_current_host_thread();
  auto* libc = Linker::get().open("libc.so", kRtldNow, nullptr);
  REQUIRE(libc);
  // ILP32 struct sigaction: handler, 32-bit mask, flags, restorer.
  CHECK(thread.call(libc->find_symbol("sigaction"), {signals::kSigSegv, 0, memory.address}) == 0);
  CHECK(words[0] == debuggerd);
  CHECK(words[1] == 0xfffbfeffu);
  CHECK(words[2] == debuggerd_flags);
  // struct sigaction64: handler, flags, restorer, two-word mask.
  CHECK(thread.call(libc->find_symbol("sigaction64"), {signals::kSigIll, 0, memory.address + 64}) == 0);
  CHECK(words[16] == debuggerd);
  CHECK(words[17] == debuggerd_flags);
  CHECK(words[19] == 0xfffbfeffu);
  CHECK(words[20] == 0xffffffffu);
  CHECK(thread.call(libc->find_symbol("sigaction"), {12, 0, memory.address + 128}) == 0);
  CHECK(words[32] == 0);  // other signals stay SIG_DFL
  constexpr std::array null_load{0xe5900000u, 0xe12fff1eu};  // ldr r0, [r0]; bx lr
  CHECK(exit_status(thread, hle::code_stub(null_load, "A32 null load"), {0}) == 128 + signals::kSigSegv);
  signals::Action now;
  CHECK(signals::set_action(signals::kSigSegv, nullptr, &now) == 0);
  CHECK(now.handler == 0);
}
TEST_CASE("A32 crash reporter chaining to debuggerd ends with the default action" * doctest::skip()) {
  register_all_hle_libraries();
  GuestThread thread(62003); thread.attach_to_current_host_thread();
  // old_action.sa_sigaction(SIGABRT, info, context) with a null siginfo.
  CHECK(exit_status(thread, signals::debuggerd_handler(), {signals::kSigAbrt, 0, 0}) == 128 + signals::kSigAbrt);
}
