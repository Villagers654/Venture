// Host paths returned by the Java framework are aliases of these Android mounts.
#pragma once
#include <array>
#include <optional>
#include "core/process.h"

namespace venture {
inline std::optional<std::string> host_path_to_guest(std::string_view path) {
  const auto& c = process_config();
  const auto requested = std::filesystem::path(std::u8string(path.begin(), path.end())).lexically_normal();
  if (!requested.is_absolute()) return std::nullopt;
  const std::array<std::pair<std::filesystem::path, std::string>, 9> roots{{
      {c.apk, c.guest_apk_path()},
      {c.native_lib_dir, c.guest_native_lib_dir()},
      {c.obb_dir, c.guest_obb_dir()},
      {c.data_dir.empty() ? std::filesystem::path{} : c.data_dir / "external-data", "/storage/emulated/0/Android/data/" + c.package},
      {c.guest_cache_dir, c.guest_data_dir() + "/cache"},
      {c.data_dir.empty() ? std::filesystem::path{} : c.data_dir / "external", "/storage/emulated/0"},
      {c.data_dir.empty() ? std::filesystem::path{} : c.data_dir / "volumes/data", "/data"},
      {c.data_dir.empty() ? std::filesystem::path{} : c.data_dir / "volumes/system", "/system"},
      {c.data_dir, c.guest_data_dir()},
  }};
  for (const auto& [host, guest] : roots) {
    if (host.empty()) continue;
    const auto root = std::filesystem::absolute(host).lexically_normal();
    auto p = requested.begin(), r = root.begin();
    for (; p != requested.end() && r != root.end() && *p == *r; ++p, ++r) {}
    if (r != root.end() || (host == c.apk && p != requested.end())) continue;
    const auto relative = requested.lexically_relative(root).generic_u8string();
    if (relative == u8".") return guest;
    return guest + "/" + std::string(reinterpret_cast<const char*>(relative.data()), relative.size());
  }
  return std::nullopt;
}
}
