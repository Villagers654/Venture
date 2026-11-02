#include <doctest/doctest.h>

#include "core/hle.h"
#include "core/linker.h"
#include "libc_a_test_support.h"

using namespace venture;

namespace {
// Runs the HLE handler behind a guest function pointer, as LibcFixture::invoke
// does for an exported symbol.
std::uint64_t call(test::LibcFixture& f, GuestAddr thunk, std::initializer_list<std::uint64_t> args) {
  int i = 0;
  for (auto value : args) f.cpu.set_x(i++, value);
  const auto* code = g2h<std::uint32_t>(thunk);
  REQUIRE((code[0] & 0xffe0001fu) == 0xd2800010u);  // an HLE thunk, not guest code
  const auto id = ((code[0] >> 5) & 0xffff) | (((code[1] >> 5) & 0xffff) << 16);
  const auto& info = hle::function(id);
  hle::CallContext ctx(f.thread, f.cpu, info);
  info.handler(ctx);
  return f.cpu.x(0);
}
GuestAddr slot(GuestAddr object, int index) {
  return g2h<std::uint64_t>(*g2h<std::uint64_t>(object))[index];
}
}  // namespace

TEST_CASE("OSSDK telemetry v3: an inert handler and event that Interaction SDK can call through") {
  test::LibcFixture f;
  // Interaction SDK opens the library under its device name.
  REQUIRE(Linker::get().open("libossdk.oculus.so", kRtldNow, nullptr));
  const auto handler = f.invoke("createTelemetryHandler", {3}, "libossdk.oculus.so");
  REQUIRE(handler);
  CHECK(f.invoke("createTelemetryHandler", {3}, "libossdk.so") == handler);  // one shared handler
  CHECK(f.invoke("createTelemetryHandler", {2}, "libossdk.so") == 0);        // other layouts stay unavailable

  // Its calls: slot 0 once, slot 3 for an event, event slots 4-7 and 9 for
  // fields, slot 9 to send; then both destructors.
  CHECK(call(f, slot(handler, 0), {handler, 0}) == 0);
  const auto event = call(f, slot(handler, 3), {handler, 0, 0});
  REQUIRE(event);
  CHECK(event != handler);
  for (int field : {4, 5, 6, 7, 9}) CHECK(call(f, slot(event, field), {event, 0, 0}) == 0);
  CHECK(call(f, slot(handler, 9), {handler, event, 0}) == 0);
  CHECK(f.invoke("destroyTelemetryEvent", {3, event}, "libossdk.so") == 0);
  CHECK(f.invoke("destroyTelemetryHandler", {3, handler}, "libossdk.so") == 0);

  // Every slot of both vtables (the device has 18 and 19) does nothing, beyond
  // the event that handler slot 3 creates.
  for (int i = 0; i < 32; ++i) {
    CAPTURE(i);
    CHECK(call(f, slot(handler, i), {handler, 0, 0}) == (i == 3 ? event : 0));
    CHECK(call(f, slot(event, i), {event, 0, 0}) == 0);
  }
}
