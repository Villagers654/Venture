// venture_ngx.dll: NGX DLSS Neural Rendering for Venture NR
// (src/runtime/xr/venture_nr.cpp). Built with MSVC against NVIDIA's NGX SDK; see
// ngx_bridge_abi.h for why Venture does not call the SDK directly.
#include <windows.h>
#include <vulkan/vulkan.h>

#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>

#include <cstdio>
#include <mutex>
#include <string>

#include "xr/ngx_bridge_abi.h"

namespace {
// A generic application id (no NVIDIA project id).
constexpr unsigned long long application_id = 0x0876232cULL;
std::mutex mutex;
VkDevice initialized{};
NVSDK_NGX_Parameter* capabilities{};
std::wstring dlss_directory;  // nvngx_dlssnr.dll

NVSDK_NGX_Resource_VK resource(const VentureNgxImage& image, bool read_write) {
  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, image.layer, 1};
  return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, range, image.format, image.width,
                                                image.height, read_write);
}
}  // namespace

extern "C" {
__declspec(dllexport) uint32_t venture_ngx_required_extensions(uint32_t* instance_count, const char*** instance_names,
                                                               uint32_t* device_count, const char*** device_names) {
  unsigned int ic{}, dc{};
  const auto result = NVSDK_NGX_VULKAN_RequiredExtensions(&ic, instance_names, &dc, device_names);
  if (instance_count) *instance_count = ic;
  if (device_count) *device_count = dc;
  return result;
}

__declspec(dllexport) uint32_t venture_ngx_init(const VentureNgxInit* info) {
  if (!info || info->abi_version != VENTURE_NGX_ABI_VERSION) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  std::lock_guard lock(mutex);
  if (initialized) return initialized == info->device ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_FeatureAlreadyExists;
  dlss_directory = info->dlss_path ? info->dlss_path : L"";
  const wchar_t* paths[] = {dlss_directory.c_str()};
  NVSDK_NGX_FeatureCommonInfo common{};
  if (!dlss_directory.empty()) common.PathListInfo = {paths, 1};
  auto result = NVSDK_NGX_VULKAN_Init(application_id, info->data_path ? info->data_path : L".", info->instance,
                                      info->physical_device, info->device, info->get_instance_proc,
                                      info->get_device_proc, &common);
  if (NVSDK_NGX_FAILED(result)) return result;
  result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&capabilities);
  if (NVSDK_NGX_FAILED(result)) {
    NVSDK_NGX_VULKAN_Shutdown1(info->device);
    return result;
  }
  initialized = info->device;
  return result;
}

namespace {
// DLSS-NR's scaling ratio query: 1:1 (NR runs at the region's own size).
NVSDK_NGX_Result NVSDK_CONV nr_scaling_ratio(NVSDK_NGX_Parameter* parameters) {
  if (!parameters) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  float scale = 1.0f;
  if (NVSDK_NGX_FAILED(parameters->Get("DLSSNR.Scale", &scale))) scale = 1.0f;
  parameters->Set("DLSSNR.ScalingRatio", scale < 0.1f ? 0.1f : scale > 1.0f ? 1.0f : scale);
  return NVSDK_NGX_Result_Success;
}
void nr_parameters(NVSDK_NGX_Parameter* p, unsigned w, unsigned h, const VentureNgxNrTuning& t) {
  for (const char* key : {"Width", "OutWidth", "DLSSNR.Width", "DLSSNR.InputWidth", "DLSSNR.OutputWidth"}) p->Set(key, w);
  for (const char* key : {"Height", "OutHeight", "DLSSNR.Height", "DLSSNR.InputHeight", "DLSSNR.OutputHeight"}) p->Set(key, h);
  p->Set("DLSSNR.Enabled", 1u);
  p->Set("DLSSNR.ScalingRatio", 1.0f);
  p->Set("DLSSNR.Scale", 1.0f);
  p->Set("DLSSNR.Upscaling", 0u);
  p->Set("PerfQualityValue", 0u);
  p->Set("CreationNodeMask", 1u);
  p->Set("VisibilityNodeMask", 1u);
  p->Set("DLSSNRComputeScalingRatioCallback", reinterpret_cast<void*>(&nr_scaling_ratio));
  p->Set("DLSSNR.Hint.Render.Preset", t.preset);
  p->Set("DLSSNR.Style", t.style);
  p->Set("DLSSNR.Intensity", t.intensity);
  p->Set("DLSSNR.LocalToneStrength", t.local_tone);
  p->Set("DLSSNR.LocalStructureStrength", t.local_structure);
  p->Set("DLSSNR.SkinStructureStrength", t.skin_structure);
  p->Set("DLSSNR.ControlMask", static_cast<void*>(nullptr));
  p->Set("DLSSNR.UseAutoMask", 0u);
  p->Set("DLSSNR.UICorrection", 0u);
}
}  // namespace

