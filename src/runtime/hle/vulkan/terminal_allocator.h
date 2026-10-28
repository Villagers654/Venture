#pragma once
namespace venture::vulkan {
// Caller must have irreversibly stopped/drained guests and Java, flushed saves,
// and proved no remaining host calls. No production resume/reset is supported.
bool begin_terminal_allocator_bridge();
bool terminal_allocator_bridge_healthy();
}
