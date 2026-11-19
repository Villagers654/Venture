// SPDX-License-Identifier: GPL-3.0-or-later
// libvrapi.so's exports: VrApi's C entry points (VrApi.h, VrApi_Input.h,
// VrApi_Vulkan.h) with its ABI, each forwarding to the shared core
// (hle/vrapi/vrapi.h). exports.map lists the same names. The calls that carry
// the game's Java VM hand it to the host first: the runtime cannot be reached
// without it.
#include "android_game/vrapi/platform_xr.h"

#include "android_game/vrapi/native.h"
#include "common/log.h"
#include "hle/vrapi/vrapi.h"

#define VENTURE_VRAPI_EXPORT extern "C" __attribute__((visibility("default")))

namespace venture::hle::vrapi {
using namespace abi;
// clang-format off
#if defined(__LP64__)  // ILP32 forms: exports_ilp32.cpp
VENTURE_VRAPI_EXPORT std::int32_t vrapi_Initialize(const Init * a0) {
  if (a0) vrapi_native::remember_java(&a0->java);
  return guest::vrapi_Initialize(a0);
}
#endif
VENTURE_VRAPI_EXPORT void vrapi_Shutdown() { guest::vrapi_Shutdown(); }
#if defined(__LP64__)
VENTURE_VRAPI_EXPORT std::int32_t vrapi_CreateSystemVulkan(const SystemVulkan * a0) { return guest::vrapi_CreateSystemVulkan(a0); }
#endif
VENTURE_VRAPI_EXPORT void vrapi_DestroySystemVulkan() { guest::vrapi_DestroySystemVulkan(); }
VENTURE_VRAPI_EXPORT VkImage vrapi_GetTextureSwapChainBufferVulkan(TextureSwapChain * a0, std::int32_t a1) { return guest::vrapi_GetTextureSwapChainBufferVulkan(a0, a1); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetTextureSwapChainBufferFoveationVulkan(TextureSwapChain * a0, std::int32_t a1, VkImage * a2, std::uint32_t * a3, std::uint32_t * a4) { return guest::vrapi_GetTextureSwapChainBufferFoveationVulkan(a0, a1, a2, a3, a4); }
#if defined(__LP64__)
VENTURE_VRAPI_EXPORT Mobile * vrapi_EnterVrMode(const Mode * a0) {
  if (a0) vrapi_native::remember_java(&a0->java);
  return guest::vrapi_EnterVrMode(a0);
}
#endif
VENTURE_VRAPI_EXPORT void vrapi_LeaveVrMode(Mobile * a0) { guest::vrapi_LeaveVrMode(a0); }
VENTURE_VRAPI_EXPORT double vrapi_GetTimeInSeconds() { return guest::vrapi_GetTimeInSeconds(); }
VENTURE_VRAPI_EXPORT double vrapi_GetPredictedDisplayTime(Mobile * a0, std::int64_t a1) { return guest::vrapi_GetPredictedDisplayTime(a0, a1); }
VENTURE_VRAPI_EXPORT Tracking vrapi_GetPredictedTracking2(Mobile * a0, double a1) { return guest::vrapi_GetPredictedTracking2(a0, a1); }
VENTURE_VRAPI_EXPORT TrackingHead vrapi_GetPredictedTracking(Mobile * a0, double a1) { return guest::vrapi_GetPredictedTracking(a0, a1); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_WaitFrame(Mobile * a0, std::uint64_t a1) { return guest::vrapi_WaitFrame(a0, a1); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_BeginFrame(Mobile * a0, std::uint64_t a1) { return guest::vrapi_BeginFrame(a0, a1); }
#if defined(__LP64__)
VENTURE_VRAPI_EXPORT std::int32_t vrapi_SubmitFrame2(Mobile * a0, const Submit * a1) { return guest::vrapi_SubmitFrame2(a0, a1); }
VENTURE_VRAPI_EXPORT void vrapi_SubmitFrame(Mobile * a0, const LegacyFrame * a1) { guest::vrapi_SubmitFrame(a0, a1); }
#endif
VENTURE_VRAPI_EXPORT TextureSwapChain * vrapi_CreateTextureSwapChain4(const SwapChainCreate * a0) { return guest::vrapi_CreateTextureSwapChain4(a0); }
VENTURE_VRAPI_EXPORT TextureSwapChain * vrapi_CreateTextureSwapChain3(std::int32_t a0, std::int64_t a1, std::int32_t a2, std::int32_t a3, std::int32_t a4, std::int32_t a5) { return guest::vrapi_CreateTextureSwapChain3(a0, a1, a2, a3, a4, a5); }
VENTURE_VRAPI_EXPORT TextureSwapChain * vrapi_CreateTextureSwapChain2(std::int32_t a0, std::int32_t a1, std::int32_t a2, std::int32_t a3, std::int32_t a4, std::int32_t a5) { return guest::vrapi_CreateTextureSwapChain2(a0, a1, a2, a3, a4, a5); }
VENTURE_VRAPI_EXPORT TextureSwapChain * vrapi_CreateTextureSwapChain(std::int32_t a0, std::int32_t a1, std::int32_t a2, std::int32_t a3, std::int32_t a4, bool a5) { return guest::vrapi_CreateTextureSwapChain(a0, a1, a2, a3, a4, a5); }
VENTURE_VRAPI_EXPORT void vrapi_DestroyTextureSwapChain(TextureSwapChain * a0) { guest::vrapi_DestroyTextureSwapChain(a0); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetTextureSwapChainLength(TextureSwapChain * a0) { return guest::vrapi_GetTextureSwapChainLength(a0); }
VENTURE_VRAPI_EXPORT std::uint32_t vrapi_GetTextureSwapChainHandle(TextureSwapChain * a0, std::int32_t a1) { return guest::vrapi_GetTextureSwapChainHandle(a0, a1); }
VENTURE_VRAPI_EXPORT void vrapi_SetTextureSwapChainHandle(TextureSwapChain * a0, std::int32_t a1, std::uint32_t a2) { guest::vrapi_SetTextureSwapChainHandle(a0, a1, a2); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetTextureSwapChainSamplerState(TextureSwapChain * a0, TextureSamplerState * a1) { return guest::vrapi_GetTextureSwapChainSamplerState(a0, a1); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_SetTextureSwapChainSamplerState(TextureSwapChain * a0, const TextureSamplerState * a1) { return guest::vrapi_SetTextureSwapChainSamplerState(a0, a1); }
#if defined(__LP64__)
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetSystemPropertyInt(const Java * a0, std::int32_t a1) {
  vrapi_native::remember_java(a0);
  return guest::vrapi_GetSystemPropertyInt(a0, a1);
}
VENTURE_VRAPI_EXPORT float vrapi_GetSystemPropertyFloat(const Java * a0, std::int32_t a1) {
  vrapi_native::remember_java(a0);
  return guest::vrapi_GetSystemPropertyFloat(a0, a1);
}
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetSystemPropertyFloatArray(const Java * a0, std::int32_t a1, float * a2, std::int32_t a3) {
  vrapi_native::remember_java(a0);
  return guest::vrapi_GetSystemPropertyFloatArray(a0, a1, a2, a3);
}
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetSystemStatusInt(const Java * a0, std::int32_t a1) {
  vrapi_native::remember_java(a0);
  return guest::vrapi_GetSystemStatusInt(a0, a1);
}
VENTURE_VRAPI_EXPORT float vrapi_GetSystemStatusFloat(const Java * a0, std::int32_t a1) {
  vrapi_native::remember_java(a0);
  return guest::vrapi_GetSystemStatusFloat(a0, a1);
}
VENTURE_VRAPI_EXPORT bool vrapi_GetPropertyInt(const Java * a0, std::int32_t a1, std::int32_t * a2) {
  vrapi_native::remember_java(a0);
  return guest::vrapi_GetPropertyInt(a0, a1, a2);
}
VENTURE_VRAPI_EXPORT void vrapi_SetPropertyInt(const Java * a0, std::int32_t a1, std::int32_t a2) {
  vrapi_native::remember_java(a0);
  guest::vrapi_SetPropertyInt(a0, a1, a2);
}
VENTURE_VRAPI_EXPORT void vrapi_SetPropertyFloat(const Java * a0, std::int32_t a1, float a2) {
  vrapi_native::remember_java(a0);
  guest::vrapi_SetPropertyFloat(a0, a1, a2);
}
#endif
VENTURE_VRAPI_EXPORT std::int32_t vrapi_SetClockLevels(Mobile * a0, std::int32_t a1, std::int32_t a2) { return guest::vrapi_SetClockLevels(a0, a1, a2); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_SetPerfThread(Mobile * a0, std::int32_t a1, std::uint32_t a2) { return guest::vrapi_SetPerfThread(a0, a1, a2); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_SetExtraLatencyMode(Mobile * a0, std::int32_t a1) { return guest::vrapi_SetExtraLatencyMode(a0, a1); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_SetDisplayRefreshRate(Mobile * a0, float a1) { return guest::vrapi_SetDisplayRefreshRate(a0, a1); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_SetTrackingSpace(Mobile * a0, std::int32_t a1) { return guest::vrapi_SetTrackingSpace(a0, a1); }
VENTURE_VRAPI_EXPORT Pose vrapi_LocateTrackingSpace(Mobile * a0, std::int32_t a1) { return guest::vrapi_LocateTrackingSpace(a0, a1); }
VENTURE_VRAPI_EXPORT Pose vrapi_GetTrackingTransform(Mobile * a0, std::int32_t a1) { return guest::vrapi_GetTrackingTransform(a0, a1); }
VENTURE_VRAPI_EXPORT void vrapi_RecenterPose(Mobile * a0) { guest::vrapi_RecenterPose(a0); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_PollEvent(Event * a0) { return guest::vrapi_PollEvent(a0); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetBoundaryGeometry(Mobile * a0, std::uint32_t a1, std::uint32_t * a2, Vec3 * a3) { return guest::vrapi_GetBoundaryGeometry(a0, a1, a2, a3); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetBoundaryOrientedBoundingBox(Mobile * a0, Pose * a1, Vec3 * a2) { return guest::vrapi_GetBoundaryOrientedBoundingBox(a0, a1, a2); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetBoundaryTriggerState(Mobile * a0, std::int32_t a1, Boundary * a2) { return guest::vrapi_GetBoundaryTriggerState(a0, a1, a2); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_EnumerateInputDevices(Mobile * a0, std::uint32_t a1, InputHeader * a2) { return guest::vrapi_EnumerateInputDevices(a0, a1, a2); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetInputDeviceCapabilities(Mobile * a0, InputHeader * a1) { return guest::vrapi_GetInputDeviceCapabilities(a0, a1); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetCurrentInputState(Mobile * a0, std::uint32_t a1, InputStateHeader * a2) { return guest::vrapi_GetCurrentInputState(a0, a1, a2); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetInputTrackingState(Mobile * a0, std::uint32_t a1, double a2, TrackingHead * a3) { return guest::vrapi_GetInputTrackingState(a0, a1, a2, a3); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_SetRemoteEmulation(Mobile * a0, bool a1) { return guest::vrapi_SetRemoteEmulation(a0, a1); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_SetHapticVibrationSimple(Mobile * a0, std::uint32_t a1, float a2) { return guest::vrapi_SetHapticVibrationSimple(a0, a1, a2); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_SetHapticVibrationBuffer(Mobile * a0, std::uint32_t a1, const HapticBuffer * a2) { return guest::vrapi_SetHapticVibrationBuffer(a0, a1, a2); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetHandPose(Mobile * a0, std::uint32_t a1, double a2, HandPoseHeader * a3) { return guest::vrapi_GetHandPose(a0, a1, a2, a3); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetHandSkeleton(Mobile * a0, std::int32_t a1, std::uint32_t * a2) { return guest::vrapi_GetHandSkeleton(a0, a1, a2); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetHandMesh(Mobile * a0, std::int32_t a1, std::uint32_t * a2) { return guest::vrapi_GetHandMesh(a0, a1, a2); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetInstanceExtensionsVulkan(char * a0, std::uint32_t * a1) { return guest::vrapi_GetInstanceExtensionsVulkan(a0, a1); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetDeviceExtensionsVulkan(char * a0, std::uint32_t * a1) { return guest::vrapi_GetDeviceExtensionsVulkan(a0, a1); }
VENTURE_VRAPI_EXPORT ColorDesc vrapi_GetHmdColorDesc(Mobile * a0) { return guest::vrapi_GetHmdColorDesc(a0); }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_SetClientColorDesc(Mobile * a0, const ColorDesc * a1) { return guest::vrapi_SetClientColorDesc(a0, a1); }
// clang-format on

// A >16-byte aggregate argument is passed by reference (AAPCS64), which the
// compiler does for this by-value parameter as it does in the game.
VENTURE_VRAPI_EXPORT void vrapi_SetTrackingTransform(Mobile *mobile, Pose pose) {
  guest::set_tracking_transform(mobile, &pose);
}
VENTURE_VRAPI_EXPORT std::int32_t vrapi_TestPointIsInBoundary(Mobile *mobile, Vec3 point, bool *inside,
                                                              Boundary *out) {
  return guest::test_point(mobile, point, inside, out);
}
// Identifies Venture's library; available before vrapi_Initialize.
VENTURE_VRAPI_EXPORT const char *vrapi_GetVersionString() { return "Venture VrApi compatibility 1.1"; }

// Android surface swapchains and the system UI have no OpenXR counterpart:
// a game gets no surface and no UI, as on PC.
VENTURE_VRAPI_EXPORT TextureSwapChain *vrapi_CreateAndroidSurfaceSwapChain(std::int32_t, std::int32_t) {
  log::warn("vrapi", "vrapi_CreateAndroidSurfaceSwapChain is unsupported");
  return nullptr;
}
VENTURE_VRAPI_EXPORT jobject vrapi_GetTextureSwapChainAndroidSurface(TextureSwapChain *) { return nullptr; }
VENTURE_VRAPI_EXPORT bool vrapi_ShowSystemUI(const Java *, std::int32_t) {
  log::warn("vrapi", "vrapi_ShowSystemUI is unsupported");
  return false;
}
VENTURE_VRAPI_EXPORT std::int32_t vrapi_GetBoundaryVisible(Mobile *, bool *) { return unsupported; }
VENTURE_VRAPI_EXPORT std::int32_t vrapi_RequestBoundaryVisible(Mobile *, bool) { return unsupported; }
}  // namespace venture::hle::vrapi

// Some SDK headers declared the hand entry points without C linkage.
extern "C" __attribute__((visibility("default"), alias("vrapi_GetHandMesh"))) std::int32_t
_Z17vrapi_GetHandMeshP9ovrMobile14ovrHandedness_P18ovrHandMeshHeader_(void *, std::int32_t, std::uint32_t *);
extern "C" __attribute__((visibility("default"), alias("vrapi_GetHandPose"))) std::int32_t
_Z17vrapi_GetHandPoseP9ovrMobilejdP18ovrHandPoseHeader_(void *, std::uint32_t, double, void *);
extern "C" __attribute__((visibility("default"), alias("vrapi_GetHandSkeleton"))) std::int32_t
_Z21vrapi_GetHandSkeletonP9ovrMobile14ovrHandedness_P22ovrHandSkeletonHeader_(void *, std::int32_t, std::uint32_t *);
