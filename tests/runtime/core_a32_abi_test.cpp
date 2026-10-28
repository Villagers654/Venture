#include <doctest/doctest.h>
#include "core/a32_abi.h"
#include "core/thread.h"
#include "core/error.h"
#include "core/hle_runtime.h"
#include "core/process.h"
#include "core/linker.h"
#include "fake_cpu32.h"
#include "core/low_address_space.h"
#include "cpu/a32_backend.h"
#include "cpu/state.h"
#include "hle/libc/libc_a.h"

namespace venture::hle::libc { void register_aeabi(HleLibrary&); }
namespace venture::hle::ossdk { void register_libraries(); }

using namespace venture;
using namespace venture::hle;
using namespace venture::hle::a32;
namespace {
struct CompilerImage { const char* name; bool thumb; std::uint32_t registers[4], stack[16]; };
#include "a32_compiler_images.inc"


std::uint64_t next_output = 0;
int output(std::uint64_t* pointer) { *pointer = next_output; return 0; }
// Reads the cell before writing it: the host sees the guest's word, widened.

double mixed(std::int32_t i, std::uint64_t q, float f, double d, std::uint64_t size) {
  CHECK(i == -7); CHECK(q == 0x1122334455667788ull); CHECK(f == 1.25f);
  CHECK(d == -3.5); CHECK(size == 0xfedcba98); return f + d;
}
}
TEST_CASE("AAPCS32 NDK compiler oracle: ARM and Thumb callers") {
  test::LowMemory32 memory(true);
  for (const auto& image : compiler_images) for (bool direct : {false, true}) {
    CAPTURE(image.name);
    CAPTURE(image.thumb);
    test::FakeCpu32 cpu;
    for (int i = 0; i < 4; ++i) cpu.set_x(i, image.registers[i]);
    std::memcpy(memory.at<void>(), image.stack, sizeof(image.stack)); cpu.set_sp(memory.address);
    std::array<std::uint32_t, 16> registers{};
    std::copy_n(image.registers, 4, registers.begin()); registers[13] = memory.address;
    ArgCursor32 cursor(cpu, direct ? registers.data() : nullptr);
    const std::string_view name = image.name;
    if (name == "mixed") {
      CHECK(cursor.next({Kind::I32}) == std::uint64_t(-7));
      CHECK(cursor.next({Kind::U64}) == 0x1122334455667788ull);
      CHECK(std::bit_cast<float>(std::uint32_t(cursor.next({Kind::F32}))) == 1.25f);
      CHECK(std::bit_cast<double>(cursor.next({Kind::F64})) == -3.5);
      CHECK(cursor.next({Kind::Size}) == 0xfedcba98);
    } else if (name == "skip") {
      CHECK(cursor.word() == 11); CHECK(cursor.word() == 22); CHECK(cursor.word() == 33);
      CHECK(cursor.pair() == 0x1234567887654321ull); CHECK(cursor.next({Kind::I32}) == std::uint64_t(-44));
      CHECK(std::bit_cast<double>(cursor.pair()) == 9.25);
    } else if (name == "split") {
      CHECK(cursor.word() == 7); CHECK(cursor.word() == 8); std::array<std::uint32_t, 3> words;
      cursor.composite(std::as_writable_bytes(std::span(words)));
      CHECK(words == std::array<std::uint32_t, 3>{11, 22, 33}); CHECK(cursor.word() == 44);
    } else if (name == "narrow") {
      CHECK(cursor.next({Kind::I8}) == std::uint64_t(-1)); CHECK(cursor.next({Kind::U8}) == 255);
      CHECK(cursor.next({Kind::I16}) == std::uint64_t(-2)); CHECK(cursor.next({Kind::U16}) == 65535);
    } else if (name == "varargs") {
      CHECK(cursor.word() == 4); VarArgs32 args(cursor);
      CHECK(args.next<int>() == -7); CHECK(args.next<std::uint64_t>() == 0xfedcba9876543210ull);
      CHECK(args.next<double>() == 1.25); CHECK(h2g(args.next<void*>()) == 0x34567890);
    } else if (name == "sret") {
      ArgCursor32 hidden(cpu, direct ? registers.data() : nullptr, true);
      CHECK(image.registers[0] != 77); CHECK(hidden.word() == 77); CHECK(std::bit_cast<double>(hidden.pair()) == 2.5);
    }
  }
}
TEST_CASE("AAPCS32 borrowed invocation keeps guarded Out cells and direct results") {
  test::LowMemory32 memory; test::FakeCpu32 cpu; GuestThread thread(61004);
  std::array<std::uint32_t, 16> registers{};
  registers[13] = memory.address + 256;
  const Type args[]{{Kind::OutSize}};
  Descriptor desc{"test", "out", {Kind::I32}, args, false, "fixture"};
  FunctionInfo info; info.a32 = &desc;
  CallContext ctx(thread, cpu, info, Registers(registers.data()));
  CHECK(ctx.sp() == registers[13]);
  auto* cell = memory.at<std::uint32_t>(); cell[0] = 7; cell[1] = 0xfeedface;
  cpu.set_x(0, 1); registers[0] = memory.address;
  next_output = 42; a32::invoke(&output, ctx);
  CHECK(cell[0] == 42); CHECK(cell[1] == 0xfeedface);
  CHECK(cpu.x(0) == 1);
  registers[0] = 1; a32::invoke(&output, ctx);
  CHECK(*thread.errno_location() == venture::err::kEFAULT);
  CHECK(registers[0] == UINT32_MAX);
  CHECK(cell[0] == 42); CHECK(cell[1] == 0xfeedface);
}
TEST_CASE("AAPCS32 va_list pointer, sret and signed result access") {
  test::LowMemory32 memory; test::FakeCpu32 cpu;
  auto* words = memory.at<std::uint32_t>(); words[0] = 7; words[2] = 0x11223344; words[3] = 0x55667788;
  VaList32 list(memory.address); CHECK(list.next<int>() == 7); CHECK(list.next<std::uint64_t>() == 0x5566778811223344ull);
  cpu.set_x(0, memory.address); ResultWriter32 writer(cpu); cpu.set_x(0, 999);
  std::array<std::uint32_t,3> result{1,2,3}; writer.composite(std::as_bytes(std::span(result)));
  CHECK(words[0] == 1); CHECK(words[1] == 2); CHECK(words[2] == 3);
  GuestCallResult call{UINT32_MAX, UINT32_MAX, {}, GuestIsa::a32}; CHECK(call.i32() == -1); CHECK(call.i64() == -1);
}
TEST_CASE("AAPCS32 printf widths, positional stars, pointers and long double") {
  test::LowMemory32 memory; test::FakeCpu32 cpu; cpu.set_sp(memory.address + 32768);
  auto count = memory.at<std::uint32_t>(); count[0] = 0; count[1] = 0xfeedface;
  CallBuilder32 b; b.value(-7).value(std::int64_t(-9)).value(std::uint32_t(123)).pointer(0xabcdef01).pointer(memory.address).value(1.25);
  b.apply(cpu, memory.address + 4096);
  libc::PrintArgs<VarArgs32> args{VarArgs32{ArgCursor32(cpu)}};
  CHECK(libc::format_printf32("%ld %lld %zu %p%ln %.2Lf", args) == "-7 -9 123 0xabcdef01 1.25");
  CHECK(count[0] == 20); CHECK(count[1] == 0xfeedface);
  cpu.set_sp(memory.address + 32768); CallBuilder32{}.value(6).value(-12).apply(cpu, memory.address + 4096);
  libc::PrintArgs<VarArgs32> positional{VarArgs32{ArgCursor32(cpu)}};
  CHECK(libc::format_printf32("%2$*1$ld", positional) == "   -12");
  cpu.set_sp(memory.address + 32768); CallBuilder32{}.value(-1).apply(cpu, memory.address + 4096);
  libc::PrintArgs<VarArgs32> repeated{VarArgs32{ArgCursor32(cpu)}};
  CHECK(libc::format_printf32("%1$d %1$x", repeated) == "-1 ffffffff");
}

