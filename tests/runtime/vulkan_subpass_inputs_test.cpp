#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "hle/vulkan/internal.h"

namespace {
namespace spirv10 {
#include "vulkan_subpass_inputs.1.0.inc"
}
namespace spirv13 {
#include "vulkan_subpass_inputs.1.1.inc"
}
struct Instruction {
  std::uint32_t op;
  std::span<const std::uint32_t> words;  // operands
};
std::vector<Instruction> instructions(std::span<const std::uint32_t> code) {
  std::vector<Instruction> out;
  for (std::size_t i = 5; i < code.size();) {
    const auto n = code[i] >> 16;
    REQUIRE(n > 0);
    REQUIRE(i + n <= code.size());
    out.push_back({code[i] & 0xffff, code.subspan(i + 1, n - 1)});
    i += n;
  }
  return out;
}
std::size_t count(const std::vector<Instruction>& code, std::uint32_t op) {
  std::size_t n = 0;
  for (const auto& i : code) n += i.op == op;
  return n;
}
}  // namespace

TEST_CASE("Subpass inputs become texel fetches from the fragment's view layer") {
  using venture::vulkan::sample_subpass_inputs;
  for (std::span<const std::uint32_t> original : {std::span<const std::uint32_t>(spirv10::decode_shader),
                                                  std::span<const std::uint32_t>(spirv13::decode_shader)}) {
    CAPTURE(original[1]);
    const auto before = instructions(original);
    REQUIRE(count(before, 98) == 2);  // OpImageRead
    const auto rewritten = sample_subpass_inputs(original);
    REQUIRE_FALSE(rewritten.empty());
    CHECK(rewritten[3] > original[3]);
    const auto after = instructions(rewritten);
    CHECK(count(after, 98) == 0);
    CHECK(count(after, 95) == 2);  // OpImageFetch
    bool multiview = false, extension = false, view_index = false, frag_coord = false, flat_view = false;
    std::uint32_t view_variable = 0;
    for (const auto& i : after) {
      if (i.op == 17 && i.words[0] == 4439) multiview = true;
      if (i.op == 10) extension |= std::string(reinterpret_cast<const char*>(i.words.data())) == "SPV_KHR_multiview";
      if (i.op == 71 && i.words.size() == 3 && i.words[1] == 11 && i.words[2] == 4440) {
        view_index = true;
        view_variable = i.words[0];
      }
      if (i.op == 71 && i.words.size() == 3 && i.words[1] == 11 && i.words[2] == 15) frag_coord = true;
      CHECK_FALSE((i.op == 71 && i.words.size() >= 2 && i.words[1] == 43));  // InputAttachmentIndex
      // Image types: none is a subpass input; the converted ones are sampled 2D arrays.
      if (i.op == 25) {
        CHECK(i.words[2] != 6);
        if (i.words[2] == 1) {
          CHECK(i.words[4] == 1);
          CHECK(i.words[6] == 1);
        }
      }
      if (i.op == 95) {
        // Result type, result, image, coordinate, then Sample (multisampled) or Lod 0.
        REQUIRE(i.words.size() == 6);
        CHECK((i.words[4] == 0x40 || i.words[4] == 0x2));
      }
    }
    for (const auto& i : after)
      if (i.op == 71 && i.words.size() == 2 && i.words[0] == view_variable && i.words[1] == 14) flat_view = true;
    CHECK(multiview);
    CHECK(extension == (original[1] < 0x00010300));
    CHECK(view_index);
    CHECK(flat_view);
    CHECK(frag_coord);
    // The fragment entry point lists the added inputs.
    for (const auto& i : after)
      if (i.op == 15) CHECK(std::find(i.words.begin(), i.words.end(), view_variable) != i.words.end());
    // A module without subpass inputs is left alone, and a rewritten one has none left.
    CHECK(sample_subpass_inputs(rewritten).empty());
  }
}
TEST_CASE("Subpass inputs are read as sampled images on AMD's Windows driver unless set otherwise") {
  using venture::vulkan::sampled_input_driver;
  CHECK(sampled_input_driver(VK_DRIVER_ID_AMD_PROPRIETARY, true));
  CHECK_FALSE(sampled_input_driver(VK_DRIVER_ID_NVIDIA_PROPRIETARY, true));
  CHECK_FALSE(sampled_input_driver(VK_DRIVER_ID_MESA_RADV, true));
  CHECK_FALSE(sampled_input_driver(VK_DRIVER_ID_AMD_PROPRIETARY, false));
  venture::vulkan::set_sampled_input_reads(true);
  CHECK(venture::vulkan::sampled_input_type(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
  CHECK(venture::vulkan::sampled_input_type(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
  venture::vulkan::set_sampled_input_reads(false);
  CHECK(venture::vulkan::sampled_input_type(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT);
}
