#include "common/steam_library.h"
#include "common/environment.h"
#include "common/file_ops.h"
#include "common/log.h"
#include "common/process.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <map>
#include <set>
#include <thread>
#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
#include <tlhelp32.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace venture::steam_library {
namespace {
constexpr size_t limit = 16 * 1024 * 1024;
struct Reader {
  std::string_view bytes;
  size_t pos = 0;
  size_t fields = 0;
  std::string take(size_t n) {
    if (n > bytes.size() - pos)
      throw std::runtime_error("Truncated Steam VDF.");
    auto s = std::string(bytes.substr(pos, n));
    pos += n;
    return s;
  }
  std::string string() {
    auto end = bytes.find('\0', pos);
    if (end == bytes.npos)
      throw std::runtime_error("Unterminated Steam VDF string.");
    return take(end - pos + 1);
  }
  Vdf object(unsigned depth) {
    if (depth > 64)
      throw std::runtime_error("Steam VDF nesting exceeds 64.");
    Vdf result;
    for (;;) {
      auto type = uint8_t(take(1)[0]);
      if (type == 8 || type == 11) {
        result.end = type;
        return result;
      }
      if (++fields > 200000)
        throw std::runtime_error("Steam VDF field count exceeds limit.");
      Vdf node;
      node.type = type;
      node.key = string();
      node.key.pop_back();
      if (type == 0) {
        auto o = object(depth + 1);
        node.children = std::move(o.children);
        node.end = o.end;
      } else if (type == 1)
        node.value = string();
      else if (type == 5) {
        do {
          node.value += take(2);
        } while (node.value.substr(node.value.size() - 2) !=
                 std::string(2, '\0'));
      } else if (type == 2 || type == 3 || type == 4 || type == 6)
        node.value = take(4);
      else if (type == 7 || type == 10)
        node.value = take(8);
      else if (type == 9)
        node.value = take(1);
      else
        throw std::runtime_error(
            "Unsupported Steam VDF wire type; leaving file unchanged.");
      result.children.push_back(std::move(node));
    }
  }
};
void encode(const Vdf &obj, std::string &out, unsigned depth) {
  if (depth > 64)
    throw std::runtime_error("Steam VDF nesting exceeds 64.");
  for (const auto &n : obj.children) {
    if (n.key.find('\0') != n.key.npos)
      throw std::runtime_error("Invalid Steam VDF key.");
    out += char(n.type);
    out += n.key;
    out += '\0';
    if (n.type == 0)
      encode(n, out, depth + 1);
    else
      out += n.value;
    if (out.size() > limit)
      throw std::runtime_error("Steam VDF exceeds size limit.");
  }
  out += char(obj.end);
}
Vdf *field(Vdf &obj, std::string_view key) {
  Vdf *result = nullptr;
  for (auto &n : obj.children)
    if (n.key == key) {
      if (result)
        throw std::runtime_error(
            "Duplicate Steam VDF field; leaving file unchanged.");
      result = &n;
    }
  return result;
}
std::string text(Vdf &obj, std::string_view key) {
  auto n = field(obj, key);
  return n && n->type == 1 && !n->value.empty()
             ? n->value.substr(0, n->value.size() - 1)
             : "";
}
void put(Vdf &obj, std::string key, std::string value, uint8_t type = 1) {
  if (type == 1)
    value += '\0';
  Vdf n;
  n.key = std::move(key);
  n.value = std::move(value);
  n.type = type;
  if (auto old = field(obj, n.key))
    *old = std::move(n);
  else
    obj.children.push_back(std::move(n));
}
std::string integer(uint32_t n) {
  std::string s;
  for (int i = 0; i < 4; ++i)
    s += char(n >> (i * 8));
  return s;
}
uint32_t number(Vdf *n) {
  if (!n || n->type != 2 || n->value.size() != 4)
    return 0;
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i)
    v |= uint32_t(uint8_t(n->value[i])) << (8 * i);
  return v;
}
// Venture in a Flatpak sandbox (FLATPAK_ID): Steam, outside it, starts it
// through the host's flatpak command.
bool sandboxed() {
#ifdef _WIN32
  return false;
#else
  const char *id = std::getenv("FLATPAK_ID");
  return id && *id && paths::safe_name(id);
#endif
}
#ifndef _WIN32
// A program on PATH, or empty.
fs::path on_path(std::string_view name) {
  const char *path = std::getenv("PATH");
  std::string_view dirs = path ? path : "/usr/local/bin:/usr/bin:/bin";
  while (!dirs.empty()) {
    const auto colon = dirs.find(':');
    const auto dir = dirs.substr(0, colon);
    dirs = colon == std::string_view::npos ? std::string_view{} : dirs.substr(colon + 1);
    if (dir.empty()) continue;
    const auto candidate = fs::path(std::string(dir)) / std::string(name);
    if (access(candidate.c_str(), X_OK) == 0) return candidate;
  }
  return {};
}
#endif
#ifndef _WIN32
// The parent of a process (from /proc/<pid>/stat), or 0.
pid_t parent_of(pid_t pid) {
  try {
    // "<pid> (<comm>) <state> <ppid> ...": comm may hold spaces and parentheses.
    const auto stat = paths::read(fs::path("/proc") / std::to_string(pid) / "stat", 4096);
    const auto close = stat.rfind(')');
    if (close == std::string::npos || close + 4 > stat.size()) return 0;
    const auto rest = std::string_view(stat).substr(close + 4);
    long parent = 0;
    const auto [end, error] = std::from_chars(rest.data(), rest.data() + rest.size(), parent);
    return error == std::errc{} ? pid_t(parent) : 0;
  } catch (...) {
    return 0;
  }
}
// This user's processes: pid and directory.
template <class F> void each_process(F &&f) {
  std::error_code error;
  fs::directory_iterator it("/proc", error);
  if (error) return;
  for (auto &e : it) {
    const auto name = e.path().filename().string();
    if (name.empty() || !std::ranges::all_of(name, [](char c) { return c >= '0' && c <= '9'; })) continue;
    struct stat st{};
    if (stat(e.path().c_str(), &st) || st.st_uid != getuid()) continue;
    f(pid_t(std::stol(name)), e.path());
  }
}
#endif
// What a shortcut runs: the quoted program and folder, and the arguments
// before "--launch <package>".
struct Command {
  std::string exe, start, prefix;
};
Command command(const fs::path &executable) {
  if (sandboxed())
    return {"\"/usr/bin/flatpak\"", "\"/usr/bin\"", std::string("run ") + std::getenv("FLATPAK_ID") + " "};
  return {"\"" + paths::utf8(fs::absolute(executable)) + "\"",
          "\"" + paths::utf8(fs::absolute(executable).parent_path()) + "\"", ""};
}
bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y));
         });
}
// A string field in any letter case: Steam reads keys regardless of case and
// may spell those of the shortcuts it makes its own way.
std::string text_any(const Vdf &obj, std::string_view key) {
  for (const auto &n : obj.children)
    if (n.type == 1 && iequals(n.key, key) && !n.value.empty()) return n.value.substr(0, n.value.size() - 1);
  return {};
}
std::string unquoted(std::string value) {
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
  return value;
}
// The desktop entries Steam's live add makes shortcuts of (LiveAdd).
fs::path entry_folder() { return paths::data_dir() / "steam-shortcuts"; }
fs::path entry_file(std::string_view package) { return entry_folder() / paths::from_utf8(std::string(package) + ".desktop"); }
// The package of a shortcut Steam made from one of those entries: Steam keeps
// the entry's path (ShortcutPath; Heroic matches the same way).
std::string entry_package(const Vdf &entry) {
  auto folder = paths::utf8(entry_folder() / "x");  // with its separator
  folder.pop_back();
  for (auto key : {"ShortcutPath", "exe"}) {
    const auto value = unquoted(text_any(entry, key));
    if (!value.starts_with(folder) || !value.ends_with(".desktop")) continue;
    auto package = value.substr(folder.size(), value.size() - folder.size() - 8);
    if (package.find('/') == std::string::npos && paths::safe_launch_target(package)) return package;
  }
  return {};
}
std::string owned_package(Vdf &entry) {
  if (auto package = entry_package(entry); !package.empty()) return package;
  auto tags = field(entry, "tags");
  if (!tags || tags->type != 0)
    return {};
  bool ours = std::ranges::any_of(tags->children, [](const Vdf &n) {
    return n.type == 1 && n.value == std::string("Venture\0", 8);
  });
  // "--launch <package>", after the Flatpak's "run <id> " when it starts Venture.
  auto options = text(entry, "LaunchOptions");
  const auto at = options.rfind("--launch ");
  if (!ours || at == std::string::npos || (at != 0 && !options.starts_with("run ")))
    return {};
  auto package = options.substr(at + 9);
  return paths::safe_launch_target(package) ? package : "";
}
// Small bounded text KeyValues reader; no credentials or profile names are
// logged.
struct TextReader {
  std::string_view bytes;
  size_t pos = 0;
  std::string token() {
    for (;;) {
      while (pos < bytes.size() && std::isspace(uint8_t(bytes[pos])))
        ++pos;
      if (bytes.substr(pos, 2) != "//")
        break;
      pos = bytes.find('\n', pos);
      if (pos == bytes.npos)
        pos = bytes.size();
    }
    if (pos == bytes.size())
      return {};
    char c = bytes[pos++];
    if (c == '{' || c == '}')
      return std::string(1, c);
    std::string s;
    if (c == '"') {
      while (pos < bytes.size()) {
        c = bytes[pos++];
        if (c == '"')
          return s;
        if (c == '\\' && pos < bytes.size() &&
            (bytes[pos] == '"' || bytes[pos] == '\\'))
          c = bytes[pos++];
        s += c;
      }
      throw std::runtime_error("Invalid Steam loginusers VDF.");
    }
    s += c;
    while (pos < bytes.size() && !std::isspace(uint8_t(bytes[pos])) &&
           bytes[pos] != '{' && bytes[pos] != '}')
      s += bytes[pos++];
    return s;
  }
  Vdf object(unsigned depth = 0) {
    if (depth > 64)
      throw std::runtime_error("Steam loginusers nesting exceeds 64.");
    Vdf obj;
    for (;;) {
      auto k = token();
      if (k.empty()) {
        if (depth)
          throw std::runtime_error("Truncated loginusers.");
        return obj;
      }
      if (k == "}") {
        if (!depth)
          throw std::runtime_error("Unexpected loginusers brace.");
        return obj;
      }
      if (k == "{")
        throw std::runtime_error("Invalid loginusers key.");
      auto v = token();
      if (v == "{") {
        auto n = object(depth + 1);
        n.key = k;
        obj.children.push_back(std::move(n));
      } else {
        if (v == "}")
          throw std::runtime_error("Invalid loginusers value.");
        put(obj, k, v);
      }
    }
  }
};
void safe_target(const fs::path &config, const fs::path &path) {
  if (!paths::within(config, path) || is_link(path))
    throw std::runtime_error("Steam output path escapes account config.");
}
} // namespace
Vdf read_vdf(std::string_view bytes) {
  if (bytes.size() > limit)
    throw std::runtime_error("Steam VDF exceeds size limit.");
  Reader r{bytes};
  auto out = r.object(0);
  if (r.pos != bytes.size())
    throw std::runtime_error("Trailing Steam VDF bytes.");
  return out;
}
std::string write_vdf(const Vdf &obj) {
  std::string out;
  encode(obj, out, 0);
  (void)read_vdf(out);
  return out;
}
uint32_t appid(std::string_view exe, std::string_view name) {
  uint32_t crc = 0xffffffffu;
  for (auto s : {exe, name})
    for (uint8_t c : s) {
      crc ^= c;
      for (int i = 0; i < 8; ++i)
        crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
  return (crc ^ 0xffffffffu) | 0x80000000u;
}
std::string artwork_name(uint32_t id, std::string_view kind) {
  const std::map<std::string_view, std::string_view> suffix{{"portrait", "p"},
                                                            {"hero", "_hero"},
                                                            {"logo", "_logo"},
                                                            {"header", ""},
                                                            {"icon", "_icon"}};
  return std::to_string(id) + std::string(suffix.at(kind)) + ".png";
}
bool profile_sync_allowed() {
  return std::getenv("VENTURE_STEAM_ROOT") ||
      (!std::getenv("VENTURE_DATA_DIR") && !std::getenv("QUESTPORT_DATA_DIR"));
}
fs::path discover_root() {
  if (!profile_sync_allowed()) return {};
  if (auto root = std::getenv("VENTURE_STEAM_ROOT"))
    return paths::from_utf8(root); // Never fall back from an override.
#ifdef _WIN32
  wchar_t value[32768];
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                   RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS)
    return fs::path(value);
#else
  if (auto home = std::getenv("HOME"))
    for (auto relative : {".steam/steam", ".local/share/Steam",
                          ".var/app/com.valvesoftware.Steam/data/Steam",
                          ".var/app/com.valvesoftware.Steam/.steam/steam"}) {
      auto root = paths::from_utf8(home) / relative;
      if (fs::is_directory(root))
        return root;
    }
#endif
  return {};
}
namespace {
// loginusers.vdf keys vary in case between Steam clients ("MostRecent", "mostrecent").
std::string login_text(const Vdf &user, std::string_view key) {
  for (auto &n : user.children)
    if (n.type == 1 && !n.value.empty() &&
        std::ranges::equal(n.key, key, [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); }))
      return n.value.substr(0, n.value.size() - 1);
  return {};
}
} // namespace
fs::path account_config(const fs::path &root) {
  std::map<std::string, fs::path> users;
  if (fs::is_directory(root / "userdata"))
    for (auto &e : fs::directory_iterator(root / "userdata")) {
      auto key = paths::utf8(e.path().filename());
      // userdata/0 holds settings from before any sign-in, not an account.
      if (digits(key) && key != "0" && e.is_directory() && !is_link(e.path()))
        users[key] = e.path() / "config";
    }
  // The profile Steam marks most recent (older clients), else the one that signed in last (current
  // clients write only a lowercase "timestamp"), else the only signed-in account with a profile.
  std::vector<fs::path> recent, listed;
  fs::path latest;
  uint64_t latest_time = 0;
  bool latest_tied = false;
  try {
    if (fs::exists(root / "config/loginusers.vdf")) {
      auto bytes = paths::read(root / "config/loginusers.vdf", limit);
      TextReader r{bytes};
      auto doc = r.object();
      auto list = field(doc, "users");
      if (list && list->type == 0)
        for (auto &u : list->children) {
          if (!digits(u.key) || u.type != 0)
            continue;
          try {
            auto id = std::to_string(uint32_t(std::stoull(u.key)));
            if (!users.contains(id))
              continue;
            listed.push_back(users.at(id));
            if (login_text(u, "MostRecent") == "1")
              recent.push_back(users.at(id));
            auto stamp = login_text(u, "Timestamp");
            if (digits(stamp, 1, 19)) {
              auto time = std::stoull(stamp);
              if (time > latest_time) {
                latest_time = time;
                latest = users.at(id);
                latest_tied = false;
              } else if (time == latest_time) {
                latest_tied = true;
              }
            }
          } catch (const std::out_of_range &) {
          }
        }
    }
  } catch (...) {
    recent.clear();
    listed.clear();
    latest.clear();
    log::warn("steam", "Cannot parse Steam loginusers; only a single local "
                       "profile can be selected.");
  }
  fs::path config;
  if (recent.size() == 1)
    config = recent.front();
  else if (!latest.empty() && !latest_tied)
    config = latest;
  else if (listed.size() == 1)
    config = listed.front();
  else if (users.size() == 1)
    config = users.begin()->second;
  else
    throw std::runtime_error(
        "Steam profile is ambiguous or unavailable; sign in to Steam once.");
  // userdata itself may be a link (Scoop keeps it in its persist directory);
  // nothing inside it may be.
  if (is_link(config) || !paths::within(root / "userdata", config))
    throw std::runtime_error("Linked Steam account config is unsupported.");
  return config;
}
namespace {
// Whether one of the user's processes runs one of these programs (no extension; ".exe" on Windows).
bool any_process(std::initializer_list<std::string_view> names) {
#ifdef _WIN32
  auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return true; // Fail closed.
  PROCESSENTRY32W e{};
  e.dwSize = sizeof(e);
  bool found = false;
  if (Process32FirstW(snapshot, &e))
    do {
      for (auto name : names)
        found = found || !_wcsicmp(e.szExeFile, paths::from_utf8(std::string(name) + ".exe").c_str());
    } while (!found && Process32NextW(snapshot, &e));
  else
    found = true;
  CloseHandle(snapshot);
  return found;
#else
  std::error_code error;
  fs::directory_iterator it("/proc", error);
  if (error)
    return true;
  for (auto &e : it) {
    auto name = e.path().filename().string();
    if (!digits(name))
      continue;
    struct stat st{};
    if (stat(e.path().c_str(), &st) || st.st_uid != getuid())
      continue;
    try {
      auto comm = paths::read(e.path() / "comm", 256);
      for (auto name : names)
        if (comm == std::string(name) + "\n")
          return true;
    } catch (...) {
    }
  }
  return false;
#endif
}
#ifndef _WIN32
// A sandbox (the Flatpak) has its own process list. Steam reads commands from
// the FIFO ~/.steam/steam.pipe while it runs (the Flatpak exposes it), and
// opening a FIFO's write end without blocking fails with ENXIO while no one
// reads it. Nothing is written. Fails closed when the answer is unclear.
bool steam_pipe_read() {
  const char *home = std::getenv("HOME");
  if (!home || !*home) return true;
  const auto pipe = paths::from_utf8(home) / ".steam/steam.pipe";
  const int fd = open(pipe.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd >= 0) {
    close(fd);
    return true;
  }
  return errno != ENXIO && errno != ENOENT;
}
#endif
} // namespace
bool running() {
#ifndef _WIN32
  if (sandboxed()) return steam_pipe_read();
#endif
  return any_process({"steam", "steamwebhelper"});
}
// A sandbox cannot see SteamVR's processes: Steam's own refusal to quit tells.
bool vr_running() { return !sandboxed() && any_process({"vrserver", "vrmonitor"}); }
std::string restart_blocker() {
#ifdef _WIN32
  return vr_running() ? "Close SteamVR first: Steam does not quit while SteamVR is running." : "";
#else
  // A sandbox sees only its own processes.
  if (sandboxed()) return {};
  std::set<pid_t> ancestors;
  for (pid_t pid = getpid(); pid > 1 && ancestors.size() < 64; pid = parent_of(pid)) ancestors.insert(pid);
  std::string blocker;
  each_process([&](pid_t pid, const fs::path &dir) {
    if (!blocker.empty() || ancestors.contains(pid)) return;
    std::string cmdline;
    try {
      cmdline = paths::read(dir / "cmdline", 64 << 10);
    } catch (...) {
      return;
    }
    // Steam's reaper: "<steam>/ubuntu12_32/reaper SteamLaunch AppId=<id> -- <game>".
    std::vector<std::string_view> args;
    for (size_t at = 0; at < cmdline.size();) {
      const auto end = std::min(cmdline.find('\0', at), cmdline.size());
      args.push_back(std::string_view(cmdline).substr(at, end - at));
      at = end + 1;
    }
    if (args.empty() || fs::path(std::string(args[0])).filename() != "reaper" ||
        std::ranges::find(args, "SteamLaunch") == args.end())
      return;
    for (auto arg : args) {
      if (arg == "--") break;
      if (!arg.starts_with("AppId=")) continue;
      const auto id = arg.substr(6);
      if (id != "250820" && !id.empty())
        blocker = "Close the game Steam is running first (Steam app " + std::string(id) +
                  "): Steam closes it when it quits.";
    }
  });
  return blocker;
#endif
}
std::optional<LiveAdd> live_add(const fs::path &root) {
#ifdef _WIN32
  (void)root;
  return std::nullopt;
#else
  if (sandboxed()) return std::nullopt;
  // The steam command hands a running Steam its URL (as steamos-add-to-steam
  // and Heroic call it).
  auto steam = on_path("steam");
  if (steam.empty() && !root.empty() && access((root / "steam.sh").c_str(), X_OK) == 0) steam = root / "steam.sh";
  if (steam.empty()) return std::nullopt;
  LiveAdd live;
  live.steam_running = running;
  live.open = [steam](const std::string &url) {
    try {
      start_detached(steam, {url});
      return true;
    } catch (const std::exception &e) {
      log::warn("steam", "Could not hand Steam a game: {}", e.what());
      return false;
    }
  };
  return live;
#endif
}
namespace {
void client_command(const fs::path &root, const char *arg) {
  // In a sandbox Steam is the host's: asked through its steam:// handler (the
  // desktop portal), the way the sandboxed program can reach it.
  if (sandboxed()) {
    open_url(std::string_view(arg) == "-shutdown" ? "steam://exit" : "steam://open/games");
    return;
  }
#ifdef _WIN32
  auto exe = root / "steam.exe";
  // No working directory: Steam ignores -shutdown from a process started in its
  // own folder (observed with Steam's 2026 client).
  if (reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", exe.c_str(),
                                               paths::from_utf8(arg).c_str(),
                                               nullptr, SW_HIDE)) <= 32)
    throw std::runtime_error("Could not request Steam restart.");
