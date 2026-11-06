#include "xr/vulkan_layers.h"
#include <doctest/doctest.h>
#include <algorithm>
#include <map>
#include <string>

using namespace venture::xr;

namespace {
std::vector<ImplicitVulkanLayer> machine_layers() {
  // The registered layers of a reporter's machine (overlays, capture, post-processing).
  std::vector<ImplicitVulkanLayer> layers;
  auto add = [&](const char* manifest, const char* json) {
    for (auto& layer : parse_implicit_layer_manifest(manifest, json)) layers.push_back(std::move(layer));
  };
  add("C:/Program Files (x86)/RivaTuner Statistics Server/Vulkan/RTSSVkLayer64.json",
      R"({"file_format_version":"1.0.0","layer":{"name":"VK_LAYER_RTSS","type":"GLOBAL",
          "library_path":".\\RTSSVkLayer64.dll","api_version":"1.0.0","implementation_version":"1",
          "disable_environment":{"DISABLE_RTSS_LAYER":"1"}}})");
  add("C:/ProgramData/obs-studio-hook/obs-vulkan64.json",
      R"({"file_format_version":"1.1.2","layer":{"name":"VK_LAYER_OBS_hook","library_path":".\\graphics-hook64.dll",
          "disable_environment":{"DISABLE_VULKAN_OBS_CAPTURE":"1"}}})");
  add("C:/New folder/SteamOverlayVulkanLayer64.json",
      R"({"file_format_version":"1.0.0","layer":{"name":"VK_LAYER_VALVE_steam_overlay_64","library_path":".\\SteamOverlayVulkanLayer64.dll",
          "enable_environment":{"ENABLE_VK_LAYER_VALVE_steam_overlay_1":"1"},
          "disable_environment":{"DISABLE_VK_LAYER_VALVE_steam_overlay_1":"1"}}})");
  add("C:/New folder/SteamFossilizeVulkanLayer64.json",
      R"({"file_format_version":"1.0.0","layer":{"name":"VK_LAYER_VALVE_steam_fossilize_64","library_path":".\\SteamFossilizeVulkanLayer64.dll",
          "enable_environment":{"ENABLE_VK_LAYER_VALVE_steam_fossilize_1":"1"},
          "disable_environment":{"DISABLE_VK_LAYER_VALVE_steam_fossilize_1":"1"}}})");
  add("C:/ProgramData/ReShade/ReShade64.json",
      R"({"file_format_version":"1.2.0","layers":[{"name":"VK_LAYER_reshade","library_path":"ReShade64.dll",
          "disable_environment":{"DISABLE_VK_LAYER_reshade_1":"1"}}]})");
  return layers;
}
std::vector<std::string> names(const std::vector<const ImplicitVulkanLayer*>& layers) {
  std::vector<std::string> out;
  for (const auto* layer : layers) out.push_back(layer->name);
  return out;
}
}  // namespace

TEST_CASE("Implicit Vulkan layer manifests yield names, libraries and their variables") {
  const auto layers = machine_layers();
  REQUIRE(layers.size() == 5);
  CHECK(layers[0].name == "VK_LAYER_RTSS");
  CHECK(layers[0].library == ".\\RTSSVkLayer64.dll");
  REQUIRE(layers[0].disable_environment.size() == 1);
  CHECK(layers[0].disable_environment[0].first == "DISABLE_RTSS_LAYER");
  CHECK(layers[2].enable_environment[0].first == "ENABLE_VK_LAYER_VALVE_steam_overlay_1");
  CHECK(layers[4].name == "VK_LAYER_reshade");
  CHECK(parse_implicit_layer_manifest("x.json", "not json").empty());
  CHECK(parse_implicit_layer_manifest("x.json", R"({"ICD":{"library_path":"a.dll"}})").empty());
}

TEST_CASE("Loader layer filters match names by prefix, suffix, substring and keyword") {
  CHECK(vulkan_layer_filter_matches("VK_LAYER_RTSS", "VK_LAYER_RTSS"));
  CHECK(vulkan_layer_filter_matches("vk_layer_rtss", "VK_LAYER_RTSS"));
  CHECK(vulkan_layer_filter_matches("VK_LAYER_reshade*", "VK_LAYER_reshade_runtime"));
  CHECK(vulkan_layer_filter_matches("*overlay_64", "VK_LAYER_VALVE_steam_overlay_64"));
  CHECK(vulkan_layer_filter_matches("a, *OBS* ,b", "VK_LAYER_OBS_hook"));
  CHECK(vulkan_layer_filter_matches("~implicit~", "VK_LAYER_anything"));
  CHECK_FALSE(vulkan_layer_filter_matches("VK_LAYER_RTS", "VK_LAYER_RTSS"));
  CHECK_FALSE(vulkan_layer_filter_matches("", "VK_LAYER_RTSS"));
  CHECK_FALSE(vulkan_layer_filter_matches("*reshade", "VK_LAYER_reshade_runtime"));
}

