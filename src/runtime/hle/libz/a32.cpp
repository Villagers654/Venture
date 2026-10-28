#include "hle/libz/a32.h"
#include "hle/libc/libc_a.h"
#include "core/token32.h"
#include "core/error.h"
#include "platform/memory.h"
#include <zlib.h>
#include <mutex>

namespace venture::hle::libc::zlib32 {
namespace {
GuestAddr alloc_thunk, free_thunk;
Token32Registry streams;
struct Stream {
  std::recursive_mutex mutex;
  z_stream host{};
  GuestThread* thread = nullptr;
  GuestAddr alloc = 0, free = 0, opaque = 0;
};
void* default_alloc(GuestAddr, std::uint32_t count, std::uint32_t size) {
  auto n = std::uint64_t(count) * size;
  auto* p = allocate(n);
  if (p) std::memset(p, 0, n);
  return p;
}
void default_free(GuestAddr, void* p) { deallocate(p); }
void* host_alloc(void* opaque, uInt count, uInt size) {
  auto& s = *static_cast<Stream*>(opaque);
  return s.alloc ? g2h(s.thread->call(s.alloc, {s.opaque, count, size})) : default_alloc(0, count, size);
}
void host_free(void* opaque, void* pointer) {
  auto& s = *static_cast<Stream*>(opaque);
  if (s.free) s.thread->call(s.free, {s.opaque, h2g32(pointer)});
  else deallocate(pointer);
}
void input(Stream& s, const WireStream& w, GuestThread& thread) {
  s.thread = &thread;
  s.host.next_in = g2h<Bytef>(w.next_in);
  s.host.avail_in = w.avail_in;
  s.host.total_in = w.total_in;
  s.host.next_out = g2h<Bytef>(w.next_out);
  s.host.avail_out = w.avail_out;
  s.host.total_out = w.total_out;
}
void output(const Stream& s, WireStream& w) {
  const auto& h = s.host;
  w.next_in = h2g32(h.next_in); w.avail_in = h.avail_in; w.total_in = static_cast<std::uint32_t>(h.total_in);
  w.next_out = h2g32(h.next_out); w.avail_out = h.avail_out; w.total_out = static_cast<std::uint32_t>(h.total_out);
  w.msg = h.msg ? h2g32(low_string(h.msg)) : 0;
  w.data_type = h.data_type; w.adler = static_cast<std::uint32_t>(h.adler); w.reserved = static_cast<std::uint32_t>(h.reserved);
}
WireStream read(GuestAddr address) {
  if (!platform::memory_accessible(address, sizeof(WireStream), true)) throw a32::AbiError{err::kEFAULT};
  WireStream wire;
  a32::read_bytes(address, &wire, sizeof(wire));
  return wire;
}
template<class Init>
int initialize(CallContext& c, GuestAddr address, const char* version, std::int32_t size, Init init) {
  if (!version || version[0] != ZLIB_VERSION[0] || size != sizeof(WireStream)) return Z_VERSION_ERROR;
  if (!address) return Z_STREAM_ERROR;
  auto w = read(address);
  auto s = std::make_shared<Stream>();
  if (!w.zalloc) { w.zalloc = static_cast<std::uint32_t>(alloc_thunk); w.opaque = 0; }
  if (!w.zfree) w.zfree = static_cast<std::uint32_t>(free_thunk);
  s->alloc = w.zalloc == alloc_thunk ? 0 : w.zalloc;
  s->free = w.zfree == free_thunk ? 0 : w.zfree;
  s->opaque = w.opaque;
  s->host.zalloc = host_alloc; s->host.zfree = host_free; s->host.opaque = s.get();
  input(*s, w, c.thread());
  auto result = init(&s->host);
  output(*s, w);
  w.state = result == Z_OK ? streams.insert(s).value : 0;
  a32::write_bytes(address, &w, sizeof(w));
  return result;
}
int inflate_init(CallContext& c, GuestAddr w, const char* v, std::int32_t size) {
  return initialize(c, w, v, size, [](auto* h) { return inflateInit(h); });
}
int inflate_init2(CallContext& c, GuestAddr w, std::int32_t bits, const char* v, std::int32_t size) {
  return initialize(c, w, v, size, [=](auto* h) { return inflateInit2(h, bits); });
}
int deflate_init(CallContext& c, GuestAddr w, std::int32_t level, const char* v, std::int32_t size) {
  return initialize(c, w, v, size, [=](auto* h) { return deflateInit(h, level); });
}
int deflate_init2(CallContext& c, GuestAddr w, std::int32_t level, std::int32_t method,
                  std::int32_t bits, std::int32_t memory, std::int32_t strategy, const char* v, std::int32_t size) {
  return initialize(c, w, v, size, [=](auto* h) { return deflateInit2(h, level, method, bits, memory, strategy); });
}
template<auto Fn, bool End = false, class... A>
int operation(CallContext& c, GuestAddr address, A... args) {
  if (!address) return Z_STREAM_ERROR;
  auto w = read(address);
  auto s = streams.resolve(Token32<Stream>{w.state});
  if (!s) return Z_STREAM_ERROR;
  std::lock_guard lock(s->mutex);
  if (!s->host.state) return Z_STREAM_ERROR;
  input(*s, w, c.thread());
  auto result = Fn(&s->host, args...);
  output(*s, w);
  if constexpr (End) {
    if (!s->host.state) { streams.erase(Token32<Stream>{w.state}); w.state = 0; }
  }
  a32::write_bytes(address, &w, sizeof(w));
  return result;
}
template<bool Compress>
int one_shot(GuestAddr dst, GuestAddr dst_size, GuestAddr src, std::uint32_t source, std::int32_t level = Z_DEFAULT_COMPRESSION) {
  std::uint32_t dn;
  a32::read_bytes(dst_size, &dn, 4); a32::check_out(dst_size);
  uLongf out = dn; uLong in = source;
  auto result = Compress ? compress2(g2h<Bytef>(dst), &out, g2h<const Bytef>(src), source, level)
                         : uncompress2(g2h<Bytef>(dst), &out, g2h<const Bytef>(src), &in);
  dn = static_cast<std::uint32_t>(out);
  a32::write_bytes(dst_size, &dn, 4);
  return result;
}
// Layout parameters are consumed explicitly as guest addresses; invoking the
// generic interpreter would reject z_streamp and uLongf* (correctly).
template<auto F, class R, class... A>
void invoke(R (*)(A...), CallContext& c) {
  a32::trace_enter(c);
  try {
    a32::ArgCursor32 cursor(c.cpu());
    std::size_t i = 0;
    auto read_arg = [&]<class T>() -> T {
      if constexpr (std::is_same_v<T, CallContext&>) return c;
      else {
        auto bits = cursor.next(c.info().a32->arguments[i++]);
        if constexpr (std::is_pointer_v<T>) return g2h<std::remove_pointer_t<T>>(bits);
        else return static_cast<T>(bits);
      }
    };
    std::tuple<A...> args{read_arg.template operator()<A>()...};
    a32::ResultWriter32(c.cpu()).scalar(c.info().a32->result, std::apply(F, args));
  } catch (const a32::AbiError& e) {
    c.thread().set_errno(e.error);
    c.return_x(static_cast<std::uint32_t>(Z_STREAM_ERROR));
  }
  a32::trace_leave(c);
}
template<auto F>
void adapter(const char* name) {
  a32::register_adapter("libz.so", name, [](CallContext& c) { invoke<F>(F, c); });
}
int compress_impl(GuestAddr d, GuestAddr dn, GuestAddr s, std::uint32_t sn) { return one_shot<true>(d, dn, s, sn); }
int compress2_impl(GuestAddr d, GuestAddr dn, GuestAddr s, std::uint32_t sn, std::int32_t level) { return one_shot<true>(d, dn, s, sn, level); }
int uncompress_impl(GuestAddr d, GuestAddr dn, GuestAddr s, std::uint32_t sn) { return one_shot<false>(d, dn, s, sn); }
}
void register_adapters(HleLibrary& lib) {
  if (process_config().isa() != GuestIsa::a32) return;
  alloc_thunk = make_thunk(lib.soname(), "<A32 zalloc>", wrap<&default_alloc>());
  free_thunk = make_thunk(lib.soname(), "<A32 zfree>", wrap<&default_free>());
  adapter<&inflate_init>("inflateInit_"); adapter<&inflate_init2>("inflateInit2_");
  adapter<&deflate_init>("deflateInit_"); adapter<&deflate_init2>("deflateInit2_");
  adapter<&operation<&inflate, false, int>>("inflate"); adapter<&operation<&deflate, false, int>>("deflate");
  adapter<&operation<&inflateEnd, true>>("inflateEnd"); adapter<&operation<&deflateEnd, true>>("deflateEnd");
  adapter<&operation<&inflateReset>>("inflateReset"); adapter<&operation<&deflateReset>>("deflateReset");
  adapter<&compress_impl>("compress"); adapter<&compress2_impl>("compress2");
  adapter<&uncompress_impl>("uncompress");
}
}
