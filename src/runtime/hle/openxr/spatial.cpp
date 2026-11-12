// XR_FB_spatial_entity, _query, _storage, XR_FB_scene and XR_FB_scene_capture,
// answered as a Quest without a room setup. A spatial anchor is a host LOCAL
// reference space fixed where the guest placed it, so every host locates it
// like any other space. Saved anchors last for the process: host LOCAL
// coordinates do not carry over to another host session. Queries find saved
// anchors and never scene anchors; a scene capture request completes with a
// failure, as when the user leaves Space Setup without capturing a room.
// Every completion is an event on the guest's queue (state().events).
#include "common/log.h"
#include "hle/openxr/state.h"
#include "xr/math.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

namespace venture::hle::openxr {
namespace {
using UuidKey = std::array<std::uint8_t, XR_UUID_SIZE_EXT>;
UuidKey uuid_key(const XrUuidEXT& uuid) {
  UuidKey key;
  std::memcpy(key.data(), uuid.data, key.size());
  return key;
}
XrUuidEXT uuid_of(const UuidKey& key) {
  XrUuidEXT uuid;
  std::memcpy(uuid.data, key.data(), key.size());
  return uuid;
}
// RFC 4122 version 4, as Quest anchor UUIDs are.
XrUuidEXT new_uuid() {
  static std::mt19937_64 random{std::random_device{}()};
  XrUuidEXT uuid;
  const std::uint64_t words[]{random(), random()};
  std::memcpy(uuid.data, words, sizeof(words));
  uuid.data[6] = std::uint8_t((uuid.data[6] & 0x0f) | 0x40);
  uuid.data[8] = std::uint8_t((uuid.data[8] & 0x3f) | 0x80);
  return uuid;
}
bool valid_pose(const XrPosef& pose) {
  const auto& q = pose.orientation;
  const auto& p = pose.position;
  const float norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  return std::isfinite(norm) && std::abs(norm - 1) <= 0.01f && std::isfinite(p.x) && std::isfinite(p.y) &&
         std::isfinite(p.z);
}
State::SpatialAnchor* anchor(XrSpace space) {
  auto found = state().anchors.find(space);
  return found == state().anchors.end() ? nullptr : &found->second;
}
// A host LOCAL space at `pose`, tracked as the guest's anchor `uuid`.
XrResult make_anchor(XrSession session, const XrPosef& pose, const XrUuidEXT& uuid, XrSpace* out) {
  const XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                        pose};
  const auto result = ::xrCreateReferenceSpace(session, &info, out);
  if (XR_FAILED(result)) return result;
  track_handle(*out, "XrSpace", session);
  state().anchors[*out] = {session, uuid, pose};
  return XR_SUCCESS;
}
// Where `space` is at `time` in host LOCAL coordinates.
XrResult locate_in_local(XrSession session, XrSpace space, XrTime time, XrSpaceLocation& where) {
  const XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                        {{0, 0, 0, 1}, {0, 0, 0}}};
  XrSpace local{};
  auto result = ::xrCreateReferenceSpace(session, &info, &local);
  if (XR_FAILED(result)) return result;
  result = ::xrLocateSpace(space, local, time, &where);
  ::xrDestroySpace(local);
  if (XR_SUCCEEDED(result) && !anchor_locatable(space)) where.locationFlags = 0;
  return result;
}
constexpr XrSpaceLocationFlags kPoseValid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
bool anchor_component(XrSpaceComponentTypeFB type) {
  return type == XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB ||
         (type == XR_SPACE_COMPONENT_TYPE_STORABLE_FB && enabled("XR_FB_spatial_entity_storage"));
}
XrResult scene_component(XrSession session, XrSpace space, const void* output, const char* extension) {
  if (!valid_handle(session, "XrSession") || !valid_handle(space, "XrSpace")) return XR_ERROR_HANDLE_INVALID;
  if (!enabled(extension)) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!output) return XR_ERROR_VALIDATION_FAILURE;
  // No space carries a scene component: there is no captured room.
  return XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB;
}
}  // namespace

