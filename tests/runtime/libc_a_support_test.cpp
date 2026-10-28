// Registered handlers use real core register state; callbacks execute ARM64 thunks.
#include <atomic>
#include <deque>
#include <mutex>
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
