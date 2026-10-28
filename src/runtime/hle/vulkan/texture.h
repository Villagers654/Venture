#pragma once
#include <span>
#include <vector>

#include "vulkan.h"

namespace venture::vulkan {
struct TextureFormat {
  VkFormat compressed{};
  VkFormat decoded{};
  std::uint32_t width{}, height{}, block_bytes{}, texel_bytes{};
  explicit operator bool() const { return block_bytes != 0; }
};
TextureFormat texture_format(VkFormat format);

// Host storage for a guest image of an emulated compressed format. A guest
// image created with VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT may be viewed with any
// format of its compatibility class (Vulkan spec "Format Compatibility
// Classes": UNORM/SRGB for ETC2 and ASTC, UNORM/SNORM for EAC), restricted to
// VkImageFormatListCreateInfo::pViewFormats when that list is not empty.
struct TextureStorage {
  // One host image per decode the permitted views need. planes[0] is bound to
  // the guest's VkImage; later planes are private companion images kept in the
  // guest image's layouts, or GENERAL for sparse companions.
  struct Plane {
    VkFormat format{};                   // host image format
    std::vector<VkFormat> view_formats;  // host view formats; non-empty = host image is mutable
    std::vector<VkFormat> guest_views;   // compressed view formats this plane serves
    VkFormat decode{};                   // compressed format whose decode rules fill it
    std::uint32_t texel_bytes{};
  };
  std::vector<Plane> planes;
  explicit operator bool() const { return !planes.empty(); }
};
// Empty when the format is not decoded, or a view format is outside the class.
TextureStorage texture_storage(VkFormat format, bool mutable_format, std::span<const VkFormat> view_formats);
struct TextureView {
  std::size_t plane{};
  VkFormat format{};
};
// Host view of the storage that reads as the guest view format would, or
// nullopt when the image does not permit a view of that format.
std::optional<TextureView> texture_view(const TextureStorage& storage, VkFormat format,
                                        VkFormat decode_mode = VK_FORMAT_UNDEFINED);
struct SparseImageBindings {
  VkDeviceSize bytes{};
  std::vector<VkSparseImageMemoryBind> tiles;
  std::vector<VkSparseMemoryBind> tails;
};
// Physical pages for every color subresource, including per-layer or shared
// mip tails. Metadata aspects require eager backing and do not use this path.
SparseImageBindings sparse_image_bindings(const VkImageCreateInfo& image,
    const VkMemoryRequirements& memory, const VkSparseImageMemoryRequirements& sparse);

// Standalone compute decoder, also used by the image-copy translator. Inputs
// must be storage buffers. The caller owns resource lifetime and inserts memory
// barriers. This records no host reads and works with GPU-produced source data.
struct DecodeRegion {
  TextureFormat format;
  std::uint32_t width{}, height{}, row_blocks{}, source_word{};
};
struct DecodeResources;
std::shared_ptr<DecodeResources> record_decode(VkDevice device, VkCommandBuffer command, VkBuffer source,
                                               VkDeviceSize source_size, VkBuffer destination,
                                               VkDeviceSize destination_size, const DecodeRegion& region);
// Direct ASTC image decode. A zero activation word performs no image accesses;
// this keeps recorded uploads valid when a sparse companion is backed later.
// The destination is a GENERAL storage view of one mip (2D array or 3D).
std::shared_ptr<DecodeResources> record_decode_image(VkDevice device, VkCommandBuffer command,
    VkBuffer source, VkDeviceSize source_size, VkImageView destination, VkBuffer activation,
    const DecodeRegion& region, bool volume, std::uint32_t slice);
}  // namespace venture::vulkan
