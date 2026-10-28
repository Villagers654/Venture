#include "hle/registry.h"

#include <mutex>
namespace venture::hle::libc {
void register_libraries();
}
namespace venture::hle::ossdk {
void register_libraries();
}
namespace venture {
void register_all_hle_libraries() {
  static std::once_flag once;
  std::call_once(once, [] {
    hle::libc::register_libraries();
    hle::ossdk::register_libraries();
  });
}
}  // namespace venture
