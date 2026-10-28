#pragma once
#include "xr/api.h"
#include "xr/host_xr.h"
#include <cstring>

// Host modes deliberately start below the active PC display mode.
struct RefreshHost {
  void* previous = venture::xr::host_xr().get_instance_proc_addr;
  inline static PFN_xrGetInstanceProcAddr fallback{};
  inline static unsigned requests = 0, enumerations = 0;
  inline static XrResult query_result = XR_SUCCESS;
  static XrResult XRAPI_CALL current(XrSession, float* rate) {
    if (XR_SUCCEEDED(query_result)) *rate = 144;
    return query_result;
  }
  static XrResult XRAPI_CALL request(XrSession, float) { ++requests; return XR_SUCCESS; }
  static XrResult XRAPI_CALL enumerate(XrSession, std::uint32_t capacity, std::uint32_t* count, float* rates) {
    ++enumerations;
    *count = 3;
    if (capacity >= 3) { rates[0] = 72; rates[1] = 90; rates[2] = 144; }
    return capacity && capacity < 3 ? XR_ERROR_SIZE_INSUFFICIENT : XR_SUCCESS;
  }
  static XrResult XRAPI_CALL get(XrInstance instance, const char* name, PFN_xrVoidFunction* out) {
    if (!std::strcmp(name, "xrGetDisplayRefreshRateFB")) *out = reinterpret_cast<PFN_xrVoidFunction>(current);
    else if (!std::strcmp(name, "xrRequestDisplayRefreshRateFB")) *out = reinterpret_cast<PFN_xrVoidFunction>(request);
    else if (!std::strcmp(name, "xrEnumerateDisplayRefreshRatesFB")) *out = reinterpret_cast<PFN_xrVoidFunction>(enumerate);
    else return fallback(instance, name, out);
    return XR_SUCCESS;
  }
  RefreshHost() {
    fallback = reinterpret_cast<PFN_xrGetInstanceProcAddr>(previous);
    requests = enumerations = 0;
    query_result = XR_SUCCESS;
    venture::xr::host_xr().enabled_extensions.insert("XR_FB_display_refresh_rate");
    venture::xr::host_xr().get_instance_proc_addr = reinterpret_cast<void*>(get);
  }
  ~RefreshHost() {
    venture::xr::host_xr().get_instance_proc_addr = previous;
    venture::xr::host_xr().enabled_extensions.erase("XR_FB_display_refresh_rate");
  }
};
