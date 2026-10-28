// Android AArch64 VrApi ABI, Oculus Mobile SDK 1.42.0 (VrApi_Types.h,
// VrApi_Input.h, VrApi_Vulkan.h). Earlier SDKs retain the same public layouts
// below; CompletionFence occupied the 8 bytes called Pad in 1.42.0.
// Declarations only: no Oculus implementation is included.
#pragma once
#include <cstddef>
#include <cstdint>

namespace venture::hle::vrapi::abi {
struct Mobile;
struct TextureSwapChain;
struct Java {
  std::uint64_t vm, env, activity;
};
struct Vec2 {
  float x, y;
};
struct Vec3 {
  float x, y, z;
};
struct Vec4 {
  float x, y, z, w;
};
using Quat = Vec4;
struct Matrix {
  float m[4][4];
};
struct Pose {
  Quat orientation;
  Vec3 position;
};
struct Rect {
  float x, y, width, height;
};
struct Init {
  std::int32_t type, product, major, minor, patch, graphics_api;
  Java java;
};
struct Mode {
  std::int32_t type;
  std::uint32_t flags;
  Java java;
  std::uint64_t display, window, context;
};
// VrApi_Vulkan.h / VrApi_Types.h: dispatchable Vulkan handles are 64-bit on
// AArch64. SynchronizationQueue follows the complete ovrModeParms, at byte 56.
struct SystemVulkan { std::uint64_t instance, physical, device; };
struct ModeVulkan { Mode mode; std::uint64_t queue; };
inline constexpr std::int32_t vulkan_api = 0x40100;
struct RigidPose {
  Pose pose;
  Vec3 angular_velocity, linear_velocity, angular_acceleration, linear_acceleration;
  std::uint32_t padding;
  double time, prediction;
};
struct Tracking {  // VrApi_Types.h ovrTracking2 (360 bytes)
  std::uint32_t status, padding;
  RigidPose head;
  struct {
    Matrix projection, view;
  } eye[2];
};
// VrApi_Types.h ovrTracking (104 bytes): the head-only form that
// vrapi_GetInputTrackingState fills. Writing ovrTracking2 through an
// ovrTracking* would overrun the caller's object by 256 bytes.
struct TrackingHead {
  std::uint32_t status, padding;
  RigidPose head;
};
struct SwapChainCreate {
  std::int64_t format;
  std::int32_t width, height, levels, faces, array_size, count;
  std::uint64_t create_flags, usage_flags;
};
// Mobile SDK 1.50 VrApi_Types.h: ovrTextureSamplerState. Cactus OVRPlugin
// 1.66's helper at 0x1566dc edits the first 32 bytes between Get (0x159264)
// and Set (0x15927c), preserving the trailing MaxAnisotropy supplied by Get.
enum class TextureFilter : std::int32_t {
  nearest = 0, linear = 1, nearest_mipmap_linear = 2,
  linear_mipmap_nearest = 3, linear_mipmap_linear = 4,
  cubic = 5, cubic_mipmap_nearest = 6, cubic_mipmap_linear = 7
};
enum class TextureWrap : std::int32_t { repeat = 0, clamp_edge = 1, clamp_border = 2 };
struct TextureSamplerState {
  TextureFilter min_filter, mag_filter;
  TextureWrap wrap_s, wrap_t;
  float border_color[4];
  float max_anisotropy;
};
static_assert(sizeof(TextureSamplerState) == 36 &&
              offsetof(TextureSamplerState, border_color) == 16 &&
              offsetof(TextureSamplerState, max_anisotropy) == 32);
struct LayerHeader {
  std::int32_t type;
  std::uint32_t flags;
  Vec4 color_scale;
  std::int32_t src_blend, dst_blend;
  std::uint64_t reserved;
};
struct ProjectionTexture {
  TextureSwapChain *chain;
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
    TextureSwapChain *chain;
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
    TextureSwapChain *chain;
    std::int32_t index;
  } textures[2];
};
struct Equirect {
  LayerHeader header;
  RigidPose head;
  Matrix tan_angles;
  struct {
    TextureSwapChain *chain;
    std::int32_t index;
    Rect rect;
    Matrix texture_matrix;
  } textures[2];
};
struct LoadingIcon {
  LayerHeader header;
  float spin_speed, spin_scale;
  TextureSwapChain *chain;
  std::int32_t index;
};
struct Submit {
  std::uint32_t flags, swap_interval;
  std::uint64_t frame_index;
  double display_time;
  std::uint64_t completion_fence;
  std::uint32_t layer_count;
  const LayerHeader *const *layers;
};
struct LegacyTexture {
  TextureSwapChain *color;
  TextureSwapChain *depth;
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
struct Performance {
  std::int32_t cpu, gpu, main_tid, render_tid;
};
struct LegacyFrame {
  std::int32_t type;
  std::uint32_t padding;
  LegacyLayer layers[4];
  std::int32_t layer_count, flags;
  std::int64_t frame_index;
  std::int32_t swap_interval, latency_mode;
  Matrix reserved;
  std::uint64_t reserved1;
  Performance performance;
  Java java;
};
struct InputHeader {
  std::uint32_t type, id;
};
struct RemoteCapabilities {
  InputHeader header;
  std::uint32_t capabilities, buttons;
  std::uint16_t pad_max_x, pad_max_y;
  float pad_size_x, pad_size_y;
  std::uint32_t haptic_max, haptic_ms, touches, reserved4, reserved5;
};
struct InputStateHeader {
  std::uint32_t type;
  double time;
};
struct RemoteState {
  InputStateHeader header;
  std::uint32_t buttons, pad_status;
  Vec2 pad;
  std::uint8_t battery, recenter;
  std::uint16_t reserved;
  float trigger, grip;
  std::uint32_t touches, reserved5;
  Vec2 joystick, raw_joystick;
};
struct HapticBuffer {
  double time;
  std::uint32_t count;
  bool terminated;
  const std::uint8_t *samples;
};
struct Boundary {
  Vec3 closest, normal;
  float distance;
  bool triggering;
};
struct HandCapabilities {
  InputHeader header;
  std::uint32_t capabilities, state_capabilities;
};
struct HandState {
  InputStateHeader header;
  float pinch[4];
  Pose pointer;
  std::uint32_t status;
};
struct HandPoseHeader {
  std::uint32_t version;
  double reserved;
};
struct HandPose {
  HandPoseHeader header;
  std::int32_t status;
  Pose root;
  Quat rotations[24];
  double requested_time, sample_time;
  std::uint32_t confidence;
  float scale;
  std::uint32_t finger_confidence[5];
};
struct Capsule {
  std::int16_t bone;
  Vec3 points[2];
  float radius;
};
struct HandSkeleton {
  std::uint32_t version, bone_count, capsule_count, reserved[5];
  Pose poses[24];
  std::int16_t parents[24];
  Capsule capsules[19];
};
struct Indices4 {
  std::int16_t x, y, z, w;
};
struct HandMesh {
  std::uint32_t version, vertex_count, index_count, reserved[13];
  Vec3 positions[3000];
  std::int16_t indices[18000];
  Vec3 normals[3000];
  Vec2 uv[3000];
  Indices4 blend_indices[3000];
  Vec4 blend_weights[3000];
};
struct ColorDesc {
  std::int32_t color_space;
  std::uint32_t padding;
};
struct Event {
  std::int32_t type;
};
inline constexpr std::uint32_t hand_version = 0xdf000001;
inline constexpr std::int32_t event_unavailable = 1002; // ovrSuccess_EventUnavailable, VrApi_Types.h
inline constexpr std::int32_t success = 0, invalid = -1005, not_initialized = -1004,
                              unavailable = -1054, not_ready = -1053, no_device = -1051,
                              unsupported = -1052, invalid_operation = -1015;

// SDK 1.42.0 OVR_VRAPI_ASSERT_TYPE_SIZE_64_BIT declarations and AAPCS64
// aggregate alignment (including the explicit padding before double fields).
static_assert(sizeof(Java) == 24 && sizeof(Init) == 48 && sizeof(Mode) == 56);
static_assert(sizeof(SystemVulkan) == 24 && sizeof(ModeVulkan) == 64 && offsetof(ModeVulkan, queue) == 56);
static_assert(sizeof(Pose) == 28 && sizeof(RigidPose) == 96 && sizeof(Tracking) == 360);
static_assert(sizeof(TrackingHead) == 104);
static_assert(sizeof(SwapChainCreate) == 48 && sizeof(Submit) == 48);
static_assert(sizeof(LayerHeader) == 40 && sizeof(Projection) == 328 && sizeof(Cylinder) == 456);
static_assert(sizeof(Cube) == 248 && sizeof(Equirect) == 392 && sizeof(LoadingIcon) == 64);
static_assert(sizeof(LegacyTexture) == 208 && sizeof(LegacyLayer) == 448 &&
              sizeof(LegacyFrame) == 1936);
static_assert(offsetof(Submit, layers) == 40 && offsetof(Tracking, eye) == 104);
static_assert(sizeof(InputHeader) == 8 && sizeof(RemoteCapabilities) == 48 &&
              sizeof(RemoteState) == 72);
static_assert(sizeof(HandPoseHeader) == 16 && sizeof(HandPose) == 480);
static_assert(sizeof(HandSkeleton) == 1360 && sizeof(HandMesh) == 204064);
} // namespace venture::hle::vrapi::abi
