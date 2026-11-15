#include "xr/openxr_layers.h"
#include <dlfcn.h>

namespace venture::xr {
QuietLibraryErrors::QuietLibraryErrors() = default;  // dlopen reports through dlerror only
QuietLibraryErrors::~QuietLibraryErrors() = default;
void *open_layer_library(const std::string &path, std::string &error) {
  // The loader's LoaderPlatformLibraryOpen.
  void *library = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
  if (!library) {
    const char *text = dlerror();
    error = text ? text : "dlopen failed";
  }
  return library;
}
void *layer_library_symbol(void *library, const char *name) { return dlsym(library, name); }
void close_layer_library(void *library) { dlclose(library); }
}  // namespace venture::xr
