// ILP32 OpenXR for A32 guests. The generated tables (tools/codegen/xr_a32.py)
// give each registry structure its ILP32 and LP64 layout; a call converts the
// guest's structures (chains, arrays, pointer arrays) into host LP64 copies,
// runs the A64 implementation unchanged, then writes the non-pointer members
// of every writable structure back in the guest layout. Handles, atoms and
// XrTime are 64-bit on both ABIs and need no tokens.
#include "common/log.h"
#include "core/a32_abi.h"
#include "core/process.h"
#include "hle/openxr/openxr.h"
#include <bit>
#include <cstddef>
#include <cstring>
#include <memory_resource>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace venture::hle::openxr {
namespace {
enum class Kind32 : std::uint8_t { Data, Nested, Next, Opaque, StructPtr, PtrArray };
struct Member32 {
  Kind32 kind;
  std::uint8_t is_const;
  std::uint16_t a32, host;
  std::uint32_t size;  // Data: bytes; Nested: element count
  std::int16_t target, len;
};
struct Struct32 {
  const char *name;
  XrStructureType type;
  std::uint32_t a32_size, host_size, host_align, first, count;
};
#define VENTURE_XR32_LAYOUTS
#include "hle/openxr/generated/a32.inc"
#undef VENTURE_XR32_LAYOUTS

bool typed(const Struct32 &s) { return s.count >= 2 && kMembers[s.first + 1].kind == Kind32::Next; }
const Struct32 *by_type(std::uint32_t type) {
  static const auto map = [] {
    std::unordered_map<std::uint32_t, std::uint16_t> m;
    for (std::size_t i = 0; i < kStructCount; ++i)
      if (kStructs[i].type) m.emplace(static_cast<std::uint32_t>(kStructs[i].type), static_cast<std::uint16_t>(i));
    return m;
  }();
  auto it = map.find(type);
  return it == map.end() ? nullptr : &kStructs[it->second];
}
void report_unknown(std::uint32_t type) {
  static std::mutex mutex;
  static std::set<std::uint32_t> seen;
  std::lock_guard lock(mutex);
  if (seen.insert(type).second)
    log::warn("xr", "A32 OpenXR: structure type {} has no ILP32 layout; dropped from the chain", type);
}
struct Pointer {
  void *p;
  template <class T> operator T *() const { return static_cast<T *>(p); }
};
struct Value {
  std::uint64_t v;
  template <class T> operator T() const {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(static_cast<std::uintptr_t>(v));
    else return static_cast<T>(v);
  }
};
struct Float {
  std::uint64_t v;
  operator float() const { return std::bit_cast<float>(static_cast<std::uint32_t>(v)); }
};
} // namespace

class Call32 {
 public:
  explicit Call32(CallContext &ctx) : ctx_(ctx), cursor_(ctx.cpu(), ctx.registers32()) {}
  std::uint32_t word() { return cursor_.word(); }
  std::uint64_t u64() { return cursor_.pair(); }
  Pointer identity(std::uint32_t a) { return {a ? g2h<void>(a) : nullptr}; }
  Pointer pfn_out(std::uint32_t a) {
    if (!a) return {nullptr};
    a32::check_out(a);
    pfn_guest_ = a;
    return {&pfn_};
  }
  Pointer timespec_in(std::uint32_t a) {
    if (!a) return {nullptr};
    std::int32_t v[2];
    a32::read_bytes(a, v, sizeof(v));
    timespec_ = {v[0], v[1]};
    return {&timespec_};
  }
  Pointer timespec_out(std::uint32_t a) {
    if (!a) return {nullptr};
    a32::check_out(a);
    timespec_guest_ = a;
    return {&timespec_};
  }
  Pointer in(std::uint32_t a, unsigned index, std::uint64_t count) { return {convert(a, index, count, true)}; }
  Pointer inout(std::uint32_t a, unsigned index, std::uint64_t count) { return {convert(a, index, count, false)}; }
  void finish(XrResult result) {
    try {
      for (const auto &e : writeback_) store(e.guest, e.host, kStructs[e.index], true);
      if (pfn_guest_) {
        const auto address = reinterpret_cast<std::uintptr_t>(pfn_);
        if (address > UINT32_MAX) throw a32::AbiError{0};
        const auto word = static_cast<std::uint32_t>(address);
        a32::write_bytes(pfn_guest_, &word, 4);
      }
      if (timespec_guest_) {
        const std::int32_t v[2]{static_cast<std::int32_t>(timespec_.sec), static_cast<std::int32_t>(timespec_.nsec)};
        a32::write_bytes(timespec_guest_, v, sizeof(v));
      }
    } catch (const a32::AbiError &) {
      log::error("xr", "A32 OpenXR {}: guest output is not writable", ctx_.info().name);
      if (XR_SUCCEEDED(result)) result = XR_ERROR_VALIDATION_FAILURE;
    }
    ctx_.return_x(static_cast<std::uint32_t>(result));
  }

