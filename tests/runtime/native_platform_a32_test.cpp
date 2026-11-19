// The 32-bit Platform loader's call conversion (src/android_game/platform/
// a32_call.h) on the x64 host: AAPCS32 frames as a32_entry.S stores them,
// with the game's memory faked at 32-bit addresses, become the A64 registers
// the handlers read, and the results come back in r0:r1.
#include "android_game/platform/a32_call.h"

#include <doctest/doctest.h>

#include <bit>
#include <cstring>
#include <map>
#include <vector>

#include "hle/ovrplatform/abi.h"

using namespace venture;
using namespace venture::ovrplatform;

namespace {
// The game's memory: byte ranges at 32-bit addresses.
struct FakeMemory {
  std::map<std::uint32_t, std::vector<std::uint8_t>> ranges;
  void put(std::uint32_t address, const void* data, std::size_t size) {
    auto& r = ranges[address];
    r.assign(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
  }
  template <class T> void put(std::uint32_t address, const T& value) { put(address, &value, sizeof(value)); }
  bool read(std::uint64_t address, void* out, std::size_t size) const {
    for (const auto& [start, bytes] : ranges)
      if (address >= start && address + size <= start + bytes.size()) {
        std::memcpy(out, bytes.data() + (address - start), size);
        return true;
      }
    return false;
  }
  bool write(std::uint64_t address, const void* data, std::size_t size) {
    for (auto& [start, bytes] : ranges)
      if (address >= start && address + size <= start + bytes.size()) {
        std::memcpy(bytes.data() + (address - start), data, size);
        return true;
      }
    return false;
  }
  a32_native::Memory view() {
    return {[this](std::uint64_t a, void* o, std::size_t s) { return read(a, o, s); },
            [this](std::uint64_t a, const void* d, std::size_t s) { return write(a, d, s); }};
  }
};
constexpr std::uint32_t kStack = 0x7000'0000;

struct Pair32 {
  std::uint32_t key;
  std::int32_t type;
  std::uint32_t string_value;
  std::int32_t int_value;
  double double_value;
};
}  // namespace

TEST_CASE("A32 Platform calls: words, doublewords and the stack") {
  FakeMemory memory;
  // (u, I, u, u): the 64-bit value skips r1 for the r2:r3 pair, and the
  // words after it are on the stack.
  const std::uint32_t stack_words[] = {0x11, 0x22};
  memory.put(kStack, stack_words, sizeof(stack_words));
  std::uint32_t frame[4] = {0xabc, 0xdead, 0x89abcdef, 0x01234567};
  NativeContext seen{};
  REQUIRE(a32_native::call("IuIuu", 7, frame, kStack, memory.view(),
                           [&](NativeContext& c) {
                             seen = c;
                             c.x[0] = 0x1122334455667788ull;
                           }));
  CHECK(seen.x[16] == 7);
  CHECK(seen.x[0] == 0xabc);
  CHECK(seen.x[1] == 0x0123456789abcdefull);
  CHECK(seen.x[2] == 0x11);
  CHECK(seen.x[3] == 0x22);
  CHECK(frame[0] == 0x55667788u);
  CHECK(frame[1] == 0x11223344u);
}

TEST_CASE("A32 Platform calls: doubles go to v registers") {
  FakeMemory memory;
  // (u, u, d): the double takes the r2:r3 pair.
  const double value = 2.5;
  const auto bits = std::bit_cast<std::uint64_t>(value);
  std::uint32_t frame[4] = {0x10, 0xffffffff, std::uint32_t(bits), std::uint32_t(bits >> 32)};
  NativeContext seen{};
  REQUIRE(a32_native::call("vuud", 3, frame, kStack, memory.view(), [&](NativeContext& c) { seen = c; }) == true);
  CHECK(seen.x[0] == 0x10);
  CHECK(seen.x[1] == 0xffffffff);
  CHECK(std::bit_cast<double>(seen.q[0][0]) == value);
  CHECK(frame[0] == 0);
}

TEST_CASE("A32 Platform calls: signed words, floats and float results") {
  FakeMemory memory;
  std::uint32_t frame[4] = {0xfffffffe, std::bit_cast<std::uint32_t>(1.5f), 0, 0};
  NativeContext seen{};
  REQUIRE(a32_native::call("fif", 9, frame, kStack, memory.view(), [&](NativeContext& c) {
    seen = c;
    c.q[0][0] = std::bit_cast<std::uint32_t>(-4.25f);
  }));
  CHECK(std::int64_t(seen.x[0]) == -2);
  CHECK(std::bit_cast<float>(std::uint32_t(seen.q[0][0])) == 1.5f);
  CHECK(std::bit_cast<float>(frame[0]) == -4.25f);

  std::uint32_t frame2[4] = {0x40, 0, 0, 0};
  REQUIRE(a32_native::call("Bu", 9, frame2, kStack, memory.view(), [](NativeContext& c) { c.x[0] = 0x1201; }));
  CHECK(frame2[0] == 1);
  REQUIRE(a32_native::call("du", 9, frame2, kStack, memory.view(),
                           [](NativeContext& c) { c.q[0][0] = std::bit_cast<std::uint64_t>(-0.5); }));
  CHECK(std::bit_cast<double>(frame2[0] | std::uint64_t(frame2[1]) << 32) == -0.5);
}

TEST_CASE("A32 Platform calls: pointer and key/value arrays in the A64 layout") {
  FakeMemory memory;
  const std::uint32_t names[] = {0x1000, 0x2000, 0x3000};
  memory.put(0x500, names, sizeof(names));
  const Pair32 pairs[] = {{0x4000, 0, 0x4100, 0, 0}, {0x4200, 1, 0, 42, 0}, {0x4300, 2, 0, 0, 3.25}};
  memory.put(0x600, pairs, sizeof(pairs));

  std::uint32_t frame[4] = {0x900, 0x500, 3, 0};
  std::vector<std::uint64_t> wide;
  REQUIRE(a32_native::call("vuPu", 1, frame, kStack, memory.view(), [&](NativeContext& c) {
    CHECK(c.x[0] == 0x900);
    CHECK(c.x[2] == 3);
    const auto* p = reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(c.x[1]));
    wide.assign(p, p + 3);
  }));
  CHECK(wide == std::vector<std::uint64_t>{0x1000, 0x2000, 0x3000});

