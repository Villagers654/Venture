#pragma once
#include "common/controls.h"
#include "common/meta/store.h"
#include "common/meta/accounts.h"
#include "common/library/display.h"
#include <set>
namespace venture::library {
struct Install {
  meta::Plan plan;
  fs::path directory;
  bool complete = false;
  uint64_t bytes = 0;
  std::string label;  // an imported app's own name (its APK label), when Meta has no listing
  // Its library id: the app id, or for a version pinned beside the game's own
  // install meta::version_id. Launches, Steam and settings use it.
  std::string id() const {
    return plan.pinned ? meta::version_id(plan.app_id, plan.binary_id) : plan.app_id;
  }
};
// When requested, authoritative is false for a root on a drive that is not
// connected, unreadable entries or manifests that exist but cannot be read.
// Such a scan must never drive removal of shortcuts. A root that is gone from
// a present parent and a version folder without plan.json hold no games.
std::vector<Install> scan(const fs::path &root, bool* authoritative = nullptr);
// Only the current account may reconcile its own previously confirmed titles.
void merge(std::vector<meta::Game> &games, const meta::Snapshot &snapshot,
           std::string_view account);
std::vector<meta::Game> load(const fs::path &file);
void save(const fs::path &file, const std::vector<meta::Game> &games);
void uninstall(const fs::path &root, const Install &install);
// What uninstalling `install` removes, newest first: a pinned version is only
// itself; the game's own install comes with every older version of it an
// update left in a library folder (unpinned installs with the same id).
std::vector<Install> uninstall_set(const std::vector<Install> &installs, const Install &install);
// The older versions updates left behind: unpinned installs of a game below
// its newest unpinned install's version code, once that one is complete. An
// update installs beside the version it replaces; on a Quest it replaces it.
std::vector<Install> superseded(const std::vector<Install> &installs);
bool addon_installed(const Install &install, std::string_view sku);
// Upgrade old manifests from a fresh, same-build Meta catalog. Presence comes
// from size-checked files on disk; this never asserts account ownership.
void refresh_catalog(const fs::path &root, const meta::Plan &catalog);
// A same-build Meta plan names the store's APK, but an import may have
// installed another APK of that build (a modified one). While the APK in
// `directory` still has the size the installed manifest records, `plan` keeps
// that size and hash, so a catalog refresh or a resumed download neither marks
// the install incomplete nor replaces the APK.
void keep_installed_apk(meta::Plan &plan, const meta::Plan &installed,
                        const fs::path &directory);
void uninstall_addon(const fs::path &root, const Install &install,
                     std::string_view sku);
std::vector<std::string> owning_accounts(const std::vector<meta::Game> &games,
                                         const meta::AccountIndex &accounts,
                                         std::string_view app_id);
enum class LaunchStatus { ready, reconnect };
struct LaunchChoice {
  LaunchStatus status = LaunchStatus::ready;
  std::string account_key;  // empty: no account; platform calls are unauthenticated
};
// The account a game launches with. Ownership is never required: Venture runs
// any installed game, and the game's own platform calls get what Meta's
// servers answer for this account. An explicitly requested account must be
// signed in; otherwise a signed-in owning account, else the active account,
// else none.
LaunchChoice launch_choice(const meta::AccountIndex &accounts,
                           const std::vector<std::string> &owners,
                           std::string_view requested = {});
// What the launcher shows for a runtime that exited with `exit_code`: the
// "[runtime]" error line main() writes for a failed launch, found in the tail
// of the runtime log; that the game stopped responding and was closed when a
// frame-stall snapshot with no frame after it ends the log and no fatal signal
// follows; else the exit code with the last error line as context.
std::string runtime_failure(std::string_view log_tail, int exit_code);
// Whether a run's log shows the game asking Meta for its signed-in user or
// entitlement and getting "not signed in" (no account, or an expired one).
// Games such as Beat Saber then quit on their own, often with exit code 0.
// A failed platform initialization counts when the game then quit before
// rendering more than a few frames, or crashed right after a later platform
// request failed.
bool needed_meta_sign_in(const fs::path &log);
// The expansion files (.obb) an install's plan requires that are absent from
// its directory. Files only probed for (an optional patch OBB) are not listed.
std::vector<std::string> missing_expansions(const Install &install);
// The expansion file (.obb) a run looked for and its install lacks, when the
// game then never rendered past its first frames; empty otherwise.
std::string missing_expansion(const fs::path &log);
// A run whose game asked for Meta device attestation (an integrity token only
// a real headset can produce), was refused, and closed within a minute.
bool needed_device_attestation(const fs::path &log);
// The text ("<title>: <message>") of a dialog the game showed, which Venture
// cannot present and answers itself, when the game then closed within a minute
// without rendering more than a few frames after it (Unity's "Your device does
// not match the hardware requirements" warning); empty otherwise.
std::string game_dialog(const fs::path &log);
// Before a launch: moves the previous run's log to <stem>.prev.log (replacing
// an older one), so the runtime, which appends, starts a log of this run only.
void rotate_log(const fs::path &log);
struct Settings {
  // `runtime` is the bundled runtime beside the launcher unless the user set
  // another one; only such an override is saved.
  fs::path games, runtime;
  fs::path adb;  // Android platform-tools adb for imports; empty finds it
  int log_level = 2;
  bool console = false;
  // The launcher asks GitHub for a newer release at start; "Not now" skips that version.
  bool check_updates = true;
  std::string dismissed_update;
  bool add_games_to_steam = true;
  // A newly installed game goes to Steam unless the user opts out: the
  // default, and whether installing asks (a checkbox in the GUI). Games left
  // out are listed by app id.
  bool steam_add_new = true;
  std::set<std::string> steam_skip;
  // Starting a new game's install asks where it goes and whether it goes to
  // Steam (a dialog in the GUI); off installs to `games` with the defaults.
  bool install_ask = true;
  // Imported games no signed-in account owns are linked to their store listing
  // by package name; this also asks OculusDB (Settings > Library, on by
  // default; only the package name is sent).
  bool lookup_imports_online = true;
  // Other folders that hold games: earlier games folders and ones picked when
  // installing. All are scanned; new downloads go to `games` unless one is picked.
  std::vector<fs::path> libraries;
  // Last, as defaults() initializes positionally. Launcher interface size in percent of the display's scale (Settings > Appearance).
  int interface_size = 100;
  // Decoded textures stored as BC7 (Settings > Runtime): 0 always, 1 once
  // texture_compression_threshold percent of GPU memory is in use, 2 off.
  int texture_compression = 0;
  int texture_compression_threshold = 50;
  // The warning before turning compression down from Always was answered with
  // "Change anyway": it is not shown again, globally or for any game.
  bool texture_compression_warned = false;
  // Translated game code and pipelines kept between launches (Settings > Runtime).
  bool keep_translated_code = true;
  // Games that download at once (Settings > Library); one by default.
  int parallel_downloads = 1;
  // The language games see as the system language, a BCP-47 tag such as
  // "en-US" (Settings > Runtime); empty follows the host's language.
  std::string game_language;
  // The launcher's own language (Settings > Appearance): a translation's code
  // such as "fr" or "pt_BR", "en", or empty to follow the system.
  std::string interface_language;
  // Mixed reality (passthrough) where the host runtime or headset can show the
  // real world; on by default, an opt-out. Applies from the next launch.
  bool passthrough = true;
  // Implicit Vulkan layers (overlays, capture, post-processing) kept in game
  // processes, by layer name or loader glob ("VK_LAYER_reshade*"); the runtime
  // leaves the other third-party hooks out of its Vulkan instance (driver and
  // system layers stay, xr/vulkan_layers.h).
  std::vector<std::string> vulkan_layers;
  // Controls (the Controls page): the player's choices per controller, and the
  // games (by app id) that have their own. Settings > Runtime's former "Menu from
  // both stick clicks" (menuButton) is migrated into them once.
  controls::Config controls;
  std::map<std::string, controls::Config> game_controls;
  // The choices a game launches with: its own, else the player's.
  const controls::Config& controls_for(std::string_view app_id) const;
  // Whether a texture compression change, from one effective mode to another
  // (texture_compression_modes ids), warns first: from Always to Automatic or
  // Off, until the warning has been acknowledged.
  bool texture_compression_warns(std::string_view before, std::string_view after) const;
  // Passthrough by color key (Settings > Runtime, opt-in): the game's
  // see-through regions are drawn in key_color (RRGGBB, sRGB) for a tool that
  // keys that color (Virtual Desktop, ALVR...). Never chosen automatically.
  bool color_key = false;
  std::string key_color = "FF00FF";
  // Resolution and frame rate for every game (Settings > Display) and a game's
  // own, by app id (its Settings tab); a game's empty choices follow the global ones.
  DisplaySettings display;
  std::map<std::string, DisplaySettings> game_display;
  bool runtime_override() const;
  // `games` first, then the other library folders.
  std::vector<fs::path> roots() const;
  static Settings defaults(const fs::path &data);
  static Settings load(const fs::path &data);
  void save(const fs::path &data) const;
};
// The environment a launch of `app_id` takes from the settings, the game's own
// choices over the global ones: VENTURE_LOG_LEVEL, passthrough, controls,
// locale, kept Vulkan layers, texture compression, translated code and display.
// The launcher passes it to the runtime and the debug report lists it, so the
// report shows what the launch used. `automation` (smoke runs) opens no mirror.
std::vector<std::pair<std::string, std::string>> launch_environment(const Settings &s, std::string_view app_id,
                                                                    bool automation = false);
} // namespace venture::library
