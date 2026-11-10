// Render-target bounds. Quest drivers render tile by tile and tolerate
// rendering that strays past a render pass instance's bounds; desktop GPUs
// write such fragments and fault past an attachment's edge (Moss: NVIDIA Xid 13
// "CT Violation", then VK_ERROR_DEVICE_LOST at a later vkQueueSubmit).
//
// - "The application must ensure (using scissor if necessary) that all
//   rendering is contained within the render area" (Vulkan spec, Render Pass
//   Commands). While a render pass instance is open, dynamic scissors reach the
//   host clamped to its render area, which discards the same fragments a tiler
//   would.
// - A framebuffer must not be larger than any of its attachments
//   (VUID-VkFramebufferCreateInfo-flags-04533/04534), and a render area must lie
//   inside the framebuffer and its attachments. UE4's grow-only scene targets
//   break this: after a 1536x1536 scene capture, Moss renders its eyes through
//   a 1536x1536 multisampled scene color whose resolve attachment is the
//   768x768 eye image. Host framebuffers and render areas are clamped to the
//   smallest attachment, which keeps exactly the pixels every attachment has.
// - SwiftShader workaround: attachments may be larger than the framebuffer and
//   a subpass resolve covers only the render area, but SwiftShader resolves the
//   source's whole extent and writes past a smaller resolve attachment (the
//   clamped Moss eye pass faulted the host). On SwiftShader only (driverID), a
//   multisampled resolve source larger than its resolve attachment that no
//   subpass reads as an input attachment is replaced in the host framebuffer by
//   a transient image of the framebuffer's size. The begun render pass decides
//   per instance: if it loads or stores the source, a plain host framebuffer
//   with the guest's views is used instead. Otherwise the stand-in is moved to
//   the pass's initial layout before the instance, and the guest's image to its
//   final layout after it. Separate depth/stencil layouts and attachment
//   feedback loops are not handled on this path.
// - Render passes reach the host with the subpass dependencies their
//   attachment accesses need (subpass_sync.cpp).
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "chain.h"
#include "common/environment.h"
#include "common/log.h"
#include "internal.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace venture::vulkan {
namespace {
struct Scissors {
  std::optional<VkRect2D> area;  // the open render pass instance's render area
  std::vector<VkRect2D> rects;   // the guest's dynamic scissors, unclamped
  bool with_count{};             // last set by vkCmdSetScissorWithCount
  std::vector<VkRect2D> host;    // the scissors last sent to the host
  // Final-layout transitions of guest images replaced in the open instance.
  std::vector<VkImageMemoryBarrier> after;
};
std::mutex mutex;
std::unordered_map<VkCommandBuffer, Scissors> states;
// Graphics pipelines whose scissors are static: binding one invalidates the
// dynamic scissors ("made invalid by another pipeline bind with that state
// specified as static", Pipeline Dynamic State).
// Pipelines whose scissors the guest baked in: the host pipeline takes them
// as dynamic state, so they can be clamped to each render area like dynamic ones.
std::unordered_map<VkPipeline, std::vector<VkRect2D>> static_scissor;
// Images that can be attachments (extent of mip 0), their views (extent of the
// base mip), and host framebuffers created smaller than the guest asked.
struct Image {
  VkExtent2D extent;
  std::uint32_t layers;
  VkSampleCountFlagBits samples;
  VkImageUsageFlags usage;
};
struct View {
  VkExtent2D extent;
  VkImage image;
  VkFormat format;
  VkImageViewType type;
  VkImageSubresourceRange range;
};
std::unordered_map<VkImage, Image> images;
std::unordered_map<VkImageView, View> views;
std::unordered_map<VkFramebuffer, VkExtent2D> clamped;
struct Pass {
  // The attachments its subpasses use as input, color, resolve or
  // depth/stencil attachments. Only those must cover the framebuffer
  // (VUID-VkFramebufferCreateInfo-flags-04533/04534); unused, preserve-only,
  // shading-rate and density-map attachments may be smaller.
  std::vector<bool> used;
  // Attachments whose contents are neither loaded nor stored, and attachments
  // some subpass reads as input attachments.
  std::vector<bool> transient, input;
  std::vector<VkImageLayout> initial_layouts, final_layouts;
  // Attachment formats and sample counts, and the layers a multiview subpass renders (1 without multiview).
  std::vector<VkFormat> formats;
  std::vector<VkSampleCountFlagBits> samples;
  std::uint32_t view_layers = 1;
  // (source, resolve attachment) pairs of color and depth/stencil resolves.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> resolves;
};
std::unordered_map<VkRenderPass, Pass> passes;
// A depth (or depth/stencil) format.
bool is_depth_format(VkFormat format) {
  switch (format) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return true;
    default:
      return false;
  }
}
// VENTURE_VRS: foveated variable rate shading on the guest's stereo scene passes
// (VK_KHR_fragment_shading_rate). Each multiview render pass with a depth
// attachment gets a shading-rate attachment (one layer per view): full rate
// in a VENTURE_VRS_WIDTH x VENTURE_VRS_HEIGHT region around a stereo-aligned
// center, 2x2 (or 4x4) outside. The center is a point straight ahead (VENTURE_VRS_DISTANCE, 2 m),
// projected into each eye with its own field of view, so both eyes keep the
// same part of the scene at full rate. Pipelines take the map's rate
// (combiner REPLACE); where no map is bound it is 1x1.
float vrs_setting(const char* name, float fallback, float lo, float hi) {
  const char* text = venture::environment::getenv(name);
  const float value = text && *text ? float(std::atof(text)) : fallback;
  return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
}
struct VrsTarget {
  VkImage image{};
  VkDeviceMemory memory{};
  VkImageView view{};
  VkBuffer staging{};
  VkDeviceMemory staging_memory{};
  VkExtent2D extent{};       // in texels
  VkExtent2D framebuffer{};  // in pixels
  std::uint64_t version = 0; // the centers it was filled with (0: never filled)
};
std::mutex vrs_mutex;
std::unordered_map<VkDevice, VkExtent2D> vrs_devices;           // texel size
std::unordered_map<VkRenderPass, std::uint32_t> vrs_passes;      // the shading-rate attachment's index
std::unordered_map<VkFramebuffer, std::pair<VkDevice, VrsTarget>> vrs_targets;
float vrs_centers[2][2]{{0.5f, 0.5f}, {0.5f, 0.5f}};
std::uint64_t vrs_centers_version = 1;  // 1: the image centers, until views are located
constexpr std::uint8_t vrs_full = 0;               // 1x1
constexpr std::uint8_t vrs_coarse = (1 << 2) | 1;  // 2x2: log2(width) << 2 | log2(height)
// The settings in use: the launch environment, then the launcher's live values
// (HKCU\SOFTWARE\Venture\VRS: On, Width, Height in %, Rate 2/4, Distance in cm;
// written at launch and by Apply in game). Off keeps the attachment but fills it at 1x1.
struct VrsLive {
  bool on = true;
  float width = 0.45f, height = 0.45f;
  bool rate4 = false;
  float distance = 2.f;
  float margin = 0.10f;          // around VDXR's NR region
  float gaze_smoothing = 0.5f;   // Venture's own gaze: fraction kept from the last position
  int gaze_hold_ms = 300;        // Venture's own gaze lost: keep the area in place
  bool debug = false;            // the red outline (drawn by Venture NR, xr/venture_nr.cpp)
  bool operator==(const VrsLive&) const = default;
};
// The settings, from the environment on first use. Call with vrs_mutex held.
VrsLive& vrs_settings() {
  static VrsLive live = [] {
    VrsLive v;
    v.width = vrs_setting("VENTURE_VRS_WIDTH", 0.45f, 0.05f, 1.f);
    v.height = vrs_setting("VENTURE_VRS_HEIGHT", 0.45f, 0.05f, 1.f);
    v.rate4 = vrs_setting("VENTURE_VRS_RATE", 2.f, 2.f, 4.f) >= 4.f;
    v.distance = vrs_setting("VENTURE_VRS_DISTANCE", 2.f, 0.f, 100.f);
    return v;
  }();
  return live;
}
// VDXR DLSS-NR's foveal center (VirtualDesktop-OpenXR dlssnr.cpp, publishDlssnrFovea; same layout).
struct VdxrFovea {
  std::uint32_t magic;   // 0x564F4656
  std::uint32_t sequence;  // odd while written
  std::int64_t qpc;
  float tangent[2][2];   // per eye: the center's tangents (x right, y up)
  float nrWidth, nrHeight;
  std::uint32_t following;
};
float vrs_follow[2]{};                      // the area's size while following VDXR (0: fixed); vrs_mutex
// The center to follow, when VDXR wrote one in the last 200 ms with the gaze tracked.
#ifdef _WIN32
std::optional<VdxrFovea> read_vdxr_fovea() {
  static HANDLE mapping{};
  static const VdxrFovea* view{};
  static std::chrono::steady_clock::time_point next_open{};
  if (!view) {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_open) return std::nullopt;
    next_open = now + std::chrono::seconds(1);
    mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\VirtualDesktopOpenXR_DlssnrFovea");
    if (!mapping) return std::nullopt;
    view = static_cast<const VdxrFovea*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(VdxrFovea)));
    if (!view) {
      CloseHandle(mapping);
      mapping = {};
      return std::nullopt;
    }
    log::info("vulkan", "VRS: VDXR shares its foveal center");
  }
  VdxrFovea copy{};
  for (int attempt = 0; attempt < 4; ++attempt) {
    const std::uint32_t before = reinterpret_cast<const volatile VdxrFovea*>(view)->sequence;
    std::atomic_thread_fence(std::memory_order_acquire);
    std::memcpy(&copy, view, sizeof(copy));
    std::atomic_thread_fence(std::memory_order_acquire);
    if (!(before & 1) && reinterpret_cast<const volatile VdxrFovea*>(view)->sequence == before) {
      LARGE_INTEGER now, frequency;
      QueryPerformanceCounter(&now);
      QueryPerformanceFrequency(&frequency);
      const bool fresh = copy.magic == 0x564F4656u && now.QuadPart - copy.qpc < frequency.QuadPart / 5;
      if (!fresh || !copy.following) return std::nullopt;
      for (const auto& eye : copy.tangent)
        if (!std::isfinite(eye[0]) || !std::isfinite(eye[1])) return std::nullopt;
      if (!std::isfinite(copy.nrWidth) || !std::isfinite(copy.nrHeight)) return std::nullopt;
      copy.nrWidth = std::clamp(copy.nrWidth, 0.05f, 1.f);
      copy.nrHeight = std::clamp(copy.nrHeight, 0.05f, 1.f);
      return copy;
    }
  }
  return std::nullopt;
}
VrsLive read_vrs_live(VrsLive current) {
  HKEY key{};
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Venture\\VRS", 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
    return current;
  auto dword = [&](const wchar_t* name, std::optional<DWORD>& out) {
    DWORD value{}, size = sizeof(value), type{};
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD)
      out = value;
  };
  std::optional<DWORD> on, width, height, rate, distance, margin, smoothing, hold, debug;
  dword(L"On", on);
  dword(L"Width", width);
  dword(L"Height", height);
  dword(L"Rate", rate);
  dword(L"Distance", distance);
  dword(L"Margin", margin);
  dword(L"GazeSmoothing", smoothing);
  dword(L"GazeHoldMs", hold);
  dword(L"Debug", debug);
  RegCloseKey(key);
  if (on) current.on = *on != 0;
  if (width) current.width = std::clamp(float(*width) / 100.f, 0.05f, 1.f);
  if (height) current.height = std::clamp(float(*height) / 100.f, 0.05f, 1.f);
  if (rate) current.rate4 = *rate >= 4;
  if (distance) current.distance = std::clamp(float(*distance) / 100.f, 0.f, 100.f);
  if (margin) current.margin = std::clamp(float(*margin) / 100.f, 0.f, 0.4f);
  if (smoothing) current.gaze_smoothing = std::clamp(float(*smoothing) / 100.f, 0.f, 0.95f);
  if (hold) current.gaze_hold_ms = int(std::min<DWORD>(*hold, 2000));
  if (debug) current.debug = *debug != 0;
  return current;
}
#else
std::optional<VdxrFovea> read_vdxr_fovea() { return std::nullopt; }
VrsLive read_vrs_live(VrsLive current) { return current; }
#endif

