#include "java_test_support.h"
#include "core/hle.h"
#include <array>
#include <bit>
#include <deque>
#include <mutex>
namespace venture::test {
thread_local std::function<GuestCallResult(GuestAddr, const GuestCallArgs&)> java_guest_call;
GuestAddr java_thunk(GuestAddr logical_id, unsigned gp, unsigned fp, unsigned stack) {
  struct Signature { GuestAddr id; unsigned gp, fp, stack; };
  static std::deque<Signature> signatures;
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  auto& signature = signatures.emplace_back(Signature{logical_id, gp, fp, stack});
  return hle::make_thunk("java-test", std::to_string(logical_id), [](hle::CallContext& ctx) {
    const auto& sig = *static_cast<Signature*>(ctx.user());
    std::array<std::uint64_t, 8> integers{};
    std::array<double, 8> doubles{};
    for (unsigned i = 0; i < sig.gp; ++i) integers[i] = ctx.cpu().x(i);
    for (unsigned i = 0; i < sig.fp; ++i) doubles[i] = std::bit_cast<double>(ctx.cpu().v(i).lo);
    auto result = java_guest_call(sig.id, {{integers.data(), sig.gp}, {doubles.data(), sig.fp},
        {g2h<const std::uint64_t>(ctx.cpu().sp()), sig.stack}});
    ctx.cpu().set_x(0, result.x0);
    ctx.cpu().set_x(1, result.x1);
    ctx.cpu().set_v(0, result.v0);
  }, &signature);
}
}
