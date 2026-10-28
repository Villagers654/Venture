#pragma once
#include <cstdint>
#include <span>
#include "core/guest.h"
namespace venture {
class GuestThread;
namespace tls {
struct Module { std::uint64_t id; std::int64_t offset; };
Module add_module(std::span<const std::byte> image, std::uint64_t size, std::uint64_t alignment, std::uint64_t skew = 0);
void attach(GuestThread& thread);
void detach(GuestThread& thread);
GuestAddr get_addr(GuestThread& thread, std::uint64_t module, std::uint64_t offset);
GuestAddr descriptor_resolver();
GuestAddr weak_descriptor_resolver();
GuestAddr get_addr_thunk();
}
}
