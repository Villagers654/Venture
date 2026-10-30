#include "libc_b/test.h"
#include "common/paths.h"
#include "core/signals.h"
#include "core/syscall.h"
#include "core/address_space.h"
#include <array>
#include <cstdlib>
#include <fstream>
#include <thread>
using namespace venture;
using namespace venture::hle::libc;
using namespace venture::test::libc_b;

// Opt-in, isolated CPU probe of the owned title's Rust SDK. Its real null
// backend exercises clip parsing and the background thread without an XR GPU.
TEST_CASE(
    "libc shared file views retain coherent backing across close, "
    "protection and sync") {
  Thread thread;
  auto path = process_config().guest_data_dir() + "/shared-mapping.bin";
  auto fd = call("open", {h2g(path.c_str()), 0x242, 0600});
  REQUIRE(fd >= 0);
  REQUIRE(call("ftruncate", {static_cast<std::uint64_t>(fd), 12288}) == 0);
  const std::uint32_t original = 0x12345678, changed = 0xabcdef01;
  REQUIRE(call("pwrite", {static_cast<std::uint64_t>(fd), h2g(&original), 4, 4096}) == 4);
  auto view = call("mmap", {0, 8192, 3, 1, static_cast<std::uint64_t>(fd), 4096});
  REQUIRE(view > 0);
  auto readonly = call("mmap", {0, 8192, 1, 1, static_cast<std::uint64_t>(fd), 4096});
  REQUIRE(readonly > 0);
  CHECK(*g2h<std::uint32_t>(view) == original);
  *g2h<std::uint32_t>(view) = changed;
  CHECK(*g2h<std::uint32_t>(readonly) == changed);
  std::uint32_t from_file{};
  REQUIRE(call("pread", {static_cast<std::uint64_t>(fd), h2g(&from_file), 4, 4096}) == 4);
  CHECK(from_file == changed);
  REQUIRE(call("close", {static_cast<std::uint64_t>(fd)}) == 0);
  REQUIRE(call("mprotect", {static_cast<GuestAddr>(view), 4096, 1}) == 0);
  REQUIRE(call("mprotect", {static_cast<GuestAddr>(view), 4096, 3}) == 0);
  *g2h<std::uint32_t>(view + 4096) = original;
  CHECK(*g2h<std::uint32_t>(readonly + 4096) == original);
  REQUIRE(call("msync", {static_cast<GuestAddr>(view), 8192, 4}) == 0);
  CHECK(call("munmap", {static_cast<GuestAddr>(view), 4096}) == -1);
  CHECK(*thread.thread.errno_location() == err::enotsup);
  CHECK(call("munmap", {static_cast<GuestAddr>(view), 8192}) == 0);
  CHECK(call("munmap", {static_cast<GuestAddr>(readonly), 8192}) == 0);
  fd = call("open", {h2g(path.c_str()), 0, 0});
  REQUIRE(fd >= 0);
  REQUIRE(call("pread", {static_cast<std::uint64_t>(fd), h2g(&from_file), 4, 4096}) == 4);
  CHECK(from_file == changed);
  CHECK(call("close", {static_cast<std::uint64_t>(fd)}) == 0);
  CHECK(call("unlink", {h2g(path.c_str())}) == 0);
  fd = call("open", {h2g(path.c_str()), 0x242, 0600});
  REQUIRE(fd >= 0);
  REQUIRE(call("ftruncate", {static_cast<std::uint64_t>(fd), 5000}) == 0);
  readonly = call("mmap", {0, 8192, 1, 1, static_cast<std::uint64_t>(fd), 0});
  REQUIRE(readonly > 0);
  CHECK(*g2h<unsigned char>(readonly + 8191) == 0);
  CHECK(call("munmap", {static_cast<GuestAddr>(readonly), 8192}) == 0);
  CHECK(call("close", {static_cast<std::uint64_t>(fd)}) == 0);
  CHECK(call("unlink", {h2g(path.c_str())}) == 0);
}

