#include "common/android/adb.h"

#include "common/environment.h"
#include "common/log.h"
#include "common/process.h"

#include <algorithm>
#include <cstdio>
#include <format>

namespace venture::android::adb {
namespace {
std::vector<std::string_view> lines(std::string_view text) {
  std::vector<std::string_view> out;
  while (!text.empty()) {
    auto end = text.find('\n');
    auto line = text.substr(0, end);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    if (!line.empty()) out.push_back(line);
    if (end == text.npos) break;
    text.remove_prefix(end + 1);
  }
  return out;
}
std::string first_error(std::string_view text) {
  for (auto line : lines(text)) return std::string(line);
  return {};
}
}  // namespace

std::optional<std::string> quote(std::string_view path) {
  if (path.empty() || path.front() != '/' ||
      std::ranges::any_of(path, [](unsigned char c) { return c < 32 || c == 127; }))
    return std::nullopt;
  std::string out = "'";
  for (char c : path) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return out + "'";
}
std::uint64_t App::bytes() const {
  std::uint64_t n = 0;
  for (auto &f : apks) n += f.size;
  for (auto &f : data) n += f.size;
  return n;
}
std::optional<fs::path> locate(const fs::path &configured) {
#ifdef _WIN32
  const fs::path name = "adb.exe";
#else
  const fs::path name = "adb";
#endif
  auto usable = [&](const fs::path &p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
  };
  if (!configured.empty()) {
    if (usable(configured)) return configured;
    if (usable(configured / name)) return configured / name;
    return std::nullopt;
  }
  std::vector<fs::path> candidates;
  if (auto path = environment::getenv("PATH")) {
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    std::string_view rest(path);
    while (!rest.empty()) {
      auto end = rest.find(separator);
      if (auto dir = rest.substr(0, end); !dir.empty()) candidates.push_back(paths::from_utf8(dir) / name);
      if (end == rest.npos) break;
      rest.remove_prefix(end + 1);
    }
  }
  for (auto variable : {"ANDROID_HOME", "ANDROID_SDK_ROOT"})
    if (auto sdk = environment::getenv(variable)) candidates.push_back(paths::from_utf8(sdk) / "platform-tools" / name);
#ifdef _WIN32
  if (auto local = environment::getenv("LOCALAPPDATA"))
    candidates.push_back(paths::from_utf8(local) / "Android" / "Sdk" / "platform-tools" / name);
#else
  if (auto home = environment::getenv("HOME")) {
    candidates.push_back(paths::from_utf8(home) / "Android" / "Sdk" / "platform-tools" / name);
    candidates.push_back(paths::from_utf8(home) / "Library" / "Android" / "sdk" / "platform-tools" / name);
  }
#endif
  for (auto &c : candidates)
    if (usable(c)) return c;
  return std::nullopt;
}
std::vector<Device> parse_devices(std::string_view text) {
  std::vector<Device> out;
  for (auto line : lines(text)) {
    if (line.starts_with("List of devices") || line.starts_with("*")) continue;
    auto tab = line.find_first_of(" \t");
    if (tab == line.npos) continue;
    Device d{std::string(line.substr(0, tab)), {}, {}};
    auto rest = line.substr(tab);
    rest.remove_prefix(std::min(rest.size(), rest.find_first_not_of(" \t")));
    // "no permissions (...)" is the one multi-word state.
    if (rest.starts_with("no permissions")) {
      d.state = "no permissions";
    } else {
      d.state = std::string(rest.substr(0, rest.find(' ')));
    }
    if (auto at = rest.find("model:"); at != rest.npos) {
      auto model = rest.substr(at + 6);
      d.model = std::string(model.substr(0, model.find(' ')));
      std::ranges::replace(d.model, '_', ' ');
    }
    out.push_back(std::move(d));
  }
  return out;
}
std::vector<std::pair<std::string, std::int64_t>> parse_packages(std::string_view text) {
  std::vector<std::pair<std::string, std::int64_t>> out;
  for (auto line : lines(text)) {
    if (!line.starts_with("package:")) continue;
    line.remove_prefix(8);
    auto space = line.find(' ');
    std::string package(line.substr(0, space));
    std::int64_t code = 0;
    if (auto at = line.find("versionCode:"); at != line.npos) {
      auto v = line.substr(at + 12);
      v = v.substr(0, v.find(' '));
      if (digits(v, 1, 19)) code = std::stoll(std::string(v));
    }
    if (paths::safe_package(package)) out.emplace_back(std::move(package), code);
  }
  return out;
}
std::vector<App> parse_files(std::string_view listing,
                             const std::vector<std::pair<std::string, std::int64_t>> &packages) {
  std::vector<App> out;
  App *current = nullptr;
  for (auto line : lines(listing)) {
    if (line.starts_with('@')) {
      auto package = line.substr(1);
      auto known = std::ranges::find(packages, package, &std::pair<std::string, std::int64_t>::first);
      current = nullptr;
      if (known == packages.end()) continue;
      out.push_back({known->first, known->second, {}, {}});
      current = &out.back();
      continue;
    }
    auto space = line.find(' ');
    if (!current || space == line.npos || !digits(line.substr(0, space), 1, 19)) continue;
    RemoteFile f{std::string(line.substr(space + 1)), std::stoull(std::string(line.substr(0, space)))};
    const auto obb = "/sdcard/Android/obb/" + current->package + "/";
    const auto external = "/sdcard/Android/data/" + current->package + "/";
    if (f.path.starts_with(external)) {
      if (!f.path.starts_with(external + "cache/")) current->saves.push_back(std::move(f));
    } else if (f.path.ends_with(".apk") && !f.path.starts_with(obb)) {
      // pm lists base.apk first; keep it first.
      if (f.path.ends_with("/base.apk")) current->apks.insert(current->apks.begin(), std::move(f));
      else current->apks.push_back(std::move(f));
    } else if (f.path.starts_with(obb)) {
      current->data.push_back(std::move(f));
    }
  }
  std::erase_if(out, [](auto &app) { return app.apks.empty(); });
  return out;
}
std::string state_message(const Device &d) {
  if (d.ready()) return {};
  if (d.state == "unauthorized")
    return "The headset has not authorized USB debugging for this computer. Put it on and choose Allow "
           "(tick Always allow) in the prompt.";
  if (d.state == "no permissions")
    return "This computer has no permission to use the headset's USB connection. Install the Android udev "
           "rules (android-udev-rules) or reconnect the cable.";
  if (d.state == "offline") return "The headset is connected but offline. Unplug it and plug it back in.";
  if (d.state == "authorizing" || d.state == "connecting") return "Connecting to the headset...";
  return "The headset is not ready (" + d.state + ").";
}

Client::Client(fs::path adb, std::string serial) : adb_(std::move(adb)), serial_(std::move(serial)) {}
std::vector<std::string> Client::base() const {
  if (serial_.empty()) return {};
  return {"-s", serial_};
}
std::vector<Device> Client::devices(std::stop_token stop) const {
  std::string out, error;
  const int code = run_tool(adb_, {"devices", "-l"}, [&](std::string_view s) { out += s; }, &error, stop);
  if (code != 0) {
    auto reason = first_error(error);
    throw std::runtime_error("adb could not list devices" + (reason.empty() ? "." : ": " + reason));
  }
  return parse_devices(out);
}
std::string Client::shell(const std::string &command, std::stop_token stop) const {
  auto args = base();
  args.insert(args.end(), {"shell", command});
  std::string out, error;
  const int code = run_tool(adb_, args, [&](std::string_view s) { out += s; }, &error, stop);
  if (stop.stop_requested()) throw std::runtime_error("Cancelled.");
  if (code != 0) {
    auto reason = first_error(error);
    throw std::runtime_error("The headset did not answer" + (reason.empty() ? "." : ": " + reason));
  }
  return out;
}
std::vector<App> Client::files(const std::vector<std::pair<std::string, std::int64_t>> &packages,
                               std::stop_token stop) const {
  // A few shell round trips: each package's APK paths from pm and every file
  // in its expansion folder and external storage (less its cache), with
  // sizes. Batches keep commands short for older adb versions.
  std::string listing;
  for (std::size_t i = 0; i < packages.size(); i += 16) {
    std::string script;
    for (std::size_t j = i; j < std::min(packages.size(), i + 16); ++j) {
      const auto &package = packages[j].first;
      script += "echo @" + package + "; for a in $(pm path " + package +
                " | cut -d: -f2); do stat -c '%s %n' \"$a\"; done; d=/sdcard/Android/obb/" + package +
                "; [ -d \"$d\" ] && find \"$d\" -type f -exec stat -c '%s %n' {} +; d=/sdcard/Android/data/" +
                package + "; [ -d \"$d\" ] && find \"$d\" -path \"$d/cache\" -prune -o -type f -exec stat -c '%s %n' {} +; ";
    }
    listing += shell(script + "true", stop);
  }
  return parse_files(listing, packages);
}
std::vector<App> Client::apps(std::stop_token stop) const {
  return files(parse_packages(shell("pm list packages -3 --show-versioncode", stop)), stop);
}
std::optional<App> Client::app(std::string_view package, std::stop_token stop) const {
  if (!paths::safe_package(package)) return std::nullopt;
  auto all = parse_packages(shell("pm list packages -3 --show-versioncode", stop));
  std::erase_if(all, [&](auto &p) { return p.first != package; });
  auto found = files(all, stop);
  if (found.empty()) return std::nullopt;
  return std::move(found.front());
}
void Client::pull(const RemoteFile &file, const std::function<void(std::span<const std::uint8_t>)> &sink,
                  std::stop_token stop, std::uint64_t offset) const {
  const auto quoted = quote(file.path);
  if (!quoted || offset % kPullBlock || offset > file.size)
    throw std::runtime_error("The headset reported an unsupported file name.");
  // exec-out is binary-clean (no pty), so the stream is the file itself.
  auto args = base();
  args.insert(args.end(), {"exec-out", offset ? std::format("dd if={} bs={} skip={} 2>/dev/null", *quoted,
                                                             kPullBlock, offset / kPullBlock)
                                              : "cat " + *quoted});
  std::uint64_t received = 0;
  std::string error;
  const int code = run_tool(
      adb_, args,
      [&](std::string_view chunk) {
        received += chunk.size();
        sink({reinterpret_cast<const std::uint8_t *>(chunk.data()), chunk.size()});
      },
      &error, stop);
  if (stop.stop_requested()) throw std::runtime_error("Transfer paused.");
  if (code != 0 || received != file.size - offset) {
    // exec-out drops the remote command's errors. Ask the headset again through
    // the shell, which reports them, to tell a changed or unreadable file from
    // a dropped connection.
    auto reason = first_error(error);
    std::string now;
    if (reason.empty() && !stop.stop_requested()) {
      try {
        now = shell("stat -c %s " + *quoted + " && cat " + *quoted + " >/dev/null && echo readable", stop);
      } catch (const std::exception &e) {
        reason = e.what();
      }
    }
    std::uint64_t size = 0;
    const bool readable = now.find("readable") != std::string::npos;
    const bool changed = readable && std::sscanf(now.c_str(), "%llu", reinterpret_cast<unsigned long long *>(&size)) == 1 &&
        size != file.size;
    const auto name = file.path.substr(file.path.rfind('/') + 1);
    log::warn("adb", "Transfer of {} stopped: exit {}, {} of {} bytes from offset {}; now {} bytes, readable {}; {}",
              file.path, code, received, file.size - offset, offset, size, readable, reason.empty() ? "no error" : reason);
    if (changed)
      throw std::runtime_error(name + " changed on the headset while it was copied. Close the game on the headset and retry.");
    if (!reason.empty())
      throw std::runtime_error("The headset could not send " + name + ": " + reason);
    throw std::runtime_error("The headset transfer of " + name + " stopped early. Check the cable and retry.");
  }
}
std::optional<std::string> Client::sha256(const RemoteFile &file, std::stop_token stop) const {
  const auto quoted = quote(file.path);
  if (!quoted) return std::nullopt;
  auto args = base();
  args.insert(args.end(), {"exec-out", "sha256sum " + *quoted + " 2>/dev/null"});
  std::string out;
  if (run_tool(adb_, args, [&](std::string_view s) { out += s; }, nullptr, stop) != 0 || out.size() < 64)
    return std::nullopt;
  auto hash = out.substr(0, 64);
  if (hash.find_first_not_of("0123456789abcdef") != std::string::npos) return std::nullopt;
  return hash;
}
std::string Client::connect(std::string_view address, std::stop_token stop) const {
  auto valid = !address.empty() && address.size() < 256 && std::ranges::all_of(address, [](unsigned char c) {
    return std::isalnum(c) || std::string_view(".:-_[]").find(char(c)) != std::string_view::npos;
  });
  if (!valid) throw std::runtime_error("Enter the headset's IP address, like 192.168.1.20 or 192.168.1.20:5555.");
  std::string out, error;
  run_tool(adb_, {"connect", std::string(address)}, [&](std::string_view s) { out += s; }, &error, stop);
  // adb connect reports failure in its output, often with exit status 0.
  const auto reply = first_error(out.empty() ? error : out);
  if (!reply.starts_with("connected to ") && !reply.starts_with("already connected to "))
    throw std::runtime_error("Could not connect to the headset" + (reply.empty() ? "." : ": " + reply));
  return reply.substr(reply.find("connected to ") + 13);
}
}  // namespace venture::android::adb