 private:
  struct Entry {
    GuestAddr guest;
    std::byte *host;
    std::uint16_t index;
  };
  std::byte *allocate(std::size_t bytes, std::size_t align) {
    auto *p = static_cast<std::byte *>(arena_.allocate(bytes ? bytes : 1, align < 16 ? 16 : align));
    std::memset(p, 0, bytes ? bytes : 1);
    return p;
  }
  static std::uint16_t index_of(const Struct32 &s) { return static_cast<std::uint16_t>(&s - kStructs); }
  // A typed structure is converted as the type its guest header names, so base
  // headers (layers, swapchain images, events) take their concrete layout.
  const Struct32 &concrete(GuestAddr a, const Struct32 &declared) {
    if (!typed(declared)) return declared;
    std::uint32_t type;
    a32::read_bytes(a, &type, 4);
    if (auto *s = by_type(type)) return *s;
    return declared;
  }
  void *convert(GuestAddr a, unsigned index, std::uint64_t count, bool is_const) {
    if (!a) return nullptr;
    if (count > 65536) throw a32::AbiError{0};
    const auto &s = concrete(a, kStructs[index]);
    auto *host = allocate(s.host_size * count, s.host_align);
    for (std::uint64_t i = 0; i < count; ++i)
      fill(a + i * s.a32_size, host + i * s.host_size, s, is_const, true);
    return host;
  }
  void *chain(GuestAddr a, bool is_const) {
    for (unsigned hops = 0; a; ++hops) {
      if (hops > 64 || ++depth_ > 256) throw a32::AbiError{0};
      std::uint32_t header[2];
      a32::read_bytes(a, header, sizeof(header));
      if (auto *s = by_type(header[0])) {
        auto *host = allocate(s->host_size, s->host_align);
        fill(a, host, *s, is_const, true);
        return host;
      }
      report_unknown(header[0]);
      a = header[1];
    }
    return nullptr;
  }
  void fill(GuestAddr g, std::byte *h, const Struct32 &s, bool is_const, bool record) {
    if (++depth_ > 4096) throw a32::AbiError{0};
    auto *local = allocate(s.a32_size, 8);
    a32::read_bytes(g, local, s.a32_size);
    if (!is_const && record) writeback_.push_back({g, h, index_of(s)});
    auto word = [&](const Member32 &m) {
      std::uint32_t v;
      std::memcpy(&v, local + m.a32, 4);
      return v;
    };
    auto length = [&](const Member32 &m) -> std::uint64_t {
      if (m.len < 0) return 1;
      const auto &n = kMembers[s.first + m.len];
      std::uint64_t v = 0;
      std::memcpy(&v, local + n.a32, n.size < 8 ? n.size : 8);
      return v;
    };
    for (std::uint32_t i = 0; i < s.count; ++i) {
      const auto &m = kMembers[s.first + i];
      const bool member_const = is_const || m.is_const;
      std::uint64_t pointer = 0;
      switch (m.kind) {
      case Kind32::Data: std::memcpy(h + m.host, local + m.a32, m.size); continue;
      case Kind32::Nested: {
        const auto &t = kStructs[m.target];
        for (std::uint32_t k = 0; k < m.size; ++k)
          fill(g + m.a32 + k * t.a32_size, h + m.host + k * t.host_size, t, is_const, false);
        continue;
      }
      case Kind32::Next: pointer = reinterpret_cast<std::uintptr_t>(chain(word(m), member_const)); break;
      case Kind32::Opaque: pointer = word(m); break;
      case Kind32::StructPtr:
        pointer = reinterpret_cast<std::uintptr_t>(convert(word(m), m.target, length(m), member_const));
        break;
      case Kind32::PtrArray: {
        const auto guest = word(m);
        const auto count = length(m);
        if (!guest) break;
        if (count > 65536) throw a32::AbiError{0};
        std::vector<std::uint32_t> words(count);
        if (count) a32::read_bytes(guest, words.data(), count * 4);
        auto *array = reinterpret_cast<std::uint64_t *>(allocate(count * 8, 8));
        for (std::uint64_t k = 0; k < count; ++k)
          array[k] = m.target < 0 ? words[k]
                                  : reinterpret_cast<std::uintptr_t>(convert(words[k], m.target, 1, member_const));
        pointer = reinterpret_cast<std::uintptr_t>(array);
        break;
      }
      }
      std::memcpy(h + m.host, &pointer, 8);
    }
  }
  // Writes non-pointer members back; the guest's pointers are never replaced.
  // A host that changed the structure type (xrPollEvent filling an
  // XrEventDataBuffer) is written in that type's guest layout.
  void store(GuestAddr g, const std::byte *h, const Struct32 &declared, bool top) {
    const Struct32 *s = &declared;
    if (top && typed(declared)) {
      std::uint32_t type;
      std::memcpy(&type, h, 4);
      if (type != static_cast<std::uint32_t>(declared.type))
        if (auto *t = by_type(type); t && t->a32_size <= declared.a32_size) s = t;
    }
    for (std::uint32_t i = 0; i < s->count; ++i) {
      const auto &m = kMembers[s->first + i];
      if (m.kind == Kind32::Data) a32::write_bytes(g + m.a32, h + m.host, m.size);
      else if (m.kind == Kind32::Nested) {
        const auto &t = kStructs[m.target];
        for (std::uint32_t k = 0; k < m.size; ++k)
          store(g + m.a32 + k * t.a32_size, h + m.host + k * t.host_size, t, false);
      }
    }
  }

