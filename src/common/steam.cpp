#include "common/steam.h"
#include "common/log.h"
#include <limits>
#include <set>

namespace venture::steam {
std::string Plan::compatibility_key() const { return "steam:" + std::to_string(appid); }
Plan parse_plan(const Json& j) {
  if (str(j, "source") != "steam") throw std::runtime_error("Not a Steam launch plan");
  const auto app = number(j, "appid"), depot = number(j, "depot"), version = number(j, "versionCode");
  if (!app || app > UINT32_MAX || !depot || depot > UINT32_MAX || !version || version > INT64_MAX)
    throw std::runtime_error("Invalid Steam plan identity");
  Plan p{static_cast<std::uint32_t>(app), static_cast<std::uint32_t>(depot), str(j, "manifest"),
         str(j, "name"), str(j, "package"), str(j, "version"), static_cast<std::int64_t>(version), {}};
  if (!digits(p.manifest, 1, 20) || !number(j, "manifest") || !paths::safe_package(p.package))
    throw std::runtime_error("Invalid Steam manifest or Android package");
  std::set<std::string> names;
  unsigned apks = 0;
  if (!field(j, "files").is_array()) throw std::runtime_error("Steam plan has no files");
  for (const auto& f : j.at("files")) {
    File file{str(f, "name"), str(f, "kind"), number(f, "size")};
    const auto path = paths::from_utf8(file.name);
    if (file.name.empty() || path.is_absolute() || path.has_root_name() || file.name.find_first_of("\\:") != std::string::npos)
      throw std::runtime_error("Invalid Steam plan filename");
    for (const auto& part : path) if (part == ".." || part == ".") throw std::runtime_error("Unsafe Steam plan path");
    if (!names.insert(file.name).second || file.name == "plan.json") throw std::runtime_error("Duplicate Steam file");
    if (file.kind == "apk") {
      ++apks;
      if (path.has_parent_path() || path.extension() != ".apk") throw std::runtime_error("Steam launch APK must be beside plan");
    } else if (file.kind != "obb" && file.kind != "asset") throw std::runtime_error("Invalid Steam file kind");
    p.files.push_back(std::move(file));
  }
  if (apks != 1) throw std::runtime_error("Steam plan needs exactly one launch APK");
  return p;
}
std::vector<Install> scan(const fs::path& games) {
  std::vector<Install> result;
  if (!fs::is_directory(games)) return result;
  for (const auto& app : fs::directory_iterator(games)) {
    const auto name = paths::utf8(app.path().filename());
    if (!name.starts_with("steam-") || !digits(std::string_view(name).substr(6)) || !app.is_directory() || !paths::within(games, app.path())) continue;
    for (const auto& manifest : fs::directory_iterator(app.path())) {
      if (!manifest.is_directory() || !digits(paths::utf8(manifest.path().filename())) || !paths::within(games, manifest.path())) continue;
      try {
        auto plan = parse_plan(paths::read_json(manifest.path() / "plan.json"));
        if (name != "steam-" + std::to_string(plan.appid) || paths::utf8(manifest.path().filename()) != plan.manifest) continue;
        Install install{std::move(plan), manifest.path(), true};
        for (const auto& f : install.plan.files) {
          const auto path = install.directory / paths::from_utf8(f.name);
          if (!paths::within(install.directory, path) || !fs::is_regular_file(path) || fs::file_size(path) != f.size) install.complete = false;
        }
        result.push_back(std::move(install));
      } catch (const std::exception&) { log::warn("steam", "Skipped an unreadable Steam installation plan"); }
    }
  }
  return result;
}
} // namespace venture::steam
