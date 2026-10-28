#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include "hle/libc/libc_a.h"
#include "hle/libz/guest_zlib.h"
#include "libc_a_test_support.h"
using namespace venture;
using venture::hle::libc::GuestZStream;
namespace {
std::uint64_t z(test::LibcFixture& f, const char* name, std::initializer_list<std::uint64_t> args = {}) {
  return f.invoke(name, args, "libz.so");
}

}  // namespace
TEST_CASE("libc A zlib 112-byte LP64 stream, round trip and 64-bit counters") {
  test::LibcFixture f;
  auto version = z(f, "zlibVersion");
  GuestZStream stream{};
  std::vector<unsigned char> input(200000);
  for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<unsigned char>(i % 251);
  std::vector<unsigned char> compressed(z(f, "compressBound", {input.size()}));
  CHECK(z(f, "deflateInit_", {h2g(&stream), 6, version, 112}) == 0);
  REQUIRE(stream.state != nullptr);
  stream.next_in = input.data();
  stream.avail_in = static_cast<unsigned>(input.size());
  stream.next_out = compressed.data();
  stream.avail_out = static_cast<unsigned>(compressed.size());
  stream.total_in = 0x100000000ull;
  stream.total_out = 0x200000000ull;
  CHECK(z(f, "deflate", {h2g(&stream), 4}) == 1);
  CHECK(stream.total_in == 0x100000000ull + input.size());
  auto produced = stream.total_out - 0x200000000ull;
  CHECK(produced > 0);
  CHECK(produced < compressed.size());
  CHECK(z(f, "deflateEnd", {h2g(&stream)}) == 0);
  CHECK(stream.state == nullptr);
  stream = {};
  CHECK(z(f, "inflateInit_", {h2g(&stream), version, 112}) == 0);
  std::vector<unsigned char> output(input.size());
  stream.next_in = compressed.data();
  stream.avail_in = static_cast<unsigned>(produced);
  stream.next_out = output.data();
  stream.avail_out = static_cast<unsigned>(output.size());
  CHECK(z(f, "inflate", {h2g(&stream), 4}) == 1);
  CHECK(stream.total_out == input.size());
  CHECK(output == input);
  CHECK(z(f, "inflateEnd", {h2g(&stream)}) == 0);
  CHECK(z(f, "inflateInit_", {h2g(&stream), version, 88}) == static_cast<std::uint64_t>(-6));
}
TEST_CASE("libc A zlib host callbacks invoke guest allocators and translate gzip headers") {
  test::LibcFixture f;
  GuestZStream stream{};
  int allocations = 0, frees = 0;
  stream.zalloc = test::callback([&](auto args) {
    CHECK(args[0] == 123);
    ++allocations;
    auto p = hle::libc::allocate(args[1] * args[2]);
    if (p) std::memset(p, 0, args[1] * args[2]);
    return h2g(p);
  });
  stream.zfree = test::callback([&](auto args) {
    CHECK(args[0] == 123);
    ++frees;
    hle::libc::deallocate(g2h(args[1]));
    return 0;
  });
  stream.opaque = 123;
  auto version = z(f, "zlibVersion");
  CHECK(z(f, "deflateInit2_", {h2g(&stream), 6, 8, 31, 8, 0, version, 112}) == 0);
  hle::libc::GuestGzHeader header{};
  header.time = 0x76543210;
  header.os = 3;
  unsigned char name[] = "quest.txt";
  header.name = name;
  CHECK(z(f, "deflateSetHeader", {h2g(&stream), h2g(&header)}) == 0);
  unsigned char input[] = "Venture gzip translation";
  std::array<unsigned char, 256> compressed{};
  stream.next_in = input;
  stream.avail_in = sizeof(input);
  stream.next_out = compressed.data();
  stream.avail_out = compressed.size();
  CHECK(z(f, "deflate", {h2g(&stream), 4}) == 1);
  auto count = stream.total_out;
  CHECK(compressed[4] == 0x10);
  CHECK(compressed[7] == 0x76);
  z(f, "deflateEnd", {h2g(&stream)});
  CHECK(allocations > 0);
  CHECK(frees == allocations);
  stream = {};
  CHECK(z(f, "inflateInit2_", {h2g(&stream), 31, version, 112}) == 0);
  header = {};
  std::array<unsigned char, 32> filename{};
  header.name = filename.data();
  header.name_max = filename.size();
  CHECK(z(f, "inflateGetHeader", {h2g(&stream), h2g(&header)}) == 0);
  std::array<unsigned char, 128> output{};
  stream.next_in = compressed.data();
  stream.avail_in = static_cast<unsigned>(count);
  stream.next_out = output.data();
  stream.avail_out = output.size();
  CHECK(z(f, "inflate", {h2g(&stream), 4}) == 1);
  CHECK(header.time == 0x76543210);
  CHECK(header.done == 1);
  CHECK(std::string(reinterpret_cast<char*>(filename.data())) == "quest.txt");
  z(f, "inflateEnd", {h2g(&stream)});
}
