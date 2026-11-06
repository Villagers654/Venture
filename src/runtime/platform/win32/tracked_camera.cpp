// IVRTrackedCamera through the OpenVR client already loaded by the host OpenXR
// runtime. VR_Init must not run in an OpenXR process, so the client core's
// factory and the C function tables (FnTable:) are used, as OpenVR's own C API
// does. Frames come as D3D11 textures on a private device and are read back on
// a worker; GetVideoStreamFrameBuffer with a buffer is avoided (it fails in
// Vulkan applications), its header-only form polls for new frames.
#include "xr/tracked_camera.h"
#include "common/log.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace venture::xr {
namespace {
// OpenVR API contract (openvr_capi.h, IVRSystem_022 / IVRTrackedCamera_006).
struct HmdMatrix34 { float m[3][4]; };
struct HmdVector2 { float v[2]; };
struct HmdVector3 { float v[3]; };
struct TrackedDevicePose {
  HmdMatrix34 pose; HmdVector3 velocity, angular_velocity;
  std::int32_t result; bool valid, connected;
};
struct FrameHeader {
  std::int32_t type; std::uint32_t width, height, bytes_per_pixel, sequence;
  TrackedDevicePose pose; std::uint64_t exposure;
};
static_assert(sizeof(TrackedDevicePose) == 80 && sizeof(FrameHeader) == 112);
constexpr std::uint32_t hmd = 0;               // k_unTrackedDeviceIndex_Hmd
constexpr std::int32_t frame_type = 2;         // VRTrackedCameraFrameType_MaximumUndistorted
constexpr std::int32_t prop_num_cameras = 1039, prop_frame_layout = 1040, prop_camera_to_head = 2055;
constexpr std::uint32_t matrix34_tag = 20;
struct SystemTable {
  void* f[27];
  void dxgi_output(std::int32_t* index) const { reinterpret_cast<void (*)(std::int32_t*)>(f[7])(index); }
  void poses(std::int32_t origin, float predicted, TrackedDevicePose* out, std::uint32_t count) const {
    reinterpret_cast<void (*)(std::int32_t, float, TrackedDevicePose*, std::uint32_t)>(f[11])(origin, predicted, out, count);
  }
  std::int32_t int32(std::int32_t prop, std::int32_t* error) const {
    return reinterpret_cast<std::int32_t (*)(std::uint32_t, std::int32_t, std::int32_t*)>(f[23])(hmd, prop, error);
  }
  std::uint32_t array(std::int32_t prop, std::uint32_t tag, void* out, std::uint32_t size, std::int32_t* error) const {
    return reinterpret_cast<std::uint32_t (*)(std::uint32_t, std::int32_t, std::uint32_t, void*, std::uint32_t, std::int32_t*)>(
        f[26])(hmd, prop, tag, out, size, error);
  }
};
struct CameraTable {
  void* f[14];
  std::int32_t has_camera(bool* out) const { return reinterpret_cast<std::int32_t (*)(std::uint32_t, bool*)>(f[1])(hmd, out); }
  std::int32_t frame_size(std::uint32_t* w, std::uint32_t* h, std::uint32_t* bytes) const {
    return reinterpret_cast<std::int32_t (*)(std::uint32_t, std::int32_t, std::uint32_t*, std::uint32_t*, std::uint32_t*)>(f[2])(
        hmd, frame_type, w, h, bytes);
  }
  std::int32_t intrinsics(std::uint32_t camera, HmdVector2* focal, HmdVector2* center) const {
    return reinterpret_cast<std::int32_t (*)(std::uint32_t, std::uint32_t, std::int32_t, HmdVector2*, HmdVector2*)>(f[3])(
        hmd, camera, frame_type, focal, center);
  }
  std::int32_t acquire(std::uint64_t* handle) const {
    return reinterpret_cast<std::int32_t (*)(std::uint32_t, std::uint64_t*)>(f[5])(hmd, handle);
  }
  std::int32_t release(std::uint64_t handle) const { return reinterpret_cast<std::int32_t (*)(std::uint64_t)>(f[6])(handle); }
  std::int32_t header(std::uint64_t handle, FrameHeader* out) const {
    return reinterpret_cast<std::int32_t (*)(std::uint64_t, std::int32_t, void*, std::uint32_t, FrameHeader*, std::uint32_t)>(f[7])(
        handle, frame_type, nullptr, 0, out, sizeof(FrameHeader));
  }
  std::int32_t texture(std::uint64_t handle, void* device, void** view, FrameHeader* out) const {
    return reinterpret_cast<std::int32_t (*)(std::uint64_t, std::int32_t, void*, void**, FrameHeader*, std::uint32_t)>(f[9])(
        handle, frame_type, device, view, out, sizeof(FrameHeader));
  }
  std::int32_t tracking_space() const { return reinterpret_cast<std::int32_t (*)()>(f[13])(); }
};
struct Tables { const SystemTable* system = nullptr; const CameraTable* camera = nullptr; };
// IVRClientCore_003 has no virtual destructor; GetGenericInterface is slot 3.
Tables tables() {
  const auto module = GetModuleHandleW(L"vrclient_x64.dll");
  if (!module) return {};
  using Factory = void* (*)(const char*, int*);
  const auto factory = reinterpret_cast<Factory>(reinterpret_cast<void*>(GetProcAddress(module, "VRClientCoreFactory")));
  if (!factory) return {};
  int error = 0;
  auto* core = factory("IVRClientCore_003", &error);
  if (!core || error) return {};
  using Generic = void* (*)(void*, const char*, std::int32_t*);
  const auto generic = reinterpret_cast<Generic>((*static_cast<void***>(core))[3]);
  std::int32_t system_error = 0, camera_error = 0;
  auto* system = generic(core, "FnTable:IVRSystem_022", &system_error);
  auto* camera = generic(core, "FnTable:IVRTrackedCamera_006", &camera_error);
  if (!system || !camera || system_error || camera_error) return {};
  return {static_cast<const SystemTable*>(system), static_cast<const CameraTable*>(camera)};
}
XrPosef pose_of(const HmdMatrix34& matrix) { return rigid_pose(matrix.m); }
template<class T> struct Com {
  T* value = nullptr;
  ~Com() { if (value) value->Release(); }
  T** put() { if (value) { value->Release(); value = nullptr; } return &value; }
  T* operator->() const { return value; }
  explicit operator bool() const { return value; }
};

class OpenVRCamera final : public TrackedCamera {
 public:
  Tables api;
  std::uint64_t handle = 0;
  std::int32_t universe = 1;
  std::vector<TrackedCameraView> regions;
  std::uint32_t declared_width = 0, declared_height = 0;
  Com<ID3D11Device> device;
  Com<ID3D11DeviceContext> context;
  Com<ID3D11Texture2D> staging;
  D3D11_TEXTURE2D_DESC staging_desc{};
  std::mutex mutex;
  TrackedCameraFrame ready;   // newest complete frame (guarded)
  TrackedCameraFrame filling; // worker-owned
  std::atomic<std::uint64_t> last_frame_ms{0};
  std::atomic<bool> stopping{false};
  std::thread worker;