namespace guest {
XrResult xrCreateSpatialAnchorFB(XrSession session, const XrSpatialAnchorCreateInfoFB* info,
                                 XrAsyncRequestIdFB* requestId) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(session, "XrSession")) return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_FB_spatial_entity")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!requestId || !info || info->type != XR_TYPE_SPATIAL_ANCHOR_CREATE_INFO_FB) return XR_ERROR_VALIDATION_FAILURE;
  if (!valid_handle(info->space, "XrSpace")) return XR_ERROR_HANDLE_INVALID;
  if (!valid_pose(info->poseInSpace)) return XR_ERROR_POSE_INVALID;
  if (info->time <= 0) return XR_ERROR_TIME_INVALID;
  XrSpaceLocation where{XR_TYPE_SPACE_LOCATION};
  const auto located = locate_in_local(session, info->space, info->time, where);
  if (located == XR_ERROR_TIME_INVALID || located == XR_ERROR_HANDLE_INVALID) return located;
  XrEventDataSpatialAnchorCreateCompleteFB done{XR_TYPE_EVENT_DATA_SPATIAL_ANCHOR_CREATE_COMPLETE_FB};
  done.requestId = *requestId = state().next_request++;
  // A space that is not tracked now gives no world position: the request fails as on a Quest.
  done.result = XR_FAILED(located) ? located
              : (where.locationFlags & kPoseValid) != kPoseValid ? XR_ERROR_RUNTIME_FAILURE
              : make_anchor(session, venture::xr::compose(where.pose, info->poseInSpace), new_uuid(), &done.space);
  if (XR_SUCCEEDED(done.result)) done.uuid = state().anchors.at(done.space).uuid;
  log::info("xr", "Spatial anchor request {} result={} space={}", done.requestId, int(done.result),
            reinterpret_cast<std::uintptr_t>(done.space));
  queue_event(done);
  return XR_SUCCESS;
}
XrResult xrGetSpaceUuidFB(XrSpace space, XrUuidEXT* uuid) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(space, "XrSpace")) return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_FB_spatial_entity")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  const auto* found = anchor(space);
  if (!uuid || !found) return XR_ERROR_VALIDATION_FAILURE;
  *uuid = found->uuid;
  return XR_SUCCESS;
}
XrResult xrEnumerateSpaceSupportedComponentsFB(XrSpace space, uint32_t componentTypeCapacityInput,
                                               uint32_t* componentTypeCountOutput,
                                               XrSpaceComponentTypeFB* componentTypes) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(space, "XrSpace")) return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_FB_spatial_entity")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!componentTypeCountOutput || (componentTypeCapacityInput && !componentTypes)) return XR_ERROR_VALIDATION_FAILURE;
  // Reference and action spaces are not spatial entities and have no components.
  std::vector<XrSpaceComponentTypeFB> types;
  if (anchor(space)) {
    types.push_back(XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB);
    if (enabled("XR_FB_spatial_entity_storage")) types.push_back(XR_SPACE_COMPONENT_TYPE_STORABLE_FB);
  }
  *componentTypeCountOutput = std::uint32_t(types.size());
  if (!componentTypeCapacityInput) return XR_SUCCESS;
  if (componentTypeCapacityInput < types.size()) return XR_ERROR_SIZE_INSUFFICIENT;
  std::copy(types.begin(), types.end(), componentTypes);
  return XR_SUCCESS;
}
XrResult xrSetSpaceComponentStatusFB(XrSpace space, const XrSpaceComponentStatusSetInfoFB* info,
                                     XrAsyncRequestIdFB* requestId) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(space, "XrSpace")) return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_FB_spatial_entity")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!requestId || !info || info->type != XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB) return XR_ERROR_VALIDATION_FAILURE;
  auto* found = anchor(space);
  if (!found || !anchor_component(info->componentType)) return XR_ERROR_SPACE_COMPONENT_NOT_SUPPORTED_FB;
  auto& status = info->componentType == XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB ? found->locatable : found->storable;
  if (status == bool(info->enabled)) return XR_ERROR_SPACE_COMPONENT_STATUS_ALREADY_SET_FB;
  status = info->enabled;
  XrEventDataSpaceSetStatusCompleteFB done{XR_TYPE_EVENT_DATA_SPACE_SET_STATUS_COMPLETE_FB};
  done.requestId = *requestId = state().next_request++;
  done.result = XR_SUCCESS;
  done.space = space;
  done.uuid = found->uuid;
  done.componentType = info->componentType;
  done.enabled = info->enabled ? XR_TRUE : XR_FALSE;
  queue_event(done);
  return XR_SUCCESS;
}
XrResult xrGetSpaceComponentStatusFB(XrSpace space, XrSpaceComponentTypeFB componentType,
                                     XrSpaceComponentStatusFB* status) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(space, "XrSpace")) return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_FB_spatial_entity")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!status || status->type != XR_TYPE_SPACE_COMPONENT_STATUS_FB) return XR_ERROR_VALIDATION_FAILURE;
  const auto* found = anchor(space);
  if (!found || !anchor_component(componentType)) return XR_ERROR_SPACE_COMPONENT_NOT_SUPPORTED_FB;
  status->enabled = (componentType == XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB ? found->locatable : found->storable)
                        ? XR_TRUE : XR_FALSE;
  status->changePending = XR_FALSE;
  return XR_SUCCESS;
}

