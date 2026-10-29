#pragma once
#include "common/meta/store.h"
#include "common/meta/accounts.h"
#include <set>
namespace venture::library {
struct Install {
  meta::Plan plan;
  fs::path directory;
  bool complete = false;
  uint64_t bytes = 0;
  std::string label;  // an imported app's own name (its APK label), when Meta has no listing
};
// When requested, authoritative is false for missing roots, unreadable entries
// or skipped manifests. Such a scan must never drive removal of shortcuts.
std::vector<Install> scan(const fs::path &root, bool* authoritative = nullptr);
// Only the current account may reconcile its own previously confirmed titles.
void merge(std::vector<meta::Game> &games, const meta::Snapshot &snapshot,
           std::string_view account);
std::vector<meta::Game> load(const fs::path &file);
void save(const fs::path &file, const std::vector<meta::Game> &games);
void uninstall(const fs::path &root, const Install &install);
bool addon_installed(const Install &install, std::string_view sku);
// Upgrade old manifests from a fresh, same-build Meta catalog. Presence comes
// from size-checked files on disk; this never asserts account ownership.
void refresh_catalog(const fs::path &root, const meta::Plan &catalog);
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
// of the runtime log, else the exit code with the last error line as context.
std::string runtime_failure(std::string_view log_tail, int exit_code);
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
  bool steam_add_new = true, steam_ask = true;
  std::set<std::string> steam_skip;
  bool runtime_override() const;
  static Settings defaults(const fs::path &data);
  static Settings load(const fs::path &data);
  void save(const fs::path &data) const;
};
} // namespace venture::library
