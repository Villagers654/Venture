#include "common/download/asset_files.h"

#include <algorithm>

#include "common/meta/accounts.h"

namespace venture::download {
bool remove_asset(const fs::path& install, std::string_view app,
                  std::string_view binary, std::string_view filename) {
  if (!paths::within(install, install / "plan.json") ||
      !paths::within(install, install / "plan.json.tmp"))
    throw std::runtime_error("Unsafe asset manifest.");
  auto plan = meta::parse_plan(paths::read_json(install / "plan.json"));
  if (plan.app_id != app || plan.binary_id != binary)
    throw std::runtime_error("Asset installation changed.");
  auto found = std::ranges::find(plan.files, filename, &meta::File::name);
  if (found == plan.files.end() || found->kind != "asset")
    throw std::runtime_error("Unknown asset.");
  auto path = install / paths::from_utf8(found->name), partial = path;
  partial += ".part";
  for (auto& p : {path, partial})
    if (!paths::within(install, p) ||
        (fs::exists(p) && !fs::is_regular_file(p)))
      throw std::runtime_error("Unsafe asset file.");
  if (!found->sku.empty()) std::erase(plan.installed_addons, found->sku);
  paths::atomic_write(install / "plan.json", plan.json().dump(2));
  bool removed = fs::remove(path);
  return fs::remove(partial) || removed;
}
PipeChannel::Handler asset_launcher_handler(Manager& manager, fs::path data,
                                           fs::path root, fs::path install,
                                           std::string account) {
  auto launch = meta::parse_plan(paths::read_json(install / "plan.json"));
  if (fs::weakly_canonical(
          paths::game_dir(root, launch.package, launch.version_code)) !=
      fs::weakly_canonical(install))
    throw std::runtime_error("Invalid asset installation.");
  return [&manager, data = std::move(data), root = std::move(root),
          install = std::move(install), account = std::move(account),
          app = launch.app_id, binary = launch.binary_id](
             const Json& request) -> Json {
    try {
      auto op = str(request, "op"), id = str(request, "id"),
           name = str(request, "name");
      if (!op.starts_with("asset.") || !digits(id) ||
          !paths::safe_name(name))
        return {{"ok", false}};
      auto index = meta::AccountVault(data).index();
      auto entry = index.find(account);
      if (!entry || !entry->signed_in || entry->needs_reauth)
        return {{"ok", false}};
      if (!paths::within(install, install / "plan.json"))
        return {{"ok", false}};
      auto plan = meta::parse_plan(paths::read_json(install / "plan.json"));
      auto file = std::ranges::find(plan.files, name, &meta::File::name);
      if (plan.app_id != app || plan.binary_id != binary ||
          file == plan.files.end() || file->kind != "asset" || file->id != id)
        return {{"ok", false}};
      if (op == "asset.download") {
        auto session = meta::AccountVault(data).load(account);
        if (!session) return {{"ok", false}};
        manager.enqueue_asset(app, root, binary, id, name,
                              session->access_token, account);
      } else if (op == "asset.cancel") {
        return {{"ok", true},
                {"success", manager.cancel_asset(app, binary, name, account)}};
      } else if (op == "asset.delete") {
        return {{"ok", true},
                {"success", manager.delete_asset(install, app, binary, name)}};
      } else if (op != "asset.status")
        return {{"ok", false}};
      for (auto& job : manager.snapshot(account))
        if (job.app_id == app && job.binary_id == binary &&
            job.asset_id == id && job.asset_name == name)
          return {{"ok", true},
                  {"state", static_cast<int>(job.state)},
                  {"done", job.done},
                  {"total", job.total},
                  {"generation", job.generation}};
      return {{"ok", true}, {"state", -1}, {"done", 0}, {"total", file->size}};
    } catch (...) {
      return {{"ok", false}};
    }  // Never return vault/network exception text.
  };
}
}  // namespace venture::download
