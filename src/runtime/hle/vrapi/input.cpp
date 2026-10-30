#include "xr/menu_chord.h"
#include "core/av_telemetry.h"
#include "xr/haptic_output.h"
#include "hle/openxr/state.h"
#include "hle/vrapi/state.h"
#include "xr/bindings.h"
#include "xr/hands.h"
#include "common/log.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>

namespace venture::hle::vrapi {
XrResult initialize_input() {
  auto &input = state().input;
  auto instance = host_xr().instance;
  XrActionSetCreateInfo sci{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strcpy(sci.actionSetName, "vrapi");
  std::strcpy(sci.localizedActionSetName, "Venture VrApi");
  auto r = ::xrCreateActionSet(instance, &sci, &input.set);
  if (XR_FAILED(r))
    return r;
  ::xrStringToPath(instance, "/user/hand/left", &input.hands[0]);
  ::xrStringToPath(instance, "/user/hand/right", &input.hands[1]);
  struct Spec {
    XrAction *handle;
    const char *name;
    XrActionType type;
    const char *left;
    const char *right;
  };
  Spec specs[]{
      {&input.grip_action, "grip", XR_ACTION_TYPE_POSE_INPUT, "/input/grip/pose",
       "/input/grip/pose"},
      {&input.aim_action, "aim", XR_ACTION_TYPE_POSE_INPUT, "/input/aim/pose", "/input/aim/pose"},
      {&input.trigger, "trigger", XR_ACTION_TYPE_FLOAT_INPUT, "/input/trigger/value",
       "/input/trigger/value"},
      {&input.squeeze, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT, "/input/squeeze/value",
       "/input/squeeze/value"},
      {&input.stick, "stick", XR_ACTION_TYPE_VECTOR2F_INPUT, "/input/thumbstick",
       "/input/thumbstick"},
      {&input.a, "a", XR_ACTION_TYPE_BOOLEAN_INPUT, "/input/x/click", "/input/a/click"},
      {&input.b, "b", XR_ACTION_TYPE_BOOLEAN_INPUT, "/input/y/click", "/input/b/click"},
      {&input.click, "click", XR_ACTION_TYPE_BOOLEAN_INPUT, "/input/thumbstick/click",
       "/input/thumbstick/click"},
      {&input.menu, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT, "/input/menu/click", nullptr},
      {&input.haptic, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, "/output/haptic",
       "/output/haptic"},
      {&input.touch_trigger, "touch_trigger", XR_ACTION_TYPE_BOOLEAN_INPUT, "/input/trigger/touch",
       "/input/trigger/touch"},
      {&input.touch_stick, "touch_stick", XR_ACTION_TYPE_BOOLEAN_INPUT, "/input/thumbstick/touch",
       "/input/thumbstick/touch"},
      {&input.touch_a, "touch_a", XR_ACTION_TYPE_BOOLEAN_INPUT, "/input/x/touch", "/input/a/touch"},
      {&input.touch_b, "touch_b", XR_ACTION_TYPE_BOOLEAN_INPUT, "/input/y/touch", "/input/b/touch"},
      {&input.touch_rest, "touch_rest", XR_ACTION_TYPE_BOOLEAN_INPUT, "/input/thumbrest/touch",
       "/input/thumbrest/touch"},
  };
  for (auto &spec : specs) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    std::strcpy(ci.actionName, spec.name);
    std::strcpy(ci.localizedActionName, spec.name);
    ci.actionType = spec.type;
    ci.countSubactionPaths = 2;
    ci.subactionPaths = input.hands;
    r = ::xrCreateAction(input.set, &ci, spec.handle);
    if (XR_FAILED(r))
      return r;
    if (spec.handle == &input.menu)
      venture::xr::register_menu_action(input.set, input.menu);
  }
  XrPath menu_path{};
  r = ::xrStringToPath(instance, "/user/hand/left/input/menu/click", &menu_path);
  if (XR_FAILED(r)) return r;
  // VrApi hands every game its stick clicks, so declare them too: the automatic menu
  // chord then never assumes a VrApi game leaves both sticks free.
  XrPath stick_path{};
  r = ::xrStringToPath(instance, "/user/hand/left/input/thumbstick/click", &stick_path);
  if (XR_FAILED(r)) return r;
  const XrActionSuggestedBinding declared_menu[]{{input.menu, menu_path}, {input.menu, stick_path}};
  for (auto profile : {"/interaction_profiles/oculus/touch_controller",
                       "/interaction_profiles/valve/index_controller",
                       "/interaction_profiles/htc/vive_controller",
                       "/interaction_profiles/khr/simple_controller"}) {
    std::vector<XrActionSuggestedBinding> bindings;
    for (auto &spec : specs)
      for (int hand = 0; hand < 2; ++hand) {
        auto suffix = hand ? spec.right : spec.left;
        if (!suffix)
          continue;
        auto mapped = touch_binding(
            std::string(hand ? "/user/hand/right" : "/user/hand/left") + suffix, profile);
        if (!mapped)
          continue;
        XrPath path{};
        if (XR_SUCCEEDED(::xrStringToPath(instance, mapped->c_str(), &path)))
          bindings.push_back({*spec.handle, path});
      }
    XrInteractionProfileSuggestedBinding bi{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    ::xrStringToPath(instance, profile, &bi.interactionProfile);
    bi.countSuggestedBindings = std::uint32_t(bindings.size());
    bi.suggestedBindings = bindings.data();
    auto suggested = venture::xr::suggest_with_menu_chord(instance, &bi);
    if (XR_FAILED(suggested))
      failure("controller bindings", suggested);
    else {
      // Preserve the guest's menu intent even when this host profile omits
      // that native binding (Index, or a runtime-reserved Touch menu).
      const XrInteractionProfileSuggestedBinding declared{
          XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr, bi.interactionProfile,
          2, declared_menu};
      venture::xr::remember_menu_bindings(instance, bi.interactionProfile, &declared);
    }
  }
  XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &input.set};
  r = venture::xr::attach_with_menu_chord(session().handle, &ai);
  if (XR_FAILED(r))
    return r;
  for (int hand = 0; hand < 2; ++hand) {
    XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    ci.action = input.grip_action;
    ci.subactionPath = input.hands[hand];
    // VrApi reports the legacy Oculus controller pose, not the OpenXR grip: Meta's OpenXR plugin puts
    // the grip 4 cm behind and 3 cm below it, pitched up 60 degrees (OculusPoseToGrip). This is
    // that legacy pose within the grip.
    ci.poseInActionSpace = {{-0.5f, 0, 0, 0.8660254f}, {0, -0.0196410f, -0.0459808f}};
    r = ::xrCreateActionSpace(session().handle, &ci, &input.grip[hand]);
    if (XR_FAILED(r))
      return r;
    ci.poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    ci.action = input.aim_action;
    r = ::xrCreateActionSpace(session().handle, &ci, &input.aim[hand]);
    if (XR_FAILED(r))
      return r;
  }
  if (host_xr().enabled_extensions.contains("XR_EXT_hand_tracking")) {
    XrSystemHandTrackingPropertiesEXT hands{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES, &hands};
    if (XR_SUCCEEDED(::xrGetSystemProperties(instance, host_xr().system, &props)) &&
        hands.supportsHandTracking) {
      // A Quest reports hands only while they are free; held controllers are not hands. Hosts
      // that derive skeletons from controllers (SteamVR with Touch or Index) report those as
      // the controller data source, so ask for unobstructed hands only.
      XrHandTrackingDataSourceEXT free_hands = XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT;
      XrHandTrackingDataSourceInfoEXT source{XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT, nullptr, 1,
                                             &free_hands};
      for (int hand = 0; hand < 2; ++hand) {
        XrHandTrackerCreateInfoEXT ci{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        if (host_xr().enabled_extensions.contains("XR_EXT_hand_tracking_data_source"))
          ci.next = &source;
        ci.hand = hand ? XR_HAND_RIGHT_EXT : XR_HAND_LEFT_EXT;
        ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        auto created = host_call<PFN_xrCreateHandTrackerEXT, "xrCreateHandTrackerEXT">(session().handle, &ci, &input.trackers[hand]);
        if (created == XR_ERROR_FEATURE_UNSUPPORTED && ci.next && hle::openxr::hand_tracking_required()) {
          // A hands-only app on a host without free hands (Monado: controller-conforming only):
          // the host's controller-derived skeleton is the only hand source there is.
          static constexpr XrHandTrackingDataSourceEXT any_hands[]{
              XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT, XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT};
          XrHandTrackingDataSourceInfoEXT conforming{XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT, nullptr, 2,
                                                     const_cast<XrHandTrackingDataSourceEXT*>(any_hands)};
          ci.next = &conforming;
          created = host_call<PFN_xrCreateHandTrackerEXT, "xrCreateHandTrackerEXT">(session().handle, &ci, &input.trackers[hand]);
          ci.next = &source;
          if (XR_SUCCEEDED(created) && hand == 0)
            log::info("vrapi", "App requires hand tracking; host hands include controller-conforming skeletons");
        }
        if (XR_FAILED(created)) {
          failure("hand tracker", created);
          break;
        }
      }
      input.hands_supported = input.trackers[0] && input.trackers[1];
    }
  }
  return XR_SUCCESS;
}
void destroy_input() {
  cancel_haptics();
  venture::xr::clear_haptic_bindings(session().handle);
  auto &i = state().input;
  for (auto space : {i.grip[0], i.grip[1], i.aim[0], i.aim[1]})
    if (space)
      ::xrDestroySpace(space);
  for (auto tracker : i.trackers)
    if (tracker)
      host_call<PFN_xrDestroyHandTrackerEXT, "xrDestroyHandTrackerEXT">(tracker);
  if (i.set) {
    venture::xr::forget_menu_object(reinterpret_cast<std::uintptr_t>(i.set));
    ::xrDestroyActionSet(i.set);
  }
  i = {};
}
XrResult sync_input() {
  XrActiveActionSet active{state().input.set, XR_NULL_PATH};
  XrActionsSyncInfo info{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
  return venture::xr::sync_with_menu_chord(session().handle, &info);
}
namespace {
float axis(XrAction action, int hand) {
  XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action,
                          state().input.hands[hand]};
  XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
  return XR_SUCCEEDED(::xrGetActionStateFloat(session().handle, &gi, &value)) && value.isActive
             ? value.currentState
             : 0;
}
bool button(XrAction action, int hand) {
  XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action,
                          state().input.hands[hand]};
  XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
  return XR_SUCCEEDED(::xrGetActionStateBoolean(session().handle, &gi, &value)) && value.isActive &&
         value.currentState;
}
bool active(int hand) {
  XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, state().input.grip_action,
                          state().input.hands[hand]};
  XrActionStatePose value{XR_TYPE_ACTION_STATE_POSE};
  return XR_SUCCEEDED(::xrGetActionStatePose(session().handle, &gi, &value)) && value.isActive;
}
XrResult joints(int hand, XrTime time, XrHandJointLocationsEXT &out) {
  if (!state().input.hands_supported)
    return XR_ERROR_FEATURE_UNSUPPORTED;
  XrHandJointsLocateInfoEXT li{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT, nullptr, state().app, time};
  auto r = host_call<PFN_xrLocateHandJointsEXT, "xrLocateHandJointsEXT">(state().input.trackers[hand],
                                                                         &li, &out);
  if (XR_SUCCEEDED(r))
    for (std::uint32_t i = 0; i < out.jointCount; ++i) {
      hle::openxr::sanitize(out.jointLocations[i].locationFlags, out.jointLocations[i].pose);
      if (!std::isfinite(out.jointLocations[i].radius)) out.jointLocations[i].radius = 0;
    }
  return r;
}
XrQuaternionf hand_basis(bool left) {
  // OVR hand frames point along +/-X, whereas OpenXR fingers point along -Z.
  // Meta Unity-Movement SkeletonUtilities.ConvertOpenXRHandToOvrHand uses
  // (180,90,0) for the left frame and (0,270,0) for the right frame.
  constexpr float h = 0.7071067811865475f;
  return left ? XrQuaternionf{h, 0, -h, 0} : XrQuaternionf{0, -h, 0, h};
}
XrQuaternionf hand_local_rotation(XrQuaternionf rotation, XrQuaternionf basis) {
  return multiply(multiply(inverse(basis), rotation), basis);
}
} // namespace
namespace guest {
std::int32_t vrapi_EnumerateInputDevices(Mobile *mobile, std::uint32_t index, InputHeader *out) {
  std::lock_guard lock(state().mutex);
  if (!valid(mobile) || !out)
    return invalid;
  // One device per hand, as on a Quest: a tracked free hand replaces that side's controller (hosts
  // in hand-tracking mode may still present a simulated controller), and without the data-source
  // extension a present controller wins over a skeleton the host may derive from it, unless the
  // app requires hand tracking.
  static const bool sourced = host_xr().enabled_extensions.contains("XR_EXT_hand_tracking_data_source");
  std::vector<InputHeader> devices, hands;
  bool hand_shown[2]{};
  if (state().input.hands_supported)
    for (int hand = 0; hand < 2; ++hand) {
      XrHandJointLocationEXT locations[26]{};
      XrHandJointLocationsEXT loc{XR_TYPE_HAND_JOINT_LOCATIONS_EXT, nullptr, XR_FALSE, 26,
                                  locations};
      hand_shown[hand] = venture::xr::hand_replaces_controller(
          XR_SUCCEEDED(joints(hand, time_from_seconds(0), loc)) && loc.isActive, sourced, active(hand),
          hle::openxr::hand_tracking_required());
      if (hand_shown[hand])
        hands.push_back({32, std::uint32_t(hand + 3)});
    }
  for (int hand = 0; hand < 2; ++hand)
    if (!hand_shown[hand] && active(hand))
      devices.push_back({4, std::uint32_t(hand + 1)});
  devices.insert(devices.end(), hands.begin(), hands.end());
  if (index >= devices.size())
    return no_device;
  *out = devices[index];
  return success;
}
std::int32_t vrapi_GetInputDeviceCapabilities(Mobile *mobile, InputHeader *out) {
  std::lock_guard lock(state().mutex);
  if (!valid(mobile) || !out)
    return invalid;
  auto id = out->id;
  if (out->type == 4 && id >= 1 && id <= 2) {
    auto *caps = reinterpret_cast<RemoteCapabilities *>(out);
    *caps = {};
    caps->header = {4, id};
    caps->capabilities = 1 | 2 | (id == 1 ? 4 : 8) | 0x40 | 0x80 | 0x200 | 0x400 | 0x2000 | 0x4000;
    caps->haptic_max = 256;
    caps->haptic_ms = 4;
    caps->buttons =
        0x20000000 | 0x04000000 | 0x80000000 | (id == 1 ? 0x100 | 0x200 | 0x100000 : 1 | 2);
    caps->touches = 0x40 | 0x20 | (id == 1 ? 4 | 8 : 1 | 2);
    return success;
  }
  if (out->type == 32 && id >= 3 && id <= 4 && state().input.hands_supported) {
    auto *caps = reinterpret_cast<HandCapabilities *>(out);
    *caps = {{32, id}, id == 3 ? 1u : 2u, 15};
    return success;
  }
  return no_device;
}
std::int32_t vrapi_GetCurrentInputState(Mobile *mobile, std::uint32_t id, InputStateHeader *out) {
  std::lock_guard lock(state().mutex);
  if (!valid(mobile) || !out)
    return invalid;
  auto &i = state().input;
  if (id >= 1 && id <= 2 && out->type == 4) {
    int hand = int(id - 1);
    auto *remote = reinterpret_cast<RemoteState *>(out);
    *remote = {};
    remote->header.type = 4;
    remote->header.time = vrapi_GetTimeInSeconds();
    if (!active(hand))
      return no_device;
    remote->trigger = axis(i.trigger, hand);
    remote->grip = axis(i.squeeze, hand);
    remote->recenter = std::uint8_t(state().recenter_count);
    remote->buttons =
        (button(i.a, hand) ? (hand ? 1u : 0x100u) : 0u) |
        (button(i.b, hand) ? (hand ? 2u : 0x200u) : 0u) |
        (button(i.click, hand) ? 0x80000000u : 0u) | ((button(i.menu, hand) || (!hand && venture::xr::menu_chord_pulse(session().handle))) ? 0x100000u : 0u) |
        (remote->trigger > 0.55f ? 0x20000000u : 0u) | (remote->grip > 0.55f ? 0x04000000u : 0u);
    remote->touches = (button(i.touch_trigger, hand) ? 0x40u : 0u) |
                      (button(i.touch_stick, hand) ? 0x20u : 0u) |
                      (button(i.touch_a, hand) ? (hand ? 1u : 4u) : 0u) |
                      (button(i.touch_b, hand) ? (hand ? 2u : 8u) : 0u) |
                      (button(i.touch_rest, hand) ? 0x1000u : 0u);
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, i.stick, i.hands[hand]};
    XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(::xrGetActionStateVector2f(session().handle, &gi, &stick)) && stick.isActive) {
      remote->raw_joystick = {stick.currentState.x, stick.currentState.y};
      if (std::hypot(stick.currentState.x, stick.currentState.y) >= 0.1f)
        remote->joystick = remote->raw_joystick;
    }
    if (av::enabled()) {
      using Clock = std::chrono::steady_clock;
      static std::array<Clock::time_point, 2> reported{}; // protected by state().mutex
      const auto now = Clock::now();
      if (now - reported[hand] >= std::chrono::milliseconds(250)) {
        reported[hand] = now;
        av::emit({{"kind", "controller_state"}, {"api", "vrapi"},
                  {"hand", hand ? "/user/hand/right" : "/user/hand/left"},
                  {"trigger", remote->trigger}, {"squeeze", remote->grip},
                  {"buttons", remote->buttons}, {"touches", remote->touches},
                  {"input_frame_index", state().input_frame_index}});
      }
    }
    return success;
  }
  if (id >= 3 && id <= 4 && out->type == 32) {
    auto *hand = reinterpret_cast<HandState *>(out);
    *hand = {};
    hand->header.type = 32;
    hand->header.time = vrapi_GetTimeInSeconds();
    XrHandJointLocationEXT data[26]{};
    XrHandJointLocationsEXT loc{XR_TYPE_HAND_JOINT_LOCATIONS_EXT, nullptr, XR_FALSE, 26, data};
    auto r = joints(int(id - 3), time_from_seconds(0), loc);
    if (XR_FAILED(r))
      return result(r, "GetCurrentInputState hand");
    if (!loc.isActive)
      return no_device;
    XrHandTrackingAimStateFB aim{XR_TYPE_HAND_TRACKING_AIM_STATE_FB};
    std::array<bool, 4> pinch{};
    hand_aim(loc, pinch, aim);
    hand->pointer = pose(aim.aimPose);
    hand->status = std::uint32_t(aim.status) & ~1u;
    hand->pinch[0] = aim.pinchStrengthIndex;
    hand->pinch[1] = aim.pinchStrengthMiddle;
    hand->pinch[2] = aim.pinchStrengthRing;
    hand->pinch[3] = aim.pinchStrengthLittle;
    return success;
  }
  return no_device;
}
std::int32_t vrapi_GetInputTrackingState(Mobile *mobile, std::uint32_t id, double time,
                                         TrackingHead *out) {
  std::lock_guard lock(state().mutex);
  if (!valid(mobile) || !out)
    return invalid;
  if (id < 1 || id > 2)
    return no_device;
  const auto query_time = time_from_seconds(time);
  const auto tracking = tracking_at(query_time, state().input.grip[id - 1]);
  *out = TrackingHead{tracking.status, 0, tracking.head};
  // Record the pose actually returned to the guest, without another host
  // locate or any clock/serialization work when optional AV telemetry is off.
  if (av::enabled()) {
    using Clock = std::chrono::steady_clock;
    static std::array<Clock::time_point, 2> reported{}; // protected by state().mutex
    const auto now = Clock::now();
    const auto hand = id - 1;
    if (now - reported[hand] >= std::chrono::milliseconds(250)) {
      reported[hand] = now;
      const auto& p = out->head.pose.position;
      const auto& q = out->head.pose.orientation;
      av::emit({{"kind", "controller_pose"}, {"api", "vrapi"},
                {"hand", hand ? "/user/hand/right" : "/user/hand/left"},
                {"space", reinterpret_cast<std::uintptr_t>(state().input.grip[hand])},
                {"tracking_status", out->status}, {"orientation_valid", bool(out->status & 4u)},
                {"query_seconds", time}, {"query_xr_time", query_time},
                {"input_frame_index", state().input_frame_index},
                {"prediction_frame_index", state().prediction_index},
                {"bound_frame_index", state().frame_index},
                {"position", {p.x, p.y, p.z}},
                {"orientation", {q.x, q.y, q.z, q.w}}});
    }
  }
  return success;
}
std::int32_t vrapi_SetHapticVibrationSimple(Mobile *mobile, std::uint32_t id, float amplitude) {
  std::lock_guard lock(state().mutex);
  if (!valid(mobile) || id < 1 || id > 2 || !std::isfinite(amplitude)) {
    XrHapticActionInfo ai{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, state().input.haptic,
        id >= 1 && id <= 2 ? state().input.hands[id - 1] : XR_NULL_PATH};
    venture::xr::haptic_event("vrapi_simple", session().handle, ai, amplitude, 0, 0,
        "validate_guest_request", XR_ERROR_VALIDATION_FAILURE,
        {{"stage", "guest_request"}, {"host_query_safe", false}, {"device_id", id}, {"guest_result", invalid},
         {"hand", id == 1 ? "/user/hand/left" : id == 2 ? "/user/hand/right" : "invalid"}});
    return invalid;
  }
  cancel_haptics(int(id - 1));
  XrHapticActionInfo ai{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, state().input.haptic,
                        state().input.hands[id - 1]};
  if (amplitude <= 0) {
    nlohmann::json fields;
    if (venture::av::enabled()) fields = {{"input_amplitude", amplitude}, {"amplitude_clamped", amplitude < 0}};
    return result(venture::xr::stop_haptic("vrapi_simple", session().handle, ai, "guest_request", std::move(fields)), "StopHaptic");
  }
  XrHapticVibration vibration{
      XR_TYPE_HAPTIC_VIBRATION, nullptr,
      std::max<XrDuration>(session().frame.predictedDisplayPeriod, 20'000'000),
      XR_FREQUENCY_UNSPECIFIED, amplitude};
  // The shared output translator saturates finite positive overdrive and
  // records the original strength, as it does for core OpenXR submissions.
  return result(venture::xr::apply_haptic("vrapi_simple", session().handle, ai, vibration, "guest_request"),
                "Haptic");
}
std::int32_t vrapi_GetHandPose(Mobile *mobile, std::uint32_t id, double seconds,
                               HandPoseHeader *header) {
  std::lock_guard lock(state().mutex);
  if (!valid(mobile) || !header || header->version != hand_version)
    return invalid;
  if (id < 3 || id > 4)
    return no_device;
  auto *out = reinterpret_cast<HandPose *>(header);
  *out = {};
  out->header.version = hand_version;
  out->scale = 1;
  out->requested_time = seconds;
  XrHandJointLocationEXT data[26]{};
  XrHandJointLocationsEXT loc{XR_TYPE_HAND_JOINT_LOCATIONS_EXT, nullptr, XR_FALSE, 26, data};
  auto r = joints(int(id - 3), time_from_seconds(seconds), loc);
  if (XR_FAILED(r))
    return result(r, "GetHandPose");
  out->sample_time = vrapi_GetTimeInSeconds();
  if (!loc.isActive)
    return success;
  if (!(data[1].locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
    return success;
  out->status = 1;
  const auto basis = hand_basis(id == 3);
  auto root = data[1].pose;
  root.orientation = multiply(root.orientation, basis);
  out->root = pose(root);
  out->confidence = 0x3f800000;
  for (int bone = 0; bone < 24; ++bone) {
    const auto &joint = data[ovr_joint_map[bone]];
    int parent = ovr_parent_map[bone];
    auto rotation = parent < 0 ? XrQuaternionf{0, 0, 0, 1}
                               : multiply(inverse(data[ovr_joint_map[parent]].pose.orientation),
                                          joint.pose.orientation);
    rotation = hand_local_rotation(rotation, basis);
    out->rotations[bone] = {rotation.x, rotation.y, rotation.z, rotation.w};
    if (!(joint.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT))
      out->confidence = 0;
  }
  for (auto &confidence : out->finger_confidence)
    confidence = out->confidence;
  return success;
}
std::int32_t vrapi_GetHandSkeleton(Mobile *mobile, std::int32_t hand, std::uint32_t *header) {
  std::lock_guard lock(state().mutex);
  if (!valid(mobile) || !header || *header != hand_version || hand < 1 || hand > 2)
    return invalid;
  if (!state().input.hands_supported)
    return unavailable;
  auto *out = reinterpret_cast<HandSkeleton *>(header);
  *out = {};
  out->version = hand_version;
  out->bone_count = 24;
  out->capsule_count = 19;
  const auto &model = hand_model(hand == 1);
  const auto basis = hand_basis(hand == 1);
  for (int bone = 0; bone < 24; ++bone) {
    int parent = ovr_parent_map[bone];
    out->parents[bone] = std::int16_t(parent);
    auto joint = model.joints[ovr_joint_map[bone]];
    if (parent >= 0)
      joint = compose(inverse(model.joints[ovr_joint_map[parent]]), joint);
    joint.position = rotate(inverse(basis), joint.position);
    joint.orientation = hand_local_rotation(joint.orientation, basis);
    out->poses[bone] = pose(joint);
    if (bone < 19) {
      auto &capsule = out->capsules[bone];
      capsule.bone = std::int16_t(bone);
      capsule.radius = model.radii[ovr_joint_map[bone]];
      const auto end = rotate(inverse(basis), {0, 0, -0.02f});
      capsule.points[1] = {end.x, end.y, end.z};
    }
  }
  return success;
}
std::int32_t vrapi_GetHandMesh(Mobile *mobile, std::int32_t hand, std::uint32_t *header) {
  std::lock_guard lock(state().mutex);
  if (!valid(mobile) || !header || *header != hand_version || hand < 1 || hand > 2)
    return invalid;
  if (!state().input.hands_supported)
    return unavailable;
  auto *out = reinterpret_cast<HandMesh *>(header);
  *out = {};
  out->version = hand_version;
  const auto &model = hand_model(hand == 1);
  const auto basis = inverse(hand_basis(hand == 1));
  out->vertex_count = std::uint32_t(model.positions.size());
  out->index_count = std::uint32_t(model.indices.size());
  for (std::size_t n = 0; n < model.positions.size(); ++n) {
    auto p = rotate(basis, model.positions[n]), normal = rotate(basis, model.normals[n]);
    out->positions[n] = {p.x, p.y, p.z};
    out->normals[n] = {normal.x, normal.y, normal.z};
    out->uv[n] = {model.uv[n].x, model.uv[n].y};
    int joint = model.blends[n].x, bone = 0;
    for (int k = 0; k < 19; ++k)
      if (ovr_joint_map[k] == joint)
        bone = k;
    out->blend_indices[n] = {std::int16_t(bone), -1, -1, -1};
    out->blend_weights[n] = {1, 0, 0, 0};
  }
  std::copy(model.indices.begin(), model.indices.end(), out->indices);
  return success;
}
} // namespace guest
} // namespace venture::hle::vrapi
