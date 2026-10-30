// venture-cli: the Venture launcher from a terminal. Every command drives
// launcher::Core, the same implementation the launcher window uses, so an
// install, import or launch here is exactly the one the window performs.
#include "common/log.h"
#include "launcher/core.h"
#include "launcher/ui/rich_text.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <format>
#include <iostream>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace venture::launcher {
namespace {
volatile std::sig_atomic_t interrupted = 0;
void on_interrupt(int) { interrupted = 1; }

constexpr const char *kUsage = R"(Venture command line

Usage: venture-cli [--json] [--offline] <command> [arguments]

Library
  status                         Accounts, games folder, installs and downloads
  library [--installed] [--search TEXT]
                                 Games your accounts own and games installed here
  info GAME                      Store details, installation and add-ons
  install GAME [--as ACCOUNT] [--to FOLDER]
                                 Download and install (or update) a game; a new
                                 one goes to FOLDER instead of the games folder
  uninstall GAME [--yes]         Remove a game's files; saves are kept
  play GAME [--as ACCOUNT|local] Launch a game and wait for it to exit
  logs GAME [--lines N]          The end of a game's last runtime log

Add-ons
  addons GAME [--as ACCOUNT]     List a game's add-ons (as the owning account)
  addons install GAME SKU|--all  Install owned add-ons
  addons remove GAME SKU         Remove an installed add-on

Downloads
  downloads                      The download and import queue
  downloads cancel GAME          Cancel a game's download
  downloads resume GAME          Resume a paused or failed download

Import
  import PATH [--to FOLDER] [--yes]
                                 Import an APK, a folder or an archive
  import --headset [--serial SERIAL] [--only PKG,...] [--to FOLDER] [--yes]
                                 Import games from a Quest connected over adb
  import-saves GAME --from FOLDER | --headset [--serial SERIAL]
                                 Import an installed game's save data (the save
                                 data it replaces is kept in a backup)

Accounts
  accounts                       Signed-in Meta accounts
  accounts sign-in               Add an account (opens Meta's sign-in window)
  accounts use ACCOUNT           Make an account active
  accounts primary ACCOUNT       Mark the primary account
  accounts sign-out ACCOUNT      Sign out; its games and saves stay
  accounts remove ACCOUNT [--delete-saves] [--yes]

Settings
  settings                       Show settings
  settings set KEY VALUE         games-folder PATH | runtime PATH|default |
                                 adb PATH|auto | log-level trace|debug|info|warn|error |
                                 console on|off | add-to-steam on|off |
                                 add-new-to-steam on|off | steam-frame-passthrough on|off

GAME is a name (or a unique part of it), an app id or a package name.
ACCOUNT is a name, alias, account key or the number `accounts` shows.
Exit codes: 0 success, 1 failure, 2 usage, 130 interrupted; play returns the
game's own exit code.
)";

struct UsageError : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct Interrupted {};

struct Args {
  std::vector<std::string> words;
  std::map<std::string, std::string> values;
  std::set<std::string> flags;
  bool flag(std::string_view f) const { return flags.contains(std::string(f)); }
  std::optional<std::string> value(std::string_view v) const {
    auto it = values.find(std::string(v));
    return it == values.end() ? std::nullopt : std::optional(it->second);
  }
  const std::string &word(size_t i, std::string_view what) const {
    if (i >= words.size())
      throw UsageError(std::format("Missing {}.", what));
    return words[i];
  }
};

Args parse(const std::vector<std::string> &argv) {
  static const std::set<std::string> with_value{"--as", "--to", "--search", "--serial", "--lines", "--only", "--from"};
  static const std::set<std::string> known{"--json",   "--offline", "--yes",          "-y",
                                           "--all",    "--installed", "--delete-saves", "--headset",
                                           "--help",   "-h",        "--version"};
  Args out;
  for (size_t i = 1; i < argv.size(); ++i) {
    const auto &a = argv[i];
    if (with_value.contains(a)) {
      if (i + 1 >= argv.size())
        throw UsageError(std::format("{} needs a value.", a));
      out.values[a] = argv[++i];
    } else if (known.contains(a)) {
      out.flags.insert(a == "-y" ? "--yes" : a == "-h" ? "--help" : a);
    } else if (a.starts_with("--")) {
      throw UsageError(std::format("Unknown option {}.", a));
    } else {
      out.words.push_back(a);
    }
  }
  return out;
}

bool terminal(FILE *f) {
#ifdef _WIN32
  // _isatty is also true for NUL and other character devices.
  DWORD mode;
  return GetConsoleMode(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f))), &mode) != 0;
#else
  return isatty(fileno(f)) != 0;
#endif
}

