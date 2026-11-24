// A 2D app's panel in the headset (panel_window.h): Venture's own host session
// for a process that starts on a 2D panel and never makes one of its own.
//
// One thread owns the session: it attaches when a runtime and headset are there
// (now, or polled later), runs the frame loop, and ends the session when the app
// quits, when the runtime ends it, or when the app opens its own XR session.
// The panel's picture goes in a quad in front of the player; each controller's
// aim ray is drawn as a laser with a cursor where it meets the panel, and its
// trigger and thumbstick become the panel's touch and scroll input, through
// the desktop window's input queue (Panel.pointer, Panel.scroll). The system
// keyboard (system_keyboard.h) goes on top of the panel and takes a ray that
// meets it first.
#include "xr/panel_window.h"
#include "common/log.h"
#include "core/process.h"
#include "gfx/gpu_device.h"
#include "xr/dev_dump.h"
#include "xr/host_xr.h"
#include "xr/math.h"
#include "xr/queue.h"
#include "xr/reference_spaces.h"
#include "xr/session.h"
#include "xr/system_keyboard.h"
#include "xr/vulkan_extensions.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace venture::xr::panel {

// ---- Geometry ---------------------------------------------------------------------
namespace {
constexpr float kBelowEye = 0.1f;  // the panel's centre, below eye level (metres)
XrVector3f add(XrVector3f a, XrVector3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
XrVector3f sub(XrVector3f a, XrVector3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
XrVector3f scale(XrVector3f a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(XrVector3f a, XrVector3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
XrVector3f cross(XrVector3f a, XrVector3f b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
XrVector3f normalized(XrVector3f a) {
  const float l = length(a);
  return l > 1e-6f ? scale(a, 1 / l) : XrVector3f{0, 0, 0};
}
// The rotation whose columns are the orthonormal axes x, y, z.
XrQuaternionf from_axes(XrVector3f x, XrVector3f y, XrVector3f z) {
  const float trace = x.x + y.y + z.z;
  XrQuaternionf q;
  if (trace > 0) {
    const float s = std::sqrt(trace + 1) * 2;
    q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, s / 4};
  } else if (x.x > y.y && x.x > z.z) {
    const float s = std::sqrt(1 + x.x - y.y - z.z) * 2;
    q = {s / 4, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
  } else if (y.y > z.z) {
    const float s = std::sqrt(1 + y.y - x.x - z.z) * 2;
    q = {(y.x + x.y) / s, s / 4, (z.y + y.z) / s, (z.x - x.z) / s};
  } else {
    const float s = std::sqrt(1 + z.z - x.x - y.y) * 2;
    q = {(z.x + x.z) / s, (z.y + y.z) / s, s / 4, (x.y - y.x) / s};
  }
  const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  return {q.x / n, q.y / n, q.z / n, q.w / n};
}
}  // namespace

XrPosef headset_panel_pose(XrPosef head) {
  const auto heading = yaw_only(head.orientation);
  const auto ahead = rotate(heading, {0, 0, -kPanelDistance});
  // Tilted back to face the eyes above its centre: a negative turn about +X
  // lifts its front (+Z), as the system keyboard's panel_pose does.
  const float half = std::atan2(kBelowEye, kPanelDistance) / 2;
  const XrQuaternionf back{-std::sin(half), 0, 0, std::cos(half)};
  return {multiply(heading, back), add(head.position, add(ahead, {0, -kBelowEye, 0}))};
}

std::optional<PanelHit> panel_hit(XrPosef panel, XrExtent2Df size, XrPosef aim, std::uint32_t width,
                                  std::uint32_t height, bool bounded) {
  const auto to_panel = inverse(panel);
  const auto origin = compose(to_panel, aim).position;
  const auto direction = rotate(multiply(to_panel.orientation, aim.orientation), {0, 0, -1});
  // The front faces +Z: the ray must come from in front and travel toward it.
  if (origin.z <= 0 || direction.z >= -1e-6f) return std::nullopt;
  const float t = -origin.z / direction.z;
  const float x = origin.x + direction.x * t, y = origin.y + direction.y * t;
  if (bounded && (std::abs(x) > size.width / 2 || std::abs(y) > size.height / 2)) return std::nullopt;
  return PanelHit{(x / size.width + 0.5f) * float(width), (0.5f - y / size.height) * float(height), t};
}

XrPosef laser_pose(XrPosef aim, float extent, XrVector3f eye) {
  const auto along = normalized(rotate(aim.orientation, {0, 0, -1}));
  const auto centre = add(aim.position, scale(along, extent / 2));
  auto face = sub(eye, centre);
  face = normalized(sub(face, scale(along, dot(face, along))));
  if (length(face) < 0.5f) face = normalized(rotate(aim.orientation, {0, 1, 0}));
  return {from_axes(cross(along, face), along, face), centre};
}

// ---- The headset session ------------------------------------------------------------
namespace {
constexpr auto kRetry = std::chrono::seconds(3);
constexpr float kPress = 0.55f, kRelease = 0.35f;  // trigger, with hysteresis
constexpr float kScrollDeadZone = 0.2f, kScrollRate = 8.f;  // wheel notches per second at full tilt
constexpr std::uint32_t kCursor = 32, kLaserWidth = 16, kLaserLength = 256;
// The space around the panel: an opaque projection layer of one dark colour, as
// a Quest shows its 2D panels in a dim void, rather than whatever a runtime leaves
// behind a frame of quads alone. It also gives each frame the eyes' poses and fields
// of view (development frame dumps).
constexpr std::uint32_t kBackdrop = 16, kBackdropColour = 0xff17181b;
constexpr float kCursorSize = 0.022f, kLaserThickness = 0.004f, kLaserReach = 3.f;  // metres
// Android's MotionEvent actions and sources, as Panel.pointer takes them.
constexpr std::int32_t kDown = 0, kUp = 1, kMove = 2, kHoverMove = 7, kHoverExit = 10;
constexpr std::int32_t kTouchscreen = 0x1002, kMouse = 0x2002;

std::int32_t bits(float value) { return std::bit_cast<std::int32_t>(value); }

struct Control {
  std::mutex mutex;
  std::condition_variable changed;
  bool started = false, released = false, stopping = false, finished = false, attached = false;
};
Control &control() {
  static Control value;
  return value;
}
std::atomic<bool> showing{false};

bool valid(const XrSpaceLocation &location) {
  constexpr auto both = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  return (location.locationFlags & both) == both;
}

// A Vulkan device of the panel's own, made through the runtime, for a process
// whose shared device was made before any runtime was there (a late attach).
struct PrivateDevice {
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
  bool tried = false;
  bool create() {
    if (tried) return binding.device != nullptr;
    tried = true;
    try {
      auto &h = host_xr();
      auto gipa = gfx::gpu_device().get_instance_proc_addr;
      XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
      if (XR_FAILED((host_call<PFN_xrGetVulkanGraphicsRequirements2KHR, "xrGetVulkanGraphicsRequirements2KHR">(
              h.instance, h.system, &requirements))))
        return false;
      std::uint32_t version = VK_API_VERSION_1_1;
      const auto lowest = VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(requirements.minApiVersionSupported),
                                              XR_VERSION_MINOR(requirements.minApiVersionSupported), 0);
      if (version < lowest) version = lowest;
      std::vector<std::string> instance_names, device_names;
      if (h.enabled_extensions.contains("XR_KHR_vulkan_enable")) {
        vulkan_extension_requirements(h.instance, h.system, false, instance_names);
        vulkan_extension_requirements(h.instance, h.system, true, device_names);
      }
      std::vector<const char *> instance_list, device_list;
      for (auto &n : instance_names) instance_list.push_back(n.c_str());
      for (auto &n : device_names) device_list.push_back(n.c_str());
      VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "Venture 2D panel", 1, "Venture", 1, version};
      VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
      ici.pApplicationInfo = &app;
      ici.enabledExtensionCount = std::uint32_t(instance_list.size());
      ici.ppEnabledExtensionNames = instance_list.data();
      XrVulkanInstanceCreateInfoKHR xi{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
      xi.systemId = h.system;
      xi.pfnGetInstanceProcAddr = gipa;
      xi.vulkanCreateInfo = &ici;
      VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
      VkInstance instance{};
      if (XR_FAILED((host_call<PFN_xrCreateVulkanInstanceKHR, "xrCreateVulkanInstanceKHR">(h.instance, &xi, &instance,
                                                                                          &vr))) ||
          vr != VK_SUCCESS)
        return false;
      note_runtime_created_instance(instance);
      VkPhysicalDevice physical{};
      if (XR_FAILED(host_graphics_device(h.system, instance, &physical)) || !physical) return false;
      auto families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
          gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
      std::uint32_t count = 0;
      families(physical, &count, nullptr);
      std::vector<VkQueueFamilyProperties> properties(count);
      families(physical, &count, properties.data());
      std::uint32_t family = count;
      for (std::uint32_t i = 0; i < count && family == count; ++i)
        if (properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) family = i;
      if (family == count) return false;
      const float priority = 1;
      VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, family, 1, &priority};
      VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      dci.queueCreateInfoCount = 1;
      dci.pQueueCreateInfos = &queue;
      dci.enabledExtensionCount = std::uint32_t(device_list.size());
      dci.ppEnabledExtensionNames = device_list.data();
      XrVulkanDeviceCreateInfoKHR xd{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
      xd.systemId = h.system;
      xd.pfnGetInstanceProcAddr = gipa;
      xd.vulkanPhysicalDevice = physical;
      xd.vulkanCreateInfo = &dci;
      VkDevice device{};
      vr = VK_ERROR_INITIALIZATION_FAILED;
      if (XR_FAILED((host_call<PFN_xrCreateVulkanDeviceKHR, "xrCreateVulkanDeviceKHR">(h.instance, &xd, &device, &vr))) ||
          vr != VK_SUCCESS)
        return false;
      binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, nullptr, instance, physical, device, family, 0};
      log::info("panel", "The headset view has a Vulkan device of its own (the shared one predates the runtime)");
      return true;
    } catch (const std::exception &e) {
      log::warn("panel", "No Vulkan device for the headset view: {}", e.what());
      return false;
    }
  }
};

struct Image {
  XrSwapchain chain{};
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  std::vector<XrSwapchainImageVulkanKHR> images;
  std::uint32_t index = 0;
  bool acquired = false, waited = false, content = false;
};

struct Hand {
  XrPath path{};
  XrSpace aim{};
  std::optional<XrPosef> pose;  // the aim pose this frame, in `local_`
  float trigger = 0;
  XrVector2f stick{};
  bool down = true;  // a trigger held as the session starts must release before it presses
  bool touching = false, hovering = false;
  std::optional<PanelHit> at;  // where its ray meets the panel
  float ray = kLaserReach;     // how long its laser is drawn
};

class Headset {
 public:
  void run();

 private:
  bool attach();
  void detach();
  bool create_input();
  void pump();
  void frame();
  void place(XrTime time);
  void input(XrTime time);
  bool upload();
  bool create_image(Image &, std::uint32_t width, std::uint32_t height);
  bool ready(Image &);
  void record_copy(Image &, VkDeviceSize offset);
  bool create_gpu(VkDeviceSize bytes);
  void destroy_gpu();
  template <class F> F vk(const char *name) {
    return reinterpret_cast<F>(gfx::gpu_device().get_instance_proc_addr(binding_.instance, name));
  }
  bool stop_requested();
  bool wait_retry();

  PrivateDevice private_;
  XrSession handle_{};
  XrGraphicsBindingVulkanKHR binding_{};
  XrSpace local_{}, view_{};
  XrActionSet set_{};
  XrAction aim_{}, select_{}, scroll_{};
  std::array<Hand, 2> hands_;
  int active_ = 1;  // the hand that hovers: the last that pressed
  float hover_x_ = -1, hover_y_ = -1;  // where it last hovered
  XrEnvironmentBlendMode blend_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  bool exited_ = false, lost_ = false, placed_ = false, shown_logged_ = false, failed_ = false;
  bool unavailable_logged_ = false;
  XrPosef pose_{{0, 0, 0, 1}, {0, 0, 0}};
  XrExtent2Df size_{};
  XrVector3f eye_{};
  std::int64_t last_input_ns_ = 0;
  // Pictures and their upload.
  Image panel_, laser_, cursor_, backdrop_;
  std::uint64_t generation_ = 0;
  bool dirty_ = false;  // pixels_ is newer than the panel swapchain's picture
  std::vector<std::uint8_t> pixels_;
  int width_ = 0, height_ = 0;
  VkCommandPool pool_{};
  VkCommandBuffer command_{};
  VkFence fence_{};
  bool in_flight_ = false;
  VkBuffer staging_{};
  VkDeviceMemory memory_{};
  void *mapped_ = nullptr;
  bool coherent_ = false;
  VkDeviceSize capacity_ = 0;
};

bool Headset::stop_requested() {
  auto &c = control();
  std::lock_guard lock(c.mutex);
  return c.stopping || c.released;
}
bool Headset::wait_retry() {
  auto &c = control();
  std::unique_lock lock(c.mutex);
  return !c.changed.wait_for(lock, kRetry, [&] { return c.stopping || c.released; });
}

bool Headset::attach() {
  if (close_requested() || exited_ || failed_) return false;
  if (!host_xr().available && !retry_host_xr()) return false;
  const auto [width, height] = frame_size();
  if (width <= 0 || height <= 0) return false;
  std::optional<XrGraphicsBindingVulkanKHR> binding;
  // The shared device serves the session when the runtime made it, as for a game.
  if (!gfx::gpu_device_through_xr()) {
    if (!private_.create()) {
      if (!unavailable_logged_) log::warn("panel", "No Vulkan device for the headset view; the panel stays on the desktop");
      unavailable_logged_ = true;
      return false;
    }
    binding = private_.binding;
  }
  {
    auto &s = session();
    std::lock_guard lock(s.mutex);
    if (s.handle) {  // the app's own session came first
      std::lock_guard control_lock(control().mutex);
      control().released = true;
      return false;
    }
    const auto result = binding ? s.create(SessionOwner::panel, &*binding) : s.create(SessionOwner::panel);
    if (XR_FAILED(result)) {
      if (!unavailable_logged_) log::warn("panel", "The headset view could not start (XrResult {}); retrying", int(result));
      unavailable_logged_ = true;
      return false;
    }
    handle_ = s.handle;
    binding_ = s.binding;
  }
  {
    std::lock_guard lock(control().mutex);
    control().attached = true;
  }
  exited_ = lost_ = placed_ = false;
  auto &h = host_xr();
  std::uint32_t count = 0;
  blend_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  if (XR_SUCCEEDED(::xrEnumerateEnvironmentBlendModes(h.instance, h.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                      0, &count, nullptr)) && count) {
    std::vector<XrEnvironmentBlendMode> modes(count);
    if (XR_SUCCEEDED(::xrEnumerateEnvironmentBlendModes(h.instance, h.system,
                                                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count,
                                                        modes.data())) && count)
      blend_ = modes.front();  // the runtime's preferred mode
  }
  XrReferenceSpaceCreateInfo view{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_VIEW,
                                  {{0, 0, 0, 1}, {0, 0, 0}}};
  auto local = upright_reference_space(handle_, {XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr,
                                                 XR_REFERENCE_SPACE_TYPE_LOCAL, {{0, 0, 0, 1}, {0, 0, 0}}});
  if (XR_FAILED(::xrCreateReferenceSpace(handle_, &view, &view_)) ||
      XR_FAILED(::xrCreateReferenceSpace(handle_, &local, &local_))) {
    log::warn("panel", "The headset view has no reference spaces; the panel stays on the desktop");
    failed_ = true;
    detach();
    return false;
  }
  if (!create_input()) log::warn("panel", "No controller input for the headset view; the panel shows without a laser");
  log::info("panel", "Host session for the 2D app's panel created ({} runtime)", h.runtime_name);
  return true;
}

bool Headset::create_input() {
  auto &h = host_xr();
  XrActionSetCreateInfo set{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strcpy(set.actionSetName, "venture_panel");
  std::strcpy(set.localizedActionSetName, "Venture panel");
  if (XR_FAILED(::xrCreateActionSet(h.instance, &set, &set_))) return false;
  std::array<XrPath, 2> paths{};
  if (XR_FAILED(::xrStringToPath(h.instance, "/user/hand/left", &paths[0])) ||
      XR_FAILED(::xrStringToPath(h.instance, "/user/hand/right", &paths[1])))
    return false;
  for (int i = 0; i < 2; ++i) hands_[std::size_t(i)].path = paths[std::size_t(i)];
  auto action = [&](const char *name, const char *label, XrActionType type, XrAction &out) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    std::strcpy(info.actionName, name);
    std::strcpy(info.localizedActionName, label);
    info.actionType = type;
    info.countSubactionPaths = 2;
    info.subactionPaths = paths.data();
    return XR_SUCCEEDED(::xrCreateAction(set_, &info, &out));
  };
  if (!action("aim", "Point", XR_ACTION_TYPE_POSE_INPUT, aim_) ||
      !action("select", "Select", XR_ACTION_TYPE_FLOAT_INPUT, select_) ||
      !action("scroll", "Scroll", XR_ACTION_TYPE_VECTOR2F_INPUT, scroll_))
    return false;
  // Standard controller profiles of the core specification; a runtime maps the
  // ones it knows onto its controllers.
  struct Profile { const char *name, *select, *scroll; };
  constexpr Profile profiles[] = {
      {"/interaction_profiles/khr/simple_controller", "/input/select/click", nullptr},
      {"/interaction_profiles/oculus/touch_controller", "/input/trigger/value", "/input/thumbstick"},
      {"/interaction_profiles/valve/index_controller", "/input/trigger/value", "/input/thumbstick"},
      {"/interaction_profiles/htc/vive_controller", "/input/trigger/value", "/input/trackpad"},
      {"/interaction_profiles/microsoft/motion_controller", "/input/trigger/value", "/input/thumbstick"},
  };
  int suggested = 0;
  for (const auto &profile : profiles) {
    std::vector<XrActionSuggestedBinding> bindings;
    bool ok = true;
    for (const char *side : {"/user/hand/left", "/user/hand/right"}) {
      auto bind = [&](XrAction action, const char *input) {
        XrPath path{};
        if (XR_FAILED(::xrStringToPath(h.instance, (std::string(side) + input).c_str(), &path))) ok = false;
        bindings.push_back({action, path});
      };
      bind(aim_, "/input/aim/pose");
      bind(select_, profile.select);
      if (profile.scroll) bind(scroll_, profile.scroll);
    }
    XrPath name{};
    if (!ok || XR_FAILED(::xrStringToPath(h.instance, profile.name, &name))) continue;
    XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggestion.interactionProfile = name;
    suggestion.countSuggestedBindings = std::uint32_t(bindings.size());
    suggestion.suggestedBindings = bindings.data();
    if (XR_SUCCEEDED(::xrSuggestInteractionProfileBindings(h.instance, &suggestion))) ++suggested;
  }
  if (!suggested) return false;
  XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &set_};
  if (XR_FAILED(::xrAttachSessionActionSets(handle_, &attach))) return false;
  for (auto &hand : hands_) {
    XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO, nullptr, aim_, hand.path, {{0, 0, 0, 1}, {0, 0, 0}}};
    if (XR_FAILED(::xrCreateActionSpace(handle_, &space, &hand.aim))) return false;
  }
  return true;
}

void Headset::destroy_gpu() {
  if (binding_.device) {
    if (in_flight_) vk<PFN_vkWaitForFences>("vkWaitForFences")(binding_.device, 1, &fence_, VK_TRUE, 1'000'000'000);
    if (mapped_) vk<PFN_vkUnmapMemory>("vkUnmapMemory")(binding_.device, memory_);
    if (staging_) vk<PFN_vkDestroyBuffer>("vkDestroyBuffer")(binding_.device, staging_, nullptr);
    if (memory_) vk<PFN_vkFreeMemory>("vkFreeMemory")(binding_.device, memory_, nullptr);
    if (fence_) vk<PFN_vkDestroyFence>("vkDestroyFence")(binding_.device, fence_, nullptr);
    if (pool_) vk<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(binding_.device, pool_, nullptr);
  }
  pool_ = {};
  command_ = {};
  fence_ = {};
  staging_ = {};
  memory_ = {};
  mapped_ = nullptr;
  in_flight_ = false;
  capacity_ = 0;
}

void Headset::detach() {
  if (!handle_) return;
  showing = false;
  // Pending events of this session would only describe its end.
  auto &s = session();
  {
    std::lock_guard lock(s.mutex);
    {
      HostQueueLock queue(s.uses_shared_gpu);
      destroy_gpu();
      for (auto *image : {&panel_, &laser_, &cursor_, &backdrop_}) {
        if (image->chain) {
          if (image->acquired) {
            if (!image->waited) {
              XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
              ::xrWaitSwapchainImage(image->chain, &wait);
            }
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            ::xrReleaseSwapchainImage(image->chain, &release);
          }
          dev_destroy_swapchain(image->chain);
        }
        *image = {};
      }
    }
    for (auto &hand : hands_) {
      if (hand.aim) ::xrDestroySpace(hand.aim);
      hand = {};
    }
    if (set_) ::xrDestroyActionSet(set_);
    set_ = {};
    aim_ = select_ = scroll_ = {};
    if (local_) ::xrDestroySpace(local_);
    if (view_) ::xrDestroySpace(view_);
    local_ = view_ = {};
    if (s.handle == handle_ && s.owner == SessionOwner::panel) {
      if (s.running) s.end_session();
      s.destroy(SessionOwner::panel);
    }
  }
  handle_ = {};
  generation_ = 0;
  dirty_ = false;
  shown_logged_ = false;
  log::info("panel", "Host session for the 2D app's panel ended");
  {
    std::lock_guard lock(control().mutex);
    control().attached = false;
  }
  control().changed.notify_all();
}

void Headset::pump() {
  auto &h = host_xr();
  auto &s = session();
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (::xrPollEvent(h.instance, &event) == XR_SUCCESS) {
    std::lock_guard lock(s.mutex);
    // A host EXITING the app did not ask for (a quit from the runtime's
    // dashboard) records host_ended, which quits the app as a removed task.
    s.deliver_host_event(event);
    s.observe_host_event(event);
    if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      const auto &changed = reinterpret_cast<const XrEventDataSessionStateChanged &>(event);
      if (changed.session == handle_) {
        s.state = changed.state;
        log::info("panel", "Host session state={}", int(changed.state));
        switch (changed.state) {
          case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
            if (XR_FAILED(s.begin_session(&begin))) lost_ = true;
            placed_ = false;  // opens in front of the player, wherever they look now
            break;
          }
          case XR_SESSION_STATE_STOPPING:
            showing = false;
            if (s.running) s.end_session();
            break;
          case XR_SESSION_STATE_EXITING: exited_ = true; break;
          case XR_SESSION_STATE_LOSS_PENDING: lost_ = true; break;
          default: break;
        }
      }
    } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
      // A recenter: the panel comes back in front of the player.
      const auto &changed = reinterpret_cast<const XrEventDataReferenceSpaceChangePending &>(event);
      if (changed.session == handle_ && changed.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) placed_ = false;
    } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
      lost_ = exited_ = true;
    }
    event = {XR_TYPE_EVENT_DATA_BUFFER};
  }
}

