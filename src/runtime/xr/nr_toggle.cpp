#include "xr/nr_toggle.h"
#include "common/environment.h"
#include "common/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace venture::xr::nr_toggle {
namespace {
std::atomic<bool> nr_state{[] {
  const char* v = venture::environment::getenv("VENTURE_OVERLAY_NR");
  return v && *v == '1';
}()};
struct StickHold {
  std::int64_t since = -1;
  bool fired = false;
} hold;
// The shortcut: both thumbstick clicks, or one (VENTURE_NR_SHORTCUT), held VENTURE_NR_HOLD_MS (2 s).
enum class Shortcut { both, left, right };
Shortcut shortcut() {
  static const Shortcut value = [] {
    const char* v = venture::environment::getenv("VENTURE_NR_SHORTCUT");
    const std::string_view text = v ? v : "";
    return text == "left" ? Shortcut::left : text == "right" ? Shortcut::right : Shortcut::both;
  }();
  return value;
}
std::int64_t hold_ns() {
  static const std::int64_t value = [] {
    const char* v = venture::environment::getenv("VENTURE_NR_HOLD_MS");
    const long ms = v && *v ? std::strtol(v, nullptr, 10) : 2000;
    return std::int64_t(std::clamp(ms, 500L, 5000L)) * 1'000'000;
  }();
  return value;
}
// VENTURE_NR_TARGET: the shortcut switches VDXR's DLSS-NR ("vdxr") or Venture NR ("venture", xr/venture_nr.h)
// through a per-user key that NR watches.
enum class Target { none, vdxr, venture };
Target target() {
  static const Target value = [] {
    const char* v = venture::environment::getenv("VENTURE_NR_TARGET");
    const std::string_view text = v ? v : "";
    return text == "vdxr" ? Target::vdxr : text == "venture" ? Target::venture : Target::none;
  }();
  return value;
}
bool vdxr_target() { return target() != Target::none; }
const wchar_t* target_key() {
  return target() == Target::vdxr ? L"SOFTWARE\\VirtualDesktop-OpenXR\\DLSSNR" : L"SOFTWARE\\Venture\\NR";
}
// VDXR keeps running with NR bypassed (DLSSNR_Bypass, inverted): the VRS outline and the shared foveal center stay.
const wchar_t* target_value() { return target() == Target::vdxr ? L"DLSSNR_Bypass" : L"On"; }
bool inverted() { return target() == Target::vdxr; }
bool set_vdxr_nr(bool on) {
#ifdef _WIN32
  HKEY key{};
  if (RegCreateKeyExW(HKEY_CURRENT_USER, target_key(), 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key,
                      nullptr) != ERROR_SUCCESS)
    return false;
  const DWORD value = on != inverted() ? 1 : 0;
  const bool ok = RegSetValueExW(key, target_value(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
                                 sizeof(value)) == ERROR_SUCCESS;
  RegCloseKey(key);
  return ok;
#else
  (void)on;
  return false;
#endif
}
std::mutex hold_mutex;
}  // namespace

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
    const bool pressed = shortcut() == Shortcut::both  ? left && right
                         : shortcut() == Shortcut::left ? left && !right
                                                        : right && !left;
    if (!active || !pressed) {
      // Every click releases before the next toggle.
      if (!left && !right) hold.fired = false;
      hold.since = -1;
      return;
    }
    if (hold.fired) return;
    if (hold.since < 0) hold.since = now_ns;
    if (now_ns - hold.since >= hold_ns()) hold.fired = fire = true;
  }
  if (!fire) return;
  const bool next = !nr_state.load();
  if (!vdxr_target()) return;
  const bool applied = set_vdxr_nr(next);
  if (applied) nr_state = next;
  log::info("nr", "Toggle NR ({}): {} {}", target() == Target::vdxr ? "VDXR DLSS-NR" : "Venture NR",
            next ? "on" : "off", applied ? "applied" : "refused");
}

bool nr_enabled() {
#ifdef _WIN32
  // As the NR's per-user key says (the shortcut, Apply in game), read again every second.
  if (vdxr_target()) {
    static std::atomic<std::int64_t> next_read{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    if (now >= next_read.load()) {
      next_read = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(1)).count();
      DWORD value = 0, size = sizeof(value);
      if (RegGetValueW(HKEY_CURRENT_USER, target_key(), target_value(),
                       RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
        nr_state = (value != 0) != inverted();
    }
  }
#endif
  return nr_state.load();
}
}  // namespace venture::xr::nr_toggle
