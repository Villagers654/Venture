#include "cpu/block_list.h"
#include "common/environment.h"
#include "common/log.h"
#include "common/meta/auth.h"
#include "common/paths.h"
#include "core/elf.h"
#include "core/linker.h"
#include "core/process.h"
#include "core/profile.h"
#include <dynarmic/interface/profiling.h>
#include "common/file_ops.h"
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace venture {
namespace {
const std::size_t limit = [] { const auto* e = environment::getenv("VENTURE_JIT_PRETRANSLATE_LIMIT"); return e ? std::size_t(std::stoull(e)) : std::size_t(1) << 20; }();
constexpr std::uintmax_t file_limit = 64 * 1024 * 1024;
constexpr std::uintmax_t directory_limit = 512ull * 1024 * 1024;
// Soon after a burst of new blocks (a level load), else twice a minute.
constexpr std::int64_t snapshot_interval_ns = 30'000'000'000, burst_interval_ns = 5'000'000'000;
constexpr std::uint64_t burst_blocks = 4096;
constexpr char magic[4] = {'V', 'H', 'L', '1'};
thread_local JitBlockList* recording = nullptr;
std::mutex files_mutex;
std::atomic<void (*)()> snapshot_observer{nullptr};
struct Module {
  GuestAddr base;
  std::string key;
  std::vector<std::pair<GuestAddr, GuestAddr>> executable;
  bool contains(GuestAddr pc) const {
    return std::any_of(executable.begin(), executable.end(), [&](auto r) { return pc >= r.first && pc < r.second; });
  }
};
// A file's identity across runs: its name, size and first and last 64 KiB. A stale
// hint costs nothing, since only cached blocks whose guest code matches are installed.
std::string identity(const std::filesystem::path& path) {
  static std::map<std::pair<std::filesystem::path, std::filesystem::file_time_type>, std::string> cache;
  static std::mutex cache_mutex;
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) return {};
  const auto time = std::filesystem::last_write_time(path, error);
  if (error) return {};
  std::lock_guard lock(cache_mutex);
  auto key = std::pair{path, time};
  if (const auto it = cache.find(key); it != cache.end()) return it->second;
  constexpr std::uintmax_t window = 64 * 1024;
  std::ifstream stream(paths::io_path(path), std::ios::binary);
  std::string bytes(std::size_t(std::min(size, window)), '\0');
  if (!stream.read(bytes.data(), std::streamsize(bytes.size()))) return {};
  if (size > window) {
    const auto tail = std::min(size - window, window);
    bytes.resize(std::size_t(window + tail));
    stream.seekg(std::streamoff(size - tail));
    if (!stream.read(bytes.data() + window, std::streamsize(tail))) return {};
  }
  auto result = paths::utf8(path.filename()) + ":" + std::to_string(size) + ":" + meta::sha256(bytes).substr(0, 32);
  if (cache.size() > 256) cache.erase(cache.begin());
  return cache.emplace(std::move(key), std::move(result)).first->second;
}
// Background worker only: waits for the loader, and reads files after its lock is released.
std::vector<Module> modules(bool a32) {
  if (auto observer = snapshot_observer.load()) observer();
  std::vector<std::pair<std::filesystem::path, Module>> found;
  Linker::get().for_each_image([&](const Library& lib) {
    // The linker also visits synthesized images (the HLE unwind image), which have no file.
    const auto* image = dynamic_cast<const elf::Image*>(&lib);
    if (!image || (image->isa() == GuestIsa::a32) != a32 || image->source_file().empty()) return;
    Module module{image->load_bias, {}, {}};
    for (const auto& ph : image->headers) if (ph.type == 1 && (ph.flags & 1))
      module.executable.emplace_back(image->load_bias + ph.vaddr, image->load_bias + ph.vaddr + ph.memsz);
    found.emplace_back(image->source_file(), std::move(module));
  });
  std::vector<Module> result;
  for (auto& [path, module] : found) {
    module.key = identity(path);
    if (!module.key.empty()) result.push_back(std::move(module));
  }
  return result;
}
std::uint64_t mask(bool a32) { return a32 ? 0xffffffffull : 0x00ffffffffffffffull; }
struct Row {
  std::uint32_t module;
  std::uint64_t offset, mode;
  bool operator==(const Row&) const = default;
};
struct RowHash {
  std::size_t operator()(const Row& r) const noexcept {
    return std::size_t(((std::uint64_t(r.module) << 48) ^ r.offset ^ r.mode) * 0x9e3779b97f4a7c15ull);
  }
};
struct Hints {
  std::vector<std::string> modules;
  std::vector<Row> rows;
};
// Empty on any malformed input.
// Read with delete sharing (paths::read): another process's save replaces the
// file under its directory lock, which loads do not take, and on Windows that
// replace fails while any reader's handle lacks FILE_SHARE_DELETE.
Hints read(const std::filesystem::path& file) {
  std::string bytes;
  try {
    bytes = paths::read(file, file_limit);
  } catch (...) {
    return {};
  }
  if (bytes.size() < 12 || std::memcmp(bytes.data(), magic, 4)) return {};
  std::size_t at = 4;
  const auto take = [&](void* out, std::size_t n) {
    if (bytes.size() - at < n) return false;
    std::memcpy(out, bytes.data() + at, n);
    at += n;
    return true;
  };
  Hints hints;
  std::uint32_t count = 0;
  if (!take(&count, 4) || count > 4096) return {};
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t length = 0;
    if (!take(&length, 4) || length > 4096 || bytes.size() - at < length) return {};
    hints.modules.emplace_back(bytes.data() + at, length);
    at += length;
  }
  if (!take(&count, 4) || count > limit || (bytes.size() - at) / 20 != count || (bytes.size() - at) % 20) return {};
  hints.rows.resize(count);
  for (auto& row : hints.rows) {
    take(&row.module, 4); take(&row.offset, 8); take(&row.mode, 8);
    if (row.module >= hints.modules.size()) return {};
  }
  return hints;
}
std::string serialize(const Hints& hints) {
  std::string out(magic, 4);
  const auto put = [&](const void* p, std::size_t n) { out.append(static_cast<const char*>(p), n); };
  const auto modules = std::uint32_t(hints.modules.size()), rows = std::uint32_t(hints.rows.size());
  put(&modules, 4);
  for (const auto& m : hints.modules) {
    const auto length = std::uint32_t(m.size());
    put(&length, 4); put(m.data(), m.size());
  }
  put(&rows, 4);
  for (const auto& row : hints.rows) { put(&row.module, 4); put(&row.offset, 8); put(&row.mode, 8); }
  return out;
}
std::filesystem::path directory() {
  if (const auto* override_dir = environment::getenv("VENTURE_JIT_BLOCK_LIST_DIR")) return paths::from_utf8(override_dir);
  const auto& cache = process_config().cache_dir;
  return cache.empty() ? cache : cache / "jit-block-lists";
}
// One background worker for loads and saves. Never destroyed, so process exit
// never joins it; work still queued at exit is dropped (the hints are optional).
struct Worker {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::function<void()>> queue;
  bool started = false, busy = false;
  void push(std::function<void()> task) {
    std::lock_guard lock(mutex);
    if (queue.size() >= 64) return; // Bounded; dropped work only loses hints.
    if (!started) { std::thread([this] { run(); }).detach(); started = true; }
    queue.push_back(std::move(task));
    changed.notify_all();
  }
  void run() {
    std::unique_lock lock(mutex);
    for (;;) {
      changed.wait(lock, [&] { return !queue.empty(); });
      auto task = std::move(queue.front());
      queue.pop_front(); busy = true;
      lock.unlock();
      try { task(); } catch (...) {}
      task = nullptr;
      lock.lock();
      busy = false; changed.notify_all();
    }
  }
  void flush() {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return queue.empty() && !busy; });
  }
};
Worker& worker() { static auto* instance = new Worker; return *instance; }
}
void JitBlockList::retire(JitBlockList&& list) noexcept {
  if (list.file_.empty() || list.descriptors_.empty()) return;
  try { worker().push([list = std::make_shared<JitBlockList>(std::move(list))] { list->save(); }); } catch (...) {}
}
void JitBlockList::flush() noexcept {
  try { worker().flush(); } catch (...) {}
}
std::mutex& JitBlockList::persistence_mutex_for_test() { return files_mutex; }
void JitBlockList::snapshot_observer_for_test(void (*observer)()) { snapshot_observer.store(observer); }
bool JitBlockList::enabled() {
  // On by default with the block cache; VENTURE_JIT_PRETRANSLATE=0 disables it.
  static const bool value = [] { const auto* e = environment::getenv("VENTURE_JIT_PRETRANSLATE"); return !e || std::string_view(e) != "0"; }();
  return value;
}
JitBlockList::Scope::Scope(JitBlockList* list) noexcept : previous(recording) { recording = list; }
JitBlockList::Scope::~Scope() { recording = previous; }
void JitBlockList::compiled(std::uint64_t descriptor) noexcept { if (recording) recording->record(descriptor); }
void JitBlockList::compilation(const Dynarmic::CompileTiming& timing) noexcept {
  if (timing.completed) compiled(timing.location_descriptor);
  profile::register_compile_timing(timing);
}
void JitBlockList::record(std::uint64_t descriptor) noexcept {
  if (!enabled()) return;
  try {
    if (descriptors_.size() < limit) descriptors_.push_back(descriptor);
    else { descriptors_[next_] = descriptor; next_ = (next_ + 1) % limit; }
    ++recorded_;
  } catch (...) {}
}
void JitBlockList::name(std::string_view thread_name, bool a32) noexcept {
  if (!enabled()) return;
  try {
    std::string stem;
    for (const char c : thread_name) stem += std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' ? c : '_';
    const auto dir = directory();
    if (stem.empty() || dir.empty()) return;
    auto file = dir / (stem + (a32 ? "-32.vhl" : "-64.vhl"));
    if (file == file_) return;
    file_ = std::move(file);
    a32_ = a32;
    queued_.clear();
    queued_next_ = 0;
    auto loading = std::make_shared<Loading>();
    loading_ = loading;
    worker().push([file = file_, a32, loading] {
      Hints hints;
      {
        std::lock_guard lock(files_mutex);
        hints = read(file);
      }
      if (!hints.rows.empty()) {
        const auto loaded = modules(a32);
        std::vector<const Module*> by_index(hints.modules.size(), nullptr);
        for (std::size_t i = 0; i < hints.modules.size(); ++i)
          for (const auto& m : loaded) if (m.key == hints.modules[i]) by_index[i] = &m;
        loading->descriptors.reserve(hints.rows.size());
        for (const auto& row : hints.rows) {
          const auto* m = by_index[row.module];
          if (!m || (row.mode & mask(a32)) || row.offset > mask(a32) - m->base || !m->contains(m->base + row.offset)) continue;
          loading->descriptors.push_back((m->base + row.offset) | row.mode);
        }
      }
      loading->ready.store(true, std::memory_order_release);
    });
  } catch (...) { file_.clear(); } // Optional cache failure must never reach guest code.
}
bool JitBlockList::next_hint(std::uint64_t& descriptor) noexcept {
  if (queued_next_ >= queued_.size()) {
    if (!loading_ || !loading_->ready.load(std::memory_order_acquire)) return false;
    queued_ = std::move(loading_->descriptors);
    queued_next_ = 0;
    loading_.reset();
    if (queued_.empty()) return false;
  }
  descriptor = queued_[queued_next_++];
  if (queued_next_ == queued_.size()) { queued_.clear(); queued_.shrink_to_fit(); queued_next_ = 0; }
  return true;
}
bool JitBlockList::snapshot_due(std::int64_t now_ns) const noexcept {
  if (file_.empty() || recorded_ == snapshotted_) return false;
  return now_ns - snapshot_ns_ >= (recorded_ - snapshotted_ >= burst_blocks ? burst_interval_ns : snapshot_interval_ns);
}
JitBlockList JitBlockList::snapshot(std::int64_t now_ns) {
  snapshot_ns_ = now_ns;
  snapshotted_ = recorded_;
  JitBlockList copy;
  copy.a32_ = a32_;
  copy.file_ = file_;
  copy.descriptors_ = descriptors_;
  copy.next_ = next_;
  return copy;
}
void JitBlockList::save() noexcept {
  if (file_.empty() || descriptors_.empty()) return;
  try {
    const auto loaded = modules(a32_);
    std::lock_guard lock(files_mutex);
    // Other processes share the directory: merging, the size budget and the
    // replacement form one critical section under a cross-process lock, so no
    // writer loses another's update or exceeds the budget alongside it.
    std::filesystem::create_directories(file_.parent_path());
    FileLock directory_lock(file_.parent_path() / "persist.lock");
    // Every writer holds this lock until its replace, so a temporary file seen
    // here was left by a process that exited mid-save; it would otherwise count
    // against the directory budget forever. So would the former JSON lists (.vbl).
    for (const auto& e : std::filesystem::directory_iterator(file_.parent_path())) {
      std::error_code ignored;
      const auto extension = e.path().extension();
      if (!e.is_symlink() && e.is_regular_file() && (extension == ".tmp" || extension == ".vbl")) std::filesystem::remove(e.path(), ignored);
    }
    // Oldest first. A hint seen again moves to the end and the oldest beyond the
    // limit are dropped, so a full file adapts to a different scene.
    auto merged = read(file_);
    std::unordered_map<std::string, std::uint32_t> index;
    for (std::uint32_t i = 0; i < merged.modules.size(); ++i) index.emplace(merged.modules[i], i);
    // Newest first, keeping each hint's latest position, then back to oldest first.
    std::vector<Row> fresh;
    std::unordered_set<Row, RowHash> seen;
    for (std::size_t i = descriptors_.size(); i-- > 0;) {
      const auto descriptor = descriptors_[(i + next_) % descriptors_.size()];
      const auto pc = descriptor & mask(a32_);
      const auto module = std::find_if(loaded.begin(), loaded.end(), [&](const auto& m) { return m.contains(pc); });
      if (module == loaded.end()) continue;
      const auto [it, added] = index.emplace(module->key, std::uint32_t(merged.modules.size()));
      if (added) merged.modules.push_back(module->key);
      const Row row{it->second, pc - module->base, descriptor & ~mask(a32_)};
      if (seen.insert(row).second) fresh.push_back(row);
    }
    std::reverse(fresh.begin(), fresh.end());
    std::vector<Row> rows;
    rows.reserve(merged.rows.size() + fresh.size());
    for (const auto& row : merged.rows) if (!seen.count(row)) rows.push_back(row);
    rows.insert(rows.end(), fresh.begin(), fresh.end());
    if (rows.size() > limit) rows.erase(rows.begin(), rows.end() - std::ptrdiff_t(limit));
    merged.rows = std::move(rows);
    const auto bytes = serialize(merged);
    if (bytes.size() > file_limit) return;
    std::uintmax_t total = 0;
    unsigned files = 0;
    for (const auto& e : std::filesystem::directory_iterator(file_.parent_path())) {
      if (++files > 1024 || e.is_symlink()) return;
      if (e.is_regular_file() && e.path() != file_) total += e.file_size();
    }
    if (total + bytes.size() > directory_limit) return;
    // A unique temporary name, then an atomic replace of the hint file.
    auto temporary = file_;
    temporary += "." + random_account_key() + ".tmp";
    try {
      {
        std::ofstream out(paths::io_path(temporary), std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), std::streamsize(bytes.size()));
        out.flush();
        if (!out) throw std::runtime_error("hint write failed");
      }
      paths::replace_file(temporary, file_);
    } catch (...) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
    }
  } catch (...) {}
}
} // namespace venture
