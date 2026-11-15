#include "common/system_fonts.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <vector>

namespace venture::system_fonts {
namespace {
// Big-endian fields of an OpenType file (ISO/IEC 14496-22), read at absolute
// offsets; a read past the end leaves `failed` set and returns zeros.
class Reader {
 public:
  explicit Reader(const std::filesystem::path& file) : in_(file, std::ios::binary) {}
  bool ok() const { return bool(in_); }
  bool failed() const { return failed_; }
  std::vector<unsigned char> bytes(std::uint64_t at, std::uint64_t size) {
    std::vector<unsigned char> out(size);
    in_.clear();
    in_.seekg(std::streamoff(at));
    if (!in_.read(reinterpret_cast<char*>(out.data()), std::streamsize(size))) {
      failed_ = true;
      out.assign(size, 0);
    }
    return out;
  }
  std::uint32_t u32(std::uint64_t at) { return be(bytes(at, 4), 0, 4); }
  std::uint16_t u16(std::uint64_t at) { return std::uint16_t(be(bytes(at, 2), 0, 2)); }
  static std::uint32_t be(const std::vector<unsigned char>& data, std::uint64_t at, int size) {
    std::uint32_t value = 0;
    for (int i = 0; i < size; ++i) value = value << 8 | (at + i < data.size() ? data[at + i] : 0);
    return value;
  }

 private:
  std::ifstream in_;
  bool failed_ = false;
};
// Subtable formats 4 (BMP segments) and 12 (groups over all planes), read whole.
bool subtable_maps(Reader& r, std::uint64_t at, char32_t c) {
  const auto format = r.u16(at);
  if (format == 4) {
    if (c > 0xFFFF) return false;
    const auto t = r.bytes(at, r.u16(at + 2));
    if (r.failed()) return false;
    const auto u16 = [&](std::uint64_t offset) { return std::uint16_t(Reader::be(t, offset, 2)); };
    const unsigned segments = u16(6) / 2;
    const std::uint64_t ends = 14, starts = ends + 2 * segments + 2, deltas = starts + 2 * segments,
                        ranges = deltas + 2 * segments;
    for (unsigned i = 0; i < segments; ++i) {
      if (c > u16(ends + 2 * i)) continue;
      const auto start = u16(starts + 2 * i);
      if (c < start) return false;
      const auto range = u16(ranges + 2 * i);
      if (!range) return std::uint16_t(c + u16(deltas + 2 * i)) != 0;
      const auto glyph = u16(ranges + 2 * i + range + 2 * (c - start));
      return glyph && std::uint16_t(glyph + u16(deltas + 2 * i)) != 0;
    }
    return false;
  }
  if (format == 12) {
    const auto length = r.u32(at + 4);
    if (length < 16 || length > 1u << 24) return false;
    const auto t = r.bytes(at, length);
    if (r.failed()) return false;
    const auto groups = std::min<std::uint64_t>(Reader::be(t, 12, 4), (length - 16) / 12);
    for (std::uint64_t i = 0; i < groups; ++i)
      if (c >= Reader::be(t, 16 + 12 * i, 4) && c <= Reader::be(t, 20 + 12 * i, 4))
        return Reader::be(t, 24 + 12 * i, 4) != 0;
  }
  return false;
}
}  // namespace

bool covers(const Face& face, char32_t c) {
  Reader r(face.file);
  if (!r.ok()) return false;
  std::uint64_t font = 0;
  if (r.u32(0) == 0x74746366) {  // 'ttcf'
    if (face.index < 0 || std::uint32_t(face.index) >= r.u32(8)) return false;
    font = r.u32(12 + 4ull * face.index);
  } else if (face.index != 0) {
    return false;
  }
  const unsigned tables = r.u16(font + 4);
  std::uint64_t cmap = 0;
  for (unsigned i = 0; i < tables && !r.failed(); ++i)
    if (r.u32(font + 12 + 16ull * i) == 0x636D6170)  // 'cmap'
      cmap = r.u32(font + 12 + 16ull * i + 8);
  if (!cmap || r.failed()) return false;
  // Unicode encodings: platform 0 (Unicode), and Windows (3) BMP (1) or full (10).
  const unsigned encodings = r.u16(cmap + 2);
  for (unsigned i = 0; i < encodings && !r.failed(); ++i) {
    const auto record = cmap + 4 + 8ull * i;
    const auto platform = r.u16(record), encoding = r.u16(record + 2);
    if (platform != 0 && !(platform == 3 && (encoding == 1 || encoding == 10))) continue;
    if (subtable_maps(r, cmap + r.u32(record + 4), c)) return true;
  }
  return false;
}
}  // namespace venture::system_fonts
