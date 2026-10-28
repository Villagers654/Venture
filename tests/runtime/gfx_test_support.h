#pragma once
#include <string_view>
#include <vector>
#include <array>

#include "core/hle.h"
namespace venture::test {
const hle::FunctionInfo* graphics_function(std::string_view name);
struct GraphicsCallback {
  GuestAddr function;
  std::vector<std::uint64_t> arguments;
};
GuestAddr graphics_callback();
void configure_graphics_test_xr();
GraphicsCallback last_graphics_callback();
void dump_graphics_functions(const char* path);
std::array<unsigned, 4> graphics_xr_calls();
}  // namespace venture::test
