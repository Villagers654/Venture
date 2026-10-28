#pragma once
#include "xr/api.h"
#include <cmath>
namespace venture::xr {
inline XrQuaternionf multiply(XrQuaternionf a, XrQuaternionf b) {
  return {
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline XrQuaternionf inverse(XrQuaternionf q) { return {-q.x, -q.y, -q.z, q.w}; }
inline XrVector3f rotate(XrQuaternionf q, XrVector3f v) {
  auto r = multiply(multiply(q, {v.x, v.y, v.z, 0}), inverse(q));
  return {r.x, r.y, r.z};
}
inline XrPosef compose(XrPosef a, XrPosef b) {
  auto p = rotate(a.orientation, b.position);
  return {multiply(a.orientation, b.orientation),
          {a.position.x + p.x, a.position.y + p.y, a.position.z + p.z}};
}
inline XrPosef inverse(XrPosef p) {
  auto q = inverse(p.orientation);
  return {q, rotate(q, {-p.position.x, -p.position.y, -p.position.z})};
}
inline float length(XrVector3f v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
} // namespace venture::xr
