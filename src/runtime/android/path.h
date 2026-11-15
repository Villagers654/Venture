#pragma once
#include "common/paths.h"
#include <filesystem>
#include <string_view>
namespace venture::android {
// Never throws: asset and guest names need not be UTF-8 (paths::from_utf8).
inline std::filesystem::path host_path(std::string_view utf8) { return paths::from_utf8(utf8); }
}  // namespace venture::android
