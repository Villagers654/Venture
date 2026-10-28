#pragma once
#include "common/download/manager.h"
#include "common/ipc.h"

namespace venture::download {
// Removes one catalog asset only, and invalidates its SKU's completion marker.
bool remove_asset(const fs::path& install, std::string_view app,
                  std::string_view binary, std::string_view filename);
// Bind every request to the launch's install and account, independently of the
// launcher's current UI selection. The guest supplies only an op/id/name.
PipeChannel::Handler asset_launcher_handler(Manager&, fs::path data, fs::path root,
                                           fs::path install, std::string account);
}  // namespace venture::download
