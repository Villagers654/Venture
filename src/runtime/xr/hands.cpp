#include "xr/hands.h"
#include "xr/math.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace venture::xr {
namespace {
HandModel make_model(bool left) {
  HandModel m{};
  for (auto &p : m.joints)
    p.orientation.w = 1;
  for (auto &r : m.radii)
    r = 0.007f;
  m.parents[0] = XR_HAND_JOINT_WRIST_EXT;
  m.parents[1] = XR_HAND_JOINT_MAX_ENUM_EXT;
  m.joints[0].position = {0, 0, -0.04f};
  m.radii[0] = 0.025f;
  m.radii[1] = 0.022f;
  const float handed = left ? 1.0f : -1.0f;
  const int starts[]{2, 6, 11, 16, 21};
  const int lengths[]{4, 5, 5, 5, 5};
  for (int finger = 0; finger < 5; ++finger) {
    int start = starts[finger];
    float x = handed * (finger == 0 ? 0.032f : (0.027f - 0.018f * (finger - 1)));
    for (int k = 0; k < lengths[finger]; ++k) {
      int j = start + k;
      m.parents[j] = static_cast<XrHandJointEXT>(k ? j - 1 : 1);
      m.joints[j].position = {x + handed * (finger == 0 ? 0.012f * k : 0), 0,
                              -0.025f - 0.025f * k - (finger == 2 ? 0.008f : 0)};
      m.radii[j] = 0.009f - 0.0012f * k;
    }
  }
  // XR_EXT_hand_tracking: each joint's -Z points along its bone toward the
  // tip. The thumb steps sideways as it extends, so its frames turn about +Y
  // onto that bone; runtimes' thumb rotations then drive this skeleton as-is.
  const float thumb = -handed * std::atan2(0.012f, 0.025f);
  for (int j = 2; j <= 5; ++j)
    m.joints[j].orientation = {0, std::sin(thumb / 2), 0, std::cos(thumb / 2)};
  // Six-sided bone segments with simple rigid skin weights. This intentionally
  // modest asset supplies consistent bind geometry for compatibility APIs.
  for (int joint = 0; joint < 26; ++joint) {
    auto parent = m.parents[joint];
    if (parent == XR_HAND_JOINT_MAX_ENUM_EXT)
      continue;
    auto a = m.joints[parent].position, b = m.joints[joint].position;
    const auto base = static_cast<std::int16_t>(m.positions.size());
    for (int end = 0; end < 2; ++end)
      for (int k = 0; k < 6; ++k) {
        float angle = float(k) * std::numbers::pi_v<float> / 3;
        auto p = end ? b : a;
        float radius = m.radii[joint];
        XrVector3f normal{std::cos(angle), std::sin(angle), 0};
        p.x += normal.x * radius;
        p.y += normal.y * radius;
        m.positions.push_back(p);
        m.normals.push_back(normal);
        m.uv.push_back({float(k) / 6, float(end)});
        m.blends.push_back({static_cast<std::int16_t>(parent), -1, -1, -1});
        m.weights.push_back({1, 0, 0, 0});
      }
    for (int k = 0; k < 6; ++k) {
      std::int16_t a0 = base + k, b0 = base + (k + 1) % 6, c0 = base + 6 + k,
                   d0 = base + 6 + (k + 1) % 6;
      for (auto index : {a0, b0, c0, b0, d0, c0})
        m.indices.push_back(index);
    }
  }
  return m;
}
bool valid_joint(const XrHandJointLocationsEXT &joints, int i) {
  constexpr auto valid =
      XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  return joints.isActive && joints.jointLocations && joints.jointCount == 26 &&
         (joints.jointLocations[i].locationFlags & valid) == valid;
}
} // namespace
const HandModel &hand_model(bool left) {
  static const HandModel l = make_model(true), r = make_model(false);
  return left ? l : r;
}
void hand_aim(const XrHandJointLocationsEXT &joints, std::array<bool, 4> &pinching,
              XrHandTrackingAimStateFB &aim) {
  auto next = aim.next;
  aim = {XR_TYPE_HAND_TRACKING_AIM_STATE_FB};
  aim.next = next;
  aim.status = XR_HAND_TRACKING_AIM_COMPUTED_BIT_FB;
  if (!valid_joint(joints, 1) || !valid_joint(joints, 5) || !valid_joint(joints, 10)) {
    pinching.fill(false);
    return;
  }
  aim.status |= XR_HAND_TRACKING_AIM_VALID_BIT_FB;
  aim.aimPose = joints.jointLocations[1].pose;
  aim.aimPose.position = joints.jointLocations[7].pose.position;
  const int tips[]{10, 15, 20, 25};
  float strengths[4]{};
  for (int finger = 0; finger < 4; ++finger) {
    if (!valid_joint(joints, tips[finger])) {
      pinching[finger] = false;
      continue;
    }
    auto thumb = joints.jointLocations[5].pose.position,
         tip = joints.jointLocations[tips[finger]].pose.position;
    float gap = length({thumb.x - tip.x, thumb.y - tip.y, thumb.z - tip.z}) -
                joints.jointLocations[5].radius - joints.jointLocations[tips[finger]].radius;
    strengths[finger] = std::clamp(1.0f - gap / 0.04f, 0.0f, 1.0f);
    pinching[finger] = strengths[finger] > (pinching[finger] ? 0.65f : 0.8f);
    if (pinching[finger])
      aim.status |= XrHandTrackingAimFlagsFB(4) << finger;
  }
  aim.pinchStrengthIndex = strengths[0];
  aim.pinchStrengthMiddle = strengths[1];
  aim.pinchStrengthRing = strengths[2];
  aim.pinchStrengthLittle = strengths[3];
}
void hand_capsules(const XrHandJointLocationsEXT &joints, XrHandTrackingCapsulesStateFB &out) {
  const int starts[19]{1, 2, 3, 4, 6, 7, 8, 9, 11, 12, 13, 14, 16, 17, 18, 19, 21, 22, 23};
  for (int n = 0; n < 19; ++n) {
    int a = starts[n], b = a == 1 ? 0 : a + 1;
    auto &c = out.capsules[n];
    c = {};
    c.joint = static_cast<XrHandJointEXT>(a);
    if (!valid_joint(joints, a) || !valid_joint(joints, b))
      continue;
    c.points[0] = joints.jointLocations[a].pose.position;
    c.points[1] = joints.jointLocations[b].pose.position;
    c.radius = joints.jointLocations[a].radius;
  }
}
XrResult hand_scale(bool left, XrHandJointLocationsEXT &joints, XrHandJointVelocitiesEXT *velocities,
                    XrHandTrackingScaleFB &scale) {
  if (scale.overrideHandScale &&
      (!std::isfinite(scale.overrideValueInput) || scale.overrideValueInput <= 0))
    return XR_ERROR_VALIDATION_FAILURE;
  scale.sensorOutput = scale.currentOutput = 1;
  if (!valid_joint(joints, XR_HAND_JOINT_WRIST_EXT)) return XR_SUCCESS;
  const auto &model = hand_model(left);
  const auto wrist = joints.jointLocations[XR_HAND_JOINT_WRIST_EXT].pose.position;
  // Estimate uniform size using palm bones, which do not change with finger
  // curl. This scale belongs to our independent fallback mesh, not a vendor
  // sensor calibration. XR_FB_hand_tracking_mesh defines override 1 as bind size.
  float measured = 0, bind = 0;
  for (int index : {6, 11, 16, 21}) {
    if (!valid_joint(joints, index)) continue;
    const auto p = joints.jointLocations[index].pose.position;
    const auto b = model.joints[index].position;
    measured += length({p.x - wrist.x, p.y - wrist.y, p.z - wrist.z});
    bind += length(b);
  }
  if (bind <= 0 || measured <= 0 || !std::isfinite(measured)) return XR_SUCCESS;
  scale.sensorOutput = measured / bind;
  scale.currentOutput = scale.overrideHandScale ? scale.overrideValueInput : scale.sensorOutput;
  if (!scale.overrideHandScale) return XR_SUCCESS;
  const float ratio = scale.currentOutput / scale.sensorOutput;
  const auto wristVelocity = velocities ? velocities->jointVelocities[1] : XrHandJointVelocityEXT{};
  for (std::uint32_t index = 0; index < joints.jointCount; ++index) {
    auto &joint = joints.jointLocations[index];
    if (joint.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
      auto &p = joint.pose.position;
      p = {wrist.x + (p.x - wrist.x) * ratio, wrist.y + (p.y - wrist.y) * ratio,
           wrist.z + (p.z - wrist.z) * ratio};
      joint.radius *= ratio;
    }
    if (velocities) {
      auto &velocity = velocities->jointVelocities[index];
      if ((velocity.velocityFlags & wristVelocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0) {
        auto &v = velocity.linearVelocity;
        const auto w = wristVelocity.linearVelocity;
        v = {w.x + (v.x - w.x) * ratio, w.y + (v.y - w.y) * ratio, w.z + (v.z - w.z) * ratio};
      } else velocity.velocityFlags &= ~XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
    }
  }
  return XR_SUCCESS;
}
XrResult copy_hand_mesh(const HandModel &model, XrHandTrackingMeshFB *out) {
  if (!out || out->type != XR_TYPE_HAND_TRACKING_MESH_FB)
    return XR_ERROR_VALIDATION_FAILURE;
  // No recognized mesh output extensions: OpenXR requires ignoring unknown
  // next-chain structures and preserving their contents, type and next.
  out->jointCountOutput = 26;
  out->vertexCountOutput = std::uint32_t(model.positions.size());
  out->indexCountOutput = std::uint32_t(model.indices.size());
  // OpenXR multi-array enumeration: a zero capacity in any array requests
  // the sizes of all arrays, ignoring every capacity and data pointer.
  if (!out->jointCapacityInput || !out->vertexCapacityInput || !out->indexCapacityInput)
    return XR_SUCCESS;
  if ((out->jointCapacityInput && out->jointCapacityInput < 26) ||
      (out->vertexCapacityInput && out->vertexCapacityInput < model.positions.size()) ||
      (out->indexCapacityInput && out->indexCapacityInput < model.indices.size()))
    return XR_ERROR_SIZE_INSUFFICIENT;
  if (out->jointCapacityInput) {
    if (!out->jointBindPoses || !out->jointRadii || !out->jointParents)
      return XR_ERROR_VALIDATION_FAILURE;
    std::copy(model.joints.begin(), model.joints.end(), out->jointBindPoses);
    std::copy(model.radii.begin(), model.radii.end(), out->jointRadii);
    std::copy(model.parents.begin(), model.parents.end(), out->jointParents);
  }
  if (out->vertexCapacityInput) {
    if (!out->vertexPositions || !out->vertexNormals || !out->vertexUVs ||
        !out->vertexBlendIndices || !out->vertexBlendWeights)
      return XR_ERROR_VALIDATION_FAILURE;
    std::copy(model.positions.begin(), model.positions.end(), out->vertexPositions);
    std::copy(model.normals.begin(), model.normals.end(), out->vertexNormals);
    std::copy(model.uv.begin(), model.uv.end(), out->vertexUVs);
    std::copy(model.blends.begin(), model.blends.end(), out->vertexBlendIndices);
    std::copy(model.weights.begin(), model.weights.end(), out->vertexBlendWeights);
  }
  if (out->indexCapacityInput) {
    if (!out->indices)
      return XR_ERROR_VALIDATION_FAILURE;
    std::copy(model.indices.begin(), model.indices.end(), out->indices);
  }
  return XR_SUCCESS;
}
} // namespace venture::xr
