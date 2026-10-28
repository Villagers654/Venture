#include "platform/cpu_affinity.h"
#include "platform/posix/cpu_topology.h"
#include "core/error.h"
#include <algorithm>
#include <cerrno>
#include <fstream>
#include <optional>
#include <sched.h>
#include <set>
#include <stdexcept>
#include <string>

namespace venture::platform {
int current_cpu() {
  int cpu = sched_getcpu();
  // sched_getcpu(3) documents ENOSYS when the kernel lacks getcpu support.
  // Keep Linux guest errors here; venture_runtime must not depend on venture_hle.
  return cpu < 0 ? -(errno == ENOSYS ? err::kENOSYS : err::kEIO) : cpu;
}
namespace {
// A sysfs CPU list ("0-3,8,10-11"); empty if absent or malformed.
std::set<std::uint32_t> cpu_list(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::string text;
  std::set<std::uint32_t> out;
  if (!std::getline(input, text)) return out;
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t end = 0;
    try {
      const auto first = std::stoul(text.substr(at), &end);
      auto last = first;
      at += end;
      if (at < text.size() && text[at] == '-') {
        last = std::stoul(text.substr(++at), &end);
        at += end;
      }
      if (last < first || last - first > 1u << 20) return {};
      for (auto i = first; i <= last; ++i) out.insert(static_cast<std::uint32_t>(i));
    } catch (const std::exception&) { return {}; }
    if (at < text.size() && text[at] == ',') ++at;
    else break;
  }
  return out;
}
std::optional<unsigned long> read_number(const std::filesystem::path& path) {
  std::ifstream input(path);
  unsigned long value = 0;
  if (input >> value) return value;
  return std::nullopt;
}
}  // namespace
std::vector<HostCpu> classify_cpus(std::span<const std::uint32_t> allowed, const std::filesystem::path& sys) {
  std::vector<HostCpu> out;
  for (auto i : allowed) out.push_back({i, 0, false});
  // Hybrid x86 parts register one perf PMU per core type, listing its CPUs:
  // exactly the core-type split Windows reports as EfficiencyClass.
  const auto performance = cpu_list(sys / "devices/cpu_core/cpus");
  const auto efficiency = cpu_list(sys / "devices/cpu_atom/cpus");
  if (!performance.empty() && !efficiency.empty()) {
    for (auto& cpu : out) {
      cpu.efficiency_known = performance.contains(cpu.index) || efficiency.contains(cpu.index);
      cpu.efficiency_class = performance.contains(cpu.index) ? 1 : 0;
    }
    return out;
  }
  // Otherwise the scheduler's relative capacity. Favored cores of one type can
  // differ by a few percent; core types differ by far more, so capacities
  // within 20% of the next lower one share a class.
  std::vector<unsigned long> capacities;
  for (auto& cpu : out) {
    const auto capacity = read_number(sys / ("devices/system/cpu/cpu" + std::to_string(cpu.index)) / "cpu_capacity");
    if (!capacity || !*capacity) return out;  // unknown
    capacities.push_back(*capacity);
  }
  auto sorted = capacities;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  std::vector<std::uint8_t> classes(sorted.size(), 0);
  for (std::size_t i = 1; i < sorted.size(); ++i)
    classes[i] = static_cast<std::uint8_t>(classes[i - 1] + (sorted[i] * 5 > sorted[i - 1] * 6 ? 1 : 0));
  for (std::size_t i = 0; i < out.size(); ++i) {
    const auto rank = std::lower_bound(sorted.begin(), sorted.end(), capacities[i]) - sorted.begin();
    out[i].efficiency_class = classes[static_cast<std::size_t>(rank)];
    out[i].efficiency_known = true;
  }
  return out;
}
std::vector<HostCpu> host_cpus() {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set)) throw std::runtime_error("cannot query CPU affinity");
  std::vector<std::uint32_t> allowed;
  for (unsigned i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &set)) allowed.push_back(i);
  static const auto classified = classify_cpus(allowed, "/sys");
  // The affinity can change after the first call; keep classes for CPUs still allowed.
  std::vector<HostCpu> out;
  for (auto i : allowed) {
    const auto it = std::find_if(classified.begin(), classified.end(), [&](const HostCpu& c) { return c.index == i; });
    out.push_back(it != classified.end() ? *it : HostCpu{i, 0, false});
  }
  return out;
}
bool pin_current_thread(std::uint32_t index) {
  if (index >= CPU_SETSIZE) return false;
  cpu_set_t set;
  CPU_ZERO(&set); CPU_SET(index, &set);
  return sched_setaffinity(0, sizeof(set), &set) == 0;
}
} // namespace venture::platform