void Headset::place(XrTime time) {
  XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(::xrLocateSpace(view_, local_, time, &head)) || !valid(head)) return;
  eye_ = head.pose.position;
  if (placed_) return;
  pose_ = headset_panel_pose(head.pose);
  placed_ = true;
}

void Headset::input(XrTime time) {
  std::vector<std::array<std::int32_t, 6>> events;
  const auto [width, height] = frame_size();
  const bool keyboard = keyboard::shown();
  bool synced = false;
  if (set_) {
    XrActiveActionSet active{set_, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
    synced = ::xrSyncActions(handle_, &sync) == XR_SUCCESS;
  }
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const float dt = last_input_ns_ ? std::clamp(float(now - last_input_ns_) * 1e-9f, 0.f, 0.1f) : 0.f;
  last_input_ns_ = now;
  for (int i = 0; i < 2; ++i) {
    auto &hand = hands_[std::size_t(i)];
    hand.pose.reset();
    hand.at.reset();
    hand.ray = kLaserReach;
    hand.trigger = 0;
    hand.stick = {};
    if (synced && hand.aim) {
      XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, aim_, hand.path};
      XrActionStatePose pose{XR_TYPE_ACTION_STATE_POSE};
      XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
      if (XR_SUCCEEDED(::xrGetActionStatePose(handle_, &get, &pose)) && pose.isActive &&
          XR_SUCCEEDED(::xrLocateSpace(hand.aim, local_, time, &location)) && valid(location))
        hand.pose = location.pose;
      XrActionStateFloat select{XR_TYPE_ACTION_STATE_FLOAT};
      get.action = select_;
      if (XR_SUCCEEDED(::xrGetActionStateFloat(handle_, &get, &select)) && select.isActive)
        hand.trigger = select.currentState;
      XrActionStateVector2f scroll{XR_TYPE_ACTION_STATE_VECTOR2F};
      get.action = scroll_;
      if (XR_SUCCEEDED(::xrGetActionStateVector2f(handle_, &get, &scroll)) && scroll.isActive)
        hand.stick = scroll.currentState;
    }
    // The keyboard first: a ray that meets it does not reach the panel behind.
    std::optional<float> on_keyboard;
    if (keyboard) {
      keyboard::point(i, hand.pose ? hand.aim : XrSpace{}, hand.trigger, time);
      on_keyboard = keyboard::hit_distance(i);
    }
    if (hand.pose && width > 0 && height > 0) {
      hand.at = panel_hit(pose_, size_, *hand.pose, std::uint32_t(width), std::uint32_t(height));
      if (hand.at && on_keyboard && *on_keyboard < hand.at->distance) hand.at.reset();
    }
    if (on_keyboard) hand.ray = *on_keyboard;
    else if (hand.at) hand.ray = hand.at->distance;
    const bool down = hand.down ? hand.trigger > kRelease : hand.trigger > kPress;
    if (down && !hand.down && hand.at && !hands_[std::size_t(1 - i)].touching) {
      active_ = i;
      if (hands_[std::size_t(i)].hovering || hands_[std::size_t(1 - i)].hovering)
        events.push_back({pointer, kHoverExit, bits(hand.at->x), bits(hand.at->y), 0, kMouse});
      for (auto &h : hands_) h.hovering = false;
      hand.touching = true;
      events.push_back({pointer, kDown, bits(hand.at->x), bits(hand.at->y), 0, kTouchscreen});
    } else if (hand.touching && down) {
      // A drag keeps following the ray across the panel's plane, past its edges.
      std::optional<PanelHit> plane;
      if (hand.pose)
        plane = panel_hit(pose_, size_, *hand.pose, std::uint32_t(width), std::uint32_t(height), false);
      if (plane) events.push_back({pointer, kMove, bits(plane->x), bits(plane->y), 0, kTouchscreen});
    } else if (hand.touching && !down) {
      std::optional<PanelHit> plane;
      if (hand.pose)
        plane = panel_hit(pose_, size_, *hand.pose, std::uint32_t(width), std::uint32_t(height), false);
      const float x = plane ? plane->x : 0, y = plane ? plane->y : 0;
      events.push_back({pointer, kUp, bits(x), bits(y), 0, kTouchscreen});
      hand.touching = false;
    }
    hand.down = down;
    // The thumbstick scrolls what the ray points at, as a mouse wheel does.
    if (hand.at && !hand.touching && dt > 0 &&
        (std::abs(hand.stick.y) > kScrollDeadZone || std::abs(hand.stick.x) > kScrollDeadZone)) {
      auto amount = [&](float v) { return std::abs(v) > kScrollDeadZone ? v * kScrollRate * dt : 0.f; };
      events.push_back({scroll, bits(hand.at->x), bits(hand.at->y), bits(amount(hand.stick.x)),
                        bits(amount(hand.stick.y)), 0});
    }
  }
  // Hover follows the hand last used, as a mouse would.
  if (!hands_[0].touching && !hands_[1].touching) {
    auto &hand = hands_[std::size_t(active_)];
    if (!hand.at) {
      auto &other = hands_[std::size_t(1 - active_)];
      if (other.at) active_ = 1 - active_;
    }
    auto &hovering = hands_[std::size_t(active_)];
    if (hovering.at) {
      // Only a move is an event: a still controller sends nothing.
      const bool moved = !hovering.hovering || std::abs(hovering.at->x - hover_x_) > 0.25f ||
                         std::abs(hovering.at->y - hover_y_) > 0.25f;
      hovering.hovering = true;
      if (moved) {
        hover_x_ = hovering.at->x;
        hover_y_ = hovering.at->y;
        events.push_back({pointer, kHoverMove, bits(hover_x_), bits(hover_y_), 0, kMouse});
      }
    } else if (hands_[0].hovering || hands_[1].hovering) {
      events.push_back({pointer, kHoverExit, 0, 0, 0, kMouse});
      hands_[0].hovering = hands_[1].hovering = false;
    }
  }
  post_events(events);
}

