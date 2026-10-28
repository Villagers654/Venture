#include "common/log.h"
#include "core/a32_abi.h"
#include "core/token32.h"
#include "hle/vrapi/abi32.h"
#include "hle/vrapi/integration.h"
#include "hle/vrapi/state.h"
#include "hle/vrapi/vrapi.h"
#include "xr/haptic_output.h"
#include <variant>

namespace venture::hle::vrapi {
namespace {
template <class T> T load(GuestAddr p) {
  T v;
  a32::read_bytes(p, &v, sizeof(v));
  return v;
}
template <class T> void store(GuestAddr p, const T &v) {
  a32::write_bytes(p, &v, sizeof(v));
}
struct BadHandle {};
template <class T> struct Handle {
  T *value;
};
Token32Registry tokens;
template <class T> T *resolve(std::uint32_t token) {
  if (!token)
    return nullptr;
  // VrApi_Types.h reserves chains 1/2 for the default black/loading textures.
  if constexpr (std::is_same_v<T, abi::TextureSwapChain>)
    if (token <= 2)
      return reinterpret_cast<T *>(std::uintptr_t(token));
  auto h = tokens.resolve(Token32<Handle<T>>{token});
  if (!h)
    throw BadHandle{};
  return h->value;
}
template <class T> std::uint32_t insert(T *value) {
  return value ? tokens.insert(std::make_shared<Handle<T>>(Handle<T>{value}))
                     .value
               : 0;
}
abi::Java java(abi32::Java j) { return {j.vm, j.env, j.activity}; }

// Only the explicitly listed, layout-invariant pointees below use identity
// pointers. Handles and Java/version structures always pass through this layer.
template <class T> struct Argument {
  T value{};
  void read(a32::ArgCursor32 &c) {
    if constexpr (std::is_same_v<T, float>)
      value = std::bit_cast<float>(c.word());
    else if constexpr (std::is_same_v<T, double>)
      value = std::bit_cast<double>(c.pair());
    else if constexpr (std::is_same_v<T, abi::Mobile *> ||
                       std::is_same_v<T, abi::TextureSwapChain *>)
      value = resolve<std::remove_pointer_t<T>>(c.word());
    else if constexpr (std::is_pointer_v<T>)
      value = g2h<std::remove_pointer_t<T>>(c.word());
    else
      value = static_cast<T>(sizeof(T) == 8 ? c.pair() : c.word());
  }
};
template <> struct Argument<const abi::Java *> {
  abi::Java storage{};
  const abi::Java *value{};
  void read(a32::ArgCursor32 &c) {
    if (auto p = c.word()) {
      storage = java(load<abi32::Java>(p));
      value = &storage;
    }
  }
};
template <class R, class... A> void invoke(R (*fn)(A...), CallContext &ctx) {
  constexpr bool aggregate = std::is_class_v<R>;
  a32::ResultWriter32 result(ctx.cpu());
  a32::ArgCursor32 cursor(ctx.cpu(), aggregate);
  std::tuple<Argument<A>...> args;
  std::apply([&](auto &...a) { (a.read(cursor), ...); }, args);
  auto call = [&](auto &...a) { return fn(a.value...); };
  if constexpr (std::is_void_v<R>)
    std::apply(call, args);
  else {
    auto value = std::apply(call, args);
    if constexpr (aggregate)
      result.composite(std::as_bytes(std::span(&value, 1)));
    else if constexpr (std::is_pointer_v<R>)
      result.scalar({a32::Kind::U32}, insert(value));
    else if constexpr (std::is_same_v<R, double>)
      result.scalar({a32::Kind::F64}, std::bit_cast<std::uint64_t>(value));
    else if constexpr (std::is_same_v<R, float>)
      result.scalar({a32::Kind::F32}, std::bit_cast<std::uint32_t>(value));
    else
      result.scalar({sizeof(R) == 8 ? a32::Kind::U64 : a32::Kind::U32},
                    static_cast<std::uint64_t>(value));
  }
}
void haptic_abi_error(CallContext &ctx, std::string_view reason) {
  auto name = std::string_view(ctx.info().name);
  if (name != "vrapi_SetHapticVibrationSimple" && name != "vrapi_SetHapticVibrationBuffer") return;
  auto id = std::uint32_t(ctx.x(1));
  auto bits = std::uint32_t(ctx.x(2));
  auto amplitude = name == "vrapi_SetHapticVibrationSimple" ? std::bit_cast<float>(bits) : 0.f;
  XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
  venture::xr::haptic_event(name == "vrapi_SetHapticVibrationSimple" ? "vrapi_simple" : "vrapi_buffer",
      XR_NULL_HANDLE, info, amplitude, 0, 0, "validate_guest_abi", XR_ERROR_VALIDATION_FAILURE,
      {{"stage", "guest_request"}, {"host_query_safe", false}, {"abi_bits", 32},
       {"device_id", id}, {"reason", reason}, {"argument2_bits", bits},
       {"hand", id == 1 ? "/user/hand/left" : id == 2 ? "/user/hand/right" : "invalid"}});
}
template <auto F> void guarded(CallContext &ctx) {
  try {
    F(ctx);
  } catch (const BadHandle &) {
    haptic_abi_error(ctx, "invalid_handle");
    log::warn("vrapi", "{}: invalid A32 handle", ctx.info().name);
    const auto type = ctx.info().a32->result;
    a32::ResultWriter32 result(ctx.cpu());
    if (type.kind == a32::Kind::Composite) {
      std::vector<std::byte> empty(type.size);
      result.composite(empty);
    } else {
      const bool zero = type.kind == a32::Kind::Handle ||
                        type.kind == a32::Kind::F32 ||
                        type.kind == a32::Kind::F64;
      result.scalar(type, zero ? 0 : std::uint64_t(std::int64_t(abi::invalid)));
    }
  } catch (const a32::AbiError &e) {
    haptic_abi_error(ctx, "invalid_memory");
    a32::fail(ctx, e.error);
  }
}
template <auto F> void forward(CallContext &ctx) { invoke(F, ctx); }
void initialize(CallContext &ctx) {
  if (!ctx.x(0)) {
    ctx.return_x(abi::invalid);
    return;
  }
  auto w = load<abi32::Init>(ctx.x(0));
  abi::Init n{w.type,  w.product,      w.major,     w.minor,
              w.patch, w.graphics_api, java(w.java)};
  auto r = guest::vrapi_Initialize(&n);
  log::info("vrapi", "A32 Initialize {}.{}.{} -> {}", n.major, n.minor, n.patch,
            r);
  ctx.return_x(r);
}
void integration_initialize(CallContext &ctx) {
  if (!ctx.x(0)) {
    ctx.return_x(abi::invalid);
    return;
  }
  auto w = load<abi32::IntegrationInit>(ctx.x(0));
  integration::Init n{w.product, w.major, w.minor, w.patch, java(w.java)};
  auto r = integration::initialize(&n);
  log::info("vrintegration", "A32 Initialize {}.{}.{} -> {}", n.major, n.minor,
            n.patch, r);
  ctx.return_x(r);
}
void enter(CallContext &ctx) {
  if (!ctx.x(0)) {
    ctx.return_x(0);
    return;
  }
  auto w = load<abi32::Mode>(ctx.x(0));
  abi::Mode n{w.type, w.flags, java(w.java), w.display, w.window, w.context};
  auto token = insert(guest::vrapi_EnterVrMode(&n));
  log::info("vrapi", "A32 EnterVrMode -> {}", token ? "entered" : "failed");
  ctx.return_x(token);
}
template <class T, auto F> void release(CallContext &ctx) {
  auto token = std::uint32_t(ctx.x(0));
  auto value = resolve<T>(token);
  F(value);
  std::lock_guard lock(state().mutex);
  // A failed host teardown leaves the object usable for a later retry.
  if constexpr (std::is_same_v<T, abi::Mobile>) {
    if (state().mobile != value)
      tokens.erase(Token32<Handle<T>>{token});
  } else if (!state().rings.contains(value))
    tokens.erase(Token32<Handle<T>>{token});
}
void input_tracking(CallContext &ctx) {
  a32::ArgCursor32 c(ctx.cpu());
  auto mobile = resolve<abi::Mobile>(c.word());
  auto id = c.word();
  auto time = std::bit_cast<double>(c.pair());
  auto out = c.word();
  if (!out) {
    ctx.return_x(abi::invalid);
    return;
  }
  abi::TrackingHead tracking{};
  auto r = guest::vrapi_GetInputTrackingState(mobile, id, time, &tracking);
  // VrApi_Input.h returns ovrTracking (104), NOT ovrTracking2 (360).
  // OVRPlugin @0x2fc06 passes its output pointer on the stack after r2:r3 time.
  if (r == abi::success)
    store(out, abi32::Tracking{tracking.status, 0, tracking.head});
  ctx.return_x(r);
}
void input_state(CallContext &ctx) {
  auto out = ctx.x(2);
  if (!out) {
    ctx.return_x(abi::invalid);
    return;
  }
  auto header = load<abi::InputStateHeader>(out);
  if (header.type != 4) {
    ctx.return_x(abi::no_device);
    return;
  }
  abi::RemoteState value{};
  value.header = header;
  auto r = guest::vrapi_GetCurrentInputState(resolve<abi::Mobile>(ctx.x(0)),
                                             ctx.x(1), &value.header);
  // FC MapControllerState4 @0x32fc0 clears 0x48 bytes at sp+0x38;
  // RemoteState contains no pointers and double alignment is unchanged.
  if (r == abi::success)
    store(out, value);
  ctx.return_x(r);
}
void input_capabilities(CallContext &ctx) {
  auto out = ctx.x(1);
  if (!out) {
    ctx.return_x(abi::invalid);
    return;
  }
  auto header = load<abi::InputHeader>(out);
  if (header.type != 4) {
    ctx.return_x(abi::no_device);
    return;
  }
  abi::RemoteCapabilities value{};
  value.header = header;
  auto r = guest::vrapi_GetInputDeviceCapabilities(
      resolve<abi::Mobile>(ctx.x(0)), &value.header);
  if (r == abi::success)
    store(out, value);
  ctx.return_x(r);
}
void enumerate_input(CallContext &ctx) {
  if (!ctx.x(2)) {
    ctx.return_x(abi::invalid);
    return;
  }
  auto mobile = resolve<abi::Mobile>(ctx.x(0));
  auto wanted = std::uint32_t(ctx.x(1));
  // First Contact's 1.21 input ABI supports tracked remotes. Do not advertise
  // newer hand records until their A32 layouts and entry points are supported.
  for (std::uint32_t index = 0;; ++index) {
    abi::InputHeader header{};
    auto r = guest::vrapi_EnumerateInputDevices(mobile, index, &header);
    if (r != abi::success) {
      ctx.return_x(r);
      return;
    }
    if (header.type == 4 && wanted-- == 0) {
      store(ctx.x(2), header);
      ctx.return_x(abi::success);
      return;
    }
  }
}
void haptic(CallContext &ctx) {
  if (!ctx.x(2)) {
    haptic_abi_error(ctx, "null_buffer");
    ctx.return_x(abi::invalid);
    return;
  }
  auto w = load<abi32::HapticBuffer>(ctx.x(2));
  if (w.count > 256 || (w.count && !w.samples)) {
    haptic_abi_error(ctx, "invalid_buffer");
    ctx.return_x(abi::invalid);
    return;
  }
  std::array<std::uint8_t, 256> samples{};
  if (w.count)
    a32::read_bytes(w.samples, samples.data(), w.count);
  abi::HapticBuffer n{w.time, w.count, w.terminated != 0, samples.data()};
  ctx.return_x(guest::vrapi_SetHapticVibrationBuffer(
      resolve<abi::Mobile>(ctx.x(0)), ctx.x(1), &n));
}
abi::LayerHeader header(abi32::LayerHeader h) {
  return {h.type, h.flags, h.color_scale, h.src_blend, h.dst_blend, h.reserved};
}
using Layer = std::variant<abi::LayerHeader, abi::Projection, abi::Cylinder,
                           abi::Cube, abi::Equirect, abi::LoadingIcon>;
template <class W, class N> N layer(GuestAddr p) {
  const auto w = load<W>(p);
  N n{};
  n.header = header(w.header);
  n.head = w.head;
  if constexpr (requires { n.tan_angles; })
    n.tan_angles = w.tan_angles;
  if constexpr (requires { n.offset; })
    n.offset = w.offset;
  for (int i = 0; i < 2; ++i) {
    auto &t = n.textures[i];
    const auto &s = w.textures[i];
    t.chain = resolve<abi::TextureSwapChain>(s.chain);
    t.index = s.index;
    if constexpr (requires { t.rect; })
      t.rect = s.rect;
    if constexpr (requires { t.tan_angles; })
      t.tan_angles = s.tan_angles;
    if constexpr (requires { t.texture_matrix; })
      t.texture_matrix = s.texture_matrix;
  }
  return n;
}
void submit(CallContext &ctx) {
  if (!ctx.x(1)) {
    ctx.return_x(abi::invalid);
    return;
  }
  auto w = load<abi32::Submit>(ctx.x(1));
  if (w.layer_count > 16 || (w.layer_count && !w.layers)) {
    ctx.return_x(abi::invalid);
    return;
  }
  std::array<Layer, 16> storage;
  std::array<const abi::LayerHeader *, 16> pointers{};
  for (std::uint32_t i = 0; i < w.layer_count; ++i) {
    auto p = load<std::uint32_t>(w.layers + 4ull * i);
    auto h = load<abi32::LayerHeader>(p);
    switch (h.type) {
    case 1:
      storage[i] = layer<abi32::Projection, abi::Projection>(p);
      break;
    case 3:
      storage[i] = layer<abi32::Cylinder, abi::Cylinder>(p);
      break;
    case 4:
      storage[i] = layer<abi32::Cube, abi::Cube>(p);
      break;
    case 5:
      storage[i] = layer<abi32::Equirect, abi::Equirect>(p);
      break;
    case 6: {
      auto l = load<abi32::LoadingIcon>(p);
      storage[i] =
          abi::LoadingIcon{header(l.header), l.spin_speed, l.spin_scale,
                           resolve<abi::TextureSwapChain>(l.chain), l.index};
      break;
    }
    default:
      storage[i] = header(h);
      break; // existing compositor rejects unsupported types
    }
    pointers[i] = std::visit(
        [](const auto &l) -> const abi::LayerHeader * {
          if constexpr (std::is_same_v<std::decay_t<decltype(l)>,
                                       abi::LayerHeader>)
            return &l;
          else
            return &l.header;
        },
        storage[i]);
  }
  abi::Submit n{w.flags,        w.swap_interval,    w.frame_index,
                w.display_time, w.completion_fence, w.layer_count,
                pointers.data()};
  ctx.return_x(guest::vrapi_SubmitFrame2(resolve<abi::Mobile>(ctx.x(0)), &n));
}
void legacy_submit(CallContext &ctx) {
  if (!ctx.x(1)) {
    guest::vrapi_SubmitFrame(resolve<abi::Mobile>(ctx.x(0)), nullptr);
    return;
  }
  auto w = load<abi32::LegacyFrame>(ctx.x(1));
  abi::LegacyFrame n{};
  n.type = w.type;
  n.layer_count = w.layer_count;
  n.flags = w.flags;
  n.frame_index = w.frame_index;
  n.swap_interval = w.swap_interval;
  n.latency_mode = w.latency_mode;
  n.reserved = w.reserved;
  n.reserved1 = w.reserved1;
  n.performance = w.performance;
  n.java = java(w.java);
  if (w.layer_count < 0 || w.layer_count > 4) {
    log::warn("vrapi", "SubmitFrame: invalid A32 layer count");
    return;
  }
  for (int i = 0; i < w.layer_count; ++i) {
    auto &d = n.layers[i];
    const auto &s = w.layers[i];
    d.spin_speed = s.spin_speed;
    d.spin_scale = s.spin_scale;
    d.color_scale = s.color_scale;
    d.src_blend = s.src_blend;
    d.dst_blend = s.dst_blend;
    d.flags = s.flags;
    for (int e = 0; e < 2; ++e) {
      const auto &t = s.textures[e];
      d.textures[e] = {resolve<abi::TextureSwapChain>(t.color),
                       resolve<abi::TextureSwapChain>(t.depth),
                       t.index,
                       t.tan_angles,
                       t.rect,
                       0,
                       t.head,
                       t.completion_fence};
    }
  }
  guest::vrapi_SubmitFrame(resolve<abi::Mobile>(ctx.x(0)), &n);
}
void point(CallContext &ctx) {
  a32::ArgCursor32 c(ctx.cpu());
  auto m = resolve<abi::Mobile>(c.word());
  abi::Vec3 v;
  c.composite(std::as_writable_bytes(std::span(&v, 1)));
  auto inside = c.word();
  auto boundary = c.word();
  ctx.return_x(
      guest::test_point(m, v, g2h<bool>(inside), g2h<abi::Boundary>(boundary)));
}
void vulkan_extensions(CallContext &ctx) {
  // This packet implements GLES swapchains only. Do not expose host Vulkan
  // extension requirements as if vrapi_CreateSystemVulkan were implemented.
  if (!ctx.x(1)) {
    ctx.return_x(abi::invalid);
    return;
  }
  store(ctx.x(1), std::uint32_t(0));
  log::warn("vrapi", "{}: A32 Vulkan bridge unsupported", ctx.info().name);
  ctx.return_x(abi::unsupported);
}
} // namespace
void register_a32_adapters() {
  auto add = [](const char *name, Handler h) {
    a32::register_adapter("libvrapi.so", name, h);
  };
#define FORWARD(name) add(#name, guarded<forward<&guest::name>>)
  FORWARD(vrapi_GetPredictedTracking2);
  FORWARD(vrapi_GetPredictedTracking);
  FORWARD(vrapi_GetPredictedDisplayTime);
  FORWARD(vrapi_LocateTrackingSpace);
  FORWARD(vrapi_GetSystemPropertyInt);
  FORWARD(vrapi_GetSystemPropertyFloat);
  FORWARD(vrapi_GetSystemPropertyFloatArray);
  FORWARD(vrapi_GetSystemStatusInt);
  FORWARD(vrapi_GetSystemStatusFloat);
  FORWARD(vrapi_GetPropertyInt);
  FORWARD(vrapi_SetPropertyInt);
  FORWARD(vrapi_SetPropertyFloat);
  FORWARD(vrapi_SetClockLevels);
  FORWARD(vrapi_SetPerfThread);
  FORWARD(vrapi_SetExtraLatencyMode);
  FORWARD(vrapi_SetDisplayRefreshRate);
  FORWARD(vrapi_SetTrackingSpace);
  FORWARD(vrapi_RecenterPose);
  FORWARD(vrapi_SetRemoteEmulation);
  FORWARD(vrapi_SetHapticVibrationSimple);
  FORWARD(vrapi_GetBoundaryGeometry);
  FORWARD(vrapi_GetBoundaryOrientedBoundingBox);
  FORWARD(vrapi_GetBoundaryTriggerState);
  FORWARD(vrapi_CreateTextureSwapChain);
  FORWARD(vrapi_GetTextureSwapChainLength);
  FORWARD(vrapi_GetTextureSwapChainHandle);
  FORWARD(vrapi_SetTextureSwapChainHandle);
#undef FORWARD
  add("vrapi_Initialize", guarded<initialize>);
  add("vrapi_EnterVrMode", guarded<enter>);
  add("vrapi_LeaveVrMode",
      guarded<release<abi::Mobile, guest::vrapi_LeaveVrMode>>);
  add("vrapi_DestroyTextureSwapChain",
      guarded<release<abi::TextureSwapChain,
                      guest::vrapi_DestroyTextureSwapChain>>);
  add("vrapi_GetInputTrackingState", guarded<input_tracking>);
  add("vrapi_EnumerateInputDevices", guarded<enumerate_input>);
  add("vrapi_GetInputDeviceCapabilities", guarded<input_capabilities>);
  add("vrapi_GetCurrentInputState", guarded<input_state>);
  add("vrapi_SetHapticVibrationBuffer", guarded<haptic>);
  add("vrapi_SubmitFrame2", guarded<submit>);
  add("vrapi_SubmitFrame", guarded<legacy_submit>);
  add("vrapi_TestPointIsInBoundary", guarded<point>);
  add("vrapi_GetInstanceExtensionsVulkan", guarded<vulkan_extensions>);
  add("vrapi_GetDeviceExtensionsVulkan", guarded<vulkan_extensions>);
  a32::register_adapter("libvrintegrationloader.so", "vrintegration_Initialize",
                        guarded<integration_initialize>);
}
} // namespace venture::hle::vrapi
