#include <doctest/doctest.h>

#include <bit>
#include <string>

#include "core/hle.h"
#include "core/linker.h"
#include "fake_cpu.h"
#include "core/hle_runtime.h"
#include "core/thread.h"
#include "core/thread_runtime.h"
#include "common/json.h"
#include "common/paths.h"
#include <cstdlib>
#include <fstream>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <set>
#include <cfenv>

using namespace venture;

namespace venture::hle::libc { void register_aeabi(HleLibrary&); }

namespace {
std::int64_t add9(std::int32_t a, std::uint8_t b, std::int64_t c, std::uint64_t d, std::int16_t e, std::int64_t f,
                  std::int64_t g, std::int64_t h, std::int64_t i) {
  return a + b + c + static_cast<std::int64_t>(d) + e + f + g + h + i;
}
double mix(double a, std::int32_t b, float c, const char* s) { return a + b + c + static_cast<double>(std::string(s).size()); }
std::int64_t sum_varargs(std::int32_t count, hle::VarArgs args) {
  std::int64_t total = 0;
  for (int i = 0; i < count; ++i) total += args.next<std::int32_t>();
  return total;
}
}  // namespace

TEST_CASE("AAPCS64 marshaling: integers, stack overflow, narrow truncation") {
  test::FakeCpu cpu;
  std::uint64_t stack[4] = {9, 0, 0, 0};
  cpu.use_stack(stack);
  for (int i = 0; i < 8; ++i) cpu.xs[i] = static_cast<std::uint64_t>(i + 1);
  cpu.xs[1] = 0xffffff02;  // only the low byte belongs to the uint8_t
  hle::FunctionInfo info;
  hle::CallContext ctx(*reinterpret_cast<GuestThread*>(&cpu), cpu, info);
  hle::wrap<&add9>()(ctx);
  CHECK(cpu.xs[0] == 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9);
}

TEST_CASE("AAPCS64 marshaling: floating point and pointers") {
  test::FakeCpu cpu;
  cpu.vs[0].lo = std::bit_cast<std::uint64_t>(1.5);
  cpu.vs[1].lo = std::bit_cast<std::uint32_t>(2.25f);
  cpu.xs[0] = static_cast<std::uint64_t>(-3);
  const char* text = "abcd";
  cpu.xs[1] = reinterpret_cast<std::uint64_t>(text);
  hle::FunctionInfo info;
  hle::CallContext ctx(*reinterpret_cast<GuestThread*>(&cpu), cpu, info);
  hle::wrap<&mix>()(ctx);
  CHECK(std::bit_cast<double>(cpu.vs[0].lo) == doctest::Approx(1.5 - 3 + 2.25 + 4));
}

TEST_CASE("AAPCS64 marshaling: variadic arguments continue after named ones") {
  test::FakeCpu cpu;
  std::uint64_t stack[4] = {70, 80, 0, 0};
  cpu.use_stack(stack);
  cpu.xs[0] = 9;
  for (int i = 1; i < 8; ++i) cpu.xs[i] = static_cast<std::uint64_t>(i * 10);
  hle::FunctionInfo info;
  hle::CallContext ctx(*reinterpret_cast<GuestThread*>(&cpu), cpu, info);
  hle::wrap<&sum_varargs>()(ctx);
  CHECK(cpu.xs[0] == 10 + 20 + 30 + 40 + 50 + 60 + 70 + 70 + 80);
}
