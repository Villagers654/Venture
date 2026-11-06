// TEMPORARY COMPATIBILITY PATH. Remove once the Steam Link native passthrough
// companion offers passthrough through a standard OpenXR path (an API layer
// with XR_FB_passthrough or an ALPHA_BLEND environment blend mode), which
// Venture's native and blend backends already use.
//
// The external passthrough source backend's transport: that companion's
// application-source protocol (SBN1, version 1 and 2) on its pipe
// \\.\pipe\SFNP.StreamBridge.V1.<server pid>, used only when a server of this
// user answers. The images Venture submits are SBN1 profile 1: one
// premultiplied SDR RGBA layer per eye. Nothing here checks which program
// serves the pipe. Any valid reply means the source is there; one granting
// RGBA means it shows the world (a new grant generation is a new grant).
#pragma once
#include "xr/passthrough_source.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <optional>

namespace venture::xr::passthrough_source_sbn1 {
using Record = std::array<std::uint8_t, 128>;
using Nonce = std::array<std::uint8_t, 16>;
// Little-endian fields.
inline std::uint64_t read(const Record& record, unsigned at, unsigned bytes) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < bytes; ++i) value |= std::uint64_t(record[at + i]) << (8 * i);
  return value;
}
inline void write(Record& record, unsigned at, unsigned bytes, std::uint64_t value) {
  for (unsigned i = 0; i < bytes; ++i) record[at + i] = std::uint8_t(value >> (8 * i));
}
inline bool nonzero(const Nonce& value) {
  return std::any_of(value.begin(), value.end(), [](auto byte) { return byte != 0; });
}
// What a conversation publishes: the source is there (available), shows the
// world under this process's images (shown) with this grant (generation).
struct State {
  bool available = false, shown = false;
  std::uint64_t generation = 0;
  bool operator==(const State&) const = default;
};
// The application's request: version 2 adds the frame budget flag; a server
// that does not answer version 2 is asked again with version 1.
inline Record request(std::uint32_t process, const Nonce& nonce, bool v2) {
  Record record{};
  std::memcpy(record.data(), "SBN1", 4);
  write(record, 4, 2, 1); write(record, 6, 2, v2 ? 2 : 1);
  write(record, 8, 2, record.size()); write(record, 12, 4, 1);
  write(record, 16, 4, process);
  std::copy(nonce.begin(), nonce.end(), record.begin() + 24);
  return record;
}
struct Permission {
  bool rgba = false, budget = false;
  Nonce binding{};
  std::uint64_t generation = 0;
  bool operator==(const Permission&) const = default;
};
// The server's answer to this request, or nothing when it is malformed or
// belongs to another conversation.
inline std::optional<Permission> reply(const Record& record, std::uint32_t process, std::uint32_t server,
                                       const Nonce& nonce, bool v2) {
  if (!process || !server || !nonzero(nonce) || std::memcmp(record.data(), "SBN1", 4) ||
      read(record, 4, 2) != 2 || read(record, 6, 2) != (v2 ? 2 : 1) ||
      read(record, 8, 2) != record.size() || read(record, 12, 4) != 1 ||
      read(record, 16, 4) != process || read(record, 20, 4) != server ||
      !std::equal(nonce.begin(), nonce.end(), record.begin() + 24) ||
      std::any_of(record.begin() + 64, record.end(), [](auto byte) { return byte != 0; }))
    return std::nullopt;
  const auto flags = read(record, 10, 2);
  // Bit 2 is coverage telemetry in version 2; it is never a rendering prerequisite.
  if (flags & ~(v2 ? 7u : 3u)) return std::nullopt;
  Permission permission;
  permission.rgba = flags & 1;
  permission.budget = flags & 4;
  std::copy_n(record.begin() + 40, 16, permission.binding.begin());
  permission.generation = read(record, 56, 8);
  if (permission.rgba) {
    if (!nonzero(permission.binding) || !permission.generation || permission.budget != v2) return std::nullopt;
  } else if (flags || nonzero(permission.binding) || permission.generation) {
    return std::nullopt;
  }
  return permission;
}
// A server answered a request within `wait_ms`, granting or not (the
// connection is then closed, which withdraws it). A server grants RGBA only
// once this process submits frames, so a refusal still means it is there.
bool answers(unsigned wait_ms);
// One conversation while `alive`: each valid answer is published (available;
// shown with the grant's generation when RGBA is granted).
// Returns when the server closes, misbehaves or `alive` turns false.
void converse(const std::function<bool()>& alive,
              const std::function<void(const State&)>& publish);

namespace platform {
// A connection to a same-user server of the SBN1 pipe and that server's process
// ID, or nullptr (no such pipe, or not this user's). Windows only.
std::unique_ptr<passthrough_source_platform::Channel> connect(std::uint32_t& server);
} // namespace platform
} // namespace venture::xr::passthrough_source_sbn1
