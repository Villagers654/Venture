#pragma once
#include "hle/libc/vfs.h"
#include "hle/registry.h"
#include <doctest/doctest.h>
#include <cstring>
namespace venture::test::libc_b {
inline HleLibrary& library() {
  register_all_hle_libraries();
  return *static_cast<HleLibrary*>(Linker::get().open("libc.so", kRtldNow, nullptr));
}
inline hle::FunctionId function_id(GuestAddr thunk) {
  const auto* code = g2h<std::uint32_t>(thunk);
  return ((code[0] >> 5) & 0xffff) | (((code[1] >> 5) & 0xffff) << 16);
}
inline std::int64_t call(const char* name, std::initializer_list<std::uint64_t> args = {}) {
  auto* t = GuestThread::current();
  auto p = library().find_symbol(name);
  if (!p) p = Linker::get().open("libdl.so", kRtldNow, nullptr)->find_symbol(name);
  REQUIRE(p != 0);
  return static_cast<std::int64_t>(t->call(p, GuestCallArgs{std::span(args.begin(), args.size()), {}}).x0);
}

struct Thread {
  GuestThread thread;
  explicit Thread(int tid = 1001) : thread(tid + 40000) {
    thread.attach_to_current_host_thread();
    library();
    static std::once_flag config;
    std::call_once(config, [] {
      auto root = std::filesystem::path(VENTURE_LIBC_B_TEST_ROOT);
      std::filesystem::create_directories(root / "data" / "external");
      std::filesystem::create_directories(root / "obb");
      auto& c = process_config();
      c.package = "test.game";
      c.data_dir = root / "data";
      c.obb_dir = root / "obb";
      c.apk = root / "base.apk";
      c.native_lib_dir = root / "lib";
    });
  }
};
}  // namespace venture::test::libc_b
