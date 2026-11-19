#include "common/android/dex.h"

#include <cstring>
#include <stdexcept>

namespace venture::android::dex {
namespace {
[[noreturn]] void malformed() { throw std::runtime_error("Not a dex file."); }
struct Reader {
  std::span<const std::uint8_t> bytes;
  std::uint32_t u32(std::uint64_t at) const {
    if (at + 4 > bytes.size()) malformed();
    std::uint32_t value;
    std::memcpy(&value, bytes.data() + at, 4);
    return value;
  }
  // A string_data_item: ULEB128 UTF-16 length, then MUTF-8 bytes up to NUL.
  std::string string(std::uint32_t index, std::uint32_t ids, std::uint32_t count) const {
    if (index >= count) malformed();
    auto at = std::uint64_t(u32(ids + std::uint64_t(index) * 4));
    for (int shift = 0; at < bytes.size() && (bytes[at] & 0x80) && shift < 35; shift += 7) ++at;
    if (++at >= bytes.size()) malformed();
    const auto *begin = reinterpret_cast<const char *>(bytes.data() + at);
    return std::string(begin, strnlen(begin, bytes.size() - at));
  }
};
}  // namespace

std::vector<std::string> classes(std::span<const std::uint8_t> dex) {
  const Reader r{dex};
  if (dex.size() < 0x70 || std::memcmp(dex.data(), "dex\n", 4) != 0) malformed();
  const auto string_count = r.u32(0x38), string_ids = r.u32(0x3c);
  const auto type_count = r.u32(0x40), type_ids = r.u32(0x44);
  const auto class_count = r.u32(0x60), class_defs = r.u32(0x64);
  std::vector<std::string> out;
  out.reserve(class_count);
  for (std::uint32_t i = 0; i < class_count; ++i) {
    const auto type = r.u32(class_defs + std::uint64_t(i) * 32);  // class_def_item.class_idx
    if (type >= type_count) malformed();
    out.push_back(r.string(r.u32(type_ids + std::uint64_t(type) * 4), string_ids, string_count));
  }
  return out;
}
}  // namespace venture::android::dex
