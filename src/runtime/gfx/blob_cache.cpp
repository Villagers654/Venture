#include "gfx/blob_cache.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <thread>

#include <vulkan/vulkan.h>

#include "common/log.h"
#include "common/paths.h"
#include "gfx/angle.h"
#include "gfx/gpu_device_internal.h"

namespace venture::gfx {
namespace {
// File: magic, format, identity, entry count, payload size, payload checksum,
// then per entry u32 key size, u32 value size, key, value (little endian).
// Persisted shader caches retain their released format magic across the rebrand.
constexpr char kLegacyBlobMagic[4]{'Q', 'P', 'B', 'C'};
constexpr std::uint32_t kFormat = 1;
constexpr std::uint64_t kChecksumSeed = 0xcbf29ce484222325ull;
std::uint64_t checksum(std::uint64_t hash, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (; size >= 8; bytes += 8, size -= 8) {
    std::uint64_t word;
    std::memcpy(&word, bytes, 8);
    hash = (hash ^ word) * 0x100000001b3ull;
    hash ^= hash >> 29;
  }
  for (; size; ++bytes, --size) hash = (hash ^ *bytes) * 0x100000001b3ull;
  return hash;
}
template <class T>
bool take(std::string_view& in, T& value) {
  if (in.size() < sizeof(T)) return false;
  std::memcpy(&value, in.data(), sizeof(T));
  in.remove_prefix(sizeof(T));
  return true;
}
}  // namespace

BlobStore::BlobStore(Limits limits)
    : limits_(limits),
      random_(static_cast<std::uint64_t>(
                  std::chrono::steady_clock::now().time_since_epoch().count()) |
              1) {}

void BlobStore::set_locked(std::string key,
                           std::shared_ptr<const std::string> value,
                           bool replace) {
  if (key.empty() || value->empty() || key.size() > limits_.key ||
      value->size() > limits_.value ||
      key.size() + value->size() > limits_.total)
    return;
  for (;;) {
    auto it = std::lower_bound(
        entries_.begin(), entries_.end(), key,
        [](const Entry& e, const std::string& k) { return e.key < k; });
    const bool found = it != entries_.end() && it->key == key;
    if (found && !replace) return;
    const auto next = total_ + value->size() -
                      (found ? it->value->size() : 0) +
                      (found ? 0 : key.size());
    if (next > limits_.total) {
      if (total_ <= limits_.total / 2) return;
      // BlobCache::clean: random entries until at most half is used. Mark
      // victims first so the vector is compacted once.
      std::vector<char> victim(entries_.size());
      while (total_ > limits_.total / 2) {
        random_ ^= random_ << 13;
        random_ ^= random_ >> 7;
        random_ ^= random_ << 17;
        const auto i = static_cast<std::size_t>(random_ % entries_.size());
        if (victim[i]) continue;
        victim[i] = 1;
        total_ -= entries_[i].key.size() + entries_[i].value->size();
      }
      std::size_t index = 0;
      std::erase_if(entries_, [&](const Entry&) { return victim[index++]; });
      continue;
    }
    if (found)
      it->value = std::move(value);
    else
      entries_.insert(it, Entry{std::move(key), std::move(value)});
    total_ = next;
    return;
  }
}

void BlobStore::set(const void* key, std::size_t key_size, const void* value,
                    std::size_t value_size) {
  if (!key || !value) return;
  auto blob = std::make_shared<const std::string>(
      static_cast<const char*>(value), value_size);
  std::lock_guard lock(lock_);
  set_locked(std::string(static_cast<const char*>(key), key_size),
             std::move(blob), true);
}

std::size_t BlobStore::get(const void* key, std::size_t key_size, void* value,
                           std::size_t value_size) const {
  if (!key || key_size > limits_.key) return 0;
  const std::string_view wanted(static_cast<const char*>(key), key_size);
  std::lock_guard lock(lock_);
  auto it = std::lower_bound(
      entries_.begin(), entries_.end(), wanted,
      [](const Entry& e, std::string_view k) { return e.key < k; });
  if (it == entries_.end() || it->key != wanted) return 0;
  const auto size = it->value->size();
  if (value && size <= value_size) std::memcpy(value, it->value->data(), size);
  return size;
}

std::size_t BlobStore::entries() const {
  std::lock_guard lock(lock_);
  return entries_.size();
}

std::size_t BlobStore::total_bytes() const {
  std::lock_guard lock(lock_);
  return total_;
}

bool BlobStore::load(const std::filesystem::path& file,
                     std::string_view identity) {
  std::string bytes;
  try {
    bytes = paths::read(file, 2 * limits_.total + identity.size() + 4096);
  } catch (const std::exception&) {
    return false;
  }
  std::string_view in(bytes);
  char magic[4];
  std::uint32_t format{}, identity_size{}, count{};
  std::uint64_t payload_size{}, expected{};
  if (!take(in, magic) || std::memcmp(magic, kLegacyBlobMagic, 4) || !take(in, format) ||
      format != kFormat || !take(in, identity_size) ||
      identity_size != identity.size() || in.size() < identity_size ||
      in.substr(0, identity_size) != identity)
    return false;
  in.remove_prefix(identity_size);
  if (!take(in, count) || !take(in, payload_size) || !take(in, expected) ||
      in.size() != payload_size)
    return false;
  std::vector<std::pair<std::string_view, std::string_view>> parsed;
  std::uint64_t sum = kChecksumSeed;
  for (std::uint32_t n = 0; n < count; ++n) {
    std::uint32_t sizes[2];
    if (!take(in, sizes) || !sizes[0] || !sizes[1] ||
        sizes[0] > limits_.key || sizes[1] > limits_.value ||
        in.size() < std::uint64_t(sizes[0]) + sizes[1])
      return false;
    sum = checksum(sum, sizes, sizeof(sizes));
    parsed.emplace_back(in.substr(0, sizes[0]), in.substr(sizes[0], sizes[1]));
    sum = checksum(sum, parsed.back().first.data(), sizes[0]);
    sum = checksum(sum, parsed.back().second.data(), sizes[1]);
    in.remove_prefix(std::size_t(sizes[0]) + sizes[1]);
  }
  if (!in.empty() || sum != expected) return false;
  std::lock_guard lock(lock_);
  for (auto [key, value] : parsed)
    set_locked(std::string(key), std::make_shared<const std::string>(value),
               false);
  return true;
}

bool BlobStore::save(const std::filesystem::path& file,
                     std::string_view identity) const {
  std::vector<Entry> snapshot;
  {
    std::lock_guard lock(lock_);
    snapshot = entries_;
  }
  std::uint64_t sum = kChecksumSeed, payload = 0;
  for (const auto& e : snapshot) {
    const std::uint32_t sizes[2]{static_cast<std::uint32_t>(e.key.size()),
                                 static_cast<std::uint32_t>(e.value->size())};
    sum = checksum(sum, sizes, sizeof(sizes));
    sum = checksum(sum, e.key.data(), e.key.size());
    sum = checksum(sum, e.value->data(), e.value->size());
    payload += sizeof(sizes) + e.key.size() + e.value->size();
  }
  auto temporary = file;
  temporary += ".tmp";
  try {
    std::filesystem::create_directories(file.parent_path());
    {
      std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
      auto put = [&](const void* data, std::size_t size) {
        out.write(static_cast<const char*>(data),
                  static_cast<std::streamsize>(size));
      };
      const auto identity_size = static_cast<std::uint32_t>(identity.size());
      const auto count = static_cast<std::uint32_t>(snapshot.size());
      put(kLegacyBlobMagic, 4);
      put(&kFormat, 4);
      put(&identity_size, 4);
      put(identity.data(), identity.size());
      put(&count, 4);
      put(&payload, 8);
      put(&sum, 8);
      for (const auto& e : snapshot) {
        const std::uint32_t sizes[2]{
            static_cast<std::uint32_t>(e.key.size()),
            static_cast<std::uint32_t>(e.value->size())};
        put(sizes, sizeof(sizes));
        put(e.key.data(), e.key.size());
        put(e.value->data(), e.value->size());
      }
      out.flush();
      if (!out) throw std::runtime_error("short write");
    }
    // The rename is atomic: a crash leaves the previous file or the new one.
    paths::replace_file(temporary, file);
    return true;
  } catch (const std::exception&) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
  }
}

