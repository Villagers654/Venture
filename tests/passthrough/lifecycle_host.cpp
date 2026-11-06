// Script only the external OpenXR host and unrelated process services.
// openxr.cpp, graphics.cpp and xr/session.cpp are production code in this target.
#include "support.h"
#include "lifecycle_host.h"
#include "core/wait.h"
#include "xr/clock.h"
#include "xr/desktop_mirror.h"
#include "xr/dev_dump.h"
#include <deque>
#include <cstring>
#include <mutex>
namespace venture::test::passthrough::host {
XrResult begin_result = XR_SUCCESS;
std::deque<XrSessionState> events;
std::mutex mutex;
Frame last;
void reset() { events.clear(); last = {}; begin_result = XR_SUCCESS; }
void event(XrSessionState state) { events.push_back(state); }
Frame last_frame() { std::lock_guard lock(mutex); return last; }
}
namespace host = venture::test::passthrough::host;
XrResult XRAPI_CALL xrBeginSession(XrSession, const XrSessionBeginInfo*) { return host::begin_result; }
XrResult XRAPI_CALL xrEndSession(XrSession) { return XR_SUCCESS; }
XrResult XRAPI_CALL xrWaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState* out) {
  out->predictedDisplayPeriod = 11'111'111; out->predictedDisplayTime = venture::monotonic_ns() + out->predictedDisplayPeriod;
  out->shouldRender = XR_TRUE; return XR_SUCCESS;
}
XrResult XRAPI_CALL xrBeginFrame(XrSession, const XrFrameBeginInfo*) { return XR_SUCCESS; }
XrResult XRAPI_CALL xrEndFrame(XrSession, const XrFrameEndInfo* info) {
  std::lock_guard lock(host::mutex); host::last = {}; host::last.layers = info->layerCount;
  host::last.blend_mode = info->environmentBlendMode;
  for (std::uint32_t i = 0; i < info->layerCount; ++i) host::last.types.push_back(info->layers[i]->type);
  for (std::uint32_t i = 0; i < info->layerCount; ++i)
    if (info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_QUAD)
      host::last.quads.push_back(*reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[i]));
  for (std::uint32_t i=0;i<info->layerCount;++i) if (info->layers[i]->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
    const auto* p=reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
    host::last.projection_flags=p->layerFlags;
    for (std::uint32_t j=0;j<std::min(p->viewCount,2u);++j) {
      host::last.views[j]=p->views[j]; host::last.views[j].next=nullptr;
      for (auto* n=static_cast<const XrBaseInStructure*>(p->views[j].next);n;n=n->next)
        if(n->type==XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) host::last.depth=true;
    }
  }
  return XR_SUCCESS;
}
XrResult XRAPI_CALL xrPollEvent(XrInstance, XrEventDataBuffer* out) {
  if(host::events.empty()) return XR_EVENT_UNAVAILABLE;
  const XrEventDataSessionStateChanged e{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED,nullptr,
      venture::test::passthrough::target,host::events.front(),0};
  host::events.pop_front(); std::memcpy(out,&e,sizeof(e)); return XR_SUCCESS;
}

#include "core/thread.h"
#include "core/profile.h"
#include "common/startup_progress.h"
#include "platform/profile.h"
#include "hle/vulkan/timing.h"
#include "xr/display_memory.h"
#include <stdexcept>
namespace venture {
GuestThread* GuestThread::current() { return nullptr; }
int futex_wake(GuestAddr, int, std::uint32_t, sync_census::WakeSource) { return 0; }
int futex_wait(GuestThread&, GuestAddr, std::uint32_t, std::int64_t, std::uint32_t, bool) {
  throw std::runtime_error("Unexpected emulated guest-thread wait in host-only lifecycle fixture");
}
namespace log { void crash_write(const char*, std::size_t) noexcept {} }
namespace platform { std::uint32_t profile_thread_id() noexcept { return 1; } }
namespace profile {
const bool enabled = false;
void mark(std::string_view) {}
void frame(std::uint64_t) {}
void report_pipelines(std::string_view) {}
}
namespace startup_diagnostics {
void arm() {}
void note_frame_submitted() {}
void note_host_session_state(int) {}
}
namespace vulkan {
ScopedHostWait::ScopedHostWait(const char*) {}
ScopedHostWait::~ScopedHostWait() {}
// Guest image bookkeeping belongs to Venture's Vulkan layer, not this fixture.
void note_image(VkImage, const VkImageCreateInfo&) {}
void forget_image(VkImage) {}
VkFormat host_depth_format(VkPhysicalDevice, VkFormat format) { return format; }
void timing_end_frame(std::int64_t, std::uint32_t, float) {}
VkResult flush_queue_submissions(VkDevice, std::uint32_t, std::uint32_t) { return VK_SUCCESS; }
}
namespace xr {
void remember_display_period(std::int64_t) {}
void remember_eye_fov(const std::array<XrFovf, 2>&) {}
void remember_host_rates(float, const std::vector<float>&) {}
void dev_dump_frame(const XrFrameEndInfo*, XrResult) {}
void note_menu_profiles_changed(XrSession) {}
// No desktop mirror window in the fixture.
bool mirror_ready() { return false; }
void mirror_axis(const XrFovf&, float& x, float& y) { x = y = 0; }
void mirror_capture(const XrGraphicsBindingVulkanKHR&, bool, const MirrorSource&) {}
XrResult time_to_monotonic(XrTime value, std::int64_t* out) { *out=value;return XR_SUCCESS; }
XrResult clock_sample(XrTime* time, std::int64_t* monotonic) { *time = *monotonic = venture::monotonic_ns(); return XR_SUCCESS; }
}
namespace hle::openxr { XrResult present_android_surfaces() { return XR_SUCCESS; } }
}
XrResult XRAPI_CALL xrPathToString(XrInstance, XrPath, std::uint32_t, std::uint32_t*, char*) { return XR_ERROR_PATH_INVALID; }
// The head 1.5 m above LOCAL's origin, looking along -Z.
XrResult XRAPI_CALL xrLocateSpace(XrSpace, XrSpace, XrTime, XrSpaceLocation* location) {
  location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
  location->pose = {{0, 0, 0, 1}, {0, 1.5f, 0}};
  return XR_SUCCESS;
}
XrResult XRAPI_CALL xrLocateViews(XrSession, const XrViewLocateInfo*, XrViewState*, std::uint32_t, std::uint32_t*, XrView*) { return XR_ERROR_FUNCTION_UNSUPPORTED; }
XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType,
    std::uint32_t capacity, std::uint32_t* count, XrViewConfigurationView* out) {
  *count = 2;
  if (!capacity) return XR_SUCCESS;
  if (capacity < 2) return XR_ERROR_SIZE_INSUFFICIENT;
  for (unsigned i = 0; i < 2; ++i) { out[i].maxImageRectWidth = out[i].maxImageRectHeight = 4096; out[i].maxSwapchainSampleCount = 4; }
  return XR_SUCCESS;
}

XrResult XRAPI_CALL xrCreateSwapchain(XrSession target, const XrSwapchainCreateInfo* info, XrSwapchain* out) {
  return venture::xr::dev_create_swapchain(target,info,out);
}
XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession, std::uint32_t capacity, std::uint32_t* count, std::int64_t* out) {
  const std::int64_t formats[]{VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R8G8B8A8_SRGB};
  *count=4;if(capacity&&capacity<4)return XR_ERROR_SIZE_INSUFFICIENT;
  if(capacity)std::copy(std::begin(formats),std::end(formats),out);return XR_SUCCESS;
}
