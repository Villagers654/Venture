#include "android/errors.h"
#include "android/path.h"
#include "android/package_assets.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <unordered_map>
#include <zlib.h>

#include "common/file_ops.h"
#include "common/log.h"
#include "common/paths.h"
#ifdef _WIN32
#include "common/platform/win32/native_path.h"
#endif

namespace venture::android {
namespace {
// ZIP APPNOTE 6.3.10 sections 4.3 / 4.5.3: central sizes and offsets are
// authoritative, including entries whose local header uses a data descriptor.
std::uint16_t u16(const unsigned char* p) { return p[0] | (std::uint16_t(p[1]) << 8); }
std::uint32_t u32(const unsigned char* p) { return u16(p) | (std::uint32_t(u16(p + 2)) << 16); }
std::uint64_t u64(const unsigned char* p) { return u32(p) | (std::uint64_t(u32(p + 4)) << 32); }
bool range(std::uint64_t off, std::uint64_t size, std::uint64_t end) {
  return off <= end && size <= end - off;
}
bool read_at(std::ifstream& file, std::uint64_t offset, void* out, std::uint64_t count) {
  if (offset > INT64_MAX || count > INT64_MAX) return false;
  file.clear();
  file.seekg(static_cast<std::streamoff>(offset));
  if (count) file.read(static_cast<char*>(out), static_cast<std::streamsize>(count));
  return bool(file);
}
struct Record {
  AssetEntry entry;
  std::string path;
  std::uint64_t packed = 0;
  std::uint64_t offset = 0;
  std::uint16_t method = 0;
  std::uint32_t crc = 0;
};
struct Index {
  std::unordered_map<std::string, std::shared_ptr<const Record>> entries;
  std::map<std::string, std::set<std::string>, std::less<>> directories;
  std::vector<std::shared_ptr<const Record>> extraction;
  std::string extraction_manifest;
};
std::shared_mutex index_mutex;
std::shared_ptr<const Index> current_index = std::make_shared<Index>();

void add_directory_links(Index& index, const std::string& name) {
  std::size_t start = 0;
  while (start < name.size()) {
    auto slash = name.find('/', start);
    auto end = slash == std::string::npos ? name.size() : slash;
    std::string dir = start ? name.substr(0, start - 1) : "";
    index.directories[dir].insert(name.substr(start, end - start));
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
}
void insert(Index& index, const std::string& name, std::shared_ptr<const Record> record) {
  if (!index.entries.emplace(name, record).second) return;
  add_directory_links(index, name);
  constexpr std::string_view prefix = "assets/bin/Data/Managed/";
  if (name.starts_with(prefix)) {
    auto relative = std::string_view(name).substr(prefix.size());
    if (relative.starts_with("Metadata/") || relative.starts_with("Resources/"))
      index.extraction.push_back(std::move(record));
  }
}
void finish_inventory(Index& index) {
  std::sort(index.extraction.begin(), index.extraction.end(), [](const auto& a, const auto& b) {
    return a->entry.name < b->entry.name;
  });
  index.extraction_manifest = "venture-il2cpp-extraction-v1\n";
  for (const auto& record : index.extraction)
    index.extraction_manifest += record->entry.name + " " + std::to_string(record->entry.size) + " " +
                                 std::to_string(record->crc) + "\n";
}
bool index_zip(Index& result, const std::string& path, bool obb) {
  std::ifstream file(host_path(path), std::ios::binary | std::ios::ate);
  if (!file || file.tellg() < 22) return false;
  auto file_size = static_cast<std::uint64_t>(file.tellg());
  std::vector<unsigned char> tail(static_cast<std::size_t>(std::min<std::uint64_t>(file_size, 65557)));
  if (!read_at(file, file_size - tail.size(), tail.data(), tail.size())) return false;
  std::size_t end = tail.size() - 22;
  while (u32(tail.data() + end) != 0x06054b50 || end + 22 + u16(tail.data() + end + 20) != tail.size()) {
    if (!end) return false;
    --end;
  }
  const auto* eocd = tail.data() + end;
  if (u16(eocd + 4) || u16(eocd + 6)) return false;  // split archives unsupported
  std::uint64_t count = u16(eocd + 10), bytes = u32(eocd + 12), offset = u32(eocd + 16);
  std::uint64_t end_offset = file_size - tail.size() + end;
  if (count == 0xffff || bytes == 0xffffffff || offset == 0xffffffff) {
    std::array<unsigned char, 20> locator{};
    std::array<unsigned char, 56> zip64{};
    if (end_offset < 20 || !read_at(file, end_offset - 20, locator.data(), locator.size()) ||
        u32(locator.data()) != 0x07064b50 || u32(locator.data() + 4) || u32(locator.data() + 16) != 1)
      return false;
    auto zip64_offset = u64(locator.data() + 8);
    if (!range(zip64_offset, 56, end_offset - 20) ||
        !read_at(file, zip64_offset, zip64.data(), zip64.size()) || u32(zip64.data()) != 0x06064b50 ||
        u64(zip64.data() + 4) < 44 || u32(zip64.data() + 16) || u32(zip64.data() + 20) ||
        u64(zip64.data() + 24) != u64(zip64.data() + 32))
      return false;
    count = u64(zip64.data() + 32);
    bytes = u64(zip64.data() + 40);
    offset = u64(zip64.data() + 48);
    end_offset = zip64_offset;
  } else if (u16(eocd + 8) != count)
    return false;
  if (!range(offset, bytes, end_offset) || count > bytes / 46 || bytes > SIZE_MAX) return false;
  std::vector<unsigned char> central(static_cast<std::size_t>(bytes));
  if (!read_at(file, offset, central.data(), bytes)) return false;
  auto basename = host_path(path).filename().u8string();
  std::string prefix(reinterpret_cast<const char*>(basename.data()), basename.size());
  prefix += '/';
  std::size_t pos = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    if (!range(pos, 46, bytes)) return false;
    auto* p = central.data() + pos;
    if (u32(p) != 0x02014b50) return false;
    auto name_len = u16(p + 28), extra_len = u16(p + 30), comment_len = u16(p + 32);
    auto size = std::uint64_t(46) + name_len + extra_len + comment_len;
    if (!range(pos, size, bytes)) return false;
    auto record = std::make_shared<Record>();
    record->entry.name.assign(reinterpret_cast<char*>(p + 46), name_len);
    if (record->entry.name.find('\0') != std::string::npos) return false;
    record->method = u16(p + 10);
    record->crc = u32(p + 16);
    record->packed = u32(p + 20);
    record->entry.size = u32(p + 24);
    record->entry.compressed = record->method != 0;
    record->path = path;
    std::uint64_t local = u32(p + 42), disk = u16(p + 34);
    bool needs64 = record->packed == 0xffffffff || record->entry.size == 0xffffffff || local == 0xffffffff ||
                   disk == 0xffff;
    bool has64 = false;
    std::size_t ex = 46 + name_len, ex_end = ex + extra_len;
    while (ex < ex_end) {
      if (ex_end - ex < 4) return false;
      auto tag = u16(p + ex), len = u16(p + ex + 2);
      ex += 4;
      if (len > ex_end - ex) return false;
      if (tag == 1) {
        auto cursor = ex;
        auto take = [&](std::uint64_t& field, std::uint64_t sentinel, std::size_t width) {
          if (field != sentinel) return true;
          if (ex + len - cursor < width) return false;
          field = width == 8 ? u64(p + cursor) : u32(p + cursor);
          cursor += width;
          return true;
        };
        if (!take(record->entry.size, 0xffffffff, 8) || !take(record->packed, 0xffffffff, 8) ||
            !take(local, 0xffffffff, 8) || !take(disk, 0xffff, 4))
          return false;
        has64 = true;
      }
      ex += len;
    }
    if ((needs64 && !has64) || disk || record->entry.size > INT64_MAX) return false;
    std::array<unsigned char, 30> header{};
    if (!range(local, 30, offset) || !read_at(file, local, header.data(), header.size()) ||
        u32(header.data()) != 0x04034b50 || u16(header.data() + 8) != record->method)
      return false;
    record->offset = local + 30 + u16(header.data() + 26) + u16(header.data() + 28);
    if (!range(record->offset, record->packed, offset)) return false;
    record->entry.data_offset = record->entry.compressed ? 0 : record->offset;
    if (u16(p + 8) & (1 | 64)) record->method = 0xffff;  // encrypted: fail on open
    if (record->method == 0 && record->packed != record->entry.size) return false;
    if (!record->entry.name.empty() && record->entry.name.back() != '/') {
      insert(result, record->entry.name, record);
      if (obb) insert(result, prefix + record->entry.name, record);
    } else if (!record->entry.name.empty()) {
      auto directory = record->entry.name.substr(0, record->entry.name.size() - 1);
      add_directory_links(result, directory);
      if (obb) add_directory_links(result, prefix + directory);
    }
    pos += static_cast<std::size_t>(size);
  }
  return true;
}

class ZipStream final : public AssetStream {
 public:
  explicit ZipStream(std::shared_ptr<const Record> record)
      : record_(std::move(record)), file_(host_path(record_->path), std::ios::binary) {}
  ~ZipStream() override {
    if (inflating_) inflateEnd(&z_);
  }
  bool valid() const { return bool(file_); }
  std::uint64_t length() const override { return record_->entry.size; }
  bool file_range(std::string* path, std::uint64_t* offset) const override {
    if (record_->method || !path || !offset) return false;
    *path = record_->path;
    *offset = record_->offset;
    return true;
  }
  std::int64_t read(void* out, std::uint64_t size) override {
    std::lock_guard lock(mutex_);
    if (!out && size) return -err::fault;
    return read_locked(out, std::min(size, length() - position_));
  }
  std::int64_t seek(std::int64_t offset, int whence) override {
    std::lock_guard lock(mutex_);
    std::int64_t base = whence == 0   ? 0
                        : whence == 1 ? static_cast<std::int64_t>(position_)
                        : whence == 2 ? static_cast<std::int64_t>(length())
                                      : -1;
    if (base < 0 || offset < -base || offset > static_cast<std::int64_t>(length()) - base)
      return -err::invalid;
    auto target = static_cast<std::uint64_t>(base + offset);
    if (!record_->method) {
      position_ = target;
      return static_cast<std::int64_t>(target);
    }
    if (failed_) return -err::io;
    if (target < position_) reset();
    std::array<unsigned char, 65536> discard{};
    while (position_ < target) {
      auto n = read_locked(discard.data(), std::min<std::uint64_t>(target - position_, discard.size()));
      if (n <= 0) return -err::io;
    }
    return static_cast<std::int64_t>(target);
  }

