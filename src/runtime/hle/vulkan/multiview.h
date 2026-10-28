#pragma once
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace venture::vulkan {
using VaryingSlot = std::pair<std::uint32_t, std::uint32_t>;  // Location, Component
// Scalar float varyings which carry a single, unchanged ViewIndex expression.
std::vector<VaryingSlot> view_index_outputs(std::span<const std::uint32_t>, std::string_view entry);
std::vector<VaryingSlot> scalar_varyings(std::span<const std::uint32_t>, std::string_view entry,
                                         bool output);
std::vector<std::uint32_t> flat_varyings(std::span<const std::uint32_t>, std::string_view entry,
                                         bool output, std::span<const VaryingSlot> slots);
}  // namespace venture::vulkan
