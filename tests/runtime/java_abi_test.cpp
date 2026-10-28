#include <doctest/doctest.h>

#include <bit>

#include "fake_cpu.h"
#include "java/abi.h"

using namespace venture;
using namespace venture::java;
TEST_CASE("JNI descriptors reject malformed input and preserve JVM widths") {
  auto d = Descriptor::parse("(ZBCSIJFDLjava/lang/Object;[[I)V");
  CHECK(d.parameters.size() == 10);
  CHECK(d.parameters[8] == Type::Object);
  CHECK(d.result == Type::Void);
  for (auto bad : {"", "V", "(V)V", "([V)V", "(I)", "()VV", "(L;)V", "(Ljava.lang.String;)V", "(I)Vx"})
    CHECK_THROWS_AS(Descriptor::parse(bad), std::invalid_argument);
  CHECK(Descriptor::parse("(JLandroid/view/MotionEvent;IIIIIJJIIIIIIFF)Z").parameters.size() == 17);
}
TEST_CASE("JNI mangling round-trips escaped underscores, arrays, Unicode and empty overloads") {
  std::u16string owner = u"com/example/Cl_ass\u03bb";
  std::u16string name = u"foo_bar\U0001F600";
  std::u16string params = u"J[Ljava/lang/String;[[I";
  auto n = demangle(native_name(owner, name, params));
  CHECK(n.owner == owner);
  CHECK(n.method == name);
  CHECK(n.parameters == params);
  CHECK(demangle(native_name(owner, u"method")).parameters == std::nullopt);
  CHECK(demangle(native_name(owner, u"method", u"")).parameters == u"");
  auto leading = demangle(native_name(u"p/_Cls", u"_method", u"[I"));
  CHECK(leading.owner == u"p/_Cls");
  CHECK(leading.method == u"_method");
  CHECK(leading.parameters == u"[I");
  auto short_leading = demangle(native_name(u"p/_Cls", u"_method"));
  CHECK(short_leading.method == u"_method");
  CHECK(mangle(u"a_b/[;") == "a_1b__3_2");
  CHECK(from_modified_utf8(to_modified_utf8(name)) == name);
  CHECK_THROWS(demangle("Java_bad"));
  CHECK_THROWS(unescape("_0ab"));
}
TEST_CASE("JNI variadic decoder handles independent register exhaustion and ordered mixed spills") {
  test::FakeCpu cpu;
  // 3 fixed GPR args, then 10 pairs of long/float. Float is promoted to double.
  std::uint64_t stack[32]{};
  unsigned spilled = 0;
  for (unsigned i = 0; i < 10; ++i) {
    if (i < 5)
      cpu.xs[3 + i] = 100 + i;
    else
      stack[spilled++] = 100 + i;
    auto bits = std::bit_cast<std::uint64_t>(i + 0.25);
    if (i < 8)
      cpu.vs[i].lo = bits;
    else
      stack[spilled++] = bits;
  }
  cpu.use_stack(stack);
  hle::FunctionInfo info;
  hle::CallContext ctx(*reinterpret_cast<GuestThread*>(&cpu), cpu, info);
  hle::ArgCursor cursor(ctx);
  for (int i = 0; i < 3; ++i) cursor.next_gpr();
  auto values = read_arguments(Descriptor::parse("(JFJFJFJFJFJFJFJFJFJF)V"), cursor, ArgumentForm::Ellipsis);
  for (unsigned i = 0; i < 10; ++i) {
    CHECK(values[2 * i].j == 100 + i);
    CHECK(values[2 * i + 1].f == doctest::Approx(i + 0.25));
  }
}
TEST_CASE("JNI va_list uses signed offsets, 16-byte FP saves and shared spill cursor") {
  test::FakeCpu cpu;
  std::uint64_t gp[2] = {11, 12},
                vr[4] = {std::bit_cast<std::uint64_t>(1.25), 0, std::bit_cast<std::uint64_t>(2.25), 0};
  std::uint64_t stack[2] = {13, std::bit_cast<std::uint64_t>(3.25)};
  hle::GuestVaListStruct list{h2g(stack), h2g(gp + 2), h2g(vr + 4), -16, -32};
  cpu.xs[0] = h2g(&list);
  hle::FunctionInfo info;
  hle::CallContext ctx(*reinterpret_cast<GuestThread*>(&cpu), cpu, info);
  hle::ArgCursor cursor(ctx);
  auto values = read_arguments(Descriptor::parse("(JFJFJF)V"), cursor, ArgumentForm::VaList);
  for (int i = 0; i < 3; ++i) {
    CHECK(values[i * 2].j == 11 + i);
    CHECK(values[i * 2 + 1].f == doctest::Approx(i + 1.25));
  }
  CHECK(list.gr_offs == -16);
  CHECK(list.vr_offs == -32);
}
