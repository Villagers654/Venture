#include "common/android/apk_rewrite.h"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace venture::android {
namespace {
constexpr std::uint32_t kLocal = 0x04034b50, kCentral = 0x02014b50, kEnd = 0x06054b50;
constexpr std::uint16_t kAlignmentField = 0xd935;  // zipalign's extra field
constexpr std::uint64_t kLimit = std::numeric_limits<std::uint32_t>::max();

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("APK rewrite: " + what); }

void put16(std::vector<std::uint8_t>& out, std::uint32_t v) {
  out.push_back(std::uint8_t(v));
  out.push_back(std::uint8_t(v >> 8));
}
void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  put16(out, v & 0xffff);
  put16(out, v >> 16);
}
std::uint32_t get16(const std::uint8_t* p) { return std::uint32_t(p[0] | p[1] << 8); }
std::uint32_t get32(const std::uint8_t* p) { return get16(p) | get16(p + 2) << 16; }

struct Written {
  std::string name;
  std::uint16_t flags = 0, method = 0, time = 0, date = 0;
  std::uint32_t crc = 0;
  std::uint64_t compressed = 0, size = 0, offset = 0;
};

class Writer {
 public:
  explicit Writer(const fs::path& path) : out_(path, std::ios::binary | std::ios::trunc) {
    if (!out_) fail("cannot write " + path.string());
  }
  // Writes a local header for `entry` (offset filled in), padded so its data
  // starts aligned when it is stored.
  void header(Written& entry) {
    entry.offset = position_;
    if (entry.offset > kLimit || entry.compressed > kLimit || entry.size > kLimit) fail("ZIP64 is not supported");
    std::vector<std::uint8_t> h;
    put32(h, kLocal);
    put16(h, entry.method ? 20 : 10);
    put16(h, entry.flags);
    put16(h, entry.method);
    put16(h, entry.time);
    put16(h, entry.date);
    put32(h, entry.crc);
    put32(h, std::uint32_t(entry.compressed));
    put32(h, std::uint32_t(entry.size));
    put16(h, std::uint32_t(entry.name.size()));
    std::vector<std::uint8_t> extra;
    if (entry.method == 0) {
      const auto alignment = stored_alignment(entry.name);
      const auto start = position_ + 30 + entry.name.size() + 6;
      const auto pad = std::uint32_t((alignment - start % alignment) % alignment);
      put16(extra, kAlignmentField);
      put16(extra, 2 + pad);
      put16(extra, alignment);
      extra.resize(extra.size() + pad, 0);
    }
    put16(h, std::uint32_t(extra.size()));
    h.insert(h.end(), entry.name.begin(), entry.name.end());
    h.insert(h.end(), extra.begin(), extra.end());
    write(h.data(), h.size());
  }
  void write(const void* data, std::size_t size) {
    out_.write(static_cast<const char*>(data), std::streamsize(size));
    if (!out_) fail("write failed");
    position_ += size;
  }
  void finish(const std::vector<Written>& entries) {
    const auto start = position_;
    std::vector<std::uint8_t> c;
    for (const auto& e : entries) {
      put32(c, kCentral);
      put16(c, 20);  // made by: MS-DOS, ZIP 2.0
      put16(c, e.method ? 20 : 10);
      put16(c, e.flags);
      put16(c, e.method);
      put16(c, e.time);
      put16(c, e.date);
      put32(c, e.crc);
      put32(c, std::uint32_t(e.compressed));
      put32(c, std::uint32_t(e.size));
      put16(c, std::uint32_t(e.name.size()));
      put16(c, 0);
      put16(c, 0);
      put16(c, 0);
      put16(c, 0);
      put32(c, 0);
      put32(c, std::uint32_t(e.offset));
      c.insert(c.end(), e.name.begin(), e.name.end());
    }
    if (entries.size() > 0xffff || start > kLimit) fail("ZIP64 is not supported");
    put32(c, kEnd);
    put16(c, 0);
    put16(c, 0);
    put16(c, std::uint32_t(entries.size()));
    put16(c, std::uint32_t(entries.size()));
    put32(c, std::uint32_t(c.size() - 0));  // patched below
    put32(c, std::uint32_t(start));
    put16(c, 0);
    const auto directory = std::uint32_t(c.size() - 22);
    std::memcpy(c.data() + c.size() - 10, &directory, 4);
    write(c.data(), c.size());
    out_.flush();
    if (!out_) fail("write failed");
  }

 private:
  std::ofstream out_;
  std::uint64_t position_ = 0;
};

// DOS time and date for entries Venture adds: a fixed stamp keeps the output
// reproducible.
constexpr std::uint16_t kTime = 0, kDate = (2008 - 1980) << 9 | 1 << 5 | 1;

