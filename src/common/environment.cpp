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
bool flatpak_steamvr() {
  const auto* id = getenv("FLATPAK_ID");
  if (!id || !*id) return false;
  std::filesystem::path manifest;
  if (const auto* json = getenv("XR_RUNTIME_JSON"); json && *json) manifest = json;
  else if (const auto* home = getenv("HOME"); home && *home) manifest = std::filesystem::path(home) / ".config/openxr/1/active_runtime.json";
  else return false;
  std::error_code ec;
  std::string path = manifest.string();
  // SteamVR's manifest is a link into the Steam library, which the sandbox may not see.
  if (auto target = std::filesystem::read_symlink(manifest, ec); !ec) path += " " + target.string();
  std::ostringstream contents;
  if (std::ifstream file(manifest); file) contents << file.rdbuf();
  return steamvr_manifest(path, contents.str());
}
} // namespace venture::environment
