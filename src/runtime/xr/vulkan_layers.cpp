#include "xr/vulkan_layers.h"
#include "common/environment.h"
#include "common/json.h"
#include "common/log.h"
#include "common/paths.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <sstream>
namespace venture::xr {
namespace {
bool equal_folded(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
           return std::tolower(x) == std::tolower(y);
         });
}
bool glob_matches(std::string_view glob, std::string_view name) {
  const bool head = glob.starts_with('*'), tail = glob.size() > 1 && glob.ends_with('*');
  if (head) glob.remove_prefix(1);
  if (tail) glob.remove_suffix(1);
  if (glob.empty()) return head || tail;
  if (head && tail) {
    for (std::size_t i = 0; i + glob.size() <= name.size(); ++i)
      if (equal_folded(name.substr(i, glob.size()), glob)) return true;
    return false;
  }
  if (name.size() < glob.size()) return false;
  if (head) return equal_folded(name.substr(name.size() - glob.size()), glob);
  if (tail) return equal_folded(name.substr(0, glob.size()), glob);
  return equal_folded(name, glob);
}
std::string_view trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  return text;
}
template <class F> void each_item(std::string_view list, std::string_view separators, F&& f) {
  while (!list.empty()) {
    const auto end = std::min(list.find_first_of(separators), list.size());
    if (auto item = trim(list.substr(0, end)); !item.empty()) f(item);
    list.remove_prefix(std::min(end + 1, list.size()));
  }
}
std::vector<std::pair<std::string, std::string>> environment_map(const Json& value) {
  std::vector<std::pair<std::string, std::string>> out;
  if (value.is_object())
    for (const auto& [name, setting] : value.items())
      if (!name.empty() && setting.is_string()) out.emplace_back(name, setting.get<std::string>());
  return out;
}
std::string names(const std::vector<const ImplicitVulkanLayer*>& layers) {
  std::string out;
  for (const auto* layer : layers) out += (out.empty() ? "" : ", ") + layer->name;
  return out.empty() ? "none" : out;
}
std::vector<ImplicitVulkanLayer>& registered() {
  static std::vector<ImplicitVulkanLayer> layers;
  return layers;
}
}  // namespace
// Driver and system layers that select the GPU or present on this system
// (hybrid graphics, gamescope), and the owner's FoveaBridge, stay by default.
const std::string_view builtin_kept_vulkan_layers =
    "VK_LAYER_MESA_device_select,VK_LAYER_MESA_anti_lag,VK_LAYER_NV_optimus,VK_LAYER_AMD_switchable_graphics*,"
    "VK_LAYER_FROG_gamescope_wsi*,*FOVEABRIDGE*";
