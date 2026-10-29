#pragma once
#include "common/paths.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>

namespace venture {
// Cross-process lock; the file remains in place so every opener locks the same
// inode.
class FileLock {
public:
  explicit FileLock(const fs::path &file, bool wait = true);
  ~FileLock();
  FileLock(const FileLock &) = delete;
  FileLock &operator=(const FileLock &) = delete;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
std::string random_account_key();
bool is_link(const fs::path &path);
struct FileIdentity {
  std::uint64_t volume, file, size;
  std::int64_t write_seconds;
  std::uint32_t write_nanoseconds;
  bool operator==(const FileIdentity &) const = default;
};
// Native volume/file ID and modification metadata. Empty if unavailable or
// not a regular file; callers must treat this as a cache miss.
std::optional<FileIdentity> file_identity(const fs::path &path);
// Same-volume rename only, never copy/delete and never replace a destination.
std::error_code move_directory(const fs::path &source, const fs::path &target);
// Deletes links themselves, never their targets. The caller supplies the root
// boundary.
void remove_tree(const fs::path &root, const fs::path &target);
// A directory link at `link` (absent) to the directory `target`: a junction on
// Windows, which needs no privilege and may cross volumes; a symlink elsewhere.
void link_directory(const fs::path &link, const fs::path &target);
// Durably writes a file's data, or a directory's entries (a no-op on Windows,
// where renames are write-through).
void flush_file(const fs::path &file);
void flush_directory(const fs::path &directory);
} // namespace venture
