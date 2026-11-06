#include "cpu/pretranslate.h"
#include "cpu/cpu.h"
#include "cpu/portable_cache.h"
#include <dynarmic/backend/x64/portable_block.h>
#include "common/environment.h"
#include "common/log.h"
#include "core/address_space.h"
#include "core/elf.h"
#include "core/linker.h"
#include "core/memory_pressure.h"
#include "core/process.h"
#include "core/wait.h"
#include "platform/memory.h"
#include "platform/thread_priority.h"
#include <algorithm>
#include <array>
#include <utility>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <boost/unordered/unordered_flat_set.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

namespace venture::pretranslate {
namespace {
constexpr std::uint64_t kPcMask = 0x00ffffffffffffffull;
constexpr unsigned kLevels = 64;
// Successors this many links from executed code come before the function
// entries near executed code; deeper ones after them. Entries far from any
// executed code come last.
constexpr unsigned kNearRootLevel = 6, kFarRootLevel = kLevels - 1;
constexpr unsigned kWindowShift = 16;  // "near": the same 64 KiB of code
constexpr std::size_t kReachedLimit = 1 << 20;
// The shortest time between two explored marks in the cache file.
constexpr std::int64_t kMarkInterval = 5'000'000'000;

struct Item {
  std::uint64_t value;  // a descriptor, or a PC for a root (the current mode is added)
  bool root;
};

struct State {
  PortableBlockCache* cache = nullptr;
  // Guest threads append to their shard (uncontended in practice); workers take them all.
  struct Shard {
    std::mutex mutex;
    std::vector<std::uint64_t> reached;
  };
  std::array<Shard, 64> shards;
  std::atomic<unsigned> next_shard{0};
  std::atomic<bool> reached_any{false};
  // Workers only.
  std::mutex mutex;
  std::condition_variable wake;
  std::array<std::deque<Item>, kLevels> levels;
  std::size_t queued = 0;
  boost::unordered_flat_set<std::uint64_t> seen;     // descriptors queued once
  boost::unordered_flat_set<std::uint64_t> done;     // descriptors whose successors are queued
  boost::unordered_flat_set<std::uint64_t> windows;  // pc >> kWindowShift of executed code
  boost::unordered_flat_set<std::uint64_t> images;   // load biases scanned
  std::vector<std::uint64_t> roots;                  // sorted function entries
  boost::unordered_flat_set<std::uint64_t> roots_queued;
  std::unordered_map<std::uint64_t, std::uint64_t> modes;  // descriptor mode bits -> count
  std::uint64_t mode = 0;
  std::int64_t scanned_ns = 0, marked_ns = 0;
  std::size_t marked_done = 0;  // done.size() at the last explored mark
  std::atomic<bool> stopping{false};
  std::atomic<bool> paused{false};
  std::atomic<unsigned> busy{0}, waiting{0};
  std::atomic<std::uint64_t> translated{0}, failed{0}, explored{0}, translate_ns{0}, writer_wait_ns{0};
};

State& state() {
  static auto* value = new State;  // never destroyed: workers outlive static destruction
  return *value;
}

bool enabled() {
  static const bool value = [] {
    const auto* e = environment::getenv("VENTURE_JIT_AOT");
    if (e && std::string_view(e) == "0") return false;
    if (process_config().isa() != GuestIsa::a64) return false;
    auto* cache = PortableBlockCache::get();
    if (!cache || !cache->accepting()) return false;
    state().cache = cache;
    return true;
  }();
  return value;
}

thread_local bool worker_thread = false;

// Caller holds the mutex.
void push(State& s, Item item, unsigned level) {
  if (!item.root && (s.cache->explored(item.value) || !s.seen.insert(item.value).second)) return;
  s.levels[std::min(level, kLevels - 1)].push_back(item);
  ++s.queued;
}

// Caller holds the mutex: queue the entries in a window that just became near executed code.
void queue_window_roots(State& s, std::uint64_t window) {
  const auto begin = std::lower_bound(s.roots.begin(), s.roots.end(), window << kWindowShift);
  const auto end = std::lower_bound(begin, s.roots.end(), (window + 1) << kWindowShift);
  for (auto it = begin; it != end; ++it)
    if (s.roots_queued.insert(*it).second) push(s, {*it, true}, kNearRootLevel);
}

// A DWARF exception-header pointer (DW_EH_PE_*) at p, guest address `at`; false for
// encodings unwind tables do not use for code addresses.
bool read_encoded(const std::uint8_t*& p, const std::uint8_t* end, std::uint8_t encoding, GuestAddr at, GuestAddr base, std::uint64_t& value) {
  const auto fixed = [&](auto type) {
    using T = decltype(type);
    if (end - p < std::ptrdiff_t(sizeof(T))) return false;
    T x;
    std::memcpy(&x, p, sizeof(T));
    p += sizeof(T);
    value = std::uint64_t(x);  // signed types sign-extend
    return true;
  };
  const auto leb = [&](bool sign) {
    std::uint64_t v = 0;
    unsigned shift = 0;
    for (;;) {
      if (p == end || shift >= 64) return false;
      const std::uint8_t b = *p++;
      v |= std::uint64_t(b & 0x7f) << shift;
      shift += 7;
      if (!(b & 0x80)) {
        if (sign && shift < 64 && (b & 0x40)) v |= ~std::uint64_t(0) << shift;
        value = v;
        return true;
      }
    }
  };
  bool ok;
  switch (encoding & 0x0f) {
    case 0x00: case 0x04: ok = fixed(std::uint64_t{}); break;
    case 0x01: ok = leb(false); break;
    case 0x02: ok = fixed(std::uint16_t{}); break;
    case 0x03: ok = fixed(std::uint32_t{}); break;
    case 0x09: ok = leb(true); break;
    case 0x0a: ok = fixed(std::int16_t{}); break;
    case 0x0b: ok = fixed(std::int32_t{}); break;
    case 0x0c: ok = fixed(std::int64_t{}); break;
    default: return false;
  }
  if (!ok || (encoding & 0x80)) return false;
  switch (encoding & 0x70) {
    case 0x00: return true;
    case 0x10: value += at; return true;
    case 0x30: value += base; return true;
    default: return false;
  }
}

std::uint64_t read_uleb(const std::uint8_t*& p, const std::uint8_t* end) {
  std::uint64_t value = 0;
  read_encoded(p, end, 0x01, 0, 0, value);
  return value;
}

struct Segments {
  std::vector<std::pair<GuestAddr, GuestAddr>> code, all;  // [begin, end)
  bool code_at(GuestAddr pc) const {
    return std::any_of(code.begin(), code.end(), [&](const auto& r) { return pc >= r.first && pc < r.second; });
  }
};

// Every FDE's initial location in .eh_frame [begin, end), for an .eh_frame_hdr without
// its search table: one per function with unwind information.
void read_eh_frame(GuestAddr begin, GuestAddr end, const Segments& segments, std::vector<std::uint64_t>& out) {
  std::vector<std::uint8_t> data(std::min<std::uint64_t>(end - begin, 256ull << 20));
  if (data.empty() || !platform::read_memory(begin, data.data(), data.size())) return;
  boost::unordered_flat_map<std::size_t, std::uint8_t> encodings;  // CIE offset -> FDE pointer encoding
  std::size_t at = 0;
  while (data.size() - at >= 8) {
    std::uint32_t length, id;
    std::memcpy(&length, data.data() + at, 4);
    // The terminator, 64-bit DWARF (not in Android unwind tables) or the segment's end.
    if (length == 0 || length == 0xffffffffu || length > data.size() - at - 4) break;
    const std::size_t record = at, body = at + 4;
    at = body + length;
    if (length < 4) continue;
    std::memcpy(&id, data.data() + body, 4);
    const std::uint8_t* p = data.data() + body + 4;
    const std::uint8_t* stop = data.data() + at;
    if (id == 0) {
      // CIE: version, augmentation, alignment factors, return register, augmentation data.
      if (p == stop) continue;
      const std::uint8_t version = *p++;
      const auto* augmentation = p;
      while (p < stop && *p) ++p;
      if (p == stop) continue;
      const std::string_view aug(reinterpret_cast<const char*>(augmentation), std::size_t(p - augmentation));
      ++p;
      if (aug.find("eh") != std::string_view::npos) p += std::min<std::ptrdiff_t>(8, stop - p);
      std::uint64_t ignored;
      read_uleb(p, stop);
      read_encoded(p, stop, 0x09, 0, 0, ignored);
      if (version == 1) p += p < stop ? 1 : 0;
      else read_uleb(p, stop);
      std::uint8_t encoding = 0x00;
      bool known = aug.empty();
      if (!aug.empty() && aug[0] == 'z') {
        known = true;
        const std::uint64_t size = read_uleb(p, stop);
        if (size > std::uint64_t(stop - p)) continue;
        const std::uint8_t* aug_end = p + size;
        for (const char c : aug.substr(1)) {
          if (c == 'R' && p < aug_end) {
            encoding = *p++;
          } else if (c == 'P' && p < aug_end) {
            const std::uint8_t personality = *p++;
            known = read_encoded(p, aug_end, personality & 0x7f, 0, 0, ignored);
          } else if (c == 'L' && p < aug_end) {
            ++p;
          } else if (c != 'S' && c != 'B' && c != 'G') {
            known = false;
          }
          if (!known) break;
        }
      }
      if (known) encodings.insert_or_assign(record, encoding);
    } else if (id <= body) {
      // FDE: the CIE pointer counts back from its own field.
      const auto cie = encodings.find(body - id);
      std::uint64_t pc;
      if (cie != encodings.end() && read_encoded(p, stop, cie->second, begin + GuestAddr(p - data.data()), 0, pc) && !(pc & 3) &&
          segments.code_at(pc))
        out.push_back(pc);
    }
  }
}

// The function entries in an image's unwind information: .eh_frame_hdr's sorted
// table, or .eh_frame itself when the header has none. Returns why there are none, for the log.
const char* read_function_entries(GuestAddr hdr, std::uint64_t size, const Segments& segments, std::vector<std::uint64_t>& out) {
  std::uint8_t head[12]{};
  const auto head_size = std::min<std::uint64_t>(size, sizeof(head));
  if (size < 8 || !platform::read_memory(hdr, head, head_size)) return "unreadable header";
  if (head[0] != 1) return "unknown header version";
  // version 1; eh_frame_ptr pcrel|sdata4; fde_count udata4; table datarel|sdata4 (what lld and ld.bfd write).
  if (size >= 12 && head[1] == 0x1b && head[2] == 0x03 && head[3] == 0x3b) {
    std::uint32_t count;
    std::memcpy(&count, head + 8, 4);
    if (count > (size - 12) / 8) return "count beyond the header";
    std::vector<std::int32_t> table(std::size_t(count) * 2);
    if (!table.empty() && !platform::read_memory(hdr + 12, table.data(), table.size() * 4)) return "unreadable table";
    for (std::size_t i = 0; i < table.size(); i += 2) {
      const std::uint64_t pc = hdr + std::int64_t(table[i]);
      if (!(pc & 3) && segments.code_at(pc)) out.push_back(pc);
    }
    return count ? nullptr : "empty table";
  }
  const std::uint8_t* p = head + 4;
  std::uint64_t eh_frame;
  if (!read_encoded(p, head + head_size, head[1], hdr + 4, hdr, eh_frame)) return "unknown eh_frame_ptr encoding";
  for (const auto& [first, last] : segments.all) {
    if (eh_frame < first || eh_frame >= last) continue;
    const auto before = out.size();
    read_eh_frame(eh_frame, last, segments, out);
    return out.size() > before ? nullptr : "no FDEs in .eh_frame";
  }
  return ".eh_frame outside the image";
}

// Worker: adds the function entries of images loaded since the last scan.
void scan_images(State& s) {
  struct Found {
    std::uint64_t bias;
    std::vector<std::pair<GuestAddr, std::uint64_t>> headers;  // PT_GNU_EH_FRAME
    Segments segments;
  };
  std::vector<Found> found;
  {
    std::lock_guard lock(s.mutex);
    if (s.stopping) return;
  }
  Linker::get().try_for_each_image([&](const Library& lib) {
    const auto* image = dynamic_cast<const elf::Image*>(&lib);
    if (!image || image->isa() != GuestIsa::a64 || image->source_file().empty() || !image->relocated) return;
    auto& f = found.emplace_back();
    f.bias = image->load_bias;
    for (const auto& ph : image->headers) {
      const GuestAddr address = image->load_bias + ph.vaddr;
      if (ph.type == 0x6474e550) f.headers.emplace_back(address, ph.memsz);
      if (ph.type == 1 && ph.memsz) {
        f.segments.all.emplace_back(address, address + ph.memsz);
        if (ph.flags & 1) f.segments.code.emplace_back(address, address + ph.memsz);  // PF_X
      }
    }
  });
  for (const auto& [bias, headers, segments] : found) {
    {
      std::lock_guard lock(s.mutex);
      if (s.images.contains(bias)) continue;
    }
    std::vector<std::uint64_t> entries;
    const char* missing = headers.empty() ? "no PT_GNU_EH_FRAME" : nullptr;
    for (const auto& [hdr, size] : headers)
      if (const char* why = read_function_entries(hdr, size, segments, entries)) missing = why;
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    std::uint64_t code_bytes = 0;
    for (const auto& [first, last] : segments.code) code_bytes += last - first;
    if (!entries.empty()) log::info("jit", "Background translation: {} function entries in the image at {:#x}", entries.size(), bias);
    else if (code_bytes >= 1024 * 1024)
      log::info("jit", "Background translation: no function entries in the image at {:#x} ({} MB of code): {}", bias, code_bytes >> 20, missing ? missing : "?");
    std::lock_guard lock(s.mutex);
    if (!s.images.insert(bias).second) continue;
    std::vector<std::uint64_t> merged;
    merged.reserve(s.roots.size() + entries.size());
    std::merge(s.roots.begin(), s.roots.end(), entries.begin(), entries.end(), std::back_inserter(merged));
    s.roots = std::move(merged);
    for (const auto pc : entries) {
      if (s.windows.contains(pc >> kWindowShift)) {
        if (s.roots_queued.insert(pc).second) push(s, {pc, true}, kNearRootLevel);
      } else {
        push(s, {pc, true}, kFarRootLevel);
      }
    }
  }
}

// Worker, mutex held: moves descriptors guest threads reached into level 0.
void take_reached(State& s) {
  s.reached_any.store(false, std::memory_order_relaxed);
  std::vector<std::uint64_t> taken, shard;
  for (auto& x : s.shards) {
    {
      std::lock_guard lock(x.mutex);
      shard.swap(x.reached);
    }
    taken.insert(taken.end(), shard.begin(), shard.end());
    shard.clear();
  }
  for (const auto d : taken) {
    if (const auto mode = d & ~kPcMask; ++s.modes[mode] > s.modes[s.mode]) s.mode = mode;
    if (s.windows.insert((d & kPcMask) >> kWindowShift).second) queue_window_roots(s, (d & kPcMask) >> kWindowShift);
    // An executed block is explored at level 0 even if it was queued deeper.
    if (s.done.contains(d) || s.cache->explored(d)) continue;
    s.seen.insert(d);
    s.levels[0].push_back({d, false});
    ++s.queued;
  }
}

void work(unsigned index) {
  worker_thread = true;
  platform::lower_background_thread_priority();
  auto& s = state();
  std::unique_ptr<Cpu> cpu;
  try { cpu = cpu_backend().create_cpu(); } catch (const std::exception& e) {
    log::warn("jit", "Background translation worker unavailable: {}", e.what());
    return;
  }
  std::vector<std::uint8_t> bytes;
  Dynarmic::Backend::X64::PortableBlock block;
  auto reported = std::chrono::steady_clock::now();
  for (;;) {
    Item item{};
    unsigned level = 0;
    {
      std::unique_lock lock(s.mutex);
      for (;;) {
        if (s.stopping) return;
        if (s.reached_any.load(std::memory_order_relaxed)) take_reached(s);
        const auto now = monotonic_ns();
        if (index == 0 && now - s.scanned_ns > 2'000'000'000) {
          s.scanned_ns = now;
          lock.unlock();
          scan_images(s);
          lock.lock();
          continue;
        }
        // Little memory left: translate nothing ahead (each translation adds cache records and
        // exploration state); guest threads still translate what they reach.
        if (memory_pressure::level() != memory_pressure::Level::normal) {
          if (index == 0 && !s.paused.exchange(true)) log::info("jit", "Background translation paused: memory pressure");
          s.waiting.fetch_add(1);
          s.wake.wait_for(lock, std::chrono::seconds(1));
          s.waiting.fetch_sub(1);
          continue;
        }
        if (index == 0 && s.paused.exchange(false)) log::info("jit", "Background translation resumed");
        // The cache writer is behind (a first launch translates faster than it writes): wait
        // for it below half the unwritten limit rather than have records dropped or held.
        if (s.queued && s.cache->unwritten_bytes() > PortableBlockCache::kUnwrittenLimit / 2) {
          s.waiting.fetch_add(1);
          s.wake.wait_for(lock, std::chrono::milliseconds(10));
          s.waiting.fetch_sub(1);
          s.writer_wait_ns.fetch_add(10'000'000, std::memory_order_relaxed);
          continue;
        }
        if (s.queued) break;
        // Nothing queued or in progress: every block in `done` has its successors explored.
        // Recorded in the cache file, so later runs skip reading those blocks again (an
        // unchanged run would otherwise walk the whole file to find the few new blocks).
        if (index == 0 && s.done.size() != s.marked_done && !s.busy.load() && !s.reached_any.load(std::memory_order_relaxed) &&
            now - s.marked_ns > kMarkInterval) {
          s.marked_ns = now;
          s.marked_done = s.done.size();
          s.cache->flush();  // the last translations' records come before the mark
          s.cache->mark_explored([&](std::uint64_t d) { return s.done.contains(d); });
          continue;
        }
        s.waiting.fetch_add(1);
        s.wake.wait_for(lock, std::chrono::milliseconds(200));
        s.waiting.fetch_sub(1);
      }
      while (s.levels[level].empty()) ++level;
      item = s.levels[level].front();
      s.levels[level].pop_front();
      --s.queued;
      if (item.root) item.value = (item.value & kPcMask) | s.mode;
      if (item.root && (s.cache->explored(item.value) || !s.seen.insert(item.value).second)) continue;
      s.busy.fetch_add(1);
    }
    struct Busy { State& s; ~Busy() { s.busy.fetch_sub(1); } } busy{s};
    const auto descriptor = item.value;
    if (!s.cache->contains(descriptor)) {
      if (!s.cache->accepting() || !AddressSpace::get().is_guest_executable(descriptor & kPcMask)) continue;
      const auto begin = monotonic_ns();
      const bool ok = cpu->pretranslate(descriptor);
      s.translate_ns.fetch_add(std::uint64_t(monotonic_ns() - begin), std::memory_order_relaxed);
      (ok ? s.translated : s.failed).fetch_add(1, std::memory_order_relaxed);
      if (!ok) continue;
    }
    if (!s.cache->copy(descriptor, bytes) || !Dynarmic::Backend::X64::DeserializePortableBlock(bytes, block)) continue;
    s.explored.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard lock(s.mutex);
      if (!s.done.insert(descriptor).second) continue;
      for (const auto& link : block.links) push(s, {link.target, false}, level + 1);
    }
    if (index == 0) {
      if (const auto now = std::chrono::steady_clock::now(); now - reported > std::chrono::seconds(10)) {
        reported = now;
        std::size_t queued, state_bytes;
        {
          std::lock_guard lock(s.mutex);
          queued = s.queued;
          // Exploration state held in memory (flat containers: bucket_count is their slot count).
          state_bytes = (s.seen.bucket_count() + s.done.bucket_count() + s.windows.bucket_count() + s.roots_queued.bucket_count()) * 8 +
                        s.roots.capacity() * 8 + queued * sizeof(Item);
        }
        log::info("jit", "Background translation: {} translated ({} ms), {} failed, {} explored, {} queued, state {} MiB, {} ms waiting for the cache writer",
                  s.translated.load(), s.translate_ns.load() / 1'000'000, s.failed.load(), s.explored.load(), queued,
                  state_bytes >> 20, s.writer_wait_ns.load() / 1'000'000);
      }
    }
  }
}

void start() {
  // A third of the logical processors, at most 8: at background priority they take idle time, and a first
  // launch's blocks are ready sooner (8 instead of 2 on 24 threads: guest translations after the first eye
  // fell from 56k to 7.7k in Batman, hitch excess in North Star from 4.6 s to 2.0 s). Each costs a code cache.
  unsigned workers = std::clamp(std::thread::hardware_concurrency() / 3, 1u, 8u);
  if (const auto* e = environment::getenv("VENTURE_JIT_AOT_WORKERS")) workers = unsigned(std::clamp(std::atoi(e), 1, 16));
  for (unsigned i = 0; i < workers; ++i) std::thread([i] { work(i); }).detach();
  log::info("jit", "Background translation: {} workers", workers);
}
} // namespace

