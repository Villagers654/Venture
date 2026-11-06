#include "hle/openxr/passthrough_camera.h"
#include "hle/openxr/state.h"
#include "common/log.h"
#include "xr/clock.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace venture::hle::openxr {
bool CameraPassthrough::start(unsigned first_frame_ms) {
  if (!camera_) camera_ = xr::open_tracked_camera(first_frame_ms);
  return camera_ != nullptr;
}
XrResult CameraPassthrough::release_session() {
  XrResult result = XR_SUCCESS;
  if (chain_) {
    result = guest::xrDestroySwapchain(chain_);
    chain_ = XR_NULL_HANDLE;
  }
  for (auto* space : {&local_, &view_})
    if (*space) { forget_handle(*space); ::xrDestroySpace(*space); *space = XR_NULL_HANDLE; }
  upload_.reset();
  session_ = XR_NULL_HANDLE;
  width_ = height_ = 0;
  uploaded_sequence_ = 0;
  uploaded_opacity_ = -1;
  return result;
}
XrResult CameraPassthrough::stop() {
  const auto result = release_session();
  camera_.reset();
  frame_ = {};
  return result;
}
XrResult CameraPassthrough::prepare(XrSession target) {
  if (session_ != target || width_ != frame_.width || height_ != frame_.height) {
    session().finish_host_end();
    const auto result = release_session();
    if (XR_FAILED(result)) return result;
  }
  if (chain_) return XR_SUCCESS;
  session_ = target;
  // Host spaces (guest spaces are host handles): the planes are placed and
  // submitted in the same host LOCAL space the head is located in.
  for (auto [space, type] : {std::pair{&local_, XR_REFERENCE_SPACE_TYPE_LOCAL}, std::pair{&view_, XR_REFERENCE_SPACE_TYPE_VIEW}}) {
    XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, type, {{0, 0, 0, 1}, {0, 0, 0}}};
    const auto result = ::xrCreateReferenceSpace(target, &info, space);
    if (XR_FAILED(result)) { *space = XR_NULL_HANDLE; return result; }
    track_handle(*space, "XrSpace", target);
  }
  // The camera's bytes are sRGB-encoded, like an Android Surface's.
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                    XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  info.format = VK_FORMAT_R8G8B8A8_SRGB;
  info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
  info.width = frame_.width;
  info.height = frame_.height;
  auto result = guest::create_swapchain(target, &info, &chain_, true);
  if (XR_FAILED(result)) { chain_ = XR_NULL_HANDLE; return result; }
  upload_ = std::make_unique<ImageUpload>();
  result = upload_->initialize(frame_.width, frame_.height);
  if (XR_FAILED(result)) return result;
  width_ = frame_.width;
  height_ = frame_.height;
  return XR_SUCCESS;
}
XrResult CameraPassthrough::layers(XrSession target, float opacity, std::deque<XrCompositionLayerQuad>& storage,
                                   std::vector<const XrCompositionLayerBaseHeader*>& out) {
  if (!camera_) return XR_SUCCESS;
  const bool fresh = camera_->latest(frame_);
  if (!frame_.sequence || !frame_.width || !frame_.height ||
      frame_.rgba.size() != std::size_t(frame_.width) * frame_.height * 4) return XR_SUCCESS;
  auto result = prepare(target);
  if (XR_FAILED(result)) return result;
  const auto alpha = std::uint8_t(std::lround(std::clamp(opacity, 0.f, 1.f) * 255));
  if (fresh || uploaded_sequence_ != frame_.sequence || uploaded_opacity_ != opacity) {
    auto* pixels = upload_->pixels();
    std::memcpy(pixels, frame_.rgba.data(), frame_.rgba.size());
    for (std::size_t i = 3; i < frame_.rgba.size(); i += 4) pixels[i] = alpha;
    result = upload_->publish(state().swapchains.at(chain_));
    if (XR_FAILED(result)) return result;
    uploaded_sequence_ = frame_.sequence;
    uploaded_opacity_ = opacity;
  }
  // World-locked placement: the head pose now in LOCAL, the head's motion
  // since exposure, and the camera's pose on the head.
  XrTime now = 0;
  std::int64_t monotonic = 0;
  if (XR_FAILED(xr::clock_sample(&now, &monotonic))) return XR_SUCCESS;
  XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
  constexpr auto tracked = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
  if (XR_FAILED(::xrLocateSpace(view_, local_, now, &head)) || (head.locationFlags & tracked) != tracked)
    return XR_SUCCESS;
  XrPosef motion{{0, 0, 0, 1}, {0, 0, 0}}, head_now{};
  if (frame_.head_valid && camera_->head_now(head_now)) motion = xr::compose(xr::inverse(head_now), frame_.head);
  const auto& views = camera_->views();
  const float sx = camera_->frame_width() ? float(frame_.width) / float(camera_->frame_width()) : 1.f;
  const float sy = camera_->frame_height() ? float(frame_.height) / float(camera_->frame_height()) : 1.f;
  const XrEyeVisibility eyes[] = {XR_EYE_VISIBILITY_LEFT, XR_EYE_VISIBILITY_RIGHT};
  for (std::size_t i = 0; i < views.size() && i < 2; ++i) {
    const auto& view = views[i];
    const float w = float(view.width) * sx, h = float(view.height) * sy;
    const float fx = view.fx * sx, fy = view.fy * sy, cx = view.cx * sx, cy = view.cy * sy;
    if (w < 1 || h < 1 || fx <= 0 || fy <= 0) continue;
    const auto camera = xr::compose(head.pose, xr::compose(motion, view.camera_to_head));
    // The image centre's ray, from the principal point (image rows run down).
    const float d = plane_distance;
    const XrPosef plane{{0, 0, 0, 1}, {(w / 2 - cx) / fx * d, -(h / 2 - cy) / fy * d, -d}};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.layerFlags = alpha < 255 ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                        XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT : 0;
    quad.space = local_;
    quad.eyeVisibility = views.size() == 1 ? XR_EYE_VISIBILITY_BOTH : eyes[i];
    quad.subImage.swapchain = chain_;
    quad.subImage.imageRect = {{std::int32_t(std::lround(float(view.x) * sx)), std::int32_t(std::lround(float(view.y) * sy))},
                               {std::int32_t(std::lround(w)), std::int32_t(std::lround(h))}};
    quad.pose = xr::compose(camera, plane);
    quad.size = {w / fx * d, h / fy * d};
    storage.push_back(quad);
    out.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&storage.back()));
  }
  return XR_SUCCESS;
}
} // namespace venture::hle::openxr
