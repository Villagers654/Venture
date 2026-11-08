#include "hle/steam/abi.h"
#include "common/meta/auth.h"
#include "core/elf.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace venture::steam {
namespace {
using Bytes = std::span<const std::byte>;

template<class T>
T read(Bytes bytes, std::size_t offset = 0) {
  T value;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

Bytes file_range(Bytes file, std::uint64_t offset, std::uint64_t size) {
  if (offset > file.size() || size > file.size() - offset) return {};
  return file.subspan(offset, size);
}

bool has_build_id(Bytes notes, std::string_view expected) {
  constexpr std::string_view hex = "0123456789abcdef";
  while (notes.size() >= 12) {
    const auto name_size = read<std::uint32_t>(notes);
    const auto desc_size = read<std::uint32_t>(notes, 4);
    const auto type = read<std::uint32_t>(notes, 8);
    const auto aligned = [](std::uint32_t n) { return (std::uint64_t{n} + 3) & ~std::uint64_t{3}; };
    const auto end = 12 + aligned(name_size) + aligned(desc_size);
    if (end > notes.size()) return false;
    if (type == 3 && name_size == 4 && std::memcmp(notes.data() + 12, "GNU", 4) == 0) {
      if (desc_size * std::uint64_t{2} != expected.size()) return false;
      for (std::size_t i = 0; i < desc_size; ++i) {
        const auto byte = std::to_integer<unsigned>(notes[16 + i]);
        if (hex[byte >> 4] != expected[2 * i] || hex[byte & 15] != expected[2 * i + 1]) return false;
      }
      return true;
    }
    notes = notes.subspan(end);
  }
  return false;
}

bool matches_repacked_sdk(Bytes file, const Sdk& sdk) {
  // Use the loader-visible data so stripping section headers preserves the identity.
  const auto header = read<elf::Header>(file);
  if (header.type != 3 || header.phentsize != sizeof(elf::Phdr)) return false; // ET_DYN
  const auto headers = file_range(file, header.phoff, header.phnum * sizeof(elf::Phdr));
  if (headers.empty()) return false;

  bool build_id_matches = false;
  elf::Phdr dynamic_segment{};
  std::vector<elf::Phdr> loads;
  for (std::size_t i = 0; i < headers.size(); i += sizeof(elf::Phdr)) {
    const auto ph = read<elf::Phdr>(headers, i);
    if (ph.type == 1) loads.push_back(ph); // PT_LOAD
    if (ph.type == 2) { // PT_DYNAMIC
      if (dynamic_segment.type != 0) return false;
      dynamic_segment = ph;
    }
    if (ph.type == 4 && has_build_id(file_range(file, ph.offset, ph.filesz), sdk.android_build_id))
      build_id_matches = true;
  }
  if (!build_id_matches || !dynamic_segment.type) return false;

  const auto mapped = [&](std::uint64_t address, std::uint64_t size) -> Bytes {
    for (const auto& load : loads) {
      if (address < load.vaddr || address - load.vaddr >= load.filesz) continue;
      const auto segment = file_range(file, load.offset, load.filesz);
      return file_range(segment, address - load.vaddr, size);
    }
    return {};
  };
  const auto dynamic = mapped(dynamic_segment.vaddr, dynamic_segment.filesz);
  if (dynamic.empty() || dynamic.data() != file_range(file, dynamic_segment.offset, dynamic_segment.filesz).data())
    return false;

  enum : std::size_t { plt_size = 2, hash = 4, strings = 5, symbols = 6, rela = 7,
                        rela_size = 8, rela_entry = 9, string_size = 10, symbol_entry = 11,
                        plt_format = 20, jmprel = 23 };
  std::array<std::uint64_t, jmprel + 1> tags{};
  bool terminated = false;
  for (std::size_t i = 0; i + sizeof(elf::Dyn) <= dynamic.size(); i += sizeof(elf::Dyn)) {
    const auto entry = read<elf::Dyn>(dynamic, i);
    if (entry.tag == 0) { terminated = true; break; }
    // The pinned ABI uses RELA, with no alternate or packed relocation tables.
    if (entry.tag == 17 || entry.tag == 36 || entry.tag == 0x6000000f ||
        entry.tag == 0x60000011 || entry.tag == 0x6fffe000) return false;
    if (entry.tag > 0 && static_cast<std::size_t>(entry.tag) < tags.size())
      tags[entry.tag] = entry.value;
  }
  if (!terminated || !tags[hash] || tags[symbol_entry] != sizeof(elf::Sym) ||
      tags[rela_entry] != sizeof(elf::Rela) || tags[plt_format] != rela) return false;
  const auto hash_header = mapped(tags[hash], 8);
  if (hash_header.empty()) return false;
  const auto symbol_count = read<std::uint32_t>(hash_header, 4); // DT_HASH nchain
  const Bytes ranges[]{
      mapped(tags[symbols], std::uint64_t{symbol_count} * sizeof(elf::Sym)),
      mapped(tags[strings], tags[string_size]),
      mapped(tags[rela], tags[rela_size]),
      mapped(tags[jmprel], tags[plt_size]),
  };
  std::string hashes;
  for (const auto range : ranges) {
    if (range.empty()) return false;
    hashes += meta::sha256({reinterpret_cast<const char*>(range.data()), range.size()});
  }
  return meta::sha256(hashes) == sdk.android_abi_sha256;
}
} // namespace

const Sdk* detect_sdk(std::span<const std::byte> elf) {
  // ELF64 little-endian AArch64.
  if (elf.size() < sizeof(elf::Header) || std::memcmp(elf.data(), "\x7f" "ELF\x02\x01", 6) != 0 ||
      std::to_integer<unsigned>(elf[18]) != 183 || elf[19] != std::byte{}) return nullptr;
  const auto hash = meta::sha256({reinterpret_cast<const char*>(elf.data()), elf.size()});
  for (const auto* sdk : {&sdk163(), &sdk164(), &sdk165()}) if (sdk->android_sha256 == hash) return sdk;
  for (const auto* sdk : {&sdk163(), &sdk164(), &sdk165()})
    if (!sdk->android_build_id.empty() && matches_repacked_sdk(elf, *sdk)) return sdk;
  return nullptr;
}
const Layout* Sdk::callback(std::int32_t id) const {
  for (const auto& l : layouts) if (l.callback == id) return &l;
  return nullptr;
}
const Layout* Sdk::layout(std::string_view name) const {
  for (const auto& l : layouts) if (l.name == name) return &l;
  return nullptr;
}
const Interface* Sdk::interface(std::string_view version) const {
  for (const auto& i : interfaces) if (i.version == version) return &i;
  return nullptr;
}
std::uint16_t Sdk::slot(std::string_view symbol) const {
  for (const auto& m : methods) if (m.symbol == symbol) return m.slot;
  throw std::runtime_error("Steam method absent from pinned MSVC table");
}
bool convert(const Layout& l, std::span<std::byte> to, std::span<const std::byte> from, bool guest) {
  if (to.size() != (guest ? l.guest_size : l.host_size) || from.size() != (guest ? l.host_size : l.guest_size)) return false;
  std::fill(to.begin(), to.end(), std::byte{});
  for (const auto& f : l.fields) {
    auto dest = guest ? f.guest : f.host, src = guest ? f.host : f.guest;
    if (dest + f.bytes > to.size() || src + f.bytes > from.size()) throw std::logic_error("invalid generated Steam layout");
    std::memcpy(to.data() + dest, from.data() + src, f.bytes);
  }
  return true;
}
} // namespace venture::steam
