#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "common/environment.h"
#include "hle/vulkan/internal.h"
#include "vulkan_test_support.h"

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
TEST_CASE("A multisampled subpass input of a one-sample attachment is read single-sampled") {
  using venture::vulkan::sample_subpass_inputs;
  using venture::vulkan::single_sample_subpass_inputs;
  for (std::span<const std::uint32_t> original : {std::span<const std::uint32_t>(spirv10::decode_shader),
                                                  std::span<const std::uint32_t>(spirv13::decode_shader)}) {
    CAPTURE(original[1]);
    // Input 0 is the subpassInputMS, input 1 an ordinary subpassInput.
    CHECK(single_sample_subpass_inputs(original, 0).empty());
    CHECK(single_sample_subpass_inputs(original, 2).empty());  // only the single-sampled input has one sample
    const auto single = single_sample_subpass_inputs(original, 1 | 2);
    REQUIRE_FALSE(single.empty());
    CHECK(single[3] == original[3]);
    const auto after = instructions(single);
    // One subpass image type is left, single-sampled: the changed one would duplicate the declared one.
    std::size_t subpass_types = 0;
    for (const auto& i : after)
      if (i.op == 25 && i.words[2] == 6) {
        ++subpass_types;
        CHECK(i.words[5] == 0);
      }
    CHECK(subpass_types == 1);
    // Both reads remain, neither with a Sample operand.
    CHECK(count(after, 98) == 2);
    for (const auto& i : after)
      if (i.op == 98) CHECK(i.words.size() == 4);
    // Every type a pointer or load names is still declared.
    std::vector<std::uint32_t> declared;
    for (const auto& i : after)
      if (i.op >= 19 && i.op <= 39) declared.push_back(i.words[0]);
    for (const auto& i : after) {
      if (i.op == 32) CHECK(std::find(declared.begin(), declared.end(), i.words[2]) != declared.end());
      if (i.op == 61) CHECK(std::find(declared.begin(), declared.end(), i.words[0]) != declared.end());
    }
    // Read as sampled images, both inputs are level-0 fetches, not multisampled ones.
    const auto sampled = sample_subpass_inputs(single);
    REQUIRE_FALSE(sampled.empty());
    std::size_t fetches = 0;
    for (const auto& i : instructions(sampled)) {
      if (i.op == 25) CHECK(i.words[5] == 0);
      if (i.op == 95) {
        ++fetches;
        REQUIRE(i.words.size() == 6);
        CHECK(i.words[4] == 0x2);
      }
    }
    CHECK(fetches == 2);
  }
}
TEST_CASE("Subpass inputs are read as sampled images where the probe shows another view's layer") {
  using venture::vulkan::sampled_input_driver;
  using Probe = venture::vulkan::SubpassInputProbe;
  // The probe decides, whatever the driver.
  CHECK(sampled_input_driver(VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS, true, Probe::other_layer));
  CHECK(sampled_input_driver(VK_DRIVER_ID_NVIDIA_PROPRIETARY, true, Probe::other_layer));
  CHECK_FALSE(sampled_input_driver(VK_DRIVER_ID_AMD_PROPRIETARY, true, Probe::own_layer));
  // Without a result, every driver takes the sampled reads, which are right everywhere.
  CHECK(sampled_input_driver(VK_DRIVER_ID_AMD_PROPRIETARY, true, Probe::unavailable));
  CHECK(sampled_input_driver(VK_DRIVER_ID_NVIDIA_PROPRIETARY, true, Probe::unavailable));
  CHECK(sampled_input_driver(VK_DRIVER_ID_MESA_RADV, true, Probe::unavailable));
  CHECK_FALSE(sampled_input_driver(VK_DRIVER_ID_AMD_PROPRIETARY, false, Probe::other_layer));
  // The environment variable overrides the probe.
  auto set = [](const char* value) {
#ifdef _WIN32
    _putenv_s("VENTURE_VULKAN_SAMPLED_INPUTS", value);
#else
    if (*value) setenv("VENTURE_VULKAN_SAMPLED_INPUTS", value, 1);
    else unsetenv("VENTURE_VULKAN_SAMPLED_INPUTS");
#endif
    venture::environment::refresh();
  };
  set("0");
  CHECK_FALSE(sampled_input_driver(VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS, true, Probe::other_layer));
  set("1");
  CHECK(sampled_input_driver(VK_DRIVER_ID_NVIDIA_PROPRIETARY, true, Probe::own_layer));
  set("");
  CHECK_FALSE(venture::vulkan::sampled_input_override());
  venture::vulkan::set_sampled_input_reads(true);
  CHECK(venture::vulkan::sampled_input_type(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
  CHECK(venture::vulkan::sampled_input_type(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
  venture::vulkan::set_sampled_input_reads(false);
  CHECK(venture::vulkan::sampled_input_type(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT);
}
TEST_CASE("Multiview subpass input probe results are kept per driver") {
  using Probe = venture::vulkan::SubpassInputProbe;
  namespace v = venture::vulkan;
  const auto directory = std::filesystem::temp_directory_path() / "venture-subpass-probe-test";
  std::filesystem::remove_all(directory);
  const auto file = v::subpass_input_probe_file(directory, 0x8086, 0xe20b, 0x65a2f6);
  CHECK(file != v::subpass_input_probe_file(directory, 0x8086, 0xe20b, 0x65a2f7));  // a new driver probes again
  CHECK(file != v::subpass_input_probe_file(directory, 0x8086, 0xe20c, 0x65a2f6));
  CHECK(file != v::subpass_input_probe_file(directory, 0x1002, 0xe20b, 0x65a2f6));
  CHECK(v::cached_subpass_input_probe(file) == Probe::unavailable);
  v::cache_subpass_input_probe(file, Probe::unavailable);  // not kept: the next launch tries again
  CHECK_FALSE(std::filesystem::exists(file));
  v::cache_subpass_input_probe(file, Probe::other_layer);
  CHECK(v::cached_subpass_input_probe(file) == Probe::other_layer);
  v::cache_subpass_input_probe(file, Probe::own_layer);
  CHECK(v::cached_subpass_input_probe(file) == Probe::own_layer);
  std::filesystem::remove_all(directory);
}
TEST_CASE("Multiview subpass input probe renders two views on the host GPU and its kept result decides") {
  using venture::test::vk_call;
  using Probe = venture::vulkan::SubpassInputProbe;
  namespace v = venture::vulkan;
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  VkInstance instance{};
  REQUIRE(vk_call("vkCreateInstance", &ci, nullptr, &instance) == VK_SUCCESS);
  std::uint32_t count = 1;
  VkPhysicalDevice physical{};
  REQUIRE(vk_call("vkEnumeratePhysicalDevices", instance, &count, &physical) == VK_SUCCESS);
  vk_call("vkGetPhysicalDeviceQueueFamilyProperties", physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vk_call("vkGetPhysicalDeviceQueueFamilyProperties", physical, &count, families.data());
  std::uint32_t family = 0;
  while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
  REQUIRE(family < count);
  float priority = 1;
  VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueFamilyIndex = family;
  queue.queueCount = 1;
  queue.pQueuePriorities = &priority;
  VkPhysicalDeviceMultiviewFeatures multiview{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
  multiview.multiview = VK_TRUE;
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &multiview};
  di.queueCreateInfoCount = 1;
  di.pQueueCreateInfos = &queue;
  VkDevice device{};
  REQUIRE(vk_call("vkCreateDevice", physical, &di, nullptr, &device) == VK_SUCCESS);
  double milliseconds = 0;
  const auto result = v::probe_subpass_inputs(physical, device, std::span(&queue, 1), &milliseconds);
  MESSAGE("Probe: ", result == Probe::own_layer ? "own layer" : result == Probe::other_layer ? "other layer" : "unavailable",
          " in ", milliseconds, " ms");
  CHECK(result != Probe::unavailable);
  // A kept result stands without probing; without one the probe runs and is kept.
  VkPhysicalDeviceProperties properties{};
  vk_call("vkGetPhysicalDeviceProperties", physical, &properties);
  const auto directory = std::filesystem::temp_directory_path() / "venture-subpass-probe-device-test";
  std::filesystem::remove_all(directory);
  const auto file = v::subpass_input_probe_file(directory, properties.vendorID, properties.deviceID,
                                                properties.driverVersion);
  const auto flipped = result == Probe::own_layer ? Probe::other_layer : Probe::own_layer;
  v::cache_subpass_input_probe(file, flipped);
  v::choose_sampled_input_reads(physical, device, di, directory);
  CHECK(v::sampled_input_reads() == (flipped == Probe::other_layer));
  std::filesystem::remove_all(directory);
  v::choose_sampled_input_reads(physical, device, di, directory);
  CHECK(v::sampled_input_reads() == (result == Probe::other_layer));
  CHECK(v::cached_subpass_input_probe(file) == result);
  std::filesystem::remove_all(directory);
  v::set_sampled_input_reads(false);
  vk_call("vkDestroyDevice", device, nullptr);
  vk_call("vkDestroyInstance", instance, nullptr);
}
