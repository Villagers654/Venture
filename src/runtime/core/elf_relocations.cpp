#include "core/elf.h"
#include <limits>
#include <stdexcept>

namespace venture::elf {
namespace {
class Sleb {
 public:
  explicit Sleb(std::span<const std::byte> data) : data_(data) {}
  std::int64_t next() {
    std::uint64_t result = 0;
    for (unsigned shift = 0; shift < 70; shift += 7) {
      if (pos_ == data_.size()) throw std::runtime_error("truncated APS2 SLEB128");
      auto b = std::to_integer<unsigned>(data_[pos_++]);
      if (shift == 63 && b != 0 && b != 0x7f) throw std::runtime_error("APS2 SLEB128 overflow");
      result |= static_cast<std::uint64_t>(b & 0x7f) << shift;
      if (!(b & 0x80)) {
        if ((b & 0x40) && shift < 63) result |= ~std::uint64_t{0} << (shift + 7);
        return static_cast<std::int64_t>(result);
      }
    }
    throw std::runtime_error("APS2 SLEB128 overflow");
  }
 private:
  std::span<const std::byte> data_;
  std::size_t pos_ = 0;
};
}
void decode_aps2(std::span<const std::byte> bytes, const std::function<void(const Rela&)>& visit) {
  if (bytes.size() < 4 || std::to_integer<char>(bytes[0]) != 'A' || std::to_integer<char>(bytes[1]) != 'P' ||
      std::to_integer<char>(bytes[2]) != 'S' || std::to_integer<char>(bytes[3]) != '2') throw std::runtime_error("invalid Android APS2 magic");
  // bionic/linker/linker_reloc_iterators.h: grouped info=1, offset=2, addend=4, has-addend=8.
  Sleb stream(bytes.subspan(4));
  auto count = stream.next();
  if (count < 0 || count > 100'000'000) throw std::runtime_error("invalid APS2 relocation count");
  Rela r{static_cast<std::uint64_t>(stream.next()), 0, 0};
  for (std::int64_t index = 0; index < count;) {
    auto group = stream.next(), flags = stream.next();
    if (group <= 0 || group > count - index || (flags & ~15) || ((flags & 4) && !(flags & 8)))
      throw std::runtime_error("invalid APS2 relocation group");
    auto delta = (flags & 2) ? stream.next() : 0;
    if (flags & 1) r.info = static_cast<std::uint64_t>(stream.next());
    if ((flags & 12) == 12) r.addend = static_cast<std::int64_t>(static_cast<std::uint64_t>(r.addend) + stream.next());
    else if (!(flags & 8)) r.addend = 0;
    for (std::int64_t i = 0; i < group; ++i) {
      r.offset += (flags & 2) ? delta : stream.next();
      if (!(flags & 1)) r.info = static_cast<std::uint64_t>(stream.next());
      if ((flags & 12) == 8) r.addend = static_cast<std::int64_t>(static_cast<std::uint64_t>(r.addend) + stream.next());
      visit(r);
    }
    index += group;
  }
}
void decode_relr(std::span<const std::uint64_t> words, const std::function<void(std::uint64_t)>& visit) {
  std::uint64_t cursor = 0;
  bool base = false;
  for (auto word : words) {
    if (!(word & 1)) {
      if ((word & 7) || word > std::numeric_limits<std::uint64_t>::max() - 8) throw std::runtime_error("invalid RELR base");
      visit(word); cursor = word + 8; base = true;
    } else {
      if (!base || cursor > std::numeric_limits<std::uint64_t>::max() - 63 * 8) throw std::runtime_error("invalid RELR bitmap");
      for (unsigned bit = 1; bit < 64; ++bit) if (word & (1ull << bit)) visit(cursor + (bit - 1) * 8);
      cursor += 63 * 8;
    }
  }
}
void decode_relr32(std::span<const std::uint32_t> words, const std::function<void(std::uint64_t)>& visit) {
  std::uint64_t cursor = 0;
  bool base = false;
  for (auto word : words) {
    if (!(word & 1)) {
      if (word & 3) throw std::runtime_error("invalid RELR base");
      visit(word); cursor = std::uint64_t{word} + 4; base = true;
    } else {
      if (!base) throw std::runtime_error("invalid RELR bitmap");
      for (unsigned bit = 1; bit < 32; ++bit) if (word & (1u << bit)) visit(cursor + (bit - 1) * 4);
      cursor += 31 * 4;
    }
  }
}
std::span<const ArmRelocation> arm_relocation_table() {
  static constexpr ArmRelocation table[]{
      {0, "NONE", ArmRelocation::none}, {2, "ABS32", ArmRelocation::absolute},
      {21, "GLOB_DAT", ArmRelocation::absolute}, {22, "JUMP_SLOT", ArmRelocation::absolute},
      {23, "RELATIVE", ArmRelocation::relative}};
  return table;
}
std::string relocation_name32(std::uint32_t type) {
  for (const auto& entry : arm_relocation_table()) if (entry.type == type) return entry.name;
  if (type == 3) return "R_ARM_REL32";
  if (type == 42) return "R_ARM_PREL31";
  return "R_ARM_" + std::to_string(type);
}
std::string relocation_name(std::uint32_t type) {
  switch (type) {
    case 0: return "NONE";
    case 257: return "ABS64";
    case 258: return "ABS32";
    case 260: return "PREL64";
    case 261: return "PREL32";
    case 1024: return "COPY";
    case 1025: return "GLOB_DAT";
    case 1026: return "JUMP_SLOT";
    case 1027: return "RELATIVE";
    case 1028: return "TLS_DTPMOD64";
    case 1029: return "TLS_DTPREL64";
    case 1030: return "TLS_TPREL64";
    case 1031: return "TLSDESC";
    case 1032: return "IRELATIVE";
    default: return "R_AARCH64_" + std::to_string(type);
  }
}
}
