#pragma once
#include <vulkan/vulkan_core.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace venture::xr {
// One layer of a registered implicit Vulkan layer manifest. The loader loads
// these into every Vulkan instance of the process unless they are disabled.
struct ImplicitVulkanLayer {
  std::string name;
  std::filesystem::path manifest;
  std::string library;  // library_path as written in the manifest
  std::vector<std::pair<std::string, std::string>> disable_environment, enable_environment;
};
// The layers of one manifest (its "layer" or "layers"); empty for anything else.
std::vector<ImplicitVulkanLayer> parse_implicit_layer_manifest(const std::filesystem::path& manifest,
                                                               std::string_view text);
// The loader's layer filter globs (VK_LOADER_LAYERS_*): "*" at the start, the
// end or both, or the keywords ~all~ and ~implicit~.
bool vulkan_layer_filter_matches(std::string_view filters, std::string_view name);
struct VulkanLayerPlan {
  std::vector<const ImplicitVulkanLayer*> disabled, kept, already_disabled;
  // VK_LOADER_LAYERS_DISABLE after the plan (the user's own filters first).
  std::string loader_disable;
};
// Third-party implicit layers (overlays, capture and post-processing hooks)
// wrap every Vulkan call of Venture's single shared instance, which is no
// application of theirs; they stay out unless the player keeps them:
// VENTURE_VULKAN_LAYERS (Settings "vulkanLayers") or the loader's own
// VK_LOADER_LAYERS_ALLOW / VK_LOADER_LAYERS_ENABLE / VK_INSTANCE_LAYERS name
// them, or the layer is one that runs only on request (enable_environment)
// and was requested, or it is on the built-in keep list (GPU selection,
// gamescope presentation, FoveaBridge).
extern const std::string_view builtin_kept_vulkan_layers;
VulkanLayerPlan plan_vulkan_layers(const std::vector<ImplicitVulkanLayer>& layers,
                                   const std::function<const char*(const char*)>& getenv);
// Explicit Vulkan layers in the final create info of an instance Venture makes
// through xrCreateVulkanInstanceKHR. Venture asks for none there, so each was
// added by the OpenXR runtime or one of its API layers, and it would wrap
// every Vulkan call on that instance: the game's translated rendering and
// Venture's own composition, not the native application such a layer is made
// for (its heuristics, such as treating render passes the size of an XR
// swapchain as eye rendering, do not hold there). They are left out like
// third-party implicit layers and kept by the same rule: the player's
// VENTURE_VULKAN_LAYERS, VK_INSTANCE_LAYERS, VK_LOADER_LAYERS_ALLOW or
// VK_LOADER_LAYERS_ENABLE, or the built-in keep list. `getenv` reads the
// process environment as it was at start.
struct HostAddedVulkanLayers {
  std::vector<const char*> kept;  // pointers into the given names
  std::vector<std::string> left_out;
};
HostAddedVulkanLayers plan_host_added_vulkan_layers(std::uint32_t count, const char* const* names,
                                                    const std::function<const char*(const char*)>& getenv);
// Applies that plan to `info` (its layer list then points into the result's
// `kept`, which must outlive the call that uses `info`) and logs what the host
// added, once per distinct list. `what` names the instance in the log.
HostAddedVulkanLayers leave_out_host_added_vulkan_layers(VkInstanceCreateInfo& info, std::string_view what);
// Before the first Vulkan instance of the process: applies the plan through
// each layer's disable_environment (every loader) and VK_LOADER_LAYERS_DISABLE
// (loader 1.3.234 and later), and logs it. Steam's Fossilize layer is always
// disabled: it serves only Steam's own titles.
void configure_vulkan_layers_for_process();
// After the shared device exists: logs the implicit layer libraries the
// process has loaded, so a report shows what sits between Venture and the driver.
void log_loaded_vulkan_layers();
// Platform parts: registered implicit manifests and whether a library is loaded.
std::vector<std::filesystem::path> implicit_vulkan_layer_manifests();
bool vulkan_layer_library_loaded(const ImplicitVulkanLayer& layer);
std::string vulkan_loader_description();  // the system loader's path and version
bool set_process_environment(const char* name, const char* value);
}  // namespace venture::xr
