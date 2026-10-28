#include "libc_b/test.h"
#include "core/error.h"
#include "core/address_space.h"
#include "libc_a_test_support.h"
#include <array>
#include <algorithm>
#include <future>

#include "../async_wait.h"

using namespace venture;
using namespace venture::test::libc_b;

namespace {
struct Cookie { std::int64_t position = 0; int closed = 0; };
int cookie_read(Cookie* cookie, char* out, int n) {
  auto count = std::min(n, 3);
  std::memcpy(out, "abc", count);
  cookie->position += count;
  return count;
}
std::int64_t cookie_seek(Cookie* cookie, std::int64_t offset, int whence) {
  if (whence == 0) cookie->position = offset;
  else if (whence == 1) cookie->position += offset;
  else { GuestThread::current()->set_errno(err::kEINVAL); return -1; }
  return cookie->position;
}
int cookie_close(Cookie* cookie) { ++cookie->closed; return 0; }
}

TEST_CASE("funopen64 uses 64-bit seek callbacks and fread_unlocked bypasses flockfile") {
  Thread thread(1172);
  Cookie cookie;
  auto read = hle::make_thunk("test", "cookie read", hle::wrap<&cookie_read>());
  auto seek = hle::make_thunk("test", "cookie seek", hle::wrap<&cookie_seek>());
  auto close = hle::make_thunk("test", "cookie close", hle::wrap<&cookie_close>());
  CHECK(call("funopen64", {h2g(&cookie), 0, 0, seek, close}) == 0);
  CHECK(*thread.thread.errno_location() == err::kEINVAL);
  auto f = static_cast<GuestAddr>(call("funopen64", {h2g(&cookie), read, 0, seek, close}));
  REQUIRE(f != 0);
  REQUIRE(call("setvbuf", {f, 0, 2, 0}) == 0); // _IONBF, exercise callbacks directly
  const std::uint64_t large = (1ull << 33) + 17;
  REQUIRE(call("fseeko64", {f, large, 0}) == 0);
  CHECK(cookie.position == large);
  CHECK(call("ftello64", {f}) == large);
  call("flockfile", {f});
  auto worker = std::async(std::launch::async, [f] {
    Thread other(1173);
    char out[4]{};
    CHECK(call("fread_unlocked", {h2g(out), 1, 3, f}) == 3);
    return std::string(out);
  });
  auto status = worker.wait_for(venture::test::hang_timeout);
  call("funlockfile", {f}); // release even on failure, so a regression cannot hang
  INFO("fread_unlocked finished while flockfile was held=", status == std::future_status::ready);
  CHECK(status == std::future_status::ready);
  CHECK(worker.get() == "abc");
  CHECK(cookie.position == large + 3);
  CHECK(call("fclose", {f}) == 0);
  CHECK(cookie.closed == 1);
}

TEST_CASE("fortified fgets checks bounds before reading the stream") {
  test::LibcFixture fixture;
  char contents[] = "first\nsecond\n";
  auto f = fixture.invoke("fmemopen", {h2g(contents), sizeof(contents) - 1, h2g("r")});
  REQUIRE(f != 0);
  char out[8]{};
  CHECK(fixture.invoke("__fgets_chk", {h2g(out), 7, f, sizeof(out)}) == h2g(out));
  CHECK(std::string(out) == "first\n");
  CHECK_THROWS_AS(fixture.invoke_fatal("__fgets_chk", {h2g(out), 9, f, sizeof(out)}), test::GuestCrash);
  CHECK_THROWS_AS(fixture.invoke_fatal("__fgets_chk", {h2g(out), UINT32_MAX, f, sizeof(out)}), test::GuestCrash);
  CHECK(fixture.invoke("__fgets_chk", {h2g(out), 8, f, sizeof(out)}) == h2g(out));
  CHECK(std::string(out) == "second\n");
  CHECK(fixture.invoke("fclose", {f}) == 0);
}

