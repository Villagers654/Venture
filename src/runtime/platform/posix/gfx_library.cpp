#include "platform/gfx_library.h"

#include <dlfcn.h>
#include <system_error>
namespace venture::platform {
void* open_graphics_library(const std::filesystem::path& path) {
  // Windows' DLL search starts in the application directory, then the system.
  // dlopen of a bare name would pick the system's (glvnd) libEGL over the
  // staged ANGLE, so a library beside the executable is loaded first; the
  // Vulkan loader, which Venture does not ship, comes from the system.
  if (!path.has_parent_path()) {
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error && std::filesystem::exists(executable.parent_path() / path, error))
      return dlopen((executable.parent_path() / path).c_str(), RTLD_NOW | RTLD_LOCAL);
  }
  return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}
void* graphics_symbol(void* lib, const char* name) {
  return lib ? dlsym(lib, name) : nullptr;
}
const char* egl_library_name() { return "libEGL.so"; }
const char* gles_library_name() { return "libGLESv2.so"; }
const char* vulkan_library_name() { return "libvulkan.so.1"; }
}  // namespace venture::platform
