#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
namespace venture::platform {
// Keep loaded for process lifetime: driver worker threads can outlive EGL
// state.
void* open_graphics_library(const std::filesystem::path& path);
void* graphics_symbol(void* library, const char* name);
const char* egl_library_name();
const char* gles_library_name();
const char* vulkan_library_name();
// The system's hardware GPUs (Windows: DXGI adapters, software and memoryless
// virtual adapters left out), each with its LUID, which Vulkan reports as deviceLUID. Empty
// where the system has no such list.
struct DisplayAdapter {
  std::string name;
  std::array<std::uint8_t, 8> luid{};
};
std::vector<DisplayAdapter> display_adapters();
}  // namespace venture::platform