XrResult xrQuerySpacesFB(XrSession session, const XrSpaceQueryInfoBaseHeaderFB* info, XrAsyncRequestIdFB* requestId) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(session, "XrSession")) return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_FB_spatial_entity_query")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!requestId || !info || info->type != XR_TYPE_SPACE_QUERY_INFO_FB) return XR_ERROR_VALIDATION_FAILURE;
  const auto& query = *reinterpret_cast<const XrSpaceQueryInfoFB*>(info);
  if (query.queryAction != XR_SPACE_QUERY_ACTION_LOAD_FB) return XR_ERROR_VALIDATION_FAILURE;
  // What a filter selects among the saved anchors. Every saved anchor is in
  // LOCAL storage, has the anchor components and no scene component.
  const auto matches = [](const XrSpaceFilterInfoBaseHeaderFB* filter, const UuidKey& uuid, bool& valid) {
    valid = true;
    for (auto p = reinterpret_cast<const XrBaseInStructure*>(filter ? filter->next : nullptr); p; p = p->next)
      if (p->type == XR_TYPE_SPACE_STORAGE_LOCATION_FILTER_INFO_FB &&
          reinterpret_cast<const XrSpaceStorageLocationFilterInfoFB*>(p)->location != XR_SPACE_STORAGE_LOCATION_LOCAL_FB)
        return false;
    if (!filter) return true;
    if (filter->type == XR_TYPE_SPACE_UUID_FILTER_INFO_FB) {
      const auto& uuids = *reinterpret_cast<const XrSpaceUuidFilterInfoFB*>(filter);
      if (uuids.uuidCount && !uuids.uuids) return valid = false;
      for (std::uint32_t i = 0; i < uuids.uuidCount; ++i)
        if (uuid_key(uuids.uuids[i]) == uuid) return true;
      return false;
    }
    if (filter->type == XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB)
      return anchor_component(reinterpret_cast<const XrSpaceComponentFilterInfoFB*>(filter)->componentType);
    return valid = false;
  };
  std::vector<XrSpaceQueryResultFB> results;
  for (const auto& [uuid, pose] : state().saved_anchors) {
    bool valid = true, excluded = false;
    const bool selected = matches(query.filter, uuid, valid);
    if (!valid) return XR_ERROR_VALIDATION_FAILURE;
    if (query.excludeFilter) {
      excluded = matches(query.excludeFilter, uuid, valid);
      if (!valid) return XR_ERROR_VALIDATION_FAILURE;
    }
    if (!selected || excluded || (query.maxResultCount && results.size() >= query.maxResultCount)) continue;
    // An anchor already loaded in this session is the same space.
    XrSpace space{};
    for (const auto& [handle, live] : state().anchors)
      if (live.session == session && uuid_key(live.uuid) == uuid) space = handle;
    if (!space && XR_FAILED(make_anchor(session, pose, uuid_of(uuid), &space))) continue;
    results.push_back({space, uuid_of(uuid)});
  }
  const auto id = *requestId = state().next_request++;
  log::info("xr", "Space query {} found {} saved anchors{}", id, results.size(),
            query.filter && query.filter->type == XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB ? " (component filter)" : "");
  // Results not retrieved stay bounded.
  while (state().space_queries.size() >= 64) state().space_queries.erase(state().space_queries.begin());
  state().space_queries[id] = std::move(results);
  queue_event(XrEventDataSpaceQueryResultsAvailableFB{XR_TYPE_EVENT_DATA_SPACE_QUERY_RESULTS_AVAILABLE_FB, nullptr, id});
  queue_event(XrEventDataSpaceQueryCompleteFB{XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB, nullptr, id, XR_SUCCESS});
  return XR_SUCCESS;
}
XrResult xrRetrieveSpaceQueryResultsFB(XrSession session, XrAsyncRequestIdFB requestId, XrSpaceQueryResultsFB* results) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(session, "XrSession")) return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_FB_spatial_entity_query")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!results || results->type != XR_TYPE_SPACE_QUERY_RESULTS_FB ||
      (results->resultCapacityInput && !results->results))
    return XR_ERROR_VALIDATION_FAILURE;
  auto found = state().space_queries.find(requestId);
  if (found == state().space_queries.end()) return XR_ERROR_VALIDATION_FAILURE;
  const auto& list = found->second;
  results->resultCountOutput = std::uint32_t(list.size());
  if (!results->resultCapacityInput) return XR_SUCCESS;
  if (results->resultCapacityInput < list.size()) return XR_ERROR_SIZE_INSUFFICIENT;
  std::copy(list.begin(), list.end(), results->results);
  state().space_queries.erase(found);
  return XR_SUCCESS;
}