  ~OpenVRCamera() override {
    stopping.store(true);
    if (worker.joinable()) worker.join();
    if (handle) api.camera->release(handle);
  }
  const std::vector<TrackedCameraView>& views() const override { return regions; }
  std::uint32_t frame_width() const override { return declared_width; }
  std::uint32_t frame_height() const override { return declared_height; }
  bool latest(TrackedCameraFrame& frame) override {
    std::lock_guard lock(mutex);
    if (!ready.sequence || ready.sequence == frame.sequence) return false;
    std::swap(frame, ready);
    return true;
  }
  bool head_now(XrPosef& pose) override {
    TrackedDevicePose current{};
    api.system->poses(universe, 0, &current, 1);
    if (!current.valid) return false;
    pose = pose_of(current.pose);
    return true;
  }
  std::uint64_t stalled_ms() const override { return GetTickCount64() - last_frame_ms.load(); }

  // D3D11 is loaded only when a camera streams, never into every game.
  bool create_device() {
    const auto d3d11 = LoadLibraryW(L"d3d11.dll"), dxgi = LoadLibraryW(L"dxgi.dll");
    if (!d3d11 || !dxgi) return false;
    const auto create_device = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
        reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice")));
    using CreateFactory = HRESULT (WINAPI*)(REFIID, void**);
    const auto create_factory = reinterpret_cast<CreateFactory>(
        reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
    if (!create_device || !create_factory) return false;
    std::int32_t adapter_index = -1;
    api.system->dxgi_output(&adapter_index);
    Com<IDXGIFactory1> factory;
    Com<IDXGIAdapter1> adapter;
    if (adapter_index >= 0 && SUCCEEDED(create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.put()))))
      factory->EnumAdapters1(UINT(adapter_index), adapter.put());
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    auto create = [&](IDXGIAdapter* which, D3D_DRIVER_TYPE type) {
      return SUCCEEDED(create_device(which, type, nullptr, 0, levels, 1, D3D11_SDK_VERSION,
                                     device.put(), nullptr, context.put()));
    };
    return (adapter && create(adapter.value, D3D_DRIVER_TYPE_UNKNOWN)) || create(nullptr, D3D_DRIVER_TYPE_HARDWARE) ||
           create(nullptr, D3D_DRIVER_TYPE_WARP);
  }
  // Copies the camera texture into `filling`; RGBA8 and BGRA8 only.
  bool read_back(ID3D11ShaderResourceView* view) {
    Com<ID3D11Resource> resource;
    view->GetResource(resource.put());
    Com<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.put()))))
      return false;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    const auto format = desc.Format;
    const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                      format == DXGI_FORMAT_B8G8R8A8_TYPELESS || format == DXGI_FORMAT_B8G8R8X8_UNORM;
    const bool rgba = format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                      format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    if ((!bgra && !rgba) || !desc.Width || !desc.Height || desc.Width > 8192 || desc.Height > 8192) return false;
    if (!staging || staging_desc.Width != desc.Width || staging_desc.Height != desc.Height || staging_desc.Format != desc.Format) {
      staging_desc = desc;
      staging_desc.MipLevels = staging_desc.ArraySize = 1;
      staging_desc.SampleDesc = {1, 0};
      staging_desc.Usage = D3D11_USAGE_STAGING;
      staging_desc.BindFlags = 0;
      staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      staging_desc.MiscFlags = 0;
      if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, staging.put()))) return false;
    }
    context->CopySubresourceRegion(staging.value, 0, 0, 0, 0, texture.value, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.value, 0, D3D11_MAP_READ, 0, &mapped))) return false;
    filling.width = desc.Width;
    filling.height = desc.Height;
    filling.rgba.resize(std::size_t(desc.Width) * desc.Height * 4);
    for (std::uint32_t y = 0; y < desc.Height; ++y) {
      auto* out = filling.rgba.data() + std::size_t(y) * desc.Width * 4;
      std::memcpy(out, static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch, desc.Width * 4);
      if (bgra) for (std::uint32_t x = 0; x < desc.Width; ++x) std::swap(out[x * 4], out[x * 4 + 2]);
    }
    context->Unmap(staging.value, 0);
    return true;
  }
  void run() noexcept {
    std::uint32_t last = 0;
    bool reported = false;
    while (!stopping.load()) {
      FrameHeader header{};
      if (api.camera->header(handle, &header) != 0 || header.sequence == last) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      void* view = nullptr;
      header = {};
      // The view belongs to the runtime; only its resource reference is released.
      if (api.camera->texture(handle, device.value, &view, &header) != 0 || !view ||
          !read_back(static_cast<ID3D11ShaderResourceView*>(view))) {
        if (!std::exchange(reported, true))
          log::warn("xr-passthrough", "OpenVR camera frame could not be read (format or texture access)");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      last = header.sequence;
      filling.sequence = std::uint64_t(header.sequence) + 1; // never 0
      filling.head_valid = header.pose.valid;
      if (header.pose.valid) filling.head = pose_of(header.pose.pose);
      {
        std::lock_guard lock(mutex);
        std::swap(filling, ready);
      }
      last_frame_ms.store(GetTickCount64());
    }
  }
};
} // namespace

