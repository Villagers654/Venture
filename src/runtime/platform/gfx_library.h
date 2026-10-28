#pragma once
#include <filesystem>
namespace venture::platform {
// Keep loaded for process lifetime: driver worker threads can outlive EGL
// state.
void* open_graphics_library(const std::filesystem::path& path);
void* graphics_symbol(void* library, const char* name);
const char* egl_library_name();
const char* gles_library_name();
const char* vulkan_library_name();
}  // namespace venture::platform
