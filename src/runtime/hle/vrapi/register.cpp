#include "core/hle.h"
#include "core/linker.h"
#include "core/process.h"
#include "core/guest_storage.h"
#include "common/log.h"
#include "hle/vrapi/integration.h"
#include "hle/vrapi/vrapi.h"
#include "xr/marshal.h"
#include "xr/xr_images.h"
#include <cstring>
#include <mutex>

namespace venture::hle::vrapi {
namespace {
template <auto F> void add(HleLibrary &lib, const char *name) {
  lib.add_function(name, make_thunk(lib.soname(), name, xr::wrap_xr<F>()));
}
void tracking_return(CallContext &ctx) {
  // AAPCS64 5.4/6.8: a 360-byte aggregate is returned through X8, which
  // does not consume X0 (ovrMobile*) or D0 (prediction time).
  auto value = guest::vrapi_GetPredictedTracking2(g2h<abi::Mobile>(ctx.x(0)), ctx.d(0));
  std::memcpy(g2h(ctx.x(8)), &value, sizeof(value));
}
void tracking_head_return(CallContext &ctx) {
  auto value = guest::vrapi_GetPredictedTracking(g2h<abi::Mobile>(ctx.x(0)), ctx.d(0));
  std::memcpy(g2h(ctx.x(8)), &value, sizeof(value));
}
template <auto F> void pose_return(CallContext &ctx) {
  auto value = F(g2h<abi::Mobile>(ctx.x(0)), std::int32_t(ctx.x(1)));
  std::memcpy(g2h(ctx.x(8)), &value, sizeof(value));
}
void point_arg(CallContext &ctx) {
  // ovrVector3f is a homogeneous three-float aggregate: S0/S1/S2.
  auto r = guest::test_point(g2h<abi::Mobile>(ctx.x(0)), {ctx.s(0), ctx.s(1), ctx.s(2)},
                             g2h<bool>(ctx.x(1)), g2h<abi::Boundary>(ctx.x(2)));
  ctx.return_x(std::uint64_t(std::int64_t(r)));
}
void color_return(CallContext &ctx) {
  auto value = guest::vrapi_GetHmdColorDesc(g2h<abi::Mobile>(ctx.x(0)));
  std::uint64_t bits{};
  std::memcpy(&bits, &value, 8);
  ctx.return_x(bits);
}
void unavailable_entry(CallContext &ctx) {
  log::warn("vrapi", "{} is unsupported", ctx.info().name);
  const auto& name = ctx.info().name;
  ctx.return_x(name == "vrapi_CreateAndroidSurfaceSwapChain" ||
               name == "vrapi_GetTextureSwapChainAndroidSurface" ||
               name == "vrapi_ShowSystemUI" ? 0 : std::uint64_t(std::int64_t(abi::unsupported)));
}
void version_string(CallContext& ctx) {
  // This identifies Venture's adapter, not the bundled vendor loader. Like
  // VrApi's version query, it is a borrowed constant available before init.
  ctx.return_x(h2g(guest_string("Venture VrApi compatibility 1.1")));
}
} // namespace
void register_libraries() {
  xr::install_angle_xr_images();
  static std::once_flag once;
  std::call_once(once, [] {
    const bool is32 = process_config().isa() == GuestIsa::a32;
    if (is32) register_a32_adapters();
    auto lib = std::make_shared<HleLibrary>(
        "libvrapi.so",
        std::vector<std::string>{"/system/lib64/libvrapi.so", "/system/lib/libvrapi.so",
                                 process_config().guest_native_lib_dir() + "/libvrapi.so"});
#define VENTURE_VRAPI(name) add<&guest::name>(*lib, #name)
    VENTURE_VRAPI(vrapi_Initialize);
    VENTURE_VRAPI(vrapi_Shutdown);
    if (!is32) {
      VENTURE_VRAPI(vrapi_CreateSystemVulkan);
      VENTURE_VRAPI(vrapi_DestroySystemVulkan);
      VENTURE_VRAPI(vrapi_GetTextureSwapChainBufferVulkan);
      VENTURE_VRAPI(vrapi_GetTextureSwapChainBufferFoveationVulkan);
    }
    VENTURE_VRAPI(vrapi_EnterVrMode);
    VENTURE_VRAPI(vrapi_LeaveVrMode);
    VENTURE_VRAPI(vrapi_GetTimeInSeconds);
    VENTURE_VRAPI(vrapi_GetPredictedDisplayTime);
    VENTURE_VRAPI(vrapi_WaitFrame);
    VENTURE_VRAPI(vrapi_BeginFrame);
    VENTURE_VRAPI(vrapi_SubmitFrame);
    VENTURE_VRAPI(vrapi_SubmitFrame2);
    VENTURE_VRAPI(vrapi_CreateTextureSwapChain);
    VENTURE_VRAPI(vrapi_CreateTextureSwapChain3);
    VENTURE_VRAPI(vrapi_CreateTextureSwapChain4);
    VENTURE_VRAPI(vrapi_DestroyTextureSwapChain);
    VENTURE_VRAPI(vrapi_GetTextureSwapChainLength);
    VENTURE_VRAPI(vrapi_GetTextureSwapChainHandle);
    VENTURE_VRAPI(vrapi_SetTextureSwapChainHandle);
    VENTURE_VRAPI(vrapi_GetTextureSwapChainSamplerState);
    VENTURE_VRAPI(vrapi_SetTextureSwapChainSamplerState);
    VENTURE_VRAPI(vrapi_GetSystemPropertyInt);
    VENTURE_VRAPI(vrapi_GetSystemPropertyFloat);
    VENTURE_VRAPI(vrapi_GetSystemPropertyFloatArray);
    VENTURE_VRAPI(vrapi_GetSystemStatusInt);
    VENTURE_VRAPI(vrapi_GetSystemStatusFloat);
    VENTURE_VRAPI(vrapi_GetPropertyInt);
    VENTURE_VRAPI(vrapi_SetPropertyInt);
    VENTURE_VRAPI(vrapi_SetPropertyFloat);
    VENTURE_VRAPI(vrapi_SetClockLevels);
    VENTURE_VRAPI(vrapi_SetPerfThread);
    VENTURE_VRAPI(vrapi_SetExtraLatencyMode);
    VENTURE_VRAPI(vrapi_SetDisplayRefreshRate);
    VENTURE_VRAPI(vrapi_SetTrackingSpace);
    VENTURE_VRAPI(vrapi_RecenterPose);
    VENTURE_VRAPI(vrapi_PollEvent);
    VENTURE_VRAPI(vrapi_GetBoundaryGeometry);
    VENTURE_VRAPI(vrapi_GetBoundaryOrientedBoundingBox);
    VENTURE_VRAPI(vrapi_GetBoundaryTriggerState);
    VENTURE_VRAPI(vrapi_EnumerateInputDevices);
    VENTURE_VRAPI(vrapi_GetInputDeviceCapabilities);
    VENTURE_VRAPI(vrapi_GetCurrentInputState);
    VENTURE_VRAPI(vrapi_GetInputTrackingState);
    VENTURE_VRAPI(vrapi_SetRemoteEmulation);
    VENTURE_VRAPI(vrapi_SetHapticVibrationSimple);
    VENTURE_VRAPI(vrapi_GetHandPose);
    VENTURE_VRAPI(vrapi_GetHandSkeleton);
    VENTURE_VRAPI(vrapi_GetHandMesh);
    VENTURE_VRAPI(vrapi_SetHapticVibrationBuffer);
    VENTURE_VRAPI(vrapi_GetInstanceExtensionsVulkan);
    VENTURE_VRAPI(vrapi_GetDeviceExtensionsVulkan);
    VENTURE_VRAPI(vrapi_SetClientColorDesc);
#undef VENTURE_VRAPI
    auto manual = [&](const char *name, Handler handler) {
      lib->add_function(name, make_thunk(lib->soname(), name, handler));
    };
    if (is32) a32::register_adapter(lib->soname(), "vrapi_GetVersionString", version_string);
    manual("vrapi_GetVersionString", version_string);
    if (is32) for (auto name : {"vrapi_CreateSystemVulkan", "vrapi_GetTextureSwapChainBufferVulkan"}) {
      a32::register_adapter(lib->soname(), name, unavailable_entry);
      manual(name, unavailable_entry);
    }
    for (auto name : {"vrapi_CreateAndroidSurfaceSwapChain", "vrapi_GetTextureSwapChainAndroidSurface",
                      "vrapi_ShowSystemUI", "vrapi_GetBoundaryVisible", "vrapi_RequestBoundaryVisible"}) {
      if (is32) a32::register_adapter(lib->soname(), name, unavailable_entry);
      manual(name, unavailable_entry);
    }
    manual("vrapi_GetPredictedTracking2", tracking_return);
    manual("vrapi_GetPredictedTracking", tracking_head_return);
    manual("vrapi_LocateTrackingSpace", pose_return<&guest::vrapi_LocateTrackingSpace>);
    manual("vrapi_GetTrackingTransform", pose_return<&guest::vrapi_GetTrackingTransform>);
    manual("vrapi_TestPointIsInBoundary", point_arg);
    manual("vrapi_GetHmdColorDesc", color_return);
    add<&guest::set_tracking_transform>(
        *lib, "vrapi_SetTrackingTransform"); // >16-byte aggregate input is passed indirectly
    lib->add_function("_Z17vrapi_GetHandMeshP9ovrMobile14ovrHandedness_P18ovrHandMeshHeader_",
                      lib->find_symbol("vrapi_GetHandMesh"));
    lib->add_function("_Z17vrapi_GetHandPoseP9ovrMobilejdP18ovrHandPoseHeader_",
                      lib->find_symbol("vrapi_GetHandPose"));
    lib->add_function(
        "_Z21vrapi_GetHandSkeletonP9ovrMobile14ovrHandedness_P22ovrHandSkeletonHeader_",
        lib->find_symbol("vrapi_GetHandSkeleton"));
    Linker::get().register_hle_library(lib);
    auto integration_lib = std::make_shared<HleLibrary>(
        "libvrintegrationloader.so",
        std::vector<std::string>{"/system/lib64/libvrintegrationloader.so",
                                 process_config().guest_native_lib_dir() +
                                     "/libvrintegrationloader.so"});
    add<&integration::initialize>(*integration_lib, "vrintegration_Initialize");
    add<&integration::shutdown>(*integration_lib, "vrintegration_Shutdown");
    add<&integration::has_initialized>(*integration_lib, "vrintegration_HasInitialized");
    add<&integration::clear_state>(*integration_lib, "vrintegration_ClearState");
    add<&integration::set_thread_performance>(*integration_lib,
                                              "vrintegration_SetThreadPerformance");
    add<&integration::auto_thread_scheduling>(*integration_lib,
                                              "vrintegration_AutoThreadScheduling");
    add<&integration::usable_cores>(*integration_lib, "vrintegration_GetAppUsableCoreCount");
    Linker::get().register_hle_library(integration_lib);
  });
}
} // namespace venture::hle::vrapi