bool Headset::create_gpu(VkDeviceSize bytes) {
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = binding_.queueFamilyIndex;
  auto device = binding_.device;
  if (!pool_) {
    if (vk<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device, &pci, nullptr, &pool_) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool_,
                                    VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vk<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &cai, &command_) != VK_SUCCESS)
      return false;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vk<PFN_vkCreateFence>("vkCreateFence")(device, &fci, nullptr, &fence_) != VK_SUCCESS) return false;
  }
  if (capacity_ >= bytes) return true;
  if (in_flight_) {
    vk<PFN_vkWaitForFences>("vkWaitForFences")(device, 1, &fence_, VK_TRUE, 1'000'000'000);
    in_flight_ = false;
  }
  if (mapped_) vk<PFN_vkUnmapMemory>("vkUnmapMemory")(device, memory_);
  if (staging_) vk<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, staging_, nullptr);
  if (memory_) vk<PFN_vkFreeMemory>("vkFreeMemory")(device, memory_, nullptr);
  mapped_ = nullptr;
  staging_ = {};
  memory_ = {};
  capacity_ = 0;
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = bytes;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (vk<PFN_vkCreateBuffer>("vkCreateBuffer")(device, &bci, nullptr, &staging_) != VK_SUCCESS) return false;
  VkMemoryRequirements requirements{};
  vk<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device, staging_, &requirements);
  VkPhysicalDeviceMemoryProperties properties{};
  vk<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(binding_.physicalDevice,
                                                                                    &properties);
  std::uint32_t type = properties.memoryTypeCount;
  for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
    const auto flags = properties.memoryTypes[i].propertyFlags;
    if ((requirements.memoryTypeBits & (1u << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
      type = i;
      if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
    }
  }
  if (type == properties.memoryTypeCount) return false;
  coherent_ = properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
  if (vk<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &mai, nullptr, &memory_) != VK_SUCCESS ||
      vk<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, staging_, memory_, 0) != VK_SUCCESS ||
      vk<PFN_vkMapMemory>("vkMapMemory")(device, memory_, 0, VK_WHOLE_SIZE, 0, &mapped_) != VK_SUCCESS)
    return false;
  capacity_ = bytes;
  return true;
}

