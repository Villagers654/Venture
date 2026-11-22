// Mods for installed games (common/library/mods.h): mod packages, patched APKs,
// files a mod takes and the mod loader, dropped on the window or picked on a
// game's Mods tab. Copies and downloads run on the import worker; the runtime
// puts the enabled mods in place at each launch.
#include "launcher/core.h"

#include "common/android/manifest.h"
#include "common/http/client.h"
#include "common/library/mods.h"
#include "common/log.h"
#include "common/saves_migration.h"
#include "launcher/i18n/i18n.h"

#include <algorithm>

namespace venture::launcher {
using namespace i18n::markers;
namespace {
std::string lower_extension(const fs::path &file) {
  auto ext = paths::utf8(file.extension());
  std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return ext;
}
// Scotland2 is on the headset's storage, not in the APK its patcher made: a
// game whose APK carries it gets it from its release, as the patchers do.
std::string ensure_loader(const fs::path &mods, const fs::path &apk, bool offline, std::stop_token stop) {
  std::optional<mods::Loader> loader;
  try {
    loader = mods::apk_loader(apk);
  } catch (const std::exception &) {
  }
  if (loader != mods::Loader::scotland2 || !mods::loader_file(mods).empty()) return {};
  if (offline) return tr("Its mod loader (Scotland2) is not here yet: go online once, or drop libsl2.so on its page.");
  const auto file = mods / paths::from_utf8(".libsl2-" + random_account_key().substr(0, 12) + ".so");
  try {
    http::download_mod(mods::scotland2_url, file, stop);
    mods::set_loader_file(mods, file);
    std::error_code ec;
    fs::remove(file, ec);
    return {};
  } catch (const std::exception &e) {
    std::error_code ec;
    fs::remove(file, ec);
    log::warn("mods", "Could not download Scotland2: {}", e.what());
    return tr("Its mod loader (Scotland2) could not be downloaded: drop libsl2.so on its page.");
  }
}
// The installed APK; parse_plan accepts no plan without one.
fs::path apk_path(const library::Install &install) {
  auto apk = std::ranges::find(install.plan.files, std::string("apk"), &meta::File::kind);
  if (apk == install.plan.files.end()) throw std::runtime_error(tr("This installation has no APK."));
  return install.directory / paths::from_utf8(apk->name);
}
}  // namespace

fs::path Core::mods_folder(const library::Install &install) const {
  return mods_dir(data_, install.plan.package, install.plan.pinned ? paths::utf8(install.directory.filename()) : "");
}

std::vector<const library::Install *> Core::mod_targets(std::string_view package, std::string_view shown_id,
                                                        std::string_view version) const {
  std::vector<std::pair<int, const library::Install *>> ranked;
  for (const auto &i : installs_) {
    if (i.plan.package != package || !i.complete) continue;
    int rank = i.plan.pinned ? 0 : 1;
    if (!version.empty()) {
      // A pinned version's plan has no version name: its APK has.
      std::string name = i.plan.version;
      if (name.empty()) try {
          name = android::read_apk(apk_path(i)).version_name;
        } catch (const std::exception &) {
        }
      if (name == version) rank += 2;
    }
    if (!shown_id.empty() && i.id() == shown_id) rank += 4;
    ranked.emplace_back(rank, &i);
  }
  std::ranges::stable_sort(ranked, std::greater{}, &std::pair<int, const library::Install *>::first);
  std::vector<const library::Install *> out;
  for (const auto &r : ranked) out.push_back(r.second);
  return out;
}

bool Core::add_mod_file(const fs::path &file, std::string_view shown_id) {
  const auto ext = lower_extension(file);
  const library::Install *shown = shown_id.empty() ? nullptr : installed(shown_id);
  if (ext == ".qmod") {
    const auto manifest = mods::read_package(file);
    const library::Install *target = shown;
    if (!manifest.package.empty()) {
      const auto targets = mod_targets(manifest.package, shown_id, manifest.package_version);
      target = targets.empty() ? nullptr : targets.front();
    }
    if (!target) {
      if (manifest.package.empty()) throw std::runtime_error(tr("Open the game's page, then add this mod there."));
      throw std::runtime_error(trf("{0} is a mod for {1}, which is not installed.", manifest.name, manifest.package));
    }
    const auto package = target->plan.package;
    const auto apk = apk_path(*target);
    const auto folder = mods_folder(*target);
    mods_busy_.insert(package);
    notice_ = trf("Installing {0}\xE2\x80\xA6", manifest.name);
    import_worker_->submit([this, file, package, apk, folder,
                            offline = options_.offline](std::stop_token stop) -> Worker::Completion {
      std::string message, problem;
      try {
        mods::Fetch fetch;
        if (!offline) fetch = [](const std::string &url, const fs::path &to, std::stop_token s) { http::download_mod(url, to, s); };
        auto report = mods::install_with_dependencies(folder, package, file, fetch, stop);
        const auto &mod = report.installed.front().manifest;
        message = report.installed.size() > 1
                      ? trn("Installed {1} {2} with {0} mod it needs.", "Installed {1} {2} with {0} mods it needs.",
                            report.installed.size() - 1, mod.name, mod.version)
                      : trf("Installed {0} {1}.", mod.name, mod.version);
        if (!report.missing.empty()) {
          std::string names;
          for (const auto &m : report.missing) names += (names.empty() ? "" : ", ") + m;
          problem = trf("{0} also needs {1}: add their .qmod files.", mod.name, names);
        }
        if (auto note = ensure_loader(folder, apk, offline, stop); !note.empty()) problem += (problem.empty() ? "" : " ") + note;
      } catch (...) {
        problem = exception_text();
      }
      return [this, package, message, problem] {
        mods_busy_.erase(package);
        ++mods_generation_;
        if (!problem.empty()) error(message.empty() ? problem : message + " " + problem);
        else notice_ = message;
      };
    });
    return true;
  }
  if (ext == ".apk") {
    const auto manifest = android::read_apk(file);
    const library::Install *target = nullptr;
    for (const auto *i : mod_targets(manifest.package, shown_id, {}))
      if (std::int64_t(number(Json(i->plan.version_code))) == manifest.version_code) {
        target = i;
        break;
      }
    if (!target) return false;  // another version or a new game: the import page takes it
    if (auto r = running_.find(target->id()); r != running_.end() && !r->second.exit)
      throw std::runtime_error(tr("Close the game before replacing its APK."));
    const auto directory = target->directory;
    const auto package = target->plan.package;
    const auto apk = apk_path(*target);
    const auto folder = mods_folder(*target);
    // Also keeps the game from starting until the APK is in place.
    auto lock = save_lock(data_, package);
    mods_busy_.insert(package);
    notice_ = tr("Replacing the game's APK\xE2\x80\xA6");
    import_worker_->submit([this, file, directory, package, apk, folder, lock,
                            offline = options_.offline](std::stop_token stop) -> Worker::Completion {
      std::string message, problem;
      try {
        if (!mods::replace_apk(directory, file)) {
          message = tr("This APK is already installed.");
        } else {
          std::optional<mods::Loader> loader;
          try {
            loader = mods::apk_loader(apk);
          } catch (const std::exception &) {
          }
          message = loader ? trf("Installed the patched APK ({0} mod loader). Saves and settings stay.", mods::loader_name(*loader))
                           : tr("Installed the APK. Saves and settings stay.");
          problem = ensure_loader(folder, apk, offline, stop);
        }
      } catch (...) {
        problem = exception_text();
      }
      return [this, package, message, problem] {
        mods_busy_.erase(package);
        ++mods_generation_;
        rescan();  // the install's manifest records the new APK
        if (!problem.empty()) error(message.empty() ? problem : message + " " + problem);
        else notice_ = message;
      };
    });
    return true;
  }
  if (!shown) return false;
  if (ext == ".obb") {
    // The expansion file of the same name, else the game's only one.
    const auto names = mods::obb_names(shown->directory);
    std::string name = paths::utf8(file.filename());
    if (std::ranges::find(names, name) == names.end()) {
      if (names.size() != 1)
        throw std::runtime_error(names.empty() ? tr("This game has no expansion file (OBB) to replace.")
                                               : tr("Choose which expansion file to replace on the game's Mods tab."));
      name = names.front();
    }
    replace_obb(shown->id(), name, file);
    return true;
  }
  const auto folder = mods_folder(*shown);
  const bool loader = lower_extension(file) == ".so" && file.filename() == "libsl2.so";
  if (!loader && !mods::takes_item(folder, file)) return false;
  import_worker_->submit([this, file, folder, loader](std::stop_token) -> Worker::Completion {
    std::string message, problem;
    try {
      if (loader) {
        mods::set_loader_file(folder, file);
        message = tr("Added the Scotland2 mod loader.");
      } else {
        auto item = mods::add_item(folder, file);
        message = trf("Added {0} for its mods.", item.name);
      }
    } catch (...) {
      problem = exception_text();
    }
    return [this, message, problem] {
      ++mods_generation_;
      if (!problem.empty()) error(problem);
      else notice_ = message;
    };
  });
  return true;
}

void Core::set_mod_enabled(std::string_view app_id, const std::string &id, bool enabled) {
  const auto *install = installed(app_id);
  if (!install) throw std::runtime_error(tr("Install this game first."));
  mods::set_enabled(mods_folder(*install), id, enabled);
  ++mods_generation_;
}

void Core::remove_mod(std::string_view app_id, const std::string &id) {
  const auto *install = installed(app_id);
  if (!install) throw std::runtime_error(tr("Install this game first."));
  mods::remove(mods_folder(*install), id);
  ++mods_generation_;
}

void Core::remove_mod_item(std::string_view app_id, const std::string &extension, const std::string &name) {
  const auto *install = installed(app_id);
  if (!install) throw std::runtime_error(tr("Install this game first."));
  mods::remove_item(mods_folder(*install), extension, name);
  ++mods_generation_;
}

void Core::replace_obb(std::string_view app_id, const std::string &name, const fs::path &file) {
  const auto *install = installed(app_id, false);
  if (!install) throw std::runtime_error(tr("Install this game first."));
  if (auto r = running_.find(std::string(app_id)); r != running_.end() && !r->second.exit)
    throw std::runtime_error(tr("Close the game before replacing its expansion file."));
  const auto directory = install->directory;
  const auto package = install->plan.package;
  auto lock = save_lock(data_, package);
  mods_busy_.insert(package);
  notice_ = trf("Replacing {0}\xE2\x80\xA6", name);
  import_worker_->submit([this, directory, package, name, file, lock](std::stop_token) -> Worker::Completion {
    std::string problem;
    try {
      mods::replace_obb(directory, name, file);
    } catch (...) {
      problem = exception_text();
    }
    return [this, package, name, problem] {
      mods_busy_.erase(package);
      ++mods_generation_;
      rescan();
      if (!problem.empty()) error(problem);
      else notice_ = trf("Replaced {0}. Restore downloads the store's file again.", name);
    };
  });
}

void Core::restore_obb(std::string_view app_id, const std::string &name) {
  const auto *install = installed(app_id, false);
  if (!install) throw std::runtime_error(tr("Install this game first."));
  if (install->plan.local) throw std::runtime_error(tr("This game was imported: import its expansion file again."));
  if (auto r = running_.find(std::string(app_id)); r != running_.end() && !r->second.exit)
    throw std::runtime_error(tr("Close the game before replacing its expansion file."));
  if (options_.offline || !session_) throw std::runtime_error(tr("Sign in and go online to download the store's file."));
  const auto names = mods::obb_names(install->directory);
  if (std::ranges::find(names, name) == names.end()) throw std::runtime_error(tr("Install this game first."));
  auto *g = game(app_id);
  if (!g) throw std::runtime_error(tr("Install this game first."));
  const auto copy = *g;
  {
    auto lock = save_lock(data_, install->plan.package);
    fs::remove(install->directory / paths::from_utf8(name));
  }
  log::info("mods", "Removed the replaced {} of {}; downloading the store's file", name, install->plan.package);
  ++mods_generation_;
  install_for_owner(copy);
  rescan();
}

void Core::restore_original_apk(std::string_view app_id) {
  const auto *install = installed(app_id);
  if (!install) throw std::runtime_error(tr("Install this game first."));
  if (auto r = running_.find(std::string(app_id)); r != running_.end() && !r->second.exit)
    throw std::runtime_error(tr("Close the game before replacing its APK."));
  auto lock = save_lock(data_, install->plan.package);
  mods::restore_apk(install->directory);
  ++mods_generation_;
  rescan();
  notice_ = tr("The original APK is back. Saves and settings stay.");
}
}  // namespace venture::launcher
