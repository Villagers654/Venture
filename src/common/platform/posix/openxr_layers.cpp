#include "common/openxr_layers.h"
#include <algorithm>
#include <cstdlib>
#include <iterator>

namespace venture::openxr_layers {
std::vector<std::filesystem::path> implicit_manifests() {
  // The loader's ReadDataFilesInSearchPaths: config dirs, its system config
  // dirs, data dirs, then the user's data dir.
  std::vector<std::filesystem::path> roots;
  auto list = [&](const char *variable, std::string_view fallback) {
    const char *value = std::getenv(variable);
    std::string_view rest = value && *value ? value : fallback;
    while (!rest.empty()) {
      const auto end = std::min(rest.find(':'), rest.size());
      if (end) roots.emplace_back(std::string(rest.substr(0, end)));
      rest.remove_prefix(std::min(end + 1, rest.size()));
    }
  };
  list("XDG_CONFIG_DIRS", "/etc/xdg");
  roots.emplace_back("/usr/local/etc");
  roots.emplace_back("/etc");
  list("XDG_DATA_DIRS", "/usr/local/share:/usr/share");
  if (const char *home = std::getenv("XDG_DATA_HOME"); home && *home) roots.emplace_back(home);
  else if (const char *h = std::getenv("HOME"); h && *h) roots.push_back(std::filesystem::path(h) / ".local/share");
  std::vector<std::filesystem::path> out;
  for (const auto &root : roots) {
    std::error_code error;
    std::vector<std::filesystem::path> found;
    for (const auto &entry : std::filesystem::directory_iterator(root / "openxr/1/api_layers/implicit.d", error))
      if (entry.path().extension() == ".json") found.push_back(entry.path());
    std::ranges::sort(found);
    for (auto &file : found)
      if (std::ranges::find(out, file) == out.end()) out.push_back(std::move(file));
  }
  return out;
}
bool environment_set(const char *name) { return std::getenv(name) != nullptr; }
}  // namespace venture::openxr_layers
