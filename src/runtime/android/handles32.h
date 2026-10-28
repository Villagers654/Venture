#pragma once
#include "core/guest.h"
#include <memory>
#include <string_view>

namespace venture::android::handles32 {
// NDK opaque pointers are generation-checked tokens on ILP32. Owners publish
// once at construction and retire on close, so handles cannot resurrect after
// a host allocator reuses an address. Resolve pins storage across an HLE call.
void publish(std::shared_ptr<void>, std::string_view tag);
void retire(GuestAddr host);
GuestAddr encode(GuestAddr host, std::string_view tag);
std::shared_ptr<void> resolve(std::uint32_t token, std::string_view tag);
}  // namespace venture::android::handles32
