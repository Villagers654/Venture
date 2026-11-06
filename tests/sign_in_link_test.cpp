// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/sign_in_link.h"
#include "common/environment.h"
#include "common/process.h"
#include <doctest/doctest.h>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace venture;
using namespace venture::launcher;

namespace {
struct TempDir {
  fs::path path;
  explicit TempDir(std::string_view name)
      : path(fs::temp_directory_path() / (std::string(name) + "-" + std::to_string(current_process_id()))) {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};
} // namespace

#ifndef _WIN32
namespace {
std::string read(const fs::path &file) {
  std::ifstream in(file, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}
} // namespace
#endif

TEST_CASE("sign-in link: an address handed over is received once") {
  TempDir data("venture-sign-in-link");
  CHECK(sign_in_link::receive(data.path).empty());
  CHECK(sign_in_link::deliver(data.path, "OCULUS://auth?code=1") == 0);
  CHECK(sign_in_link::receive(data.path) == "OCULUS://auth?code=1");
  CHECK(sign_in_link::receive(data.path).empty());
  // Nothing but an oculus:// address, and only into an existing absolute folder.
  CHECK(sign_in_link::deliver(data.path, "https://example.com") == 2);
  CHECK(sign_in_link::deliver("relative", "oculus://auth") == 2);
  CHECK(sign_in_link::deliver(data.path / "missing", "oculus://auth") == 2);
  CHECK(sign_in_link::deliver(data.path, "oculus://" + std::string(20000, 'a')) == 2);
  CHECK(sign_in_link::receive(data.path).empty());
}

#ifdef _WIN32
namespace {
std::wstring value(const std::wstring &key) {
  DWORD size = 0;
  if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS)
    return {};
  std::wstring out(size / sizeof(wchar_t), L'\0');
  RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, out.data(), &size);
  out.resize(wcsnlen(out.c_str(), out.size()));
  return out;
}
} // namespace

TEST_CASE("sign-in link: the oculus:// handler before it comes back") {
  // A private stand-in for HKCU\Software\Classes, removed afterwards: the
  // user's real oculus:// handler is never touched.
  const std::wstring root = L"Software\\Venture-test-sign-in-" + std::to_wstring(current_process_id());
  const std::wstring classes = root + L"\\Classes", command = classes + L"\\oculus\\shell\\open\\command";
  RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());
  sign_in_link::set_classes_key_for_tests(classes);
  TempDir data("venture-sign-in-data");

  RegSetKeyValueW(HKEY_CURRENT_USER, command.c_str(), nullptr, REG_SZ, L"\"other.exe\" \"%1\"",
                  DWORD(sizeof(L"\"other.exe\" \"%1\"")));
  REQUIRE(sign_in_link::take_over(data.path));
  CHECK(value(command).find(L"--sign-in-callback \"" + data.path.wstring() + L"\" \"%1\"") != std::wstring::npos);
  // A second sign-in after one that never ended still knows the one before.
  CHECK(sign_in_link::take_over(data.path));
  sign_in_link::restore(data.path);
  CHECK(value(command) == L"\"other.exe\" \"%1\"");

  // With none before, none after.
  RegDeleteTreeW(HKEY_CURRENT_USER, classes.c_str());
  CHECK(sign_in_link::take_over(data.path));
  sign_in_link::restore(data.path);
  HKEY key = nullptr;
  CHECK(RegOpenKeyExW(HKEY_CURRENT_USER, (classes + L"\\oculus").c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS);
  if (key) RegCloseKey(key);

  sign_in_link::set_classes_key_for_tests(L"Software\\Classes");
  RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());
}
#else
TEST_CASE("sign-in link: the oculus:// handler before it comes back") {
  // A profile of its own, read the way a desktop without its own tools does.
  TempDir home("venture-sign-in-home");
  std::map<std::string, std::optional<std::string>> saved;
  for (const char *name : {"XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CURRENT_DESKTOP", "DESKTOP_SESSION",
                           "XDG_SESSION_DESKTOP", "KDE_FULL_SESSION", "GNOME_DESKTOP_SESSION_ID", "FLATPAK_ID"}) {
    const char *value = std::getenv(name);
    saved[name] = value ? std::optional<std::string>(value) : std::nullopt;
    unsetenv(name);
  }
  setenv("XDG_DATA_HOME", (home.path / "share").c_str(), 1);
  setenv("XDG_CONFIG_HOME", (home.path / "config").c_str(), 1);
  environment::refresh();
  const auto list = home.path / "config/mimeapps.list";
  const auto entry = home.path / "share/applications/org.venture_emu.Venture.SignIn.desktop";
  paths::atomic_write(list, "[Default Applications]\nx-scheme-handler/oculus=other.desktop\n");
  paths::atomic_write(home.path / "share/applications/other.desktop",
                      "[Desktop Entry]\nType=Application\nName=Other\nExec=other %u\nMimeType=x-scheme-handler/oculus;\n");

  TempDir data("venture-sign-in-data");
  if (sign_in_link::take_over(data.path)) {
    CHECK(read(entry).find("--sign-in-callback \"" + data.path.string() + "\" %u") != std::string::npos);
    CHECK(read(list).find("x-scheme-handler/oculus=org.venture_emu.Venture.SignIn.desktop") != std::string::npos);
    // A second sign-in after one that never ended still knows the one before.
    CHECK(sign_in_link::take_over(data.path));
    sign_in_link::restore(data.path);
    CHECK_FALSE(fs::exists(entry));
    CHECK(read(list).find("x-scheme-handler/oculus=other.desktop") != std::string::npos);

    // With none before, none after (a program that only lists the scheme does
    // not become the default).
    paths::atomic_write(list, "[Default Applications]\n");
    CHECK(sign_in_link::take_over(data.path));
    sign_in_link::restore(data.path);
    CHECK(read(list).find("x-scheme-handler/oculus") == std::string::npos);
  } else {
    MESSAGE("xdg-mime is not installed; only the address hand-over was tested.");
  }

  for (auto &[name, value] : saved)
    if (value)
      setenv(name.c_str(), value->c_str(), 1);
    else
      unsetenv(name.c_str());
  environment::refresh();
}
#endif
