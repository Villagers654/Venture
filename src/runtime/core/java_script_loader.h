#pragma once
#include <filesystem>
#include <functional>
#include <span>
#include <string_view>

namespace venture {
// Prepare supported embedded script-loader configurations before guest
// initializers run. Original APKs and script files remain unchanged.
std::size_t prepare_java_script_loaders(
    std::span<const std::filesystem::path> libraries,
    const std::function<std::filesystem::path(std::string_view)>& resolve_absolute);
}
