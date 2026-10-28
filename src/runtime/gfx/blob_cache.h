#pragma once
// EGL_ANDROID_blob_cache for guest GLES on ANGLE. AOSP libEGL gives every app
// one persistent blob cache (frameworks/native/opengl/libs/EGL/egl_cache.cpp,
// BlobCache.cpp, FileBlobCache.cpp): the framework names the file
// (ActivityThread.setupGraphicsSupport -> HardwareRenderer.setupDiskCache ->
// egl_set_cache_filename) and libEGL installs set/get callbacks on the driver
// display at initialization. ANGLE keeps its linked programs, compiled
// shaders and Vulkan pipeline cache there.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace venture::gfx {
// Thread-safe key/value store with AOSP BlobCache semantics.
class BlobStore {
 public:
  struct Limits {
    std::size_t key, value, total;
  };
  explicit BlobStore(Limits limits);
  // Pairs over a limit are not cached. When the total limit would be
  // exceeded, random entries are evicted until at most half of it is used.
  void set(const void* key, std::size_t key_size, const void* value,
           std::size_t value_size);
  // Returns the value size (0 when absent); copies only if it fits.
  std::size_t get(const void* key, std::size_t key_size, void* value,
                  std::size_t value_size) const;
  std::size_t entries() const;
  std::size_t total_bytes() const;
  // Adds the file's entries absent from the store. A file whose header
  // (format, identity), sizes or checksum do not match contributes nothing.
  bool load(const std::filesystem::path& file, std::string_view identity);
  // Writes a temporary file and renames it over `file`.
  bool save(const std::filesystem::path& file, std::string_view identity) const;

 private:
  struct Entry {
    std::string key;
    std::shared_ptr<const std::string> value;
  };
  void set_locked(std::string key, std::shared_ptr<const std::string> value,
                  bool replace);
  Limits limits_;
  mutable std::mutex lock_;
  std::vector<Entry> entries_;  // sorted by key, as BlobCache
  std::size_t total_ = 0;
  std::uint64_t random_;
};

// HardwareRenderer.setupDiskCache: the app's persistent cache file.
void set_blob_cache_file(const std::filesystem::path& file);
// egl_cache_t::initialize: install the callbacks on the initialized display.
void install_blob_cache(void* display);
// Flush pending blobs on pause/restart as well as atexit. No-op until loaded.
void flush_blob_cache() noexcept;
}  // namespace venture::gfx
