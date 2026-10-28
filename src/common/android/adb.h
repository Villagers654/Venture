#pragma once
#include "common/paths.h"
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

// A Quest (or any Android device) reached through Android platform-tools adb,
// over USB or Wi-Fi debugging. Read-only: apps are listed and their files
// pulled; nothing on the device is changed.
namespace venture::android::adb {
struct Device {
  std::string serial, state, model;  // state as adb reports it: device, unauthorized, offline...
  bool ready() const { return state == "device"; }
};
struct RemoteFile {
  std::string path;  // absolute on the device
  std::uint64_t size = 0;
};
struct App {
  std::string package;
  std::int64_t version_code = 0;
  std::vector<RemoteFile> apks;  // base.apk first, then splits
  std::vector<RemoteFile> data;  // everything under /sdcard/Android/obb/<package>/
  // The app's external storage, /sdcard/Android/data/<package>/ without its
  // cache: Unity's persistentDataPath and most Quest saves live there.
  std::vector<RemoteFile> saves;
  std::uint64_t bytes() const;  // APKs and data
};
// adb from `configured` when set, else PATH, ANDROID_HOME / ANDROID_SDK_ROOT,
// and the SDK's default locations; nullopt when none exists.
std::optional<fs::path> locate(const fs::path &configured = {});
// Parsers for adb's own output (also used by tests).
std::vector<Device> parse_devices(std::string_view devices_l);
std::vector<std::pair<std::string, std::int64_t>> parse_packages(std::string_view list_packages);
// `@<package>` headers, each followed by "<size> <path>" rows (stat -c '%s %n').
std::vector<App> parse_files(std::string_view listing, const std::vector<std::pair<std::string, std::int64_t>> &packages);
// What a device's state means for the user.
std::string state_message(const Device &device);
// `path` single-quoted for the device's shell; nullopt for names a listing
// line cannot carry (control characters).
std::optional<std::string> quote(std::string_view path);
// Resumed pulls start at a multiple of this.
constexpr std::uint64_t kPullBlock = 1 << 20;

class Client {
public:
  explicit Client(fs::path adb, std::string serial = {});
  std::vector<Device> devices(std::stop_token stop = {}) const;
  // Third-party apps with their APKs and data files.
  std::vector<App> apps(std::stop_token stop = {}) const;
  std::optional<App> app(std::string_view package, std::stop_token stop = {}) const;
  // Streams one file from the device from `offset` (a multiple of
  // kPullBlock) to its end; throws when the transfer fails.
  void pull(const RemoteFile &file, const std::function<void(std::span<const std::uint8_t>)> &sink,
            std::stop_token stop = {}, std::uint64_t offset = 0) const;
  // The file's SHA-256 as the device computes it (lowercase hex); nullopt
  // when the device has no sha256sum.
  std::optional<std::string> sha256(const RemoteFile &file, std::stop_token stop = {}) const;
  // adb connect for Wi-Fi debugging ("host" or "host:port"); returns the
  // serial adb lists the headset under, throws with adb's reason.
  std::string connect(std::string_view address, std::stop_token stop = {}) const;
  const std::string &serial() const { return serial_; }

private:
  fs::path adb_;
  std::string serial_;
  std::string shell(const std::string &command, std::stop_token stop) const;
  std::vector<std::string> base() const;
  std::vector<App> files(const std::vector<std::pair<std::string, std::int64_t>> &packages,
                         std::stop_token stop) const;
};
}  // namespace venture::android::adb
