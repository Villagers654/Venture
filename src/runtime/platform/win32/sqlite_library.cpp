#include "platform/sqlite_library.h"

#include <windows.h>
namespace venture::platform {
void* sqlite_symbol(const char* name) {
  static const HMODULE library = LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  return library ? reinterpret_cast<void*>(GetProcAddress(library, name)) : nullptr;
}
}  // namespace venture::platform
