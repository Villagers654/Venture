#pragma once
// The classes a Dalvik executable (classes.dex) defines, read from its header
// tables (dex format, "class_defs"), without loading it.
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace venture::android::dex {
// Type descriptors of the defined classes ("Lcom/example/Main;"). Throws for
// bytes that are not a dex file.
std::vector<std::string> classes(std::span<const std::uint8_t> dex);
}  // namespace venture::android::dex
