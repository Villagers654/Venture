#include <doctest/doctest.h>
#include "core/address_space.h"
#include "core/hle_runtime.h"
#include "core/linker_runtime.h"
#include "core/process_image.h"
#include "core/thread_runtime.h"
#include "hle/libc/vfs.h"
#include "hle/registry.h"
#include "java/jvm.h"
#include <map>
#include <string>
#include <string_view>

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
  std::map<std::string, GuestAddr> names;
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
  seen.valid &= seen.names.emplace(name, info.name).second;
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
  for (const auto& [name, address] : seen.names) {
    REQUIRE(AddressSpace::get().is_guest_accessible(address, name.size() + 1, false));
    CHECK(std::string_view(g2h<char>(address), name.size() + 1) ==
          std::string_view(name.c_str(), name.size() + 1));
  }
  seen = Seen{};
  seen.stop = 17;
  CHECK(thread.call(iterate, {callback, 0}) == 17);
  CHECK(seen.calls == 1);
  CHECK(seen.valid);
}

TEST_CASE("A library namespace loads a renamed memfd copy of a system library as that library") {
  // libadrenotools (liblinkernsbypass): a namespace of its own, then a copy of
  // a system library in a memfd whose DT_SONAME it rewrote, opened by
  // descriptor into the namespace to get a second instance. Venture's linker
  // is flat, so the copy is the system library itself.
  AddressSpace::get().initialize(GuestIsa::a64);
  register_all_hle_libraries();
  java::register_jni_library();
  auto& linker = Linker::get();
  REQUIRE(linker.open("libart.so", kRtldNow | kRtldGlobal, nullptr));
  process_image::publish();
  auto& thread = GuestThread::adopt_current();
  auto symbol = [&](const char* library, const char* name) {
    auto* l = linker.open(library, kRtldNow, nullptr);
    REQUIRE(l);
    const auto address = l->find_symbol(name);
    REQUIRE(address);
    return address;
  };
  const auto create = symbol("libdl_android.so", "android_create_namespace");
  const auto link_all = symbol("libdl_android.so", "android_link_namespaces_all_libs");
  const auto loader_create = symbol("ld-android.so", "__loader_android_create_namespace");
  const auto dlopen_ext = symbol("libdl.so", "android_dlopen_ext");
  const auto dlopen = symbol("libdl.so", "dlopen");
  const auto memfd = symbol("libc.so", "memfd_create");
  const std::string hooks = "/data/app/hooks", name = "adrenotools-libvulkan", copy = "default_copy";
  const auto ns = thread.call(create, {h2g(name.c_str()), h2g(hooks.c_str()), 0, 2, 0, 0});
  REQUIRE(ns);
  const auto other = thread.call(loader_create, {h2g(copy.c_str()), 0, 0, 2, 0, 0, 0});
  REQUIRE(other);
  CHECK(other != ns);
  CHECK((thread.call(link_all, {ns, other}) & 0xff) == 1);
  CHECK((thread.call(link_all, {ns, 0}) & 0xff) == 0);

  const std::string soname = "libart.so";
  auto bytes = process_image::file(process_image::system_path(soname));
  REQUIRE(bytes);
  const auto at = bytes->find(soname + '\0');
  REQUIRE(at != std::string::npos);
  bytes->replace(at, soname.size(), std::string(soname.size(), '0'));
  const std::string memfd_name = process_image::system_path(soname);
  const auto fd = static_cast<int>(thread.call(memfd, {h2g(memfd_name.c_str()), 0}));
  REQUIRE(fd >= 0);
  REQUIRE(hle::libc::fd_write(fd, bytes->data(), bytes->size()) == static_cast<std::int64_t>(bytes->size()));
  struct {
    std::uint64_t flags, address, size;
    std::int32_t relro, library_fd;
    std::int64_t offset;
    GuestAddr ns;
  } ext{0x210, 0, 0, 0, fd, 0, ns};
  const auto path = "/proc/self/fd/" + std::to_string(fd);
  const auto loaded = thread.call(dlopen_ext, {h2g(path.c_str()), kRtldNow, h2g(&ext)});
  CHECK(loaded != 0);
  CHECK(loaded == thread.call(dlopen, {h2g(soname.c_str()), kRtldNow}));
  // An extended flag Venture does not implement still fails visibly.
  ext.flags |= 0x400;
  CHECK(thread.call(dlopen_ext, {h2g(path.c_str()), kRtldNow, h2g(&ext)}) == 0);
  hle::libc::FdTable::get().close(fd);
}
