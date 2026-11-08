#include "hle/vrapi/layers.h"
#include "common/log.h"
#include <algorithm>
#include <format>
#include <mutex>
#include <set>
#include <string>
#include <limits>
#include <numbers>
#include <utility>

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
// The Meta PC runtime over Link (OpenXR runtime "Oculus" 1.208, Quest 3)
// shows a projection view's image rect across the display's own field of
// view, whatever fov the view sends. A wider frustum, such as the symmetric one
// SUGGESTED_EYE_FOV describes (-54..+54 degrees for a -54..+40 eye), then
// reached each eye stretched and about 15 degrees off, outwards: a cross-eyed
// world. Narrow the rect to the
// part inside the host's view; fov() then gives that part's frustum, the same
// rays on every runtime. A side where the guest frustum is the narrower keeps
// its edge, and the result is false: no projection view shows such a frustum
// alike everywhere. A rotated plane's tangents are not the eye's: leave it.
bool fit_host_view(Rect &rect, const XrFovf &guest, bool bottom_up, XrQuaternionf orientation,
                   const XrFovf &host) {
  if (!(host.angleLeft < host.angleRight && host.angleDown < host.angleUp) ||
      1 - std::abs(orientation.w) > 1e-6f)
    return true;
  const float l = std::tan(guest.angleLeft), r = std::tan(guest.angleRight);
  const float u = std::tan(guest.angleUp), d = std::tan(guest.angleDown);
  // S/Q and T/Q are affine in a ray's tangents (fov()).
  auto part = [](float host_tan, float low, float high) { return (host_tan - low) / (high - low); };
  float s0 = part(std::tan(host.angleLeft), l, r), s1 = part(std::tan(host.angleRight), l, r);
  // bottom_up: T grows upwards, from the frustum's bottom edge.
  float t0 = part(std::tan(host.angleDown), d, u), t1 = part(std::tan(host.angleUp), d, u);
  if (!bottom_up) {
    t0 = 1 - t0;
    t1 = 1 - t1;
    std::swap(t0, t1);
  }
  constexpr float slack = 1e-4f; // float noise of a frustum equal to the host's
  const bool spans = s0 > -slack && t0 > -slack && s1 < 1 + slack && t1 < 1 + slack;
  s0 = std::max(s0, 0.f);
  t0 = std::max(t0, 0.f);
  s1 = std::min(s1, 1.f);
  t1 = std::min(t1, 1.f);
  if (s1 <= s0 || t1 <= t0)
    return spans;
  rect = {rect.x + s0 * rect.width, rect.y + t0 * rect.height, (s1 - s0) * rect.width,
          (t1 - t0) * rect.height};
  return spans;
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
// The host layers that show this layer exactly, or an error with dst.reason.
static XrResult translate_native(const LayerHeader &header, const XrView *eyes, LayerImage image,
                          LayerStorage &dst,
                          std::vector<const XrCompositionLayerBaseHeader *> &layers, int first_eye) {
  auto unsupported = [&](const char *reason) {
    dst.reason = reason;
    return XR_ERROR_FEATURE_UNSUPPORTED;
  };
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
    return unsupported("blend factors without a host layer form");
  const bool has_color = inverse_alpha || header.color_scale.x != 1 || header.color_scale.y != 1 ||
                         header.color_scale.z != 1 || header.color_scale.w != 1;
  if (has_color &&
      !host_xr().enabled_extensions.contains("XR_KHR_composition_layer_color_scale_bias"))
    return unsupported("color scale without XR_KHR_composition_layer_color_scale_bias");
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
  // The plane a view's rays reach through its frustum, `distance` metres
  // ahead: the same rays and image, shown to that eye only.
  auto eye_quad = [&](int eye, float distance = 1) {
    const auto &view = dst.views[eye];
    const float l = std::tan(view.fov.angleLeft), r = std::tan(view.fov.angleRight);
    const float u = std::tan(view.fov.angleUp), d = std::tan(view.fov.angleDown);
    auto &quad = dst.quad[eye];
    quad.pose = compose(view.pose, {{0, 0, 0, 1}, {(l + r) / 2 * distance, (u + d) / 2 * distance, -distance}});
    quad.size = {(r - l) * distance, (u - d) * distance};
    quad.subImage = view.subImage;
    quad.eyeVisibility = eye ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
    append(quad);
  };
  if (header.type == 17) {
    // OVRPlugin's quad (UE4 stereo layers: Moss, The Walking Dead menus) uses
    // the cylinder layout with TanAngleMatrixFromUnitSquare matrices, the
    // projection layer's ray homography. Each eye sees the rays its matrix maps.
    auto &src = reinterpret_cast<const Cylinder &>(header);
    for (int eye = 0; eye < 2; ++eye) {
      auto &t = src.textures[eye];
      const auto &tm = t.texture_matrix.m;
      if (!texture_transform(t.texture_matrix) || tm[0][0] != 1 || tm[1][1] != 1 || tm[0][2] != 0 || tm[1][2] != 0)
        return unsupported("quad texture matrix other than identity");
      auto &view = dst.views[eye];
      XrQuaternionf orientation{0, 0, 0, 1};
      bool bottom_up = api_bottom_up;
      auto r = fov(t.tan_angles, t.rect, view.fov, orientation, bottom_up);
      if (XR_FAILED(r))
        return dst.reason = "quad tan-angle matrix is not an off-axis frustum", r;
      view.pose = compose(compose(head(src.head), eyes[eye].pose), {orientation, {0, 0, 0}});
      r = image(t.chain, t.index, t.rect, eye, bottom_up, view.subImage);
      if (XR_FAILED(r))
        return r;
      eye_quad(eye);
    }
  } else if (header.type == 1) {
    auto &src = reinterpret_cast<const Projection &>(header);
    int constant = 0;
    bool spans = true;
    for (int i = 0; i < 2; ++i) {
      const int eye = first_eye == 1 ? 1 - i : i;
      auto &t = src.textures[eye];
      auto &view = dst.views[eye];
      XrQuaternionf orientation{0, 0, 0, 1};
      bool bottom_up = api_bottom_up;
      Rect rect = t.rect;
      auto r = fov(t.tan_angles, rect, view.fov, orientation, bottom_up);
      if (XR_FAILED(r)) {
        if (!constant_projection(t, header.flags))
          return dst.reason = "projection tan-angle matrix is not an off-axis frustum", r;
        view.fov = eyes[eye].fov;
        ++constant;
      } else {
        spans = fit_host_view(rect, view.fov, bottom_up, orientation, eyes[eye].fov) && spans;
        r = fov(t.tan_angles, rect, view.fov, orientation, bottom_up);
        if (XR_FAILED(r)) return r;
      }
      view.pose = compose(compose(head(src.head), eyes[eye].pose), {orientation, {0, 0, 0}});
      r = image(t.chain, t.index, rect, eye, bottom_up, view.subImage);
      if (XR_FAILED(r))
        return r;
    }
    if ((header.flags & 4) && !constant) {
      // A head-locked projection is a plane fixed in front of each eye: submit
      // it as one head-locked quad per eye, with the same rays and image.
      // Virtual Desktop 1.34 shows the whole frame as a head-locked panel of
      // about 90 degrees when any projection layer is in VIEW space (The
      // Climb 2's loading layer stays in the frame after loading).
      for (int eye = 0; eye < 2; ++eye) eye_quad(eye);
      return XR_SUCCESS;
    }
    if (!spans && !constant) {
      // A frustum narrower than the host's view (The Climb 2's 45-degree
      // credits) is a plane in front of each eye too; the Meta runtime would
      // stretch it over the display (fit_host_view). Place it far, where
      // reprojection moves it about as little as a projection without depth.
      for (int eye = 0; eye < 2; ++eye) eye_quad(eye, 10);
      return XR_SUCCESS;
    }
    if (constant == 2) {
      // A constant texel covers every ray of each eye wherever the head is.
      // Head-lock it, as the guest's predicted head pose would let reprojection
      // expose the view's edges: one quad per eye over that eye's whole view,
      // not a projection layer, which Virtual Desktop would show as a
      // head-locked panel surrounded by black (#63). OVRPlugin's transition
      // layer (The Walking Dead, Tales from the Galaxy's Edge) takes this form.
      for (int eye = 0; eye < 2; ++eye) {
        dst.views[eye].pose = eyes[eye].pose;
        eye_quad(eye);
        dst.quad[eye].space = state().view;
      }
      return XR_SUCCESS;
    }
    dst.projection.viewCount = 2;
    dst.projection.views = dst.views;
    append(dst.projection);
  } else if (header.type == 3) { // VRAPI_LAYER_TYPE_CYLINDER2
    if (!host_xr().enabled_extensions.contains("XR_KHR_composition_layer_cylinder"))
      return unsupported("host lacks XR_KHR_composition_layer_cylinder");
    auto &src = reinterpret_cast<const Cylinder &>(header);
    for (int eye = 0; eye < 2; ++eye) {
      auto &t = src.textures[eye];
      auto &layer = dst.cylinder[eye];
      XrVector3f scale{};
      if (!texture_transform(t.texture_matrix))
        return unsupported("cylinder texture matrix is not a positive scale and offset");
      auto r = layer_pose(t.tan_angles, head(src.head), layer.pose, scale);
      if (XR_FAILED(r))
        return dst.reason = "cylinder matrix is not a rotation, translation and scale", r;
      if (!close(scale.x, scale.z))
        return unsupported("elliptical cylinder (x and z radii differ)");
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
        return dst.reason = "cylinder arc outside (0, 2 pi] or no height", XR_ERROR_VALIDATION_FAILURE;
      layer.aspectRatio = layer.radius * layer.centralAngle / height;
      layer.eyeVisibility = eye ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
      r = image(t.chain, t.index, t.rect, eye, api_bottom_up, layer.subImage);
      if (XR_FAILED(r))
        return r;
      append(layer);
    }
  } else if (header.type == 4) { // VRAPI_LAYER_TYPE_CUBE2
    if (!host_xr().enabled_extensions.contains("XR_KHR_composition_layer_cube"))
      return unsupported("host lacks XR_KHR_composition_layer_cube");
    auto &src = reinterpret_cast<const Cube &>(header);
    if (src.offset.x || src.offset.y || src.offset.z)
      return unsupported("cube offset");
    XrPosef p{};
    XrVector3f scale{};
    auto r = layer_pose(src.tan_angles, head(src.head), p, scale);
    if (XR_FAILED(r))
      return dst.reason = "cube matrix is not a rotation, translation and scale", r;
    if (!close(scale.x, scale.y) || !close(scale.x, scale.z))
      return unsupported("cube matrix scales its axes differently");
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
      return unsupported("host lacks XR_KHR_composition_layer_equirect");
    auto &src = reinterpret_cast<const Equirect &>(header);
    XrPosef p{};
    XrVector3f scale{};
    auto r = layer_pose(src.tan_angles, head(src.head), p, scale);
    if (XR_FAILED(r))
      return dst.reason = "equirect matrix is not a rotation, translation and scale", r;
    if (!close(scale.x, scale.y) || !close(scale.x, scale.z))
      return unsupported("equirect matrix scales its axes differently");
    for (int eye = 0; eye < 2; ++eye) {
      auto &t = src.textures[eye];
      auto &layer = dst.equirect[eye];
      if (!texture_transform(t.texture_matrix) || t.rect.width <= 0 || t.rect.height <= 0)
        return unsupported("equirect texture matrix is not a positive scale and offset");
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
    return unsupported("unknown layer type");
  return XR_SUCCESS;
}
// The layer drawn into an image per eye (drawn_layers.h), from the same
// fields VrApi's compositor reads.
static XrResult draw_layer(const LayerHeader &header, const XrView *eyes, LayerStorage &dst, DrawnLayers &drawn,
                    std::vector<const XrCompositionLayerBaseHeader *> &layers) {
  auto factor = [](std::uint32_t f) { return f == 0 || f == 1 || f == 2 || f == 5; };
  if (!factor(header.src_blend) || !factor(header.dst_blend)) {
    dst.reason = "unknown blend factor";
    return XR_ERROR_FEATURE_UNSUPPORTED;
  }
  const bool head_locked = header.flags & 4;
  const XrPosef identity{{0, 0, 0, 1}, {0, 0, 0}};
  // GLES rows go bottom up: T = 0 (memory row 0) is a shape's bottom there,
  // its top in Vulkan (see the native cylinder above).
  const float up = state().init.graphics_api != vulkan_api ? 1.f : -1.f;
  DrawnLayer layer;
  layer.space = head_locked ? state().view : state().app;
  auto finite = [](const Matrix &m) {
    for (const auto &row : m.m)
      for (float v : row)
        if (!std::isfinite(v)) return false;
    return true;
  };
  // Common to every eye: the ring slot, the sampled rect and the blend.
  auto prepare = [&](int eye, TextureSwapChain *chain, int slot, const Rect &rect, const Matrix &tan_angles,
                     XrPosef frame, venture::xr::LayerShape shape) -> XrResult {
    if (reinterpret_cast<std::uintptr_t>(chain) <= 2) {
      dst.reason = "built-in texture swapchain";
      return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    const auto ring = state().rings.find(chain);
    if (ring == state().rings.end()) return XR_ERROR_HANDLE_INVALID;
    if (slot < 0 || std::size_t(slot) >= ring->second->textures.size()) return XR_ERROR_INDEX_OUT_OF_RANGE;
    if (!finite(tan_angles)) return XR_ERROR_VALIDATION_FAILURE;
    {
      // A singular matrix collapses the layer to a line or point: invalid,
      // not something to draw (its 3x3 against its row lengths).
      const auto &m = tan_angles.m;
      const float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                        m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                        m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
      float size = 1;
      for (int i = 0; i < 3; ++i) size *= std::sqrt(m[i][0] * m[i][0] + m[i][1] * m[i][1] + m[i][2] * m[i][2]);
      if (!(std::abs(det) > 1e-5f * size)) return XR_ERROR_VALIDATION_FAILURE;
    }
    if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) ||
        !std::isfinite(rect.height) || rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 ||
        rect.x + rect.width > 1.00001f || rect.y + rect.height > 1.00001f)
      return XR_ERROR_SWAPCHAIN_RECT_INVALID;
    auto &e = layer.eyes[eye];
    e.chain = chain;
    e.slot = slot;
    auto &p = e.placement;
    p.shape = shape;
    p.frame = frame;
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) p.local[i][j] = tan_angles.m[i][j];
    p.map[0][0] = 1, p.map[1][1] = 1;
    p.clip[0] = rect.x, p.clip[1] = rect.y, p.clip[2] = rect.x + rect.width, p.clip[3] = rect.y + rect.height;
    const auto &desc = ring->second->desc;
    p.source_layer = desc.layers > 1 ? std::uint32_t(eye) * desc.face_count : 0;
    p.source_factor = header.src_blend;
    p.destination_factor = header.dst_blend;
    p.scale = {header.color_scale.x, header.color_scale.y, header.color_scale.z, header.color_scale.w};
    return XR_SUCCESS;
  };
  // The texture matrix after the shape's canonical (S, T) = (s0 + sa a, t0 + tb b).
  auto texture = [](venture::xr::LayerPlacement &p, const Matrix &tm, float sa, float s0, float tb, float t0) {
    for (int i = 0; i < 2; ++i) {
      p.map[i][0] = tm.m[i][0] * sa;
      p.map[i][1] = tm.m[i][1] * tb;
      p.map[i][2] = tm.m[i][0] * s0 + tm.m[i][1] * t0 + tm.m[i][2];
    }
  };
  auto affine_texture = [&](const Matrix &tm) {
    // (S, T, 1) rows; a projective texture matrix has no 2D form.
    return finite(tm) && tm.m[2][0] == 0 && tm.m[2][1] == 0 && tm.m[2][2] == 1;
  };
  XrResult r = XR_SUCCESS;
  if (header.type == 1 || header.type == 17) {
    // Planes: each eye's tan-angle matrix maps its rays in the render eye's
    // frame (the head pose and that eye's offset) to (S, T, Q).
    const auto head = head_locked ? identity : pose(reinterpret_cast<const Projection &>(header).head.pose);
    for (int eye = 0; eye < 2 && XR_SUCCEEDED(r); ++eye) {
      const auto frame = compose(head, eyes[eye].pose);
      if (header.type == 1) {
        const auto &t = reinterpret_cast<const Projection &>(header).textures[eye];
        r = prepare(eye, t.chain, t.index, t.rect, t.tan_angles, frame, venture::xr::LayerShape::plane);
      } else {
        const auto &t = reinterpret_cast<const Cylinder &>(header).textures[eye];
        if (!affine_texture(t.texture_matrix)) return XR_ERROR_VALIDATION_FAILURE;
        r = prepare(eye, t.chain, t.index, t.rect, t.tan_angles, frame, venture::xr::LayerShape::plane);
        if (XR_SUCCEEDED(r)) texture(layer.eyes[eye].placement, t.texture_matrix, 1, 0, 1, 0);
      }
    }
  } else if (header.type == 3) {
    // The canonical cylinder: radius 1 about +Y, pi radians from S = 0 (left)
    // to 1, tan(30 degrees) above and below the horizon.
    const auto &src = reinterpret_cast<const Cylinder &>(header);
    const auto head = head_locked ? identity : pose(src.head.pose);
    for (int eye = 0; eye < 2 && XR_SUCCEEDED(r); ++eye) {
      const auto &t = src.textures[eye];
      if (!affine_texture(t.texture_matrix)) return XR_ERROR_VALIDATION_FAILURE;
      r = prepare(eye, t.chain, t.index, t.rect, t.tan_angles, head, venture::xr::LayerShape::cylinder);
      if (XR_SUCCEEDED(r))
        texture(layer.eyes[eye].placement, t.texture_matrix, 1 / pi, 0.5f, up / (2 * std::tan(pi / 6)), 0.5f);
    }
  } else if (header.type == 5) {
    // An equirect at infinity: longitude from S = 0 to 1, latitude bottom to top.
    const auto &src = reinterpret_cast<const Equirect &>(header);
    const auto head = head_locked ? identity : pose(src.head.pose);
    for (int eye = 0; eye < 2 && XR_SUCCEEDED(r); ++eye) {
      const auto &t = src.textures[eye];
      if (!affine_texture(t.texture_matrix)) return XR_ERROR_VALIDATION_FAILURE;
      r = prepare(eye, t.chain, t.index, t.rect, src.tan_angles, head, venture::xr::LayerShape::far_sphere);
      if (XR_SUCCEEDED(r)) texture(layer.eyes[eye].placement, t.texture_matrix, 1 / (2 * pi), 0.5f, up / pi, 0.5f);
    }
  } else if (header.type == 4) {
    const auto &src = reinterpret_cast<const Cube &>(header);
    if (src.offset.x || src.offset.y || src.offset.z) {
      dst.reason = "cube offset";
      return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    const auto head = head_locked ? identity : pose(src.head.pose);
    for (int eye = 0; eye < 2 && XR_SUCCEEDED(r); ++eye) {
      const auto &t = src.textures[eye];
      const auto ring = state().rings.find(t.chain);
      if (ring != state().rings.end() && ring->second->desc.face_count != 6) return XR_ERROR_LAYER_INVALID;
      r = prepare(eye, t.chain, t.index, {0, 0, 1, 1}, src.tan_angles, head, venture::xr::LayerShape::cube);
    }
  } else {
    return XR_ERROR_FEATURE_UNSUPPORTED;  // keeps the native reason
  }
  if (XR_FAILED(r)) return r;
  drawn.add(layer, layers);
  return XR_SUCCESS;
}
void log_layer_shape(const LayerHeader &header) {
  auto matrix = [](const Matrix &m) {
    return std::format("[{},{},{},{};{},{},{},{};{},{},{},{};{},{},{},{}]", m.m[0][0], m.m[0][1], m.m[0][2],
                       m.m[0][3], m.m[1][0], m.m[1][1], m.m[1][2], m.m[1][3], m.m[2][0], m.m[2][1], m.m[2][2],
                       m.m[2][3], m.m[3][0], m.m[3][1], m.m[3][2], m.m[3][3]);
  };
  auto ring = [](TextureSwapChain *chain) {
    const auto it = state().rings.find(chain);
    if (it == state().rings.end()) return std::string("none");
    const auto &d = it->second->desc;
    return std::format("{}x{} layers={} faces={} levels={}", d.width, d.height, d.layers, d.face_count, d.mip_count);
  };
  if (header.type == 1) {
    const auto &src = reinterpret_cast<const Projection &>(header);
    for (int eye = 0; eye < 2; ++eye) {
      const auto &t = src.textures[eye];
      log::warn("vrapi", "Layer eye={} ring={} rect=[{},{},{},{}] matrix={}", eye, ring(t.chain), t.rect.x, t.rect.y,
                t.rect.width, t.rect.height, matrix(t.tan_angles));
    }
  } else if (header.type == 3 || header.type == 17) {
    const auto &src = reinterpret_cast<const Cylinder &>(header);
    for (int eye = 0; eye < 2; ++eye) {
      const auto &t = src.textures[eye];
      log::warn("vrapi", "Layer eye={} ring={} rect=[{},{},{},{}] matrix={} texture={}", eye, ring(t.chain), t.rect.x,
                t.rect.y, t.rect.width, t.rect.height, matrix(t.tan_angles), matrix(t.texture_matrix));
    }
  } else if (header.type == 4) {
    const auto &src = reinterpret_cast<const Cube &>(header);
    log::warn("vrapi", "Layer ring={} matrix={} offset=[{},{},{}]", ring(src.textures[0].chain),
              matrix(src.tan_angles), src.offset.x, src.offset.y, src.offset.z);
  } else if (header.type == 5) {
    const auto &src = reinterpret_cast<const Equirect &>(header);
    for (int eye = 0; eye < 2; ++eye) {
      const auto &t = src.textures[eye];
      log::warn("vrapi", "Layer eye={} ring={} rect=[{},{},{},{}] matrix={} texture={}", eye, ring(t.chain), t.rect.x,
                t.rect.y, t.rect.width, t.rect.height, matrix(src.tan_angles), matrix(t.texture_matrix));
    }
  }
}
XrResult translate_layer(const LayerHeader &header, const XrView *eyes, LayerImage image, LayerStorage &dst,
                         std::vector<const XrCompositionLayerBaseHeader *> &layers, int first_eye,
                         DrawnLayers *drawn) {
  dst.reason = nullptr;
  // Flags other than those below change nothing VrApi's compositor shows on a
  // PC host (chromatic aberration correction, sRGB framebuffer and filter
  // hints); keep the layer rather than lose it over one.
  constexpr std::uint32_t known_flags = 2 | 4 | 16 | 256 | (1u << 19);
  if (header.flags & ~known_flags || header.reserved) {
    static std::once_flag reported;
    std::call_once(reported, [&] {
      log::warn("vrapi", "Layer type={} flags={:#x} reserved={:#x}: unknown bits ignored", header.type, header.flags,
                header.reserved);
    });
  }
  const auto before = layers.size();
  const auto r = translate_native(header, eyes, image, dst, layers, first_eye);
  if (XR_SUCCEEDED(r) || !drawn || (r != XR_ERROR_FEATURE_UNSUPPORTED && r != XR_ERROR_VALIDATION_FAILURE))
    return r;
  // No host layer shows it exactly: draw it at its geometry instead.
  layers.resize(before);
  const char *reason = dst.reason;
  const auto d = draw_layer(header, eyes, dst, *drawn, layers);
  if (XR_FAILED(d)) {
    layers.resize(before);
    if (!dst.reason) dst.reason = reason;
    return d == XR_ERROR_FEATURE_UNSUPPORTED || d == XR_ERROR_VALIDATION_FAILURE ? r : d;
  }
  static std::set<std::pair<std::uint32_t, std::string>> reported;
  if (reported.emplace(header.type, reason ? reason : "").second) {
    log::info("vrapi", "Layer type={} blend={}/{} drawn at its geometry: {}", header.type, header.src_blend,
              header.dst_blend, reason ? reason : "no exact host layer");
    log_layer_shape(header);
  }
  return XR_SUCCESS;
}
} // namespace venture::hle::vrapi
