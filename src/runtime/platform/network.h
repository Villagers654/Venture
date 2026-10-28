#pragma once
#include <cstdint>

namespace venture::platform {
// Android ConnectivityManager TYPE_* value; -1 means an unknown connected
// transport, -2 means no active network. This does not assert Internet access.
std::int32_t active_network_type(); // throws a fixed, secret-free error on query failure
// Host Internet validation (Windows NCSI, Linux NetworkManager connectivity);
// false when the host has no such service.
bool has_internet_connectivity(); // throws on host query failure
} // namespace venture::platform
