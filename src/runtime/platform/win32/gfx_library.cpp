#include "platform/gfx_library.h"

#include <windows.h>
namespace venture::platform {
void* open_graphics_library(const std::filesystem::path& path) {
  // Absolute ANGLE paths load adjacent dependencies without changing process
  // PATH.
  const auto flags = path.is_absolute() ? LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                              LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
                                        : LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
  return LoadLibraryExW(path.c_str(), nullptr, flags);
}
void* graphics_symbol(void* lib, const char* name) {
  return lib ? reinterpret_cast<void*>(
                   GetProcAddress(static_cast<HMODULE>(lib), name))
             : nullptr;
}
const char* egl_library_name() { return "libEGL.dll"; }
const char* gles_library_name() { return "libGLESv2.dll"; }
const char* vulkan_library_name() { return "vulkan-1.dll"; }
}  // namespace venture::platform