TEST_CASE("sbrk grows and trims contiguous zeroed pages without replacing mappings") {
  Thread thread(1175);
  const auto initial = static_cast<GuestAddr>(call("sbrk", {0}));
  REQUIRE(initial != UINT64_MAX);
  auto& space = AddressSpace::get();
  const auto unrelated = space.allocate(kGuestPageSize, kProtRead | kProtWrite, "[sbrk-unrelated]");
  REQUIRE(unrelated != 0);
  const auto grown = initial + 3 * kGuestPageSize + 7;
  REQUIRE(call("sbrk", {grown - initial}) == static_cast<std::int64_t>(initial));
  CHECK(call("sbrk", {0}) == static_cast<std::int64_t>(grown));
  auto* memory = g2h<unsigned char>(initial);
  CHECK(memory[2 * kGuestPageSize] == 0);
  memory[0] = 42;
  memory[2 * kGuestPageSize] = 99;
  REQUIRE(call("brk", {initial + kGuestPageSize}) == 0);
  MemoryRegion region;
  REQUIRE(space.find(initial + 2 * kGuestPageSize, &region));
  CHECK(region.prot == kProtNone);
  CHECK(region.name == "[heap]");
  const auto while_shrunk = space.allocate(kGuestPageSize, kProtRead | kProtWrite, "[sbrk-unrelated]");
  REQUIRE(while_shrunk != 0);
  CHECK(while_shrunk != initial + 2 * kGuestPageSize);
  REQUIRE(call("brk", {grown}) == 0);
  CHECK(memory[0] == 42);
  CHECK(memory[2 * kGuestPageSize] == 0);
  const auto occupied = align_up(grown, kGuestPageSize);
  REQUIRE(AddressSpace::get().map(occupied, kGuestPageSize, kProtRead | kProtWrite,
      kMapPrivate | kMapAnonymous | kMapFixed, nullptr, 0, "[sbrk-test]") == static_cast<std::int64_t>(occupied));
  *g2h<int>(occupied) = 1234;
  CHECK(call("sbrk", {kGuestPageSize}) == -1);
  CHECK(*thread.thread.errno_location() == err::kENOMEM);
  CHECK(*g2h<int>(occupied) == 1234);
  CHECK(call("sbrk", {0}) == static_cast<std::int64_t>(grown));
  CHECK(call("sbrk", {1ull << 63}) == -1);
  CHECK(*thread.thread.errno_location() == err::kENOMEM);
  CHECK(space.map(occupied, kGuestPageSize, kProtNone,
      kMapPrivate | kMapAnonymous | kMapFixed, nullptr, 0, "[heap]") == static_cast<std::int64_t>(occupied));
  CHECK(call("brk", {initial}) == 0);
  space.free(while_shrunk);
  space.free(unrelated);
}

TEST_CASE("fortified recvfrom checks bounds before consuming socket bytes") {
  test::LibcFixture fixture;
  int fds[2]{};
  REQUIRE(fixture.invoke("socketpair", {1, 1, 0, h2g(fds)}) == 0);
  REQUIRE(fixture.invoke("send", {static_cast<GuestAddr>(fds[0]), h2g("hello"), 5, 0}) == 5);
  char out[6]{};
  CHECK_THROWS_AS(fixture.invoke_fatal("__recvfrom_chk", {static_cast<GuestAddr>(fds[1]), h2g(out), 7, sizeof(out), 0, 0, 0}), test::GuestCrash);
  // MSG_PEEK (2) is argument five, after the FORTIFY object size.
  CHECK(fixture.invoke("__recvfrom_chk", {static_cast<GuestAddr>(fds[1]), h2g(out), 5, sizeof(out), 2, 0, 0}) == 5);
  CHECK(std::string(out) == "hello");
  std::memset(out, 0, sizeof(out));
  CHECK(fixture.invoke("__recvfrom_chk", {static_cast<GuestAddr>(fds[1]), h2g(out), 5, sizeof(out), 0, 0, 0}) == 5);
  CHECK(std::string(out) == "hello");
  CHECK(fixture.invoke("close", {static_cast<GuestAddr>(fds[0])}) == 0);
  CHECK(fixture.invoke("close", {static_cast<GuestAddr>(fds[1])}) == 0);
}