std::string lower(std::string s) {
  for (auto &c : s)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string bytes(uint64_t n) {
  const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  double v = double(n);
  int u = 0;
  while (v >= 1000 && u < 4) {
    v /= 1000;
    ++u;
  }
  return u == 0 ? std::format("{} B", n) : std::format("{:.1f} {}", v, units[u]);
}

std::string duration(double seconds) {
  auto s = uint64_t(std::max(0.0, seconds));
  if (s >= 3600)
    return std::format("{}h{:02}m", s / 3600, s / 60 % 60);
  if (s >= 60)
    return std::format("{}m{:02}s", s / 60, s % 60);
  return std::format("{}s", s);
}

// Columns padded to their widest cell (UTF-8 aware enough for names).
void table(const std::vector<std::string> &head, const std::vector<std::vector<std::string>> &rows) {
  auto width = [](const std::string &s) {
    return size_t(std::ranges::count_if(s, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
  };
  std::vector<size_t> w(head.size());
  for (size_t c = 0; c < head.size(); ++c) {
    w[c] = width(head[c]);
    for (auto &r : rows)
      w[c] = std::max(w[c], width(r[c]));
  }
  auto line = [&](const std::vector<std::string> &r) {
    std::string out;
    for (size_t c = 0; c < r.size(); ++c) {
      out += r[c];
      if (c + 1 < r.size())
        out += std::string(w[c] - width(r[c]) + 2, ' ');
    }
    std::cout << out << "\n";
  };
  line(head);
  for (auto &r : rows)
    line(r);
}

bool confirm(const Args &args, const std::string &question) {
  if (args.flag("--yes"))
    return true;
  if (!terminal(stdin))
    throw UsageError(question + " Pass --yes to confirm.");
  std::cout << question << " [y/N] " << std::flush;
  std::string answer;
  std::getline(std::cin, answer);
  if (lower(answer) == "y" || lower(answer) == "yes")
    return true;
  std::cout << "Cancelled.\n";
  return false;
}

const char *job_state(download::State s) { return download::label(s); }

// A store description (Markdown or HTML) as wrapped plain text, cut after
// about `limit` characters.
std::string plain_description(std::string_view text, size_t limit) {
  std::string out;
  for (const auto &block : ui::parse_description(text)) {
    std::string words;
    for (const auto &span : block.spans)
      words += span.text;
    if (block.kind == ui::Block::Kind::rule || words.find_first_not_of(" \t") == std::string::npos)
      continue;
    std::string prefix, indent;
    if (block.kind == ui::Block::Kind::bullet) {
      prefix = "  - ";
      indent = "    ";
    } else if (block.kind == ui::Block::Kind::numbered) {
      prefix = "  " + block.marker + " ";
      indent = std::string(prefix.size(), ' ');
    }
    if (!out.empty() && (block.kind == ui::Block::Kind::heading || block.kind == ui::Block::Kind::paragraph ||
                         block.after_blank))
      out += "\n";
    std::string line = prefix;
    size_t column = 0;
    for (size_t at = 0; at < words.size();) {
      auto end = words.find(' ', at);
      auto word = words.substr(at, end == std::string::npos ? std::string::npos : end - at);
      at = end == std::string::npos ? words.size() : end + 1;
      if (word.empty())
        continue;
      if (column && column + 1 + word.size() + prefix.size() > 78) {
        out += line + "\n";
        line = indent;
        column = 0;
      }
      line += (column ? " " : "") + word;
      column += word.size() + (column ? 1 : 0);
    }
    out += line + "\n";
    if (out.size() > limit) {
      out += "...\n";
      break;
    }
  }
  return out;
}

class Cli : public Core {
public:
  Cli(Options options, bool json) : Core(std::move(options)), json_(json) {}
  ~Cli() override { shutdown(); }
  int run(const Args &args);

private:
  bool json_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool woken_ = false;
  void wake() override {
    {
      std::lock_guard lock(mutex_);
      woken_ = true;
    }
    cv_.notify_all();
  }
  // Applies completions until `done`; throws Interrupted on Ctrl+C.
  void wait(const std::function<bool()> &done, const std::function<void()> &each = {}) {
    for (;;) {
      tick();
      if (done())
        return;
      if (interrupted)
        throw Interrupted{};
      if (each)
        each();
      std::unique_lock lock(mutex_);
      cv_.wait_for(lock, std::chrono::milliseconds(200), [&] { return woken_; });
      woken_ = false;
    }
  }
  void wait_scan() { wait([&] { return !scanning_; }); }
  // The Meta library of every signed-in account, or the cached one offline.
  void wait_library() {
    wait([&] { return !refreshing_ && !scanning_ && library_pending_.empty(); });
    if (!refresh_error_.empty())
      std::cerr << "warning: could not refresh the Meta library (" << refresh_error_
                << "); showing the saved library.\n";
  }
  void wait_details(const std::string &id) {
    if (options_.offline || !session_)
      return;
    select(id);
    wait([&] { return !details_pending_.contains(id) && !addons_pending_.contains(id); });
  }
  // --as ACCOUNT: the owning account this command acts as.
  void pick_owner(const Args &args, const meta::Game &g) {
    auto as = args.value("--as");
    if (!as)
      return;
    auto key = resolve_account(*as);
    auto owners = game_owners(g.id);
    if (std::ranges::find(owners, key) == owners.end())
      throw std::runtime_error(std::format("{} does not own {}.", account_label(key), g.name));
    owner_pick_[g.id] = key;
  }
  // Runs `fn` as the account that owns the game (as the window's Install
  // switches to it), then returns to the account that was active.
  template <class F> int as_owner(const meta::Game &g, F &&fn) {
    const auto home = account_key_;
    auto restore = [&] {
      if (!home.empty() && account_key_ != home)
        action([&] { switch_account(home); });
    };
    try {
      const auto owner = owner_for(g.id);
      if (!owner.empty() && owner != account_key_) {
        auto other = account(owner);
        if (!other || !other->signed_in || other->needs_reauth)
          throw std::runtime_error(std::format("Reconnect {}, which owns {}.", account_label(owner), g.name));
        switch_account(owner);
        wait_library();
      }
      int rc = fn();
      restore();
      return rc;
    } catch (...) {
      restore();
      throw;
    }
  }
  void print(const Json &j) { std::cout << j.dump(2) << "\n"; }
  void need_queue() {
    if (!owns_download_queue())
      throw std::runtime_error("The Venture launcher is open and manages downloads. Use it, or close it first.");
  }
  void need_online() {
    if (options_.offline)
      throw std::runtime_error("This needs a connection. Run without --offline.");
    if (!session_)
      throw std::runtime_error("Sign in first: venture-cli accounts sign-in");
  }

  std::string account_name(const AccountView &a) const {
    return !a.display_name.empty() ? a.display_name : !a.alias.empty() ? a.alias : "Meta account";
  }
  std::string account_label(std::string_view key) const {
    if (key == "local")
      return "no account";
    auto a = account(key);
    return a ? account_name(*a) : std::string(key);
  }
  std::string resolve_account(const std::string &query, bool allow_local = false) const {
    if (allow_local && query == "local")
      return "local";
    const auto q = lower(query);
    std::vector<std::string> found;
    for (size_t i = 0; i < accounts_.size(); ++i) {
      const auto &a = accounts_[i];
      if (a.key == query || std::to_string(i + 1) == query || (!a.alias.empty() && lower(a.alias) == q) ||
          (!a.display_name.empty() && lower(a.display_name) == q))
        found.push_back(a.key);
    }
    if (found.size() == 1)
      return found.front();
    throw std::runtime_error(found.empty() ? std::format("No account matches \"{}\". See: venture-cli accounts", query)
                                           : std::format("\"{}\" matches several accounts; use its key.", query));
  }

  std::string package_of(const meta::Game &g) const {
    if (auto i = installed(g.id, false))
      return i->plan.package;
    return g.package;
  }
  meta::Game resolve_game(const std::string &query) const {
    auto games = visible_games();
    const auto q = lower(query);
    auto pick = [&](auto pred) {
      std::vector<meta::Game> out;
      for (auto &g : games)
        if (pred(g))
          out.push_back(g);
      return out;
    };
    for (auto found : {pick([&](auto &g) { return g.id == query; }),
                       pick([&](auto &g) { return !package_of(g).empty() && package_of(g) == query; }),
                       pick([&](auto &g) { return lower(g.name) == q; }),
                       pick([&](auto &g) { return lower(g.name).find(q) != std::string::npos; })}) {
      if (found.size() == 1)
        return found.front();
      if (found.size() > 1) {
        std::string names;
        for (size_t i = 0; i < found.size() && i < 8; ++i)
          names += std::format("\n  {} ({})", found[i].name, found[i].id);
        throw std::runtime_error(std::format("\"{}\" matches {} games:{}", query, found.size(), names));
      }
    }
    throw std::runtime_error(std::format("No game matches \"{}\". See: venture-cli library", query));
  }

  struct GameState {
    std::string label;
    const library::Install *install = nullptr;
    std::optional<download::Job> job;
  };
  GameState state_of(const meta::Game &g) const {
    GameState s;
    s.install = installed(g.id, false);
    s.job = download::base_job(downloads_->snapshot(), g.id);
    bool active = s.job && (s.job->state == download::State::downloading || s.job->state == download::State::queued ||
                            s.job->state == download::State::checking);
    if (active)
      s.label = s.job->importing ? "importing" : "downloading";
    else if (s.job && s.job->state == download::State::failed)
      s.label = "needs attention";
    else if (s.install && s.install->complete)
      s.label = "installed";
    else if (s.install || (s.job && s.job->state == download::State::paused && s.job->done))
      s.label = "paused";
    else
      s.label = "not installed";
    return s;
  }
  Json game_json(const meta::Game &g) const {
    auto s = state_of(g);
    Json j{{"id", g.id}, {"name", g.name}, {"publisher", g.publisher}, {"status", s.label}};
    auto package = package_of(g);
    if (!package.empty())
      j["package"] = package;
    Json owners = Json::array();
    for (auto &key : game_owners(g.id))
      owners.push_back(account_label(key));
    j["owners"] = owners;
    if (s.install)
      j["install"] = {{"directory", paths::utf8(s.install->directory)},
                      {"version", s.install->plan.version},
                      {"versionCode", s.install->plan.version_code},
                      {"complete", s.install->complete},
                      {"bytes", s.install->bytes}};
    if (s.job)
      j["download"] = {{"state", job_state(s.job->state)}, {"done", s.job->done}, {"total", s.job->total},
                       {"error", s.job->error}};
    return j;
  }

  // Follows one queued job to its end, printing progress.
  download::Job follow(const std::string &id, const std::string &sku, const std::string &verb) {
    const bool tty = terminal(stdout) && !json_;
    std::optional<download::Job> last;
    auto started = std::chrono::steady_clock::now();
    auto printed = started - std::chrono::hours(1);
    int printed_tenth = -1;
    // Imports queue under each game's owner, which need not be the active account.
    auto find = [&]() -> std::optional<download::Job> {
      auto jobs = downloads_->snapshot();
      for (auto &a : accounts_)
        if (a.key != account_key_)
          std::ranges::move(downloads_->snapshot(a.key), std::back_inserter(jobs));
      for (auto &j : jobs)
        if (j.app_id == id && j.sku == sku && j.asset_id.empty())
          return j;
      return std::nullopt;
    };
    wait(
        [&] {
          last = find();
          return !last || last->state == download::State::complete || last->state == download::State::failed ||
                 last->state == download::State::paused;
        },
        [&] {
          if (!last || json_)
            return;
          auto &j = *last;
          auto now = std::chrono::steady_clock::now();
          std::string text = std::format("{} {}", verb, j.name.empty() ? id : j.name);
          if (j.state == download::State::queued)
            text += "  queued";
          else if (j.state == download::State::checking)
            text += "  checking files";
          else if (j.total) {
            const double f = double(j.done) / double(j.total);
            text += std::format("  {:>3}%  {} of {}", int(f * 100), bytes(j.done), bytes(j.total));
            if (j.bytes_per_second > 1)
              text += std::format("  {}/s  {} left", bytes(uint64_t(j.bytes_per_second)),
                                  duration(double(j.total - j.done) / j.bytes_per_second));
          }
          if (tty) {
            if (now - printed > std::chrono::milliseconds(250)) {
              std::cout << "\r" << text << "\x1b[K" << std::flush;
              printed = now;
            }
          } else {
            int tenth = j.total ? int(10 * j.done / j.total) : -1;
            if (tenth != printed_tenth || now - printed > std::chrono::seconds(60)) {
              std::cout << text << "\n" << std::flush;
              printed_tenth = tenth;
              printed = now;
            }
          }
        });
    if (tty)
      std::cout << "\r\x1b[K" << std::flush;
    if (!last)
      throw std::runtime_error("The job left the queue before it finished.");
    if (last->state != download::State::complete)
      throw std::runtime_error(last->error.empty() ? std::format("The {} did not finish.", lower(verb))
                                                   : last->error);
    return *last;
  }

  int status(const Args &);
  int library_list(const Args &);
  int info(const Args &);
  int install_game(const Args &);
  int uninstall_game(const Args &);
  int play_game(const Args &);
  int logs(const Args &);
  int addons_cmd(const Args &);
  int addons_owned(const Args &, const meta::Game &, const std::string &sub);
  int downloads_cmd(const Args &);
  int import_cmd(const Args &);
  int import_saves_cmd(const Args &);
  int accounts_cmd(const Args &);
  int settings_cmd(const Args &);
};

int Cli::status(const Args &) {
  wait_scan();
  Json accounts = Json::array();
  for (auto &a : accounts_)
    accounts.push_back({{"name", account_name(a)}, {"active", a.active}, {"primary", a.primary},
                        {"signedIn", a.signed_in && !a.needs_reauth}});
  size_t complete = std::ranges::count_if(installs_, [](auto &i) { return i.complete; });
  uint64_t total = 0;
  for (auto &i : installs_)
    total += i.bytes;
  Json jobs = Json::array();
  for (auto &j : downloads_->snapshot())
    if (j.state != download::State::complete)
      jobs.push_back({{"name", j.name}, {"state", job_state(j.state)}});
  Json out{{"version", VENTURE_VERSION},
           {"dataFolder", paths::utf8(data_)},
           {"gamesFolder", paths::utf8(settings_.games)},
           {"accounts", accounts},
           {"installed", complete},
           {"installedBytes", total},
           {"downloads", jobs},
           {"launcherOpen", !owns_download_queue()}};
  if (json_)
    return print(out), 0;
  std::cout << "Venture " << VENTURE_VERSION << "\n";
  std::cout << "Games folder: " << paths::utf8(settings_.games) << "\n";
  if (accounts_.empty())
    std::cout << "Accounts:     none (venture-cli accounts sign-in)\n";
  for (auto &a : accounts_)
    if (a.active)
      std::cout << "Account:      " << account_name(a) << " (" << accounts_.size() << " signed in)\n";
  std::cout << "Installed:    " << complete << " games, " << bytes(total) << "\n";
  std::cout << "Downloads:    " << (jobs.empty() ? std::string("none") : std::to_string(jobs.size()) + " in the queue")
            << "\n";
  if (!owns_download_queue())
    std::cout << "The Venture launcher is open; it manages downloads until it closes.\n";
  return 0;
}

int Cli::library_list(const Args &args) {
  wait_library();
  auto games = visible_games();
  const bool only_installed = args.flag("--installed");
  const auto search = lower(args.value("--search").value_or(""));
  std::erase_if(games, [&](auto &g) {
    return (only_installed && !installed(g.id, false)) ||
           (!search.empty() && lower(g.name).find(search) == std::string::npos);
  });
  std::ranges::sort(games, [](auto &a, auto &b) { return lower(a.name) < lower(b.name); });
  if (json_) {
    Json out = Json::array();
    for (auto &g : games)
      out.push_back(game_json(g));
    return print(out), 0;
  }
  if (games.empty()) {
    std::cout << (only_installed || !search.empty() ? "No games match.\n" : "No games yet.\n");
    return 0;
  }
  std::vector<std::vector<std::string>> rows;
  for (auto &g : games) {
    auto s = state_of(g);
    rows.push_back({g.name, s.label, s.install ? s.install->plan.version : "",
                    s.install ? bytes(s.install->bytes) : "", g.id});
  }
  table({"Game", "Status", "Version", "Size", "App ID"}, rows);
  std::cout << std::format("\n{} games, {} installed\n", games.size(),
                           std::ranges::count_if(games, [&](auto &g) { return installed(g.id) != nullptr; }));
  return 0;
}

int Cli::info(const Args &args) {
  wait_library();
  auto g = resolve_game(args.word(1, "game"));
  wait_details(g.id);
  if (auto found = game(g.id))
    g = *found;
  auto j = game_json(g);
  auto detail = details_.find(g.id);
  if (detail != details_.end()) {
    j["latestVersion"] = detail->second.version;
    j["downloadBytes"] = detail->second.size;
  }
  j["updateAvailable"] = update_available(g.id);
  j["description"] = g.description;
  Json list = Json::array();
  for (auto &a : addons(g.id))
    list.push_back({{"sku", a.sku}, {"name", a.name}, {"owned", a.owned}, {"installed", a.installed}});
  j["addons"] = list;
  if (json_)
    return print(j), 0;
  auto s = state_of(g);
  std::cout << g.name << "\n";
  if (!g.publisher.empty())
    std::cout << "Publisher:  " << g.publisher << "\n";
  std::cout << "App ID:     " << g.id << "\n";
  if (auto p = package_of(g); !p.empty())
    std::cout << "Package:    " << p << "\n";
  std::string owners;
  for (auto &key : game_owners(g.id))
    owners += (owners.empty() ? "" : ", ") + account_label(key);
  std::cout << "Owned by:   " << (owners.empty() ? "no signed-in account" : owners) << "\n";
  std::cout << "Status:     " << s.label << (update_available(g.id) ? " (update available)" : "") << "\n";
  if (s.install) {
    std::cout << "Installed:  version " << s.install->plan.version << ", " << bytes(s.install->bytes) << "\n";
    std::cout << "Folder:     " << paths::utf8(s.install->directory) << "\n";
  }
  if (detail != details_.end() && !detail->second.version.empty())
    std::cout << "Latest:     version " << detail->second.version
              << (detail->second.size ? ", " + bytes(detail->second.size) : "") << "\n";
  if (!list.empty())
    std::cout << "Add-ons:    " << list.size() << " (venture-cli addons \"" << g.name << "\")\n";
  if (auto d = plain_description(g.description, 700); !d.empty())
    std::cout << "\n" << d;
  if (auto e = detail_errors_.find(g.id); e != detail_errors_.end())
    std::cerr << "warning: store details unavailable: " << e->second << "\n";
  return 0;
}

int Cli::install_game(const Args &args) {
  need_online();
  need_queue();
  wait_library();
  auto g = resolve_game(args.word(1, "game"));
  if (game_owners(g.id).empty())
    throw std::runtime_error(std::format("No signed-in account owns {}.", g.name));
  pick_owner(args, g);
  if (installed(g.id)) {
    wait_details(g.id);
    if (!update_available(g.id)) {
      if (json_)
        return print(game_json(g)), 0;
      std::cout << g.name << " is installed and up to date.\n";
      return 0;
    }
  }
  const auto home = account_key_;
  auto restore = [&] {
    // Finding the owner switches accounts as the window does; the command
    // leaves the active account as it found it.
    if (!home.empty() && account_key_ != home)
      action([&] { switch_account(home); });
  };
  try {
    auto entry = game(g.id);
    if (!entry)
      throw std::runtime_error(std::format("{} is not in the Meta library.", g.name));
    const auto to = args.value("--to");
    install_for_owner(*entry, to ? fs::absolute(paths::from_utf8(*to)).lexically_normal() : fs::path{});
    if (!json_)
      std::cout << "Installing " << g.name << " as " << account_label(account_key_) << "\n";
    follow(g.id, "", "Downloading");
    // Judge the files from a scan that started after the job completed.
    wait_scan();
    rescan();
    wait_scan();
  } catch (...) {
    restore();
    throw;
  }
  restore();
  auto done = installed(g.id);
  if (!done)
    throw std::runtime_error("The installed files did not pass the size check.");
  if (json_)
    return print(game_json(g)), 0;
  std::cout << std::format("Installed {} (version {}, {}).\n", g.name, done->plan.version, bytes(done->bytes));
  return 0;
}

int Cli::uninstall_game(const Args &args) {
  wait_scan();
  auto g = resolve_game(args.word(1, "game"));
  auto i = installed(g.id, false);
  if (!i)
    throw std::runtime_error(std::format("{} is not installed.", g.name));
  if (!confirm(args, std::format("Uninstall {} ({})? Saves are kept.", g.name, bytes(i->bytes))))
    return 1;
  const auto copy = *i;
  uninstall(copy);
  wait_scan();
  if (json_)
    return print({{"id", g.id}, {"name", g.name}, {"uninstalled", true}}), 0;
  std::cout << "Uninstalled " << g.name << ". Its saves are kept.\n";
  return 0;
}

int Cli::import_saves_cmd(const Args &args) {
  wait_scan();
  auto g = resolve_game(args.word(1, "game"));
  const auto from = args.value("--from");
  if (from.has_value() == args.flag("--headset"))
    throw UsageError("Give --from FOLDER or --headset.");
  auto task = import_saves(g.id, from ? paths::from_utf8(*from) : fs::path{}, args.value("--serial").value_or(""));
  auto n = task(std::stop_token{});
  if (json_)
    return print({{"id", g.id}, {"files", n.files}, {"bytes", n.bytes}, {"backup", paths::utf8(n.backup)}}), 0;
  std::cout << std::format("Imported {} save files ({}) of {}.\n", n.files, bytes(n.bytes), g.name);
  if (!n.backup.empty()) std::cout << "The save data it replaced is in " << paths::utf8(n.backup) << "\n";
  return 0;
}

int Cli::play_game(const Args &args) {
  wait_scan();
  auto g = resolve_game(args.word(1, "game"));
  if (!installed(g.id))
    throw std::runtime_error(std::format("{} is not installed. Install it first: venture-cli install \"{}\"", g.name,
                                         g.name));
  std::string key = play_key(g.id);
  if (auto as = args.value("--as"))
    key = resolve_account(*as, true);
  launch_game(g.id, key, {});
  if (!json_)
    std::cout << "Starting " << g.name << "\n" << std::flush;
  wait([&] { return !launch_pending_.contains(g.id); });
  auto r = running_.find(g.id);
  if (r == running_.end())
    throw std::runtime_error(notice_.empty() ? "The game did not start." : notice_);
  if (!json_)
    std::cout << "Running. Log: " << paths::utf8(r->second.log) << "\n" << std::flush;
  bool stopped = false;
  try {
    wait([&] { return r->second.exit.has_value(); });
  } catch (const Interrupted &) {
    // The game quits through the Activity lifecycle, as it would when this
    // process exits. A second Ctrl+C leaves now.
    std::cerr << "\nStopping " << g.name << "...\n";
    interrupted = 0;
    r->second.process.request_stop();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    wait([&] { return r->second.exit.has_value() || std::chrono::steady_clock::now() > until; });
    if (!r->second.exit)
      throw;
    stopped = true;
  }
  const int code = *r->second.exit;
  if (json_)
    print({{"id", g.id}, {"name", g.name}, {"exitCode", code}, {"stopped", stopped}, {"error", r->second.failure},
           {"log", paths::utf8(r->second.log)}});
  else if (code != 0)
    std::cerr << g.name << " stopped: " << r->second.failure << "\n";
  else
    std::cout << g.name << (stopped ? " stopped.\n" : " exited.\n");
  return stopped && code == 0 ? 130 : code;
}

int Cli::logs(const Args &args) {
  wait_scan();
  auto g = resolve_game(args.word(1, "game"));
  auto package = package_of(g);
  if (package.empty())
    throw std::runtime_error(std::format("{} has no installation to log.", g.name));
  auto file = data_ / "logs" / ("runtime-" + package + ".log");
  if (!fs::exists(file))
    throw std::runtime_error(std::format("{} has not run yet.", g.name));
  const auto wanted = size_t(number(Json(args.value("--lines").value_or("40"))));
  std::ifstream in(file, std::ios::binary);
  std::deque<std::string> lines;
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    lines.push_back(std::move(line));
    if (lines.size() > wanted)
      lines.pop_front();
  }
  if (json_)
    return print({{"log", paths::utf8(file)}, {"lines", lines}}), 0;
  std::cout << "# " << paths::utf8(file) << "\n";
  for (auto &l : lines)
    std::cout << l << "\n";
  return 0;
}

int Cli::addons_cmd(const Args &args) {
  const std::string sub = args.words.size() > 1 && (args.words[1] == "install" || args.words[1] == "remove")
                              ? args.words[1]
                              : "list";
  const size_t at = sub == "list" ? 1 : 2;
  wait_library();
  auto g = resolve_game(args.word(at, "game"));
  if (sub == "remove") {
    const auto &sku = args.word(at + 1, "add-on SKU");
    uninstall_addon(g.id, sku);
    wait_scan();
    if (!json_)
      std::cout << "Removed " << sku << " from " << g.name << ".\n";
    return 0;
  }
  pick_owner(args, g);
  // Add-ons are listed and downloaded with the owning account's entitlements.
  return as_owner(g, [&] { return addons_owned(args, g, sub); });
}

int Cli::addons_owned(const Args &args, const meta::Game &g, const std::string &sub) {
  const size_t at = 2;
  wait_details(g.id);
  if (sub == "install") {
    need_online();
    need_queue();
    std::vector<std::string> skus;
    if (args.flag("--all")) {
      for (auto &a : addons(g.id))
        if (a.owned && !a.installed && !a.downloading)
          skus.push_back(a.sku);
      install_all_owned_addons(g.id);
    } else {
      skus.push_back(args.word(at + 1, "add-on SKU or --all"));
      install_addon(g.id, skus.front());
    }
    for (auto &sku : skus)
      follow(g.id, sku, "Downloading add-on");
    wait_scan();
    if (!json_)
      std::cout << (skus.empty() ? "Every owned add-on is installed.\n"
                                 : std::format("Installed {} add-on(s) for {}.\n", skus.size(), g.name));
    return 0;
  }
  auto list = addons(g.id);
  if (json_) {
    Json out = Json::array();
    for (auto &a : list)
      out.push_back({{"sku", a.sku}, {"name", a.name}, {"owned", a.owned}, {"installed", a.installed},
                     {"bytes", a.size}, {"price", a.price_text}, {"error", a.error}});
    return print(out), 0;
  }
  if (list.empty()) {
    std::cout << g.name << " has no add-ons" << (addon_errors_.contains(g.id) ? " listed: " + addon_errors_[g.id] : ".")
              << "\n";
    return 0;
  }
  std::vector<std::vector<std::string>> rows;
  for (auto &a : list)
    rows.push_back({a.name, a.installed ? "installed" : a.downloading ? "downloading" : a.owned ? "owned" : a.price_text,
                    a.size ? bytes(a.size) : "", a.sku});
  table({"Add-on", "Status", "Size", "SKU"}, rows);
  return 0;
}

int Cli::downloads_cmd(const Args &args) {
  const std::string sub = args.words.size() > 1 ? args.words[1] : "list";
  if (sub == "cancel" || sub == "resume") {
    need_queue();
    wait_scan();
    auto g = resolve_game(args.word(2, "game"));
    auto job = download::base_job(downloads_->snapshot(), g.id);
    if (!job)
      throw std::runtime_error(std::format("{} has no download in the queue.", g.name));
    if (sub == "cancel") {
      downloads_->cancel(g.id);
      if (!json_)
        std::cout << "Cancelled the download of " << g.name << ".\n";
      return 0;
    }
    if (options_.offline)
      throw std::runtime_error("Downloads need a connection. Run without --offline.");
    auto owner = job->account == "local" ? std::nullopt : vault_.load(job->account);
    if (!owner && job->account != "local")
      throw std::runtime_error("Reconnect the account this download belongs to.");
    downloads_->resume(*job, owner ? owner->access_token : std::string{});
    follow(g.id, "", job->importing ? "Importing" : "Downloading");
    wait_scan();
    rescan();
    wait_scan();
    if (!json_)
      std::cout << "Finished " << g.name << ".\n";
    return 0;
  }
  if (sub != "list")
    throw UsageError(std::format("Unknown downloads command \"{}\".", sub));
  auto jobs = downloads_->snapshot();
  if (json_) {
    Json out = Json::array();
    for (auto &j : jobs)
      out.push_back({{"id", j.app_id}, {"name", j.name}, {"sku", j.sku}, {"state", job_state(j.state)},
                     {"done", j.done}, {"total", j.total}, {"error", j.error}, {"import", j.importing},
                     {"account", account_label(j.account)}});
    return print(out), 0;
  }
  if (jobs.empty()) {
    std::cout << "The download queue is empty.\n";
    return 0;
  }
  std::vector<std::vector<std::string>> rows;
  for (auto &j : jobs)
    rows.push_back({j.name + (j.sku.empty() ? "" : " (" + j.sku + ")"), job_state(j.state),
                    j.total ? std::format("{} of {}", bytes(j.done), bytes(j.total)) : "", j.error});
  table({"Download", "State", "Progress", "Error"}, rows);
  return 0;
}

int Cli::import_cmd(const Args &args) {
  need_queue();
  const bool headset = args.flag("--headset");
  // Matching an import to its Meta game reads the library.
  wait_library();
  auto &scan = headset ? import_device_ : import_local_;
  if (headset) {
    if (auto serial = args.value("--serial"))
      import_serial_ = *serial;
    scan_headset();
  } else {
    scan_local(paths::from_utf8(args.word(1, "path to import")));
  }
  wait([&] { return !scan.busy; });
  if (!scan.error.empty())
    throw std::runtime_error(scan.error);
  if (scan.items.empty())
    throw std::runtime_error("No Android apps were found to import.");
  // --only com.a,com.b imports just those packages.
  std::set<std::string> only;
  if (auto list = args.value("--only"))
    for (std::string_view rest = *list; !rest.empty();) {
      auto comma = rest.find(',');
      if (auto name = rest.substr(0, comma); !name.empty()) only.insert(std::string(name));
      rest = comma == rest.npos ? std::string_view{} : rest.substr(comma + 1);
    }
  for (auto &item : scan.items)
    if (!only.empty()) item.selected = only.contains(item.package);
  std::vector<std::vector<std::string>> rows;
  size_t ready = 0;
  for (auto &item : scan.items) {
    auto match = import_match(item);
    ready += match.importable && item.selected;
    rows.push_back({match.game ? match.game->name : item.label.empty() ? item.package : item.label, item.package,
                    item.version_name, bytes(item.bytes), item.selected ? match.status : "Skipped"});
  }
  if (!json_)
    table({"App", "Package", "Version", "Size", "Import"}, rows);
  if (!ready)
    throw std::runtime_error("Nothing here can be imported.");
  if (!confirm(args, std::format("Import {} app(s)?", ready)))
    return 1;
  std::vector<std::string> ids;
  for (auto &item : scan.items)
    if (item.selected)
      if (auto match = import_match(item); match.importable)
        ids.push_back(match.game ? match.game->id : item.package);
  const auto to = args.value("--to");
  start_import(headset, to ? fs::absolute(paths::from_utf8(*to)).lexically_normal() : fs::path{});
  for (auto &id : ids)
    follow(id, "", "Importing");
  wait_scan();
  rescan();
  wait_scan();
  if (json_)
    return print({{"imported", ids}}), 0;
  std::cout << std::format("Imported {} app(s).\n", ids.size());
  return 0;
}

int Cli::accounts_cmd(const Args &args) {
  const std::string sub = args.words.size() > 1 ? args.words[1] : "list";
  if (sub == "list") {
    if (json_) {
      Json out = Json::array();
      for (auto &a : accounts_)
        out.push_back({{"key", a.key}, {"name", account_name(a)}, {"alias", a.alias}, {"active", a.active},
                       {"primary", a.primary}, {"signedIn", a.signed_in}, {"needsReconnect", a.needs_reauth}});
      return print(out), 0;
    }
    if (accounts_.empty()) {
      std::cout << "No accounts. Add one: venture-cli accounts sign-in\n";
      return 0;
    }
    std::vector<std::vector<std::string>> rows;
    for (size_t i = 0; i < accounts_.size(); ++i) {
      auto &a = accounts_[i];
      std::string state = !a.signed_in ? "signed out" : a.needs_reauth ? "needs reconnect" : "signed in";
      if (a.primary)
        state += ", primary";
      rows.push_back({std::string(a.active ? "*" : " ") + std::to_string(i + 1), account_name(a), a.alias, state,
                      a.key});
    }
    table({" #", "Account", "Alias", "Status", "Key"}, rows);
    return 0;
  }
  if (sub == "sign-in") {
    if (options_.offline)
      throw std::runtime_error("Sign-in needs a connection. Run without --offline.");
    notice_.clear();
    add_account();
    std::string shown;
    if (!json_)
      std::cout << "Complete sign-in in the Meta window.\n" << std::flush;
    wait([&] { return !signing_in_; },
         [&] {
           if (!json_ && auth_status_ != shown && !auth_status_.empty())
             std::cout << auth_status_ << "\n" << std::flush;
           shown = auth_status_;
         });
    if (!notice_.empty())
      throw std::runtime_error(notice_);
    if (!session_ || auth_status_ == "Sign-in cancelled.")
      throw std::runtime_error("Sign-in did not finish.");
    if (!json_)
      std::cout << "Signed in as " << account_label(account_key_) << ".\n";
    return 0;
  }
  const auto key = resolve_account(args.word(2, "account"));
  const auto name = account_label(key);
  if (sub == "use")
    switch_account(key);
  else if (sub == "primary")
    mark_primary(key);
  else if (sub == "sign-out")
    sign_out_account(key);
  else if (sub == "remove") {
    const bool saves = args.flag("--delete-saves");
    if (!confirm(args, std::format("Remove {} from Venture{}?", name, saves ? " and delete its saves" : "")))
      return 1;
    remove_account(key, saves);
  } else
    throw UsageError(std::format("Unknown accounts command \"{}\".", sub));
  if (!json_)
    std::cout << (sub == "use"        ? "Active account: " + name
                  : sub == "primary"  ? "Primary account: " + name
                  : sub == "sign-out" ? "Signed out " + name + ". Its games and saves stay on this PC."
                                      : "Removed " + name + ".")
              << "\n";
  return 0;
}

constexpr std::string_view kLevels[] = {"trace", "debug", "info", "warn", "error"};

int Cli::settings_cmd(const Args &args) {
  const std::string sub = args.words.size() > 1 ? args.words[1] : "show";
  if (sub == "set") {
    const auto &key = args.word(2, "setting");
    const auto &value = args.word(3, "value");
    auto next = settings_;
    auto on_off = [&](const std::string &v) {
      if (v == "on" || v == "true" || v == "1")
        return true;
      if (v == "off" || v == "false" || v == "0")
        return false;
      throw UsageError(std::format("{} takes on or off.", key));
    };
    if (key == "games-folder")
      next.games = fs::absolute(paths::from_utf8(value)).lexically_normal();
    else if (key == "runtime")
      next.runtime = value == "default" ? library::Settings::defaults(data_).runtime
                                        : fs::absolute(paths::from_utf8(value)).lexically_normal();
    else if (key == "adb")
      next.adb = value == "auto" ? fs::path{} : fs::absolute(paths::from_utf8(value)).lexically_normal();
    else if (key == "log-level") {
      const auto it = std::ranges::find(kLevels, lower(value));
      if (it == std::end(kLevels))
        throw UsageError("log-level takes trace, debug, info, warn or error.");
      next.log_level = int(it - std::begin(kLevels));
    } else if (key == "steam-frame-passthrough") {
      next.steam_frame_passthrough = on_off(value);
    } else if (key == "console")
      next.console = on_off(value);
    else if (key == "add-to-steam")
      next.add_games_to_steam = on_off(value);
    else if (key == "add-new-to-steam")
      next.steam_add_new = on_off(value);
    else
      throw UsageError(std::format("Unknown setting \"{}\".", key));
    apply_settings(std::move(next), false);
    wait_scan();
  } else if (sub != "show") {
    throw UsageError(std::format("Unknown settings command \"{}\".", sub));
  }
  Json out{{"games-folder", paths::utf8(settings_.games)},
           {"runtime", settings_.runtime_override() ? paths::utf8(settings_.runtime) : "default"},
           {"adb", settings_.adb.empty() ? "auto" : paths::utf8(settings_.adb)},
           {"log-level", std::string(kLevels[std::clamp(settings_.log_level, 0, 4)])},
           {"steam-frame-passthrough", settings_.steam_frame_passthrough},
           {"console", settings_.console},
           {"add-to-steam", settings_.add_games_to_steam},
           {"add-new-to-steam", settings_.steam_add_new}};
  Json folders = Json::array();
  for (const auto &folder : settings_.libraries)
    folders.push_back(paths::utf8(folder));
  out["library-folders"] = std::move(folders);
  if (json_)
    return print(out), 0;
  for (auto &[k, v] : out.items())
    std::cout << std::format("{:<17}{}\n", k, v.is_string() ? v.get<std::string>() : v.is_boolean() ? (v.get<bool>() ? "on" : "off") : v.dump());
  return 0;
}

int Cli::run(const Args &args) {
  const auto &cmd = args.words.front();
  if (cmd == "status") return status(args);
  if (cmd == "library") return library_list(args);
  if (cmd == "info") return info(args);
  if (cmd == "install") return install_game(args);
  if (cmd == "uninstall") return uninstall_game(args);
  if (cmd == "play") return play_game(args);
  if (cmd == "logs") return logs(args);
  if (cmd == "addons") return addons_cmd(args);
  if (cmd == "downloads") return downloads_cmd(args);
  if (cmd == "import") return import_cmd(args);
  if (cmd == "import-saves") return import_saves_cmd(args);
  if (cmd == "accounts") return accounts_cmd(args);
  if (cmd == "settings") return settings_cmd(args);
  throw UsageError(std::format("Unknown command \"{}\".", cmd));
}

// Commands that read Meta's library start its refresh with the core.
bool needs_library(std::string_view cmd) {
  return cmd == "library" || cmd == "info" || cmd == "install" || cmd == "addons" || cmd == "import";
}
} // namespace

int run_cli(const std::vector<std::string> &argv) {
  Args args;
  try {
    args = parse(argv);
  } catch (const UsageError &e) {
    std::cerr << "error: " << e.what() << "\n\n" << kUsage;
    return 2;
  }
  if (args.flag("--version")) {
    std::cout << "Venture " << VENTURE_VERSION << "\n";
    return 0;
  }
  if (args.flag("--help") || args.words.empty() || args.words.front() == "help") {
    std::cout << kUsage;
    return args.words.empty() && !args.flag("--help") ? 2 : 0;
  }
  Options options;
  options.cli = true;
  options.automation = true;
  options.offline = args.flag("--offline");
  const auto &cmd = args.words.front();
  // Play is the launcher's direct launch: no library refresh, and the window's
  // download queue and catalog stay untouched.
  if (cmd == "play")
    options.launch_requested = true;
  std::signal(SIGINT, on_interrupt);
  std::signal(SIGTERM, on_interrupt);
#ifdef SIGBREAK
  std::signal(SIGBREAK, on_interrupt); // Ctrl+Break, or a harness stopping the process group
#endif
  // The core's warnings stay in logs/cli-<pid>.log; the terminal shows results.
  log::set_stderr(false);
  try {
    Cli cli(options, args.flag("--json"));
    cli.start(needs_library(cmd));
    return cli.run(args);
  } catch (const UsageError &e) {
    std::cerr << "error: " << e.what() << "\nSee: venture-cli --help\n";
    return 2;
  } catch (const Interrupted &) {
    std::cerr << "\nInterrupted. Queued downloads continue the next time Venture opens.\n";
    return 130;
  } catch (...) {
    std::cerr << "error: " << exception_text() << "\n";
    return 1;
  }
}
} // namespace venture::launcher

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
  SetConsoleOutputCP(CP_UTF8);
  std::vector<std::string> args;
  for (int i = 0; i < argc; ++i)
    args.push_back(venture::paths::utf8(venture::fs::path(argv[i])));
  return venture::launcher::run_cli(args);
}
#else
int main(int argc, char **argv) {
  // Sign-in runs Meta's page in a helper process started from this executable.
  if (int helper = venture::launcher::auth_browser_helper(argc, argv); helper >= 0)
    return helper;
  return venture::launcher::run_cli({argv, argv + argc});
}
#endif
