#include "common/log.h"
#include "common/meta/native_host.h"
#include <windows.h>

namespace venture::meta {
namespace {
// Each Chromium browser looks under its own HKCU key for a native messaging
// host's manifest path, keyed by the host's own name.
constexpr const wchar_t *kBrowserKeys[] = {
    L"Software\\Google\\Chrome\\NativeMessagingHosts\\",
    L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\",
    L"Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts\\",
};
void set_registry_value(const std::wstring &key, const std::wstring &value) {
  HKEY handle{};
  if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS)
    throw std::runtime_error(
        "Could not register the Meta session native messaging host.");
  auto status =
      RegSetValueExW(handle, L"", 0, REG_SZ,
                     reinterpret_cast<const BYTE *>(value.c_str()),
                     DWORD((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(handle);
  if (status != ERROR_SUCCESS)
    throw std::runtime_error(
        "Could not register the Meta session native messaging host.");
}
} // namespace
void register_native_messaging_host() {
  auto manifest = paths::data_dir() / "native-messaging" /
                  (std::string(kNativeMessagingHostName) + ".json");
  paths::atomic_write(manifest, native_messaging_host_manifest().dump(2));
  auto name = paths::from_utf8(kNativeMessagingHostName).wstring();
  for (auto *prefix : kBrowserKeys)
    set_registry_value(std::wstring(prefix) + name, manifest.wstring());
  log::info("meta", "Registered the Meta session native messaging host at {}.",
           paths::utf8(native_messaging_host_executable()));
}
} // namespace venture::meta
