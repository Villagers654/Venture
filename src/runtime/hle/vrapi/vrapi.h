#pragma once
#include "hle/vrapi/abi.h"
#include "xr/api.h"
namespace venture::hle::vrapi {
void register_libraries();
void register_a32_adapters();
namespace guest {
using namespace abi;
std::int32_t vrapi_Initialize(const Init *);
void vrapi_Shutdown();
std::int32_t vrapi_CreateSystemVulkan(const SystemVulkan *);
void vrapi_DestroySystemVulkan();
VkImage vrapi_GetTextureSwapChainBufferVulkan(TextureSwapChain *, std::int32_t);
std::int32_t vrapi_GetTextureSwapChainBufferFoveationVulkan(TextureSwapChain *, std::int32_t,
                                                         VkImage *, std::uint32_t *, std::uint32_t *);
Mobile *vrapi_EnterVrMode(const Mode *);
void vrapi_LeaveVrMode(Mobile *);
double vrapi_GetTimeInSeconds();
double vrapi_GetPredictedDisplayTime(Mobile *, std::int64_t);
Tracking vrapi_GetPredictedTracking2(Mobile *, double);
TrackingHead vrapi_GetPredictedTracking(Mobile *, double);
std::int32_t vrapi_WaitFrame(Mobile *, std::uint64_t);
std::int32_t vrapi_BeginFrame(Mobile *, std::uint64_t);
std::int32_t vrapi_SubmitFrame2(Mobile *, const Submit *);
void vrapi_SubmitFrame(Mobile *, const LegacyFrame *);
TextureSwapChain *vrapi_CreateTextureSwapChain4(const SwapChainCreate *);
TextureSwapChain *vrapi_CreateTextureSwapChain3(std::int32_t, std::int64_t, std::int32_t,
                                                std::int32_t, std::int32_t, std::int32_t);
TextureSwapChain *vrapi_CreateTextureSwapChain(std::int32_t, std::int32_t, std::int32_t,
                                               std::int32_t, std::int32_t, bool);
void vrapi_DestroyTextureSwapChain(TextureSwapChain *);
std::int32_t vrapi_GetTextureSwapChainLength(TextureSwapChain *);
std::uint32_t vrapi_GetTextureSwapChainHandle(TextureSwapChain *, std::int32_t);
void vrapi_SetTextureSwapChainHandle(TextureSwapChain *, std::int32_t, std::uint32_t);
std::int32_t vrapi_GetTextureSwapChainSamplerState(TextureSwapChain *, TextureSamplerState *);
std::int32_t vrapi_SetTextureSwapChainSamplerState(TextureSwapChain *, const TextureSamplerState *);
std::int32_t vrapi_GetSystemPropertyInt(const Java *, std::int32_t);
float vrapi_GetSystemPropertyFloat(const Java *, std::int32_t);
std::int32_t vrapi_GetSystemPropertyFloatArray(const Java *, std::int32_t, float *, std::int32_t);
std::int32_t vrapi_GetSystemStatusInt(const Java *, std::int32_t);
float vrapi_GetSystemStatusFloat(const Java *, std::int32_t);
bool vrapi_GetPropertyInt(const Java *, std::int32_t, std::int32_t *);
void vrapi_SetPropertyInt(const Java *, std::int32_t, std::int32_t);
void vrapi_SetPropertyFloat(const Java *, std::int32_t, float);
std::int32_t vrapi_SetClockLevels(Mobile *, std::int32_t, std::int32_t);
std::int32_t vrapi_SetPerfThread(Mobile *, std::int32_t, std::uint32_t);
std::int32_t vrapi_SetExtraLatencyMode(Mobile *, std::int32_t);
std::int32_t vrapi_SetDisplayRefreshRate(Mobile *, float);
std::int32_t vrapi_SetTrackingSpace(Mobile *, std::int32_t);
Pose vrapi_LocateTrackingSpace(Mobile *, std::int32_t);
Pose vrapi_GetTrackingTransform(Mobile *, std::int32_t);
void set_tracking_transform(Mobile *, const Pose *);
void vrapi_RecenterPose(Mobile *);
std::int32_t vrapi_PollEvent(Event *);
std::int32_t vrapi_GetBoundaryGeometry(Mobile *, std::uint32_t, std::uint32_t *, Vec3 *);
std::int32_t vrapi_GetBoundaryOrientedBoundingBox(Mobile *, Pose *, Vec3 *);
std::int32_t test_point(Mobile *, Vec3, bool *, Boundary *);
std::int32_t vrapi_GetBoundaryTriggerState(Mobile *, std::int32_t, Boundary *);
std::int32_t vrapi_EnumerateInputDevices(Mobile *, std::uint32_t, InputHeader *);
std::int32_t vrapi_GetInputDeviceCapabilities(Mobile *, InputHeader *);
std::int32_t vrapi_GetCurrentInputState(Mobile *, std::uint32_t, InputStateHeader *);
std::int32_t vrapi_GetInputTrackingState(Mobile *, std::uint32_t, double, TrackingHead *);
std::int32_t vrapi_SetRemoteEmulation(Mobile *, bool);
std::int32_t vrapi_SetHapticVibrationSimple(Mobile *, std::uint32_t, float);
std::int32_t vrapi_SetHapticVibrationBuffer(Mobile *, std::uint32_t, const HapticBuffer *);
std::int32_t vrapi_GetHandPose(Mobile *, std::uint32_t, double, HandPoseHeader *);
std::int32_t vrapi_GetHandSkeleton(Mobile *, std::int32_t, std::uint32_t *);
std::int32_t vrapi_GetHandMesh(Mobile *, std::int32_t, std::uint32_t *);
std::int32_t vrapi_GetInstanceExtensionsVulkan(char *, std::uint32_t *);
std::int32_t vrapi_GetDeviceExtensionsVulkan(char *, std::uint32_t *);
ColorDesc vrapi_GetHmdColorDesc(Mobile *);
std::int32_t vrapi_SetClientColorDesc(Mobile *, const ColorDesc *);
} // namespace guest
} // namespace venture::hle::vrapi
