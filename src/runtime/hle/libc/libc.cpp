#include <memory>

#include "core/linker.h"
#include "core/process.h"
#include "core/signals.h"
#include "hle/libc/libc_parts.h"
namespace venture::hle::libc {
void register_libraries() {
  auto make = [](const char* name) {
    return std::make_shared<HleLibrary>(
        name, std::vector<std::string>{process_config().guest_system_lib_dir() + "/" + name,
                                       "/apex/com.android.runtime/" + std::string(process_config().isa() == GuestIsa::a32 ? "lib" : "lib64") + "/bionic/" + name});
  };
  auto c = make("libc.so");
  register_strings(*c);
  register_memory(*c);
  register_stdlib(*c);
  register_ctype_locale(*c);
  register_formatting(*c);
  register_time_format(*c);
  register_misc(*c);
  register_threads(*c);
  register_signals(*c);
  register_files(*c);
  register_net(*c);
  register_memory_map(*c);
  register_process(*c);
  register_clock(*c);
  register_syscall(*c);
  Linker::get().register_hle_library(c);
  std::shared_ptr<HleLibrary> libdl;
  for (auto [name, registrar] : {std::pair{"libm.so", &register_libm},
                                 {"libdl.so", &register_libdl},
                                 {"liblog.so", &register_liblog},
                                 {"libz.so", &register_libz},
                                 {"libstdc++.so", &register_libstdcpp}}) {
    auto lib = make(name);
    registrar(*lib);
    if (registrar == &register_libdl) libdl = lib;
    Linker::get().register_hle_library(lib);
  }
  // bionic's linker presents itself as ld-android.so (the __loader_* exports
  // libdl.so forwards to). A32 keeps its existing flat libdl view.
  if (process_config().isa() == GuestIsa::a64) {
    auto loader = make("ld-android.so");
    register_loader(*loader);
    shape_dlopen(*libdl, *loader);
    Linker::get().register_hle_library(loader);
  }
  // libc++_shared.so is always guest code shipped with the APK.
  // bionic's linker calls debuggerd_init() before loading the app's libraries.
  signals::install_debuggerd_handlers();
}
}  // namespace venture::hle::libc