bool Headset::create_image(Image &image, std::uint32_t width, std::uint32_t height) {
  image.info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
  image.info.usageFlags =
      XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  // The picture's bytes are sRGB: an sRGB image takes them unchanged.
  image.info.format = VK_FORMAT_R8G8B8A8_SRGB;
  image.info.sampleCount = image.info.faceCount = image.info.arraySize = image.info.mipCount = 1;
  image.info.width = width;
  image.info.height = height;
  XrResult result;
  {
    HostQueueLock queue(session().uses_shared_gpu);
    if (!queue) return false;
    result = create_host_swapchain(handle_, image.info, &image.chain);
  }
  if (XR_FAILED(result)) return false;
  std::uint32_t count = 0;
  if (XR_FAILED(::xrEnumerateSwapchainImages(image.chain, 0, &count, nullptr)) || !count || count > 32) return false;
  image.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  return XR_SUCCEEDED(::xrEnumerateSwapchainImages(
      image.chain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader *>(image.images.data())));
}

bool Headset::ready(Image &image) {
  if (!image.acquired) {
    HostQueueLock queue(session().uses_shared_gpu);
    if (!queue || XR_FAILED(::xrAcquireSwapchainImage(image.chain, nullptr, &image.index)) ||
        image.index >= image.images.size())
      return false;
    image.acquired = true;
    image.waited = false;
  }
  if (!image.waited) {
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, 0};
    if (::xrWaitSwapchainImage(image.chain, &wait) != XR_SUCCESS) return false;
    image.waited = true;
  }
  return true;
}

