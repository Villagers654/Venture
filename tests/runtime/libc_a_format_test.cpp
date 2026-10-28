#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <limits>

#include "hle/libc/libc_a.h"
#include "libc_a_test_support.h"
using namespace venture;
namespace {
struct Args : hle::libc::PrintfArgs {
  std::vector<std::uint64_t> ints;
  std::vector<double> floats;
  std::size_t i = 0, j = 0;
  Args(std::initializer_list<std::uint64_t> x = {}, std::initializer_list<double> y = {})
      : ints(x), floats(y) {}
  std::uint64_t next_int() override { return ints.at(i++); }
  double next_double() override { return floats.at(j++); }
};
std::string fmt(const char* f, std::initializer_list<std::uint64_t> x = {},
                std::initializer_list<double> y = {}) {
  Args a(x, y);
  return hle::libc::format_printf(f, a);
}
}  // namespace
TEST_CASE("libc A printf integer, pointer, flags, lengths and positional arguments") {
  test::LibcFixture f;
  CHECK(fmt("[%p]", {0}) == "[0x0]");
  CHECK(fmt("%#08x", {42}) == "0x00002a");
  CHECK(fmt("%+.4d", {static_cast<std::uint64_t>(-12)}) == "-0012");
  CHECK(fmt("%.0u|%#.0o|%#.0x", {0, 0, 0}) == "|0|");
  CHECK(fmt("%2$*1$d", {6, 42}) == "    42");
  CHECK(fmt("%1$d %1$x", {42}) == "42 2a");
  CHECK(fmt("%3$*1$.*2$f", {10, 2}, {1.25}) == "      1.25");
  CHECK(fmt("%hhd %hhu %hd", {255, 511, 65535}) == "-1 255 -1");
  CHECK(fmt("%zu/%td/%jd/%lld", {0x100000000ull, static_cast<std::uint64_t>(-9), INT64_MAX, 1ull << 63}) ==
        "4294967296/-9/9223372036854775807/-9223372036854775808");
  CHECK(fmt("%*d", {static_cast<std::uint64_t>(-5), 12}) == "12   ");
  CHECK(fmt("%-08d", {1}) == "1       ");
}
TEST_CASE("libc A printf bionic floating point and UTF-32 wide strings") {
  test::LibcFixture f;
  CHECK(fmt("%.0f %.0f", {}, {2.5, 3.5}) == "2 4");
  CHECK(fmt("%.2f", {}, {1.125}) == "1.12");
  CHECK(fmt("%a %A", {}, {1.5, 0.0}) == "0x1.8p+0 0X0P+0");
  CHECK(fmt("%a", {}, {std::numeric_limits<double>::denorm_min()}) == "0x1p-1074");
  CHECK(fmt("%#.0f %#.4g", {}, {2, 12}) == "2. 12.00");
  CHECK(fmt("%.2e %.4g", {}, {1234.5, 0.000012345}) == "1.23e+03 1.234e-05");
  CHECK(fmt("%+f %f %f", {}, {INFINITY, NAN, -NAN}) == "+inf nan -nan");
  CHECK(fmt("%08f", {}, {INFINITY}) == "     inf");
  CHECK(fmt("%.1f", {}, {-0.0}) == "-0.0");
  std::uint32_t text[] = {0x41, 0x20ac, 0x1f600, 0};
  CHECK(fmt("%.4ls", {h2g(text)}) == "A\xe2\x82\xac");
  CHECK(fmt("%.3ls", {h2g(text)}) == "A");
  CHECK(fmt("%lc", {0x1f600}) == "\xf0\x9f\x98\x80");
  CHECK(fmt("%.3s", {h2g("abcdef")}) == "abc");
  hle::libc::set_error(hle::err::kEACCES);
  CHECK(fmt("%m") == "Permission denied");
  int n = -1;
  CHECK(fmt("abc%nxyz", {h2g(&n)}) == "abcxyz");
  CHECK(n == 3);
}
TEST_CASE("libc A snprintf VarArgs registers, stack, truncation and FORTIFY") {
  test::LibcFixture f;
  char out[64];
  auto n = f.invoke("snprintf", {h2g(out), sizeof(out), h2g("%d %d %d %d %d %d"), 1, 2, 3, 4, 5, 6});
  CHECK(n == 11);
  CHECK(std::string(out) == "1 2 3 4 5 6");
  f.real_arg(0, 1.25);
  CHECK(f.invoke("snprintf", {h2g(out), 5, h2g("%.3f")}) == 5);
  CHECK(std::string(out) == "1.25");
  CHECK(f.invoke("snprintf", {0, 0, h2g("%d"), 1234}) == 4);
  std::memset(out, '!', sizeof(out));
  CHECK_THROWS_AS(f.invoke_fatal("__snprintf_chk", {h2g(out), 10, 0, 4, h2g("test")}), test::GuestCrash);
  CHECK(out[0] == '!');
  CHECK(f.invoke("snprintf", {h2g(out), sizeof(out), h2g("%2147483648d"), 1}) == UINT64_MAX);
  CHECK(*f.thread.errno_location() == hle::err::kEOVERFLOW);
}
TEST_CASE("libc A vsnprintf AAPCS64 va_list independently consumes GP and SIMD saves") {
  test::LibcFixture f;
  char out[128];
  std::uint64_t gp[] = {17, h2g("quest")};
  alignas(16) Vec128 fp[] = {{std::bit_cast<std::uint64_t>(2.5), 0}, hle::libc::parse_quad("1.125", nullptr)};
  alignas(16) std::uint64_t stack[] = {99, 0};
  hle::GuestVaListStruct list{h2g(stack), h2g(gp + 2), h2g(fp + 2), -16, -32};
  auto original = list;
  auto result = f.invoke("vsnprintf", {h2g(out), sizeof(out), h2g("%d %.1f %s %.3Lf %d"), h2g(&list)});
  CHECK(result == 21);
  CHECK(std::string(out) == "17 2.5 quest 1.125 99");
  CHECK(std::memcmp(&list, &original, sizeof(list)) == 0);
  // Stack-spilled binary128 must round SP up to 16 bytes.
  alignas(16) std::array<std::uint64_t, 6> spill{};
  auto q = hle::libc::parse_quad("3.5", nullptr);
  std::memcpy(spill.data() + 2, &q, 16);
  spill[4] = 23;
  list = {h2g(spill.data() + 1), 0, 0, 0, 0};
  f.invoke("vsnprintf", {h2g(out), sizeof(out), h2g("%.1Lf %d"), h2g(&list)});
  CHECK(std::string(out) == "3.5 23");
}
TEST_CASE("libc A sscanf assignments, scansets, widths, wide conversions and va_list") {
  test::LibcFixture f;
  std::int64_t integer = 0;
  double value = 0;
  char word[16]{};
  int n = -1;
  CHECK(f.invoke("sscanf", {h2g("-4294967297 0x1.8p+2 abc123!"), h2g("%lld %lf %6[a-z0-9]%n"), h2g(&integer),
                            h2g(&value), h2g(word), h2g(&n)}) == 3);
  CHECK(integer == -4294967297ll);
  CHECK(value == 6);
  CHECK(std::string(word) == "abc123");
  CHECK(n == 27);
  CHECK(f.invoke("sscanf", {h2g(""), h2g("%d"), h2g(&n)}) == UINT64_MAX);
  CHECK(f.invoke("sscanf", {h2g("xyz"), h2g("%d"), h2g(&n)}) == 0);
  CHECK(f.invoke("sscanf", {h2g("42"), h2g("%*d%d"), h2g(&n)}) == UINT64_MAX);
  CHECK(f.invoke("sscanf", {h2g("ab"), h2g("%3c"), h2g(word)}) == 1);
  CHECK(std::string(word, 2) == "ab");
  std::uint32_t wide[8]{};
  CHECK(f.invoke("sscanf", {h2g("\xf0\x9f\x98\x80!"), h2g("%lc"), h2g(wide)}) == 1);
  CHECK(wide[0] == 0x1f600);
  std::uint64_t pointers[] = {h2g(&integer), h2g(word)};
  hle::GuestVaListStruct list{0, h2g(pointers + 2), 0, -16, 0};
  CHECK(f.invoke("vsscanf", {h2g("77 hello"), h2g("%lld %s"), h2g(&list)}) == 2);
  CHECK(integer == 77);
  CHECK(std::string(word) == "hello");
}
