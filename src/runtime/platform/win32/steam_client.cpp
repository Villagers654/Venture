#include "common/environment.h"
#include "platform/steam_client.h"
#include <windows.h>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace venture::platform {
namespace { HMODULE module{}; std::once_flag loaded; }
bool steam_client_running() {
  DWORD pid{}, count=sizeof(pid);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"pid",
                   RRF_RT_REG_DWORD, nullptr, &pid, &count)!=ERROR_SUCCESS || !pid) return false;
  HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);
  if (!process) return false;
  const bool active=WaitForSingleObject(process,0)==WAIT_TIMEOUT;
  CloseHandle(process);
  return active;
}
void prepare_steam_environment(std::uint32_t appid) {
  const auto id = std::to_wstring(appid);
  if (!appid || !SetEnvironmentVariableW(L"SteamAppId", id.c_str()) ||
      !SetEnvironmentVariableW(L"SteamGameId", id.c_str()) ||
      !SetEnvironmentVariableW(L"DISABLE_VK_LAYER_VALVE_steam_overlay_1", L"1"))
    throw std::runtime_error("Cannot set Steam launch environment");
  environment::refresh();
}
void load_running_steam_client() {
  std::call_once(loaded, [] {
    constexpr auto key = L"Software\\Valve\\Steam\\ActiveProcess";
    if (!steam_client_running())
      throw std::runtime_error("Steam client is not running");
    DWORD count = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"SteamClientDll64", RRF_RT_REG_SZ, nullptr, nullptr, &count) != ERROR_SUCCESS || count < 4 || count > 65536)
      throw std::runtime_error("SteamClientDll64 is unavailable");
    std::vector<wchar_t> path(count / sizeof(wchar_t) + 1);
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"SteamClientDll64", RRF_RT_REG_SZ, nullptr, path.data(), &count) != ERROR_SUCCESS ||
        !std::filesystem::path(path.data()).is_absolute())
      throw std::runtime_error("SteamClientDll64 path is invalid");
    // Constrain dependencies to Steam's own DLL directory and Windows system
    // directories. No current-directory DLL search, no global PATH mutation.
    module = LoadLibraryExW(path.data(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) throw std::runtime_error("Cannot load running client's steamclient64.dll (error " + std::to_string(GetLastError()) + ")");
    // Retain this module for process lifetime: Steam owns worker/TLS code that
    // may outlive a pipe. Sessions still release their user and pipe normally.
  });
}
void* steam_client_export(const char* name) {
  auto result = module ? GetProcAddress(module, name) : nullptr;
  if (!result) throw std::runtime_error(std::string("Steam client export unavailable: ") + name);
  return reinterpret_cast<void*>(result);
}
bool steam_overlay_loaded() { return GetModuleHandleW(L"gameoverlayrenderer64.dll") != nullptr; }
} // namespace venture::platform
