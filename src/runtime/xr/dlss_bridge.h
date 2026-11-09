#pragma once
// venture_ngx.dll as loaded by dlss_host.cpp (venture_host_xr), shared with the
// upscaler (dlss.cpp, venture_xr). Windows only.
#ifdef _WIN32
#include "xr/ngx_bridge_abi.h"
#include <string>

namespace venture::xr::dlss {
struct Bridge {
  PFN_ventureNgxRequiredExtensions required_extensions{};
  PFN_ventureNgxInit init{};
  PFN_ventureNgxDlssAvailable available{};
  PFN_ventureNgxDlssCreate create{};
  PFN_ventureNgxDlssEvaluate evaluate{};
  PFN_ventureNgxRelease release{};
  PFN_ventureNgxShutdown shutdown{};
};
// venture_ngx.dll beside the runtime, loaded once; null when it is missing or
// incomplete (logged).
const Bridge* bridge();
// The runtime executable's folder.
std::wstring runtime_directory();
// An NVSDK_NGX_Result that is not a failure.
inline bool succeeded(std::uint32_t result) { return (result & 0xFFF00000u) != 0xBAD00000u; }
}  // namespace venture::xr::dlss
#endif