 private:
  void reset() {
    if (inflating_) inflateEnd(&z_);
    z_ = {};
    inflating_ = false;
    position_ = packed_position_ = 0;
    crc_ = 0;
  }
  std::int64_t read_locked(void* out, std::uint64_t size) {
    if (failed_) return -err::io;
    if (!size) return 0;
    if (!record_->method) {
      if (!read_at(file_, record_->offset + position_, out, size)) return -err::io;
      position_ += size;
      return static_cast<std::int64_t>(size);
    }
    if (!inflating_) {
      if (inflateInit2(&z_, -MAX_WBITS) != Z_OK) return -err::no_memory;
      inflating_ = true;
    }
    std::uint64_t produced = 0;
    while (produced < size) {
      if (!z_.avail_in && packed_position_ < record_->packed) {
        auto n = std::min<std::uint64_t>(input_.size(), record_->packed - packed_position_);
        if (!read_at(file_, record_->offset + packed_position_, input_.data(), n)) break;
        packed_position_ += n;
        z_.next_in = input_.data();
        z_.avail_in = static_cast<uInt>(n);
      }
      auto chunk = static_cast<uInt>(std::min<std::uint64_t>(size - produced, 1u << 30));
      z_.next_out = static_cast<Bytef*>(out) + produced;
      z_.avail_out = chunk;
      auto before = z_.avail_in;
      int status = inflate(&z_, Z_NO_FLUSH);
      auto n = chunk - z_.avail_out;
      crc_ = static_cast<std::uint32_t>(crc32(crc_, static_cast<Bytef*>(out) + produced, n));
      produced += n;
      position_ += n;
      if (status == Z_STREAM_END) {
        if (position_ != length() || crc_ != record_->crc ||
            packed_position_ - z_.avail_in != record_->packed)
          break;
        return static_cast<std::int64_t>(produced);
      }
      if (status != Z_OK || (!n && before == z_.avail_in)) break;
      if (produced == size && position_ != length()) return static_cast<std::int64_t>(produced);
      if (position_ == length()) {
        // Consume the end marker even when the caller's buffer ended exactly
        // at the uncompressed size. No extra decompressed byte is permitted.
        unsigned char extra;
        z_.next_out = &extra;
        z_.avail_out = 1;
        auto status_end = inflate(&z_, Z_FINISH);
        if (status_end == Z_STREAM_END && z_.avail_out == 1 && crc_ == record_->crc &&
            packed_position_ - z_.avail_in == record_->packed)
          return static_cast<std::int64_t>(produced);
        break;
      }
    }
    failed_ = true;
    log::error("assets", "Invalid or truncated deflated entry: {}", record_->entry.name);
    return -err::io;
  }
  std::shared_ptr<const Record> record_;
  std::ifstream file_;
  std::mutex mutex_;
  std::uint64_t position_ = 0, packed_position_ = 0;
  z_stream z_{};
  std::array<unsigned char, 65536> input_{};
  std::uint32_t crc_ = 0;
  bool inflating_ = false, failed_ = false;
};
}  // namespace

namespace {
std::filesystem::path io_path(const std::filesystem::path& path) {
#ifdef _WIN32
  return std::filesystem::path(platform::native_path(path));
#else
  return path;
#endif
}
}
bool extract_asset_atomic(AssetStream& stream, const std::filesystem::path& destination) {
  try {
    auto temporary = destination;
    temporary += ".venture-part";
    std::filesystem::create_directories(io_path(destination.parent_path()));
    {
      std::ofstream out(io_path(temporary), std::ios::binary | std::ios::trunc);
      if (!out || stream.seek(0, 0) != 0) return false;
      std::array<char, 65536> bytes;
      std::uint64_t written = 0;
      while (written < stream.length()) {
        const auto count = std::min<std::uint64_t>(bytes.size(), stream.length() - written);
        auto n = stream.read(bytes.data(), count);
        if (n <= 0 || static_cast<std::uint64_t>(n) > count) return false;
        out.write(bytes.data(), n);
        if (!out) return false;
        written += n;
      }
      out.flush();
      if (!out) return false;
      out.close();
      if (!out) return false;
    }
    flush_file(temporary);
    paths::replace_file(temporary, destination);
    flush_directory(destination.parent_path());
    return true;
  } catch (const std::exception& e) {
    log::error("assets", "Atomic extraction failed: {}", e.what());
    return false;
  }
}

bool PackageAssets::prepare_unity_extraction(const std::filesystem::path& external_files) const {
  constexpr std::string_view prefix = "assets/bin/Data/Managed/";
  struct Copy { std::string asset; std::filesystem::path destination; std::uint64_t size; bool invalid; };
  std::vector<Copy> copies;
  std::string manifest;
  {
    std::shared_lock lock(index_mutex);
    // Only the engine's packaged extraction directories. No package identity
    // or title-specific paths participate in this check.
    manifest = current_index->extraction_manifest;
    for (const auto& record : current_index->extraction) {
      const auto& name = record->entry.name;
      auto relative = std::string_view(name).substr(prefix.size());
      if (!relative.starts_with("Metadata/") && !relative.starts_with("Resources/")) continue;
      auto child = host_path(relative);
      if (child.is_absolute()) return false;
      for (const auto& component : child)
        if (component == "..") return false;
      copies.push_back({name, external_files / "il2cpp" / child, record->entry.size, false});
    }
  }
  if (copies.empty()) return true;
  try {
    const auto root = external_files / "il2cpp";
    const auto marker = root / ".venture-complete";
    bool complete = true;
    for (auto& copy : copies) {
      std::error_code ec;
      auto size = std::filesystem::file_size(io_path(copy.destination), ec);
      copy.invalid = ec || size != copy.size;
      if (copy.invalid) complete = false;
    }
    bool marked = false;
    try { marked = paths::read(marker, manifest.size()) == manifest; } catch (const std::exception&) {}
    if (complete && marked) return true;
    const bool changed = !marked && std::filesystem::exists(io_path(marker));
    // A legacy cache with matching sizes is already complete. Adopt it without
    // rewriting or flushing data files. An existing, mismatched manifest means
    // the packaged bytes changed, even if their lengths did not.
    if (!complete || changed)
      std::filesystem::remove(io_path(root / "unity.ver"));
    for (const auto& copy : copies) {
      if (!changed && !copy.invalid) continue;
      auto stream = open(copy.asset);
      if (!stream || !extract_asset_atomic(*stream, copy.destination)) return false;
    }
    paths::atomic_write(marker, manifest);
    if (!complete || changed) {
      flush_file(marker);
      flush_directory(root);
    }
    return true;
  } catch (const std::exception& e) {
    log::error("assets", "Cannot prepare IL2CPP extraction: {}", e.what());
    return false;
  }
}

PackageAssets& PackageAssets::get() {
  static PackageAssets assets;
  return assets;
}
bool PackageAssets::open_apk(const std::string& path) {
  try {
    auto next = std::make_shared<Index>();
    if (!index_zip(*next, path, false)) {
      log::error("assets", "APK ZIP index rejected: {}", path);
      return false;
    }
    std::vector<std::filesystem::path> obbs;
    std::error_code ec;
    for (auto& item : std::filesystem::directory_iterator(
             host_path(path).has_parent_path() ? host_path(path).parent_path() : std::filesystem::path("."),
             ec)) {
      auto name = item.path().filename().string();
      if (item.path().extension() == ".obb" && (name.starts_with("main.") || name.starts_with("patch.")))
        obbs.push_back(item.path());
    }
    std::sort(obbs.begin(), obbs.end());
    for (auto& obb : obbs) {
      auto utf8 = obb.u8string();
      auto with_obb = std::make_shared<Index>(*next);
      if (!index_zip(*with_obb, std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()), true)) {
        log::warn("assets", "OBB is not a supported ZIP: {}", obb.filename().string());
        // UE4 expansion files may use other formats; the VFS still exposes the
        // original file. Never report those files as mounted ZIP assets.
      } else {
        next = std::move(with_obb);
      }
    }
    finish_inventory(*next);
    std::unique_lock lock(index_mutex);
    current_index = std::move(next);
    return true;
  } catch (const std::exception& e) {
    log::error("assets", "ZIP index failed: {}", e.what());
    return false;
  }
}
bool PackageAssets::open_obb(const std::string& path) {
  try {
    std::unique_lock lock(index_mutex);
    auto next = std::make_shared<Index>(*current_index);
    if (!index_zip(*next, path, true)) {
      log::error("assets", "OBB ZIP index rejected: {}", path);
      return false;
    }
    finish_inventory(*next);
    current_index = std::move(next);
    return true;
  } catch (const std::exception& e) {
    log::error("assets", "OBB index failed: {}", e.what());
    return false;
  }
}
std::optional<AssetEntry> PackageAssets::find(std::string_view name) const {
  std::shared_lock lock(index_mutex);
  auto it = current_index->entries.find(std::string(name));
  if (it == current_index->entries.end()) return {};
  return it->second->entry;
}
std::vector<std::string> PackageAssets::list(std::string_view dir) const {
  while (dir.ends_with('/')) dir.remove_suffix(1);
  std::shared_lock lock(index_mutex);
  auto it = current_index->directories.find(dir);
  if (it == current_index->directories.end()) return {};
  return {it->second.begin(), it->second.end()};
}
std::unique_ptr<AssetStream> PackageAssets::open(std::string_view name) const {
  std::shared_ptr<const Record> record;
  {
    std::shared_lock lock(index_mutex);
    auto it = current_index->entries.find(std::string(name));
    if (it == current_index->entries.end()) return {};
    record = it->second;
  }
  if (record->method != 0 && record->method != 8) {
    log::error("assets", "Unsupported ZIP compression/encryption for {}", name);
    return {};
  }
  auto stream = std::make_unique<ZipStream>(std::move(record));
  if (!stream->valid()) return {};
  return stream;
}
}  // namespace venture::android
