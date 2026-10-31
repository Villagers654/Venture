// Registered handlers use real core register state; callbacks execute ARM64 thunks.
#include <atomic>
#include <bit>
#include <deque>
#include <mutex>
#include "core/hle_runtime.h"
#include "core/libc_host_files.h"
#include "core/process.h"
#include "core/signals.h"
#include "cpu/state.h"
#include "hle/libc/libc_a.h"
#include "hle/registry.h"
#include "libc_a_test_support.h"
namespace venture {
namespace test {
GuestAddr callback(std::function<std::uint64_t(std::span<const std::uint64_t>)> fn) {
  using Callback = decltype(fn);
  static std::mutex mutex;
  static std::deque<Callback> callbacks;
  std::lock_guard lock(mutex);
  auto* stored = &callbacks.emplace_back(std::move(fn));
  return hle::make_thunk("libc-a-test", "callback", [](hle::CallContext& ctx) {
    std::array<std::uint64_t, 8> args;
    for (int i = 0; i < 8; ++i) args[i] = ctx.x(i);
    ctx.return_x((*static_cast<Callback*>(ctx.user()))(args));
  }, stored);
}
namespace { std::atomic<int> fixture_tid{30000}; }
LibcFixture::LibcFixture() : thread(fixture_tid.fetch_add(1)),
    cpu((thread.attach_to_current_host_thread(), thread.cpu())),
    stack(g2h<std::uint64_t>(cpu.sp() - 256), 32) {
  static std::once_flag init;
  std::call_once(init, [] { process_config().properties["test.venture"] = "configured"; process_config().properties["ro.product.model"] = "Quest 3"; register_all_hle_libraries(); });
  cpu.set_sp(h2g(stack.data()));
}
GuestAddr LibcFixture::data(const char* name, const char* library) {
  auto* lib = Linker::get().open(library, kRtldNow, nullptr);
  auto address = lib ? lib->find_symbol(name) : 0;
  if (!address) throw std::runtime_error("unregistered HLE symbol " + std::string(name));
  return address;
}
std::uint64_t LibcFixture::invoke(const char* name, std::initializer_list<std::uint64_t> args, const char* library) {
  auto address = data(name, library);
  unsigned i = 0;
  for (auto value : args) {
    if (i < 8) cpu.set_x(i, value);
    else stack[i - 8] = value;
    ++i;
  }
  // Decode the documented MOVZ/MOVK id in a registered HLE thunk.
  const auto* instructions = g2h<std::uint32_t>(address);
  if ((instructions[0] & 0xffe0001fu) != 0xd2800010u) {
    // A guest routine (guest_strings.cpp) runs in the JIT. GuestThread::call
    // restores FPCR/FPSR, so a probe sets them from the fixture (x6, x5),
    // calls x7 and returns the routine's FPSR in x1.
    static const auto probe = hle::code_stub(std::array<std::uint32_t, 7>{
        0xa9bf7bfdu, 0xd51b4406u, 0xd51b4425u, 0xd63f00e0u, 0xd53b4421u, 0xa8c17bfdu, 0xd65f03c0u}, "libc fixture probe");
    if (args.size() > 5) throw std::invalid_argument("guest routine probe passes at most five integers");
    std::array<std::uint64_t, 8> integers{};
    for (i = 0; i < 5; ++i) integers[i] = cpu.x(i);
    integers[5] = cpu.fpsr(); integers[6] = cpu.fpcr(); integers[7] = address;
    std::array<double, 8> doubles;
    for (int v = 0; v < 8; ++v) doubles[v] = std::bit_cast<double>(cpu.v(v).lo);
    const auto result = thread.call(probe, GuestCallArgs{integers, doubles});
    cpu.set_v(0, result.v0);
    cpu.set_fpsr(static_cast<std::uint32_t>(result.x1));
    cpu.set_x(0, result.x0);
    return result.x0;
  }
  const auto id = ((instructions[0] >> 5) & 0xffff) | (((instructions[1] >> 5) & 0xffff) << 16);
  const auto& info = hle::function(id);
  hle::CallContext ctx(thread, cpu, info);
  info.handler(ctx);
  return cpu.x(0);
}
std::uint64_t LibcFixture::invoke_fatal(const char* name, std::initializer_list<std::uint64_t> args) {
  // Intercept the real SIGABRT handler to keep a fortify assertion from ending the test process.
  auto handler = hle::make_thunk("libc-a-test", "abort-handler", [](hle::CallContext&) { throw GuestCrash("guest SIGABRT"); });
  signals::Action action{handler}, old;
  signals::set_action(signals::kSigAbrt, &action, &old);
  const auto mask = thread.blocked_signals;
  CpuState saved(thread.cpu());
  struct Restore {
    GuestThread& thread; signals::Action old; std::uint64_t mask; CpuState& saved;
    ~Restore() { signals::set_action(signals::kSigAbrt, &old, nullptr); signals::set_mask(thread, 2, &mask, nullptr); saved.restore(thread.cpu()); }
  } restore{thread, old, mask, saved};
  return invoke(name, args);
}
}
}