  std::uint32_t init[4] = {0x700, 0x800, 0x880, 0x600};
  memory.put(kStack, std::uint32_t(3));  // the count, the fifth word
  std::vector<KeyValuePair64> seen;
  REQUIRE(a32_native::call("IuuuKu", 2, init, kStack, memory.view(), [&](NativeContext& c) {
    CHECK(c.x[0] == 0x700);
    CHECK(c.x[4] == 3);
    const auto* p = reinterpret_cast<const KeyValuePair64*>(static_cast<std::uintptr_t>(c.x[3]));
    seen.assign(p, p + 3);
    c.x[0] = 77;
  }));
  REQUIRE(seen.size() == 3);
  CHECK(seen[0].key == 0x4000);
  CHECK(seen[0].string_value == 0x4100);
  CHECK(seen[1].type == 1);
  CHECK(seen[1].int_value == 42);
  CHECK(seen[2].double_value == 3.25);
  CHECK(init[0] == 77);
  CHECK(init[1] == 0);

  // A null array stays null.
  std::uint32_t none[4] = {0x700, 0x800, 0x880, 0};
  REQUIRE(a32_native::call("IuuuKu", 2, none, kStack, memory.view(),
                           [&](NativeContext& c) { CHECK(c.x[3] == 0); }));
}

TEST_CASE("A32 Platform calls: matchmaking queries") {
  FakeMemory memory;
  const Pair32 data[] = {{0x4000, 1, 0, 5, 0}};
  memory.put(0x600, data, sizeof(data));
  const Pair32 parameters[] = {{0x4400, 0, 0x4500, 0, 0}, {0x4600, 2, 0, 0, 1.5}};
  memory.put(0x680, parameters, sizeof(parameters));
  const std::uint32_t criterion[] = {0x4700, 2, 0x680, 2};
  memory.put(0x6c0, criterion, sizeof(criterion));
  const std::uint32_t query[] = {0x600, 1, 0x6c0, 1};
  memory.put(0x6e0, query, sizeof(query));

  std::uint32_t frame[4] = {0x900, 0x6e0, 0, 0};
  REQUIRE(a32_native::call("IuQ", 4, frame, kStack, memory.view(), [&](NativeContext& c) {
    const auto& q = *reinterpret_cast<const MatchmakingQuery64*>(static_cast<std::uintptr_t>(c.x[1]));
    REQUIRE(q.data_count == 1);
    CHECK(reinterpret_cast<const KeyValuePair64*>(static_cast<std::uintptr_t>(q.data))->int_value == 5);
    REQUIRE(q.criteria_count == 1);
    const auto& m = *reinterpret_cast<const MatchmakingCriterion64*>(static_cast<std::uintptr_t>(q.criteria));
    CHECK(m.key == 0x4700);
    CHECK(m.importance == 2);
    REQUIRE(m.count == 2);
    const auto* p = reinterpret_cast<const KeyValuePair64*>(static_cast<std::uintptr_t>(m.parameters));
    CHECK(p[0].string_value == 0x4500);
    CHECK(p[1].double_value == 1.5);
  }));
}

TEST_CASE("A32 Platform calls: ovrKeyValuePair_makeString and failures") {
  FakeMemory memory;
  memory.put(0x800, Pair32{});
  std::uint32_t frame[4] = {0x800, 0x1000, 0x2000, 0};
  bool ran = false;
  REQUIRE(a32_native::call("Cuu", 5, frame, kStack, memory.view(), [&](NativeContext&) { ran = true; }));
  CHECK_FALSE(ran);
  Pair32 out{};
  REQUIRE(memory.read(0x800, &out, sizeof(out)));
  CHECK(out.key == 0x1000);
  CHECK(out.type == 0);
  CHECK(out.string_value == 0x2000);

  // An unreadable array or stack word: no call, zero result.
  std::uint32_t bad[4] = {0x900, 0xdead0000, 2, 0};
  CHECK_FALSE(a32_native::call("wuPu", 6, bad, kStack, memory.view(), [&](NativeContext&) { ran = true; }));
  CHECK_FALSE(ran);
  CHECK(bad[0] == 0);
  std::uint32_t far[4] = {1, 2, 3, 4};
  CHECK_FALSE(a32_native::call("wuuuuu", 6, far, 0x10, memory.view(), [&](NativeContext&) { ran = true; }));
  CHECK_FALSE(a32_native::call("", 6, far, kStack, memory.view(), [&](NativeContext&) { ran = true; }));
  CHECK_FALSE(ran);
}

TEST_CASE("A32 Platform calls: more than eight integer arguments go on the A64 stack") {
  FakeMemory memory;
  std::uint32_t words[6] = {5, 6, 7, 8, 9, 10};
  memory.put(kStack, words, sizeof(words));
  std::uint32_t frame[4] = {1, 2, 3, 4};
  REQUIRE(a32_native::call("wuuuuuuuuuu", 8, frame, kStack, memory.view(), [&](NativeContext& c) {
    for (int i = 0; i < 8; ++i) CHECK(c.x[i] == std::uint64_t(i + 1));
    const auto* stack = reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(c.sp));
    CHECK(stack[0] == 9);
    CHECK(stack[1] == 10);
  }));
}
