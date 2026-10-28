// Read access to the game's packaged content: APK entries (assets/, res/,
// resources.arsc, lib/) and expansion files (OBB). One implementation serves
// the Java AssetManager, the NDK AAssetManager HLE and the VFS, so all three see
// identical content.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace venture::android {

struct AssetEntry {
  std::string name;        // path inside the archive, e.g. "assets/bin/Data/data.unity3d"
  std::uint64_t size = 0;  // uncompressed size
  bool compressed = false;
  std::uint64_t data_offset = 0;  // offset of the (stored) data in the archive file, when !compressed
};

class AssetStream {
 public:
  virtual ~AssetStream() = default;
  virtual std::int64_t read(void* out, std::uint64_t size) = 0;    // bytes read, <0 on error
  virtual std::int64_t seek(std::int64_t offset, int whence) = 0;  // SEEK_SET/CUR/END
  virtual std::uint64_t length() const = 0;
  // Host file + offset when the entry is stored uncompressed (AAsset_openFileDescriptor).
  virtual bool file_range(std::string* host_path, std::uint64_t* offset) const = 0;
};

// Publish only a complete stream. An interrupted copy leaves the previous
// destination intact; the temporary file is overwritten on retry.
bool extract_asset_atomic(AssetStream& stream, const std::filesystem::path& destination);

class PackageAssets {
 public:
  static PackageAssets& get();
  // Archives searched in order: base APK, then main/patch OBBs (their own namespaces).
  bool open_apk(const std::string& host_apk_path);
  // Adds an expansion ZIP. Entries are accessible as <filename>.obb/<entry>
  // and, after APK entries, by their original path. open_apk automatically adds
  // sibling main/patch OBBs in sorted order. Existing streams survive reloads.
  bool open_obb(const std::string& host_obb_path);
  std::optional<AssetEntry> find(std::string_view name) const;      // full archive path
  std::vector<std::string> list(std::string_view directory) const;  // immediate children
  std::unique_ptr<AssetStream> open(std::string_view name) const;
  // Validate Unity's IL2CPP extraction before any guest code can consume it.
  // Cached checks use ZIP central-directory sizes, never hash extracted files.
  bool prepare_unity_extraction(const std::filesystem::path& external_files) const;
};

}  // namespace venture::android
