#include "common/saves_migration.h"
#include "common/file_ops.h"
#include "common/log.h"
#include "common/meta/accounts.h"
#ifdef _WIN32
#include "common/platform/win32/native_path.h"
#endif
#include <algorithm>

namespace venture {
fs::path account_data_dir(const fs::path &data, std::string_view key,
                          std::string_view package) {
  if ((key != "local" && !meta::valid_account_key(key)) ||
      !paths::safe_package(package))
    throw std::runtime_error("Invalid account save path.");
  auto root = data / "data/users";
  auto path = root / paths::from_utf8(key) / paths::from_utf8(package);
  if (!paths::within(root, path) || is_link(root.parent_path()) ||
      is_link(root) || is_link(path.parent_path()) || is_link(path))
    throw std::runtime_error(
        "Account save path is linked or escapes its root.");
  return path;
}
SaveMigration migrate_saves(const fs::path &data, std::string_view primary,
                            bool adopt_local) {
  const std::string key = primary.empty() ? "local" : std::string(primary);
  if (key != "local" && !meta::valid_account_key(key))
    throw std::runtime_error("Invalid primary account key.");
  const auto legacy = data / "data", users = legacy / "users";
  if (is_link(legacy) || is_link(users))
    throw std::runtime_error("Cannot migrate saves through a linked root.");
  FileLock lock(users / ".migration.lock");
  auto journal_file = users / "migration.json";
  Json rows = Json::array();
  if (fs::exists(journal_file)) {
    auto journal = paths::read_json(journal_file);
    if (number(journal, "version") != 1 || !field(journal, "moves").is_array())
      throw std::runtime_error(
          "Save migration journal is unreadable; saves have been retained.");
    rows = journal["moves"];
  }
  SaveMigration result;
  auto move_from = [&](const fs::path &source_root, bool local) {
    if (!fs::exists(source_root) || is_link(source_root))
      return;
    for (const auto &entry : fs::directory_iterator(source_root)) {
      auto package = paths::utf8(entry.path().filename());
      if (!paths::safe_package(package) || is_link(entry.path()) ||
          !entry.is_directory())
        continue;
      auto target = account_data_dir(data, key, package);
      fs::create_directories(target.parent_path());
      std::string state;
      int error = 0;
      if (fs::exists(target)) {
        state = "conflict";
        ++result.conflicts;
      } else if (auto ec = move_directory(entry.path(), target)) {
        state = "deferred";
        error = ec.value();
        ++result.deferred;
      } else {
        state = local ? "adopted-local" : "moved";
        ++result.moved;
      }
      auto source = local ? "local" : "legacy";
      auto row = std::ranges::find_if(rows, [&](const auto &j) {
        return str(j, "package") == package && str(j, "source") == source &&
               str(j, "account") == key;
      });
      Json value{{"package", package}, {"source", source},
                 {"account", key},     {"state", state},
                 {"error", error},     {"updatedAt", paths::timestamp()}};
      if (row == rows.end())
        rows.push_back(value);
      else
        *row = value;
      // Persist each move: an interrupted pass resumes from actual directory
      // existence.
      paths::atomic_write(journal_file,
                          Json{{"version", 1}, {"moves", rows}}.dump(2));
    }
  };
  move_from(legacy, false);
  if (adopt_local && key != "local")
    move_from(users / "local", true);
  if (result.moved || result.conflicts || result.deferred)
    log::info("accounts",
              "Save migration moved={} conflicts={} deferred={}",
              result.moved, result.conflicts, result.deferred);
  return result;
}
namespace {
fs::path obb_copy_sidecar(const fs::path& view) {
  return view.parent_path() / (view.filename().string() + ".copies.json");
}
Json identity_record(const FileIdentity& id) {
  return Json::array({id.volume, id.file, id.size, id.write_seconds, id.write_nanoseconds});
}
void publish_obb_file_locked(const fs::path& source, const fs::path& view) {
  const auto target = view / source.filename();
  if (!fs::is_regular_file(source)) {
    remove_tree(view, target);
    return;
  }
  if (!paths::within(source.parent_path(), source)) throw std::runtime_error("Linked OBB source escapes installation");
  std::error_code ec;
  if (!is_link(target) && fs::equivalent(source, target, ec) && !ec) return;
  const auto source_id = file_identity(source), target_id = file_identity(target);
  const auto sidecar = obb_copy_sidecar(view);
  const auto name = paths::utf8(source.filename());
  Json copies = Json::object();
  if (fs::exists(sidecar) && !is_link(sidecar)) {
    // A missing or malformed cache record repairs the copy, never trusts it.
    copies = Json::parse(paths::read(sidecar), nullptr, false);
    if (!copies.is_object()) copies = Json::object();
  }
  // Bind the record to the destination too: backups/copies of a data directory
  // must repair their view even if they also copied this sidecar.
  if (source_id && target_id &&
      field(field(copies, name), "source") == identity_record(*source_id) &&
      field(field(copies, name), "copy") == identity_record(*target_id)) return;
  // Publish outside the enumerated directory so temporary files never leak
  // into getObbDir().list() or native getdents.
  auto temporary = view.parent_path() / (".obb-" + random_account_key());
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code ignored; fs::remove(path, ignored); } } cleanup{temporary};
  fs::create_hard_link(source, temporary, ec);
  const bool copied = bool(ec);
  if (copied) {
    log::info("obb", "Hard links unavailable; copying expansion/asset {}", paths::utf8(source.filename()));
    fs::copy_file(source, temporary);
  }
  if (fs::is_directory(target) || is_link(target)) remove_tree(view, target);
  paths::replace_file(temporary, target);
  // Do not cache a copy if the source changed while it was being read.
  if (copied && source_id && source_id == file_identity(source)) {
    if (const auto copy_id = file_identity(target)) {
      copies[name] = Json{{"source", identity_record(*source_id)}, {"copy", identity_record(*copy_id)}};
      paths::atomic_write(sidecar, copies.dump());
    }
  }
}
}
fs::path mods_dir(const fs::path& data, std::string_view package) {
  if (!paths::safe_package(package)) throw std::runtime_error("Invalid game package.");
  return data / "mods" / paths::from_utf8(package);
}
void sync_obb_view(const fs::path& install, const fs::path& raw_view,
                   std::span<const std::string> names, const fs::path& mods) {
  if (fs::weakly_canonical(install) == fs::weakly_canonical(raw_view))
    throw std::runtime_error("OBB view must be separate from installation");
#ifdef _WIN32
  // Per-account expansion views repeat the package name and can exceed MAX_PATH.
  const fs::path view{platform::native_path(raw_view)};
#else
  const auto& view = raw_view;
#endif
  for (const auto& name : names)
    if (!paths::safe_name(name)) throw std::runtime_error("Invalid OBB view filename");
  fs::create_directories(view.parent_path());
  FileLock lock(view.parent_path() / (view.filename().string() + ".lock"));
  if (is_link(view)) throw std::runtime_error("Linked OBB view directory");
  fs::create_directories(view);
  // Remove unknown and no-longer-installed entries, including plain copies
  // left by backups. This directory is exclusively a view, never save storage.
  for (const auto& entry : fs::directory_iterator(view)) {
    const auto name = paths::utf8(entry.path().filename());
    if (std::ranges::find(names, name) == names.end() ||
        !fs::is_regular_file(install / entry.path().filename()))
      remove_tree(view, entry.path());
  }
  for (const auto& name : names) {
    auto source = install / paths::from_utf8(name);
    std::error_code ec;
    if (!mods.empty() && fs::is_regular_file(source, ec)) {
      const auto replacement = mods / paths::from_utf8(name);
      if (paths::within(mods, replacement) && !is_link(replacement) && fs::is_regular_file(replacement, ec)) {
        log::info("obb", "Using the replacement {} from the mods folder ({} bytes)", name, fs::file_size(replacement, ec));
        source = replacement;
      }
    }
    publish_obb_file_locked(source, view);
  }
}
fs::path obb_view_for(const fs::path& install, const fs::path& view, std::string_view account) {
  if (account != "local" && !meta::valid_account_key(account))
    throw std::runtime_error("Invalid account for the OBB view.");
  const auto parent = view.parent_path();
  fs::create_directories(parent);
  const auto probe = parent / (".link-" + random_account_key());
  std::error_code ec;
  fs::create_hard_link(install / "plan.json", probe, ec);
  if (!ec) {
    fs::remove(probe, ec);
    if (is_link(view)) remove_tree(parent, view);  // the games folder is on this drive again
    return view;
  }
  if (fs::exists(view, ec) && !is_link(view)) {
    log::info("obb", "Games folder is on another drive; removing the copied view at {}", paths::utf8(view));
    remove_tree(parent, view);
  }
  if (const auto sidecar = obb_copy_sidecar(view); fs::exists(sidecar, ec)) remove_tree(parent, sidecar);
  const auto beside = install / ".obb" / paths::from_utf8(account);
  // Java code also looks for expansion files under the external storage root
  // rather than Context.getObbDir() (Google's downloader library in Unreal's
  // GameActivity: "No OBB found"), so the usual place links to this view.
  fs::create_directories(beside);
  try {
    if (is_link(view)) remove_tree(parent, view);
    link_directory(view, beside);
  } catch (const std::exception& e) {
    log::info("obb", "Could not link {} to the expansion files beside the install: {}", paths::utf8(view), e.what());
  }
  return beside;
}
void publish_obb_file(const fs::path& source, const fs::path& raw_view) {
#ifdef _WIN32
  const fs::path view{platform::native_path(raw_view)};
#else
  const auto& view = raw_view;
#endif
  if (!paths::safe_name(paths::utf8(source.filename())) || is_link(view))
    throw std::runtime_error("Invalid OBB publication path");
  fs::create_directories(view);
  FileLock lock(view.parent_path() / (view.filename().string() + ".lock"));
  publish_obb_file_locked(source, view);
}

