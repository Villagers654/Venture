// SPDX-License-Identifier: GPL-3.0-or-later
// VrApi texture swapchains inside a game on an Android headset: each is one
// OpenXR swapchain of the device's runtime, and its slots are the runtime's
// own images (GLES textures or the game device's VkImages), so the game
// renders straight into what the compositor shows. VrApi lets a game write
// slot N while the compositor shows N-1; OpenXR wants an image acquired before
// it is written. Each swapchain therefore keeps the image the game draws next
// acquired and waited: a submission of that slot releases it and acquires the
// following one. A game that submits an older slot unchanged keeps showing
// it; one that wrote a slot out of order has it copied into the acquired image
// (logged once).
//
// A layer whose rows run opposite to the API's (a flipped texture matrix) is
// shown from a second swapchain the slot is mirrored into, made when first
// needed: OpenXR image rects cannot flip.
//
// Vulkan slots rest in SHADER_READ_ONLY_OPTIMAL between frames, the layout
// VrApi's Vulkan games leave them in; the runtime gets them in
// COLOR_ATTACHMENT_OPTIMAL. The changes are recorded on the game's queue
// (vulkan.h), after its rendering.
#include "android_game/vrapi/platform_xr.h"

#include <GLES3/gl32.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#include "android_game/vrapi/native.h"
#include "android_game/vrapi/vulkan.h"
#include "common/log.h"
#include "hle/vrapi/submit.h"

namespace venture::hle::vrapi {
namespace {
constexpr VkImageLayout kResting = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
constexpr VkImageLayout kRuntime = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

// One runtime swapchain: its images (GL names or VkImages), the one acquired
// for the next write and the one shown (the last released).
struct Chain {
  XrSwapchain handle{};
  std::vector<std::uint64_t> images;
  std::int32_t acquired = -1, released = -1;
};
struct Native {
  bool vulkan = false;
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  Chain slots;    // the game's
  Chain flipped;  // mirrored slots, made when a layer first needs them
};
std::map<TextureSwapChain *, Native> natives;  // state().mutex

std::uint32_t layer_count(const Ring &ring) { return ring.desc.layers * ring.desc.face_count; }

XrResult create_chain(const Native &n, Chain &chain) {
  auto r = ::xrCreateSwapchain(session().handle, &n.info, &chain.handle);
  if (XR_FAILED(r)) return r;
  std::uint32_t count = 0;
  ::xrEnumerateSwapchainImages(chain.handle, 0, &count, nullptr);
  if (n.vulkan) {
    std::vector<XrSwapchainImageVulkanKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    r = ::xrEnumerateSwapchainImages(chain.handle, count, &count,
                                     reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data()));
    for (const auto &image : images) chain.images.push_back(reinterpret_cast<std::uint64_t>(image.image));
  } else {
    std::vector<XrSwapchainImageOpenGLESKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    r = ::xrEnumerateSwapchainImages(chain.handle, count, &count,
                                     reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data()));
    for (const auto &image : images) chain.images.push_back(image.image);
  }
  if (XR_FAILED(r) || chain.images.empty()) {
    ::xrDestroySwapchain(chain.handle);
    chain = {};
    return XR_FAILED(r) ? r : XR_ERROR_RUNTIME_FAILURE;
  }
  return XR_SUCCESS;
}

VkImage vk_image(const Chain &chain, std::int32_t index) {
  return reinterpret_cast<VkImage>(chain.images[std::size_t(index)]);
}

