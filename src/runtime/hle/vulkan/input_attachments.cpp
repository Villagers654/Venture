// Subpass inputs read as sampled images.
//
// AMD's Windows driver reads a multiview subpass input from the first view's
// layer in every view: the H3VR2 eye pass resolves its second subpass from
// subpassLoad of the first subpass's color, and the right eye came out a copy
// of the left (diagnostic build 5: both views render correctly, and reading the
// same attachment as a sampled image at layer ViewIndex gives each eye its own
// image). On that driver every subpass input is read that way:
//
// - fragment shaders: each subpass input image type becomes a sampled 2D array
//   image, and each subpassLoad (OpImageRead) becomes a texel fetch at the
//   fragment's pixel from layer ViewIndex, keeping the sample operand of
//   multisampled inputs; ViewIndex is 0 outside multiview (sample_subpass_inputs);
// - descriptors: input attachment descriptors become sampled image descriptors
//   in set layouts, pools, writes, templates and pushes; a 2D view written to
//   one is replaced by a one-layer 2D array view of the same subresource
//   (render_area.cpp, texture.cpp);
// - images with input attachment usage also get sampled usage and lose
//   transient usage; dependencies into input attachment reads also cover
//   fragment shader reads (render_area.cpp).
//
// The input attachment references stay in the subpasses for their layouts.
// VENTURE_VULKAN_SAMPLED_INPUTS=1 turns this on for any driver, =0 off.
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "common/environment.h"
#include "common/log.h"
#include "internal.h"

