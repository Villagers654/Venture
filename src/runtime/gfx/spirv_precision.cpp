#include "gfx/spirv_precision.h"
#include <cstring>
#include <string_view>
#include <unordered_set>

namespace venture::gfx {
namespace {
constexpr std::uint32_t magic = 0x07230203;
constexpr std::uint32_t op_ext_inst_import = 11, op_ext_inst = 12, op_decorate = 71, op_dot = 148;
constexpr std::uint32_t relaxed_precision = 0;
// GLSL.std.450 instruction numbers.
constexpr std::uint32_t glsl_sqrt = 31, glsl_inverse_sqrt = 32, glsl_length = 66, glsl_distance = 67,
                        glsl_normalize = 69;
}

bool promote_relaxed_magnitudes(std::span<const std::uint32_t> words, std::vector<std::uint32_t>& out) {
  if (words.size() < 5 || words[0] != magic) return false;
  std::unordered_set<std::uint32_t> relaxed, glsl, promote;
  // Pass 1: which ids are relaxed, and which results are magnitudes. Function
  // bodies define a value before any non-phi use, so a sqrt sees its dot first.
  for (std::size_t i = 5; i < words.size();) {
    const auto count = words[i] >> 16, op = words[i] & 0xffff;
    if (!count || i + count > words.size()) return false;
    if (op == op_decorate && count >= 3 && words[i + 2] == relaxed_precision) {
      relaxed.insert(words[i + 1]);
    } else if (op == op_ext_inst_import && count >= 3) {
      const auto* name = reinterpret_cast<const char*>(&words[i + 2]);
      if (std::string_view(name, strnlen(name, (count - 2) * 4)) == "GLSL.std.450") glsl.insert(words[i + 1]);
    } else if (op == op_dot && count >= 5) {
      if (relaxed.contains(words[i + 2])) promote.insert(words[i + 2]);
    } else if (op == op_ext_inst && count >= 6 && glsl.contains(words[i + 3]) && relaxed.contains(words[i + 2])) {
      const auto inst = words[i + 4];
      if (inst == glsl_length || inst == glsl_distance || inst == glsl_normalize ||
          ((inst == glsl_sqrt || inst == glsl_inverse_sqrt) && promote.contains(words[i + 5])))
        promote.insert(words[i + 2]);
    }
    i += count;
  }
  if (promote.empty()) return false;
  // Pass 2: drop their RelaxedPrecision decorations.
  out.clear();
  out.reserve(words.size());
  out.insert(out.end(), words.begin(), words.begin() + 5);
  for (std::size_t i = 5; i < words.size();) {
    const auto count = words[i] >> 16, op = words[i] & 0xffff;
    const bool drop = op == op_decorate && count >= 3 && words[i + 2] == relaxed_precision &&
                      promote.contains(words[i + 1]);
    if (!drop) out.insert(out.end(), words.begin() + i, words.begin() + i + count);
    i += count;
  }
  return true;
}
}
