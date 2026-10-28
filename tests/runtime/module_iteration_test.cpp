#include <doctest/doctest.h>
#include "core/address_space.h"
#include "core/hle_runtime.h"
#include "core/linker_runtime.h"
#include "core/process_image.h"
#include "core/thread_runtime.h"
#include "hle/registry.h"
#include "java/jvm.h"
#include <set>
#include <string>

using namespace venture;
namespace {
struct PhdrInfo {
  GuestAddr address, name, phdr;
  std::uint16_t count;
  std::uint8_t padding[6];
  std::uint64_t adds, subs, tls_module;
  GuestAddr tls_data;
};
struct Seen {
  std::set<std::string> names;
  unsigned calls = 0;
  bool valid = true;
  int stop = 0;
};
void visit(hle::CallContext& c) {
  auto& seen = *static_cast<Seen*>(c.user());
  const auto& info = *g2h<PhdrInfo>(c.x(0));
  ++seen.calls;
  seen.valid &= c.x(1) >= sizeof(PhdrInfo) && info.name && info.phdr && info.count;
  const std::string name = g2h<char>(info.name);
  seen.valid &= seen.names.insert(name).second;
  c.return_x(seen.stop);
}
}

TEST_CASE("Published system libraries are visible through the guest module API") {
  AddressSpace::get().initialize(GuestIsa::a64);
  register_all_hle_libraries();
  java::register_jni_library();
  auto& linker = Linker::get();
  auto* art = linker.open("libart.so", kRtldNow | kRtldGlobal, nullptr);
  REQUIRE(art);
  REQUIRE(art->find_symbol("JNI_GetCreatedJavaVMs"));
  REQUIRE_FALSE(art->mapped_image());
  process_image::publish();
  auto* dl = linker.open("libdl.so", kRtldNow, nullptr);
  REQUIRE(dl);
  const auto iterate = dl->find_symbol("dl_iterate_phdr");
  REQUIRE(iterate);
  auto& thread = GuestThread::adopt_current();
  Seen seen;
  const auto callback = hle::make_thunk("module-iteration-test", "visit", visit, &seen);
  CHECK(thread.call(iterate, {callback, 0}) == 0);
  CHECK(seen.valid);
  CHECK(seen.names.contains(process_image::system_path("libart.so")));
  CHECK(seen.names.contains(process_image::program_path()));
  CHECK(seen.names.contains(process_image::system_path("ld-android.so")));
  seen = Seen{};
  seen.stop = 17;
  CHECK(thread.call(iterate, {callback, 0}) == 17);
  CHECK(seen.calls == 1);
  CHECK(seen.valid);
}
