#pragma once
#include <doctest/doctest.h>
#include "core/linker.h"
#include "core/thread_runtime.h"
#include "core/process.h"
#include "hle/registry.h"

namespace venture::test {
inline int run_jni_invocation_fixture() {
  register_all_hle_libraries();
  REQUIRE(Linker::get().open("libdl.so", kRtldNow | kRtldGlobal, nullptr));
  auto* library = Linker::get().open(VENTURE_TEST_JNI_INVOCATION_FIXTURE, kRtldNow, nullptr);
  REQUIRE(library);
  auto entry = library->find_symbol("vtest_jni_invocation");
  REQUIRE(entry);
  auto& thread = GuestThread::adopt_current();
  return process_config().isa() == GuestIsa::a32
             ? thread.call32(entry, hle::a32::CallBuilder32{}).i32()
             : static_cast<int>(thread.call(entry, {}));
}
}  // namespace venture::test
