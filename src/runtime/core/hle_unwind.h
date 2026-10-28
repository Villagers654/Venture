#pragma once
#include "core/guest.h"
#include "cpu/stubs.h"
#include <functional>
namespace venture { class Library; }
namespace venture::hle {
void record_unwind_stub(GuestAddr address, std::uint32_t size, GuestStub kind, std::uint32_t argument = 0);
void for_each_unwind_image(const std::function<void(const Library&)>& visit);
GuestAddr unwind_exidx(GuestAddr pc, std::int32_t* count);
bool is_unwind_leaf(GuestAddr pc);
bool legacy_wait_unwind_observation();  // opt-in stepping census, never the default
}