void remove_cached_obb_aliases(const fs::path& data, std::string_view package) {
  if (!paths::safe_package(package))
    throw std::runtime_error("Invalid game package.");
  const auto cache = data / "cache" / paths::from_utf8(package);
  if (fs::exists(cache) && !is_link(cache) && paths::within(data, cache))
    for (const auto& version : fs::directory_iterator(cache)) {
      const auto view = version.path() / "obb";
      if (version.is_directory() && !is_link(version.path()) && fs::exists(view))
        remove_tree(cache, view);
    }
}
void remove_obb_aliases(const fs::path &data, std::string_view package,
                        const fs::path &install) {
  remove_cached_obb_aliases(data, package);
  auto users = data / "data/users";
  if (!fs::exists(users) || is_link(users))
    return;
  for (const auto &account : fs::directory_iterator(users)) {
    auto key = paths::utf8(account.path().filename());
    if ((key != "local" && !meta::valid_account_key(key)) ||
        is_link(account.path()) || !account.is_directory())
      continue;
    auto root = account_data_dir(data, key, package);
    const auto view = root / "external/Android/obb" / paths::from_utf8(package);
    if (fs::exists(view) && paths::within(root, view)) remove_tree(root, view);
    if (paths::within(root, obb_copy_sidecar(view))) remove_tree(root, obb_copy_sidecar(view));
    // Legacy Java OBB aliases are under external-data/files (some engines open
    // them there).
    for (auto aliases :
         {root / "external-data/files"}) {
      if (!fs::exists(aliases) || !paths::within(root, aliases) ||
          is_link(aliases))
        continue;
      for (const auto &file : fs::directory_iterator(install)) {
        if (file.path().extension() != ".obb" || is_link(file.path()) ||
            !file.is_regular_file())
          continue;
        auto alias = aliases / file.path().filename();
        std::error_code ec;
        if (!is_link(alias) && fs::equivalent(alias, file.path(), ec) && !ec)
          fs::remove(alias);
      }
    }
  }
}
} // namespace venture
