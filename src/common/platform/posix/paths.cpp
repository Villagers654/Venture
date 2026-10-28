#include "common/paths.h"
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
namespace venture::paths {
fs::path io_path(const fs::path &file) { return file; }
fs::path data_dir() {
  // VENTURE_DATA_DIR relocates all state (tests, portable installs).
  if (auto p = std::getenv("VENTURE_DATA_DIR"); p && *p)
    return fs::path(p);
  constexpr auto legacy_env = "QUESTPORT_DATA_DIR"; // rebrand: legacy override
  if (auto p = std::getenv(legacy_env); p && *p) return fs::path(p);
  static const fs::path directory = [] {
    constexpr auto legacy_name = "questport"; // rebrand: legacy default directory
    if (auto p = std::getenv("XDG_DATA_HOME"); p && fs::path(p).is_absolute())
      return migrate_data_dir(fs::path(p) / "venture", fs::path(p) / legacy_name);
    if (auto p = std::getenv("HOME"))
      return migrate_data_dir(fs::path(p) / ".local/share/venture", fs::path(p) / ".local/share" / legacy_name);
    throw std::runtime_error(
        "Set XDG_DATA_HOME or HOME to locate application data.");
  }();
  return directory;
}
fs::path executable() { return fs::read_symlink("/proc/self/exe"); }
void atomic_write(const fs::path &file, std::string_view text) {
  fs::create_directories(file.parent_path());
  auto tmp = file;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(text.data(), text.size());
    out.flush();
    if (!out)
      throw std::runtime_error("Could not save local file. Check free space "
                               "and folder permissions.");
  }
  chmod(tmp.c_str(), 0600);
  replace_file(tmp, file);
}
void replace_file(const fs::path &source, const fs::path &destination) {
  fs::rename(source, destination);
}
} // namespace venture::paths
