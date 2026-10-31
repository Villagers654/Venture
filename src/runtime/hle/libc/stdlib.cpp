#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/process_image.h"
#include "hle/libc/libc_a.h"
#include "hle/libc/abi32.h"
#include "platform/libc_host.h"
namespace venture::hle::libc {
namespace {
// Environment belongs to the guest, never to the launcher or host process.
// Retain replaced storage: getenv pointers can be in use on another guest thread.
std::mutex env_mutex;
char* initial_environment[] = {nullptr};
char** environment = initial_environment;
std::vector<std::unique_ptr<char[]>> env_strings;
std::vector<std::unique_ptr<char*[]>> env_arrays;
bool env_match(const char* s, const char* name, std::size_t len) {
  return !std::strncmp(s, name, len) && s[len] == '=';
}
char* getenv_impl(const char* name) {
  if (!name || !*name || std::strchr(name, '=')) return nullptr;
  std::lock_guard lock(env_mutex);
  auto n = std::strlen(name);
  for (auto p = environment; p && *p; ++p)
    if (env_match(*p, name, n)) return *p + n + 1;
  return nullptr;
}
void publish_environment(std::vector<char*> entries) {
  auto p = std::make_unique<char*[]>(entries.size() + 1);
  std::copy(entries.begin(), entries.end(), p.get());
  p[entries.size()] = nullptr;
  auto raw = p.get();
  env_arrays.push_back(std::move(p));
  environment = raw;
}
int unsetenv_impl(const char* name) {
  if (!name || !*name || std::strchr(name, '=')) {
    set_error(err::kEINVAL);
    return -1;
  }
  std::lock_guard lock(env_mutex);
  std::vector<char*> entries;
  auto n = std::strlen(name);
  for (auto p = environment; p && *p; ++p)
    if (!env_match(*p, name, n)) entries.push_back(*p);
  publish_environment(std::move(entries));
  return 0;
}
int putenv_impl(char* text) {
  auto equal = std::strchr(text, '=');
  if (!equal) return unsetenv_impl(text);
  if (equal == text) {
    set_error(err::kEINVAL);
    return -1;
  }
  std::lock_guard lock(env_mutex);
  std::vector<char*> entries;
  auto n = equal - text;
  bool replaced = false;
  for (auto p = environment; p && *p; ++p) {
    if (env_match(*p, text, n)) {
      if (!replaced) entries.push_back(text);
      replaced = true;
    } else
      entries.push_back(*p);
  }
  if (!replaced) entries.push_back(text);
  publish_environment(std::move(entries));
  return 0;
}
int setenv_impl(const char* name, const char* value, int overwrite) {
  if (!name || !*name || std::strchr(name, '=') || !value) {
    set_error(err::kEINVAL);
    return -1;
  }
  std::lock_guard lock(env_mutex);
  auto n = std::strlen(name);
  std::vector<char*> entries;
  bool found = false;
  for (auto p = environment; p && *p; ++p)
    if (env_match(*p, name, n)) found = true;
  if (found && !overwrite) return 0;
  auto s = std::make_unique<char[]>(n + std::strlen(value) + 2);
  std::memcpy(s.get(), name, n);
  s[n] = '=';
  std::strcpy(s.get() + n + 1, value);
  auto raw = s.get();
  env_strings.push_back(std::move(s));
  bool inserted = false;
  for (auto p = environment; p && *p; ++p) {
    if (env_match(*p, name, n)) {
      if (!inserted) entries.push_back(raw);
      inserted = true;
    } else
      entries.push_back(*p);
  }
  if (!inserted) entries.push_back(raw);
  publish_environment(std::move(entries));
  return 0;
}
int clearenv_impl() {
  std::lock_guard lock(env_mutex);
  environment = initial_environment;
  return 0;
}
void arc4random_buf_impl(void* p, std::uint64_t n) {
  if (!platform::secure_random(p, n)) fatal("arc4random: host entropy source failed");
}
std::uint32_t arc4random_impl() {
  std::uint32_t n;
  arc4random_buf_impl(&n, 4);
  return n;
}
std::uint32_t arc4random_uniform_impl(std::uint32_t bound) {
  if (bound < 2) return 0;
  std::uint32_t n, threshold = (0u - bound) % bound;
  do {
    n = arc4random_impl();
  } while (n < threshold);
  return n % bound;
}
int getentropy_impl(void* p, std::uint64_t n) {
  if (n > 256) {
    set_error(err::kEIO);
    return -1;
  }
  if (!platform::secure_random(p, n)) {
    set_error(err::kEIO);
    return -1;
  }
  return 0;
}
// bionic/upstream-netbsd/lib/libc/stdlib/rand_r.c (API 32).
int rand_r_impl(std::uint32_t* state) {
  *state = *state * 1103515245u + 12345u;
  return static_cast<int>(*state & 0x7fffffffu);
}
std::mutex rand_mutex;
// BSD random(): additive feedback, degree 31, separation 3. rand shares it in bionic.
std::array<std::uint32_t, 31> random_state{};
unsigned random_front = 3, random_rear = 0;
bool random_seeded = false;
std::uint32_t random_step() {
  auto v = random_state[random_front] += random_state[random_rear];
  random_front = (random_front + 1) % 31;
  random_rear = (random_rear + 1) % 31;
  return (v >> 1) & 0x7fffffff;
}
void seed_random(std::uint32_t seed) {
  if (!seed) seed = 1;
  random_state[0] = seed;
  std::int64_t word = static_cast<std::int32_t>(seed);
  for (unsigned i = 1; i < 31; ++i) {
    word = 16807 * (word % 127773) - 2836 * (word / 127773);
    if (word < 0) word += 2147483647;
    random_state[i] = static_cast<std::uint32_t>(word);
  }
  random_front = 3;
  random_rear = 0;
  for (int i = 0; i < 310; ++i) random_step();
  random_seeded = true;
}
void srand_impl(std::uint32_t seed) {
  std::lock_guard lock(rand_mutex);
  seed_random(seed);
}
int rand_impl() {
  std::lock_guard lock(rand_mutex);
  if (!random_seeded) seed_random(1);
  return static_cast<int>(random_step());
}
std::int64_t random_impl() { return rand_impl(); }
std::mutex rand48_mutex;
std::uint16_t rand48_state[3] = {0x330e, 0xabcd, 0x1234}, rand48_old[3]{};
std::uint64_t rand48_multiplier = 0x5deece66dull;
std::uint16_t rand48_addend = 11;
std::uint64_t step48(std::uint16_t* x) {
  auto n = std::uint64_t(x[0]) | (std::uint64_t(x[1]) << 16) | (std::uint64_t(x[2]) << 32);
  n = (n * rand48_multiplier + rand48_addend) & 0xffffffffffffull;
  x[0] = n;
  x[1] = n >> 16;
  x[2] = n >> 32;
  return n;
}
double erand48_impl(std::uint16_t* x) {
  std::lock_guard lock(rand48_mutex);
  return std::ldexp(static_cast<double>(step48(x)), -48);
}
std::int64_t nrand48_impl(std::uint16_t* x) {
  std::lock_guard lock(rand48_mutex);
  return static_cast<std::int64_t>(step48(x) >> 17);
}
std::int64_t jrand48_impl(std::uint16_t* x) {
  std::lock_guard lock(rand48_mutex);
  return static_cast<std::int32_t>(step48(x) >> 16);
}
double drand48_impl() { return erand48_impl(rand48_state); }
std::int64_t lrand48_impl() { return nrand48_impl(rand48_state); }
std::int64_t mrand48_impl() { return jrand48_impl(rand48_state); }
void srand48_impl(std::int64_t seed) {
  std::lock_guard lock(rand48_mutex);
  rand48_state[0] = 0x330e;
  rand48_state[1] = static_cast<std::uint16_t>(seed);
  rand48_state[2] = static_cast<std::uint16_t>(static_cast<std::uint64_t>(seed) >> 16);
  rand48_multiplier = 0x5deece66dull;
  rand48_addend = 11;
}
std::uint16_t* seed48_impl(const std::uint16_t* x) {
  std::lock_guard lock(rand48_mutex);
  std::copy_n(rand48_state, 3, rand48_old);
  std::copy_n(x, 3, rand48_state);
  rand48_multiplier = 0x5deece66dull;
  rand48_addend = 11;
  return rand48_old;
}
void lcong48_impl(const std::uint16_t* x) {
  std::lock_guard lock(rand48_mutex);
  std::copy_n(x, 3, rand48_state);
  rand48_multiplier = std::uint64_t(x[3]) | (std::uint64_t(x[4]) << 16) | (std::uint64_t(x[5]) << 32);
  rand48_addend = x[6];
}
void qsort_r_impl(CallContext& c, void* base, std::uint64_t count, std::uint64_t size, GuestAddr cmp,
                  GuestAddr user) {
  if (count < 2 || !size) return;
  if (count > UINT64_MAX / size) {
    set_error(err::kEINVAL);
    return;
  }
  auto p = static_cast<unsigned char*>(base);
  auto less = [&](std::uint64_t a, std::uint64_t b) {
    return static_cast<std::int32_t>(c.thread().call(cmp, {h2g(p + a * size), h2g(p + b * size), user})) < 0;
  };
  auto swap = [&](std::uint64_t a, std::uint64_t b) {
    for (std::uint64_t i = 0; i < size; ++i) std::swap(p[a * size + i], p[b * size + i]);
  };
  // In-place heapsort: callbacks always receive addresses of elements in base.
  auto sift = [&](std::uint64_t root, std::uint64_t n) {
    while (root < n / 2) {
      auto child = root * 2 + 1;
      if (child + 1 < n && less(child, child + 1)) ++child;
      if (!less(root, child)) return;
      swap(root, child);
      root = child;
    }
  };
  for (auto i = count / 2; i--;) sift(i, count);
  for (auto n = count; n > 1;) {
    swap(0, --n);
    sift(0, n);
  }
}
void qsort_impl(CallContext& c, void* p, std::uint64_t n, std::uint64_t s, GuestAddr f) {
  qsort_r_impl(c, p, n, s, f, 0);
}
void* bsearch_impl(CallContext& c, const void* key, const void* base, std::uint64_t count, std::uint64_t size,
                   GuestAddr cmp) {
  auto p = static_cast<const char*>(base);
  while (count) {
    auto half = count / 2;
    auto mid = p + half * size;
    auto order = static_cast<std::int32_t>(c.thread().call(cmp, {h2g(key), h2g(mid)}));
    if (!order) return const_cast<char*>(mid);
    if (order < 0)
      count = half;
    else {
      p = mid + size;
      count -= half + 1;
    }
  }
  return nullptr;
}
std::uint64_t getauxval_impl(std::uint64_t type) {
  static auto random = [] {
    std::array<unsigned char, 16> b{};
    arc4random_buf_impl(b.data(), b.size());
    return b;
  }();
  if (std::uint64_t value; process_image::auxval(type, &value)) return value;
  switch (type) {
    case 6:
      return 4096;
    case 16:
      return cpu_backend().info().hwcap;
    case 26:
      return cpu_backend().info().hwcap2;
    case 17:
      return 100;
    case 23:
      return 0;
    case 25:
      return h2g(random.data());
    case 15:
      return h2g("aarch64");
    case 51:
      return 5120;
    case 11:
    case 12:
    case 13:
    case 14:
      return 10000;
    default:
      set_error(err::kENOENT);
      return 0;
  }
}
std::uint64_t getauxval32(std::uint64_t type) {
  // arm64 Linux arch/arm64/include/asm/elf.h: COMPAT_ELF_PLATFORM.
  if (type == 15) return h2g32(low_string("v8l"));
  auto result = getauxval_impl(type);
  if (type == 25) return low_data(std::span(g2h<const std::byte>(result), 16));
  return result;
}
}  // namespace
const char* guest_getenv(const char* name) { return getenv_impl(name); }
int set_environment(std::string_view name, std::string_view value, bool overwrite) {
  if (name.empty() || name.find_first_of("=\0", 0, 2) != name.npos ||
      value.find('\0') != value.npos) return err::kEINVAL;
  try {
    const std::string key(name), text(value);
    const int result = process_config().isa() == GuestIsa::a32
        ? setenv32(key.c_str(), text.c_str(), overwrite)
        : setenv_impl(key.c_str(), text.c_str(), overwrite);
    return result ? *error_location() : 0;
  } catch (const std::bad_alloc&) {
    return err::kENOMEM;
  }
}
void register_stdlib(HleLibrary& lib) {
  extern void register_numeric(HleLibrary&);
  register_numeric(lib);
#define B(n) bind<&n##_impl>(lib, #n)
  if (process_config().isa() == GuestIsa::a32) {
    // bionic stdlib.h: comparator arguments are two guest pointers and its
    // int result is returned in r0. call()
    // selects the ARM word-call path once when the guest thread attaches.
    abi32::adapt<&qsort_impl>("qsort");
    abi32::adapt<&bsearch_impl>("bsearch");
    extern void register_environment32(HleLibrary&);
    register_environment32(lib);
  } else {
    B(getenv);
    B(setenv);
    B(unsetenv);
    B(putenv);
    B(clearenv);
    lib.add_data("environ", h2g(&environment));
    bind<&getenv_impl>(lib, "secure_getenv");
  }
  B(arc4random_buf);
  B(arc4random);
  B(arc4random_uniform);
  B(getentropy);
  B(rand);
  B(rand_r);
  B(srand);
  B(random);
  bind<&srand_impl>(lib, "srandom");
  B(drand48);
  B(lrand48);
  B(mrand48);
  B(erand48);
  B(nrand48);
  B(jrand48);
  B(srand48);
  B(seed48);
  B(lcong48);
  B(qsort);
  B(qsort_r);
  B(bsearch);
  if (process_config().isa() == GuestIsa::a32)
    bind<&getauxval32>(lib, "getauxval");
  else
    B(getauxval);
#undef B
  extern void register_guest_search(HleLibrary&);
  register_guest_search(lib);
}
}  // namespace venture::hle::libc
