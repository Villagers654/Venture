#pragma once
// C interface of venture_ngx.dll (tools/ngx-bridge), an MSVC-built module that
// links NVIDIA's NGX SDK. Venture is built with clang/mingw, whose C++ ABI does
// not match the SDK's NVSDK_NGX_Parameter vtable, so it only crosses this C
// boundary. Every function returns an NVSDK_NGX_Result (success has the high
// bits clear of 0xBAD00000) unless stated otherwise.
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VENTURE_NGX_ABI_VERSION 1u

typedef struct VentureNgxInit {
  uint32_t abi_version;  // VENTURE_NGX_ABI_VERSION
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  PFN_vkGetInstanceProcAddr get_instance_proc;
  PFN_vkGetDeviceProcAddr get_device_proc;
  const wchar_t* data_path;   // NGX logs and caches
  const wchar_t* dlss_path;   // directory with nvngx_dlss.dll, or null
} VentureNgxInit;

// One sampled or storage view of a single array layer.
typedef struct VentureNgxImage {
  VkImage image;
  VkImageView view;
  VkFormat format;
  uint32_t width, height;
  uint32_t layer;
} VentureNgxImage;

typedef struct VentureNgxDlssCreate {
  uint32_t render_width, render_height;
  uint32_t output_width, output_height;
  int32_t quality;       // NVSDK_NGX_PerfQuality_Value
  int32_t create_flags;  // NVSDK_NGX_DLSS_Feature_Flags
} VentureNgxDlssCreate;

typedef struct VentureNgxDlssEvaluate {
  VentureNgxImage color, output, depth, motion;
  float jitter_x, jitter_y;        // render pixels
  float motion_scale_x, motion_scale_y;
  uint32_t render_width, render_height;
  int32_t reset;
  // Top-left of the rendered area in color, depth and motion (dynamic
  // resolution, a view rect smaller than the image).
  uint32_t subrect_x, subrect_y;
} VentureNgxDlssEvaluate;

// Instance and device extensions NGX needs; static strings owned by NGX.
typedef uint32_t (*PFN_ventureNgxRequiredExtensions)(uint32_t* instance_count, const char*** instance_names,
                                                     uint32_t* device_count, const char*** device_names);
typedef uint32_t (*PFN_ventureNgxInit)(const VentureNgxInit* info);
// 1 when DLSS Super Resolution is available on the initialized device.
typedef int32_t (*PFN_ventureNgxDlssAvailable)(void);
typedef uint32_t (*PFN_ventureNgxDlssCreate)(VkCommandBuffer command, const VentureNgxDlssCreate* info, void** feature);
typedef uint32_t (*PFN_ventureNgxDlssEvaluate)(VkCommandBuffer command, void* feature, const VentureNgxDlssEvaluate* info);
typedef uint32_t (*PFN_ventureNgxRelease)(void* feature);
typedef uint32_t (*PFN_ventureNgxShutdown)(VkDevice device);

#ifdef __cplusplus
}
#endif
