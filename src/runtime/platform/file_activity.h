#pragma once
#include <filesystem>
#include <memory>

namespace venture::platform {
// Watches only the game's writable data root. OS notifications also see JDK,
// SQLite and exported native descriptors, which bypass the HLE write wrappers.
// A lost notification or unreadable subtree fails closed; no filenames/content
// leave this interface. Notifications supplement logical in-flight write
// accounting and buffer flushing; cached/mapped writes may notify late. The
// caller owns the monotonic quiet-period clock and must not treat silence alone
// as a durability barrier.
enum class DataWriters { closed, open, unknown };
// Query native process handles, including JVM/NIO handles that bypass HLE.
// Silence is not sufficient: an open writer may still hold userspace buffers.
DataWriters data_writers(const std::filesystem::path& root) noexcept;
class FileActivity {
 public:
  explicit FileActivity(const std::filesystem::path& root);
  ~FileActivity();
  FileActivity(const FileActivity&) = delete;
  FileActivity& operator=(const FileActivity&) = delete;
  enum class Poll { quiet, changed, failed };
  Poll poll();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace venture::platform
