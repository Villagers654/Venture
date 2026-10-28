#include "xr/capture.h"
#include <zlib.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace venture::xr {
namespace {
void be32(std::ostream& out, std::uint32_t value) {
  const std::array<char, 4> bytes{static_cast<char>(value >> 24), static_cast<char>(value >> 16),
                                 static_cast<char>(value >> 8), static_cast<char>(value)};
  out.write(bytes.data(), bytes.size());
}
void chunk(std::ostream& out, const char* kind, const std::vector<unsigned char>& bytes) {
  be32(out, static_cast<std::uint32_t>(bytes.size()));
  out.write(kind, 4);
  out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  auto crc = crc32(0, reinterpret_cast<const Bytef*>(kind), 4);
  if (!bytes.empty()) crc = crc32(crc, bytes.data(), static_cast<uInt>(bytes.size()));
  be32(out, static_cast<std::uint32_t>(crc));
}
void png(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height, const void* pixels) {
  // PNG RGBA8, filter None on every row; zlib owns compression, no image approximations.
  std::vector<unsigned char> rows((width * 4ull + 1) * height);
  for (std::uint32_t y = 0; y < height; ++y)
    std::memcpy(rows.data() + y * (width * 4ull + 1) + 1,
                static_cast<const unsigned char*>(pixels) + y * width * 4ull, width * 4ull);
  uLongf size = compressBound(static_cast<uLong>(rows.size()));
  std::vector<unsigned char> compressed(size);
  if (compress2(compressed.data(), &size, rows.data(), static_cast<uLong>(rows.size()), Z_BEST_SPEED) != Z_OK)
    throw std::runtime_error("PNG compression failed");
  compressed.resize(size);
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out.exceptions(std::ios::badbit | std::ios::failbit);
  const unsigned char signature[]{137, 80, 78, 71, 13, 10, 26, 10};
  out.write(reinterpret_cast<const char*>(signature), sizeof(signature));
  std::vector<unsigned char> header(13);
  for (int i = 0; i < 4; ++i) {
    header[i] = static_cast<unsigned char>(width >> (24 - i * 8));
    header[4 + i] = static_cast<unsigned char>(height >> (24 - i * 8));
  }
  header[8] = 8; header[9] = 6;
  chunk(out, "IHDR", header);
  chunk(out, "IDAT", compressed);
  chunk(out, "IEND", {});
}
}
struct PendingCapture {
  XrGraphicsBindingVulkanKHR graphics;
  PFN_vkGetInstanceProcAddr get;
  VkBuffer buffer{};
  VkDeviceMemory memory{};
  VkCommandPool pool{};
  VkFence fence{};
  void* mapped{};
  bool submitted = false;
  bool encode_srgb = false;
  template<class F> F fn(const char* name) const { return reinterpret_cast<F>(get(graphics.instance, name)); }
  VkFormat format{};
  std::uint32_t width = 0, height = 0;
  std::filesystem::path path;
  PendingCapture(const XrGraphicsBindingVulkanKHR& g, PFN_vkGetInstanceProcAddr f) : graphics(g), get(f) {}
  ~PendingCapture() {
    if (submitted) fn<PFN_vkWaitForFences>("vkWaitForFences")(graphics.device, 1, &fence, VK_TRUE, UINT64_MAX);
    if (mapped) fn<PFN_vkUnmapMemory>("vkUnmapMemory")(graphics.device, memory);
    if (fence) fn<PFN_vkDestroyFence>("vkDestroyFence")(graphics.device, fence, nullptr);
    if (pool) fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(graphics.device, pool, nullptr);
    if (buffer) fn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(graphics.device, buffer, nullptr);
    if (memory) fn<PFN_vkFreeMemory>("vkFreeMemory")(graphics.device, memory, nullptr);
  }
};
namespace {
void check(VkResult result, const char* operation) {
  if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}
}
void PendingCaptureDeleter::operator()(PendingCapture* capture) const { delete capture; }
PendingCapturePtr submit_eye(const XrGraphicsBindingVulkanKHR& g, PFN_vkGetInstanceProcAddr get,
                             VkImage image, VkFormat format, const XrSwapchainSubImage& sub,
                             const std::filesystem::path& path, std::string* error) {
  try {
    if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB &&
        format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_B8G8R8A8_SRGB)
      throw std::runtime_error("Eye capture supports RGBA8/BGRA8 UNORM and sRGB images");
    const auto width = static_cast<std::uint32_t>(sub.imageRect.extent.width);
    const auto height = static_cast<std::uint32_t>(sub.imageRect.extent.height);
    if (!width || !height || width > 16384 || height > 16384) throw std::runtime_error("Invalid eye capture extent");
    PendingCapturePtr owned(new PendingCapture(g, get));
    auto& r = *owned;
    r.format = format; r.width = width; r.height = height; r.path = path;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = width * height * 4ull;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    check(r.fn<PFN_vkCreateBuffer>("vkCreateBuffer")(g.device, &bi, nullptr, &r.buffer), "capture buffer");
    VkMemoryRequirements requirements{};
    r.fn<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(g.device, r.buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    r.fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(g.physicalDevice, &properties);
    // The CPU reads the whole buffer back: uncached (write-combined) memory
    // reads an eye at a few hundred MB/s, so prefer a host-cached type.
    auto find_type = [&](VkMemoryPropertyFlags wanted) {
      std::uint32_t type = 0;
      while (type < properties.memoryTypeCount &&
             (!(requirements.memoryTypeBits & (1u << type)) ||
              (properties.memoryTypes[type].propertyFlags & wanted) != wanted)) ++type;
      return type;
    };
    auto type = find_type(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type == properties.memoryTypeCount) type = find_type(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (type == properties.memoryTypeCount) throw std::runtime_error("No host-visible eye readback memory");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
    check(r.fn<PFN_vkAllocateMemory>("vkAllocateMemory")(g.device, &ai, nullptr, &r.memory), "capture memory");
    check(r.fn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(g.device, r.buffer, r.memory, 0), "capture bind");
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = g.queueFamilyIndex;
    check(r.fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(g.device, &pi, nullptr, &r.pool), "capture pool");
    VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, r.pool,
                                    VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer command{};
    check(r.fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(g.device, &ci, &command), "capture command");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(r.fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &begin), "capture begin");
    // OpenXR Vulkan swapchain contract: waited color images are in COLOR_ATTACHMENT_OPTIMAL.
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, sub.imageArrayIndex, 1};
    auto pipeline_barrier = r.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    pipeline_barrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, sub.imageArrayIndex, 1};
    copy.imageOffset = {sub.imageRect.offset.x, sub.imageRect.offset.y, 0};
    copy.imageExtent = {width, height, 1};
    r.fn<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
        command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.buffer, 1, &copy);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    pipeline_barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = r.buffer; host.size = VK_WHOLE_SIZE;
    pipeline_barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                     0, 0, nullptr, 1, &host, 0, nullptr);
    check(r.fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command), "capture end");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(r.fn<PFN_vkCreateFence>("vkCreateFence")(g.device, &fi, nullptr, &r.fence), "capture fence");
    VkQueue queue{};
    r.fn<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(g.device, g.queueFamilyIndex, g.queueIndex, &queue);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
    check(r.fn<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, r.fence), "capture submit");
    r.submitted = true;
    return owned;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return nullptr;
  }
}
void encode_srgb_rgba8(unsigned char* rgba, std::size_t bytes) {
  static const auto table = [] {
    std::array<unsigned char, 256> t{};
    for (int i = 0; i < 256; ++i) {
      const double v = i / 255.0, e = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
      t[i] = static_cast<unsigned char>(std::lround(std::clamp(e, 0.0, 1.0) * 255));
    }
    return t;
  }();
  for (std::size_t i = 0; i + 3 < bytes; i += 4)
    for (int c = 0; c < 3; ++c) rgba[i + c] = table[rgba[i + c]];
}
void encode_srgb_on_finish(PendingCapture& r) { r.encode_srgb = true; }
bool finish_eye(PendingCapture& r, std::vector<unsigned char>& pixels, std::string* error) {
  try {
    const auto device = r.graphics.device;
    check(r.fn<PFN_vkWaitForFences>("vkWaitForFences")(device, 1, &r.fence, VK_TRUE, UINT64_MAX), "capture wait");
    check(r.fn<PFN_vkMapMemory>("vkMapMemory")(device, r.memory, 0, VK_WHOLE_SIZE, 0, &r.mapped), "capture map");
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, r.memory, 0, VK_WHOLE_SIZE};
    check(r.fn<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(device, 1, &range), "capture invalidate");
    pixels.resize(r.width * r.height * 4ull);
    std::memcpy(pixels.data(), r.mapped, pixels.size());
    if (r.format == VK_FORMAT_B8G8R8A8_UNORM || r.format == VK_FORMAT_B8G8R8A8_SRGB)
      for (std::size_t i = 0; i < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
    if (r.encode_srgb) encode_srgb_rgba8(pixels.data(), pixels.size());
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}
bool write_pending_eye(PendingCapture& capture, std::string* error) {
  std::vector<unsigned char> pixels;
  if (!finish_eye(capture, pixels, error)) return false;
  try { png(capture.path, capture.width, capture.height, pixels.data()); }
  catch (const std::exception& e) { if (error) *error = e.what(); return false; }
  return true;
}
bool read_eye(const XrGraphicsBindingVulkanKHR& g, PFN_vkGetInstanceProcAddr get,
              VkImage image, VkFormat format, const XrSwapchainSubImage& sub,
              std::vector<unsigned char>& pixels, std::string* error) {
  auto pending = submit_eye(g, get, image, format, sub, {}, error);
  return pending && finish_eye(*pending, pixels, error);
}
bool capture_eye(const XrGraphicsBindingVulkanKHR& g, PFN_vkGetInstanceProcAddr get,
                 VkImage image, VkFormat format, const XrSwapchainSubImage& sub,
                 const std::filesystem::path& path, std::string* error) {
  std::vector<unsigned char> pixels;
  if (!read_eye(g, get, image, format, sub, pixels, error)) return false;
  try { png(path, sub.imageRect.extent.width, sub.imageRect.extent.height, pixels.data()); }
  catch (const std::exception& e) { if (error) *error = e.what(); return false; }
  return true;
}
void write_eye(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
               const std::vector<unsigned char>& pixels, const XrRect2Di& rect) {
  if (rect.offset.x < 0 || rect.offset.y < 0 || rect.extent.width <= 0 || rect.extent.height <= 0 ||
      std::uint64_t(rect.offset.x) + rect.extent.width > width ||
      std::uint64_t(rect.offset.y) + rect.extent.height > height || pixels.size() != width * height * 4ull)
    throw std::runtime_error("Invalid eye crop");
  std::vector<unsigned char> crop(rect.extent.width * rect.extent.height * 4ull);
  for (int y = 0; y < rect.extent.height; ++y)
    std::memcpy(crop.data() + y * rect.extent.width * 4ull,
        pixels.data() + ((y + rect.offset.y) * width + rect.offset.x) * 4ull, rect.extent.width * 4ull);
  png(path, rect.extent.width, rect.extent.height, crop.data());
}
}
