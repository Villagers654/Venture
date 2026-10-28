#include "android/handles32.h"
#include "core/token32.h"
#include "core/process.h"
#include <map>

namespace venture::android::handles32 {
namespace {
struct Handle {
  std::shared_ptr<void> owner;
  std::string tag;
};
struct State {
  std::mutex mutex;
  Token32Registry tokens;
  std::map<GuestAddr, Token32<Handle>> by_host;
};
State& state() {
  static auto* s = new State;
  return *s;
}
std::string_view base(std::string_view tag) {
  if (tag.starts_with("const ")) tag.remove_prefix(6);
  return tag.substr(0, tag.find_first_of(" *"));
}
}  // namespace
void publish(std::shared_ptr<void> owner, std::string_view tag) {
  if (process_config().isa() != GuestIsa::a32 || tag.empty()) return;
  auto& s = state();
  auto host = h2g(owner.get());
  std::lock_guard lock(s.mutex);
  if (s.by_host.contains(host)) throw std::logic_error("duplicate NDK handle owner");
  s.by_host.emplace(
      host, s.tokens.insert(std::make_shared<Handle>(Handle{std::move(owner), std::string(base(tag))})));
}
void retire(GuestAddr host) {
  if (process_config().isa() != GuestIsa::a32) return;
  auto& s = state();
  Token32<Handle> token;
  {
    std::lock_guard lock(s.mutex);
    auto it = s.by_host.find(host);
    if (it == s.by_host.end()) return;
    token = it->second;
    s.by_host.erase(it);
  }
  s.tokens.erase(token);
}
GuestAddr encode(GuestAddr host, std::string_view tag) {
  if (!host || process_config().isa() != GuestIsa::a32) return host;
  auto& s = state();
  std::lock_guard lock(s.mutex);
  auto it = s.by_host.find(host);
  if (it == s.by_host.end()) throw std::invalid_argument("unpublished NDK handle");
  auto pin = s.tokens.resolve(it->second);
  if (!pin || pin->tag != base(tag)) throw std::invalid_argument("NDK handle type mismatch");
  return it->second.value;
}
std::shared_ptr<void> resolve(std::uint32_t token, std::string_view tag) {
  if (!token) return {};
  auto pin = state().tokens.resolve(Token32<Handle>{token});
  if (!pin || pin->tag != base(tag)) throw std::invalid_argument("stale or wrong-type NDK token");
  return pin->owner;
}
}  // namespace venture::android::handles32
