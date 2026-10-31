#include "common/library/save_import.h"

#include "common/file_ops.h"
#include "common/library/import.h"
#include "common/log.h"
#include "common/saves_migration.h"

#include <chrono>
#include <format>
#include <fstream>

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

// The area's folder in the account's storage, and a staging and a backup
// folder for this import on the same volume.
struct Target {
  fs::path root, staging, backup;
};
Target target(const fs::path &data, std::string_view key, std::string_view package, Area area) {
  const auto game = account_data_dir(data, key, package);
  const auto backups = backups_dir(data, key, package);
  auto stamp =
      std::format("{:%Y-%m-%d %H-%M-%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
  std::error_code ec;
  for (int i = 2; fs::exists(backups / stamp, ec) || fs::exists(backups / (stamp + ".import"), ec); ++i)
    stamp = std::format("{} ({})", stamp.substr(0, 19), i);
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
    for (const auto &file : source->saves) {
      const auto path = t.staging / paths::from_utf8(file.name);
      if (!paths::within(t.staging, path)) throw std::runtime_error("A save's path escapes the game's storage.");
      fs::create_directories(path.parent_path());
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      try {
        source->read(file, [&](std::span<const std::uint8_t> bytes) {
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
        log::warn("saves", "Skipped {} of {}: {}", file.name, package, error.what());
        continue;
      }
      out.close();
      if (!out || fs::file_size(path) != file.size)
        throw std::runtime_error("Copying " + file.name + " from the headset failed.");
      count(n, path);
    }
    if (!n.files && n.skipped)
      throw std::runtime_error("The headset would not send any of this game's save files (see the log).");
  });
}

fs::path backups_dir(const fs::path &data, std::string_view key, std::string_view package) {
  account_data_dir(data, key, package);  // validates both names
  return data / "data" / "save-backups" / paths::from_utf8(key) / paths::from_utf8(package);
}
}  // namespace venture::saves
