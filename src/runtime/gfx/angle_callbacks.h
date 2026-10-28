// Host C ABI matching wt/angle's eglext_angle_venture.h version 1, defined in
// third_party/angle-patches/0001-venture-vulkan-creation-callbacks.patch.
// Keep version, size, userdata, calling convention and member order identical.
#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
namespace venture::gfx {
inline constexpr std::int64_t kAngleVulkanCallbacksAttribute = 0x51500001;
struct AngleVulkanCallbacksV1 {
  std::uint32_t version;
  std::uint32_t size;
  void* userData;
  VkResult(VKAPI_PTR* createInstance)(void*, PFN_vkGetInstanceProcAddr,
                                      const VkInstanceCreateInfo*,
                                      const VkAllocationCallbacks*,
                                      VkInstance*);
  VkResult(VKAPI_PTR* choosePhysicalDevice)(void*, PFN_vkGetInstanceProcAddr,
                                            VkInstance, std::uint32_t,
                                            const VkPhysicalDevice*,
                                            VkPhysicalDevice*);
  VkResult(VKAPI_PTR* createDevice)(void*, PFN_vkGetInstanceProcAddr,
                                    VkInstance, VkPhysicalDevice,
                                    const VkDeviceCreateInfo*,
                                    const VkAllocationCallbacks*, VkDevice*);
  void(VKAPI_PTR* deviceReady)(void*, VkInstance, VkPhysicalDevice, VkDevice,
                               std::uint32_t, std::uint32_t, VkQueue);
  // Optional tail, guarded by size in ANGLE. Wrap its resolver before volk
  // loads instance/device dispatch. Older 48-byte v1 tables still work.
  PFN_vkGetInstanceProcAddr(VKAPI_PTR* wrapLoader)(void*, PFN_vkGetInstanceProcAddr);
};
static_assert(sizeof(AngleVulkanCallbacksV1) == 56);
const AngleVulkanCallbacksV1& angle_vulkan_callbacks();
}  // namespace venture::gfx
