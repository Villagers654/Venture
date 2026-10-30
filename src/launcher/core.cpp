#include "launcher/core.h"
#include "launcher/debug_report.h"
#include "common/android/manifest.h"
#include "common/download/asset_files.h"
#include "common/file_ops.h"
#include "common/http/client.h"
#include "common/library/import.h"
#include "common/log.h"
#include "common/saves_migration.h"
#include "hle/ovrplatform/broker.h"
#include <chrono>

namespace venture::launcher {
namespace {
uint64_t now_ms() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
} // namespace
std::string exception_text() {
  try {
    throw;
  } catch (const Json::exception &) {
    return "Unexpected response data. Refresh and try again.";
  } catch (const fs::filesystem_error &) {
    return "Could not access local files. Check the selected folder and "
           "permissions.";
  } catch (const std::exception &e) {
    return e.what();
  } catch (...) {
    return "An unexpected operation failed. Try again.";
  }
}
Core::Core(Options options)
    : options_(std::move(options)), data_(paths::data_dir()), secrets_(data_),
      vault_(data_), settings_(library::Settings::defaults(data_)) {
  fs::create_directories(data_ / "logs");
  log::set_file(paths::utf8(data_ / "logs" / (options_.launch_requested ?
      "launcher-direct-" + std::to_string(current_process_id()) + ".log" :
      options_.cli ? "cli-" + std::to_string(current_process_id()) + ".log" : "launcher.log")));
  if (options_.cli)
    log::info("launcher", "Venture command line starting.");
  else
    log::info("launcher", "Venture native launcher starting.");
  steam_sync_allowed_ = options_.interactive() && steam_library::profile_sync_allowed();
  if (!options_.interactive())
    log::info("steam", "Steam sync disabled for non-interactive launcher mode.");
  else if (!steam_sync_allowed_)
    log::info("steam", "Steam sync disabled: redirected data profile has no Steam root override.");
  action([&] { settings_ = library::Settings::load(data_); });
  log::set_level(static_cast<log::Level>(settings_.log_level));
  action([&] {
    auto migration = vault_.migrate_v1(
        options_.offline ? std::function<Session(Session)>{}
                         : std::function<Session(Session)>{[](Session s) {
                             auto snapshot =
                                 meta::Store(s.access_token).identity();
                             s.account_id = snapshot.account_id;
                             s.alias = snapshot.alias;
                             s.display_name = snapshot.display_name;
                             return s;
                           }});
    account_index_ = vault_.reconcile();
    reload_accounts();
    account_key_ = account_index_.active;
    if (!account_key_.empty())
      session_ = vault_.load(account_key_);
    if (migration == meta::Migration::needs_identity) {
      session_ = secrets_.load();
      warning_ = "Legacy account mode: connect to Meta to finish account "
                 "migration. Your original credentials are retained.";
    } else {
      auto saves = migrate_saves(data_, account_index_.primary,
                                 !account_index_.primary.empty());
      if (saves.conflicts)
        warning_ = "Save migration found conflicts; both copies were retained. "
                   "Review the account migration status.";
      if (saves.deferred)
        warning_ =
            "Some saves are busy; migration will retry on the next start.";
      if (migration == meta::Migration::migrated)
        notice_ = "Account and saves migrated. Older Venture builds will "
                  "show signed out.";
    }
    if (session_)
      account_scope_ = session_->account_id;
    // The old persistent WebView cookie store is never reused by InPrivate
    // sign-in.
    remove_tree(data_, data_ / "auth-webview");
  });
  action([&] {
    games_ = library::load(data_ / "library.json");
    save_library();
  });
  adb_setting_ = settings_.adb;
}
Core::~Core() { shutdown(); }
void Core::start(bool refresh_library) {
  if (started_)
    return;
  started_ = true;
  worker_ = std::make_unique<Worker>([this] { wake(); });
  import_worker_ = std::make_unique<Worker>([this] { wake(); });
  download::Manager::Services services;
  // Imports reopen their source with the adb set in Settings (or found).
  services.open_source = [this](std::string_view spec, std::string_view package,
                                std::stop_token stop) {
    return import::reopen(spec, package, adb_path(), stop);
  };
  // One process at a time keeps the persistent download queue: a second
  // launcher or the command line beside an open launcher would download the
  // same jobs twice. A direct launch never keeps it.
  bool persistent = !options_.launch_requested;
  if (persistent)
    try {
      queue_lock_ = std::make_unique<FileLock>(data_ / "locks" / "download-queue.lock", false);
    } catch (...) {
      if (options_.cli)
        persistent = false;
      else
        log::warn("launcher", "Another Venture process holds the download queue.");
    }
  downloads_ = std::make_unique<download::Manager>(data_, [this] { wake(); },
      std::move(services), persistent);
  downloads_->set_account(account_key_);
  downloads_->set_parallel(unsigned(settings_.parallel_downloads));
  rescan();
  if (refresh_library && session_ && !options_.offline && !options_.launch_requested)
    refresh();
}
bool Core::update_available(std::string_view id) const {
  const auto *local = installed(id);
  auto detail = details_.find(std::string(id));
  if (!local || detail == details_.end())
    return false;
  auto live = number(Json(detail->second.version_code)), current = number(Json(local->plan.version_code));
  return live && current ? live > current
                         : (!detail->second.live_id.empty() && detail->second.live_id != local->plan.binary_id);
}
void Core::shutdown() {
  if (stopped_)
    return;
  stopped_ = true;
  {
    std::lock_guard lock(launch_queue_->mutex);
    launch_queue_->closed = true;
    for (auto& weak : launch_queue_->outstanding)
      if (auto request = weak.lock()) request->cancel();
    launch_queue_->pending.clear();
  }
  ++epoch_;
  browser_.close();
  running_.clear();  // Join IPC handlers before destroying the download manager.
  downloads_.reset();
  queue_lock_.reset();
  if (import_worker_)
    import_worker_->stop();
  if (worker_)
    worker_->stop();
  worker_.reset();
  import_worker_.reset();
}
void Core::uninstall(const library::Install &install) {
  std::unique_ptr<FileLock> lock;
  try {
    lock = std::make_unique<FileLock>(data_ / "locks" / ("game-" + install.plan.package + ".lock"), false);
  } catch (...) {
    throw std::runtime_error("Close this game before uninstalling it.");
  }
  library::uninstall(root_of(install), install);
  rescan();
}
void Core::apply_settings(library::Settings next, bool announce) {
  // Games already installed stay where they are: a games folder that holds
  // some stays a library folder.
  if (next.games != settings_.games &&
      std::ranges::find(next.libraries, settings_.games) == next.libraries.end() &&
      std::ranges::any_of(installs_, [&](auto &i) { return root_of(i) == settings_.games; }))
    next.libraries.push_back(settings_.games);
  std::erase(next.libraries, next.games);
  next.save(data_);
  if (next.roots() != settings_.roots()) {
    addons_.clear();
    addon_errors_.clear();
  }
  settings_ = std::move(next);
  if (downloads_) downloads_->set_parallel(unsigned(settings_.parallel_downloads));
  {
    std::lock_guard lock(adb_mutex_);
    adb_setting_ = settings_.adb;
  }
  log::set_level(static_cast<log::Level>(settings_.log_level));
  if (announce)
    notice_ = "Settings saved.";
  rescan();
}
void Core::action(const std::function<void()> &fn) {
  try {
    fn();
  } catch (...) {
    error(exception_text());
  }
}
void Core::error(std::string message) {
  notice_ = std::move(message);
  log::warn("launcher", "{}", notice_);
}
void Core::save_library() {
  if (options_.launch_requested) return; // The visible UI owns the shared catalog.
  library::save(data_ / "library.json", games_);
  if (worker_ && !scanning_ && !options_.launch_requested) sync_steam();
}
void Core::reload_accounts() {
  account_index_ = vault_.index();
  accounts_.clear();
  for (const auto &a : account_index_.accounts) {
    AccountView view{a.key,
                     {},
                     {},
                     a.key == account_index_.active,
                     a.key == account_index_.primary,
                     a.signed_in,
                     a.needs_reauth};
    if (a.signed_in && !a.needs_reauth) {
      try {
        if (auto s = vault_.load(a.key)) {
          view.alias = s->alias;
          view.display_name = s->display_name;
        } else
          view.needs_reauth = true;
      } catch (...) {
        view.needs_reauth = true;
      }
    }
    accounts_.push_back(std::move(view));
  }
}
// Meta's library document usually carries neither the alias nor the display
// name, so accounts would all read "Meta account". Read both once per account
// from the app-scoped identity of an installed game it owns (the same
// authenticate_application + /me the runtime does at launch) and keep them in
// the vault.
void Core::backfill_identity() {
  if (options_.offline || options_.launch_requested)
    return;
  for (const auto &a : accounts_) {
    if (!a.signed_in || a.needs_reauth || !a.display_name.empty() ||
        identity_pending_.contains(a.key) || identity_tried_.contains(a.key))
      continue;
    // A title with a pending Meta Data Use Checkup withholds the identity, so
    // try a few owned installs.
    std::vector<ovrplatform::AppIdentity> candidates;
    for (const auto &i : installs_) {
      auto owners = game_owners(i.plan.app_id);
      if (i.complete && std::ranges::find(owners, a.key) != owners.end())
        candidates.push_back({i.plan.app_id, i.plan.package, i.plan.binary_id,
                              static_cast<std::int64_t>(number(Json(i.plan.version_code)))});
      if (candidates.size() == 6)
        break;
    }
    if (candidates.empty())
      continue; // retried once the account's library or installs arrive
    auto key = a.key;
    identity_pending_.insert(key);
    worker_->submit([this, candidates, key](std::stop_token stop) -> Worker::Completion {
      ovrplatform::User user;
      for (const auto &identity : candidates) {
        if (stop.stop_requested())
          break;
        try {
          auto provider = ovrplatform::owned_library_provider(
              identity, ovrplatform::runtime_secret_store(key),
              ovrplatform::meta_http_transport());
          auto result = ovrplatform::Result{provider->prepare()};
          if (!result.error)
            result = provider->logged_in_user(stop);
          if (!result.error)
            user = result.user;
          else
            log::warn("accounts", "Could not read the account identity (code {}, HTTP {})",
                      static_cast<int>(result.error.code), result.error.http_status);
        } catch (...) {
          log::warn("accounts", "Could not read the account identity: {}", exception_text());
        }
        if (!user.alias.empty() || !user.display_name.empty())
          break;
      }
      return [this, key, user] {
        identity_pending_.erase(key);
        identity_tried_.insert(key);
        if (user.alias.empty() && user.display_name.empty())
          return;
        action([&] {
          vault_.update_profile(key, user.alias, user.display_name);
          if (key == account_key_ && session_) {
            if (session_->alias.empty())
              session_->alias = user.alias;
            if (session_->display_name.empty())
              session_->display_name = user.display_name;
          }
          reload_accounts();
        });
      };
    });
  }
}
void Core::refresh_accounts() {
  if (options_.offline || options_.launch_requested)
    return;
  for (const auto &a : accounts_) {
    if (a.key == account_key_ || !a.signed_in || a.needs_reauth ||
        library_pending_.contains(a.key))
      continue;
    std::optional<Session> stored;
    try {
      stored = vault_.load(a.key);
    } catch (...) {
      continue;
    }
    if (!stored || stored->account_id.empty())
      continue;
    auto key = a.key, token = stored->access_token, account = stored->account_id;
    library_pending_.insert(key);
    worker_->submit([this, key, token, account](std::stop_token stop) -> Worker::Completion {
      try {
        auto snapshot = meta::Store(token, stop).library();
        return [this, key, account, snapshot = std::move(snapshot)] {
          library_pending_.erase(key);
          if (!snapshot.account_id.empty() && snapshot.account_id != account) {
            log::warn("library", "Skipped a library refresh that returned a different account.");
            return;
          }
          action([&] {
            library::merge(games_, snapshot, account);
            vault_.update_profile(key, snapshot.alias, snapshot.display_name);
            reload_accounts();
            save_library();
            log::info("library", "Refreshed {} Quest titles for another account; complete={}",
                      snapshot.games.size(), snapshot.complete);
            backfill_identity();
            fill_metadata();
          });
        };
      } catch (...) {
        auto e = exception_text();
        return [this, key, e] {
          library_pending_.erase(key);
          log::warn("library", "Could not refresh another account's library: {}", e);
          fill_metadata();
        };
      }
    });
  }
}
// Installed games only: the library grid shows their publisher, and a game
// page fetches its own details when opened. Runs after the account refreshes,
// which bring most of this, so it fills only what they left out.
void Core::fill_metadata() {
  if (options_.offline || options_.launch_requested || metadata_busy_ ||
      refreshing_ || !library_pending_.empty())
    return;
  // An owner's token reads the listing from Meta's graph like a game page
  // does; games no signed-in account owns fall back to the public store page.
  std::vector<std::pair<std::string, std::string>> wanted; // app id, token
  std::map<std::string, std::string> tokens;               // account key -> token
  for (const auto &i : installs_) {
    const auto &id = i.plan.app_id;
    const auto *g = game(id);
    if ((g && !g->description.empty() && !g->publisher.empty()) ||
        metadata_tried_.contains(id))
      continue;
    metadata_tried_.insert(id);
    std::string token;
    // Meta has no listing for an import it does not know (keyed by package).
    if (!digits(id, 5))
      continue;
    for (const auto &key : game_owners(id)) {
      if (!tokens.contains(key))
        try {
          auto stored = vault_.load(key);
          tokens[key] = stored ? stored->access_token : std::string();
        } catch (...) {
          tokens[key].clear();
        }
      if (!tokens[key].empty()) {
        token = tokens[key];
        break;
      }
    }
    wanted.emplace_back(id, std::move(token));
  }
  if (wanted.empty())
    return;
  metadata_busy_ = true;
  worker_->submit([this, wanted](std::stop_token stop) -> Worker::Completion {
    std::vector<meta::Game> found;
    for (const auto &[id, token] : wanted) {
      if (stop.stop_requested())
        break;
      try {
        found.push_back(token.empty() ? meta::Store({}, stop).artwork(id)
                                      : meta::Store(token, stop).details(id).game);
      } catch (...) {
        log::warn("library", "No public store listing for application {}: {}", id,
                  exception_text());
      }
    }
    return [this, found] {
      metadata_busy_ = false;
      action([&] {
        for (const auto &listing : found) {
          bool known = false;
          for (auto &g : games_)
            if (g.id == listing.id) {
              known = true;
              if (g.description.empty())
                g.description = listing.description;
              if (g.publisher.empty())
                g.publisher = listing.publisher;
            }
          // Installed games no signed-in account owns still get their store
          // name; the entry asserts no ownership.
          if (!known && !listing.name.empty()) {
            auto g = listing;
            g.owned = false;
            g.ownership_account.clear();
            g.image.clear();
            games_.push_back(std::move(g));
          }
        }
        if (!found.empty())
          save_library();
      });
      fill_metadata(); // games that appeared meanwhile
    };
  });
}
std::vector<std::string> Core::game_owners(std::string_view app_id) const {
  return library::owning_accounts(games_, account_index_, app_id);
}
library::LaunchChoice Core::launch_choice(std::string_view app_id,
                                         std::string_view account) const {
  return library::launch_choice(account_index_, game_owners(app_id), account);
}
void Core::add_account() {
  if (std::ranges::count(account_index_.accounts, true,
                         &meta::Account::signed_in) >= 4)
    throw std::runtime_error(
        "Sign out an account before adding another (maximum four).");
  sign_in();
}
void Core::switch_account(std::string_view key) {
  auto loaded = vault_.load(key);
  if (!loaded)
    throw std::runtime_error("Reconnect this account before switching.");
  vault_.set_active(key);
  ++epoch_;
  browser_.close();
  auth_.reset();
  signing_in_ = verifying_ = refreshing_ = false;
  details_.clear();
  detail_errors_.clear();
  details_pending_.clear();
  session_ = std::move(loaded);
  account_key_ = key;
  account_scope_ = session_->account_id;
  reload_accounts();
  downloads_->set_account(account_key_);
  warning_.clear();
  library_partial_ = false;
  auth_status_.clear();
  if (!options_.offline)
    refresh();
}
void Core::sign_out_account(std::string_view key) {
  if (!meta::valid_account_key(key))
    throw std::runtime_error("Invalid account key.");
  const bool active = account_key_ == key;
  downloads_->cancel_account(key);
  vault_.sign_out(key);
  if (active) {
    ++epoch_;
    browser_.close();
    auth_.reset();
    session_.reset();
    account_key_.clear();
    account_scope_.clear();
    signing_in_ = verifying_ = refreshing_ = false;
    details_.clear();
    details_pending_.clear();
    detail_errors_.clear();
    warning_.clear();
    library_partial_ = false;
    auth_status_.clear();
    downloads_->set_account({});
  }
  reload_accounts();
  notice_ =
      "Account signed out. Its installed games and saves remain on this PC.";
}
void Core::remove_account(std::string_view key, bool delete_saves) {
  std::string selected(key);
  if (!meta::valid_account_key(selected))
    throw std::runtime_error("Invalid account key.");
  for (const auto &[id, running] : running_)
    if (running.process.active())
      throw std::runtime_error(
          "Close running games before removing an account.");
  sign_out_account(selected);
  vault_.remove(selected);
  if (delete_saves)
    remove_tree(data_ / "data/users", data_ / "data/users" / selected);
  reload_accounts();
}
void Core::mark_primary(std::string_view key) {
  vault_.set_primary(key);
  reload_accounts();
}
Json Core::save_migration_status() const {
  auto file = data_ / "data/users/migration.json";
  return fs::exists(file) ? paths::read_json(file) : Json::object();
}
const library::Install *Core::installed(std::string_view id,
                                       bool complete) const {
  for (auto &i : installs_)
    if (i.plan.app_id == id && (!complete || i.complete))
      return &i;
  return nullptr;
}
meta::Game *Core::game(std::string_view id) {
  for (auto &g : games_)
    if (g.id == id && g.ownership_account == account_scope_)
      return &g;
  for (auto &g : games_)
    if (g.id == id)
      return &g;
  return nullptr;
}
std::vector<meta::Game> Core::visible_games() const {
  std::vector<meta::Game> out;
  std::set<std::string> seen;
  for (auto &g : games_)
    if (g.owned && (!g.system || installed(g.id, false)) && !game_owners(g.id).empty() &&
        seen.insert(g.id).second) {
      out.push_back(g);
    }
  for (auto &i : installs_)
    if (seen.insert(i.plan.app_id).second) {
      auto old = std::ranges::find_if(
          games_, [&](auto &g) { return g.id == i.plan.app_id; });
      if (old != games_.end()) {
        out.push_back(*old);
        out.back().owned = false;
      } else {
        // The APK's own label, else until a store listing arrives:
        // "com.studio.my_game" -> "My game".
        meta::Game g;
        g.id = i.plan.app_id;
        if (!i.label.empty()) {
          g.name = i.label;
          out.push_back(g);
          continue;
        }
        auto last = i.plan.package.substr(i.plan.package.rfind('.') + 1);
        for (auto &ch : last)
          if (ch == '_' || ch == '-')
            ch = ' ';
        if (!last.empty())
          last[0] = char(std::toupper(static_cast<unsigned char>(last[0])));
        g.name = last.empty() ? i.plan.package : last;
        out.push_back(g);
      }
    }
  return out;
}
void Core::rescan() {
  if (scanning_)
    return;
  scanning_ = true;
  steam_scan_reliable_ = false;
  ++steam_generation_;
  auto roots = settings_.roots();
  auto art = data_ / "cache/art";
  worker_->submit([this, roots, art](std::stop_token) -> Worker::Completion {
    try {
      // The games folder first, so a game found in two folders is the one there.
      bool reliable = true;
      std::vector<library::Install> result;
      for (const auto &root : roots) {
        // Another library folder may be on a drive that is not connected now;
        // its games are missing from this scan, which then is not reliable.
        bool whole = false;
        auto found = library::scan(root, steam_sync_allowed_ || root != roots.front() ? &whole : nullptr);
        reliable = reliable && whole;
        std::ranges::move(found, std::back_inserter(result));
      }
      // An import without a Meta listing shows what a headset's home shows:
      // the APK's label, its banner (or icon) as the cover and its icon.
      for (auto &install : result) {
        if (!install.plan.local || !install.complete) continue;
        auto apk = std::ranges::find(install.plan.files, std::string("apk"), &meta::File::kind);
        if (apk == install.plan.files.end()) continue;
        try {
          auto shown = android::read_presentation(install.directory / paths::from_utf8(apk->name));
          install.label = std::move(shown.label);
          auto save = [&](std::string_view suffix, const std::vector<std::uint8_t> &image) {
            auto file = art / paths::from_utf8(install.plan.app_id + std::string(suffix) + ".image");
            if (!image.empty() && paths::safe_package(install.plan.app_id) && !fs::exists(file))
              paths::atomic_write(file, std::string_view(reinterpret_cast<const char *>(image.data()), image.size()));
          };
          save("", shown.banner.empty() ? shown.icon : shown.banner);
          save("_icon", shown.icon);
        } catch (const std::exception &e) {
          log::warn("library", "Could not read how {} presents itself: {}", install.plan.package, e.what());
        }
      }
      return [this, result = std::move(result), roots, reliable] {
        scanning_ = false;
        if (roots == settings_.roots()) {
          std::erase_if(addons_, [&](auto &entry) {
            auto previous = installed(entry.first, false);
            auto next = std::ranges::find_if(result, [&](auto &install) {
              return install.plan.app_id == entry.first;
            });
            return (previous != nullptr) != (next != result.end()) ||
                   (previous && next != result.end() &&
                    previous->plan.binary_id != next->plan.binary_id);
          });
          installs_ = result;
          steam_scan_reliable_ = reliable;
          if (!options_.launch_requested) sync_steam();
          log::info("library", "Found {} complete installations.",
                    std::ranges::count_if(installs_,
                                          [](auto &i) { return i.complete; }));
          if (details_.contains(selected_) && !addons_.contains(selected_))
            refresh_addons(selected_);
          backfill_identity();
          fill_metadata();
        } else
          rescan();
      };
    } catch (...) {
      auto e = exception_text();
      return [this, e] {
        scanning_ = false;
        error(e);
      };
    }
  });
}
void Core::add_owned() {
  if (adding_ || !session_ || options_.offline)
    return;
  // meta.com/experiences/<id>, oculus.com/experiences/quest/<id>, or the id:
  // the last run of digits long enough to be an app id.
  std::string id;
  for (std::size_t i = 0; i < owned_link_.size();) {
    auto end = i;
    while (end < owned_link_.size() && owned_link_[end] >= '0' && owned_link_[end] <= '9')
      ++end;
    if (end - i >= 5)
      id = owned_link_.substr(i, end - i);
    i = std::max(end, i + 1);
  }
  if (id.empty()) {
    error("Paste the game's Meta store link, like meta.com/experiences/1234567890123456.");
    return;
  }
  adding_ = true;
  auto token = session_->access_token;
  auto epoch = epoch_;
  worker_->submit([this, token, epoch, id](std::stop_token stop) -> Worker::Completion {
    try {
      auto snapshot = meta::Store(token, stop).owned_listing(id);
      return [this, snapshot = std::move(snapshot), epoch] {
        if (epoch != epoch_)
          return;
        adding_ = false;
        action([&] {
          if (snapshot.games.empty())
            throw std::runtime_error("Meta says this account does not own that game.");
          if (session_ && !session_->account_id.empty() && session_->account_id != snapshot.account_id)
            throw std::runtime_error("Meta returned a different account. Sign out and reconnect.");
          library::merge(games_, snapshot, account_scope_);
          save_library();
          owned_link_.clear();
          log::info("library", "Added {} from its store listing", snapshot.games.front().id);
          notice_ = snapshot.games.front().name + " is in your library.";
          fill_metadata();
        });
      };
    } catch (...) {
      auto e = exception_text();
      return [this, epoch, e] {
        if (epoch == epoch_) {
          adding_ = false;
          error(e);
        }
      };
    }
  });
}
void Core::refresh() {
  if (options_.offline) {
    notice_ = "Offline mode. Restart without --offline to connect to Meta.";
    return;
  }
  if (refreshing_ || !session_)
    return;
  refreshing_ = true;
  refresh_error_.clear();
  notice_.clear();
  auto token = session_->access_token;
  auto epoch = epoch_;
  on_refresh();
  addons_.clear();
  worker_->submit([this, token,
                   epoch](std::stop_token stop) -> Worker::Completion {
    try {
      auto snapshot = meta::Store(token, stop).library();
      return [this, snapshot = std::move(snapshot), epoch] {
        if (epoch != epoch_)
          return;
        refreshing_ = false;
        action([&] {
          if (!snapshot.account_id.empty()) {
            if (session_ && !session_->account_id.empty() &&
                session_->account_id != snapshot.account_id)
              throw std::runtime_error(
                  "Meta returned a different account. Sign out and reconnect.");
            account_scope_ = snapshot.account_id;
            if (session_)
              session_->account_id = snapshot.account_id;
          }
          library::merge(games_, snapshot, account_scope_);
          if (session_ && !snapshot.alias.empty())
            session_->alias = snapshot.alias;
          if (session_ && !snapshot.display_name.empty())
            session_->display_name = snapshot.display_name;
          if (session_) {
            if (account_key_.empty()) {
              vault_.migrate_v1([&](Session) { return *session_; });
              account_index_ = vault_.index();
              account_key_ = account_index_.active;
              migrate_saves(data_, account_index_.primary, true);
            } else {
              // Updating the alias must not change the launch selection
              // unexpectedly.
              vault_.upsert(*session_);
            }
            reload_accounts();
            downloads_->set_account(account_key_);
          }
          warning_ = snapshot.warning;
          library_partial_ = !snapshot.complete;
          save_library();
          log::info("library", "Refreshed {} Quest titles; complete={}",
                    snapshot.games.size(), snapshot.complete);
          refresh_accounts();
          backfill_identity();
          fill_metadata();
        });
      };
    } catch (...) {
      auto e = exception_text();
      return [this, epoch, e] {
        if (epoch == epoch_) {
          refreshing_ = false;
          refresh_error_ = e;
          error(e);
          fill_metadata();
        }
      };
    }
  });
}
void Core::select(std::string id) {
  selected_ = std::move(id);
  if (details_.contains(selected_) && !addons_.contains(selected_))
    refresh_addons(selected_);
  if (!session_ || options_.offline || details_.contains(selected_) ||
      !details_pending_.insert(selected_).second)
    return;
  auto app = selected_, token = session_->access_token;
  auto epoch = epoch_;
  detail_errors_.erase(app);
  worker_->submit(
      [this, app, token, epoch](std::stop_token stop) -> Worker::Completion {
        try {
          auto d = meta::Store(token, stop).details(app);
          return [this, app, epoch, d = std::move(d)] {
            if (epoch != epoch_)
              return;
            details_pending_.erase(app);
            details_[app] = d;
            refresh_addons(app);
            if (auto g = game(app)) {
              if (!d.game.description.empty())
                g->description = d.game.description;
              if (!d.game.publisher.empty())
                g->publisher = d.game.publisher;
              if (!d.game.image.empty())
                g->image = d.game.image;
              action([&] { save_library(); });
            }
          };
        } catch (...) {
          auto e = exception_text();
          return [this, app, epoch, e] {
            if (epoch == epoch_) {
              details_pending_.erase(app);
              detail_errors_[app] = e;
            }
          };
        }
      });
}
void Core::sign_in() {
  if (options_.offline) {
    notice_ = "Sign-in needs a connection. Restart without --offline.";
    return;
  }
  if (signing_in_)
    return;
  signing_in_ = true;
  verifying_ = false;
  auth_status_ = "Opening secure Meta sign-in...";
  notice_.clear();
  auto epoch = ++epoch_;
  refreshing_ = false;
  details_pending_.clear();
  details_.clear();
  addons_.clear();
  addons_pending_.clear();
  addon_errors_.clear();
  worker_->submit([this, epoch](std::stop_token stop) -> Worker::Completion {
    try {
      auto auth = std::make_shared<meta::Auth>();
      auto url = auth->begin(stop);
      return [this, epoch, auth, url] {
        if (epoch != epoch_)
          return;
        auth_ = auth;
        action([&] { browser_.open(url, data_); });
        auth_status_ = "Complete sign-in in the Meta window.";
      };
    } catch (...) {
      auto e = exception_text();
      return [this, epoch, e] {
        if (epoch == epoch_) {
          signing_in_ = false;
          auth_status_.clear();
          error(e);
        }
      };
    }
  });
}
void Core::sign_out() {
  if (!account_key_.empty()) {
    sign_out_account(account_key_);
    return;
  }
  ++epoch_;
  browser_.close();
  auth_.reset();
  downloads_->cancel_all();
  session_.reset();
  account_scope_.clear();
  signing_in_ = verifying_ = refreshing_ = false;
  details_.clear();
  details_pending_.clear();
  detail_errors_.clear();
  addons_.clear();
  addons_pending_.clear();
  addon_errors_.clear();
  warning_.clear();
  library_partial_ = false;
  auth_status_.clear();
  secrets_.clear();
  notice_ = "Signed out. Installed games remain on this PC.";
}
void Core::install(const meta::Game &g, const fs::path &root) {
  if (options_.offline)
    throw std::runtime_error("Downloads are unavailable in offline mode.");
  if (!session_)
    throw std::runtime_error("Sign in to install or update this game.");
  const auto *current = installed(g.id, false);
  auto target = current ? root_of(*current) : root.empty() ? settings_.games : root.lexically_normal();
  if (!target.is_absolute())
    throw std::runtime_error("Choose an absolute folder to install to.");
  if (!std::ranges::contains(settings_.roots(), target)) {
    fs::create_directories(target);
    auto next = settings_;
    next.libraries.push_back(target);
    apply_settings(std::move(next), false);
  }
  downloads_->enqueue(g.id, g.name, target, session_->access_token, account_key_);
  // A new game takes the default for new games, unless the GUI asked.
  if (!current && settings_.add_games_to_steam && !options_.launch_requested)
    set_in_steam(g.id, settings_.steam_add_new);
  on_install_queued();
}
void Core::set_in_steam(std::string_view app_id, bool in) {
  const std::string id(app_id);
  if (in == !settings_.steam_skip.contains(id)) return;
  if (in) settings_.steam_skip.erase(id);
  else settings_.steam_skip.insert(id);
  settings_.save(data_);
  sync_steam();
}
void Core::install_for_owner(const meta::Game &g, const fs::path &root) {
  const auto game_copy = g;
  const auto owner = owner_for(game_copy.id);
  if (!owner.empty() && owner != account_key_) {
    auto other = account(owner);
    if (!other || !other->signed_in || other->needs_reauth)
      throw std::runtime_error("Reconnect the owning account before installing.");
    switch_account(owner);
  }
  install(game_copy, root);
}
std::vector<meta::Addon> Core::addons(std::string_view app) const {
  auto found = addons_.find(std::string(app));
  auto result =
      found == addons_.end() ? std::vector<meta::Addon>{} : found->second;
  auto install = installed(app, false);
  if (result.empty() && install)
    result = meta::group_addons(install->plan);
  if (result.empty())
    if (auto detail = details_.find(std::string(app)); detail != details_.end())
      result = meta::group_addons(detail->second.plan);
  auto jobs = downloads_->snapshot();
  for (auto &addon : result) {
    if (addon.name.empty())
      addon.name = addon.sku;
    try {
      addon.installed =
          install && library::addon_installed(*install, addon.sku);
    } catch (const fs::filesystem_error &) {
      addon.error = "Could not verify installed add-on files. Rescan to retry.";
    }
    if (auto error = addon_errors_.find(std::string(app));
        error != addon_errors_.end())
      addon.error = error->second;
    for (auto &job : jobs) {
      if (job.app_id != app || job.sku != addon.sku || !install ||
          job.binary_id != install->plan.binary_id)
        continue;
      addon.downloading = job.state == download::State::queued ||
                          job.state == download::State::checking ||
                          job.state == download::State::downloading;
      addon.done = job.done;
      addon.total = job.total;
      if (!job.error.empty())
        addon.error = job.error;
    }
  }
  return result;
}
void Core::refresh_addons(std::string app) {
  if (!session_ || options_.offline || !details_.contains(app) ||
      !addons_pending_.insert(app).second)
    return;
  auto plan = details_.at(app).plan;
  auto current = installed(app, false);
  auto binary = current ? current->plan.binary_id : plan.binary_id;
  auto root = current ? root_of(*current) : settings_.games;
  bool have_install = current != nullptr;
  auto token = session_->access_token;
  auto epoch = epoch_;
  addon_errors_.erase(app);
  worker_->submit([this, app, plan, binary, root, have_install, token,
                   epoch](std::stop_token stop) -> Worker::Completion {
    try {
      meta::Store store(token, stop);
      // A cached plan without its asset catalog is queried again, so each
      // refresh retries the full catalog until it succeeds.
      auto catalog = binary == plan.binary_id && plan.assets_complete
                         ? plan
                         : store.owned_plan(app, binary);
      // A reduced catalog keeps the current add-ons and installed plan.json.
      meta::require_asset_catalog(catalog);
      auto list = store.addons(catalog);
      return [this, app, epoch, root, have_install,
              catalog = std::move(catalog), list = std::move(list)] {
        if (epoch != epoch_)
          return;
        addons_pending_.erase(app);
        if (auto d = details_.find(app); d != details_.end() &&
            d->second.plan.binary_id == catalog.binary_id)
          d->second.plan = catalog;
        auto current = installed(app, false);
        if (root != (current ? root_of(*current) : settings_.games) ||
            (current && current->plan.binary_id != catalog.binary_id) ||
            (!current && have_install))
          return;
        addons_[app] = list;
        if (current)
          action([&] {
            if (downloads_->refresh_catalog(root, catalog))
              rescan();
          });
      };
    } catch (...) {
      auto message = exception_text();
      return [this, app, epoch, root, message] {
        if (epoch != epoch_)
          return;
        addons_pending_.erase(app);
        auto current = installed(app, false);
        if (root == (current ? root_of(*current) : settings_.games))
          addon_errors_[app] = message;
      };
    }
  });
}
void Core::install_addon(std::string_view app, std::string_view sku) {
  if (options_.offline || !session_)
    throw std::runtime_error("Sign in to install add-ons.");
  auto current = installed(app);
  if (!current)
    throw std::runtime_error("Install the base game before its add-ons.");
  auto list = addons(app);
  auto addon = std::ranges::find(list, sku, &meta::Addon::sku);
  if (addon == list.end() || !addon->owned)
    throw std::runtime_error(
        "Refresh add-ons to confirm ownership before installing.");
  downloads_->enqueue_addon(std::string(app), addon->name, root_of(*current),
                            current->plan.binary_id, std::string(sku),
                            session_->access_token);
}
void Core::uninstall_addon(std::string_view app, std::string_view sku) {
  auto current = installed(app, false);
  if (!current)
    throw std::runtime_error("Installed game was not found.");
  auto running = running_.find(std::string(app));
  if (running != running_.end() && !running->second.exit)
    throw std::runtime_error("Close this game before removing an add-on.");
  FileLock lock(data_ / "locks" / ("game-" + current->plan.package + ".lock"), false);
  downloads_->uninstall_addon(app, root_of(*current), current->plan.binary_id,
                              sku);
  rescan();
}
Core::SaveImport Core::import_saves(std::string_view app_id, const fs::path &folder, std::string serial) {
  auto current = installed(app_id, false);
  if (!current) throw std::runtime_error("Install this game before importing its save data.");
  const auto package = current->plan.package;
  auto running = running_.find(std::string(app_id));
  if (running != running_.end() && !running->second.exit)
    throw std::runtime_error("Close this game before importing its save data.");
  // Also keeps the game from starting until the import is done.
  auto lock = std::make_shared<FileLock>(data_ / "locks" / ("game-" + package + ".lock"), false);
  auto key = launch_choice(app_id, play_key(app_id)).account_key;
  if (key.empty()) key = "local";
  if (!folder.empty())
    return [data = data_, key, package, folder, lock](std::stop_token stop) {
      auto found = saves::find_local(folder, package);
      if (!found)
        throw std::runtime_error("No save data for " + package + " was found there. Pick a copy of the game's "
                                 "Android/data/" + package + " folder, or of its files folder.");
      return saves::import_local(data, key, package, *found, stop);
    };
  const auto adb = adb_path();
  if (adb.empty()) throw std::runtime_error("adb was not found. Install Android platform-tools, or set adb's location in Settings.");
  return [data = data_, key, package, adb, serial, lock](std::stop_token stop) {
    auto chosen = serial;
    if (chosen.empty()) {
      auto devices = android::adb::Client(adb).devices(stop);
      auto ready = std::ranges::find_if(devices, &android::adb::Device::ready);
      if (ready == devices.end())
        throw std::runtime_error(devices.empty() ? "No headset found. Connect it with a USB cable (or turn on Wi-Fi "
                                                   "debugging) and enable developer mode in the Meta Horizon app."
                                                 : android::adb::state_message(devices.front()));
      chosen = ready->serial;
    }
    return saves::import_headset(data, key, package, android::adb::Client(adb, chosen), stop);
  };
}
void Core::import_saves_later(std::string_view app_id, const fs::path &folder) {
  auto task = import_saves(app_id, folder);
  notice_ = "Importing save data...";
  import_worker_->submit([this, task = std::move(task)](std::stop_token stop) -> Worker::Completion {
    std::string message;
    bool imported = false;
    try {
      auto n = task(stop);
      message = std::format("Imported {} save file{}.{}", n.files, n.files == 1 ? "" : "s",
                            n.backup.empty() ? "" : " The save data it replaced is kept in a backup.");
      imported = true;
    } catch (...) {
      message = exception_text();
    }
    return [this, message, imported] {
      if (imported) notice_ = message;
      else error(message);
    };
  });
}
void Core::install_all_owned_addons(std::string_view app) {
  if (!addons_.contains(std::string(app)))
    throw std::runtime_error(
        "Refresh add-ons to confirm ownership before installing.");
  for (auto &addon : addons(app))
    if (addon.owned && !addon.installed && !addon.downloading)
      install_addon(app, addon.sku);
}
void Core::play(const meta::Game &g) { play_as(g.id, play_key(g.id)); }
std::string Core::play_key(std::string_view app_id) const {
  auto pick = owner_pick_.find(std::string(app_id));
  return pick == owner_pick_.end() ? std::string{} : pick->second;
}
const AccountView *Core::account(std::string_view key) const {
  auto it = std::ranges::find(accounts_, key, &AccountView::key);
  return it == accounts_.end() ? nullptr : &*it;
}
std::string Core::owner_for(std::string_view app_id) const {
  auto owners = game_owners(app_id);
  auto owns = [&](std::string_view key) {
    return std::ranges::find(owners, key) != owners.end();
  };
  if (auto pick = owner_pick_.find(std::string(app_id));
      pick != owner_pick_.end() && owns(pick->second))
    return pick->second;
  if (owns(account_key_))
    return account_key_;
  for (const auto &key : owners)
    if (auto a = account(key); a && a->signed_in && !a->needs_reauth)
      return key;
  return owners.empty() ? std::string{} : owners.front();
}
void Core::play_as(std::string_view id, std::string_view requested) {
  play_as(id, requested, {});
}
void Core::play_as(std::string_view id, std::string_view requested, const LaunchDetails& details) {
  launch_game(id, requested, read_launch_details(launch_details_json(details)));
}
bool Core::LaunchRequest::begin() {
  std::lock_guard lock(mutex);
  if (done) return false;
  started = true;
  return true;
}
void Core::LaunchRequest::finish(bool success) {
  std::lock_guard lock(mutex);
  if (!done) { done = true; result.set_value(success); }
}
void Core::LaunchRequest::cancel() {
  std::lock_guard lock(mutex);
  // Once spawn begins, its short UI-thread completion supplies the outcome.
  if (!started && !done) { done = true; result.set_value(false); }
}
Json Core::request_launch(const std::shared_ptr<LaunchQueue>& queue,
                         const std::string& account, const Json& message) {
  auto request = std::make_shared<LaunchRequest>();
  request->target = message.at("app").get<std::string>();
  request->account = account; // launcher-bound caller; never a guest account
  request->details = read_launch_details(message.at("details"));
  auto result = request->result.get_future();
  {
    std::lock_guard lock(queue->mutex);
    std::erase_if(queue->outstanding, [](auto& weak) { return weak.expired(); });
    if (queue->closed || queue->outstanding.size() >= 32) return {{"success", false}};
    queue->outstanding.push_back(request);
    queue->pending.push_back(request);
  }
  if (result.wait_for(std::chrono::seconds(20)) != std::future_status::ready)
    request->cancel();
  return {{"success", result.get()}};
}
void Core::launch_game(std::string_view id, std::string_view requested,
                       const LaunchDetails& details, std::shared_ptr<LaunchRequest> reply) {
  // Any installed game launches; the account only decides whose platform
  // answers the game gets.
  auto choice = requested == "local" ? library::LaunchChoice{}
                                     : launch_choice(id, requested);
  if (choice.status == library::LaunchStatus::reconnect)
    throw std::runtime_error("Reconnect that account before playing, or choose another.");
  const std::string game_id(id);
  auto i = installed(game_id);
  if (!i)
    throw std::runtime_error(
        "This game is not completely installed. Resume its download first.");
  // A manifest another installer wrote (the legacy CLI) lacks the launcher's
  // catalog record. With an account, Play upgrades it from Meta's same-build
  // catalog before the runtime reads it, as selecting the game would.
  // A reduced catalog without asset files (Meta's full query failed at
  // install) is retried too, so its assets appear once Meta lists them.
  const bool no_assets = !i->plan.assets_complete &&
      std::ranges::none_of(i->plan.files, [](auto &f) { return f.kind == "asset"; });
  // Meta gives every asset a type (default, store, language_pack); a catalog
  // saved before Venture read it has none, so its language packs are hidden.
  const bool untyped_assets = i->plan.assets_complete &&
      std::ranges::any_of(i->plan.files, [](auto &f) {
        return f.kind == "asset" && !f.id.empty() && f.asset_type.empty();
      });
  std::string catalog_token;
  if ((!i->plan.launcher_format || no_assets || untyped_assets) && !i->plan.local &&
      !choice.account_key.empty())
    if (auto owner = vault_.load(choice.account_key))
      catalog_token = owner->access_token;
  if ((running_.contains(game_id) && running_[game_id].process.active()) ||
      !launch_pending_.insert(game_id).second)
    throw std::runtime_error("This game is already running or starting.");
  const auto install = *i;
  try {
    game_locks_[game_id] = std::make_unique<FileLock>(
        data_ / "locks" / ("game-" + install.plan.package + ".lock"), false);
  } catch (...) {
    launch_pending_.erase(game_id);
    throw std::runtime_error("This game is already running in another Venture launcher.");
  }
  const auto app_id = game_id;
  // The runtime's account for saves and platform calls; "local" is none.
  const auto key = choice.account_key.empty() ? std::string("local") : choice.account_key;
  const auto runtime = settings_.runtime;
  const auto root = root_of(install);
  const bool console = settings_.console;
  if (settings_.runtime_override())
    log::warn("launcher", "Using the runtime set in Settings: {}",
              paths::utf8(runtime));
  notice_ = catalog_token.empty()
                ? "Preparing application credentials..."
                : "Updating the installation manifest from Meta...";
  worker_->submit([this, install, app_id, key, runtime, root, details, reply,
                   console, catalog_token](std::stop_token stop) -> Worker::Completion {
    try {
      if (reply) {
        std::lock_guard lock(reply->mutex);
        if (reply->done) return [this, app_id] {
          launch_pending_.erase(app_id);
          game_locks_.erase(app_id);
        };
      }
      std::optional<meta::Plan> catalog;
      // Best effort: without Meta's asset catalog the installed manifest
      // stays; its files are still recovered in memory.
      if (!catalog_token.empty()) {
        try {
          auto fresh =
              meta::Store(catalog_token, stop)
                  .owned_plan(install.plan.app_id, install.plan.binary_id);
          if (meta::catalog_replaces(fresh, install.plan) &&
              (fresh.assets_complete || fresh.files.size() > install.plan.files.size()))
            catalog = std::move(fresh);
          else
            log::warn("launcher",
                      "Meta's asset catalog for {} is unavailable; launching "
                      "with the installed manifest.",
                      app_id);
        } catch (...) {
          if (stop.stop_requested())
            throw;
          log::warn("launcher",
                    "Could not read Meta's catalog for {} ({}); launching with "
                    "the installed manifest.",
                    app_id, exception_text());
        }
      }
      ovrplatform::AppIdentity identity{
          install.plan.app_id, install.plan.package, install.plan.binary_id,
          static_cast<std::int64_t>(std::stoll(install.plan.version_code))};
      auto broker = std::make_shared<ovrplatform::CredentialBroker>(
          identity, key, ovrplatform::runtime_secret_store(key),
          ovrplatform::meta_http_transport());
      auto error = broker->prepare(stop);
      if (error)
        throw std::runtime_error(error.message);
      if (stop.stop_requested())
        throw std::runtime_error("Game launch cancelled.");
      return [this, install, app_id, key, runtime, root, console, broker,
              details, reply, catalog = std::move(catalog)] {
        launch_pending_.erase(app_id);
        if (reply && !reply->begin()) {
          game_locks_.erase(app_id);
          return;
        }
        bool success = false;
        action([&] {
          auto valid = broker->handle(Json{{"op", "validate"}});
          if (number(valid, "error") != 0)
            throw std::runtime_error(
                "Account changed while the game was starting. Try again.");
          if (catalog) {
            if (!std::ranges::contains(settings_.roots(), root) ||
                paths::game_dir(root, catalog->package, catalog->version_code) !=
                    install.directory)
              throw std::runtime_error(
                  "The library folders changed while the game was starting. Try "
                  "again.");
            if (!downloads_->refresh_catalog(root, *catalog))
              throw std::runtime_error(
                  "Wait for this game's downloads to finish, then play again.");
            log::info("launcher",
                      "Upgraded the installation manifest of {} from Meta's "
                      "catalog before launch.",
                      app_id);
            rescan();
          }
          auto &r = running_[app_id];
          r.quitting = false;
          r.log = data_ / "logs" / ("runtime-" + install.plan.package + ".log");
          // The runtime writes its log itself; its stdio goes to a separate
          // file so lines are not written twice into one.
          r.console = data_ / "logs" /
                      ("runtime-" + install.plan.package + ".console.log");
          // The runtime appends to its log; start this run's log empty so
          // a failure is never explained by an earlier run.
          library::rotate_log(r.log);
          // Java exception traces go to the console log: keep the last run's.
          library::rotate_log(r.console);
          std::vector<std::string> args{
              "--game",       paths::utf8(install.directory),
              "--package",    install.plan.package,
              "--log",        paths::utf8(r.log),
              "--account",    key,
              "--parent-pid", std::to_string(current_process_id())};
          if (console)
            args.push_back("--console");
          if (options_.smoke_seconds)
            args.insert(args.end(), {"--run-seconds",
                                     std::to_string(options_.smoke_seconds)});
          std::map<std::string, PipeChannel::Handler, std::less<>> routes{
              {"asset.", download::asset_launcher_handler(
                             *downloads_, data_, root,
                             install.directory, key)}};
          // VENTURE_LOG_LEVEL: the launcher's log level, 0 (trace) to 4 (error).
          Environment environment{
              {"VENTURE_LOG_LEVEL", std::to_string(settings_.log_level)}};
          // The menu chord setting; automatic leaves the runtime's default.
          if (settings_.menu_button == 1) environment.push_back({"VENTURE_MENU_CHORD", "stick-clicks"});
          if (settings_.menu_button == 2) environment.push_back({"VENTURE_MENU_CHORD", "off"});
          // Texture compression; always is the runtime's default.
          if (settings_.texture_compression == 1) {
            environment.push_back({"VENTURE_TEXTURE_COMPRESSION", "auto"});
            environment.push_back({"VENTURE_TEXTURE_COMPRESSION_THRESHOLD",
                                   std::to_string(settings_.texture_compression_threshold)});
          }
          if (settings_.texture_compression == 2) environment.push_back({"VENTURE_TEXTURE_COMPRESSION", "never"});
          r.process.spawn(runtime, args, r.console, console,
                          [broker, routes = std::move(routes), details, key, queue = launch_queue_](const Json &request) {
                            const auto op = str(request, "op");
                            if (op == "launch_details" || op == "launch_other_app") {
                              if (number(broker->handle(Json{{"op", "validate"}}), "error") != 0)
                                return Json{{"success", false}};
                              if (op == "launch_details") return Json{{"details", launch_details_json(details)}};
                              return request_launch(queue, key, request);
                            }
                            for (const auto &[prefix, handler] : routes)
                              if (op.starts_with(prefix)) return handler(request);
                            return broker->handle(request);
                          }, environment);
          success = true; // the process is running, even if cache bookkeeping fails
          r.exit.reset();
          r.tail.clear();
          r.failure.clear();
          notice_.clear();
          if (auto entry = game(app_id)) {
            entry->recent = uint64_t(std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now()));
            save_library();
          }
          log::info("runtime",
                    "Launched Quest application {} using credential broker.",
                    app_id);
        });
        if (!success) {
          game_locks_.erase(app_id);
          if (!running_[app_id].process.active()) running_.erase(app_id);
        }
        if (reply) reply->finish(success);
      };
    } catch (...) {
      auto e = exception_text();
      return [this, app_id, e, reply] {
        launch_pending_.erase(app_id);
        game_locks_.erase(app_id);
        error(e);
        if (reply) reply->finish(false);
      };
    }
  });
}
fs::path Core::save_debug_report(std::string_view app_id, std::string_view version) const {
  DebugReport request{.data = data_, .version = std::string(version)};
  // A game no longer installed falls back to the most recent launch.
  for (const auto &install : installs_)
    if (!app_id.empty() && install.plan.app_id == app_id) {
      request.package = install.plan.package;
      request.plan = install.directory / "plan.json";
      if (auto g = std::ranges::find(games_, app_id, &meta::Game::id); g != games_.end()) request.game = g->name;
    }
  for (const auto &account : account_index_.accounts)
    request.private_text.insert(request.private_text.end(), {account.key, account.account_id});
  for (const auto &account : accounts_)
    request.private_text.insert(request.private_text.end(), {account.alias, account.display_name});
  return write_debug_report(request);
}
void Core::share_debug_report(fs::path file, std::function<void(std::string url, std::string error)> done) {
  worker_->submit([file = std::move(file), done = std::move(done)](std::stop_token stop) -> Worker::Completion {
    std::string url, error;
    try {
      url = upload_debug_report(file, stop);
      log::info("launcher", "Uploaded debug report {}", paths::utf8(file.filename()));
    } catch (...) {
      error = exception_text();
      log::warn("launcher", "Debug report upload failed: {}", error);
    }
    return [done, url = std::move(url), error = std::move(error)] { done(url, error); };
  });
}