VkExtent2D vrs_texel(VkDevice d) {
  std::lock_guard lock(vrs_mutex);
  const auto it = vrs_devices.find(d);
  return it == vrs_devices.end() ? VkExtent2D{} : it->second;
}
std::uint32_t vrs_pass_index(VkRenderPass pass) {
  std::lock_guard lock(vrs_mutex);
  const auto it = vrs_passes.find(pass);
  return it == vrs_passes.end() ? VK_ATTACHMENT_UNUSED : it->second;
}
// SwiftShader: host images standing in for guest resolve sources.
struct StandIn {
  std::uint32_t index;  // the attachment it replaces
  VkImage image;
  VkImageView view;
  VkDeviceMemory memory;
  VkImageSubresourceRange range;
  VkImage guest;
  VkImageSubresourceRange guest_range;
};
struct Substitution {
  VkDevice device;
  VkFramebufferCreateInfo info;  // the host framebuffer's, without pNext
  std::vector<VkImageView> views;  // the guest's attachments
  std::vector<StandIn> stand_ins;
  VkFramebuffer plain{};  // with the guest's views, for passes that keep a source's contents
};
std::unordered_map<VkFramebuffer, Substitution> substitutions;
// Host placeholders standing in for a degenerate guest framebuffer's null attachments.
std::unordered_map<VkFramebuffer, std::pair<VkDevice, std::vector<StandIn>>> placeholders;
std::unordered_map<VkPhysicalDevice, bool> whole_extent_resolves;
std::optional<bool> forced_stand_ins;

template <class F>
F host(VkCommandBuffer cmd, const char* name) {
  return reinterpret_cast<F>(raw_device_proc(command_device(cmd), name));
}
template <class F, ProcedureName Name>
F next(VkDevice device) {
  if (auto f = cached_layer_proc<Layer::render_area, Name>()) return reinterpret_cast<F>(f);
  return reinterpret_cast<F>(raw_device_proc(device, Name.value));
}
template <class T>
const T* find_chain(const void* next, VkStructureType type) {
  for (auto* p = static_cast<const VkBaseInStructure*>(next); p; p = p->pNext)
    if (p->sType == type) return reinterpret_cast<const T*>(p);
  return nullptr;
}
VkRect2D clamp(const VkRect2D& r, const VkRect2D& area) {
  const auto x0 = std::max<std::int64_t>(r.offset.x, area.offset.x);
  const auto y0 = std::max<std::int64_t>(r.offset.y, area.offset.y);
  const auto x1 = std::max(x0, std::min<std::int64_t>(std::int64_t(r.offset.x) + r.extent.width,
                                                      std::int64_t(area.offset.x) + area.extent.width));
  const auto y1 = std::max(y0, std::min<std::int64_t>(std::int64_t(r.offset.y) + r.extent.height,
                                                      std::int64_t(area.offset.y) + area.extent.height));
  return {{static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0)},
          {static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)}};
}
bool same(const VkRect2D& a, const VkRect2D& b) {
  return a.offset.x == b.offset.x && a.offset.y == b.offset.y && a.extent.width == b.extent.width &&
         a.extent.height == b.extent.height;
}
// Caller holds mutex. Passes this layer did not see narrow over every attachment.
bool used(VkRenderPass pass, std::uint32_t index) {
  auto it = passes.find(pass);
  return it == passes.end() || (index < it->second.used.size() && it->second.used[index]);
}
// Narrows `bounds` to a view's extent when the view is known. Caller holds mutex.
void narrow(VkExtent2D& bounds, VkImageView view) {
  if (auto it = views.find(view); it != views.end()) {
    bounds.width = std::min(bounds.width, it->second.extent.width);
    bounds.height = std::min(bounds.height, it->second.extent.height);
  }
}
void report_clamp(const char* what, std::uint32_t width, std::uint32_t height, VkExtent2D to) {
  static std::atomic<int> reports{0};
  if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
    log::warn("vulkan", "{} {}x{} exceeds its smallest attachment; the host uses {}x{}", what, width, height,
              to.width, to.height);
}
// Scissors to forward from `first` on: clamped inside a render pass.
struct Forward {
  bool with_count{};
  std::uint32_t first{};
  std::vector<VkRect2D> rects;
};
// Also records them as the host's scissors.
Forward forwarded(Scissors& s, std::uint32_t first, std::uint32_t count) {
  Forward out{s.with_count, first, {s.rects.begin() + first, s.rects.begin() + first + count}};
  if (s.area)
    for (auto& r : out.rects) r = clamp(r, *s.area);
  if (s.with_count) s.host.clear();
  if (s.host.size() < first + count) s.host.resize(first + count);
  std::copy(out.rects.begin(), out.rects.end(), s.host.begin() + first);
  return out;
}
void forward(VkCommandBuffer cmd, const Forward& f) {
  const auto count = static_cast<std::uint32_t>(f.rects.size());
  if (f.with_count)
    host<PFN_vkCmdSetScissorWithCount>(cmd, "vkCmdSetScissorWithCount")(cmd, count, f.rects.data());
  else
    host<PFN_vkCmdSetScissor>(cmd, "vkCmdSetScissor")(cmd, f.first, count, f.rects.data());
}
// Dynamic state persists into a render pass instance: scissors the guest set
// before it began are clamped to the new render area, unless the host already
// has them. A subpass recorded in secondary command buffers takes no other
// command (VkSubpassContents), and its secondaries inherit no dynamic state.
void begin_area(VkCommandBuffer cmd, const VkRect2D& area, bool inline_contents,
                std::vector<VkImageMemoryBarrier> after = {}) {
  Forward f;
  {
    std::lock_guard lock(mutex);
    auto& s = states[cmd];
    s.area = area;
    s.after = std::move(after);
    if (!inline_contents || s.rects.empty()) return;
    const auto host = s.host;
    f = forwarded(s, 0, static_cast<std::uint32_t>(s.rects.size()));
    if (host.size() == f.rects.size() && std::equal(host.begin(), host.end(), f.rects.begin(), same)) return;
  }
  forward(cmd, f);
}
bool inline_subpass(VkSubpassContents contents) {
  return contents == VK_SUBPASS_CONTENTS_INLINE || contents == VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS_KHR;
}
// Returns the final-layout transitions of guest images replaced in the instance.
std::vector<VkImageMemoryBarrier> end_area(VkCommandBuffer cmd) {
  std::lock_guard lock(mutex);
  auto it = states.find(cmd);
  if (it == states.end()) return {};
  it->second.area.reset();
  return std::exchange(it->second.after, {});
}
// Layout transitions of stand-ins and replaced guest images, outside instances.
void transition(VkCommandBuffer cmd, const std::vector<VkImageMemoryBarrier>& barriers) {
  if (barriers.empty()) return;
  host<PFN_vkCmdPipelineBarrier>(cmd, "vkCmdPipelineBarrier")(
      cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
      static_cast<std::uint32_t>(barriers.size()), barriers.data());
}

