#pragma once
#include <functional>

#include "core/hle.h"
#include "core/thread.h"

namespace venture::test {
using VulkanCallback = std::function<GuestCallResult(GuestAddr, const GuestCallArgs&)>;
GuestAddr vulkan_callback_thunk(GuestAddr id, unsigned arguments);
void set_vulkan_callback(VulkanCallback callback);
const hle::FunctionInfo& vulkan_thunk_info(GuestAddr thunk);
}  // namespace venture::test
