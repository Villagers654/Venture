// TEMPORARY COMPATIBILITY PATH: see passthrough_source_sbn1.h.
#include "xr/passthrough_source_sbn1.h"
#include "common/log.h"

namespace venture::xr::passthrough_source_sbn1 {
namespace sys = passthrough_source_platform;
namespace {
bool new_nonce(Nonce& nonce) { return sys::random(nonce.data(), nonce.size()) && nonzero(nonce); }
// One request and its answers. Returns whether a valid answer arrived.
bool session(bool v2, const std::function<bool()>& alive, std::uint64_t first_answer_ms,
             const std::function<void(const State&)>& publish, bool once) {
  std::uint32_t server = 0;
  auto channel = platform::connect(server);
  if (!channel) return false;
  Nonce nonce{};
  if (!new_nonce(nonce)) return false;
  const auto process = sys::process_id();
  auto record = request(process, nonce, v2);
  const auto deadline = sys::now_ms() + first_answer_ms;
  if (!channel->transfer(true, record.data(), record.size(), alive, deadline)) return false;
  bool observed = false;
  while (alive()) {
    Record answer{};
    if (!channel->transfer(false, answer.data(), answer.size(), alive, observed ? 0 : deadline)) break;
    const auto permission = reply(answer, process, server, nonce, v2);
    if (!permission) break;
    if (!observed) log::info("xr-passthrough", "External passthrough source answered over SBN1 v{} (compatibility)", v2 ? 2 : 1);
    observed = true;
    // Any valid answer means the source is there: the server grants RGBA only
    // once this process's frames reach it, on this connection.
    publish({true, permission->rgba, permission->rgba ? permission->generation : 0});
    if (once) break;
  }
  return observed;
}
} // namespace
bool answers(unsigned wait_ms) {
  try {
    const auto alive = [] { return true; };
    const auto ignore = [](const State&) {};
    // Half the wait each: a version 1 server does not answer a version 2 request.
    return session(true, alive, wait_ms / 2, ignore, true) || session(false, alive, wait_ms / 2, ignore, true);
  } catch (...) {
    return false;
  }
}
void converse(const std::function<bool()>& alive,
              const std::function<void(const State&)>& publish) {
  // A version 1 server does not answer a version 2 request; one that answered
  // version 2 and then closed is not asked again here.
  if (!session(true, alive, 2000, publish, false) && alive()) session(false, alive, 2000, publish, false);
}
} // namespace venture::xr::passthrough_source_sbn1
