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
namespace {
// The loader's runtime search roots: the user's config dir, then the config dirs and /etc.
std::vector<std::filesystem::path> config_roots() {
  std::vector<std::filesystem::path> roots;
  if (const char *home = std::getenv("XDG_CONFIG_HOME"); home && *home) roots.emplace_back(home);
  else if (const char *h = std::getenv("HOME"); h && *h) roots.push_back(std::filesystem::path(h) / ".config");
  const char *dirs = std::getenv("XDG_CONFIG_DIRS");
  std::string_view rest = dirs && *dirs ? dirs : "/etc/xdg";
  while (!rest.empty()) {
    const auto end = std::min(rest.find(':'), rest.size());
    if (end) roots.emplace_back(std::string(rest.substr(0, end)));
    rest.remove_prefix(std::min(end + 1, rest.size()));
  }
  roots.emplace_back("/etc");
  return roots;
}
}  // namespace
std::filesystem::path active_runtime() {
#if defined(__aarch64__)
  constexpr const char *kArch = "active_runtime.aarch64.json";
#else
  constexpr const char *kArch = "active_runtime.x86_64.json";
#endif
  std::error_code ec;
  for (const auto &root : config_roots())
    for (const char *name : {kArch, "active_runtime.json"})
      if (const auto file = root / "openxr/1" / name; std::filesystem::exists(file, ec))
        return std::filesystem::canonical(file, ec);
  return {};
}
std::vector<std::filesystem::path> runtime_manifests() {
  std::vector<std::filesystem::path> roots = config_roots();
  const char *data = std::getenv("XDG_DATA_DIRS");
  std::string_view rest = data && *data ? data : "/usr/local/share:/usr/share";
  while (!rest.empty()) {
    const auto end = std::min(rest.find(':'), rest.size());
    if (end) roots.emplace_back(std::string(rest.substr(0, end)));
    rest.remove_prefix(std::min(end + 1, rest.size()));
  }
  std::filesystem::path home;
  if (const char *h = std::getenv("HOME"); h && *h) home = h;
  if (const char *dh = std::getenv("XDG_DATA_HOME"); dh && *dh) roots.emplace_back(dh);
  else if (!home.empty()) roots.push_back(home / ".local/share");
  std::vector<std::filesystem::path> out;
  for (const auto &root : roots) {
    std::error_code error;
    std::vector<std::filesystem::path> found;
    for (const auto &entry : std::filesystem::directory_iterator(root / "openxr/1", error))
      if (entry.path().extension() == ".json" && !entry.path().filename().string().starts_with("active_runtime"))
        found.push_back(entry.path());
    std::ranges::sort(found);
    out.insert(out.end(), found.begin(), found.end());
  }
  // SteamVR registers no manifest outside its own folder.
  if (!home.empty())
    for (const char *steam : {".local/share/Steam", ".steam/steam", ".var/app/com.valvesoftware.Steam/.local/share/Steam"})
      out.push_back(home / steam / "steamapps/common/SteamVR/steamxr_linux64.json");
  return out;
}
bool environment_set(const char *name) { return std::getenv(name) != nullptr; }
}  // namespace venture::openxr_layers
