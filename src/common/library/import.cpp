#include "common/library/import.h"

#include "common/android/archive.h"
#include "common/log.h"

#include <algorithm>
#include <fstream>
#include <set>

namespace venture::import {
namespace {
std::string lower(std::string s) {
  std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}
bool is_apk(const fs::path &p) { return lower(paths::utf8(p.extension())) == ".apk"; }
constexpr std::size_t kScanLimit = 20000;  // directory entries looked at per pick

// Loose files on this computer.
class LocalSource final : public Source {
public:
  explicit LocalSource(fs::path picked) : picked_(std::move(picked)) {}
  std::string spec() const override { return "path:" + paths::utf8(picked_); }
  std::string describe() const override { return paths::utf8(picked_); }

protected:
  void read_from(const SourceFile &file, std::uint64_t offset,
                 const std::function<void(std::span<const std::uint8_t>)> &sink,
                 std::stop_token stop) const override {
    std::ifstream in(paths::from_utf8(file.location), std::ios::binary);
    if (!in) throw std::runtime_error("Could not read " + file.name + " from the import folder.");
    in.seekg(std::streamoff(offset));
    std::vector<std::uint8_t> buffer(1 << 20);
    std::uint64_t total = offset;
    while (in) {
      if (stop.stop_requested()) throw std::runtime_error("Transfer paused.");
      in.read(reinterpret_cast<char *>(buffer.data()), std::streamsize(buffer.size()));
      const auto n = std::size_t(in.gcount());
      if (!n) break;
      total += n;
      sink({buffer.data(), n});
    }
    if (in.bad() || total != file.size)
      throw std::runtime_error(file.name + " changed or could not be read while importing.");
  }

private:
  fs::path picked_;
};

// Members of a ZIP archive.
class ArchiveSource final : public Source {
public:
  ArchiveSource(fs::path file, std::shared_ptr<android::Archive> archive)
      : file_(std::move(file)), archive_(std::move(archive)) {}
  std::string spec() const override { return "path:" + paths::utf8(file_); }
  std::string describe() const override { return paths::utf8(file_); }

protected:
  void read_from(const SourceFile &file, std::uint64_t offset,
                 const std::function<void(std::span<const std::uint8_t>)> &sink,
                 std::stop_token stop) const override {
    const auto *entry = archive_->find(file.location);
    if (!entry || entry->size != file.size) throw std::runtime_error(file.name + " is no longer in the archive.");
    // Compressed members only read from the start; skip what is already there.
    std::uint64_t skip = offset;
    archive_->extract(
        *entry,
        [&](std::span<const std::uint8_t> chunk) {
          const auto n = std::min<std::uint64_t>(skip, chunk.size());
          skip -= n;
          if (n < chunk.size()) sink(chunk.subspan(n));
        },
        stop);
  }

private:
  fs::path file_;
  std::shared_ptr<android::Archive> archive_;
};

// An app on a headset.
class DeviceSource final : public Source {
public:
  explicit DeviceSource(android::adb::Client client) : client_(std::move(client)) {}
  std::string spec() const override { return "adb:" + client_.serial(); }
  std::string describe() const override { return "headset " + client_.serial(); }
  std::optional<std::string> sha256(const SourceFile &file, std::stop_token stop) const override {
    return client_.sha256({file.location, file.size}, stop);
  }

protected:
  void read_from(const SourceFile &file, std::uint64_t offset,
                 const std::function<void(std::span<const std::uint8_t>)> &sink,
                 std::stop_token stop) const override {
    client_.pull({file.location, file.size}, sink, stop, offset);
  }

private:
  android::adb::Client client_;
};

// Data files for `package` in a directory listing: a folder named after the
// package (anywhere up to two levels down, the Android/obb layout included),
// else expansion files named for it beside the APK.
template <class Listing>
void add_data(Source &source, const Listing &listing, std::string_view apk_location,
              std::string_view root_name = {}) {
  const auto package = source.manifest.package;
  const auto folder = "/" + package + "/";
  std::set<std::string> names;
  for (auto &[location, size] : listing) {
    const auto slash = "/" + location;
    auto at = slash.rfind(folder);
    // Reopening an APK inside its package-named directory makes that
    // directory the scan root. Keep its loose data, just as a parent pick does.
    const bool package_root = at == std::string::npos && root_name == package;
    if ((at == std::string::npos && !package_root) || location == apk_location) continue;
    auto rest = package_root ? location : slash.substr(at + folder.size());
    if (rest.find('/') != std::string::npos) {
      source.skipped.push_back(package + "/" + rest);
      continue;
    }
    if (!paths::safe_name(rest) || lower(rest) == "plan.json" || lower(rest).ends_with(".part") ||
        !names.insert(lower(rest)).second) {
      source.skipped.push_back(rest);
      continue;
    }
    source.data.push_back({rest, size, location});
  }
  if (!source.data.empty()) return;
  // Expansion files beside the APK: [main|patch].<version>.<package>.obb.
  const auto dir = std::string(apk_location.substr(0, apk_location.rfind('/') + 1));
  for (auto &[location, size] : listing) {
    if (!location.starts_with(dir) || location.find('/', dir.size()) != std::string::npos) continue;
    const auto name = location.substr(dir.size());
    if (lower(name).ends_with("." + lower(package) + ".obb") && names.insert(lower(name)).second)
      source.data.push_back({name, size, location});
  }
}
// Split APKs (split_config.*.apk) belong to their base; Meta builds are a
// single APK, so they are reported and left out.
bool split_apk(std::string_view name) { return lower(std::string(name)).starts_with("split_"); }
}  // namespace

std::uint64_t Source::bytes() const {
  std::uint64_t n = apk.size;
  for (auto &f : data) n += f.size;
  return n;
}
std::uint64_t Source::save_bytes() const {
  std::uint64_t n = 0;
  for (auto &f : saves) n += f.size;
  return n;
}

std::vector<std::unique_ptr<Source>> open_local(const fs::path &picked, std::stop_token stop) {
  std::vector<std::unique_ptr<Source>> out;
  std::error_code ec;
  if (fs::is_regular_file(picked, ec) && !is_apk(picked)) {
    if (!android::is_archive(picked))
      throw std::runtime_error("Choose an APK, a folder, or a ZIP archive holding an APK and its data folder.");
    auto archive = std::make_shared<android::Archive>(picked);
    std::vector<std::pair<std::string, std::uint64_t>> listing;
    for (auto &e : archive->entries()) listing.emplace_back(e.name, e.size);
    for (auto &e : archive->entries()) {
      if (!is_apk(paths::from_utf8(e.name)) || std::ranges::count(e.name, '/') > 2) continue;
      const auto base = e.name.substr(e.name.rfind('/') + 1);
      if (split_apk(base)) continue;
      // An APK inside an archive is read in memory for its manifest only.
      android::Manifest manifest;
      try {
        if (e.size > (1ull << 31)) throw std::runtime_error("too large to inspect");
        std::vector<std::uint8_t> bytes;
        bytes.reserve(e.size);
        archive->extract(e, [&](auto chunk) { bytes.insert(bytes.end(), chunk.begin(), chunk.end()); }, stop);
        manifest = android::read_apk(bytes);
      } catch (const std::exception &error) {
        log::warn("import", "Skipped {} in an archive: {}", e.name, error.what());
        continue;
      }
      auto source = std::make_unique<ArchiveSource>(picked, archive);
      source->manifest = manifest;
      source->apk = {base, e.size, e.name};
      add_data(*source, listing, e.name);
      out.push_back(std::move(source));
    }
  } else if (fs::is_regular_file(picked, ec) || fs::is_directory(picked, ec)) {
    const bool file = fs::is_regular_file(picked, ec);
    const auto root = file ? picked.parent_path() : picked;
    // Files under the root, up to three levels deep, '/'-separated.
    std::vector<std::pair<std::string, std::uint64_t>> listing;
    auto options = fs::directory_options::skip_permission_denied;
    for (auto it = fs::recursive_directory_iterator(root, options, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (stop.stop_requested()) throw std::runtime_error("Cancelled.");
      if (listing.size() >= kScanLimit) break;
      if (it.depth() >= 3) it.disable_recursion_pending();
      std::error_code entry_ec;
      if (!it->is_regular_file(entry_ec)) continue;
      auto relative = paths::utf8(it->path().lexically_relative(root));
      std::ranges::replace(relative, '\\', '/');
      listing.emplace_back(std::move(relative), it->file_size(entry_ec));
    }
    for (auto &[relative, size] : listing) {
      const auto path = root / paths::from_utf8(relative);
      if (!is_apk(path) || std::ranges::count(relative, '/') > 1) continue;
      if (file && relative != paths::utf8(picked.filename())) continue;
      if (split_apk(paths::utf8(path.filename()))) continue;
      android::Manifest manifest;
      try {
        manifest = android::read_apk(path);
      } catch (const std::exception &error) {
        log::warn("import", "Skipped {}: {}", paths::utf8(path.filename()), error.what());
        if (file) throw;
        continue;
      }
      auto source = std::make_unique<LocalSource>(file ? picked : path);
      source->manifest = manifest;
      source->apk = {paths::utf8(path.filename()), size, paths::utf8(path)};
      add_data(*source, listing, relative, paths::utf8(root.filename()));
      for (auto &d : source->data) d.location = paths::utf8(root / paths::from_utf8(d.location));
      out.push_back(std::move(source));
    }
    if (out.empty())
      throw std::runtime_error(file ? "This APK could not be read." : "No Android app (APK) was found in this folder.");
  } else {
    throw std::runtime_error("The chosen file or folder no longer exists.");
  }
  if (out.empty()) throw std::runtime_error("No Android app (APK) was found in this archive.");
  return out;
}

std::unique_ptr<Source> open_device(const android::adb::Client &client, const android::adb::App &app) {
  auto source = std::make_unique<DeviceSource>(client);
  source->manifest.package = app.package;
  source->manifest.version_code = app.version_code;
  const auto &base = app.apks.front();
  source->apk = {base.path.substr(base.path.rfind('/') + 1), base.size, base.path};
  for (std::size_t i = 1; i < app.apks.size(); ++i)
    source->skipped.push_back(app.apks[i].path.substr(app.apks[i].path.rfind('/') + 1));
  std::vector<std::pair<std::string, std::uint64_t>> listing;
  for (auto &f : app.data) listing.emplace_back(f.path, f.size);
  add_data(*source, listing, base.path);
  // Saves keep their folders; each part must be a valid file name here too.
  const auto external = "/sdcard/Android/data/" + app.package + "/";
  for (auto &f : app.saves) {
    const auto relative = f.path.substr(external.size());
    bool valid = !relative.empty();
    for (std::size_t at = 0; valid && at <= relative.size();) {
      const auto end = std::min(relative.find('/', at), relative.size());
      const auto part = relative.substr(at, end - at);
      valid = paths::safe_name(part) && part != "." && part != "..";
      at = end + 1;
    }
    if (valid) source->saves.push_back({relative, f.size, f.path});
    else source->skipped.push_back("Android/data/" + app.package + "/" + relative);
  }
  return source;
}

std::unique_ptr<Source> reopen(std::string_view spec, std::string_view package, const fs::path &adb,
                               std::stop_token stop) {
  if (spec.starts_with("path:")) {
    for (auto &source : open_local(paths::from_utf8(spec.substr(5)), stop))
      if (source->manifest.package == package) return std::move(source);
    throw std::runtime_error("The import folder no longer holds this game.");
  }
  if (spec.starts_with("adb:")) {
    if (adb.empty()) throw std::runtime_error("Android platform-tools (adb) were not found. Set adb in Settings.");
    android::adb::Client client(adb, std::string(spec.substr(4)));
    auto devices = client.devices(stop);
    auto device = std::ranges::find(devices, client.serial(), &android::adb::Device::serial);
    if (device == devices.end()) throw std::runtime_error("Connect the headset to resume this import.");
    if (!device->ready()) throw std::runtime_error(android::adb::state_message(*device));
    auto app = client.app(package, stop);
    if (!app) throw std::runtime_error("The game is no longer installed on the headset.");
    return open_device(client, *app);
  }
  throw std::runtime_error("Unknown import source.");
}

Plan plan(const meta::Plan &catalog, const Source &source) {
  Plan out{catalog, {}};
  std::set<std::string> catalog_names, used;
  for (auto &f : catalog.files) catalog_names.insert(lower(f.name));
  auto local = [&](const meta::File &f) -> const SourceFile * {
    if (f.kind == "apk") return &source.apk;
    for (auto &d : source.data)
      if (lower(d.name) == lower(f.name) && d.size == f.size) return &d;
    return nullptr;
  };
  for (auto &f : out.plan.files) {
    const auto *from = local(f);
    if (from) used.insert(lower(from->name));
    // The source's APK, possibly a modified build, is the one installed.
    if (f.kind == "apk" && f.size != from->size) {
      f.size = from->size;
      f.sha256.clear();
    }
    // Required base files always; other catalog files (languages, owned
    // add-ons) only when the source brings them.
    if (f.base() || (from && f.kind != "apk")) out.steps.push_back({f, from});
  }
  for (auto &d : source.data) {
    if (used.contains(lower(d.name)) || catalog_names.contains(lower(d.name))) continue;
    meta::File extra{{}, d.name, {}, "asset", d.size};
    extra.required = false;
    out.plan.files.push_back(extra);
    out.steps.push_back({extra, &d});
  }
  return out;
}

Plan local_plan(const Source &source, std::string app_id) {
  Plan out;
  auto &p = out.plan;
  p.app_id = app_id.empty() ? source.manifest.package : std::move(app_id);
  p.package = source.manifest.package;
  p.version_code = std::to_string(source.manifest.version_code);
  p.version = source.manifest.version_name.empty() ? p.version_code : source.manifest.version_name;
  p.local = true;
  p.files.push_back({{}, p.package + ".apk", {}, "apk", source.apk.size});
  for (auto &d : source.data)
    if (lower(d.name) != lower(p.files.front().name))
      p.files.push_back({{}, d.name, {}, meta::expansion_name(d.name, p.package) ? "obb" : "asset", d.size});
  out.steps.push_back({p.files.front(), &source.apk});
  for (std::size_t i = 1; i < p.files.size(); ++i)
    for (auto &d : source.data)
      if (d.name == p.files[i].name) out.steps.push_back({p.files[i], &d});
  return out;
}

std::string binary_for(const Json &builds, std::int64_t version_code) {
  for (auto &b : builds)
    if (number(b, "version_code") == std::uint64_t(version_code) && digits(str(b, "id"))) return str(b, "id");
  return {};
}
}  // namespace venture::import
