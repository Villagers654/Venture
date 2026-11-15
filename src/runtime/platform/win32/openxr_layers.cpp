#include "xr/openxr_layers.h"
#include "common/paths.h"
#include <string>
#include <windows.h>

namespace venture::xr {
QuietLibraryErrors::QuietLibraryErrors() {
  DWORD previous = 0;
  if (SetThreadErrorMode(GetThreadErrorMode() | SEM_FAILCRITICALERRORS, &previous)) previous_ = previous;
  else previous_ = GetThreadErrorMode();
}
QuietLibraryErrors::~QuietLibraryErrors() { SetThreadErrorMode(previous_, nullptr); }
void *open_layer_library(const std::string &path, std::string &error) {
  // The loader's LoaderPlatformLibraryOpen: LoadLibraryW, then the DLL's own
  // directory for its dependencies.
  const std::wstring wide = paths::from_utf8(path).wstring();
  HMODULE module = LoadLibraryW(wide.c_str());
  DWORD code = module ? 0 : GetLastError();
  if (!module && code == ERROR_MOD_NOT_FOUND) {
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
      module = LoadLibraryExW(wide.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
      if (!module) code = GetLastError();
    }
  }
  if (!module) {
    char text[512]{};
    const DWORD length = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                                        text, DWORD(sizeof(text)), nullptr);
    std::string message(text, length);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' '))
      message.pop_back();
    error = "Windows error " + std::to_string(code) + (message.empty() ? "" : ": " + message);
  }
  return module;
}
void *layer_library_symbol(void *library, const char *name) {
  return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(library), name));
}
void close_layer_library(void *library) { FreeLibrary(static_cast<HMODULE>(library)); }
}  // namespace venture::xr
