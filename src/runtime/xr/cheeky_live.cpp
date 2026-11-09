#include "xr/cheeky_live.h"
#include "common/environment.h"
#include "common/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace venture::xr::cheeky_live {
namespace {
std::atomic<bool> nr_state{[] {
  const char* v = venture::environment::getenv("VENTURE_CHEEKY_NR");
  return v && *v == '1';
}()};
std::mutex command_mutex;

#ifdef _WIN32
std::filesystem::path cheeky_directory() {
  wchar_t path[32768]{};
  const auto length = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  return std::filesystem::path(std::wstring(path, length)).parent_path() / L"CheekyFoveatedDLSS";
}
// CheekyHost_Command of the host Cheeky's Vulkan layer loaded in this process.
bool send(const std::string& command) {
  using Command = bool (*)(const char*);
  const auto host = GetModuleHandleW(L"CheekyFoveatedDLSSHost.dll");
  const auto run = host ? reinterpret_cast<Command>(reinterpret_cast<void*>(GetProcAddress(host, "CheekyHost_Command")))
                        : nullptr;
  if (!run) return false;
  std::lock_guard lock(command_mutex);
  return run(command.c_str());
}
#else
std::filesystem::path cheeky_directory() { return {}; }
bool send(const std::string&) { return false; }
#endif

std::uint64_t request_id() {
  return std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count());
}
// Follows NrEnabled in a command that Cheeky accepted.
void note_command(const std::string& command) {
  const auto at = command.find("\nNrEnabled=");
  if (at == std::string::npos) return;
  const auto value = command.substr(at + 11, command.find('\n', at + 11) - (at + 11));
  nr_state = value == "true" || value == "1";
}

std::chrono::steady_clock::time_point next_poll{};
std::string last_request;

struct StickHold {
  static constexpr std::int64_t threshold = 2'000'000'000;
  std::int64_t since = -1;
  bool fired = false;
} hold;
std::mutex hold_mutex;
}  // namespace

void tick() {
  const auto now = std::chrono::steady_clock::now();
  if (now < next_poll) return;
  next_poll = now + std::chrono::milliseconds(500);
  const auto directory = cheeky_directory();
  if (directory.empty()) return;
  std::ifstream in(directory / "venture-live-command.txt", std::ios::binary);
  if (!in) return;
  std::stringstream text;
  text << in.rdbuf();
  const auto command = text.str();
  // "1\n<id>\n...": the id identifies a request; each applies once.
  const auto first = command.find('\n'), second = first == std::string::npos ? first : command.find('\n', first + 1);
  if (second == std::string::npos || command.size() > 8192) return;
  const auto id = command.substr(first + 1, second - first - 1);
  if (id == last_request) return;
  last_request = id;
  const bool applied = send(command);
  if (applied) note_command(command);
  log::info("cheeky", "Apply in game request {} {}", id, applied ? "applied" : "refused (Cheeky is not loaded)");
  std::ofstream(directory / "venture-live-status.txt", std::ios::binary | std::ios::trunc)
      << id << ' ' << (applied ? 1 : 0) << '\n';
}

bool sticks_wanted() {
  static const bool wanted = [] {
    const char* v = venture::environment::getenv("VENTURE_NR_THUMBSTICK");
    return v && *v == '1';
  }();
  return wanted;
}

void note_sticks(std::int64_t now_ns, bool left, bool right, bool active) {
  if (!sticks_wanted()) return;
  bool fire = false;
  {
    std::lock_guard lock(hold_mutex);
    if (!active || !left || !right) {
      // Both clicks release before the next toggle.
      if (!left && !right) hold.fired = false;
      hold.since = -1;
      return;
    }
    if (hold.fired) return;
    if (hold.since < 0) hold.since = now_ns;
    if (now_ns - hold.since >= StickHold::threshold) hold.fired = fire = true;
  }
  if (!fire) return;
  const bool next = !nr_state.load();
  const auto command = "1\n" + std::to_string(request_id()) + "\nset\nNrEnabled=" + (next ? "true" : "false") + "\n";
  const bool applied = send(command);
  if (applied) nr_state = next;
  log::info("cheeky", "Toggle NR with both thumbsticks: {} {}", next ? "on" : "off",
            applied ? "applied" : "refused (Cheeky is not loaded)");
}

bool nr_enabled() { return nr_state.load(); }
}  // namespace venture::xr::cheeky_live
