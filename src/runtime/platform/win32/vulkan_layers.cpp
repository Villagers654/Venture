#include "xr/vulkan_layers.h"
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>
#include <windows.h>
namespace venture::xr {
bool set_process_environment(const char* name, const char* value) {
  // The loader reads the Win32 environment; CRT getenv users keep their own copy.
  return SetEnvironmentVariableA(name, value) && _putenv_s(name, value) == 0;
}
std::vector<std::filesystem::path> implicit_vulkan_layer_manifests() {
  // The loader's registry list: value names are manifests, nonzero data disables one.
  std::vector<std::filesystem::path> out;
  for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
    HKEY key{};
    if (RegOpenKeyExW(root, L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers", 0, KEY_READ, &key) != ERROR_SUCCESS)
      continue;
    for (DWORD i = 0;; ++i) {
      wchar_t name[2048];
      DWORD length = DWORD(std::size(name)), type = 0, data = 1, size = sizeof(data);
      const auto status = RegEnumValueW(key, i, name, &length, nullptr, &type,
                                        reinterpret_cast<BYTE*>(&data), &size);
      if (status == ERROR_NO_MORE_ITEMS) break;
      if (status == ERROR_SUCCESS && type == REG_DWORD && data == 0) out.emplace_back(name);
    }
    RegCloseKey(key);
  }
  return out;
}
std::string vulkan_loader_description() {
  // The loader comes with the GPU driver or a game installer; a crash offset in
  // it means something only with its version.
  HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
  wchar_t path[MAX_PATH]{};
  if (!loader || !GetModuleFileNameW(loader, path, MAX_PATH)) return "vulkan-1.dll not loaded";
  const auto u8 = std::filesystem::path(path).u8string();
  std::string text(u8.begin(), u8.end());
  DWORD ignored = 0;
  if (const auto size = GetFileVersionInfoSizeW(path, &ignored)) {
    std::vector<std::byte> data(size);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (GetFileVersionInfoW(path, 0, size, data.data()) &&
        VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) && info)
      text += " " + std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) +
              "." + std::to_string(HIWORD(info->dwFileVersionLS)) + "." + std::to_string(LOWORD(info->dwFileVersionLS));
  }
  return text;
}
bool vulkan_layer_library_loaded(const ImplicitVulkanLayer& layer) {
  // A library path relative to its manifest; a bare name is found by the search order.
  std::filesystem::path library = std::u8string(layer.library.begin(), layer.library.end());
  if (library.has_parent_path() && library.is_relative()) library = layer.manifest.parent_path() / library;
  return GetModuleHandleW(library.lexically_normal().c_str()) != nullptr;
}
}  // namespace venture::xr