void Headset::record_copy(Image &image, VkDeviceSize offset) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // the whole image is written
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image.images[image.index].image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                                       &barrier);
  VkBufferImageCopy copy{};
  copy.bufferOffset = offset;
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {image.info.width, image.info.height, 1};
  vk<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(command_, staging_, barrier.image,
                                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  // OpenXR: a released Vulkan color image is in COLOR_ATTACHMENT_OPTIMAL.
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                                                       nullptr, 1, &barrier);
}

// The laser: white, fading toward its far end (row 0, the quad's +Y edge),
// soft at its sides. The cursor: a white dot with a dark rim. Premultiplied ARGB.
std::vector<std::uint32_t> laser_pixels() {
  std::vector<std::uint32_t> out(kLaserWidth * kLaserLength);
  for (std::uint32_t y = 0; y < kLaserLength; ++y)
    for (std::uint32_t x = 0; x < kLaserWidth; ++x) {
      const float across = 1 - std::abs((float(x) + 0.5f) / kLaserWidth * 2 - 1);
      const float along = 0.15f + 0.7f * float(y) / float(kLaserLength - 1);
      const auto a = std::uint32_t(std::lround(std::clamp(across * 1.6f, 0.f, 1.f) * along * 255));
      out[y * kLaserWidth + x] = (a << 24) | (a << 16) | (a << 8) | a;
    }
  return out;
}
std::vector<std::uint32_t> cursor_pixels() {
  std::vector<std::uint32_t> out(kCursor * kCursor);
  const float c = (kCursor - 1) / 2.f;
  for (std::uint32_t y = 0; y < kCursor; ++y)
    for (std::uint32_t x = 0; x < kCursor; ++x) {
      const float d = std::hypot(x - c, y - c);
      const float alpha = std::clamp(c - d + 0.5f, 0.f, 1.f);
      const float white = std::clamp(c - 4.f - d + 0.5f, 0.f, 1.f);
      const auto a = std::uint32_t(std::lround(alpha * 255)), v = std::uint32_t(std::lround(white * alpha * 255));
      out[y * kCursor + x] = (a << 24) | (v << 16) | (v << 8) | v;
    }
  return out;
}

