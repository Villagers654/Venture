#pragma once
#include "xr/api.h"
#include <array>
#include <vector>
namespace venture::xr {
struct HandModel {
  std::array<XrPosef, 26> joints;
  std::array<XrHandJointEXT, 26> parents;
  std::array<float, 26> radii;
  std::vector<XrVector3f> positions, normals;
  std::vector<XrVector2f> uv;
  std::vector<XrVector4sFB> blends;
  std::vector<XrVector4f> weights;
  std::vector<std::int16_t> indices;
};
// Independently authored low-poly hand geometry. No Quest mesh is copied and
// EXT joints are not claimed to provide a runtime mesh.
const HandModel &hand_model(bool left);
void hand_aim(const XrHandJointLocationsEXT &, std::array<bool, 4> &pinching,
              XrHandTrackingAimStateFB &);
void hand_capsules(const XrHandJointLocationsEXT &, XrHandTrackingCapsulesStateFB &);
// Scale fallback joints about the wrist against our bind skeleton. Native FB
// mesh/scale implementations are forwarded to the host instead.
XrResult hand_scale(bool left, XrHandJointLocationsEXT &, XrHandJointVelocitiesEXT *,
                    XrHandTrackingScaleFB &);
XrResult copy_hand_mesh(const HandModel &, XrHandTrackingMeshFB *);
// One device per hand, as on a Quest: a tracked free hand replaces that side's controller. Without
// the data-source extension a present controller wins over a skeleton the host may derive from it,
// unless the app requires hand tracking; then the host's hands are the only input it accepts.
inline bool hand_replaces_controller(bool hand_active, bool sourced, bool controller_active,
                                     bool hands_required) {
  return hand_active && (sourced || hands_required || !controller_active);
}
inline constexpr int ovr_joint_map[24]{1,  1,  2,  2,  3,  4,  7,  8, 9,  12, 13, 14,
                                       17, 18, 19, 21, 22, 23, 24, 5, 10, 15, 20, 25};
inline constexpr int ovr_parent_map[24]{-1, 0,  0,  2, 3,  4,  0,  6, 7, 0,  9,  10,
                                        0,  12, 13, 0, 15, 16, 17, 5, 8, 11, 14, 18};
} // namespace venture::xr
