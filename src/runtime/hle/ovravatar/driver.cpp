#include "hle/ovravatar/driver.h"
#include "common/log.h"
#include "core/hle.h"
#include "platform/memory.h"
#include <string>

namespace venture::hle::ovravatar {
namespace {
// The lookup answers every ovrAvatar_ function with one that returns zero, so no
// avatar specification, message or asset ever arrives and the game runs without
// avatars, as it does for a user who has none. The loader exits the game if any
// function is missing (First Steps).
void absent(CallContext& ctx) { ctx.return_x(0); }
void lookup(CallContext& ctx) {
  std::string name;
  for (GuestAddr p = ctx.x(0); p && name.size() < 128; ++p) {
    char c = 0;
    if (!platform::read_memory(p, &c, 1) || !c) break;
    name.push_back(c);
  }
  static const GuestAddr function = make_thunk("libovravatar.so", "<no avatar>", absent);
  ctx.return_x(name.starts_with("ovrAvatar") ? function : 0);
}
}  // namespace
GuestAddr driver_lookup() {
  static const GuestAddr address = [] {
    log::info("avatar", "Avatar SDK 1 driver requested; the game runs without avatars");
    return make_thunk("libovravatar.so", "<avatar driver lookup>", lookup);
  }();
  return address;
}
}  // namespace venture::hle::ovravatar
