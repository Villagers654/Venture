// Application-side SBN1 wire format. No pixels or camera authority travel here.
// https://github.com/McFredward/steam-link-native-passthrough/blob/main/docs/protocols/application-source-v1.md
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>

namespace venture::xr::steam_link {
using Record = std::array<std::uint8_t, 128>;
using Nonce = std::array<std::uint8_t, 16>;
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
inline Record request(std::uint32_t process, const Nonce& nonce, bool budget = true) {
  Record record{};
  std::memcpy(record.data(), "SBN1", 4);
  write(record, 4, 2, 1); write(record, 6, 2, budget ? 2 : 1);
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
inline std::optional<Permission> reply(const Record& record, std::uint32_t process,
                                      std::uint32_t server, const Nonce& nonce, bool budget) {
  if (!process || !server || !nonzero(nonce) || std::memcmp(record.data(), "SBN1", 4) ||
      read(record, 4, 2) != 2 || read(record, 6, 2) != (budget ? 2 : 1) ||
      read(record, 8, 2) != record.size() || read(record, 12, 4) != 1 ||
      read(record, 16, 4) != process || read(record, 20, 4) != server ||
      !std::equal(nonce.begin(), nonce.end(), record.begin() + 24) ||
      std::any_of(record.begin() + 64, record.end(), [](auto byte) { return byte != 0; }))
    return std::nullopt;
  const auto flags = read(record, 10, 2);
  // Bit 2 is reserved for coverage telemetry; it is never a rendering prerequisite.
  if (flags & ~(budget ? 7u : 3u)) return std::nullopt;
  Permission permission;
  permission.rgba = flags & 1;
  permission.budget = flags & 4;
  std::copy_n(record.begin() + 40, 16, permission.binding.begin());
  permission.generation = read(record, 56, 8);
  if (permission.rgba) {
    if (!nonzero(permission.binding) || !permission.generation || permission.budget != budget)
      return std::nullopt;
  } else if (flags || nonzero(permission.binding) || permission.generation) {
    return std::nullopt;
  }
  return permission;
}
} // namespace venture::xr::steam_link
