// SPDX-License-Identifier: GPL-3.0-or-later
// The 32-bit libvrapi.so's (armeabi-v7a, for ARM32 games on CPUs that run
// AArch32) entry points whose records hold pointers: the game passes VrApi's
// ILP32 layouts (hle/vrapi/abi32.h, the ones the PC runtime's A32 adapters
// read), converted here to the core's (hle/vrapi/abi.h, compiled for this
// target). Every other entry point is in exports.cpp: its records hold no
// pointer, so the layouts are the same, as the asserts below check.
#include <array>
#include <atomic>
#include <variant>

#include "android_game/vrapi/native.h"
#include "common/log.h"
#include "hle/vrapi/abi32.h"
#include "hle/vrapi/vrapi.h"

#define VENTURE_VRAPI_EXPORT extern "C" __attribute__((visibility("default")))

namespace venture::hle::vrapi {
static_assert(sizeof(void*) == 4, "exports_ilp32.cpp is the armeabi-v7a library's");
// Pointer-free records: one layout on both sides.
static_assert(sizeof(abi::TrackingHead) == sizeof(abi32::Tracking) && sizeof(abi::Tracking) == 360);
static_assert(sizeof(abi::Submit) == sizeof(abi32::Submit) && offsetof(abi::Submit, layers) == offsetof(abi32::Submit, layers));
static_assert(sizeof(abi::HapticBuffer) == sizeof(abi32::HapticBuffer) &&
              offsetof(abi::HapticBuffer, samples) == offsetof(abi32::HapticBuffer, samples));
static_assert(sizeof(abi::RemoteState) == 72 && sizeof(abi::RemoteCapabilities) == 48 && sizeof(abi::HandPose) == 480);
static_assert(sizeof(abi::SwapChainCreate) == 48 && sizeof(abi::InputStateHeader) == 16);

namespace {
template <class T> T* pointer(std::uint32_t address) { return reinterpret_cast<T*>(static_cast<std::uintptr_t>(address)); }
abi::Java java(const abi32::Java& j) { return {j.vm, j.env, j.activity}; }
// ovrSystemCreateInfoVulkan: three dispatchable handles, one word each.
struct SystemVulkan32 {
  std::uint32_t instance, physical, device;
};
// ovrModeParmsVulkan: ovrModeParms, then the 64-bit SynchronizationQueue.
struct ModeVulkan32 {
  abi32::Mode mode;
  std::uint64_t queue;
};
static_assert(sizeof(SystemVulkan32) == 12 && sizeof(ModeVulkan32) == 56 && offsetof(ModeVulkan32, queue) == 48);
// Whether vrapi_Initialize chose Vulkan: only then does EnterVrMode's record
// carry the queue.
std::atomic<bool> vulkan{false};

abi::LayerHeader header(const abi32::LayerHeader& h) {
  return {h.type, h.flags, h.color_scale, h.src_blend, h.dst_blend, h.reserved};
}
template <class W, class N> N layer(const W& w) {
  N n{};
  n.header = header(w.header);
  n.head = w.head;
  if constexpr (requires { n.tan_angles; }) n.tan_angles = w.tan_angles;
  if constexpr (requires { n.offset; }) n.offset = w.offset;
  for (int i = 0; i < 2; ++i) {
    auto& t = n.textures[i];
    const auto& s = w.textures[i];
    t.chain = pointer<abi::TextureSwapChain>(s.chain);
    t.index = s.index;
    if constexpr (requires { t.rect; }) t.rect = s.rect;
    if constexpr (requires { t.tan_angles; }) t.tan_angles = s.tan_angles;
    if constexpr (requires { t.texture_matrix; }) t.texture_matrix = s.texture_matrix;
  }
  return n;
}
// An EGLSync is one word; older SDKs left the high word (then Pad) unset.
std::uint64_t fence(std::uint64_t value) { return std::uint32_t(value); }
using Layer = std::variant<abi::LayerHeader, abi::Projection, abi::Cylinder, abi::Cube, abi::Equirect, abi::LoadingIcon>;
}  // namespace

VENTURE_VRAPI_EXPORT std::int32_t vrapi_Initialize(const abi32::Init* w) {
  if (!w) return guest::vrapi_Initialize(nullptr);
  const abi::Init n{w->type, w->product, w->major, w->minor, w->patch, w->graphics_api, java(w->java)};
  vrapi_native::remember_java(&n.java);
  vulkan = n.graphics_api == abi::vulkan_api;
  return guest::vrapi_Initialize(&n);
}
VENTURE_VRAPI_EXPORT std::int32_t vrapi_CreateSystemVulkan(const SystemVulkan32* w) {
  if (!w) return guest::vrapi_CreateSystemVulkan(nullptr);
  const abi::SystemVulkan n{w->instance, w->physical, w->device};
  return guest::vrapi_CreateSystemVulkan(&n);
}
VENTURE_VRAPI_EXPORT abi::Mobile* vrapi_EnterVrMode(const abi32::Mode* w) {
  if (!w) return guest::vrapi_EnterVrMode(nullptr);
  // The core reads the queue after the record when the game chose Vulkan.
  abi::ModeVulkan n{{w->type, w->flags, java(w->java), w->display, w->window, w->context}, 0};
  if (vulkan) n.queue = reinterpret_cast<const ModeVulkan32*>(w)->queue;
  vrapi_native::remember_java(&n.mode.java);
  return guest::vrapi_EnterVrMode(&n.mode);
}

VENTURE_VRAPI_EXPORT std::int32_t vrapi_SubmitFrame2(abi::Mobile* mobile, const abi32::Submit* w) {
  if (!w) return guest::vrapi_SubmitFrame2(mobile, nullptr);
  if (w->layer_count > 16 || (w->layer_count && !w->layers)) return abi::invalid;
  std::array<Layer, 16> storage;
  std::array<const abi::LayerHeader*, 16> pointers{};
  const auto* layers = pointer<const std::uint32_t>(w->layers);
  for (std::uint32_t i = 0; i < w->layer_count; ++i) {
    const auto* h = pointer<const abi32::LayerHeader>(layers[i]);
    if (!h) return abi::invalid;
    switch (h->type) {
      case 1: storage[i] = layer<abi32::Projection, abi::Projection>(*reinterpret_cast<const abi32::Projection*>(h)); break;
      case 3: storage[i] = layer<abi32::Cylinder, abi::Cylinder>(*reinterpret_cast<const abi32::Cylinder*>(h)); break;
      case 4: storage[i] = layer<abi32::Cube, abi::Cube>(*reinterpret_cast<const abi32::Cube*>(h)); break;
      case 5: storage[i] = layer<abi32::Equirect, abi::Equirect>(*reinterpret_cast<const abi32::Equirect*>(h)); break;
      case 6: {
        const auto& l = *reinterpret_cast<const abi32::LoadingIcon*>(h);
        storage[i] = abi::LoadingIcon{header(l.header), l.spin_speed, l.spin_scale,
                                      pointer<abi::TextureSwapChain>(l.chain), l.index};
        break;
      }
      default: storage[i] = header(*h); break;  // the core rejects a type it does not know
    }
    pointers[i] = std::visit(
        [](const auto& l) -> const abi::LayerHeader* {
          if constexpr (std::is_same_v<std::decay_t<decltype(l)>, abi::LayerHeader>) return &l;
          else return &l.header;
        },
        storage[i]);
  }
  const abi::Submit n{w->flags, w->swap_interval, w->frame_index, w->display_time, fence(w->completion_fence),
                      w->layer_count, pointers.data()};
  return guest::vrapi_SubmitFrame2(mobile, &n);
}
VENTURE_VRAPI_EXPORT void vrapi_SubmitFrame(abi::Mobile* mobile, const abi32::LegacyFrame* w) {
  if (!w) return guest::vrapi_SubmitFrame(mobile, nullptr);
  if (w->layer_count < 0 || w->layer_count > 4) {
    log::warn("vrapi", "SubmitFrame: invalid layer count {}", w->layer_count);
    return;
  }
  abi::LegacyFrame n{};
  n.type = w->type;
  n.layer_count = w->layer_count;
  n.flags = w->flags;
  n.frame_index = w->frame_index;
  n.swap_interval = w->swap_interval;
  n.latency_mode = w->latency_mode;
  n.reserved = w->reserved;
  n.reserved1 = w->reserved1;
  n.performance = w->performance;
  n.java = java(w->java);
  for (int i = 0; i < w->layer_count; ++i) {
    auto& d = n.layers[i];
    const auto& s = w->layers[i];
    d.spin_speed = s.spin_speed;
    d.spin_scale = s.spin_scale;
    d.color_scale = s.color_scale;
    d.src_blend = s.src_blend;
    d.dst_blend = s.dst_blend;
    d.flags = s.flags;
    for (int e = 0; e < 2; ++e) {
      const auto& t = s.textures[e];
      d.textures[e] = {pointer<abi::TextureSwapChain>(t.color), pointer<abi::TextureSwapChain>(t.depth), t.index,
                       t.tan_angles, t.rect, 0, t.head, fence(t.completion_fence)};
    }
  }
  guest::vrapi_SubmitFrame(mobile, &n);
}

// The calls that carry the game's Java VM.
namespace {
abi::Java remembered(const abi32::Java* w) {
  const abi::Java n = w ? java(*w) : abi::Java{};
  if (w) vrapi_native::remember_java(&n);
  return n;
}
}  // namespace
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetSystemPropertyInt(const abi32::Java* w, std::int32_t id) {
  const auto n = remembered(w);
  return guest::vrapi_GetSystemPropertyInt(w ? &n : nullptr, id);
}
VENTURE_VRAPI_EXPORT float vrapi_GetSystemPropertyFloat(const abi32::Java* w, std::int32_t id) {
  const auto n = remembered(w);
  return guest::vrapi_GetSystemPropertyFloat(w ? &n : nullptr, id);
}
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetSystemPropertyFloatArray(const abi32::Java* w, std::int32_t id, float* values,
                                                                    std::int32_t count) {
  const auto n = remembered(w);
  return guest::vrapi_GetSystemPropertyFloatArray(w ? &n : nullptr, id, values, count);
}
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetSystemStatusInt(const abi32::Java* w, std::int32_t id) {
  const auto n = remembered(w);
  return guest::vrapi_GetSystemStatusInt(w ? &n : nullptr, id);
}
VENTURE_VRAPI_EXPORT float vrapi_GetSystemStatusFloat(const abi32::Java* w, std::int32_t id) {
  const auto n = remembered(w);
  return guest::vrapi_GetSystemStatusFloat(w ? &n : nullptr, id);
}
VENTURE_VRAPI_EXPORT bool vrapi_GetPropertyInt(const abi32::Java* w, std::int32_t id, std::int32_t* value) {
  const auto n = remembered(w);
  return guest::vrapi_GetPropertyInt(w ? &n : nullptr, id, value);
}
VENTURE_VRAPI_EXPORT void vrapi_SetPropertyInt(const abi32::Java* w, std::int32_t id, std::int32_t value) {
  const auto n = remembered(w);
  guest::vrapi_SetPropertyInt(w ? &n : nullptr, id, value);
}
VENTURE_VRAPI_EXPORT void vrapi_SetPropertyFloat(const abi32::Java* w, std::int32_t id, float value) {
  const auto n = remembered(w);
  guest::vrapi_SetPropertyFloat(w ? &n : nullptr, id, value);
}
}  // namespace venture::hle::vrapi