#else
  auto exe = (root / "steam.sh").string();
  pid_t pid;
  char *args[] = {exe.data(), const_cast<char *>(arg), nullptr};
  std::vector<std::string> inherited;
  for (char **e = environ; e && *e; ++e) inherited.emplace_back(*e);
  auto entries = client_environment(inherited);
  std::vector<char *> envp;
  for (auto &entry : entries) envp.push_back(entry.data());
  envp.push_back(nullptr);
  if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, args, envp.data()))
    throw std::runtime_error("Could not request Steam restart.");
  std::thread([pid] {
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  }).detach();
#endif
}
} // namespace
void request_shutdown(const fs::path &root) {
  client_command(root, "-shutdown");
}
void start_client(const fs::path &root) { client_command(root, "-silent"); }
// Implemented separately with the same bounded decoders used by the launcher.
// A portrait from a landscape cover is made to Steam's capsule shape, framed
// as third_party/venture-steam-art/titles.json says for a known package.
bool install_art(const fs::path &source, const fs::path &destination, std::string_view kind,
                 std::string_view package);
// Whether `existing` is art an earlier sync made that a new one replaces: a
// portrait copied from the store's small icon, or made of the whole cover
// before the title's framing was known. `id` names the cached art.
bool replaced_art(const fs::path &existing, const fs::path &cache, std::string_view id, std::string_view package,
                  std::string_view kind);
