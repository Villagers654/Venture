#include "common/library/save_import.h"

#include "common/file_ops.h"
#include "common/library/import.h"
#include "common/log.h"
#include "common/saves_migration.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <set>
#include <sstream>

namespace venture::saves {
namespace {
bool directory(const fs::path &p) {
  std::error_code ec;
  return fs::is_directory(p, ec) && !is_link(p);
}
// Folders only an app's private storage has.
bool private_storage(const fs::path &p) {
  for (auto name : {"shared_prefs", "databases", "no_backup", "app_webview"})
    if (directory(p / name)) return true;
  return false;
}
// What the game or runtime regenerates, and in private storage the folders
// that hold the other storage areas here, are never imported.
bool imported(Area area, const fs::path &name) {
  if (name == "cache" || name == "code_cache" || name == "lib") return false;
  return area == Area::external ||
         (name != "external-data" && name != "external" && name != "volumes");
}
std::optional<Found> found_in(const fs::path &folder) {
  return Found{folder, private_storage(folder) ? Area::internal : Area::external};
}

// A backup name for now that `backups` does not use yet.
std::string new_stamp(const fs::path &backups) {
  auto stamp =
      std::format("{:%Y-%m-%d %H-%M-%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
  std::error_code ec;
  for (int i = 2; fs::exists(backups / stamp, ec) || fs::exists(backups / (stamp + ".import"), ec) ||
                  fs::exists(backups / (stamp + ".export"), ec);
       ++i)
    stamp = std::format("{} ({})", stamp.substr(0, 19), i);
  return stamp;
}
// The area's folder in the account's storage, and a staging and a backup
// folder for this import on the same volume.
struct Target {
  fs::path root, staging, backup;
};
Target target(const fs::path &data, std::string_view key, std::string_view package, Area area) {
  const auto game = account_data_dir(data, key, package);
  const auto backups = backups_dir(data, key, package);
  const auto stamp = new_stamp(backups);
  return {area == Area::internal ? game : game / "external-data", backups / (stamp + ".import"),
          backups / stamp / (area == Area::internal ? "private" : "Android-data")};
}
void count(Imported &n, const fs::path &file) {
  ++n.files;
  n.bytes += fs::file_size(file);
}
void copy_tree(const fs::path &from, const fs::path &to, Imported &n, std::stop_token stop) {
  fs::create_directories(to);
  for (auto it = fs::recursive_directory_iterator(from); it != fs::recursive_directory_iterator(); ++it) {
    if (stop.stop_requested()) throw std::runtime_error("Save import cancelled.");
    if (is_link(it->path())) {
      it.disable_recursion_pending();
      continue;
    }
    const auto target = to / it->path().lexically_relative(from);
    if (it->is_directory()) {
      fs::create_directories(target);
    } else if (it->is_regular_file()) {
      fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing);
      count(n, target);
    }
  }
}
// Each staged top-level entry replaces the current one, which moves to the
// backup first.
Imported publish(const Target &t, Imported n, std::string_view package) {
  if (!n.files) throw std::runtime_error("No save files were found to import.");
  fs::create_directories(t.root);
  for (const auto &entry : fs::directory_iterator(t.staging)) {
    const auto name = entry.path().filename();
    const auto current = t.root / name;
    std::error_code ec;
    if (fs::exists(fs::symlink_status(current, ec))) {
      fs::create_directories(t.backup);
      fs::rename(current, t.backup / name);
      n.backup = t.backup.parent_path();
    }
    fs::rename(entry.path(), current);
  }
  fs::remove_all(t.staging);
  log::info("saves", "Imported {} save files ({} bytes, {} skipped) of {}{}", n.files, n.bytes, n.skipped, package,
            n.backup.empty() ? std::string() : "; the replaced data is in " + paths::utf8(n.backup));
  return n;
}
// Copies the headset's save files into `folder`. A file the headset will not
// send is skipped, logged and named in `refused`.
void pull_saves(const import::Source &source, const fs::path &folder, Imported &n, std::string_view package,
                std::stop_token stop, std::vector<std::string> *refused = nullptr) {
  for (const auto &file : source.saves) {
    const auto path = folder / paths::from_utf8(file.name);
    if (!paths::within(folder, path)) throw std::runtime_error("A save's path escapes the game's storage.");
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    try {
      source.read(file, [&](std::span<const std::uint8_t> bytes) {
        out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
      }, stop);
    } catch (const std::exception &error) {
      if (stop.stop_requested()) throw;
      // Shared storage also holds SDK caches the headset will not let adb read
      // (Asgard's Wrath 2: Facebook's .fbns/MqttFizzCache.store), as in game imports.
      out.close();
      std::error_code ec;
      fs::remove(path, ec);
      ++n.skipped;
      if (refused) refused->push_back(file.name);
      log::warn("saves", "Skipped {} of {}: {}", file.name, package, error.what());
      continue;
    }
    out.close();
    if (!out || fs::file_size(path) != file.size)
      throw std::runtime_error("Copying " + file.name + " from the headset failed.");
    count(n, path);
  }
}
// What the runtime keeps beside a game's saves in its shared storage is not
// sent: Unity's il2cpp extraction (the game redoes it), Venture's stand-in for
// Meta's cloud storage, and the OBB aliases older versions put in files/.
bool exported(const fs::path &relative) {
  auto it = relative.begin();
  if (it == relative.end() || *it == "cache") return false;
  if (*it != "files" || ++it == relative.end()) return true;
  const auto next = std::next(it);
  if (*it == "il2cpp" || (*it == "cloud" && next != relative.end() && *next == "data")) return false;
  return next != relative.end() || it->extension() != ".obb";
}
// `relative` as the headset names it under Android/data/<package>.
std::string device_name(const fs::path &relative) {
  std::string out;
  for (const auto &part : relative) out += (out.empty() ? "" : "/") + paths::utf8(part);
  return out;
}
template <class Fill>
Imported staged(const Target &t, std::string_view package, Fill fill) {
  Imported n;
  try {
    fs::create_directories(t.staging);
    fill(n);
    return publish(t, n, package);
  } catch (...) {
    std::error_code ec;
    fs::remove_all(t.staging, ec);
    throw;
  }
}
}  // namespace

std::optional<Found> find_local(const fs::path &picked, std::string_view package) {
  if (!directory(picked) || !paths::safe_package(package)) return std::nullopt;
  const auto name = paths::from_utf8(package);
  if (const auto shared = picked / "Android" / "data" / name; directory(shared))
    return Found{shared, Area::external};
  if (directory(picked / name)) return found_in(picked / name);
  if (picked.filename() == name || private_storage(picked) || directory(picked / "files")) return found_in(picked);
  if (picked.filename() == "files") return Found{picked, Area::external, "files"};
  return std::nullopt;
}

Imported import_local(const fs::path &data, std::string_view key, std::string_view package, const Found &found,
                      std::stop_token stop) {
  const auto t = target(data, key, package, found.area);
  return staged(t, package, [&](Imported &n) {
    if (!found.entry.empty()) return copy_tree(found.folder, t.staging / paths::from_utf8(found.entry), n, stop);
    for (const auto &entry : fs::directory_iterator(found.folder)) {
      const auto name = entry.path().filename();
      if (!imported(found.area, name) || is_link(entry.path())) continue;
      if (entry.is_directory()) {
        copy_tree(entry.path(), t.staging / name, n, stop);
      } else if (entry.is_regular_file()) {
        fs::copy_file(entry.path(), t.staging / name);
        count(n, t.staging / name);
      }
    }
  });
}

Imported import_headset(const fs::path &data, std::string_view key, std::string_view package,
                        const android::adb::Client &client, std::stop_token stop) {
  const auto app = client.app(package, stop);
  if (!app) throw std::runtime_error("This game is not installed on the headset.");
  const auto source = import::open_device(client, *app);
  if (source->saves.empty())
    throw std::runtime_error("The headset has no save data for this game in Android/data/" + std::string(package) +
                             ". Saves a game keeps in its private storage cannot be read over USB.");
  const auto t = target(data, key, package, Area::external);
  return staged(t, package, [&](Imported &n) {
    pull_saves(*source, t.staging, n, package, stop);
    if (!n.files && n.skipped)
      throw std::runtime_error("The headset would not send any of this game's save files (see the log).");
  });
}

Exported export_headset(const fs::path &data, std::string_view key, std::string_view package,
                        const android::adb::Client &client, std::stop_token stop) {
  const auto game_root = account_data_dir(data, key, package);
  const auto from = game_root / "external-data";
  const auto backups = backups_dir(data, key, package);
  const auto app = client.app(package, stop);
  if (!app) throw std::runtime_error("This game is not installed on the headset.");
  const auto stamp = new_stamp(backups);
  const auto staging = backups / (stamp + ".export");
  const std::string name(package), dest = "/sdcard/Android/data/" + name,
                    stage = "/data/local/tmp/venture-saves/" + name, removals = stage + ".remove";
  const auto q = [](const std::string &path) { return *android::adb::quote(path); };
  Exported n;
  bool pushed = false;
  try {
    // The headset's saves first, so an export can be undone.
    std::vector<std::string> backed_up, refused;
    if (!app->saves.empty()) {
      const auto source = import::open_device(client, *app);
      n.backup = backups / stamp;
      Imported pulled;
      pull_saves(*source, n.backup / "headset-Android-data", pulled, package, stop, &refused);
      for (const auto &file : source->saves)
        if (std::ranges::find(refused, file.name) == refused.end()) backed_up.push_back(file.name);
      if (!pulled.files) {
        std::error_code ec;
        fs::remove_all(n.backup, ec);
        n.backup.clear();
      }
    }
    // Venture's copy, staged as hard links (copies where the volume has none).
    const auto tree = staging / "saves";
    std::set<std::string> sent;
    if (directory(from)) {
      for (auto it = fs::recursive_directory_iterator(from); it != fs::recursive_directory_iterator(); ++it) {
        if (stop.stop_requested()) throw std::runtime_error("Save export cancelled.");
        const auto relative = it->path().lexically_relative(from);
        if (is_link(it->path()) || !exported(relative)) {
          it.disable_recursion_pending();
          continue;
        }
        if (!it->is_regular_file()) continue;
        const auto file = device_name(relative);
        if (std::ranges::find(refused, file) != refused.end()) {
          ++n.kept;
          log::warn("saves", "Not sending {} of {}: the headset would not send its copy for the backup", file, package);
          continue;
        }
        if (!android::adb::quote(dest + "/" + file)) {
          log::warn("saves", "Not sending {} of {}: the headset's shell cannot name it", file, package);
          continue;
        }
        const auto target = tree / relative;
        fs::create_directories(target.parent_path());
        std::error_code ec;
        fs::create_hard_link(it->path(), target, ec);
        if (ec) fs::copy_file(it->path(), target);
        ++n.files;
        n.bytes += fs::file_size(target);
        sent.insert(file);
      }
    }
    // adb cannot write private storage, so its saves stay as the headset has them.
    n.has_private = private_storage(game_root);
    if (!n.files)
      throw std::runtime_error(n.has_private ? "This game keeps its saves in private storage, which can't be sent over USB."
                                             : "Venture has no save data for this game to send. Play it here first, or "
                                               "import its saves.");
    // Like an import, each folder sent replaces the headset's: backed-up files
    // Venture lacks in it go.
    std::set<std::string> folders;
    for (const auto &file : sent)
      if (auto slash = file.find('/'); slash != std::string::npos) folders.insert(file.substr(0, slash));
    std::string remove;
    for (const auto &file : backed_up)
      if (auto slash = file.find('/');
          slash != std::string::npos && folders.contains(file.substr(0, slash)) && !sent.contains(file) &&
          exported(paths::from_utf8(file)))
        remove += dest + "/" + file + "\n", ++n.removed;
    if (n.removed)
      log::info("saves", "Removing {} file{} from the headset that Venture's copy no longer has", n.removed,
                n.removed == 1 ? "" : "s");

    // Everything goes to a staging folder on the headset first, so a dropped
    // cable leaves its saves as they were.
    pushed = true;
    client.shell("rm -rf " + q(stage) + " " + q(removals) + " && mkdir -p /data/local/tmp/venture-saves", stop);
    client.push(tree, stage, stop);
    if (!remove.empty()) {
      std::ofstream(staging / "remove", std::ios::binary) << remove;
      client.push(staging / "remove", removals, stop);
    }
    std::istringstream listing(client.shell("find " + q(stage) + " -type f -exec stat -c '%s %n' {} +", stop));
    std::size_t files = 0;
    std::uint64_t bytes = 0;
    for (std::string line; std::getline(listing, line);) {
      unsigned long long size = 0;
      if (std::sscanf(line.c_str(), "%llu ", &size) == 1) ++files, bytes += size;
    }
    if (files != n.files || bytes != n.bytes)
      throw std::runtime_error("The headset did not receive every save file. Check the cable and retry.");
    // The game must not write its saves while they are replaced.
    const auto reply = client.shell(
        "am force-stop " + name + "; d=" + q(dest) + "; s=" + q(stage) + "; r=" + q(removals) +
            "; mkdir -p \"$d\" || exit 1; if [ -f \"$r\" ]; then while IFS= read -r f; do rm -f \"$f\"; done < \"$r\"; "
            "fi; cp -r \"$s/.\" \"$d/\" && rm -rf \"$s\" \"$r\" && echo sent",
        stop);
    if (reply.find("sent") == std::string::npos)
      throw std::runtime_error(
          n.backup.empty() ? std::string("The headset could not replace the game's saves.")
                           : "The headset could not replace the game's saves. Its previous saves are in " +
                                 paths::utf8(n.backup) + ".");
  } catch (...) {
    std::error_code ec;
    fs::remove_all(staging, ec);
    if (pushed) try {
        client.shell("rm -rf " + q(stage) + " " + q(removals), {});
      } catch (...) {
      }
    throw;
  }
  std::error_code ec;
  fs::remove_all(staging, ec);
  log::info("saves", "Exported {} save files ({} bytes, {} kept, {} removed) of {} to the headset{}{}", n.files, n.bytes,
            n.kept, n.removed, package,
            n.backup.empty() ? std::string() : "; its saves before are in " + paths::utf8(n.backup),
            n.has_private ? "; the game also has private storage (not sendable over USB)" : "");
  return n;
}

fs::path backups_dir(const fs::path &data, std::string_view key, std::string_view package) {
  account_data_dir(data, key, package);  // validates both names
  return data / "data" / "save-backups" / paths::from_utf8(key) / paths::from_utf8(package);
}
}  // namespace venture::saves
