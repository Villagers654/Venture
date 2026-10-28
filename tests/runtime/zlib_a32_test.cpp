#include <doctest/doctest.h>
#include "fake_cpu32.h"
#include "core/hle_runtime.h"
#include "core/guest_storage.h"
#include "core/thread.h"
#include "hle/libc/libc_parts.h"
#include "hle/libz/a32.h"
#include "core/linker.h"
#include <cstring>

using namespace venture;
namespace {
std::int32_t call(std::string_view name, const hle::a32::CallBuilder32& args) {
  static HleLibrary lib("libz.so");
  static bool registered = [] { hle::libc::register_libz(lib); return true; }();
  (void)registered;
  test::LowMemory32 stack;
  test::FakeCpu32 cpu;
  cpu.set_sp(stack.address + 65536);
  args.apply(cpu, stack.address);
  for (const auto& f : hle::registered_functions())
    if (f.thunk == lib.find_symbol(name)) {
      REQUIRE(f.handler != hle::a32::diagnostic);
      hle::CallContext ctx(GuestThread::adopt_current(), cpu, f);
      f.handler(ctx);
      return static_cast<std::int32_t>(cpu.x(0));
    }
  FAIL("missing zlib symbol");
  return -99;
}
}
TEST_CASE("A32 zlib length outputs and streams preserve canaries and roundtrip real data") {
  using namespace hle::a32;
  using hle::libc::zlib32::WireStream;
  test::LowMemory32 memory;
  const char text[] = "First Contact compressed OBB asset test with repeated repeated repeated bytes";
  auto source = memory.address + 256, compressed = memory.address + 512, decoded = memory.address + 1024;
  std::memcpy(g2h(source), text, sizeof(text));
  auto* length = memory.at<std::uint32_t>(128);
  length[1] = 0xfeedface;
  for (auto name : {"compress", "compress2"}) {
    *length = 512;
    CHECK(call(name, CallBuilder32{}.pointer(compressed).pointer(h2g(length)).pointer(source)
                         .value(static_cast<std::uint32_t>(sizeof(text))).value(6)) == 0);
    CHECK(length[1] == 0xfeedface);
    auto size = *length;
    {
      *length = 512;
      auto args = CallBuilder32{}.pointer(decoded).pointer(h2g(length)).pointer(compressed).value(size);
      CHECK(call("uncompress", args) == 0);
      CHECK(*length == sizeof(text)); CHECK(length[1] == 0xfeedface);
      CHECK(std::memcmp(g2h(decoded), text, sizeof(text)) == 0);
    }
  }
  auto* wire = memory.at<WireStream>();
  auto version = h2g(low_string("1.2.8"));
  auto init = [&](bool deflate, bool extended) {
    CallBuilder32 args;
    args.pointer(memory.address);
    if (deflate) {
      args.value(6);
      if (extended) args.value(8).value(15).value(8).value(0);
    } else if (extended) args.value(15);
    return args.pointer(version).value(56);
  };
  for (bool extended : {false, true}) {
    *wire = {};
    *memory.at<std::uint32_t>(56) = 0xdeadbeef;
    auto deflate_name = extended ? "deflateInit2_" : "deflateInit_";
    auto inflate_name = extended ? "inflateInit2_" : "inflateInit_";
    CHECK(call(deflate_name, init(true, extended)) == 0);
    REQUIRE(wire->state); REQUIRE(wire->zalloc); REQUIRE(wire->zfree);
    auto old_token = wire->state;
    CHECK(call("deflateReset", CallBuilder32{}.pointer(memory.address)) == 0);
    wire->next_in = static_cast<std::uint32_t>(source); wire->avail_in = sizeof(text);
    wire->next_out = static_cast<std::uint32_t>(compressed); wire->avail_out = 512;
    CHECK(call("deflate", CallBuilder32{}.pointer(memory.address).value(4)) == 1); // Z_FINISH -> Z_STREAM_END
    auto size = wire->total_out;
    CHECK(wire->total_in == sizeof(text)); CHECK(*memory.at<std::uint32_t>(56) == 0xdeadbeef);
    CHECK(call("deflateEnd", CallBuilder32{}.pointer(memory.address)) == 0); CHECK(wire->state == 0);
    wire->state = old_token;
    CHECK(call("deflate", CallBuilder32{}.pointer(memory.address).value(4)) == -2);
    *wire = {};
    CHECK(call(inflate_name, init(false, extended)) == 0);
    CHECK(wire->state != old_token);
    CHECK(call("inflateReset", CallBuilder32{}.pointer(memory.address)) == 0);
    wire->next_in = static_cast<std::uint32_t>(compressed); wire->avail_in = size;
    wire->next_out = static_cast<std::uint32_t>(decoded); wire->avail_out = 512;
    CHECK(call("inflate", CallBuilder32{}.pointer(memory.address).value(4)) == 1);
    CHECK(wire->total_out == sizeof(text)); CHECK(std::memcmp(g2h(decoded), text, sizeof(text)) == 0);
    CHECK(call("inflateEnd", CallBuilder32{}.pointer(memory.address)) == 0);
    CHECK(wire->state == 0); CHECK(*memory.at<std::uint32_t>(56) == 0xdeadbeef);
  }
}
