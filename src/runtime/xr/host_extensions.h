// The runtime extensions Venture enables on its host instance wherever it runs
// (the PC runtime's xr/host_xr.cpp, libvrapi.so on Android headsets), each
// only where the runtime offers it. SDK strings are not a guest capability
// list: this is only the host side; guest enumeration is filtered
// independently (r4-xr/report.md).
#pragma once
#include <string_view>

namespace venture::xr {
inline constexpr std::string_view kHostExtensions[] = {
    "XR_KHR_vulkan_enable",
    "XR_KHR_vulkan_enable2",
    "XR_KHR_win32_convert_performance_counter_time",
    "XR_KHR_convert_timespec_time",
    "XR_EXT_hand_tracking",
    "XR_EXT_hand_tracking_data_source",
    "XR_EXT_hand_joints_motion_range",
    "XR_FB_hand_tracking_aim",
    "XR_FB_hand_tracking_capsules",
    "XR_FB_hand_tracking_mesh",
    "XR_FB_display_refresh_rate",
    "XR_FB_color_space",
    "XR_FB_swapchain_update_state",
    "XR_FB_swapchain_update_state_vulkan",
    "XR_META_performance_metrics",
    "XR_KHR_generic_controller",
    "XR_FB_haptic_pcm",
    "XR_FB_haptic_amplitude_envelope",
    "XR_KHR_composition_layer_depth",
    "XR_KHR_swapchain_usage_input_attachment_bit",
    "XR_MND_swapchain_usage_input_attachment_bit",
    "XR_KHR_composition_layer_cylinder",
    "XR_KHR_composition_layer_cube",
    "XR_KHR_composition_layer_equirect",
    "XR_KHR_composition_layer_equirect2",
    "XR_KHR_composition_layer_color_scale_bias",
    "XR_FB_space_warp",
    "XR_EXT_local_floor",
    "XR_EXT_performance_settings",
    "XR_EXT_view_configuration_depth_range",
    // Controllers (common/controls.h): their profiles on an OpenXR 1.0
    // instance, and trackpad halves.
    "XR_KHR_binding_modification",
    "XR_EXT_dpad_binding",
    "XR_META_touch_controller_plus",
    "XR_FB_touch_controller_pro",
    "XR_HTC_vive_cosmos_controller_interaction",
    "XR_HTC_vive_focus3_controller_interaction",
    "XR_EXT_hp_mixed_reality_controller",
    "XR_BD_controller_interaction",
    "XR_MNDX_psvr2_interaction",
    "XR_VALVE_frame_controller_interaction",
};
}  // namespace venture::xr
