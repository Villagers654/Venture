#include "android/errors.h"
#include "hle/libandroid/parts.h"
#include "android/handles.h"
#include "android/framework_bridge.h"
#include "android/package_assets.h"
#include "android/libc_bridge.h"
#include "core/address_space.h"
#include <algorithm>
#include <climits>

namespace venture::android {
GuestAddr package_asset_manager() {
  static GuestAddr manager = AddressSpace::get().allocate(8, kProtRead | kProtWrite, "[asset-manager]");
  return manager;
}
GuestAddr asset_manager_from_java(_jobject* object) { return object ? package_asset_manager() : 0; }
}  // namespace venture::android
namespace venture::hle::android_hle {
namespace {
using namespace venture::android;
struct Asset {
  explicit Asset(std::unique_ptr<AssetStream> s) : stream(std::move(s)) {}
  ~Asset() {
    if (buffer) AddressSpace::get().free(buffer);
  }
  std::mutex mutex;
  std::unique_ptr<AssetStream> stream;
  GuestAddr buffer = 0;
};
struct Directory {
  std::vector<std::string> names;
  std::size_t next = 0;
  std::mutex mutex;
};
Handles<Asset> assets{"AAsset"};
Handles<Directory> directories{"AAssetDir"};
GuestAddr from_java(void*, _jobject* object) { return asset_manager_from_java(object); }
std::string asset_path(const char* name) {
  if (!name || *name == '/') return {};
  std::string_view path(name);
  if (path == ".." || path.starts_with("../") || path.ends_with("/..") || path.find("/../") != path.npos)
    return {};
  return "assets/" + std::string(path);
}
Asset* open(GuestAddr manager, const char* name, int mode) {
  if (!manager || manager != package_asset_manager() || mode < 0 || mode > 3) return nullptr;
  auto stream = PackageAssets::get().open(asset_path(name));
  if (!stream) return nullptr;
  try {
    return assets.create(std::move(stream));
  } catch (const std::bad_alloc&) {
    fail(err::no_memory);
    return nullptr;
  }
}
Directory* open_dir(GuestAddr manager, const char* name) {
  if (!manager || manager != package_asset_manager() || !name) return nullptr;
  // frameworks/base/native/android/asset_manager.cpp + AssetManager2::OpenDir:
  // a name that matches no "assets/<name>/" entry (absolute paths, "..", a
  // missing directory) still yields an empty, non-NULL AAssetDir. Wwise's
  // Android I/O treats non-NULL as "the base path exists".
  auto prefix = asset_path(name);
  while (prefix.ends_with('/')) prefix.pop_back();
  auto* d = directories.create();
  if (prefix.empty()) return d;
  for (const auto& child : PackageAssets::get().list(prefix)) {
    // AAssetDir enumerates files only (asset_manager.h), not subdirectories.
    if (PackageAssets::get().find(prefix + '/' + child)) d->names.push_back(child);
  }
  return d;
}
void close_dir(Directory* d) { directories.erase(d); }
const char* next_name(Directory* ptr) {
  auto d = directories.get(ptr);
  if (!d) return nullptr;
  std::lock_guard lock(d->mutex);
  return d->next < d->names.size() ? d->names[d->next++].c_str() : nullptr;
}
void rewind_dir(Directory* ptr) {
  if (auto d = directories.get(ptr)) {
    std::lock_guard lock(d->mutex);
    d->next = 0;
  }
}
void close(Asset* a) { assets.erase(a); }
int read(Asset* ptr, void* out, std::uint64_t count) {
  auto a = assets.get(ptr);
  if (!a) return fail(err::invalid);
  std::lock_guard lock(a->mutex);
  auto result = a->stream->read(out, std::min<std::uint64_t>(count, INT_MAX));
  return result < 0 ? fail(static_cast<int>(-result)) : static_cast<int>(result);
}
std::int64_t seek(Asset* ptr, std::int64_t off, int whence) {
  auto a = assets.get(ptr);
  if (!a) return fail(err::invalid);
  std::lock_guard lock(a->mutex);
  auto n = a->stream->seek(off, whence);
  return n < 0 ? fail(static_cast<int>(-n)) : n;
}
std::int64_t length(Asset* ptr) {
  auto a = assets.get(ptr);
  return a ? static_cast<std::int64_t>(a->stream->length()) : 0;
}
std::int64_t remaining(Asset* ptr) {
  auto a = assets.get(ptr);
  if (!a) return 0;
  std::lock_guard lock(a->mutex);
  auto pos = a->stream->seek(0, 1);
  return pos < 0 ? 0 : static_cast<std::int64_t>(a->stream->length()) - pos;
}
const void* buffer(Asset* ptr) {
  auto a = assets.get(ptr);
  if (!a) return nullptr;
  std::lock_guard lock(a->mutex);
  if (a->buffer) return g2h(a->buffer);
  auto size = a->stream->length();
  auto memory = AddressSpace::get().allocate(std::max<std::uint64_t>(1, size), kProtRead | kProtWrite,
                                             "[asset-buffer]");
  if (!memory) {
    fail(err::no_memory);
    return nullptr;
  }
  auto pos = a->stream->seek(0, 1);
  bool ok = pos >= 0 && a->stream->seek(0, 0) == 0;
  if (ok) ok = a->stream->read(g2h(memory), size) == static_cast<std::int64_t>(size);
  if (pos >= 0) ok = a->stream->seek(pos, 0) == pos && ok;
  if (!ok) {
    AddressSpace::get().free(memory);
    fail(err::io);
    return nullptr;
  }
  a->buffer = memory;
  return g2h(memory);
}
int allocated(Asset* ptr) {
  auto a = assets.get(ptr);
  if (!a) return 0;
  std::lock_guard lock(a->mutex);
  return a->buffer ? 1 : 0;
}
int fd(Asset* ptr, std::int64_t* start, std::int64_t* len) {
  auto a = assets.get(ptr);
  if (!a || !start || !len) return fail(err::invalid);
  std::string path;
  std::uint64_t offset;
  if (!a->stream->file_range(&path, &offset)) return -1;  // compressed assets have no fd
  if (offset > INT64_MAX) return fail(err::overflow);
  int result = libc_open_host_file_readonly(path, offset, a->stream->length());
  if (result < 0) return fail(-result);
  *start = static_cast<std::int64_t>(offset);
  *len = static_cast<std::int64_t>(a->stream->length());
  return result;
}
int fd32(Asset* ptr, std::int64_t* start, std::int64_t* len) {
  auto a = assets.get(ptr);
  if (!a || !start || !len) return fail(err::invalid);
  std::string path;
  std::uint64_t offset;
  if (!a->stream->file_range(&path, &offset)) return -1;
  // bionic ARM off_t is signed 32-bit. Reject before opening an fd; the
  // adapter's copy-back check alone would leak it on EOVERFLOW.
  if (offset > INT32_MAX || a->stream->length() > INT32_MAX) return fail(err::overflow);
  return fd(ptr, start, len);
}
}  // namespace
void register_assets(HleLibrary& lib) {
#define API(name, fn) add<&fn>(lib, name)
  API("AAssetManager_fromJava", from_java);
  API("AAssetManager_open", open);
  API("AAssetManager_openDir", open_dir);
  API("AAssetDir_close", close_dir);
  API("AAssetDir_getNextFileName", next_name);
  API("AAssetDir_rewind", rewind_dir);
  API("AAsset_close", close);
  API("AAsset_read", read);
  API("AAsset_seek", seek);
  API("AAsset_seek64", seek);
  API("AAsset_getLength", length);
  API("AAsset_getLength64", length);
  API("AAsset_getRemainingLength", remaining);
  API("AAsset_getRemainingLength64", remaining);
  API("AAsset_getBuffer", buffer);
  API("AAsset_isAllocated", allocated);
  // off_t is 64 bits on bionic LP64, including the non-suffixed APIs.
  if (process_config().isa() == GuestIsa::a32)
    API("AAsset_openFileDescriptor", fd32);
  else
    API("AAsset_openFileDescriptor", fd);
  API("AAsset_openFileDescriptor64", fd);
#undef API
}
}  // namespace venture::hle::android_hle
