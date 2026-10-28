#pragma once
#include <cstdint>
#include <vector>
namespace venture::platform {
struct HostCpu {
  std::uint32_t index;
  std::uint8_t efficiency_class; // higher is higher performance (Windows EfficiencyClass)
  // False when the OS does not say which CPUs differ in performance: the class
  // is then 0 for every CPU but means "unknown", not "homogeneous".
  bool efficiency_known = true;
};
std::vector<HostCpu> host_cpus();
bool pin_current_thread(std::uint32_t index);
int current_cpu(); // host logical CPU index, or negative Linux errno
} // namespace venture::platform
