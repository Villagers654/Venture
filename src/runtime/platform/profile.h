#pragma once
#include <cstdint>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "cpu/cpu.h"

namespace venture::platform {
struct ProfileThread {
  std::uint32_t id = 0;
  std::string name;
  std::shared_ptr<void> handle;
  std::uint64_t creation_ticks = 0;
};
struct ProfileSample {
  // Diagnostic only. Spilled architectural state is not a reconstruction of
  // registers cached in emitted code. Probe memory is a best-effort OS copy.
  bool inspect_state = false;
  unsigned spilled_register_count = 0;
  std::array<std::uint64_t, 31> spilled_registers{};
  struct MemoryProbe {
    std::uint64_t address = 0;
    std::array<std::uint64_t, 4> words{};
    bool readable = false;
  };
  std::array<MemoryProbe, 10> register_memory{}; // A64 x19..x28
  // A64 frame records from the spilled x29: return addresses, innermost first.
  // host_rbp holds x30 while the register cache is live in translated code.
  std::array<std::uint64_t, 16> frame_returns{};
  unsigned frame_count = 0;
  std::uint64_t host_rbp = 0;
  // Words between the first frame records (spilled return addresses of
  // functions that keep no frame record, such as engine call wrappers).
  std::array<std::uint64_t, 128> stack_words{};
  unsigned stack_word_count = 0;
  std::uintptr_t ip = 0;
  GuestAddr guest_pc = 0;
  bool translated = false;
  bool outlined_atomics = false;
  std::array<std::uint64_t, 16> outlined_counts{};
  std::uint64_t cpu_ns = 0;
  std::int64_t cpu_cycles = -1;
  std::uint32_t activity = 0;
};
struct ProfileSymbol { std::uintptr_t begin = 0, end = 0; std::string name; };
struct ProfileModule {
  std::uintptr_t begin = 0, end = 0;
  std::string name, path;
  bool system = false;
  bool vulkan_driver = false;
  std::vector<ProfileSymbol> exports;
};
bool profile_supported();
// The signal sampling interrupts threads with, or 0 when sampling needs none
// (Windows suspends threads) or is unavailable.
int profile_sample_signal() noexcept;
std::uint32_t profile_thread_id() noexcept;
struct ProfilePowerThrottling {
  bool available = false;
  std::uint32_t control_mask = 0, state_mask = 0, error = 0;
};
ProfilePowerThrottling profile_power_throttling() noexcept;
ProfileThread profile_current_thread();
bool profile_cpu_time(const ProfileThread&, std::uint64_t& nanoseconds) noexcept;
std::int64_t profile_cpu_cycles(const ProfileThread&) noexcept;
std::vector<ProfileThread> profile_threads();
// No allocation, logging, or mutex acquisition while the target is suspended.
// Cpu lifetime is pinned by the caller's per-thread lifetime guard.
bool profile_sample(const ProfileThread&, const Cpu*, const std::atomic<std::uint32_t>*, ProfileSample&) noexcept;
std::vector<ProfileModule> profile_modules();
std::vector<ProfileSymbol> profile_wait_symbols();
std::uintptr_t profile_image_base();
std::filesystem::path profile_executable();
void profile_delay(std::int64_t nanoseconds);
} // namespace venture::platform
