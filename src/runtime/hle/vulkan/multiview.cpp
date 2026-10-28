#include "multiview.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace venture::vulkan {
namespace {
// SPIR-V core opcodes and decorations (SPIRV-Headers).
enum : std::uint32_t {
  EntryPoint = 15,
  TypeInt = 21,
  TypeFloat = 22,
  TypePointer = 32,
  Variable = 59,
  Load = 61,
  Store = 62,
  AccessChain = 65,
  InBoundsAccessChain = 66,
  PtrAccessChain = 67,
  InBoundsPtrAccessChain = 70,
  Decorate = 71,
  CopyObject = 83,
  Bitcast = 124,
  Select = 169,
  Phi = 245,
  FunctionCall = 57,
  BuiltIn = 11,
  Flat = 14,
  NoPerspective = 13,
  Centroid = 16,
  Sample = 17,
  Location = 30,
  Component = 31,
  ViewIndex = 4440,
  Input = 1,
  Output = 3,
  Function = 7
};
struct Instruction {
  std::size_t offset;
  std::span<const std::uint32_t> words;
  std::uint32_t op;
};
struct VariableInfo {
  std::uint32_t type, storage;
};
struct Decoration {
  std::optional<std::uint32_t> location;
  std::uint32_t component{};
  bool flat{}, other_interpolation{}, view_index{};
};
struct Module {
  std::vector<Instruction> instructions;
  std::map<std::uint32_t, std::uint32_t> pointers;
  std::set<std::uint32_t> floats, signed_ints, interface, escaped;
  std::map<std::uint32_t, VariableInfo> variables;
  std::map<std::uint32_t, Decoration> decorations;
  std::map<std::uint32_t, std::vector<std::uint32_t>> stores;
};
std::optional<Module> parse(std::span<const std::uint32_t> code, std::string_view entry,
                            bool output) {
  if (code.size() < 5 || code[0] != 0x07230203) return {};
  Module m;
  for (std::size_t i = 5; i < code.size();) {
    const auto count = code[i] >> 16, op = code[i] & 0xffff;
    if (!count || count > code.size() - i) return {};
    // Grouped interface decorations need group expansion; leave such modules
    // intact.
    if (op == 74 || op == 75) return {};
    const auto w = code.subspan(i, count);
    m.instructions.push_back({i, w, op});
    if (op == EntryPoint && count >= 4) {
      std::string name;
      std::size_t end = 3;
      bool terminated = false;
      for (; end < count && !terminated; ++end)
        for (unsigned b = 0; b < 4; ++b) {
          const auto c = char((w[end] >> (8 * b)) & 255);
          if (!c) {
            terminated = true;
            break;
          }
          name += c;
        }
      if (terminated && name == entry && w[1] == (output ? 0u : 4u))
        m.interface.insert(w.begin() + end, w.end());
    } else if (op == TypeInt && count == 4 && w[2] == 32 && w[3] == 1)
      m.signed_ints.insert(w[1]);
    else if (op == TypeFloat && count == 3 && w[2] == 32)
      m.floats.insert(w[1]);
    else if (op == TypePointer && count == 4)
      m.pointers[w[1]] = w[3];
    else if (op == Variable && count >= 4) {
      m.variables[w[2]] = {w[1], w[3]};
      if (count == 5) m.stores[w[2]].push_back(w[4]);
    } else if (op == Store && count >= 3)
      m.stores[w[1]].push_back(w[2]);
    else if (op == Decorate && count >= 3) {
      auto& d = m.decorations[w[1]];
      if (w[2] == Location && count == 4) d.location = w[3];
      if (w[2] == Component && count == 4) d.component = w[3];
      if (w[2] == BuiltIn && count == 4 && w[3] == ViewIndex) d.view_index = true;
      if (w[2] == Flat) d.flat = true;
      if (w[2] == NoPerspective || w[2] == Centroid || w[2] == Sample) d.other_interpolation = true;
    }
    if ((op == AccessChain || op == InBoundsAccessChain || op == PtrAccessChain ||
         op == InBoundsPtrAccessChain) &&
        count >= 4)
      m.escaped.insert(w[3]);
    if ((op == CopyObject || op == Bitcast) && count == 4 && m.pointers.contains(w[1]))
      m.escaped.insert(w[3]);
    if (op == Phi && count >= 5 && m.pointers.contains(w[1]))
      for (std::size_t j = 3; j < count; j += 2) m.escaped.insert(w[j]);
    if (op == Select && count == 6 && m.pointers.contains(w[1])) {
      m.escaped.insert(w[4]);
      m.escaped.insert(w[5]);
    }
    if ((op == 63 || op == 64) && count >= 3) {
      m.escaped.insert(w[1]);
      m.escaped.insert(w[2]);
    }
    // Explicit interpolation and other extended instructions may take pointer
    // operands.
    if (op == 12 && count >= 6) m.escaped.insert(w.begin() + 5, w.end());
    if (op == FunctionCall && count >= 4) m.escaped.insert(w.begin() + 4, w.end());
    i += count;
  }
  return m;
}
std::map<std::uint32_t, VaryingSlot> varyings(const Module& m, bool output) {
  std::map<std::uint32_t, VaryingSlot> result;
  for (const auto& [id, v] : m.variables) {
    auto p = m.pointers.find(v.type);
    auto d = m.decorations.find(id);
    if (v.storage == (output ? Output : Input) && m.interface.contains(id) &&
        p != m.pointers.end() && m.floats.contains(p->second) && d != m.decorations.end() &&
        d->second.location && !d->second.other_interpolation && !m.escaped.contains(id))
      result[id] = {*d->second.location, d->second.component};
  }
  return result;
}
// The expression includes its casts, not merely a "depends on ViewIndex" bit:
// different expressions in branches may depend on vertex attributes for their
// selection.
using Origin = std::vector<std::uint32_t>;
std::map<std::uint32_t, Origin> origins(const Module& m) {
  std::map<std::uint32_t, Origin> known;
  for (const auto& [id, d] : m.decorations)
    if (d.view_index) {
      auto v = m.variables.find(id);
      if (v != m.variables.end() && v->second.storage == Input && m.interface.contains(id)) {
        auto p = m.pointers.find(v->second.type);
        if (p != m.pointers.end() && m.signed_ints.contains(p->second)) known[id] = {id};
      }
    }
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& [id, writes] : m.stores) {
      if (known.contains(id) || writes.empty() || m.escaped.contains(id)) continue;
      auto v = m.variables.find(id);
      if (v == m.variables.end() || (v->second.storage != Function && v->second.storage != Output))
        continue;
      auto first = known.find(writes[0]);
      if (first == known.end()) continue;
      if (std::all_of(writes.begin(), writes.end(), [&](auto value) {
            auto it = known.find(value);
            return it != known.end() && it->second == first->second;
          })) {
        known[id] = first->second;
        changed = true;
      }
    }
    for (const auto& ins : m.instructions) {
      const auto w = ins.words;
      if (w.size() < 4 || known.contains(w[2])) continue;
      if (ins.op == Load || ins.op == CopyObject || (ins.op >= 109 && ins.op <= 115) ||
          ins.op == Bitcast) {
        auto from = known.find(w[3]);
        if (from == known.end()) continue;
        auto value = from->second;
        if (ins.op != Load && ins.op != CopyObject) {
          value.push_back(ins.op);
          value.push_back(w[1]);
        }
        known[w[2]] = std::move(value);
        changed = true;
      } else if (ins.op == Select && w.size() == 6) {
        auto a = known.find(w[4]), b = known.find(w[5]);
        if (a != known.end() && b != known.end() && a->second == b->second) {
          known[w[2]] = a->second;
          changed = true;
        }
      } else if (ins.op == Phi && w.size() >= 5 && w.size() % 2 == 1) {
        auto first = known.find(w[3]);
        if (first == known.end()) continue;
        bool same = true;
        for (std::size_t j = 5; j < w.size(); j += 2) {
          auto it = known.find(w[j]);
          same &= it != known.end() && it->second == first->second;
        }
        if (same) {
          known[w[2]] = first->second;
          changed = true;
        }
      }
    }
  }
  return known;
}
}  // namespace
std::vector<VaryingSlot> view_index_outputs(std::span<const std::uint32_t> code,
                                            std::string_view entry) {
  auto m = parse(code, entry, true);
  if (!m) return {};
  const auto known = origins(*m);
  std::vector<VaryingSlot> result;
  for (auto [id, slot] : varyings(*m, true))
    if (known.contains(id)) result.push_back(slot);
  std::sort(result.begin(), result.end());
  return result;
}
std::vector<VaryingSlot> scalar_varyings(std::span<const std::uint32_t> code,
                                         std::string_view entry, bool output) {
  auto m = parse(code, entry, output);
  if (!m) return {};
  std::vector<VaryingSlot> result;
  for (auto [id, slot] : varyings(*m, output)) result.push_back(slot);
  std::sort(result.begin(), result.end());
  return result;
}
std::vector<std::uint32_t> flat_varyings(std::span<const std::uint32_t> code,
                                         std::string_view entry, bool output,
                                         std::span<const VaryingSlot> slots) {
  auto m = parse(code, entry, output);
  if (!m) return {code.begin(), code.end()};
  std::set<std::uint32_t> ids;
  for (auto [id, slot] : varyings(*m, output))
    if (std::find(slots.begin(), slots.end(), slot) != slots.end() && !m->decorations.at(id).flat)
      ids.insert(id);
  if (ids.empty()) return {code.begin(), code.end()};
  std::size_t insert = 5;
  for (const auto& ins : m->instructions)
    if ((ins.op >= 71 && ins.op <= 75) || ins.op == 332 || ins.op == 5632 || ins.op == 5633)
      insert = ins.offset + ins.words.size();
  std::vector<std::uint32_t> result(code.begin(), code.begin() + insert);
  for (auto id : ids) result.insert(result.end(), {(3u << 16) | Decorate, id, Flat});
  result.insert(result.end(), code.begin() + insert, code.end());
  return result;
}
}  // namespace venture::vulkan
