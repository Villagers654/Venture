// Controller render models through the OpenVR client core's C function table
// (FnTable:IVRSystem_022), as xr/tracked_camera does for the headset camera.
#include "xr/openvr_devices.h"
#include <cstdint>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace venture::xr {
namespace {
// OpenVR API contract (openvr_capi.h, IVRSystem_022: the IVRSystem_026 table
// without ComputeDistortionSet, slot 4 there).
constexpr std::uint32_t kInvalidDevice = 0xFFFFFFFFu;  // k_unTrackedDeviceIndexInvalid
constexpr std::int32_t kLeftHand = 1, kRightHand = 2;  // TrackedControllerRole_LeftHand / RightHand
constexpr std::int32_t kRenderModelName = 1003;        // Prop_RenderModelName_String
struct SystemTable {
  void* f[28];
  std::uint32_t device_for_role(std::int32_t role) const {
    return reinterpret_cast<std::uint32_t (*)(std::int32_t)>(f[17])(role);
  }
  bool connected(std::uint32_t device) const { return reinterpret_cast<bool (*)(std::uint32_t)>(f[20])(device); }
  std::uint32_t string(std::uint32_t device, std::int32_t prop, char* out, std::uint32_t size, std::int32_t* error) const {
    return reinterpret_cast<std::uint32_t (*)(std::uint32_t, std::int32_t, char*, std::uint32_t, std::int32_t*)>(f[27])(
        device, prop, out, size, error);
  }
};
void* client_factory() {
#ifdef _WIN32
  const auto module = GetModuleHandleW(L"vrclient_x64.dll");
  return module ? reinterpret_cast<void*>(GetProcAddress(module, "VRClientCoreFactory")) : nullptr;
#else
  // Only a client already loaded (RTLD_NOLOAD): Venture never loads OpenVR itself.
  void* module = dlopen("vrclient.so", RTLD_NOW | RTLD_NOLOAD);
  if (!module) return nullptr;
  void* factory = dlsym(module, "VRClientCoreFactory");
  dlclose(module);  // drops only the reference RTLD_NOLOAD added
  return factory;
#endif
}
const SystemTable* system_table() {
  using Factory = void* (*)(const char*, int*);
  const auto factory = reinterpret_cast<Factory>(client_factory());
  if (!factory) return nullptr;
  int error = 0;
  auto* core = factory("IVRClientCore_003", &error);
  if (!core || error) return nullptr;
  // IVRClientCore_003 has no virtual destructor; GetGenericInterface is slot 3.
  using Generic = void* (*)(void*, const char*, std::int32_t*);
  const auto generic = reinterpret_cast<Generic>((*static_cast<void***>(core))[3]);
  std::int32_t system_error = 0;
  auto* system = generic(core, "FnTable:IVRSystem_022", &system_error);
  return system && !system_error ? static_cast<const SystemTable*>(system) : nullptr;
}
}  // namespace

std::array<std::string, 2> openvr_controller_render_models() {
  std::array<std::string, 2> out;
  const auto* system = system_table();
  if (!system) return out;
  for (int hand = 0; hand < 2; ++hand) {
    const auto device = system->device_for_role(hand ? kRightHand : kLeftHand);
    if (device == kInvalidDevice || !system->connected(device)) continue;
    char name[256]{};
    std::int32_t error = 0;
    const auto size = system->string(device, kRenderModelName, name, sizeof(name), &error);
    if (!error && size > 1 && size <= sizeof(name)) out[std::size_t(hand)].assign(name, size - 1);
  }
  return out;
}
}  // namespace venture::xr
