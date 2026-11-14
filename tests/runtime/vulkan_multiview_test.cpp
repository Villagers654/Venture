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
TEST_CASE("Vulkan ViewIndex transport accepts an unsigned ViewIndex as Unity declares it") {
  // Unity's HLSLcc output: uint gl_ViewID_OVR; out float v = float(gl_ViewID_OVR).
  // GLSL cannot declare it, so the module is assembled here.
  constexpr std::uint32_t main = 1, view = 2, out = 3, void_t = 4, fn = 5, uint_t = 6, float_t = 7,
                          in_ptr = 8, out_ptr = 9, label = 10, loaded = 11, converted = 12;
  const std::vector<std::uint32_t> code{
      0x07230203, 0x00010000, 0, 13, 0,
      (2u << 16) | 17, 1,                                    // OpCapability Shader
      (2u << 16) | 17, 4439,                                 // OpCapability MultiView
      (3u << 16) | 14, 0, 1,                                 // OpMemoryModel Logical GLSL450
      (7u << 16) | 15, 0, main, 0x6e69616d, 0, view, out,    // OpEntryPoint Vertex "main"
      (4u << 16) | 71, view, 11, 4440,                       // BuiltIn ViewIndex
      (4u << 16) | 71, out, 30, 1,                           // Location 1
      (2u << 16) | 19, void_t,
      (3u << 16) | 33, fn, void_t,
      (4u << 16) | 21, uint_t, 32, 0,                        // unsigned 32-bit int
      (3u << 16) | 22, float_t, 32,
      (4u << 16) | 32, in_ptr, 1, uint_t,
      (4u << 16) | 32, out_ptr, 3, float_t,
      (4u << 16) | 59, in_ptr, view, 1,
      (4u << 16) | 59, out_ptr, out, 3,
      (5u << 16) | 54, void_t, main, 0, fn,
      (2u << 16) | 248, label,
      (4u << 16) | 61, uint_t, loaded, view,
      (4u << 16) | 112, float_t, converted, loaded,          // OpConvertUToF
      (3u << 16) | 62, out, converted,
      (1u << 16) | 253,
      (1u << 16) | 56};
  const std::vector<VaryingSlot> expected{{1, 0}};
  CHECK(venture::vulkan::view_index_outputs(code, "main") == expected);
  CHECK(flat_locations(venture::vulkan::flat_varyings(code, "main", true, expected)) ==
        (std::set<std::uint32_t>{1}));
}
