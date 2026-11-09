// ARMv7 guest oracle for the system libstdc++.so and libc exports a Unity 2018
// armeabi-v7a title (Shadow Point) imports. Clang, not Venture, decides the ARM
// C++ ABI guard word, size_t mangling (_Znwj), AAPCS32 struct returns through
// memory (div_t, lldiv_t) and the ILP32 record layouts. Built freestanding
// against empty libc.so/libstdc++.so stubs that only provide DT_NEEDED; the
// imports bind to Venture's HLE libraries. Returns 0, or the failing line.
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
typedef unsigned int size_t;
typedef int ssize_t;
typedef long long intmax_t;
static_assert(sizeof(long) == 4 && sizeof(void*) == 4 && sizeof(intmax_t) == 8, "ILP32");
namespace std {
struct nothrow_t { explicit nothrow_t() = default; };
extern const nothrow_t nothrow;
enum class align_val_t : size_t {};
}
void* operator new(size_t, const std::nothrow_t&) noexcept;
void operator delete(void*, const std::nothrow_t&) noexcept;
void* operator new[](size_t, const std::nothrow_t&) noexcept;
extern "C" {
// bionic ARM layouts, declared here because the fixture has no sysroot.
struct timeval { long tv_sec, tv_usec; };
struct rusage { timeval ru_utime, ru_stime; long counters[14]; };
struct group { char* gr_name; char* gr_passwd; unsigned gr_gid; char** gr_mem; };
struct in_addr { unsigned s_addr; };
struct iovec { void* iov_base; size_t iov_len; };
struct msghdr { void* msg_name; unsigned msg_namelen; iovec* msg_iov; size_t msg_iovlen;
                void* msg_control; size_t msg_controllen; int msg_flags; };
struct div_t { int quot, rem; };
struct ldiv_t { long quot, rem; };
struct lldiv_t { long long quot, rem; };
struct imaxdiv_t { intmax_t quot, rem; };
static_assert(sizeof(rusage) == 72 && sizeof(group) == 16 && sizeof(msghdr) == 28, "bionic ARM layouts");
int __cxa_guard_acquire(int*);
void __cxa_guard_release(int*);
void __cxa_guard_abort(int*);
int* __errno();
div_t div(int, int);
ldiv_t ldiv(long, long);
lldiv_t lldiv(long long, long long);
imaxdiv_t imaxdiv(intmax_t, intmax_t);
intmax_t strtoimax(const char*, char**, int);
intmax_t imaxabs(intmax_t);
int getrusage(int, rusage*);
group* getgrnam(const char*);
int inet_aton(const char*, in_addr*);
char* inet_ntoa(in_addr);
void* mmap(void*, size_t, int, int, int, long);
void* mremap(void*, size_t, size_t, int, ...);
int munmap(void*, size_t);
int socketpair(int, int, int, int*);
ssize_t sendmsg(int, const msghdr*, int);
ssize_t recvmsg(int, msghdr*, int);
int close(int);
void openlog(const char*, int, int);
void syslog(int, const char*, ...);
void closelog();
int execv(const char*, char* const*);
void* popen(const char*, const char*);
int strcmp(const char*, const char*);
int memcmp(const void*, const void*, size_t);
int pthread_create(long*, const void*, void* (*)(void*), void*);
int pthread_join(long, void**);
int usleep(unsigned);
}

static volatile int constructions;
struct Slow {
  int value;
  Slow() : value(42) { usleep(20000); constructions = constructions + 1; }
};
static Slow& instance() {
  static Slow slow;  // clang: ARM guard word, __cxa_guard_acquire/release
  return slow;
}
static void* worker(void* out) {
  *static_cast<int*>(out) = instance().value;
  return nullptr;
}
struct Base {
  virtual int f() = 0;  // the out-of-line key function emits a vtable slot for __cxa_pure_virtual
  virtual int g();
};
int Base::g() { return 1; }
struct Derived final : Base { int f() override { return 7; } };
struct alignas(64) Wide { int value; };

static int cxx() {
  // Concurrent first use: one construction, every thread sees the object.
  long threads[4];
  int seen[4] = {0, 0, 0, 0};
  for (int i = 0; i < 4; ++i) CHECK(pthread_create(&threads[i], nullptr, worker, &seen[i]) == 0);
  for (int i = 0; i < 4; ++i) CHECK(pthread_join(threads[i], nullptr) == 0);
  for (int i = 0; i < 4; ++i) CHECK(seen[i] == 42);
  CHECK(constructions == 1); CHECK(instance().value == 42); CHECK(constructions == 1);
  // An aborted initialization leaves the guard reusable; bit 0 marks completion.
  int guard[2] = {0, 0x5a5a5a5a};
  CHECK(__cxa_guard_acquire(guard) == 1); CHECK((guard[0] & 1) == 0);
  __cxa_guard_abort(guard);
  CHECK(guard[0] == 0);
  CHECK(__cxa_guard_acquire(guard) == 1);
  __cxa_guard_release(guard);
  CHECK(guard[0] == 1); CHECK(guard[1] == 0x5a5a5a5a);
  CHECK(__cxa_guard_acquire(guard) == 0);
  // _Znwj/_ZdlPvj, _Znaj/_ZdaPv, nothrow and aligned forms.
  int* one = new int(5);
  CHECK(*one == 5);
  delete one;
  int* many = new int[16];
  many[15] = 3;
  CHECK(many[15] == 3);
  delete[] many;
  int* quiet = new (std::nothrow) int(9);
  CHECK(quiet && *quiet == 9);
  operator delete(quiet, std::nothrow);
  int* quiet_array = new (std::nothrow) int[4];
  CHECK(quiet_array);
  delete[] quiet_array;
  Wide* wide = new Wide{11};
  CHECK((reinterpret_cast<unsigned>(wide) & 63) == 0); CHECK(wide->value == 11);
  delete wide;
  Base* derived = new Derived;
  CHECK(derived->f() == 7); CHECK(derived->g() == 1);
  delete static_cast<Derived*>(derived);
  return 0;
}