// Acquires and waits the next image. The game's own slots then move to the
// layout it renders from.
XrResult acquire(const Ring &ring, const Native &n, Chain &chain) {
  XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  std::uint32_t index = 0;
  auto r = ::xrAcquireSwapchainImage(chain.handle, &ai, &index);
  if (XR_FAILED(r)) return r;
  XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
  r = ::xrWaitSwapchainImage(chain.handle, &wi);
  if (XR_FAILED(r)) return r;
  chain.acquired = std::int32_t(index);
  if (n.vulkan && &chain == &n.slots) {
    const auto image = vk_image(chain, chain.acquired);
    return vrapi_native::vulkan::submit([&](VkCommandBuffer c) {
      vrapi_native::vulkan::barrier(c, image, layer_count(ring), kRuntime, kResting);
    });
  }
  return XR_SUCCESS;
}
XrResult release(Chain &chain) {
  XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  auto r = ::xrReleaseSwapchainImage(chain.handle, &ri);
  if (XR_FAILED(r)) return r;
  chain.released = chain.acquired;
  chain.acquired = -1;
  return XR_SUCCESS;
}

GLenum gl_target(const Ring &ring) {
  return ring.desc.face_count == 6 ? GL_TEXTURE_CUBE_MAP : ring.desc.layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
}
void gl_attach(GLenum framebuffer, const Ring &ring, GLuint texture, std::uint32_t layer) {
  if (ring.desc.face_count == 6)
    glFramebufferTexture2D(framebuffer, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer, texture, 0);
  else if (ring.desc.layers > 1)
    glFramebufferTextureLayer(framebuffer, GL_COLOR_ATTACHMENT0, texture, 0, GLint(layer));
  else
    glFramebufferTexture2D(framebuffer, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
}
// Mirrors level 0 of every layer vertically, restoring the game's GL state.
void gl_flip(const Ring &ring, GLuint from, GLuint to) {
  GLint read = 0, draw = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST), discard = glIsEnabled(GL_RASTERIZER_DISCARD);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_RASTERIZER_DISCARD);
  GLuint framebuffers[2]{};
  glGenFramebuffers(2, framebuffers);  // framebuffers are not shared between contexts
  glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffers[0]);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[1]);
  const auto w = GLint(ring.desc.width), h = GLint(ring.desc.height);
  for (std::uint32_t layer = 0; layer < layer_count(ring); ++layer) {
    gl_attach(GL_READ_FRAMEBUFFER, ring, from, layer);
    gl_attach(GL_DRAW_FRAMEBUFFER, ring, to, layer);
    glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(read));
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(draw));
  glDeleteFramebuffers(2, framebuffers);
  if (scissor) glEnable(GL_SCISSOR_TEST);
  if (discard) glEnable(GL_RASTERIZER_DISCARD);
}

void report_out_of_order() {
  static std::once_flag reported;
  std::call_once(reported, [] {
    log::warn("vrapi", "A swapchain slot was drawn out of the runtime's order; its frames are copied");
  });
}