TEST_CASE("Third-party implicit Vulkan layers stay out of the process unless the player keeps them") {
  const auto layers = machine_layers();
  std::map<std::string, std::string> env;
  auto getenv = [&](const char* name) -> const char* {
    auto it = env.find(name);
    return it == env.end() ? nullptr : it->second.c_str();
  };
  SUBCASE("defaults: always-on hooks are disabled; Steam's on-request layers are not running") {
    const auto plan = plan_vulkan_layers(layers, getenv);
    CHECK(names(plan.disabled) == std::vector<std::string>{"VK_LAYER_RTSS", "VK_LAYER_OBS_hook",
                                                            "VK_LAYER_VALVE_steam_overlay_64",
                                                            "VK_LAYER_VALVE_steam_fossilize_64", "VK_LAYER_reshade"});
    CHECK(plan.kept.empty());
    CHECK(plan.loader_disable == "VK_LAYER_RTSS,VK_LAYER_OBS_hook,VK_LAYER_VALVE_steam_overlay_64,"
                                 "VK_LAYER_VALVE_steam_fossilize_64,VK_LAYER_reshade");
  }
  SUBCASE("a layer started on request (Steam's overlay for a Steam launch) was asked for; Fossilize never is") {
    env["ENABLE_VK_LAYER_VALVE_steam_overlay_1"] = "1";
    env["ENABLE_VK_LAYER_VALVE_steam_fossilize_1"] = "1";
    const auto plan = plan_vulkan_layers(layers, getenv);
    CHECK(names(plan.kept) == std::vector<std::string>{"VK_LAYER_VALVE_steam_overlay_64"});
    CHECK(std::ranges::count(names(plan.disabled), "VK_LAYER_VALVE_steam_fossilize_64") == 1);
  }
  SUBCASE("the player keeps layers by Venture's setting or the loader's own variables") {
    env["VENTURE_VULKAN_LAYERS"] = "VK_LAYER_reshade*";
    env["VK_LOADER_LAYERS_ALLOW"] = "*OBS*";
    env["VK_INSTANCE_LAYERS"] = "VK_LAYER_KHRONOS_validation;VK_LAYER_RTSS";
    env["VK_LOADER_LAYERS_DISABLE"] = "VK_LAYER_mine";
    const auto plan = plan_vulkan_layers(layers, getenv);
    CHECK(names(plan.kept) == std::vector<std::string>{"VK_LAYER_RTSS", "VK_LAYER_OBS_hook", "VK_LAYER_reshade"});
    CHECK(plan.loader_disable == "VK_LAYER_mine,VK_LAYER_VALVE_steam_overlay_64,VK_LAYER_VALVE_steam_fossilize_64");
  }
  SUBCASE("everything kept with a keyword") {
    env["VENTURE_VULKAN_LAYERS"] = "~all~";
    const auto plan = plan_vulkan_layers(layers, getenv);
    CHECK(plan.disabled.size() == 1);  // Steam's Fossilize serves only Steam's own titles
    CHECK(plan.kept.size() == 4);
  }
  SUBCASE("a layer already disabled by its own variable is left as it is") {
    env["DISABLE_RTSS_LAYER"] = "1";
    const auto plan = plan_vulkan_layers(layers, getenv);
    CHECK(names(plan.already_disabled) == std::vector<std::string>{"VK_LAYER_RTSS"});
    CHECK(plan.loader_disable.find("VK_LAYER_RTSS") == std::string::npos);
  }
}
TEST_CASE("Driver and system implicit Vulkan layers and FoveaBridge stay by default") {
  // Names from real manifests: bazzite (implicit_layer.d), NVIDIA/AMD hybrid drivers, this owner's PC.
  std::vector<ImplicitVulkanLayer> layers;
  for (const char* name : {"VK_LAYER_MESA_device_select", "VK_LAYER_MESA_anti_lag", "VK_LAYER_NV_optimus",
                           "VK_LAYER_AMD_switchable_graphics_64", "VK_LAYER_AMD_switchable_graphics_32",
                           "VK_LAYER_FROG_gamescope_wsi_x86_64", "VK_LAYER_FROG_gamescope_wsi_x86",
                           "VK_LAYER_FOVEABRIDGE_foveation", "VK_LAYER_FOVEABRIDGE_foveation_32",
                           "VK_LAYER_MANGOHUD_overlay_x86_64", "VK_LAYER_OBS_vkcapture_64",
                           "VK_LAYER_VKBASALT_post_processing", "VK_LAYER_VALVE_steam_overlay_64",
                           "VK_LAYER_VALVE_steam_fossilize_64"})
    layers.push_back({name, "/usr/share/vulkan/implicit_layer.d/layer.json", "layer.so", {}, {}});
  const auto plan = plan_vulkan_layers(layers, [](const char*) -> const char* { return nullptr; });
  CHECK(names(plan.kept) == std::vector<std::string>{"VK_LAYER_MESA_device_select", "VK_LAYER_MESA_anti_lag",
                                                     "VK_LAYER_NV_optimus", "VK_LAYER_AMD_switchable_graphics_64",
                                                     "VK_LAYER_AMD_switchable_graphics_32",
                                                     "VK_LAYER_FROG_gamescope_wsi_x86_64",
                                                     "VK_LAYER_FROG_gamescope_wsi_x86",
                                                     "VK_LAYER_FOVEABRIDGE_foveation",
                                                     "VK_LAYER_FOVEABRIDGE_foveation_32"});
  CHECK(names(plan.disabled) == std::vector<std::string>{"VK_LAYER_MANGOHUD_overlay_x86_64",
                                                         "VK_LAYER_OBS_vkcapture_64",
                                                         "VK_LAYER_VKBASALT_post_processing",
                                                         "VK_LAYER_VALVE_steam_overlay_64",
                                                         "VK_LAYER_VALVE_steam_fossilize_64"});
  CHECK(vulkan_layer_filter_matches(builtin_kept_vulkan_layers, "VK_LAYER_foveabridge_test"));
}
