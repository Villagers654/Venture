#include "vulkan_callbacks_test_support.h"
#include <array>
#include <deque>
#include <mutex>

namespace venture::test {
namespace {
std::mutex mutex;
VulkanCallback callback;
struct Signature { GuestAddr id; unsigned arguments; };
std::deque<Signature> signatures;
}
void set_vulkan_callback(VulkanCallback cb) { std::lock_guard lock(mutex); callback = std::move(cb); }
const hle::FunctionInfo& vulkan_thunk_info(GuestAddr thunk) {
  auto* code = g2h<const std::uint32_t>(thunk);
  return hle::function(((code[0] >> 5) & 0xffff) | (((code[1] >> 5) & 0xffff) << 16));
}
GuestAddr vulkan_callback_thunk(GuestAddr id, unsigned arguments) {
  std::lock_guard lock(mutex);
  auto& signature = signatures.emplace_back(Signature{id, arguments});
  return hle::make_thunk("vulkan-test", "callback", [](hle::CallContext& ctx) {
    const auto& sig = *static_cast<Signature*>(ctx.user());
    VulkanCallback invoke;
    { std::lock_guard lock(mutex); invoke = callback; }
    std::array<std::uint64_t, 8> words{};
    for (unsigned i = 0; i < sig.arguments; ++i) words[i] = ctx.x(i);
    auto result = invoke(sig.id, {{words.data(), sig.arguments}, {}});
    ctx.cpu().set_x(0, result.x0); ctx.cpu().set_x(1, result.x1); ctx.cpu().set_v(0, result.v0);
  }, &signature);
}
}
