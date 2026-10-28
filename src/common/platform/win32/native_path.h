#pragma once
#include <filesystem>
#include <string>

namespace venture::platform {
// Win32 file APIs need the extended namespace for long per-account OBB paths.
inline std::wstring native_path(const std::filesystem::path& path) {
  auto preferred = path;
  preferred.make_preferred();
  // Lexical normalization treats the extended namespace as an ordinary UNC
  // root and can change its prefix. Preserve a path already in that namespace.
  if (preferred.native().starts_with(L"\\\\?\\")) return preferred.native();
  auto value = std::filesystem::absolute(preferred).lexically_normal().make_preferred().native();
  if (value.starts_with(L"\\\\?\\")) return value;
  return value.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + value.substr(2) : L"\\\\?\\" + value;
}
}
