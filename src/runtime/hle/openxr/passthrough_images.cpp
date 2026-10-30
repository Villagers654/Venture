#include "hle/openxr/passthrough.h"
#include "hle/openxr/state.h"
#include "xr/queue.h"
#include "xr/dev_dump.h"
#include "common/log.h"

namespace venture::hle::openxr {
namespace {
struct Eye {
  XrSwapchain handle{};
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  std::vector<XrSwapchainImageVulkanKHR> images;
  std::unique_ptr<ColorImages> color;
  std::int64_t source_format = 0;
  XrResult destroy() {
    color.reset(); // Retire GPU commands before destroying their target images.
    if (!handle) return XR_SUCCESS;
    HostQueueLock queue(session().uses_shared_gpu);
    if (!queue) return XR_ERROR_RUNTIME_FAILURE;
    // A failed conversion may still have an earlier copy in flight.
    const auto& binding = state().graphics;
    auto idle = reinterpret_cast<PFN_vkQueueWaitIdle>(gfx::gpu_device().get_instance_proc_addr(binding.instance, "vkQueueWaitIdle"));
    if (idle(session().queue) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;
    auto result = dev_destroy_swapchain(handle);
    if (XR_SUCCEEDED(result)) { handle = XR_NULL_HANDLE; images.clear(); }
    return result;
  }
  XrResult prepare(XrSession target, const Swapchain& chain) {
    const auto format = VkFormat(chain.info.format);
    // SBN1 profile 1 is SDR RGBA. Never declare float/HDR or alpha-less data.
    if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB &&
        format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_B8G8R8A8_SRGB)
      return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    if (chain.info.sampleCount != 1 || chain.info.faceCount != 1) return XR_ERROR_FEATURE_UNSUPPORTED;
    if (handle && color && info.width == chain.info.width && info.height == chain.info.height && source_format == chain.info.format)
      return XR_SUCCESS;
    session().finish_host_end();
    auto result = destroy();
    if (XR_FAILED(result)) return result;
    info = chain.info;
    source_format = chain.info.format;
    info.next = nullptr; info.createFlags = 0;
    info.arraySize = info.faceCount = info.mipCount = info.sampleCount = 1;
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    {
      HostQueueLock queue(session().uses_shared_gpu);
      if (!queue) return XR_ERROR_RUNTIME_FAILURE;
      result = create_host_swapchain(target, info, &handle);
    }
    if (XR_FAILED(result)) return result;
    std::uint32_t count{};
    result = ::xrEnumerateSwapchainImages(handle, 0, &count, nullptr);
    if (XR_FAILED(result)) return result;
    if (!count || count > 32) return XR_ERROR_LIMIT_REACHED;
    images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    result = ::xrEnumerateSwapchainImages(handle, count, &count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(result)) return result;
    color = std::make_unique<ColorImages>(state().graphics, gfx::gpu_device().get_instance_proc_addr, info, images);
    return result;
  }
};
std::array<Eye, 2> eyes;
bool image_flip(const XrCompositionLayerProjection& projection) {
  for (auto* node = static_cast<const XrBaseInStructure*>(projection.next); node; node = node->next)
    if (node->type == XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB && enabled("XR_FB_composition_layer_image_layout"))
      return reinterpret_cast<const XrCompositionLayerImageLayoutFB*>(node)->flags & XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
  return false;
}
} // namespace
XrResult destroy_passthrough_images() {
  for (auto& eye : eyes) {
    const auto result = eye.destroy();
    if (XR_FAILED(result)) return result;
  }
  return XR_SUCCESS;
}
XrResult prepare_passthrough_images(XrSession target, const XrCompositionLayerProjection& projection,
                                    PassthroughFrame& output, float camera_opacity) {
  if (projection.type != XR_TYPE_COMPOSITION_LAYER_PROJECTION || projection.viewCount != 2 || !projection.views)
    return XR_ERROR_LAYER_INVALID;
  if (!valid_handle(projection.space, "XrSpace")) return XR_ERROR_HANDLE_INVALID;
  // Validate the core header separately: disabled/unknown extension nodes in
  // next must be ignored, just as InputChain does for ordinary VR submissions.
  auto root = projection; root.next = nullptr;
  auto result = validate_chain(&root, false);
  if (XR_FAILED(result)) return result;
  result = validate_chain(projection.next, false, projection.type, true);
  if (XR_FAILED(result)) return result;
  if (projection.layerFlags & ~(XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
      XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT | XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT))
    return XR_ERROR_FEATURE_UNSUPPORTED;
  // Camera-unaware color transforms cannot be forwarded after flattening.
  // The image-layout node is applied by the copy; unknown/disabled nodes follow
  // normal OpenXR chain rules, but enabled unsupported transforms are refused.
  for (auto* node = static_cast<const XrBaseInStructure*>(projection.next); node; node = node->next) {
    for (const auto& metadata : structure_metadata())
      if (metadata.type == node->type && *metadata.extension && enabled(metadata.extension) &&
          node->type != XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB) return XR_ERROR_FEATURE_UNSUPPORTED;
  }
  std::array<Swapchain*, 2> chains{};
  for (unsigned i = 0; i < 2; ++i) {
    const auto& view = projection.views[i];
    if (view.type != XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW) return XR_ERROR_LAYER_INVALID;
    result = validate_chain(view.next, false, view.type, true);
    if (XR_FAILED(result)) return result;
    const auto found = state().swapchains.find(view.subImage.swapchain);
    if (found == state().swapchains.end()) return XR_ERROR_HANDLE_INVALID;
    auto& chain = found->second;
    const auto& rect = view.subImage.imageRect;
    if (view.subImage.imageArrayIndex >= chain.info.arraySize || rect.offset.x < 0 || rect.offset.y < 0 ||
        rect.extent.width <= 0 || rect.extent.height <= 0 ||
        std::int64_t(rect.offset.x) + rect.extent.width > chain.info.width ||
        std::int64_t(rect.offset.y) + rect.extent.height > chain.info.height) return XR_ERROR_SWAPCHAIN_RECT_INVALID;
    if (chain.released_index < 0 || !chain.backing || chain.operation_in_progress) return XR_ERROR_LAYER_INVALID;
    chains[i] = &chain;
  }
  // Both eyes are validated before touching either host image. Each copy reads
  // this submission's released image/slice; no previous successful eye is reused.
  output.projection = projection;
  output.projection.next = nullptr;
  output.projection.views = output.views.data();
  for (unsigned i = 0; i < 2; ++i) {
    auto& chain = *chains[i]; auto& eye = eyes[i];
    result = eye.prepare(target, chain);
    if (XR_FAILED(result)) {
      log::warn("xr-passthrough", "Eye {} unavailable: result={} format={} size={}x{} arrays={} samples={}",
          i, int(result), chain.info.format, chain.info.width, chain.info.height, chain.info.arraySize, chain.info.sampleCount);
      return result;
    }
    std::uint32_t index{};
    {
      HostQueueLock queue(session().uses_shared_gpu);
      if (!queue) return XR_ERROR_RUNTIME_FAILURE;
      result = ::xrAcquireSwapchainImage(eye.handle, nullptr, &index);
    }
    if (XR_FAILED(result)) return result;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    result = ::xrWaitSwapchainImage(eye.handle, &wait);
    if (result != XR_SUCCESS && result != XR_SESSION_LOSS_PENDING) { eye.destroy(); return result; }
    if (index >= eye.images.size()) { eye.destroy(); return XR_ERROR_RUNTIME_FAILURE; }
    const bool flip = image_flip(projection);
    result = chain.backing->copy(std::uint32_t(chain.released_index), eye.images[index].image,
        VkFormat(eye.info.format), {}, flip, std::int32_t(projection.views[i].subImage.imageArrayIndex));
    const auto alpha = !(projection.layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT)
        ? ColorImages::Alpha::opaque
        : (projection.layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT)
            ? ColorImages::Alpha::premultiply : ColorImages::Alpha::preserve;
    const auto matrix = color_matrix(state().color_space, XR_COLOR_SPACE_REC709_FB);
    if (XR_SUCCEEDED(result)) result = eye.color->convert(index, matrix, false, alpha, camera_opacity);
    {
      HostQueueLock queue(session().uses_shared_gpu);
      if (!queue) return XR_ERROR_RUNTIME_FAILURE;
      const auto released = dev_release_swapchain(eye.handle, nullptr, eye.images[index].image);
      if (XR_SUCCEEDED(result)) result = released;
    }
    if (XR_FAILED(result)) return result;
    output.views[i] = projection.views[i];
    // Depth/motion are not part of the ordinary-RGBA profile. The original
    // color's pose/FOV and submitted rectangle remain authoritative.
    output.views[i].next = nullptr;
    output.views[i].subImage.swapchain = eye.handle;
    output.views[i].subImage.imageArrayIndex = 0;
    if (chain.textures.empty() == flip) output.views[i].subImage.imageRect.offset.y = std::int32_t(chain.info.height) -
        output.views[i].subImage.imageRect.offset.y - output.views[i].subImage.imageRect.extent.height;
  }
  return XR_SUCCESS;
}
} // namespace venture::hle::openxr
