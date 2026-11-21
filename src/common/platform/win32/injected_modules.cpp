// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/injected_modules.h"
#include "common/json.h"
#include "common/openxr_layers.h"
#include "common/paths.h"
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <windows.h>
#include <tlhelp32.h>

namespace venture::injected_modules {
namespace {
namespace fs = std::filesystem;
std::wstring environment(const wchar_t *name) {
  std::wstring value(32768, L'\0');
  const auto n = GetEnvironmentVariableW(name, value.data(), DWORD(value.size()));
  value.resize(n < value.size() ? n : 0);
  return value;
}
std::wstring folder(UINT (*get)(LPWSTR, UINT)) {
  wchar_t path[MAX_PATH]{};
  const auto n = get(path, MAX_PATH);
  return n && n < MAX_PATH ? std::wstring(path, n) : std::wstring{};
}
// The directory of an OpenXR manifest and of the library it names.
void add_manifest(std::vector<std::string> &roots, const fs::path &manifest) {
  if (manifest.empty()) return;
  roots.push_back(paths::utf8(manifest.parent_path()));
  try {
    const auto library = paths::from_utf8(str(field(paths::read_json(manifest), "runtime"), "library_path"));
    if (!library.empty() && library.has_parent_path())
      roots.push_back(paths::utf8((library.is_relative() ? manifest.parent_path() / library : library)
                                      .lexically_normal()
                                      .parent_path()));
  } catch (const std::exception &) {
  }
}
// The folder of the library a Vulkan driver manifest names.
void add_vulkan_driver(std::vector<std::string> &roots, const fs::path &manifest) {
  try {
    const auto library = paths::from_utf8(str(field(paths::read_json(manifest), "ICD"), "library_path"));
    if (!library.empty() && library.has_parent_path())
      roots.push_back(paths::utf8((library.is_relative() ? manifest.parent_path() / library : library)
                                      .lexically_normal()
                                      .parent_path()));
  } catch (const std::exception &) {
  }
}
// The places whose modules are not injected. Computed once: the install, the
// runtime and its layers do not change while the game runs.
const std::vector<std::string> &trusted_roots() {
  static std::once_flag once;
  static std::vector<std::string> roots;
  std::call_once(once, [] {
    // System32 (with the GPU drivers' DriverStore), SysWOW64 and WinSxS.
    for (auto dir : {folder(GetSystemDirectoryW), folder(GetSystemWow64DirectoryW)})
      if (!dir.empty()) roots.push_back(paths::utf8(fs::path(dir)));
    if (auto windows = folder(GetSystemWindowsDirectoryW); !windows.empty())
      roots.push_back(paths::utf8(fs::path(windows) / L"WinSxS"));
    // Venture's install: the runtime, ANGLE, the JRE and the libraries it ships.
    try {
      roots.push_back(paths::utf8(paths::executable().parent_path()));
    } catch (const std::exception &) {
    }
    // The Vulkan drivers the loader is given (installed GPU drivers are in the
    // DriverStore): the environment's lists, then the registered manifests.
    for (auto variable : {L"VK_DRIVER_FILES", L"VK_ICD_FILENAMES", L"VK_ADD_DRIVER_FILES"}) {
      const std::wstring list = environment(variable);
      std::wstring_view rest = list;
      while (!rest.empty()) {
        const auto end = (std::min)(rest.find(L';'), rest.size());
        if (end) add_vulkan_driver(roots, fs::path(rest.substr(0, end)));
        rest.remove_prefix((std::min)(end + 1, rest.size()));
      }
    }
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
      HKEY key{};
      if (RegOpenKeyExW(root, L"SOFTWARE\\Khronos\\Vulkan\\Drivers", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        continue;
      for (DWORD i = 0;; ++i) {
        wchar_t name[1024];
        DWORD length = DWORD(std::size(name)), type = 0, data = 1, size = sizeof(data);
        const auto status =
            RegEnumValueW(key, i, name, &length, nullptr, &type, reinterpret_cast<BYTE *>(&data), &size);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status == ERROR_SUCCESS && size == sizeof(data) && data == 0) add_vulkan_driver(roots, fs::path(name));
      }
      RegCloseKey(key);
    }
    // The OpenXR runtime the game runs on (the loader takes XR_RUNTIME_JSON
    // first) and the registered API layers.
    const auto chosen = environment(L"XR_RUNTIME_JSON");
    add_manifest(roots, chosen.empty() ? openxr_layers::active_runtime() : fs::path(chosen));
    for (const auto &manifest : openxr_layers::implicit_manifests()) {
      const auto layer = openxr_layers::read(manifest);
      if (layer.library.has_parent_path()) roots.push_back(paths::utf8(layer.library.lexically_normal().parent_path()));
    }
  });
  return roots;
}
std::string version_text(const std::vector<std::byte> &data, const wchar_t *name, const std::wstring &language) {
  wchar_t *value = nullptr;
  UINT length = 0;
  const auto key = L"\\StringFileInfo\\" + language + L"\\" + name;
  if (!VerQueryValueW(data.data(), key.c_str(), reinterpret_cast<void **>(&value), &length) || !value || !length)
    return {};
  std::wstring text(value, length);
  while (!text.empty() && text.back() == L'\0') text.pop_back();
  return paths::utf8(fs::path(text));
}
void read_version(Module &module, const std::wstring &path) {
  DWORD ignored = 0;
  const auto size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
  if (!size) return;
  std::vector<std::byte> data(size);
  if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return;
  // The first translation the file lists, then US English in Unicode and in Windows-1252.
  std::vector<std::wstring> languages;
  struct Translation {
    WORD language, code_page;
  } *translations = nullptr;
  UINT length = 0;
  if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&translations), &length) &&
      translations && length >= sizeof(Translation)) {
    wchar_t text[16];
    std::swprintf(text, 16, L"%04x%04x", translations->language, translations->code_page);
    languages.emplace_back(text);
  }
  languages.emplace_back(L"040904b0");
  languages.emplace_back(L"040904e4");
  for (const auto &language : languages) {
    module.product = version_text(data, L"ProductName", language);
    module.company = version_text(data, L"CompanyName", language);
    module.description = version_text(data, L"FileDescription", language);
    if (!module.product.empty() || !module.company.empty() || !module.description.empty()) break;
  }
  VS_FIXEDFILEINFO *info = nullptr;
  if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &length) && info)
    module.version = std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                                 HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
}
}  // namespace

