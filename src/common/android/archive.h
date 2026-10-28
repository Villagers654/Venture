#pragma once
#include "common/paths.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <vector>

namespace venture::android {
// A read-only ZIP archive (an APK, or a game packed for import). Members are
// streamed, so multi-gigabyte expansion files never sit in memory.
class Archive {
public:
  struct Entry {
    std::string name;  // as stored, '/'-separated
    std::uint64_t size = 0;
    unsigned index = 0;
  };
  explicit Archive(const fs::path &file);  // throws for a missing or non-ZIP file
  ~Archive();
  Archive(const Archive &) = delete;
  Archive &operator=(const Archive &) = delete;
  const std::vector<Entry> &entries() const { return entries_; }
  const Entry *find(std::string_view name) const;
  // The whole member, for small ones (manifests); throws above `limit`.
  std::vector<std::uint8_t> read(const Entry &entry, std::uint64_t limit = 16 << 20) const;
  // Streams a member to `sink`, checking its CRC; throws when stopped.
  void extract(const Entry &entry, const std::function<void(std::span<const std::uint8_t>)> &sink,
               std::stop_token stop = {}) const;

  struct State;  // the open file and miniz reader

private:
  std::unique_ptr<State> state_;
  std::vector<Entry> entries_;
};
bool is_archive(const fs::path &file);  // a ZIP by its signature
}  // namespace venture::android
