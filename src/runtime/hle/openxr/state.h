#pragma once
#include "hle/openxr/openxr.h"
#include "xr/session.h"
#include "xr/sampler.h"
#include "xr/xr_images.h"
#include "xr/color_images.h"
#include "xr/guest_images.h"
#include "hle/vulkan/full_rate_image.h"
#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace venture::hle::openxr {
using namespace venture::xr;
struct Object {
  std::string type;
  std::uintptr_t parent;
};
struct AndroidSurface;
struct Swapchain {
  XrSwapchain handle{}; // stable, guest-only identity
  std::unique_ptr<std::uint64_t> identity;
  std::unique_ptr<GuestImages> backing;
  XrSwapchain host_handle{};
  XrSwapchainCreateInfo host_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  std::vector<XrSwapchainImageVulkanKHR> host_images;
  std::uint64_t release_serial = 0, host_serial = 0, last_submitted = 0;
  XrColorSpaceFB host_color = XR_COLOR_SPACE_UNMANAGED_FB;
  std::int32_t released_index = -1;
  std::uint32_t next_index = 0, host_index = 0;
  bool static_acquired = false;
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  ImageDesc desc{};
  std::vector<XrSwapchainImageVulkanKHR> images;
  bool full_rate_resource_requested = false;
  std::unique_ptr<vulkan::FullRateImage> full_rate_resource;
  std::vector<std::uint32_t> textures;
  std::unique_ptr<ColorImages> color_images;
  // OpenXR permits multiple outstanding acquisitions. Wait and release apply
  // to the oldest acquired image, independently of the returned image index.
  std::deque<std::uint32_t> acquired;
  bool waited = false;
  bool ever_submitted = false;
  bool operation_in_progress = false; // pins host images across unlocked waits
  XrSwapchainStateSamplerVulkanFB sampler_vulkan = default_sampler();
  SamplerOpenGLES sampler_gles{};
  XrSwapchainStateSamplerVulkanFB sampler_host = default_sampler();
  bool sampler_updated = false;
  XrFoveationProfileFB profile{};
  XrSwapchainStateFoveationFlagsFB foveation_flags = 0;
  std::shared_ptr<AndroidSurface> android_surface;
};
struct Foveation {
  XrFoveationLevelFB level;
  float vertical;
  XrFoveationDynamicFB dynamic;
};
struct State {
  std::recursive_mutex mutex;
  bool instance_live = false, gles = false;
  XrVersion api_version = XR_MAKE_VERSION(1, 0, 0);
  GuestAddr vm = 0, context = 0;
  XrGraphicsBindingVulkanKHR graphics{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
  std::uint32_t guest_graphics_queue_index = 0;
  std::set<std::string> enabled;
  std::unordered_map<std::uintptr_t, Object> objects;
  std::map<XrSwapchain, Swapchain> swapchains;
  std::uint64_t presentation_epoch = 0;
  std::size_t host_swapchain_limit = 0; // learned from XR_ERROR_LIMIT_REACHED, zero means unknown
  std::uint64_t host_evictions = 0;
  std::map<XrFoveationProfileFB, std::unique_ptr<Foveation>> foveation;
  struct Action {
    XrActionType type;
    std::vector<XrPath> subactions;
    // Mobile SDKs use hand-specific output on an aggregate vibration action.
    // Keep the original declaration, while the native action explicitly
    // declares every core user path so targeting stays valid and per-device.
    std::vector<XrPath> native_haptic_subactions;
    std::span<const XrPath> haptic_subactions() const {
      return native_haptic_subactions.empty() ? std::span<const XrPath>(subactions)
                                             : std::span<const XrPath>(native_haptic_subactions);
    }
  };
  std::map<XrAction, Action> actions;
  std::map<XrSession, std::set<std::uintptr_t>> attached_sets;
  std::map<XrPath, XrPath> guest_profiles;
  std::map<std::pair<XrAction, XrPath>, XrPath> guest_sources;
  // Action states change only in xrSyncActions; reads in between reuse the
  // host answer stored for this sync (state.cpp stored_action_state).
  struct ActionRead {
    XrSession session; XrAction action; XrPath path; XrStructureType type;
    bool operator==(const ActionRead &) const = default;
  };
  struct ActionReadHash { std::size_t operator()(const ActionRead &) const noexcept; };
  struct StoredActionState { std::uint64_t sync = 0; XrResult result{}; std::array<std::byte, 32> value{}; };
  std::unordered_map<ActionRead, StoredActionState, ActionReadHash> action_states;
  std::uint64_t action_sync = 1;
  struct Hand {
    bool left;
    std::array<bool, 4> pinching{};
  };
  std::map<XrHandTrackerEXT, Hand> hands;
  std::deque<XrEventDataBuffer> events;
  XrColorSpaceFB color_space = XR_COLOR_SPACE_REC709_FB;
  bool color_spaces_logged = false, color_space_set_logged = false;
  // The live instance when XR_KHR_convert_timespec_time is enabled. The time
  // conversions read only this and a fixed clock offset, so they skip mutex,
  // which xrEndFrame holds while validating the frame.
  std::atomic<XrInstance> timespec_instance{};
};
State &state();
template <class H> std::uintptr_t key(H h) { return reinterpret_cast<std::uintptr_t>(h); }
template <class H> bool valid_handle(H h, std::string_view type) {
  auto it = state().objects.find(key(h));
  return h && it != state().objects.end() && it->second.type == type;
}
template <class H, class P> void track_handle(H h, const char *type, P parent) {
  state().objects[key(h)] = {type, key(parent)};
}
void forget_object(std::uintptr_t);
// A sync or attach starts a new action state; destroying any object drops all.
void forget_action_states(bool all = false);
void cancel_object_haptics(std::uintptr_t);
template <class H> void forget_handle(H h) { forget_object(key(h)); }
// ignore_unenabled: skip nodes of extensions the guest did not enable (OpenXR
// §2.9.7) for chains that are inspected locally and never forwarded raw.
XrResult validate_chain(const void *next, bool host_only = true,
                        XrStructureType parent = XR_STRUCTURE_TYPE_MAX_ENUM,
                        bool ignore_unenabled = false);
bool extends_structure(const StructureInfo &, XrStructureType parent);
XrResult validate_struct(const void *, XrStructureType);
// Host-facing input copies: omit unknown/unenabled next nodes without changing
// guest memory or accidentally activating an extension enabled only by the host.
class InputChain {
 public:
  XrResult copy(const void* root, bool validate_parent = true);
  const void* head() const { return storage_.empty() ? nullptr : storage_.front().get(); }
 private:
  std::vector<std::unique_ptr<std::byte[]>> storage_;
};
bool enabled(std::string_view extension);
// Diagnostic prototype: native-rate presentation of legacy mobile composition
// inputs. This does not enable synthetic frames or half-rate application pacing.
bool full_rate_composition();
// The APK requires hand tracking: <uses-feature android:name="oculus.software.handtracking"> without
// android:required="false" (a hands-only app, such as Elixir).
bool hand_tracking_required();
std::map<std::string, std::uint32_t> capabilities();
GuestAddr command_thunk(std::string_view name);
XrResult refresh_rate(XrSession, float *);
VkFormat gl_to_vk(std::int64_t);
std::int64_t vk_to_gl(std::int64_t);
XrResult present_android_surfaces();
XrResult prepare_swapchains(const std::set<XrSwapchain>& submitted);
XrResult destroy_host_swapchain(Swapchain&);
XrResult convert_color_image(Swapchain&, std::uint32_t index, XrColorSpaceFB, bool external_queue_held = false);
namespace guest {
XrResult create_action_for_host(XrActionSet, const XrActionCreateInfo*, XrAction*);
XrResult create_swapchain(XrSession, const XrSwapchainCreateInfo*, XrSwapchain*, bool surface);
// XR_META_vulkan_swapchain_create_info flags as core usage bits; FEATURE_UNSUPPORTED otherwise.
XrResult vulkan_swapchain_flags(const XrVulkanSwapchainCreateInfoMETA& info, XrSwapchainUsageFlags& usage);
}
} // namespace venture::hle::openxr
