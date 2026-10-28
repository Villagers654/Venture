#include <windows.h>
struct IUnknown;
#define XR_USE_PLATFORM_WIN32
#include "core/wait.h"
#include "xr/clock.h"
namespace venture::xr {
XrResult clock_sample(XrTime *time, std::int64_t *monotonic) {
  auto fn = host_proc<PFN_xrConvertWin32PerformanceCounterToTimeKHR, "xrConvertWin32PerformanceCounterToTimeKHR">();
  if (!fn)
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  auto before = venture::monotonic_ns();
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  auto after = venture::monotonic_ns();
  *monotonic = before + (after - before) / 2;
  return fn(host_xr().instance, &counter, time);
}
} // namespace venture::xr
