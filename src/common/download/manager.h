#pragma once
#include "common/meta/store.h"
#include "common/library/import.h"
#include <condition_variable>
#include <mutex>
#include <thread>
namespace venture::download {
enum class State { queued, checking, downloading, paused, failed, complete };
const char *label(State state);
struct Job {
  std::string app_id, name, file, error;
  fs::path root;
  State state = State::paused;
  uint64_t done = 0, total = 0;
  double bytes_per_second = 0;
  std::string sku, binary_id;
  std::string account;
  std::string asset_id, asset_name;
  // An import: where its files come from (import::Source::spec) and the
  // package there. `importing` is set while the current file is copied from
  // it rather than downloaded.
  std::string source, package;
  bool importing = false;
  // The import brought saves, but this computer's saves for the game were kept.
  bool saves_kept = false;
  std::uint64_t generation = 1;
};
// The base-game job of an app (no add-on SKU or single asset) in a snapshot.
std::optional<Job> base_job(const std::vector<Job> &jobs, std::string_view app);
class Manager {
public:
  struct Services {
    meta::Store::Transport query;
    meta::Store::ReadTransport read;
    std::function<void(std::string, const fs::path &, uint64_t, std::stop_token,
                       const http::Progress &)>
        transfer;
    // Reopens an import source; the default finds adb on its own.
    std::function<std::unique_ptr<import::Source>(std::string_view spec, std::string_view package,
                                                  std::stop_token)>
        open_source;
  };
  // Hidden direct-launch supervisors service on-demand assets without reading
  // or rewriting the visible launcher's persistent install queue.
  Manager(fs::path data, std::function<void()> wake, Services services = {},
          bool persistent = true);
  ~Manager();
  void enqueue(std::string app, std::string name, fs::path root,
               std::string token);
  void enqueue(std::string app, std::string name, fs::path root,
               std::string token, std::string account);
  void enqueue_addon(std::string app, std::string name, fs::path root,
                     std::string binary_id, std::string sku, std::string token);
  void enqueue_asset(std::string app, fs::path root, std::string binary_id,
                     std::string id, std::string filename, std::string token,
                     std::string account);
  // Installs `app` from an import source for `account` ("local" for none, with
  // no token); see common/library/import.h. `app` is Meta's id for it, or the
  // package when Meta's is unknown.
  void enqueue_import(std::string app, std::string name, fs::path root,
                      std::string token, std::string account,
                      std::string source, std::string package);
  bool cancel_asset(std::string_view app, std::string_view binary,
                    std::string_view filename, std::string_view account);
  bool delete_asset(const fs::path& install, std::string_view app,
                    std::string_view binary, std::string_view filename);
  void uninstall_addon(std::string_view app, const fs::path &root,
                       std::string_view binary_id, std::string_view sku);
  bool refresh_catalog(const fs::path &root, const meta::Plan &catalog);
  void cancel(std::string_view app);
  // Drops a job from the queue, stopping it first. With `discard`, the files
  // of a base game or import that never finished installing are deleted too.
  void remove(const Job &job, bool discard);
  // Moves a job one place earlier (-1) or later (+1) among the visible jobs;
  // queued jobs start in this order.
  void move(const Job &job, int step);
  // Counts the partial installs remove() deleted; callers rescan on a change.
  std::uint64_t discarded() const;
  void resume(const Job &job, std::string token);
  void cancel_all();
  void cancel_account(std::string_view key);
  void set_account(std::string key);
  void adopt_legacy(std::string_view primary);
  std::vector<Job> snapshot(std::string_view account) const;
  std::vector<Job> snapshot() const;
  bool busy() const;
  // How many jobs transfer at once (1 to max_parallel); jobs of one game
  // always run one after another.
  void set_parallel(unsigned jobs);
  static constexpr unsigned max_parallel = 4;

private:
  struct Entry {
    Job job;
    std::string token;
    std::stop_source cancel;
    // Set by remove(); a running job leaves when its worker releases it.
    bool removed = false, discard = false;
  };
  mutable std::mutex mutex_;
  std::condition_variable_any cv_;
  std::vector<std::shared_ptr<Entry>> jobs_;
  fs::path data_, file_;
  std::string active_account_;
  std::uint64_t next_generation_ = 1, discarded_ = 0;
  std::function<void()> wake_;
  unsigned parallel_ = 1;
  std::vector<std::jthread> threads_;
  Services services_;
  void enqueue_job(std::string app, std::string name, fs::path root,
                   std::string token, std::string account, std::string binary,
                   std::string sku, std::string asset_id = {},
                   std::string asset_name = {}, std::string source = {},
                   std::string package = {});
  std::string active_key() const;
  void run(std::stop_token stop);
  // An import job's files; returns the bytes written.
  // `store` is null without a signed-in account.
  uint64_t run_import(Entry &e, meta::Store *store, std::stop_token stop);
  // Jobs of the active account, and imports made with none.
  bool visible(const Job &job) const {
    return job.account == active_account_ || job.account == "local";
  }
  void save_locked();
  void release(const std::shared_ptr<Entry> &e);
  void discard_files(const Job &job);
};
} // namespace venture::download