// Uploads a new panel picture (and the laser and cursor once). Never waits on
// the GPU: a copy still in flight leaves the new picture for a later frame.
bool Headset::upload() {
  {
    int width = 0, height = 0;
    std::vector<std::uint8_t> fresh;
    if (const auto generation = copy_frame(generation_, fresh, width, height)) {
      pixels_ = std::move(fresh);
      width_ = width;
      height_ = height;
      generation_ = generation;
      dirty_ = true;
    }
  }
  if (!dirty_ && laser_.content && cursor_.content && backdrop_.content) return true;
  if (in_flight_) {
    if (vk<PFN_vkGetFenceStatus>("vkGetFenceStatus")(binding_.device, fence_) != VK_SUCCESS) return true;
    in_flight_ = false;
  }
  if (width_ <= 0 || height_ <= 0 || pixels_.size() != std::size_t(width_) * std::size_t(height_) * 4) return true;
  if (panel_.chain && (panel_.info.width != std::uint32_t(width_) || panel_.info.height != std::uint32_t(height_))) {
    HostQueueLock queue(session().uses_shared_gpu);
    if (panel_.acquired) {
      XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      ::xrReleaseSwapchainImage(panel_.chain, &release);
    }
    dev_destroy_swapchain(panel_.chain);
    panel_ = {};
  }
  const VkDeviceSize panel_bytes = VkDeviceSize(width_) * VkDeviceSize(height_) * 4;
  const VkDeviceSize laser_offset = panel_bytes, cursor_offset = laser_offset + kLaserWidth * kLaserLength * 4;
  const VkDeviceSize backdrop_offset = cursor_offset + kCursor * kCursor * 4;
  const VkDeviceSize bytes = backdrop_offset + kBackdrop * kBackdrop * 4;
  if (!create_gpu(bytes)) return false;
  if (!panel_.chain && !create_image(panel_, std::uint32_t(width_), std::uint32_t(height_))) return false;
  if (!laser_.chain && !create_image(laser_, kLaserWidth, kLaserLength)) return false;
  if (!cursor_.chain && !create_image(cursor_, kCursor, kCursor)) return false;
  if (!backdrop_.chain && !create_image(backdrop_, kBackdrop, kBackdrop)) return false;
  const bool panel = (dirty_ || !panel_.content) && ready(panel_);
  const bool laser = !laser_.content && ready(laser_);
  const bool cursor = !cursor_.content && ready(cursor_);
  const bool backdrop = !backdrop_.content && ready(backdrop_);
  if (!panel && !laser && !cursor && !backdrop) return true;
  auto *staging = static_cast<std::uint8_t *>(mapped_);
  if (panel) {
    const auto format = panel_.info.format;
    if (format == VK_FORMAT_R8G8B8A8_SRGB) {
      std::memcpy(staging, pixels_.data(), pixels_.size());
    } else if (format == VK_FORMAT_B8G8R8A8_SRGB) {
      for (std::size_t i = 0; i < pixels_.size(); i += 4) {
        staging[i] = pixels_[i + 2];
        staging[i + 1] = pixels_[i + 1];
        staging[i + 2] = pixels_[i];
        staging[i + 3] = pixels_[i + 3];
      }
    } else {  // a linear image: through the keyboard's conversion
      std::vector<std::uint32_t> argb(pixels_.size() / 4);
      for (std::size_t i = 0; i < argb.size(); ++i) {
        const auto *p = &pixels_[i * 4];
        argb[i] = (std::uint32_t(p[3]) << 24) | (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2];
      }
      keyboard::convert(argb.data(), argb.size(), format, staging);
    }
  }
  if (laser) {
    const auto pixels = laser_pixels();
    keyboard::convert(pixels.data(), pixels.size(), laser_.info.format, staging + laser_offset);
  }
  if (cursor) {
    const auto pixels = cursor_pixels();
    keyboard::convert(pixels.data(), pixels.size(), cursor_.info.format, staging + cursor_offset);
  }
  if (backdrop) {
    const std::vector<std::uint32_t> pixels(kBackdrop * kBackdrop, kBackdropColour);
    keyboard::convert(pixels.data(), pixels.size(), backdrop_.info.format, staging + backdrop_offset);
  }
  if (!coherent_) {
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, memory_, 0, VK_WHOLE_SIZE};
    vk<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(binding_.device, 1, &range);
  }
  HostQueueLock queue(session().uses_shared_gpu);
  if (!queue) return false;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vk<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(command_, 0) != VK_SUCCESS ||
      vk<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command_, &begin) != VK_SUCCESS)
    return false;
  if (panel) record_copy(panel_, 0);
  if (laser) record_copy(laser_, laser_offset);
  if (cursor) record_copy(cursor_, cursor_offset);
  if (backdrop) record_copy(backdrop_, backdrop_offset);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_;
  if (vk<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command_) != VK_SUCCESS ||
      vk<PFN_vkResetFences>("vkResetFences")(binding_.device, 1, &fence_) != VK_SUCCESS ||
      vk<PFN_vkQueueSubmit>("vkQueueSubmit")(session().queue, 1, &submit, fence_) != VK_SUCCESS)
    return false;
  in_flight_ = true;
  // The copy is queued ahead of the release on the binding's queue, as OpenXR requires.
  for (auto *image : {panel ? &panel_ : nullptr, laser ? &laser_ : nullptr, cursor ? &cursor_ : nullptr,
                      backdrop ? &backdrop_ : nullptr}) {
    if (!image) continue;
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_FAILED(dev_release_swapchain(image->chain, &release, image->images[image->index].image))) return false;
    image->acquired = image->waited = false;
    image->content = true;
  }
  if (panel) dirty_ = false;
  return true;
}

