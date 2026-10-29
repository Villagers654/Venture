#pragma once
// Where a new game can be installed: the games folder, the other library
// folders, and a "Venture Games" folder on each other local drive.
#include "common/library/library.h"
#include <cstdint>
#include <string>
#include <vector>
namespace venture::launcher {
struct Volume {
  fs::path root;     // C:\ or a mount point
  std::string name;  // its label, or a name from the mount point
};
// Local drives with media (Windows) or the user's mounted drives (Linux).
std::vector<Volume> local_volumes();
struct InstallLocation {
  fs::path folder;
  std::string drive;  // "C:  Local Disk", or the mount's name
  bool library = false;  // the games folder or another library folder
};
// The library folders first (the games folder leads), then one new folder on
// each volume that holds none.
std::vector<InstallLocation> install_locations(const library::Settings &settings,
                                               const std::vector<Volume> &volumes);
// Free bytes where `folder` is, or would be created; 0 when unknown.
uint64_t free_space(const fs::path &folder);
} // namespace venture::launcher