static int libc() {
  // AAPCS32: div_t and wider results return through memory at r0.
  div_t d = div(-7, 2);
  CHECK(d.quot == -3 && d.rem == -1);
  d = div(-2147483647 - 1, -1);
  CHECK(d.quot == -2147483647 - 1 && d.rem == 0);
  ldiv_t l = ldiv(100, 7);
  CHECK(l.quot == 14 && l.rem == 2);
  lldiv_t ll = lldiv(-9000000000ll, 7);
  CHECK(ll.quot == -1285714285ll && ll.rem == -5);
  imaxdiv_t im = imaxdiv(9000000000ll, 1000);
  CHECK(im.quot == 9000000 && im.rem == 0);
  char* end = nullptr;
  const char* text = "  -9000000000xyz";
  CHECK(strtoimax(text, &end, 10) == -9000000000ll); CHECK(end == text + 13);
  CHECK(imaxabs(-5ll) == 5);
  struct { rusage usage; unsigned canary; } usage;
  usage.canary = 0xa5a5a5a5;
  CHECK(getrusage(0, &usage.usage) == 0);
  CHECK(usage.usage.ru_utime.tv_usec < 1000000); CHECK(usage.canary == 0xa5a5a5a5);
  group* g = getgrnam("inet");
  CHECK(g && strcmp(g->gr_name, "inet") == 0 && g->gr_gid == 3003 && g->gr_mem && !g->gr_mem[0]);
  CHECK(!getgrnam("no-such-group"));
  struct { in_addr address; unsigned canary; } address = {{0}, 0xa5a5a5a5};
  CHECK(inet_aton("1.2.3.4", &address.address) == 1);
  CHECK(address.address.s_addr == 0x04030201); CHECK(address.canary == 0xa5a5a5a5);
  CHECK(strcmp(inet_ntoa(address.address), "1.2.3.4") == 0);
  // mremap: MREMAP_MAYMOVE growth keeps contents; MAP_FAILED is (void*)-1.
  auto* page = static_cast<unsigned*>(mmap(nullptr, 4096, 3, 0x22, -1, 0));
  CHECK(page != reinterpret_cast<void*>(-1));
  page[0] = 0x12345678;
  auto* grown = static_cast<unsigned*>(mremap(page, 4096, 8192, 1));
  CHECK(grown != reinterpret_cast<void*>(-1)); CHECK(grown[0] == 0x12345678);
  grown[2047] = 1;
  CHECK(mremap(grown, 8192, 4096, 4) == reinterpret_cast<void*>(-1)); CHECK(*__errno() == 22);
  CHECK(munmap(grown, 8192) == 0);
  // 28-byte msghdr with two 8-byte iovecs.
  int fds[2];
  CHECK(socketpair(1, 1, 0, fds) == 0);
  char a[] = "ven", b[] = "ture", in[16] = {0};
  iovec out_iov[2] = {{a, 3}, {b, 4}};
  msghdr out = {nullptr, 0, out_iov, 2, nullptr, 0, 0};
  CHECK(sendmsg(fds[0], &out, 0) == 7);
  iovec in_iov = {in, sizeof(in)};
  msghdr incoming = {nullptr, 0, &in_iov, 1, nullptr, 0, 0x7777};
  CHECK(recvmsg(fds[1], &incoming, 0) == 7);
  CHECK(memcmp(in, "venture", 7) == 0); CHECK(incoming.msg_flags == 0); CHECK(incoming.msg_controllen == 0);
  CHECK(close(fds[0]) == 0 && close(fds[1]) == 0);
  openlog("a32_cxx_libc", 1, 8);
  syslog(6, "syslog %d %s %lld", 42, "a32", 9000000000ll);
  closelog();
  // No process images or popen: ENOSYS (38) instead of the ABI trap.
  char* const argv[] = {nullptr};
  CHECK(execv("/system/bin/sh", argv) == -1); CHECK(*__errno() == 38);
  CHECK(!popen("ls", "r")); CHECK(*__errno() == 38);
  return 0;
}

extern "C" __attribute__((visibility("default"))) int vtest_main(int, char**) {
  if (int line = cxx()) return line;
  return libc();
}