void Headset::frame() {
  pump();
  auto &s = session();
  if (exited_ || lost_) {
    detach();
    return;
  }
  bool running;
  {
    std::lock_guard lock(s.mutex);
    running = s.running && XR_SUCCEEDED(s.lost);
  }
  if (!running) {
    auto &c = control();
    std::unique_lock lock(c.mutex);
    c.changed.wait_for(lock, std::chrono::milliseconds(20), [&] { return c.stopping || c.released; });
    return;
  }
  XrFrameState state{XR_TYPE_FRAME_STATE};
  auto result = ::xrWaitFrame(handle_, nullptr, &state);
  if (XR_FAILED(result)) {
    if (result == XR_ERROR_SESSION_LOST || result == XR_ERROR_INSTANCE_LOST) lost_ = true;
    else std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return;
  }
  std::lock_guard lock(s.mutex);
  {
    HostQueueLock queue(s.uses_shared_gpu);
    if (!queue) return;
    result = ::xrBeginFrame(handle_, nullptr);
  }
  if (XR_FAILED(result)) return;
  std::vector<const XrCompositionLayerBaseHeader *> layers;
  std::array<XrCompositionLayerQuad, 5> quads{};
  std::size_t used = 0;
  XrCompositionLayerProjection backdrop{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  std::array<XrCompositionLayerProjectionView, 2> backdrop_views{};
  if (state.shouldRender) {
    place(state.predictedDisplayTime);
    if (!upload()) {
      log::warn("panel", "The panel picture could not be uploaded; the headset view ends");
      failed_ = true;
    }
    if (placed_ && panel_.content) {
      size_ = {kPanelWidth, kPanelWidth * float(panel_.info.height) / float(panel_.info.width)};
      input(state.predictedDisplayTime);
      auto quad = [&](const Image &image, XrPosef at, XrExtent2Df extent) {
        XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
        layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // premultiplied pictures
        layer.space = local_;
        layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        layer.subImage = {image.chain,
                          {{0, 0}, {std::int32_t(image.info.width), std::int32_t(image.info.height)}}, 0};
        layer.pose = at;
        layer.size = extent;
        return layer;
      };
      // The void behind the panel, from where the eyes are this frame.
      std::array<XrView, 2> views{XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
      XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                              state.predictedDisplayTime, local_};
      XrViewState view_state{XR_TYPE_VIEW_STATE};
      std::uint32_t located = 0;
      if (backdrop_.content &&
          XR_SUCCEEDED(::xrLocateViews(handle_, &locate, &view_state, 2, &located, views.data())) && located == 2) {
        for (std::size_t eye = 0; eye < 2; ++eye) {
          backdrop_views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
          backdrop_views[eye].pose = views[eye].pose;
          backdrop_views[eye].fov = views[eye].fov;
          backdrop_views[eye].subImage = {backdrop_.chain, {{0, 0}, {std::int32_t(kBackdrop), std::int32_t(kBackdrop)}}, 0};
        }
        backdrop.space = local_;
        backdrop.viewCount = 2;
        backdrop.views = backdrop_views.data();
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&backdrop));
      }
      quads[used++] = quad(panel_, pose_, size_);
      for (const auto &hand : hands_) {
        if (!hand.pose) continue;
        if (laser_.content && used < quads.size())
          quads[used++] = quad(laser_, laser_pose(*hand.pose, hand.ray, eye_), {kLaserThickness, hand.ray});
        if (hand.at && cursor_.content && used < quads.size()) {
          const float meters = size_.width / float(panel_.info.width);
          const XrPosef offset{{0, 0, 0, 1},
                               {(hand.at->x - float(panel_.info.width) / 2) * meters,
                                (float(panel_.info.height) / 2 - hand.at->y) * meters, 0.002f}};
          quads[used++] = quad(cursor_, compose(pose_, offset), {kCursorSize, kCursorSize});
        }
      }
      for (std::size_t i = 0; i < used; ++i)
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quads[i]));
    }
  }
  XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
  info.displayTime = state.predictedDisplayTime;
  info.environmentBlendMode = blend_;
  info.layerCount = std::uint32_t(layers.size());
  info.layers = layers.data();
  // The system keyboard goes on top of the panel, as it does over a game.
  std::shared_ptr<const keyboard::Frame> keyboard_frame;
  if (used && keyboard::shown()) {
    keyboard_frame = keyboard::extend(handle_, info, nullptr);
    if (keyboard_frame) info = keyboard_frame->info;
  }
  {
    HostQueueLock queue(s.uses_shared_gpu);
    if (!queue) return;
    result = ::xrEndFrame(handle_, &info);
  }
  dev_dump_frame(&info, result);
  if (XR_FAILED(result)) {
    if (result == XR_ERROR_SESSION_LOST) lost_ = true;
    return;
  }
  const bool visible = used && (s.state == XR_SESSION_STATE_VISIBLE || s.state == XR_SESSION_STATE_FOCUSED);
  showing = visible;
  if (visible && !shown_logged_) {
    shown_logged_ = true;
    log::info("panel", "The 2D app's panel is shown in the headset ({}x{} px, {:.2f} x {:.2f} m at {:.1f} m)",
              panel_.info.width, panel_.info.height, size_.width, size_.height, kPanelDistance);
  }
}

