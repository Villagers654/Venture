#pragma once
#include "common/paths.h"
#include <chrono>
#include <functional>
#include <optional>
#include <set>
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
// Steam does not quit while SteamVR runs (Windows).
bool vr_running();
// Why Steam should not be quit now for a restart, or empty. Steam closes what
// it runs when it quits: on Linux a game session it started (its reaper's
// "SteamLaunch AppId=<id>") other than SteamVR itself (250820, the always-on
// shell of a SteamVR headset such as the Steam Frame) and this Venture's own.
// On Windows, where Steam does not quit while SteamVR runs, SteamVR.
std::string restart_blocker();
// Only invoked by the explicit UI button; uses Steam's own shutdown command.
void request_shutdown(const fs::path &root);
void start_client(const fs::path &root);
struct Game {
  // `package` is what the shortcut launches (paths::safe_launch_target).
  std::string package, name, art_id;
};
struct Result {
  bool pending = false;
  size_t games = 0;
  // While pending: what Steam's shortcuts lack, for the log ("<name>: its
  // shortcut starts <old exe>"), so a restart prompt that keeps coming back
  // shows what keeps changing.
  std::string reason;
};
// Steam's own way to add a non-Steam game while it runs (Linux): a desktop
// entry handed to its steam://addnonsteamgame handler, which takes a local file
// only while /tmp/addnonsteamgamefile exists. Valve's steamos-add-to-steam
// (SteamOS's "Add to Steam") and Heroic Games Launcher add games this way.
// Steam lists the shortcut at once, under an id of its own, saves it with its
// other shortcuts and starts the entry's Exec line; the next sync with Steam
// closed makes it an ordinary Venture shortcut, keeping its id. Each game is
// tried once per LiveAdd. The entries go in the data folder's steam-shortcuts.
struct LiveAdd {
  fs::path sentinel = "/tmp/addnonsteamgamefile";
  std::function<bool(const std::string &url)> open;  // hands Steam the URL
  std::function<bool()> steam_running;               // never starts Steam to add
  std::chrono::milliseconds wait{10000}, poll{250};  // for Steam to list each game
  std::set<std::string> tried;
};
// What this system offers: Linux outside a sandbox (a Flatpak has its own
// /tmp, and Steam cannot read its files) with Steam's command at hand.
std::optional<LiveAdd> live_add(const fs::path &root);
// The desktop entry that starts `game` (Desktop Entry Specification quoting).
std::string desktop_entry(const fs::path &executable, const Game &game);
// steam://addnonsteamgame/<the entry's path, percent-encoded>.
std::string add_game_url(const fs::path &entry);
// Explicit roots and injectable process probe keep tests away from real Steam.
// While Steam runs, `live` (when given) adds the games Steam lacks at once;
// the rest waits for Steam to close.
Result sync(const fs::path &root, const fs::path &executable,
            const fs::path &art, const std::vector<Game> &games,
            const std::function<bool()> &is_running = running, LiveAdd *live = nullptr);

// A restart Venture cannot see through itself: Steam closes the programs it
// started when it quits, so a Venture started from Steam's library (SteamOS's
// game mode, where Steam always runs) would close before the sync. A helper
// outside Steam's process tree waits for Steam to close, syncs, and brings
// Steam back.
//
// Whether this Venture was started by Steam: the Steam client among its
// ancestor processes, or in a sandbox, which cannot see them, Steam's launch
// variable (SteamGameId). Linux only; Steam on Windows leaves them running.
bool started_by_steam();
// Steam's launch identity in an environment: the variables naming the app
// Steam started (SteamGameId, SteamAppId, SteamOverlayGameId,
// STEAM_COMPAT_APP_ID) and its overlay library in LD_PRELOAD. SteamVR's client
// reads SteamGameId and then waits for Steam to name the app that connects,
// so a game started by a Venture that Steam started as an app of its own
// would be taken for Venture itself: on the Steam Frame a non-VR app, which
// SteamVR shows on its theater screen. Without them the game is an app of its
// own, as when Venture was not started by Steam. A Venture that Steam started
// for one game (a shortcut's --launch) is that game, and keeps them.
struct LaunchIdentity {
  std::vector<std::string> removed;                          // variables the game does not inherit
  std::vector<std::pair<std::string, std::string>> replaced;  // LD_PRELOAD without the overlay
};
LaunchIdentity launch_identity(const std::function<const char *(const char *)> &getenv);
// The environment ("NAME=value" entries) to start Steam's own steam.sh with: this
// process's without what Steam (and its runtime) set in a program it started. A
// Venture that Steam started carries Steam's library path, overlay preload and
// runtime variables, and steam.sh given them cannot set up its runtime ("Couldn't
// set up the Steam Runtime. Are you running low on disk space?" on the Steam
// Frame). PATH and LD_LIBRARY_PATH go back to the session's own, which Steam
// keeps in SYSTEM_PATH and SYSTEM_LD_LIBRARY_PATH.
std::vector<std::string> client_environment(const std::vector<std::string> &environment);
struct Handoff {
  fs::path root, executable, art;
  std::vector<Game> games;
  // The session variables the helper needs (display, D-Bus, data folder),
  // since a helper the service manager starts has the manager's own.
  std::vector<std::pair<std::string, std::string>> environment;
};
std::string write_handoff(const Handoff &handoff);
Handoff read_handoff(std::string_view json);
// The variables of this process a Handoff carries.
std::vector<std::pair<std::string, std::string>> session_environment();
// Starts `program --steam-sync-after-exit <file>` outside Steam's process
// tree: a transient systemd user unit (systemd-run --user), or in the Flatpak
// a new sandbox through the Flatpak portal (flatpak-spawn). False when neither
// is available.
bool start_handoff(const fs::path &program, const fs::path &file);
struct HandoffResult {
  bool synced = false;   // the shortcuts were written with Steam closed
  bool started = false;  // the helper started Steam again
  std::string reason;    // why not synced
};
// The helper's work: waits up to `wait` for the Steam that was asked to quit
// to close (`open`; a service manager may start a new one at once, as a
// systemd unit with Restart=always does), syncs before Steam reads its
// shortcuts again, then starts Steam unless one runs within `grace`
// (`is_running`; SteamOS's game mode session starts it again).
HandoffResult finish_handoff(const Handoff &handoff, const std::function<bool()> &open,
                             const std::function<bool()> &is_running, const std::function<void()> &start,
                             std::chrono::milliseconds wait, std::chrono::milliseconds grace,
                             std::chrono::milliseconds poll);
// The helper's entry point (--steam-sync-after-exit <file>): exit code.
int run_handoff(const fs::path &file);
} // namespace venture::steam_library
