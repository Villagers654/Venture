#include "launcher/sign_in_link.h"
#include "common/environment.h"
#include "common/process.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#else
#include <ranges>
#include <unistd.h>
#endif

namespace venture::launcher::sign_in_link {
namespace {
// The address a browser handed over, and the handler to put back.
fs::path inbox(const fs::path &data) { return data / "sign-in-link"; }
fs::path previous_file(const fs::path &data) { return data / "sign-in-link.previous"; }

#ifdef _WIN32
std::wstring g_classes = L"Software\\Classes";
std::wstring scheme_key() { return g_classes + L"\\oculus"; }
std::wstring command_key() { return scheme_key() + L"\\shell\\open\\command"; }

std::wstring command_value() {
  DWORD size = 0;
  const auto key = command_key();
  if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS)
    return {};
  std::wstring value(size / sizeof(wchar_t), L'\0');
  if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, value.data(), &size) !=
      ERROR_SUCCESS)
    return {};
  value.resize(wcsnlen(value.c_str(), value.size()));
  return value;
}
bool set(const std::wstring &key, const wchar_t *name, const std::wstring &value) {
  return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(),
                         DWORD((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}
#else
// The handler's desktop entry, beside the user's other applications.
constexpr char kEntry[] = "org.venture_emu.Venture.SignIn.desktop";
constexpr char kScheme[] = "x-scheme-handler/oculus";

fs::path user_data() {
  if (auto p = environment::getenv("XDG_DATA_HOME"); p && fs::path(p).is_absolute()) return p;
  auto home = environment::getenv("HOME");
  return home && *home ? fs::path(home) / ".local/share" : fs::path();
}
fs::path user_config() {
  if (auto p = environment::getenv("XDG_CONFIG_HOME"); p && fs::path(p).is_absolute()) return p;
  auto home = environment::getenv("HOME");
  return home && *home ? fs::path(home) / ".config" : fs::path();
}
fs::path which(std::string_view name) {
  const char *path = environment::getenv("PATH");
  for (auto dir : std::views::split(std::string_view(path && *path ? path : "/usr/bin:/bin"), ':')) {
    fs::path candidate = fs::path(std::string(dir.begin(), dir.end())) / name;
    if (access(candidate.c_str(), X_OK) == 0) return candidate;
  }
  return {};
}
bool xdg_mime(const std::vector<std::string> &args) {
  auto tool = which("xdg-mime");
  if (tool.empty()) return false;
  try {
    return run_tool(tool, args, [](std::string_view) {}) == 0;
  } catch (...) {
    return false;
  }
}
// The user's own default for oculus:// in mimeapps.list; empty when there is
// none. Read from the file, not `xdg-mime query default`, which skips entries
// whose desktop file is missing and falls back to any program listing the scheme.
std::string user_default() {
  std::error_code ec;
  const auto list = user_config() / "mimeapps.list";
  if (user_config().empty() || !fs::exists(list, ec)) return {};
  std::istringstream in(paths::read(list));
  std::string line, section;
  const std::string key = std::string(kScheme) + "=";
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.starts_with('[')) section = line;
    else if (section == "[Default Applications]" && line.starts_with(key)) return line.substr(key.size());
  }
  return {};
}
// One Exec= argument, quoted as the desktop entry spec asks.
std::string exec_arg(std::string_view arg) {
  std::string out = "\"";
  for (char c : arg) {
    if (c == '"' || c == '`' || c == '$' || c == '\\') out += "\\\\";  // escaped twice: quoting, then the string
    if (c == '%') out += '%';
    out += c;
  }
  return out + '"';
}
// What restore() leaves when there was no default before: no user default.
void forget_default() {
  std::error_code ec;
  const auto list = user_config() / "mimeapps.list";
  if (user_config().empty() || !fs::exists(list, ec)) return;
  std::istringstream in(paths::read(list));
  std::string out, line;
  const std::string ours = std::string(kScheme) + "=" + kEntry;
  bool changed = false;
  while (std::getline(in, line)) {
    if (line == ours || line == ours + ";") {
      changed = true;
      continue;
    }
    out += line + '\n';
  }
  if (changed) paths::atomic_write(list, out);
}
#endif
} // namespace

#ifdef _WIN32
void set_classes_key_for_tests(std::wstring key) { g_classes = std::move(key); }
#endif

bool take_over(const fs::path &data) {
  (void)receive(data);  // an address from an earlier sign-in
  std::error_code ec;
#ifdef _WIN32
  // Per user, so it needs no administrator, and it shadows the Meta app's own.
  // One left by a sign-in Venture never finished keeps the handler from before it.
  auto before = command_value();
  const bool ours = before.find(L"--sign-in-callback") != std::wstring::npos;
  if (!ours || !fs::exists(previous_file(data), ec))
    paths::atomic_write(previous_file(data), ours ? std::string() : paths::utf8(fs::path(before)));
  const std::wstring command =
      L"\"" + paths::executable().wstring() + L"\" --sign-in-callback \"" + data.wstring() + L"\" \"%1\"";
  return set(scheme_key(), nullptr, L"URL:oculus") && set(scheme_key(), L"URL Protocol", L"") &&
         set(command_key(), nullptr, command);
#else
  // The sandbox has no say over the host's handlers.
  if (environment::getenv("FLATPAK_ID") || user_data().empty() || user_config().empty()) return false;
  if (which("xdg-mime").empty()) return false;
  const auto before = user_default();
  const bool ours = before == kEntry || before == std::string(kEntry) + ";";
  if (!ours || !fs::exists(previous_file(data), ec))
    paths::atomic_write(previous_file(data), ours ? std::string() : before);
  const char *image = environment::getenv("APPIMAGE");
  const fs::path program = image && *image ? paths::from_utf8(image) : paths::executable();
  paths::atomic_write(user_data() / "applications" / kEntry,
                      "[Desktop Entry]\nType=Application\nName=Venture sign-in\nNoDisplay=true\n"
                      "Exec=" + exec_arg(paths::utf8(program)) + " --sign-in-callback " +
                          exec_arg(paths::utf8(data)) + " %u\n"
                      "MimeType=" + kScheme + ";\n");
  return xdg_mime({"default", kEntry, kScheme});
#endif
}

void restore(const fs::path &data) {
  std::error_code ec;
#ifndef _WIN32
  fs::remove(user_data() / "applications" / kEntry, ec);
#endif
  if (!fs::exists(previous_file(data), ec)) return;
  const auto before = paths::read(previous_file(data));
  fs::remove(previous_file(data), ec);
#ifdef _WIN32
  if (before.empty())
    RegDeleteTreeW(HKEY_CURRENT_USER, scheme_key().c_str());
  else
    set(command_key(), nullptr, paths::from_utf8(before).wstring());
#else
  if (before.empty())
    forget_default();
  else
    xdg_mime({"default", before, kScheme});
#endif
}

int deliver(const fs::path &data, std::string_view address) {
  constexpr std::string_view scheme = "oculus://";
  auto lower = [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); };
  if (!data.is_absolute() || !fs::is_directory(data) || address.size() > 16 * 1024 ||
      address.size() < scheme.size() || !std::ranges::equal(address.substr(0, scheme.size()), scheme, {}, lower))
    return 2;
  try {
    paths::atomic_write(inbox(data), address);
    return 0;
  } catch (...) {
    return 1;
  }
}

std::string receive(const fs::path &data) {
  std::error_code ec;
  if (!fs::exists(inbox(data), ec)) return {};
  auto address = paths::read(inbox(data), 16 * 1024);
  fs::remove(inbox(data), ec);
  return address;
}
} // namespace venture::launcher::sign_in_link
