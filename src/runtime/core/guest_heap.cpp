#include "core/guest_heap.h"
#include "core/os_quit.h"
#include "core/thread.h"
#include "common/environment.h"
#include "core/low_address_space.h"
#include "common/log.h"
#include "platform/memory.h"
#include <mimalloc.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
// mimalloc's static library registers its thread-exit callback with MSVC
// section pragmas (prim/windows/prim.c), which clang ignores for MinGW: the
// callback lands outside the TLS callback array and _mi_thread_done never
// runs, so every exited host thread kept its mimalloc thread data and
// segments. .CRT$XLY sorts after the C runtime's callback, so this runs after
// thread_local destructors (the guest heap's among them), as mimalloc's own
// "post" callback would.
namespace {
void __stdcall mimalloc_thread_exit(void*, unsigned long reason, void*) {
  constexpr unsigned long thread_detach = 3;  // DLL_THREAD_DETACH
  if (reason == thread_detach && !venture::os_quit::requested()) mi_thread_done();
}
}  // namespace
extern "C" __attribute__((section(".CRT$XLY"), used)) void (__stdcall* const venture_mimalloc_thread_exit)(
    void*, unsigned long, void*) = mimalloc_thread_exit;
#endif

namespace venture::guest_heap {
Allocator allocator{mi_malloc_aligned, mi_calloc_aligned, mi_realloc_aligned, mi_free};
void select32() {
  allocator = {[](std::uint64_t n, std::uint64_t alignment) { return allocate(n, alignment); },
               [](std::uint64_t count, std::uint64_t n, std::uint64_t alignment) -> void* {
                 if (n && count > UINT64_MAX / n) return nullptr;
                 return allocate(count * n, alignment, true);
               },
               [](void* p, std::uint64_t n, std::uint64_t) { return reallocate(p, n); }, deallocate};
}
namespace {
std::atomic<GuestAddr> arena64_begin{0}, arena64_end{0};
}
std::pair<GuestAddr, GuestAddr> arena64() { return {arena64_begin.load(), arena64_end.load()}; }
void select64() {
  static std::once_flag once;
  std::call_once(once, [] {
    // Address space only: mimalloc commits and purges its own pages. First-fit
    // arena search (arena.c) keeps live memory at the low end.
    constexpr std::uint64_t window = 4ull << 30, size = 32ull << 30;
    const auto p = platform::reserve(0, size + window, false);
    if (!p) {
      log::warn("memory", "A64 guest heap reservation failed; mimalloc reserves OS memory itself");
      return;
    }
    const auto start = (p + window - 1) & ~(window - 1);
    if (!mi_manage_os_memory(g2h(start), size, false, false, true, -1)) {
      platform::release(p, size + window);
      log::warn("memory", "A64 guest heap arena rejected; mimalloc reserves OS memory itself");
      return;
    }
    mi_option_set(mi_option_arena_reserve, 0);
    mi_option_set(mi_option_disallow_os_alloc, 1);
    arena64_begin = start;
    arena64_end = start + size;
    log::info("memory", "A64 guest heap arena [{:#x}, {:#x})", start, start + size);
  });
}
namespace {
constexpr std::uint64_t arena_size = 1ull << 30, segment_alignment = 64ull << 20;
struct Arena {
  std::once_flag once;
  mi_arena_id_t id = 0;
  std::atomic<GuestAddr> base{0};
  // Only live allocation pages are accessible. Mimalloc can decommit empty
  // arena segments without notifying AddressSpace; advertising the whole arena
  // as RW would make a fault callback dereference an uncommitted page.
  std::array<std::atomic<std::uint32_t>, arena_size / 4096> live{};
  // One bit per 16-byte block start, set while that block is allocated. A
  // free takes the bit, so a second free of the same block (on any thread)
  // never reaches mimalloc. Untouched words stay uncommitted zero pages.
  std::array<std::atomic<std::uint64_t>, arena_size / 16 / 64> blocks{};
};
Arena heap_arena;
Arena& arena() { return heap_arena; }
struct Heap {
  mi_heap_t* value = nullptr;
  ~Heap() {
    if (value && !os_quit::requested()) mi_heap_delete(value);
  }
};
thread_local Heap heap;
mi_heap_t* current() {
  auto& a = arena();
  std::call_once(a.once, [&] {
    // Real private reservation inside the placeholder arena, initially
    // uncommitted. Mimalloc owns commit/decommit of its 4 KiB pages.
    auto p = low32::allocate(arena_size, kProtNone, "[A32 mimalloc arena]", segment_alignment);
    if (!p) throw std::bad_alloc();
    if (!mi_manage_os_memory_ex(g2h(p), arena_size, false, false, true, -1, true, &a.id)) {
      low32::free(p);
      throw std::bad_alloc();
    }
    a.base.store(p, std::memory_order_release);
    low32::publish_heap(p);
    log::info("memory", "A32 mimalloc exclusive arena [{:#x}, {:#x}), id {}", p, p + arena_size, a.id);
  });
  if (!heap.value) heap.value = mi_heap_new_in_arena(a.id);
  return heap.value;
}
void pages(const void* p, std::uint64_t n, bool add) {
  if (!p || !n) return;
  auto& a = arena();
  auto base = a.base.load(std::memory_order_acquire);
  auto begin = (h2g(p) - base) / 4096, end = (h2g(p) + n - 1 - base) / 4096;
  if (h2g(p) < base || end >= a.live.size()) throw std::runtime_error("mimalloc escaped exclusive A32 arena");
  for (auto i = begin; i <= end; ++i) {
    if (add)
      a.live[i].fetch_add(1, std::memory_order_release);
    else
      a.live[i].fetch_sub(1, std::memory_order_release);
  }
}
// VENTURE_HEAP_CHECK=1: every A32 block's owner and caller, so a free of a
// block that is not live is reported (and skipped) instead of corrupting a
// mimalloc free list. mimalloc's own double-free check sees only frees on the
// allocating thread.
struct Use {
  std::int32_t tid = -1;
  GuestAddr caller = 0;
};
struct Record {
  Use allocated, freed;
  bool live = false;
};
struct Check {
  bool enabled = [] {
    const char* v = environment::getenv("VENTURE_HEAP_CHECK");
    return v && *v && *v != '0';
  }();
  std::mutex mutex;
  std::unordered_map<GuestAddr, Record> blocks;
};
Check& check() {
  static Check c;
  return c;
}
Use use() {
  auto* t = GuestThread::current();
  if (!t) return {};
  if (!t->has_cpu()) return {t->tid(), 0};
  return {t->tid(), static_cast<GuestAddr>(t->cpu().x(14))};
}
void note_allocated(const void* p) {
  auto& c = check();
  std::lock_guard lock(c.mutex);
  auto& r = c.blocks[h2g(p)];
  r = {use(), {}, true};
}
// False when the block is not live: the free is reported and must be skipped.
bool note_freed(const void* p) {
  auto& c = check();
  const Use now = use();
  std::lock_guard lock(c.mutex);
  auto it = c.blocks.find(h2g(p));
  if (it == c.blocks.end()) {
    log::error("memory", "Heap check: free of unknown block {:#x} by tid {} (caller {:#x}); skipped", h2g(p),
               now.tid, now.caller);
    return false;
  }
  auto& r = it->second;
  if (!r.live) {
    log::error("memory",
               "Heap check: double free of {:#x} by tid {} (caller {:#x}); first freed by tid {} (caller {:#x}), "
               "allocated by tid {} (caller {:#x}); skipped",
               h2g(p), now.tid, now.caller, r.freed.tid, r.freed.caller, r.allocated.tid, r.allocated.caller);
    return false;
  }
  r.live = false;
  r.freed = now;
  return true;
}
std::pair<std::atomic<std::uint64_t>&, std::uint64_t> block_bit(const void* p) {
  auto& a = arena();
  const auto offset = (h2g(p) - a.base.load(std::memory_order_acquire)) / 16;
  return {a.blocks[offset / 64], std::uint64_t(1) << (offset % 64)};
}
void mark_allocated(const void* p) {
  auto [word, bit] = block_bit(p);
  word.fetch_or(bit, std::memory_order_relaxed);
}
// False when the block is not allocated. mimalloc frees on the allocating
// thread onto a local list and from other threads onto a shared one; the
// same block on both makes a cycle that hangs the heap's teardown and
// corrupts later allocations. Android's allocators tolerate or abort on a
// double free; a guest that does one keeps running here, as on its device.
bool take_allocated(const void* p) {
  auto [word, bit] = block_bit(p);
  if (word.fetch_and(~bit, std::memory_order_relaxed) & bit) return true;
  static std::atomic<int> reports{0};
  if (reports.fetch_add(1, std::memory_order_relaxed) < 16) {
    const Use now = use();
    log::warn("memory", "Guest double free of {:#x} by tid {} (caller {:#x}) ignored", h2g(p), now.tid, now.caller);
  }
  return false;
}
}  // namespace
void* allocate(std::uint64_t n, std::uint64_t alignment, bool zero) {
  if (n >= arena_size || !alignment || (alignment & (alignment - 1)) || alignment > segment_alignment)
    return nullptr;
  mi_heap_t* h;
  try {
    h = current();
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
  if (!h) return nullptr;
  // mimalloc v2.2.4 arena.c:_mi_arena_alloc_aligned explicitly returns ENOMEM
  // for a requested arena id before its OS fallback. No global options needed.
  auto* p = zero ? mi_heap_zalloc_aligned(h, n, alignment) : mi_heap_malloc_aligned(h, n, alignment);
  if (p) {
    h2g32(p);
    pages(p, mi_usable_size(p), true);
    mark_allocated(p);
    if (check().enabled) note_allocated(p);
  }
  return p;
}
void deallocate(void* p) {
  if (!p) return;
  if (!contains(h2g(p))) throw std::invalid_argument("free of non-A32-heap address");
  if (check().enabled && !note_freed(p)) return;
  if (!take_allocated(p)) return;
  pages(p, mi_usable_size(p), false);
  mi_free(p);
}
std::uint64_t usable_size(const void* p) {
  if (!p) return 0;
  if (!contains(h2g(p))) throw std::invalid_argument("non-A32-heap address");
  return mi_usable_size(p);
}
void* reallocate(void* p, std::uint64_t n) {
  if (!p) return allocate(n);
  if (!n) {
    deallocate(p);
    return nullptr;
  }
  auto old = usable_size(p);
  // Also permits realloc on another guest thread without using the old heap.
  auto* out = allocate(n);
  if (out) {
    std::memcpy(out, p, std::min(n, old));
    deallocate(p);
  }
  return out;
}
void ThreadHeapDeleter::operator()(mi_heap_s* value) const noexcept {
  if (!os_quit::requested()) mi_heap_delete(value);
}
DetachedThreadHeap detach_thread() { return DetachedThreadHeap(std::exchange(heap.value, nullptr)); }
void release_thread() { auto retired = detach_thread(); }
void prepare_thread() {
  if (!current()) throw std::bad_alloc();
}
bool contains(GuestAddr p) {
  auto base = arena().base.load(std::memory_order_acquire);
  return base && p >= base && p - base < arena_size;
}
bool accessible(GuestAddr p, std::uint64_t n) {
  auto& a = arena();
  auto base = a.base.load(std::memory_order_acquire);
  if (!n || p + n < p) return false;
  if (!base || p >= base + arena_size || p + n <= base) return true;
  auto stop = std::min(p + n, base + arena_size);
  p = std::max(p, base);
  n = stop - p;
  auto begin = (p - base) / 4096, end = (p + n - 1 - base) / 4096;
  for (auto i = begin; i <= end; ++i)
    if (!a.live[i].load(std::memory_order_acquire)) return false;
  return true;
}
}  // namespace venture::guest_heap
