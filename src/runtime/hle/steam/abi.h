#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include "core/hle.h"

namespace venture::steam {
struct CopyField { std::uint32_t guest, host, bytes; };
struct Layout {
  std::string_view name;
  std::int32_t callback;
  std::uint32_t guest_size, host_size;
  std::span<const CopyField> fields;
};
struct Method { std::string_view symbol; std::uint16_t slot; };
struct Interface { std::string_view name, version, accessor; };
struct Export { std::string_view name; hle::Handler handler; };
struct Sdk {
  int version;
  std::string_view android_sha256;
  std::span<const Layout> layouts;
  std::span<const Method> methods;
  std::span<const Interface> interfaces;
  std::span<const Export> exports;
  const Layout* callback(std::int32_t id) const;
  const Layout* layout(std::string_view name) const;
  const Interface* interface(std::string_view version) const;
  std::uint16_t slot(std::string_view symbol) const;
};
const Sdk& sdk163();
const Sdk& sdk164();
const Sdk& sdk165();
const Sdk* detect_sdk(std::span<const std::byte> elf);
bool convert(const Layout&, std::span<std::byte> target, std::span<const std::byte> source, bool to_guest);
} // namespace venture::steam