void Headset::run() {
  log::info("panel", "The 2D app's panel looks for a headset");
  while (!stop_requested()) {
    if (!handle_) {
      if (exited_ || failed_ || close_requested()) {
        // Nothing more to show: wait for the process to end.
        auto &c = control();
        std::unique_lock lock(c.mutex);
        c.changed.wait(lock, [&] { return c.stopping || c.released; });
        break;
      }
      if (!attach() && !stop_requested()) wait_retry();
      continue;
    }
    frame();
  }
  detach();
  std::lock_guard lock(control().mutex);
  control().finished = true;
  control().changed.notify_all();
}
}  // namespace

void start_headset() {
  const auto &config = process_config();
  if (!config.launches_2d || !config.starts_on_panel) return;
  {
    auto &c = control();
    std::lock_guard lock(c.mutex);
    if (c.started || c.released || c.stopping) return;
    c.started = true;
  }
  std::thread([] {
    Headset headset;
    headset.run();
  }).detach();
}

void release_headset() {
  auto &c = control();
  std::unique_lock lock(c.mutex);
  if (c.released) return;
  c.released = true;
  c.changed.notify_all();
  if (!c.started) return;
  log::info("panel", "The app opens its own XR session; the panel's headset view ends");
  c.changed.wait_for(lock, std::chrono::seconds(3), [&] { return c.finished; });
}

void stop_headset() {
  auto &c = control();
  std::unique_lock lock(c.mutex);
  c.stopping = true;
  c.changed.notify_all();
  if (c.started) c.changed.wait_for(lock, std::chrono::seconds(3), [&] { return c.finished; });
}

bool in_headset() { return showing.load(std::memory_order_relaxed); }
}  // namespace venture::xr::panel
