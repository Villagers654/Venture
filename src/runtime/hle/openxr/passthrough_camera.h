#pragma once
#include "hle/openxr/image_upload.h"
#include "hle/openxr/passthrough.h"
#include "xr/tracked_camera.h"
#include <memory>

namespace venture::hle::openxr {
// The camera backend's presentation: the newest camera frame in an internal
// guest swapchain, shown as one plane per camera view at plane_distance in front
// of the camera, placed from the camera's intrinsics and its pose at exposure
// (head pose at exposure x camera-to-head), world-locked in an internal LOCAL
// space so the compositor reprojects it like any scene content. The guest's
// layer opacity becomes the planes' alpha.
class CameraPassthrough {
 public:
  static constexpr float plane_distance = 2.0f; // metres; content at this depth lines up exactly
  static constexpr std::uint64_t stall_ms = 2000;
  // Starts the camera stream, waiting at most first_frame_ms for its first frame.
  bool start(unsigned first_frame_ms);
  bool started() const { return camera_ != nullptr; }
  bool stalled() const { return camera_ && camera_->stalled_ms() > stall_ms; }
  // The planes for one guest reconstruction layer, in guest form.
  XrResult layers(XrSession, float opacity, std::deque<XrCompositionLayerQuad>& storage,
                  std::vector<const XrCompositionLayerBaseHeader*>& out);
  // Releases the session's swapchain and spaces (after the host finished with them).
  XrResult release_session();
  // Also stops the stream.
  XrResult stop();
 private:
  XrResult prepare(XrSession);
  std::unique_ptr<xr::TrackedCamera> camera_;
  xr::TrackedCameraFrame frame_;
  XrSession session_ = XR_NULL_HANDLE;
  XrSwapchain chain_ = XR_NULL_HANDLE;
  XrSpace local_ = XR_NULL_HANDLE, view_ = XR_NULL_HANDLE;
  std::unique_ptr<ImageUpload> upload_;
  std::uint32_t width_ = 0, height_ = 0;
  std::uint64_t uploaded_sequence_ = 0;
  float uploaded_opacity_ = -1;
};
} // namespace venture::hle::openxr
