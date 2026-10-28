#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "hle/libc/libc_a.h"
#include "core/hle_runtime.h"
#include "platform/memory.h"
namespace venture::hle::libc {
[[noreturn]] void throw_memory_range_fault(GuestAddr address, bool write) {
  throw MemoryFault{address, write};
}
namespace {
template<GuestIsa Isa>
std::uint64_t native_for_isa(platform::NativeMemoryOp op, void* d, const void* s, std::uint64_t n, int value = 0) {
  // The platform leaf validates 64-bit overflow before entering the CRT. Only
  // A32 needs an additional guest-width check; keep it inline in that binding.
  if constexpr (Isa == GuestIsa::a32) {
    if (op != platform::NativeMemoryOp::find) check_memory_range32(h2g(d), n, op != platform::NativeMemoryOp::compare);
    if (op != platform::NativeMemoryOp::fill) check_memory_range32(h2g(s), n, false);
  }
  std::uint64_t result;
  platform::NativeMemoryFault fault;
  if (!platform::native_memory(op, h2g(d), h2g(s), n, value, result, fault))
    throw MemoryFault{fault.address, fault.write, fault.bus};
  return result;
}
std::uint64_t native(platform::NativeMemoryOp op, void* d, const void* s, std::uint64_t n, int value = 0) {
  return process_config().isa() == GuestIsa::a32 ? native_for_isa<GuestIsa::a32>(op, d, s, n, value)
                                              : native_for_isa<GuestIsa::a64>(op, d, s, n, value);
}
void copy_bytes(void* d, const void* s, std::uint64_t n) { native(platform::NativeMemoryOp::copy, d, s, n); }
template<class C> C load(const C* p) {
  // Pointer cells may be host temporaries owned by the ILP32 marshaler.
  if constexpr (!std::is_pointer_v<C>) check_memory_range(h2g(p), sizeof(C), false);
  C value;
  if (!platform::read_memory(h2g(p), &value, sizeof(value))) throw MemoryFault{h2g(p)};
  return value;
}
template<class C> void store(C* p, C value) {
  if constexpr (!std::is_pointer_v<C>) check_memory_range(h2g(p), sizeof(C), true);
  if (!platform::guarded_store(h2g(p), &value, sizeof(value))) throw MemoryFault{h2g(p), true};
}
std::uint64_t bytes(std::uint64_t n, std::uint64_t width, const void* p) {
  if (n > UINT64_MAX / width) throw MemoryFault{h2g(p)};
  return n * width;
}
template <class C, bool CheckGuestWidth = true>
std::uint64_t nlength(const C* s, std::uint64_t n) {
  std::uint64_t i = 0;
  while (i < n) {
    const auto address = h2g(s) + i * sizeof(C);
    const auto page_left = kGuestPageSize - (address & (kGuestPageSize - 1));
    std::array<C, 1024> block;
    const auto count = std::min<std::uint64_t>({n-i, block.size(), std::max<std::uint64_t>(1, page_left / sizeof(C))});
    if constexpr (CheckGuestWidth) check_memory_range(address, count*sizeof(C), false);
    if (!platform::read_memory(address, block.data(), count*sizeof(C))) throw hle::MemoryFault{address};
    for (std::uint64_t j = 0; j < count; ++j) if (!block[j]) return i+j;
    i += count;
  }
  return i;
}
template <class C>
std::uint64_t length(const C* s) { return nlength(s, UINT64_MAX / sizeof(C)); }
// Reads up to `limit` elements from s, stopping at the end of its guest page, so
// that every page read holds the next element a byte-wise loop would load and a
// fault reports that element. Comparisons and searches scan these blocks; the
// first block is short because most comparisons end within a few elements.
template <class C, std::size_t N>
std::uint64_t page_block(const C* s, std::array<C, N>& block, std::uint64_t limit) {
  const auto address = h2g(s);
  const auto page_left = kGuestPageSize - (address & (kGuestPageSize - 1));
  const auto count = std::min<std::uint64_t>({limit, N, std::max<std::uint64_t>(1, page_left / sizeof(C))});
  check_memory_range(address, count * sizeof(C), false);
  if (!platform::read_memory(address, block.data(), count * sizeof(C))) throw hle::MemoryFault{address};
  return count;
}
constexpr std::uint64_t first_block = 16;
template<class C> std::uint64_t length64(const C* s) { return nlength<C, false>(s, UINT64_MAX / sizeof(C)); }
template <class C>
C* copy(C* d, const C* s) {
  copy_bytes(d, s, bytes(length(s) + 1, sizeof(C), s));
  return d;
}
template <class C>
C* endcopy(C* d, const C* s) {
  auto n = length(s);
  copy_bytes(d, s, bytes(n + 1, sizeof(C), s));
  return d + n;
}
template <class C>
C* ncopy(C* d, const C* s, std::uint64_t n) {
  auto r = d;
  auto len = nlength(s, n);
  copy_bytes(d, s, bytes(len, sizeof(C), s));
  native(platform::NativeMemoryOp::fill, d + len, nullptr, bytes(n - len, sizeof(C), d), 0);
  return r;
}
template <class C>
C* endncopy(C* d, const C* s, std::uint64_t n) {
  auto len = nlength(s, n);
  ncopy(d, s, n);
  return d + len;
}
template <class C>
C* cat(C* d, const C* s) {
  copy(d + length(d), s);
  return d;
}
template <class C>
C* ncat(C* d, const C* s, std::uint64_t n) {
  auto tail = d + length(d);
  auto len = nlength(s, n);
  copy_bytes(tail, s, bytes(len, sizeof(C), s));
  store(tail + len, C{});
  return d;
}
template <class C>
int compare(const C* a, const C* b) {
  using U = std::make_unsigned_t<C>;
  std::array<C, 256> x, y;
  for (auto limit = first_block;; limit = x.size()) {
    const auto count = page_block(b, y, page_block(a, x, limit));
    for (std::uint64_t i = 0; i < count; ++i)
      if (x[i] != y[i] || !x[i]) return U(x[i]) > U(y[i]) ? 1 : U(x[i]) < U(y[i]) ? -1 : 0;
    a += count;
    b += count;
  }
}
template <class C>
int ncompare(const C* a, const C* b, std::uint64_t n) {
  using U = std::make_unsigned_t<C>;
  std::array<C, 256> x, y;
  for (auto limit = first_block; n; limit = x.size()) {
    const auto count = page_block(b, y, page_block(a, x, std::min(limit, n)));
    for (std::uint64_t i = 0; i < count; ++i) {
      if (x[i] != y[i]) return U(x[i]) > U(y[i]) ? 1 : -1;
      if (!x[i]) return 0;
    }
    a += count;
    b += count;
    n -= count;
  }
  return 0;
}
template <class C>
C* find(const C* s, std::int32_t c) {
  for (;; ++s) {
    if (load(s) == static_cast<C>(c)) return const_cast<C*>(s);
    if (!load(s)) return nullptr;
  }
}
template <class C>
C* rfind(const C* s, std::int32_t c) {
  C* r = nullptr;
  do {
    if (load(s) == static_cast<C>(c)) r = const_cast<C*>(s);
  } while (load(s++));
  return r;
}
template <class C>
std::uint64_t span(const C* s, const C* accept) {
  auto p = s;
  while (load(p) && find(accept, load(p))) ++p;
  return p - s;
}
template <class C>
std::uint64_t cspan(const C* s, const C* reject) {
  auto p = s;
  while (load(p) && !find(reject, load(p))) ++p;
  return p - s;
}
template <class C>
C* pbrk(const C* s, const C* accept) {
  auto p = s + cspan(s, accept);
  return load(p) ? const_cast<C*>(p) : nullptr;
}
template <class C>
C* search(const C* s, const C* sub) {
  if (!load(sub)) return const_cast<C*>(s);
  std::vector<C> needle(length(sub));
  check_memory_range(h2g(sub), needle.size() * sizeof(C), false);
  if (!platform::read_memory(h2g(sub), needle.data(), needle.size() * sizeof(C))) throw hle::MemoryFault{h2g(sub)};
  // The window holds the haystack from `base` through the last block read; it
  // keeps needle.size() - 1 elements between blocks, so no match is split.
  std::vector<C> window;
  std::uint64_t base = 0;
  std::array<C, 256> block;
  for (auto limit = first_block;; limit = block.size()) {
    const auto count = page_block(s + base + window.size(), block, limit);
    const auto end = std::find(block.begin(), block.begin() + count, C{});
    window.insert(window.end(), block.begin(), end);
    const auto hit = std::search(window.begin(), window.end(), needle.begin(), needle.end());
    if (hit != window.end()) return const_cast<C*>(s + base + (hit - window.begin()));
    if (end != block.begin() + count) return nullptr;
    if (window.size() >= needle.size()) {
      const auto drop = window.size() - (needle.size() - 1);
      window.erase(window.begin(), window.begin() + drop);
      base += drop;
    }
  }
}
template <class C>
C* tokenize(C* s, const C* delim, C** state) {
  if (!s) s = load(state);
  if (!s) return nullptr;
  s += span(s, delim);
  if (!load(s)) {
    store(state, s);
    return nullptr;
  }
  auto end = s + cspan(s, delim);
  if (load(end)) store(end++, C{});
  store(state, end);
  return s;
}
template <class C>
C* duplicate(const C* s) {
  auto n = length(s) + 1;
  auto p = static_cast<C*>(allocate(n * sizeof(C)));
  if (p) copy_bytes(p, s, bytes(n, sizeof(C), s));
  return p;
}
template <class C>
std::uint64_t transform(C* d, const C* s, std::uint64_t n) {
  auto len = length(s);
  if (n) ncopy(d, s, n);
  return len;
}
template <class C>
int collate_l(const C* a, const C* b, GuestAddr) {
  return compare(a, b);
}
template <class C>
std::uint64_t transform_l(C* a, const C* b, std::uint64_t n, GuestAddr) {
  return transform(a, b, n);
}
char* nduplicate(const char* s, std::uint64_t n) {
  auto len = nlength(s, n);
  auto p = static_cast<char*>(allocate(len + 1));
  if (p) {
    copy_bytes(p, s, len);
    store(p + len, char{});
  }
  return p;
}
char* strtok_impl(char* s, const char* d) {
  thread_local char* state = nullptr;
  return tokenize(s, d, &state);
}
char* strsep_impl(char** p, const char* delim) {
  auto start = load(p);
  if (!start) return nullptr;
  auto end = start + cspan(start, delim);
  store(p, load(end) ? end + 1 : nullptr);
  if (load(end)) store(end, char{});
  return start;
}
int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int casecompare(const char* a, const char* b, std::uint64_t n) {
  for (std::uint64_t i = 0; i < n; ++i) {
    auto x = lower(static_cast<unsigned char>(load(a + i))), y = lower(static_cast<unsigned char>(load(b + i)));
    if (x != y) return x - y;
    if (!x) break;
  }
  return 0;
}
int strcase(const char* a, const char* b) { return casecompare(a, b, UINT64_MAX); }
int strcase_l(const char* a, const char* b, GuestAddr) { return strcase(a, b); }
int strncase_l(const char* a, const char* b, std::uint64_t n, GuestAddr) { return casecompare(a, b, n); }
char* casesearch(const char* s, const char* sub) {
  auto n = length(sub);
  if (!n) return const_cast<char*>(s);
  for (; load(s); ++s)
    if (!casecompare(s, sub, n)) return const_cast<char*>(s);
  return nullptr;
}
char* chrnul(const char* s, int c) {
  while (load(s) && load(s) != static_cast<char>(c)) ++s;
  return const_cast<char*>(s);
}
std::uint64_t lcopy(char* d, const char* s, std::uint64_t n) {
  auto len = length(s);
  if (n) {
    auto k = std::min(len, n - 1);
    copy_bytes(d, s, k);
    store(d + k, char{});
  }
  return len;
}
std::uint64_t lcat(char* d, const char* s, std::uint64_t n) {
  auto dn = nlength(d, n);
  return dn + (dn < n ? lcopy(d + dn, s, n - dn) : length(s));
}
template<GuestIsa Isa, platform::NativeMemoryOp Op>
std::uint64_t memory_bound(void* d, const void* s, std::uint64_t n, int value = 0) {
  if constexpr (Isa == GuestIsa::a32) {
    check_memory_range32(h2g(d), n, Op != platform::NativeMemoryOp::compare);
    if constexpr (Op != platform::NativeMemoryOp::fill) check_memory_range32(h2g(s), n, false);
  }
  platform::NativeMemoryFault fault;
  bool ok; int comparison = 0;
  if constexpr (Op == platform::NativeMemoryOp::copy) ok = platform::native_copy(h2g(d), h2g(s), n, fault);
  if constexpr (Op == platform::NativeMemoryOp::move) ok = platform::native_move(h2g(d), h2g(s), n, fault);
  if constexpr (Op == platform::NativeMemoryOp::fill) ok = platform::native_fill(h2g(d), value, n, fault);
  if constexpr (Op == platform::NativeMemoryOp::compare) ok = platform::native_compare(h2g(d), h2g(s), n, comparison, fault);
  if (!ok) throw MemoryFault{fault.address, fault.write, fault.bus};
  if constexpr (Op == platform::NativeMemoryOp::compare) return static_cast<std::uint64_t>(static_cast<std::int64_t>(comparison));
  else return h2g(d);
}
template<GuestIsa Isa> void* memcpy_bound(void* d, const void* s, std::uint64_t n) { return g2h<void>(memory_bound<Isa, platform::NativeMemoryOp::copy>(d, s, n)); }
template<GuestIsa Isa> void* memmove_bound(void* d, const void* s, std::uint64_t n) { return g2h<void>(memory_bound<Isa, platform::NativeMemoryOp::move>(d, s, n)); }
template<GuestIsa Isa> void* memset_bound(void* d, int v, std::uint64_t n) { return g2h<void>(memory_bound<Isa, platform::NativeMemoryOp::fill>(d, nullptr, n, v)); }
template<GuestIsa Isa> int memcmp_bound(const void* a, const void* b, std::uint64_t n) { return static_cast<int>(memory_bound<Isa, platform::NativeMemoryOp::compare>(const_cast<void*>(a), b, n)); }
void* memcpy_impl(void* d, const void* s, std::uint64_t n) { return g2h<void>(native(platform::NativeMemoryOp::copy, d, s, n)); }
void* memmove_impl(void* d, const void* s, std::uint64_t n) { return g2h<void>(native(platform::NativeMemoryOp::move, d, s, n)); }
void* memset_impl(void* d, int v, std::uint64_t n) { return g2h<void>(native(platform::NativeMemoryOp::fill, d, nullptr, n, v)); }
int memcmp_impl(const void* a, const void* b, std::uint64_t n) { return static_cast<int>(native(platform::NativeMemoryOp::compare, const_cast<void*>(a), b, n)); }
void* memchr_impl(const void* a, int v, std::uint64_t n) { return g2h<void>(native(platform::NativeMemoryOp::find, nullptr, a, n, v)); }
void* memrchr_impl(const void* a, int v, std::uint64_t n) {
  auto p = static_cast<const unsigned char*>(a);
  while (n)
    if (load(p + --n) == static_cast<unsigned char>(v)) return const_cast<unsigned char*>(p + n);
  return nullptr;
}
void* mempcpy_impl(void* d, const void* s, std::uint64_t n) {
  copy_bytes(d, s, n);
  return static_cast<char*>(d) + n;
}
void* memccpy_impl(void* d, const void* s, int c, std::uint64_t n) {
  auto hit = g2h<const char>(native(platform::NativeMemoryOp::find, nullptr, s, n, c));
  auto count = hit ? static_cast<std::uint64_t>(hit - static_cast<const char*>(s) + 1) : n;
  copy_bytes(d, s, count);
  return hit ? static_cast<char*>(d) + count : nullptr;
}
void* memmem_impl(const void* s, std::uint64_t n, const void* sub, std::uint64_t m) {
  auto p = static_cast<const char*>(s);
  if (!m) return const_cast<void*>(s);
  if (m > n) return nullptr;
  for (std::uint64_t i = 0; i <= n - m; ++i)
    if (!native(platform::NativeMemoryOp::compare, const_cast<char*>(p + i), sub, m)) return const_cast<char*>(p + i);
  return nullptr;
}
void bzero_impl(void* p, std::uint64_t n) { native(platform::NativeMemoryOp::fill, p, nullptr, n, 0); }
void explicit_bzero_impl(void* p, std::uint64_t n) {
  native(platform::NativeMemoryOp::fill, p, nullptr, n, 0);
}
void bcopy_impl(const void* s, void* d, std::uint64_t n) { native(platform::NativeMemoryOp::move, d, s, n); }
int ffs_impl(int x) { return x ? std::countr_zero(static_cast<unsigned>(x)) + 1 : 0; }
int ffs64(std::int64_t x) { return x ? std::countr_zero(static_cast<std::uint64_t>(x)) + 1 : 0; }
void swab_impl(const void* s, void* d, std::int64_t n) {
  auto a = static_cast<const char*>(s);
  auto b = static_cast<char*>(d);
  for (std::int64_t i = 0; i + 1 < n; i += 2) {
    auto x = load(a + i), y = load(a + i + 1);
    store(b + i, y);
    store(b + i + 1, x);
  }
}
using W = std::uint32_t;
W* wmemcopy(W* d, const W* s, std::uint64_t n) {
  copy_bytes(d, s, bytes(n, 4, s));
  return d;
}
W* wmemmove(W* d, const W* s, std::uint64_t n) {
  native(platform::NativeMemoryOp::move, d, s, bytes(n, 4, s));
  return d;
}
W* wmemset(W* d, W c, std::uint64_t n) {
  for (std::uint64_t i = 0; i < n; ++i) store(d + i, c);
  return d;
}
W* wmemchr(const W* s, W c, std::uint64_t n) {
  for (std::uint64_t i = 0; i < n; ++i)
    if (load(s + i) == c) return const_cast<W*>(s + i);
  return nullptr;
}
int wmemcmp(const W* a, const W* b, std::uint64_t n) {
  for (std::uint64_t i = 0; i < n; ++i)
    if (load(a + i) != load(b + i)) return load(a + i) < load(b + i) ? -1 : 1;
  return 0;
}
W* wmempcopy(W* d, const W* s, std::uint64_t n) { return wmemcopy(d, s, n) + n; }
// FORTIFY checks precede any read/write of the bounded object. Source: bionic/libc/bionic/fortify.cpp.
void check(std::uint64_t n, std::uint64_t cap) {
  if (n > cap) fatal("FORTIFY: buffer overflow");
}
std::uint64_t strlen_chk(const char* s, std::uint64_t cap) {
  auto n = nlength(s, cap);
  if (n == cap) fatal("FORTIFY: unterminated string");
  return n;
}
void* memcpy_chk(void* d, const void* s, std::uint64_t n, std::uint64_t cap) {
  check(n, cap);
  return memcpy_impl(d, s, n);
}
void* memmove_chk(void* d, const void* s, std::uint64_t n, std::uint64_t cap) {
  check(n, cap);
  return memmove_impl(d, s, n);
}
void* memset_chk(void* d, int v, std::uint64_t n, std::uint64_t cap) {
  check(n, cap);
  return memset_impl(d, v, n);
}
void* memchr_chk(const void* s, int v, std::uint64_t n, std::uint64_t cap) {
  check(n, cap);
  return memchr_impl(s, v, n);
}
void* memrchr_chk(const void* s, int v, std::uint64_t n, std::uint64_t cap) {
  check(n, cap);
  return memrchr_impl(s, v, n);
}
char* strcpy_chk(char* d, const char* s, std::uint64_t cap) {
  check(length(s) + 1, cap);
  return copy(d, s);
}
char* stpcpy_chk(char* d, const char* s, std::uint64_t cap) {
  check(length(s) + 1, cap);
  return endcopy(d, s);
}
char* strncpy_chk(char* d, const char* s, std::uint64_t n, std::uint64_t cap) {
  check(n, cap);
  return ncopy(d, s, n);
}
char* stpncpy_chk(char* d, const char* s, std::uint64_t n, std::uint64_t cap) {
  check(n, cap);
  return endncopy(d, s, n);
}
char* strncpy_chk2(char* d, const char* s, std::uint64_t n, std::uint64_t cap, std::uint64_t scap) {
  check(n, cap);
  if (n > scap) strlen_chk(s, scap);
  return ncopy(d, s, n);
}
char* stpncpy_chk2(char* d, const char* s, std::uint64_t n, std::uint64_t cap, std::uint64_t scap) {
  strncpy_chk2(d, s, n, cap, scap);
  return d + nlength(s, n);
}
char* strcat_chk(char* d, const char* s, std::uint64_t cap) {
  auto dn = strlen_chk(d, cap);
  check(length(s) + 1, cap - dn);
  return cat(d, s);
}
char* strncat_chk(char* d, const char* s, std::uint64_t n, std::uint64_t cap) {
  auto dn = strlen_chk(d, cap);
  check(nlength(s, n) + 1, cap - dn);
  return ncat(d, s, n);
}
std::uint64_t strlcpy_chk(char* d, const char* s, std::uint64_t n, std::uint64_t cap) {
  check(n, cap);
  return lcopy(d, s, n);
}
std::uint64_t strlcat_chk(char* d, const char* s, std::uint64_t n, std::uint64_t cap) {
  check(n, cap);
  return lcat(d, s, n);
}
char* strchr_chk(const char* s, int c, std::uint64_t cap) {
  for (std::uint64_t i = 0; i < cap; ++i) {
    if (load(s + i) == static_cast<char>(c)) return const_cast<char*>(s + i);
    if (!load(s + i)) return nullptr;
  }
  fatal("FORTIFY: strchr read overflow");
}
char* strrchr_chk(const char* s, int c, std::uint64_t cap) {
  strlen_chk(s, cap);
  return rfind(s, c);
}
}  // namespace
void register_strings(HleLibrary& lib) {
#define B(name, fn) bind<&fn>(lib, name, Execution::InJit)
  bind_cursors<&memcpy_bound<GuestIsa::a64>, &memcpy_bound<GuestIsa::a32>>(lib, "memcpy", Execution::InJit);
  bind_cursors<&memmove_bound<GuestIsa::a64>, &memmove_bound<GuestIsa::a32>>(lib, "memmove", Execution::InJit);
  bind_cursors<&memset_bound<GuestIsa::a64>, &memset_bound<GuestIsa::a32>>(lib, "memset", Execution::InJit);
  bind_cursors<&memcmp_bound<GuestIsa::a64>, &memcmp_bound<GuestIsa::a32>>(lib, "memcmp", Execution::InJit);
  B("memchr", memchr_impl);
  B("memrchr", memrchr_impl);
  B("mempcpy", mempcpy_impl);
  B("memccpy", memccpy_impl);
  B("memmem", memmem_impl);
  B("bzero", bzero_impl);
  B("explicit_bzero", explicit_bzero_impl);
  B("bcopy", bcopy_impl);
  B("bcmp", memcmp_impl);
  B("ffs", ffs_impl);
  B("ffsl", ffs64);
  B("ffsll", ffs64);
  B("swab", swab_impl);
  bind<&duplicate<char>>(lib, "strdup");
  bind<&nduplicate>(lib, "strndup");
  B("strtok", strtok_impl);
  B("strtok_r", tokenize<char>);
  B("strsep", strsep_impl);
  B("strcasecmp", strcase);
  B("strncasecmp", casecompare);
  B("strcasecmp_l", strcase_l);
  B("strncasecmp_l", strncase_l);
  B("strcasestr", casesearch);
  B("strchrnul", chrnul);
  B("strlcpy", lcopy);
  B("strlcat", lcat);
  B("index", find<char>);
  B("rindex", rfind<char>);
#define STRINGS(P, C)          \
  bind_cursors<&length64<C>, &length<C>>(lib, P "len", Execution::InJit);       \
  bind_cursors<&nlength<C, false>, &nlength<C>>(lib, P "nlen", Execution::InJit);     \
  B(P "cpy", copy<C>);         \
  B(P "ncpy", ncopy<C>);       \
  B(P "cat", cat<C>);          \
  B(P "ncat", ncat<C>);        \
  B(P "cmp", compare<C>);      \
  B(P "ncmp", ncompare<C>);    \
  B(P "chr", find<C>);         \
  B(P "rchr", rfind<C>);       \
  B(P "spn", span<C>);         \
  B(P "cspn", cspan<C>);       \
  B(P "pbrk", pbrk<C>);        \
  B(P "coll", compare<C>);     \
  B(P "coll_l", collate_l<C>); \
  B(P "xfrm", transform<C>);   \
  B(P "xfrm_l", transform_l<C>)
  STRINGS("str", char);
  STRINGS("wcs", W);
  B("strstr", search<char>);
  B("wcsstr", search<W>);
  B("wcstok", tokenize<W>);
  bind<&duplicate<W>>(lib, "wcsdup");
  B("stpcpy", endcopy<char>);
  B("stpncpy", endncopy<char>);
  B("wcpcpy", endcopy<W>);
  B("wcpncpy", endncopy<W>);
  B("wmemcpy", wmemcopy);
  B("wmemmove", wmemmove);
  B("wmemset", wmemset);
  B("wmemchr", wmemchr);
  B("wmemcmp", wmemcmp);
  B("wmempcpy", wmempcopy);
// FORTIFY failures call the guest abort machinery; allocation and error-string
// registrations also retain the run-loop path.
#define CHK(n) bind<&n>(lib, "__" #n)
  CHK(memcpy_chk);
  CHK(memmove_chk);
  CHK(memset_chk);
  CHK(memchr_chk);
  CHK(memrchr_chk);
  CHK(strlen_chk);
  CHK(strcpy_chk);
  CHK(stpcpy_chk);
  CHK(strncpy_chk);
  CHK(strncpy_chk2);
  CHK(stpncpy_chk);
  CHK(stpncpy_chk2);
  CHK(strcat_chk);
  CHK(strncat_chk);
  CHK(strlcpy_chk);
  CHK(strlcat_chk);
  CHK(strchr_chk);
  CHK(strrchr_chk);
#undef CHK
#undef STRINGS
#undef B
  extern void register_string_errors(HleLibrary&);
  register_string_errors(lib);
  extern void register_aeabi(HleLibrary&);
  register_aeabi(lib);
  extern void register_guest_strings(HleLibrary&);
  register_guest_strings(lib);
}
}  // namespace venture::hle::libc
