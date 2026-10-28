#pragma once
#include "core/guest.h"
#include "xr/api.h"
#include <span>
#include <string>
#include <string_view>

namespace venture::hle::openxr {
void register_library();
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
} // namespace guest
} // namespace venture::hle::openxr