namespace {
// AOSP egl_cache.cpp limits: 12 KiB keys and 64 KiB values (ANGLE splits its
// pipeline cache into chunks of that size). The total is raised from AOSP's
// 2 MiB: Vader Immortal's full set is 1,586 blobs and 8.7 MiB (largest blob
// 16.4 KiB), so 64 MiB holds about seven times that before evicting.
constexpr BlobStore::Limits kLimits{12 * 1024, 64 * 1024, 64u << 20};
// AOSP deferredSaveDelay is 4 s; also bound how long a busy stream of new
// programs can postpone the write.
constexpr auto kQuiet = std::chrono::seconds(4);
constexpr auto kMaxDelay = std::chrono::seconds(30);
using Clock = std::chrono::steady_clock;
struct Persistent {
  BlobStore store{kLimits};
  std::mutex lock;  // everything below
  std::condition_variable wake;
  std::filesystem::path file;
  std::string identity;
  bool loaded = false, dirty = false;
  Clock::time_point first_dirty, last_set;
  std::mutex write_lock;  // serializes file writes
};
// Never destroyed: ANGLE worker threads may still call back during exit.
Persistent& persistent() {
  static auto* state = new Persistent;
  return *state;
}
void load_if_ready(Persistent& p) {
  if (p.loaded || p.file.empty() || p.identity.empty()) return;
  p.loaded = true;
  const auto before = p.store.entries();
  std::error_code ec;
  const bool existed = std::filesystem::exists(p.file, ec);
  if (p.store.load(p.file, p.identity))
    log::info("gfx", "EGL blob cache: loaded {} entries ({:.1f} MiB) from {}",
              p.store.entries() - before, p.store.total_bytes() / 1048576.0,
              paths::utf8(p.file.filename()));
  else
    log::info("gfx", "EGL blob cache: {} {}; starting empty",
              existed ? "discarded stale or damaged" : "no",
              paths::utf8(p.file.filename()));
  if (before) {
    p.dirty = true;
    p.first_dirty = p.last_set = Clock::now();
  }
}
void write_now() {
  auto& p = persistent();
  std::lock_guard write(p.write_lock);
  std::filesystem::path file;
  std::string identity;
  {
    std::lock_guard lock(p.lock);
    if (!p.dirty || !p.loaded) return;
    p.dirty = false;
    file = p.file;
    identity = p.identity;
  }
  if (p.store.save(file, identity))
    log::info("gfx", "EGL blob cache: saved {} entries ({:.1f} MiB)",
              p.store.entries(), p.store.total_bytes() / 1048576.0);
  else
    log::warn("gfx", "EGL blob cache: could not write {}",
              paths::utf8(file.filename()));
}
void writer() {
  auto& p = persistent();
  std::unique_lock lock(p.lock);
  for (;;) {
    p.wake.wait(lock, [&] { return p.dirty && p.loaded; });
    const auto due = std::min(p.last_set + kQuiet, p.first_dirty + kMaxDelay);
    if (Clock::now() < due) {
      p.wake.wait_until(lock, due);
      continue;
    }
    lock.unlock();
    write_now();
    lock.lock();
  }
}
void set_blob(const void* key, EGLsizeiANDROID key_size, const void* value,
              EGLsizeiANDROID value_size) {
  if (key_size < 0 || value_size < 0) return;
  auto& p = persistent();
  p.store.set(key, static_cast<std::size_t>(key_size), value,
              static_cast<std::size_t>(value_size));
  std::lock_guard lock(p.lock);
  const auto now = Clock::now();
  if (!p.dirty) p.first_dirty = now;
  p.dirty = true;
  p.last_set = now;
  p.wake.notify_one();
}
EGLsizeiANDROID get_blob(const void* key, EGLsizeiANDROID key_size,
                         void* value, EGLsizeiANDROID value_size) {
  if (key_size < 0 || value_size < 0) return 0;
  return static_cast<EGLsizeiANDROID>(persistent().store.get(
      key, static_cast<std::size_t>(key_size), value,
      static_cast<std::size_t>(value_size)));
}
}  // namespace

