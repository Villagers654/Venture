#pragma once
// venture_ngx.dll (tools/ngx-bridge), the MSVC-built bridge to NVIDIA NGX that Venture NR (xr/venture_nr.h) uses.
// It is loaded from the runtime's directory on first use, only when VENTURE_NR=1. Its Vulkan extension query lives
// here, beside the host XR code, so the Vulkan layer can ask for NGX's extensions without the XR module.
#include <cstdint>
#include <string>
#include <vector>
#ifdef _WIN32
#include "xr/ngx_bridge_abi.h"
#endif

namespace venture::xr::ngx_bridge {
// VENTURE_NR=1: the launcher's Venture NR mode.
bool requested();
// NGX's instance (device false) or device extensions; empty when NR is off or the bridge is unavailable.
std::vector<const char*> required_extensions(bool device);
#ifdef _WIN32
struct Bridge {
  PFN_ventureNgxRequiredExtensions required_extensions{};
  PFN_ventureNgxInit init{};
  PFN_ventureNgxRelease release{};
  PFN_ventureNgxShutdown shutdown{};
  PFN_ventureNgxNrCreate nr_create{};
  PFN_ventureNgxNrEvaluate nr_evaluate{};
};
// The loaded bridge, or null (logged once).
const Bridge* get();
std::wstring runtime_directory();
inline bool succeeded(std::uint32_t result) { return (result & 0xFFF00000u) != 0xBAD00000u; }
#endif
}  // namespace venture::xr::ngx_bridge
