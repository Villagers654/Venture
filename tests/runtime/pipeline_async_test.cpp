// Background guest graphics pipeline creation (gfx/pipeline_trace.cpp)
// against a simulated driver: proxies resolve at bind, pending creations keep
// the objects they read alive, and anything not reproducible stays synchronous.
#include <doctest/doctest.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include "gfx/pipeline_trace.h"
#include "hle/vulkan/deferred_recording.h"

namespace {
struct Driver {
  std::mutex mutex;
  std::condition_variable changed;
  bool hold = false;              // creations wait while set
  unsigned holding = 0;           // creations waiting for hold
  bool slow_destroy = false;      // destroys take a while
  std::thread::id creator;        // thread of the last creation
  unsigned bad_copies = 0;        // creations whose copied description was wrong
  std::vector<std::uint64_t> created, bound, destroyed, destroyed_modules;
  std::uint64_t next = 0x10000;
};
Driver driver;
void* dispatch_table[4];
struct FakeHandle { void* table = dispatch_table; } device_object, command_object;
VkDevice device() { return reinterpret_cast<VkDevice>(&device_object); }
VkCommandBuffer command() { return reinterpret_cast<VkCommandBuffer>(&command_object); }

VkResult VKAPI_CALL create(VkDevice, VkPipelineCache, std::uint32_t count, const VkGraphicsPipelineCreateInfo* infos,
                           const VkAllocationCallbacks*, VkPipeline* out) {
  std::unique_lock lock(driver.mutex);
  ++driver.holding;
  driver.changed.notify_all();
  driver.changed.wait(lock, [] { return !driver.hold; });
  --driver.holding;
  driver.creator = std::this_thread::get_id();
  for (std::uint32_t i = 0; i < count; ++i) {
    // The copy must still name the stage entry point and specialization data. Counted,
    // not asserted: this runs on pipeline workers, outside any test case.
    const auto& stage = infos[i].pStages[0];
    if (std::strcmp(stage.pName, "main") != 0 || !stage.pSpecializationInfo || stage.pSpecializationInfo->dataSize != 4)
      ++driver.bad_copies;
    out[i] = reinterpret_cast<VkPipeline>(driver.next += 0x10);
    driver.created.push_back(reinterpret_cast<std::uint64_t>(out[i]));
  }
  driver.changed.notify_all();
  return VK_SUCCESS;
}
void VKAPI_CALL bind(VkCommandBuffer, VkPipelineBindPoint, VkPipeline pipeline) {
  std::lock_guard lock(driver.mutex);
  driver.bound.push_back(reinterpret_cast<std::uint64_t>(pipeline));
}
void VKAPI_CALL destroy(VkDevice, VkPipeline pipeline, const VkAllocationCallbacks*) {
  std::unique_lock lock(driver.mutex);
  if (driver.slow_destroy) {
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    lock.lock();
  }
  driver.destroyed.push_back(reinterpret_cast<std::uint64_t>(pipeline));
  driver.changed.notify_all();
}
void VKAPI_CALL destroy_module(VkDevice, VkShaderModule module, const VkAllocationCallbacks*) {
  std::lock_guard lock(driver.mutex);
  driver.destroyed_modules.push_back(reinterpret_cast<std::uint64_t>(module));
  driver.changed.notify_all();
}
PFN_vkVoidFunction VKAPI_CALL gdpa(VkDevice, const char* name) {
  if (!std::strcmp(name, "vkCreateGraphicsPipelines")) return reinterpret_cast<PFN_vkVoidFunction>(create);
  if (!std::strcmp(name, "vkCmdBindPipeline")) return reinterpret_cast<PFN_vkVoidFunction>(bind);
  if (!std::strcmp(name, "vkDestroyPipeline")) return reinterpret_cast<PFN_vkVoidFunction>(destroy);
  if (!std::strcmp(name, "vkDestroyShaderModule")) return reinterpret_cast<PFN_vkVoidFunction>(destroy_module);
  return nullptr;
}
template<class F> F proc(const char* name) { return reinterpret_cast<F>(venture::gfx::pipeline_proc(name)); }

struct Description {
  std::uint32_t constant = 7;
  VkSpecializationMapEntry entry{0, 0, 4};
  VkSpecializationInfo special{1, &entry, 4, &constant};
  VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  explicit Description(std::uint64_t module) {
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = reinterpret_cast<VkShaderModule>(module);
    stage.pName = "main";
    stage.pSpecializationInfo = &special;
    raster.rasterizerDiscardEnable = VK_TRUE;
    info.stageCount = 1;
    info.pStages = &stage;
    info.pRasterizationState = &raster;
    info.basePipelineIndex = -1;
  }
};
bool wait_until(auto predicate) {
  std::unique_lock lock(driver.mutex);
  return driver.changed.wait_for(lock, std::chrono::seconds(10), predicate);
}
// A destroyed pipeline's background creation is over, and its real pipeline (if any)
// destroyed: nothing of a test reaches the driver after it ends.
bool settled(VkPipeline pipeline) {
  return wait_until([&] { return !venture::gfx::pipeline_pending(pipeline) && driver.created.size() == driver.destroyed.size(); });
}
unsigned bad_copies() { std::lock_guard lock(driver.mutex); return driver.bad_copies; }
void registered() {
  // Released at exit like a destroyed device, which waits for every background
  // creation: workers must not call the driver above after its state is destroyed
  // (this object is destroyed first, being constructed after it).
  static const struct Registration {
    Registration() { venture::gfx::register_pipeline_device(device(), gdpa, false, venture::profile::PipelineSource::guest, true); }
    ~Registration() {
      // A test that failed while holding creations must not hang the exit.
      { std::lock_guard lock(driver.mutex); driver.hold = false; }
      driver.changed.notify_all();
      venture::gfx::release_pipeline_device(device());
    }
  } once;
  (void)once;
}
}  // namespace

