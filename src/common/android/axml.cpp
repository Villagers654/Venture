#include "common/android/axml.h"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

namespace venture::android::axml {
namespace {
constexpr std::uint16_t kXml = 0x0003, kStringPool = 0x0001, kResourceMap = 0x0180, kNamespaceStart = 0x0100,
                        kNamespaceEnd = 0x0101, kElementStart = 0x0102, kElementEnd = 0x0103, kCdata = 0x0104;
constexpr std::uint32_t kNone = 0xffffffff, kUtf8 = 0x100;

[[noreturn]] void malformed() { throw std::runtime_error("The APK manifest is malformed."); }

struct Reader {
  std::span<const std::uint8_t> bytes;
  std::uint16_t u16(std::size_t at) const {
    if (at > bytes.size() || bytes.size() - at < 2) malformed();
    return std::uint16_t(bytes[at] | bytes[at + 1] << 8);
  }
  std::uint32_t u32(std::size_t at) const { return u16(at) | std::uint32_t(u16(at + 2)) << 16; }
};

void utf8_append(std::string& out, std::uint32_t c) {
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
// UTF-8 to UTF-16 code units; invalid bytes become U+FFFD.
std::vector<std::uint16_t> utf16(std::string_view s) {
  std::vector<std::uint16_t> out;
  for (std::size_t i = 0; i < s.size();) {
    const auto b = std::uint8_t(s[i]);
    std::uint32_t c = 0xfffd;
    std::size_t n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
    if (n == 0 || i + n > s.size()) {
      n = 1;
    } else {
      c = n == 1 ? b : b & (0x7f >> n);
      for (std::size_t k = 1; k < n; ++k) c = c << 6 | (std::uint8_t(s[i + k]) & 0x3f);
    }
    i += n;
    if (c >= 0x10000) {
      c -= 0x10000;
      out.push_back(std::uint16_t(0xd800 + (c >> 10)));
      out.push_back(std::uint16_t(0xdc00 + (c & 0x3ff)));
    } else {
      out.push_back(std::uint16_t(c));
    }
  }
  return out;
}

std::vector<std::string> read_pool(const Reader& r, std::size_t at, std::size_t header, std::size_t size) {
  const auto count = r.u32(at + 8), styles = r.u32(at + 12), flags = r.u32(at + 16);
  const auto start = r.u32(at + 20);
  if (styles) throw std::runtime_error("The APK manifest has styled strings.");
  if (count > (size - header) / 4 || start > size) malformed();
  const bool utf8 = flags & kUtf8;
  std::vector<std::string> strings;
  strings.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::size_t p = at + start + r.u32(at + header + i * 4);
    const std::size_t end = at + size;
    if (p >= end) malformed();
    std::string out;
    if (utf8) {
      auto length = [&] {
        if (p >= end) malformed();
        std::uint32_t n = r.bytes[p++];
        if (n & 0x80) {
          if (p >= end) malformed();
          n = ((n & 0x7f) << 8) | r.bytes[p++];
        }
        return n;
      };
      length();  // UTF-16 length
      const auto n = length();
      if (n > end - p) malformed();
      out.assign(reinterpret_cast<const char*>(r.bytes.data() + p), n);
    } else {
      std::uint32_t n = r.u16(p);
      p += 2;
      if (n & 0x8000) {
        n = ((n & 0x7fff) << 16) | r.u16(p);
        p += 2;
      }
      if (std::size_t(n) * 2 > end - p) malformed();
      for (std::uint32_t k = 0; k < n; ++k) {
        std::uint32_t c = r.u16(p + k * 2);
        if (c >= 0xd800 && c < 0xdc00 && k + 1 < n) {
          const std::uint32_t low = r.u16(p + (k + 1) * 2);
          if (low >= 0xdc00 && low < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
            ++k;
          }
        }
        utf8_append(out, c);
      }
    }
    strings.push_back(std::move(out));
  }
  return strings;
}

bool before(const Attribute& a, const Attribute& b) {
  // aapt2's order: resource IDs ascending, then attributes without one.
  if (a.resource_id && b.resource_id) return a.resource_id < b.resource_id;
  return a.resource_id && !b.resource_id;
}
}  // namespace

const Attribute* Node::attribute(std::uint32_t resource_id, std::string_view local) const {
  for (const auto& a : attributes)
    if (resource_id ? a.resource_id == resource_id : (!a.resource_id && a.name == local)) return &a;
  return nullptr;
}

void Node::set(std::uint32_t resource_id, std::string_view local, std::string value) {
  for (auto& a : attributes)
    if (a.resource_id == resource_id && (resource_id || a.name == local)) {
      a.type = kString;
      a.raw = value;
      a.string = std::move(value);
      a.data = 0;
      return;
    }
  Attribute a{.ns = std::string(kAndroidNamespace), .name = std::string(local), .resource_id = resource_id,
              .raw = value, .type = kString, .string = std::move(value)};
  attributes.insert(std::ranges::upper_bound(attributes, a, before), std::move(a));
}

void Node::set_value(std::uint32_t resource_id, std::string_view local, std::uint8_t type, std::uint32_t data) {
  for (auto& a : attributes)
    if (a.resource_id == resource_id && (resource_id || a.name == local)) {
      a.type = type;
      a.raw.reset();
      a.string.clear();
      a.data = data;
      return;
    }
  Attribute a{.ns = std::string(kAndroidNamespace), .name = std::string(local), .resource_id = resource_id,
              .type = type, .data = data};
  attributes.insert(std::ranges::upper_bound(attributes, a, before), std::move(a));
}

std::string text(const Attribute& attribute) {
  if (attribute.type == kString) return attribute.string;
  return attribute.raw.value_or(std::string());
}

Document parse(std::span<const std::uint8_t> xml) {
  const Reader r{xml};
  if (r.u16(0) != kXml || r.u16(2) != 8 || r.u32(4) > xml.size()) malformed();
  const std::size_t end = r.u32(4);
  std::vector<std::string> strings;
  std::vector<std::uint32_t> ids;
  auto string = [&](std::uint32_t index) -> std::string {
    if (index == kNone) return {};
    if (index >= strings.size()) malformed();
    return strings[index];
  };
  Document document;
  std::vector<Node*> open;
  std::vector<Namespace> pending;
  bool have_root = false;
  for (std::size_t at = 8; at < end;) {
    const auto type = r.u16(at), header = r.u16(at + 2);
    const auto size = r.u32(at + 4);
    if (header < 8 || size < header || size > end - at) malformed();
    switch (type) {
      case kStringPool:
        if (!strings.empty()) malformed();
        strings = read_pool(r, at, header, size);
        break;
      case kResourceMap:
        for (std::size_t i = header; i + 4 <= size; i += 4) ids.push_back(r.u32(at + i));
        break;
      case kNamespaceStart:
        if (size < 24) malformed();
        pending.push_back({string(r.u32(at + 16)), string(r.u32(at + 20))});
        break;
      case kNamespaceEnd:
        break;
      case kElementStart: {
        if (size - header < 20) malformed();
        const auto ext = at + header;
        Node node;
        node.line = r.u32(at + 8);
        node.ns = string(r.u32(ext));
        node.name = string(r.u32(ext + 4));
        node.namespaces = std::move(pending);
        pending.clear();
        const auto start = r.u16(ext + 8), stride = r.u16(ext + 10), count = r.u16(ext + 12);
        if (start < 20 || stride < 20 || start > size - header || count > (size - header - start) / stride)
          malformed();
        for (std::size_t i = 0; i < count; ++i) {
          const auto p = ext + start + i * stride;
          Attribute a;
          a.ns = string(r.u32(p));
          const auto name = r.u32(p + 4);
          a.name = string(name);
          a.resource_id = name < ids.size() ? ids[name] : 0;
          if (const auto raw = r.u32(p + 8); raw != kNone) a.raw = string(raw);
          a.type = xml[p + 15];
          a.data = r.u32(p + 16);
          if (a.type == kString) a.string = string(a.data);
          node.attributes.push_back(std::move(a));
        }
        Node* placed;
        if (open.empty()) {
          if (have_root) malformed();
          have_root = true;
          document.root = std::move(node);
          placed = &document.root;
        } else {
          placed = &open.back()->children.emplace_back(std::move(node));
        }
        open.push_back(placed);
        break;
      }
      case kElementEnd:
        if (open.empty()) malformed();
        open.pop_back();
        break;
      case kCdata:
        if (open.empty() || size < 28) malformed();
        open.back()->children.push_back(Node{.text = true, .value = string(r.u32(at + 16)), .line = r.u32(at + 8)});
        break;
      default:
        throw std::runtime_error("The APK manifest has an unknown chunk.");
    }
    at += size;
  }
  if (!have_root || !open.empty()) malformed();
  return document;
}

std::vector<std::uint8_t> serialize(const Document& document) {
  // Attribute names with resource IDs come first: the resource map indexes
  // the pool. The same local name with two IDs needs two entries.
  std::vector<std::string> strings;
  std::vector<std::uint32_t> ids;
  std::map<std::pair<std::string, std::uint32_t>, std::uint32_t> named;
  std::map<std::string, std::uint32_t> plain;
  auto visit = [&](auto&& self, const Node& node) -> void {
    for (const auto& a : node.attributes)
      if (a.resource_id && named.try_emplace({a.name, a.resource_id}, std::uint32_t(strings.size())).second) {
        strings.push_back(a.name);
        ids.push_back(a.resource_id);
      }
    for (const auto& child : node.children) self(self, child);
  };
  visit(visit, document.root);
  auto intern = [&](const std::string& s) {
    auto [it, added] = plain.try_emplace(s, std::uint32_t(strings.size()));
    if (added) strings.push_back(s);
    return it->second;
  };
  auto optional = [&](const std::string& s) { return s.empty() ? kNone : intern(s); };

  std::vector<std::uint8_t> body;
  auto u16 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(std::uint8_t(x));
    v.push_back(std::uint8_t(x >> 8));
  };
  auto u32 = [&](std::vector<std::uint8_t>& v, std::uint32_t x) {
    u16(v, x & 0xffff);
    u16(v, x >> 16);
  };
  auto chunk_header = [&](std::uint16_t type, std::uint32_t size, std::uint32_t line) {
    u16(body, type);
    u16(body, 16);
    u32(body, size);
    u32(body, line);
    u32(body, kNone);  // comment
  };
  auto emit = [&](auto&& self, const Node& node) -> void {
    if (node.text) {
      chunk_header(kCdata, 28, node.line);
      u32(body, intern(node.value));
      u16(body, 8);
      body.push_back(0);
      body.push_back(kNull);
      u32(body, 0);
      return;
    }
    for (const auto& n : node.namespaces) {
      chunk_header(kNamespaceStart, 24, node.line);
      u32(body, optional(n.prefix));
      u32(body, intern(n.uri));
    }
    auto attributes = node.attributes;
    std::ranges::stable_sort(attributes, before);
    std::uint16_t id = 0, klass = 0, style = 0;
    for (std::size_t i = 0; i < attributes.size(); ++i) {
      if (!attributes[i].ns.empty()) continue;
      const auto& n = attributes[i].name;
      if (n == "id") id = std::uint16_t(i + 1);
      if (n == "class") klass = std::uint16_t(i + 1);
      if (n == "style") style = std::uint16_t(i + 1);
    }
    const auto ns = optional(node.ns), name = intern(node.name);
    chunk_header(kElementStart, 16 + 20 + std::uint32_t(attributes.size()) * 20, node.line);
    u32(body, ns);
    u32(body, name);
    u16(body, 20);
    u16(body, 20);
    u16(body, std::uint32_t(attributes.size()));
    u16(body, id);
    u16(body, klass);
    u16(body, style);
    for (const auto& a : attributes) {
      const auto attr_name = a.resource_id ? named.at({a.name, a.resource_id}) : intern(a.name);
      const auto value = a.type == kString ? intern(a.string) : a.data;
      const auto raw = a.type == kString ? value : a.raw ? intern(*a.raw) : kNone;
      u32(body, optional(a.ns));
      u32(body, attr_name);
      u32(body, raw);
      u16(body, 8);
      body.push_back(0);
      body.push_back(a.type);
      u32(body, value);
    }
    for (const auto& child : node.children) self(self, child);
    chunk_header(kElementEnd, 24, node.line);
    u32(body, ns);
    u32(body, name);
    for (auto n = node.namespaces.rbegin(); n != node.namespaces.rend(); ++n) {
      chunk_header(kNamespaceEnd, 24, node.line);
      u32(body, optional(n->prefix));
      u32(body, intern(n->uri));
    }
  };
  emit(emit, document.root);

