// Cases follow bionic tests/iconv_test.cpp.
#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <tuple>
#include <utility>

#include "hle/libc/libc_a.h"
#include "libc_a_test_support.h"
using namespace venture;
namespace {
constexpr std::uint64_t kInvalid = UINT64_MAX;
int& guest_errno() { return *hle::libc::error_location(); }
struct Cursor {
  char* in;
  std::uint64_t in_left;
  char* out;
  std::uint64_t out_left;
};
std::uint64_t run(test::LibcFixture& f, std::uint64_t cd, Cursor& c) {
  return f.invoke("iconv", {cd, h2g(&c.in), h2g(&c.in_left), h2g(&c.out), h2g(&c.out_left)});
}
std::uint64_t open(test::LibcFixture& f, const char* dst, const char* src) {
  return f.invoke("iconv_open", {h2g(dst), h2g(src)});
}
// Converts with a fresh descriptor; returns the result and the output bytes.
std::pair<std::uint64_t, std::string> convert(test::LibcFixture& f, const char* dst, const char* src,
                                              std::string input, int* error = nullptr) {
  auto cd = open(f, dst, src);
  REQUIRE(cd != kInvalid);
  char buf[64]{};
  Cursor c{input.data(), input.size(), buf, sizeof(buf)};
  guest_errno() = 0;
  auto r = run(f, cd, c);
  if (error) *error = guest_errno();
  CHECK(f.invoke("iconv_close", {cd}) == 0);
  return {r, std::string(buf, sizeof(buf) - c.out_left)};
}
}  // namespace

TEST_CASE("libc A iconv_open matches TR22 aliases and rejects unknown or stateful encodings") {
  test::LibcFixture f;
  for (auto [dst, src] : {std::pair{"silly", "silly"}, {"silly", "UTF-8"}, {"UTF-8", "silly"},
                          {"UTF-8", "utf-80"}, {"UTF-8", "ut80"}, {"UTF-8", "UTF-16"}, {"UTF-32", "UTF-8"},
                          {"UTF-8", "UTF-8//IGNORE"}, {"UTF-8//BOGUS", "UTF-8"}}) {
    guest_errno() = 0;
    CHECK(open(f, dst, src) == kInvalid);
    CHECK(guest_errno() == hle::err::kEINVAL);
  }
  for (auto [dst, src] : {std::pair{"UTF-8", "utf8"}, {"UTF-8", "u.t.f-008"}, {"US-ASCII//TRANSLIT", "wchar_t"},
                          {"ascii//IGNORE", "UTF_16le"}, {"UTF-32BE", "utf 32 le"}}) {
    auto cd = open(f, dst, src);
    REQUIRE(cd != kInvalid);
    CHECK(f.invoke("iconv_close", {cd}) == 0);
  }
  guest_errno() = 0;
  CHECK(static_cast<std::int32_t>(f.invoke("iconv_close", {kInvalid})) == -1);
  CHECK(guest_errno() == hle::err::kEBADF);
  Cursor c{};
  CHECK(run(f, kInvalid, c) == kInvalid);
  CHECK(guest_errno() == hle::err::kEBADF);
}

TEST_CASE("libc A iconv round trips UTF-8 through every encoding") {
  test::LibcFixture f;
  const std::string utf8 = "$\xe2\x82\xac\xf0\x90\x90\xb7";  // U+0024, U+20AC, U+10437
  for (auto [enc, bytes] : {std::pair<const char*, std::string>{"utf8", utf8},
                            {"utf16be", std::string("\x00\x24\x20\xac\xd8\x01\xdc\x37", 8)},
                            {"utf16le", std::string("\x24\x00\xac\x20\x01\xd8\x37\xdc", 8)},
                            {"utf32be", std::string("\x00\x00\x00\x24\x00\x00\x20\xac\x00\x01\x04\x37", 12)},
                            {"utf32le", std::string("\x24\x00\x00\x00\xac\x20\x00\x00\x37\x04\x01\x00", 12)},
                            {"wchar_t", std::string("\x24\x00\x00\x00\xac\x20\x00\x00\x37\x04\x01\x00", 12)}}) {
    CAPTURE(enc);
    auto [r, out] = convert(f, enc, "UTF-8", utf8);
    CHECK(r == 0);
    CHECK(out == bytes);
    auto [r2, back] = convert(f, "UTF-8", enc, bytes);
    CHECK(r2 == 0);
    CHECK(back == utf8);
  }
  auto [r, out] = convert(f, "ascii//TRANSLIT", "UTF-8", utf8);
  CHECK(r == 2);
  CHECK(out == "$??");
  // A NUL converts like any other character instead of stalling the loop.
  auto [rn, nul] = convert(f, "UTF-16LE", "UTF-8", std::string("a\0b", 3));
  CHECK(rn == 0);
  CHECK(nul == std::string("a\0\0\0b\0", 6));
}

