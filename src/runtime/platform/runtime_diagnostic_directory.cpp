#include "platform/runtime_child.h"
#include "common/environment.h"
#include "common/paths.h"
namespace venture::platform {
std::filesystem::path runtime_dump_directory(const std::filesystem::path& directory) {
  const auto* local = environment::getenv("LOCALAPPDATA");
  if (directory.empty() || !local || !*local) return {};
  try {
    const auto root = paths::from_utf8(local) / "Venture/dev-cache/runs";
    const auto run = std::filesystem::weakly_canonical(directory);
    const auto dumps = run / "dumps";
    if (!paths::within(paths::from_utf8(local), root) || !paths::within(root, run) ||
        paths::within(run, root) || !paths::within(root, dumps) || !paths::within(run, dumps)) return {};
    return dumps;
  } catch (const std::filesystem::filesystem_error&) { return {}; }
}
}