// Steam keeps grid artwork as <appid><suffix> with the image's own extension;
// the file there, if any.
fs::path artwork_present(const fs::path &png) {
  std::error_code ec;
  for (auto ext : {".png", ".jpg", ".jpeg", ".webp", ".gif"})
    if (auto file = fs::path(png).replace_extension(ext); fs::is_regular_file(file, ec))
      return file;
  return {};
}
fs::path best_art(const fs::path &cache, std::string_view id,
                  std::string_view kind);
// While Steam runs its shortcuts cannot change. They need a restart only when
// a game is missing or stale there, or a removed game is still listed; art
// waits for the next sync with Steam closed.
// Empty when Steam's shortcuts are current, else why not.
std::string unlisted(const fs::path &root, const fs::path &executable,
                     const std::vector<Game> &games) {
  try {
    auto config = account_config(root);
    auto target = config / "shortcuts.vdf";
    safe_target(config, target);
    if (!fs::exists(target))
      return games.empty() ? "" : "Steam has no shortcuts file yet";
    auto doc = read_vdf(paths::read(target, limit));
    auto list = field(doc, "shortcuts");
    std::map<std::string, Vdf *> owned;
    if (list)
      for (auto &e : list->children)
        if (auto package = owned_package(e); !package.empty()) {
          // A live-added shortcut stands in for a stale one, as sync keeps it.
          auto [at, added] = owned.try_emplace(package, &e);
          if (!added && !entry_package(e).empty()) at->second = &e;
        }
    const auto [exe, start, prefix] = command(executable);
    std::set<std::string> packages;
    for (const auto &g : games) {
      if (!paths::safe_launch_target(g.package) || !packages.insert(g.package).second)
        continue;
      auto found = owned.find(g.package);
      if (found == owned.end()) return g.name + ": no shortcut yet";
      // Added while Steam ran: it starts Venture through its desktop entry.
      if (!entry_package(*found->second).empty()) {
        std::error_code ec;
        if (fs::is_regular_file(entry_file(g.package), ec)) continue;
        return g.name + ": its shortcut's desktop entry is gone";
      }
      if (auto name = text(*found->second, "appname"); name != g.name)
        return g.name + ": its shortcut is named " + name;
      if (auto old = text(*found->second, "exe"); old != exe)
        return g.name + ": its shortcut starts " + old + ", not " + exe;
      if (auto old = text(*found->second, "StartDir"); old != start)
        return g.name + ": its shortcut starts in " + old + ", not " + start;
      if (auto old = text(*found->second, "LaunchOptions"); old != prefix + "--launch " + g.package)
        return g.name + ": its shortcut runs " + old;
    }
    for (const auto &e : owned)
      if (!packages.contains(e.first)) return e.first + " is no longer installed but still has a shortcut";
    return {};
  } catch (const std::exception &e) {
    return std::string("Steam's shortcuts could not be read: ") + e.what();
  }
}
namespace {
// LiveAdd's work, while Steam runs: each game Steam lacks (or whose shortcut
// starts a program that is gone, such as an AppImage's old mount) goes in
// through Steam's handler, one at a time, each confirmed in Steam's saved
// shortcuts before the next. Its artwork follows under Steam's id.
void add_while_running(const fs::path &root, const fs::path &executable, const fs::path &art,
                       const std::vector<Game> &games, LiveAdd &live) {
  struct Listed {
    uint32_t id = 0;    // of the shortcut made from Venture's desktop entry
    uint32_t appid = 0;  // of the shortcut that works, the desktop entry's first
    bool works = false;
  };
  try {
    const auto config = account_config(root);
    const auto target = config / "shortcuts.vdf";
    safe_target(config, target);
    auto listing = [&] {
      std::map<std::string, Listed> out;
      if (!fs::exists(target)) return out;
      auto doc = read_vdf(paths::read(target, limit));
      auto list = field(doc, "shortcuts");
      if (!list) return out;
      for (auto &e : list->children) {
        const auto package = owned_package(e);
        if (package.empty()) continue;
        auto &l = out[package];
        std::error_code ec;
        const bool from_entry = !entry_package(e).empty();
        bool works = from_entry;
        if (from_entry) {
          l.id = number(field(e, "appid"));
        } else if (const auto program = paths::from_utf8(unquoted(text_any(e, "exe")));
                   !program.is_absolute() || fs::exists(program, ec)) {
          works = true;
        }
        // The shortcut that works, the live-added one first.
        if (works && (!l.appid || from_entry)) l.appid = number(field(e, "appid"));
        l.works = l.works || works;
      }
      return out;
    };
    const auto before = listing();
    for (const auto &g : games) {
      if (!paths::safe_launch_target(g.package) || live.tried.contains(g.package)) continue;
      if (!live.steam_running || !live.steam_running()) return;
      if (auto found = before.find(g.package); found != before.end() && found->second.works) {
        // Grid images may change while Steam runs, as for the games added
        // below: a portrait an earlier sync made that a new one replaces
        // (replaced_art) is replaced now. A file that cannot be is left.
        try {
          const auto destination = config / "grid" / artwork_name(found->second.appid, "portrait");
          safe_target(config, destination);
          if (const auto existing = artwork_present(destination);
              !existing.empty() && replaced_art(existing, art, g.art_id, g.package, "portrait"))
            if (const auto source = best_art(art, g.art_id, "portrait"); !source.empty())
              install_art(source, destination, "portrait", g.package);
        } catch (...) {
        }
        continue;
      }
      live.tried.insert(g.package);
      fs::create_directories(entry_folder());
      const auto entry = entry_file(g.package);
      paths::atomic_write(entry, desktop_entry(executable, g));
      paths::atomic_write(live.sentinel, "");
      const bool handed = live.open(add_game_url(entry));
      Listed listed;
      const auto deadline = std::chrono::steady_clock::now() + live.wait;
      while (handed && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(live.poll);
        try {
          auto now = listing();
          if (auto found = now.find(g.package); found != now.end() && found->second.id) {
            listed = found->second;
            break;
          }
        } catch (...) {
          // Steam may be writing the file; read it again.
        }
      }
      // Never left behind: while it exists Steam's handler takes any local file.
      std::error_code ec;
      fs::remove(live.sentinel, ec);
      if (!listed.id) {
        log::warn("steam", "Steam did not add {} while running; it waits for Steam to close.", g.name);
        return;
      }
      log::info("steam", "Steam added {} while running.", g.name);
      for (auto kind : {"portrait", "hero", "logo", "header", "icon"}) {
        const auto destination = config / "grid" / artwork_name(listed.id, kind);
        safe_target(config, destination);
        if (const auto existing = artwork_present(destination);
            !existing.empty() && !replaced_art(existing, art, g.art_id, g.package, kind))
          continue;
        if (const auto source = best_art(art, g.art_id, kind); !source.empty())
          install_art(source, destination, kind, g.package);
      }
    }
  } catch (const std::exception &e) {
    log::warn("steam", "Steam could not add games while running: {}", e.what());
  }
}
// Desktop Entry Specification: a string value's escapes, and an Exec argument
// quoted (inside the quotes ", `, $ and \ take a backslash; % is %%).
std::string entry_string(std::string_view value) {
  std::string out;
  for (char c : value) {
    if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\t') out += "\\t";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  if (!out.empty() && out.front() == ' ') out = "\\s" + out.substr(1);
  return out;
}
std::string exec_argument(std::string_view value) {
  std::string out = "\"";
  for (char c : value) {
    if (c == '"' || c == '`' || c == '$' || c == '\\') out += '\\';
    if (c == '%') out += '%';
    out += c;
  }
  return out + "\"";
}
} // namespace

std::string desktop_entry(const fs::path &executable, const Game &game) {
  const auto program = fs::absolute(executable);
  return "[Desktop Entry]\nType=Application\nName=" + entry_string(game.name) + "\nExec=" +
         entry_string(exec_argument(paths::utf8(program)) + " --launch " + exec_argument(game.package)) +
         "\nPath=" + entry_string(paths::utf8(program.parent_path())) + "\nTerminal=false\nNoDisplay=true\n";
}

std::string add_game_url(const fs::path &entry) {
  std::string out = "steam://addnonsteamgame/";
  constexpr char hex[] = "0123456789ABCDEF";
  for (unsigned char c : paths::utf8(entry)) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += char(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

Result sync(const fs::path &root, const fs::path &executable,
            const fs::path &art, const std::vector<Game> &games,
            const std::function<bool()> &probe, LiveAdd *live) {
  if (root.empty())
    return {};
  auto deferred = [&]() -> Result {
    auto reason = unlisted(root, executable, games);
    return {!reason.empty(), games.size(), std::move(reason)};
  };
  if (probe()) {
    if (live) add_while_running(root, executable, art, games, *live);
    return deferred();
  }
  auto config = account_config(root);
  auto target = config / "shortcuts.vdf";
  safe_target(config, target);
  safe_target(config, config / "shortcuts.vdf.venture.bak");
  // Serializes Venture writers without creating a lock file in Steam's tree.
  FileLock lock(paths::data_dir() / "steam-sync.lock");
  const bool existed = fs::exists(target);
  auto old = existed ? paths::read(target, limit) : std::string{};
  Vdf doc = existed ? read_vdf(old) : Vdf{};
  auto list = field(doc, "shortcuts");
  if (!list) {
    Vdf n;
    n.key = "shortcuts";
    doc.children.push_back(n);
    list = &doc.children.back();
  }
  if (list->type != 0)
    throw std::runtime_error("Steam shortcuts is not an object.");
  std::map<std::string, Vdf> prior;
  std::vector<Vdf> retained;
  std::set<std::string> adopted;  // added while Steam ran, from desktop entries
  for (auto &e : list->children) {
    auto package = owned_package(e);
    if (package.empty()) {
      retained.push_back(e);
      continue;
    }
    // A live-added shortcut wins over a stale one of the same game: it has the
    // id Steam knows the game by now.
    const bool from_entry = !entry_package(e).empty();
    if (from_entry) adopted.insert(package);
    if (from_entry && prior.contains(package) && entry_package(prior.at(package)).empty())
      prior.at(package) = e;
    else
      prior.try_emplace(package, e);
  }
  std::set<std::string> packages, keys;
  for (auto &e : retained)
    keys.insert(e.key);
  const auto [exe, start, prefix] = command(executable);
  // Reserve existing keys; foreign entries keep even their original index keys.
  size_t next = 0;
  struct Art {
    fs::path source, destination;
    size_t index;
    std::string_view kind, package;
  };
  std::vector<Art> artwork;
  for (const auto &g : games) {
    if (!paths::safe_launch_target(g.package) || !packages.insert(g.package).second)
      continue;
    Vdf entry = prior.contains(g.package) ? prior.at(g.package) : Vdf{};
    auto id = number(field(entry, "appid"));
    if (!id)
      id = appid(exe, g.name);
    while (keys.contains(std::to_string(next)))
      ++next;
    entry.key = std::to_string(next++);
    keys.insert(entry.key);
    // One spelling per key: Steam's own may differ in case from ours.
    std::erase_if(entry.children, [](const Vdf &n) {
      for (std::string_view key : {"appid", "appname", "exe", "StartDir", "LaunchOptions", "ShortcutPath"})
        if (iequals(n.key, key) && n.key != key) return true;
      return false;
    });
    put(entry, "appid", integer(id), 2);
    put(entry, "appname", g.name);
    put(entry, "exe", exe);
    put(entry, "StartDir", start);
    put(entry, "LaunchOptions", prefix + "--launch " + g.package);
    if (field(entry, "ShortcutPath")) put(entry, "ShortcutPath", "");
    put(entry, "OpenVR", integer(1), 2);
    put(entry, "AllowOverlay", integer(1), 2);
    put(entry, "AllowDesktopConfig", integer(1), 2);
    if (!field(entry, "tags")) {
      Vdf tags;
      tags.key = "tags";
      entry.children.push_back(tags);
    }
    if (auto *tags = field(entry, "tags"); tags->type == 0 &&
        std::ranges::none_of(tags->children, [](const Vdf &n) { return n.type == 1 && n.value == std::string("Venture\0", 8); })) {
      size_t index = 0;
      while (field(*tags, std::to_string(index))) ++index;
      put(*tags, std::to_string(index), "Venture");
    }
    for (auto kind : {"portrait", "hero", "logo", "header", "icon"}) {
      auto destination = config / "grid" / artwork_name(id, kind);
      safe_target(config, destination);
      // Artwork is added once. What Steam already has there, ours from an
      // earlier sync or a custom image the user set in Steam (any image
      // type), stays (but an icon copied as a portrait); a custom icon is the
      // shortcut's own icon field.
      const bool icon_kind = std::string_view(kind) == "icon";
      if (icon_kind)
        if (auto *icon = field(entry, "icon"); icon && !icon->value.empty() &&
            fs::exists(paths::from_utf8(icon->value.c_str())))
          continue;
      if (auto existing = artwork_present(destination);
          !existing.empty() && !replaced_art(existing, art, g.art_id, g.package, kind)) {
        if (icon_kind)
          put(entry, "icon", paths::utf8(existing));
        continue;
      }
      auto source = best_art(art, g.art_id, kind);
      if (source.empty())
        continue;
      artwork.push_back({source, destination, retained.size(), kind, g.package});
    }
    retained.push_back(std::move(entry));
  }
  list->children = std::move(retained);
  auto bytes = write_vdf(doc);
  if (probe())
    return deferred();
  for (auto &a : artwork) {
    if (probe())
      return deferred();
    if (install_art(a.source, a.destination, a.kind, a.package) && a.kind == "icon")
      put(list->children[a.index], "icon", paths::utf8(a.destination));
  }
  bytes = write_vdf(doc);
  if (probe())
    return deferred();
  if (bytes != old) {
    if (fs::exists(target) != existed ||
        (existed && paths::read(target, limit) != old))
      throw std::runtime_error("Steam shortcuts changed during sync; leaving "
                               "them unchanged. Retry sync.");
    if (fs::exists(target))
      paths::atomic_write(config / "shortcuts.vdf.venture.bak", old);
    paths::atomic_write(target, bytes);
  }
  // Adopted shortcuts start Venture directly now.
  for (const auto &package : adopted) {
    std::error_code ec;
    fs::remove(entry_file(package), ec);
  }
  return {false, packages.size()};
}

namespace {
bool handoff_variable(std::string_view name) {
  constexpr std::string_view exact[] = {"HOME", "USER", "LOGNAME", "LANG", "LANGUAGE", "DBUS_SESSION_BUS_ADDRESS",
                                        "WAYLAND_DISPLAY", "DISPLAY", "GAMESCOPE_WAYLAND_DISPLAY",
                                        "QUESTPORT_DATA_DIR"};
  constexpr std::string_view prefixes[] = {"XDG_", "LC_", "VENTURE_"};
  return std::ranges::find(exact, name) != std::end(exact) ||
         std::ranges::any_of(prefixes, [&](auto p) { return name.starts_with(p); });
}
} // namespace

LaunchIdentity launch_identity(const std::function<const char *(const char *)> &getenv) {
  LaunchIdentity out;
  for (const char *name : {"SteamGameId", "SteamAppId", "SteamOverlayGameId", "STEAM_COMPAT_APP_ID"})
    if (getenv(name)) out.removed.emplace_back(name);
  // Steam preloads its overlay (gameoverlayrenderer.so, one per architecture)
  // into what it starts; in the game it would hook its frames as Venture's.
  if (const char *preload = getenv("LD_PRELOAD")) {
    std::string kept;
    bool dropped = false;
    std::string_view rest(preload);
    while (!rest.empty()) {
      const auto end = rest.find_first_of(": ");
      const auto entry = rest.substr(0, end);
      rest = end == rest.npos ? std::string_view() : rest.substr(end + 1);
      if (entry.empty()) continue;
      if (fs::path(entry).filename() == "gameoverlayrenderer.so") {
        dropped = true;
        continue;
      }
      kept += (kept.empty() ? "" : ":") + std::string(entry);
    }
    if (dropped && kept.empty()) out.removed.emplace_back("LD_PRELOAD");
    else if (dropped) out.replaced.emplace_back("LD_PRELOAD", kept);
  }
  return out;
}

std::vector<std::string> client_environment(const std::vector<std::string> &environment) {
  constexpr std::string_view dropped[] = {"LD_PRELOAD", "LD_LIBRARY_PATH", "SteamGameId", "SteamAppId",
                                          "SteamOverlayGameId", "SteamEnv", "STEAM_ZENITY", "SYSTEM_PATH",
                                          "SYSTEM_LD_LIBRARY_PATH", "PATH"};
  constexpr std::string_view dropped_prefixes[] = {"STEAM_RUNTIME", "PRESSURE_VESSEL_", "STEAM_COMPAT_"};
  std::vector<std::string> out;
  std::optional<std::string> path, system_path, system_libraries;
  for (const auto &entry : environment) {
    const auto equals = entry.find('=');
    if (equals == std::string::npos) continue;
    const std::string_view name(entry.data(), equals);
    const auto value = entry.substr(equals + 1);
    if (name == "PATH") path = value;
    else if (name == "SYSTEM_PATH") system_path = value;
    else if (name == "SYSTEM_LD_LIBRARY_PATH") system_libraries = value;
    if (std::ranges::find(dropped, name) != std::end(dropped) ||
        std::ranges::any_of(dropped_prefixes, [&](auto p) { return name.starts_with(p); }))
      continue;
    out.push_back(entry);
  }
  if (system_path || path) out.push_back("PATH=" + (system_path ? *system_path : *path));
  if (system_libraries && !system_libraries->empty()) out.push_back("LD_LIBRARY_PATH=" + *system_libraries);
  return out;
}

bool started_by_steam() {
#ifdef _WIN32
  return false;
#else
  if (sandboxed()) {
    const char *id = std::getenv("SteamGameId");
    return id && *id;
  }
  pid_t pid = getppid();
  for (int depth = 0; pid > 1 && depth < 64; ++depth, pid = parent_of(pid)) {
    try {
      if (paths::read(fs::path("/proc") / std::to_string(pid) / "comm", 256) == "steam\n") return true;
    } catch (...) {
      return false;
    }
  }
  return false;
#endif
}

std::vector<std::pair<std::string, std::string>> session_environment() {
  std::vector<std::pair<std::string, std::string>> out;
#ifndef _WIN32
  for (char **e = environ; e && *e; ++e) {
    const std::string_view entry(*e);
    const auto equals = entry.find('=');
    if (equals == std::string_view::npos || !handoff_variable(entry.substr(0, equals))) continue;
    out.emplace_back(entry.substr(0, equals), entry.substr(equals + 1));
  }
#endif
  return out;
}

std::string write_handoff(const Handoff &handoff) {
  Json games = Json::array();
  for (const auto &g : handoff.games)
    games.push_back({{"package", g.package}, {"name", g.name}, {"art_id", g.art_id}});
  Json environment = Json::object();
  for (const auto &[key, value] : handoff.environment) environment[key] = value;
  return Json{{"root", paths::utf8(handoff.root)},
              {"executable", paths::utf8(handoff.executable)},
              {"art", paths::utf8(handoff.art)},
              {"games", std::move(games)},
              {"environment", std::move(environment)}}
      .dump();
}

Handoff read_handoff(std::string_view json) {
  const auto doc = parse_json(json);
  Handoff out{paths::from_utf8(str(doc, "root")), paths::from_utf8(str(doc, "executable")),
              paths::from_utf8(str(doc, "art"))};
  if (out.root.empty() || out.executable.empty())
    throw std::runtime_error("Incomplete Steam sync request.");
  if (const auto &games = venture::field(doc, "games"); games.is_array())
    for (const auto &g : games)
      out.games.push_back({str(g, "package"), str(g, "name"), str(g, "art_id")});
  if (const auto &environment = venture::field(doc, "environment"); environment.is_object())
    for (const auto &[key, value] : environment.items())
      if (value.is_string() && handoff_variable(key)) out.environment.emplace_back(key, value.get<std::string>());
  return out;
}

bool start_handoff(const fs::path &program, const fs::path &file) {
#ifdef _WIN32
  (void)program, (void)file;
  return false;
#else
  try {
    if (sandboxed()) {
      // The portal starts it in a new sandbox of this app, a child of the
      // portal service rather than of Steam.
      const auto spawn = on_path("flatpak-spawn");
      if (spawn.empty()) return false;
      start_detached(spawn, {paths::utf8(program), "--steam-sync-after-exit", paths::utf8(file)});
      return true;
    }
    const auto run = on_path("systemd-run");
    if (run.empty()) return false;
    std::string error;
    const int code = run_tool(run,
                              {"--user", "--collect", "--quiet",
                               "--unit=venture-steam-sync-" + std::to_string(getpid()), "--",
                               paths::utf8(program), "--steam-sync-after-exit", paths::utf8(file)},
                              [](std::string_view) {}, &error);
    if (code != 0) log::warn("steam", "systemd-run could not start the Steam sync helper: {}", error);
    return code == 0;
  } catch (const std::exception &e) {
    log::warn("steam", "The Steam sync helper could not start: {}", e.what());
    return false;
  }
#endif
}

HandoffResult finish_handoff(const Handoff &handoff, const std::function<bool()> &open,
                             const std::function<bool()> &is_running, const std::function<void()> &start,
                             std::chrono::milliseconds wait, std::chrono::milliseconds grace,
                             std::chrono::milliseconds poll) {
  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + wait;
  while (open()) {
    if (clock::now() >= deadline) return {false, false, "Steam did not close"};
    std::this_thread::sleep_for(poll);
  }
  HandoffResult out;
  try {
    // Right away: SteamOS's session starts Steam again within seconds, and a
    // new Steam reads its shortcuts only once its user signs in.
    const auto result = sync(handoff.root, handoff.executable, handoff.art, handoff.games, open);
    out.synced = !result.pending;
    if (result.pending) out.reason = result.reason.empty() ? "Steam started again first" : result.reason;
  } catch (const std::exception &e) {
    out.reason = e.what();
  }
  const auto back = clock::now() + grace;
  while (!is_running() && clock::now() < back) std::this_thread::sleep_for(poll);
  if (!is_running()) {
    start();
    out.started = true;
  }
  return out;
}

int run_handoff(const fs::path &file) {
#ifndef _WIN32
  // Steam's close signals the tree that started this helper, and
  // flatpak-spawn forwards signals to what it started.
  signal(SIGTERM, SIG_IGN);
  signal(SIGHUP, SIG_IGN);
  signal(SIGINT, SIG_IGN);
#endif
  Handoff handoff;
  try {
    handoff = read_handoff(paths::read(file, 1 << 20));
  } catch (...) {
    return 2;
  }
  std::error_code ec;
  fs::remove(file, ec);
#ifndef _WIN32
  for (const auto &[key, value] : handoff.environment) setenv(key.c_str(), value.c_str(), 1);
#endif
  environment::refresh();
  try {
    fs::create_directories(paths::data_dir() / "logs");
    log::set_file(paths::utf8(paths::data_dir() / "logs" / "steam-sync.log"));
  } catch (...) {
  }
  log::info("steam", "Waiting for Steam to close to add {} games.", handoff.games.size());
  // The Steam processes asked to quit: a service manager may start new ones
  // at once (a systemd unit with Restart=always). A sandbox sees none of
  // them and asks whether a Steam runs.
  std::vector<int> asked;
#ifndef _WIN32
  if (!sandboxed())
    each_process([&](pid_t pid, const fs::path &dir) {
      try {
        if (paths::read(dir / "comm", 256) == "steam\n") asked.push_back(pid);
      } catch (...) {
      }
    });
#endif
  const auto open = [&] {
    if (asked.empty()) return running();
#ifndef _WIN32
    return std::ranges::any_of(asked, [](int pid) {
      try {
        return paths::read(fs::path("/proc") / std::to_string(pid) / "comm", 256) == "steam\n";
      } catch (...) {
        return false;
      }
    });
#else
    return running();
#endif
  };
  const auto result = finish_handoff(
      handoff, open, [] { return running(); }, [&] { start_client(handoff.root); }, std::chrono::minutes(5),
      std::chrono::seconds(10), std::chrono::milliseconds(100));
  if (result.synced)
    log::info("steam", "Steam has {} Venture games.", handoff.games.size());
  else
    log::warn("steam", "Steam's shortcuts were not updated: {}", result.reason);
  if (result.started) log::info("steam", "Started Steam again.");
  log::flush();
  return result.synced ? 0 : 1;
}
} // namespace venture::steam_library
