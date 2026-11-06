#include "cpu/portable_cache.h"
#include <dynarmic/backend/x64/portable_block.h>
#include "common/environment.h"
#include "common/file_ops.h"
#include "common/log.h"
#include "common/paths.h"
#include "core/linker.h"
#include "core/process.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <array>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#define LZ4_STATIC_LINKING_ONLY  // LZ4_attach_dictionary
#include <lz4.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace venture {
namespace {
constexpr char kMagic[4] = {'V', 'P', 'C', '5'};
// Records are LZ4-compressed against a dictionary: the first record of every file (this
// descriptor), the first 64 KiB of blocks it stored. Blocks are a few hundred bytes, too
// small to compress well alone (1.3x); with the dictionary they shrink 1.65x.
constexpr std::uint64_t kDictionaryDescriptor = ~0ull;
constexpr std::size_t kDictionarySize = 64 * 1024;
// A mark (mark_explored): every block record before it has had its successors explored by
// background translation, except the descriptors it lists (u64 each). The last one counts.
constexpr std::uint64_t kExploredDescriptor = ~1ull;
// One title's whole-library translation (cpu/pretranslate.cpp) fits; all titles share kTotalLimit.
constexpr std::uint64_t kFileLimit = 4ull * 1024 * 1024 * 1024;
constexpr std::uint64_t kTotalLimit = 24ull * 1024 * 1024 * 1024;
constexpr std::uint32_t kRecordLimit = 4 * 1024 * 1024;

std::uint64_t fnv(std::uint64_t h, const void* data, std::size_t size) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  for (std::size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}
std::uint64_t fnv(std::uint64_t h, std::string_view s) { return fnv(h, s.data(), s.size()); }
// A record's integrity check: eight bytes per step, as installs read it on the guest thread.
std::uint32_t check(const std::uint8_t* p, std::size_t size) {
  // Four independent lanes: every install checks its record, so the multiply chain must not serialize.
  std::uint64_t lane[4] = {0x9e3779b97f4a7c15ull ^ size, 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull, 0x27d4eb2f165667c5ull};
  std::size_t i = 0;
  for (; i + 32 <= size; i += 32) {
    for (int k = 0; k < 4; ++k) {
      std::uint64_t w;
      std::memcpy(&w, p + i + 8 * k, 8);
      lane[k] = (lane[k] ^ w) * 0xff51afd7ed558ccdull;
      lane[k] ^= lane[k] >> 32;
    }
  }
  std::uint64_t h = lane[0] ^ (lane[1] * 0x100000001b3ull) ^ (lane[2] * 0xff51afd7ed558ccdull) ^ (lane[3] * 0xc4ceb9fe1a85ec53ull);
  for (; i + 8 <= size; i += 8) {
    std::uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = (h ^ w) * 0xff51afd7ed558ccdull;
    h ^= h >> 32;
  }
  for (; i < size; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  h ^= h >> 29;
  return std::uint32_t(h);
}

// Everything the machine code depends on besides the guest code: the runtime
// build, the JIT options and the host CPU's features.
std::uint64_t fingerprint() {
  std::uint64_t h = 0xcbf29ce484222325ull;
  // Blocks call runtime functions by their offset in the image, which any rebuild may move.
  std::error_code error;
  const auto exe = paths::executable();
  const auto size = std::filesystem::file_size(exe, error);
  const auto time = std::filesystem::last_write_time(exe, error).time_since_epoch().count();
  h = fnv(h, &size, sizeof(size));
  h = fnv(h, &time, sizeof(time));
  // The options that change emitted code (cpu/dynarmic_backend.cpp).
  for (const char* name : {"VENTURE_HLE_THUNK_METADATA", "VENTURE_JIT_ADDRESS_FOLD", "VENTURE_JIT_BLOCK_ALIGN", "VENTURE_JIT_BOUNDED_HALT",
                           "VENTURE_JIT_COLD_ABORT_STATE", "VENTURE_JIT_COLD_REGION", "VENTURE_JIT_COMPACT_LINKS", "VENTURE_JIT_DISABLED_HOST_FEATURES",
                           "VENTURE_JIT_DISPATCH_BITS", "VENTURE_JIT_EXCLUSIVE_ELISION", "VENTURE_JIT_EXCLUSIVE_GENERATIONS", "VENTURE_JIT_FRAME_CHAIN",
                           "VENTURE_JIT_HOST_RETURNS", "VENTURE_JIT_INLINE_ATOMICS", "VENTURE_JIT_INLINE_DISPATCH", "VENTURE_JIT_INLINE_RSB_POP",
                           "VENTURE_JIT_LOCKED_EXCLUSIVE_LOADS", "VENTURE_JIT_LOW_LANE_ALIAS", "VENTURE_JIT_NZCV_LIVENESS", "VENTURE_JIT_RECIP_ESTIMATE_CALL",
                           "VENTURE_JIT_REDUCED_ERROR_FP", "VENTURE_JIT_SITE_CACHE", "VENTURE_JIT_SVC_RETURN_LINK", "VENTURE_PROFILE_OUTLINED_ATOMICS"}) {
    const auto* value = environment::getenv(name);
    h = fnv(h, name);
    h = fnv(h, value ? std::string_view(value) : std::string_view("unset"));
  }
#if defined(__x86_64__) || defined(_M_X64)
  unsigned regs[4]{};
  for (const unsigned leaf : {0x1u, 0x7u, 0x80000001u}) {
    if (__get_cpuid_count(leaf, 0, &regs[0], &regs[1], &regs[2], &regs[3])) {
      regs[1] &= leaf == 1 ? 0x00ffffffu : ~0u;  // leaf 1 EBX: APIC ID and CPU count vary
      h = fnv(h, regs, sizeof(regs));
    }
  }
#endif
  return h;
}

std::FILE* open_file(const std::filesystem::path& file, const char* mode) {
#ifdef _WIN32
  const std::wstring wide(mode, mode + std::strlen(mode));
  return _wfopen(paths::io_path(file).c_str(), wide.c_str());
#else
  return std::fopen(paths::io_path(file).c_str(), mode);
#endif
}

// A read-only view of the first `size` bytes, kept for the process lifetime.
const std::uint8_t* map_file(const std::filesystem::path& file, std::uint64_t size) {
  if (!size) return nullptr;
#ifdef _WIN32
  HANDLE handle = CreateFileW(paths::io_path(file).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return nullptr;
  HANDLE mapping = CreateFileMappingW(handle, nullptr, PAGE_READONLY, DWORD(size >> 32), DWORD(size), nullptr);
  CloseHandle(handle);
  if (!mapping) return nullptr;
  void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, size);
  CloseHandle(mapping);
  return static_cast<const std::uint8_t*>(view);
#else
  const int fd = ::open(paths::io_path(file).c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return nullptr;
  void* view = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  return view == MAP_FAILED ? nullptr : static_cast<const std::uint8_t*>(view);
#endif
}

// A read-only view of [offset, offset + size), kept for the process lifetime: the
// records one batch appended, so installs read them without a lock or a file read.
const std::uint8_t* map_range(const std::filesystem::path& file, std::uint64_t offset, std::uint64_t size) {
  if (!size) return nullptr;
#ifdef _WIN32
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  const std::uint64_t base = offset / info.dwAllocationGranularity * info.dwAllocationGranularity, end = offset + size;
  HANDLE handle = CreateFileW(paths::io_path(file).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return nullptr;
  HANDLE mapping = CreateFileMappingW(handle, nullptr, PAGE_READONLY, DWORD(end >> 32), DWORD(end), nullptr);
  CloseHandle(handle);
  if (!mapping) return nullptr;
  void* view = MapViewOfFile(mapping, FILE_MAP_READ, DWORD(base >> 32), DWORD(base), SIZE_T(end - base));
  CloseHandle(mapping);
  return view ? static_cast<const std::uint8_t*>(view) + (offset - base) : nullptr;
#else
  const auto page = std::uint64_t(sysconf(_SC_PAGESIZE));
  const std::uint64_t base = offset / page * page;
  const int fd = ::open(paths::io_path(file).c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return nullptr;
  void* view = mmap(nullptr, offset + size - base, PROT_READ, MAP_PRIVATE, fd, off_t(base));
  ::close(fd);
  return view == MAP_FAILED ? nullptr : static_cast<const std::uint8_t*>(view) + (offset - base);
#endif
}

void unmap(const std::uint8_t* view, std::uint64_t size) {
#ifdef _WIN32
  (void)size;
  UnmapViewOfFile(view);
#else
  munmap(const_cast<std::uint8_t*>(view), size);
#endif
}

struct Header {
  std::uint64_t descriptor;
  std::uint32_t size;
  std::uint32_t check;
};
static_assert(sizeof(Header) == 16);

// The OS reader-writer lock: a shared acquire is one atomic operation (libc++'s
// std::shared_mutex takes a mutex for every reader).
class RwLock {
 public:
#ifdef _WIN32
  void lock() noexcept { AcquireSRWLockExclusive(&lock_); }
  void unlock() noexcept { ReleaseSRWLockExclusive(&lock_); }
  void lock_shared() noexcept { AcquireSRWLockShared(&lock_); }
  void unlock_shared() noexcept { ReleaseSRWLockShared(&lock_); }
 private:
  SRWLOCK lock_ = SRWLOCK_INIT;
#else
  ~RwLock() { pthread_rwlock_destroy(&lock_); }
  void lock() noexcept { pthread_rwlock_wrlock(&lock_); }
  void unlock() noexcept { pthread_rwlock_unlock(&lock_); }
  void lock_shared() noexcept { pthread_rwlock_rdlock(&lock_); }
  void unlock_shared() noexcept { pthread_rwlock_unlock(&lock_); }
 private:
  pthread_rwlock_t lock_ = PTHREAD_RWLOCK_INITIALIZER;
#endif
};

// Blocks stored this run, by descriptor. Every guest thread looks blocks up and
// stores them, the background translators more often still: each shard has its own lock.
struct Shard {
  RwLock lock;
  // Stored, not yet taken by the writer; being written; written, in the views their batches mapped.
  std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> pending, in_flight;
  boost::unordered_flat_map<std::uint64_t, const std::uint8_t*> index;
  // The newest copy first: a recompile (changed guest code) replaces a written block. Under the lock.
  const std::vector<std::uint8_t>* unwritten(std::uint64_t descriptor) const {
    if (const auto it = pending.find(descriptor); it != pending.end()) return &it->second;
    if (const auto it = in_flight.find(descriptor); it != in_flight.end()) return &it->second;
    return nullptr;
  }
};
constexpr std::size_t kShards = 64;
std::size_t shard_of(std::uint64_t descriptor) { return (descriptor * 0x9e3779b97f4a7c15ull) >> 58; }
static_assert(kShards == 64);
} // namespace

struct PortableBlockCache::Impl {
  std::filesystem::path file;
  // <cache>/<package>/<version>/jit-blocks of every title, when the directory is the default one.
  std::filesystem::path titles;
  // The writer's state (end, writing) and flush(); the blocks are in the shards.
  std::mutex mutex;
  std::FILE* writer = nullptr;
  std::unique_ptr<FileLock> lock;
  // Blocks present at startup: an immutable index into a mapped view, read without locks.
  const std::uint8_t* view = nullptr;
  std::uint64_t view_size = 0;
  boost::unordered_flat_map<std::uint64_t, std::uint64_t> loaded;  // descriptor -> record offset
  // The file's last explored mark at startup: loaded records before it, bar its exceptions, are explored.
  std::uint64_t explored_end = 0;
  boost::unordered_flat_set<std::uint64_t> explored_exceptions;
  // Descriptors of records no mark covers yet (the writer's, under the mutex): loaded ones past
  // the last mark, the mark's exceptions, and every record written since.
  std::vector<std::uint64_t> unmarked;
  std::array<Shard, kShards> shards;
  Shard& shard(std::uint64_t descriptor) { return shards[shard_of(descriptor)]; }
  std::uint64_t end = 0;
  std::atomic<std::uint64_t> end_published{0};  // end, for readers without the mutex
  std::condition_variable changed;
  bool writing = false;
  bool write_failed = false;  // a write to the file failed; only the batch being written uses it
  std::atomic<std::uint64_t> hits{0}, misses{0}, stores{0}, verified{0}, mismatched{0}, benign{0};
  // Raw bytes of the records in pending and in_flight, and stores dropped past kUnwrittenLimit.
  std::atomic<std::uint64_t> unwritten{0}, dropped{0};
  // Verify mode: every stored block is translated again and compared.
  bool verify = false;
  std::uint32_t exclude = 0;
  std::uint64_t svc_mask = 0, svc_value = 0, mask = 0, value = 0;
  std::atomic<std::uint64_t> svc_installed{0};
  // The file's dictionary: set by open() or by the first batch the writer stores, before
  // any record that needs it is indexed, and never changed after.
  std::vector<std::uint8_t> dictionary;
  bool has_dictionary = false;
  LZ4_stream_t* dictionary_stream = nullptr;  // the writer's, loaded once
  LZ4_stream_t* record_stream = nullptr;

  // A stored record ([u32 size][LZ4 data]) into `out`; false if it does not decode.
  bool decompress(const std::uint8_t* stored, std::size_t size, std::vector<std::uint8_t>& out) const {
    std::uint32_t raw;
    if (size < sizeof(raw)) return false;
    std::memcpy(&raw, stored, sizeof(raw));
    if (raw > kRecordLimit) return false;
    out.resize(raw);
    const int n = LZ4_decompress_safe_usingDict(reinterpret_cast<const char*>(stored + sizeof(raw)), reinterpret_cast<char*>(out.data()),
                                                int(size - sizeof(raw)), int(raw), reinterpret_cast<const char*>(dictionary.data()),
                                                int(dictionary.size()));
    return n == int(raw);
  }

  // Writer: compresses one block against the dictionary into `out`.
  void compress(const std::vector<std::uint8_t>& raw, std::vector<std::uint8_t>& out) {
    if (!dictionary_stream) {
      dictionary_stream = LZ4_createStream();
      record_stream = LZ4_createStream();
      LZ4_loadDict(dictionary_stream, reinterpret_cast<const char*>(dictionary.data()), int(dictionary.size()));
    }
    const std::uint32_t size = std::uint32_t(raw.size());
    out.resize(sizeof(size) + std::size_t(LZ4_compressBound(int(size))));
    std::memcpy(out.data(), &size, sizeof(size));
    LZ4_resetStream_fast(record_stream);
    LZ4_attach_dictionary(record_stream, dictionary_stream);
    const int n = LZ4_compress_fast_continue(record_stream, reinterpret_cast<const char*>(raw.data()), reinterpret_cast<char*>(out.data() + sizeof(size)),
                                             int(size), int(out.size() - sizeof(size)), 1);
    out.resize(sizeof(size) + std::size_t(n > 0 ? n : 0));
  }

  void open() {
    const auto directory = file.parent_path();
    std::filesystem::create_directories(directory);
    try {
      lock = std::make_unique<FileLock>(directory / "ptc.lock", false);
    } catch (...) {
      // Another process for this game owns the file: read-only.
    }
    if (lock) {
      // Files of other builds or options never match again.
      for (const auto& e : std::filesystem::directory_iterator(directory)) {
        std::error_code ignored;
        if (e.path().extension() == ".vpc" && e.path() != file) std::filesystem::remove(e.path(), ignored);
      }
    }
    if (lock && !titles.empty()) trim_other_titles(kTotalLimit - std::min(kTotalLimit, kFileLimit));
    std::uint64_t good = 0;
    std::error_code error;
    const auto size = std::filesystem::exists(file, error) ? std::filesystem::file_size(file, error) : 0;
    if (const auto* bytes = size >= 4 ? map_file(file, size) : nullptr) {
      if (!std::memcmp(bytes, kMagic, 4)) {
        good = 4;
        Header h;
        while (good + sizeof(h) <= size) {
          std::memcpy(&h, bytes + good, sizeof(h));
          if (h.size > kRecordLimit || good + sizeof(h) + h.size > size) break;  // a torn tail
          if (good == 4) {
            // The dictionary first, or the file holds nothing usable.
            if (h.descriptor != kDictionaryDescriptor || check(bytes + good + sizeof(h), h.size) != h.check) break;
            dictionary.assign(bytes + good + sizeof(h), bytes + good + sizeof(h) + h.size);
            has_dictionary = true;
          } else if (h.descriptor == kExploredDescriptor) {
            if (h.size % 8 == 0 && check(bytes + good + sizeof(h), h.size) == h.check) {
              explored_end = good;
              explored_exceptions.clear();
              for (std::uint32_t i = 0; i < h.size; i += 8) {
                std::uint64_t d;
                std::memcpy(&d, bytes + good + sizeof(h) + i, 8);
                explored_exceptions.insert(d);
              }
            }
          } else {
            loaded.insert_or_assign(h.descriptor, good);
          }
          good += sizeof(h) + h.size;
        }
        if (!has_dictionary) loaded.clear();
      }
      if (good) {
        view = bytes;
        view_size = good;
      } else {
        // Another format: the file is rewritten below, and a mapped file cannot be truncated.
        unmap(bytes, size);
      }
    }
    if (lock) {
      if (good == 0) {
        std::FILE* f = open_file(file, "wb");
        if (!f) return;
        std::fwrite(kMagic, 1, 4, f);
        std::fclose(f);
        good = 4;
      } else if (size != good) {
        // A torn tail from a killed process. The view must go first: a mapped file cannot shrink.
        unmap(view, size);
        error.clear();
        std::filesystem::resize_file(file, good, error);
        view = map_file(file, good);
        if (!view) loaded.clear();
        if (error) {
          // Still mapped elsewhere (a previous run not yet gone): appended records would land
          // after the torn bytes, not at `good` where they are indexed. Read-only this run.
          log::warn("jit", "Portable block cache: cannot cut a torn tail ({}); not writing this run", error.message());
          lock.reset();
        }
      }
      if (lock) writer = open_file(file, "ab");
    }
    end = good;
    end_published.store(end);
    if (writer)
      for (const auto& [descriptor, offset] : loaded)
        if (offset >= explored_end || explored_exceptions.contains(descriptor)) unmarked.push_back(descriptor);
    log::info("jit", "Portable block cache: {} blocks ({} MB) in {}, {} explored", loaded.size(), end >> 20, paths::utf8(file.filename()),
              explored_end ? loaded.size() - std::min(loaded.size(), unmarked.size()) : 0);
    if (view && view_size) {
      // Read the file ahead off the guest threads, so an install never waits on the disk. Only into
      // the OS file cache: the pages join this process's working set when an install reads them,
      // most records are never installed in a run, and the file is gigabytes for big titles.
      std::thread([bytes = view, size = view_size] {
#ifdef _WIN32
        WIN32_MEMORY_RANGE_ENTRY range{const_cast<std::uint8_t*>(bytes), SIZE_T(size)};
        PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
#else
        madvise(const_cast<std::uint8_t*>(bytes), size, MADV_WILLNEED);
#endif
      }).detach();
    }
    if (writer) std::thread([this] { run(); }).detach();
  }

  // Least recently written first, other titles' caches go until the rest fit in
  // `budget`. A cache in use (its lock held) is never removed.
  void trim_other_titles(std::uint64_t budget) noexcept {
    try {
      struct Entry { std::filesystem::path file; std::uintmax_t size; std::filesystem::file_time_type time; };
      std::vector<Entry> entries;
      std::uint64_t total = 0;
      std::error_code error;
      for (const auto& package : std::filesystem::directory_iterator(titles, error)) {
        if (!package.is_directory(error)) continue;
        for (const auto& version : std::filesystem::directory_iterator(package.path(), error)) {
          const auto blocks = version.path() / "jit-blocks";
          if (blocks == file.parent_path() || !std::filesystem::is_directory(blocks, error)) continue;
          for (const auto& e : std::filesystem::directory_iterator(blocks, error)) {
            if (e.path().extension() != ".vpc") continue;
            const auto size = e.file_size(error);
            if (error) continue;
            entries.push_back({e.path(), size, e.last_write_time(error)});
            total += size;
          }
        }
      }
      if (total <= budget) return;
      std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
      for (const auto& e : entries) {
        if (total <= budget) break;
        std::unique_ptr<FileLock> other;
        try { other = std::make_unique<FileLock>(e.file.parent_path() / "ptc.lock", false); } catch (...) { continue; }
        if (std::filesystem::remove(e.file, error)) {
          total -= e.size;
          log::info("jit", "Portable block cache: removed {} MB of another title's blocks (all titles over {} GB)", e.size >> 20, kTotalLimit >> 30);
        }
      }
    } catch (...) {}
  }

  static long long _ftelli64_or(std::FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return ftello(f);
#endif
  }

  // One batch at a time: flush() and the background writer both call this, and a batch
  // uses in_flight, the file position, the dictionary and the compression streams
  // without other locks. A second batch would replace in_flight under the first one
  // and free its buffers while the first still reads and then frees them.
  void write_pending(std::unique_lock<std::mutex>& held) {
    if (!writer) return;
    changed.wait(held, [&] { return !writing; });
    writing = true;
    const auto base = end;
    held.unlock();
    // Each batch stays visible to lookups (in_flight) until its records are indexed.
    // Only this thread changes in_flight, so it reads them without the lock.
    bool any = false;
    for (auto& shard : shards) {
      std::unique_lock lock(shard.lock);
      shard.in_flight = std::move(shard.pending);
      shard.pending.clear();
      any = any || !shard.in_flight.empty();
    }
    std::uint64_t batch_bytes = 0;
    for (const auto& shard : shards)
      for (const auto& [descriptor, bytes] : shard.in_flight) batch_bytes += bytes.size();
    std::uint64_t offset = base;
    std::array<std::vector<std::pair<std::uint64_t, std::uint64_t>>, kShards> written;
    // After a failed write the file holds bytes past `end` that no record accounts for:
    // a later batch's offsets would no longer match the file, so nothing more is written.
    const bool writable = any && !write_failed;
    if (writable && !has_dictionary && offset == sizeof(kMagic)) {
      // A new file: its first blocks become the dictionary every record is compressed against.
      std::vector<std::uint8_t> sample;
      for (std::size_t i = 0; i < kShards && sample.size() < kDictionarySize; ++i)
        for (const auto& [descriptor, bytes] : shards[i].in_flight) {
          sample.insert(sample.end(), bytes.begin(), bytes.begin() + std::ptrdiff_t(std::min(bytes.size(), kDictionarySize - sample.size())));
          if (sample.size() >= kDictionarySize) break;
        }
      const Header h{kDictionaryDescriptor, std::uint32_t(sample.size()), check(sample.data(), sample.size())};
      if (std::fwrite(&h, sizeof(h), 1, writer) == 1 && std::fwrite(sample.data(), 1, sample.size(), writer) == sample.size()) {
        dictionary = std::move(sample);
        has_dictionary = true;
        offset += sizeof(h) + dictionary.size();
      } else {
        write_failed = true;
      }
    }
    if (writable && has_dictionary && !write_failed) {
      bool full = false;
      std::vector<std::uint8_t> stored;
      for (std::size_t i = 0; i < kShards && !full; ++i) {
        for (const auto& [descriptor, bytes] : shards[i].in_flight) {
          compress(bytes, stored);
          const Header h{descriptor, std::uint32_t(stored.size()), check(stored.data(), stored.size())};
          if (stored.size() <= sizeof(std::uint32_t) || offset + sizeof(Header) + stored.size() > kFileLimit) {
            full = true;
            break;
          }
          if (std::fwrite(&h, sizeof(h), 1, writer) != 1 || std::fwrite(stored.data(), 1, stored.size(), writer) != stored.size()) {
            full = write_failed = true;
            break;
          }
          written[i].emplace_back(descriptor, offset);
          offset += sizeof(h) + stored.size();
        }
      }
    }
    if (writable) std::fflush(writer);
    // Unmapped records (no view) are translated again when they are next needed.
    // The records were just written, so their pages are in the OS file cache: an install reads
    // them without the disk, and only the pages installs read join the working set.
    const auto* view = offset > base ? map_range(file, base, offset - base) : nullptr;
    if (any) {
      for (std::size_t i = 0; i < kShards; ++i) {
        // The batch's buffers are freed after the lock is released: freeing thousands of
        // them under it stalled lookups on the shard for milliseconds.
        std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> done;
        std::unique_lock lock(shards[i].lock);
        if (view)
          for (const auto& [descriptor, at] : written[i]) shards[i].index.insert_or_assign(descriptor, view + (at - base));
        done.swap(shards[i].in_flight);
      }
    }
    unwritten.fetch_sub(batch_bytes, std::memory_order_relaxed);
    held.lock();
    for (const auto& batch : written)
      for (const auto& [descriptor, at] : batch) unmarked.push_back(descriptor);
    end = offset;
    // A failed file takes no more stores (store() and accepting() read this).
    end_published.store(write_failed ? std::max(end, kFileLimit) : end, std::memory_order_relaxed);
    writing = false;
    changed.notify_all();
  }

  // Appends a mark at the end of the file: every record before it is explored, bar the
  // unmarked ones `explored` rejects. False if nothing was new or the file takes no writes.
  bool mark_explored(const std::function<bool(std::uint64_t)>& explored) {
    std::unique_lock held(mutex);
    if (!writer) return false;
    changed.wait(held, [&] { return !writing; });
    if (write_failed || end + sizeof(Header) >= kFileLimit) return false;
    std::vector<std::uint64_t> exceptions;
    std::sort(unmarked.begin(), unmarked.end());
    unmarked.erase(std::unique(unmarked.begin(), unmarked.end()), unmarked.end());
    for (const auto d : unmarked)
      if (!explored(d)) exceptions.push_back(d);
    // No record became explored since the last mark: a new one would cover nothing more.
    if (exceptions.size() == unmarked.size()) return false;
    if (exceptions.size() * 8 > kRecordLimit) return false;
    const Header h{kExploredDescriptor, std::uint32_t(exceptions.size() * 8),
                   check(reinterpret_cast<const std::uint8_t*>(exceptions.data()), exceptions.size() * 8)};
    if (std::fwrite(&h, sizeof(h), 1, writer) != 1 || std::fwrite(exceptions.data(), 8, exceptions.size(), writer) != exceptions.size()) {
      write_failed = true;
      end_published.store(std::max(end, kFileLimit), std::memory_order_relaxed);
      return false;
    }
    std::fflush(writer);
    end += sizeof(h) + h.size;
    end_published.store(end, std::memory_order_relaxed);
    log::info("jit", "Portable block cache: explored through {} MB; {} of {} new records not yet", end >> 20, exceptions.size(), unmarked.size());
    unmarked = std::move(exceptions);
    return true;
  }

  void run() {
    std::unique_lock held(mutex);
    auto reported = std::chrono::steady_clock::now();
    std::uint64_t last = 0;
    for (;;) {
      // Batches back to back while a quarter of the unwritten limit or more waits.
      if (unwritten.load(std::memory_order_relaxed) < kUnwrittenLimit / 4) changed.wait_for(held, std::chrono::milliseconds(500));
      write_pending(held);
      if (const auto now = std::chrono::steady_clock::now(); now - reported > std::chrono::seconds(10)) {
        reported = now;
        const auto h = hits.load(), m = misses.load(), s = stores.load();
        if (h + m + s != last) {
          last = h + m + s;
          log::info("jit", "Portable block cache: {} found, {} missed, {} stored, {} MB, {} verified, {} differ, {} benign; {}", h, m, s, end >> 20, verified.load(),
                    mismatched.load(), benign.load(), Dynarmic::Backend::X64::PortableStatsReport());
          // Memory its indexes and unwritten records hold (flat maps: bucket_count 16-byte slots).
          std::uint64_t index_bytes = loaded.bucket_count() * 16 + explored_exceptions.bucket_count() * 8 + unmarked.capacity() * 8, unwritten_held = 0;
          std::size_t records = 0;
          for (auto& shard : shards) {
            std::shared_lock lock(shard.lock);
            index_bytes += shard.index.bucket_count() * 16;
            for (const auto* map : {&shard.pending, &shard.in_flight}) {
              records += map->size();
              unwritten_held += map->bucket_count() * 8;
              for (const auto& entry : *map) unwritten_held += entry.second.capacity() + 48;
            }
          }
          log::info("jit", "Portable block cache memory: indexes {} MiB, {} unwritten records {} MiB, {} dropped at the limit", index_bytes >> 20,
                    records, unwritten_held >> 20, dropped.load());
          if (svc_mask) log::info("jit", "bisect: {} HLE-call blocks installed", svc_installed.load());
        }
      }
    }
  }
};

PortableBlockCache* PortableBlockCache::get() {
  static PortableBlockCache* instance = []() -> PortableBlockCache* {
    // On by default; VENTURE_JIT_PTC=0 (Settings > Runtime > Keep translated code off) disables it.
    const auto* e = environment::getenv("VENTURE_JIT_PTC");
    if (e && std::string_view(e) == "0") return nullptr;
    try {
      // No cache directory (tests, tools): no cache, never one relative to the working directory.
      const auto& cache = process_config().cache_dir;
      auto directory = cache.empty() ? cache : cache / "jit-blocks";
      if (const auto* override_dir = environment::getenv("VENTURE_JIT_PTC_DIR")) directory = paths::from_utf8(override_dir);
      if (directory.empty()) return nullptr;
      char name[32];
      std::snprintf(name, sizeof(name), "%016llx.vpc", static_cast<unsigned long long>(fingerprint()));
      auto* impl = new Impl;
      impl->file = directory / name;
      if (auto titles = directory.parent_path().parent_path().parent_path(); !environment::getenv("VENTURE_JIT_PTC_DIR") && titles.filename() == "cache")
        impl->titles = std::move(titles);
      if (const auto* verify = environment::getenv("VENTURE_JIT_PTC_VERIFY")) impl->verify = std::string_view(verify) == "1";
      if (const auto* exclude = environment::getenv("VENTURE_JIT_PTC_EXCLUDE")) impl->exclude = std::uint32_t(std::strtoul(exclude, nullptr, 0));
      if (const auto* mask = environment::getenv("VENTURE_JIT_PTC_SVC_MASK")) impl->svc_mask = std::strtoull(mask, nullptr, 0);
      if (const auto* value = environment::getenv("VENTURE_JIT_PTC_SVC_VALUE")) impl->svc_value = std::strtoull(value, nullptr, 0);
      if (const auto* mask = environment::getenv("VENTURE_JIT_PTC_MASK")) impl->mask = std::strtoull(mask, nullptr, 0);
      if (const auto* value = environment::getenv("VENTURE_JIT_PTC_VALUE")) impl->value = std::strtoull(value, nullptr, 0);
      impl->open();
      if (!impl->view && !impl->writer) return nullptr;
      return new PortableBlockCache(impl);
    } catch (const std::exception& error) {
      log::warn("jit", "Portable block cache disabled: {}", error.what());
      return nullptr;
    }
  }();
  return instance;
}

namespace {
thread_local std::uint64_t verify_descriptor = ~0ull;
thread_local std::vector<std::uint8_t> verify_expected;
}
void PortableBlockCache::store(std::uint64_t descriptor, const std::uint8_t* data, std::size_t size) noexcept {
  if (impl_->verify && descriptor == verify_descriptor) {
    verify_descriptor = ~0ull;
    const auto difference = Dynarmic::Backend::X64::ComparePortableBlocks(verify_expected, {data, size});
    impl_->verified.fetch_add(1, std::memory_order_relaxed);
    if (difference.starts_with('~') || difference == "guest ranges differ") {
      impl_->benign.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (!difference.empty() && impl_->mismatched.fetch_add(1, std::memory_order_relaxed) < 200)
      log::warn("jit", "Portable block {:#x} differs: {}", descriptor, difference);
    return;
  }
  if (!impl_->writer || size > kRecordLimit) return;
  try {
    if (impl_->end_published.load(std::memory_order_relaxed) >= kFileLimit) return;
    // A first launch's background translation outran the writer by 2.3M records (934 MiB
    // at 13 s in AC Nexus): past the limit the record is dropped instead of held.
    const auto before = impl_->unwritten.load(std::memory_order_relaxed);
    if (before + size > kUnwrittenLimit) {
      impl_->dropped.fetch_add(1, std::memory_order_relaxed);
      impl_->changed.notify_all();
      return;
    }
    std::vector<std::uint8_t> bytes(data, data + size);
    auto& shard = impl_->shard(descriptor);
    {
      std::unique_lock lock(shard.lock);
      // A recompile of a stored block (its guest code changed) replaces it.
      auto [it, inserted] = shard.pending.try_emplace(descriptor);
      const std::size_t replaced = inserted ? 0 : it->second.size();
      it->second = std::move(bytes);
      impl_->unwritten.fetch_add(size - replaced, std::memory_order_relaxed);
    }
    impl_->stores.fetch_add(1, std::memory_order_relaxed);
    // A quarter of the limit waiting: the writer starts now instead of at its next half-second tick.
    if (before < kUnwrittenLimit / 4 && before + size >= kUnwrittenLimit / 4) impl_->changed.notify_all();
  } catch (...) {}
}

std::pair<const std::uint8_t*, std::size_t> PortableBlockCache::find(std::uint64_t descriptor) noexcept {
  thread_local std::vector<std::uint8_t> buffer;
  try {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    if (const auto it = impl_->loaded.find(descriptor); it != impl_->loaded.end() && impl_->view) {
      Header h;
      std::memcpy(&h, impl_->view + it->second, sizeof(h));
      if (check(impl_->view + it->second + sizeof(h), h.size) == h.check && impl_->decompress(impl_->view + it->second + sizeof(h), h.size, buffer)) {
        data = buffer.data();
        size = buffer.size();
      }
    } else {
      const std::uint8_t* record = nullptr;
      {
        auto& shard = impl_->shard(descriptor);
        std::shared_lock lock(shard.lock);
        // Written records first: nearly every hit is one. A recompile still in pending
        // loses to its older record, which the install then rejects by the guest code hash.
        if (const auto written = shard.index.find(descriptor); written != shard.index.end()) {
          record = written->second;
        } else if (const auto* bytes = shard.unwritten(descriptor)) {
          buffer = *bytes;
          data = buffer.data();
          size = buffer.size();
        }
      }
      if (record) {
        Header h;
        std::memcpy(&h, record, sizeof(h));
        if (h.descriptor == descriptor && check(record + sizeof(h), h.size) == h.check && impl_->decompress(record + sizeof(h), h.size, buffer)) {
          data = buffer.data();
          size = buffer.size();
        }
      }
    }
    if (!data) {
      impl_->misses.fetch_add(1, std::memory_order_relaxed);
      return {nullptr, 0};
    }
    const std::span<const std::uint8_t> bytes{data, size};
    if (impl_->exclude) {
      // Diagnostics: refuse blocks with any reference kind in the mask (bit 16: fastmem sites, 17: links).
      const auto block = Dynarmic::Backend::X64::DeserializePortableBlock(bytes);
      if (!block) return {nullptr, 0};
      std::uint32_t kinds = (block->fastmem.empty() ? 0 : 1u << 16) | (block->links.empty() ? 0 : 1u << 17);
      for (const auto& r : block->relocs) kinds |= 1u << unsigned(r.target.kind);
      if (kinds & impl_->exclude) return {nullptr, 0};
    }
    if (impl_->mask) {
      // Diagnostics: install only the blocks whose descriptor hash matches (value bit 63: all others).
      std::uint64_t h = descriptor * 0x9e3779b97f4a7c15ull;
      h ^= h >> 31;
      const bool invert = impl_->value >> 63, match = (h & impl_->mask) == (impl_->value & impl_->mask);
      if (match == invert) return {nullptr, 0};
      if (impl_->mask >= 0x3fff && match) {
        DlInfo info;
        const auto pc = descriptor & 0x00ffffffffffffffull;
        Linker::get().try_address_info(pc, &info);
        log::info("jit", "bisect match {:#x} {}+{:#x} {}+{:#x}", descriptor, info.path, pc - info.base, info.symbol, pc - info.symbol_address);
      }
    }
    if (impl_->svc_mask) {
      // Diagnostics: of the blocks with HLE calls, install only those whose hash matches.
      const auto block = Dynarmic::Backend::X64::DeserializePortableBlock(bytes);
      if (!block) return {nullptr, 0};
      if (std::any_of(block->relocs.begin(), block->relocs.end(), [](const auto& r) { return r.target.kind == Dynarmic::Backend::X64::PortableRefKind::Svc; })) {
        std::uint64_t h = descriptor * 0x9e3779b97f4a7c15ull;
        h ^= h >> 31;
        if ((h & impl_->svc_mask) != impl_->svc_value) return {nullptr, 0};
        impl_->svc_installed.fetch_add(1, std::memory_order_relaxed);
        if (impl_->svc_mask >= 0x3fff) log::info("jit", "bisect install {:#x}", descriptor);
      }
    }
    impl_->hits.fetch_add(1, std::memory_order_relaxed);
    if (impl_->verify) {
      verify_descriptor = descriptor;
      verify_expected.assign(data, data + size);
      return {nullptr, 0};
    }
    return {data, size};
  } catch (...) {
    return {nullptr, 0};
  }
}

bool PortableBlockCache::accepting() const noexcept {
  return impl_->writer && impl_->end_published.load(std::memory_order_relaxed) < kFileLimit;
}

bool PortableBlockCache::contains(std::uint64_t descriptor) noexcept {
  if (impl_->view && impl_->loaded.contains(descriptor)) return true;
  auto& shard = impl_->shard(descriptor);
  std::shared_lock lock(shard.lock);
  return shard.index.contains(descriptor) || shard.unwritten(descriptor);
}

bool PortableBlockCache::explored(std::uint64_t descriptor) const noexcept {
  if (!impl_->explored_end) return false;
  const auto it = impl_->loaded.find(descriptor);
  return it != impl_->loaded.end() && it->second < impl_->explored_end && !impl_->explored_exceptions.contains(descriptor);
}

bool PortableBlockCache::mark_explored(const std::function<bool(std::uint64_t)>& explored) noexcept {
  try {
    return impl_->mark_explored(explored);
  } catch (...) {
    return false;
  }
}

bool PortableBlockCache::copy(std::uint64_t descriptor, std::vector<std::uint8_t>& out) noexcept {
  try {
    const std::uint8_t* record = nullptr;
    if (const auto it = impl_->loaded.find(descriptor); it != impl_->loaded.end() && impl_->view) record = impl_->view + it->second;
    if (!record) {
      auto& shard = impl_->shard(descriptor);
      std::shared_lock lock(shard.lock);
      if (const auto* bytes = shard.unwritten(descriptor)) {
        out = *bytes;
        return true;
      }
      if (const auto it = shard.index.find(descriptor); it != shard.index.end()) record = it->second;
    }
    if (!record) return false;
    Header h;
    std::memcpy(&h, record, sizeof(h));
    return check(record + sizeof(h), h.size) == h.check && impl_->decompress(record + sizeof(h), h.size, out);
  } catch (...) {
    return false;
  }
}

std::uint64_t PortableBlockCache::unwritten_bytes() const noexcept { return impl_->unwritten.load(std::memory_order_relaxed); }

void PortableBlockCache::flush() noexcept {
  try {
    std::unique_lock held(impl_->mutex);
    impl_->write_pending(held);
  } catch (...) {}
}
} // namespace venture