namespace {
// A guest thread's reached blocks, handed to the workers 64 at a time (or when it waits):
// one shard lock per batch instead of one per installed block.
struct Pending {
  std::array<std::uint64_t, 64> items;
  unsigned count = 0;
};
thread_local Pending pending;

void hand_over() noexcept {
  auto& s = state();
  thread_local auto& shard = s.shards[s.next_shard.fetch_add(1, std::memory_order_relaxed) % s.shards.size()];
  const auto count = std::exchange(pending.count, 0u);
  try {
    static std::once_flag started;
    std::call_once(started, start);
    {
      std::lock_guard lock(shard.mutex);
      const auto room = kReachedLimit / s.shards.size() - std::min(shard.reached.size(), kReachedLimit / s.shards.size());
      shard.reached.insert(shard.reached.end(), pending.items.begin(), pending.items.begin() + std::min<std::size_t>(count, room));
    }
    // Read first: an unconditional exchange would keep the flag's cache line moving between cores.
    if (!s.reached_any.load(std::memory_order_relaxed) && !s.reached_any.exchange(true, std::memory_order_relaxed) &&
        s.waiting.load(std::memory_order_relaxed))
      s.wake.notify_one();
  } catch (...) {}
}
} // namespace

void reached(std::uint64_t descriptor) noexcept {
  if (worker_thread || !enabled()) return;
  pending.items[pending.count++] = descriptor;
  if (pending.count == pending.items.size()) hand_over();
}

void flush_reached() noexcept {
  if (pending.count) hand_over();
}

std::vector<std::uint64_t> detail::function_entries(std::uint64_t hdr, std::uint64_t size, const std::vector<std::pair<std::uint64_t, std::uint64_t>>& code,
                                                    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& segments) {
  std::vector<std::uint64_t> out;
  read_function_entries(hdr, size, Segments{code, segments}, out);
  return out;
}

void stop() noexcept {
  // No lock: exit may run while another thread holds one (the logger's, with a worker waiting on it).
  auto& s = state();
  s.stopping.store(true);
  s.wake.notify_all();
  // A block takes well under a millisecond; a stuck worker never delays exit for long.
  for (int i = 0; i < 100 && s.busy.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
}
} // namespace venture::pretranslate
