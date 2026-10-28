#pragma once
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/hle.h"
#include "core/thread.h"
#include <array>
namespace venture::test {
struct GuestCrash : std::runtime_error {
  using std::runtime_error::runtime_error;
};
GuestAddr callback(std::function<std::uint64_t(std::span<const std::uint64_t>)> fn);
struct LibcFixture {
  GuestThread thread;
  Cpu& cpu;
  std::span<std::uint64_t> stack;
  LibcFixture();
  std::uint64_t invoke(const char* name, std::initializer_list<std::uint64_t> args = {},
                       const char* library = "libc.so");
  std::uint64_t invoke_fatal(const char* name, std::initializer_list<std::uint64_t> args);
  GuestAddr data(const char* name, const char* library = "libc.so");
  double real() const { return std::bit_cast<double>(cpu.v(0).lo); }
  void real_arg(int index, double value) { cpu.set_v(index, {std::bit_cast<std::uint64_t>(value), 0}); }
};
}  // namespace venture::test
