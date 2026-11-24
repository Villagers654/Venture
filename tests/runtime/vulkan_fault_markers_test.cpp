#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "common/environment.h"
#include "hle/vulkan/fault_markers.h"
#include "hle/vulkan/internal.h"
#include "hle/vulkan/texture.h"
#include "vulkan_test_support.h"

using namespace venture;
using namespace venture::vulkan::fault_markers;

namespace {
// VENTURE_VULKAN_BREADCRUMBS for one scope.
struct Breadcrumbs {
  explicit Breadcrumbs(const char* value) { set(value); }
  ~Breadcrumbs() { set(""); }
  static void set(const char* value) {
#ifdef _WIN32
    _putenv_s("VENTURE_VULKAN_BREADCRUMBS", value);
#else
    if (*value) setenv("VENTURE_VULKAN_BREADCRUMBS", value, 1); else unsetenv("VENTURE_VULKAN_BREADCRUMBS");
#endif
    environment::refresh();
  }
};
Pass astc_decode() {
  Pass p{PassKind::decode, VK_FORMAT_ASTC_8x8_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, 2048, 2048, 0, 5592464,
         {256, 256, 1}};
  p.site = {VK_IMAGE_TILING_LINEAR, {2048, 2048, 1}, 0, 0, 0};
  return p;
}
Pass decoded_copy() {
  Pass p{PassKind::decoded_copy, VK_FORMAT_ASTC_8x8_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, 2048, 2048, 0, 16777216};
  p.site = {VK_IMAGE_TILING_LINEAR, {2048, 2048, 1}, 0, 0, 0};
  return p;
}
bool contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }
}  // namespace

TEST_CASE("fault marker words locate a command buffer's stop among its Venture passes") {
  // Two passes: began 1; pass 0 began 2, ended 3; pass 1 began 4, ended 5; end 6.
  CHECK(locate(2, 0, 0).state == Position::not_started);
  CHECK(locate(2, began, 0).state == Position::in_commands);
  CHECK(locate(2, began, 0).pass == 0);
  CHECK_FALSE(locate(2, began, 0).next_pass_started);
  // Pass 0 began issuing while the game's commands before it still ran.
  auto p = locate(2, pass_began(0), 0);
  CHECK(p.state == Position::in_commands);
  CHECK(p.next_pass_started);
  // Everything before pass 0 finished; pass 0 did not.
  p = locate(2, pass_began(0), pass_began(0));
  CHECK(p.state == Position::in_pass);
  CHECK(p.pass == 0);
  // Pass 0 finished; the game's commands between the passes ran.
  p = locate(2, pass_began(0), pass_ended(0));
  CHECK(p.state == Position::in_commands);
  CHECK(p.pass == 1);
  CHECK_FALSE(p.next_pass_started);
  p = locate(2, pass_began(1), pass_began(1));
  CHECK(p.state == Position::in_pass);
  CHECK(p.pass == 1);
  p = locate(2, pass_began(1), pass_ended(1));
  CHECK(p.state == Position::in_commands);
  CHECK(p.pass == 2);
  CHECK(locate(2, pass_began(1), ended(2)).state == Position::completed);
  // No passes: begun, then ended.
  CHECK(locate(0, began, 0).state == Position::in_commands);
  CHECK(locate(0, began, ended(0)).state == Position::completed);
  // Words that cannot come from this recording.
  CHECK(locate(1, began, began).state == Position::unknown);
  CHECK(locate(1, 9, 0).state == Position::unknown);
  CHECK(locate(1, began, 7).state == Position::unknown);
}

TEST_CASE("fault marker word pairs go to the recording released longest ago") {
  Slots slots(3);
  CHECK(slots.acquire(10) == 0u);
  CHECK(slots.acquire(11) == 1u);
  slots.release(0);
  // Slot 2 was never used; 0 waits behind it, so a freed command buffer's
  // execution that still runs cannot write into a new owner's words at once.
  CHECK(slots.acquire(12) == 2u);
  CHECK(slots.owner(0) == 10u);
  CHECK(slots.acquire(13) == 0u);
  CHECK(slots.owner(0) == 13u);
  CHECK_FALSE(slots.acquire(14).has_value());
  CHECK(slots.owner(7) == 0u);
}

