#include "common/android/manifest.h"

#include "common/android/archive.h"

#include <miniz.h>

#include <climits>
#include <optional>
#include <vector>

namespace venture::android {
namespace {
// android.R.attr resource IDs: shrunk APKs may drop attribute names and keep
// only these, through the XML resource map.
constexpr std::uint32_t kLabel = 0x01010001, kIcon = 0x01010002, kBanner = 0x0101031f, kVersionCode = 0x0101021b, kVersionName = 0x0101021c,
                        kVersionCodeMajor = 0x01010576, kName = 0x01010003, kRequired = 0x0101028e;
constexpr std::uint64_t kTableLimit = 64u << 20;
constexpr std::uint8_t kReference = 0x01, kString = 0x03, kIntDec = 0x10, kIntHex = 0x11, kBoolean = 0x12;

void utf8_append(std::string &out, std::uint32_t c) {
  if (c < 0x80) {
    out += char(c);
  } else if (c < 0x800) {
    out += char(0xc0 | (c >> 6));
    out += char(0x80 | (c & 0x3f));
  } else if (c < 0x10000) {
    out += char(0xe0 | (c >> 12));
    out += char(0x80 | ((c >> 6) & 0x3f));
    out += char(0x80 | (c & 0x3f));
  } else {
    out += char(0xf0 | (c >> 18));
    out += char(0x80 | ((c >> 12) & 0x3f));
    out += char(0x80 | ((c >> 6) & 0x3f));
    out += char(0x80 | (c & 0x3f));
  }
}
struct Reader {
  std::span<const std::uint8_t> xml;
  std::uint16_t u16(std::size_t at) const {
    if (at > xml.size() || xml.size() - at < 2) throw std::runtime_error("The APK manifest is truncated.");
    return std::uint16_t(xml[at] | (xml[at + 1] << 8));
  }
  std::uint32_t u32(std::size_t at) const { return u16(at) | (std::uint32_t(u16(at + 2)) << 16); }
};
// A ResStringPool chunk (AOSP ResourceTypes.h), shared by binary XML and resources.arsc.
struct Pool {
  Reader r;
  std::size_t strings = 0, end = 0;
  bool utf8 = false;
  std::vector<std::uint32_t> offsets;
  void load(std::size_t at, std::size_t header, std::size_t size) {
    if (header < 28) throw std::runtime_error("The APK manifest is malformed.");
    const auto count = r.u32(at + 8), start = r.u32(at + 20);
    if (count > (size - header) / 4 || start < header + std::size_t(count) * 4 || start > size)
      throw std::runtime_error("The APK manifest is malformed.");
    utf8 = (r.u32(at + 16) & 0x100) != 0;
    strings = at + start;
    end = at + size;
    offsets.clear();
    for (std::size_t i = 0; i < count; ++i) offsets.push_back(r.u32(at + header + i * 4));
  }
  std::string get(std::uint32_t index) const {
    if (index == 0xffffffff) return {};
    if (index >= offsets.size() || offsets[index] >= end - strings)
      throw std::runtime_error("The APK manifest has an invalid string.");
    std::size_t at = strings + offsets[index];
    auto length = [&]() -> std::uint32_t {
      if (utf8) {
        if (at >= end) throw std::runtime_error("The APK manifest is truncated.");
        std::uint32_t n = r.xml[at++];
        if (n & 0x80) {
          if (at >= end) throw std::runtime_error("The APK manifest is truncated.");
          n = ((n & 0x7f) << 8) | r.xml[at++];
        }
        return n;
      }
      std::uint32_t n = r.u16(at);
      at += 2;
      if (n & 0x8000) {
        n = ((n & 0x7fff) << 16) | r.u16(at);
        at += 2;
      }
      return n;
    };
    std::string out;
    if (utf8) {
      length();  // UTF-16 length precedes the UTF-8 byte count
      const auto n = length();
      if (n > end - at) throw std::runtime_error("The APK manifest is truncated.");
      out.assign(reinterpret_cast<const char *>(r.xml.data() + at), n);
      return out;
    }
    const auto n = length();
    if (std::size_t(n) * 2 > end - at) throw std::runtime_error("The APK manifest is truncated.");
    for (std::size_t i = 0; i < n; ++i) {
      std::uint32_t c = r.u16(at + i * 2);
      if (c >= 0xd800 && c < 0xdc00 && i + 1 < n) {
        const std::uint32_t low = r.u16(at + (i + 1) * 2);
        if (low >= 0xdc00 && low < 0xe000) {
          c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
          ++i;
        }
      }
      utf8_append(out, c);
    }
    return out;
  }
};
}  // namespace

Manifest parse_manifest(std::span<const std::uint8_t> xml) {
  const Reader r{xml};
  if (r.u16(0) != 3 || r.u16(2) != 8 || r.u32(4) > xml.size())
    throw std::runtime_error("The APK has no binary AndroidManifest.xml.");
  const std::size_t end = r.u32(4);
  Pool pool{r};
  std::vector<std::uint32_t> resource_ids;
  auto string = [&](std::uint32_t index) { return pool.get(index); };
  Manifest m;
  std::int64_t major = 0;
  int depth = 0;
  for (std::size_t at = 8; at < end;) {
    const auto type = r.u16(at), header = r.u16(at + 2);
    const auto size = r.u32(at + 4);
    if (header < 8 || size < header || size > end - at) throw std::runtime_error("The APK manifest is malformed.");
    if (type == 0x0001 && !pool.strings) {  // string pool
      pool.load(at, header, size);
    } else if (type == 0x0180) {  // resource map: attribute name index -> resource ID
      for (std::size_t i = header; i + 4 <= size; i += 4) resource_ids.push_back(r.u32(at + i));
    } else if (type == 0x0102) {  // element start
      ++depth;
      if (!pool.strings || size - header < 20) throw std::runtime_error("The APK manifest is malformed.");
      const auto ext = at + header;
      const auto tag = string(r.u32(ext + 4));
      const auto start = r.u16(ext + 8), stride = r.u16(ext + 10), count = r.u16(ext + 12);
      if (start < 20 || stride < 20 || start > size - header || count > (size - header - start) / stride)
        throw std::runtime_error("The APK manifest is malformed.");
      const bool manifest = depth == 1 && tag == "manifest", application = depth == 2 && tag == "application";
      const bool feature = depth == 2 && tag == "uses-feature";
      std::string feature_name;
      bool feature_required = true;  // android:required defaults to true
      for (std::size_t i = 0; (manifest || application || feature) && i < count; ++i) {
        const auto attr = ext + start + i * stride;
        const auto name_index = r.u32(attr + 4);
        const auto name = string(name_index);
        const auto id = name_index < resource_ids.size() ? resource_ids[name_index] : 0;
        const auto raw = r.u32(attr + 8);
        const std::uint8_t data_type = xml[attr + 15];
        const auto data = r.u32(attr + 16);
        auto text = [&] { return raw != 0xffffffff ? string(raw) : data_type == kString ? string(data) : ""; };
        const bool integer = data_type == kIntDec || data_type == kIntHex;
        if (manifest && name == "package" && id == 0) {
          m.package = text();
        } else if (manifest && (id == kVersionCode || (!id && name == "versionCode"))) {
          if (integer) m.version_code = data;
          else if (const auto v = text(); !v.empty() && digits(v)) m.version_code = std::stoll(v);
        } else if (manifest && (id == kVersionCodeMajor || (!id && name == "versionCodeMajor"))) {
          if (integer) major = data;
        } else if (manifest && (id == kVersionName || (!id && name == "versionName"))) {
          m.version_name = text();
        } else if (application && (id == kLabel || (!id && name == "label"))) {
          if (data_type == kReference && raw == 0xffffffff) m.label_id = data;
          else m.label = text();
        } else if (application && data_type == kReference && (id == kBanner || (!id && name == "banner"))) {
          m.banner_id = data;
        } else if (application && data_type == kReference && (id == kIcon || (!id && name == "icon"))) {
          m.icon_id = data;
        } else if (feature && (id == kName || (!id && name == "name"))) {
          feature_name = text();
        } else if (feature && (id == kRequired || (!id && name == "required"))) {
          if (data_type == kBoolean) feature_required = data != 0;
          else if (const auto v = text(); !v.empty()) feature_required = v != "false";
        }
      }
      if (feature && feature_required && !feature_name.empty()) m.required_features.push_back(feature_name);
    } else if (type == 0x0103) {  // element end
      --depth;
    }
    at += size;
  }
  if (major) m.version_code |= major << 32;
  if (!paths::safe_package(m.package)) throw std::runtime_error("The APK manifest names no valid package.");
  return m;
}
Manifest read_apk(const fs::path &apk) {
  Archive archive(apk);
  const auto *entry = archive.find("AndroidManifest.xml");
  if (!entry) throw std::runtime_error(paths::utf8(apk.filename()) + " is not an Android app (no manifest).");
  auto m = parse_manifest(archive.read(*entry));
  if (m.label.empty() && m.label_id)
    if (const auto *table = archive.find("resources.arsc")) try {
        m.label = resource_string(archive.read(*table, kTableLimit), m.label_id);
      } catch (const std::exception &) {
      }
  return m;
}
Manifest read_apk(std::span<const std::uint8_t> apk) {
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_mem(&zip, apk.data(), apk.size(), 0)) throw std::runtime_error("The APK is not a ZIP archive.");
  struct End {
    mz_zip_archive *zip;
    ~End() { mz_zip_reader_end(zip); }
  } end{&zip};
  const int index = mz_zip_reader_locate_file(&zip, "AndroidManifest.xml", nullptr, 0);
  mz_zip_archive_file_stat stat{};
  if (index < 0 || !mz_zip_reader_file_stat(&zip, mz_uint(index), &stat) || stat.m_uncomp_size > (16u << 20))
    throw std::runtime_error("The APK has no readable manifest.");
  std::vector<std::uint8_t> xml(stat.m_uncomp_size);
  if (!mz_zip_reader_extract_to_mem(&zip, mz_uint(index), xml.data(), xml.size(), 0))
    throw std::runtime_error("The APK manifest could not be read.");
  auto m = parse_manifest(xml);
  const int table = mz_zip_reader_locate_file(&zip, "resources.arsc", nullptr, 0);
  if (m.label.empty() && m.label_id && table >= 0 && mz_zip_reader_file_stat(&zip, mz_uint(table), &stat) &&
      stat.m_uncomp_size <= kTableLimit) {
    std::vector<std::uint8_t> arsc(stat.m_uncomp_size);
    if (mz_zip_reader_extract_to_mem(&zip, mz_uint(table), arsc.data(), arsc.size(), 0)) try {
        m.label = resource_string(arsc, m.label_id);
      } catch (const std::exception &) {
      }
  }
  return m;
}

