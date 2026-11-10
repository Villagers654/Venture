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
  const wchar_t* dlss_path;   // directory with nvngx_dlssnr.dll, or null
} VentureNgxInit;

// One sampled or storage view of a single array layer.
typedef struct VentureNgxImage {
  VkImage image;
  VkImageView view;
  VkFormat format;
  uint32_t width, height;
  uint32_t layer;
} VentureNgxImage;

// DLSS Neural Rendering (NGX feature 18) on one foveated region, at 1:1 (xr/venture_nr.cpp).

typedef struct VentureNgxNrTuning {
  uint32_t preset;  // 0: NVIDIA's choice, 1: model A, 2: model B
  uint32_t style;   // 0 Neutral, 1 Natural, 2 Cinematic
  float intensity, local_tone, local_structure, skin_structure;
} VentureNgxNrTuning;
typedef struct VentureNgxNrCreate {
  uint32_t width, height;
  VentureNgxNrTuning tuning;
} VentureNgxNrCreate;
typedef struct VentureNgxNrEvaluate {
  VentureNgxImage color, output, motion, depth;  // all `width` x `height`
  uint32_t width, height;
  int32_t reset;
  int32_t depth_inverted;
  VentureNgxNrTuning tuning;
} VentureNgxNrEvaluate;
typedef uint32_t (*PFN_ventureNgxNrCreate)(VkCommandBuffer command, const VentureNgxNrCreate* info, void** feature);
typedef uint32_t (*PFN_ventureNgxNrEvaluate)(VkCommandBuffer command, void* feature, const VentureNgxNrEvaluate* info);

// Instance and device extensions NGX needs; static strings owned by NGX.
typedef uint32_t (*PFN_ventureNgxRequiredExtensions)(uint32_t* instance_count, const char*** instance_names,
                                                     uint32_t* device_count, const char*** device_names);
typedef uint32_t (*PFN_ventureNgxInit)(const VentureNgxInit* info);
typedef uint32_t (*PFN_ventureNgxRelease)(void* feature);
typedef uint32_t (*PFN_ventureNgxShutdown)(VkDevice device);

#ifdef __cplusplus
}
#endif
