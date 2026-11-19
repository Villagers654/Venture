// SPDX-License-Identifier: GPL-3.0-or-later
#include "host_check.h"

#include <openxr/openxr.h>

#include <cstdlib>
#include <cstring>
#include <format>
#include <string_view>
#include <vector>

#include "cpu/engine_choice.h"
#include "cpu/native_a64.h"
#if defined(__linux__) && defined(__aarch64__)
#include <sys/personality.h>
#endif

namespace venture {
namespace {
const char* arch_name(HostArch arch) {
  switch (arch) {
    case HostArch::x64: return "x86-64";
    case HostArch::arm64: return "AArch64";
    case HostArch::other: break;
  }
  return "other";
}
const char* yes(bool value) { return value ? "yes" : "no"; }

// The engine process_engine() would take for a game of `isa` in this build
// (cpu/native_a64.h): choose_engine, then the build's and the user's overrides.
EngineChoice engine_for(GuestIsa isa, const HostCpuFacts& host) {
  auto chosen = choose_engine(isa, host);
  if (const char* forced = std::getenv("VENTURE_CPU_ENGINE");
      forced && std::string_view(forced) == "dynarmic" && chosen.engine == CpuEngine::native_a64)
    chosen = {CpuEngine::dynarmic_arm64, "VENTURE_CPU_ENGINE=dynarmic: translated"};
  if (!VENTURE_NATIVE_A64 && chosen.engine == CpuEngine::native_a64)
    chosen = {CpuEngine::dynarmic_arm64, "no native-a64 engine in this build: translated"};
  return chosen;
}

// Whether the CPU and kernel run 32-bit Arm code (AArch32 at EL0): the arm64
// kernel accepts the PER_LINUX32 personality only then.
const char* aarch32() {
#if defined(__linux__) && defined(__aarch64__)
  // Flatpak's seccomp filter refuses personality() except for its own values.
  const int before = personality(0xffffffff);
  if (before == -1) return "unknown (the sandbox refuses the probe; A32 games are translated either way)";
  const bool supported = personality(PER_LINUX32) != -1;
  personality(static_cast<unsigned long>(before));
  return supported ? "yes" : "no (A32 games are translated)";
#else
  return "not an Arm host";
#endif
}

std::string result_name(XrInstance instance, XrResult result) {
  char name[XR_MAX_RESULT_STRING_SIZE] = {};
  if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, result, name)))
    return std::format("{} ({})", name, int(result));
  return std::format("XrResult {}", int(result));
}

void openxr(std::string& out) {
  if (const char* json = std::getenv("XR_RUNTIME_JSON"); json && *json)
    out += std::format("OpenXR runtime manifest (XR_RUNTIME_JSON): {}\n", json);
  std::uint32_t count = 0;
  XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
  std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
  if (XR_SUCCEEDED(result) && count)
    result = xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data());
  if (XR_FAILED(result)) {
    out += std::format("OpenXR: the loader found no usable runtime (XrResult {})\n", int(result));
    return;
  }
  extensions.resize(count);
  // The newest version the runtime accepts, as games ask: 1.1, then 1.0.
  XrInstance instance = XR_NULL_HANDLE;
  int minor = -1;
  for (int m : {1, 0}) {
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(info.applicationInfo.applicationName, "Venture host check", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(info.applicationInfo.engineName, "Venture", XR_MAX_ENGINE_NAME_SIZE - 1);
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, m, 0);
    result = xrCreateInstance(&info, &instance);
    if (XR_SUCCEEDED(result)) {
      minor = m;
      break;
    }
    if (result != XR_ERROR_API_VERSION_UNSUPPORTED) break;
  }
  if (minor < 0) {
    out += std::format("OpenXR: the runtime refused an instance ({})\n", result_name(XR_NULL_HANDLE, result));
  } else {
    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &properties)))
      out += std::format("OpenXR: {} {}.{}.{}, accepts version 1.{}\n", properties.runtimeName,
                         XR_VERSION_MAJOR(properties.runtimeVersion), XR_VERSION_MINOR(properties.runtimeVersion),
                         XR_VERSION_PATCH(properties.runtimeVersion), minor);
    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    result = xrGetSystem(instance, &system_info, &system);
    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(result) && XR_SUCCEEDED(xrGetSystemProperties(instance, system, &system_properties)))
      out += std::format("Headset: {} (vendor {:#x}), up to {}x{} per view\n", system_properties.systemName,
                         system_properties.vendorId, system_properties.graphicsProperties.maxSwapchainImageWidth,
                         system_properties.graphicsProperties.maxSwapchainImageHeight);
    else
      out += std::format("Headset: none found ({})\n", result_name(instance, result));
    xrDestroyInstance(instance);
  }
  std::string names;
  for (const auto& extension : extensions)
    names += std::format("{}{} v{}", names.empty() ? "" : ", ", extension.extensionName, extension.extensionVersion);
  out += std::format("OpenXR extensions ({}): {}\n", extensions.size(), names.empty() ? "none" : names);
}
}  // namespace

std::string host_check() {
  std::string out;
  const auto host = probe_host_cpu_facts();
  out += std::format("CPU: {}, page size {}\n", arch_name(host.arch), host.page_size);
  if (host.arch == HostArch::arm64) {
    out += std::format("32-bit Arm code on this CPU: {}\n", aarch32());
    out += std::format("Guest system call trap (seccomp by instruction pointer): {}\n", yes(host.guest_syscall_trap));
  }
  for (auto [isa, name] : {std::pair{GuestIsa::a64, "A64"}, std::pair{GuestIsa::a32, "A32"}}) {
    const auto chosen = engine_for(isa, host);
    out += std::format("CPU engine for {} games: {} ({})\n", name, engine_name(chosen.engine), chosen.reason);
  }
  // ANGLE (bundled beside the runtime) on the host's Vulkan driver is the
  // build's one GLES path (docs/design/native-headsets.md, section 4.2).
  out += "GLES: ANGLE on Vulkan\n";
  openxr(out);
  return out;
}
}  // namespace venture
