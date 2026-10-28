#include <time.h>
#define XR_USE_TIMESPEC
#include "core/wait.h"
#include "xr/clock.h"
namespace venture::xr {
XrResult clock_sample(XrTime *time, std::int64_t *monotonic) {
  auto fn = host_proc<PFN_xrConvertTimespecTimeToTimeKHR, "xrConvertTimespecTimeToTimeKHR">();
  if (!fn)
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  auto before = venture::monotonic_ns();
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return XR_ERROR_RUNTIME_FAILURE;
  auto after = venture::monotonic_ns();
  *monotonic = before + (after - before) / 2;
  return fn(host_xr().instance, &now, time);
}
} // namespace venture::xr