__declspec(dllexport) uint32_t venture_ngx_nr_create(VkCommandBuffer command, const VentureNgxNrCreate* info,
                                                     void** feature) {
  if (!info || !feature) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  std::lock_guard lock(mutex);
  if (!capabilities) return NVSDK_NGX_Result_FAIL_NotInitialized;
  nr_parameters(capabilities, info->width, info->height, info->tuning);
  NVSDK_NGX_Handle* handle{};
  const auto result = NVSDK_NGX_VULKAN_CreateFeature1(initialized, command, NVSDK_NGX_Feature_Reserved18, capabilities,
                                                      &handle);
  *feature = NVSDK_NGX_SUCCEED(result) ? handle : nullptr;
  return result;
}

__declspec(dllexport) uint32_t venture_ngx_nr_evaluate(VkCommandBuffer command, void* feature,
                                                       const VentureNgxNrEvaluate* info) {
  if (!feature || !info) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  std::lock_guard lock(mutex);
  if (!capabilities) return NVSDK_NGX_Result_FAIL_NotInitialized;
  auto color = resource(info->color, false), output = resource(info->output, true),
       motion = resource(info->motion, false), depth = resource(info->depth, false);
  auto* p = capabilities;
  nr_parameters(p, info->width, info->height, info->tuning);
  p->Set("DLSSNR.Color", static_cast<void*>(&color));
  p->Set("DLSSNR.Output", static_cast<void*>(&output));
  p->Set("DLSSNR.MVec", static_cast<void*>(&motion));
  p->Set("DLSSNR.Depth", static_cast<void*>(&depth));
  for (const char* name : {"DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY", "DLSSNR.OutputSubrectBaseX",
                           "DLSSNR.OutputSubrectBaseY", "DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY",
                           "DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY"})
    p->Set(name, 0u);
  for (const char* name : {"DLSSNR.ColorSubrectWidth", "DLSSNR.OutputSubrectWidth", "DLSSNR.MVecSubrectWidth",
                           "DLSSNR.DepthSubrectWidth"})
    p->Set(name, info->width);
  for (const char* name : {"DLSSNR.ColorSubrectHeight", "DLSSNR.OutputSubrectHeight", "DLSSNR.MVecSubrectHeight",
                           "DLSSNR.DepthSubrectHeight"})
    p->Set(name, info->height);
  p->Set("DLSSNR.MVecScaleX", float(info->width));
  p->Set("DLSSNR.MVecScaleY", float(info->height));
  p->Set("DLSSNR.Reset", info->reset ? 1u : 0u);
  p->Set("DLSSNR.DepthInverted", info->depth_inverted ? 1u : 0u);
  return NVSDK_NGX_VULKAN_EvaluateFeature(command, static_cast<NVSDK_NGX_Handle*>(feature), p, nullptr);
}

__declspec(dllexport) uint32_t venture_ngx_release(void* feature) {
  if (!feature) return NVSDK_NGX_Result_Success;
  std::lock_guard lock(mutex);
  return NVSDK_NGX_VULKAN_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(feature));
}

__declspec(dllexport) uint32_t venture_ngx_shutdown(VkDevice device) {
  std::lock_guard lock(mutex);
  if (!initialized || device != initialized) return NVSDK_NGX_Result_Success;
  if (capabilities) NVSDK_NGX_VULKAN_DestroyParameters(capabilities);
  capabilities = nullptr;
  initialized = VK_NULL_HANDLE;
  return NVSDK_NGX_VULKAN_Shutdown1(device);
}
}