TEST_CASE("libc A iconv lossy modes, malformed and incomplete input, E2BIG progress") {
  test::LibcFixture f;
  const std::string lossy = "a\xd9\xa6\xe1\x84\x80z";  // a U+0666 U+1100 z
  int error = 0;
  auto [rt, translit] = convert(f, "ASCII//TRANSLIT", "UTF-8", lossy, &error);
  CHECK(rt == 2);
  CHECK(translit == "a??z");
  auto [ri, ignore] = convert(f, "ASCII//IGNORE", "UTF-8", lossy, &error);
  CHECK(ri == kInvalid);
  CHECK(error == hle::err::kEILSEQ);
  CHECK(ignore == "az");
  // //IGNORE skipping the final character ends the call.
  auto [rt2, tail] = convert(f, "UTF-8//IGNORE", "ASCII", "a\xac", &error);
  CHECK(rt2 == kInvalid);
  CHECK(error == hle::err::kEILSEQ);
  CHECK(tail == "a");
  auto [rb, bad] = convert(f, "ASCII//TRANSLIT", "UTF-8", "a\x82z", &error);
  CHECK(rb == 1);
  CHECK(bad == "a?z");

  auto cd = open(f, "ASCII", "UTF-8");
  REQUIRE(cd != kInvalid);
  char buf[16]{};
  std::string input = lossy;
  Cursor c{input.data(), input.size(), buf, sizeof(buf)};
  guest_errno() = 0;
  CHECK(run(f, cd, c) == kInvalid);
  CHECK(guest_errno() == hle::err::kEILSEQ);
  CHECK(c.in_left == 6);
  CHECK(c.out_left == sizeof(buf) - 1);
  CHECK(f.invoke("iconv_close", {cd}) == 0);

  cd = open(f, "UTF-8", "UTF-8");
  REQUIRE(cd != kInvalid);
  input = "a\xd9z";
  c = {input.data(), input.size(), buf, sizeof(buf)};
  CHECK(run(f, cd, c) == kInvalid);
  CHECK(guest_errno() == hle::err::kEILSEQ);
  CHECK(*c.in == '\xd9');
  ++c.in;
  --c.in_left;
  CHECK(run(f, cd, c) == 0);
  CHECK(guest_errno() == 0);
  CHECK(std::string(buf, 2) == "az");
  input = "a\xd9";
  c = {input.data(), input.size(), buf, sizeof(buf)};
  CHECK(run(f, cd, c) == kInvalid);
  CHECK(guest_errno() == hle::err::kEINVAL);
  CHECK(*c.in == '\xd9');
  CHECK(c.in_left == 1);

  input = "abc";
  std::memset(buf, 0, sizeof(buf));
  c = {input.data(), input.size(), buf, 1};
  CHECK(run(f, cd, c) == kInvalid);
  CHECK(guest_errno() == hle::err::kE2BIG);
  CHECK(c.in_left == 2);
  CHECK(c.out_left == 0);
  CHECK(run(f, cd, c) == kInvalid);
  CHECK(c.in_left == 2);
  c.out_left = 1;
  CHECK(run(f, cd, c) == kInvalid);
  CHECK(c.in_left == 1);
  c.out_left = 1;
  CHECK(run(f, cd, c) == 0);
  CHECK(c.in_left == 0);
  CHECK(std::string(buf) == "abc");

  // POSIX initial shift state: inbuf null, *inbuf null, and (like glibc) a null in_left too.
  c = {nullptr, 0, buf, sizeof(buf)};
  CHECK(run(f, cd, c) == 0);
  CHECK(c.out_left == sizeof(buf));
  CHECK(f.invoke("iconv", {cd, 0, h2g(&c.in_left), h2g(&c.out), h2g(&c.out_left)}) == 0);
  CHECK(f.invoke("iconv", {cd, 0, 0, h2g(&c.out), h2g(&c.out_left)}) == 0);
  CHECK(c.out_left == sizeof(buf));
  CHECK(f.invoke("iconv_close", {cd}) == 0);
}

TEST_CASE("libc A iconv EILSEQ and EINVAL per source encoding") {
  test::LibcFixture f;
  for (auto [expected, enc, bytes] : {std::tuple<int, const char*, std::string>{hle::err::kEILSEQ, "ASCII", "\xac"},
         {hle::err::kEILSEQ, "utf8", "\x82"},
         {hle::err::kEILSEQ, "utf8", "\xe2\xe2\x82"},
         {hle::err::kEILSEQ, "utf16be", "\xdc\x37\xd8\x01"},
         {hle::err::kEILSEQ, "utf16le", "\x37\xdc\x01\xd8"},
         {hle::err::kEINVAL, "utf8", "\xe2\x82"},
         {hle::err::kEINVAL, "utf16be", std::string("\x00", 1)},
         {hle::err::kEINVAL, "utf16be", "\xd8\x01"},
         {hle::err::kEINVAL, "utf16be", "\xd8\x01\xdc"},
         {hle::err::kEINVAL, "utf16le", "\x24"},
         {hle::err::kEINVAL, "utf16le", "\x01\xd8"},
         {hle::err::kEINVAL, "utf16le", "\x01\xd8\x37"},
         {hle::err::kEINVAL, "utf32be", std::string("\x00\x00\x00", 3)},
         {hle::err::kEINVAL, "utf32le", std::string("\x24\x00\x00", 3)}}) {
    CAPTURE(enc);
    int error = 0;
    CHECK(convert(f, "wchar_t", enc, bytes, &error).first == kInvalid);
    CHECK(error == expected);
  }
}
