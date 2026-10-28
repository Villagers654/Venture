#include "xr/hands.h"
#include "common/log.h"
#include "hle/openxr/state.h"
#include <cstring>
#include <cmath>
namespace venture::hle::openxr::guest {
XrResult xrCreateHandTrackerEXT(XrSession handle, const XrHandTrackerCreateInfoEXT *info,
                                XrHandTrackerEXT *out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_EXT_hand_tracking"))
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!out || !info || info->type != XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT)
    return XR_ERROR_VALIDATION_FAILURE;
  auto native = *info;
  native.next = nullptr;
  XrHandTrackingDataSourceInfoEXT source{};
  std::set<const void*> seen;
  std::set<XrStructureType> types;
  for (auto p = static_cast<const XrBaseInStructure*>(info->next); p; p = p->next) {
    if (!seen.insert(p).second || seen.size() > 64) return XR_ERROR_VALIDATION_FAILURE;
    const StructureInfo* metadata = nullptr;
    for (const auto& m : structure_metadata()) if (m.type == p->type) { metadata = &m; break; }
    // OpenXR 1.1, "Valid Usage for Structure Pointer Chains" and "Next Chain
    // Structure Uniqueness": ignore disabled extensions and later duplicates.
    // https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#fundamentals-validusage-next
    // Epic's OVRPlugin supplies disabled data-source preferences; skipping
    // them preserves the runtime's default tracking source.
    if (!metadata || !types.insert(p->type).second) continue;
    if (*metadata->extension && (!enabled(metadata->extension) ||
        !host_xr().enabled_extensions.contains(metadata->extension))) continue;
    if (!extends_structure(*metadata, XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT))
      return XR_ERROR_VALIDATION_FAILURE;
    // This is the only supported input extension of HandTrackerCreateInfo.
    // Copy only to remove skipped next nodes; never modify the guest chain.
    if (p->type != XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT)
      return XR_ERROR_FEATURE_UNSUPPORTED;
    source = *reinterpret_cast<const XrHandTrackingDataSourceInfoEXT*>(p);
    source.next = nullptr;
    native.next = &source;
  }
  // Without a guest preference, keep Quest semantics: hands are reported only while free. Hosts
  // that derive skeletons from held controllers (SteamVR with Touch or Index) label those as the
  // controller source, which a Quest app never sees as hand tracking.
  XrHandTrackingDataSourceEXT free_hands = XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT;
  if (!native.next && host_xr().enabled_extensions.contains("XR_EXT_hand_tracking_data_source")) {
    source = {XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT, nullptr, 1, &free_hands};
    native.next = &source;
  }
  auto r = host_call<PFN_xrCreateHandTrackerEXT, "xrCreateHandTrackerEXT">(handle, &native, out);
  if (r == XR_ERROR_FEATURE_UNSUPPORTED && native.next == &source && source.requestedDataSources == &free_hands &&
      hand_tracking_required()) {
    // A hands-only app on a host without free hands: its controller-derived skeleton is the only hand source.
    static constexpr XrHandTrackingDataSourceEXT any_hands[]{XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT,
                                                             XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT};
    source.requestedDataSourceCount = 2;
    source.requestedDataSources = const_cast<XrHandTrackingDataSourceEXT*>(any_hands);
    r = host_call<PFN_xrCreateHandTrackerEXT, "xrCreateHandTrackerEXT">(handle, &native, out);
  }
  if (XR_SUCCEEDED(r)) {
    track_handle(*out, "XrHandTrackerEXT", handle);
    state().hands[*out] = {info->hand == XR_HAND_LEFT_EXT, {}};
  }
  return r;
}
XrResult xrDestroyHandTrackerEXT(XrHandTrackerEXT tracker) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(tracker, "XrHandTrackerEXT"))
    return XR_ERROR_HANDLE_INVALID;
  auto r = host_call<PFN_xrDestroyHandTrackerEXT, "xrDestroyHandTrackerEXT">(tracker);
  if (XR_SUCCEEDED(r)) {
    state().hands.erase(tracker);
    forget_handle(tracker);
  }
  return r;
}
XrResult xrLocateHandJointsEXT(XrHandTrackerEXT tracker, const XrHandJointsLocateInfoEXT *info,
                               XrHandJointLocationsEXT *out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(tracker, "XrHandTrackerEXT"))
    return XR_ERROR_HANDLE_INVALID;
  if (!out || out->type != XR_TYPE_HAND_JOINT_LOCATIONS_EXT || out->jointCount != 26 ||
      !out->jointLocations ||
      XR_FAILED(validate_struct(info, XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT)) ||
      !valid_handle(info->baseSpace, "XrSpace"))
    return XR_ERROR_VALIDATION_FAILURE;
  auto copy = *out;
  copy.next = nullptr;
  XrHandJointVelocitiesEXT *velocities = nullptr;
  XrHandTrackingAimStateFB *aim = nullptr;
  XrHandTrackingCapsulesStateFB *capsules = nullptr;
  XrHandTrackingScaleFB *scale = nullptr;
  XrHandTrackingDataSourceStateEXT *source = nullptr;
  std::set<void *> seen;
  std::set<XrStructureType> types;
  for (auto p = static_cast<XrBaseOutStructure *>(out->next); p; p = p->next) {
    if (!seen.insert(p).second || seen.size() > 64)
      return XR_ERROR_VALIDATION_FAILURE;
    const StructureInfo* metadata = nullptr;
    for (const auto& m : structure_metadata()) if (m.type == p->type) { metadata = &m; break; }
    // The same OpenXR next-chain rules apply to output structures. Skipped
    // nodes, including their payloads and type/next fields, stay untouched.
    if (!metadata || !types.insert(p->type).second) continue;
    if (*metadata->extension && !enabled(metadata->extension)) continue;
    if (!extends_structure(*metadata, XR_TYPE_HAND_JOINT_LOCATIONS_EXT))
      return XR_ERROR_VALIDATION_FAILURE;
    switch (p->type) {
    case XR_TYPE_HAND_JOINT_VELOCITIES_EXT:
      velocities = reinterpret_cast<XrHandJointVelocitiesEXT *>(p);
      break;
    case XR_TYPE_HAND_TRACKING_AIM_STATE_FB:
      aim = reinterpret_cast<XrHandTrackingAimStateFB *>(p);
      break;
    case XR_TYPE_HAND_TRACKING_CAPSULES_STATE_FB:
      capsules = reinterpret_cast<XrHandTrackingCapsulesStateFB *>(p);
      break;
    case XR_TYPE_HAND_TRACKING_SCALE_FB:
      scale = reinterpret_cast<XrHandTrackingScaleFB *>(p);
      break;
    case XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT:
      if (!host_xr().enabled_extensions.contains(metadata->extension))
        return XR_ERROR_FEATURE_UNSUPPORTED;
      source = reinterpret_cast<XrHandTrackingDataSourceStateEXT *>(p);
      break;
    default:
      return XR_ERROR_FEATURE_UNSUPPORTED;
    }
  }
  XrHandJointVelocitiesEXT velocity_copy{XR_TYPE_HAND_JOINT_VELOCITIES_EXT};
  XrHandTrackingDataSourceStateEXT source_copy{XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT};
  if (velocities) {
    if (velocities->jointCount != 26 || !velocities->jointVelocities)
      return XR_ERROR_VALIDATION_FAILURE;
    velocity_copy = *velocities;
    velocity_copy.next = nullptr;
    copy.next = &velocity_copy;
  }
  if (source) {
    source_copy.next = copy.next;
    copy.next = &source_copy;
  }
  XrHandTrackingScaleFB scale_copy{XR_TYPE_HAND_TRACKING_SCALE_FB};
  const bool native_scale = scale && host_xr().enabled_extensions.contains("XR_FB_hand_tracking_mesh");
  if (scale) {
    if (scale->overrideHandScale &&
        (!std::isfinite(scale->overrideValueInput) || scale->overrideValueInput <= 0))
      return XR_ERROR_VALIDATION_FAILURE;
    if (native_scale) {
      scale_copy = *scale;
      scale_copy.next = copy.next;
      copy.next = &scale_copy;
    }
  }
  auto r = host_call<PFN_xrLocateHandJointsEXT, "xrLocateHandJointsEXT">(tracker, info, &copy);
  if (XR_SUCCEEDED(r)) {
    out->isActive = copy.isActive;
    if (scale) {
      if (native_scale) {
        scale->sensorOutput = scale_copy.sensorOutput;
        scale->currentOutput = scale_copy.currentOutput;
      } else if (auto scaled = hand_scale(state().hands.at(tracker).left, copy, velocities, *scale);
                 XR_FAILED(scaled)) return scaled;
    }
    if (aim)
      hand_aim(copy, state().hands.at(tracker).pinching, *aim);
    if (capsules)
      hand_capsules(copy, *capsules);
    if (source) {
      source->isActive = source_copy.isActive;
      source->dataSource = source_copy.dataSource;
    }
  }
  return r;
}
XrResult xrGetHandMeshFB(XrHandTrackerEXT tracker, XrHandTrackingMeshFB *out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(tracker, "XrHandTrackerEXT"))
    return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_FB_hand_tracking_mesh"))
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  const auto result = host_xr().enabled_extensions.contains("XR_FB_hand_tracking_mesh")
      ? host_call<PFN_xrGetHandMeshFB, "xrGetHandMeshFB">(tracker, out)
      : copy_hand_mesh(hand_model(state().hands.at(tracker).left), out);
  if (XR_FAILED(result) && out)
    log::warn("xr", "xrGetHandMeshFB returned {} capacities={}/{}/{} next_present={}",
              int(result), out->jointCapacityInput, out->vertexCapacityInput,
              out->indexCapacityInput, out->next != nullptr);
  return result;
}
} // namespace venture::hle::openxr::guest
