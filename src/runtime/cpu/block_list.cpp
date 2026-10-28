#include "cpu/block_list.h"
#include "common/environment.h"
#include "common/json.h"
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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace venture {
namespace {
constexpr std::size_t limit = 32768, file_limit = 8 * 1024 * 1024;
constexpr std::uintmax_t directory_limit = 64 * 1024 * 1024;
thread_local JitBlockList* recording = nullptr;
std::mutex files_mutex;
std::atomic<void (*)()> snapshot_observer{nullptr};
struct Module {
  GuestAddr base;
  std::string hash;
  std::vector<std::pair<GuestAddr, GuestAddr>> executable;
  bool contains(GuestAddr pc) const {
    return std::any_of(executable.begin(), executable.end(), [&](auto r) { return pc >= r.first && pc < r.second; });
  }
};
// `wait` false: give up (nullopt) while another thread owns the loader. A
// thread's first call may start while a constructor on another thread holds
// the loader and waits for it; real thread start never takes the loader lock.
std::optional<std::vector<Module>> modules(bool a32, bool wait) {
  if (auto observer = snapshot_observer.load()) observer();
  // Cache file hashes, not mapped bytes: mappings may contain relocations.
  static std::map<std::pair<std::filesystem::path, std::filesystem::file_time_type>, std::string> hashes;
  static std::mutex hashes_mutex;
  std::vector<Module> result;
  const auto visit = [&](const Library& lib) {
    // The linker also visits synthesized images (the HLE unwind image), which have no file.
    const auto* elf_image = dynamic_cast<const elf::Image*>(&lib);
    if (!elf_image || (elf_image->isa() == GuestIsa::a32) != a32) return;
    const auto& image = *elf_image;
    const auto& path = image.source_file();
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > 512 * 1024 * 1024) return;
    const auto time = std::filesystem::last_write_time(path, error);
    if (error) return;
    auto key = std::pair{path, time};
    // The linker snapshot precedes this lock. No hash-cache owner enters the linker.
    std::lock_guard hash_lock(hashes_mutex);
    auto it = hashes.find(key);
    if (it == hashes.end()) {
      std::ifstream stream(path, std::ios::binary);
      std::string bytes(size, '\0');
      if (!stream.read(bytes.data(), bytes.size())) return;
      it = hashes.emplace(std::move(key), meta::sha256(bytes)).first;
      // Bound metadata even when an application repeatedly dlopens new files.
    }
    Module module{image.load_bias, it->second, {}};
    if (hashes.size() > 256) hashes.erase(hashes.begin());
    for (const auto& ph : image.headers) if (ph.type == 1 && (ph.flags & 1))
      module.executable.emplace_back(image.load_bias + ph.vaddr, image.load_bias + ph.vaddr + ph.memsz);
    result.push_back(std::move(module));
  };
  if (!wait) {
    if (!Linker::get().try_for_each_image(visit)) return std::nullopt;
  } else Linker::get().for_each_image(visit);
  return result;
}
Json read(const std::filesystem::path& file) {
  if (!std::filesystem::exists(file) || std::filesystem::file_size(file) > file_limit) return Json::array();
  std::ifstream stream(file, std::ios::binary);
  auto document = Json::parse(stream, nullptr, false);
  if (document.is_discarded() || !document.is_object() || document.value("version", 0) != 1) return Json::array();
  auto rows = document.value("blocks", Json::array());
  return rows.is_array() && rows.size() <= limit ? rows : Json::array();
}
std::uint64_t mask(bool a32) { return a32 ? 0xffffffffull : 0x00ffffffffffffffull; }
// One background writer: retiring guest threads only queue their hints.
// Never destroyed, so process exit never joins it; hints still queued at exit
// are dropped (the cache is optional).
struct Writer {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<JitBlockList> queue;
  bool started = false, busy = false;
  void push(JitBlockList&& list) {
    std::lock_guard lock(mutex);
    if (queue.size() >= 64) return; // Bounded; a dropped list only loses hints.
    if (!started) { std::thread([this] { run(); }).detach(); started = true; }
    queue.push_back(std::move(list));
    changed.notify_all();
  }
  void run() {
    std::unique_lock lock(mutex);
    for (;;) {
      changed.wait(lock, [&] { return !queue.empty(); });
      auto list = std::move(queue.front());
      queue.pop_front(); busy = true;
      lock.unlock(); list.save(); lock.lock();
      busy = false; changed.notify_all();
    }
  }
  void flush() {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return queue.empty() && !busy; });
  }
};
Writer& writer() { static auto* instance = new Writer; return *instance; }
}
void JitBlockList::retire(JitBlockList&& list) noexcept {
  if (list.file_.empty() || list.descriptors_.empty()) return;
  try { writer().push(std::move(list)); } catch (...) {}
}
void JitBlockList::flush() noexcept {
  try { writer().flush(); } catch (...) {}
}
std::mutex& JitBlockList::persistence_mutex_for_test() { return files_mutex; }
void JitBlockList::snapshot_observer_for_test(void (*observer)()) { snapshot_observer.store(observer); }
bool JitBlockList::enabled() {
  static const bool value = [] { const auto* e = environment::getenv("VENTURE_JIT_PRETRANSLATE"); return e && std::string_view(e) == "1"; }();
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
  if (!prepared_ || file_.empty()) return;
  try {
    if (descriptors_.size() < limit) descriptors_.push_back(descriptor);
    else { descriptors_[next_] = descriptor; next_ = (next_ + 1) % limit; }
  } catch (...) {}
}
void JitBlockList::prepare(GuestAddr entry, bool a32, const std::function<bool(std::uint64_t)>& compile) {
  if (prepared_ || !enabled()) return;
  try {
    const auto snapshot = modules(a32, false);
    if (!snapshot) return; // Loader busy: no hints now; a later top-level call retries.
    prepared_ = true; a32_ = a32;
    const auto& loaded = *snapshot;
    std::unique_lock lock(files_mutex);
    const auto code_entry = a32 ? entry & ~GuestAddr{1} : entry;
    const auto module = std::find_if(loaded.begin(), loaded.end(), [&](const auto& m) { return m.contains(code_entry); });
    if (module == loaded.end()) return; // Anonymous code is never persisted.
    auto directory = process_config().cache_dir / "jit-block-lists";
    if (const auto* override_dir = environment::getenv("VENTURE_JIT_BLOCK_LIST_DIR")) directory = paths::from_utf8(override_dir);
    if (directory.empty()) return;
    file_ = directory / (module->hash + "-" + std::to_string(entry - module->base) + (a32 ? "-32.vbl" : "-64.vbl"));
    descriptors_.reserve(limit);
    const auto rows = read(file_);
    lock.unlock();
    const auto start = std::chrono::steady_clock::now();
    unsigned warmed = 0;
    // No recording of speculative work. Failed/missing hints use normal Run.
    Scope scope(nullptr);
    for (const auto& row : rows) {
      if (!row.is_array() || row.size() != 3 || !row[0].is_string() || !row[1].is_number_unsigned() || !row[2].is_number_unsigned()) continue;
      const auto offset = row[1].get<std::uint64_t>(), mode = row[2].get<std::uint64_t>();
      if (mode & mask(a32)) continue;
      const auto& hash = row[0].get_ref<const std::string&>();
      const auto target = std::find_if(loaded.begin(), loaded.end(), [&](const auto& m) { return m.hash == hash; });
      if (target == loaded.end() || offset > mask(a32) - target->base || !target->contains(target->base + offset)) continue;
      if (!compile((target->base + offset) | mode)) break;
      ++warmed;
      if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(250)) break;
    }
    log::info("jit", "Pretranslated {} descriptor hints before guest entry", warmed);
  } catch (...) { prepared_ = true; file_.clear(); } // Optional cache failure must not prevent launch.
}
void JitBlockList::save() noexcept {
  if (file_.empty() || descriptors_.empty()) return;
  try {
    const auto loaded = *modules(a32_, true);
    std::lock_guard lock(files_mutex);
    // Other processes share the directory: merging, the size budget and the
    // replacement form one critical section under a cross-process lock, so no
    // writer loses another's update or exceeds the budget alongside it.
    std::filesystem::create_directories(file_.parent_path());
    FileLock directory_lock(file_.parent_path() / "persist.lock");
    // Every writer holds this lock until its replace, so a temporary file seen
    // here was left by a process that exited mid-save; it would otherwise count
    // against the directory budget forever.
    for (const auto& e : std::filesystem::directory_iterator(file_.parent_path())) {
      std::error_code ignored;
      if (!e.is_symlink() && e.is_regular_file() && e.path().extension() == ".tmp") std::filesystem::remove(e.path(), ignored);
    }
    // Oldest first. Refresh an existing hint on demand and evict the least
    // recently observed hint, so a full file can adapt to a different scene.
    std::list<std::pair<std::string, Json>> recent;
    std::map<std::string, decltype(recent)::iterator> unique;
    const auto remember = [&](Json row) {
      auto key = row.dump();
      if (auto it = unique.find(key); it != unique.end()) {
        recent.splice(recent.end(), recent, it->second);
        return;
      }
      recent.emplace_back(std::move(key), std::move(row));
      unique.emplace(recent.back().first, std::prev(recent.end()));
      if (recent.size() > limit) {
        unique.erase(recent.front().first);
        recent.pop_front();
      }
    };
    for (auto& row : read(file_)) remember(std::move(row));
    for (std::size_t i = 0; i < descriptors_.size(); ++i) {
      const auto descriptor = descriptors_[(i + next_) % descriptors_.size()];
      const auto pc = descriptor & mask(a32_);
      const auto module = std::find_if(loaded.begin(), loaded.end(), [&](const auto& m) { return m.contains(pc); });
      if (module == loaded.end()) continue;
      Json row = Json::array({module->hash, pc - module->base, descriptor & ~mask(a32_)});
      remember(std::move(row));
    }
    Json rows = Json::array();
    for (auto& entry : recent) rows.push_back(std::move(entry.second));
    const auto bytes = Json{{"version", 1}, {"blocks", rows}}.dump();
    if (bytes.size() > file_limit) return;
    std::uintmax_t total = 0;
    unsigned files = 0;
    for (const auto& e : std::filesystem::directory_iterator(file_.parent_path())) {
      if (++files > 256 || e.is_symlink()) return;
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