std::vector<ImplicitVulkanLayer> parse_implicit_layer_manifest(const std::filesystem::path& manifest,
                                                               std::string_view text) {
  std::vector<ImplicitVulkanLayer> out;
  const auto json = Json::parse(text.begin(), text.end(), nullptr, false);
  if (json.is_discarded() || !json.is_object()) return out;
  auto add = [&](const Json& layer) {
    if (!layer.is_object() || !layer.contains("name") || !layer["name"].is_string()) return;
    ImplicitVulkanLayer entry{layer["name"].get<std::string>(), manifest, {}, {}, {}};
    if (auto library = layer.find("library_path"); library != layer.end() && library->is_string())
      entry.library = library->get<std::string>();
    if (auto e = layer.find("disable_environment"); e != layer.end()) entry.disable_environment = environment_map(*e);
    if (auto e = layer.find("enable_environment"); e != layer.end()) entry.enable_environment = environment_map(*e);
    if (!entry.name.empty()) out.push_back(std::move(entry));
  };
  if (auto layer = json.find("layer"); layer != json.end()) add(*layer);
  if (auto layers = json.find("layers"); layers != json.end() && layers->is_array())
    for (const auto& layer : *layers) add(layer);
  return out;
}
bool vulkan_layer_filter_matches(std::string_view filters, std::string_view name) {
  bool match = false;
  each_item(filters, ",", [&](std::string_view glob) {
    match = match || glob == "~all~" || glob == "~implicit~" || glob_matches(glob, name);
  });
  return match;
}
VulkanLayerPlan plan_vulkan_layers(const std::vector<ImplicitVulkanLayer>& layers,
                                   const std::function<const char*(const char*)>& getenv) {
  auto value = [&](const char* name) -> std::string_view {
    const char* v = getenv(name);
    return v ? std::string_view(v) : std::string_view();
  };
  const auto venture = value("VENTURE_VULKAN_LAYERS"), allow = value("VK_LOADER_LAYERS_ALLOW"),
             enable = value("VK_LOADER_LAYERS_ENABLE"), instance = value("VK_INSTANCE_LAYERS");
  VulkanLayerPlan plan;
  plan.loader_disable = std::string(value("VK_LOADER_LAYERS_DISABLE"));
  for (const auto& layer : layers) {
    const bool fossilize = layer.name.starts_with("VK_LAYER_VALVE_steam_fossilize");
    bool requested = false;
    each_item(instance, ";:,", [&](std::string_view item) { requested = requested || equal_folded(item, layer.name); });
    // A layer that runs only when its variable says so was asked for.
    const bool asked = !layer.enable_environment.empty() &&
        std::ranges::all_of(layer.enable_environment, [&](const auto& e) { return value(e.first.c_str()) == e.second; });
    const bool keep = !fossilize && (requested || asked || vulkan_layer_filter_matches(builtin_kept_vulkan_layers, layer.name) ||
                                     vulkan_layer_filter_matches(venture, layer.name) ||
                                     vulkan_layer_filter_matches(allow, layer.name) ||
                                     vulkan_layer_filter_matches(enable, layer.name));
    const bool off = std::ranges::any_of(layer.disable_environment, [&](const auto& e) { return getenv(e.first.c_str()); });
    if (off && !keep) plan.already_disabled.push_back(&layer);
    else if (keep) plan.kept.push_back(&layer);
    else {
      plan.disabled.push_back(&layer);
      plan.loader_disable += (plan.loader_disable.empty() ? "" : ",") + layer.name;
    }
  }
  return plan;
}
void configure_vulkan_layers_for_process() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto& layers = registered();
    for (const auto& manifest : implicit_vulkan_layer_manifests()) {
      std::ifstream in(paths::io_path(manifest), std::ios::binary);
      if (!in) continue;
      std::ostringstream text;
      text << in.rdbuf();
      std::ranges::move(parse_implicit_layer_manifest(manifest, text.str()), std::back_inserter(layers));
    }
    // Present on every machine with Steam, sometimes unregistered (Linux runtimes).
    bool fossilize = false;
    for (const auto& layer : layers) fossilize = fossilize || layer.name.starts_with("VK_LAYER_VALVE_steam_fossilize");
    if (!fossilize)
      layers.push_back({"VK_LAYER_VALVE_steam_fossilize", {}, {}, {{"DISABLE_VK_LAYER_VALVE_steam_fossilize_1", "1"}}, {}});
    const auto plan = plan_vulkan_layers(layers, [](const char* name) { return environment::getenv(name); });
    for (const auto* layer : plan.disabled)
      for (const auto& [name, value] : layer->disable_environment) set_process_environment(name.c_str(), value.c_str());
    if (!plan.disabled.empty()) set_process_environment("VK_LOADER_LAYERS_DISABLE", plan.loader_disable.c_str());
    environment::refresh();
    std::vector<const ImplicitVulkanLayer*> shown;
    for (const auto* layer : plan.disabled)
      if (!layer->manifest.empty()) shown.push_back(layer);
    if (!shown.empty())
      log::info("vulkan", "Implicit Vulkan layers left out of this process: {} (keep one with Settings "
                "\"vulkanLayers\" or VENTURE_VULKAN_LAYERS)", names(shown));
    if (!plan.kept.empty()) log::info("vulkan", "Implicit Vulkan layers kept: {}", names(plan.kept));
  });
}
void log_loaded_vulkan_layers() {
  std::vector<const ImplicitVulkanLayer*> loaded;
  for (const auto& layer : registered())
    if (!layer.library.empty() && !layer.manifest.empty() && vulkan_layer_library_loaded(layer))
      loaded.push_back(&layer);
  log::info("vulkan", "Vulkan loader {}; implicit layers loaded: {}", vulkan_loader_description(), names(loaded));
}
}  // namespace venture::xr