TEST_CASE("fault marker passes describe what the GPU was doing") {
  CHECK(describe(astc_decode()) ==
        "ASTC 8x8 texture decode of a 2048x2048 linear image (mip 0 at 2048x2048, layer 0; reads from byte 0 of a "
        "5592464-byte source; 256x256x1 workgroups; RGBA8 output)");
  CHECK(describe(decoded_copy()) == "copy of a decoded ASTC 8x8 mip (RGBA8) into a 2048x2048 linear image (mip 0 at "
                                    "2048x2048, layer 0; reads from byte 0 of a 16777216-byte source)");
  Pass bc7{PassKind::bc7_encode, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_BC7_SRGB_BLOCK, 64, 32, 0, 8192, {2, 1, 1}};
  bc7.site = {VK_IMAGE_TILING_OPTIMAL, {128, 64, 1}, 1, 3, 0};
  CHECK(describe(bc7) == "BC7 encode of a decoded mip of a 128x64 image (mip 1 at 64x32, layer 3; reads from byte 0 "
                         "of a 8192-byte source; 2x1x1 workgroups)");
  Pass sparse{PassKind::decode_image, VK_FORMAT_ASTC_6x6_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_SRGB, 30, 30, 64, 1024,
              {4, 4, 1}};
  sparse.site = {VK_IMAGE_TILING_OPTIMAL, {60, 60, 4}, 1, 0, 2};
  CHECK(describe(sparse) == "ASTC 6x6 sRGB texture decode into a sparse RGBA8 sRGB companion of a 60x60x4 image (mip "
                            "1 at 30x30, slice 2; reads from byte 64 of a 1024-byte source; 4x4x1 workgroups)");
  Pass depth{PassKind::depth_conversion, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT, 4096, 1, 0, 16384,
             {64, 1, 1}};
  CHECK(describe(depth) == "conversion of 4096 depth words from 24-bit depth to 32-bit float depth (64x1x1 "
                           "workgroups)");
  // A pass recorded without an image (direct decodes).
  Pass bare{PassKind::decode, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, 16, 16, 0, 0, {2, 2, 1}};
  CHECK(describe(bare) == "ETC2 RGB8 texture decode (16x16 texels; 2x2x1 workgroups; RGBA8 output)");
  CHECK(format_name(VK_FORMAT_ASTC_10x5_SFLOAT_BLOCK) == "ASTC 10x5 HDR");
  CHECK(format_name(VK_FORMAT_ASTC_12x12_SRGB_BLOCK) == "ASTC 12x12 sRGB");
}

