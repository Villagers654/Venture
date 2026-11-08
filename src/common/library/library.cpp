#include "common/library/library.h"
#include "common/environment.h"
#include "common/log.h"
#include "common/saves_migration.h"
#include "common/file_ops.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <utility>
namespace venture::library {
std::vector<Install> scan(const fs::path &root, bool* authoritative) {
  if (authoritative) *authoritative = false;
  std::vector<Install> result;
  std::error_code missing;
  if (!fs::exists(root, missing) && !missing) {
    // A folder that is simply gone holds no games, so its scan is complete. A
    // folder on a drive that is not connected (its parent is gone too, or it
    // is the drive itself) may still hold games: that scan is not.
    if (authoritative) {
      const auto parent = root.parent_path();
      std::error_code ec;
      *authoritative = root.has_relative_path() && parent != root &&
                       fs::is_directory(parent, ec);
      if (!*authoritative)
        log::warn("library", "Library folder {} is not reachable; its games keep their Steam shortcuts.",
                  paths::utf8(root));
    }
    return result;
  }
  if (missing || (authoritative && !fs::is_directory(root)))
    return result;
  bool reliable = true;
  const auto options = authoritative ? fs::directory_options::none :
      fs::directory_options::skip_permission_denied;
  auto entries = [&](const fs::path& directory) {
    std::error_code error;
    auto iterator = fs::directory_iterator(directory, options, error);
    if (error) {
      if (!authoritative)
        throw fs::filesystem_error("Cannot read installation directory.", directory, error);
      log::warn("library", "Cannot read {}: {}", paths::utf8(directory), error.message());
      reliable = false;
      return fs::directory_iterator{};
    }
    return iterator;
  };
  for (auto &package : entries(root)) {
    if (paths::utf8(package.path().filename()).starts_with("steam-")) continue;
    if (!package.is_directory() ||
        !paths::safe_package(paths::utf8(package.path().filename())) ||
        !paths::within(root, package.path()))
      continue;
    for (auto &version : entries(package.path())) {
      if (!version.is_directory() ||
          !paths::version_folder(paths::utf8(version.path().filename())) ||
          !paths::within(root, version.path()))
        continue;
      // A version folder without a manifest is not an installation: what an
      // interrupted first download or an uninstall that could not remove
      // every file leaves behind. It neither lists a game nor makes the scan
      // incomplete; a manifest that exists but cannot be read does.
      if (std::error_code ec; !fs::exists(version.path() / "plan.json", ec) && !ec) {
        static std::mutex mutex;
        static std::set<fs::path> reported;
        if (std::lock_guard lock(mutex); reported.insert(version.path()).second)
          log::info("library", "{} has no installation manifest; it is not an installation.",
                    paths::utf8(version.path()));
        continue;
      }
      try {
        auto p =
            meta::parse_plan(paths::read_json(version.path() / "plan.json"));
        if (paths::game_dir(root, p.package, p.folder()) != version.path()) {
          log::warn("library", "{} holds a plan for {} version {}.", paths::utf8(version.path()), p.package,
                    p.version_code);
          reliable = false;
          continue;
        }
        // A plan without its asset catalog lists only the APK and OBB; those
        // decide completeness, and the plan keeps the marker for the launcher.
        Install install{p, version.path(), true, 0};
        for (auto &f : p.files) {
          if (!f.base())
            continue;
          auto target = version.path() / paths::from_utf8(f.name);
          if (!paths::within(version.path(), target) ||
              !fs::is_regular_file(target)) {
            install.complete = false;
            // An interrupted download's bytes count toward its progress.
            auto part = target;
            part += ".part";
            std::error_code ec;
            if (paths::within(version.path(), part) && fs::is_regular_file(part, ec)) {
              auto n = fs::file_size(part, ec);
              if (!ec) install.bytes += std::min<uint64_t>(n, f.size);
            }
            continue;
          }
          auto n = fs::file_size(target);
          install.bytes += n;
          if (!n || (f.size && f.size != n))
            install.complete = false;
        }
        result.push_back(std::move(install));
      } catch (const std::exception &e) {
        reliable = false;
        log::warn("library", "Skipped an unreadable installation manifest in {}: {}", paths::utf8(version.path()),
                  e.what());
      } catch (...) {
        reliable = false;
        log::warn("library", "Skipped an unreadable installation manifest.");
      }
    }
  }
  std::sort(result.begin(), result.end(), [](auto &a, auto &b) {
    return number(Json(a.plan.version_code)) >
           number(Json(b.plan.version_code));
  });
  if (authoritative) *authoritative = reliable;
  return result;
}
void merge(std::vector<meta::Game> &games, const meta::Snapshot &s,
           std::string_view account) {
  if (account.empty() || (!s.account_id.empty() && s.account_id != account))
    throw std::runtime_error(
        "Meta account changed. Sign in again before refreshing.");
  std::set<std::string> confirmed;
  for (auto &g : s.games) {
    if (!g.owned || !digits(g.id, 5) || g.platform == "PC")
      throw std::runtime_error("Invalid Quest entitlement snapshot.");
    confirmed.insert(g.id);
  }
  // Without a stable identity, absence is never used to revoke cached entries.
  if (s.complete && !s.account_id.empty())
    for (auto &g : games)
      if (g.ownership_account == account && !confirmed.contains(g.id))
        g.owned = false;
  for (auto g : s.games) {
    g.ownership_account = account;
    auto found = std::ranges::find_if(games, [&](auto &old) {
      return old.id == g.id && old.ownership_account == account;
    });
    // Store text Meta left out of this snapshot: this account's previous entry
    // first, else what another entry for the same app already knows.
    auto known = [&](std::string meta::Game::*member) -> std::string {
      if (found != games.end() && !((*found).*member).empty())
        return (*found).*member;
      for (const auto &old : games)
        if (old.id == g.id && !(old.*member).empty())
          return old.*member;
      return {};
    };
    if (g.description.empty())
      g.description = known(&meta::Game::description);
    if (g.publisher.empty())
      g.publisher = known(&meta::Game::publisher);
    if (g.package.empty())
      g.package = known(&meta::Game::package);
    if (found == games.end())
      games.push_back(g);
    else {
      if (g.image.empty())
        g.image = found->image;
      g.recent = found->recent;
      *found = std::move(g);
    }
  }
}
std::vector<meta::Game> load(const fs::path &file) {
  if (!fs::exists(file))
    return {};
  auto j = paths::read_json(file);
  std::vector<meta::Game> games;
  if (number(j, "version") != 1 || !field(j, "games").is_array())
    throw std::runtime_error(
        "Library cache is unreadable. Refresh your library.");
  for (auto &r : j["games"]) {
    if (!digits(str(r, "id"), 5))
      continue;
    meta::Game g;
    g.id = str(r, "id");
    g.name = str(r, "name");
    g.description = str(r, "description");
    g.publisher = str(r, "publisher");
    g.platform = str(r, "platform");
    if (auto package = str(r, "package"); paths::safe_package(package))
      g.package = package;
    g.ownership_account = str(r, "accountId");
    if (g.ownership_account.starts_with("session:"))
      continue;
    g.owned = field(r, "owned") == true;
    g.system = field(r, "system") == true;
    g.recent = number(r, "recent");
    // Artwork itself lives in cache/art/<id>.image. Never persist expiring
    // signed URLs.
    games.push_back(std::move(g));
  }
  return games;
}
void save(const fs::path &file, const std::vector<meta::Game> &games) {
  Json rows = Json::array();
  for (auto &g : games)
    rows.push_back({{"id", g.id},
                    {"name", g.name},
                    {"description", g.description},
                    {"publisher", g.publisher},
                    {"platform", g.platform},
                    {"package", g.package},
                    {"owned", g.owned},
                    {"system", g.system},
                    {"accountId", g.ownership_account},
                    {"recent", g.recent}});
  paths::atomic_write(
      file,
      Json{{"version", 1}, {"savedAt", paths::timestamp()}, {"games", rows}}
          .dump(2));
}
void uninstall(const fs::path &library, const Install &install) {
  // A library folder reached through a link (a games folder moved to another
  // drive with a junction) is removed from at its real location; links below
  // it are still refused.
  const auto root = fs::weakly_canonical(library);
  auto target =
      paths::game_dir(root, install.plan.package, install.plan.folder());
  if (fs::weakly_canonical(target) != fs::weakly_canonical(install.directory) ||
      !paths::within(root, target))
    throw std::runtime_error(
        "Installation path changed. Rescan the library first.");
  // Refuse traversing reparse points even when the final target is still inside
  // root.
  for (auto p = target; p != root && paths::within(root, p);
       p = p.parent_path())
    if (fs::is_symlink(p) ||
        fs::weakly_canonical(p) != fs::absolute(p).lexically_normal())
      throw std::runtime_error(
          "Cannot uninstall a linked installation directory.");
  auto manifest = meta::parse_plan(paths::read_json(target / "plan.json"));
  if (manifest.app_id != install.plan.app_id ||
      manifest.binary_id != install.plan.binary_id)
    throw std::runtime_error(
        "Installation changed. Rescan before uninstalling.");
  remove_obb_aliases(paths::data_dir(), install.plan.package, target,
                     install.plan.pinned ? install.plan.folder() : std::string());
  // The manifest goes last: an uninstall that cannot remove a file (the game
  // or a scanner still holds it) leaves an incomplete installation the
  // library still lists and can uninstall again, not anonymous leftovers.
  for (const auto &entry : fs::directory_iterator(target))
    if (entry.path().filename() != "plan.json") remove_tree(root, entry.path());
  remove_tree(root, target);
  // The runtime's extracted libraries and converted Java for this version.
  // Saves (the account data) stay.
  const auto cache = paths::data_dir() / "cache";
  const auto version = cache / paths::from_utf8(install.plan.package) / paths::from_utf8(install.plan.folder());
  if (std::error_code ec; paths::safe_name(install.plan.package) && paths::safe_name(install.plan.folder()) &&
      fs::is_directory(version, ec))
    try {
      remove_tree(cache, version);
    } catch (const std::exception &error) {
      log::warn("library", "Could not remove the cache of {}: {}", install.plan.package, error.what());
    }
}
namespace {
std::vector<Install> own_versions(const std::vector<Install> &installs, std::string_view id) {
  std::vector<Install> result;
  for (const auto &i : installs)
    if (!i.plan.pinned && i.id() == id) result.push_back(i);
  std::ranges::stable_sort(result, std::greater<>{},
                           [](const Install &i) { return number(Json(i.plan.version_code)); });
  return result;
}
}  // namespace
std::vector<Install> uninstall_set(const std::vector<Install> &installs, const Install &install) {
  if (install.plan.pinned) return {install};
  auto result = own_versions(installs, install.id());
  // The chosen one first, also when a rescan has not listed it yet.
  std::erase_if(result, [&](const Install &i) { return i.directory == install.directory; });
  result.insert(result.begin(), install);
  return result;
}
std::vector<Install> superseded(const std::vector<Install> &installs) {
  std::vector<Install> result;
  std::set<std::string> seen;
  for (const auto &i : installs) {
    if (i.plan.pinned || !seen.insert(i.id()).second) continue;
    auto versions = own_versions(installs, i.id());
    const auto &newest = versions.front();
    if (!newest.complete) continue;  // an update still downloading: the old version stays playable
    const auto code = number(Json(newest.plan.version_code));
    for (const auto &v : versions)
      if (number(Json(v.plan.version_code)) < code) result.push_back(v);
  }
  return result;
}
std::vector<std::string> owning_accounts(const std::vector<meta::Game> &games,
                                         const meta::AccountIndex &accounts,
                                         std::string_view app_id) {
  std::vector<std::string> keys;
  for (const auto &account : accounts.accounts)
    if (std::ranges::any_of(games, [&](const auto &g) {
          return g.id == app_id && g.owned &&
                 g.ownership_account == account.account_id;
        }))
      keys.push_back(account.key);
  return keys;
}
LaunchChoice launch_choice(const meta::AccountIndex &accounts,
                           const std::vector<std::string> &owners,
                           std::string_view requested) {
  auto usable = [&](std::string_view key) {
    auto account = accounts.find(key);
    return account && account->signed_in && !account->needs_reauth;
  };
  if (!requested.empty())
    return {usable(requested) ? LaunchStatus::ready : LaunchStatus::reconnect,
            std::string(requested)};
  for (const auto &owner : owners)
    if (usable(owner))
      return {LaunchStatus::ready, owner};
  if (!accounts.active.empty() && usable(accounts.active))
    return {LaunchStatus::ready, accounts.active};
  return {LaunchStatus::ready, {}};
}
bool addon_installed(const Install &install, std::string_view sku) {
  if (std::ranges::find(install.plan.installed_addons, sku) ==
      install.plan.installed_addons.end())
    return false;
  bool found = false;
  for (auto &f : install.plan.files) {
    if (f.sku != sku)
      continue;
    found = true;
    auto path = install.directory / paths::from_utf8(f.name);
    if (!paths::within(install.directory, path) || !fs::is_regular_file(path) ||
        fs::file_size(path) != f.size)
      return false;
  }
  return found;
}
void refresh_catalog(const fs::path &root, const meta::Plan &catalog) {
  auto dir = paths::game_dir(root, catalog.package, catalog.folder());
  if (!paths::within(dir, dir / "plan.json") ||
      !paths::within(dir, dir / "plan.json.tmp"))
    throw std::runtime_error("Unsafe installation manifest path.");
  auto previous = meta::parse_plan(paths::read_json(dir / "plan.json"));
  if (previous.app_id != catalog.app_id ||
      previous.binary_id != catalog.binary_id)
    throw std::runtime_error(
        "Installed build does not match the add-on catalog.");
  if (!meta::catalog_replaces(catalog, previous))
    throw std::runtime_error(meta::catalog_unavailable);
  auto plan = catalog;
  plan.pinned = previous.pinned;
  keep_installed_apk(plan, previous, dir);
  // An import's own data files (no Meta id) and recorded hashes stay.
  for (auto &f : previous.files) {
    auto same = std::ranges::find(plan.files, f.name, &meta::File::name);
    if (same == plan.files.end()) {
      if (f.id.empty())
        plan.files.push_back(f);
    } else if (same->size == f.size && same->sha256.empty()) {
      same->sha256 = f.sha256;
    }
  }
  plan.installed_addons.clear();
  auto addons = meta::group_addons(plan);
  for (auto &addon : addons)
    plan.installed_addons.push_back(addon.sku);
  Install candidate{plan, dir, false, 0};
  std::erase_if(plan.installed_addons,
                [&](auto &sku) { return !addon_installed(candidate, sku); });
  paths::atomic_write(dir / "plan.json", plan.json().dump(2));
}
void keep_installed_apk(meta::Plan &plan, const meta::Plan &installed,
                        const fs::path &directory) {
  if (plan.app_id != installed.app_id || plan.binary_id != installed.binary_id)
    return;
  auto old = std::ranges::find(installed.files, std::string("apk"), &meta::File::kind);
  if (old == installed.files.end() || !old->size)
    return;
  auto apk = std::ranges::find_if(plan.files, [&](const meta::File &f) {
    return f.kind == "apk" && f.name == old->name;
  });
  if (apk == plan.files.end() || apk->size == old->size)
    return;
  auto path = directory / paths::from_utf8(old->name);
  std::error_code ec;
  if (!paths::within(directory, path) || !fs::is_regular_file(path, ec) ||
      fs::file_size(path, ec) != old->size || ec)
    return;
  apk->size = old->size;
  apk->sha256 = old->sha256;
}
void uninstall_addon(const fs::path &root, const Install &install,
                     std::string_view sku) {
  auto target =
      paths::game_dir(root, install.plan.package, install.plan.folder());
  if (fs::weakly_canonical(target) != fs::weakly_canonical(install.directory))
    throw std::runtime_error("Installation path changed. Rescan first.");
  if (!paths::within(target, target / "plan.json") ||
      !paths::within(target, target / "plan.json.tmp"))
    throw std::runtime_error("Unsafe installation manifest path.");
  auto manifest = meta::parse_plan(paths::read_json(target / "plan.json"));
  if (manifest.app_id != install.plan.app_id ||
      manifest.binary_id != install.plan.binary_id)
    throw std::runtime_error(
        "Installation changed. Rescan before removing an add-on.");
  std::vector<fs::path> files;
  for (auto &f : manifest.files) {
    if (f.sku != sku)
      continue;
    if (f.base() || f.kind != "asset")
      throw std::runtime_error("Invalid add-on file.");
    auto path = target / paths::from_utf8(f.name);
    for (auto candidate :
         {path, paths::from_utf8(paths::utf8(path) + ".part")}) {
      if (!paths::within(target, candidate) ||
          (fs::exists(candidate) && !fs::is_regular_file(candidate)))
        throw std::runtime_error("Cannot remove an unsafe add-on file.");
      files.push_back(std::move(candidate));
    }
  }
  if (files.empty())
    throw std::runtime_error("Add-on is absent from the installed catalog.");
  // Remove the published state first; a failed/partial removal remains visibly
  // uninstalled and can be retried without touching the base game's files.
  std::erase(manifest.installed_addons, sku);
  paths::atomic_write(target / "plan.json", manifest.json().dump(2));
  for (auto &path : files)
    fs::remove(path);
}
namespace {
// The last "[xr] Frames submitted=" count in a run's log, 0 when absent.
unsigned long long frames_submitted(std::string_view line, unsigned long long frames) {
  constexpr std::string_view submitted = "[xr] Frames submitted=";
  if (auto at = line.find(submitted); at != std::string_view::npos)
    return std::strtoull(line.data() + at + submitted.size(), nullptr, 10);
  return frames;
}
}  // namespace
std::vector<std::string> missing_expansions(const Install &install) {
  std::vector<std::string> missing;
  for (const auto &f : install.plan.files) {
    if (!f.base() || f.kind != "obb") continue;
    const auto target = install.directory / paths::from_utf8(f.name);
    // Only a file that is not there; an unreadable one is not called missing.
    std::error_code ec;
    if (paths::within(install.directory, target) && fs::status(target, ec).type() == fs::file_type::not_found)
      missing.push_back(f.name);
  }
  return missing;
}
bool needed_meta_sign_in(const fs::path &log) {
  std::ifstream in(log, std::ios::binary);
  bool init_failed = false, crashed_on_reply = false;
  unsigned long long frames = 0;
  double last = 0, refused = -1, first_poll = -1;
  // The newest log lines, true for each one that is an unanswered user poll.
  std::deque<bool> recent;
  for (std::string line; std::getline(in, line);) {
    // log::write lines start with "<seconds>.<ms>"; continuation lines do not.
    char *end = nullptr;
    if (const double at = std::strtod(line.c_str(), &end); end != line.c_str() && at > 0) {
      last = at;
      const bool poll = line.find("[ovrplatform] ovr_GetLoggedInUserID ") != std::string::npos &&
                        line.find(" identity_returned=false") != std::string::npos;
      if (poll && first_poll < 0) first_poll = at;
      recent.push_back(poll);
      if (recent.size() > 200) recent.pop_front();
    }
    if (line.find("[ovrplatform] ovr_User_GetLoggedInUser request=") != std::string::npos ||
        line.find("[ovrplatform] ovr_Entitlement_GetIsViewerEntitled request=") != std::string::npos)
      if (line.find(" error=-20001") != std::string::npos) return true;
    // Synchronous initialization logs its own line; the asynchronous one
    // completes as an ordinary request.
    if (line.find("[ovrplatform] initialization failed error=-20001") != std::string::npos ||
        (line.find("[ovrplatform] ovr_PlatformInitializeAndroidAsynchronous request=") != std::string::npos &&
         line.find(" error=-20001") != std::string::npos))
      init_failed = true;
    if (line.find("[ovrplatform] ") != std::string::npos && line.find(" request=") != std::string::npos &&
        line.find(" error=") != std::string::npos)
      refused = last;
    if (line.find("[libc] Fatal signal ") != std::string::npos && refused >= 0 && last - refused < 10)
      crashed_on_reply = true;
    frames = frames_submitted(line, frames);
  }
  // A failed platform initialization alone is not fatal: many games play on
  // without it. One that quits before rendering past its first frames (Pinball
  // FX, Deadpool VR), or that crashes handling the failed reply of a later
  // platform request (a null user in an IAP callback), needed the account.
  // So did one that rendered on but spent its last lines doing nothing except
  // asking for the user (Pinball FX shows black and polls ~45 times a second).
  const auto polls = std::count(recent.begin(), recent.end(), true);
  const bool waiting_for_user = recent.size() == 200 && polls >= 180 && last - first_poll >= 30;
  return init_failed && (frames < 10 || crashed_on_reply || waiting_for_user);
}
bool needed_device_attestation(const fs::path &log) {
  std::ifstream in(log, std::ios::binary);
  double failed = -1, last = 0;
  for (std::string line; std::getline(in, line);) {
    // log::write lines start with "<seconds>.<ms>"; continuation lines do not.
    char *end = nullptr;
    const double at = std::strtod(line.c_str(), &end);
    if (end == line.c_str() || at <= 0) continue;
    last = at;
    if (line.find("[ovrplatform] ovr_DeviceApplicationIntegrity_GetIntegrityToken request=") != std::string::npos &&
        line.find(" error=") != std::string::npos)
      failed = at;
  }
  // Games that only report the token keep playing; one that closes soon after
  // the refusal (VirZOOM FLY shows "Integrity Token could not be retrieved")
  // was stopped by it.
  return failed >= 0 && last - failed < 60;
}
std::string game_dialog(const fs::path &log) {
  std::ifstream in(log, std::ios::binary);
  // Logged by the framework's android.app.Dialog.show().
  constexpr std::string_view marker = " [dialog] Shown without presentation: ";
  std::string text;
  double shown = -1, last = 0;
  unsigned long long frames = 0, frames_at_dialog = 0;
  for (std::string line; std::getline(in, line);) {
    // log::write lines start with "<seconds>.<ms>"; continuation lines do not.
    char *end = nullptr;
    const double at = std::strtod(line.c_str(), &end);
    if (end == line.c_str() || at <= 0) continue;
    last = at;
    frames = frames_submitted(line, frames);
    if (const auto found = line.find(marker); found != std::string::npos) {
      text = line.substr(found + marker.size());
      while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) text.pop_back();
      shown = at;
      frames_at_dialog = frames;
    }
  }
  return shown >= 0 && last - shown < 60 && frames - frames_at_dialog < 10 ? text : std::string{};
}
std::string missing_expansion(const fs::path &log) {
  std::ifstream in(log, std::ios::binary);
  std::string name;
  std::vector<std::string> errors;
  unsigned long long frames = 0;
  constexpr std::string_view marker = "[vfs] Missing expansion file: the game looked for ";
  for (std::string line; std::getline(in, line);) {
    if (auto at = line.find(marker); at != std::string::npos) {
      if (name.empty()) {
        name = line.substr(at + marker.size());
        name = name.substr(0, name.find(','));
      }
    } else if (line.find(" E [") != std::string::npos && line.find(".obb") != std::string::npos &&
               errors.size() < 64) {
      errors.push_back(line);
    }
    frames = frames_submitted(line, frames);
  }
  const bool failed = !name.empty() && std::ranges::any_of(errors, [&](const std::string &e) {
    return e.find(name) != std::string::npos;
  });
  // Games probe for optional patch files too (Unity looks for one at every
  // start: Batman: Arkham Shadow ships none). Only a game that reported an
  // error about the file and never got past its first frames was stopped by
  // it (Iron Man VR: "Unable to load GUID for '.../patch...obb'").
  return failed && frames < 10 ? name : std::string{};
}
std::string runtime_failure(std::string_view tail, int code) {
  // fatal: the last fatal signal; cause: the trap that raised it (an import
  // with no implementation, or an A32 call without an ABI adapter), if any.
  std::string_view last, last_runtime, dump, fatal, cause, trap, guest_fault, guest_cause, session, stall_seconds;
  // java_fatal: the first uncaught Java exception (AppMain logs it as Android's
  // AndroidRuntime does); it closes the app, so later error lines are its fallout.
  std::string_view java_fatal;
  // A frame-stall snapshot with no frame after it: the game was hung when it
  // was closed (by the user or the quit backstop). Its thread rows are
  // diagnostics, not an error, and must not be quoted as the reason.
  bool stalled = false;
  // An exit status 128+N is signal N. A guest crash handler (Unity's) that
  // re-raises leaves no libc line; Venture's fault summary still names it.
  const auto fault_marker = code > 128 && code < 160 ? std::format(" signal {} ", code - 128) : std::string{};
  while (!tail.empty()) {
    auto end = tail.find('\n');
    auto line = tail.substr(0, end);
    tail = end == tail.npos ? std::string_view{} : tail.substr(end + 1);
    constexpr std::string_view dump_marker = " I [runtime] Crash dump: ";
    if (auto at = line.find(dump_marker); at != line.npos)
      dump = line.substr(at + dump_marker.size());
    // A session created after a failed one supersedes the failure.
    if (line.find(" I [xr] Host xrCreateSession result=0") != line.npos) session = {};
    if (line.find("[xr] Frames submitted=") != line.npos) {
      stalled = false;  // frames resumed after a stall (a long level load)
      stall_seconds = {};
    }
    // log::write: "<seconds>.<ms> <thread> <level> [<tag>] <message>"
    auto marker = line.find(" E [");
    if (marker == line.npos)
      continue;
    auto close = line.find("] ", marker + 4);
    if (close == line.npos)
      continue;
    auto message = line.substr(close + 2);
    while (!message.empty() && (message.back() == '\r' || message.back() == ' '))
      message.remove_suffix(1);
    if (message.empty())
      continue;
    const auto tag = line.substr(marker + 4, close - marker - 4);
    if (tag == "startup-snapshot") {
      stalled = true;
      constexpr std::string_view no_frame = "No frame submitted for ";
      if (message.starts_with(no_frame)) {
        auto seconds = message.substr(no_frame.size());
        stall_seconds = seconds.substr(0, seconds.find(' '));
      }
      continue;
    }
    last = message;
    if (tag == "AndroidRuntime" && message.starts_with("FATAL EXCEPTION: ") && java_fatal.empty())
      java_fatal = message;
    if (tag == "runtime")
      last_runtime = message;
    if ((tag == "a32-abi" && message.starts_with("unsupported ")) ||
        (tag == "hle" && message.starts_with("unimplemented ")))
      trap = message;
    if (tag == "signal" && !fault_marker.empty() && message.starts_with("guest tid ") &&
        message.find(fault_marker) != message.npos) {
      guest_fault = message;
      // Only a trap raises SIGSYS; any other signal is not caused by one.
      guest_cause = code == 128 + 31 ? std::exchange(trap, {}) : std::string_view{};
    }
    if (tag == "xr" && message.starts_with("The VR session could not be created: "))
      session = message;
    if (tag == "libc" && message.starts_with("Fatal signal ")) {
      fatal = message;
      cause = std::exchange(trap, {});
    }
  }
  // main() reports a failed launch as "[runtime] <reason>".
  if (!last_runtime.empty())
    return std::string(last_runtime) + (dump.empty() ? "" : std::format(" Crash dump: {}", dump));
  const auto exit_code = code < 0 || code > 255
      ? std::format("0x{:08X}", static_cast<std::uint32_t>(code)) : std::to_string(code);
  // A game without its VR session crashes on the invalid handle later
  // (OVRPlugin aborts in ovrp_SetupLayer); the failed session is the cause.
  if (!session.empty())
    return std::format("{}. The game stopped after that (exit code {}).", session, exit_code) +
        (dump.empty() ? "" : std::format(" Crash dump: {}", dump));
  // A hung game that was closed did not crash: say so instead of quoting an
  // unrelated error line. A fatal signal still outranks the stall.
  if (stalled && fatal.empty() && guest_fault.empty() && java_fatal.empty())
    return std::format("The game stopped responding{} and was closed (exit code {}).",
                       stall_seconds.empty() ? "" : std::format(" (no new frame for {} s)", stall_seconds),
                       exit_code) +
           (dump.empty() ? "" : std::format(" Crash dump: {}", dump));
  // Anything else (a crash) gets the exit code; the last error line is context
  // only, because some subsystems log recoverable errors.
  auto message = std::format("The runtime stopped unexpectedly (exit code {}).", exit_code);
  // A fatal signal outranks error lines around it: a game's own non-fatal
  // errors (a failed HTTPS request) are often logged last, even after it.
  if (!java_fatal.empty())
    message += std::format(" Last error: {}", java_fatal);
  else if (!fatal.empty())
    message += cause.empty() ? std::format(" Last error: {}", fatal)
                             : std::format(" Last error: {} ({})", cause, fatal);
  else if (!guest_fault.empty())
    message += guest_cause.empty() ? std::format(" Last error: {}", guest_fault)
                                   : std::format(" Last error: {} ({})", guest_cause, guest_fault);
  else if (!last.empty())
    message += std::format(" Last error: {}", last);
  if (!dump.empty()) message += std::format(" Crash dump: {}", dump);
  return message;
}
void rotate_log(const fs::path &log) {
  std::error_code ec;
  if (!fs::exists(log, ec))
    return;
  auto previous = log;
  previous.replace_extension(".prev" + log.extension().string());
  fs::rename(log, previous, ec);
  if (ec) {
    // Still open (an orphaned runtime): at least drop the old contents.
    log::warn("launcher", "Could not move aside {}: {}", paths::utf8(log),
              ec.message());
    fs::resize_file(log, 0, ec);
  }
}
bool Settings::runtime_override() const {
  return runtime.lexically_normal() !=
         defaults({}).runtime.lexically_normal();
}
std::vector<fs::path> Settings::roots() const {
  std::vector<fs::path> result{games};
  for (const auto &folder : libraries)
    if (std::ranges::find(result, folder) == result.end())
      result.push_back(folder);
  return result;
}
Settings Settings::defaults(const fs::path &data) {
#ifdef _WIN32
  return {data / "games",
          paths::executable().parent_path() / "venture-runtime.exe", {}, 2,
          false};
#else
  return {data / "games",
          paths::executable().parent_path() / "venture-runtime", {}, 2, false};
#endif
}
bool Settings::texture_compression_warns(std::string_view before, std::string_view after) const {
  return !texture_compression_warned && before == "always" && after != "always";
}
const controls::Config& Settings::controls_for(std::string_view app_id) const {
  const auto it = game_controls.find(std::string(app_id));
  return it == game_controls.end() ? controls : it->second;
}
Settings Settings::load(const fs::path &data) {
  auto s = defaults(data);
  if (!fs::exists(data / "settings.json"))
    return s;
  auto j = paths::read_json(data / "settings.json");
  auto games = paths::from_utf8(str(j, "gamesDirectory")),
       runtime = paths::from_utf8(str(j, "runtimeExecutable"));
  if (games.is_absolute())
    s.games = games.lexically_normal();
  // A saved runtime that no longer exists (moved build, old install) falls back
  // to the one shipped beside the launcher.
  if (runtime.is_absolute() && fs::exists(runtime))
    s.runtime = runtime.lexically_normal();
  if (auto adb = paths::from_utf8(str(j, "adbExecutable")); adb.is_absolute())
    s.adb = adb.lexically_normal();
  s.log_level = int(std::min<uint64_t>(4, number(j, "logLevel")));
  if (const auto size = number(j, "interfaceSize"); size >= 100 && size <= 200)
    s.interface_size = int(size);
  if (const auto& controls = field(j, "controls"); controls.is_object()) {
    if (const auto& global = field(controls, "global"); global.is_object()) s.controls = controls::decode(global.dump());
    if (const auto& games = field(controls, "games"); games.is_object())
      for (const auto& [id, config] : games.items())
        if (config.is_object() && id.size() <= 64) s.game_controls[id] = controls::decode(config.dump());
  } else {
    // Before the Controls page: Settings > Runtime "Menu from both stick clicks".
    s.controls = controls::migrate_menu_button(int(std::min<uint64_t>(2, number(j, "menuButton"))));
  }
  s.texture_compression = int(std::min<uint64_t>(2, number(j, "textureCompression")));
  if (auto percent = number(j, "textureCompressionThreshold"); percent >= 10 && percent <= 95)
    s.texture_compression_threshold = int(percent);
  s.texture_compression_warned = field(j, "textureCompressionWarned") == true;
  if (auto jobs = number(j, "parallelDownloads"); jobs >= 1 && jobs <= 4)
    s.parallel_downloads = int(jobs);
  s.keep_translated_code = field(j, "keepTranslatedCode") != false;
  if (auto tag = str(j, "gameLanguage"); tag.size() <= 16 && std::ranges::all_of(tag, [](unsigned char c) {
        return std::isalnum(c) || c == '-';
      }))
    s.game_language = tag;
  if (auto code = str(j, "interfaceLanguage"); code.size() <= 16 && std::ranges::all_of(code, [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
      }))
    s.interface_language = code;
  // On unless turned off. The earlier opt-in keys ("passthrough",
  // "steamFramePassthrough") only recorded a default-off state and are ignored.
  s.passthrough = field(j, "mixedReality") != false;
  s.color_key = field(j, "passthroughColorKey") == true;
  if (const auto color = str(j, "passthroughKeyColor"); environment::parse_color_key(color))
    s.key_color = color.starts_with('#') ? color.substr(1) : color;
  s.console = field(j, "launchWithConsole") == true;
  s.check_updates = field(j, "checkForUpdates") != false;
  s.dismissed_update = str(j, "dismissedUpdate");
  s.add_games_to_steam = field(j, "addGamesToSteam") != false;
  s.steam_add_new = field(j, "steamAddNew") != false;
  // Before install locations, the install dialog only asked about Steam.
  s.install_ask = field(j, j.contains("installAsk") ? "installAsk" : "steamAsk") != false;
  s.lookup_imports_online = field(j, "lookupImportsOnline") != false;
  if (const auto &folders = field(j, "libraryFolders"); folders.is_array())
    for (const auto &folder : folders)
      if (auto path = folder.is_string() ? paths::from_utf8(folder.get<std::string>()) : fs::path();
          path.is_absolute() && path.lexically_normal() != s.games)
        s.libraries.push_back(path.lexically_normal());
  s.display = display_from_json(field(j, "display"), false);
  if (const auto &games_display = field(j, "gameDisplay"); games_display.is_object())
    for (const auto &[id, value] : games_display.items())
      if (auto d = display_from_json(value, true); d != game_display_defaults()) s.game_display[id] = d;
  if (const auto &layers = field(j, "vulkanLayers"); layers.is_array())
    for (const auto &layer : layers)
      if (layer.is_string() && !layer.get<std::string>().empty() &&
          layer.get<std::string>().find_first_of(",;") == std::string::npos)
        s.vulkan_layers.push_back(layer.get<std::string>());
  if (const auto &skip = field(j, "steamSkip"); skip.is_array())
    for (const auto &id : skip)
      if (id.is_string())
        s.steam_skip.insert(id.get<std::string>());
  return s;
}
void Settings::save(const fs::path &data) const {
  if (!games.is_absolute() || !runtime.is_absolute())
    throw std::runtime_error(
        "Use absolute paths for the games folder and runtime executable.");
  fs::create_directories(games);
  Json j{{"version", 1},
         {"gamesDirectory", paths::utf8(games)},
         {"logLevel", log_level},
         {"interfaceSize", interface_size},
         {"textureCompression", texture_compression},
         {"textureCompressionThreshold", texture_compression_threshold},
         {"textureCompressionWarned", texture_compression_warned},
         {"parallelDownloads", parallel_downloads},
         {"keepTranslatedCode", keep_translated_code},
         {"gameLanguage", game_language},
         {"interfaceLanguage", interface_language},
         {"mixedReality", passthrough},
         {"passthroughColorKey", color_key},
         {"passthroughKeyColor", key_color},
         {"launchWithConsole", console},
         {"checkForUpdates", check_updates},
         {"dismissedUpdate", dismissed_update},
         {"addGamesToSteam", add_games_to_steam},
         {"steamAddNew", steam_add_new},
         {"installAsk", install_ask},
         {"lookupImportsOnline", lookup_imports_online},
         {"steamSkip", steam_skip}};
  {
    Json games = Json::object();
    for (const auto& [id, config] : game_controls) games[id] = Json::parse(controls::encode(config));
    j["controls"] = Json{{"global", Json::parse(controls::encode(controls))}, {"games", std::move(games)}};
  }
  j["display"] = display_to_json(display);
  Json games_display = Json::object();
  for (const auto &[id, d] : game_display)
    if (d != game_display_defaults()) games_display[id] = display_to_json(d);
  if (!games_display.empty()) j["gameDisplay"] = std::move(games_display);
  if (!vulkan_layers.empty()) j["vulkanLayers"] = vulkan_layers;
  if (!libraries.empty()) {
    Json folders = Json::array();
    for (const auto &folder : libraries)
      folders.push_back(paths::utf8(folder));
    j["libraryFolders"] = std::move(folders);
  }
  // The bundled runtime is never pinned: a later launcher update brings its own.
  if (runtime_override())
    j["runtimeExecutable"] = paths::utf8(runtime);
  if (!adb.empty()) {
    if (!adb.is_absolute())
      throw std::runtime_error("Use an absolute path for adb.");
    j["adbExecutable"] = paths::utf8(adb);
  }
  paths::atomic_write(data / "settings.json", j.dump(2));
}
std::vector<std::pair<std::string, std::string>> launch_environment(const Settings &s, std::string_view app_id,
                                                                    bool automation) {
  // VENTURE_LOG_LEVEL: the launcher's log level, 0 (trace) to 4 (error).
  std::vector<std::pair<std::string, std::string>> environment{{"VENTURE_LOG_LEVEL", std::to_string(s.log_level)}};
  // Always set, so an inherited value never overrides the setting.
  environment.push_back({"VENTURE_PASSTHROUGH", s.passthrough ? "1" : "0"});
  // The player's controls (the game's own when it has them).
  environment.push_back({"VENTURE_CONTROLS", controls::encode(s.controls_for(app_id))});
  // The color key passthrough color, only when the user chose it; empty otherwise.
  environment.push_back({"VENTURE_PASSTHROUGH_KEY", s.passthrough && s.color_key ? s.key_color : ""});
  if (!s.game_language.empty()) environment.push_back({"VENTURE_LOCALE", s.game_language});
  // Implicit Vulkan layers the player keeps; the runtime leaves out the rest.
  if (!s.vulkan_layers.empty()) {
    std::string layers;
    for (const auto &layer : s.vulkan_layers) layers += (layers.empty() ? "" : ",") + layer;
    environment.push_back({"VENTURE_VULKAN_LAYERS", layers});
  }
  const auto own = s.game_display.find(std::string(app_id));
  const auto *game = own == s.game_display.end() ? nullptr : &own->second;
  // Texture compression, this game's over the global one; always is the
  // runtime's default and sets nothing.
  const auto [mode, threshold] =
      texture_compression_environment(s.texture_compression, s.texture_compression_threshold, game);
  if (mode != "always") environment.push_back({"VENTURE_TEXTURE_COMPRESSION", mode});
  if (threshold) environment.push_back({"VENTURE_TEXTURE_COMPRESSION_THRESHOLD", std::to_string(threshold)});
  // Keep translated code; on is the runtime's default.
  if (!s.keep_translated_code) {
    environment.push_back({"VENTURE_JIT_PTC", "0"});
    environment.push_back({"VENTURE_JIT_PRETRANSLATE", "0"});
  }
  // Display settings, this game's over the global ones; "runtime" overrides inherited values too.
  const auto display = effective_display(s.display, game);
  environment.push_back({"VENTURE_RESOLUTION", resolution_environment(display)});
  environment.push_back({"VENTURE_FRAME_RATE", frame_rate_environment(display)});
  environment.push_back({"VENTURE_MIRROR", automation ? "off" : mirror_environment(display)});
  environment.push_back({"VENTURE_MIRROR_EYE", mirror_eye_environment(display)});
  // The game's audio on the PC too; off by default.
  environment.push_back({"VENTURE_PC_AUDIO", pc_audio_environment(display)});
  return environment;
}
} // namespace venture::library
