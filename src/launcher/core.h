#pragma once
// The launcher without its window: accounts, the Meta library, installs,
// add-ons, imports and game launches. The GUI (launcher/app.h) and the
// command line (launcher/cli.cpp) both drive this one implementation.
//
// Single-threaded like a UI: every method runs on the owner's thread. Network
// and disk work runs on workers whose completions apply in tick(); wake() tells
// the owner that completions are ready.
#include "common/download/manager.h"
#include "common/file_ops.h"
#include "common/launch_details.h"
#include "common/library/library.h"
#include "common/library/save_import.h"
#include "common/meta/accounts.h"
#include "common/meta/native_host.h"
#include "common/process.h"
#include "common/steam_library.h"
#include "common/android/adb.h"
#include "launcher/auth_browser.h"
#include "launcher/options.h"
#include "launcher/worker.h"
#include <atomic>
#include <deque>
#include <future>
#include <map>
#include <set>
namespace venture::import {
class Source;
}
namespace venture::launcher {
// The message shown for the exception being handled.
std::string exception_text();
struct AccountView {
  std::string key, alias, display_name;
  bool active = false, primary = false, signed_in = false, needs_reauth = false;
};
class Core {
public:
  explicit Core(Options options);
  virtual ~Core();
  Core(const Core &) = delete;
  Core &operator=(const Core &) = delete;

  // Starts the workers, the first scan and (unless offline, a direct launch
  // or `refresh_library` is false) the library refresh. Call once the derived
  // object is fully constructed: completions call wake().
  void start(bool refresh_library = true);
  // Stops launches, downloads and workers; idempotent.
  void shutdown();
  // Applies finished background work and polls games and downloads.
  virtual void tick();

  std::vector<meta::Addon> addons(std::string_view app) const;
  void refresh_addons(std::string app);
  void install_addon(std::string_view app, std::string_view sku);
  void uninstall_addon(std::string_view app, std::string_view sku);
  void install_all_owned_addons(std::string_view app);
  const std::vector<AccountView> &accounts() const { return accounts_; }
  std::vector<std::string> game_owners(std::string_view app_id) const;
  library::LaunchChoice launch_choice(std::string_view app_id,
                                      std::string_view account = {}) const;
  void add_account();
  void remove_account(std::string_view key, bool delete_saves = false);
  void sign_out_account(std::string_view key);
  void switch_account(std::string_view key);
  void mark_primary(std::string_view key);
  void play_as(std::string_view app_id, std::string_view key);
  void play_as(std::string_view app_id, std::string_view key, const LaunchDetails&);
  // The owning account a Play/Install on this game uses: the user's pick, else
  // the active account if it owns the game, else the first usable owner.
  std::string owner_for(std::string_view app_id) const;
  // The account Play asks for: the one picked with Play as, else none, so
  // launch_choice takes a usable owner, the active account, or no account.
  std::string play_key(std::string_view app_id) const;
  const AccountView *account(std::string_view key) const;
  Json save_migration_status() const;
  // Writes a redacted debug report about this game's last launch (or the
  // most recent launch when empty) and returns the file.
  fs::path save_debug_report(std::string_view app_id, std::string_view version) const;
  // Uploads a saved report in the background; `done` runs on the UI thread
  // with its link, or with why the upload failed.
  void share_debug_report(fs::path file, std::function<void(std::string url, std::string error)> done);

protected:
  // Completions are ready for tick(); called from worker threads.
  virtual void wake() {}
  // A library refresh started (the GUI retries failed artwork).
  virtual void on_refresh() {}
  // An install or import was queued (the GUI shows Downloads).
  virtual void on_install_queued() {}
  // A local import scan started (the GUI shows Import).
  virtual void on_import_scan() {}

