#include "launcher/drives.h"
#include "common/paths.h"
#include <algorithm>
#include <cctype>
#ifdef _WIN32
#include <windows.h>
#else
#include <fstream>
#include <sstream>
#include <unistd.h>
#endif
namespace venture::launcher {
namespace {
bool separator(char c) { return c == '/' || c == '\\'; }
std::string key(const fs::path &path) {
  auto text = paths::utf8(path.lexically_normal());
#ifdef _WIN32
  std::ranges::transform(text, text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
#endif
  while (text.size() > 1 && separator(text.back())) text.pop_back();
  return text;
}
// The volume holding `folder`: the longest root that contains it.
const Volume *volume_of(const fs::path &folder, const std::vector<Volume> &volumes) {
  const auto path = key(folder);
  const Volume *best = nullptr;
  size_t length = 0;
  for (const auto &v : volumes) {
    const auto root = key(v.root);
    const bool inside = path == root ||
        (path.starts_with(root) && (separator(root.back()) || separator(path[root.size()])));
    if (inside && root.size() >= length) best = &v, length = root.size();
  }
  return best;
}
} // namespace

std::vector<InstallLocation> install_locations(const library::Settings &settings,
                                               const std::vector<Volume> &volumes) {
  std::vector<InstallLocation> result;
  std::vector<const Volume *> used;
  for (const auto &folder : settings.roots()) {
    const auto *v = volume_of(folder, volumes);
    if (v) used.push_back(v);
    result.push_back({folder, v ? v->name : std::string("Local drive"), true});
  }
  for (const auto &v : volumes)
    if (std::ranges::find(used, &v) == used.end())
      result.push_back({v.root / "Venture Games", v.name, false});
  return result;
}

uint64_t free_space(const fs::path &folder) {
  std::error_code error;
  for (auto at = folder; !at.empty(); at = at.parent_path()) {
    if (fs::exists(at, error)) {
      auto space = fs::space(at, error);
      return error ? 0 : space.available;
    }
    if (at == at.parent_path()) break;
  }
  return 0;
}

#ifdef _WIN32
std::vector<Volume> local_volumes() {
  std::vector<Volume> result;
  wchar_t drives[512];
  const DWORD n = GetLogicalDriveStringsW(DWORD(std::size(drives)), drives);
  if (!n || n >= std::size(drives)) return result;
  // A card reader or optical drive without media fails GetVolumeInformation
  // without asking the user to insert a disk.
  const UINT mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
  for (const wchar_t *d = drives; *d; d += wcslen(d) + 1) {
    const UINT type = GetDriveTypeW(d);
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
    wchar_t label[MAX_PATH + 1] = {};
    if (!GetVolumeInformationW(d, label, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0)) continue;
    std::wstring root(d);
    std::string name = paths::utf8(fs::path(root.substr(0, 2))) + "  " +
        (*label ? paths::utf8(fs::path(label)) : type == DRIVE_REMOVABLE ? "Removable Disk" : "Local Disk");
    result.push_back({fs::path(root), std::move(name)});
  }
  SetErrorMode(mode);
  return result;
}
#else
std::vector<Volume> local_volumes() {
  // Mount points hold octal escapes for spaces and the like.
  auto unescape = [](const std::string &text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
      if (text[i] == '\\' &&i + 3 < text.size() && std::isdigit((unsigned char)text[i + 1])) {
        out += char(std::stoi(text.substr(i + 1, 3), nullptr, 8));
        i += 3;
      } else {
        out += text[i];
      }
    return out;
  };
  std::vector<Volume> result;
  std::ifstream mounts("/proc/self/mounts");
  std::string line;
  while (std::getline(mounts, line)) {
    std::istringstream fields(line);
    std::string device, point;
    if (!(fields >> device >> point) || !device.starts_with("/dev/")) continue;
    point = unescape(point);
    // The home folder, and drives the desktop mounted for the user or that
    // were mounted under /mnt, where the user can write.
    const bool home = point == "/home" || point == "/var/home";
    if (!home && !point.starts_with("/run/media/") && !point.starts_with("/media/") &&
        !point.starts_with("/mnt/") && !point.starts_with("/var/mnt/"))
      continue;
    if (access(point.c_str(), W_OK) != 0) continue;
    if (std::ranges::any_of(result, [&](auto &v) { return v.root == point; })) continue;
    const fs::path root(point);
    result.push_back({root, home ? std::string("Home") : paths::utf8(root.filename())});
  }
  return result;
}
#endif
} // namespace venture::launcher
