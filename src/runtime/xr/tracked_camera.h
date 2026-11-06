#pragma once
#include "xr/math.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace venture::xr {
// A headset camera reached through OpenVR's IVRTrackedCamera, using the OpenVR
// client the host OpenXR runtime already loaded into this process (SteamVR's
// OpenXR runtime is that client). Every geometric value comes from the API.
struct TrackedCameraView {
  std::uint32_t x = 0, y = 0, width = 0, height = 0; // region of the frame image
  float fx = 0, fy = 0, cx = 0, cy = 0;               // pinhole intrinsics in region pixels
  XrPosef camera_to_head{{0, 0, 0, 1}, {0, 0, 0}};     // camera looks along -Z, +Y up
};
struct TrackedCameraFrame {
  std::vector<std::uint8_t> rgba; // width * height * 4 bytes, sRGB-encoded
  std::uint32_t width = 0, height = 0;
  std::uint64_t sequence = 0;
  XrPosef head{{0, 0, 0, 1}, {0, 0, 0}}; // head at exposure, in the camera's tracking universe
  bool head_valid = false;
};
class TrackedCamera {
 public:
  virtual ~TrackedCamera() = default;
  // Views in eye order: one shown to both eyes, or left and right.
  virtual const std::vector<TrackedCameraView>& views() const = 0;
  // Swaps the newest frame into `frame` when its sequence differs from frame.sequence.
  virtual bool latest(TrackedCameraFrame& frame) = 0;
  // The head pose now, in the camera's tracking universe.
  virtual bool head_now(XrPosef& pose) = 0;
  // The frame size the views' regions and intrinsics are given for.
  virtual std::uint32_t frame_width() const = 0;
  virtual std::uint32_t frame_height() const = 0;
  // Milliseconds since the last new frame (since the stream started before the first).
  virtual std::uint64_t stalled_ms() const = 0;
};
// The host runtime's OpenVR client is loaded here and reports a headset camera
// with a frame size. Opens no stream.
bool tracked_camera_present();
// Starts streaming; nullptr when it cannot, or no frame arrives within first_frame_ms.
std::unique_ptr<TrackedCamera> open_tracked_camera(unsigned first_frame_ms);

// An OpenVR row-major 3x4 rigid transform (rotation and translation) as a pose.
inline XrPosef rigid_pose(const float m[3][4]) {
  const auto r = [&](int i, int c) { return m[i][c]; };
  XrQuaternionf q;
  const float trace = r(0, 0) + r(1, 1) + r(2, 2);
  if (trace > 0) {
    const float s = std::sqrt(trace + 1) * 2;
    q = {(r(2, 1) - r(1, 2)) / s, (r(0, 2) - r(2, 0)) / s, (r(1, 0) - r(0, 1)) / s, s / 4};
  } else if (r(0, 0) > r(1, 1) && r(0, 0) > r(2, 2)) {
    const float s = std::sqrt(1 + r(0, 0) - r(1, 1) - r(2, 2)) * 2;
    q = {s / 4, (r(0, 1) + r(1, 0)) / s, (r(0, 2) + r(2, 0)) / s, (r(2, 1) - r(1, 2)) / s};
  } else if (r(1, 1) > r(2, 2)) {
    const float s = std::sqrt(1 + r(1, 1) - r(0, 0) - r(2, 2)) * 2;
    q = {(r(0, 1) + r(1, 0)) / s, s / 4, (r(1, 2) + r(2, 1)) / s, (r(0, 2) - r(2, 0)) / s};
  } else {
    const float s = std::sqrt(1 + r(2, 2) - r(0, 0) - r(1, 1)) * 2;
    q = {(r(0, 2) + r(2, 0)) / s, (r(1, 2) + r(2, 1)) / s, s / 4, (r(1, 0) - r(0, 1)) / s};
  }
  const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  return {{q.x / n, q.y / n, q.z / n, q.w / n}, {m[0][3], m[1][3], m[2][3]}};
}
// A camera-to-head transform as a pose. Scale is removed; a camera whose
// transform has it looking backwards uses image axes turned half a turn about X
// (+Z forward, +Y down), which is turned back so it looks along -Z.
inline XrPosef tracked_camera_pose(const float m[3][4]) {
  float r[3][4];
  for (int c = 0; c < 3; ++c) {
    const float n = std::sqrt(m[0][c] * m[0][c] + m[1][c] * m[1][c] + m[2][c] * m[2][c]);
    for (int i = 0; i < 3; ++i) r[i][c] = n > 0 ? m[i][c] / n : (i == c ? 1.f : 0.f);
  }
  for (int i = 0; i < 3; ++i) r[i][3] = m[i][3];
  if (r[2][2] < 0) // the camera's forward (-Z) points behind the head (+Z)
    for (int i = 0; i < 3; ++i) { r[i][1] = -r[i][1]; r[i][2] = -r[i][2]; }
  return rigid_pose(r);
}
// Frame regions for OpenVR's Prop_CameraFrameLayout_Int32 (Mono 1, Stereo 2,
// VerticalLayout 16, HorizontalLayout 32), in eye order, each with the OpenVR
// camera index it shows. A vertical stereo frame is top/bottom = left/right,
// and its left camera is index 1.
struct TrackedCameraRegion { std::uint32_t x, y, width, height, camera; };
inline std::vector<TrackedCameraRegion> tracked_camera_regions(std::int32_t layout, std::uint32_t width,
                                                               std::uint32_t height, std::int32_t cameras) {
  if (!(layout & 2) || cameras == 1) return {{0, 0, width, height, 0}};
  if (layout & 16) return {{0, 0, width, height / 2, 1}, {0, height / 2, width, height / 2, 0}};
  return {{0, 0, width / 2, height, 0}, {width / 2, 0, width / 2, height, 1}};
}
} // namespace venture::xr
