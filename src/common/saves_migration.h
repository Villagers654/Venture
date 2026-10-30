#pragma once
#include "common/paths.h"
#include <span>
namespace venture {
struct SaveMigration {
  std::size_t moved = 0, conflicts = 0, deferred = 0;
};
// Idempotent same-volume moves. Unreadable, linked or busy roots stay in place.
SaveMigration migrate_saves(const fs::path &data, std::string_view primary,
                            bool adopt_local = false);
fs::path account_data_dir(const fs::path &data, std::string_view key,
                          std::string_view package);
// Where the player puts replacement expansion/asset files (mods) for a game:
// <data>/mods/<package>. Outside every installation, so downloads, updates and
// their size checks never see or overwrite them.
fs::path mods_dir(const fs::path &data, std::string_view package);
// Reconcile a dedicated Java/native OBB directory with the installed plan.
// Hard links are an optimization; other filesystems use atomically published copies.
// A same-named file in `mods` replaces the installed one in the view.
void sync_obb_view(const fs::path& install, const fs::path& view,
                   std::span<const std::string> names, const fs::path& mods = {});
void publish_obb_file(const fs::path& source, const fs::path& view);
// Where an installation's view goes: `view` when its folder can hard-link the
// installed files, else beside the installation under .obb/<account>, so a games
// folder on another drive never fills the data drive with copies. Copies an
// earlier fallback left at `view` are removed.
fs::path obb_view_for(const fs::path& install, const fs::path& view, std::string_view account);
void remove_cached_obb_aliases(const fs::path& data, std::string_view package);
// Removes dedicated OBB views (including legacy cache views), preserving saves.
void remove_obb_aliases(const fs::path &data, std::string_view package,
                        const fs::path &install);
} // namespace venture
