#include "common/library/library.h"
#include "common/log.h"
#include "common/saves_migration.h"
#include "common/file_ops.h"
#include <algorithm>
#include <fstream>
#include <set>
namespace venture::library {
std::vector<Install> scan(const fs::path &root, bool* authoritative) {
  if (authoritative) *authoritative = false;
  std::vector<Install> result;
  if (!fs::exists(root) || (authoritative && !fs::is_directory(root)))
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
          !digits(paths::utf8(version.path().filename())) ||
          !paths::within(root, version.path()))
        continue;
      try {
        auto p =
            meta::parse_plan(paths::read_json(version.path() / "plan.json"));
        if (paths::game_dir(root, p.package, p.version_code) != version.path()) {
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
void uninstall(const fs::path &root, const Install &install) {
  auto target =
      paths::game_dir(root, install.plan.package, install.plan.version_code);
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
  remove_obb_aliases(paths::data_dir(), install.plan.package, target);
  remove_tree(root, target);
  // The runtime's extracted libraries and converted Java for this version.
  // Saves (the account data) stay.
  const auto cache = paths::data_dir() / "cache";
  const auto version = cache / paths::from_utf8(install.plan.package) / paths::from_utf8(install.plan.version_code);
  if (std::error_code ec; paths::safe_name(install.plan.package) && paths::safe_name(install.plan.version_code) &&
      fs::is_directory(version, ec))
    try {
      remove_tree(cache, version);
    } catch (const std::exception &error) {
      log::warn("library", "Could not remove the cache of {}: {}", install.plan.package, error.what());
    }
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
  auto dir = paths::game_dir(root, catalog.package, catalog.version_code);
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
void uninstall_addon(const fs::path &root, const Install &install,
                     std::string_view sku) {
  auto target =
      paths::game_dir(root, install.plan.package, install.plan.version_code);
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
bool needed_meta_sign_in(const fs::path &log) {
  std::ifstream in(log, std::ios::binary);
  for (std::string line; std::getline(in, line);)
    if (line.find("[ovrplatform] ovr_User_GetLoggedInUser request=") != std::string::npos ||
        line.find("[ovrplatform] ovr_Entitlement_GetIsViewerEntitled request=") != std::string::npos)
      if (line.find(" error=-20001") != std::string::npos) return true;
  return false;
}
std::string runtime_failure(std::string_view tail, int code) {
  std::string_view last, last_runtime, dump;
  while (!tail.empty()) {
    auto end = tail.find('\n');
    auto line = tail.substr(0, end);
    tail = end == tail.npos ? std::string_view{} : tail.substr(end + 1);
    constexpr std::string_view dump_marker = " I [runtime] Crash dump: ";
    if (auto at = line.find(dump_marker); at != line.npos)
      dump = line.substr(at + dump_marker.size());
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
    last = message;
    if (line.substr(marker + 4, close - marker - 4) == "runtime")
      last_runtime = message;
  }
  // main() reports a failed launch as "[runtime] <reason>".
  if (!last_runtime.empty())
    return std::string(last_runtime) + (dump.empty() ? "" : std::format(" Crash dump: {}", dump));
  // Anything else (a crash) gets the exit code; the last error line is context
  // only, because some subsystems log recoverable errors.
  auto message =
      code < 0 || code > 255
          ? std::format("The runtime stopped unexpectedly (exit code 0x{:08X}).",
                        static_cast<std::uint32_t>(code))
          : std::format("The runtime stopped unexpectedly (exit code {}).", code);
  if (!last.empty())
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
  s.menu_button = int(std::min<uint64_t>(2, number(j, "menuButton")));
  s.texture_compression = int(std::min<uint64_t>(2, number(j, "textureCompression")));
  if (auto percent = number(j, "textureCompressionThreshold"); percent >= 10 && percent <= 95)
    s.texture_compression_threshold = int(percent);
  if (auto jobs = number(j, "parallelDownloads"); jobs >= 1 && jobs <= 4)
    s.parallel_downloads = int(jobs);
  s.console = field(j, "launchWithConsole") == true;
  s.check_updates = field(j, "checkForUpdates") != false;
  s.dismissed_update = str(j, "dismissedUpdate");
  s.add_games_to_steam = field(j, "addGamesToSteam") != false;
  s.steam_add_new = field(j, "steamAddNew") != false;
  // Before install locations, the install dialog only asked about Steam.
  s.install_ask = field(j, j.contains("installAsk") ? "installAsk" : "steamAsk") != false;
  if (const auto &folders = field(j, "libraryFolders"); folders.is_array())
    for (const auto &folder : folders)
      if (auto path = folder.is_string() ? paths::from_utf8(folder.get<std::string>()) : fs::path();
          path.is_absolute() && path.lexically_normal() != s.games)
        s.libraries.push_back(path.lexically_normal());
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
         {"menuButton", menu_button},
         {"textureCompression", texture_compression},
         {"textureCompressionThreshold", texture_compression_threshold},
         {"parallelDownloads", parallel_downloads},
         {"launchWithConsole", console},
         {"checkForUpdates", check_updates},
         {"dismissedUpdate", dismissed_update},
         {"addGamesToSteam", add_games_to_steam},
         {"steamAddNew", steam_add_new},
         {"installAsk", install_ask},
         {"steamSkip", steam_skip}};
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
} // namespace venture::library
