#include "common/environment.h"
#include "xr/dev_dump.h"
#include "common/log.h"
#include "core/wait.h"
#include "gfx/gpu_device.h"
#include "hle/openxr/state.h"
#include "hle/vrapi/layers.h"
#include "hle/vrapi/state.h"
#include "hle/vrapi/vulkan_images.h"
#include "xr/queue.h"
#include "hle/vulkan/timing.h"
#include "core/wait.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#if defined(VENTURE_XR_HAS_ANGLE)
#include "gfx/angle.h"
#include "hle/gles/gles_state.h"
#endif

namespace venture::hle::vrapi {
XrResult destroy_host_chains() {
  std::unique_lock queue(gfx::gpu_device().queue_lock, std::defer_lock);
  if (state().init.graphics_api != vulkan_api) queue.lock();
  auto guest_queue = vulkan_submission_lock();
  for (auto &[token, ring] : state().rings)
    for (auto &h : ring->host) {
      std::vector<VkImage> images;
      for (auto image : h.images)
        images.push_back(image.image);
      auto released = xr_images()->release_images(images);
      if (XR_FAILED(released))
        return released;
      if (h.handle) {
        HostQueueLock host_queue(session().uses_shared_gpu);
        if (!host_queue)
          return XR_ERROR_RUNTIME_FAILURE;
        auto destroyed = venture::xr::dev_destroy_swapchain(h.handle);
        if (XR_FAILED(destroyed))
          return destroyed;
      }
      h.handle = {};
      h.images.clear();
      h.sampler_current = false;
    }
  return XR_SUCCESS;
}
namespace {
using FenceMap = std::map<std::pair<TextureSwapChain *, int>, std::vector<std::uint64_t>>;
struct FrameCleanup {
  ~FrameCleanup() {
    auto &s = session();
    if (s.phase != Session::Phase::begun)
      return;
    // A rejected VrApi layer must not strand an xrBeginFrame on its producer
    // thread. End without layers, retaining the submission's original error.
    XrFrameEndInfo discard{XR_TYPE_FRAME_END_INFO};
    discard.displayTime = s.frame.predictedDisplayTime;
    discard.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    auto r = s.end(&discard);
    if (XR_SUCCEEDED(r)) {
      complete_frame();
    } else
      failure("SubmitFrame2 discard", r);
  }
};
bool valid_sampler(const TextureSamplerState &s) {
  return s.min_filter >= TextureFilter::nearest && s.min_filter <= TextureFilter::cubic_mipmap_linear &&
      (s.mag_filter == TextureFilter::nearest || s.mag_filter == TextureFilter::linear ||
       s.mag_filter == TextureFilter::cubic) &&
      s.wrap_s >= TextureWrap::repeat && s.wrap_s <= TextureWrap::clamp_border &&
      s.wrap_t >= TextureWrap::repeat && s.wrap_t <= TextureWrap::clamp_border &&
      std::isfinite(s.max_anisotropy) && s.max_anisotropy >= 1 &&
      std::all_of(std::begin(s.border_color), std::end(s.border_color),
                  [](float f) { return std::isfinite(f); });
}
XrResult apply_gles_sampler(const Ring &ring, const TextureSamplerState &s) {
#if defined(VENTURE_XR_HAS_ANGLE)
  // VrApi.h also updates the application's GLES textures. The Vulkan-only
  // image provider has no application GL sampler state.
  if (!xr_images()->application_uses_gles()) return XR_SUCCESS;
  if (!gfx::current_gl_state()) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  gfx::preserve_gl_errors();
  auto get = gfx::angle_function<PFNGLGETINTEGERVPROC>("glGetIntegerv");
  auto bind = gfx::angle_function<PFNGLBINDTEXTUREPROC>("glBindTexture");
  auto param = gfx::angle_function<PFNGLTEXPARAMETERIPROC>("glTexParameteri");
  const auto *text = gfx::angle_function<PFNGLGETSTRINGPROC>("glGetString")(GL_EXTENSIONS);
  const std::string_view extensions = text ? reinterpret_cast<const char *>(text) : "";
  const bool border = gfx::has_extension(extensions, "GL_EXT_texture_border_clamp") ||
                      gfx::has_extension(extensions, "GL_OES_texture_border_clamp");
  if (!border && (s.wrap_s == TextureWrap::clamp_border || s.wrap_t == TextureWrap::clamp_border))
    return XR_ERROR_FEATURE_UNSUPPORTED;
  constexpr GLint filters[]{GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_LINEAR,
      GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_LINEAR,
      GL_LINEAR, GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_LINEAR};
  if (s.min_filter >= TextureFilter::cubic || s.mag_filter == TextureFilter::cubic) {
    static std::once_flag warning;
    std::call_once(warning, [] {
      log::warn("vrapi", "Cubic sampler hint retained; ANGLE application textures use linear filtering");
    });
  }
  constexpr GLint wraps[]{GL_REPEAT, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_BORDER_EXT};
  const GLenum target = ring.desc.layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
  GLint previous = 0;
  get(target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D : GL_TEXTURE_BINDING_2D_ARRAY, &previous);
  const bool anisotropy = gfx::has_extension(extensions, "GL_EXT_texture_filter_anisotropic");
  GLfloat max_anisotropy = 1;
  if (anisotropy)
    gfx::angle_function<PFNGLGETFLOATVPROC>("glGetFloatv")(
        GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max_anisotropy);
  for (auto texture : ring.textures) {
    bind(target, texture);
    param(target, GL_TEXTURE_MIN_FILTER, filters[int(s.min_filter)]);
    param(target, GL_TEXTURE_MAG_FILTER, filters[int(s.mag_filter)]);
    param(target, GL_TEXTURE_WRAP_S, wraps[int(s.wrap_s)]);
    param(target, GL_TEXTURE_WRAP_T, wraps[int(s.wrap_t)]);
    if (border)
      gfx::angle_function<PFNGLTEXPARAMETERFVPROC>("glTexParameterfv")(
          target, GL_TEXTURE_BORDER_COLOR_EXT, s.border_color);
    if (anisotropy)
      gfx::angle_function<PFNGLTEXPARAMETERFPROC>("glTexParameterf")(
          target, GL_TEXTURE_MAX_ANISOTROPY_EXT, std::min(s.max_anisotropy, max_anisotropy));
  }
  bind(target, GLuint(previous));
  if (!gfx::capture_gl_errors()) return XR_ERROR_RUNTIME_FAILURE;
#endif
  return XR_SUCCESS;
}
XrResult apply_host_sampler(HostChain &h, const TextureSamplerState &s) {
  if (h.sampler_current)
    return XR_SUCCESS;
  // Share GLES enum conversion with guest OpenXR. VrApi additionally has
  // cubic filters, which must remain VK_EXT_filter_cubic on the host.
  constexpr std::uint32_t filters[]{0x2600, 0x2601, 0x2702, 0x2701, 0x2703,
                                   0x2601, 0x2701, 0x2703};
  constexpr std::uint32_t wraps[]{0x2901, 0x812f, 0x812d};
  SamplerOpenGLES gl;
  gl.minFilter = filters[int(s.min_filter)];
  gl.magFilter = filters[int(s.mag_filter)];
  gl.wrapModeS = wraps[int(s.wrap_s)];
  gl.wrapModeT = wraps[int(s.wrap_t)];
  gl.maxAnisotropy = s.max_anisotropy;
  gl.borderColor = {s.border_color[0], s.border_color[1], s.border_color[2], s.border_color[3]};
  XrSwapchainStateSamplerVulkanFB sampler{};
  if (!gles_sampler(gl, sampler)) return XR_ERROR_VALIDATION_FAILURE;
  // VrApi has no swizzle fields: Vulkan identity preserves the existing ABI.
  sampler.swizzleRed = sampler.swizzleGreen = sampler.swizzleBlue = sampler.swizzleAlpha = VK_COMPONENT_SWIZZLE_IDENTITY;
  if (s.min_filter >= TextureFilter::cubic) sampler.minFilter = VK_FILTER_CUBIC_EXT;
  if (s.mag_filter == TextureFilter::cubic) sampler.magFilter = VK_FILTER_CUBIC_EXT;
  auto r = update_host_sampler(h.handle, sampler);
  if (XR_SUCCEEDED(r)) h.sampler_current = true;
  return r;
}
XrResult ensure_host_chain(Ring &ring, std::size_t chain) {
  HostQueueLock host_queue(session().uses_shared_gpu);
  if (!host_queue)
    return XR_ERROR_RUNTIME_FAILURE;
  auto &h = ring.host[chain];
  if (h.handle)
    return apply_host_sampler(h, ring.sampler);
  XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  ci.width = ring.desc.width;
  ci.height = ring.desc.height;
  ci.sampleCount = 1;
  ci.arraySize = ring.desc.layers;
  ci.faceCount = ring.desc.face_count;
  // Guest rings remain fully mipmapped. The host compositor owns filtering;
  // use a single-level presentation image instead of depending on host support
  // for mipmapped swapchains (some hosts accept the count but allocate one).
  ci.mipCount = 1;
  // VrApi has no format negotiation; an unavailable ring format gets a converting copy.
  ci.format = ring.desc.vk_format;
  auto r = venture::xr::create_host_swapchain(session().handle, ci, &h.handle);
  if (XR_FAILED(r))
    return r;
  h.sampler_current = false;
  h.format = static_cast<VkFormat>(ci.format);
  std::uint32_t count = 0;
  r = ::xrEnumerateSwapchainImages(h.handle, 0, &count, nullptr);
  if (XR_SUCCEEDED(r)) {
    h.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    r = ::xrEnumerateSwapchainImages(
        h.handle, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader *>(h.images.data()));
  }
  if (XR_SUCCEEDED(r)) r = apply_host_sampler(h, ring.sampler);
  if (XR_FAILED(r)) {
    venture::xr::dev_destroy_swapchain(h.handle);
    h.handle = {};
    h.images.clear();
  }
  return r;
}
struct Submission {
  bool batched = xr_images()->begin_handoff_batch();
  struct Release { XrSwapchain chain; VkImage image; };
  std::vector<Release> releases;
  XrResult finish() {
    if (!batched) return XR_SUCCESS;
    batched = false;
    auto result = xr_images()->end_handoff_batch();
    HostQueueLock queue(session().uses_shared_gpu);
    if (!queue) return XR_ERROR_RUNTIME_FAILURE;
    XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    for (auto& release : releases) {
      auto next = venture::xr::dev_release_swapchain(release.chain, &info, release.image);
      if (XR_SUCCEEDED(result) && XR_FAILED(next)) result = next;
    }
    releases.clear();
    return result;
  }
  ~Submission() { if (batched) finish(); }
  std::map<std::tuple<TextureSwapChain *, int, bool>, XrSwapchain> copied;
  const FenceMap *fences = nullptr;
  XrResult image(TextureSwapChain *token, int slot, Rect rect, int eye, bool bottom_up,
                 std::uint64_t fence, XrSwapchainSubImage &out) {
    // VrApi 1/2 are default/loader sentinel values, never heap objects. These
    // require authored content; don't reinterpret them as ring pointers.
    if (reinterpret_cast<std::uintptr_t>(token) <= 2) {
      static std::once_flag reported;
      std::call_once(reported, [&] {
        log::warn("vrapi", "Built-in texture swapchain unavailable: kind={}", reinterpret_cast<std::uintptr_t>(token));
      });
      return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    auto it = state().rings.find(token);
    if (it == state().rings.end())
      return XR_ERROR_HANDLE_INVALID;
    auto &ring = *it->second;
    if (slot < 0 || std::size_t(slot) >= ring.textures.size())
      return XR_ERROR_INDEX_OUT_OF_RANGE;
    if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) ||
        !std::isfinite(rect.height) || rect.x < 0 || rect.y < 0 || rect.width <= 0 ||
        rect.height <= 0 || rect.x + rect.width > 1.00001f || rect.y + rect.height > 1.00001f)
      return XR_ERROR_SWAPCHAIN_RECT_INVALID;
    // A slot shown with both row orders needs one host chain for each.
    const auto chain = Ring::host_chain(slot, bottom_up);
    auto r = ensure_host_chain(ring, chain);
    if (XR_FAILED(r))
      return r;
    auto &h = ring.host[chain];
    const auto k = std::make_tuple(token, slot, bottom_up);
    const auto fence_key = std::make_pair(token, slot);
    if (!copied.contains(k)) {
      std::uint32_t acquired = 0;
      XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
      {
        HostQueueLock host_queue(session().uses_shared_gpu);
        if (!host_queue)
          return XR_ERROR_RUNTIME_FAILURE;
        r = ::xrAcquireSwapchainImage(h.handle, &ai, &acquired);
      }
      if (XR_FAILED(r))
        return r;
      XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
      const auto wait_start = vulkan::timing_enabled() ? monotonic_ns() : 0;
      r = ::xrWaitSwapchainImage(h.handle, &wi);
      if (wait_start) vulkan::timing_host_wait("xr_image", monotonic_ns() - wait_start);
      if (XR_FAILED(r))
        return r;
      if (acquired >= h.images.size())
        return XR_ERROR_RUNTIME_FAILURE;
      auto copy_desc = ring.desc;
      copy_desc.host_mip_count = 1;
      if (h.format != ring.desc.vk_format) copy_desc.host_format = h.format;
      if (fences && fences->contains(fence_key)) {
        // Consume all producers, then transfer this slot only once.
        for (auto completion : fences->at(fence_key)) {
          r = xr_images()->wait_producer_sync(completion);
          if (XR_FAILED(r)) break;
        }
        if (XR_SUCCEEDED(r))
          r = xr_images()->copy_to_image(ring.textures[slot], h.images[acquired].image,
                                         copy_desc, 0, bottom_up);
      } else
        r = xr_images()->copy_to_image(ring.textures[slot], h.images[acquired].image, copy_desc,
                                       fence, bottom_up);
      XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      XrResult released;
      if (batched) {
        releases.push_back({h.handle, h.images[acquired].image});
        released = XR_SUCCESS;
      } else {
        HostQueueLock host_queue(session().uses_shared_gpu);
        if (!host_queue) return XR_ERROR_RUNTIME_FAILURE;
        released = venture::xr::dev_release_swapchain(h.handle, &ri, h.images[acquired].image);
      }
      if (XR_FAILED(r))
        return r;
      if (XR_FAILED(released))
        return released;
      copied[k] = h.handle;
    }
    out.swapchain = h.handle;
    out.imageArrayIndex = ring.desc.layers > 1 ? eye : 0;
    // VrApi texture rects count rows from memory row 0; reversed rows move it.
    const float top = bottom_up ? 1 - rect.y - rect.height : rect.y;
    // Round both edges, so the span stays inside the image and keeps a pixel.
    auto span = [](float begin, float size, std::uint32_t pixels) {
      const long first = std::clamp(std::lround(begin * float(pixels)), 0L, long(pixels) - 1);
      const long last = std::clamp(std::lround((begin + size) * float(pixels)), first + 1, long(pixels));
      return std::pair{int(first), int(last - first)};
    };
    const auto [x, width] = span(rect.x, rect.width, ring.desc.width);
    const auto [y, height] = span(top, rect.height, ring.desc.height);
    out.imageRect = {{x, y}, {width, height}};
    return XR_SUCCESS;
  }
};
} // namespace
namespace guest {
TextureSwapChain *vrapi_CreateTextureSwapChain4(const SwapChainCreate *ci) {
  std::lock_guard lock(state().mutex);
  if (!state().initialized || !ci || !xr_images()) {
    failure("CreateTextureSwapChain4", not_initialized);
    return nullptr;
  }
  if (ci->width <= 0 || ci->height <= 0 || ci->levels <= 0 || ci->count <= 0 || ci->count > 16 ||
      ci->array_size < 1 || ci->array_size > 2 || (ci->faces != 1 && ci->faces != 6) ||
      ci->create_flags || ci->usage_flags != 1) {
    failure("CreateTextureSwapChain4", invalid);
    return nullptr;
  }
  const bool vulkan = state().init.graphics_api == vulkan_api;
  auto format = vulkan ? (ci->format > 0 && ci->format <= std::numeric_limits<std::int32_t>::max()
        ? static_cast<VkFormat>(ci->format) : VK_FORMAT_UNDEFINED) : hle::openxr::gl_to_vk(ci->format);
  if (format == VK_FORMAT_UNDEFINED) {
    failure("CreateTextureSwapChain4 format", unsupported);
    return nullptr;
  }
  auto ring = std::make_unique<Ring>();
  ring->desc = {ci->format,
                format,
                std::uint32_t(ci->width),
                std::uint32_t(ci->height),
                std::uint32_t(ci->array_size),
                1,
                std::uint32_t(ci->levels),
                std::uint32_t(ci->faces)};
  ring->textures.resize(ci->count);
  ring->owned.assign(ci->count, true);
  ring->host.resize(2 * std::size_t(ci->count));
  std::unique_lock queue(gfx::gpu_device().queue_lock, std::defer_lock);
  if (state().init.graphics_api != vulkan_api) queue.lock();
  auto r = xr_images()->create_ring(ring->desc, ring->textures);
  if (XR_SUCCEEDED(r)) r = apply_gles_sampler(*ring, ring->sampler);
  if (XR_FAILED(r)) {
    xr_images()->destroy_ring(ring->textures);
    log::warn("vrapi", "CreateTextureSwapChain4 images failed: {} (format={:#x}, {}x{}, layers={}, levels={}, faces={}, count={})",
              int(r), ci->format, ci->width, ci->height, ci->array_size, ci->levels, ci->faces, ci->count);
    return nullptr;
  }
  auto token = reinterpret_cast<TextureSwapChain *>(++state().generation);
  state().rings[token] = std::move(ring);
  return token;
}
TextureSwapChain *vrapi_CreateTextureSwapChain3(std::int32_t type, std::int64_t format,
                                                std::int32_t width, std::int32_t height,
                                                std::int32_t levels, std::int32_t count) {
  if (type != 0 && type != 2 && type != 3) {
    failure("CreateTextureSwapChain3 type", unsupported);
    return nullptr;
  }
  SwapChainCreate ci{format, width, height, levels, type == 3 ? 6 : 1, type == 2 ? 2 : 1,
                     count,  0,     1};
  return vrapi_CreateTextureSwapChain4(&ci);
}
TextureSwapChain *vrapi_CreateTextureSwapChain(std::int32_t type, std::int32_t format,
                                               std::int32_t width, std::int32_t height,
                                               std::int32_t levels, bool buffered) {
  constexpr std::int64_t formats[]{0,      0x8d62, 0x8057, 0x8056, 0x8058, 0x8c43,
                                   0x881a, 0x81a5, 0x81a6, 0x88f0, 0x822c};
  if (format < 0 || std::size_t(format) >= std::size(formats))
    return nullptr;
  return vrapi_CreateTextureSwapChain3(type, formats[format], width, height, levels,
                                       buffered ? 3 : 1);
}
void vrapi_DestroyTextureSwapChain(TextureSwapChain *token) {
  std::lock_guard lock(state().mutex);
  auto it = state().rings.find(token);
  if (it == state().rings.end()) {
    failure("DestroyTextureSwapChain", invalid);
    return;
  }
  std::unique_lock queue(gfx::gpu_device().queue_lock, std::defer_lock);
  if (state().init.graphics_api != vulkan_api) queue.lock();
  auto guest_queue = vulkan_submission_lock();
  for (auto &h : it->second->host)
    if (h.handle) {
      std::vector<VkImage> images;
      for (auto image : h.images)
        images.push_back(image.image);
      auto released = xr_images()->release_images(images);
      if (XR_FAILED(released)) {
        failure("DestroyTextureSwapChain bridge", released);
        return;
      }
      HostQueueLock host_queue(session().uses_shared_gpu);
      if (!host_queue) {
        failure("DestroyTextureSwapChain queue", XR_ERROR_RUNTIME_FAILURE);
        return;
      }
      auto destroyed = venture::xr::dev_destroy_swapchain(h.handle);
      if (XR_FAILED(destroyed)) {
        failure("DestroyTextureSwapChain host", destroyed);
        return;
      }
      h.handle = {};
      h.images.clear();
    }
  std::vector<std::uint32_t> owned;
  for (std::size_t i = 0; i < it->second->textures.size(); ++i)
    if (it->second->owned[i])
      owned.push_back(it->second->textures[i]);
  xr_images()->destroy_ring(owned);
  state().rings.erase(it);
}
std::int32_t vrapi_GetTextureSwapChainLength(TextureSwapChain *token) {
  std::lock_guard lock(state().mutex);
  auto it = state().rings.find(token);
  if (it == state().rings.end()) {
    failure("GetTextureSwapChainLength", invalid);
    return 0;
  }
  return std::int32_t(it->second->textures.size());
}
std::uint32_t vrapi_GetTextureSwapChainHandle(TextureSwapChain *token, std::int32_t index) {
  std::lock_guard lock(state().mutex);
  if (state().init.graphics_api == vulkan_api) {
    failure("GetTextureSwapChainHandle GLES handle", unsupported);
    return 0;
  }
  auto it = state().rings.find(token);
  if (it == state().rings.end() || index < 0 || std::size_t(index) >= it->second->textures.size()) {
    failure("GetTextureSwapChainHandle", invalid);
    return 0;
  }
  return it->second->textures[index];
}
void vrapi_SetTextureSwapChainHandle(TextureSwapChain *token, std::int32_t index,
                                     std::uint32_t handle) {
  std::lock_guard lock(state().mutex);
  if (state().init.graphics_api == vulkan_api) {
    failure("SetTextureSwapChainHandle GLES handle", unsupported);
    return;
  }
  auto it = state().rings.find(token);
  if (it == state().rings.end() || index < 0 || std::size_t(index) >= it->second->textures.size() ||
      !xr_images()->owns_texture(handle, it->second->desc)) {
    failure("SetTextureSwapChainHandle", invalid);
    return;
  }
  auto &ring = *it->second;
  if (ring.textures[index] == handle)
    return;
  std::unique_lock queue(gfx::gpu_device().queue_lock, std::defer_lock);
  if (state().init.graphics_api != vulkan_api) queue.lock();
  if (ring.owned[index])
    xr_images()->destroy_ring(std::span(&ring.textures[index], 1));
  ring.textures[index] = handle;
  ring.owned[index] = false;
}
std::int32_t vrapi_GetTextureSwapChainSamplerState(TextureSwapChain *token,
                                                 TextureSamplerState *out) {
  std::lock_guard lock(state().mutex);
  auto it = state().rings.find(token);
  if (it == state().rings.end() || !out) {
    failure("GetTextureSwapChainSamplerState", invalid);
    return invalid;
  }
  *out = it->second->sampler;
  return success;
}
std::int32_t vrapi_SetTextureSwapChainSamplerState(TextureSwapChain *token,
                                                 const TextureSamplerState *sampler) {
  std::lock_guard lock(state().mutex);
  std::lock_guard sl(session().mutex);
  auto it = state().rings.find(token);
  if (it == state().rings.end() || !sampler || !valid_sampler(*sampler)) {
    failure("SetTextureSwapChainSamplerState", invalid);
    return invalid;
  }
  auto &ring = *it->second;
  const auto previous = ring.sampler;
  std::unique_lock queue(gfx::gpu_device().queue_lock, std::defer_lock);
  if (state().init.graphics_api != vulkan_api) queue.lock();
  auto applied = apply_gles_sampler(ring, *sampler);
  if (XR_FAILED(applied))
    return result(applied, "SetTextureSwapChainSamplerState GLES");
  ring.sampler = *sampler;
  for (auto &h : ring.host) h.sampler_current = false;
  {
    HostQueueLock host_queue(session().uses_shared_gpu);
    applied = host_queue ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
    for (auto &h : ring.host) {
      if (XR_FAILED(applied)) break;
      if (!h.handle) continue; // Applied when this slot first obtains a host chain.
      applied = apply_host_sampler(h, ring.sampler);
    }
  }
  if (XR_FAILED(applied)) {
    ring.sampler = previous;
    for (auto &restore : ring.host) restore.sampler_current = false;
    auto restored = apply_gles_sampler(ring, previous);
    if (XR_FAILED(restored)) failure("SetTextureSwapChainSamplerState restore", restored);
    return result(applied, "SetTextureSwapChainSamplerState host");
  }
  return success;
}
static std::int32_t submit_frame(Mobile *mobile, const Submit *frame, const FenceMap *fences) {
  if (!mobile || !frame) return invalid;
  auto r = ensure_frame(frame->frame_index, mobile);
  if (XR_FAILED(r)) return result(r, "SubmitFrame2 wait");
  std::unique_lock lock(state().mutex);
  std::unique_lock sl(session().mutex);
  if (!valid(mobile))
    return invalid;
  if (state().final_frame)
    return invalid_operation;
  if (!state().token || state().frame_index != frame->frame_index)
    return invalid_operation;
  if (session().phase == Session::Phase::waited) {
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    r = session().begin(&bi);
    if (XR_FAILED(r))
      return result(r, "SubmitFrame2 begin");
    wake_frame_waiters();
  }
  FrameCleanup cleanup;
  if ((frame->layer_count && !frame->layers) || frame->layer_count > 16 ||
      frame->swap_interval < 1 || frame->swap_interval > 4 ||
      !std::isfinite(frame->display_time) || frame->display_time < 0)
    return invalid;
  // FLUSH is satisfied by synchronous image copying/submission. FINAL closes
  // submission until Leave/Enter; no process-wide shutdown is implied.
  if (frame->flags & ~(2u | 4u | 64u))
    return unsupported;
  // The host runtime sets the frame rate, as it does for any PC OpenXR
  // application: a larger VrApi swap interval (a Quest half-rate budget) still
  // renders every host frame instead of repeating content.
  if (session().logged_swap_interval != frame->swap_interval) {
    session().logged_swap_interval = frame->swap_interval;
    log::info("vrapi", "Swap interval={} requested; guest cadence follows the host display={} Hz",
        frame->swap_interval, session().display_timing.rate());
  }
  std::unique_lock queue(gfx::gpu_device().queue_lock, std::defer_lock);
  // ANGLE image operations lock the queue locally, after destination waits.
  if (auto prepared = prepare_vulkan_submission(); XR_FAILED(prepared)) return result(prepared, "SubmitFrame queue");
  auto guest_queue = vulkan_submission_lock();
  Submission submission;
  submission.fences = fences;
  std::vector<LayerStorage> storage(frame->layer_count);
  std::vector<const XrCompositionLayerBaseHeader *> layers;
  XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO, nullptr,
                          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                          session().frame.predictedDisplayTime, state().view};
  XrViewState vs{XR_TYPE_VIEW_STATE};
  XrView eye_offsets[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  std::uint32_t count = 0;
  r = ::xrLocateViews(session().handle, &locate, &vs, 2, &count, eye_offsets);
  if (XR_FAILED(r) || count != 2)
    return result(XR_FAILED(r) ? r : XR_ERROR_RUNTIME_FAILURE, "SubmitFrame2 eyes");
  for (std::uint32_t i = 0; i < frame->layer_count && session().frame.shouldRender; ++i) {
    const auto *header = frame->layers[i];
    if (!header)
      return invalid;
    auto copy = [&](TextureSwapChain *token, int slot, Rect rect, int eye, bool bottom_up,
                    XrSwapchainSubImage &out) {
      return submission.image(token, slot, rect, eye, bottom_up, frame->completion_fence, out);
    };
    r = translate_layer(*header, eye_offsets, copy, storage[i], layers);
    if (XR_FAILED(r)) {
      if (venture::environment::getenv("VENTURE_TRACE_VULKAN")) {
        static std::once_flag reported;
        std::call_once(reported, [&] {
          log::warn("vrapi", "Rejected layer type={} flags={:#x} blend={}/{} completion_fence_present={} result={}",
                    header->type, header->flags, header->src_blend, header->dst_blend,
                    frame->completion_fence != 0, int(r));
          if (header->type == 1) {
            const auto &projection = reinterpret_cast<const Projection &>(*header);
            for (int eye = 0; eye < 2; ++eye) {
              const auto &texture = projection.textures[eye];
              const auto &m = texture.tan_angles.m;
              const auto ring = state().rings.find(texture.chain);
              if (ring == state().rings.end()) continue;
              const auto &d = ring->second->desc;
              log::warn("vrapi", "Rejected projection eye={} {}x{} levels={} matrix=[{},{},{},{};{},{},{},{};{},{},{},{}]",
                  eye, d.width, d.height, d.mip_count,
                  m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3],
                  m[2][0], m[2][1], m[2][2], m[2][3]);
            }
          }
        });
      }
      return result(r, "SubmitFrame2 layer");
    }
  }
  r = submission.finish();
  if (XR_FAILED(r)) return result(r, "SubmitFrame2 handoff batch");
  XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
  // Legacy VrApi predicts before rendering; the native wait occurs here in
  // submission and may already target a later display after a load or stall.
  // Submit this native frame at its waited target, not an older Quest target
  // (or a queued future prediction). Preserve the guest's poses and storage.
  end.displayTime = session().frame.predictedDisplayTime;
  end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  end.layerCount = std::uint32_t(layers.size());
  end.layers = layers.data();
  r = session().end(&end);
  if (XR_SUCCEEDED(r)) {
    state().final_frame = (frame->flags & 4) != 0;
    auto now = monotonic_ns();
    state().submissions.push_back(now);
    while (state().submissions.size() > 1 && state().submissions.front() < now - 1'000'000'000)
      state().submissions.pop_front();
  }
  if (session().phase != Session::Phase::begun) complete_frame();
  return result(r, "SubmitFrame2 end");
}
std::int32_t vrapi_SubmitFrame2(Mobile *mobile, const Submit *frame) {
  return submit_frame(mobile, frame, nullptr);
}
void vrapi_SubmitFrame(Mobile *mobile, const LegacyFrame *legacy) {
  if (!legacy || legacy->type != 3 || legacy->layer_count < 0 || legacy->layer_count > 4) {
    failure("SubmitFrame", invalid);
    return;
  }
  Projection storage[4]{};
  const LayerHeader *layers[4]{};
  FenceMap fences;
  for (int i = 0; i < legacy->layer_count; ++i) {
    auto &p = storage[i];
    auto &l = legacy->layers[i];
    p.header = {1,
                std::uint32_t(l.flags),
                {l.color_scale, l.color_scale, l.color_scale, l.color_scale},
                l.src_blend,
                l.dst_blend,
                0};
    p.head = l.textures[0].head;
    if (std::memcmp(&l.textures[0].head.pose, &l.textures[1].head.pose, sizeof(Pose)) != 0) {
      failure("SubmitFrame per-eye render pose", unsupported);
      return;
    }
    for (int eye = 0; eye < 2; ++eye) {
      auto &t = l.textures[eye];
      if (t.depth) {
        failure("SubmitFrame depth", unsupported);
        return;
      }
      p.textures[eye] = {t.color, t.index, t.tan_angles, t.rect};
      if (t.completion_fence)
        fences[{t.color, t.index}].push_back(t.completion_fence);
    }
    layers[i] = &p.header;
  }
  Submit frame{std::uint32_t(legacy->flags),
               std::uint32_t(legacy->swap_interval),
               std::uint64_t(legacy->frame_index),
               0,
               0,
               std::uint32_t(legacy->layer_count),
               layers};
  auto r = submit_frame(mobile, &frame, &fences);
  if (r < 0)
    failure("SubmitFrame", r);
}
} // namespace guest
} // namespace venture::hle::vrapi