TEST_CASE("libc B: guest stat and dirent layouts") {
  CHECK(sizeof(Stat) == 128);
  CHECK(offsetof(Stat, mode) == 16);
  CHECK(offsetof(Stat, size) == 48);
  CHECK(offsetof(Stat, blocks) == 64);
  CHECK(offsetof(Stat, atim) == 72);
  CHECK(offsetof(Stat, ctim) == 104);
  CHECK(sizeof(Dirent) == 280);
  CHECK(offsetof(Dirent, name) == 19);
}
TEST_CASE("libc B: VFS roots and path traversal") {
  Thread thread;
  auto root = std::filesystem::path(VENTURE_LIBC_B_TEST_ROOT);
  std::filesystem::create_directories(root / "data" / "external");
  std::filesystem::create_directories(root / "obb");
  auto& c = process_config();
  c.package = "test.game";
  c.data_dir = root / "data";
  c.obb_dir = root / "obb";
  c.apk = root / "base.apk";
  c.native_lib_dir = root / "lib";
  c.cache_dir = root / "version-cache";
  c.guest_cache_dir = c.data_dir / "cache";
  MappedPath p;
  CHECK(resolve_path("/data/user/0/test.game/save.bin", at_cwd, &p) == 0);
  CHECK(p.host == root / "data" / "save.bin");
  CHECK_FALSE(p.readonly);
  CHECK(resolve_path("/sdcard/Android/obb/test.game/main.obb", at_cwd, &p) == 0);
  CHECK(p.host == root / "obb" / "main.obb");
  CHECK(p.readonly);
  for (const char* tail : {"Android/obb/test.game/main.obb", "Android/data/test.game/files/a", "Download/a"}) {
    MappedPath alias, canonical;
    REQUIRE(resolve_path(std::string("/sdcard/") + tail, at_cwd, &alias) == 0);
    REQUIRE(resolve_path(std::string("/storage/emulated/0/") + tail, at_cwd, &canonical) == 0);
    CHECK(alias.host == canonical.host);
    CHECK(alias.root == canonical.root);
    CHECK(alias.readonly == canonical.readonly);
  }
  CHECK(resolve_path((c.data_dir / "volumes/system").string(), at_cwd, &p) == 0);
  CHECK(p.guest == "/system");
  CHECK(p.readonly);
  CHECK(resolve_path("/storage/self/primary/Android/data/test.game/files/a", at_cwd, &p) == 0);
  CHECK(p.host == root / "data" / "external-data" / "files" / "a");
  CHECK(resolve_path("/data/user/0/test.game/../../../../Windows/x", at_cwd, &p) == -err::enoent);
  CHECK(resolve_path("/data/user/0/test.game/.. /x", at_cwd, &p) == -err::einval);
  CHECK(resolve_path("/data/user/0/test.game/a:stream", at_cwd, &p) == -err::einval);
  CHECK(resolve_path("/data/user/0/test.game/..\\outside", at_cwd, &p) == -err::einval);
  CHECK(fd_open(c.guest_apk_path().c_str(), 1) == -err::erofs);
  // Java File paths and Android paths must reach exactly the same file/rights.
  CHECK(resolve_path((c.guest_cache_dir / "shader.bin").string(), at_cwd, &p) == 0);
  CHECK(p.host == c.guest_cache_dir / "shader.bin");
  CHECK(p.guest == c.guest_data_dir() + "/cache/shader.bin");
  CHECK(resolve_path((c.cache_dir / "shader.bin").string(), at_cwd, &p) < 0);
  CHECK(resolve_path((c.data_dir / "external-data/files/save").string(), at_cwd, &p) == 0);
  CHECK(p.host == c.data_dir / "external-data/files/save");
  CHECK(p.guest == c.guest_external_files_dir() + "/save");
  CHECK(resolve_path((c.data_dir.parent_path() / "outside").string(), at_cwd, &p) < 0);
  CHECK(fd_open(c.apk.string().c_str(), 1) == -err::erofs);
}
TEST_CASE("libc B: descriptors share offsets but not close-on-exec flags") {
  Thread thread;
  auto& c = process_config();
  REQUIRE_FALSE(c.data_dir.empty());
  auto path = c.guest_data_dir() + "/fd-test.bin";
  auto fd = fd_open(path.c_str(), 2 | 0100 | 01000 | o_cloexec, 0600);
  REQUIRE(fd >= 0);
  REQUIRE(fd_write(fd, "abcdef", 6) == 6);
  REQUIRE(fd_seek(fd, 0, 0) == 0);
  auto dup = FdTable::get().duplicate(fd, 0, false);
  REQUIRE(dup >= 0);
  char data[8]{};
  CHECK(fd_read(fd, data, 2) == 2);
  CHECK(std::string(data, 2) == "ab");
  CHECK(fd_read(dup, data, 2) == 2);
  CHECK(std::string(data, 2) == "cd");
  CHECK(FdTable::get().get_fd_flags(fd) == 1);
  CHECK(FdTable::get().get_fd_flags(dup) == 0);
  CHECK(fd_pread(fd, data, 2, 0) == 2);
  CHECK(fd_seek(dup, 0, 1) == 4);
  CHECK(fd_fcntl(dup, 4, o_nonblock) == 0);
  CHECK((fd_fcntl(fd, 3, 0) & o_nonblock) != 0);
  Stat s;
  REQUIRE(fd_stat(fd, &s) == 0);
  CHECK(s.size == 6);
  CHECK((s.mode & 0170000) == 0100000);
  CHECK(FdTable::get().close(fd) == 0);
  CHECK(fd_read(fd, data, 1) == -err::ebadf);
  CHECK(fd_read(dup, data, 2) == 2);
  CHECK(std::string(data, 2) == "ef");
  CHECK(FdTable::get().close(dup) == 0);
}
TEST_CASE("libc B: stdio and legacy FILE array") {
  Thread thread;
  auto& lib = library();
  auto sf = lib.find_symbol("__sF");
  REQUIRE(sf != 0);
  CHECK(*g2h<GuestAddr>(lib.find_symbol("stdout")) == sf + 152);
  char buffer[32]{};
  auto f = call("fmemopen", {h2g(buffer), sizeof(buffer), h2g("w+")});
  REQUIRE(f != 0);
  CHECK(call("fwrite", {h2g("hello\nworld"), 1, 11, static_cast<GuestAddr>(f)}) == 11);
  CHECK(call("fseek", {static_cast<GuestAddr>(f), 0, 0}) == 0);
  char line[32]{};
  CHECK(call("fgets", {h2g(line), sizeof(line), static_cast<GuestAddr>(f)}) ==
        static_cast<std::int64_t>(h2g(line)));
  CHECK(std::string(line) == "hello\n");
  // Bionic's unlocked variant (Unreal reads /proc/meminfo with it).
  CHECK(call("fgets_unlocked", {h2g(line), sizeof(line), static_cast<GuestAddr>(f)}) ==
        static_cast<std::int64_t>(h2g(line)));
  CHECK(std::string(line) == "world");
  CHECK(call("ferror_unlocked", {static_cast<GuestAddr>(f)}) == 0);
  CHECK(call("fseek", {static_cast<GuestAddr>(f), 6, 0}) == 0);
  CHECK(call("ftell", {static_cast<GuestAddr>(f)}) == 6);
  CHECK(call("ungetc", {'X', static_cast<GuestAddr>(f)}) == 'X');
  CHECK(call("fgetc", {static_cast<GuestAddr>(f)}) == 'X');
  CHECK(call("fclose", {static_cast<GuestAddr>(f)}) == 0);
  char* output = nullptr;
  std::uint64_t size = 0;
  f = call("open_memstream", {h2g(&output), h2g(&size)});
  REQUIRE(f != 0);
  CHECK(call("fwrite", {h2g("abc"), 1, 3, static_cast<GuestAddr>(f)}) == 3);
  CHECK(call("fflush", {static_cast<GuestAddr>(f)}) == 0);
  CHECK(size == 3);
  CHECK(std::string(output) == "abc");
  CHECK(call("fclose", {static_cast<GuestAddr>(f)}) == 0);
  call("free", {h2g(output)});
}
TEST_CASE("libc B: pipes eventfd epoll and timerfd") {
  Thread thread;
  int pipes[2];
  REQUIRE(call("pipe2", {h2g(pipes), o_nonblock}) == 0);
  char byte;
  CHECK(fd_read(pipes[0], &byte, 1) == -err::eagain);
  auto ep = call("epoll_create1", {o_cloexec});
  REQUIRE(ep >= 0);
  struct Event {
    std::uint32_t events, pad;
    std::uint64_t data;
  } e{1, 0, 0x1234}, out{};
  REQUIRE(call("epoll_ctl",
               {static_cast<unsigned>(ep), 1, static_cast<unsigned>(pipes[0]), h2g(&e)}) == 0);
  CHECK(call("epoll_wait", {static_cast<unsigned>(ep), h2g(&out), 1, 0}) == 0);
  CHECK(fd_write(pipes[1], "x", 1) == 1);
  CHECK(call("epoll_wait", {static_cast<unsigned>(ep), h2g(&out), 1, 10}) == 1);
  CHECK(out.data == 0x1234);
  CHECK((out.events & 1) != 0);
  CHECK(fd_read(pipes[0], &byte, 1) == 1);
  CHECK(byte == 'x');
  FdTable::get().close(pipes[1]);
  PollFd p{pipes[0], 1, 0};
  CHECK(fd_poll(&p, 1, 0) == 1);
  CHECK((p.revents & poll_hup) != 0);
  CHECK(fd_read(pipes[0], &byte, 1) == 0);
  auto event = call("eventfd", {3, o_nonblock | 1});
  std::uint64_t value;
  CHECK(fd_read(event, &value, 8) == 8);
  CHECK(value == 1);
  CHECK(fd_read(event, &value, 4) == -err::einval);
  auto timer = call("timerfd_create", {1, o_nonblock});
  REQUIRE(timer >= 0);
  Timespec spec[2] = {{0, 0}, {0, 3000000}};
  REQUIRE(call("timerfd_settime", {static_cast<unsigned>(timer), 0, h2g(spec), 0}) == 0);
  p = {static_cast<int>(timer), 1, 0};
  CHECK(fd_poll(&p, 1, 100) == 1);
  CHECK(fd_read(timer, &value, 8) == 8);
  CHECK(value == 1);
  CHECK(fd_read(timer, &value, 8) == -err::eagain);
  FdTable::get().close(pipes[0]);
  FdTable::get().close(ep);
  FdTable::get().close(event);
  FdTable::get().close(timer);
}
TEST_CASE("libc B: syscall numbers and Linux errno convention") {
  Thread thread;
  auto& cpu = thread.thread.cpu();
  cpu.set_x(8, 172);
  linux_syscall(thread.thread);
  CHECK(cpu.x(0) == 1000);
  cpu.set_x(8, 63);
  cpu.set_x(0, 9999);
  cpu.set_x(1, 0);
  cpu.set_x(2, 0);
  linux_syscall(thread.thread);
  CHECK(static_cast<std::int64_t>(cpu.x(0)) == -err::ebadf);
  CHECK(call("syscall", {172}) == 1000);
  CHECK(call("syscall", {99999}) == -1);
  CHECK(*thread.thread.errno_location() == err::enosys);
  Timespec t{};
  CHECK(call("syscall", {113, 1, h2g(&t)}) == 0);
  CHECK(t.sec > 0);
  CHECK(call("sysconf", {0x27}) == 4096);
  CHECK(call("sysconf", {0x60}) == cpu_count());
  // Constant functions are guest code (MOVZ, RET) and agree with the syscalls.
  CHECK(call("getpagesize") == call("sysconf", {0x27}));
  CHECK(call("getpid") == call("syscall", {172}));
  CHECK(call("getppid") == 1);
  for (const char* name : {"getuid", "geteuid", "getgid", "getegid"}) CHECK(call(name) == 10125);
}
TEST_CASE("libc B: guest setjmp register layout and longjmp writeback") {
  Thread thread;
  auto& cpu = thread.thread.cpu();
  std::uint64_t buffer[32]{};
  for (int i = 18; i <= 30; ++i) cpu.set_x(i, 1000 + i);
  for (int i = 8; i <= 15; ++i) cpu.set_v(i, {2000u + static_cast<unsigned>(i), 55});
  cpu.set_sp(0x30000);
  thread.thread.blocked_signals = 0x40;
  auto p = library().find_symbol("sigsetjmp");
  cpu.set_x(0, h2g(buffer));
  cpu.set_x(1, 1);
  hle::dispatch(thread.thread, function_id(p));
  CHECK(cpu.x(0) == 0);
  CHECK(buffer[2] == 1030);
  CHECK(buffer[3] == 0x30000);
  CHECK(buffer[15] == 1019);
  CHECK(buffer[22] == 2008);
  CHECK(buffer[1] == 0x40);
  for (int i = 18; i <= 30; ++i) cpu.set_x(i, 0);
  thread.thread.blocked_signals = 0;
  cpu.set_sp(0x40000);
  cpu.set_x(0, h2g(buffer));
  cpu.set_x(1, 0);
  hle::dispatch(thread.thread, function_id(library().find_symbol("longjmp")));
  CHECK(cpu.x(0) == 1);
  CHECK(cpu.x(19) == 1019);
  CHECK(cpu.pc() == 1030);
  CHECK(cpu.sp() == 0x30000);
  CHECK(cpu.v(8).lo == 2008);
  CHECK(thread.thread.blocked_signals == 0x40);
}
TEST_CASE("libc B: Winsock sockaddr conversion and nonblocking UDP") {
  Thread thread;
  auto fd = call("socket", {2, 2 | o_nonblock, 0});
  REQUIRE(fd >= 0);
  struct Address {
    std::uint16_t family, port;
    std::uint32_t ip;
    std::uint8_t zero[8];
  } address{2, 0, 0x0100007f, {}};
  REQUIRE(call("bind", {static_cast<unsigned>(fd), h2g(&address), 16}) == 0);
  std::uint32_t size = 16;
  REQUIRE(call("getsockname", {static_cast<unsigned>(fd), h2g(&address), h2g(&size)}) == 0);
  CHECK(address.family == 2);
  CHECK(address.port != 0);
  CHECK(size == 16);
  CHECK(call("sendto", {static_cast<unsigned>(fd), h2g("data"), 4, 0, h2g(&address), 16}) == 4);
  PollFd pollfd{static_cast<int>(fd), 1, 0};
  REQUIRE(fd_poll(&pollfd, 1, 100) == 1);
  char data[8]{};
  CHECK(call("recv", {static_cast<unsigned>(fd), h2g(data), sizeof(data), 0}) == 4);
  CHECK(std::string(data) == "data");
  CHECK(call("recv", {static_cast<unsigned>(fd), h2g(data), sizeof(data), 0}) == -1);
  CHECK(*thread.thread.errno_location() == err::eagain);
  FdTable::get().close(fd);
  unsigned char ipv6[16]{};
  CHECK(call("inet_pton", {10, h2g("::1"), h2g(ipv6)}) == 1);
  CHECK(ipv6[15] == 1);
}
TEST_CASE("libc B: socketpair messages scanf signal ABI and timers") {
  Thread thread;
  int pair[2];
  REQUIRE(call("socketpair", {1, 1 | o_nonblock, 0, h2g(pair)}) == 0);
  CHECK(fd_write(pair[0], "abc", 3) == 3);
  char bytes[4]{};
  CHECK(fd_read(pair[1], bytes, 3) == 3);
  CHECK(std::string(bytes) == "abc");
  FdTable::get().close(pair[0]);
  CHECK(fd_read(pair[1], bytes, 1) == 0);
  FdTable::get().close(pair[1]);
  char input[] = "123 abcd 0xff 2.5";
  auto stream = call("fmemopen", {h2g(input), sizeof(input) - 1, h2g("r")});
  int n = 0;
  char word[8]{};
  unsigned hex = 0;
  double d = 0;
  CHECK(call("fscanf", {static_cast<GuestAddr>(stream), h2g("%d %7s %x %lf"), h2g(&n), h2g(word),
                        h2g(&hex), h2g(&d)}) == 4);
  CHECK(n == 123);
  CHECK(std::string(word) == "abcd");
  CHECK(hex == 255);
  CHECK(d == 2.5);
  call("fclose", {static_cast<GuestAddr>(stream)});
  struct Action {
    std::uint32_t flags, pad;
    GuestAddr handler;
    std::uint64_t mask;
    GuestAddr restorer;
  } action{0x10000004, 0, 0x123456, 0x1000, 0}, old{};
  CHECK(call("sigaction", {10, h2g(&action), 0}) == 0);
  CHECK(call("sigaction", {10, 0, h2g(&old)}) == 0);
  CHECK(old.handler == action.handler);
  CHECK(old.flags == action.flags);
  CHECK(old.mask == action.mask);
  std::uint64_t mask = 1ull << 11;
  std::uint64_t previous_mask;
  signals::set_mask(thread.thread, 0, &mask, &previous_mask);
  signals::send_to_thread(thread.thread.tid(), {12});
  int signo = 0;
  CHECK(call("sigwait", {h2g(&mask), h2g(&signo)}) == 0);
  CHECK(signo == 12);
  CHECK(thread.thread.pending_signals == 0);
  signals::set_mask(thread.thread, 2, &previous_mask, nullptr);
  GuestAddr timer = 0;
  struct Event {
    std::uint64_t value;
    int signal, notify;
    char reserved[48];
  } ev{0, 0, 1, {}};
  REQUIRE(call("timer_create", {1, h2g(&ev), h2g(&timer)}) == 0);
  Timespec spec[2] = {{0, 0}, {1, 0}}, remaining[2]{};
  CHECK(call("timer_settime", {timer, 0, h2g(spec), 0}) == 0);
  CHECK(call("timer_gettime", {timer, h2g(remaining)}) == 0);
  CHECK(remaining[1].sec == 0);
  CHECK(remaining[1].nsec > 0);
  CHECK(call("timer_delete", {timer}) == 0);
}
TEST_CASE("libc B: bionic resolver data and error numbering") {
  Thread thread;
  struct Info {
    int flags, family, type, protocol;
    std::uint32_t size, pad;
    GuestAddr canonical, address, next;
  } hints{4 | 8, 10, 2, 17, 0, 0, 0, 0, 0};
  GuestAddr out = 0;
  REQUIRE(call("getaddrinfo", {h2g("::1"), h2g("2345"), h2g(&hints), h2g(&out)}) == 0);
  REQUIRE(out != 0);
  auto* info = g2h<Info>(out);
  CHECK(info->family == 10);
  CHECK(info->size == 28);
  REQUIRE(info->address != 0);
  CHECK(*g2h<std::uint16_t>(info->address) == 10);
  CHECK(g2h<unsigned char>(info->address)[23] == 1);
  call("freeaddrinfo", {out});
  hints.family = 999;
  CHECK(call("getaddrinfo", {h2g("localhost"), 0, h2g(&hints), h2g(&out)}) == 5);
  CHECK(out == 0);
  GuestAddr interfaces = 0;
  CHECK(call("getifaddrs", {h2g(&interfaces)}) == 0);
  REQUIRE(interfaces != 0);
  call("freeifaddrs", {interfaces});
}
TEST_CASE("libc B: memfd seals positional I/O and UTF-8 streams") {
  Thread thread;
  int fd = call("memfd_create", {h2g("test"), 3});
  REQUIRE(fd >= 0);
  CHECK(fd_write(fd, "abcd", 4) == 4);
  CHECK(fd_fcntl(fd, 1033, 4) == 0);
  CHECK(fd_write(fd, "e", 1) == -err::eperm);
  CHECK(fd_fcntl(fd, 1034, 0) == 4);
  CHECK(fd_fcntl(fd, 1033, 8) == 0);
  CHECK(fd_pwrite(fd, "x", 1, 0) == -err::eperm);
  char bytes[4];
  CHECK(fd_pread(fd, bytes, 4, 0) == 4);
  CHECK(std::string(bytes, 4) == "abcd");
  FdTable::get().close(fd);
  char buffer[32]{};
  auto file = call("fmemopen", {h2g(buffer), 32, h2g("w+")});
  REQUIRE(file);
  CHECK(call("fputwc", {0x1f680, static_cast<GuestAddr>(file)}) == 0x1f680);
  CHECK(call("fseek", {static_cast<GuestAddr>(file), 0, 0}) == 0);
  CHECK(call("getwc", {static_cast<GuestAddr>(file)}) == 0x1f680);
  CHECK(call("fclose", {static_cast<GuestAddr>(file)}) == 0);
}
TEST_CASE(
    "libc B: epoll edges rearm after drain and preserve duplicate "
    "descriptions") {
  Thread thread;
  int pipes[2];
  REQUIRE(call("pipe2", {h2g(pipes), o_nonblock}) == 0);
  auto ep = call("epoll_create1", {0});
  struct Event {
    std::uint32_t events, pad;
    std::uint64_t data;
  } event{0x80000001u, 0, 77}, out[2]{};
  REQUIRE(call("epoll_ctl",
               {static_cast<unsigned>(ep), 1, static_cast<unsigned>(pipes[0]), h2g(&event)}) == 0);
  REQUIRE(fd_write(pipes[1], "ab", 2) == 2);
  CHECK(call("epoll_wait", {static_cast<unsigned>(ep), h2g(out), 2, 0}) == 1);
  CHECK(call("epoll_wait", {static_cast<unsigned>(ep), h2g(out), 2, 0}) == 0);
  char byte;
  REQUIRE(fd_read(pipes[0], &byte, 1) == 1);
  CHECK(call("epoll_wait", {static_cast<unsigned>(ep), h2g(out), 2, 0}) == 0);
  REQUIRE(fd_read(pipes[0], &byte, 1) == 1);
  REQUIRE(fd_write(pipes[1], "c", 1) == 1);
  CHECK(call("epoll_wait", {static_cast<unsigned>(ep), h2g(out), 2, 0}) == 1);
  auto duplicate = FdTable::get().duplicate(pipes[0], 0, false);
  FdTable::get().close(pipes[0]);
  int replacement[2];
  REQUIRE(call("pipe2", {h2g(replacement), o_nonblock}) == 0);
  REQUIRE(replacement[0] == pipes[0]);
  event.data = 88;
  REQUIRE(call("epoll_ctl", {static_cast<unsigned>(ep), 1, static_cast<unsigned>(replacement[0]),
                             h2g(&event)}) == 0);
  REQUIRE(fd_write(replacement[1], "x", 1) == 1);
  CHECK(call("epoll_wait", {static_cast<unsigned>(ep), h2g(out), 2, 0}) == 1);
  CHECK(out[0].data == 88);
  for (int fd : {replacement[0], replacement[1], pipes[1], duplicate, static_cast<int>(ep)})
    FdTable::get().close(fd);
}

