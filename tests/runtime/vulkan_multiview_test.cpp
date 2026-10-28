#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <optional>
#include <set>

#include "hle/vulkan/multiview.h"

namespace {
namespace vertex {
#include "vulkan_multiview.vert.inc"
}
namespace fragment {
#include "vulkan_multiview.frag.inc"
}
using venture::vulkan::VaryingSlot;
std::map<std::uint32_t, std::uint32_t> locations(std::span<const std::uint32_t> code) {
  std::map<std::uint32_t, std::uint32_t> result;
  for (std::size_t i = 5; i < code.size(); i += code[i] >> 16)
    if ((code[i] & 65535) == 71 && code[i] >> 16 == 4 && code[i + 2] == 30)
      result[code[i + 1]] = code[i + 3];
  return result;
}
std::set<std::uint32_t> flat_locations(std::span<const std::uint32_t> code) {
  const auto slots = locations(code);
  std::set<std::uint32_t> result;
  for (std::size_t i = 5; i < code.size(); i += code[i] >> 16)
    if ((code[i] & 65535) == 71 && code[i] >> 16 == 3 && code[i + 2] == 14 &&
        slots.contains(code[i + 1]))
      result.insert(slots.at(code[i + 1]));
  return result;
}
}  // namespace
TEST_CASE("Vulkan ViewIndex transport preserves only proven per-view scalar values") {
  const std::vector<VaryingSlot> expected{{7, 0}, {11, 0}};
  const auto outputs = venture::vulkan::view_index_outputs(vertex::decode_shader, "main");
  CHECK(outputs == expected);  // Reject attributes, mixed branches, different
                               // casts, pointer escapes and vectors.
  CHECK(venture::vulkan::scalar_varyings(fragment::decode_shader, "main", false) == expected);
  const auto v = venture::vulkan::flat_varyings(vertex::decode_shader, "main", true, expected);
  const auto f = venture::vulkan::flat_varyings(fragment::decode_shader, "main", false, expected);
  CHECK(flat_locations(v) == (std::set<std::uint32_t>{7, 11}));
  CHECK(flat_locations(f) == (std::set<std::uint32_t>{7, 11}));
  CHECK(flat_locations(vertex::decode_shader).empty());
  CHECK(flat_locations(fragment::decode_shader).empty());
  CHECK(venture::vulkan::flat_varyings(v, "main", true, expected) == v);
  CHECK(venture::vulkan::flat_varyings(f, "main", false, expected) == f);
}
