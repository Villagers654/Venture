#include "java/elf_exports.h"
#include "core/elf.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace venture::java {
namespace {
class Image {
 public:
  explicit Image(const Library& library) : lib(library) {}
  const Library& lib;
  void check(GuestAddr p, std::uint64_t bytes) const {
    if (p < lib.map_start || p - lib.map_start > lib.map_size || bytes > lib.map_size - (p - lib.map_start))
      throw std::runtime_error("ELF JNI export table outside mapped image");
  }
  template <class T>
  T read(GuestAddr p) const {
    check(p, sizeof(T));
    T value;
    std::memcpy(&value, g2h(p), sizeof(value));
    return value;
  }
  GuestAddr pointer(GuestAddr p) const {
    // ELF gABI: p_vaddr and dynamic d_ptr are link-time virtual addresses.
    // Our loader preserves both in the wire image (core/elf.cpp). A32 image
    // ranges can overlap those values, so range membership cannot distinguish
    // a link-time address from a relocated one. Always apply the load bias.
    if (p > std::numeric_limits<GuestAddr>::max() - lib.load_bias)
      throw std::runtime_error("ELF pointer overflow");
    p += lib.load_bias;
    check(p, 1);
    return p;
  }
};
}  // namespace
template<class Phdr, class Dyn, class Sym, unsigned WordSize>
void enumerate(const Library& lib, const std::function<void(std::string_view, GuestAddr)>& visit) {
  Image image(lib);
  GuestAddr dynamic = 0;
  std::uint64_t dynamic_size = 0;
  for (unsigned i = 0; i < lib.phnum; ++i) {
    // The loader may allocate a separate copy when PHDRs are outside PT_LOAD.
    // Library::phdr is always guest wire storage, independently of map_start.
    Phdr p;
    std::memcpy(&p, g2h(lib.phdr + i * sizeof(Phdr)), sizeof(p));
    if (p.type == 2) {
      dynamic = image.pointer(p.vaddr);
      dynamic_size = p.memsz;
      break;
    }
  }
  if (!dynamic) throw std::runtime_error("ELF has no PT_DYNAMIC for JNI symbol enumeration");
  image.check(dynamic, dynamic_size);
  GuestAddr symbols = 0, strings = 0, hash = 0, gnu = 0;
  std::uint64_t strsize = 0, syment = sizeof(Sym);
  for (std::uint64_t i = 0; i + sizeof(Dyn) <= dynamic_size; i += sizeof(Dyn)) {
    auto d = image.read<Dyn>(dynamic + i);
    if (d.tag == 0) break;
    switch (d.tag) {
      case 4:
        hash = image.pointer(d.value);
        break;
      case 5:
        strings = image.pointer(d.value);
        break;
      case 6:
        symbols = image.pointer(d.value);
        break;
      case 10:
        strsize = d.value;
        break;
      case 11:
        syment = d.value;
        break;
      case 0x6ffffef5:
        gnu = image.pointer(d.value);
        break;
    }
  }
  if (!symbols || !strings || !strsize || syment != sizeof(Sym))
    throw std::runtime_error("incomplete ELF JNI dynamic symbols");
  image.check(strings, strsize);
  std::uint64_t count = 0;
  if (hash)
    count = image.read<std::uint32_t>(hash + 4);  // DT_HASH nchain
  else if (gnu) {
    auto buckets = image.read<std::uint32_t>(gnu);
    auto first = image.read<std::uint32_t>(gnu + 4);
    auto bloom = image.read<std::uint32_t>(gnu + 8);
    GuestAddr bucket_start = gnu + 16 + static_cast<std::uint64_t>(bloom) * WordSize;
    image.check(bucket_start, static_cast<std::uint64_t>(buckets) * 4);
    count = first;
    // All defined symbols after symoffset are consecutive. Walk each chain
    // with image bounds checks; low hash bit marks its last symbol.
    GuestAddr chains = bucket_start + static_cast<std::uint64_t>(buckets) * 4;
    for (std::uint32_t i = 0; i < buckets; ++i) {
      std::uint64_t index = image.read<std::uint32_t>(bucket_start + 4ull * i);
      if (!index) continue;
      if (index < first) throw std::runtime_error("invalid GNU ELF symbol bucket");
      for (;; ++index) {
        auto h = image.read<std::uint32_t>(chains + 4 * (index - first));
        count = std::max(count, index + 1);
        if (h & 1) break;
      }
    }
  } else
    throw std::runtime_error("ELF JNI enumeration requires DT_HASH or DT_GNU_HASH");
  if (count > lib.map_size / sizeof(Sym)) throw std::runtime_error("invalid ELF symbol count");
  image.check(symbols, count * sizeof(Sym));
  for (std::uint64_t i = 0; i < count; ++i) {
    auto s = image.read<Sym>(symbols + i * sizeof(Sym));
    unsigned binding = s.info >> 4, type = s.info & 15, visibility = s.other & 3;
    if (!s.shndx || (binding != 1 && binding != 2) || (type != 2 && type != 10) ||
        (visibility != 0 && visibility != 3))
      continue;
    if (s.name >= strsize) throw std::runtime_error("invalid ELF symbol name offset");
    const char* name = g2h<const char>(strings + s.name);
    auto* end = static_cast<const char*>(std::memchr(name, 0, strsize - s.name));
    if (!end) throw std::runtime_error("unterminated ELF symbol name");
    std::string_view view(name, end - name);
    if (view.starts_with("Java_")) visit(view, lib.find_symbol(view));  // handles ifunc in core
  }
}
void exported_java_symbols(const Library& lib,
                           const std::function<void(std::string_view, GuestAddr)>& visit) {
  Linker::get().prepare_java_exports(lib);
  if (lib.is_hle() && !lib.mapped_image()) return;
  if (lib.isa() == GuestIsa::a32) enumerate<elf::Phdr32, elf::Dyn32, elf::Sym32, 4>(lib, visit);
  else enumerate<elf::Phdr, elf::Dyn, elf::Sym, 8>(lib, visit);
}
}  // namespace venture::java
