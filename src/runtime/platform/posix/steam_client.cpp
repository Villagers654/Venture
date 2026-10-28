#include "common/environment.h"
#include "platform/steam_client.h"
#include <cstdlib>
#include <stdexcept>
#include <string>
namespace venture::platform {
bool steam_client_running() { throw std::runtime_error("Native Steam client discovery is not implemented on POSIX"); }
void prepare_steam_environment(std::uint32_t appid) {
  const auto id = std::to_string(appid);
  if (!appid || setenv("SteamAppId", id.c_str(), 1) || setenv("SteamGameId", id.c_str(), 1) ||
      setenv("DISABLE_VK_LAYER_VALVE_steam_overlay_1", "1", 1))
    throw std::runtime_error("Cannot set Steam launch environment");
  environment::refresh();
}
void load_running_steam_client() { throw std::runtime_error("Native Steam client bridge is not implemented on POSIX"); }
void* steam_client_export(const char*) { throw std::runtime_error("Native Steam client unavailable on POSIX"); }
bool steam_overlay_loaded() { return false; }
} // namespace venture::platform