bool is_signature(const std::string& name) {
  if (!name.starts_with("META-INF/") || name.find('/', 9) != std::string::npos) return false;
  auto upper = name;
  std::ranges::transform(upper, upper.begin(), [](unsigned char c) { return char(std::toupper(c)); });
  return upper == "META-INF/MANIFEST.MF" || upper.ends_with(".SF") || upper.ends_with(".RSA") ||
         upper.ends_with(".DSA") || upper.ends_with(".EC");
}
}  // namespace

std::uint32_t stored_alignment(const std::string& name) {
  return name.starts_with("lib/") && name.ends_with(".so") ? 16384 : 4;
}

bool kept_from_game(const std::string& name, const ApkEdit& edit) {
  return !is_signature(name) && !edit.bytes.contains(name) && !edit.files.contains(name);
}

void rewrite_apk(const fs::path& source, const ApkEdit& edit, const fs::path& out, std::stop_token stop) {
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, source.string().c_str(), 0)) fail("cannot read " + source.string());
  struct Close {
    mz_zip_archive* zip;
    ~Close() { mz_zip_reader_end(zip); }
  } close{&zip};
  std::ifstream in(source, std::ios::binary);
  if (!in) fail("cannot read " + source.string());
  Writer w(out);
  std::vector<Written> entries;
  std::vector<char> buffer(1 << 20);

  // The game's entries, in their order, data copied as stored.
  const auto count = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < count; ++i) {
    if (stop.stop_requested()) fail("stopped");
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&zip, i, &stat)) fail("bad central directory");
    const std::string name = stat.m_filename;
    if (stat.m_is_directory || !kept_from_game(name, edit)) continue;
    std::uint8_t local[30];
    in.seekg(std::streamoff(stat.m_local_header_ofs));
    if (!in.read(reinterpret_cast<char*>(local), 30) || get32(local) != kLocal) fail("bad local header: " + name);
    const auto data = stat.m_local_header_ofs + 30 + get16(local + 26) + get16(local + 28);
    Written e{name};
    e.flags = std::uint16_t(stat.m_bit_flag & ~0x8u);  // sizes in the header: no data descriptor
    e.method = std::uint16_t(stat.m_method);
    e.time = std::uint16_t(get16(local + 10));
    e.date = std::uint16_t(get16(local + 12));
    e.crc = stat.m_crc32;
    e.compressed = stat.m_comp_size;
    e.size = stat.m_uncomp_size;
    if (e.method != 0 && e.method != 8) fail("unsupported compression: " + name);
    if (stat.m_bit_flag & 1) fail("encrypted entry: " + name);
    w.header(e);
    in.seekg(std::streamoff(data));
    for (auto left = e.compressed; left;) {
      const auto n = std::size_t(std::min<std::uint64_t>(left, buffer.size()));
      if (!in.read(buffer.data(), std::streamsize(n))) fail("short entry: " + name);
      w.write(buffer.data(), n);
      left -= n;
    }
    entries.push_back(e);
  }

  // Venture's: libraries stored (loaded straight from the APK), the rest deflated.
  for (const auto& [name, bytes] : edit.bytes) {
    Written e{name};
    e.time = kTime;
    e.date = kDate;
    e.size = bytes.size();
    e.crc = std::uint32_t(mz_crc32(MZ_CRC32_INIT, bytes.data(), bytes.size()));
    if (stored_alignment(name) > 4) {
      e.compressed = e.size;
      w.header(e);
      w.write(bytes.data(), bytes.size());
    } else {
      std::size_t length = 0;
      void* packed = tdefl_compress_mem_to_heap(bytes.data(), bytes.size(), &length, TDEFL_DEFAULT_MAX_PROBES);
      if (!packed && !bytes.empty()) fail("deflate failed: " + name);
      e.method = 8;
      e.compressed = length;
      w.header(e);
      w.write(packed, length);
      mz_free(packed);
    }
    entries.push_back(e);
  }
  for (const auto& [name, path] : edit.files) {
    std::ifstream file(path, std::ios::binary);
    if (!file) fail("cannot read " + path.string());
    Written e{name};
    e.time = kTime;
    e.date = kDate;
    e.size = e.compressed = fs::file_size(path);
    auto crc = MZ_CRC32_INIT;
    while (file.read(buffer.data(), std::streamsize(buffer.size())) || file.gcount())
      crc = mz_crc32(crc, reinterpret_cast<const std::uint8_t*>(buffer.data()), std::size_t(file.gcount()));
    e.crc = std::uint32_t(crc);
    w.header(e);
    file.clear();
    file.seekg(0);
    while (file.read(buffer.data(), std::streamsize(buffer.size())) || file.gcount())
      w.write(buffer.data(), std::size_t(file.gcount()));
    entries.push_back(e);
  }
  w.finish(entries);
}
}  // namespace venture::android