void set_blob_cache_file(const std::filesystem::path& file) {
  auto& p = persistent();
  std::lock_guard lock(p.lock);
  if (!p.file.empty() || file.empty()) return;
  p.file = file;
  load_if_ready(p);
  p.wake.notify_one();
}

void install_blob_cache(void* handle) {
  const auto display = static_cast<EGLDisplay>(handle);
  auto query = angle_function<PFNEGLQUERYSTRINGPROC>("eglQueryString");
  auto install = angle_function<PFNEGLSETBLOBCACHEFUNCSANDROIDPROC>(
      "eglSetBlobCacheFuncsANDROID");
  const char* extensions = query ? query(display, EGL_EXTENSIONS) : nullptr;
  if (!install || !extensions ||
      !has_extension(extensions, "EGL_ANDROID_blob_cache")) {
    log::warn("gfx", "ANGLE lacks EGL_ANDROID_blob_cache; shaders recompile "
                     "on every launch");
    return;
  }
  // ANGLE's EGL_VERSION names its revision and serialized program version.
  const char* version = query(display, EGL_VERSION);
  VkPhysicalDeviceProperties properties{};
  const auto& gpu = gpu_device_state();
  if (gpu.physical_device && gpu.get_instance_proc_addr)
    if (auto get = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            gpu.get_instance_proc_addr(gpu.instance,
                                       "vkGetPhysicalDeviceProperties")))
      get(gpu.physical_device, &properties);
  auto identity = std::format("{}|vendor={:#x}|device={:#x}|driver={:#x}|uuid=",
                              version ? version : "", properties.vendorID,
                              properties.deviceID, properties.driverVersion);
  for (auto byte : properties.pipelineCacheUUID)
    identity += std::format("{:02x}", byte);
  auto& p = persistent();
  {
    std::lock_guard lock(p.lock);
    if (!p.identity.empty()) return;
    p.identity = std::move(identity);
    load_if_ready(p);
  }
  install(display, set_blob, get_blob);
  if (auto error = angle_function<PFNEGLGETERRORPROC>("eglGetError")();
      error != EGL_SUCCESS) {
    log::warn("gfx", "eglSetBlobCacheFuncsANDROID failed: EGL 0x{:x}", error);
    return;
  }
  std::thread(writer).detach();
  std::atexit(write_now);
}
void flush_blob_cache() noexcept {
  try { write_now(); }
  catch (const std::exception& e) { log::warn("gfx", "EGL blob cache flush: {}", e.what()); }
  catch (...) {}
}
}  // namespace venture::gfx