TEST_CASE("Background pipelines resolve at bind and keep their shader modules until created") {
  registered();
  auto create_pipelines = proc<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines");
  auto bind_pipeline = proc<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
  auto destroy_pipeline = proc<PFN_vkDestroyPipeline>("vkDestroyPipeline");
  auto destroy_shader = proc<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
  REQUIRE(create_pipelines);
  REQUIRE(bind_pipeline);
  REQUIRE(destroy_pipeline);
  REQUIRE(destroy_shader);
  { std::lock_guard lock(driver.mutex); driver.hold = true; driver.created.clear(); driver.bound.clear(); }
  VkPipeline pipeline{};
  {
    Description description(0x500);
    REQUIRE(create_pipelines(device(), VK_NULL_HANDLE, 1, &description.info, nullptr, &pipeline) == VK_SUCCESS);
    // The description is the caller's to free at once.
    std::memset(static_cast<void*>(&description), 0xcd, sizeof(description));
  }
  REQUIRE(pipeline);
  // The module the pending creation reads is destroyed only after it.
  destroy_shader(device(), reinterpret_cast<VkShaderModule>(0x500), nullptr);
  { std::lock_guard lock(driver.mutex); CHECK(driver.destroyed_modules.empty()); CHECK(driver.created.empty()); }
  { std::lock_guard lock(driver.mutex); driver.hold = false; }
  driver.changed.notify_all();
  REQUIRE(wait_until([] { return driver.destroyed_modules.size() == 1; }));
  CHECK(driver.creator != std::this_thread::get_id());
  bind_pipeline(command(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  std::uint64_t real;
  {
    std::lock_guard lock(driver.mutex);
    REQUIRE(driver.created.size() == 1);
    real = driver.created[0];
    REQUIRE(driver.bound.size() == 1);
    CHECK(driver.bound[0] == real);
    CHECK(reinterpret_cast<std::uint64_t>(pipeline) != real);
  }
  destroy_pipeline(device(), pipeline, nullptr);
  std::lock_guard lock(driver.mutex);
  CHECK(driver.destroyed.back() == real);
  CHECK(driver.bad_copies == 0);
}

TEST_CASE("Binding a pipeline no worker has finished waits for its creation") {
  registered();
  auto create_pipelines = proc<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines");
  auto bind_pipeline = proc<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
  auto destroy_pipeline = proc<PFN_vkDestroyPipeline>("vkDestroyPipeline");
  { std::lock_guard lock(driver.mutex); driver.hold = true; driver.created.clear(); driver.bound.clear(); }
  Description description(0x600);
  VkPipeline pipeline{};
  REQUIRE(create_pipelines(device(), VK_NULL_HANDLE, 1, &description.info, nullptr, &pipeline) == VK_SUCCESS);
  std::thread release([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    { std::lock_guard lock(driver.mutex); driver.hold = false; }
    driver.changed.notify_all();
  });
  bind_pipeline(command(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  release.join();
  {
    std::lock_guard lock(driver.mutex);
    REQUIRE(driver.created.size() == 1);
    REQUIRE(driver.bound.size() == 1);
    CHECK(driver.bound[0] == driver.created[0]);
  }
  destroy_pipeline(device(), pipeline, nullptr);
}

TEST_CASE("Destroying a pipeline still being created destroys the real one when it is done") {
  registered();
  auto create_pipelines = proc<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines");
  auto destroy_pipeline = proc<PFN_vkDestroyPipeline>("vkDestroyPipeline");
  { std::lock_guard lock(driver.mutex); driver.hold = true; driver.created.clear(); driver.destroyed.clear(); }
  Description description(0x700);
  VkPipeline pipeline{};
  REQUIRE(create_pipelines(device(), VK_NULL_HANDLE, 1, &description.info, nullptr, &pipeline) == VK_SUCCESS);
  // Give a worker the time to start it, then destroy while it waits.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  destroy_pipeline(device(), pipeline, nullptr);
  { std::lock_guard lock(driver.mutex); driver.hold = false; }
  driver.changed.notify_all();
  // Either the creation was dropped before it started, or its result is destroyed.
  CHECK(settled(pipeline));
  CHECK(bad_copies() == 0);
}

TEST_CASE("Creation the background copy cannot reproduce stays synchronous") {
  registered();
  auto create_pipelines = proc<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines");
  { std::lock_guard lock(driver.mutex); driver.hold = false; driver.created.clear(); }
  Description description(0x800);
  // Feedback the caller reads must be written before the call returns.
  VkPipelineCreationFeedback feedback{};
  VkPipelineCreationFeedbackCreateInfo node{VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, nullptr, &feedback, 0, nullptr};
  description.info.pNext = &node;
  VkPipeline pipeline{};
  REQUIRE(create_pipelines(device(), VK_NULL_HANDLE, 1, &description.info, nullptr, &pipeline) == VK_SUCCESS);
  std::lock_guard lock(driver.mutex);
  REQUIRE(driver.created.size() == 1);
  CHECK(driver.creator == std::this_thread::get_id());
  CHECK(reinterpret_cast<std::uint64_t>(pipeline) == driver.created[0]);
}

TEST_CASE("Releasing a device returns only after its background creations stopped calling it") {
  registered();
  FakeHandle other_object;
  const auto other = reinterpret_cast<VkDevice>(&other_object);
  venture::gfx::register_pipeline_device(other, gdpa, false, venture::profile::PipelineSource::guest, true);
  auto create_pipelines = proc<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines");
  auto destroy_pipeline = proc<PFN_vkDestroyPipeline>("vkDestroyPipeline");
  { std::lock_guard lock(driver.mutex); driver.hold = true; driver.created.clear(); driver.destroyed.clear(); }
  Description description(0x780);
  VkPipeline pipeline{};
  REQUIRE(create_pipelines(other, VK_NULL_HANDLE, 1, &description.info, nullptr, &pipeline) == VK_SUCCESS);
  // A worker is creating it: the destroy is left to that worker.
  REQUIRE(wait_until([] { return driver.holding == 1; }));
  destroy_pipeline(other, pipeline, nullptr);
  { std::lock_guard lock(driver.mutex); driver.slow_destroy = true; }
  std::thread release([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    { std::lock_guard lock(driver.mutex); driver.hold = false; }
    driver.changed.notify_all();
  });
  // Like vkDestroyDevice's layer: after this the device is gone.
  venture::gfx::release_pipeline_device(other);
  {
    std::lock_guard lock(driver.mutex);
    REQUIRE(driver.created.size() == 1);
    CHECK(driver.destroyed == driver.created);
    driver.slow_destroy = false;
  }
  release.join();
}

// Deferred recording (hle/vulkan/deferred_recording.h): a command buffer that
// binds a pipeline still being created records its later commands and replays
// them, in order, once every such pipeline exists.
namespace {
struct Log {
  std::mutex mutex;
  std::vector<std::string> events;
  void add(std::string e) { std::lock_guard lock(mutex); events.push_back(std::move(e)); }
};
Log command_log;
void VKAPI_CALL logged_bind(VkCommandBuffer c, VkPipelineBindPoint point, VkPipeline pipeline) {
  // Through the background layer, which resolves the proxy.
  proc<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(c, point, pipeline);
  std::lock_guard lock(driver.mutex);
  command_log.add("bind " + std::to_string(driver.bound.back()));
}
void VKAPI_CALL logged_push(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, std::uint32_t offset,
                            std::uint32_t size, const void* values) {
  std::uint32_t v = 0;
  std::memcpy(&v, values, std::min<std::uint32_t>(size, 4));
  command_log.add("push " + std::to_string(offset) + " " + std::to_string(v));
}
void VKAPI_CALL logged_draw(VkCommandBuffer, std::uint32_t vertices, std::uint32_t, std::uint32_t, std::uint32_t) {
  command_log.add("draw " + std::to_string(vertices));
}
void VKAPI_CALL logged_viewport(VkCommandBuffer, std::uint32_t, std::uint32_t count, const VkViewport* v) {
  command_log.add("viewport " + std::to_string(count) + " " + std::to_string(int(v[count - 1].width)));
}
void VKAPI_CALL logged_push_descriptors(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, std::uint32_t,
                                        std::uint32_t, const VkWriteDescriptorSet*) {
  command_log.add("push descriptors");
}
VkResult VKAPI_CALL logged_end(VkCommandBuffer) { return VK_SUCCESS; }
VkResult VKAPI_CALL logged_reset(VkCommandBuffer, VkCommandBufferResetFlags) { return VK_SUCCESS; }
using venture::vulkan::deferred::intercept;
using venture::vulkan::deferred::Key;
Key key() { return reinterpret_cast<Key>(command()); }
VkCommandPool no_pool(Key) { return VK_NULL_HANDLE; }
VkPipeline pending_pipeline(std::uint64_t module) {
  registered();
  auto create_pipelines = proc<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines");
  {
    std::lock_guard lock(driver.mutex);
    driver.hold = true;
    driver.created.clear();
    driver.bound.clear();
    driver.destroyed.clear();
  }
  { std::lock_guard lock(command_log.mutex); command_log.events.clear(); }
  Description description(module);
  VkPipeline pipeline{};
  REQUIRE(create_pipelines(device(), VK_NULL_HANDLE, 1, &description.info, nullptr, &pipeline) == VK_SUCCESS);
  REQUIRE(venture::gfx::pipeline_pending(pipeline));
  return pipeline;
}
void release_later(std::thread& t) {
  t = std::thread([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    { std::lock_guard lock(driver.mutex); driver.hold = false; }
    driver.changed.notify_all();
  });
}
std::vector<std::string> events() { std::lock_guard lock(command_log.mutex); return command_log.events; }
}  // namespace

TEST_CASE("Commands after binding a pipeline still being created replay in order at the end of the buffer") {
  const auto pipeline = pending_pipeline(0x900);
  const auto c = command();
  CHECK(intercept<"vkCmdBindPipeline">(key(), no_pool, static_cast<PFN_vkCmdBindPipeline>(logged_bind), c,
                                       VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline));
  {
    // Arguments are the caller's to reuse as soon as the call returns.
    std::uint32_t value = 42;
    std::vector<VkViewport> viewports(2, VkViewport{0, 0, 640, 480, 0, 1});
    CHECK(intercept<"vkCmdPushConstants">(key(), no_pool, static_cast<PFN_vkCmdPushConstants>(logged_push), c,
                                          VkPipelineLayout{}, VK_SHADER_STAGE_VERTEX_BIT, 8u, 4u, &value));
    CHECK(intercept<"vkCmdSetViewport">(key(), no_pool, static_cast<PFN_vkCmdSetViewport>(logged_viewport), c, 0u,
                                        2u, viewports.data()));
    value = 0;
    viewports.assign(2, VkViewport{});
  }
  CHECK(intercept<"vkCmdDraw">(key(), no_pool, static_cast<PFN_vkCmdDraw>(logged_draw), c, 3u, 1u, 0u, 0u));
  CHECK(events().empty());
  std::thread release;
  release_later(release);
  // The end replays everything first and is then left to run normally.
  CHECK_FALSE(intercept<"vkEndCommandBuffer">(key(), no_pool, static_cast<PFN_vkEndCommandBuffer>(logged_end), c));
  release.join();
  CHECK_FALSE(venture::gfx::pipeline_pending(pipeline));
  std::uint64_t real;
  { std::lock_guard lock(driver.mutex); REQUIRE(driver.created.size() == 1); real = driver.created[0]; }
  const std::vector<std::string> expected{"bind " + std::to_string(real), "push 8 42", "viewport 2 640", "draw 3"};
  CHECK(events() == expected);
  // Nothing remains recorded: the next command runs at once.
  CHECK_FALSE(intercept<"vkCmdDraw">(key(), no_pool, static_cast<PFN_vkCmdDraw>(logged_draw), c, 6u, 1u, 0u, 0u));
  proc<PFN_vkDestroyPipeline>("vkDestroyPipeline")(device(), pipeline, nullptr);
}

TEST_CASE("A command deferred recording cannot copy replays what was recorded before it runs") {
  const auto pipeline = pending_pipeline(0xa00);
  const auto c = command();
  CHECK(intercept<"vkCmdBindPipeline">(key(), no_pool, static_cast<PFN_vkCmdBindPipeline>(logged_bind), c,
                                       VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline));
  CHECK(intercept<"vkCmdDraw">(key(), no_pool, static_cast<PFN_vkCmdDraw>(logged_draw), c, 3u, 1u, 0u, 0u));
  std::thread release;
  release_later(release);
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  CHECK_FALSE(intercept<"vkCmdPushDescriptorSetKHR">(key(), no_pool,
      static_cast<PFN_vkCmdPushDescriptorSetKHR>(logged_push_descriptors), c, VK_PIPELINE_BIND_POINT_GRAPHICS,
      VkPipelineLayout{}, 0u, 1u, &write));
  release.join();
  const auto log = events();
  REQUIRE(log.size() == 2);
  CHECK(log[0].starts_with("bind "));
  CHECK(log[1] == "draw 3");
  proc<PFN_vkDestroyPipeline>("vkDestroyPipeline")(device(), pipeline, nullptr);
}

TEST_CASE("Resetting a command buffer discards what deferred recording kept") {
  const auto pipeline = pending_pipeline(0xb00);
  const auto c = command();
  CHECK(intercept<"vkCmdBindPipeline">(key(), no_pool, static_cast<PFN_vkCmdBindPipeline>(logged_bind), c,
                                       VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline));
  CHECK_FALSE(intercept<"vkResetCommandBuffer">(key(), no_pool, static_cast<PFN_vkResetCommandBuffer>(logged_reset),
                                                c, VkCommandBufferResetFlags{}));
  { std::lock_guard lock(driver.mutex); driver.hold = false; }
  driver.changed.notify_all();
  CHECK_FALSE(intercept<"vkEndCommandBuffer">(key(), no_pool, static_cast<PFN_vkEndCommandBuffer>(logged_end), c));
  CHECK(events().empty());
  CHECK(venture::vulkan::deferred::active.load() == 0);
  proc<PFN_vkDestroyPipeline>("vkDestroyPipeline")(device(), pipeline, nullptr);
  CHECK(settled(pipeline));
  CHECK(bad_copies() == 0);
}
