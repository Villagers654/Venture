#pragma once
#include <cstdint>
#include <string_view>

namespace venture::platform {
bool steam_client_running();
// Reads only ActiveProcess/SteamClientDll64. Does not operate or modify Steam.
void prepare_steam_environment(std::uint32_t appid);
void load_running_steam_client();
void* steam_client_export(const char* name);
bool steam_overlay_loaded();
} // namespace venture::platform
