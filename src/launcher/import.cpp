// Importing games installed elsewhere: apps on a Quest over adb, or an APK
// with its data folder picked or dropped on the window. Scans run on their
// own worker; imports run in the download queue (common/library/import.h).
#include "launcher/core.h"

#include "common/library/import.h"
#include "common/log.h"


namespace venture::launcher {
Core::ImportItem Core::import_item(const import::Source &source) {
  ImportItem item;
  item.spec = source.spec();
  item.package = source.manifest.package;
  item.label = source.manifest.label;
  item.version_name = source.manifest.version_name;
  item.version_code = source.manifest.version_code;
  item.location = source.describe();
  item.bytes = source.bytes();
  item.files = 1 + source.data.size();
  item.saves = source.saves.size();
  item.save_bytes = source.save_bytes();
  item.skipped = source.skipped.size();
  return item;
}

fs::path Core::adb_path() const {
  std::lock_guard lock(adb_mutex_);
  return android::adb::locate(adb_setting_).value_or(fs::path{});
}

void Core::scan_headset() {
  if (import_device_.busy) return;
  import_device_.busy = true;
  import_device_.error.clear();
  auto configured = [&] {
    std::lock_guard lock(adb_mutex_);
    return adb_setting_;
  }();
  auto serial = import_serial_;
  import_worker_->submit([this, configured, serial](std::stop_token stop) -> Worker::Completion {
    std::optional<fs::path> adb;
    std::vector<android::adb::Device> devices;
    std::vector<ImportItem> items;
    std::string error, chosen;
    try {
      adb = android::adb::locate(configured);
      if (!adb) {
        error = configured.empty()
                    ? "Android platform-tools (adb) were not found. Install them, or set adb's location in Settings."
                    : "adb was not found at the location set in Settings.";
      } else {
        android::adb::Client list(*adb);
        devices = list.devices(stop);
        auto ready = std::ranges::find_if(devices, [&](auto &d) { return d.ready() && d.serial == serial; });
        if (ready == devices.end()) ready = std::ranges::find_if(devices, &android::adb::Device::ready);
        if (devices.empty()) {
          error = "No headset found. Connect it with a USB cable (or turn on Wi-Fi debugging) and enable "
                  "developer mode in the Meta Horizon app.";
        } else if (ready == devices.end()) {
          error = android::adb::state_message(devices.front());
        } else {
          chosen = ready->serial;
          android::adb::Client client(*adb, chosen);
          for (auto &app : client.apps(stop)) items.push_back(import_item(*import::open_device(client, app)));
          if (items.empty()) error = "No games are installed on this headset.";
        }
      }
    } catch (...) {
      error = exception_text();
    }
    return [this, adb, devices, items = std::move(items), error, chosen]() mutable {
      import_device_.busy = false;
      import_adb_ = adb;
      import_devices_ = devices;
      if (!chosen.empty()) import_serial_ = chosen;
      import_device_.error = error;
      // Keep the user's ticks across rescans of the same apps.
      for (auto &item : items)
        for (auto &old : import_device_.items)
          if (old.package == item.package && old.version_code == item.version_code) item.selected = old.selected;
      import_device_.items = std::move(items);
    };
  });
}

void Core::connect_headset() {
  if (import_device_.busy) return;
  import_device_.busy = true;
  import_device_.error.clear();
  auto configured = [&] {
    std::lock_guard lock(adb_mutex_);
    return adb_setting_;
  }();
  auto address = import_address_;
  import_worker_->submit([this, configured, address](std::stop_token stop) -> Worker::Completion {
    std::string serial, error;
    try {
      auto adb = android::adb::locate(configured);
      if (!adb)
        throw std::runtime_error("Android platform-tools (adb) were not found. Install them, or set adb's "
                                 "location in Settings.");
      serial = android::adb::Client(*adb).connect(address, stop);
    } catch (...) {
      error = exception_text();
    }
    return [this, serial, error] {
      import_device_.busy = false;
      import_device_.error = error;
      if (serial.empty()) return;
      import_serial_ = serial;
      scan_headset();
    };
  });
}

void Core::scan_local(fs::path picked) {
  on_import_scan();
  import_local_.busy = true;
  import_local_.error.clear();
  import_worker_->submit([this, picked](std::stop_token stop) -> Worker::Completion {
    std::vector<ImportItem> items;
    std::string error;
    try {
      for (auto &source : import::open_local(picked, stop)) items.push_back(import_item(*source));
    } catch (...) {
      error = exception_text();
    }
    return [this, items = std::move(items), error]() mutable {
      import_local_.busy = false;
      import_local_.error = error;
      // A new pick adds to what is listed; the same app found again replaces
      // its row.
      for (auto &item : items) {
        std::erase_if(import_local_.items,
                      [&](auto &old) { return old.package == item.package && old.version_code == item.version_code; });
        item.selected = import_match(item).importable;
        import_local_.items.push_back(std::move(item));
      }
    };
  });
}

Core::ImportMatch Core::import_match(const ImportItem &item) const {
  // Any app imports; the Meta game it is, when known, names it and lets an
  // owning account fill in files the source lacks.
  ImportMatch m;
  for (const auto &g : games_)
    if (g.package == item.package && !g.system) {
      m.game = &g;
      if (g.owned) break;
    }
  // Installed games know their package even when the library cache predates
  // package names.
  const library::Install *installed = nullptr;
  for (const auto &i : installs_)
    if (i.plan.package == item.package) {
      installed = &i;
      if (!m.game)
        for (const auto &g : games_)
          if (g.id == i.plan.app_id) m.game = &g;
    }
  if (installed) {
    const auto code = std::int64_t(number(Json(installed->plan.version_code)));
    if (installed->complete && code == item.version_code) {
      m.status = "Installed";
      return m;
    }
    if (installed->complete && code > item.version_code) {
      m.status = "A newer version is installed";
      return m;
    }
    m.status = installed->complete ? "Updates the installed version" : "Completes the installation";
  } else {
    m.status = "Ready to import";
  }
  m.importable = true;
  return m;
}

void Core::start_import(bool device) {
  auto &scan = device ? import_device_ : import_local_;
  std::vector<std::string> accounts;
  size_t queued = 0;
  for (auto &item : scan.items) {
    if (!item.selected) continue;
    auto match = import_match(item);
    if (!match.importable) continue;
    // A signed-in owner, else the active account, lets Meta supply missing
    // files; with neither the import is local.
    const auto id = match.game ? match.game->id : item.package;
    std::string key = match.game ? owner_for(id) : std::string{};
    auto usable = [&](const std::string &k) {
      const auto *a = k.empty() ? nullptr : account(k);
      return a && a->signed_in && !a->needs_reauth;
    };
    if (!usable(key)) key = usable(account_key_) ? account_key_ : std::string{};
    decltype(vault_.load(key)) session;
    if (!key.empty()) session = vault_.load(key);
    if (!session) key = "local";
    downloads_->enqueue_import(id, match.game ? match.game->name : item.label.empty() ? item.package : item.label,
                               settings_.games,
                               session ? session->access_token : std::string{}, key, item.spec, item.package);
    accounts.push_back(key);
    item.selected = false;
    ++queued;
  }
  if (!queued) throw std::runtime_error("Select a game to import.");
  // Downloads lists the active account's jobs (and local ones); show these
  // when one other account imports them all.
  if (std::ranges::all_of(accounts, [&](auto &k) { return k == accounts.front(); }) &&
      accounts.front() != "local" && accounts.front() != account_key_)
    switch_account(accounts.front());
  notice_ = queued == 1 ? "Importing 1 game." : std::format("Importing {} games.", queued);
  on_install_queued();
}
}  // namespace venture::launcher
