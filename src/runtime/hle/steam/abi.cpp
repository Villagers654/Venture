#include "hle/steam/abi.h"
#include "common/meta/auth.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace venture::steam {
const Sdk* detect_sdk(std::span<const std::byte> elf) {
  // ELF64 little-endian AArch64; exact pins deliberately refuse unknown builds.
  if (elf.size() < 64 || std::memcmp(elf.data(), "\x7f" "ELF\x02\x01", 6) != 0 ||
      std::to_integer<unsigned>(elf[18]) != 183 || elf[19] != std::byte{}) return nullptr;
  const auto hash = meta::sha256({reinterpret_cast<const char*>(elf.data()), elf.size()});
  for (const auto* sdk : {&sdk163(), &sdk164(), &sdk165()}) if (sdk->android_sha256 == hash) return sdk;
  return nullptr;
}
const Layout* Sdk::callback(std::int32_t id) const {
  for (const auto& l : layouts) if (l.callback == id) return &l;
  return nullptr;
}
const Layout* Sdk::layout(std::string_view name) const {
  for (const auto& l : layouts) if (l.name == name) return &l;
  return nullptr;
}
const Interface* Sdk::interface(std::string_view version) const {
  for (const auto& i : interfaces) if (i.version == version) return &i;
  return nullptr;
}
std::uint16_t Sdk::slot(std::string_view symbol) const {
  for (const auto& m : methods) if (m.symbol == symbol) return m.slot;
  throw std::runtime_error("Steam method absent from pinned MSVC table");
}
bool convert(const Layout& l, std::span<std::byte> to, std::span<const std::byte> from, bool guest) {
  if (to.size() != (guest ? l.guest_size : l.host_size) || from.size() != (guest ? l.host_size : l.guest_size)) return false;
  std::fill(to.begin(), to.end(), std::byte{});
  for (const auto& f : l.fields) {
    auto dest = guest ? f.guest : f.host, src = guest ? f.host : f.guest;
    if (dest + f.bytes > to.size() || src + f.bytes > from.size()) throw std::logic_error("invalid generated Steam layout");
    std::memcpy(to.data() + dest, from.data() + src, f.bytes);
  }
  return true;
}
} // namespace venture::steam
