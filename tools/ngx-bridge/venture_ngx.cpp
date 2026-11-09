// venture_ngx.dll: NGX DLSS Super Resolution for Venture's Vulkan presentation
// path (src/runtime/xr/dlss.cpp). Built with MSVC against NVIDIA's NGX SDK; see
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
// Generic application id, as used by Cheeky's D3D12 path.
constexpr unsigned long long application_id = 0x0876232cULL;
std::mutex mutex;
VkDevice initialized{};
NVSDK_NGX_Parameter* capabilities{};
std::wstring dlss_directory;

NVSDK_NGX_Resource_VK resource(const VentureNgxImage& image, bool read_write) {
  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, image.layer, 1};
  return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, range, image.format, image.width,
                                                image.height, read_write);
}
NVSDK_NGX_Resource_VK depth_resource(const VentureNgxImage& image) {
  // A depth input is a color-aspect view of an R32_SFLOAT copy (resolve pass)
  // or of a real depth image; DLSS only samples it.
  const bool depth = image.format == VK_FORMAT_D32_SFLOAT || image.format == VK_FORMAT_D16_UNORM ||
                     image.format == VK_FORMAT_X8_D24_UNORM_PACK32 || image.format == VK_FORMAT_D24_UNORM_S8_UINT ||
                     image.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
  const VkImageSubresourceRange range{VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0,
                                      1, image.layer, 1};
  return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, range, image.format, image.width,
                                                image.height, false);
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

__declspec(dllexport) int32_t venture_ngx_dlss_available() {
  std::lock_guard lock(mutex);
  if (!capabilities) return 0;
  int available{}, needs_driver{};
  if (NVSDK_NGX_FAILED(capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available))) return 0;
  capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
  return available && !needs_driver ? 1 : 0;
}

__declspec(dllexport) uint32_t venture_ngx_dlss_create(VkCommandBuffer command, const VentureNgxDlssCreate* info,
                                                       void** feature) {
  if (!info || !feature) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  std::lock_guard lock(mutex);
  if (!capabilities) return NVSDK_NGX_Result_FAIL_NotInitialized;
  NVSDK_NGX_DLSS_Create_Params params{};
  params.Feature.InWidth = info->render_width;
  params.Feature.InHeight = info->render_height;
  params.Feature.InTargetWidth = info->output_width;
  params.Feature.InTargetHeight = info->output_height;
  params.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value(info->quality);
  params.InFeatureCreateFlags = info->create_flags;
  NVSDK_NGX_Handle* handle{};
  const auto result = NGX_VULKAN_CREATE_DLSS_EXT1(initialized, command, 1, 1, &handle, capabilities, &params);
  *feature = NVSDK_NGX_SUCCEED(result) ? handle : nullptr;
  return result;
}

__declspec(dllexport) uint32_t venture_ngx_dlss_evaluate(VkCommandBuffer command, void* feature,
                                                         const VentureNgxDlssEvaluate* info) {
  if (!feature || !info) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  std::lock_guard lock(mutex);
  if (!capabilities) return NVSDK_NGX_Result_FAIL_NotInitialized;
  auto color = resource(info->color, false), output = resource(info->output, true),
       motion = resource(info->motion, false), depth = depth_resource(info->depth);
  NVSDK_NGX_VK_DLSS_Eval_Params params{};
  params.Feature.pInColor = &color;
  params.Feature.pInOutput = &output;
  params.pInDepth = &depth;
  params.pInMotionVectors = &motion;
  params.InJitterOffsetX = info->jitter_x;
  params.InJitterOffsetY = info->jitter_y;
  params.InRenderSubrectDimensions = {info->render_width, info->render_height};
  params.InColorSubrectBase = params.InDepthSubrectBase = params.InMVSubrectBase = {info->subrect_x, info->subrect_y};
  params.InReset = info->reset;
  params.InMVScaleX = info->motion_scale_x;
  params.InMVScaleY = info->motion_scale_y;
  return NGX_VULKAN_EVALUATE_DLSS_EXT(command, static_cast<NVSDK_NGX_Handle*>(feature), capabilities, &params);
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