namespace {
// One value of a resource in one configuration (ResTable_config: language,
// country and density are what a label or a launcher image varies by).
struct Candidate {
  std::uint16_t language = 0, country = 0, density = 0;
  std::uint8_t type = 0;
  std::uint32_t data = 0;
};
struct Table {
  Reader r;
  Pool strings{r};  // the table's global value pool
  std::vector<std::size_t> packages;
  explicit Table(std::span<const std::uint8_t> arsc) : r{arsc} {
    if (r.u16(0) != 2 || r.u32(4) > arsc.size()) throw std::runtime_error("The APK has no resource table.");
    const std::size_t header = r.u16(2), end = r.u32(4);
    for (std::size_t at = header; at < end;) {
      const auto type = r.u16(at), chunk_header = r.u16(at + 2);
      const auto size = r.u32(at + 4);
      if (chunk_header < 8 || size < chunk_header || size > end - at)
        throw std::runtime_error("The resource table is malformed.");
      if (type == 0x0001 && !strings.strings) strings.load(at, chunk_header, size);
      if (type == 0x0200) packages.push_back(at);
      at += size;
    }
  }
  std::vector<Candidate> find(std::uint32_t id) const {
    std::vector<Candidate> out;
    const auto entry = id & 0xffff;
    const auto type_id = std::uint8_t(id >> 16);
    for (auto package : packages) {
      if (r.u32(package + 8) != id >> 24) continue;
      const std::size_t end = package + r.u32(package + 4);
      for (std::size_t at = package + r.u16(package + 2); at < end;) {
        const auto type = r.u16(at), header = r.u16(at + 2);
        const auto size = r.u32(at + 4);
        if (header < 8 || size < header || size > end - at) throw std::runtime_error("The resource table is malformed.");
        if (type == 0x0201 && header >= 36 && r.xml[at + 8] == type_id) candidate(at, header, size, entry, out);
        at += size;
      }
    }
    return out;
  }
  // A ResTable_type chunk's value for `entry`, if it has one.
  void candidate(std::size_t at, std::size_t header, std::size_t size, std::uint32_t entry,
                 std::vector<Candidate> &out) const {
    const std::uint8_t flags = r.xml[at + 9];
    const std::size_t count = r.u32(at + 12), entries = r.u32(at + 16);
    if (entries > size || r.u32(at + 20) < 16) return;
    std::size_t offset = SIZE_MAX;
    if (flags & 0x01) {  // sparse: (index, offset / 4) pairs
      for (std::size_t i = 0; i < count && header + i * 4 + 4 <= entries; ++i)
        if (r.u16(at + header + i * 4) == entry) offset = r.u16(at + header + i * 4 + 2) * std::size_t(4);
    } else if (entry < count && (flags & 0x02)) {  // 16-bit offsets / 4
      if (const auto o = r.u16(at + header + entry * 2); o != 0xffff) offset = o * std::size_t(4);
    } else if (entry < count) {
      if (const auto o = r.u32(at + header + entry * 4); o != 0xffffffff) offset = o;
    }
    if (offset == SIZE_MAX || offset + 8 > size - entries) return;
    const auto e = at + entries + offset;
    const auto entry_size = r.u16(e), entry_flags = r.u16(e + 2);
    // ResTable_config starts at +20: language +8, country +10, density +14.
    Candidate c{r.u16(at + 28), r.u16(at + 30), r.u16(at + 34)};
    if (entry_flags & 0x0008) {  // compact: the type sits in the flags' high byte
      c.type = std::uint8_t(entry_flags >> 8);
      c.data = r.u32(e + 4);
    } else if (entry_flags & 0x0001) {  // a bag (style, array): not a single value
      return;
    } else {
      c.type = r.xml[e + entry_size + 3];
      c.data = r.u32(e + entry_size + 4);
    }
    out.push_back(c);
  }
};
constexpr std::uint16_t chars(char a, char b) { return std::uint16_t(std::uint8_t(a) | std::uint8_t(b) << 8); }
// Follows references to the chosen value; `better` orders two candidates.
template <class Better>
std::optional<Candidate> resolve(const Table &table, std::uint32_t id, Better better) {
  for (int hop = 0; hop < 8; ++hop) {
    auto found = table.find(id);
    if (found.empty()) return std::nullopt;
    auto best = found.front();
    for (auto &c : found)
      if (better(c, best)) best = c;
    if (best.type != kReference) return best;
    id = best.data;
  }
  return std::nullopt;
}
// An en-US device's choice: en-US, then en, then another English, then the default.
int locale_rank(const Candidate &c) {
  if (c.language == chars('e', 'n')) return c.country == chars('U', 'S') ? 4 : c.country == 0 ? 3 : 2;
  return c.language == 0 ? 1 : 0;
}
}  // namespace

