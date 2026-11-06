#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common/environment.h"
#include "common/log.h"
#include "hle/vulkan/breadcrumb_streams.h"
#include "vulkan_test_support.h"

using namespace venture::vulkan::breadcrumb;
namespace {
VkPhysicalDeviceMemoryProperties memory(std::initializer_list<VkMemoryPropertyFlags> types) {
  VkPhysicalDeviceMemoryProperties m{};
  for (auto flags : types) m.memoryTypes[m.memoryTypeCount++].propertyFlags = flags;
  return m;
}
constexpr VkMemoryPropertyFlags local = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
    visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, coherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT, amd_coherent = VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD,
    amd_uncached = VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD;
}  // namespace

TEST_CASE("breadcrumb markers live in host memory that survives a GPU reset") {
  // Resizable BAR lists host-visible VRAM before system memory; AMD lists
  // device-coherent types that need a feature breadcrumbs do not enable.
  auto bar = memory({local, local | visible | coherent, local | visible | coherent | amd_coherent | amd_uncached,
                     visible | coherent | amd_coherent | amd_uncached, visible | coherent, visible | coherent | cached});
  auto chosen = marker_memory_type(bar, ~0u);
  CHECK(chosen.type == 5);
  CHECK(chosen.coherent);
  // Uncached system memory when it is the only coherent host type.
  CHECK(marker_memory_type(bar, ~0u & ~(1u << 5)).type == 4);
  // The buffer's allowed types still decide.
  CHECK(marker_memory_type(bar, 1u << 1).type == 1);
  // Cached non-coherent system memory beats VRAM (reads then invalidate).
  auto noncoherent = memory({local, local | visible | coherent, visible | cached});
  chosen = marker_memory_type(noncoherent, ~0u);
  CHECK(chosen.type == 2);
  CHECK_FALSE(chosen.coherent);
  // No system memory: the previous choice, any host-visible coherent type.
  CHECK(marker_memory_type(memory({local, local | visible | coherent}), ~0u).type == 1);
  auto none = memory({local});
  CHECK(marker_memory_type(none, ~0u).type == none.memoryTypeCount);
}

TEST_CASE("breadcrumb readout names each queue's last completed marker in submission order") {
  std::map<std::uint32_t, std::uint32_t> slots;  // slot -> marker written by the GPU
  auto read = [&](std::uint32_t slot) { return slots.contains(slot) ? slots[slot] : 0u; };
  Streams streams;
  // Graphics queue: command buffer A (markers 10..12, recorded after B) then
  // B (markers 1..3, recorded first, submitted second). A finished, B reached
  // marker 2, C has not started. The largest marker id overall (12) is not the
  // queue's progress; B's 2 is.
  streams.submitted(1, {0xa, {100}});
  streams.submitted(1, {0xb, {101}});
  streams.submitted(1, {0xc, {102}});
  // A command buffer without markers (copies only) shows no progress either way.
  streams.submitted(1, {0xf, {103}, false});
  slots[100] = 12;
  slots[101] = 2;
  // Compute queue: one finished command buffer whose secondary ran last.
  streams.submitted(2, {0xd, {200, 201}});
  slots[200] = 20;
  slots[201] = 21;
  // Transfer queue: nothing reached a marker.
  streams.submitted(3, {0xe, {300}});
  auto progress = streams.progress(read);
  REQUIRE(progress.size() == 3);
  CHECK(progress[0].queue == 1);
  CHECK(progress[0].marker == 2);
  CHECK(progress[0].command == 0xb);
  CHECK(progress[0].later == 1);
  CHECK(progress[1].marker == 21);
  CHECK(progress[1].command == 0xd);
  CHECK(progress[1].later == 0);
  CHECK(progress[2].marker == 0);
  CHECK(progress[2].later == 1);
  // A queue's history is bounded and per device forgettable.
  for (std::size_t i = 0; i < Streams::history + 10; ++i) streams.submitted(4, {i, {400}});
  slots[400] = 7;
  progress = streams.progress(read);
  CHECK(progress[3].command == Streams::history + 9);
  streams.forget([](std::uint64_t queue) { return queue != 4; });
  CHECK(streams.progress(read).size() == 1);
}

