#include <doctest/doctest.h>

#include <cstring>

#include "hle/libc/libc_a.h"
#include "hle/libc/time_format.h"
#include "libc_a_test_support.h"
using namespace venture;
TEST_CASE("libc A UTF-8 split state, overlong rejection, size queries and UTF-32") {
  test::LibcFixture f;
  std::uint64_t state = 0;
  std::uint32_t cp = 0;
  char out[16]{};
  CHECK(f.invoke("mbrtowc", {h2g(&cp), h2g("\xf0\x9f"), 2, h2g(&state)}) == UINT64_MAX - 1);
  CHECK(f.invoke("mbsinit", {h2g(&state)}) == 0);
  CHECK(f.invoke("mbrtowc", {h2g(&cp), h2g("\x98\x80"), 2, h2g(&state)}) == 2);
  CHECK(cp == 0x1f600);
  CHECK(state == 0);
  CHECK(f.invoke("wcrtomb", {h2g(out), cp, h2g(&state)}) == 4);
  CHECK(std::string(out, 4) == "\xf0\x9f\x98\x80");
  CHECK(f.invoke("mbrtowc", {h2g(&cp), h2g("\xc0\x80"), 2, h2g(&state)}) == UINT64_MAX);
  CHECK(*hle::libc::error_location() == hle::err::kEILSEQ);
  CHECK(state == 0);
  CHECK(f.invoke("wcrtomb", {h2g(out), 0xd800, h2g(&state)}) == UINT64_MAX);
  CHECK(f.invoke("mbrtowc", {h2g(&cp), h2g("\xf0\x80"), 2, h2g(&state)}) == UINT64_MAX);
  CHECK(state == 0);
  const char* input = "A\xe2\x82\xac";
  const char* source = input;
  std::uint32_t wide[4]{};
  CHECK(f.invoke("mbsrtowcs", {0, h2g(&source), 0, h2g(&state)}) == 2);
  CHECK(source == input);
  CHECK(f.invoke("mbsrtowcs", {h2g(wide), h2g(&source), 4, h2g(&state)}) == 2);
  CHECK(source == nullptr);
  CHECK(wide[1] == 0x20ac);
  const std::uint32_t* wide_source = wide;
  CHECK(f.invoke("wcsnrtombs", {h2g(out), h2g(&wide_source), 4, 2, h2g(&state)}) == 1);
  CHECK(wide_source == wide + 1);
}
TEST_CASE("libc A UTC normalization, leap years, ISO weeks and strptime") {
  test::LibcFixture f;
  hle::libc::GuestTm tm{};
  std::int64_t value = -1;
  char out[128];
  CHECK(f.invoke("gmtime_r", {h2g(&value), h2g(&tm)}) == h2g(&tm));
  CHECK(tm.year == 69);
  CHECK(tm.mon == 11);
  CHECK(tm.mday == 31);
  CHECK(tm.sec == 59);
  CHECK(tm.gmtoff == 0);
  f.invoke("strftime", {h2g(out), sizeof(out), h2g("%F %T %a %j %z %Z"), h2g(&tm)});
  CHECK(std::string(out) == "1969-12-31 23:59:59 Wed 365 +0000 UTC");
  tm = {};
  tm.year = 100;
  tm.mon = 1;
  tm.mday = 29;
  CHECK(f.invoke("timegm", {h2g(&tm)}) == 951782400);
  CHECK(tm.yday == 59);
  CHECK(tm.wday == 2);
  tm.mon = 12;
  tm.mday = 1;
  f.invoke("timegm", {h2g(&tm)});
  CHECK(tm.year == 101);
  CHECK(tm.mon == 0);
  const char* input = "2021-01-01 11:59:58 PM +0530 rest";
  auto end = f.invoke("strptime", {h2g(input), h2g("%F %I:%M:%S %p %z"), h2g(&tm)});
  REQUIRE(end != 0);
  CHECK(std::string(g2h<char>(end)) == " rest");
  CHECK(tm.hour == 23);
  CHECK(tm.gmtoff == 19800);
  f.invoke("strftime", {h2g(out), sizeof(out), h2g("%G-W%V-%u"), h2g(&tm)});
  CHECK(std::string(out) == "2020-W53-5");
  CHECK(f.invoke("strftime", {h2g(out), 4, h2g("%Y"), h2g(&tm)}) == 0);
  value = 0;
  f.invoke("gmtime_r", {h2g(&value), h2g(&tm)});
  f.invoke("asctime_r", {h2g(&tm), h2g(out)});
  CHECK(std::string(out) == "Thu Jan  1 00:00:00 1970\n");
  CHECK(f.invoke("strptime", {h2g("2020-W53-5"), h2g("%G-W%V-%u"), h2g(&tm)}) != 0);
  CHECK(tm.year == 121);
  CHECK(tm.mon == 0);
  CHECK(tm.mday == 1);
}
TEST_CASE("libc A wide string operations and bounded FORTIFY source checks") {
  test::LibcFixture f;
  std::uint32_t source[] = {0x10000, 0x12345, 0}, out[8]{};
  CHECK(f.invoke("wcslen", {h2g(source)}) == 2);
  CHECK(f.invoke("wcscpy", {h2g(out), h2g(source)}) == h2g(out));
  CHECK(out[1] == 0x12345);
  CHECK(f.invoke("wmemchr", {h2g(source), 0x12345, 3}) == h2g(source + 1));
  char bytes[8] = "abcdef";
  CHECK(f.invoke("__memmove_chk", {h2g(bytes + 1), h2g(bytes), 5, 7}) == h2g(bytes + 1));
  CHECK(std::string(bytes) == "aabcde");
  CHECK_THROWS_AS(f.invoke_fatal("__strlen_chk", {h2g(bytes), 3}), test::GuestCrash);
  CHECK(f.invoke("strerror_r", {12345, h2g(bytes), 8}) == UINT64_MAX);
  CHECK(*hle::libc::error_location() == hle::err::kERANGE);
  hle::libc::set_error(17);
  CHECK(f.invoke("__gnu_strerror_r", {1, h2g(bytes), 8}) == h2g(bytes));
  CHECK(std::string(bytes) == "Operati");
  CHECK(*hle::libc::error_location() == 17);
  char bounded[3] = {'x', 'y', 'z'};
  CHECK_THROWS_AS(f.invoke_fatal("__strncpy_chk2", {h2g(bytes), h2g(bounded), 4, 8, 3}), test::GuestCrash);
}
