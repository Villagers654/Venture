#include "platform/sqlite_library.h"

#include <dlfcn.h>
#include <filesystem>
#include <system_error>
namespace venture::platform {
void* sqlite_symbol(const char* name) {
  // The release ships libsqlite3.so.0 next to the runtime; prefer it so the
  // behavior does not depend on the distribution, then fall back to the system.
  static void* const library = [] {
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    void* bundled = error ? nullptr
        : dlopen((executable.parent_path() / "libsqlite3.so.0").c_str(), RTLD_NOW | RTLD_LOCAL);
    return bundled ? bundled : dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
  }();
  return library ? dlsym(library, name) : nullptr;
}
}  // namespace venture::platform