namespace {
struct BreadcrumbEnvironment {
  static void set(const char* value) {
#ifdef _WIN32
    _putenv_s("VENTURE_VULKAN_BREADCRUMBS", value ? value : "");
#else
    if (value) setenv("VENTURE_VULKAN_BREADCRUMBS", value, 1);
    else unsetenv("VENTURE_VULKAN_BREADCRUMBS");
#endif
    venture::environment::refresh();
  }
  std::optional<std::string> previous;
  BreadcrumbEnvironment() {
    if (const char* value = std::getenv("VENTURE_VULKAN_BREADCRUMBS")) previous = value;
    set("1");
  }
  ~BreadcrumbEnvironment() { set(previous ? previous->c_str() : nullptr); }
};
struct BreadcrumbLog {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      std::format("venture-breadcrumb-log-{}", std::chrono::steady_clock::now().time_since_epoch().count());
  BreadcrumbLog() { venture::log::set_file(path.string()); }
  ~BreadcrumbLog() { venture::log::set_file(""); std::error_code ignored; std::filesystem::remove(path, ignored); }
  std::vector<std::string> lines() const {
    std::ifstream input(path, std::ios::binary);
    std::vector<std::string> result;
    for (std::string line; std::getline(input, line);) result.push_back(line);
    return result;
  }
};
std::uint32_t marker_after(const std::string& line, std::string_view key) {
  const auto at = line.find(key);
  return at == line.npos ? 0 : std::uint32_t(std::stoul(line.substr(at + key.size())));
}
}  // namespace