std::string resource_string(std::span<const std::uint8_t> arsc, std::uint32_t id) {
  const Table table(arsc);
  auto value = resolve(table, id, [](const Candidate &a, const Candidate &b) { return locale_rank(a) > locale_rank(b); });
  return value && value->type == kString ? table.strings.get(value->data) : std::string{};
}
std::string resource_file(std::span<const std::uint8_t> arsc, std::uint32_t id) {
  const Table table(arsc);
  auto raster = [&](const Candidate &c) {
    if (c.type != kString) return false;
    const auto path = table.strings.get(c.data);
    for (auto ext : {".png", ".jpg", ".jpeg", ".webp"})
      if (path.ends_with(ext)) return true;
    return false;
  };
  // The densest raster wins; anydpi and nodpi entries are usually vector or
  // adaptive XML, which raster() rejects.
  auto value = resolve(table, id, [&](const Candidate &a, const Candidate &b) {
    if (raster(a) != raster(b)) return raster(a);
    return a.density > b.density;
  });
  return value && raster(*value) ? table.strings.get(value->data) : std::string{};
}
Presentation read_presentation(const fs::path &apk) {
  Presentation p;
  Archive archive(apk);
  const auto *entry = archive.find("AndroidManifest.xml");
  if (!entry) return p;
  const auto m = parse_manifest(archive.read(*entry));
  p.label = m.label;
  const auto *table = archive.find("resources.arsc");
  if (!table) return p;
  const auto arsc = archive.read(*table, kTableLimit);
  if (p.label.empty() && m.label_id) p.label = resource_string(arsc, m.label_id);
  auto image = [&](std::uint32_t id) {
    const auto file = id ? resource_file(arsc, id) : std::string{};
    const auto *member = file.empty() ? nullptr : archive.find(file);
    return member ? archive.read(*member, 8u << 20) : std::vector<std::uint8_t>{};
  };
  p.banner = image(m.banner_id);
  p.icon = image(m.icon_id);
  return p;
}
}  // namespace venture::android
