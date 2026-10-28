#include "common/environment.h"
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "internal.h"
#include "common/log.h"

namespace venture::vulkan {
// SPIR-V OpVariable without an initializer has undefined contents. Select zero
// for numeric Function locals so an uninitialized counter cannot turn a small
// loop into billions of iterations on a different GPU. Defined shader behavior
// is preserved: explicit initializers, other storage classes and opaque types
// are untouched. OpConstantNull provides the correctly typed composite zeros.
std::vector<std::uint32_t> initialize_shader_locals(
    std::span<const std::uint32_t> code) {
  if (code.size() < 5 || code[0] != 0x07230203 || !code[3]) return {};
  std::unordered_set<std::uint32_t> numeric;
  std::unordered_map<std::uint32_t, std::uint32_t> pointers, nulls;
  std::size_t first_function = code.size();
  for (std::size_t i = 5; i < code.size();) {
    auto count = code[i] >> 16, op = code[i] & 0xffff;
    if (!count || count > code.size() - i) return {};
    auto w = code.subspan(i, count);
    if ((op == 20 && count == 2) || (op == 21 && count == 4) ||
        (op == 22 && count == 3))
      numeric.insert(w[1]);  // Bool, Int, Float
    else if ((op == 23 || op == 24 || op == 28) && count == 4 &&
             numeric.contains(w[2]))
      numeric.insert(w[1]);  // Vector, Matrix, fixed Array
    else if (op == 30 && count >= 2 &&
             std::all_of(w.begin() + 2, w.end(),
                         [&](auto id) { return numeric.contains(id); }))
      numeric.insert(w[1]);  // Struct containing only numeric members
    else if (op == 32 && count == 4 && w[2] == 7)
      pointers[w[1]] = w[3];  // Pointer in Function storage
    else if (op == 46 && count == 3)
      nulls[w[1]] = w[2];
    if (op == 54) first_function = std::min(first_function, i);
    i += count;
  }
  std::unordered_map<std::size_t, std::uint32_t> initializers;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> constants;
  std::uint64_t bound = code[3];
  bool function = false;
  for (std::size_t i = 5; i < code.size();) {
    auto count = code[i] >> 16, op = code[i] & 0xffff;
    if (op == 54) function = true;
    if (op == 56) function = false;
    if (function && op == 59 && count == 4 && code[i + 3] == 7) {
      auto p = pointers.find(code[i + 1]);
      if (p != pointers.end() && numeric.contains(p->second)) {
        auto type = p->second;
        if (!nulls.contains(type)) {
          if (bound >= std::numeric_limits<std::uint32_t>::max()) return {};
          auto id = static_cast<std::uint32_t>(bound++);
          nulls[type] = id;
          constants.emplace_back(type, id);
        }
        initializers[i] = nulls[type];
      }
    }
    i += count;
  }
  if (initializers.empty()) return {};
  std::vector<std::uint32_t> out(code.begin(), code.begin() + 5);
  out[3] = static_cast<std::uint32_t>(bound);
  for (std::size_t i = 5; i < code.size();) {
    if (i == first_function)
      for (auto [type, id] : constants)
        out.insert(out.end(), {0x0003002e, type, id});
    auto count = code[i] >> 16;
    auto start = out.size();
    out.insert(out.end(), code.begin() + i, code.begin() + i + count);
    if (auto init = initializers.find(i); init != initializers.end()) {
      out[start] = 0x0005003b;
      out.push_back(init->second);
    }
    i += count;
  }
  return out;
}
VkFormat storage_image_format(std::uint32_t spirv_format) {
  // SPIR-V Image Format operand values (SPIR-V 1.6 section 3.11).
  static constexpr VkFormat formats[] = {
      VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT,
      VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R16G16_SFLOAT,
      VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM,
      VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R16_UNORM,
      VK_FORMAT_R8_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R8G8_SNORM,
      VK_FORMAT_R16_SNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R16G16B16A16_SINT,
      VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R16G16_SINT,
      VK_FORMAT_R8G8_SINT, VK_FORMAT_R16_SINT, VK_FORMAT_R8_SINT, VK_FORMAT_R32G32B32A32_UINT,
      VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R32_UINT, VK_FORMAT_A2B10G10R10_UINT_PACK32,
      VK_FORMAT_R32G32_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R16_UINT, VK_FORMAT_R8_UINT,
      VK_FORMAT_R64_UINT, VK_FORMAT_R64_SINT};
  return spirv_format < std::size(formats) ? formats[spirv_format] : VK_FORMAT_UNDEFINED;
}
std::vector<std::uint32_t> formatless_storage_images(std::span<const std::uint32_t> code,
                                                     std::span<const std::uint32_t> keep) {
  if (code.size() < 5 || code[0] != 0x07230203) return {};
  // OpTypeImage operands after the result id, with Format (index 6 here) cleared.
  using Key = std::vector<std::uint32_t>;
  std::map<Key, unsigned> declarations;
  std::vector<std::size_t> candidates;
  std::size_t capabilities_end = 5;
  bool read = false, write = false;
  for (std::size_t i = 5; i < code.size();) {
    auto count = code[i] >> 16, op = code[i] & 0xffff;
    if (!count || count > code.size() - i) return {};
    if (op == 17 && count == 2) {  // OpCapability
      capabilities_end = i + count;
      read |= code[i + 1] == 55;   // StorageImageReadWithoutFormat
      write |= code[i + 1] == 56;  // StorageImageWriteWithoutFormat
    }
    if (op == 25 && count >= 9) {  // OpTypeImage
      Key key(code.begin() + i + 2, code.begin() + i + count);
      const auto format = key[6];
      key[6] = 0;
      ++declarations[key];
      // Sampled == 2: storage image or texel buffer. Vulkan requires a declared
      // format for image atomics (R32f/R32i/R32ui/R64ui/R64i), so keep those.
      if (key[5] == 2 && format != 0 && format != 3 && format != 24 && format != 33 && format != 40 && format != 41 &&
          std::find(keep.begin(), keep.end(), format) == keep.end())
        candidates.push_back(i);
    }
    i += count;
  }
  std::vector<std::size_t> rewrite;
  for (auto i : candidates) {
    Key key(code.begin() + i + 2, code.begin() + i + (code[i] >> 16));
    key[6] = 0;
    if (declarations[key] == 1) rewrite.push_back(i);  // a duplicate declaration would be invalid
  }
  if (rewrite.empty()) return {};
  std::vector<std::uint32_t> out(code.begin(), code.begin() + capabilities_end);
  if (!read) out.insert(out.end(), {0x00020011, 55});
  if (!write) out.insert(out.end(), {0x00020011, 56});
  const auto shift = out.size() - capabilities_end;
  out.insert(out.end(), code.begin() + capabilities_end, code.end());
  for (auto i : rewrite) out[i + shift + 8] = 0;  // Image Format = Unknown
  return out;
}
namespace {
std::mutex formatless_lock;
// Devices with formatless storage enabled, and the declared formats each keeps.
std::unordered_map<VkDevice, std::vector<std::uint32_t>> formatless_devices;
std::unordered_set<VkDevice> reported_devices;  // without the features; reported once
std::optional<std::vector<std::uint32_t>> formatless_keep(VkDevice device) {
  std::lock_guard lock(formatless_lock);
  auto it = formatless_devices.find(device);
  return it == formatless_devices.end() ? std::nullopt : std::optional(it->second);
}
bool first_report(VkDevice device) {
  std::lock_guard lock(formatless_lock);
  return reported_devices.insert(device).second;
}
}  // namespace
bool storage_format_rewrite_enabled() {
  const auto* setting = venture::environment::getenv("VENTURE_VULKAN_FORMATLESS_STORAGE");
  return !setting || std::string_view(setting) != "0";
}
void set_storage_without_format(VkDevice device, bool enabled, std::vector<std::uint32_t> keep) {
  std::lock_guard lock(formatless_lock);
  reported_devices.erase(device);
  if (enabled) formatless_devices[device] = std::move(keep);
  else formatless_devices.erase(device);
}
namespace {
VkResult VKAPI_CALL create_shader(VkDevice d,
                                  const VkShaderModuleCreateInfo* info,
                                  const VkAllocationCallbacks* allocator,
                                  VkShaderModule* out) {
  auto next = reinterpret_cast<PFN_vkCreateShaderModule>(
      cached_layer_proc<Layer::shader, "vkCreateShaderModule">());
  if (!next)
    next = reinterpret_cast<PFN_vkCreateShaderModule>(
        raw_device_proc(d, "vkCreateShaderModule"));
  if (!info->pCode || info->codeSize % 4) return next(d, info, allocator, out);
  auto disabled = venture::environment::getenv("VENTURE_VULKAN_ZERO_INIT_LOCALS");
  const bool zero_init = !disabled || std::string_view(disabled) != "0";
  try {
    std::vector<std::uint32_t> code;
    std::span<const std::uint32_t> input{info->pCode, info->codeSize / 4};
    if (zero_init) {
      code = initialize_shader_locals(input);
      if (!code.empty()) input = code;
    }
    if (const auto keep = formatless_keep(d)) {
      if (auto formatless = formatless_storage_images(input, *keep); !formatless.empty()) {
        code = std::move(formatless);
        input = code;
      }
    } else if (storage_format_rewrite_enabled() && !formatless_storage_images(input).empty() && first_report(d)) {
      // Nothing can repair a mismatch here: say so once instead of failing silently.
      log::warn("vulkan", "Guest shaders declare storage-image formats that the host would follow from the bound "
                "view, but this device lacks shaderStorageImageRead/WriteWithoutFormat: they stay as declared, and "
                "views of another format give undefined results");
    }
    if (code.empty()) return next(d, info, allocator, out);
    auto copy = *info;
    copy.pCode = code.data();
    copy.codeSize = code.size() * 4;
    return next(d, &copy, allocator, out);
  } catch (const std::bad_alloc&) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
}
}  // namespace
PFN_vkVoidFunction shader_proc(std::string_view name) {
  return name == "vkCreateShaderModule"
             ? reinterpret_cast<PFN_vkVoidFunction>(create_shader)
             : nullptr;
}
}  // namespace venture::vulkan
