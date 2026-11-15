#include "common/openxr_layers.h"
#include <iterator>
#include <windows.h>

namespace venture::openxr_layers {
std::vector<std::filesystem::path> implicit_manifests() {
  // The loader's ReadLayerDataFilesInRegistry: machine first, then the user's;
  // value names are manifests, DWORD data 0 enables one.
  std::vector<std::filesystem::path> out;
  for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
    HKEY key{};
    if (RegOpenKeyExW(root, L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS)
      continue;
    for (DWORD i = 0;; ++i) {
      wchar_t name[1024];
      DWORD length = DWORD(std::size(name)), type = 0, data = 1, size = sizeof(data);
      const auto status =
          RegEnumValueW(key, i, name, &length, nullptr, &type, reinterpret_cast<BYTE *>(&data), &size);
      if (status == ERROR_NO_MORE_ITEMS) break;
      if (status == ERROR_SUCCESS && size == sizeof(data) && data == 0) out.emplace_back(name);
    }
    RegCloseKey(key);
  }
  return out;
}
bool environment_set(const char *name) {
  // The loader's PlatformUtilsGetEnvSet: the Win32 environment, nonzero size.
  return GetEnvironmentVariableA(name, nullptr, 0) != 0;
}
}  // namespace venture::openxr_layers
