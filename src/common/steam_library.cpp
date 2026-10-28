#include "common/steam_library.h"
#include "common/file_ops.h"
#include "common/log.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <map>
#include <set>
#include <thread>
#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
#include <tlhelp32.h>
#else
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
std::string owned_package(Vdf &entry) {
  auto tags = field(entry, "tags");
  if (!tags || tags->type != 0)
    return {};
  bool ours = std::ranges::any_of(tags->children, [](const Vdf &n) {
    return n.type == 1 && n.value == std::string("Venture\0", 8);
  });
  auto options = text(entry, "LaunchOptions");
  if (!ours || !options.starts_with("--launch "))
    return {};
  auto package = options.substr(9);
  return paths::safe_package(package) ? package : "";
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
fs::path account_config(const fs::path &root) {
  std::map<std::string, fs::path> users;
  if (fs::is_directory(root / "userdata"))
    for (auto &e : fs::directory_iterator(root / "userdata")) {
      auto key = paths::utf8(e.path().filename());
      if (digits(key) && e.is_directory() && !is_link(e.path()))
        users[key] = e.path() / "config";
    }
  std::vector<fs::path> recent;
  try {
    if (fs::exists(root / "config/loginusers.vdf")) {
      auto bytes = paths::read(root / "config/loginusers.vdf", limit);
      TextReader r{bytes};
      auto doc = r.object();
      auto list = field(doc, "users");
      if (list && list->type == 0)
        for (auto &u : list->children)
          if (digits(u.key) && text(u, "MostRecent") == "1") {
            try {
              auto id = std::to_string(uint32_t(std::stoull(u.key)));
              if (users.contains(id))
                recent.push_back(users.at(id));
            } catch (const std::out_of_range &) {
            }
          }
    }
  } catch (...) {
    recent.clear();
    log::warn("steam", "Cannot parse Steam loginusers; only a single local "
                       "profile can be selected.");
  }
  fs::path config;
  if (recent.size() == 1)
    config = recent.front();
  else if (recent.empty() && users.size() == 1)
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
} // namespace
bool running() { return any_process({"steam", "steamwebhelper"}); }
bool vr_running() { return any_process({"vrserver", "vrmonitor"}); }
namespace {
void client_command(const fs::path &root, const char *arg) {
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
  if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, args, environ))
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
bool install_art(const fs::path &source, const fs::path &destination);
fs::path best_art(const fs::path &cache, std::string_view id,
                  std::string_view kind);
Result sync(const fs::path &root, const fs::path &executable,
            const fs::path &art, const std::vector<Game> &games,
            const std::function<bool()> &probe) {
  if (root.empty())
    return {};
  if (probe())
    return {true, games.size()};
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
  for (auto &e : list->children) {
    auto package = owned_package(e);
    if (package.empty())
      retained.push_back(e);
    else
      prior.try_emplace(package, e);
  }
  std::set<std::string> packages, keys;
  for (auto &e : retained)
    keys.insert(e.key);
  std::string exe = "\"" + paths::utf8(fs::absolute(executable)) + "\"";
  // Reserve existing keys; foreign entries keep even their original index keys.
  size_t next = 0;
  struct Art {
    fs::path source, destination;
    size_t index;
    bool icon;
  };
  std::vector<Art> artwork;
  for (const auto &g : games) {
    if (!paths::safe_package(g.package) || !packages.insert(g.package).second)
      continue;
    Vdf entry = prior.contains(g.package) ? prior.at(g.package) : Vdf{};
    auto id = number(field(entry, "appid"));
    if (!id)
      id = appid(exe, g.name);
    while (keys.contains(std::to_string(next)))
      ++next;
    entry.key = std::to_string(next++);
    keys.insert(entry.key);
    put(entry, "appid", integer(id), 2);
    put(entry, "appname", g.name);
    put(entry, "exe", exe);
    put(entry, "StartDir",
        "\"" + paths::utf8(fs::absolute(executable).parent_path()) + "\"");
    put(entry, "LaunchOptions", "--launch " + g.package);
    put(entry, "OpenVR", integer(1), 2);
    put(entry, "AllowOverlay", integer(1), 2);
    put(entry, "AllowDesktopConfig", integer(1), 2);
    if (!field(entry, "tags")) {
      Vdf tags;
      tags.key = "tags";
      put(tags, "0", "Venture");
      entry.children.push_back(tags);
    }
    for (auto kind : {"portrait", "hero", "logo", "header", "icon"}) {
      auto source = best_art(art, g.art_id, kind);
      if (source.empty())
        continue;
      auto destination = config / "grid" / artwork_name(id, kind);
      safe_target(config, destination);
      artwork.push_back({source, destination, retained.size(),
                         std::string_view(kind) == "icon"});
    }
    retained.push_back(std::move(entry));
  }
  list->children = std::move(retained);
  auto bytes = write_vdf(doc);
  if (probe())
    return {true, games.size()};
  for (auto &a : artwork) {
    if (probe())
      return {true, games.size()};
    if (install_art(a.source, a.destination) && a.icon)
      put(list->children[a.index], "icon", paths::utf8(a.destination));
  }
  bytes = write_vdf(doc);
  if (probe())
    return {true, games.size()};
  if (bytes != old) {
    if (fs::exists(target) != existed ||
        (existed && paths::read(target, limit) != old))
      throw std::runtime_error("Steam shortcuts changed during sync; leaving "
                               "them unchanged. Retry sync.");
    if (fs::exists(target))
      paths::atomic_write(config / "shortcuts.vdf.venture.bak", old);
    paths::atomic_write(target, bytes);
  }
  return {false, packages.size()};
}
} // namespace venture::steam_library