constexpr VkImageUsageFlags attachment_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                               VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
VkResult VKAPI_CALL create_image(VkDevice d, const VkImageCreateInfo* info, const VkAllocationCallbacks* alloc,
                                 VkImage* out) {
  auto r = next<PFN_vkCreateImage, "vkCreateImage">(d)(d, info, alloc, out);
  if (r == VK_SUCCESS) note_image(*out, *info);
  return r;
}
void VKAPI_CALL destroy_image(VkDevice d, VkImage image, const VkAllocationCallbacks* alloc) {
  forget_image(image);
  next<PFN_vkDestroyImage, "vkDestroyImage">(d)(d, image, alloc);
}
VkResult VKAPI_CALL create_view(VkDevice d, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* alloc,
                                VkImageView* out) {
  auto r = next<PFN_vkCreateImageView, "vkCreateImageView">(d)(d, info, alloc, out);
  if (r != VK_SUCCESS) return r;
  std::lock_guard lock(mutex);
  if (auto it = images.find(info->image); it != images.end()) {
    const auto mip = info->subresourceRange.baseMipLevel;
    views[*out] = {{std::max(1u, it->second.extent.width >> mip), std::max(1u, it->second.extent.height >> mip)},
                   info->image,
                   info->format,
                   info->viewType,
                   info->subresourceRange};
  }
  return r;
}
void VKAPI_CALL destroy_view(VkDevice d, VkImageView view, const VkAllocationCallbacks* alloc) {
  {
    std::lock_guard lock(mutex);
    views.erase(view);
  }
  next<PFN_vkDestroyImageView, "vkDestroyImageView">(d)(d, view, alloc);
}
template <class Subpass>
void use_subpass(Pass& out, const Subpass& sp) {
  auto use = [&](std::uint32_t index) {
    if (index != VK_ATTACHMENT_UNUSED && index < out.used.size()) out.used[index] = true;
  };
  auto resolve = [&](std::uint32_t source, std::uint32_t target) {
    if (source != VK_ATTACHMENT_UNUSED && target != VK_ATTACHMENT_UNUSED && source < out.used.size() &&
        target < out.used.size())
      out.resolves.emplace_back(source, target);
  };
  for (std::uint32_t i = 0; i < sp.inputAttachmentCount; ++i) {
    const auto index = sp.pInputAttachments[i].attachment;
    use(index);
    if (index != VK_ATTACHMENT_UNUSED && index < out.input.size()) out.input[index] = true;
  }
  for (std::uint32_t i = 0; i < sp.colorAttachmentCount; ++i) {
    use(sp.pColorAttachments[i].attachment);
    if (sp.pResolveAttachments) {
      use(sp.pResolveAttachments[i].attachment);
      resolve(sp.pColorAttachments[i].attachment, sp.pResolveAttachments[i].attachment);
    }
  }
  if (sp.pDepthStencilAttachment) use(sp.pDepthStencilAttachment->attachment);
  if constexpr (std::is_same_v<Subpass, VkSubpassDescription2>)
    if (auto* ds = find_chain<VkSubpassDescriptionDepthStencilResolve>(
            sp.pNext, VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE);
        ds && ds->pDepthStencilResolveAttachment) {
      use(ds->pDepthStencilResolveAttachment->attachment);
      if (sp.pDepthStencilAttachment)
        resolve(sp.pDepthStencilAttachment->attachment, ds->pDepthStencilResolveAttachment->attachment);
    }
}
bool has_stencil(VkFormat format) {
  return format == VK_FORMAT_S8_UINT || format == VK_FORMAT_D16_UNORM_S8_UINT ||
         format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}
template <class Info>
void record_pass(VkRenderPass pass, const Info& info) {
  Pass out;
  out.used.resize(info.attachmentCount);
  out.input.resize(info.attachmentCount);
  for (std::uint32_t i = 0; i < info.attachmentCount; ++i) {
    // STORE_OP_NONE keeps contents the instance did not write.
    const auto& a = info.pAttachments[i];
    const bool stencil = has_stencil(a.format);
    out.transient.push_back(a.loadOp != VK_ATTACHMENT_LOAD_OP_LOAD && a.storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE &&
                            (!stencil || (a.stencilLoadOp != VK_ATTACHMENT_LOAD_OP_LOAD &&
                                          a.stencilStoreOp == VK_ATTACHMENT_STORE_OP_DONT_CARE)));
    out.initial_layouts.push_back(a.initialLayout);
    out.final_layouts.push_back(a.finalLayout);
    out.formats.push_back(a.format);
    out.samples.push_back(a.samples);
  }
  for (std::uint32_t i = 0; i < info.subpassCount; ++i) use_subpass(out, info.pSubpasses[i]);
  auto views = [&](std::uint32_t mask) { out.view_layers = std::max(out.view_layers, std::uint32_t(std::bit_width(mask))); };
  if constexpr (std::is_same_v<Info, VkRenderPassCreateInfo2>) {
    for (std::uint32_t i = 0; i < info.subpassCount; ++i) views(info.pSubpasses[i].viewMask);
  } else if (auto* multiview = find_chain<VkRenderPassMultiviewCreateInfo>(
                 info.pNext, VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO)) {
    for (std::uint32_t i = 0; i < multiview->subpassCount; ++i) views(multiview->pViewMasks[i]);
  }
  std::lock_guard lock(mutex);
  passes[pass] = std::move(out);
}
void report_added_dependencies(std::size_t added, std::uint32_t subpasses) {
  static std::atomic<int> reports{0};
  if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
    log::info("vulkan", "Render pass with {} subpasses: added {} subpass dependencies its attachment accesses need",
              subpasses, added);
}
// The guest's dependencies plus the missing ones (subpass_sync.cpp). A
// VkRenderPassMultiviewCreateInfo with per-dependency view offsets gets offset
// 0 for each added dependency, which are not view-local.
struct CompletedPass {
  VkRenderPassCreateInfo info;
  std::vector<VkSubpassDependency> dependencies;
  std::vector<std::int32_t> offsets;
  std::vector<std::vector<std::byte>> chain;
};
bool complete(const VkRenderPassCreateInfo& guest, CompletedPass& out) {
  auto added = missing_subpass_dependencies(guest);
  if (added.empty()) return false;
  out.info = guest;
  out.dependencies.assign(guest.pDependencies, guest.pDependencies + guest.dependencyCount);
  out.dependencies.insert(out.dependencies.end(), added.begin(), added.end());
  out.info.dependencyCount = std::uint32_t(out.dependencies.size());
  out.info.pDependencies = out.dependencies.data();
  auto* mv = find_chain<VkRenderPassMultiviewCreateInfo>(guest.pNext, VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO);
  if (mv && mv->dependencyCount) {
    out.offsets.assign(mv->pViewOffsets, mv->pViewOffsets + mv->dependencyCount);
    out.offsets.resize(out.dependencies.size(), 0);
    // Shallow copies of the chain, relinked, with the multiview structure's
    // offsets replaced.
    const void* head = nullptr;
    VkBaseOutStructure* tail = nullptr;
    for (auto* p = static_cast<const VkBaseInStructure*>(guest.pNext); p; p = p->pNext) {
      const auto size = structure_size(p->sType);
      if (!size) continue;
      auto& copy = out.chain.emplace_back(size);
      std::memcpy(copy.data(), p, size);
      auto* base = reinterpret_cast<VkBaseOutStructure*>(copy.data());
      base->pNext = nullptr;
      if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO) {
        auto* copied = reinterpret_cast<VkRenderPassMultiviewCreateInfo*>(base);
        copied->dependencyCount = std::uint32_t(out.offsets.size());
        copied->pViewOffsets = out.offsets.data();
      }
      if (tail) tail->pNext = base;
      else head = base;
      tail = base;
    }
    out.info.pNext = head;
  }
  report_added_dependencies(added.size(), guest.subpassCount);
  return true;
}
template <ProcedureName Name>
VkResult VKAPI_CALL create_render_pass2(VkDevice d, const VkRenderPassCreateInfo2* info,
                                        const VkAllocationCallbacks* alloc, VkRenderPass* out);
