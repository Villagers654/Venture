#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>

namespace venture::download {
// The whole size a Unity bundle's header records, or nothing for any other file.
// A copy of a different size was cut short or padded on its way to this PC, and
// Unity fails it only when the game loads it ("Failed to read data for the
// AssetBundle"). Layout: "UnityFS\0", u32 format, player version, engine
// version, then the size as a big-endian 64-bit integer.
inline std::optional<std::uint64_t> unity_bundle_size(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  char head[256]{};
  in.read(head, sizeof head);
  const std::string_view text(head, std::size_t(in.gcount()));
  if (!text.starts_with(std::string_view("UnityFS\0", 8))) return std::nullopt;
  std::size_t at = 12;
  for (int field = 0; field < 2; ++field) {
    at = text.find('\0', at);
    if (at == std::string_view::npos) return std::nullopt;
    ++at;
  }
  if (at + 8 > text.size()) return std::nullopt;
  std::uint64_t size = 0;
  for (std::size_t i = 0; i < 8; ++i) size = size << 8 | std::uint8_t(text[at + i]);
  return size;
}
}  // namespace venture::download
