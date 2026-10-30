#include "launcher/debug_report.h"
#include "common/environment.h"
#include "common/http/client.h"
#include "common/process.h"
#include "common/saves_migration.h"
#include "common/json.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
#include <map>
#include <regex>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
extern char **environ;
#endif

namespace venture::launcher {
namespace {
// Each log keeps its end, where a failure is. The logs share a budget that
// keeps the whole report under the paste service's 1 MB limit, most important
// first; redaction changes the size by a little at most.
constexpr size_t kRuntimeLog = 448 << 10, kSideLog = 128 << 10, kLauncherLog = 128 << 10;
constexpr size_t kReportBudget = 900'000, kPasteLimit = 1'000'000;
constexpr size_t kKeptReports = 10;
constexpr const char *kPasteDays = "90";

std::string read_file(const fs::path &file, size_t limit = 64 << 10) {
  std::ifstream in(file, std::ios::binary);
  std::string text(limit, '\0');
  in.read(text.data(), std::streamsize(limit));
  text.resize(size_t(in.gcount()));
  return text;
}

std::string local_time(std::chrono::system_clock::time_point t, const char *format = "%Y-%m-%d %H:%M:%S") {
  std::time_t time = std::chrono::system_clock::to_time_t(t);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &time);
#else
  localtime_r(&time, &tm);
#endif
  char text[64];
  return std::string(text, std::strftime(text, sizeof text, format, &tm));
}
std::string local_time(fs::file_time_type t) {
  return local_time(std::chrono::system_clock::now() +
                    std::chrono::duration_cast<std::chrono::system_clock::duration>(t - fs::file_time_type::clock::now()));
}

#ifdef _WIN32
std::string narrow(const std::wstring &s) { return paths::utf8(fs::path(s)); }

std::wstring reg_string(HKEY root, const wchar_t *key, const wchar_t *value) {
  DWORD size = 0;
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size < 2)
    return {};
  std::wstring out(size / 2, L'\0');
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, out.data(), &size) != ERROR_SUCCESS) return {};
  out.resize(wcsnlen(out.c_str(), out.size()));
  return out;
}

