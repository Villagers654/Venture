#include "xr/reference_spaces.h"
#include "common/log.h"
#include "xr/clock.h"
#include "xr/math.h"
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace venture::xr {
namespace {
std::mutex mutex;
// Per session and requested type: nullopt when the host's own space is upright,
// else the replacement origin in STAGE.
std::map<std::pair<XrSession, XrReferenceSpaceType>, std::optional<XrPosef>> spaces;

XrSpace create(XrSession session, XrReferenceSpaceType type) {
  XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, type, {{0, 0, 0, 1}, {0, 0, 0}}};
  XrSpace space{};
  return XR_SUCCEEDED(xrCreateReferenceSpace(session, &info, &space)) ? space : XR_NULL_HANDLE;
}

bool locate(XrSpace space, XrSpace base, XrTime time, XrSpaceLocationFlags needed, XrPosef& pose) {
  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
  if (!space || !base || XR_FAILED(xrLocateSpace(space, base, time, &location)) ||
      (location.locationFlags & needed) != needed)
    return false;
  pose = location.pose;
  return true;
}

std::optional<XrPosef> measure(XrSession session, XrReferenceSpaceType type) {
  XrTime now{};
  std::int64_t monotonic{};
  if (XR_FAILED(clock_sample(&now, &monotonic))) return std::nullopt;
  const auto space = create(session, type), stage = create(session, XR_REFERENCE_SPACE_TYPE_STAGE),
             view = create(session, XR_REFERENCE_SPACE_TYPE_VIEW);
  std::optional<XrPosef> origin;
  XrPosef where{}, head{};
  // A space the host cannot place against STAGE is left as it is.
  if (locate(space, stage, now, XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT, where)) {
    const auto up = rotate(where.orientation, {0, 1, 0});
    if (up.y < 0.99f) {
      // Head position and heading when tracked, else the host origin's place.
      origin = locate(view, stage, now, XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT, head)
                   ? XrPosef{yaw_only(head.orientation), head.position}
                   : XrPosef{{0, 0, 0, 1}, where.position};
      if (type == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR) origin->position.y = 0;
      log::warn("xr", "Host reference space {} is not upright (up [{:+.2f},{:+.2f},{:+.2f}] in STAGE); the game gets "
                "STAGE at [{:.2f},{:.2f},{:.2f}] instead", int(type), up.x, up.y, up.z, origin->position.x,
                origin->position.y, origin->position.z);
    }
  }
  for (auto s : {space, stage, view})
    if (s) xrDestroySpace(s);
  return origin;
}
}  // namespace

XrQuaternionf yaw_only(XrQuaternionf q) {
  const auto forward = rotate(q, {0, 0, -1});
  const auto yaw = std::atan2(-forward.x, -forward.z);
  // Looking straight up or down has no heading; atan2(0, 0) keeps it at zero.
  return {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
}

XrReferenceSpaceCreateInfo upright_reference_space(XrSession session, XrReferenceSpaceCreateInfo info) {
  if (info.referenceSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL &&
      info.referenceSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR)
    return info;
  std::optional<XrPosef> origin;
  {
    std::lock_guard lock(mutex);
    const auto key = std::pair(session, info.referenceSpaceType);
    auto it = spaces.find(key);
    if (it == spaces.end()) it = spaces.emplace(key, measure(session, info.referenceSpaceType)).first;
    origin = it->second;
  }
  if (origin) {
    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    info.poseInReferenceSpace = compose(*origin, info.poseInReferenceSpace);
  }
  return info;
}

void forget_reference_spaces(XrSession session) {
  std::lock_guard lock(mutex);
  std::erase_if(spaces, [session](const auto& entry) { return entry.first.first == session; });
}
} // namespace venture::xr