std::vector<Module> scan() {
  auto roots = trusted_roots();
  std::ranges::move(trusted_folders(), std::back_inserter(roots));
  HANDLE snapshot = INVALID_HANDLE_VALUE;
  // ERROR_BAD_LENGTH: the module list changed while it was read.
  for (int attempt = 0; attempt < 8 && snapshot == INVALID_HANDLE_VALUE; ++attempt) {
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snapshot == INVALID_HANDLE_VALUE && GetLastError() != ERROR_BAD_LENGTH) break;
  }
  if (snapshot == INVALID_HANDLE_VALUE)
    throw std::runtime_error(std::format("CreateToolhelp32Snapshot failed (error {})", GetLastError()));
  std::vector<Module> out;
  MODULEENTRY32W entry{};
  entry.dwSize = sizeof entry;
  for (bool more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
    const std::wstring path = entry.szExePath;
    Module module{paths::utf8(fs::path(path))};
    if (inside(module.path, roots, true)) continue;
    read_version(module, path);
    out.push_back(std::move(module));
  }
  CloseHandle(snapshot);
  return out;
}

std::vector<std::string> ancestor_images() {
  // Parent process ids from a process snapshot; a parent that started after
  // its child is another process that reused the id, and ends the chain.
  std::map<DWORD, DWORD> parents;
  if (HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snapshot != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    for (bool more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
      parents[entry.th32ProcessID] = entry.th32ParentProcessID;
    CloseHandle(snapshot);
  }
  auto created = [](HANDLE process) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) return std::uint64_t(0);
    return (std::uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
  };
  std::vector<std::string> out;
  DWORD pid = GetCurrentProcessId();
  std::uint64_t child_created = created(GetCurrentProcess());
  for (int depth = 0; depth < 8; ++depth) {
    const auto parent = parents.find(pid);
    if (parent == parents.end() || parent->second == 0 || parent->second == pid) break;
    pid = parent->second;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) break;
    const auto when = created(process);
    std::wstring image(32768, L'\0');
    DWORD size = DWORD(image.size());
    const bool named = QueryFullProcessImageNameW(process, 0, image.data(), &size);
    CloseHandle(process);
    if (!when || !child_created || when > child_created || !named) break;
    image.resize(size);
    out.push_back(paths::utf8(fs::path(image)));
    child_created = when;
  }
  return out;
}

std::vector<std::string> system_folders() {
  std::vector<std::string> out;
  if (auto windows = folder(GetSystemWindowsDirectoryW); !windows.empty()) out.push_back(paths::utf8(fs::path(windows)));
  return out;
}

void set_process_variable(const char *name, const std::string &value) {
  // Children take the Win32 environment; the CRT keeps its own copy.
  const auto wide_name = fs::path(name).native(), wide_value = paths::from_utf8(value).native();
  if (!SetEnvironmentVariableW(wide_name.c_str(), wide_value.c_str()) || _wputenv_s(wide_name.c_str(), wide_value.c_str()))
    throw std::runtime_error(std::format("Could not set {}", name));
}
}  // namespace venture::injected_modules
