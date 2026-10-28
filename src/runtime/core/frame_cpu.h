#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace venture::frame_cpu {
extern const bool enabled;
// Thread lifetime identity is held by the registry, so reused native IDs reset baselines.
struct Delta {
  std::optional<std::uint64_t> previous;
  std::optional<std::uint64_t> read(std::int64_t cycles) {
    if (cycles < 0) { previous.reset(); return {}; }
    const auto current = std::uint64_t(cycles);
    const auto old = previous; previous = current;
    if (!old || current < *old) return {};
    return current - *old;
  }
};
std::string role_name(std::string_view name);
void attach(std::int32_t guest_tid);
void detach(std::int32_t guest_tid);
void name(std::int32_t guest_tid, std::string_view name);
struct Sample {
  std::uint32_t host_tid;
  std::int32_t guest_tid;
  std::string role, selection, name;
  std::uint64_t cycles;
  double cycles_per_ns;
};
std::vector<Sample> boundary();
} // namespace venture::frame_cpu
