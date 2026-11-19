// Subpass inputs read as sampled images.
//
// Some drivers read a multiview subpass input from the first view's layer in
// every view. On AMD's Windows driver the H3VR2 eye pass resolves its second
// subpass from subpassLoad of the first subpass's color, and the right eye
// came out a copy of the left (diagnostic build 5: both views render
// correctly, and reading the same attachment as a sampled image at layer
// ViewIndex gives each eye its own image); an Intel Arc on Windows showed H3VR2
// in the left eye only. Which drivers do this is not a vendor list: at device
// creation a two-view render reads back what view 1's subpassLoad got
// (subpass_input_probe.cpp), once per driver, the result kept in Venture's
// cache folder. Where a view reads another view's layer, every subpass input
// is read as a sampled image:
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
// When the probe cannot run, AMD's Windows driver (where the bug was found)
// reads sampled inputs and other drivers do not.
// VENTURE_VULKAN_SAMPLED_INPUTS=1 turns this on for any driver, =0 off; either
// skips the probe.
#include <algorithm>
#include <cstring>
#include <format>
#include <unordered_map>
#include <unordered_set>

#include "common/environment.h"
#include "common/log.h"
#include "common/paths.h"
#include "internal.h"

namespace venture::vulkan {
namespace {
std::atomic<bool> active{false};
}
bool sampled_input_reads() { return active.load(std::memory_order_relaxed); }
void set_sampled_input_reads(bool on) { active.store(on, std::memory_order_relaxed); }
std::optional<bool> sampled_input_override() {
  const auto* value = venture::environment::getenv("VENTURE_VULKAN_SAMPLED_INPUTS");
  if (value && std::string_view(value) == "0") return false;
  if (value && std::string_view(value) == "1") return true;
  return std::nullopt;
}
bool sampled_input_driver(std::uint32_t driver_id, bool multiview, SubpassInputProbe probe) {
  if (const auto forced = sampled_input_override()) return *forced;
  if (!multiview) return false;
  if (probe != SubpassInputProbe::unavailable) return probe == SubpassInputProbe::other_layer;
  // Without a result, take the reads that are right on every driver.
  (void)driver_id;
  return true;
}
std::filesystem::path gpu_probe_cache_directory() {
  try {
    return paths::data_dir() / "cache" / "gpu-probes";
  } catch (...) {
    return {};
  }
}
void choose_sampled_input_reads(VkPhysicalDevice physical, VkDevice device, const VkDeviceCreateInfo& info,
                                const std::filesystem::path& cache_directory) try {
  // Without the multiview feature no render pass has views, and the shaders
  // could not declare ViewIndex.
  bool multiview = false;
  for (auto* p = static_cast<const VkBaseInStructure*>(info.pNext); p; p = p->pNext) {
    if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES)
      multiview |= reinterpret_cast<const VkPhysicalDeviceMultiviewFeatures*>(p)->multiview != VK_FALSE;
    if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES)
      multiview |= reinterpret_cast<const VkPhysicalDeviceVulkan11Features*>(p)->multiview != VK_FALSE;
  }
  if (const auto forced = sampled_input_override()) {
    if (*forced && !multiview) {
      log::warn("vulkan", "Subpass inputs stay input attachments: the device has no multiview feature to read them by view");
      return;
    }
    set_sampled_input_reads(*forced);
    if (*forced)
      log::info("vulkan", "Subpass inputs are read as sampled images, one array layer per view "
                          "(VENTURE_VULKAN_SAMPLED_INPUTS=1)");
    return;
  }
  if (!multiview) {
    set_sampled_input_reads(false);
    return;
  }
  VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
  if (auto get = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
          raw_physical_proc(physical, "vkGetPhysicalDeviceProperties2")))
    get(physical, &properties);
  const auto& p = properties.properties;
  // Once per driver: a result in the cache folder stands until the driver changes.
  const auto file = cache_directory.empty()
      ? std::filesystem::path{}
      : subpass_input_probe_file(cache_directory, p.vendorID, p.deviceID, p.driverVersion);
  auto probe = file.empty() ? SubpassInputProbe::unavailable : cached_subpass_input_probe(file);
  const bool cached = probe != SubpassInputProbe::unavailable;
  double milliseconds = 0;
  if (!cached) {
    probe = probe_subpass_inputs(physical, device, std::span(info.pQueueCreateInfos, info.queueCreateInfoCount),
                                 &milliseconds);
    if (!file.empty()) cache_subpass_input_probe(file, probe);
  }
  const bool on = sampled_input_driver(driver.driverID, multiview, probe);
  set_sampled_input_reads(on);
  const auto gpu = std::format("{} ({} {}, vendor {:#06x} device {:#06x} driver version {:#x})", p.deviceName,
                               driver.driverName, driver.driverInfo, p.vendorID, p.deviceID, p.driverVersion);
  const char* outcome = on ? "subpass inputs are read as sampled images, one array layer per view"
                           : "subpass inputs stay input attachments";
  if (probe == SubpassInputProbe::unavailable)
    log::warn("vulkan", "Multiview subpass input probe could not run on {}; {}", gpu, outcome);
  else
    log::info("vulkan", "Multiview subpass input probe ({}): view 1 read {} on {}; {}",
              cached ? std::string("cached") : std::format("{:.1f} ms", milliseconds),
              probe == SubpassInputProbe::own_layer ? "its own layer" : "another view's layer", gpu, outcome);
} catch (...) {
  // Never fails device creation; the choice stays as it was.
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
// A subpassInputMS whose attachment has one sample: Adreno reads its only
// sample, but the SPIR-V is invalid for that attachment, and once it is read as
// a sampled image (above) a multisampled fetch goes to a single-sampled image
// descriptor (VUID-RuntimeSpirv-samples-08726): undefined, and on AMD's
// hardware a multisampled load also reads the descriptor's multisample
// metadata. UE4's mobile depth fetch (GENERATED_SubpassDepthFetchAttachment in
// Moss) is declared multisampled and bound to a one-sample depth attachment.
// The type loses MS and the read its Sample operand, which reads the same
// (only) sample. An image type shared with an input that is multisampled, an
// array of subpass inputs, and reads with operands other than Sample are left
// as they are.
std::vector<std::uint32_t> single_sample_subpass_inputs(std::span<const std::uint32_t> code,
                                                        std::uint32_t single_sampled) {
  enum : std::uint32_t {
    OpName = 5, OpTypeImage = 25, OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypePointer = 32, OpTypeFunction = 33,
    OpFunctionParameter = 55, OpVariable = 59, OpLoad = 61, OpDecorate = 71, OpImageRead = 98
  };
  constexpr std::uint32_t InputAttachmentIndex = 43, SubpassData = 6, SampleOperand = 0x40;
  if (!single_sampled || code.size() < 5 || code[0] != 0x07230203) return {};
  std::unordered_map<std::uint32_t, std::size_t> ms_types;  // multisampled subpass image type -> word offset
  std::vector<std::size_t> images;                          // every OpTypeImage
  std::unordered_map<std::uint32_t, std::uint32_t> pointee, element, variable_pointer, index_of, loads;
  for (std::size_t i = 5; i < code.size();) {
    const auto n = code[i] >> 16, op = code[i] & 0xffff;
    if (!n || i + n > code.size()) return {};
    if (op == OpTypeImage && n >= 9) {
      images.push_back(i);
      if (code[i + 3] == SubpassData && code[i + 6] == 1) ms_types[code[i + 1]] = i;
    } else if (op == OpTypePointer && n == 4) {
      pointee[code[i + 1]] = code[i + 3];
    } else if ((op == OpTypeArray && n == 4) || (op == OpTypeRuntimeArray && n == 3)) {
      element[code[i + 1]] = code[i + 2];
    } else if (op == OpVariable && n >= 4) {
      variable_pointer[code[i + 2]] = code[i + 1];
    } else if (op == OpDecorate && n == 4 && code[i + 2] == InputAttachmentIndex) {
      index_of[code[i + 1]] = code[i + 3];
    } else if (op == OpLoad && n >= 4 && ms_types.contains(code[i + 1])) {
      loads[code[i + 2]] = code[i + 1];
    }
    i += n;
  }
  if (ms_types.empty()) return {};
  // A type changes only when every variable of it reads a single-sampled attachment.
  std::unordered_map<std::uint32_t, bool> convert;
  for (const auto& [variable, pointer] : variable_pointer) {
    const auto p = pointee.find(pointer);
    if (p == pointee.end()) continue;
    auto type = p->second;
    bool array = false;
    if (const auto e = element.find(type); e != element.end()) {
      type = e->second;
      array = true;
    }
    if (!ms_types.contains(type)) continue;
    const auto index = index_of.find(variable);
    const bool single = !array && index != index_of.end() && index->second < 32 && (single_sampled >> index->second & 1);
    const auto [it, inserted] = convert.emplace(type, single);
    if (!inserted) it->second = it->second && single;
  }
  std::erase_if(convert, [](const auto& c) { return !c.second; });
  if (convert.empty()) return {};
  // A single-sampled type the module already declares (a subpassInput beside
  // the subpassInputMS) would duplicate the changed one, which is invalid for
  // non-aggregate types: the later declaration goes, its uses name the earlier.
  // Those may only be where a type is expected below; otherwise the module is
  // left as it is.
  std::unordered_map<std::uint32_t, std::uint32_t> remap;
  for (const auto& [type, ok] : convert) {
    const auto t = ms_types[type];
    const auto n = code[t] >> 16;
    for (const auto at : images) {
      if (at == t || (code[at] >> 16) != n || code[at + 6] != 0) continue;
      bool same = true;
      for (std::uint32_t w = 2; w < n && same; ++w) same = w == 6 || code[at + w] == code[t + w];
      if (!same) continue;
      if (at < t) remap[type] = code[at + 1];
      else remap[code[at + 1]] = type;
    }
  }
  auto replace = [&](std::uint32_t& id) {
    if (auto it = remap.find(id); it != remap.end()) id = it->second;
  };
  std::vector<std::uint32_t> out(code.begin(), code.begin() + 5);
  out.reserve(code.size());
  for (std::size_t i = 5; i < code.size();) {
    const auto n = code[i] >> 16, op = code[i] & 0xffff;
    if ((op == OpTypeImage || op == OpName || op == OpDecorate) && n >= 2 && remap.contains(code[i + 1])) {
      // Replaced by the declared single-sampled type, with its name and decorations.
    } else if (op == OpTypeImage && convert.contains(code[i + 1])) {
      out.insert(out.end(), code.begin() + i, code.begin() + i + n);
      out[out.size() - n + 6] = 0;  // MS
    } else if (!remap.empty() && (op == OpTypePointer || op == OpLoad || op == OpFunctionParameter ||
                                  op == OpTypeFunction)) {
      out.insert(out.end(), code.begin() + i, code.begin() + i + n);
      const auto at = out.size() - n;
      if (op == OpTypePointer) replace(out[at + 3]);
      if (op == OpLoad || op == OpFunctionParameter) replace(out[at + 1]);
      if (op == OpTypeFunction)
        for (std::uint32_t w = 2; w < n; ++w) replace(out[at + w]);
    } else if (!remap.empty() && op != OpImageRead && op != OpName && op != OpDecorate &&
               std::any_of(code.begin() + i + 1, code.begin() + i + n,
                           [&](std::uint32_t w) { return remap.contains(w); })) {
      return {};  // a use this does not know (or a literal that looks like one)
    } else if (op == OpImageRead && n >= 6 && loads.contains(code[i + 3]) && convert.contains(loads[code[i + 3]])) {
      if (code[i + 5] != SampleOperand || n != 7) return {};
      out.insert(out.end(), {(5u << 16) | OpImageRead, code[i + 1], code[i + 2], code[i + 3], code[i + 4]});
    } else {
      out.insert(out.end(), code.begin() + i, code.begin() + i + n);
    }
    i += n;
  }
  return out;
}
}  // namespace venture::vulkan
