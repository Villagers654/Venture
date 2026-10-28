#include "libc_b/test.h"
#include "core/address_space.h"
#include "core/memory_metadata.h"
#include <format>
#include <sstream>

using namespace venture;
using namespace venture::hle::libc;
using namespace venture::test::libc_b;

TEST_CASE("proc maps opens capture mapping permissions offsets and names") {
  Thread thread;
  auto& space = AddressSpace::get();
  const auto p = space.allocate(8192, 3, "[proc snapshot]");
  REQUIRE(p != 0);
  const auto fd = call("open", {h2g("/proc/self/maps"), 0});
  REQUIRE(fd >= 0);
  REQUIRE(space.protect(p + 4096, 4096, 1) == 0);
  auto read_all = [&](std::int64_t f) {
    std::string result;
    char data[4096];
    for (;;) {
      auto n = call("read", {static_cast<std::uint64_t>(f), h2g(data), sizeof(data)});
      REQUIRE(n >= 0);
      if (!n) break;
      result.append(data, n);
    }
    CHECK(call("close", {static_cast<std::uint64_t>(f)}) == 0);
    return result;
  };
  const auto before = read_all(fd);
  const auto after = read_all(call("open", {h2g("/proc/self/maps"), 0}));
  CHECK(before.find(std::format("{:012x}-{:012x} rw-p 00000000 00:00 0 [proc snapshot]\n", p, p + 8192)) != std::string::npos);
  CHECK(after.find(std::format("{:012x}-{:012x} r--p 00001000 00:00 0 [proc snapshot]\n", p + 4096, p + 8192)) != std::string::npos);
  label_memory(p, 8192, "/data/app/proc-test.so", 0x2000);
  const auto labelled = read_all(call("open", {h2g("/proc/self/maps"), 0}));
  CHECK(labelled.find(std::format("{:012x}-{:012x} r--p 00003000 fd:00 ", p + 4096, p + 8192)) != std::string::npos);
  CHECK(labelled.find(" /data/app/proc-test.so\n") != std::string::npos);
  space.free(p);
  const auto removed = read_all(call("open", {h2g("/proc/self/maps"), 0}));
  CHECK(removed.find("/data/app/proc-test.so") == std::string::npos);
}

TEST_CASE("proc stat uses Linux field positions and resident pages rather than reservations") {
  Thread thread;
  auto& space = AddressSpace::get();
  const auto reservation = space.allocate(16ull << 30, kProtNone, "[proc nonresident]");
  REQUIRE(reservation != 0);
  const auto fd = call("open", {h2g("/proc/self/stat"), 0});
  REQUIRE(fd >= 0);
  char data[2048]{};
  REQUIRE(call("read", {static_cast<GuestAddr>(fd), h2g(data), sizeof(data) - 1}) > 0);
  CHECK(call("close", {static_cast<GuestAddr>(fd)}) == 0);
  const std::string text(data);
  CHECK(text.starts_with("1000 (test.game) R "));
  std::istringstream fields(text.substr(text.find(") R ") + 4));
  std::vector<std::int64_t> values;
  for (std::int64_t v; fields >> v;) values.push_back(v);
  REQUIRE(values.size() == 49); // fields 4 through 52 of proc_pid_stat(5)
  CHECK(values[4 - 4] == 1);
  CHECK(values[8 - 4] == -1);
  CHECK(values[23 - 4] >= (16ll << 30));
  CHECK(values[24 - 4] > 0);
  CHECK(values[24 - 4] * 4096 < (16ll << 30));
  space.free(reservation);
}