TEST_CASE("AAPCS32 interpreter and typed builder cross the real A32 JIT stub boundary") {
  test::LowMemory32 memory(false, true);
  const GuestAddr base = memory.address, size = 65536;
  auto backend = make_a32_backend({2, low32::query}); auto cpu = backend->create_cpu();
  auto thunk = guest_stub(GuestIsa::a32, GuestStub::hle_call, 17);
  auto ret = guest_stub(GuestIsa::a32, GuestStub::call_return, 1);
  std::memcpy(g2h(base), thunk.instructions().data(), thunk.instructions().size_bytes());
  std::memcpy(g2h(base + 0x100), ret.instructions().data(), ret.instructions().size_bytes());
  cpu->set_sp(base + size); cpu->set_x(0, 0xa5a5a5a5);
  CpuState32 saved(*cpu);
  CallBuilder32 b; b.value(std::int32_t(-7)).value(0x1122334455667788ull).value(1.25f).value(-3.5).value(std::uint32_t(0xfedcba98));
  b.apply(*cpu, base + 0x8000); cpu->set_pc(base); cpu->set_lr(base + 0x100);
  REQUIRE(cpu->run() == CpuExit::Svc); CHECK(cpu->hle_call_id() == 17);
  const Type args[]{{Kind::I32}, {Kind::U64}, {Kind::F32}, {Kind::F64}, {Kind::Size}};
  Descriptor desc{"test", "mixed", {Kind::F64}, args, false, "fixture"};
  FunctionInfo info{nullptr, nullptr, "test", "mixed"}; info.a32 = &desc;
  GuestThread thread(61006); CallContext ctx(thread, *cpu, info); a32::invoke(&mixed, ctx);
  REQUIRE(cpu->run() == CpuExit::Svc); CHECK(cpu->svc_immediate() == kSvcReturnToHost);
  GuestCallResult result{cpu->x(0), cpu->x(1), {}, GuestIsa::a32}; CHECK(result.f64() == -2.25);
  saved.restore(*cpu); CHECK(cpu->x(0) == 0xa5a5a5a5); CHECK(cpu->sp() == base + size);
}
