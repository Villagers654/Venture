#include "hle/vrapi/layers.h"
#include <algorithm>
#include <limits>
#include <numbers>

namespace venture::hle::vrapi {
namespace {
constexpr float pi = std::numbers::pi_v<float>;
bool close(float a, float b) { return std::abs(a - b) < 0.0001f; }
bool constant_projection(const ProjectionTexture &texture, std::uint32_t flags) {
  // VrApi_Types.h TexCoordsFromTanAngles maps (tanX,tanY,-1,1) to
  // (S,T,Q,_) for texture2DProj. TanAngleMatrixFromUnitSquare in
  // VrApi_Helpers.h can produce zero X/Y scales for a plane at the eye.
  // With constant S/Q,T/Q and a single edge-clamped texel this is a real
  // full-view color layer, even when the coordinates lie outside [0,1].
  // CLIP_TO_TEXTURE_RECT would change that coverage; leave it unsupported.
  if (flags & 16 || texture.rect.x != 0 || texture.rect.y != 0 ||
      texture.rect.width != 1 || texture.rect.height != 1)
    return false;
  const auto &m = texture.tan_angles.m;
  for (int i = 0; i < 3; ++i)
    if (m[i][0] != 0 || m[i][1] != 0 || m[i][3] != 0 || !std::isfinite(m[i][2]))
      return false;
  if (m[2][2] >= 0 || !std::isfinite(m[0][2] / m[2][2]) ||
      !std::isfinite(m[1][2] / m[2][2]))
    return false;
  const auto it = state().rings.find(texture.chain);
  if (it == state().rings.end()) return false;
  const auto &ring = *it->second;
  return ring.desc.width == 1 && ring.desc.height == 1 && ring.desc.face_count == 1 &&
         ring.sampler.wrap_s == TextureWrap::clamp_edge &&
         ring.sampler.wrap_t == TextureWrap::clamp_edge;
}
XrQuaternionf quaternion(const float (&r)[3][3]);
// bottom_up reports VrApi's vertical handedness: T/Q=0 samples memory row 0 in
// every graphics API, and sy > 0 places that row at the local angleDown. The
// texture coordinates are absolute; the frustum is the part `rect` clips.
XrResult fov(const Matrix &m, const Rect &rect, XrFovf &out, XrQuaternionf &orientation,
             bool &bottom_up) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 4; ++j)
      if (!std::isfinite(m.m[i][j])) return XR_ERROR_VALIDATION_FAILURE;
  if (m.m[0][3] != 0 || m.m[1][3] != 0 || m.m[2][3] != 0)
    return XR_ERROR_FEATURE_UNSUPPORTED;
  // VrApi_Helpers.h's TanAngleMatrixFromUnitSquare is a ray homography:
  // H = Q * K * R, where K is an off-axis projection and R rotates the eye
  // ray into the plane's camera basis. Factor those orthogonal rows instead
  // of requiring a front-facing plane. The inverse rotation belongs in the
  // OpenXR view pose; image pixels and their alpha are unchanged.
  auto dot = [](XrVector3f a, XrVector3f b) { return a.x*b.x + a.y*b.y + a.z*b.z; };
  auto scale = [](XrVector3f v, float s) { return XrVector3f{v.x*s, v.y*s, v.z*s}; };
  const XrVector3f row0{m.m[0][0], m.m[0][1], m.m[0][2]};
  const XrVector3f row1{m.m[1][0], m.m[1][1], m.m[1][2]};
  const XrVector3f row2{m.m[2][0], m.m[2][1], m.m[2][2]};
  const float q = std::sqrt(dot(row2, row2));
  if (!std::isfinite(q) || q <= 0) return XR_ERROR_VALIDATION_FAILURE;
  const auto z = scale(row2, -1 / q);
  const float cx = dot(row0, z), cy = dot(row1, z);
  const XrVector3f horizontal{row0.x - cx*z.x, row0.y - cx*z.y, row0.z - cx*z.z};
  const float sx = std::sqrt(dot(horizontal, horizontal));
  if (!std::isfinite(sx) || sx <= 0) return XR_ERROR_VALIDATION_FAILURE;
  const auto x = scale(horizontal, 1 / sx);
  const XrVector3f y{z.y*x.z - z.z*x.y, z.z*x.x - z.x*x.z, z.x*x.y - z.y*x.x};
  const float sy = dot(row1, y);
  if (!std::isfinite(sy) || sy == 0) return XR_ERROR_VALIDATION_FAILURE;
  // Float matrix inversion leaves small non-orthogonal terms. Real shear
  // needs a resampling shader and cannot be encoded by an OpenXR frustum.
  const float tolerance = 8 * std::numeric_limits<float>::epsilon() *
      std::max({sx, std::abs(sy), q});
  if (std::abs(dot(row1, x)) > tolerance) return XR_ERROR_VALIDATION_FAILURE;
  const float rotation[3][3]{{x.x, y.x, z.x}, {x.y, y.y, z.y}, {x.z, y.z, z.z}};
  orientation = quaternion(rotation);
  // VrApi_Helpers.h's ovrMatrix4f_TanAngleMatrixFromProjection retains the
  // projection's Y sign. Vulkan's inverted Y reverses the two vertical limits,
  // while OpenXR's angleUp/angleDown describe the same geometric frustum.
  // Texture coordinates are S/Q,T/Q; unit-square layers need not use Q=1. A
  // ray reaching S/Q = s has horizontal tangent (s*q + cx) / sx.
  const auto y0 = std::atan((rect.y * q + cy) / sy);
  const auto y1 = std::atan(((rect.y + rect.height) * q + cy) / sy);
  out = {std::atan((rect.x * q + cx) / sx), std::atan(((rect.x + rect.width) * q + cx) / sx),
         std::max(y0, y1), std::min(y0, y1)};
  bottom_up = sy > 0;
  return XR_SUCCESS;
}
XrQuaternionf quaternion(const float (&r)[3][3]) {
  XrQuaternionf q{};
  const float trace = r[0][0] + r[1][1] + r[2][2];
  if (trace > 0) {
    const float s = 2 * std::sqrt(trace + 1);
    q = {(r[2][1] - r[1][2]) / s, (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s, s / 4};
  } else {
    int i = r[1][1] > r[0][0] ? 1 : 0;
    if (r[2][2] > r[i][i])
      i = 2;
    int j = (i + 1) % 3, k = (j + 1) % 3;
    float v[3]{}, s = 2 * std::sqrt(1 + r[i][i] - r[j][j] - r[k][k]);
    v[i] = s / 4;
    v[j] = (r[i][j] + r[j][i]) / s;
    v[k] = (r[i][k] + r[k][i]) / s;
    q = {v[0], v[1], v[2], (r[k][j] - r[j][k]) / s};
  }
  return q;
}
// SDK 1.42 VrApi_Types.h: tan-angle rays are transformed into layer-local
// coordinates before the cylinder/cube/equirect lookup. Invert an orthogonal
// affine map, keeping its scale separate. Shears need a compositor shader and
// cannot be represented by an OpenXR analytic layer.
XrResult layer_pose(const Matrix &m, XrPosef head, XrPosef &out, XrVector3f &scale) {
  float lengths[3]{}, rotation[3][3]{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      if (!std::isfinite(m.m[i][j]))
        return XR_ERROR_VALIDATION_FAILURE;
  if (!close(m.m[3][0], 0) || !close(m.m[3][1], 0) || !close(m.m[3][2], 0) || !close(m.m[3][3], 1))
    return XR_ERROR_FEATURE_UNSUPPORTED;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j)
      lengths[i] += m.m[i][j] * m.m[i][j];
    lengths[i] = std::sqrt(lengths[i]);
    if (lengths[i] < 0.00001f)
      return XR_ERROR_VALIDATION_FAILURE;
    for (int j = 0; j < 3; ++j)
      rotation[j][i] = m.m[i][j] / lengths[i];
  }
  for (int i = 0; i < 3; ++i)
    for (int j = i + 1; j < 3; ++j) {
      float dot = 0;
      for (int k = 0; k < 3; ++k)
        dot += rotation[k][i] * rotation[k][j];
      if (!close(dot, 0))
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }
  const float determinant =
      rotation[0][0] * (rotation[1][1] * rotation[2][2] - rotation[1][2] * rotation[2][1]) -
      rotation[0][1] * (rotation[1][0] * rotation[2][2] - rotation[1][2] * rotation[2][0]) +
      rotation[0][2] * (rotation[1][0] * rotation[2][1] - rotation[1][1] * rotation[2][0]);
  if (!close(determinant, 1))
    return XR_ERROR_FEATURE_UNSUPPORTED;
  scale = {1 / lengths[0], 1 / lengths[1], 1 / lengths[2]};
  auto q = quaternion(rotation);
  out = compose(head, {q, rotate(q, {-m.m[0][3] / lengths[0], -m.m[1][3] / lengths[1],
                                     -m.m[2][3] / lengths[2]})});
  return XR_SUCCESS;
}
bool texture_transform(const Matrix &m) {
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      if (!std::isfinite(m.m[i][j]))
        return false;
      if ((i < 2 && (j == i || j == 2)))
        continue;
      if (!close(m.m[i][j], i == j ? 1.f : 0.f))
        return false;
    }
  return m.m[0][0] > 0 && m.m[1][1] > 0;
}
} // namespace
XrResult translate_layer(const LayerHeader &header, const XrView *eyes, LayerImage image,
                         LayerStorage &dst,
                         std::vector<const XrCompositionLayerBaseHeader *> &layers) {
  constexpr std::uint32_t supported_flags = 2 | 4 | 16 | 256 | (1u << 19);
  if (header.flags & ~supported_flags || header.reserved)
    return XR_ERROR_FEATURE_UNSUPPORTED;
  XrCompositionLayerFlags flags = 0;
  // VrApi_Types.h ovrFrameLayerBlend: 1 ONE, 2 SRC_ALPHA, 5 ONE_MINUS_SRC_ALPHA.
  // ONE_MINUS_SRC_ALPHA/SRC_ALPHA (RE4's fades) is source-alpha blending with
  // inverted alpha; the color scale/bias extension inverts it (a' = 1 - a).
  const bool inverse_alpha = header.src_blend == 5 && header.dst_blend == 2;
  if (header.src_blend == 1 && header.dst_blend == 5)
    flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  else if ((header.src_blend == 2 && header.dst_blend == 5) || inverse_alpha)
    flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
            XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
  else if (header.src_blend != 1 || header.dst_blend != 0)
    return XR_ERROR_FEATURE_UNSUPPORTED;
  const bool has_color = inverse_alpha || header.color_scale.x != 1 || header.color_scale.y != 1 ||
                         header.color_scale.z != 1 || header.color_scale.w != 1;
  if (has_color &&
      !host_xr().enabled_extensions.contains("XR_KHR_composition_layer_color_scale_bias"))
    return XR_ERROR_FEATURE_UNSUPPORTED;
  dst.color.colorScale = {header.color_scale.x, header.color_scale.y, header.color_scale.z,
                          inverse_alpha ? -header.color_scale.w : header.color_scale.w};
  dst.color.colorBias = {0, 0, 0, inverse_alpha ? 1.f : 0.f};
  auto append = [&](auto &layer) {
    layer.layerFlags = flags;
    layer.space = (header.flags & 4) ? state().view : state().app;
    if (has_color)
      layer.next = &dst.color;
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer));
  };
  auto head = [&](const RigidPose &p) {
    return (header.flags & 4) ? XrPosef{{0, 0, 0, 1}, {0, 0, 0}} : pose(p.pose);
  };
  // GLES framebuffers store their bottom row first, Vulkan images their top
  // row. Analytic layers keep that API convention, in their content and their
  // placement alike; projection layers follow their own tan-angle matrix.
  const bool api_bottom_up = state().init.graphics_api != vulkan_api;
  if (header.type == 1) {
    auto &src = reinterpret_cast<const Projection &>(header);
    int constant = 0;
    for (int eye = 0; eye < 2; ++eye) {
      auto &t = src.textures[eye];
      auto &view = dst.views[eye];
      XrQuaternionf orientation{0, 0, 0, 1};
      bool bottom_up = api_bottom_up;
      auto r = fov(t.tan_angles, t.rect, view.fov, orientation, bottom_up);
      if (XR_FAILED(r)) {
        if (!constant_projection(t, header.flags)) return r;
        view.fov = eyes[eye].fov;
        ++constant;
      }
      view.pose = compose(compose(head(src.head), eyes[eye].pose), {orientation, {0, 0, 0}});
      r = image(t.chain, t.index, t.rect, eye, bottom_up, view.subImage);
      if (XR_FAILED(r))
        return r;
    }
    dst.projection.viewCount = 2;
    dst.projection.views = dst.views;
    append(dst.projection);
    if (constant == 2) {
      // A constant texel covers every ray of each eye wherever the head is.
      // Head-lock it, as the guest's predicted head pose would let reprojection
      // expose the view's edges.
      dst.projection.space = state().view;
      for (int eye = 0; eye < 2; ++eye) dst.views[eye].pose = eyes[eye].pose;
    }
  } else if (header.type == 3) { // VRAPI_LAYER_TYPE_CYLINDER2
    if (!host_xr().enabled_extensions.contains("XR_KHR_composition_layer_cylinder"))
      return XR_ERROR_FEATURE_UNSUPPORTED;
    auto &src = reinterpret_cast<const Cylinder &>(header);
    for (int eye = 0; eye < 2; ++eye) {
      auto &t = src.textures[eye];
      auto &layer = dst.cylinder[eye];
      XrVector3f scale{};
      if (!texture_transform(t.texture_matrix))
        return XR_ERROR_FEATURE_UNSUPPORTED;
      auto r = layer_pose(t.tan_angles, head(src.head), layer.pose, scale);
      if (XR_FAILED(r))
        return r;
      if (!close(scale.x, scale.z))
        return XR_ERROR_FEATURE_UNSUPPORTED;
      // VrApi's canonical cylinder spans pi radians and +/-30 degrees high.
      float sx = t.texture_matrix.m[0][0], sy = t.texture_matrix.m[1][1];
      float yaw = -pi * ((t.rect.x + t.rect.width / 2 - t.texture_matrix.m[0][2]) / sx - .5f);
      // T = 0 is memory row 0: the cylinder's bottom where rows are reversed,
      // its top where Vulkan keeps memory order.
      float center = 2 * (t.rect.y + t.rect.height / 2 - t.texture_matrix.m[1][2]) / sy - 1;
      float y = (api_bottom_up ? center : -center) * std::tan(pi / 6) * scale.y;
      layer.pose = compose(layer.pose, {{0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}, {0, y, 0}});
      layer.radius = scale.x;
      layer.centralAngle = pi * t.rect.width / sx;
      float height = 2 * std::tan(pi / 6) * scale.y * t.rect.height / sy;
      if (layer.centralAngle <= 0 || layer.centralAngle > 2 * pi || height <= 0)
        return XR_ERROR_VALIDATION_FAILURE;
      layer.aspectRatio = layer.radius * layer.centralAngle / height;
      layer.eyeVisibility = eye ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
      r = image(t.chain, t.index, t.rect, eye, api_bottom_up, layer.subImage);
      if (XR_FAILED(r))
        return r;
      append(layer);
    }
  } else if (header.type == 4) { // VRAPI_LAYER_TYPE_CUBE2
    if (!host_xr().enabled_extensions.contains("XR_KHR_composition_layer_cube"))
      return XR_ERROR_FEATURE_UNSUPPORTED;
    auto &src = reinterpret_cast<const Cube &>(header);
    if (src.offset.x || src.offset.y || src.offset.z)
      return XR_ERROR_FEATURE_UNSUPPORTED;
    XrPosef p{};
    XrVector3f scale{};
    auto r = layer_pose(src.tan_angles, head(src.head), p, scale);
    if (XR_FAILED(r))
      return r;
    if (!close(scale.x, scale.y) || !close(scale.x, scale.z))
      return XR_ERROR_FEATURE_UNSUPPORTED;
    for (int eye = 0; eye < 2; ++eye) {
      auto &t = src.textures[eye];
      auto &layer = dst.cube[eye];
      XrSwapchainSubImage sub{};
      auto ring = state().rings.find(t.chain);
      if (ring == state().rings.end() || ring->second->desc.face_count != 6)
        return XR_ERROR_LAYER_INVALID;
      r = image(t.chain, t.index, {0, 0, 1, 1}, eye, api_bottom_up, sub);
      if (XR_FAILED(r))
        return r;
      layer.swapchain = sub.swapchain;
      layer.imageArrayIndex = sub.imageArrayIndex;
      layer.orientation = p.orientation;
      layer.eyeVisibility = eye ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
      append(layer);
    }
  } else if (header.type == 5) { // VRAPI_LAYER_TYPE_EQUIRECT2
    if (!host_xr().enabled_extensions.contains("XR_KHR_composition_layer_equirect"))
      return XR_ERROR_FEATURE_UNSUPPORTED;
    auto &src = reinterpret_cast<const Equirect &>(header);
    XrPosef p{};
    XrVector3f scale{};
    auto r = layer_pose(src.tan_angles, head(src.head), p, scale);
    if (XR_FAILED(r))
      return r;
    if (!close(scale.x, scale.y) || !close(scale.x, scale.z))
      return XR_ERROR_FEATURE_UNSUPPORTED;
    for (int eye = 0; eye < 2; ++eye) {
      auto &t = src.textures[eye];
      auto &layer = dst.equirect[eye];
      if (!texture_transform(t.texture_matrix) || t.rect.width <= 0 || t.rect.height <= 0)
        return XR_ERROR_FEATURE_UNSUPPORTED;
      layer.pose = p;
      layer.radius = 0;
      layer.eyeVisibility = eye ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
      layer.scale = {t.texture_matrix.m[0][0] / t.rect.width,
                     t.texture_matrix.m[1][1] / t.rect.height};
      layer.bias = {(t.texture_matrix.m[0][2] - t.rect.x) / t.rect.width,
                    (t.texture_matrix.m[1][2] - t.rect.y) / t.rect.height};
      r = image(t.chain, t.index, t.rect, eye, api_bottom_up, layer.subImage);
      if (XR_FAILED(r))
        return r;
      append(layer);
    }
  } else if (header.type == 6) { // VRAPI_LAYER_TYPE_LOADING_ICON2
    auto &src = reinterpret_cast<const LoadingIcon &>(header);
    if (!std::isfinite(src.spin_speed) || !std::isfinite(src.spin_scale) || src.spin_scale <= 0)
      return XR_ERROR_VALIDATION_FAILURE;
    auto r = image(src.chain, src.index, {0, 0, 1, 1}, 0, api_bottom_up, dst.icon.subImage);
    if (XR_FAILED(r))
      return r;
    const double angle = std::remainder(
        seconds_from_time(session().frame.predictedDisplayTime) * src.spin_speed, 2 * pi);
    dst.icon.pose = {{0, 0, float(std::sin(angle / 2)), float(std::cos(angle / 2))}, {0, 0, -1}};
    dst.icon.size = {2 / src.spin_scale, 2 / src.spin_scale};
    dst.icon.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    append(dst.icon);
    dst.icon.space = state().view;
  } else
    return XR_ERROR_FEATURE_UNSUPPORTED;
  return XR_SUCCESS;
}
} // namespace venture::hle::vrapi