TEST_CASE("a stopped queue's breadcrumb report names its last completed dispatch and the command buffers after it") {
  using namespace venture;
  BreadcrumbEnvironment environment;
  BreadcrumbLog log_file;
  test::VulkanDevice gpu(false, false, true);
  // GLSL: void main() {} with local size 1.
  const std::uint32_t code[]{0x07230203, 0x00010000, 0, 5, 0, 0x00020011, 1, 0x0003000e, 0, 1, 0x0005000f, 5, 3,
                             0x6e69616d, 0, 0x00060010, 3, 17, 1, 1, 1, 0x00020013, 1, 0x00030021, 2, 1,
                             0x00050036, 1, 3, 0, 2, 0x000200f8, 4, 0x000100fd, 0x00010038};
  VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  shader_info.codeSize = sizeof(code);
  shader_info.pCode = code;
  VkShaderModule shader{};
  REQUIRE(test::vk_call("vkCreateShaderModule", gpu.device, &shader_info, nullptr, &shader) == VK_SUCCESS);
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  VkPipelineLayout layout{};
  REQUIRE(test::vk_call("vkCreatePipelineLayout", gpu.device, &layout_info, nullptr, &layout) == VK_SUCCESS);
  VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_info.stage.module = shader;
  pipeline_info.stage.pName = "main";
  pipeline_info.layout = layout;
  VkPipeline pipeline{};
  REQUIRE(test::vk_call("vkCreateComputePipelines", gpu.device, VK_NULL_HANDLE, 1u, &pipeline_info, nullptr,
                        &pipeline) == VK_SUCCESS);
  // Indirect arguments (1, 1, 1).
  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = 16;
  buffer_info.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  VkBuffer arguments{};
  REQUIRE(test::vk_call("vkCreateBuffer", gpu.device, &buffer_info, nullptr, &arguments) == VK_SUCCESS);
  VkMemoryRequirements requirements{};
  test::vk_call("vkGetBufferMemoryRequirements", gpu.device, arguments, &requirements);
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = gpu.memory_type(requirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkDeviceMemory memory{};
  REQUIRE(test::vk_call("vkAllocateMemory", gpu.device, &allocation, nullptr, &memory) == VK_SUCCESS);
  REQUIRE(test::vk_call("vkBindBufferMemory", gpu.device, arguments, memory, 0ull) == VK_SUCCESS);
  void* mapped{};
  REQUIRE(test::vk_call("vkMapMemory", gpu.device, memory, 0ull, VK_WHOLE_SIZE, 0u, &mapped) == VK_SUCCESS);
  const std::uint32_t groups[]{1, 1, 1};
  std::memcpy(mapped, groups, sizeof(groups));
  VkSemaphoreTypeCreateInfo timeline_info{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
  timeline_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline_info};
  VkSemaphore release{};
  REQUIRE(test::vk_call("vkCreateSemaphore", gpu.device, &semaphore_info, nullptr, &release) == VK_SUCCESS);
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence{};
  REQUIRE(test::vk_call("vkCreateFence", gpu.device, &fence_info, nullptr, &fence) == VK_SUCCESS);
  VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  command_info.commandPool = gpu.pool;
  command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_info.commandBufferCount = 1;
  VkCommandBuffer held{};
  REQUIRE(test::vk_call("vkAllocateCommandBuffers", gpu.device, &command_info, &held) == VK_SUCCESS);

  // The first command buffer runs to its end. The second waits for a timeline
  // value only the host signals, so the queue holds it before the GPU sees it:
  // the queue has stopped progressing without stalling the GPU. A GPU-side
  // wait (vkCmdWaitEvents on a host event) instead trips amdgpu's 2 s ring
  // timeout and resets the ring.
  gpu.begin();
  test::vk_call("vkCmdBindPipeline", gpu.command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  test::vk_call("vkCmdDispatch", gpu.command, 1u, 1u, 1u);
  test::vk_call("vkCmdDispatch", gpu.command, 1u, 1u, 1u);
  test::vk_call("vkCmdDispatchIndirect", gpu.command, arguments, 0ull);
  REQUIRE(test::vk_call("vkEndCommandBuffer", gpu.command) == VK_SUCCESS);
  VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  REQUIRE(test::vk_call("vkBeginCommandBuffer", held, &begin_info) == VK_SUCCESS);
  test::vk_call("vkCmdBindPipeline", held, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  test::vk_call("vkCmdDispatch", held, 1u, 1u, 1u);
  REQUIRE(test::vk_call("vkEndCommandBuffer", held) == VK_SUCCESS);
  VkSubmitInfo submits[2]{{VK_STRUCTURE_TYPE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SUBMIT_INFO}};
  submits[0].commandBufferCount = 1;
  submits[0].pCommandBuffers = &gpu.command;
  const std::uint64_t wait_value = 1;
  VkTimelineSemaphoreSubmitInfo wait_info{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  wait_info.waitSemaphoreValueCount = 1;
  wait_info.pWaitSemaphoreValues = &wait_value;
  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  submits[1].pNext = &wait_info;
  submits[1].waitSemaphoreCount = 1;
  submits[1].pWaitSemaphores = &release;
  submits[1].pWaitDstStageMask = &wait_stage;
  submits[1].commandBufferCount = 1;
  submits[1].pCommandBuffers = &held;
  // Separate calls: Venture defers a whole vkQueueSubmit call that waits for
  // an unsignalled timeline value, and work it holds never reaches the GPU.
  REQUIRE(test::vk_call("vkQueueSubmit", gpu.queue, 1u, &submits[0], VK_NULL_HANDLE) == VK_SUCCESS);
  REQUIRE(test::vk_call("vkQueueSubmit", gpu.queue, 1u, &submits[1], fence) == VK_SUCCESS);

  // The report is what a device loss logs; markers land asynchronously.
  std::uint32_t last = 0, next = 0;
  std::string last_line, next_line;
  for (int attempt = 0; attempt < 400 && last_line.find(" dispatch indirect cmd=") == last_line.npos; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    vulkan::report_breadcrumbs(gpu.device);
    for (const auto& line : log_file.lines()) {
      // A report's next marker line follows its last completed line; an older report's must not survive.
      if (line.find("last completed") != line.npos) { last = marker_after(line, " marker="); last_line = line; next = 0; next_line.clear(); }
      if (line.find("next marker=") != line.npos) { next = marker_after(line, "next marker="); next_line = line; }
    }
  }
  CHECK(test::vk_call("vkGetFenceStatus", gpu.device, fence) == VK_NOT_READY);
  VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
  signal.semaphore = release;
  signal.value = 1;
  REQUIRE(test::vk_call("vkSignalSemaphore", gpu.device, &signal) == VK_SUCCESS);
  CHECK(test::vk_call("vkWaitForFences", gpu.device, 1u, &fence, VK_TRUE, 10000000000ull) == VK_SUCCESS);

  bool draw_markers = false;
  std::string enabled;
  for (const auto& line : log_file.lines())
    if (line.find("vulkan-breadcrumb") != line.npos && line.find("enabled ") != line.npos) {
      enabled = line;
      draw_markers = line.find("draw_buffer_markers=true") != line.npos;
    }
  INFO(enabled);
  INFO(last_line);
  INFO(next_line);
  REQUIRE_FALSE(enabled.empty());
  CHECK(last != 0);
  CHECK(last_line.find(draw_markers ? "last completed draw marker=" : "last completed portable boundary marker=") !=
        last_line.npos);
  // The completed command buffer's last marker, and the held one after it.
  CHECK(last_line.find(" dispatch indirect cmd=") != last_line.npos);
  CHECK(last_line.find("; 1 later command buffers on this queue completed none") != last_line.npos);
  // Nothing was recorded after the indirect dispatch in its command buffer.
  CHECK(next == 0);
  if (draw_markers) {
    // Per-draw markers live in system memory, which keeps them across a reset.
    const auto at = enabled.find("memory_flags=");
    REQUIRE(at != enabled.npos);
    const bool device_local = std::stoul(enabled.substr(at + 13), nullptr, 16) & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    CHECK_FALSE(device_local);
  }

  test::vk_call("vkFreeCommandBuffers", gpu.device, gpu.pool, 1u, &held);
  test::vk_call("vkDestroyFence", gpu.device, fence, nullptr);
  test::vk_call("vkDestroySemaphore", gpu.device, release, nullptr);
  test::vk_call("vkUnmapMemory", gpu.device, memory);
  test::vk_call("vkDestroyBuffer", gpu.device, arguments, nullptr);
  test::vk_call("vkFreeMemory", gpu.device, memory, nullptr);
  test::vk_call("vkDestroyPipeline", gpu.device, pipeline, nullptr);
  test::vk_call("vkDestroyPipelineLayout", gpu.device, layout, nullptr);
  test::vk_call("vkDestroyShaderModule", gpu.device, shader, nullptr);
}
