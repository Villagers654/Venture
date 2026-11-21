#include "common/layoutlib_component.h"
#include "common/android/archive.h"
#include "common/digest.h"
#include "common/http/client.h"
#include "common/json.h"
#include "common/log.h"
#include <algorithm>
#include <cctype>
#include <fstream>

namespace venture::layoutlib_component {
namespace {
bool hex64(std::string_view text) {
  return text.size() == 64 && std::ranges::all_of(text, [](unsigned char c) { return std::isxdigit(c); });
}
std::string lower(std::string text) {
  std::ranges::transform(text, text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return text;
}
}  // namespace

std::optional<Manifest> manifest(const fs::path &bin) {
  std::error_code ec;
  const auto file = bin / "layoutlib.json";
  if (!fs::is_regular_file(file, ec)) return std::nullopt;
  try {
    const auto j = paths::read_json(file);
    Manifest m{str(j, "url"), lower(str(j, "sha256")), 0};
    if (const auto &size = field(j, "size"); size.is_number_unsigned()) m.size = size.get<std::uint64_t>();
    if (!hex64(m.sha256) || !m.size || !http::allowed_release(m.url)) return std::nullopt;
    return m;
  } catch (...) {
    return std::nullopt;
  }
}

fs::path folder(const fs::path &data, const Manifest &m) {
  return data / "components" / ("layoutlib-" + m.sha256.substr(0, 16));
}

fs::path find(const fs::path &bin, const fs::path &data) {
  std::error_code ec;
  if (const auto staged = bin / "layoutlib" / "layoutlib.jar"; fs::is_regular_file(staged, ec)) return staged;
  if (const auto m = manifest(bin))
    if (const auto jar = folder(data, *m) / "layoutlib" / "layoutlib.jar"; fs::is_regular_file(jar, ec)) return jar;
  return {};
}

void unpack(const fs::path &archive, const fs::path &target, std::stop_token stop) {
  const auto temporary = fs::path(target).concat(".unpacking");
  std::error_code ec;
  fs::remove_all(temporary, ec);
  fs::create_directories(temporary);
  {
    android::Archive zip(archive);
    for (const auto &entry : zip.entries()) {
      if (entry.name.empty() || entry.name.back() == '/') continue;
      const auto path = temporary / paths::from_utf8(entry.name);
      if (!paths::within(temporary, path))
        throw std::runtime_error("The 2D component archive names a file outside its folder.");
      fs::create_directories(path.parent_path());
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      if (!out) throw std::runtime_error("Could not write " + paths::utf8(path) + ".");
      zip.extract(entry, [&](std::span<const std::uint8_t> bytes) {
        out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
      }, stop);
      if (!out) throw std::runtime_error("Could not write " + paths::utf8(path) + ". Is the disk full?");
    }
  }
  if (!fs::is_regular_file(temporary / "layoutlib" / "layoutlib.jar", ec))
    throw std::runtime_error("The 2D component archive has no layoutlib/layoutlib.jar.");
  fs::remove_all(target, ec);
  fs::rename(temporary, target);
}

fs::path ensure(const fs::path &bin, const fs::path &data, std::stop_token stop, const Progress &progress) {
  if (auto jar = find(bin, data); !jar.empty()) return jar;
  const auto m = manifest(bin);
  if (!m) return {};
  const auto target = folder(data, *m);
  const auto download = data / "components" / ("layoutlib-" + m->sha256.substr(0, 16) + ".zip");
  log::info("layoutlib", "Downloading the Android 2D component ({} MB) from {}", m->size >> 20, m->url);
  http::download_release(m->url, download, m->size, stop, [&](std::uint64_t done, std::uint64_t total) {
    if (progress) progress(done, total);
  });
  {
    std::ifstream in(download, std::ios::binary);
    if (lower(digest::sha256(in)) != m->sha256) {
      in.close();
      std::error_code ec;
      fs::remove(download, ec);
      throw std::runtime_error("The downloaded Android 2D component did not match its checksum. Try again.");
    }
  }
  unpack(download, target, stop);
  std::error_code ec;
  fs::remove(download, ec);
  log::info("layoutlib", "The Android 2D component is installed in {}", paths::utf8(target));
  return target / "layoutlib" / "layoutlib.jar";
}
}  // namespace venture::layoutlib_component