XrResult xrSaveSpaceFB(XrSession session, const XrSpaceSaveInfoFB* info, XrAsyncRequestIdFB* requestId) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(session, "XrSession")) return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_FB_spatial_entity_storage")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!requestId || !info || info->type != XR_TYPE_SPACE_SAVE_INFO_FB) return XR_ERROR_VALIDATION_FAILURE;
  if (!valid_handle(info->space, "XrSpace")) return XR_ERROR_HANDLE_INVALID;
  if ((info->location != XR_SPACE_STORAGE_LOCATION_LOCAL_FB && info->location != XR_SPACE_STORAGE_LOCATION_CLOUD_FB) ||
      info->persistenceMode != XR_SPACE_PERSISTENCE_MODE_INDEFINITE_FB)
    return XR_ERROR_VALIDATION_FAILURE;
  const auto* found = anchor(info->space);
  if (!found || !found->storable) return XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB;
  XrEventDataSpaceSaveCompleteFB done{XR_TYPE_EVENT_DATA_SPACE_SAVE_COMPLETE_FB};
  done.requestId = *requestId = state().next_request++;
  done.space = info->space;
  done.uuid = found->uuid;
  done.location = info->location;
  // No account cloud: a Quest without anchor sharing declines cloud saves.
  done.result = info->location == XR_SPACE_STORAGE_LOCATION_CLOUD_FB ? XR_ERROR_SPACE_CLOUD_STORAGE_DISABLED_FB : XR_SUCCESS;
  if (XR_SUCCEEDED(done.result)) state().saved_anchors[uuid_key(found->uuid)] = found->pose;
  queue_event(done);
  return XR_SUCCESS;
}
XrResult xrEraseSpaceFB(XrSession session, const XrSpaceEraseInfoFB* info, XrAsyncRequestIdFB* requestId) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(session, "XrSession")) return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_FB_spatial_entity_storage")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!requestId || !info || info->type != XR_TYPE_SPACE_ERASE_INFO_FB) return XR_ERROR_VALIDATION_FAILURE;
  if (!valid_handle(info->space, "XrSpace")) return XR_ERROR_HANDLE_INVALID;
  if (info->location != XR_SPACE_STORAGE_LOCATION_LOCAL_FB && info->location != XR_SPACE_STORAGE_LOCATION_CLOUD_FB)
    return XR_ERROR_VALIDATION_FAILURE;
  const auto* found = anchor(info->space);
  if (!found || !found->storable) return XR_ERROR_SPACE_COMPONENT_NOT_ENABLED_FB;
  XrEventDataSpaceEraseCompleteFB done{XR_TYPE_EVENT_DATA_SPACE_ERASE_COMPLETE_FB};
  done.requestId = *requestId = state().next_request++;
  done.space = info->space;
  done.uuid = found->uuid;
  done.location = info->location;
  done.result = info->location == XR_SPACE_STORAGE_LOCATION_CLOUD_FB ? XR_ERROR_SPACE_CLOUD_STORAGE_DISABLED_FB : XR_SUCCESS;
  if (XR_SUCCEEDED(done.result)) state().saved_anchors.erase(uuid_key(found->uuid));
  queue_event(done);
  return XR_SUCCESS;
}

