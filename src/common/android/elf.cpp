#include "common/android/elf.h"

#include <cstring>

namespace venture::android::elf {
namespace {
// Section and dynamic-entry fields at the offsets of the ELF specification.
struct Reader {
  std::span<const std::uint8_t> bytes;
  bool wide = false;  // ELFCLASS64
  bool ok(std::uint64_t at, std::uint64_t size) const { return at <= bytes.size() && size <= bytes.size() - at; }
  std::uint64_t u(std::uint64_t at, int size) const {
    if (!ok(at, std::uint64_t(size))) return 0;
    std::uint64_t value = 0;
    std::memcpy(&value, bytes.data() + at, std::size_t(size));  // little endian host and file
    return value;
  }
  std::uint64_t word(std::uint64_t at) const { return u(at, wide ? 8 : 4); }
  std::string string(std::uint64_t table, std::uint64_t table_size, std::uint64_t at) const {
    if (at >= table_size || !ok(table, table_size)) return {};
    const auto *begin = reinterpret_cast<const char *>(bytes.data() + table + at);
    return std::string(begin, strnlen(begin, std::size_t(table_size - at)));
  }
};
struct Section {
  std::uint32_t type = 0, link = 0;
  std::uint64_t offset = 0, size = 0, entry = 0;
};
constexpr std::uint32_t kDynsym = 11, kDynamic = 6;

bool header(Reader &r) {
  const auto &b = r.bytes;
  if (b.size() < 52 || std::memcmp(b.data(), "\x7f" "ELF", 4) != 0 || b[5] != 1) return false;
  r.wide = b[4] == 2;
  return b[4] == 1 || b[4] == 2;
}
std::vector<Section> sections(const Reader &r) {
  const auto offset = r.wide ? r.u(0x28, 8) : r.u(0x20, 4);
  const auto size = r.u(r.wide ? 0x3a : 0x2e, 2), count = r.u(r.wide ? 0x3c : 0x30, 2);
  std::vector<Section> out;
  if (size < (r.wide ? 64u : 40u) || !r.ok(offset, size * count)) return out;
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto at = offset + i * size;
    Section s;
    s.type = std::uint32_t(r.u(at + 4, 4));
    s.offset = r.wide ? r.u(at + 0x18, 8) : r.u(at + 0x10, 4);
    s.size = r.wide ? r.u(at + 0x20, 8) : r.u(at + 0x14, 4);
    s.link = std::uint32_t(r.wide ? r.u(at + 0x28, 4) : r.u(at + 0x18, 4));
    s.entry = r.wide ? r.u(at + 0x38, 8) : r.u(at + 0x24, 4);
    out.push_back(s);
  }
  return out;
}
}  // namespace

std::vector<std::string> exports(std::span<const std::uint8_t> library) {
  Reader r{library};
  std::vector<std::string> out;
  if (!header(r)) return out;
  const auto all = sections(r);
  for (const auto &s : all) {
    if (s.type != kDynsym || s.link >= all.size() || !s.entry) continue;
    const auto &names = all[s.link];
    for (std::uint64_t at = s.offset + s.entry; at + s.entry <= s.offset + s.size; at += s.entry) {
      // Elf64_Sym: name u32, info u8, other u8, shndx u16; Elf32_Sym: name, value, size, info, other, shndx.
      const auto name = r.u(at, 4);
      const auto info = r.u(at + (r.wide ? 4 : 12), 1), shndx = r.u(at + (r.wide ? 6 : 14), 2);
      const auto binding = info >> 4, type = info & 0xf;
      if (shndx == 0 || (binding != 1 && binding != 2) || (type != 1 && type != 2)) continue;  // defined GLOBAL/WEAK OBJECT/FUNC
      if (auto text = r.string(names.offset, names.size, name); !text.empty()) out.push_back(std::move(text));
    }
  }
  return out;
}

std::string soname(std::span<const std::uint8_t> library) {
  Reader r{library};
  if (!header(r)) return {};
  const auto all = sections(r);
  for (const auto &s : all) {
    if (s.type != kDynamic || s.link >= all.size()) continue;
    const auto step = r.wide ? 16u : 8u;
    for (std::uint64_t at = s.offset; at + step <= s.offset + s.size; at += step) {
      const auto tag = r.word(at);
      if (tag == 0) break;
      if (tag == 14) return r.string(all[s.link].offset, all[s.link].size, r.word(at + step / 2));  // DT_SONAME
    }
  }
  return {};
}
}  // namespace venture::android::elf