TEST_CASE("fault marker readout names the command buffer and Venture pass the GPU stopped in") {
  // Recording 1 finished; 2 stopped inside its second pass (an ASTC decode of a linear image) after its first
  // finished; 3 waits on a semaphore; 4 was re-recorded since its submission (so that one finished); 5 carried
  // no markers (its pool's family could not write them).
  const std::vector<Pass> one{astc_decode()}, two{decoded_copy(), astc_decode()}, none;
  std::map<std::uint32_t, Readout> readouts;
  readouts[1] = {&one, pass_began(0), ended(1)};
  readouts[2] = {&two, pass_began(1), pass_began(1)};
  readouts[3] = {&none, 0, 0};
  readouts[4] = {};
  std::deque<Submitted> history{{134, 0, 0, 1, 0xa1, 1}, {135, 0, 0, 2, 0xa2, 2}, {135, 0, 1, 2, 0xa3, 3},
                                {136, 1, 0, 1, 0xa4, 4}, {137, 0, 0, 1, 0xa5, 0}};
  const Lookup lookup = [&](std::uint32_t id) -> std::optional<Readout> {
    auto it = readouts.find(id);
    if (it == readouts.end()) return std::nullopt;
    return it->second;
  };
  auto q = read_queue("queue 0x1 (family 0, index 0)", history, lookup);
  CHECK(q.venture);
  CHECK(q.running == 1);
  CHECK(q.waiting == 1);
  CHECK(q.completed == 2);
  CHECK(q.where == "submission 135, command buffer 1 of 2 (0xa2)");
  CHECK(q.stopped == "inside Venture's " + describe(astc_decode()));
  REQUIRE(q.lines.size() == 2);
  CHECK(contains(q.lines[0], "queue 0x1 (family 0, index 0): submission 135, command buffer 1 of 2 (0xa2) had begun "
                             "and not finished: inside Venture's ASTC 8x8 texture decode"));
  CHECK(q.lines[1] == "queue 0x1 (family 0, index 0): of its last 5 submitted command buffers 2 completed, 1 had "
                      "begun and not finished, 1 had not started, 1 carried no markers");
  const auto text = verdict({q});
  CHECK(contains(text, "the GPU stopped inside Venture's ASTC 8x8 texture decode of a 2048x2048 linear image (mip 0 "
                       "at 2048x2048, layer 0;"));
  CHECK(contains(text, "(submission 135, command buffer 1 of 2 (0xa2))"));

  // The game's own commands, after the last pass finished; on another queue a later command buffer stopped
  // inside a pass of a secondary it executes, which the verdict prefers.
  readouts[2] = {&two, pass_began(1), pass_ended(1)};
  q = read_queue("graphics", history, lookup);
  CHECK_FALSE(q.venture);
  CHECK(q.stopped == "in the game's commands after its last Venture pass (" + describe(astc_decode()) +
                     ") completed");
  readouts[6] = {&none, began, 0};
  readouts[6].secondaries = {7};
  readouts[7] = {&one, pass_began(0), pass_began(0)};
  auto compute = read_queue("compute", {{3, 0, 0, 1, 0xb6, 6}}, lookup);
  CHECK(compute.venture);
  CHECK(contains(compute.lines[1], "a secondary command buffer it executes stopped inside Venture's ASTC 8x8"));
  CHECK(contains(verdict({q, compute}), "the GPU stopped inside Venture's ASTC 8x8 texture decode"));
  CHECK(contains(verdict({q, compute}), "(submission 3, command buffer 1 of 1 (0xb6))"));

  // A slot handed to a newer recording no longer holds this one's markers.
  readouts[2].readable = false;
  q = read_queue("graphics", history, lookup);
  CHECK(q.stopped.empty());
  CHECK(contains(q.lines.back(), "1 markers were overwritten"));

  // Every command buffer that began finished: the fault was outside the game's command buffers.
  readouts[2] = {&two, pass_began(1), ended(2)};
  q = read_queue("graphics", history, lookup);
  CHECK(contains(verdict({q}), "every guest command buffer that began had finished (1 had not started)"));
  CHECK(verdict({}) == "no marked guest command buffer was submitted before the loss");
}

TEST_CASE("fault markers use the driver's checkpoints where offered, buffer markers only when asked for") {
  auto extension = [](const char* name) {
    VkExtensionProperties e{};
    std::snprintf(e.extensionName, sizeof(e.extensionName), "%s", name);
    return e;
  };
  const VkExtensionProperties both[]{extension(VK_AMD_BUFFER_MARKER_EXTENSION_NAME),
                                     extension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME)};
  CHECK(vulkan::fault_markers_for(both) == vulkan::FaultMarkers::checkpoints);
  // Buffer markers cost GPU time every frame: only VENTURE_VULKAN_BREADCRUMBS=1.
  CHECK(vulkan::fault_markers_for(std::span(both, 1)) == vulkan::FaultMarkers::none);
  CHECK(vulkan::fault_markers_for(std::span(both + 1, 1)) == vulkan::FaultMarkers::checkpoints);
  CHECK(vulkan::fault_markers_for({}) == vulkan::FaultMarkers::none);
  Breadcrumbs on("1");
  CHECK(vulkan::fault_markers_for(both) == vulkan::FaultMarkers::checkpoints);
  CHECK(vulkan::fault_markers_for(std::span(both, 1)) == vulkan::FaultMarkers::buffer_marker);
  Breadcrumbs off("0");
  CHECK(vulkan::fault_markers_for(both) == vulkan::FaultMarkers::none);
}