XrResult xrGetSpaceBoundingBox2DFB(XrSession session, XrSpace space, XrRect2Df* boundingBox2DOutput) {
  std::lock_guard lock(state().mutex);
  return scene_component(session, space, boundingBox2DOutput, "XR_FB_scene");
}
XrResult xrGetSpaceBoundingBox3DFB(XrSession session, XrSpace space, XrRect3DfFB* boundingBox3DOutput) {
  std::lock_guard lock(state().mutex);
  return scene_component(session, space, boundingBox3DOutput, "XR_FB_scene");
}
XrResult xrGetSpaceSemanticLabelsFB(XrSession session, XrSpace space, XrSemanticLabelsFB* semanticLabelsOutput) {
  std::lock_guard lock(state().mutex);
  if (semanticLabelsOutput && semanticLabelsOutput->type != XR_TYPE_SEMANTIC_LABELS_FB)
    return XR_ERROR_VALIDATION_FAILURE;
  return scene_component(session, space, semanticLabelsOutput, "XR_FB_scene");
}
XrResult xrGetSpaceBoundary2DFB(XrSession session, XrSpace space, XrBoundary2DFB* boundary2DOutput) {
  std::lock_guard lock(state().mutex);
  if (boundary2DOutput && boundary2DOutput->type != XR_TYPE_BOUNDARY_2D_FB) return XR_ERROR_VALIDATION_FAILURE;
  return scene_component(session, space, boundary2DOutput, "XR_FB_scene");
}
XrResult xrGetSpaceRoomLayoutFB(XrSession session, XrSpace space, XrRoomLayoutFB* roomLayoutOutput) {
  std::lock_guard lock(state().mutex);
  if (roomLayoutOutput && roomLayoutOutput->type != XR_TYPE_ROOM_LAYOUT_FB) return XR_ERROR_VALIDATION_FAILURE;
  return scene_component(session, space, roomLayoutOutput, "XR_FB_scene");
}
XrResult xrRequestSceneCaptureFB(XrSession session, const XrSceneCaptureRequestInfoFB* info,
                                 XrAsyncRequestIdFB* requestId) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(session, "XrSession")) return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_FB_scene_capture")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!requestId || !info || info->type != XR_TYPE_SCENE_CAPTURE_REQUEST_INFO_FB ||
      (info->requestByteCount && !info->request))
    return XR_ERROR_VALIDATION_FAILURE;
  // There is no Space Setup to capture a room: the request completes without one.
  const auto id = *requestId = state().next_request++;
  log::info("xr", "Scene capture request {} completes without a room (no Space Setup on this host)", id);
  queue_event(XrEventDataSceneCaptureCompleteFB{XR_TYPE_EVENT_DATA_SCENE_CAPTURE_COMPLETE_FB, nullptr, id,
                                                XR_ERROR_RUNTIME_FAILURE});
  return XR_SUCCESS;
}
}  // namespace guest
}  // namespace venture::hle::openxr
