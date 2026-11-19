// The PC runtime's side of the VrApi core (hle/vrapi/host.h): the game is
// guest code, its frames wait on guest threads, and its graphics reach the
// host runtime through Venture's renderer (xr_images, the shared Vulkan
// device).
#include "hle/vrapi/host.h"

#include "core/engine_policy.h"
#include "core/process.h"
#include "core/thread.h"
#include "core/wait.h"
#include "hle/openxr/state.h"
#include "hle/vrapi/state.h"
#include "hle/vrapi/vulkan_images.h"
#include "hle/vulkan/vulkan.h"

namespace venture::hle::vrapi::host {
bool vulkan_available() { return process_config().isa() == GuestIsa::a64; }
bool images_available() { return xr_images() != nullptr; }
std::int32_t device_type() { return process_config().device_identity->vrapi_type; }
bool hand_tracking_required() { return hle::openxr::hand_tracking_required(); }
bool vulkan_extension_usable(std::string_view name) { return vulkan::extension_translatable(name); }
XrResult create_session(const abi::Mode &mode) {
  const bool vulkan = state().init.graphics_api == vulkan_api;
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
  if (vulkan && XR_FAILED(bind_vulkan_queue(reinterpret_cast<const abi::ModeVulkan &>(mode), binding))) {
    failure("EnterVrMode Vulkan queue", invalid);
    return XR_ERROR_VALIDATION_FAILURE;
  }
  return session().create(SessionOwner::vrapi, vulkan ? &binding : nullptr);
}
bool keeps_session() { return false; }
void vr_mode_entered() {
  if (auto *thread = GuestThread::current()) engine_policy::on_vrapi_session(*thread);
}
bool wait_frame_change(std::uint32_t epoch, std::int64_t deadline) {
  auto *thread = GuestThread::current();
  if (!thread) return false;
  futex_wait(*thread, h2g(&session().frame_epoch), epoch, deadline);
  return true;
}
}  // namespace venture::hle::vrapi::host
