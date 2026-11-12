#pragma once
#include "core/guest.h"
#include "xr/api.h"
#include <span>
#include <string>
#include <string_view>

namespace venture::hle::openxr {
void register_library();
// ILP32 adapters for every generated command (a32.cpp); before thunk registration.
void register_a32_adapters();
struct CommandInfo {
  const char *name;
  const char *extension;
  bool translated;
};
struct StructureInfo {
  const char *name;
  XrStructureType type;
  std::size_t size, alignment;
  const char *extension;
  const char *extends;
};
struct MemberInfo {
  const char *structure;
  const char *name;
  std::size_t offset, size;
  const char *type;
  bool pointer;
  const char *length;
};
std::span<const CommandInfo> command_metadata();
std::span<const StructureInfo> structure_metadata();
std::span<const MemberInfo> member_metadata();

namespace guest {
#include "hle/openxr/generated/declarations.inc"
// Android and LP64 guest-only layouts from OpenXR-SDK 1.1.63
// openxr_platform.h; never pass JNI/EGL guest identities to the host loader.
struct AndroidInit {
  XrStructureType type;
  const void *next;
  GuestAddr vm, context;
};
struct AndroidInstance {
  XrStructureType type;
  const void *next;
  GuestAddr vm, activity;
};
struct GlesBinding {
  XrStructureType type;
  const void *next;
  GuestAddr display, config, context;
};
struct GlesImage {
  XrStructureType type;
  void *next;
  std::uint32_t image;
};
struct GlesRequirements {
  XrStructureType type;
  void *next;
  XrVersion minimum, maximum;
};
struct Timespec {
  std::int64_t sec, nsec;
};
static_assert(sizeof(AndroidInit) == 32 && sizeof(GlesBinding) == 40 && sizeof(GlesImage) == 24);
XrResult xrSetAndroidApplicationThreadKHR(XrSession, std::int32_t, std::uint32_t);
// jobject* in Android LP64. The output is a real host JNI local reference;
// this extension remains unavailable to A32 guests.
XrResult xrCreateSwapchainAndroidSurfaceKHR(XrSession, const XrSwapchainCreateInfo*, XrSwapchain*, void**);
XrResult xrGetOpenGLESGraphicsRequirementsKHR(XrInstance, XrSystemId, GlesRequirements *);
XrResult xrConvertTimespecTimeToTimeKHR(XrInstance, const Timespec *, XrTime *);
XrResult xrConvertTimeToTimespecTimeKHR(XrInstance, XrTime, Timespec *);
// openxr_loader_negotiation.h (LP64): a Khronos loader built into the game loads this library
// as its runtime from the system manifest and negotiates through this entry point.
struct NegotiateLoaderInfo {
  std::uint32_t struct_type, struct_version;
  std::size_t struct_size;
  std::uint32_t min_interface_version, max_interface_version;
  XrVersion min_api_version, max_api_version;
};
struct NegotiateRuntimeRequest {
  std::uint32_t struct_type, struct_version;
  std::size_t struct_size;
  std::uint32_t runtime_interface_version;
  XrVersion runtime_api_version;
  std::uint64_t get_instance_proc_addr;
};
static_assert(sizeof(NegotiateLoaderInfo) == 40 && sizeof(NegotiateRuntimeRequest) == 40);
XrResult xrNegotiateLoaderRuntimeInterface(const NegotiateLoaderInfo *, NegotiateRuntimeRequest *);
} // namespace guest
} // namespace venture::hle::openxr
