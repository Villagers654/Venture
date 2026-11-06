#include "common/environment.h"
#include "xr/native.h"
#include "xr/vulkan_layers.h"
#include <algorithm>
#include <cstdlib>
#include <dlfcn.h>
#include <string_view>
namespace venture::xr {
bool set_process_environment(const char* name, const char* value) { return setenv(name, value, 1) == 0; }
std::vector<std::filesystem::path> implicit_vulkan_layer_manifests() {
  // The loader's implicit search path, in its order (LoaderLayerInterface.md).
  std::vector<std::filesystem::path> roots;
  auto list = [&](const char* variable, const char* fallback) {
    const char* value = environment::getenv(variable);
    std::string_view rest = value && *value ? value : fallback;
    while (!rest.empty()) {
      const auto end = std::min(rest.find(':'), rest.size());
      if (end) roots.emplace_back(std::string(rest.substr(0, end)));
      rest.remove_prefix(std::min(end + 1, rest.size()));
    }
  };
  const char* home = environment::getenv("HOME");
  if (const char* c = environment::getenv("XDG_CONFIG_HOME"); c && *c) roots.emplace_back(c);
  else if (home) roots.push_back(std::filesystem::path(home) / ".config");
  list("XDG_CONFIG_DIRS", "/etc/xdg");
  roots.emplace_back("/etc");
  if (const char* d = environment::getenv("XDG_DATA_HOME"); d && *d) roots.emplace_back(d);
  else if (home) roots.push_back(std::filesystem::path(home) / ".local/share");
  list("XDG_DATA_DIRS", "/usr/local/share:/usr/share");
  std::vector<std::filesystem::path> out;
  for (const auto& root : roots) {
    std::error_code error;
    std::vector<std::filesystem::path> found;
    for (const auto& entry : std::filesystem::directory_iterator(root / "vulkan/implicit_layer.d", error))
      if (entry.path().extension() == ".json") found.push_back(entry.path());
    std::ranges::sort(found);
    std::ranges::move(found, std::back_inserter(out));
  }
  return out;
}
std::string vulkan_loader_description() {
  Dl_info info{};
  const auto loader = native_vulkan_loader();
  return loader && dladdr(reinterpret_cast<void*>(loader), &info) && info.dli_fname ? info.dli_fname
                                                                                 : "libvulkan.so.1 not loaded";
}
bool vulkan_layer_library_loaded(const ImplicitVulkanLayer& layer) {
  std::filesystem::path library = layer.library;
  if (library.has_parent_path() && library.is_relative()) library = layer.manifest.parent_path() / library;
  void* handle = dlopen(library.c_str(), RTLD_LAZY | RTLD_NOLOAD);
  if (handle) dlclose(handle);
  return handle != nullptr;
}
}  // namespace venture::xr
