#pragma once
#include <optional>
#include <string>
#include <string_view>
namespace venture::java {
// Context File objects/ApplicationInfo paths are HOST paths so ordinary JDK I/O
// works. The VFS should call this at its filename boundary before its normal
// Android mount lookup, accepting only aliases inside ProcessConfig roots.
// A non-matching path returns nullopt; do not grant arbitrary host filesystem
// access to guests. NativeLoader/System.load applies the same mapping in Java.
std::optional<std::string> framework_path_to_guest(std::string_view host_path);
}  // namespace venture::java