void Core::sync_steam() {
  ++steam_generation_;
  if (!steam_sync_allowed_) return;
  if (scanning_) return;  // the scan in flight syncs when it finishes
  if (!steam_scan_reliable_) {
    steam_pending_ = false;
    log::warn("steam", "Steam sync skipped: installation scan is not authoritative.");
    if (steam_restart_ && !steam_library::running()) { // Steam closed for a restart; bring it back regardless.
      steam_restart_ = false;
      action([&] { steam_library::start_client(steam_library::discover_root()); });
    }
    return;
  }
  if (options_.launch_requested || steam_busy_) return;
  if (!settings_.add_games_to_steam) { steam_pending_ = false; return; }
  steam_busy_ = true;
  const auto generation = steam_generation_.load();
  std::vector<steam_library::Game> titles;
  std::set<std::string> packages;
  for (const auto& i : installs_) if (i.complete && !settings_.steam_skip.contains(i.plan.app_id) &&
                                     packages.insert(i.plan.package).second) {
    auto g = game(i.plan.app_id);
    titles.push_back({i.plan.package, g ? g->name : i.label.empty() ? i.plan.package : i.label, i.plan.app_id});
  }
  auto art = data_ / "cache/art";
  auto roots = settings_.roots();
  const bool online = !options_.offline;
  worker_->submit([this, titles, art, roots, generation, online](std::stop_token stop) -> Worker::Completion {
    try {
      // Steam's library sidebar wants the square app icon; fetch each listing's once.
      for (const auto &title : titles) {
        if (!online || !digits(title.art_id, 5) || stop.stop_requested()) continue;
        auto icon = art / (title.art_id + "_icon.image"), none = art / (title.art_id + ".noicon");
        if (fs::exists(icon) || fs::exists(none)) continue;
        try {
          auto uri = meta::Store({}, stop).artwork(title.art_id).icon;
          if (uri.empty()) paths::atomic_write(none, "");
          else paths::atomic_write(icon, http::get_image(uri, stop));
        } catch (const std::exception &e) {
          log::warn("steam", "No icon for application {}: {}", title.art_id, e.what());
        }
      }
      // A root may disappear or become unreadable after the UI's scan.
      for (const auto &root : roots) {
        if (!fs::is_directory(root)) throw std::runtime_error("Games root is unavailable.");
        (void)fs::directory_iterator(root); // Do not suppress permission errors.
      }
      auto result = steam_library::sync(steam_library::discover_root(), paths::executable(), art, titles,
          [this, generation] { return steam_generation_.load() != generation || steam_library::running(); });
      return [this, result, generation] {
        steam_busy_ = false;
        steam_pending_ = result.pending;
        steam_games_ = result.games;
        if (result.pending)
          log::info("steam", "{} games wait for Steam to close.", result.games);
        else
          log::info("steam", "Steam has {} Venture games.", result.games);
        if (generation != steam_generation_) { sync_steam(); return; }
        if (!steam_pending_ && steam_restart_) {
          steam_restart_ = false;
          action([&] { steam_library::start_client(steam_library::discover_root()); });
        }
      };
    } catch (const std::exception& e) {
      auto message = std::string(e.what());
      return [this, message, generation] {
        steam_busy_ = false;
        // Retrying every poll cannot fix it (an ambiguous Steam profile, an
        // unreadable root); the next scan or install tries again.
        steam_pending_ = false;
        log::warn("steam", "Steam sync skipped: {}", message);
        if (steam_restart_) {
          steam_restart_ = false;
          action([&] { steam_library::start_client(steam_library::discover_root()); });
        }
        if (generation != steam_generation_) sync_steam();
      };
    }
  });
}
void Core::restart_steam() {
  if (steam_restart_ || !steam_pending_) return;
  if (steam_library::vr_running()) return error("Close SteamVR first: Steam does not quit while SteamVR is running.");
  action([&] {
    steam_library::request_shutdown(steam_library::discover_root());
    steam_restart_ = true; // Start only after the observed close and safe sync.
  });
}
void Core::tick() {
  worker_->drain();
  import_worker_->drain();
  if ((steam_pending_ || steam_restart_) && !steam_busy_ && now_ms() - steam_poll_ > 2000) {
    steam_poll_ = now_ms();
    if (!steam_library::running()) {
      if (!settings_.add_games_to_steam && steam_restart_) {
        steam_restart_ = false;
        action([&] { steam_library::start_client(steam_library::discover_root()); });
      } else sync_steam();
    }
  }
  std::deque<std::shared_ptr<LaunchRequest>> launches;
  {
    std::lock_guard lock(launch_queue_->mutex);
    launches.swap(launch_queue_->pending);
  }
  for (auto& request : launches) {
    try {
      { std::lock_guard lock(request->mutex); if (request->done) continue; }
      const auto current = vault_.index();
      const auto account = current.find(request->account);
      // The target runs with the caller's account (or none, like the caller).
      if (request->account != "local" &&
          (!account || !account->signed_in || account->needs_reauth))
        throw std::runtime_error("The launching account is no longer signed in.");
      launch_game(request->target, request->account, request->details, request);
    } catch (...) { request->finish(false); }
  }
  while (auto event = browser_.poll()) {
    if (event->kind == AuthBrowser::Kind::ready) {
      auth_page_loaded_ = true;
      auth_status_ = "Meta sign-in is ready.";
      log::info("auth", "Meta sign-in page loaded.");
    }
    if (event->kind == AuthBrowser::Kind::error) {
      signing_in_ = false;
      auth_status_.clear();
      error(event->value);
    }
    if (event->kind == AuthBrowser::Kind::closed && signing_in_ &&
        !verifying_) {
      ++epoch_;
      signing_in_ = false;
      auth_.reset();
      auth_status_ = "Sign-in cancelled.";
    }
    if (event->kind == AuthBrowser::Kind::callback && auth_ && signing_in_ &&
        !verifying_) {
      verifying_ = true;
      auth_status_ = "Verifying your Quest account...";
      auto auth = auth_;
      auto callback = std::move(event->value);
      auto epoch = epoch_;
      worker_->submit([this, auth, callback,
                       epoch](std::stop_token stop) -> Worker::Completion {
        try {
          auto session = auth->complete(callback, stop);
          meta::Store store(session.access_token, stop);
          auto who = store.identity();
          session.account_id = who.account_id;
          session.alias = who.alias;
          session.display_name = who.display_name;
          // The account is saved even when its library cannot be listed yet;
          // Refresh lists it later.
          std::optional<meta::Snapshot> snapshot;
          std::string library_error;
          try {
            snapshot = store.library();
          } catch (...) {
            library_error = exception_text();
            log::warn("library", "Signed in, but the library could not be listed: {}", library_error);
          }
          return [this, session, snapshot, library_error, epoch] {
            if (epoch != epoch_)
              return;
            action([&] {
              if (!digits(session.account_id))
                throw std::runtime_error(
                    "Meta did not return a numeric account identity. Sign-in "
                    "was not saved.");
              const bool first = vault_.index().accounts.empty();
              account_key_ = vault_.upsert(session);
              session_ = session;
              account_scope_ = session.account_id;
              reload_accounts();
              downloads_->set_account(account_key_);
              downloads_->adopt_legacy(account_index_.primary);
              auto migrated =
                  migrate_saves(data_, account_index_.primary, first);
              if (migrated.moved && first)
                notice_ =
                    "Existing local saves were attached to your first account.";
              if (snapshot) {
                library::merge(games_, *snapshot, account_scope_);
                warning_ = snapshot->warning;
                library_partial_ = !snapshot->complete;
              } else {
                refresh_error_ = library_error;
                error(library_error);
              }
              save_library();
            });
            signing_in_ = verifying_ = false;
            auth_.reset();
            auth_status_.clear();
          };
        } catch (...) {
          auto e = exception_text();
          return [this, epoch, e] {
            if (epoch == epoch_) {
              signing_in_ = verifying_ = false;
              auth_.reset();
              auth_status_.clear();
              error(e);
            }
          };
        }
      });
    }
  }
  auto now = now_ms();
  if (now - last_poll_ > 400) {
    last_poll_ = now;
    for (auto &[id, r] : running_) {
      auto previous = r.exit;
      r.exit = r.process.poll();
      r.tail = tail_log(r.log);
      if (r.exit && !previous) {
        game_locks_.erase(id);
        log::info("runtime", "Quest application {} exited with code {}.", id,
                  *r.exit);
        if (library::needed_meta_sign_in(r.log)) {
          auto entry = game(id);
          r.failure = "It needs a Meta account. Use Sign in at the bottom left with the account that owns it, then "
                      "start it again.";
          error(std::format("{} closed: {}", entry ? entry->name : id, r.failure));
        } else if (*r.exit != 0) {
          // A runtime that failed before opening its log still wrote stderr.
          r.failure = library::runtime_failure(
              r.tail.empty() ? tail_log(r.console) : r.tail, *r.exit);
          auto entry = game(id);
          error(std::format("{} stopped: {}", entry ? entry->name : id,
                            r.failure));
        }
      }
    }
    auto jobs = downloads_->snapshot();
    auto complete = std::ranges::count_if(
        jobs, [](auto &j) { return j.state == download::State::complete; });
    const auto discarded = downloads_->discarded();
    if (size_t(complete) != last_completed_ || discarded != last_discarded_) {
      last_completed_ = complete;
      last_discarded_ = discarded;
      rescan();
      for (const auto &j : jobs)
        if (j.state == download::State::complete && j.saves_kept &&
            saves_kept_notified_.insert(j.app_id + '/' + std::to_string(j.generation)).second)
          notice_ = std::format("{} is installed with the saves already on this PC. To use the headset's saves "
                                "instead, choose Import save data in its menu.", j.name);
    }
  }
}
} // namespace venture::launcher