// Shows `slot` from this frame on, from the game's chain.
XrResult present(const Ring &ring, Native &n, std::int32_t slot) {
  auto &chain = n.slots;
  if (slot == chain.released && slot != chain.acquired) return XR_SUCCESS;  // unchanged, still shown
  if (chain.acquired < 0) {
    if (auto r = acquire(ring, n, chain); XR_FAILED(r)) return r;
  }
  const auto target = chain.acquired;
  if (slot != target) report_out_of_order();
  if (n.vulkan) {
    const auto layers = layer_count(ring);
    const auto from = vk_image(chain, slot), to = vk_image(chain, target);
    auto r = vrapi_native::vulkan::submit([&](VkCommandBuffer c) {
      using namespace vrapi_native::vulkan;
      if (slot != target) {
        barrier(c, from, layers, kResting, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier(c, to, layers, kResting, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copy(c, from, to, ring.desc.width, ring.desc.height, layers, false);
        barrier(c, from, layers, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kResting);
        barrier(c, to, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kRuntime);
      } else {
        barrier(c, to, layers, kResting, kRuntime);
      }
    });
    if (XR_FAILED(r)) return r;
  } else if (slot != target) {
    glCopyImageSubData(GLuint(chain.images[std::size_t(slot)]), gl_target(ring), 0, 0, 0, 0,
                       GLuint(chain.images[std::size_t(target)]), gl_target(ring), 0, 0, 0, 0,
                       GLsizei(ring.desc.width), GLsizei(ring.desc.height), GLsizei(layer_count(ring)));
  }
  if (auto r = release(chain); XR_FAILED(r)) return r;
  return acquire(ring, n, chain);
}

// Shows `slot` mirrored, from the flipped chain.
XrResult present_flipped(const Ring &ring, Native &n, std::int32_t slot) {
  auto &chain = n.flipped;
  if (!chain.handle) {
    if (auto r = create_chain(n, chain); XR_FAILED(r)) return r;
    log::info("vrapi", "A layer's rows run opposite to the API's; its swapchain is shown mirrored");
  }
  if (chain.acquired < 0) {
    if (auto r = acquire(ring, n, chain); XR_FAILED(r)) return r;
  }
  if (n.vulkan) {
    const auto layers = layer_count(ring);
    const auto from = vk_image(n.slots, slot), to = vk_image(chain, chain.acquired);
    auto r = vrapi_native::vulkan::submit([&](VkCommandBuffer c) {
      using namespace vrapi_native::vulkan;
      barrier(c, from, layers, kResting, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      barrier(c, to, layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      copy(c, from, to, ring.desc.width, ring.desc.height, layers, true);
      barrier(c, from, layers, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kResting);
      barrier(c, to, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kRuntime);
    });
    if (XR_FAILED(r)) return r;
  } else {
    gl_flip(ring, GLuint(n.slots.images[std::size_t(slot)]), GLuint(chain.images[std::size_t(chain.acquired)]));
  }
  if (auto r = release(chain); XR_FAILED(r)) return r;
  return acquire(ring, n, chain);
}

void destroy_chains(Native &n) {
  if (n.vulkan) vrapi_native::vulkan::wait_idle();
  for (auto *chain : {&n.slots, &n.flipped})
    if (chain->handle) ::xrDestroySwapchain(chain->handle), *chain = {};
}

// One submission's images (hle/vrapi/submit.h): each ring shown once per
// frame and orientation.
class NativeImages {
 public:
  NativeImages(const Submit &, const FenceMap *) {}
  XrResult ready() const { return XR_SUCCESS; }
  int begin_layer(const LayerHeader &, const XrView *) { return 0; }
  XrResult image(TextureSwapChain *token, int slot, Rect rect, int eye, bool bottom_up, XrSwapchainSubImage &out) {
    // VrApi 1/2 are default/loader sentinel values, never rings.
    if (reinterpret_cast<std::uintptr_t>(token) <= 2) return XR_ERROR_FEATURE_UNSUPPORTED;
    auto it = state().rings.find(token);
    if (it == state().rings.end()) return XR_ERROR_HANDLE_INVALID;
    auto &ring = *it->second;
    auto &n = natives[token];
    if (slot < 0 || std::size_t(slot) >= n.slots.images.size()) return XR_ERROR_INDEX_OUT_OF_RANGE;
    if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) ||
        !std::isfinite(rect.height) || rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 ||
        rect.x + rect.width > 1.00001f || rect.y + rect.height > 1.00001f)
      return XR_ERROR_SWAPCHAIN_RECT_INVALID;
    // GLES images keep memory row 0 at the bottom and Vulkan images at the
    // top, as each API's VrApi layers do; rows running the other way are
    // shown mirrored, which moves the rect.
    const bool flip = bottom_up == n.vulkan;
    const float y_start = flip ? 1 - rect.y - rect.height : rect.y;
    auto span = [](float begin, float size, std::uint32_t pixels) {
      const long first = std::clamp(std::lround(begin * float(pixels)), 0L, long(pixels) - 1);
      const long last = std::clamp(std::lround((begin + size) * float(pixels)), first + 1, long(pixels));
      return std::pair{int(first), int(last - first)};
    };
    const auto [x, width] = span(rect.x, rect.width, ring.desc.width);
    const auto [y, height] = span(std::max(0.0f, y_start), rect.height, ring.desc.height);
    if (shown_.insert({token, flip}).second) {
      auto r = flip ? present_flipped(ring, n, slot) : present(ring, n, slot);
      if (XR_FAILED(r)) return r;
    }
    out.swapchain = flip ? n.flipped.handle : n.slots.handle;
    out.imageArrayIndex = ring.desc.layers > 1 ? std::uint32_t(eye) : 0;
    out.imageRect = {{x, y}, {width, height}};
    return XR_SUCCESS;
  }
  DrawnLayers *drawn() { return nullptr; }
  XrResult gather(std::vector<const XrCompositionLayerBaseHeader *> &) { return XR_SUCCESS; }
  XrResult finish(std::vector<const XrCompositionLayerBaseHeader *> &) { return XR_SUCCESS; }
  XrResult completion(std::function<void()> &) { return XR_SUCCESS; }
  std::shared_ptr<const void> retained() const { return nullptr; }

 private:
  std::set<std::pair<TextureSwapChain *, bool>> shown_;
};

Native *native(TextureSwapChain *token) {
  auto it = natives.find(token);
  return it == natives.end() ? nullptr : &it->second;
}
}  // namespace

// The runtime's swapchains belong to the kept session; LeaveVrMode keeps them.
XrResult destroy_host_chains() { return XR_SUCCESS; }

namespace guest {
TextureSwapChain *vrapi_CreateTextureSwapChain4(const SwapChainCreate *ci) {
  std::lock_guard lock(state().mutex);
  if (!state().initialized || !ci) {
    failure("CreateTextureSwapChain4", not_initialized);
    return nullptr;
  }
  if (!valid_swapchain_create(*ci)) {
    failure("CreateTextureSwapChain4", invalid);
    return nullptr;
  }
  const auto normalized = normalized_swapchain_create(*ci);
  ci = &normalized;
  const bool vulkan = state().init.graphics_api == vulkan_api;
  if (vulkan && !vrapi_native::vulkan::bound()) {
    failure("CreateTextureSwapChain4 Vulkan device", not_initialized);
    return nullptr;
  }
  if (auto r = vrapi_native::ensure_session(); XR_FAILED(r)) {
    failure("CreateTextureSwapChain4 session", r);
    return nullptr;
  }
  std::lock_guard sl(session().mutex);
  Native n;
  n.vulkan = vulkan;
  // VrApi's GLES format is a GL internal format, its Vulkan one a VkFormat:
  // each is the runtime's format for that API.
  n.info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  n.info.format = ci->format;
  n.info.sampleCount = 1;
  n.info.width = std::uint32_t(ci->width);
  n.info.height = std::uint32_t(ci->height);
  n.info.faceCount = std::uint32_t(ci->faces);
  n.info.arraySize = std::uint32_t(ci->array_size);
  n.info.mipCount = std::uint32_t(ci->levels);
  auto ring = std::make_unique<Ring>();
  ring->desc = {ci->format, vulkan ? VkFormat(ci->format) : VK_FORMAT_UNDEFINED, n.info.width, n.info.height,
                n.info.arraySize, 1, n.info.mipCount, n.info.faceCount};
  if (auto r = create_chain(n, n.slots); XR_FAILED(r)) {
    log::warn("vrapi", "CreateTextureSwapChain4 failed: {} (format={:#x}, {}x{}, layers={}, levels={}, faces={})",
              int(r), ci->format, ci->width, ci->height, ci->array_size, ci->levels, ci->faces);
    return nullptr;
  }
  // The game sees the runtime's image count (GetTextureSwapChainLength).
  for (auto image : n.slots.images) ring->textures.push_back(vulkan ? 0 : std::uint32_t(image));
  ring->owned.assign(ring->textures.size(), false);
  if (auto r = acquire(*ring, n, n.slots); XR_FAILED(r)) failure("CreateTextureSwapChain4 acquire", r);
  auto token = reinterpret_cast<TextureSwapChain *>(++state().generation);
  natives[token] = std::move(n);
  state().rings[token] = std::move(ring);
  return token;
}
void vrapi_DestroyTextureSwapChain(TextureSwapChain *token) {
  std::lock_guard lock(state().mutex);
  auto it = state().rings.find(token);
  if (it == state().rings.end()) {
    failure("DestroyTextureSwapChain", invalid);
    return;
  }
  session().finish_host_end();  // takes the session lock itself
  std::lock_guard sl(session().mutex);
  if (auto *n = native(token)) destroy_chains(*n);
  natives.erase(token);
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
  return it->second->textures[std::size_t(index)];
}
// The slots are the runtime's images; a game's own texture cannot replace one.
void vrapi_SetTextureSwapChainHandle(TextureSwapChain *, std::int32_t, std::uint32_t) {
  failure("SetTextureSwapChainHandle", unsupported);
}
std::int32_t vrapi_GetTextureSwapChainSamplerState(TextureSwapChain *token, TextureSamplerState *out) {
  std::lock_guard lock(state().mutex);
  auto it = state().rings.find(token);
  if (it == state().rings.end() || !out) {
    failure("GetTextureSwapChainSamplerState", invalid);
    return invalid;
  }
  *out = it->second->sampler;
  return success;
}
std::int32_t vrapi_SetTextureSwapChainSamplerState(TextureSwapChain *token, const TextureSamplerState *sampler) {
  std::lock_guard lock(state().mutex);
  auto it = state().rings.find(token);
  if (it == state().rings.end() || !sampler || !valid_sampler(*sampler)) {
    failure("SetTextureSwapChainSamplerState", invalid);
    return invalid;
  }
  // The compositor samples with its own state; the game reads it back.
  it->second->sampler = *sampler;
  return success;
}
std::int32_t vrapi_SubmitFrame2(Mobile *mobile, const Submit *frame) {
  return submit_frame<NativeImages>(mobile, frame, nullptr);
}
void vrapi_SubmitFrame(Mobile *mobile, const LegacyFrame *legacy) { submit_legacy_frame<NativeImages>(mobile, legacy); }

std::int32_t vrapi_CreateSystemVulkan(const SystemVulkan *info) {
  std::lock_guard lock(state().mutex);
  if (!state().initialized || state().init.graphics_api != vulkan_api) return not_initialized;
  if (!info || !info->instance || !info->physical || !info->device) return invalid;
  if (!state().rings.empty() || state().mobile) return invalid_operation;
  return vrapi_native::vulkan::bind_system(*info);
}
void vrapi_DestroySystemVulkan() {
  std::lock_guard lock(state().mutex);
  if (!vrapi_native::vulkan::bound()) return;
  if (state().mobile || !state().rings.empty()) {
    failure("DestroySystemVulkan", invalid_operation);
    return;
  }
  vrapi_native::vulkan::unbind_system();
}
VkImage vrapi_GetTextureSwapChainBufferVulkan(TextureSwapChain *token, std::int32_t slot) {
  std::lock_guard lock(state().mutex);
  auto *n = native(token);
  if (!n || !n->vulkan || slot < 0 || std::size_t(slot) >= n->slots.images.size()) {
    failure("GetTextureSwapChainBufferVulkan", invalid);
    return VK_NULL_HANDLE;
  }
  return vk_image(n->slots, slot);
}
std::int32_t vrapi_GetTextureSwapChainBufferFoveationVulkan(TextureSwapChain *token, std::int32_t slot,
                                                         VkImage *image, std::uint32_t *width,
                                                         std::uint32_t *height) {
  std::lock_guard lock(state().mutex);
  // VrApi_Vulkan.h requires a null image and zero dimensions on failure; full-rate
  // composition has no density image, which is a valid empty result (as on PC).
  if (image) *image = VK_NULL_HANDLE;
  if (width) *width = 0;
  if (height) *height = 0;
  auto *n = native(token);
  if (!image || !width || !height || !n || !n->vulkan || slot < 0 || std::size_t(slot) >= n->slots.images.size())
    return invalid;
  return success;
}
}  // namespace guest
}  // namespace venture::hle::vrapi
