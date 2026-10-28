#pragma once
#include "platform/cpu_affinity.h"
#include <filesystem>
#include <span>
#include <vector>
namespace venture::platform {
// Efficiency classes for `allowed` CPUs from Linux sysfs rooted at `sys`
// (normally /sys). Exposed for tests.
std::vector<HostCpu> classify_cpus(std::span<const std::uint32_t> allowed, const std::filesystem::path& sys);
}
