#pragma once
#include "common/paths.h"
#include <functional>
#include <vector>

namespace venture::steam_library {
// Ordered, lossless binary KeyValues. Scalar payloads include their wire
// terminators; float/colour/pointer/wide-string data is never normalized.
struct Vdf {
  uint8_t type = 0;
  std::string key, value;
  std::vector<Vdf> children;
  uint8_t end = 8;
  bool operator==(const Vdf &) const = default;
};
Vdf read_vdf(std::string_view bytes);
std::string write_vdf(const Vdf &);
uint32_t appid(std::string_view quoted_exe, std::string_view name);
std::string artwork_name(uint32_t id, std::string_view kind);
fs::path discover_root();
// A copied launcher profile may discover only an explicitly overridden root.
bool profile_sync_allowed();
fs::path account_config(const fs::path &root);
bool running();
// Steam does not quit while SteamVR runs.
bool vr_running();
// Only invoked by the explicit UI button; uses Steam's own shutdown command.
void request_shutdown(const fs::path &root);
void start_client(const fs::path &root);
struct Game {
  std::string package, name, art_id;
};
struct Result {
  bool pending = false;
  size_t games = 0;
};
// Explicit roots and injectable process probe keep tests away from real Steam.
Result sync(const fs::path &root, const fs::path &executable,
            const fs::path &art, const std::vector<Game> &games,
            const std::function<bool()> &is_running = running);
} // namespace venture::steam_library
