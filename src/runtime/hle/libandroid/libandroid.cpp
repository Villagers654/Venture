#include "hle/libandroid/libandroid.h"
#include "hle/libandroid/parts.h"
namespace venture::hle {
void register_libandroid() {
  auto lib = std::make_shared<HleLibrary>("libandroid.so");
  using namespace android_hle;
  register_assets(*lib);
  register_windows(*lib);
  register_looper(*lib);
  register_configuration(*lib);
  register_input(*lib);
  register_choreographer(*lib);
  register_services(*lib);
  register_hardware_buffers(*lib);
  Linker::get().register_hle_library(std::move(lib));
}
}  // namespace venture::hle
