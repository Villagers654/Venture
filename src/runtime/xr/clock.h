#pragma once
#include "xr/api.h"
namespace venture::xr {
// Implemented behind platform/. Samples the host runtime's clock and the
// guest monotonic clock together; no assumed common epoch.
XrResult clock_sample(XrTime *time, std::int64_t *monotonic);
XrResult monotonic_to_time(std::int64_t, XrTime *);
XrResult time_to_monotonic(XrTime, std::int64_t *);
} // namespace venture::xr
