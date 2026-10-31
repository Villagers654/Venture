#pragma once
#include "core/guest.h"

namespace venture::hle::ovravatar {
// Avatar SDK 1 driver, which Horizon's com.oculus.avatar.loader.EntryPoint hands
// to games' avatar loaders: the guest address of its function lookup.
GuestAddr driver_lookup();
}  // namespace venture::hle::ovravatar
