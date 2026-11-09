#include "launcher/debug_report.h"
#include "launcher/diagnosis.h"
#include "common/environment.h"
#include "common/http/client.h"
#include "common/library/library.h"
#include "common/process.h"
#include "common/saves_migration.h"
#include "common/steam_library.h"
#include "common/json.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <regex>
#include <set>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
extern char **environ;
#endif

namespace venture::launcher {
namespace {
// Each log keeps its end, where a failure is, and a long one its start. The logs share a budget that
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

// A string value; for REG_MULTI_SZ (multi = true) its first string.
std::wstring reg_string(HKEY root, const wchar_t *key, const wchar_t *value, bool multi = false) {
  const DWORD types = multi ? RRF_RT_REG_SZ | RRF_RT_REG_MULTI_SZ : RRF_RT_REG_SZ;
  DWORD size = 0;
  if (RegGetValueW(root, key, value, types, nullptr, nullptr, &size) != ERROR_SUCCESS || size < 2)
    return {};
  std::wstring out(size / 2, L'\0');
  if (RegGetValueW(root, key, value, types, nullptr, out.data(), &size) != ERROR_SUCCESS) return {};
  out.resize(wcsnlen(out.c_str(), out.size()));
  return out;
}

// A Vulkan driver manifest path, flagged when the file is gone (the loader
// then skips that driver without a message).
std::string manifest(const std::string &path) {
  std::error_code ec;
  return fs::exists(paths::from_utf8(path), ec) ? path : path + " (file missing)";
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
    if (name.empty()) continue;
    out += std::format("GPU: {}, driver {}\n", name,
                       narrow(reg_string(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverVersion")));
    // The Vulkan loader finds each adapter's driver here. With none usable it has
    // no window-surface support and ANGLE cannot start (no VK_KHR_surface).
    std::string vulkan = narrow(reg_string(HKEY_LOCAL_MACHINE, key.c_str(), L"VulkanDriverName", true));
    if (!vulkan.empty()) out += std::format("  Vulkan driver: {}\n", manifest(vulkan));
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
    for (const auto &[name, value] : reg_values(root, L"SOFTWARE\\Khronos\\Vulkan\\Drivers"))
      out += std::format("Vulkan driver ({}): {}{}\n", label, manifest(name), value ? " (disabled)" : "");
    for (const auto &[name, value] : reg_values(root, L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers"))
      out += std::format("Vulkan implicit layer ({}): {}{}\n", label, name, value ? " (disabled)" : "");
  }
}
#else
std::string gpu_names();
std::string vr_inputs();
void xr_services(std::string &out);
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
  const char *desktop = std::getenv("XDG_CURRENT_DESKTOP"), *session = std::getenv("XDG_SESSION_TYPE");
  out += std::format("Desktop: {} ({})\n", desktop ? desktop : "unknown", session ? session : "unknown session");
  std::string cpu = first_line_with("/proc/cpuinfo", "model name\t: ");
  out += std::format("CPU: {} ({} threads)\n", cpu, std::thread::hardware_concurrency());
  long pages = sysconf(_SC_PHYS_PAGES), avail = sysconf(_SC_AVPHYS_PAGES), size = sysconf(_SC_PAGE_SIZE);
  out += std::format("RAM: {:.1f} GiB, {:.1f} GiB free\n", double(pages) * size / 1073741824.0,
                     double(avail) * size / 1073741824.0);
  // Names and kernel drivers from lspci, else the PCI ids sysfs gives.
  if (auto gpus = gpu_names(); !gpus.empty()) {
    out += gpus;
  } else {
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
  out += std::format("VR input devices: {}\n", vr_inputs());
}

void xr_section(std::string &out) {
  xr_services(out);
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
  // Venture's own, the XR and graphics stacks', and the overlays and GPU
  // selections known to break VR games ("NAME=" matches one variable).
  static constexpr std::string_view wanted[] = {
      "VENTURE_", "XR_", "VK_", "OXR_", "STEAMVR_", "PRESSURE_VESSEL_", "VR_OVERRIDE=", "VR_PATHREG_OVERRIDE=",
      "LD_PRELOAD=", "DRI_PRIME=", "MESA_VK_DEVICE_SELECT=", "__NV_PRIME_RENDER_OFFLOAD=",
      "__GLX_VENDOR_LIBRARY_NAME=", "AMD_VULKAN_ICD=", "MANGOHUD", "DISABLE_MANGOHUD=", "ENABLE_VKBASALT=",
      "OBS_VKCAPTURE=", "XDG_CURRENT_DESKTOP=", "XDG_SESSION_TYPE=", "WAYLAND_DISPLAY=", "DISPLAY="};
  bool any = false;
  for (const auto &line : lines)
    for (std::string_view prefix : wanted)
      if (line.size() > prefix.size() &&
          std::equal(prefix.begin(), prefix.end(), line.begin(),
                     [](char a, char b) { return a == std::toupper(static_cast<unsigned char>(b)); })) {
        out += line + "\n";
        any = true;
        break;
      }
  if (!any) out += "(none set)\n";
}

// The settings a launch of `app_id` uses (settings.json alone does not say
// which apply): the game's own overrides, each effective value and where it
// comes from, and the environment the launch passes, which
// library::launch_environment builds for the launch itself. Variables a launch
// leaves unset are the runtime's defaults unless the launcher inherited them.
void settings_section(std::string &out, const fs::path &data, const std::string &app_id) {
  library::Settings s;
  try {
    s = library::Settings::load(data);
  } catch (const std::exception &e) {
    out += std::format("settings.json could not be read ({}); the launch used the defaults.\n", e.what());
    s = library::Settings::defaults(data);
  }
  const auto environment = library::launch_environment(s, app_id);
  // A variable's value in the launch: set by it, else inherited by the runtime.
  auto variable = [&](std::string_view name) -> std::pair<std::string, const char *> {
    for (const auto &[key, value] : environment)
      if (key == name) return {value, "set"};
    if (const char *inherited = std::getenv(std::string(name).c_str())) return {inherited, "inherited"};
    return {"", nullptr};
  };
  const auto own_display = s.game_display.find(app_id);
  const library::DisplaySettings *game = own_display == s.game_display.end() ? nullptr : &own_display->second;
  const bool own_controls = s.game_controls.contains(app_id);
  const auto global = library::global_display_defaults();
  const auto defaults = library::Settings::defaults(data);
  auto source = [](bool by_game, bool global_default) {
    return by_game ? "game override" : global_default ? "global default" : "global setting";
  };

  out += "Game overrides:";
  if (app_id.empty()) out += " (game not identified; the global settings apply)\n";
  else {
    std::string lines;
    if (game) {
      if (!game->resolution.empty())
        lines += std::format("  resolution: {}\n", library::resolution_environment(*game));
      if (!game->rate.empty()) lines += std::format("  frame rate: {}\n", library::frame_rate_environment(*game));
      if (!game->mirror.empty()) lines += std::format("  mirror: {}\n", game->mirror);
      if (!game->mirror_eye.empty()) lines += std::format("  mirror eye: {}\n", game->mirror_eye);
      if (!game->pc_audio.empty()) lines += std::format("  PC audio: {}\n", game->pc_audio);
      if (!game->overlay.empty()) lines += std::format("  performance overlay: {}\n", game->overlay);
      if (!game->texture_compression.empty())
        lines += std::format("  texture compression: {}\n", game->texture_compression);
      if (game->texture_compression_threshold)
        lines += std::format("  texture compression threshold: {}%\n", game->texture_compression_threshold);
    }
    if (own_controls) lines += std::format("  controls: {}\n", controls::encode(s.game_controls.at(app_id)));
    out += lines.empty() ? " none (the game follows the global settings)\n" : "\n" + lines;
  }

  out += "Effective settings:\n";
  const auto display = library::effective_display(s.display, game);
  {
    const auto value = library::resolution_environment(display);
    std::string text = value == "runtime" ? "the host runtime's recommended size" : value;
    for (const auto &preset : library::headset_presets)
      if (display.resolution == preset.id) text = std::format("{} ({})", preset.name, value);
    if (display.resolution == "percent") text = value + " of the host runtime's pixel count";
    out += std::format("  resolution: {} ({})\n", text,
                       source(game && !game->resolution.empty(), s.display.resolution == global.resolution));
  }
  {
    const auto value = library::frame_rate_environment(display);
    out += std::format("  frame rate: {} ({})\n", value == "runtime" ? "the host runtime's rate" : value + " Hz",
                       source(game && !game->rate.empty(), s.display.rate == global.rate));
  }
  out += std::format("  mirror: {} ({}), eye {} ({})\n", library::mirror_environment(display),
                     source(game && !game->mirror.empty(), s.display.mirror == global.mirror),
                     library::mirror_eye_environment(display),
                     source(game && !game->mirror_eye.empty(), s.display.mirror_eye == global.mirror_eye));
  out += std::format("  PC audio: {} ({})\n", library::pc_audio_environment(display),
                     source(game && !game->pc_audio.empty(), s.display.pc_audio == global.pc_audio));
  out += std::format("  performance overlay: {} ({})\n", library::overlay_environment(display),
                     source(game && !game->overlay.empty(), s.display.overlay == global.overlay));
  {
    // As the runtime reads them (texture.cpp): unset is always, a threshold
    // outside 1-100 is 50%.
    const auto [mode, mode_from] = variable("VENTURE_TEXTURE_COMPRESSION");
    const auto [threshold, threshold_from] = variable("VENTURE_TEXTURE_COMPRESSION_THRESHOLD");
    const bool by_game = game && !game->texture_compression.empty();
    const char *name = mode == "never" || mode == "0" ? "Off" : mode == "auto" || mode == "2" ? "Automatic" : "Always";
    const char *from = mode_from && std::string_view(mode_from) == "inherited"
                           ? "VENTURE_TEXTURE_COMPRESSION in the launcher's environment"
                           : source(by_game, s.texture_compression == defaults.texture_compression);
    out += std::format("  texture compression: {} ({})", name, from);
    if (std::string_view(name) == "Automatic") {
      const long parsed = threshold.empty() ? 0 : std::strtol(threshold.c_str(), nullptr, 10);
      const char *threshold_source =
          threshold_from && std::string_view(threshold_from) == "inherited"
              ? "VENTURE_TEXTURE_COMPRESSION_THRESHOLD in the launcher's environment"
              : source(by_game && game->texture_compression_threshold,
                       s.texture_compression_threshold == defaults.texture_compression_threshold);
      out += std::format(", threshold {}% ({})", parsed >= 1 && parsed <= 100 ? parsed : 50, threshold_source);
    }
    out += '\n';
  }
  out += std::format("  controls: {}\n", own_controls ? "the game's own" : "the global controls");
  out += std::format("  mixed reality (passthrough): {} ({})\n", s.passthrough ? "on" : "off",
                     source(false, s.passthrough == defaults.passthrough));
  if (s.passthrough && s.color_key) out += std::format("  passthrough color key: #{}\n", s.key_color);
  out += std::format("  game language: {}\n", s.game_language.empty() ? "the system's" : s.game_language);
  out += std::format("  log level: {} ({})\n", s.log_level, source(false, s.log_level == defaults.log_level));

  out += "Runtime toggles:\n";
  {
    // The runtime keeps translated code unless VENTURE_JIT_PTC is 0.
    const auto [ptc, from] = variable("VENTURE_JIT_PTC");
    const bool kept = ptc != "0";
    out += std::format("  keep translated code: {} ({})\n", kept ? "on" : "off",
                       from && std::string_view(from) == "inherited"
                           ? "VENTURE_JIT_PTC in the launcher's environment"
                           : source(false, s.keep_translated_code == defaults.keep_translated_code));
  }
  if (s.vulkan_layers.empty())
    out += "  overlay blocking: on, every third-party implicit Vulkan layer is left out (global default)\n";
  else {
    std::string kept;
    for (const auto &layer : s.vulkan_layers) kept += (kept.empty() ? "" : ", ") + layer;
    out += std::format("  overlay blocking: on, except the kept layers {} (global setting)\n", kept);
  }

  out += "Launch environment from these settings:\n";
  for (const auto &[key, value] : environment) out += std::format("  {}={}\n", key, value);
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
                     local_time(fs::last_write_time(file, ec)), size > limit ? ", start and end" : "");
  std::string text;
  if (size > limit) {
    // The start holds the session's setup (device, swapchains, layers), which a
    // game's log spam can push out of a plain tail.
    text = read_file(file, limit / 4);
    text.resize(text.rfind('\n') + 1);
    std::string end = tail_log(file, limit - text.size());
    out += text + std::format("... {} KiB left out ...\n", (size - text.size() - end.size() + 1023) / 1024);
    text = std::move(end);
  } else {
    text = tail_log(file, limit);
  }
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

// The Android framework classes and members the game's Java needs and Venture lacks, from
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
  out += std::format("\n=== Missing Android classes and members ({} needed by {} app classes) ===\n", sorted.size(),
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

using Clock = std::chrono::system_clock;
std::optional<Clock::time_point> modified(const fs::path &file) {
  std::error_code ec;
  auto t = fs::last_write_time(file, ec);
  if (ec) return std::nullopt;
  return Clock::now() + std::chrono::duration_cast<Clock::duration>(t - fs::file_time_type::clock::now());
}
std::string duration_text(Clock::duration d) {
  auto seconds = std::chrono::duration_cast<std::chrono::seconds>(d).count();
  if (seconds < 60) return std::format("{}s", seconds);
  if (seconds < 3600) return std::format("{}m {}s", seconds / 60, seconds % 60);
  return std::format("{}h {}m", seconds / 3600, seconds / 60 % 60);
}
std::string one_line(std::string_view text, size_t limit = 600) {
  std::string out(text.substr(0, std::min(text.size(), limit)));
  while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' ')) out.pop_back();
  size_t start = out.find_first_not_of(' ');
  return start == std::string::npos ? std::string() : out.substr(start);
}
std::vector<std::string> lines_of(std::string_view text) {
  std::vector<std::string> out;
  for (size_t at = 0; at < text.size();) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    out.emplace_back(text.substr(at, end - at));
    at = end + 1;
  }
  return out;
}

// What the runtime log says about the run: the runtime logs its wall-clock
// start ("Process started at <UTC>") and its exit code.
struct Run {
  std::optional<Clock::time_point> started, ended;
  std::string result;
  bool failed = false;
};
Run read_run(const fs::path &log, const fs::path &console) {
  Run run;
  const std::string text = tail_log(log, 4 << 20);
  run.ended = modified(log);
  if (auto c = modified(console); c && (!run.ended || *c > *run.ended)) run.ended = c;
  std::optional<int> exit;
  bool stopped = false, runtime_error = false;
  for (const auto &line : lines_of(text)) {
    // The supervisor's start comes first; its worker logs its own after it.
    constexpr std::string_view starting = "] Process started at ";
    if (auto at = line.find(starting); at != std::string::npos && !run.started) {
      int y, mo, d, h, mi, sec;
      if (std::sscanf(line.c_str() + at + starting.size(), "%4d-%2d-%2dT%2d:%2d:%2dZ", &y, &mo, &d, &h, &mi, &sec) ==
          6)
        run.started = std::chrono::sys_days(std::chrono::year(y) / mo / d) + std::chrono::hours(h) +
                      std::chrono::minutes(mi) + std::chrono::seconds(sec);
    }
    if (auto at = line.find("] Exit code: "); at != std::string::npos) {
      exit = std::atoi(line.c_str() + at + 13);
      stopped = line.find("(stop requested)") != std::string::npos;
    }
    // Before this Venture logged its exit code: a guest's own exit.
    constexpr std::string_view terminal = "] Terminal guest process exit status=";
    if (auto at = line.find(terminal); at != std::string::npos && !exit)
      exit = std::atoi(line.c_str() + at + terminal.size());
    if (line.find(" E [runtime] ") != std::string::npos) runtime_error = true;
  }
  if (auto obb = library::missing_expansion(log); !obb.empty()) {
    run.result = std::format("closed: its install lacks the expansion file {}", obb);
    run.failed = true;
  } else if (library::needed_meta_sign_in(log)) {
    run.result = "closed: it needs a signed-in Meta account that owns it";
    run.failed = true;
  } else if (library::needed_device_attestation(log)) {
    run.result = "closed: it requires Meta device attestation, which only a real Quest can provide";
    run.failed = true;
  } else if (auto dialog = library::game_dialog(log); !dialog.empty() && !stopped) {
    run.result = std::format("closed after showing the message \"{}\"", dialog);
    run.failed = true;
  } else if (stopped) {
    run.result = std::format("stopped from Venture (exit code {})", *exit);
  } else if (exit && *exit == 0) {
    run.result = "the game exited normally (exit code 0)";
  } else if (exit || runtime_error) {
    // A runtime that failed before opening its log still wrote its console.
    const std::string tail = text.empty() ? tail_log(console) : text;
    run.result = (exit ? "" : "did not start: ") + library::runtime_failure(tail, exit.value_or(1));
    run.failed = true;
  } else {
    run.result = "no exit recorded (still running, or Venture was closed while it ran)";
  }
  return run;
}

// The span system logs are matched against: the run, with a little margin.
struct Window {
  Clock::time_point from, to;
  bool contains(Clock::time_point t) const { return t >= from && t <= to; }
};

#ifndef _WIN32
fs::path which(std::string_view name) {
  const char *path = std::getenv("PATH");
  for (auto dir : std::views::split(std::string_view(path ? path : "/usr/bin:/bin"), ':')) {
    fs::path candidate = fs::path(std::string(dir.begin(), dir.end())) / name;
    if (access(candidate.c_str(), X_OK) == 0) return candidate;
  }
  return {};
}
#else
fs::path which(std::wstring_view name) {
  wchar_t dir[MAX_PATH];
  UINT n = GetSystemDirectoryW(dir, MAX_PATH);
  if (!n || n >= MAX_PATH) return {};
  return fs::path(std::wstring(dir, n)) / name;
}
#endif
// A tool's standard output, or nothing when it is missing, fails to start or
// takes longer than `timeout`.
std::string capture(const fs::path &program, const std::vector<std::string> &args,
                    std::chrono::seconds timeout = std::chrono::seconds(5)) {
  if (program.empty()) return {};
  std::string out;
  std::stop_source stop;
  std::jthread timer([&](std::stop_token done) {
    std::mutex m;
    std::unique_lock lock(m);
    std::condition_variable_any().wait_for(lock, done, timeout, [] { return false; });
    stop.request_stop();
  });
  try {
    run_tool(program, args, [&](std::string_view chunk) {
      if (out.size() < (1 << 20)) out += chunk;
    }, nullptr, stop.get_token());
  } catch (...) {
  }
  return out;
}

// Error lines from a log's end (the last `keep`), else its last lines when
// `tail` is set.
std::vector<std::string> log_errors(const fs::path &file, size_t keep, bool tail) {
  std::vector<std::string> errors, last;
  for (const auto &line : lines_of(tail_log(file, 512 << 10))) {
    auto text = one_line(line);
    if (text.empty() || noisy_line(text)) continue;
    if (error_line(text)) errors.push_back(text);
    last.push_back(text);
  }
  auto &chosen = errors.empty() && tail ? last : errors;
  if (chosen.size() > keep) chosen.erase(chosen.begin(), chosen.end() - std::ptrdiff_t(keep));
  return chosen;
}
// The newest files in `directory` written during the run whose names pass `wanted`.
std::vector<fs::path> files_in_window(const fs::path &directory, const Window &window, size_t keep,
                                      const std::function<bool(const fs::path &)> &wanted) {
  std::vector<std::pair<Clock::time_point, fs::path>> found;
  std::error_code ec;
  for (const auto &e : fs::directory_iterator(directory, ec))
    if (e.is_regular_file(ec) && wanted(e.path()))
      if (auto t = modified(e.path()); t && window.contains(*t)) found.emplace_back(*t, e.path());
  std::ranges::sort(found, std::greater{});
  std::vector<fs::path> out;
  for (size_t i = 0; i < found.size() && i < keep; ++i) out.push_back(found[i].second);
  return out;
}
std::string lower_name(const fs::path &p) {
  auto n = paths::utf8(p.filename());
  std::ranges::transform(n, n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return n;
}

// Programs that matter to a VR launch: Venture, VR runtimes and compositors, Steam.
bool relevant_process(std::string name) {
  std::ranges::transform(name, name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  for (auto key : {"venture", "openxr", "wivrn", "monado", "steamvr", "vrserver", "vrcompositor", "vrmonitor",
                   "vrstartup", "vrdashboard", "gamescope", "envision", "alvr", "ovrserver", "virtualdesktop",
                   "oculusclient", "pimax", "pressure-vessel"})
    if (name.find(key) != std::string::npos) return true;
  return false;
}
#ifdef _WIN32
void processes_section(std::string &out) {
  std::vector<std::string> found;
  if (HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snapshot != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    for (bool more = Process32FirstW(snapshot, &entry); more && found.size() < 30;
         more = Process32NextW(snapshot, &entry))
      if (auto name = narrow(entry.szExeFile); relevant_process(name) && entry.th32ProcessID != GetCurrentProcessId())
        found.push_back(std::format("{} {}", entry.th32ProcessID, name));
    CloseHandle(snapshot);
  }
  out += "\n=== VR processes running now ===\n";
  for (const auto &p : found) out += p + "\n";
  if (found.empty()) out += "(none)\n";
}
// Windows' own record of the run: application crashes (Application Error,
// Windows Error Reporting) of Venture and the VR runtime, and display driver
// resets. wevtutil's text is the system code page; anything else is dropped.
void windows_events(std::string &out, std::string &evidence, const Window &window) {
  auto utc = [](Clock::time_point t) { return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::seconds>(t)); };
  const auto when = std::format("TimeCreated[@SystemTime>='{}' and @SystemTime<='{}']", utc(window.from), utc(window.to));
  static const std::regex relevant(R"((venture|vrserver|vrcompositor|vrmonitor|vrdashboard|openxr|wivrn|vulkan|nvoglv|amdvlk|igvk))",
                                   std::regex::icase);
  struct Query {
    const char *log;
    std::string filter;
    bool all;  // every event of the filter, not only those naming a relevant program
  };
  const Query queries[] = {
      {"Application", std::format("*[System[(Level=1 or Level=2) and {}]]", when), false},
      {"System",
       std::format("*[System[Provider[@Name='Display' or @Name='nvlddmkm' or @Name='amdkmdag' or @Name='amdwddmg' "
                   "or @Name='igfx'] and (Level=1 or Level=2 or Level=3) and {}]]",
                   when),
       true}};
  std::vector<std::string> found;
  for (const auto &query : queries) {
    const auto text = capture(which(L"wevtutil.exe"),
                              {"qe", query.log, "/q:" + query.filter, "/f:text", "/c:100", "/rd:true"});
    // Blocks of "Event[n]:" followed by indented "Name: value" lines and a description.
    for (size_t at = text.find("Event["); at != std::string::npos;) {
      size_t next = text.find("Event[", at + 6);
      std::string block = text.substr(at, next == std::string::npos ? std::string::npos : next - at);
      at = next;
      if (!query.all && !std::regex_search(block, relevant)) continue;
      std::string date, source, description;
      bool in_description = false;
      for (const auto &raw : lines_of(block)) {
        auto line = one_line(raw, 300);
        if (line.starts_with("Date: ")) date = line.substr(6);
        else if (line.starts_with("Source: ")) source = line.substr(8);
        else if (line.starts_with("Description:")) in_description = true;
        else if (in_description && !line.empty() && description.size() < 400) description += (description.empty() ? "" : " ") + line;
      }
      std::string entry = std::format("{} {}: {}", date, source, description);
      std::erase_if(entry, [](char c) { return static_cast<unsigned char>(c) >= 0x80; });
      found.push_back(std::move(entry));
      if (found.size() >= 12) break;
    }
  }
  if (found.empty()) return;
  out += "Windows event log:\n";
  for (const auto &f : found) out += "  " + f + "\n", evidence += f + "\n";
}
#else
void processes_section(std::string &out) {
  std::vector<std::string> found;
  std::error_code ec;
  for (const auto &e : fs::directory_iterator("/proc", ec)) {
    auto pid = e.path().filename().string();
    if (pid.empty() || !std::ranges::all_of(pid, [](unsigned char c) { return std::isdigit(c); })) continue;
    std::string name = read_file(e.path() / "comm", 256);
    std::erase(name, '\n');
    if (relevant_process(name) && pid != std::to_string(getpid())) found.push_back(pid + " " + name);
    if (found.size() >= 30) break;
  }
  out += "\n=== VR processes running now ===\n";
  for (const auto &p : found) out += p + "\n";
  if (found.empty()) out += "(none)\n";
}
// GPU names and their kernel drivers, from lspci when it is installed.
std::string gpu_names() {
  std::string out;
  auto lines = lines_of(capture(which("lspci"), {"-nnk"}, std::chrono::seconds(3)));
  for (size_t i = 0; i < lines.size(); ++i) {
    const auto &line = lines[i];
    if (line.find("VGA compatible controller") == std::string::npos && line.find("3D controller") == std::string::npos &&
        line.find("Display controller") == std::string::npos)
      continue;
    std::string driver;
    for (size_t j = i + 1; j < lines.size() && !lines[j].empty() && std::isspace(static_cast<unsigned char>(lines[j][0])); ++j)
      if (auto at = lines[j].find("Kernel driver in use: "); at != std::string::npos) driver = lines[j].substr(at + 22);
    // "03:00.0 VGA compatible controller [0300]: AMD ... [1002:7480]"
    auto name = line.substr(line.find(": ") == std::string::npos ? 0 : line.find(": ") + 2);
    out += std::format("GPU: {}{}\n", name, driver.empty() ? "" : ", driver " + driver);
  }
  return out;
}
// Headsets and controllers the kernel sees.
std::string vr_inputs() {
  std::ifstream in("/proc/bus/input/devices");
  std::set<std::string> names;
  // "touch" as a word: Meta's Touch controllers, not touchpads or touchscreens.
  static const std::regex vr(R"((controller|gamepad|sense|oculus|rift|vive|index|quest|vr2|psvr|knuckles|\btouch\b|tracker|lighthouse|watchman|pico|bigscreen))",
                             std::regex::icase);
  for (std::string line; std::getline(in, line);)
    if (line.starts_with("N: Name=")) {
      auto name = line.substr(8);
      std::erase(name, '"');
      if (std::regex_search(name, vr)) names.insert(name);
    }
  std::string out;
  for (const auto &n : names) out += (out.empty() ? "" : ", ") + n;
  return out.empty() ? "none detected" : out;
}
// The user services and Flatpak installs of the common Linux VR runtimes.
void xr_services(std::string &out) {
  const auto systemctl = which("systemctl");
  for (auto service : {"monado.service", "wivrn.service", "wivrn-server.service"}) {
    auto load = one_line(capture(systemctl, {"--user", "show", service, "-p", "LoadState", "--value"}, std::chrono::seconds(2)));
    if (load != "loaded") continue;
    auto state = one_line(capture(systemctl, {"--user", "is-active", service}, std::chrono::seconds(2)));
    out += std::format("{}: {}\n", service, state.empty() ? "unknown" : state);
  }
  std::error_code ec;
  if (const char *home = std::getenv("HOME"))
    for (auto app : {"io.github.wivrn.wivrn", "org.monado.Monado", "org.gabmus.envision"})
      if (fs::is_directory(fs::path(home) / ".var/app" / app, ec)) out += std::format("Flatpak app data: {}\n", app);
}
#endif

void evidence_from(std::string &out, std::string_view label, const fs::path &file,
                   const std::vector<std::string> &lines) {
  if (lines.empty()) return;
  out += std::format("{}{}{}:\n", label, label.empty() ? "" : " ", paths::utf8(file));
  for (const auto &line : lines) out += "  " + line + "\n";
}

// The errors in every other log written during the run: SteamVR's, the Linux
// VR runtimes', the system journal and crash records, Windows' event log.
// Returns the evidence text for the likely-cause rules.
std::string evidence_section(std::string &out, const Window &window, [[maybe_unused]] bool failed) {
  std::string evidence;
  auto add = [&](std::string_view label, const fs::path &file, std::vector<std::string> lines) {
    for (const auto &l : lines) evidence += l + "\n";
    evidence_from(out, label, file, lines);
  };
  // SteamVR's logs (vrserver, vrcompositor, vrclient_*, vrmonitor...).
  if (auto steam = steam_library::discover_root(); !steam.empty()) {
    static const std::regex steam_log(R"(^(vr|openxr|vulkan|console|stderr|connection))", std::regex::icase);
    for (const auto &file : files_in_window(steam / "logs", window, 6, [&](const fs::path &p) {
           auto n = lower_name(p);
           return (n.ends_with(".txt") || n.ends_with(".log")) && std::regex_search(n, steam_log);
         }))
      add("SteamVR", file, log_errors(file, 5, false));
  }
#ifdef _WIN32
  windows_events(out, evidence, window);
#else
  if (const char *home = std::getenv("HOME")) {
    const fs::path h(home);
    const char *cache = std::getenv("XDG_CACHE_HOME");
    std::vector<fs::path> runtime_logs{(cache && *cache ? fs::path(cache) : h / ".cache") / "envision/logs",
                                       h / ".local/state/wivrn/wivrn-dashboard",
                                       h / ".var/app/io.github.wivrn.wivrn/.local/state/wivrn/wivrn-dashboard",
                                       h / ".var/app/org.gabmus.envision/cache/envision/logs"};
    for (const auto &dir : runtime_logs)
      for (const auto &file : files_in_window(dir, window, 2, [](const fs::path &) { return true; })) add("VR runtime", file, log_errors(file, 10, failed));
  }
  // The system journal for the run: the user's services, the kernel (GPU
  // resets, out of memory) and crashed programs.
  const auto journalctl = which("journalctl");
  const auto since = local_time(window.from), until = local_time(window.to);
  auto journal = [&](std::string_view title, std::vector<std::string> args) {
    std::vector<std::string> found;
    for (const auto &line : lines_of(capture(journalctl, args)))
      if (auto text = one_line(line); error_line(text) && !noisy_line(text)) found.push_back(text);
    if (found.size() > 10) found.erase(found.begin(), found.end() - 10);
    if (found.empty()) return;
    out += std::format("{}:\n", title);
    for (const auto &f : found) out += "  " + f + "\n", evidence += f + "\n";
  };
  journal("User journal", {"--user", "--since=" + since, "--until=" + until, "--no-pager", "-o", "short-iso", "-n",
                           "300", "--grep=(venture|openxr|wivrn|monado|envision|steamvr|vrserver|vrcompositor|"
                           "vulkan|pipewire|xdg-desktop-portal|secret|keyring)"});
  journal("Kernel", {"-k", "--since=" + since, "--until=" + until, "--no-pager", "-o", "short-iso", "-n", "400",
                     "--grep=(amdgpu|nouveau|nvidia|nvrm|i915|xe |drm|gpu|vulkan|xid|oom)"});
  std::vector<std::string> dumps;
  static const std::regex relevant(R"((venture|steam|openxr|wivrn|monado|envision|vrserver|vrcompositor))",
                                   std::regex::icase);
  for (const auto &line : lines_of(capture(which("coredumpctl"), {"--since=" + since, "--until=" + until,
                                                                  "--no-pager", "--no-legend", "list"})))
    if (line.find("steamwebhelper") == std::string::npos && std::regex_search(line, relevant))
      dumps.push_back(one_line(line));
  if (dumps.size() > 6) dumps.erase(dumps.begin(), dumps.end() - 6);
  if (!dumps.empty()) {
    out += "Crashed programs (coredumpctl):\n";
    for (const auto &d : dumps) out += "  " + d + "\n", evidence += "coredump " + d + "\n";
  }
#endif
  return evidence;
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
    // Resolved paths name the real home (rpm-ostree: /var/home/<user> behind
    // /home); it is replaced whole, before the shorter HOME it ends with.
    std::error_code ec;
    if (auto real = paths::utf8(fs::canonical(paths::from_utf8(home), ec)); !ec && real != home) {
      names.push_back({real, "~", false});
      std::ranges::replace(real, '\\', '/');
      names.push_back({real, "~", false});
    }
#ifndef _WIN32
    // A path recorded by another process (the host, outside a container)
    // may name the resolved home even where this one cannot resolve it.
    if (std::string_view(home).starts_with("/home/")) names.push_back({"/var" + std::string(home), "~", false});
#endif
  }
  // Short names would also match ordinary words.
  if (user && std::strlen(user) >= 3) names.push_back({user, "<user>", true});
  // A computer named after its distribution (bazzite, fedora) would also
  // hide the OS line; that name says nothing about the person.
  bool distribution_name = false;
#ifndef _WIN32
  if (host) {
    std::string low_host(host), os;
    for (char &c : low_host) c = char(std::tolower(static_cast<unsigned char>(c)));
    std::ifstream release("/etc/os-release");
    for (std::string line; std::getline(release, line);)
      if (line.starts_with("ID=") || line.starts_with("NAME=") || line.starts_with("PRETTY_NAME=")) os += line + " ";
    for (char &c : os) c = char(std::tolower(static_cast<unsigned char>(c)));
    distribution_name = !low_host.empty() && os.find(low_host) != std::string::npos;
  }
#endif
  if (host && std::strlen(host) >= 3 && !distribution_name) names.push_back({host, "<computer>", true});
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

  // What happened, then why: the summary a reader needs first.
  const std::string base = "runtime-" + package;
  const fs::path runtime_log = logs / (base + ".log"), console_log = logs / (base + ".console.log");
  std::optional<Run> run;
  std::optional<Window> window;
  if (!package.empty() && fs::exists(runtime_log, ec)) {
    run = read_run(runtime_log, console_log);
    const auto end = run->ended.value_or(now);
    auto start = run->started;
    // A log from before the runtime recorded its start: the previous run ended before this one began.
    if (!start) start = modified(logs / (base + ".prev.log"));
    if (!start || *start > end || end - *start > std::chrono::hours(6)) start = end - std::chrono::hours(1);
    window = Window{*start - std::chrono::seconds(5), end + std::chrono::seconds(5)};
    out += std::format("Last run: {} to {} ({}{})\n", local_time(*start), local_time(end),
                       run->started ? "" : "start estimated, ", duration_text(end - *start));
    out += std::format("Result: {}\n", run->result);
  }
  // Gathered before the summary is written, which they inform.
  std::string evidence_text, evidence;
  if (window) evidence = evidence_section(evidence_text, *window, run->failed);
  std::vector<std::string> steps;
  out += "\n=== Likely cause ===\n";
  if (!run) {
    out += package.empty() ? "No game has been started with this Venture yet.\n"
                           : std::format("{} has no run to report yet.\n", request.game.empty() ? package : request.game);
    steps.push_back("Start the game that has the problem, then send a new report.");
  } else if (!run->failed) {
    out += "The last run did not fail. If the problem happened in an earlier run, start the game again, "
           "reproduce it, then send a new report.\n";
  } else if (!library::missing_expansion(runtime_log).empty()) {
    // Not a log signature: games probe for optional patch OBBs at every start
    // (UE4 and Unity), so only a probe the game then failed on counts.
    out += "The game looked for an expansion file (OBB) that its install does not have.\n";
    steps.push_back("Install the game again, or import it again together with its OBB file.");
  } else if (auto d = diagnose(tail_log(runtime_log, 512 << 10) + "\n" + tail_log(console_log, 128 << 10) + "\n" +
                               tail_log(logs / "launcher.log", 64 << 10) + "\n" + evidence)) {
    out += d->cause + "\n";
    steps.push_back(d->advice);
  } else {
    out += "No known failure signature: the errors below are the closest leads.\n";
    steps.push_back("Send this report with an issue.");
  }
  if (!steps.empty()) {
    out += "\n=== Next steps ===\n";
    for (const auto &s : steps) out += "- " + s + "\n";
  }

  out += "\n=== System ===\n";
  system_section(out);
  out += "\n=== VR runtime ===\n";
  xr_section(out);
  processes_section(out);
  out += "\n=== Environment ===\n";
  environment_section(out);
  out += "\n=== Settings for this game ===\n";
  settings_section(out, request.data, request.app_id);
  for (const char *name : {"settings.json", "host-displays.json", "passthrough.json"}) {
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
    if (window) {
      out += std::format("\n=== Errors logged during the last run ({} to {}) ===\n", local_time(window->from),
                         local_time(window->to));
      out += evidence_text.empty() ? "No other log written during the run reports an error.\n" : evidence_text;
    }
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
