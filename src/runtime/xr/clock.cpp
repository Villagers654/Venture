#include "xr/clock.h"
#include <limits>
namespace venture::xr {
namespace {
struct Offset {
  XrResult result;
  std::int64_t ns;
};
const Offset &offset() {
  static const Offset mapping = [] {
    XrTime xr = 0;
    std::int64_t mono = 0;
    auto result = clock_sample(&xr, &mono);
    return Offset{result, xr - mono};
  }();
  return mapping;
}
bool add(std::int64_t a, std::int64_t b, std::int64_t *out) {
  if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
      (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b))
    return false;
  *out = a + b;
  return true;
}
} // namespace
XrResult monotonic_to_time(std::int64_t ns, XrTime *out) {
  if (!out)
    return XR_ERROR_VALIDATION_FAILURE;
  if (XR_FAILED(offset().result))
    return offset().result;
  return add(ns, offset().ns, out) && *out > 0 ? XR_SUCCESS : XR_ERROR_TIME_INVALID;
}
XrResult time_to_monotonic(XrTime time, std::int64_t *out) {
  if (!out)
    return XR_ERROR_VALIDATION_FAILURE;
  if (time <= 0)
    return XR_ERROR_TIME_INVALID;
  if (XR_FAILED(offset().result))
    return offset().result;
  return add(time, -offset().ns, out) ? XR_SUCCESS : XR_ERROR_TIME_INVALID;
}
} // namespace venture::xr
