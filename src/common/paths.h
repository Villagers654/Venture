#pragma once
#include "common/json.h"
#include <filesystem>

namespace venture {
namespace fs = std::filesystem;
namespace paths {
fs::path data_dir();
// Atomic default-directory migration; explicit roots make tests independent of
// the owner's profile. Failure returns legacy without copying or deleting data.
fs::path migrate_data_dir(const fs::path &current, const fs::path &legacy);
fs::path executable();
std::string utf8(const fs::path &path);
fs::path from_utf8(std::string_view text);
// A guest (Linux) file name component as a host one: on Windows the characters NTFS
// forbids (:*?"<>| and controls) become U+F000+c, as Cygwin, MSYS and WSL store them;
// unchanged elsewhere. guest_name reverses it for directory listings.
std::string host_name(std::string_view guest);
std::string guest_name(std::string_view host);
bool safe_name(std::string_view name);
bool safe_package(std::string_view name);
bool within(const fs::path &root, const fs::path &target);
// Reject symlinks/junctions escaping the selected root before
// installing/removing.
fs::path game_dir(const fs::path &root, std::string_view package,
                  std::string_view version);
// Path for standard-library/native file I/O: extended namespace on Windows,
// unchanged on POSIX. Keeps platform path rules out of cache callers.
fs::path io_path(const fs::path &file);
std::string read(const fs::path &file, size_t limit = 16 * 1024 * 1024);
void atomic_write(const fs::path &file, std::string_view bytes);
void replace_file(const fs::path &source, const fs::path &destination);
Json read_json(const fs::path &file);
std::string timestamp();
} // namespace paths
} // namespace venture