// VENTURE_VRS on a vkCreateRenderPass pass: the same pass as a VkRenderPassCreateInfo2, which
// can carry a shading-rate attachment. Only stereo scene passes (a multiview subpass over both
// views with a depth attachment) whose chain holds nothing but multiview and input aspects.
struct Pass2 {
  VkRenderPassCreateInfo2 info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
  std::vector<VkAttachmentDescription2> attachments;
  std::vector<VkSubpassDescription2> subpasses;
  std::vector<std::vector<VkAttachmentReference2>> references;  // per subpass: inputs, colors, resolves, depth
  std::vector<VkSubpassDependency2> dependencies;
};
bool vrs_pass2(const VkRenderPassCreateInfo& v1, Pass2& out) {
  const VkRenderPassMultiviewCreateInfo* multiview = nullptr;
  const VkRenderPassInputAttachmentAspectCreateInfo* aspects = nullptr;
  for (auto* p = static_cast<const VkBaseInStructure*>(v1.pNext); p; p = p->pNext) {
    if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO)
      multiview = reinterpret_cast<const VkRenderPassMultiviewCreateInfo*>(p);
    else if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO)
      aspects = reinterpret_cast<const VkRenderPassInputAttachmentAspectCreateInfo*>(p);
    else
      return false;
  }
  if (!multiview || multiview->subpassCount != v1.subpassCount) return false;
  bool scene = false;
  for (std::uint32_t i = 0; i < v1.subpassCount; ++i) {
    const auto* depth = v1.pSubpasses[i].pDepthStencilAttachment;
    scene |= multiview->pViewMasks[i] == 3 && depth && depth->attachment < v1.attachmentCount &&
             is_depth_format(v1.pAttachments[depth->attachment].format);
  }
  if (!scene) return false;
  for (std::uint32_t i = 0; i < v1.attachmentCount; ++i) {
    const auto& a = v1.pAttachments[i];
    out.attachments.push_back({VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, nullptr, a.flags, a.format, a.samples,
                               a.loadOp, a.storeOp, a.stencilLoadOp, a.stencilStoreOp, a.initialLayout,
                               a.finalLayout});
  }
  // An input attachment's aspects: as the guest gave them, else all of its format's.
  const auto aspect_of = [&](std::uint32_t subpass, std::uint32_t index, std::uint32_t attachment) {
    if (aspects)
      for (std::uint32_t i = 0; i < aspects->aspectReferenceCount; ++i)
        if (aspects->pAspectReferences[i].subpass == subpass && aspects->pAspectReferences[i].inputAttachmentIndex == index)
          return aspects->pAspectReferences[i].aspectMask;
    if (attachment >= v1.attachmentCount) return VkImageAspectFlags(0);
    const auto format = v1.pAttachments[attachment].format;
    if (!is_depth_format(format)) return VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);
    return VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0));
  };
  out.references.resize(v1.subpassCount);
  for (std::uint32_t i = 0; i < v1.subpassCount; ++i) {
    const auto& s = v1.pSubpasses[i];
    auto& refs = out.references[i];
    refs.reserve(s.inputAttachmentCount + 2 * s.colorAttachmentCount + 1);
    const auto add = [&](const VkAttachmentReference& r, VkImageAspectFlags aspect) {
      refs.push_back({VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, r.attachment, r.layout, aspect});
      return &refs.back();
    };
    VkSubpassDescription2 d{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    d.flags = s.flags;
    d.pipelineBindPoint = s.pipelineBindPoint;
    d.viewMask = multiview->pViewMasks[i];
    d.inputAttachmentCount = s.inputAttachmentCount;
    for (std::uint32_t j = 0; j < s.inputAttachmentCount; ++j) {
      auto* r = add(s.pInputAttachments[j], aspect_of(i, j, s.pInputAttachments[j].attachment));
      if (!j) d.pInputAttachments = r;
    }
    d.colorAttachmentCount = s.colorAttachmentCount;
    for (std::uint32_t j = 0; j < s.colorAttachmentCount; ++j) {
      auto* r = add(s.pColorAttachments[j], 0);
      if (!j) d.pColorAttachments = r;
    }
    if (s.pResolveAttachments)
      for (std::uint32_t j = 0; j < s.colorAttachmentCount; ++j) {
        auto* r = add(s.pResolveAttachments[j], 0);
        if (!j) d.pResolveAttachments = r;
      }
    if (s.pDepthStencilAttachment) d.pDepthStencilAttachment = add(*s.pDepthStencilAttachment, 0);
    d.preserveAttachmentCount = s.preserveAttachmentCount;
    d.pPreserveAttachments = s.pPreserveAttachments;
    out.subpasses.push_back(d);
  }
  for (std::uint32_t i = 0; i < v1.dependencyCount; ++i) {
    const auto& g = v1.pDependencies[i];
    const std::int32_t offset = i < multiview->dependencyCount && multiview->pViewOffsets ? multiview->pViewOffsets[i] : 0;
    out.dependencies.push_back({VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, nullptr, g.srcSubpass, g.dstSubpass,
                                g.srcStageMask, g.dstStageMask, g.srcAccessMask, g.dstAccessMask, g.dependencyFlags,
                                offset});
  }
  out.info.flags = v1.flags;
  out.info.attachmentCount = v1.attachmentCount;
  out.info.pAttachments = out.attachments.data();
  out.info.subpassCount = v1.subpassCount;
  out.info.pSubpasses = out.subpasses.data();
  out.info.dependencyCount = std::uint32_t(out.dependencies.size());
  out.info.pDependencies = out.dependencies.data();
  out.info.correlatedViewMaskCount = multiview->correlationMaskCount;
  out.info.pCorrelatedViewMasks = multiview->pCorrelationMasks;
  return true;
}
VkResult VKAPI_CALL create_render_pass(VkDevice d, const VkRenderPassCreateInfo* info,
                                       const VkAllocationCallbacks* alloc, VkRenderPass* out) {
  // VENTURE_VRS: a stereo scene pass goes through vkCreateRenderPass2 (it does the rest below too).
  if (vrs_texel(d).width) {
    Pass2 pass2;
    if (vrs_pass2(*info, pass2)) {
      static std::atomic<int> reports{0};
      if (reports.fetch_add(1, std::memory_order_relaxed) < 4)
        log::info("vulkan", "VRS: a vkCreateRenderPass scene pass is created as a render pass 2");
      return create_render_pass2<"vkCreateRenderPass2KHR">(d, &pass2.info, alloc, out);
    }
  }
  CompletedPass completed;
  const auto* host = complete(*info, completed) ? &completed.info : info;
  auto r = next<PFN_vkCreateRenderPass, "vkCreateRenderPass">(d)(d, host, alloc, out);
  if (r == VK_SUCCESS) record_pass(*out, *info);
  return r;
}
// Also vkCreateRenderPass2KHR, the same command for devices below 1.2.
template <ProcedureName Name>
VkResult VKAPI_CALL create_render_pass2(VkDevice d, const VkRenderPassCreateInfo2* info,
                                        const VkAllocationCallbacks* alloc, VkRenderPass* out) {
  VkRenderPassCreateInfo2 completed;
  std::vector<VkSubpassDependency2> dependencies;
  const auto* host = info;
  if (auto added = missing_subpass_dependencies(*info); !added.empty()) {
    dependencies.assign(info->pDependencies, info->pDependencies + info->dependencyCount);
    dependencies.insert(dependencies.end(), added.begin(), added.end());
    completed = *info;
    completed.dependencyCount = std::uint32_t(dependencies.size());
    completed.pDependencies = dependencies.data();
    host = &completed;
    report_added_dependencies(added.size(), info->subpassCount);
  }
  // VENTURE_VRS: a shading-rate attachment (last) for the multiview subpasses with a depth attachment.
  // The pass is recorded as the guest made it: framebuffers get the attachment at creation.
  const auto* recorded = host;
  VkRenderPassCreateInfo2 shaded;
  std::vector<VkAttachmentDescription2> shaded_attachments;
  std::vector<VkSubpassDescription2> shaded_subpasses;
  std::vector<VkFragmentShadingRateAttachmentInfoKHR> rates;
  VkAttachmentReference2 rate_reference{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, host->attachmentCount,
                                        VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR, 0};
  if (const auto texel = vrs_texel(d); texel.width) {
    rates.resize(host->subpassCount);
    for (std::uint32_t i = 0; i < host->subpassCount; ++i) {
      const auto& subpass = host->pSubpasses[i];
      const auto* depth = subpass.pDepthStencilAttachment;
      if (subpass.viewMask != 3 || !depth || depth->attachment >= host->attachmentCount ||
          !is_depth_format(host->pAttachments[depth->attachment].format) ||
          find_chain<VkFragmentShadingRateAttachmentInfoKHR>(
              subpass.pNext, VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR))
        continue;
      if (shaded_subpasses.empty()) shaded_subpasses.assign(host->pSubpasses, host->pSubpasses + host->subpassCount);
      rates[i] = {VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR, subpass.pNext, &rate_reference, texel};
      shaded_subpasses[i].pNext = &rates[i];
    }
    if (!shaded_subpasses.empty()) {
      shaded_attachments.assign(host->pAttachments, host->pAttachments + host->attachmentCount);
      VkAttachmentDescription2 rate{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
      rate.format = VK_FORMAT_R8_UINT;
      rate.samples = VK_SAMPLE_COUNT_1_BIT;
      rate.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      rate.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      rate.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      rate.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      rate.initialLayout = rate.finalLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
      shaded_attachments.push_back(rate);
      shaded = *host;
      shaded.attachmentCount = std::uint32_t(shaded_attachments.size());
      shaded.pAttachments = shaded_attachments.data();
      shaded.pSubpasses = shaded_subpasses.data();
      host = &shaded;
    }
  }
  auto r = next<PFN_vkCreateRenderPass2, Name>(d)(d, host, alloc, out);
  if (r == VK_SUCCESS) {
    record_pass(*out, *recorded);
    if (host == &shaded) {
      std::lock_guard lock(vrs_mutex);
      vrs_passes[*out] = recorded->attachmentCount;
      static std::atomic<int> reports{0};
      if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
        log::info("vulkan", "VRS: scene render pass {} gets a shading-rate attachment",
                  static_cast<const void*>(*out));
    }
  } else if (host == &shaded) {
    log::warn("vulkan", "VRS: a render pass with a shading-rate attachment was refused ({}); created without",
              int(r));
    r = next<PFN_vkCreateRenderPass2, Name>(d)(d, recorded, alloc, out);
    if (r == VK_SUCCESS) record_pass(*out, *recorded);
  }
  return r;
}
void VKAPI_CALL destroy_render_pass(VkDevice d, VkRenderPass pass, const VkAllocationCallbacks* alloc) {
  {
    std::lock_guard lock(mutex);
    passes.erase(pass);
  }
  next<PFN_vkDestroyRenderPass, "vkDestroyRenderPass">(d)(d, pass, alloc);
}
template <class F>
F raw(VkDevice d, const char* name) {
  return reinterpret_cast<F>(raw_device_proc(d, name));
}
// Whether the device's driver is SwiftShader, which resolves a source's whole
// extent. Queried once per physical device.
bool resolves_whole_extent(VkDevice d) {
  const auto physical = device_physical(d);
  {
    std::lock_guard lock(mutex);
    if (forced_stand_ins) return *forced_stand_ins;
    if (auto it = whole_extent_resolves.find(physical); it != whole_extent_resolves.end()) return it->second;
  }
  VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
  if (auto get = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
          raw_physical_proc(physical, "vkGetPhysicalDeviceProperties2")))
    get(physical, &properties);
  std::lock_guard lock(mutex);
  return whole_extent_resolves[physical] = driver.driverID == VK_DRIVER_ID_GOOGLE_SWIFTSHADER;
}
// Without separateDepthStencilLayouts a layout transition covers both aspects.
VkImageSubresourceRange transition_range(VkImageSubresourceRange range, VkFormat format) {
  if (range.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
    range.aspectMask = format == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT
                       : has_stencil(format)      ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                                  : VK_IMAGE_ASPECT_DEPTH_BIT;
  return range;
}
VkImageMemoryBarrier layout_barrier(VkImage image, const VkImageSubresourceRange& range, VkImageLayout from,
                                    VkImageLayout to) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = range;
  return b;
}
void release(VkDevice d, const StandIn& s) {
  raw<PFN_vkDestroyImageView>(d, "vkDestroyImageView")(d, s.view, nullptr);
  raw<PFN_vkDestroyImage>(d, "vkDestroyImage")(d, s.image, nullptr);
  raw<PFN_vkFreeMemory>(d, "vkFreeMemory")(d, s.memory, nullptr);
}
// A host-only transient attachment image of `extent` and its view, or nullopt.
std::uint32_t vrs_memory_type(VkDevice d, std::uint32_t bits, VkMemoryPropertyFlags wanted) {
  VkPhysicalDeviceMemoryProperties props{};
  const auto physical = device_physical(d);
  reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
      raw_physical_proc(physical, "vkGetPhysicalDeviceMemoryProperties"))(physical, &props);
  for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
  return props.memoryTypeCount;
}
void release_vrs_target(VkDevice d, VrsTarget& t) {
  if (t.view) raw<PFN_vkDestroyImageView>(d, "vkDestroyImageView")(d, t.view, nullptr);
  if (t.image) raw<PFN_vkDestroyImage>(d, "vkDestroyImage")(d, t.image, nullptr);
  if (t.memory) raw<PFN_vkFreeMemory>(d, "vkFreeMemory")(d, t.memory, nullptr);
  if (t.staging) raw<PFN_vkDestroyBuffer>(d, "vkDestroyBuffer")(d, t.staging, nullptr);
  if (t.staging_memory) raw<PFN_vkFreeMemory>(d, "vkFreeMemory")(d, t.staging_memory, nullptr);
  t = {};
}
// The shading-rate image of one framebuffer (two layers, one per view) and its upload buffer.
std::optional<VrsTarget> make_vrs_target(VkDevice d, VkExtent2D framebuffer, VkExtent2D texel) {
  VrsTarget t;
  t.framebuffer = framebuffer;
  t.extent = {(framebuffer.width + texel.width - 1) / texel.width, (framebuffer.height + texel.height - 1) / texel.height};
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = VK_FORMAT_R8_UINT;
  ci.extent = {t.extent.width, t.extent.height, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 2;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  bool ok = raw<PFN_vkCreateImage>(d, "vkCreateImage")(d, &ci, nullptr, &t.image) == VK_SUCCESS;
  if (ok) {
    VkMemoryRequirements req{};
    raw<PFN_vkGetImageMemoryRequirements>(d, "vkGetImageMemoryRequirements")(d, t.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                            vrs_memory_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    ok = raw<PFN_vkAllocateMemory>(d, "vkAllocateMemory")(d, &ai, nullptr, &t.memory) == VK_SUCCESS &&
         raw<PFN_vkBindImageMemory>(d, "vkBindImageMemory")(d, t.image, t.memory, 0) == VK_SUCCESS;
  }
  if (ok) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = VK_FORMAT_R8_UINT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
    ok = raw<PFN_vkCreateImageView>(d, "vkCreateImageView")(d, &vi, nullptr, &t.view) == VK_SUCCESS;
  }
  if (ok) {
    const VkDeviceSize layer = (VkDeviceSize(t.extent.width) * t.extent.height + 3) & ~VkDeviceSize(3);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = 2 * layer;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    ok = raw<PFN_vkCreateBuffer>(d, "vkCreateBuffer")(d, &bi, nullptr, &t.staging) == VK_SUCCESS;
    if (ok) {
      VkMemoryRequirements req{};
      raw<PFN_vkGetBufferMemoryRequirements>(d, "vkGetBufferMemoryRequirements")(d, t.staging, &req);
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                              vrs_memory_type(d, req.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
      ok = raw<PFN_vkAllocateMemory>(d, "vkAllocateMemory")(d, &ai, nullptr, &t.staging_memory) == VK_SUCCESS &&
           raw<PFN_vkBindBufferMemory>(d, "vkBindBufferMemory")(d, t.staging, t.staging_memory, 0) == VK_SUCCESS;
    }
  }
  if (!ok) {
    release_vrs_target(d, t);
    return std::nullopt;
  }
  return t;
}
// Before a scene pass begins: (re)fill its map when the centers changed.
void prepare_vrs(VkCommandBuffer cmd, VkFramebuffer framebuffer) {
  std::lock_guard lock(vrs_mutex);
  const auto it = vrs_targets.find(framebuffer);
  if (it == vrs_targets.end()) return;
  const auto d = it->second.first;
  auto& t = it->second.second;
  if (t.version == vrs_centers_version) return;
  const VkDeviceSize layer = (VkDeviceSize(t.extent.width) * t.extent.height + 3) & ~VkDeviceSize(3);
  void* mapped{};
  if (raw<PFN_vkMapMemory>(d, "vkMapMemory")(d, t.staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return;
  // The full-rate region: width x height (fractions of the image) around each eye's center.
  const VrsLive& live = vrs_settings();
  const float half_w = 0.5f * std::max(live.width, vrs_follow[0]) * float(t.framebuffer.width);
  const float half_h = 0.5f * std::max(live.height, vrs_follow[1]) * float(t.framebuffer.height);
  // Outside: 2x2, or 4x4 (a rate the device lacks is clamped to a supported one); 1x1 everywhere when off.
  const std::uint8_t coarse = !live.on ? vrs_full : live.rate4 ? std::uint8_t((2 << 2) | 2) : vrs_coarse;
  for (std::uint32_t eye = 0; eye < 2; ++eye) {
    auto* out = static_cast<std::uint8_t*>(mapped) + eye * layer;
    const float cx = vrs_centers[eye][0] * float(t.framebuffer.width);
    const float cy = vrs_centers[eye][1] * float(t.framebuffer.height);
    const float tw = float(t.framebuffer.width) / float(t.extent.width);
    const float th = float(t.framebuffer.height) / float(t.extent.height);
    for (std::uint32_t y = 0; y < t.extent.height; ++y)
      for (std::uint32_t x = 0; x < t.extent.width; ++x) {
        // A texel the region touches stays full rate.
        const bool inside = (x + 1) * tw > cx - half_w && x * tw < cx + half_w && (y + 1) * th > cy - half_h &&
                            y * th < cy + half_h;
        out[y * t.extent.width + x] = inside ? vrs_full : coarse;
      }
  }
  raw<PFN_vkUnmapMemory>(d, "vkUnmapMemory")(d, t.staging_memory);
  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
  VkImageMemoryBarrier to_copy = layout_barrier(
      t.image, range, t.version ? VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  to_copy.srcAccessMask = t.version ? VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR : 0;
  to_copy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  host<PFN_vkCmdPipelineBarrier>(cmd, "vkCmdPipelineBarrier")(
      cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_copy);
  const VkBufferImageCopy regions[2] = {
      {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {t.extent.width, t.extent.height, 1}},
      {layer, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1}, {0, 0, 0}, {t.extent.width, t.extent.height, 1}}};
  host<PFN_vkCmdCopyBufferToImage>(cmd, "vkCmdCopyBufferToImage")(cmd, t.staging, t.image,
                                                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, regions);
  VkImageMemoryBarrier to_read = layout_barrier(t.image, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR);
  to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_read.dstAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
  host<PFN_vkCmdPipelineBarrier>(cmd, "vkCmdPipelineBarrier")(
      cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, 0, 0, nullptr,
      0, nullptr, 1, &to_read);
  static std::atomic<int> reports{0};
  if (reports.fetch_add(1, std::memory_order_relaxed) < 4)
    log::info("vulkan", "VRS map {}x{} texels for a {}x{} scene pass: full rate in {:.0f}x{:.0f} px around ({:.3f},{:.3f}) / "
              "({:.3f},{:.3f}), outside rate code {}", t.extent.width, t.extent.height, t.framebuffer.width,
              t.framebuffer.height, 2 * half_w, 2 * half_h, vrs_centers[0][0], vrs_centers[0][1], vrs_centers[1][0],
              vrs_centers[1][1], int(coarse));
  t.version = vrs_centers_version;
}
std::optional<StandIn> transient_attachment(VkDevice d, std::uint32_t index, VkFormat format,
                                            VkSampleCountFlagBits samples, VkImageViewType type,
                                            VkImageAspectFlags aspect, VkExtent2D extent, std::uint32_t layers,
                                            VkImageUsageFlags usage, VkResult* failure = nullptr) {
  auto failed = [&](VkResult result) -> std::optional<StandIn> {
    if (failure) *failure = result;
    return std::nullopt;
  };
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = format;
  ci.extent = {extent.width, extent.height, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = layers;
  ci.samples = samples;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = usage | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
  StandIn s{index};
  auto result = raw<PFN_vkCreateImage>(d, "vkCreateImage")(d, &ci, nullptr, &s.image);
  if (result != VK_SUCCESS) return failed(result);
  VkMemoryRequirements req{};
  raw<PFN_vkGetImageMemoryRequirements>(d, "vkGetImageMemoryRequirements")(d, s.image, &req);
  VkPhysicalDeviceMemoryProperties props{};
  const auto physical = device_physical(d);
  reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
      raw_physical_proc(physical, "vkGetPhysicalDeviceMemoryProperties"))(physical, &props);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = props.memoryTypeCount;
  for (VkMemoryPropertyFlags wanted : {VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT),
                                       VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), VkMemoryPropertyFlags{}})
    for (std::uint32_t i = 0; i < props.memoryTypeCount && ai.memoryTypeIndex == props.memoryTypeCount; ++i)
      if ((req.memoryTypeBits >> i & 1) && (props.memoryTypes[i].propertyFlags & wanted) == wanted)
        ai.memoryTypeIndex = i;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = s.image;
  vi.viewType = type;
  vi.format = format;
  vi.subresourceRange = {aspect, 0, 1, 0, layers};
  result = ai.memoryTypeIndex == props.memoryTypeCount ? VK_ERROR_FEATURE_NOT_PRESENT
      : raw<PFN_vkAllocateMemory>(d, "vkAllocateMemory")(d, &ai, nullptr, &s.memory);
  if (result == VK_SUCCESS)
    result = raw<PFN_vkBindImageMemory>(d, "vkBindImageMemory")(d, s.image, s.memory, 0);
  if (result == VK_SUCCESS)
    result = raw<PFN_vkCreateImageView>(d, "vkCreateImageView")(d, &vi, nullptr, &s.view);
  if (result != VK_SUCCESS) {
    release(d, s);
    return failed(result);
  }
  s.range = transition_range(vi.subresourceRange, format);
  return s;
}
// A host-only transient image of `extent` for attachment `index`'s view, or nullopt.
std::optional<StandIn> stand_in(VkDevice d, std::uint32_t index, const View& view, const Image& image,
                                VkExtent2D extent) {
  if (view.type != VK_IMAGE_VIEW_TYPE_2D && view.type != VK_IMAGE_VIEW_TYPE_2D_ARRAY) return std::nullopt;
  const auto layers = view.range.layerCount == VK_REMAINING_ARRAY_LAYERS
                          ? image.layers - view.range.baseArrayLayer
                          : view.range.layerCount;
  auto s = transient_attachment(d, index, view.format, image.samples, view.type, view.range.aspectMask, extent,
                                layers, image.usage & attachment_usage);
  if (!s) return s;
  s->guest = view.image;
  s->guest_range = transition_range(view.range, view.format);
  return s;
}
VkResult create_host_framebuffer(VkDevice d, const VkFramebufferCreateInfo* info,
                                 const VkAllocationCallbacks* alloc, VkFramebuffer* out) {
  VkExtent2D bounds{info->width, info->height};
  // SwiftShader: resolve sources larger than their resolve attachments.
  struct Source {
    std::uint32_t index;
    View view;
    Image image;
  };
  std::vector<Source> sources;
  const bool imageless = info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT;
  const bool whole_extent = !imageless && resolves_whole_extent(d);
  {
    std::lock_guard lock(mutex);
    if (!imageless)
      for (std::uint32_t i = 0; i < info->attachmentCount; ++i)
        if (used(info->renderPass, i)) narrow(bounds, info->pAttachments[i]);
    if (auto* list = imageless ? find_chain<VkFramebufferAttachmentsCreateInfo>(
                                     info->pNext, VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO)
                               : nullptr)
      for (std::uint32_t i = 0; i < list->attachmentImageInfoCount; ++i) {
        const auto& a = list->pAttachmentImageInfos[i];
        if (!used(info->renderPass, i) || !(a.usage & attachment_usage)) continue;
        bounds.width = std::min(bounds.width, a.width);
        bounds.height = std::min(bounds.height, a.height);
      }
    auto pass = passes.find(info->renderPass);
    if (whole_extent && pass != passes.end())
      for (auto [source, target] : pass->second.resolves) {
        // An input attachment's descriptor names the guest's view.
        if (source >= info->attachmentCount || target >= info->attachmentCount || pass->second.input[source] ||
            std::any_of(sources.begin(), sources.end(), [&](const Source& s) { return s.index == source; }))
          continue;
        auto from = views.find(info->pAttachments[source]);
        auto to = views.find(info->pAttachments[target]);
        if (from == views.end() || to == views.end() ||
            (from->second.extent.width <= to->second.extent.width &&
             from->second.extent.height <= to->second.extent.height))
          continue;
        auto image = images.find(from->second.image);
        if (image == images.end() || image->second.samples <= VK_SAMPLE_COUNT_1_BIT) continue;
        if (!pass->second.transient[source]) {
          static std::atomic<int> kept{0};
          if (kept.fetch_add(1, std::memory_order_relaxed) < 8)
            log::warn("vulkan", "Resolve source {}x{} keeps its contents but exceeds its {}x{} resolve attachment",
                      from->second.extent.width, from->second.extent.height, to->second.extent.width,
                      to->second.extent.height);
          continue;
        }
        sources.push_back({source, from->second, image->second});
      }
  }
  Substitution sub{d, *info};
  std::vector<VkImageView> attachments;
  for (const auto& source : sources)
    if (auto s = stand_in(d, source.index, source.view, source.image, bounds)) {
      static std::atomic<int> reports{0};
      if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
        log::warn("vulkan", "Resolve source {}x{} exceeds its resolve attachment; the host renders it in a {}x{} "
                  "transient image", source.view.extent.width, source.view.extent.height, bounds.width,
                  bounds.height);
      if (attachments.empty()) attachments.assign(info->pAttachments, info->pAttachments + info->attachmentCount);
      attachments[source.index] = s->view;
      sub.stand_ins.push_back(*s);
    }
  const bool resized = bounds.width != info->width || bounds.height != info->height;
  // VENTURE_VRS: the scene pass's shading-rate map, appended to its attachments.
  const auto create = [&](const VkFramebufferCreateInfo* ci) {
    const auto index = vrs_pass_index(ci->renderPass);
    if (index == VK_ATTACHMENT_UNUSED) return next<PFN_vkCreateFramebuffer, "vkCreateFramebuffer">(d)(d, ci, alloc, out);
    if ((ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) || ci->attachmentCount != index) {
      log::warn("vulkan", "VRS: an imageless or unexpected framebuffer for a scene pass is not supported");
      return VK_ERROR_INITIALIZATION_FAILED;
    }
    auto target = make_vrs_target(d, {ci->width, ci->height}, vrs_texel(d));
    if (!target) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    std::vector<VkImageView> list(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
    list.push_back(target->view);
    auto with_rate = *ci;
    with_rate.attachmentCount = std::uint32_t(list.size());
    with_rate.pAttachments = list.data();
    const auto result = next<PFN_vkCreateFramebuffer, "vkCreateFramebuffer">(d)(d, &with_rate, alloc, out);
    if (result != VK_SUCCESS) {
      release_vrs_target(d, *target);
      return result;
    }
    std::lock_guard lock(vrs_mutex);
    vrs_targets[*out] = {d, *target};
    return result;
  };
  if (!resized && sub.stand_ins.empty()) return create(info);
  if (resized) report_clamp("Framebuffer", info->width, info->height, bounds);
  auto copy = *info;
  copy.width = bounds.width;
  copy.height = bounds.height;
  if (!sub.stand_ins.empty()) copy.pAttachments = attachments.data();
  auto r = create(&copy);
  if (r != VK_SUCCESS) {
    for (const auto& s : sub.stand_ins) release(d, s);
    return r;
  }
  std::lock_guard lock(mutex);
  if (resized) clamped[*out] = bounds;
  if (!sub.stand_ins.empty()) {
    sub.info.pNext = nullptr;
    sub.info.width = bounds.width;
    sub.info.height = bounds.height;
    sub.views.assign(info->pAttachments, info->pAttachments + info->attachmentCount);
    substitutions[*out] = std::move(sub);
  }
  return r;
}
// A guest framebuffer with no size or a null attachment view (HITMAN on Monado
// computed 0x0 render targets and never created them) is invalid, and RADV
// dereferences the null view when a pass on it begins. A Quest driver does not
// crash there, so each missing attachment gets a host placeholder: 1x1 for a
// framebuffer with no size, which then renders into nothing, otherwise of the
// framebuffer's size so the views the guest did pass keep rendering (transient,
// lazily allocated memory where the device has it; released with the
// framebuffer). Unity skips an attachment whose image it lacks
// ("Missing Vulkan framebuffer attachment image?") and passes fewer attachments
// than the render pass has (Attack on Titan VR: NVIDIA faulted on a null image
// view in vkCmdBeginRenderPass); the views it did pass keep their order, so each
// takes the next render pass attachment of its format and the rest get
// placeholders as well.
VkResult VKAPI_CALL create_framebuffer(VkDevice d, const VkFramebufferCreateInfo* info,
                                       const VkAllocationCallbacks* alloc, VkFramebuffer* out) {
  std::size_t expected = info->attachmentCount;
  {
    std::lock_guard lock(mutex);
    if (auto it = passes.find(info->renderPass); it != passes.end()) expected = it->second.formats.size();
  }
  if (info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT ||
      (info->width && info->height && info->attachmentCount >= expected &&
       std::none_of(info->pAttachments, info->pAttachments + info->attachmentCount,
                    [](VkImageView v) { return v == VK_NULL_HANDLE; })))
    return create_host_framebuffer(d, info, alloc, out);
  std::optional<Pass> pass;
  std::vector<VkImageView> attachments(info->pAttachments, info->pAttachments + info->attachmentCount);
  {
    std::lock_guard lock(mutex);
    if (auto it = passes.find(info->renderPass); it != passes.end()) pass = it->second;
    if (pass && attachments.size() < pass->formats.size()) {
      std::vector<VkImageView> aligned(pass->formats.size(), VK_NULL_HANDLE);
      std::size_t given = 0;
      for (std::size_t i = 0; i < aligned.size() && given < attachments.size(); ++i) {
        const auto view = views.find(attachments[given]);
        if (!attachments[given] || view == views.end() || view->second.format == pass->formats[i])
          aligned[i] = attachments[given++];
      }
      attachments = std::move(aligned);
    }
  }
  std::vector<StandIn> blanks;
  auto failed = [&](VkResult result) {
    for (const auto& blank : blanks) release(d, blank);
    // Image creation has errors that vkCreateFramebuffer cannot return.
    if (result != VK_ERROR_OUT_OF_HOST_MEMORY && result != VK_ERROR_OUT_OF_DEVICE_MEMORY &&
        result != VK_ERROR_VALIDATION_FAILED_EXT)
      return VK_ERROR_UNKNOWN;
    return result;
  };
  const auto layers = std::max(info->layers, pass ? pass->view_layers : 1u);
  const bool sized = info->width && info->height;
  const VkExtent2D extent = sized ? VkExtent2D{info->width, info->height} : VkExtent2D{1, 1};
  for (std::uint32_t i = 0; i < attachments.size(); ++i) {
    if (attachments[i]) continue;
    // Never forward an unresolved null view, even when repair is impossible.
    if (!pass || i >= pass->formats.size()) return failed(VK_ERROR_VALIDATION_FAILED_EXT);
    const auto format = pass->formats[i];
    const bool depth = format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_X8_D24_UNORM_PACK32 ||
                       format == VK_FORMAT_D32_SFLOAT || has_stencil(format);
    const VkImageAspectFlags aspect = !depth ? VK_IMAGE_ASPECT_COLOR_BIT
        : format == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT
        : has_stencil(format) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    VkImageUsageFlags usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (pass->input[i]) usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    VkResult failure = VK_ERROR_UNKNOWN;
    if (auto blank = transient_attachment(d, i, format, pass->samples[i], VK_IMAGE_VIEW_TYPE_2D_ARRAY, aspect, extent,
                                          layers, usage, &failure)) {
      attachments[i] = blank->view;
      blanks.push_back(*blank);
    } else return failed(failure);
  }
  static std::atomic<int> reports{0};
  if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
    log::warn("vulkan", "Guest framebuffer {}x{} has {} null or missing attachment view(s) of {}; the host gets a "
              "{}x{} framebuffer with placeholders", info->width, info->height, blanks.size(), attachments.size(),
              extent.width, extent.height);
  auto copy = *info;
  copy.attachmentCount = static_cast<std::uint32_t>(attachments.size());
  copy.pAttachments = attachments.data();
  copy.width = extent.width;
  copy.height = extent.height;
  copy.layers = std::max(info->layers, 1u);
  const auto r = create_host_framebuffer(d, &copy, alloc, out);
  if (r != VK_SUCCESS) return failed(r);
  std::lock_guard lock(mutex);
  if (!sized) clamped[*out] = extent;
  if (!blanks.empty()) placeholders[*out] = {d, std::move(blanks)};
  return r;
}
void VKAPI_CALL destroy_framebuffer(VkDevice d, VkFramebuffer framebuffer, const VkAllocationCallbacks* alloc) {
  {
    // The guest destroys a framebuffer once no submitted work uses it.
    std::lock_guard lock(vrs_mutex);
    if (auto it = vrs_targets.find(framebuffer); it != vrs_targets.end()) {
      release_vrs_target(it->second.first, it->second.second);
      vrs_targets.erase(it);
    }
  }
  std::optional<Substitution> owned;
  std::vector<StandIn> blanks;
  {
    std::lock_guard lock(mutex);
    clamped.erase(framebuffer);
    if (auto it = placeholders.find(framebuffer); it != placeholders.end()) {
      blanks = std::move(it->second.second);
      placeholders.erase(it);
    }
    if (auto it = substitutions.find(framebuffer); it != substitutions.end()) {
      owned = std::move(it->second);
      substitutions.erase(it);
    }
  }
  next<PFN_vkDestroyFramebuffer, "vkDestroyFramebuffer">(d)(d, framebuffer, alloc);
  for (const auto& b : blanks) release(d, b);
  if (!owned) return;
  if (owned->plain) next<PFN_vkDestroyFramebuffer, "vkDestroyFramebuffer">(d)(d, owned->plain, nullptr);
  for (const auto& s : owned->stand_ins) release(d, s);
}
// SwiftShader: the host framebuffer for a render pass instance on a framebuffer
// with stand-ins, and the layout transitions before and after the instance.
struct HostBegin {
  VkFramebuffer framebuffer;
  std::vector<VkImageMemoryBarrier> before, after;
};
HostBegin host_begin(const VkRenderPassBeginInfo* info) {
  HostBegin out{info->framebuffer};
  VkDevice device{};
  VkFramebufferCreateInfo ci{};
  std::vector<VkImageView> attachments;
  {
    std::lock_guard lock(mutex);
    auto sub = substitutions.find(info->framebuffer);
    if (sub == substitutions.end()) return out;
    // A compatible render pass may load, store or read what the framebuffer's discarded.
    auto pass = passes.find(info->renderPass);
    bool keeps = pass == passes.end();
    for (const auto& s : sub->second.stand_ins)
      keeps = keeps || s.index >= pass->second.transient.size() || !pass->second.transient[s.index] ||
              pass->second.input[s.index];
    if (!keeps) {
      for (const auto& s : sub->second.stand_ins) {
        const auto initial = pass->second.initial_layouts[s.index];
        const auto final = pass->second.final_layouts[s.index];
        if (initial != VK_IMAGE_LAYOUT_UNDEFINED)
          out.before.push_back(layout_barrier(s.image, s.range, VK_IMAGE_LAYOUT_UNDEFINED, initial));
        // The guest's image keeps what lies outside the render area, as the instance would.
        if (initial != final) out.after.push_back(layout_barrier(s.guest, s.guest_range, initial, final));
      }
      return out;
    }
    if (sub->second.plain) {
      out.framebuffer = sub->second.plain;
      return out;
    }
    device = sub->second.device;
    ci = sub->second.info;
    attachments = sub->second.views;
  }
  ci.pAttachments = attachments.data();
  VkFramebuffer plain{};
  if (next<PFN_vkCreateFramebuffer, "vkCreateFramebuffer">(device)(device, &ci, nullptr, &plain) != VK_SUCCESS)
    return out;
  {
    std::lock_guard lock(mutex);
    if (auto sub = substitutions.find(info->framebuffer); sub != substitutions.end()) {
      if (!sub->second.plain) {
        out.framebuffer = sub->second.plain = plain;
        return out;
      }
      out.framebuffer = sub->second.plain;
    }
  }
  next<PFN_vkDestroyFramebuffer, "vkDestroyFramebuffer">(device)(device, plain, nullptr);
  return out;
}
// The render area inside the host framebuffer and, for imageless framebuffers,
// the views supplied at begin.
VkRect2D pass_area(const VkRenderPassBeginInfo* info) {
  VkExtent2D bounds{~0u, ~0u};
  std::lock_guard lock(mutex);
  if (auto it = clamped.find(info->framebuffer); it != clamped.end()) bounds = it->second;
  if (auto* begin = find_chain<VkRenderPassAttachmentBeginInfo>(info->pNext,
                                                                VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO))
    for (std::uint32_t i = 0; i < begin->attachmentCount; ++i)
      if (used(info->renderPass, i)) narrow(bounds, begin->pAttachments[i]);
  return clamp(info->renderArea, {{0, 0}, bounds});
}
VkRect2D rendering_area(const VkRenderingInfo* info) {
  VkExtent2D bounds{~0u, ~0u};
  std::lock_guard lock(mutex);
  for (std::uint32_t i = 0; i < info->colorAttachmentCount; ++i) {
    narrow(bounds, info->pColorAttachments[i].imageView);
    if (info->pColorAttachments[i].resolveMode != VK_RESOLVE_MODE_NONE)
      narrow(bounds, info->pColorAttachments[i].resolveImageView);
  }
  for (const auto* a : {info->pDepthAttachment, info->pStencilAttachment})
    if (a) {
      narrow(bounds, a->imageView);
      if (a->resolveMode != VK_RESOLVE_MODE_NONE) narrow(bounds, a->resolveImageView);
    }
  return clamp(info->renderArea, {{0, 0}, bounds});
}
template <class Info>
Info with_area(const Info& info, const VkRect2D& area) {
  if (!same(area, info.renderArea))
    report_clamp("Render area", info.renderArea.extent.width, info.renderArea.extent.height, area.extent);
  auto copy = info;
  copy.renderArea = area;
  return copy;
}

void VKAPI_CALL set_scissor(VkCommandBuffer cmd, std::uint32_t first, std::uint32_t count, const VkRect2D* rects) {
  Forward f;
  {
    std::lock_guard lock(mutex);
    auto& s = states[cmd];
    if (s.with_count) s.rects.clear();
    s.with_count = false;
    if (s.rects.size() < first + count) s.rects.resize(first + count);
    std::copy_n(rects, count, s.rects.begin() + first);
    f = forwarded(s, first, count);
  }
  forward(cmd, f);
}
void VKAPI_CALL set_scissor_with_count(VkCommandBuffer cmd, std::uint32_t count, const VkRect2D* rects) {
  Forward f;
  {
    std::lock_guard lock(mutex);
    auto& s = states[cmd];
    s.with_count = true;
    s.rects.assign(rects, rects + count);
    f = forwarded(s, 0, count);
  }
  forward(cmd, f);
}
void VKAPI_CALL begin_pass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
  const auto area = pass_area(info);
  auto begin = host_begin(info);
  transition(cmd, begin.before);
  prepare_vrs(cmd, info->framebuffer);
  auto copy = with_area(*info, area);
  copy.framebuffer = begin.framebuffer;
  host<PFN_vkCmdBeginRenderPass>(cmd, "vkCmdBeginRenderPass")(cmd, &copy, contents);
  begin_area(cmd, area, inline_subpass(contents), std::move(begin.after));
}
void VKAPI_CALL begin_pass2(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info, const VkSubpassBeginInfo* sub) {
  const auto area = pass_area(info);
  auto begin = host_begin(info);
  transition(cmd, begin.before);
  prepare_vrs(cmd, info->framebuffer);
  auto copy = with_area(*info, area);
  copy.framebuffer = begin.framebuffer;
  host<PFN_vkCmdBeginRenderPass2>(cmd, "vkCmdBeginRenderPass2")(cmd, &copy, sub);
  begin_area(cmd, area, inline_subpass(sub->contents), std::move(begin.after));
}
void VKAPI_CALL begin_rendering(VkCommandBuffer cmd, const VkRenderingInfo* info) {
  const auto area = rendering_area(info);
  const auto copy = with_area(*info, area);
  host<PFN_vkCmdBeginRendering>(cmd, "vkCmdBeginRendering")(cmd, &copy);
  // VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT does not restrict commands.
  begin_area(cmd, area, true);
}
bool bakes_scissor(const VkGraphicsPipelineCreateInfo& info) {
  // Libraries and rasterizer-discard pipelines leave the scissor state to others.
  if ((info.flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) || !info.pViewportState ||
      find_chain<VkPipelineLibraryCreateInfoKHR>(info.pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR))
    return false;
  if (info.pDynamicState)
    for (std::uint32_t i = 0; i < info.pDynamicState->dynamicStateCount; ++i) {
      const auto state = info.pDynamicState->pDynamicStates[i];
      if (state == VK_DYNAMIC_STATE_SCISSOR || state == VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT) return false;
    }
  return true;
}
// A baked scissor cannot follow the render area: an eye-sized scissor on a
// pipeline reused for an 848x1060 panel (the Model 2 ports' menu) rasterizes
// past the panel's depth buffer, which a Quest's tiler never touches and NVIDIA
// faults ("ZT Violation", then VK_ERROR_DEVICE_LOST). The host pipeline makes
// the scissor dynamic; binding it sets the baked rects, clamped in a render pass.
bool converts_scissor(const VkGraphicsPipelineCreateInfo& info) {
  if (!bakes_scissor(info) || !info.pViewportState->pScissors || !info.pViewportState->scissorCount) return false;
  if (info.pDynamicState)
    for (std::uint32_t i = 0; i < info.pDynamicState->dynamicStateCount; ++i)
      if (info.pDynamicState->pDynamicStates[i] == VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT) return false;
  return true;
}
VkResult VKAPI_CALL create_graphics_pipelines(VkDevice d, VkPipelineCache cache, std::uint32_t count,
                                              const VkGraphicsPipelineCreateInfo* infos,
                                              const VkAllocationCallbacks* alloc, VkPipeline* out) {
  std::vector<VkGraphicsPipelineCreateInfo> copies;
  std::vector<std::vector<VkDynamicState>> states;
  std::vector<VkPipelineDynamicStateCreateInfo> dynamic;
  for (std::uint32_t i = 0; i < count; ++i)
    if (converts_scissor(infos[i])) {
      copies.assign(infos, infos + count);
      states.resize(count);
      dynamic.resize(count);
      break;
    }
  for (std::uint32_t i = 0; i < copies.size(); ++i) {
    if (!converts_scissor(infos[i])) continue;
    if (const auto* given = infos[i].pDynamicState) {
      states[i].assign(given->pDynamicStates, given->pDynamicStates + given->dynamicStateCount);
      dynamic[i] = *given;
    } else {
      dynamic[i] = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    }
    states[i].push_back(VK_DYNAMIC_STATE_SCISSOR);
    dynamic[i].dynamicStateCount = static_cast<std::uint32_t>(states[i].size());
    dynamic[i].pDynamicStates = states[i].data();
    copies[i].pDynamicState = &dynamic[i];
  }
  // VENTURE_VRS: pipelines take the shading-rate map's rate (1x1 where none is bound).
  std::vector<VkPipelineFragmentShadingRateStateCreateInfoKHR> rate_states;
  if (vrs_texel(d).width) {
    if (copies.empty()) copies.assign(infos, infos + count);
    rate_states.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      if (find_chain<VkPipelineFragmentShadingRateStateCreateInfoKHR>(
              copies[i].pNext, VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR) ||
          find_chain<VkPipelineRenderingCreateInfo>(copies[i].pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO))
        continue;
      rate_states[i] = {VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR, copies[i].pNext,
                        {1, 1},
                        {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR}};
      copies[i].pNext = &rate_states[i];
    }
  }
  auto r = next<PFN_vkCreateGraphicsPipelines, "vkCreateGraphicsPipelines">(d)(
      d, cache, count, copies.empty() ? infos : copies.data(), alloc, out);
  std::lock_guard lock(mutex);
  for (std::uint32_t i = 0; i < count; ++i)
    if (out[i] && converts_scissor(infos[i])) {
      const auto& viewport = *infos[i].pViewportState;
      static_scissor[out[i]].assign(viewport.pScissors, viewport.pScissors + viewport.scissorCount);
    }
  return r;
}
void VKAPI_CALL destroy_pipeline(VkDevice d, VkPipeline pipeline, const VkAllocationCallbacks* alloc) {
  {
    std::lock_guard lock(mutex);
    static_scissor.erase(pipeline);
  }
  next<PFN_vkDestroyPipeline, "vkDestroyPipeline">(d)(d, pipeline, alloc);
}
void VKAPI_CALL bind_pipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline) {
  std::optional<Forward> f;
  if (point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
    std::lock_guard lock(mutex);
    if (auto it = static_scissor.find(pipeline); it != static_scissor.end()) {
      // The baked rects become this command buffer's scissors, as if set.
      auto& s = states[cmd];
      s.with_count = false;
      s.rects = it->second;
      s.host.clear();
      f = forwarded(s, 0, static_cast<std::uint32_t>(s.rects.size()));
    }
  }
  next<PFN_vkCmdBindPipeline, "vkCmdBindPipeline">(command_device(cmd))(cmd, point, pipeline);
  if (f) forward(cmd, *f);
}
void VKAPI_CALL end_pass(VkCommandBuffer cmd) {
  const auto after = end_area(cmd);
  host<PFN_vkCmdEndRenderPass>(cmd, "vkCmdEndRenderPass")(cmd);
  transition(cmd, after);
}
void VKAPI_CALL end_pass2(VkCommandBuffer cmd, const VkSubpassEndInfo* info) {
  const auto after = end_area(cmd);
  host<PFN_vkCmdEndRenderPass2>(cmd, "vkCmdEndRenderPass2")(cmd, info);
  transition(cmd, after);
}
void VKAPI_CALL end_rendering(VkCommandBuffer cmd) {
  end_area(cmd);
  host<PFN_vkCmdEndRendering>(cmd, "vkCmdEndRendering")(cmd);
}
}  // namespace