TEST_CASE("NV checkpoints give each submission's fault marker words") {
  std::deque<Submitted> history{{1, 0, 0, 1, 0xa, 1}, {2, 0, 0, 1, 0xb, 2}, {3, 0, 0, 1, 0xc, 3},
                                {4, 0, 0, 1, 0xd, 4}, {5, 0, 0, 1, 0xe, 0}};
  auto passes = [](std::uint32_t id) { return id == 2 ? std::size_t(2) : std::size_t(0); };
  // Recording 2 finished up to its first pass's start, recording 3 began; 4 had not started.
  auto words = checkpoint_progress(history, std::uint64_t(3) << 32 | began, std::uint64_t(2) << 32 | pass_began(0),
                                   passes);
  CHECK(words[1] == std::pair{ended(0), ended(0)});
  CHECK(words[2] == std::pair{ended(2), pass_began(0)});
  CHECK(locate(2, words[2].first, words[2].second).state == Position::in_pass);
  CHECK(words[3] == std::pair{began, 0u});
  CHECK(words[4] == std::pair{0u, 0u});
  // Both stages in one recording.
  words = checkpoint_progress(history, std::uint64_t(2) << 32 | pass_began(1), std::uint64_t(2) << 32 | pass_ended(0),
                              passes);
  CHECK(words[2] == std::pair{pass_began(1), pass_ended(0)});
  CHECK(words[3] == std::pair{0u, 0u});
}

namespace {
// A queue's verdict from a live device, or nothing without markers.
std::optional<QueueVerdict> live_verdict(VkDevice device) {
  auto queues = vulkan::fault_marker_verdicts(device);
  if (queues.empty()) return std::nullopt;
  return queues.front();
}
}  // namespace