  Options options_;
  fs::path data_;
  SecretStore secrets_;
  meta::AccountVault vault_;
  meta::AccountIndex account_index_;
  std::vector<AccountView> accounts_;
  std::string account_key_;
  void reload_accounts();
  // Reads each signed-in account's alias and display name once (Meta's
  // library document often carries neither).
  void backfill_identity();
  std::set<std::string> identity_pending_, identity_tried_;
  // Refreshes the libraries of the signed-in accounts other than the active
  // one, so every owned game has its store metadata and owner.
  void refresh_accounts();
  std::set<std::string> library_pending_;
  // Public store listings for visible games still missing their name,
  // description or publisher (anonymous; no account token is sent).
  void fill_metadata();
  std::set<std::string> metadata_tried_;
  bool metadata_busy_ = false;
  library::Settings settings_;
  std::unique_ptr<Worker> worker_;
  std::unique_ptr<download::Manager> downloads_;
  AuthBrowser browser_;
  std::shared_ptr<meta::Auth> auth_;
  std::optional<Session> session_;
  std::string account_scope_;
  uint64_t epoch_ = 0;
  std::vector<meta::Game> games_;
  std::vector<library::Install> installs_;
  std::map<std::string, meta::Details> details_;
  std::map<std::string, std::string> detail_errors_;
  std::set<std::string> details_pending_;
  std::map<std::string, std::vector<meta::Addon>> addons_;
  std::map<std::string, std::string> addon_errors_;
  std::set<std::string> addons_pending_;
  std::map<std::string, std::string> owner_pick_; // app id -> account key
  struct Running {
    Process process;
    fs::path log,     // the runtime's own log (--log)
        console;      // its stdout and stderr
    std::string tail;
    std::optional<int> exit;
    std::string failure; // shown for a non-zero exit, or why a game closed itself
    bool quitting = false; // Quit game was pressed; the game is saving and closing
  };
  std::map<std::string, Running> running_;
  std::set<std::string> launch_pending_;
  // Cross-process game lock also covers a Steam launch beside an open UI.
  std::map<std::string, std::unique_ptr<FileLock>> game_locks_;
  bool steam_pending_ = false, steam_busy_ = false, steam_restart_ = false;
  bool steam_sync_allowed_ = false, steam_scan_reliable_ = false;
  size_t steam_games_ = 0;
  uint64_t steam_poll_ = 0;
  std::atomic<uint64_t> steam_generation_{0};
  void sync_steam();
  void restart_steam();
  // A new game the GUI asks about before it installs: where it goes and
  // whether it goes to Steam (app id, name).
  std::optional<std::pair<std::string, std::string>> install_ask_;
  // Adds a game to Steam or leaves it out (saved), then syncs.
  void set_in_steam(std::string_view app_id, bool in);
  bool in_steam(std::string_view app_id) const { return !settings_.steam_skip.contains(std::string(app_id)); }
  struct LaunchRequest {
    std::string target, account;
    LaunchDetails details;
    std::promise<bool> result;
    std::mutex mutex;
    bool started = false, done = false;
    bool begin();
    void finish(bool);
    void cancel();
  };
  struct LaunchQueue {
    std::mutex mutex;
    bool closed = false;
    std::deque<std::shared_ptr<LaunchRequest>> pending;
    std::vector<std::weak_ptr<LaunchRequest>> outstanding;
  };
  std::shared_ptr<LaunchQueue> launch_queue_ = std::make_shared<LaunchQueue>();
  static Json request_launch(const std::shared_ptr<LaunchQueue>&, const std::string&, const Json&);
  void launch_game(std::string_view, std::string_view, const LaunchDetails&,
                   std::shared_ptr<LaunchRequest> = {});
  std::string selected_, notice_, warning_, auth_status_;
  bool library_partial_ = false;  // Meta did not confirm the whole library
  bool refreshing_ = false, scanning_ = false, signing_in_ = false,
       verifying_ = false;
  bool auth_page_loaded_ = false; // the Meta sign-in page finished loading
  std::string refresh_error_;     // last library refresh
  std::set<std::string> saves_kept_notified_;  // imports told their saves were kept
  std::string owned_link_;        // store link of a game to add when Meta cannot list the library
  bool adding_ = false;
  size_t last_completed_ = 0;
  std::uint64_t last_discarded_ = 0;
  uint64_t last_poll_ = 0;
  // Import (launcher/import.cpp): apps found on a headset or at a picked
  // path, each matched to the Meta game it is when known.
  struct ImportItem {
    std::string spec, package, version_name, location, label;
    std::int64_t version_code = 0;
    uint64_t bytes = 0, save_bytes = 0;
    size_t files = 0, saves = 0, skipped = 0;
    bool selected = false;
  };
  struct ImportScan {
    bool busy = false;
    std::string error;
    std::vector<ImportItem> items;
  };
  struct ImportMatch {
    const meta::Game *game = nullptr;
    std::string status;
    bool importable = false;
  };
  ImportScan import_device_, import_local_;
  std::vector<android::adb::Device> import_devices_;
  std::string import_serial_;          // the headset shown
  std::string import_address_;         // typed for Wi-Fi debugging
  std::optional<fs::path> import_adb_; // adb found for the last scan
  std::unique_ptr<Worker> import_worker_;
  mutable std::mutex adb_mutex_;       // read by the download thread
  fs::path adb_setting_;
  static ImportItem import_item(const import::Source &source);
  void scan_headset();
  void connect_headset();
  void scan_local(fs::path picked);
  // New games go to `root` (default: the games folder); one already installed
  // stays in its library folder.
  void start_import(bool device, const fs::path &root = {});
  ImportMatch import_match(const ImportItem &item) const;
  fs::path adb_path() const;
  void refresh();
  // Adds the game at owned_link_ once Meta confirms this account owns it.
  void add_owned();
  void rescan();
  void select(std::string id);
  void sign_in();
  // Like sign_in(), but pre-loads the Meta sign-in window with the browser
  // session the sideloaded extension (tools/meta-session-extension) last
  // delivered, so Meta recognizes the account without the user typing
  // credentials again. A notice explains when none is on file.
  void sign_in_via_browser_session();
  void sign_out();
  void play(const meta::Game &game);
  // Installs or updates a game. An installed game updates where it is; a new
  // one goes to `root` (a library folder, added if new), else the games folder.
  void install(const meta::Game &game, const fs::path &root = {});
  // The Install button: downloads belong to the active account, so a game
  // that only another signed-in account owns switches to that account first.
  void install_for_owner(const meta::Game &game, const fs::path &root = {});
  // The library folder an installed game is in.
  static fs::path root_of(const library::Install &install) {
    return install.directory.parent_path().parent_path();
  }
  // Removes an installed game's files; saves stay.
  void uninstall(const library::Install &install);
  // Imports an installed game's save data for the account it plays as: from
  // `folder` (saves::find_local), or with none from a headset over adb (the
  // first ready one unless `serial`). Checks now that the game is installed
  // and closed and holds its lock; the returned task copies (on any thread).
  using SaveImport = std::function<saves::Imported(std::stop_token)>;
  SaveImport import_saves(std::string_view app_id, const fs::path &folder, std::string serial = {});
  // The same on the import thread; a notice reports the result.
  void import_saves_later(std::string_view app_id, const fs::path &folder);
  // Saves settings and rescans the library folders they name.
  void apply_settings(library::Settings next, bool announce = true);
  void save_library();
  // Shared by sign_in() (cookies empty) and sign_in_via_browser_session().
  void sign_in_with_cookies(std::vector<meta::Cookie> cookies);
  void action(const std::function<void()> &fn);
  void error(std::string message);
  const library::Install *installed(std::string_view id,
                                    bool complete = true) const;
  // An installation of an Android package, whatever app id it has.
  const library::Install *installed_package(std::string_view package) const {
    for (const auto &i : installs_)
      if (i.plan.package == package) return &i;
    return nullptr;
  }
  meta::Game *game(std::string_view id);
  std::vector<meta::Game> visible_games() const;
  // Meta lists a newer build than the installed one (needs the game's details).
  bool update_available(std::string_view id) const;

  // Whether this process keeps the persistent download queue (see start()).
  bool owns_download_queue() const { return queue_lock_ != nullptr; }

private:
  bool started_ = false, stopped_ = false;
  std::unique_ptr<FileLock> queue_lock_;
};
} // namespace venture::launcher