TEST_CASE("libc B: socket timeouts WAITALL PEEK and half-close") {
  Thread thread;
  int pair[2];
  REQUIRE(call("socketpair", {1, 1, 0, h2g(pair)}) == 0);
  Timeval timeout{0, 3000};
  REQUIRE(call("setsockopt", {static_cast<unsigned>(pair[0]), 1, 20, h2g(&timeout), 16}) == 0);
  char data[8]{};
  CHECK(call("recv", {static_cast<unsigned>(pair[0]), h2g(data), 1, 0}) == -1);
  CHECK(*thread.thread.errno_location() == err::eagain);
  REQUIRE(fd_write(pair[1], "ab", 2) == 2);
  CHECK(call("recv", {static_cast<unsigned>(pair[0]), h2g(data), 2, 2}) == 2);
  CHECK(std::string(data, 2) == "ab");
  CHECK(call("recv", {static_cast<unsigned>(pair[0]), h2g(data), 2, 0}) == 2);
  timeout = {0, 0};
  REQUIRE(call("setsockopt", {static_cast<unsigned>(pair[0]), 1, 20, h2g(&timeout), 16}) == 0);
  std::thread writer([&] {
    Thread t(1070);
    REQUIRE(fd_write(pair[1], "12", 2) == 2);
    sleep_until(t.thread, monotonic_ns() + 3000000);
    REQUIRE(fd_write(pair[1], "34", 2) == 2);
    CHECK(call("shutdown", {static_cast<unsigned>(pair[1]), 1}) == 0);
  });
  CHECK(call("recv", {static_cast<unsigned>(pair[0]), h2g(data), 4, 0x100}) == 4);
  CHECK(std::string(data, 4) == "1234");
  CHECK(call("recv", {static_cast<unsigned>(pair[0]), h2g(data), 1, 0}) == 0);
  writer.join();
  FdTable::get().close(pair[0]);
  FdTable::get().close(pair[1]);
}
