#include "common/environment.h"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
extern char** environ;
#endif

namespace venture::environment {
namespace {
constexpr unsigned char fold(unsigned char c) {
#ifdef _WIN32
  return c >= 'A' && c <= 'Z' ? c + ('a'-'A') : c;
#else
  return c;
#endif
}
struct Hash {
  using is_transparent = void;
  std::size_t operator()(std::string_view s) const noexcept {
    std::size_t h = 14695981039346656037ull;
    for (unsigned char c : s) h = (h ^ fold(c)) * 1099511628211ull;
    return h;
  }
};
struct Equal {
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i=0; i<a.size(); ++i) if (fold(a[i]) != fold(b[i])) return false;
    return true;
  }
};
using Snapshot = std::unordered_map<std::string,std::string,Hash,Equal>;
struct State {
  std::atomic<const Snapshot*> current{nullptr};
  std::mutex mutex;
  // Returned pointers remain valid even across refresh and static destruction.
  std::vector<std::unique_ptr<const Snapshot>> retained;
  std::atomic_flag warned = ATOMIC_FLAG_INIT;
};
State& state() { static auto* value = new State; return *value; }
const Snapshot* capture(State& state) {
  auto next = std::make_unique<Snapshot>();
  bool legacy = false;
  auto add = [&](std::string_view entry) {
    auto split = entry.find('=');
    if (split == entry.npos || split == 0) return; // Windows drive entries are not variables.
    auto name = entry.substr(0,split);
    legacy |= name.size() >= 3 && fold(name[0]) == fold('Q') &&
              fold(name[1]) == fold('P') && name[2] == '_';
    next->try_emplace(std::string(name), std::string(entry.substr(split+1)));
  };
#ifdef _WIN32
  // Values are UTF-8, as every caller reads them (paths::from_utf8). The ANSI
  // code page turned a non-ASCII PATH entry (a Turkish user folder) into bytes
  // the launcher could not read ("locale not supported" looking for adb).
  auto* block = GetEnvironmentStringsW();
  if (block) {
    for (auto* p=block; *p; ) {
      const std::wstring_view entry(p);
      const int length = WideCharToMultiByte(CP_UTF8,0,p,static_cast<int>(entry.size()),nullptr,0,nullptr,nullptr);
      std::string bytes(length, '\0');
      if (length && WideCharToMultiByte(CP_UTF8,0,p,static_cast<int>(entry.size()),bytes.data(),length,nullptr,nullptr)) add(bytes);
      p+=entry.size()+1;
    }
    FreeEnvironmentStringsW(block);
  }
#else
  for (auto** p=environ; p && *p; ++p) add(*p);
#endif
  auto* published = next.get();
  state.retained.push_back(std::move(next));
  state.current.store(published,std::memory_order_release);
  // No values or individual names are logged. Avoid logger initialization recursion.
  if (legacy && !state.warned.test_and_set())
    std::fputs("[environment] Legacy QP_* settings are ignored; use VENTURE_* settings.\n", stderr);
  return published;
}
}
const char* getenv(const char* name) {
  if (!name) return nullptr;
  auto& shared = state();
  auto* snapshot = shared.current.load(std::memory_order_acquire);
  if (!snapshot) {
    std::lock_guard lock(shared.mutex);
    snapshot = shared.current.load(std::memory_order_acquire);
    if (!snapshot) snapshot = capture(shared);
  }
  auto entry = snapshot->find(std::string_view(name));
  return entry == snapshot->end() ? nullptr : entry->second.c_str();
}
void refresh() {
  auto& shared = state();
  std::lock_guard lock(shared.mutex);
  capture(shared);
}
std::string gamescope_wayland_display(const char* wayland, const char* gamescope, const char* runtime_dir) {
  if ((wayland && *wayland) || !gamescope || !*gamescope) return {};
  std::filesystem::path socket(gamescope);
  if (socket.is_relative()) {
    if (!runtime_dir || !*runtime_dir) return {};
    socket = std::filesystem::path(runtime_dir) / socket;
  }
  std::error_code ec;
  return std::filesystem::is_socket(socket, ec) ? std::string(gamescope) : std::string();
}
bool use_gamescope_wayland() {
#if defined(_WIN32) || defined(__ANDROID__)
  return false;
#else
  const auto display = gamescope_wayland_display(std::getenv("WAYLAND_DISPLAY"),
                                                 std::getenv("GAMESCOPE_WAYLAND_DISPLAY"),
                                                 std::getenv("XDG_RUNTIME_DIR"));
  if (display.empty() || setenv("WAYLAND_DISPLAY", display.c_str(), 1) != 0) return false;
  refresh();
  return true;
#endif
}
bool disable_host_foveation_layer() {
  constexpr const char* name = "DISABLE_VULKAN_FDM_INJECTION_LAYER";
  if (std::getenv(name)) return false;
  // Foveated rendering off in Venture: the host's layer stays as the host set it.
  if (const char* mode = std::getenv("VENTURE_FOVEATION"); mode && std::string_view(mode) == "off") return false;
#ifdef _WIN32
  if (_putenv_s(name, "1") != 0) return false;
#else
  if (setenv(name, "1", 0) != 0) return false;
#endif
  refresh();
  return true;
}
bool inside_gamescope() {
  const auto* gamescope = getenv("GAMESCOPE_WAYLAND_DISPLAY");
  const auto* wayland = getenv("WAYLAND_DISPLAY");
  return gamescope && *gamescope && wayland && std::string_view(gamescope) == wayland;
}
bool gamescope_xwayland() {
  const auto* display = getenv("DISPLAY");
  return inside_gamescope() && display && *display;
}
bool passthrough_enabled() {
  const auto* value = getenv("VENTURE_PASSTHROUGH");
  return !value || std::string_view(value) != "0";
}
std::optional<std::array<std::uint8_t, 3>> parse_color_key(std::string_view text) {
  if (!text.empty() && text.front() == '#') text.remove_prefix(1);
  if (text.size() != 6) return std::nullopt;
  std::array<std::uint8_t, 3> rgb{};
  for (std::size_t i = 0; i < 6; ++i) {
    const char c = text[i];
    const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (digit < 0) return std::nullopt;
    rgb[i / 2] = std::uint8_t(rgb[i / 2] * 16 + digit);
  }
  return rgb;
}
std::optional<std::array<std::uint8_t, 3>> passthrough_color_key() {
  const auto* value = getenv("VENTURE_PASSTHROUGH_KEY");
  return value ? parse_color_key(value) : std::nullopt;
}
bool steamvr_manifest(std::string_view path, std::string_view contents) {
  std::string text(path);
  text += ' ';
  text += contents;
  for (char& c : text) c = char(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
  return text.find("steamvr") != std::string::npos || text.find("steamxr") != std::string::npos;
}
namespace {
// The "library_path" value of a runtime manifest, as written; empty when it names none.
std::string library_path_value(std::string_view contents) {
  constexpr std::string_view key = "\"library_path\"";
  auto at = contents.find(key);
  if (at == std::string_view::npos) return {};
  at = contents.find('"', contents.find(':', at + key.size()));
  if (at == std::string_view::npos) return {};
  const auto end = contents.find('"', at + 1);
  if (end == std::string_view::npos) return {};
  return std::string(contents.substr(at + 1, end - at - 1));
}
}  // namespace
FlatpakSteamVr flatpak_steamvr_state() {
  FlatpakSteamVr out;
  const auto* id = getenv("FLATPAK_ID");
  if (!id || !*id) return out;
  std::filesystem::path manifest;
  if (const auto* json = getenv("XR_RUNTIME_JSON"); json && *json) manifest = json;
  else if (const auto* home = getenv("HOME"); home && *home) manifest = std::filesystem::path(home) / ".config/openxr/1/active_runtime.json";
  else return out;
  std::error_code ec;
  // SteamVR's manifest is often a link into SteamVR's folder, which the sandbox
  // may not see: follow the links as far as the sandbox shows them. The last
  // target is the host path of the manifest.
  std::string path = manifest.string();
  std::filesystem::path last = manifest;
  for (int hop = 0; hop < 16 && std::filesystem::is_symlink(last, ec); ++hop) {
    auto target = std::filesystem::read_symlink(last, ec);
    if (ec) break;
    if (target.is_relative()) target = last.parent_path() / target;
    path += " " + target.string();
    last = target.lexically_normal();
  }
  std::ostringstream contents;
  std::ifstream file(manifest);
  if (file) contents << file.rdbuf();
  if (!steamvr_manifest(path, contents.str())) return out;
  out.steamvr = true;
  // The Flatpak exposes SteamVR's usual folders (tools/packaging/linux). The
  // folder to grant: the manifest's when it is missing, else the client
  // library's (SteamVR's own folder when the library is relative to it).
  if (!file) {
    out.hidden_folder = last.parent_path().generic_string();
    out.visible = false;
    return out;
  }
  const std::filesystem::path library(steamvr_library(manifest.string(), contents.str()));
  if (library.empty() || std::filesystem::exists(library, ec)) return out;
  out.visible = false;
  if (const auto raw = library_path_value(contents.str()); raw.starts_with('/') || std::filesystem::path(raw).is_absolute()) {
    out.hidden_folder = library.parent_path().generic_string();
  } else {
    auto real = std::filesystem::canonical(manifest, ec);
    out.hidden_folder = (ec ? last : real).parent_path().generic_string();
  }
  return out;
}
bool flatpak_steamvr() {
  const auto state = flatpak_steamvr_state();
  return state.steamvr && !state.visible;
}
std::string flatpak_override_command(std::string_view folder) {
  const auto* id = getenv("FLATPAK_ID");
  std::string quoted = "'";
  for (char c : folder) quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
  quoted += "'";
  return "flatpak override --user --filesystem=" + quoted + ":ro " + std::string(id && *id ? id : "org.venture_emu.Venture");
}
std::string steamvr_library(std::string_view manifest_path, std::string_view contents) {
  const std::filesystem::path manifest(manifest_path);
  // "library_path": "<path>", relative to the manifest's real folder.
  const auto raw = library_path_value(contents);
  if (raw.empty()) return {};
  std::filesystem::path library(raw);
  if (library.is_absolute()) return library.generic_string();
  std::error_code ec;
  auto real = std::filesystem::canonical(manifest, ec);
  return ((ec ? manifest : real).parent_path() / library).generic_string();
}
} // namespace venture::environment
