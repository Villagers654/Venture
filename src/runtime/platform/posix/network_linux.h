#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>

namespace venture::platform {
// Android ConnectivityManager TYPE_* of the lowest-metric default route's
// interface from Linux procfs/sysfs rooted at `proc_net` (/proc/net) and
// `sys_class_net` (/sys/class/net); -2 without one, nullopt when the route
// tables are unreadable. Exposed for tests.
std::optional<std::int32_t> default_route_network_type(const std::filesystem::path& proc_net,
                                                       const std::filesystem::path& sys_class_net);
}  // namespace venture::platform