  // String pool, UTF-16 like aapt's manifests.
  std::vector<std::uint8_t> data;
  std::vector<std::uint32_t> offsets;
  for (const auto& s : strings) {
    offsets.push_back(std::uint32_t(data.size()));
    const auto units = utf16(s);
    if (units.size() > 0x7fff) {
      u16(data, 0x8000 | std::uint32_t(units.size() >> 16));
      u16(data, std::uint32_t(units.size() & 0xffff));
    } else {
      u16(data, std::uint32_t(units.size()));
    }
    for (auto unit : units) u16(data, unit);
    u16(data, 0);
  }
  while (data.size() % 4) data.push_back(0);
  std::vector<std::uint8_t> out;
  const std::uint32_t pool_header = 28, start = pool_header + std::uint32_t(offsets.size()) * 4;
  const std::uint32_t pool_size = start + std::uint32_t(data.size()), map_size = 8 + std::uint32_t(ids.size()) * 4;
  u16(out, kXml);
  u16(out, 8);
  u32(out, 8 + pool_size + map_size + std::uint32_t(body.size()));
  u16(out, kStringPool);
  u16(out, pool_header);
  u32(out, pool_size);
  u32(out, std::uint32_t(strings.size()));
  u32(out, 0);
  u32(out, 0);
  u32(out, start);
  u32(out, 0);
  for (auto o : offsets) u32(out, o);
  out.insert(out.end(), data.begin(), data.end());
  u16(out, kResourceMap);
  u16(out, 8);
  u32(out, map_size);
  for (auto id : ids) u32(out, id);
  out.insert(out.end(), body.begin(), body.end());
  return out;
}
}  // namespace venture::android::axml
