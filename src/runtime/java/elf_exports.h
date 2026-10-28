#pragma once
#include <functional>
#include <string_view>

#include "core/linker.h"
namespace venture::java {
// Read mapped ELF dynamic tables through the existing Library map/phdr contract.
// No new linker virtual method, and no second filesystem/path resolver.
void exported_java_symbols(const Library&, const std::function<void(std::string_view, GuestAddr)>&);
}  // namespace venture::java
