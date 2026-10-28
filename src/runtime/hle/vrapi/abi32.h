// VrApi_Types.h ILP32 size assertions (Mobile SDK flexible-layer ABI),
// checked against First Contact 7 libOVRPlugin.so's armeabi-v7a call sites.
// Pointers are words; double/uint64 retain AAPCS32 eight-byte alignment.
#pragma once
#include "hle/vrapi/abi.h"

namespace venture::hle::vrapi::abi32 {
using namespace abi;
struct Java {
  std::uint32_t vm, env, activity;
};
struct Init {
  std::int32_t type, product, major, minor, patch, graphics_api;
  Java java;
};
struct IntegrationInit {
  std::int32_t product, major, minor, patch;
  Java java;
};
struct Mode {
  std::int32_t type;
  std::uint32_t flags;
  Java java;
  std::uint64_t display, window, context;
};
struct Tracking {
  std::uint32_t status, padding;
  RigidPose head;
};
struct LayerHeader {
  std::int32_t type;
  std::uint32_t flags;
  Vec4 color_scale;
  std::int32_t src_blend, dst_blend;
  std::uint32_t reserved;
};
struct ProjectionTexture {
  std::uint32_t chain;
  std::int32_t index;
  Matrix tan_angles;
  Rect rect;
};
struct Projection {
  LayerHeader header;
  RigidPose head;
  ProjectionTexture textures[2];
};
struct Cylinder {
  LayerHeader header;
  RigidPose head;
  struct {
    std::uint32_t chain;
    std::int32_t index;
    Matrix tan_angles;
    Rect rect;
    Matrix texture_matrix;
  } textures[2];
};
struct Cube {
  LayerHeader header;
  RigidPose head;
  Matrix tan_angles;
  Vec3 offset;
  struct {
    std::uint32_t chain;
    std::int32_t index;
  } textures[2];
};
struct Equirect {
  LayerHeader header;
  RigidPose head;
  Matrix tan_angles;
  struct {
    std::uint32_t chain;
    std::int32_t index;
    Rect rect;
    Matrix texture_matrix;
  } textures[2];
};
struct LoadingIcon {
  LayerHeader header;
  float spin_speed, spin_scale;
  std::uint32_t chain;
  std::int32_t index;
};
struct Submit {
  std::uint32_t flags, swap_interval;
  std::uint64_t frame_index;
  double display_time;
  std::uint64_t completion_fence;
  std::uint32_t layer_count, layers;
};
struct LegacyTexture {
  std::uint32_t color, depth;
  std::int32_t index;
  Matrix tan_angles;
  Rect rect;
  std::uint32_t padding;
  RigidPose head;
  std::uint64_t completion_fence;
};
struct LegacyLayer {
  LegacyTexture textures[2];
  float spin_speed, spin_scale, color_scale;
  std::uint32_t padding;
  std::int32_t src_blend, dst_blend, flags;
};
struct LegacyFrame {
  std::int32_t type;
  std::uint32_t padding;
  LegacyLayer layers[4];
  std::int32_t layer_count, flags;
  std::int64_t frame_index;
  std::int32_t swap_interval, latency_mode;
  Matrix reserved;
  std::uint32_t reserved1;
  Performance performance;
  Java java;
};
// OVRPlugin SetControllerHaptics @0x32b44: time@0, count@8,
// terminated@12, samples@16. The struct's trailing padding is not a pointer.
struct HapticBuffer {
  double time;
  std::uint32_t count;
  std::uint8_t terminated, padding[3];
  std::uint32_t samples;
};
static_assert(sizeof(Java) == 12 && sizeof(Init) == 36 &&
              offsetof(Init, java) == 24);
static_assert(sizeof(IntegrationInit) == 28 &&
              offsetof(IntegrationInit, java) == 16);
static_assert(sizeof(Mode) == 48 && offsetof(Mode, display) == 24);
static_assert(sizeof(Tracking) == 104 && sizeof(abi::Tracking) == 360);
static_assert(sizeof(LayerHeader) == 36 && sizeof(Projection) == 312 &&
              offsetof(Projection, head) == 40);
static_assert(sizeof(Cylinder) == 440 && sizeof(Cube) == 232 &&
              sizeof(Equirect) == 376);
static_assert(sizeof(LoadingIcon) == 52 && sizeof(Submit) == 40 &&
              offsetof(Submit, layers) == 36);
static_assert(sizeof(LegacyTexture) == 200 && sizeof(LegacyLayer) == 432 &&
              sizeof(LegacyFrame) == 1856);
static_assert(sizeof(HapticBuffer) == 24 &&
              offsetof(HapticBuffer, samples) == 16);
} // namespace venture::hle::vrapi::abi32
