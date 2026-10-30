#include "common/download/manager.h"

#include "common/download/asset_files.h"
#include "common/download/unity_bundle.h"
#include "common/digest.h"
#include "common/library/library.h"
#include "common/log.h"
#include "common/meta/accounts.h"
#include "common/saves_migration.h"
#include <algorithm>
#include <format>
#include <fstream>
namespace venture::download {
namespace {
bool valid_source(std::string_view source) {
  return source.starts_with("path:") ? source.size() > 5
         : source.starts_with("adb:") ? source.size() > 4 && source.size() < 260
                                      : false;
}
bool same(const Job &a, const Job &b) {
  return a.app_id == b.app_id && a.account == b.account &&
         a.binary_id == b.binary_id && a.sku == b.sku &&
         a.asset_name == b.asset_name;
}
} // namespace
const char *label(State s) {
  switch (s) {
  case State::queued:
    return "Queued";
  case State::checking:
    return "Checking ownership and build";
  case State::downloading:
    return "Downloading";
  case State::paused:
    return "Paused";
  case State::failed:
    return "Needs attention";
  case State::complete:
    return "Installed";
  }
  return "";
}
std::optional<Job> base_job(const std::vector<Job> &jobs, std::string_view app) {
  for (const auto &job : jobs)
    if (job.app_id == app && job.sku.empty() && job.asset_id.empty())
      return job;
  return std::nullopt;
}
Manager::Manager(fs::path data, std::function<void()> wake, Services services, bool persistent)
    : data_(data), file_(persistent ? data / "downloads.json" : fs::path{}), wake_(std::move(wake)),
      services_(std::move(services)) {
  try {
    auto index = meta::AccountVault(data).index();
    active_account_ = index.active;
    auto saved = !file_.empty() && fs::exists(file_) ? paths::read_json(file_) : Json::object();
    if (field(saved, "jobs").is_array())
      for (auto &j : saved["jobs"]) {
        auto id = str(j, "appId");
        auto root = paths::from_utf8(str(j, "root"));
        // An import Meta does not know is keyed by its package.
        const bool import = !str(j, "source").empty();
        if ((!digits(id, 5) && !(import && id == str(j, "package"))) ||
            !root.is_absolute())
          continue;
        auto e = std::make_shared<Entry>();
        e->job.app_id = id;
        e->job.name = str(j, "name");
        e->job.root = root;
        e->job.sku = str(j, "sku");
        e->job.binary_id = str(j, "binaryId");
        e->job.asset_id = str(j, "assetId");
        e->job.asset_name = str(j, "assetName");
        e->job.source = str(j, "source");
        e->job.package = str(j, "package");
        if (!e->job.source.empty() && (!valid_source(e->job.source) ||
                                       !paths::safe_package(e->job.package)))
          continue;
        e->job.account =
            number(saved, "version") == 1 ? index.primary : str(j, "account");
        if ((!e->job.sku.empty() &&
             (!meta::valid_sku(e->job.sku) || !digits(e->job.binary_id))) ||
            (!e->job.asset_id.empty() &&
             (!digits(e->job.asset_id) || !digits(e->job.binary_id) ||
              !paths::safe_name(e->job.asset_name))) ||
            (!e->job.account.empty() &&
             !(import && e->job.account == "local") &&
             !meta::valid_account_key(e->job.account)))
          continue;
        e->job.state = State::paused;
        e->job.generation = next_generation_++;
        jobs_.push_back(e);
      }
  } catch (...) {
    log::warn("downloads", "Could not read the previous queue.");
  }
  save_locked();
  for (unsigned i = 0; i < max_parallel; ++i)
    threads_.emplace_back([this](std::stop_token s) { run(s); });
}
Manager::~Manager() {
  cancel_all();
  for (auto &thread : threads_)
    thread.request_stop();
  cv_.notify_all();
  threads_.clear();
}
void Manager::set_parallel(unsigned jobs) {
  {
    std::lock_guard lock(mutex_);
    parallel_ = std::clamp(jobs, 1u, max_parallel);
  }
  cv_.notify_all();
}
void Manager::save_locked() {
  if (file_.empty()) return;
  Json j = Json::array();
  for (auto &e : jobs_)
    if (e->job.state != State::complete && !e->removed)
      j.push_back({{"appId", e->job.app_id},
                   {"name", e->job.name},
                   {"account", e->job.account},
                   {"root", paths::utf8(e->job.root)},
                   {"sku", e->job.sku},
                   {"binaryId", e->job.binary_id},
                   {"assetId", e->job.asset_id},
                   {"assetName", e->job.asset_name},
                   {"source", e->job.source},
                   {"package", e->job.package}});
  try {
    paths::atomic_write(file_, Json{{"version", 2}, {"jobs", j}}.dump(2));
  } catch (...) {
    log::warn(
        "downloads",
        "Could not save download queue. Existing partial files are retained.");
  }
}
void Manager::enqueue(std::string app, std::string name, fs::path root,
                      std::string token) {
  enqueue(std::move(app), std::move(name), std::move(root), std::move(token),
          active_key());
}
void Manager::enqueue(std::string app, std::string name, fs::path root,
                      std::string token, std::string account) {
  enqueue_job(std::move(app), std::move(name), std::move(root),
              std::move(token), std::move(account), {}, {});
}
void Manager::enqueue_addon(std::string app, std::string name, fs::path root,
                            std::string binary, std::string sku,
                            std::string token) {
  if (!digits(binary) || !meta::valid_sku(sku))
    throw std::runtime_error("Invalid add-on download.");
  enqueue_job(std::move(app), std::move(name), std::move(root),
              std::move(token), active_key(), std::move(binary), std::move(sku));
}
std::string Manager::active_key() const {
  std::lock_guard lock(mutex_);
  return active_account_;
}
void Manager::enqueue_asset(std::string app, fs::path root, std::string binary,
                            std::string id, std::string filename,
                            std::string token, std::string account) {
  if (!digits(binary) || !digits(id) || !paths::safe_name(filename))
    throw std::runtime_error("Invalid asset download.");
  auto display = filename;
  enqueue_job(std::move(app), std::move(display), std::move(root),
              std::move(token), std::move(account), std::move(binary), {},
              std::move(id), std::move(filename));
}
void Manager::enqueue_import(std::string app, std::string name, fs::path root,
                             std::string token, std::string account,
                             std::string source, std::string package) {
  if (!valid_source(source) || !paths::safe_package(package))
    throw std::runtime_error("Invalid import source.");
  enqueue_job(std::move(app), std::move(name), std::move(root),
              std::move(token), std::move(account), {}, {}, {}, {},
              std::move(source), std::move(package));
}
bool Manager::cancel_asset(std::string_view app, std::string_view binary,
                           std::string_view filename,
                           std::string_view account) {
  std::lock_guard lock(mutex_);
  bool cancelled = false;
  for (auto& e : jobs_)
    if (e->job.app_id == app && e->job.binary_id == binary &&
        e->job.account == account && e->job.asset_name == filename &&
        (e->job.state == State::queued || e->job.state == State::checking ||
         e->job.state == State::downloading)) {
      e->cancel.request_stop();
      cancelled = true;
      if (e->job.state == State::queued) {
        e->job.state = State::paused;
        e->token.clear();
      }
    }
  save_locked();
  wake_();
  return cancelled;
}
bool Manager::delete_asset(const fs::path& install, std::string_view app,
                           std::string_view binary, std::string_view filename) {
  std::lock_guard lock(mutex_);
  for (auto& e : jobs_)
    if (e->job.app_id == app &&
        (e->job.state == State::queued || e->job.state == State::checking ||
         e->job.state == State::downloading))
      throw std::runtime_error("Pause downloads before deleting an asset.");
  bool removed = remove_asset(install, app, binary, filename);
  std::erase_if(jobs_, [&](auto& e) {
    return e->job.app_id == app && e->job.binary_id == binary &&
           e->job.asset_name == filename;
  });
  save_locked();
  wake_();
  return removed;
}
void Manager::enqueue_job(std::string app, std::string name, fs::path root,
                          std::string token, std::string account,
                          std::string binary, std::string sku,
                          std::string asset_id, std::string asset_name,
                          std::string source, std::string package) {
  // Imports need no account ("local") or sign-in: Meta only supplies the
  // files a source lacks, when an account owns the game.
  const bool import = !source.empty();
  if (!(import && account == "local") && !meta::valid_account_key(account))
    throw std::runtime_error("Select a signed-in account before downloading.");
  if (token.empty() && !import)
    throw std::runtime_error("Sign in before resuming downloads.");
  if ((!digits(app, 5) && !(import && app == package)) || !root.is_absolute())
    throw std::runtime_error("Invalid download destination.");
  std::lock_guard lock(mutex_);
  for (auto &e : jobs_)
    if (e->job.app_id == app && e->job.account == account &&
        e->job.binary_id == binary && e->job.sku == sku &&
        e->job.asset_name == asset_name) {
      if (e->removed)
        throw std::runtime_error(
            "The cancelled download is still stopping. Try again shortly.");
      if (e->job.state == State::queued || e->job.state == State::checking ||
          e->job.state == State::downloading)
        return;
      e->job.state = State::queued;
      e->job.generation = next_generation_++;
      e->job.error.clear();
      e->job.done = e->job.total = 0;
      e->job.bytes_per_second = 0;
      e->job.root = std::move(root);
      e->job.name = std::move(name);
      // A base download and an import of the same game are one job; the
      // latest request decides where its files come from.
      e->job.source = std::move(source);
      e->job.package = std::move(package);
      e->token = std::move(token);
      e->cancel = std::stop_source{};
      save_locked();
      cv_.notify_all();
      wake_();
      return;
    }
  auto e = std::make_shared<Entry>();
  e->job.app_id = std::move(app);
  e->job.name = std::move(name);
  e->job.root = std::move(root);
  e->job.account = std::move(account);
  e->job.binary_id = std::move(binary);
  e->job.sku = std::move(sku);
  e->job.asset_id = std::move(asset_id);
  e->job.asset_name = std::move(asset_name);
  e->job.source = std::move(source);
  e->job.package = std::move(package);
  e->job.generation = next_generation_++;
  e->job.state = State::queued;
  e->token = std::move(token);
  jobs_.push_back(e);
  save_locked();
  cv_.notify_all();
  wake_();
}
void Manager::uninstall_addon(std::string_view app, const fs::path &root,
                              std::string_view binary, std::string_view sku) {
  std::lock_guard lock(mutex_);
  for (auto &e : jobs_)
    if (e->job.app_id == app &&
        (e->job.state == State::queued || e->job.state == State::checking ||
         e->job.state == State::downloading))
      throw std::runtime_error(
          "Pause this game's downloads before removing an add-on.");
  auto installs = library::scan(root);
  auto found = std::ranges::find_if(installs, [&](auto &i) {
    return i.plan.app_id == app && i.plan.binary_id == binary;
  });
  if (found == installs.end())
    throw std::runtime_error("Installed game was not found.");
  library::uninstall_addon(root, *found, sku);
  std::erase_if(jobs_, [&](auto &e) {
    return e->job.app_id == app && e->job.binary_id == binary &&
           e->job.sku == sku;
  });
  save_locked();
  wake_();
}
bool Manager::refresh_catalog(const fs::path &root, const meta::Plan &catalog) {
  std::lock_guard lock(mutex_);
  for (auto &e : jobs_)
    if (e->job.app_id == catalog.app_id &&
        (e->job.state == State::queued || e->job.state == State::checking ||
         e->job.state == State::downloading))
      return false;
  library::refresh_catalog(root, catalog);
  return true;
}
void Manager::cancel(std::string_view app) {
  std::lock_guard lock(mutex_);
  for (auto &e : jobs_)
    if (e->job.app_id == app && visible(e->job)) {
      e->cancel.request_stop();
      if (e->job.state == State::queued) {
        e->job.state = State::paused;
        e->token.clear();
      }
    }
  save_locked();
  wake_();
}
void Manager::remove(const Job &job, bool discard) {
  std::shared_ptr<Entry> gone;
  {
    std::lock_guard lock(mutex_);
    auto it = std::ranges::find_if(
        jobs_, [&](auto &e) { return !e->removed && same(e->job, job); });
    if (it == jobs_.end())
      return;
    auto e = *it;
    e->cancel.request_stop();
    e->token.clear();
    e->removed = true;
    e->discard = discard;
    if (e->job.state != State::checking && e->job.state != State::downloading) {
      jobs_.erase(it);
      gone = e;
    }
    save_locked();
  }
  if (gone && gone->discard)
    discard_files(gone->job);
  wake_();
}
void Manager::release(const std::shared_ptr<Entry> &e) {
  // The job's slot is free for another worker.
  cv_.notify_all();
  {
    std::lock_guard lock(mutex_);
    if (!e->removed)
      return;
    std::erase(jobs_, e);
    save_locked();
  }
  if (e->discard)
    discard_files(e->job);
  wake_();
}
void Manager::discard_files(const Job &job) {
  if (job.sku.empty() && job.asset_id.empty())
    try {
      for (auto &i : library::scan(job.root))
        if (!i.complete && (i.plan.app_id == job.app_id ||
                            (!job.package.empty() && i.plan.package == job.package)))
          library::uninstall(job.root, i);
    } catch (const std::exception &error) {
      log::warn("downloads", "Could not delete the partial files of {}: {}",
                job.app_id, error.what());
    }
  std::lock_guard lock(mutex_);
  ++discarded_;
}
std::uint64_t Manager::discarded() const {
  std::lock_guard lock(mutex_);
  return discarded_;
}
void Manager::move(const Job &job, int step) {
  std::lock_guard lock(mutex_);
  std::vector<std::size_t> shown;
  std::optional<std::size_t> at;
  for (std::size_t i = 0; i < jobs_.size(); ++i)
    if (visible(jobs_[i]->job) && !jobs_[i]->removed) {
      if (same(jobs_[i]->job, job))
        at = shown.size();
      shown.push_back(i);
    }
  if (!at || std::int64_t(*at) + step < 0 ||
      std::int64_t(*at) + step >= std::int64_t(shown.size()))
    return;
  std::swap(jobs_[shown[*at]], jobs_[shown[*at + step]]);
  save_locked();
  wake_();
}
void Manager::resume(const Job &job, std::string token) {
  if (!job.source.empty())
    enqueue_import(job.app_id, job.name, job.root, std::move(token),
                   job.account == "local" ? job.account : active_key(),
                   job.source, job.package);
  else if (!job.asset_id.empty())
    enqueue_asset(job.app_id, job.root, job.binary_id, job.asset_id,
                  job.asset_name, std::move(token), active_key());
  else if (job.sku.empty())
    enqueue(job.app_id, job.name, job.root, std::move(token));
  else
    enqueue_addon(job.app_id, job.name, job.root, job.binary_id, job.sku,
                  std::move(token));
}
void Manager::cancel_all() {
  std::lock_guard lock(mutex_);
  for (auto &e : jobs_) {
    e->cancel.request_stop();
    if (e->job.state == State::queued)
      e->job.state = State::paused;
    e->token.clear();
  }
  save_locked();
  wake_();
}
std::vector<Job> Manager::snapshot() const {
  std::lock_guard lock(mutex_);
  std::vector<Job> out;
  for (auto &e : jobs_)
    if (visible(e->job) && !e->removed)
      out.push_back(e->job);
  return out;
}
std::vector<Job> Manager::snapshot(std::string_view account) const {
  std::lock_guard lock(mutex_);
  std::vector<Job> out;
  for (auto &e : jobs_)
    if (e->job.account == account && !e->removed)
      out.push_back(e->job);
  return out;
}
void Manager::set_account(std::string key) {
  if (!key.empty() && !meta::valid_account_key(key))
    throw std::runtime_error("Invalid account key.");
  std::lock_guard lock(mutex_);
  active_account_ = std::move(key);
}
void Manager::adopt_legacy(std::string_view primary) {
  if (!meta::valid_account_key(primary))
    throw std::runtime_error("Invalid primary account key.");
  std::lock_guard lock(mutex_);
  for (auto &e : jobs_)
    if (e->job.account.empty())
      e->job.account = primary;
  save_locked();
}
void Manager::cancel_account(std::string_view key) {
  std::lock_guard lock(mutex_);
  for (auto &e : jobs_)
    if (e->job.account == key) {
      e->cancel.request_stop();
      if (e->job.state == State::queued)
        e->job.state = State::paused;
      e->token.clear();
    }
  save_locked();
  wake_();
}
bool Manager::busy() const {
  std::lock_guard lock(mutex_);
  return std::ranges::any_of(jobs_, [](auto &e) {
    return e->job.state == State::queued || e->job.state == State::checking ||
           e->job.state == State::downloading;
  });
}
void Manager::run(std::stop_token shutdown) {
  while (!shutdown.stop_requested()) {
    std::shared_ptr<Entry> e;
    std::string token;
    std::stop_token stop;
    {
      std::unique_lock lock(mutex_);
      // The first queued job in list order whose game has no running job,
      // while fewer than parallel_ run.
      auto next = [&]() -> std::shared_ptr<Entry> {
        const auto running = [](const auto &x) {
          return x->job.state == State::checking || x->job.state == State::downloading;
        };
        if (std::size_t(std::ranges::count_if(jobs_, running)) >= parallel_)
          return nullptr;
        for (auto &x : jobs_)
          if (x->job.state == State::queued &&
              std::ranges::none_of(jobs_, [&](auto &y) { return running(y) && y->job.app_id == x->job.app_id; }))
            return x;
        return nullptr;
      };
      cv_.wait(lock, shutdown, [&] { return (e = next()) != nullptr; });
      if (shutdown.stop_requested() || !e)
        return;
      e->job.state = State::checking;
      token = std::exchange(e->token, {});
      stop = e->cancel.get_token();
    }
    struct Release {
      Manager &manager;
      std::shared_ptr<Entry> entry;
      ~Release() { manager.release(entry); }
    } release{*this, e};
    wake_();
    try {
      std::optional<meta::Store> signed_in;
      if (!token.empty())
        signed_in.emplace(std::move(token), stop, services_.query,
                          services_.read);
      if (!e->job.source.empty()) {
        const auto total =
            run_import(*e, signed_in ? &*signed_in : nullptr, stop);
        {
          std::lock_guard lock(mutex_);
          e->job.state = State::complete;
          e->job.done = total;
          e->job.bytes_per_second = 0;
          e->job.importing = false;
          save_locked();
        }
        log::info("downloads", "Imported Quest application {}.", e->job.app_id);
        wake_();
        continue;
      }
      if (!signed_in)
        throw std::runtime_error("Sign in before resuming downloads.");
      auto &store = *signed_in;
      auto plan = e->job.binary_id.empty()
                      ? store.owned_plan(e->job.app_id)
                      : store.owned_plan(e->job.app_id, e->job.binary_id);
      if (!e->job.sku.empty() || !e->job.asset_id.empty())
        meta::require_asset_catalog(plan);
      std::vector<meta::File> selected;
      if (!e->job.sku.empty() &&
          !store.owned_skus(e->job.app_id).contains(e->job.sku))
        throw std::runtime_error("Meta did not confirm ownership of this "
                                 "add-on. No files were downloaded.");
      for (auto &f : plan.files)
        if (!e->job.asset_id.empty()
                ? f.kind == "asset" && f.id == e->job.asset_id &&
                      f.name == e->job.asset_name
            : e->job.sku.empty() ? f.base()
                                 : f.sku == e->job.sku)
          selected.push_back(f);
      if (selected.empty())
        throw std::runtime_error("No files were found for this add-on.");
      if (!e->job.asset_id.empty() &&
          (!selected[0].sku.empty() || !selected[0].iap_id.empty())) {
        if (selected[0].sku.empty() ||
            !store.owned_skus(e->job.app_id).contains(selected[0].sku))
          throw std::runtime_error(
              "Meta did not confirm ownership of this asset.");
      }
      if (stop.stop_requested())
        throw std::runtime_error("Transfer paused.");
      auto dir = paths::game_dir(e->job.root, plan.package, plan.version_code);
      if (!e->job.binary_id.empty() && !fs::exists(dir / "plan.json"))
        throw std::runtime_error("Install the base game before its add-ons.");
      fs::create_directories(dir);
      if (!paths::within(dir, dir / "plan.json") ||
          !paths::within(dir, dir / "plan.json.tmp"))
        throw std::runtime_error(
            "Installation manifest path escapes the game folder.");
      bool keep_manifest = false;
      if (fs::exists(dir / "plan.json")) {
        auto previous = meta::parse_plan(paths::read_json(dir / "plan.json"));
        // Never replace an installed asset catalog with the reduced one.
        keep_manifest = !meta::catalog_replaces(plan, previous);
        if (previous.binary_id != plan.binary_id ||
            previous.app_id != plan.app_id)
          throw std::runtime_error(
              "This folder belongs to another build. Uninstall that version or "
              "choose another games directory.");
        auto catalog = meta::group_addons(plan);
        for (auto &sku : previous.installed_addons)
          if (std::ranges::find(catalog, sku, &meta::Addon::sku) !=
                  catalog.end() &&
              library::addon_installed({previous, dir, false, 0}, sku))
            plan.installed_addons.push_back(sku);
      }
      uint64_t total = 0;
      for (auto &f : selected) {
        if (f.size > UINT64_MAX - total)
          throw std::runtime_error("Invalid download size.");
        total += f.size;
      }
      uint64_t remaining = total;
      for (auto &f : selected) {
        auto p = dir / paths::from_utf8(f.name);
        if (!paths::within(dir, p))
          throw std::runtime_error("File path escapes installation directory.");
        if (fs::is_regular_file(p) && fs::file_size(p) == f.size)
          remaining -= f.size;
        else {
          p += ".part";
          if (!paths::within(dir, p))
            throw std::runtime_error(
                "Partial file path escapes installation directory.");
          if (fs::is_regular_file(p))
            remaining -= std::min(f.size, fs::file_size(p));
        }
      }
      if (fs::space(dir).available < remaining + 64 * 1024 * 1024)
        throw std::runtime_error("Not enough free space. Free disk space or "
                                 "choose another games folder in Settings.");
      if (!plan.assets_complete)
        log::warn("downloads",
                  "Meta's asset catalog is unavailable; installing the APK "
                  "and OBB of {} without asset files.",
                  e->job.app_id);
      if (!keep_manifest)
        paths::atomic_write(dir / "plan.json", plan.json().dump(2));
      {
        std::lock_guard lock(mutex_);
        e->job.total = total;
        e->job.state = State::downloading;
      }
      wake_();
      uint64_t finished = 0;
      for (auto &f : selected) {
        auto start = std::chrono::steady_clock::now();
        auto last = start;
        uint64_t initial = 0;
        bool first = true;
        {
          std::lock_guard lock(mutex_);
          e->job.file = f.name;
        }
        auto transfer =
            services_.transfer ? services_.transfer : http::download;
        transfer(store.download_url(f), dir / paths::from_utf8(f.name), f.size,
                 stop, [&](uint64_t done, uint64_t) {
                   auto now = std::chrono::steady_clock::now();
                   if (first) {
                     initial = done;
                     start = now;
                     first = false;
                   }
                   {
                     std::lock_guard lock(mutex_);
                     e->job.done = finished + done;
                     auto seconds =
                         std::chrono::duration<double>(now - start).count();
                     e->job.bytes_per_second =
                         seconds > 0.2 ? double(done - initial) / seconds : 0;
                   }
                   if (now - last > std::chrono::milliseconds(100)) {
                     last = now;
                     wake_();
                   }
                 });
        finished += f.size;
      }
      if (stop.stop_requested())
        throw std::runtime_error("Transfer paused.");
      if (!e->job.sku.empty() || !e->job.asset_id.empty()) {
        // Publish only after every file in the SKU passed the size check.
        for (auto &f : selected) {
          auto target = dir / paths::from_utf8(f.name);
          if (!fs::is_regular_file(target) || fs::file_size(target) != f.size)
            throw std::runtime_error(
                "Add-on content did not pass the size check.");
        }
        auto sku = e->job.sku.empty() ? selected[0].sku : e->job.sku;
        if (!sku.empty() &&
            std::ranges::all_of(plan.files,
                                [&](const auto& f) {
                                  if (f.sku != sku) return true;
                                  auto p = dir / paths::from_utf8(f.name);
                                  return paths::within(dir, p) &&
                                         fs::is_regular_file(p) &&
                                         fs::file_size(p) == f.size;
                                }) &&
            std::ranges::find(plan.installed_addons, sku) ==
                plan.installed_addons.end())
          plan.installed_addons.push_back(sku);
        paths::atomic_write(dir / "plan.json", plan.json().dump(2));
      }
      {
        std::lock_guard lock(mutex_);
        e->job.state = State::complete;
        e->job.done = total;
        e->job.bytes_per_second = 0;
        save_locked();
      }
      log::info("downloads", "Installed Quest application {}.", e->job.app_id);
    } catch (const fs::filesystem_error &) {
      std::lock_guard lock(mutex_);
      e->job.importing = false;
      e->job.state = stop.stop_requested() ? State::paused : State::failed;
      e->job.error = "Could not access the download folder. Check free space "
                     "and permissions, then resume.";
      save_locked();
    } catch (const Json::exception &) {
      std::lock_guard lock(mutex_);
      e->job.importing = false;
      e->job.state = State::failed;
      e->job.error = "Meta returned unexpected file details. Refresh the "
                     "library and retry.";
      save_locked();
    } catch (const std::exception &error) {
      std::lock_guard lock(mutex_);
      e->job.importing = false;
      e->job.state = stop.stop_requested() ? State::paused : State::failed;
      e->job.error = stop.stop_requested()
                         ? "Partial files saved. Resume to continue."
                         : error.what();
      e->job.bytes_per_second = 0;
      save_locked();
      if (!stop.stop_requested())
        log::warn("downloads", "{} of {} failed: {}", e->job.source.empty() ? "Download" : "Import",
                  e->job.app_id, error.what());
    }
    wake_();
  }
}
namespace {
// One file from an import source into `part`, continuing a partial copy left
// by an interrupted import. Returns the copy's SHA-256; with `verify`, it is
// checked against the source's own hash when the source can compute one (a
// headset).
std::string copy(const import::Source &source, const import::SourceFile &file,
                 const fs::path &part, std::stop_token stop,
                 const std::function<void(uint64_t)> &report, bool verify) {
  digest::Sha256 sha;
  uint64_t done = 0;
  std::error_code ec;
  if (auto have = fs::file_size(part, ec); !ec && have <= file.size) {
    done = have - have % android::adb::kPullBlock;
    fs::resize_file(part, done);
    std::ifstream in(part, std::ios::binary);
    std::vector<std::uint8_t> buffer(1 << 20);
    for (uint64_t left = done; left && in;) {
      in.read(reinterpret_cast<char *>(buffer.data()),
              std::streamsize(std::min<uint64_t>(left, buffer.size())));
      const auto n = uint64_t(in.gcount());
      sha.update({buffer.data(), std::size_t(n)});
      left -= n;
      if (!n) done -= left, left = 0;
    }
  }
  if (done)
    log::info("downloads", "Resuming the import of {} at {} MiB.", file.name,
              done >> 20);
  // The source hashes its copy while this one transfers.
  std::optional<std::string> remote;
  std::jthread checker([&](std::stop_token own) {
    std::stop_source both;
    std::stop_callback outer(stop, [&] { both.request_stop(); });
    std::stop_callback inner(own, [&] { both.request_stop(); });
    try {
      if (verify) remote = source.sha256(file, both.get_token());
    } catch (...) {
    }
  });
  {
    std::ofstream out(part, std::ios::binary | (done ? std::ios::app : std::ios::trunc));
    if (!out)
      throw std::runtime_error("Could not write to the games folder. Check "
                               "free space and permissions, then retry.");
    report(done);
    source.read(
        file,
        [&](std::span<const std::uint8_t> chunk) {
          out.write(reinterpret_cast<const char *>(chunk.data()),
                    std::streamsize(chunk.size()));
          if (!out)
            throw std::runtime_error(
                "Could not write to the games folder. Check free space "
                "and permissions, then retry.");
          sha.update(chunk);
          done += chunk.size();
          report(done);
        },
        stop, done);
    out.close();
    if (!out)
      throw std::runtime_error("Could not write to the games folder. Check "
                               "free space and permissions, then retry.");
  }
  if (done != file.size)
    throw std::runtime_error(file.name + " changed while it was being imported.");
  checker.join();
  const auto hash = sha.finish();
  if (remote && *remote != hash) {
    fs::remove(part, ec);
    throw std::runtime_error(file.name +
                             " arrived damaged (its checksum differs from the "
                             "headset's). Check the cable and retry.");
  }
  return hash;
}
} // namespace

uint64_t Manager::run_import(Entry &e, meta::Store *store, std::stop_token stop) {
  std::string spec, package, account, app_id;
  fs::path root;
  {
    std::lock_guard lock(mutex_);
    spec = e.job.source;
    package = e.job.package;
    root = e.job.root;
    account = e.job.account;
    app_id = e.job.app_id;
  }
  auto source =
      services_.open_source
          ? services_.open_source(spec, package, stop)
          : import::reopen(spec, package,
                           android::adb::locate().value_or(fs::path{}), stop);
  if (!source || source->manifest.package != package)
    throw std::runtime_error("The import source no longer holds this game.");
  // The source's files install as they are. When a signed-in account owns
  // the Meta build of the same version, that build only supplies the files
  // the source lacks.
  std::optional<meta::Plan> catalog;
  if (store && digits(app_id, 5)) {
    try {
      const auto binary = import::binary_for(store->builds(app_id),
                                             source->manifest.version_code);
      if (!binary.empty())
        catalog = store->owned_plan(app_id, binary);
    } catch (...) {
      if (stop.stop_requested())
        throw std::runtime_error("Transfer paused.");
    }
    if (catalog && catalog->package != package)
      catalog.reset();
    if (!catalog)
      log::info("downloads",
                "No owned Meta build {} of {}; importing the source's files only.",
                source->manifest.version_code, package);
  }
  auto imported = catalog ? import::plan(*catalog, *source)
                          : import::local_plan(*source, digits(app_id, 5) ? app_id : "");
  auto &plan = imported.plan;
  const auto dir = paths::game_dir(root, plan.package, plan.version_code);
  fs::create_directories(dir);
  if (!paths::within(dir, dir / "plan.json") ||
      !paths::within(dir, dir / "plan.json.tmp"))
    throw std::runtime_error(
        "Installation manifest path escapes the game folder.");
  if (fs::exists(dir / "plan.json")) {
    auto previous = meta::parse_plan(paths::read_json(dir / "plan.json"));
    if (previous.binary_id != plan.binary_id || previous.app_id != plan.app_id)
      throw std::runtime_error(
          "This folder belongs to another build. Uninstall that version or "
          "choose another games directory.");
    auto catalog_addons = meta::group_addons(plan);
    for (auto &sku : previous.installed_addons)
      if (std::ranges::find(catalog_addons, sku, &meta::Addon::sku) !=
              catalog_addons.end() &&
          library::addon_installed({previous, dir, false, 0}, sku))
        plan.installed_addons.push_back(sku);
    // Hashes of files an interrupted import already wrote.
    for (auto &f : plan.files)
      for (auto &old : previous.files)
        if (old.name == f.name && old.size == f.size && f.sha256.empty())
          f.sha256 = old.sha256;
  }
  auto target_of = [&](const meta::File &f) {
    auto target = dir / paths::from_utf8(f.name);
    if (!paths::within(dir, target) ||
        !paths::within(dir, paths::from_utf8(paths::utf8(target) + ".part")))
      throw std::runtime_error("File path escapes installation directory.");
    return target;
  };
  auto present = [&](const meta::File &f) {
    std::error_code ec;
    auto target = target_of(f);
    // A bundle cut short at an earlier source is copied again.
    return fs::is_regular_file(target, ec) && fs::file_size(target, ec) == f.size &&
           unity_bundle_size(target).value_or(f.size) == f.size;
  };
  // Saves go to the importing account's storage for the game ("local" for
  // none), only when it has none yet: saves made on this computer are never
  // replaced.
  fs::path saves;
  if (!source->saves.empty() && !account.empty()) {
    saves = account_data_dir(data_, account, package) / "external-data";
    std::error_code ec;
    if (fs::exists(saves, ec) || ec) {
      log::info("downloads", "Kept the saves of {} on this computer.", package);
      saves.clear();
      std::lock_guard lock(mutex_);
      e.job.saves_kept = true;
    }
  }
  uint64_t total = 0, remaining = 0;
  for (auto &step : imported.steps) {
    if (step.file.size > UINT64_MAX - total)
      throw std::runtime_error("Invalid import size.");
    total += step.file.size;
    if (!present(step.file))
      remaining += step.file.size;
  }
  if (!saves.empty()) {
    total += source->save_bytes();
    remaining += source->save_bytes();
  }
  if (fs::space(dir).available < remaining + 64 * 1024 * 1024)
    throw std::runtime_error("Not enough free space. Free disk space or "
                             "choose another games folder in Settings.");
  paths::atomic_write(dir / "plan.json", plan.json().dump(2));
  {
    std::lock_guard lock(mutex_);
    e.job.total = total;
    e.job.done = 0;
    e.job.state = State::downloading;
  }
  wake_();
  uint64_t finished = 0;
  for (auto &step : imported.steps) {
    if (stop.stop_requested())
      throw std::runtime_error("Transfer paused.");
    const auto target = target_of(step.file);
    if (present(step.file)) {
      finished += step.file.size;
      continue;
    }
    {
      std::lock_guard lock(mutex_);
      e.job.file = step.file.name;
      e.job.importing = step.from != nullptr;
    }
    auto start = std::chrono::steady_clock::now(), last = start;
    auto report = [&](uint64_t done) {
      auto now = std::chrono::steady_clock::now();
      {
        std::lock_guard lock(mutex_);
        e.job.done = finished + done;
        auto seconds = std::chrono::duration<double>(now - start).count();
        e.job.bytes_per_second = seconds > 0.2 ? double(done) / seconds : 0;
      }
      if (now - last > std::chrono::milliseconds(100)) {
        last = now;
        wake_();
      }
    };
    if (step.from) {
      auto part = paths::from_utf8(paths::utf8(target) + ".part");
      const auto hash = copy(*source, *step.from, part, stop, report, true);
      if (const auto whole = unity_bundle_size(part); whole && *whole != step.from->size) {
        std::error_code ec;
        fs::remove(part, ec);
        throw std::runtime_error(std::format(
            "{} is damaged in {}: it has {} bytes of {}. Reinstall the game on the headset, or copy the "
            "file again, then retry.",
            step.file.name, source->describe(), step.from->size, *whole));
      }
      fs::rename(part, target);
      for (auto &f : plan.files)
        if (f.name == step.file.name) f.sha256 = hash;
      // Hashes are published as files land, so a resumed import keeps them.
      paths::atomic_write(dir / "plan.json", plan.json().dump(2));
    } else {
      auto transfer = services_.transfer ? services_.transfer : http::download;
      transfer(store->download_url(step.file), target, step.file.size, stop,
               [&](uint64_t done, uint64_t) { report(done); });
    }
    if (stop.stop_requested())
      throw std::runtime_error("Transfer paused.");
    if (!present(step.file))
      throw std::runtime_error(step.file.name + " did not pass the size check.");
    finished += step.file.size;
  }
  if (!saves.empty()) {
    // Staged beside the target and published whole, so an interrupted
    // import never leaves half a save.
    auto staging = paths::from_utf8(paths::utf8(saves) + ".import");
    fs::remove_all(staging);
    size_t skipped = 0;
    for (auto &file : source->saves) {
      const auto target = staging / paths::from_utf8(file.name);
      if (!paths::within(staging, target))
        throw std::runtime_error("A save's path escapes the game's storage.");
      fs::create_directories(target.parent_path());
      {
        std::lock_guard lock(mutex_);
        e.job.file = file.name;
        e.job.importing = true;
      }
      try {
        copy(*source, file, target, stop, [&](uint64_t done) {
          std::lock_guard lock(mutex_);
          e.job.done = finished + done;
        }, false);
      } catch (const std::exception &error) {
        if (stop.stop_requested()) throw;
        // A game's storage also holds caches the headset rewrites while it
        // is copied; one unreadable file must not cost the whole import.
        std::error_code ec;
        fs::remove(target, ec);
        ++skipped;
        log::warn("downloads", "Skipped the save {} of {}: {}", file.name, package, error.what());
      }
      finished += file.size;
      wake_();
    }
    if (stop.stop_requested())
      throw std::runtime_error("Transfer paused.");
    fs::rename(staging, saves);
    log::info("downloads", "Imported {} saves of {}.", source->saves.size() - skipped, package);
  }
  return total;
}
} // namespace venture::download