bool tracked_camera_present() {
  const auto api = tables();
  if (!api.camera) return false;
  bool has = false;
  std::uint32_t width = 0, height = 0, bytes = 0;
  if (api.camera->has_camera(&has) != 0 || !has) return false;
  std::int32_t error = 0;
  const auto cameras = api.system->int32(prop_num_cameras, &error);
  if (!error && cameras < 1) return false; // a camera without cameras (reported by some headsets)
  return api.camera->frame_size(&width, &height, &bytes) == 0 && width && height;
}

std::unique_ptr<TrackedCamera> open_tracked_camera(unsigned first_frame_ms) {
  auto camera = std::make_unique<OpenVRCamera>();
  camera->api = tables();
  const auto& api = camera->api;
  if (!api.camera || !tracked_camera_present()) return nullptr;
  std::uint32_t width = 0, height = 0, bytes = 0;
  api.camera->frame_size(&width, &height, &bytes);
  camera->declared_width = width;
  camera->declared_height = height;
  std::int32_t error = 0;
  const auto layout = api.system->int32(prop_frame_layout, &error);
  const auto cameras = api.system->int32(prop_num_cameras, &error);
  HmdMatrix34 transforms[4]{};
  error = 0;
  const auto transform_bytes = api.system->array(prop_camera_to_head, matrix34_tag, transforms, sizeof(transforms), &error);
  const auto transform_count = error ? 0u : transform_bytes / sizeof(HmdMatrix34);
  for (const auto& region : tracked_camera_regions(layout, width, height, cameras)) {
    TrackedCameraView view{region.x, region.y, region.width, region.height};
    HmdVector2 focal{}, center{};
    if (api.camera->intrinsics(region.camera, &focal, &center) != 0 || focal.v[0] <= 0 || focal.v[1] <= 0) return nullptr;
    view.fx = focal.v[0]; view.fy = focal.v[1]; view.cx = center.v[0]; view.cy = center.v[1];
    // Intrinsics given in whole-frame pixels are moved into the region.
    if (view.cx > float(region.width)) view.cx -= float(region.x);
    if (view.cy > float(region.height)) view.cy -= float(region.y);
    if (region.camera >= transform_count) return nullptr;
    view.camera_to_head = tracked_camera_pose(transforms[region.camera].m);
    camera->regions.push_back(view);
  }
  if (!camera->create_device()) return nullptr;
  if (api.camera->acquire(&camera->handle) != 0 || !camera->handle) {
    camera->handle = 0;
    return nullptr;
  }
  camera->universe = api.camera->tracking_space();
  camera->last_frame_ms.store(GetTickCount64());
  try { camera->worker = std::thread([raw = camera.get()] { raw->run(); }); } catch (...) { return nullptr; }
  const auto deadline = GetTickCount64() + first_frame_ms;
  while (GetTickCount64() < deadline) {
    {
      std::lock_guard lock(camera->mutex);
      if (camera->ready.sequence) break;
    }
    Sleep(5);
  }
  std::lock_guard lock(camera->mutex);
  if (!camera->ready.sequence) return nullptr;
  const auto& first = camera->ready;
  log::info("xr-passthrough", "OpenVR camera streaming: frame {}x{}, {} view(s), layout {:#x}", first.width, first.height,
            camera->regions.size(), layout);
  return camera;
}
} // namespace venture::xr