namespace venture::vulkan {
namespace {
std::atomic<bool> active{false};
}
bool sampled_input_reads() { return active.load(std::memory_order_relaxed); }
void set_sampled_input_reads(bool on) { active.store(on, std::memory_order_relaxed); }
bool sampled_input_driver(std::uint32_t driver_id, bool multiview) {
  const auto* value = venture::environment::getenv("VENTURE_VULKAN_SAMPLED_INPUTS");
  if (value && std::string_view(value) == "0") return false;
  if (value && std::string_view(value) == "1") return true;
  return multiview && driver_id == VK_DRIVER_ID_AMD_PROPRIETARY;
}
void choose_sampled_input_reads(VkPhysicalDevice physical, const VkDeviceCreateInfo& info) {
  // Without the multiview feature no render pass has views, and the shaders
  // could not declare ViewIndex.
  bool multiview = false;
  for (auto* p = static_cast<const VkBaseInStructure*>(info.pNext); p; p = p->pNext) {
    if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES)
      multiview |= reinterpret_cast<const VkPhysicalDeviceMultiviewFeatures*>(p)->multiview != VK_FALSE;
    if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES)
      multiview |= reinterpret_cast<const VkPhysicalDeviceVulkan11Features*>(p)->multiview != VK_FALSE;
  }
  VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
  if (auto get = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
          raw_physical_proc(physical, "vkGetPhysicalDeviceProperties2")))
    get(physical, &properties);
  const bool on = sampled_input_driver(driver.driverID, multiview);
  if (on && !multiview) {
    log::warn("vulkan", "Subpass inputs stay input attachments: the device has no multiview feature to read them by view");
    return;
  }
  set_sampled_input_reads(on);
  if (on) log::info("vulkan", "Subpass inputs are read as sampled images, one array layer per view (driver {})",
                    int(driver.driverID));
}
VkDescriptorType sampled_input_type(VkDescriptorType type) {
  return type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT && sampled_input_reads() ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                                                 : type;
}
std::vector<std::uint32_t> sample_subpass_inputs(std::span<const std::uint32_t> code) {
  enum : std::uint32_t {
    OpExtension = 10, OpEntryPoint = 15, OpCapability = 17, OpTypeVoid = 19, OpTypeInt = 21, OpTypeFloat = 22,
    OpTypeVector = 23, OpTypeImage = 25, OpTypePointer = 32, OpConstant = 43, OpFunction = 54, OpVariable = 59,
    OpLoad = 61, OpDecorate = 71, OpVectorShuffle = 79, OpCompositeConstruct = 80, OpImageFetch = 95,
    OpImageRead = 98, OpConvertFToS = 110, OpBitcast = 124
  };
  constexpr std::uint32_t Input = 1, BuiltIn = 11, Flat = 14, InputAttachmentIndex = 43, FragCoord = 15,
                          ViewIndex = 4440, MultiView = 4439, Fragment = 4, LodOperand = 0x2;
  if (code.size() < 5 || code[0] != 0x07230203) return {};
  std::unordered_map<std::uint32_t, std::size_t> subpass_types;  // image type -> its word offset
  std::unordered_map<std::uint32_t, std::uint32_t> subpass_values;  // loaded image -> its type
  std::unordered_map<std::uint32_t, std::uint32_t> var_type;  // variable -> pointer type
  std::unordered_map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> pointers;  // (storage, pointee)
  std::unordered_map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> vector_of;  // (component, count)
  std::uint32_t int_t = 0, float_t = 0, v4f = 0, v2f = 0, v2i = 0, v3i = 0;
  std::uint32_t frag_coord = 0, view_index = 0, zero = 0;
  bool has_capability = false;
  std::size_t first_type = 0, first_function = 0, after_capabilities = 0;
  for (std::size_t i = 5; i < code.size();) {
    const auto n = code[i] >> 16, op = code[i] & 0xffff;
    if (!n || i + n > code.size()) return {};
    if (op == OpCapability && n == 2) {
      after_capabilities = i + n;
      has_capability |= code[i + 1] == MultiView;
    }
    if (op == OpTypeImage && n >= 9 && code[i + 3] == 6) subpass_types[code[i + 1]] = i;
    if (op == OpTypeInt && n == 4 && code[i + 2] == 32 && code[i + 3] && !int_t) int_t = code[i + 1];
    if (op == OpTypeFloat && n == 3 && code[i + 2] == 32 && !float_t) float_t = code[i + 1];
    if (op == OpTypeVector && n == 4) vector_of[code[i + 1]] = {code[i + 2], code[i + 3]};
    if (op == OpTypePointer && n == 4) pointers[code[i + 1]] = {code[i + 2], code[i + 3]};
    if (op == OpVariable && n >= 4) var_type[code[i + 2]] = code[i + 1];
    if (op == OpConstant && n == 4 && int_t && code[i + 1] == int_t && code[i + 3] == 0 && !zero) zero = code[i + 2];
    if (op == OpDecorate && n == 4 && code[i + 2] == BuiltIn) {
      if (code[i + 3] == FragCoord) frag_coord = code[i + 1];
      if (code[i + 3] == ViewIndex) view_index = code[i + 1];
    }
    if (op == OpLoad && n >= 4 && subpass_types.contains(code[i + 1])) subpass_values[code[i + 2]] = code[i + 1];
    if (!first_type && op >= OpTypeVoid && op <= 39) first_type = i;
    if (!first_function && op == OpFunction) first_function = i;
    i += n;
  }
  if (subpass_types.empty() || !first_type || !first_function || !after_capabilities) return {};
  // A converted image type must not duplicate one the module declares.
  for (std::size_t i = 5; i < code.size(); i += code[i] >> 16)
    if ((code[i] & 0xffff) == OpTypeImage && (code[i] >> 16) >= 9 && code[i + 3] == 1 && code[i + 5] == 1 &&
        code[i + 7] == 1)
      for (const auto& [id, at] : subpass_types)
        if (code[at + 2] == code[i + 2] && code[at + 4] == code[i + 4] && code[at + 6] == code[i + 6] &&
            code[at + 8] == code[i + 8])
          return {};
  // FragCoord is a float vector: an int scalar of 32 bits is what the module has, or added.
  for (const auto& [id, of] : vector_of) {
    if (float_t && of.first == float_t && of.second == 4) v4f = id;
    if (float_t && of.first == float_t && of.second == 2) v2f = id;
    if (int_t && of.first == int_t && of.second == 2) v2i = id;
    if (int_t && of.first == int_t && of.second == 3) v3i = id;
  }
  // A signed int is needed for the coordinate; an existing unsigned int is not used.
  if (int_t) {
    for (std::size_t i = 5; i < code.size(); i += code[i] >> 16)
      if ((code[i] & 0xffff) == OpTypeInt && code[i + 1] == int_t && code[i + 3] != 1) return {};
  }
  std::uint32_t bound = code[3];
  std::vector<std::uint32_t> types;  // inserted before the first function
  std::vector<std::uint32_t> decorations;  // inserted before the first type
  std::vector<std::uint32_t> interface;
  if (!int_t) { int_t = bound++; types.insert(types.end(), {(4u << 16) | OpTypeInt, int_t, 32, 1}); }
  if (!float_t) { float_t = bound++; types.insert(types.end(), {(3u << 16) | OpTypeFloat, float_t, 32}); }
  auto vector = [&](std::uint32_t& id, std::uint32_t component, std::uint32_t count) {
    if (!id) { id = bound++; types.insert(types.end(), {(4u << 16) | OpTypeVector, id, component, count}); }
  };
  vector(v4f, float_t, 4); vector(v2f, float_t, 2); vector(v2i, int_t, 2); vector(v3i, int_t, 3);
  if (!zero) { zero = bound++; types.insert(types.end(), {(4u << 16) | OpConstant, int_t, zero, 0}); }
  auto input_pointer = [&](std::uint32_t pointee) {
    for (const auto& [id, p] : pointers) if (p.first == Input && p.second == pointee) return id;
    const auto id = bound++;
    types.insert(types.end(), {(4u << 16) | OpTypePointer, id, Input, pointee});
    pointers[id] = {Input, pointee};
    return id;
  };
  auto builtin = [&](std::uint32_t& var, std::uint32_t pointee, std::uint32_t which, bool flat) {
    if (var) return false;
    const auto pointer = input_pointer(pointee);
    var = bound++;
    types.insert(types.end(), {(4u << 16) | OpVariable, pointer, var, Input});
    var_type[var] = pointer;
    decorations.insert(decorations.end(), {(4u << 16) | OpDecorate, var, BuiltIn, which});
    if (flat) decorations.insert(decorations.end(), {(3u << 16) | OpDecorate, var, Flat});
    return true;
  };
  builtin(frag_coord, v4f, FragCoord, false);
  const bool added_view = builtin(view_index, int_t, ViewIndex, true);
  interface = {frag_coord, view_index};
  const std::uint32_t view_type = pointers[var_type[view_index]].second;
  const std::uint32_t coord_type = pointers[var_type[frag_coord]].second;
  if (coord_type != v4f) return {};

  std::vector<std::uint32_t> out(code.begin(), code.begin() + 5);
  out.reserve(code.size() + types.size() + decorations.size() + 64);
  for (std::size_t i = 5; i < code.size();) {
    const auto n = code[i] >> 16, op = code[i] & 0xffff;
    if (i == after_capabilities && added_view) {
      if (!has_capability) out.insert(out.end(), {(2u << 16) | OpCapability, MultiView});
      if (code[1] < 0x00010300) {
        const char name[] = "SPV_KHR_multiview";
        std::vector<std::uint32_t> words((sizeof(name) + 3) / 4, 0);
        std::memcpy(words.data(), name, sizeof(name));
        out.push_back(std::uint32_t((1 + words.size()) << 16) | OpExtension);
        out.insert(out.end(), words.begin(), words.end());
      }
    }
    if (i == first_type) out.insert(out.end(), decorations.begin(), decorations.end());
    if (i == first_function) out.insert(out.end(), types.begin(), types.end());
    if (op == OpEntryPoint && n >= 3 && code[i + 1] == Fragment) {
      // The name ends at the word holding a zero byte; the interface follows.
      std::size_t w = i + 3;
      while (w < i + n) {
        const auto word = code[w++];
        if (!(word & 0xff) || !(word & 0xff00) || !(word & 0xff0000) || !(word & 0xff000000)) break;
      }
      std::vector<std::uint32_t> add;
      for (auto v : interface)
        if (std::find(code.begin() + w, code.begin() + i + n, v) == code.begin() + i + n) add.push_back(v);
      out.push_back(std::uint32_t((n + add.size()) << 16) | OpEntryPoint);
      out.insert(out.end(), code.begin() + i + 1, code.begin() + i + n);
      out.insert(out.end(), add.begin(), add.end());
    } else if (op == OpTypeImage && subpass_types.contains(code[i + 1])) {
      out.insert(out.end(), code.begin() + i, code.begin() + i + n);
      const auto at = out.size() - n;
      out[at + 3] = 1;  // Dim 2D
      out[at + 5] = 1;  // Arrayed
      out[at + 7] = 1;  // Sampled
    } else if (op == OpDecorate && n >= 3 && code[i + 2] == InputAttachmentIndex) {
      // Valid only on subpass inputs.
    } else if (op == OpImageRead && n >= 4 && subpass_values.contains(code[i + 3])) {
      const bool multisampled = code[subpass_types[subpass_values[code[i + 3]]] + 6] != 0;
      const auto fc = bound++, xy = bound++, ixy = bound++, vi = bound++, coordinate = bound++;
      out.insert(out.end(), {(4u << 16) | OpLoad, v4f, fc, frag_coord});
      out.insert(out.end(), {(7u << 16) | OpVectorShuffle, v2f, xy, fc, fc, 0, 1});
      out.insert(out.end(), {(4u << 16) | OpConvertFToS, v2i, ixy, xy});
      out.insert(out.end(), {(4u << 16) | OpLoad, view_type, vi, view_index});
      std::uint32_t view = vi;
      if (view_type != int_t) {
        view = bound++;
        out.insert(out.end(), {(4u << 16) | OpBitcast, int_t, view, vi});
      }
      out.insert(out.end(), {(5u << 16) | OpCompositeConstruct, v3i, coordinate, ixy, view});
      // A multisampled read keeps its Sample operand; a single-sampled fetch names level 0.
      if (multisampled) {
        out.push_back(std::uint32_t(n << 16) | OpImageFetch);
        out.insert(out.end(), {code[i + 1], code[i + 2], code[i + 3], coordinate});
        out.insert(out.end(), code.begin() + i + 5, code.begin() + i + n);
      } else {
        out.insert(out.end(),
                   {(7u << 16) | OpImageFetch, code[i + 1], code[i + 2], code[i + 3], coordinate, LodOperand, zero});
      }
    } else {
      out.insert(out.end(), code.begin() + i, code.begin() + i + n);
    }
    i += n;
  }
  out[3] = bound;
  return out;
}
}  // namespace venture::vulkan