TEST_CASE("Vulkan fault markers follow guest command buffers and texture passes on the GPU") {
  struct ForceEmulation {
    ForceEmulation() { vulkan::force_texture_emulation(true); }
    ~ForceEmulation() { vulkan::force_texture_emulation(false); }
  } force;
  Breadcrumbs markers("1");  // buffer markers are opt-in
  test::VulkanDevice gpu(false, false, true);  // timeline semaphores
  // Live words need buffer markers; checkpoints report progress after a loss only.
  if (vulkan::device_fault_markers(gpu.device) != vulkan::FaultMarkers::buffer_marker) {
    MESSAGE("the host driver writes no live GPU marker words");
    return;
  }
  constexpr std::uint32_t size = 256;
  test::VulkanImage image(gpu, VK_FORMAT_ASTC_8x8_UNORM_BLOCK, size, size);
  test::VulkanBuffer upload(gpu, (size / 8) * (size / 8) * 16);
  std::mt19937 random(164);
  for (VkDeviceSize i = 0; i < upload.size / 4; ++i) static_cast<std::uint32_t*>(upload.mapped)[i] = random();
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {size, size, 1};

  // A command buffer that uploads (a decode pass and a copy pass) and then waits on the GPU for an event the host
  // sets: its markers show it stopped in the game's commands after its last pass.
  VkEventCreateInfo event_info{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
  VkEvent event{};
  REQUIRE(test::vk_call("vkCreateEvent", gpu.device, &event_info, nullptr, &event) == VK_SUCCESS);
  gpu.begin();
  image.transition();
  test::vk_call("vkCmdCopyBufferToImage", gpu.command, upload.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1u,
                &copy);
  test::vk_call("vkCmdWaitEvents", gpu.command, 1u, &event, VK_PIPELINE_STAGE_HOST_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, nullptr, 0u, nullptr, 0u, nullptr);
  REQUIRE(test::vk_call("vkEndCommandBuffer", gpu.command) == VK_SUCCESS);
  // A second one waits for a timeline value only the host signals: it has not started.
  VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
  timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline};
  VkSemaphore gate{};
  REQUIRE(test::vk_call("vkCreateSemaphore", gpu.device, &semaphore_info, nullptr, &gate) == VK_SUCCESS);
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = gpu.pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer held{};
  REQUIRE(test::vk_call("vkAllocateCommandBuffers", gpu.device, &alloc, &held) == VK_SUCCESS);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  REQUIRE(test::vk_call("vkBeginCommandBuffer", held, &begin) == VK_SUCCESS);
  REQUIRE(test::vk_call("vkEndCommandBuffer", held) == VK_SUCCESS);

  // The GPU waits on the event for at most a few hundred milliseconds, well inside any driver's hang timeout.
  std::atomic<bool> released{false};
  auto release = [&] {
    if (!released.exchange(true)) test::vk_call("vkSetEvent", gpu.device, event);
  };
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &gpu.command;
  REQUIRE(test::vk_call("vkQueueSubmit", gpu.queue, 1u, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
  std::thread watchdog([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    release();
  });
  const std::uint64_t open = 1;
  VkTimelineSemaphoreSubmitInfo wait_value{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  wait_value.waitSemaphoreValueCount = 1;
  wait_value.pWaitSemaphoreValues = &open;
  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkSubmitInfo waiting{VK_STRUCTURE_TYPE_SUBMIT_INFO, &wait_value};
  waiting.waitSemaphoreCount = 1;
  waiting.pWaitSemaphores = &gate;
  waiting.pWaitDstStageMask = &wait_stage;
  waiting.commandBufferCount = 1;
  waiting.pCommandBuffers = &held;
  // A separate call: Venture defers a whole vkQueueSubmit that waits for an unsignalled timeline value.
  REQUIRE(test::vk_call("vkQueueSubmit", gpu.queue, 1u, &waiting, VK_NULL_HANDLE) == VK_SUCCESS);

  std::optional<QueueVerdict> stopped;
  while (!released.load()) {
    auto v = live_verdict(gpu.device);
    if (v && v->running == 1 && v->stopped.starts_with("in the game's commands after its last Venture pass")) {
      stopped = v;
      break;
    }
    std::this_thread::yield();
  }
  release();
  watchdog.join();
  REQUIRE(stopped);
  INFO(stopped->stopped);
  CHECK(stopped->stopped.starts_with("in the game's commands after its last Venture pass (copy of a decoded ASTC "
                                     "8x8 mip (RGBA8) into a 256x256 image (mip 0 at 256x256, layer 0;"));
  CHECK(stopped->waiting == 1);
  CHECK(contains(stopped->where, "command buffer 1 of 1"));

  // The first finishes once the event is set; the second still waits.
  for (int attempt = 0; attempt < 400; ++attempt) {
    auto v = live_verdict(gpu.device);
    if (v && !v->running) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  auto v = live_verdict(gpu.device);
  REQUIRE(v);
  CHECK(v->running == 0);
  CHECK(v->waiting == 1);
  CHECK(v->completed >= 1);
  CHECK(contains(vulkan::fault_markers::verdict({*v}), "every guest command buffer that began had finished"));
  VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
  signal.semaphore = gate;
  signal.value = open;
  REQUIRE(test::vk_call("vkSignalSemaphore", gpu.device, &signal) == VK_SUCCESS);
  REQUIRE(test::vk_call("vkQueueWaitIdle", gpu.queue) == VK_SUCCESS);
  v = live_verdict(gpu.device);
  REQUIRE(v);
  CHECK(v->waiting == 0);
  CHECK(v->running == 0);
  test::vk_call("vkFreeCommandBuffers", gpu.device, gpu.pool, 1u, &held);
  test::vk_call("vkDestroySemaphore", gpu.device, gate, nullptr);
  test::vk_call("vkDestroyEvent", gpu.device, event, nullptr);
}
