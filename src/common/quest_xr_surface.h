#pragma once
// The OpenXR extensions a Quest game can enable that Venture answers itself,
// whatever the host runtime supports. The PC runtime advertises them to the
// game (hle/openxr/state.cpp capabilities()); a native host plan
// (common/android/native_host.h) compares them with a headset's own runtime to
// decide which ones Venture must still supply there. One list, so both agree.
#include <array>
#include <cstdint>
#include <string_view>

namespace venture::quest_xr {
enum class Kind : std::uint8_t {
  // The Android OpenXR loader interface. On an Android host the device's
  // loader and runtime own these; on a PC Venture is the loader.
  loader,
  // Venture implements the extension's behaviour without the host runtime
  // (in the PC bridge, or in a shim layer on a native host).
  emulated,
};
enum class Isa : std::uint8_t { any, a64 };  // a64: the A32 adapters lack its pointer layouts

struct Extension {
  std::string_view name;
  std::uint32_t version;
  Kind kind;
  Isa isa;
};

// Versions are the pinned OpenXR SDK's spec versions where one is named
// (hle/openxr/state.cpp asserts them against the headers).
inline constexpr std::array kExtensions{
    Extension{"XR_KHR_loader_init", 2, Kind::loader, Isa::any},
    Extension{"XR_KHR_loader_init_android", 1, Kind::loader, Isa::any},
    Extension{"XR_KHR_android_create_instance", 3, Kind::loader, Isa::any},
    Extension{"XR_KHR_android_thread_settings", 6, Kind::emulated, Isa::any},
    Extension{"XR_FB_display_refresh_rate", 1, Kind::emulated, Isa::any},
    Extension{"XR_FB_color_space", 3, Kind::emulated, Isa::any},
    Extension{"XR_FB_foveation", 1, Kind::emulated, Isa::any},
    Extension{"XR_FB_foveation_configuration", 1, Kind::emulated, Isa::any},
    Extension{"XR_FB_swapchain_update_state", 3, Kind::emulated, Isa::any},
    Extension{"XR_FB_touch_controller_pro", 1, Kind::emulated, Isa::any},
    Extension{"XR_META_touch_controller_plus", 1, Kind::emulated, Isa::any},
    Extension{"XR_KHR_locate_spaces", 1, Kind::emulated, Isa::any},
    Extension{"XR_FB_haptic_pcm", 1, Kind::emulated, Isa::any},
    Extension{"XR_FB_haptic_amplitude_envelope", 1, Kind::emulated, Isa::any},
    // Guest Vulkan images are copied to the host chain, which applies the flip.
    Extension{"XR_FB_composition_layer_image_layout", 1, Kind::emulated, Isa::any},
    // Drawn flat where the host has no cylinders (xrEndFrame).
    Extension{"XR_KHR_composition_layer_cylinder", 1, Kind::emulated, Isa::any},
    // Drawn into a projection layer where the host has none (sphere_layers.h).
    Extension{"XR_KHR_composition_layer_equirect", 3, Kind::emulated, Isa::any},
    Extension{"XR_KHR_composition_layer_equirect2", 1, Kind::emulated, Isa::any},
    // Answered without the host (extensions.cpp xrRequestBoundaryVisibilityMETA).
    Extension{"XR_META_boundary_visibility", 1, Kind::emulated, Isa::any},
    Extension{"XR_META_recommended_layer_resolution", 1, Kind::emulated, Isa::a64},
    Extension{"XR_META_headset_id", 2, Kind::emulated, Isa::a64},
    // Translated to core usage flags (graphics.cpp).
    Extension{"XR_META_vulkan_swapchain_create_info", 1, Kind::emulated, Isa::a64},
    // Compositor filter hints: host copies omit the node (PresentationLayers drops
    // structures without metadata) and the host composites the full image.
    Extension{"XR_FB_composition_layer_settings", 1, Kind::emulated, Isa::a64},
    // Answered as a Quest without a room setup (spatial.cpp): anchors work in
    // process, saves last for the process, queries find no scene, capture fails.
    Extension{"XR_FB_spatial_entity", 3, Kind::emulated, Isa::a64},
    Extension{"XR_FB_spatial_entity_query", 1, Kind::emulated, Isa::a64},
    Extension{"XR_FB_spatial_entity_storage", 1, Kind::emulated, Isa::a64},
    Extension{"XR_FB_scene", 4, Kind::emulated, Isa::a64},
    Extension{"XR_FB_scene_capture", 1, Kind::emulated, Isa::a64},
    // Messengers and labels are accepted; messages go to the log (debug_utils.cpp).
    Extension{"XR_EXT_debug_utils", 5, Kind::emulated, Isa::a64},
};

constexpr const Extension* find(std::string_view name) {
  for (const auto& extension : kExtensions)
    if (extension.name == name) return &extension;
  return nullptr;
}
}  // namespace venture::quest_xr