bool vrs_requested() {
  static const bool on = [] {
    const char* text = venture::environment::getenv("VENTURE_VRS");
    return text && *text == '1';
  }();
  return on;
}
std::atomic<float> local_nr_width{0.f}, local_nr_height{0.f};
void fovea_center(std::uint32_t eye, float& u, float& v) {
  std::lock_guard lock(vrs_mutex);
  u = vrs_centers[eye & 1][0];
  v = vrs_centers[eye & 1][1];
}
void note_local_nr_area(float width, float height) {
  local_nr_width = width;
  local_nr_height = height;
}
bool vrs_outline(float& width, float& height) {
  if (!vrs_requested()) return false;
  std::lock_guard lock(vrs_mutex);
  const auto& live = vrs_settings();
  if (!live.on || !live.debug || vrs_targets.empty()) return false;
  width = std::max(live.width, vrs_follow[0]);
  height = std::max(live.height, vrs_follow[1]);
  return true;
}
bool vrs_active() {
  if (!vrs_requested()) return false;
  std::lock_guard lock(vrs_mutex);
  return vrs_settings().on && !vrs_targets.empty();
}
void vrs_device_created(VkDevice device, VkExtent2D texel_size) {
  std::lock_guard lock(vrs_mutex);
  vrs_devices[device] = texel_size;
  log::info("vulkan", "VRS: attachment shading rates enabled ({}x{} pixel texels)", texel_size.width,
            texel_size.height);
}
void vrs_note_views(const float tangents[2][4], float half_ipd, const float* gaze) {
  // The launcher's live values, a few times a second.
  static std::chrono::steady_clock::time_point next_read{};
  const auto now = std::chrono::steady_clock::now();
  if (now >= next_read) {
    next_read = now + std::chrono::milliseconds(250);
    VrsLive current;
    {
      std::lock_guard lock(vrs_mutex);
      current = vrs_settings();
    }
    const VrsLive fresh = read_vrs_live(current);
    if (!(fresh == current)) {
      std::lock_guard lock(vrs_mutex);
      vrs_settings() = fresh;
      ++vrs_centers_version;
      log::info("vulkan", "VRS live: {} , full rate {:.0f}% x {:.0f}%, outside {}", fresh.on ? "on" : "off",
                100 * fresh.width, 100 * fresh.height, fresh.rate4 ? "4x4" : "2x2");
    }
  }
  VrsLive live;
  {
    std::lock_guard lock(vrs_mutex);
    live = vrs_settings();
  }
  const float distance = live.distance;
  const auto hold = std::chrono::milliseconds(live.gaze_hold_ms);
  // VDXR's NR region when it follows the gaze; else Venture's own gaze, smoothed (held for a blink);
  // else straight ahead. Both look at a point `distance` away (infinity when 0), seen from each eye.
  const auto fovea = read_vdxr_fovea();
  struct Direction { float x, y, z; };
  static Direction smoothed{0.f, 0.f, -1.f};
  static std::chrono::steady_clock::time_point last_gaze{};
  bool own_gaze = false;
  if (!fovea && gaze) {
    const float length = std::max(1e-6f, std::sqrt(gaze[0] * gaze[0] + gaze[1] * gaze[1] + gaze[2] * gaze[2]));
    const Direction d{gaze[0] / length, gaze[1] / length, gaze[2] / length};
    const bool fresh = now - last_gaze > std::max(hold, std::chrono::milliseconds(50));
    const float follow = fresh ? 1.f : 1.f - live.gaze_smoothing;
    smoothed = {smoothed.x + (d.x - smoothed.x) * follow, smoothed.y + (d.y - smoothed.y) * follow,
                smoothed.z + (d.z - smoothed.z) * follow};
    last_gaze = now;
  }
  own_gaze = !fovea && smoothed.z < -0.1f && now - last_gaze < std::max(hold, std::chrono::milliseconds(50));
  float centers[2][2];
  for (std::uint32_t eye = 0; eye < 2; ++eye) {
    const auto* t = tangents[eye];
    if (!(t[1] > t[0]) || !(t[2] > t[3])) return;
    const float eye_x = eye == 0 ? -half_ipd : half_ipd;
    float x = distance > 0 ? -eye_x / distance : 0.f, y = 0.f;
    if (fovea) {
      x = fovea->tangent[eye][0];
      y = fovea->tangent[eye][1];
    } else if (own_gaze) {
      const float ahead = -smoothed.z;
      x = distance > 0 ? (smoothed.x / ahead * distance - eye_x) / distance : smoothed.x / ahead;
      y = smoothed.y / ahead;
    }
    centers[eye][0] = std::clamp((x - t[0]) / (t[1] - t[0]), 0.f, 1.f);
    centers[eye][1] = std::clamp((t[2] - y) / (t[2] - t[3]), 0.f, 1.f);
  }
  // Following, the area covers the NR region plus a margin: the gaze arrives one frame late.
  // Venture NR (xr/venture_nr.h) on the same center: VRS covers its region plus the margin too.
  const float local_w = local_nr_width.load(), local_h = local_nr_height.load();
  const float follow_w = fovea ? std::min(1.f, fovea->nrWidth + live.margin)
                         : local_w > 0.f ? std::min(1.f, local_w + live.margin) : 0.f;
  const float follow_h = fovea ? std::min(1.f, fovea->nrHeight + live.margin)
                         : local_h > 0.f ? std::min(1.f, local_h + live.margin) : 0.f;
  std::lock_guard lock(vrs_mutex);
  bool changed = std::abs(follow_w - vrs_follow[0]) > 0.005f || std::abs(follow_h - vrs_follow[1]) > 0.005f;
  for (std::uint32_t eye = 0; eye < 2; ++eye)
    for (std::uint32_t axis = 0; axis < 2; ++axis)
      changed |= std::abs(centers[eye][axis] - vrs_centers[eye][axis]) > 0.005f;
  if (!changed) return;
  static int reported_mode = -1;
  const int mode = fovea ? 2 : own_gaze ? 1 : 0;
  if (mode != reported_mode) {
    reported_mode = mode;
    log::info("vulkan", "VRS: {}",
              mode == 2 ? "follows VDXR's gaze-tracked NR region" : mode == 1 ? "follows the eye gaze" : "fixed center");
  }
  std::memcpy(vrs_centers, centers, sizeof(centers));
  vrs_follow[0] = follow_w;
  vrs_follow[1] = follow_h;
  ++vrs_centers_version;
}
void note_image(VkImage image, const VkImageCreateInfo& info) {
  // Only images that can be attachments ever bound a framebuffer.
  if (!(info.usage & attachment_usage)) return;
  std::lock_guard lock(mutex);
  images[image] = {{info.extent.width, info.extent.height}, info.arrayLayers, info.samples, info.usage};
}
void forget_image(VkImage image) {
  std::lock_guard lock(mutex);
  images.erase(image);
}
VkExtent2D attachment_extent(VkImage image) {
  std::lock_guard lock(mutex);
  auto it = images.find(image);
  return it == images.end() ? VkExtent2D{} : it->second.extent;
}
void release_render_commands(VkCommandBuffer command) {
  std::lock_guard lock(mutex);
  states.erase(command);
}
void force_resolve_stand_ins(std::optional<bool> enabled) {
  std::lock_guard lock(mutex);
  forced_stand_ins = enabled;
}
PFN_vkVoidFunction render_area_proc(std::string_view name) {
#define FN(command, function) \
  if (name == #command) return reinterpret_cast<PFN_vkVoidFunction>(function)
  FN(vkCreateImage, create_image);
  FN(vkDestroyImage, destroy_image);
  FN(vkCreateImageView, create_view);
  FN(vkDestroyImageView, destroy_view);
  FN(vkCreateRenderPass, create_render_pass);
  FN(vkCreateRenderPass2, create_render_pass2<"vkCreateRenderPass2">);
  FN(vkCreateRenderPass2KHR, create_render_pass2<"vkCreateRenderPass2KHR">);
  FN(vkDestroyRenderPass, destroy_render_pass);
  FN(vkCreateFramebuffer, create_framebuffer);
  FN(vkDestroyFramebuffer, destroy_framebuffer);
  FN(vkCreateGraphicsPipelines, create_graphics_pipelines);
  FN(vkDestroyPipeline, destroy_pipeline);
  FN(vkCmdBindPipeline, bind_pipeline);
  FN(vkCmdSetScissor, set_scissor);
  FN(vkCmdSetScissorWithCount, set_scissor_with_count);
  FN(vkCmdBeginRenderPass, begin_pass);
  FN(vkCmdBeginRenderPass2, begin_pass2);
  FN(vkCmdBeginRendering, begin_rendering);
  FN(vkCmdEndRenderPass, end_pass);
  FN(vkCmdEndRenderPass2, end_pass2);
  FN(vkCmdEndRendering, end_rendering);
#undef FN
  return nullptr;
}
}  // namespace venture::vulkan