  CallContext &ctx_;
  a32::ArgCursor32 cursor_;
  alignas(16) std::byte buffer_[16384];
  std::pmr::monotonic_buffer_resource arena_{buffer_, sizeof(buffer_)};
  std::vector<Entry> writeback_;
  unsigned depth_ = 0;
  PFN_xrVoidFunction pfn_ = nullptr;
  GuestAddr pfn_guest_ = 0, timespec_guest_ = 0;
  guest::Timespec timespec_{};
};

namespace {
#define VENTURE_XR32_ADAPTERS
#include "hle/openxr/generated/a32.inc"
#undef VENTURE_XR32_ADAPTERS
template <void (*F)(Call32 &)> void run(CallContext &ctx) {
  a32::trace_enter(ctx);
  {
    Call32 call(ctx);
    try {
      F(call);
    } catch (const a32::AbiError &) {
      log::error("xr", "A32 OpenXR {}: guest structure is not readable or too deep", ctx.info().name);
      ctx.return_x(static_cast<std::uint32_t>(XR_ERROR_VALIDATION_FAILURE));
    }
  }
  a32::trace_leave(ctx);
}
} // namespace

void register_a32_adapters() {
  static std::once_flag once;
  std::call_once(once, [] {
#define VENTURE_XR32_COMMAND(symbol, function)                                                        \
  hle::a32::register_adapter("libopenxr_loader.so", #symbol, &run<&xr32_##function>);
#define VENTURE_XR32_COMMANDS
#include "hle/openxr/generated/a32.inc"
#undef VENTURE_XR32_COMMANDS
#undef VENTURE_XR32_COMMAND
  });
}
} // namespace venture::hle::openxr
