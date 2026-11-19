#include "platform/gfx_library.h"

#include <windows.h>
#include <dxgi.h>
#include <cstring>
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
std::vector<DisplayAdapter> display_adapters() {
  std::vector<DisplayAdapter> out;
  const auto dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!dxgi) return out;
  using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
  const auto create = reinterpret_cast<CreateFactory>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
  IDXGIFactory1* factory = nullptr;
  if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
      DXGI_ADAPTER_DESC1 desc{};
      if (adapter && SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
          // Virtual display adapters (remote and indirect displays) own no
          // memory and are not GPUs a runtime renders on.
          (desc.DedicatedVideoMemory || desc.DedicatedSystemMemory)) {
        DisplayAdapter a;
        const int n = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
          a.name.resize(std::size_t(n - 1));
          WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, a.name.data(), n, nullptr, nullptr);
        }
        static_assert(sizeof(desc.AdapterLuid) == 8);
        std::memcpy(a.luid.data(), &desc.AdapterLuid, 8);
        out.push_back(std::move(a));
      }
      if (adapter) adapter->Release();
      adapter = nullptr;
    }
    factory->Release();
  }
  FreeLibrary(dxgi);
  return out;
}
}  // namespace venture::platform
