// A stand-in OpenVR client (vrclient_x64.dll) whose headset reports one camera
// and whose AcquireVideoStreamingService faults, as SteamVR's client did inside
// an AMD driver library. Only the function table slots Venture's OpenVR camera
// uses are filled (openvr_capi.h, IVRSystem_022 / IVRTrackedCamera_006).
#include <cstdint>
#include <cstring>

namespace {
struct Vector2 { float v[2]; };

std::int32_t has_camera(std::uint32_t, bool* out) { *out = true; return 0; }
std::int32_t frame_size(std::uint32_t, std::int32_t, std::uint32_t* w, std::uint32_t* h, std::uint32_t* bytes) {
  *w = 640; *h = 480; *bytes = 4; return 0;
}
std::int32_t intrinsics(std::uint32_t, std::uint32_t, std::int32_t, Vector2* focal, Vector2* center) {
  *focal = {{300, 300}}; *center = {{320, 240}}; return 0;
}
std::int32_t acquire(std::uint32_t, std::uint64_t* handle) {
  *handle = 0;
  *static_cast<volatile std::int32_t*>(nullptr) = 1; // the driver library's fault
  return 0;
}
std::int32_t release(std::uint64_t) { return 0; }
std::int32_t tracking_space() { return 1; }

void dxgi_output(std::int32_t* index) { *index = -1; }
std::int32_t int32_property(std::uint32_t, std::int32_t prop, std::int32_t* error) {
  *error = 0;
  if (prop == 1039) return 1;   // Prop_NumCameras_Int32
  if (prop == 1040) return 1;   // Prop_CameraFrameLayout_Int32: mono
  *error = 1;
  return 0;
}
std::uint32_t array_property(std::uint32_t, std::int32_t, std::uint32_t, void* out, std::uint32_t size, std::int32_t* error) {
  const float identity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
  *error = 0;
  if (size < sizeof(identity)) return sizeof(identity);
  std::memcpy(out, identity, sizeof(identity));
  return sizeof(identity);
}

void* camera_table[14] = {nullptr, reinterpret_cast<void*>(has_camera), reinterpret_cast<void*>(frame_size),
                          reinterpret_cast<void*>(intrinsics), nullptr, reinterpret_cast<void*>(acquire),
                          reinterpret_cast<void*>(release), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                          reinterpret_cast<void*>(tracking_space)};
void* system_table[27] = {};

void* generic_interface(void*, const char* name, std::int32_t* error) {
  *error = 0;
  if (!std::strcmp(name, "FnTable:IVRSystem_022")) return system_table;
  if (!std::strcmp(name, "FnTable:IVRTrackedCamera_006")) return camera_table;
  *error = 1;
  return nullptr;
}
void* core_vtable[4] = {nullptr, nullptr, nullptr, reinterpret_cast<void*>(generic_interface)};
struct Core { void** vtable; } core{core_vtable};
} // namespace

extern "C" __declspec(dllexport) void* VRClientCoreFactory(const char*, int* error) {
  system_table[7] = reinterpret_cast<void*>(dxgi_output);
  system_table[23] = reinterpret_cast<void*>(int32_property);
  system_table[26] = reinterpret_cast<void*>(array_property);
  *error = 0;
  return &core;
}
