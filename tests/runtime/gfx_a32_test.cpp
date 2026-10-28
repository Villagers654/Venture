#include <doctest/doctest.h>
#include "gfx/a32_handles.h"
#include "gfx/dispatch.h"
#include "core/hle_runtime.h"
#include "core/thread.h"
#include "fake_cpu32.h"

using namespace venture;
namespace {
void* high(std::uint64_t bits) { return g2h(bits); }
std::uint64_t seen_timeout;
std::int64_t seen_offset, seen_size;
void parameters(void* sync, std::uint32_t flags, std::uint64_t timeout) {
  CHECK(sync == high(0x123456780000ull)); CHECK(flags == 7); seen_timeout = timeout;
}
void offsets(std::uint32_t, std::int64_t offset, std::int64_t size, std::uint32_t) {
  seen_offset = offset; seen_size = size;
}
std::uint32_t bad_output(std::uint32_t* out) {
  std::uint32_t value = 7;
  hle::a32::write_bytes(h2g(out), &value, 4);
  return 1;
}
}
TEST_CASE("A32 graphics tokens have typed identities and generation checked destruction") {
  using namespace gfx::a32;
  auto host = high(0x123456780000ull);
  auto display = encode(Handle::display, host), context = encode(Handle::context, host);
  CHECK(display != context); CHECK(display == encode(Handle::display, host));
  CHECK(decode(Handle::display, display) == host);
  CHECK_THROWS_AS(decode(Handle::context, display), BadHandle);
  retire(Handle::context, host);
  CHECK_THROWS_AS(decode(Handle::context, context), BadHandle);
  auto replacement = encode(Handle::context, host);
  CHECK(replacement != context); CHECK(decode(Handle::context, replacement) == host);
  CHECK(encode(Handle::surface, nullptr) == 0); CHECK(decode(Handle::surface, 0) == nullptr);
  clear_egl_handles(); CHECK(decode(Handle::display, display) == host);
  CHECK_THROWS_AS(decode(Handle::context, replacement), BadHandle);
}
TEST_CASE("A32 graphics handle adapter preserves GLuint64 and signed ILP32 slots") {
  test::LowMemory32 memory; test::FakeCpu32 cpu; GuestThread thread(64001);
  auto token = gfx::a32::encode(gfx::a32::Handle::gl_sync, high(0x123456780000ull));
  hle::FunctionInfo info{nullptr, nullptr, "libGLESv2.so", "glClientWaitSync"};
  info.a32 = hle::a32::descriptor(info.library, info.name); REQUIRE(info.a32);
  cpu.set_x(0, token); cpu.set_x(1, 7); cpu.set_x(2, 0x76543210); cpu.set_x(3, 0xfedcba98);
  hle::CallContext ctx(thread, cpu, info); gfx::a32::invoke(parameters, ctx);
  CHECK(seen_timeout == 0xfedcba9876543210ull);
  info.name = "glMapBufferRange"; info.a32 = hle::a32::descriptor(info.library, info.name); REQUIRE(info.a32);
  cpu.set_x(0, 0x8892); cpu.set_x(1, std::uint32_t(-4)); cpu.set_x(2, 64); cpu.set_x(3, 2);
  gfx::a32::invoke(offsets, ctx); CHECK(seen_offset == -4); CHECK(seen_size == 64);
  info.name = "glClientWaitSync"; info.a32 = hle::a32::descriptor(info.library, info.name);
  cpu.set_x(0, 1); gfx::a32::invoke(parameters, ctx);
  CHECK(cpu.x(0) == 0x911d); // GLES3/gl3.h: GL_WAIT_FAILED.
}
TEST_CASE("A32 EGL ABI failures return false and preserve Linux errno") {
  test::FakeCpu32 cpu; GuestThread thread(64002);
  const hle::a32::Type args[]{{hle::a32::Kind::Layout, "EGLConfig *"}};
  hle::a32::Descriptor desc{"libEGL.so", "bad-output", {hle::a32::Kind::U32}, args, false, "fixture"};
  hle::FunctionInfo info{nullptr, nullptr, "libEGL.so", "bad-output"}; info.a32 = &desc;
  cpu.set_x(0, 1);
  hle::CallContext ctx(thread, cpu, info); gfx::a32::invoke(bad_output, ctx);
  CHECK(cpu.x(0) == 0);
  CHECK(*thread.errno_location() == err::kEFAULT);
}
