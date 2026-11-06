#pragma once
#include <cstdint>
#include <functional>

// Memory headroom while a game runs. A monitor thread samples the host's
// allocation headroom (platform::memory_headroom: Windows commit, Linux available
// memory or strict-overcommit commit) and GPU memory against its budget
// (VK_EXT_memory_budget, fed by the Vulkan layer) once a second. When headroom gets
// tight, Venture stores new textures compressed even in auto mode and its own
// caches release memory, before any allocation fails. VENTURE_MEMORY_PRESSURE=0
// turns the monitor off.
namespace venture::memory_pressure {
enum class Level : int { normal = 0, tight = 1, critical = 2 };

// Thresholds on available bytes: tight below max(2 GiB, 1/16 of the limit), critical
// below max(1 GiB, 1/32); a raised level clears only 512 MiB above its threshold, so a
// level does not flap around one value.
Level classify(std::uint64_t available, std::uint64_t limit, Level previous) noexcept;
// GPU memory: tight past 95% of the budget, critical past it; clears below 90%.
Level classify_gpu(std::uint64_t usage, std::uint64_t budget, Level previous) noexcept;

// The higher of the host and GPU levels at the last sample; normal before the first.
Level level() noexcept;
// Starts the monitor once per process (a no-op when turned off).
void start();

// A cache that can give memory back. Runs on the monitor thread when the level
// rises, and again at most every 5 s while it stays raised; returns the bytes it
// released (an estimate is fine; it goes to the log).
void on_pressure(const char* name, std::function<std::uint64_t(Level)> release);
// GPU memory in use and the driver's budget in bytes; false when unknown. One
// source; the Vulkan layer sets it once a device exists.
void set_gpu_probe(std::function<bool(std::uint64_t& usage, std::uint64_t& budget)> probe);
}  // namespace venture::memory_pressure