std::optional<DWORD> reg_dword(HKEY root, const wchar_t *key, const wchar_t *value) {
  DWORD v = 0, size = sizeof v;
  if (RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return {};
  return v;
}

// The values of a loader key (layer and runtime lists): manifest path and its
// DWORD (0 = enabled for layers).
std::vector<std::pair<std::string, DWORD>> reg_values(HKEY root, const wchar_t *key) {
  std::vector<std::pair<std::string, DWORD>> out;
  HKEY h;
  if (RegOpenKeyExW(root, key, 0, KEY_READ, &h) != ERROR_SUCCESS) return out;
  for (DWORD i = 0;; ++i) {
    wchar_t name[2048];
    DWORD name_size = DWORD(std::size(name)), type = 0, data = 0, data_size = sizeof data;
    LONG r = RegEnumValueW(h, i, name, &name_size, nullptr, &type, reinterpret_cast<BYTE *>(&data), &data_size);
    if (r == ERROR_NO_MORE_ITEMS) break;
    if (r == ERROR_SUCCESS || r == ERROR_MORE_DATA) out.emplace_back(narrow(name), type == REG_DWORD ? data : 0);
  }
  RegCloseKey(h);
  return out;
}

std::vector<std::wstring> reg_subkeys(HKEY root, const wchar_t *key) {
  std::vector<std::wstring> out;
  HKEY h;
  if (RegOpenKeyExW(root, key, 0, KEY_READ, &h) != ERROR_SUCCESS) return out;
  for (DWORD i = 0;; ++i) {
    wchar_t name[256];
    DWORD size = DWORD(std::size(name));
    if (RegEnumKeyExW(h, i, name, &size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
    out.emplace_back(name);
  }
  RegCloseKey(h);
  return out;
}

void system_section(std::string &out) {
  const wchar_t *nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
  std::string build = narrow(reg_string(HKEY_LOCAL_MACHINE, nt, L"CurrentBuildNumber"));
  auto ubr = reg_dword(HKEY_LOCAL_MACHINE, nt, L"UBR");
  out += std::format("OS: Windows {} build {}{} ({})\n", std::atoi(build.c_str()) >= 22000 ? "11" : "10", build,
                     ubr ? std::format(".{}", *ubr) : "",
                     narrow(reg_string(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion")));
  out += std::format("CPU: {} ({} threads)\n",
                     narrow(reg_string(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                       L"ProcessorNameString")),
                     std::thread::hardware_concurrency());
  MEMORYSTATUSEX memory{.dwLength = sizeof(MEMORYSTATUSEX)};
  if (GlobalMemoryStatusEx(&memory))
    out += std::format("RAM: {:.1f} GiB, {:.1f} GiB free\n", memory.ullTotalPhys / 1073741824.0,
                       memory.ullAvailPhys / 1073741824.0);
  const std::wstring display = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
  for (const auto &sub : reg_subkeys(HKEY_LOCAL_MACHINE, display.c_str())) {
    std::wstring key = display + L"\\" + sub;
    std::string name = narrow(reg_string(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverDesc"));
    if (!name.empty())
      out += std::format("GPU: {}, driver {}\n", name,
                         narrow(reg_string(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverVersion")));
  }
}

void xr_section(std::string &out) {
  const wchar_t *xr = L"SOFTWARE\\Khronos\\OpenXR\\1";
  std::string active = narrow(reg_string(HKEY_LOCAL_MACHINE, xr, L"ActiveRuntime"));
  out += std::format("Active OpenXR runtime: {}\n", active.empty() ? "(none registered)" : active);
  for (auto [root, label] : {std::pair{HKEY_LOCAL_MACHINE, "machine"}, std::pair{HKEY_CURRENT_USER, "user"}}) {
    for (const auto &[name, value] : reg_values(root, L"SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes"))
      out += std::format("OpenXR runtime ({}): {}{}\n", label, name, value ? " (disabled)" : "");
    for (const auto &[name, value] : reg_values(root, L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit"))
      out += std::format("OpenXR implicit layer ({}): {}{}\n", label, name, value ? " (disabled)" : "");
    for (const auto &[name, value] : reg_values(root, L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers"))
      out += std::format("Vulkan implicit layer ({}): {}{}\n", label, name, value ? " (disabled)" : "");
  }
}
#else
std::string first_line_with(const fs::path &file, std::string_view prefix) {
  std::ifstream in(file);
  for (std::string line; std::getline(in, line);)
    if (line.starts_with(prefix)) return line.substr(prefix.size());
  return {};
}

void system_section(std::string &out) {
  std::string os = first_line_with("/etc/os-release", "PRETTY_NAME=");
  std::erase(os, '"');
  utsname u{};
  uname(&u);
  out += std::format("OS: {} (kernel {} {})\n", os, u.release, u.machine);
  if (const char *session = std::getenv("XDG_SESSION_TYPE")) out += std::format("Session: {}\n", session);
  std::string cpu = first_line_with("/proc/cpuinfo", "model name\t: ");
  out += std::format("CPU: {} ({} threads)\n", cpu, std::thread::hardware_concurrency());
  long pages = sysconf(_SC_PHYS_PAGES), avail = sysconf(_SC_AVPHYS_PAGES), size = sysconf(_SC_PAGE_SIZE);
  out += std::format("RAM: {:.1f} GiB, {:.1f} GiB free\n", double(pages) * size / 1073741824.0,
                     double(avail) * size / 1073741824.0);
  std::error_code ec;
  for (const auto &card : fs::directory_iterator("/sys/class/drm", ec)) {
    auto name = card.path().filename().string();
    if (!name.starts_with("card") || name.find('-') != std::string::npos) continue;
    auto device = card.path() / "device";
    std::string vendor = read_file(device / "vendor"), id = read_file(device / "device");
    auto driver = fs::read_symlink(device / "driver", ec).filename().string();
    std::erase(vendor, '\n');
    std::erase(id, '\n');
    out += std::format("GPU: {}:{}, driver {}\n", vendor, id, driver);
  }
}

void xr_section(std::string &out) {
  std::vector<fs::path> config;
  if (const char *c = std::getenv("XDG_CONFIG_HOME"); c && *c) config.emplace_back(c);
  else if (const char *h = std::getenv("HOME")) config.push_back(fs::path(h) / ".config");
  config.emplace_back("/etc/xdg");
  config.emplace_back("/etc");
  std::error_code ec;
  for (const auto &dir : config) {
    auto active = dir / "openxr/1/active_runtime.json";
    if (!fs::exists(active, ec)) continue;
    auto target = fs::canonical(active, ec);
    out += std::format("Active OpenXR runtime: {} -> {}\n", paths::utf8(active), paths::utf8(target));
    break;
  }
  std::vector<fs::path> data;
  if (const char *d = std::getenv("XDG_DATA_HOME"); d && *d) data.emplace_back(d);
  else if (const char *h = std::getenv("HOME")) data.push_back(fs::path(h) / ".local/share");
  for (auto d : {"/usr/local/share", "/usr/share", "/etc"}) data.emplace_back(d);
  for (const auto &dir : data)
    for (auto [sub, label] : {std::pair{"openxr/1/api_layers/implicit.d", "OpenXR implicit layer"},
                              std::pair{"vulkan/implicit_layer.d", "Vulkan implicit layer"}})
      for (const auto &f : fs::directory_iterator(dir / sub, ec))
        out += std::format("{}: {}\n", label, paths::utf8(f.path()));
}
#endif

void environment_section(std::string &out) {
  std::vector<std::string> lines;
#ifdef _WIN32
  if (wchar_t *block = GetEnvironmentStringsW()) {
    for (wchar_t *p = block; *p; p += wcslen(p) + 1) lines.push_back(narrow(p));
    FreeEnvironmentStringsW(block);
  }
#else
  for (char **p = environ; *p; ++p) lines.emplace_back(*p);
#endif
  std::ranges::sort(lines);
  bool any = false;
  for (const auto &line : lines)
    for (std::string_view prefix : {"VENTURE_", "XR_", "VK_", "OXR_", "STEAMVR_", "PRESSURE_VESSEL_"})
      if (line.size() > prefix.size() &&
          std::equal(prefix.begin(), prefix.end(), line.begin(),
                     [](char a, char b) { return a == std::toupper(static_cast<unsigned char>(b)); })) {
        out += line + "\n";
        any = true;
        break;
      }
  if (!any) out += "(none set)\n";
}

void log_section(std::string &out, const fs::path &file, size_t limit) {
  std::error_code ec;
  auto size = fs::file_size(file, ec);
  if (ec) return;
  constexpr size_t header = 256;
  if (out.size() + header >= kReportBudget) {
    out += std::format("\n=== {} left out: the report is full ===\n", paths::utf8(file.filename()));
    return;
  }
  limit = std::min(limit, kReportBudget - out.size() - header);
  out += std::format("\n=== {} ({} KiB, written {}{}) ===\n", paths::utf8(file.filename()), (size + 1023) / 1024,
                     local_time(fs::last_write_time(file, ec)), size > limit ? ", end only" : "");
  std::string text = tail_log(file, limit);
  out += text;
  if (!text.empty() && text.back() != '\n') out += '\n';
}

// The runtime's [facts] lines (headset, FOV, audio endpoint, clock rate) from
// the whole log, which the tail section may cut.
void facts_section(std::string &out, const fs::path &file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return;
  std::string facts;
  for (std::string line; std::getline(in, line) && facts.size() < 16 * 1024;)
    if (line.find(" [facts] ") != std::string::npos) facts += line.substr(0, 1024) + "\n";
  if (!facts.empty()) out += "\n=== Session facts ===\n" + facts;
}

// The Android framework classes the game's Java needs and Venture lacks, from
// the conversion report the runtime log names ("Details: .../report.json"):
// each missing class with how many of the app's classes need it.
void missing_classes_section(std::string &out, const fs::path &log) {
  std::ifstream in(log, std::ios::binary);
  if (!in) return;
  fs::path report;
  const std::string marker = "Details: ";
  for (std::string line; std::getline(in, line);)
    if (auto at = line.find(marker); at != std::string::npos && line.find("report.json") != std::string::npos) {
      auto path = line.substr(at + marker.size());
      while (!path.empty() && (path.back() == '\r' || path.back() == ' ')) path.pop_back();
      report = paths::from_utf8(path);
    }
  if (report.empty()) return;
  Json failures;
  try {
    failures = Json::parse(read_file(report, 8 << 20)).at("failures");
  } catch (...) {
    return;
  }
  std::map<std::string, std::pair<int, std::string>> missing;  // class -> (users, first user)
  for (const auto &f : failures) {
    auto name = f.value("detail", std::string());
    if (name.empty()) continue;
    auto &[users, first] = missing[name];
    if (!users++) first = f.value("class", std::string());
  }
  if (missing.empty()) return;
  std::vector<std::pair<std::string, std::pair<int, std::string>>> sorted(missing.begin(), missing.end());
  std::ranges::stable_sort(sorted, [](auto &a, auto &b) { return a.second.first > b.second.first; });
  out += std::format("\n=== Missing Android classes ({} needed by {} app classes) ===\n", sorted.size(),
                     failures.size());
  for (std::size_t i = 0; i < sorted.size() && i < 80; ++i)
    out += std::format("{} ({}; e.g. {})\n", sorted[i].first, sorted[i].second.first, sorted[i].second.second);
}

// Keeps the newest few reports.
void prune(const fs::path &dir) {
  std::error_code ec;
  std::vector<fs::directory_entry> reports;
  for (const auto &e : fs::directory_iterator(dir, ec))
    if (e.path().extension() == ".txt") reports.push_back(e);
  if (reports.size() <= kKeptReports) return;
  std::ranges::sort(reports, std::greater{}, [](const auto &e) { return e.last_write_time(); });
  for (size_t i = kKeptReports; i < reports.size(); ++i) fs::remove(reports[i].path(), ec);
}
} // namespace

std::string redact(std::string text, const std::vector<std::string> &private_text) {
  struct Name {
    std::string text, replacement;
    bool word;  // a name, matched only as a whole word
  };
  std::vector<Name> names;
#ifdef _WIN32
  const char *home = std::getenv("USERPROFILE");
  const char *user = std::getenv("USERNAME");
  const char *host = std::getenv("COMPUTERNAME");
#else
  const char *home = std::getenv("HOME");
  const char *user = std::getenv("USER");
  char host_buffer[256] = {};
  const char *host = gethostname(host_buffer, sizeof host_buffer - 1) == 0 ? host_buffer : nullptr;
#endif
  if (home && *home) {
    std::string h = home;
    names.push_back({h, "~", false});
    std::ranges::replace(h, '\\', '/');
    names.push_back({h, "~", false});
    std::string escaped;  // as JSON writes it
    for (char c : std::string(home)) escaped += c == '\\' ? std::string("\\\\") : std::string(1, c);
    names.push_back({escaped, "~", false});
  }
  // Short names would also match ordinary words.
  if (user && std::strlen(user) >= 3) names.push_back({user, "<user>", true});
  if (host && std::strlen(host) >= 3) names.push_back({host, "<computer>", true});
  for (const auto &p : private_text)
    if (p.size() >= 3) names.push_back({p, "<account>", true});
  std::ranges::sort(names, std::greater{}, [](const auto &n) { return n.text.size(); });
  // Paths and names compare without case, as Windows does.
  auto lower = [](std::string s) {
    for (char &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  auto alnum = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
  for (const auto &name : names) {
    std::string low = lower(text), key = lower(name.text);
    std::string out;
    size_t at = 0;
    for (size_t hit = 0; (hit = low.find(key, hit)) != std::string::npos;) {
      size_t end = hit + key.size();
      if (name.word && ((hit > 0 && alnum(low[hit - 1])) || (end < low.size() && alnum(low[end])))) {
        hit = end;
        continue;
      }
      out.append(text, at, hit - at).append(name.replacement);
      at = hit = end;
    }
    if (at == 0) continue;
    out.append(text, at);
    text = std::move(out);
  }
  // Anything shaped like a credential, line by line so ordinary lines skip
  // the expressions.
  static const std::regex token(R"(\b(OC|FRL)[A-Za-z0-9]{40,}|eyJ[\w-]{8,}\.[\w-]{8,}\.[\w-]*)");
  static const std::regex keyword(
      R"(((bearer|access_token|refresh_token|password|nonce)(["']?\s*[:=]\s*["']?|\s+))[^\s"',&]{8,})",
      std::regex::icase);
  std::string out;
  out.reserve(text.size());
  for (size_t at = 0; at < text.size();) {
    size_t end = text.find('\n', at);
    end = end == std::string::npos ? text.size() : end + 1;
    std::string_view line(text.data() + at, end - at);
    std::string low = lower(std::string(line));
    if (line.find("OC") != line.npos || line.find("FRL") != line.npos || line.find("eyJ") != line.npos ||
        low.find("bearer") != std::string::npos || low.find("token") != std::string::npos ||
        low.find("password") != std::string::npos || low.find("nonce") != std::string::npos)
      out += std::regex_replace(std::regex_replace(std::string(line), token, "<removed>"), keyword, "$1<removed>");
    else
      out += line;
    at = end;
  }
  return out;
}

fs::path write_debug_report(const DebugReport &request) {
  const fs::path logs = request.data / "logs";
  std::error_code ec;
  // The game: the one asked for, else the most recently written runtime log.
  std::string package = request.package;
  if (package.empty()) {
    // libstdc++'s file clock epoch is in 2174, so present times are negative.
    auto newest = fs::file_time_type::min();
    for (const auto &e : fs::directory_iterator(logs, ec)) {
      std::string name = paths::utf8(e.path().filename());
      if (!name.starts_with("runtime-") || !name.ends_with(".log") || name.ends_with(".prev.log") ||
          name.ends_with(".console.log") || name.find("-hs_err-") != std::string::npos)
        continue;
      if (auto t = e.last_write_time(ec); !ec && t > newest) {
        newest = t;
        package = name.substr(8, name.size() - 12);
      }
    }
  }

  auto now = std::chrono::system_clock::now();
  std::string out = "Venture debug report\n";
  out += std::format("Created: {}\n", local_time(now, "%Y-%m-%d %H:%M:%S %z"));
  out += std::format("Venture: {}\n", request.version);
  if (package.empty()) out += "Game: none launched yet\n";
  else if (request.game.empty()) out += std::format("Game: {}\n", package);
  else out += std::format("Game: {} ({})\n", request.game, package);

  out += "\n=== System ===\n";
  system_section(out);
  out += "\n=== VR runtime ===\n";
  xr_section(out);
  out += "\n=== Environment ===\n";
  environment_section(out);
  for (const char *name : {"settings.json", "host-displays.json"}) {
    std::string text = read_file(request.data / name);
    if (text.empty()) continue;
    out += std::format("\n=== {} ===\n{}", name, text);
    if (text.back() != '\n') out += '\n';
  }
  if (!request.plan.empty()) {
    std::string text = read_file(request.plan);
    if (!text.empty()) out += std::format("\n=== Installed game (plan.json) ===\n{}\n", text);
  }
  if (!package.empty() && paths::safe_package(package)) {
    // Files the player put in the mods folder replace same-named installed files.
    std::string mods;
    for (const auto &e : fs::directory_iterator(mods_dir(request.data, package), ec))
      if (e.is_regular_file(ec))
        mods += std::format("{} ({} bytes)\n", paths::utf8(e.path().filename()), e.file_size(ec));
    if (!mods.empty()) out += "\n=== Mods folder ===\n" + mods;
  }

  if (!package.empty()) {
    // Crash dumps hold memory from the game and stay on this PC; listing them
    // tells a developer to ask for one.
    std::string dumps;
    for (const auto &e : fs::directory_iterator(logs, ec)) {
      std::string name = paths::utf8(e.path().filename());
      if (name.starts_with("runtime-" + package + "-") && name.ends_with(".dmp"))
        dumps += std::format("{} ({} KiB)\n", name, (e.file_size(ec) + 1023) / 1024);
    }
    if (!dumps.empty()) out += "\n=== Crash dumps on this PC (not included) ===\n" + dumps;
    const std::string base = "runtime-" + package;
    // HotSpot's report of the newest native crash, cut to what names it (a few
    // KiB of the budget): the summary, the crashing thread and its native
    // stack (which driver or layer). Registers, memory and the module and
    // environment tables are left out.
    fs::path crash;
    auto crash_time = fs::file_time_type::min();
    for (const auto &e : fs::directory_iterator(logs, ec)) {
      std::string name = paths::utf8(e.path().filename());
      if (name.starts_with(base + "-hs_err-") && name.ends_with(".log"))
        if (auto t = e.last_write_time(ec); !ec && t > crash_time) {
          crash = e.path();
          crash_time = t;
        }
    }
    if (!crash.empty()) {
      const std::string full = read_file(crash, 256 << 10);
      std::string text;
      bool frames = false;
      for (size_t at = 0; at < full.size() && text.size() < (8 << 10);) {
        size_t end = full.find('\n', at);
        if (end == std::string::npos) end = full.size();
        std::string_view line(full.data() + at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with("Native frames:") || line.starts_with("Java frames:")) frames = true;
        else if (frames && line.empty()) frames = false;
        if (frames || (line.starts_with("#") && line.size() > 2) || line.starts_with("Current thread")) {
          text += line;
          text += '\n';
        }
      }
      out += std::format("\n=== {} (written {}) ===\n{}", paths::utf8(crash.filename()), local_time(crash_time), text);
      if (!text.empty() && text.back() != '\n') out += '\n';
    }
    facts_section(out, logs / (base + ".log"));
    missing_classes_section(out, logs / (base + ".log"));
    log_section(out, logs / (base + ".log"), kRuntimeLog);
    log_section(out, logs / (base + ".console.log"), kSideLog);
    log_section(out, logs / (base + ".prev.log"), kSideLog);
    log_section(out, logs / (base + ".console.prev.log"), kSideLog);
  }
  log_section(out, logs / "launcher.log", kLauncherLog);
  // A game started from Steam or a shortcut logs its launcher separately.
  fs::path direct;
  auto direct_time = fs::file_time_type::min();
  for (const auto &e : fs::directory_iterator(logs, ec))
    if (paths::utf8(e.path().filename()).starts_with("launcher-direct-"))
      if (auto t = e.last_write_time(ec); !ec && t > direct_time) {
        direct = e.path();
        direct_time = t;
      }
  if (!direct.empty()) log_section(out, direct, kSideLog);

  const fs::path dir = request.data / "reports";
  fs::create_directories(dir);
  fs::path file = dir / std::format("venture-report-{}{}.txt", package.empty() ? "" : package + "-",
                                    local_time(now, "%Y%m%d-%H%M%S"));
  std::ofstream(file, std::ios::binary) << redact(std::move(out), request.private_text);
  prune(dir);
  return file;
}

std::string upload_debug_report(const fs::path &file, std::stop_token stop) {
  std::string text = read_file(file, kPasteLimit + 1);
  if (text.empty()) throw std::runtime_error("The report is empty.");
  if (text.size() > kPasteLimit) throw std::runtime_error("The report is too large to upload.");
  auto r = http::post("https://dpaste.com/api/v2/",
                      {{"content", text}, {"title", paths::utf8(file.filename())}, {"expiry_days", kPasteDays}},
                      stop);
  std::string url = r.body.substr(0, r.body.find_first_of("\r\n"));
  if (r.status != 201 || !url.starts_with("https://dpaste.com/"))
    throw std::runtime_error(std::format("The upload service answered {}.", r.status));
  return url;
}
} // namespace venture::launcher
