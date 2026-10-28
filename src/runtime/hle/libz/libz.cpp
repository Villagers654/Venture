#include <zlib.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>

#include "hle/libc/libc_a.h"
#include "hle/libz/guest_zlib.h"
#include "hle/libz/a32.h"
namespace venture::hle::libc {
static_assert(sizeof(z_off64_t) == 8);
namespace {
struct Header {
  gz_header host{};
  GuestGzHeader* guest = nullptr;
};
GuestAddr default_alloc_thunk = 0, default_free_thunk = 0;
void* default_alloc(GuestAddr, unsigned count, unsigned size) {
  auto n = std::uint64_t(count) * size;
  auto p = allocate(n);
  if (p) std::memset(p, 0, n);
  return p;
}
void default_free(GuestAddr, void* p) { deallocate(p); }
struct Stream {
  z_stream host{};
  GuestThread* thread = nullptr;
  GuestAddr alloc = 0, free = 0, opaque = 0;
  // zlib copies retain their gz_header pointer, including midway through a
  // header. Share this bridge until all copied streams release it.
  std::shared_ptr<Header> header;
};
void* host_alloc(void* opaque, uInt count, uInt size) {
  auto& s = *static_cast<Stream*>(opaque);
  if (s.alloc) return g2h(s.thread->call(s.alloc, {s.opaque, count, size}));
  return default_alloc(0, count, size);
}
void host_free(void* opaque, void* p) {
  auto& s = *static_cast<Stream*>(opaque);
  if (s.free)
    s.thread->call(s.free, {s.opaque, h2g(p)});
  else
    deallocate(p);
}
void input(Stream& s, const GuestZStream& g, GuestThread& thread) {
  s.thread = &thread;
  s.host.next_in = g.next_in;
  s.host.avail_in = g.avail_in;
  s.host.total_in = static_cast<uLong>(g.total_in);
  s.host.next_out = g.next_out;
  s.host.avail_out = g.avail_out;
  s.host.total_out = static_cast<uLong>(g.total_out);
}
void header_output(Stream& s) {
  if (!s.header) return;
  auto& g = *s.header->guest;
  auto& h = s.header->host;
  g.text = h.text;
  g.time = h.time;
  g.xflags = h.xflags;
  g.os = h.os;
  g.extra_len = h.extra_len;
  g.hcrc = h.hcrc;
  g.done = h.done;
}
void output(Stream& s, GuestZStream& g, bool reset = false) {
  auto& h = s.host;
  g.next_in = h.next_in;
  g.avail_in = h.avail_in;
  g.next_out = h.next_out;
  g.avail_out = h.avail_out;
  // Accumulate modulo-host-uLong deltas to preserve guest totals past 4 GiB.
  g.total_in =
      reset ? h.total_in : g.total_in + static_cast<uLong>(h.total_in - static_cast<uLong>(g.total_in));
  g.total_out =
      reset ? h.total_out : g.total_out + static_cast<uLong>(h.total_out - static_cast<uLong>(g.total_out));
  g.msg = h.msg;
  g.data_type = h.data_type;
  g.adler = h.adler;
  g.reserved = h.reserved;
  header_output(s);
}
bool version_ok(const char* version, int size) {
  return version && version[0] == ZLIB_VERSION[0] && size == sizeof(GuestZStream);
}
template <class Init>
int initialize(CallContext& c, GuestZStream* g, const char* version, int size, Init init) {
  if (!version_ok(version, size)) return Z_VERSION_ERROR;
  if (!g) return Z_STREAM_ERROR;
  auto s = std::make_unique<Stream>();
  s->thread = &c.thread();
  // zlib.h promises callable default allocators after initialization. These
  // are guest thunks, never native function addresses.
  if (!g->zalloc) {
    g->zalloc = default_alloc_thunk;
    g->opaque = 0;
  }
  if (!g->zfree) g->zfree = default_free_thunk;
  s->alloc = g->zalloc == default_alloc_thunk ? 0 : g->zalloc;
  s->free = g->zfree == default_free_thunk ? 0 : g->zfree;
  s->opaque = g->opaque;
  s->host.zalloc = host_alloc;
  s->host.zfree = host_free;
  s->host.opaque = s.get();
  input(*s, *g, c.thread());
  int result = init(s->host);
  output(*s, *g, true);
  if (result == Z_OK)
    g->state = s.release();
  else
    g->state = nullptr;
  return result;
}
int deflate_init(CallContext& c, GuestZStream* g, int level, const char* v, int size) {
  return initialize(c, g, v, size, [&](auto& h) { return deflateInit_(&h, level, ZLIB_VERSION, sizeof(h)); });
}
int deflate_init2(CallContext& c, GuestZStream* g, int level, int method, int bits, int memory, int strategy,
                  const char* v, int size) {
  return initialize(c, g, v, size, [&](auto& h) {
    return deflateInit2_(&h, level, method, bits, memory, strategy, ZLIB_VERSION, sizeof(h));
  });
}
int inflate_init(CallContext& c, GuestZStream* g, const char* v, int size) {
  return initialize(c, g, v, size, [&](auto& h) { return inflateInit_(&h, ZLIB_VERSION, sizeof(h)); });
}
int inflate_init2(CallContext& c, GuestZStream* g, int bits, const char* v, int size) {
  return initialize(c, g, v, size, [&](auto& h) { return inflateInit2_(&h, bits, ZLIB_VERSION, sizeof(h)); });
}
int inflate_back_init(CallContext& c, GuestZStream* g, int bits, unsigned char* window, const char* v,
                      int size) {
  return initialize(c, g, v, size,
                    [&](auto& h) { return inflateBackInit_(&h, bits, window, ZLIB_VERSION, sizeof(h)); });
}
template <auto Fn, bool Reset = false, class... Args>
int operation(CallContext& c, GuestZStream* g, Args... args) {
  if (!g || !g->state) return Z_STREAM_ERROR;
  auto& s = *static_cast<Stream*>(g->state);
  input(s, *g, c.thread());
  int result = Fn(&s.host, args...);
  output(s, *g, Reset && result == Z_OK);
  return result;
}
template <auto Fn>
int end(CallContext& c, GuestZStream* g) {
  if (!g || !g->state) return Z_STREAM_ERROR;
  auto s = static_cast<Stream*>(g->state);
  input(*s, *g, c.thread());
  int result = Fn(&s->host);
  output(*s, *g);
  if (!s->host.state) {
    delete s;
    g->state = nullptr;
  }
  return result;
}
template <auto Fn>
int copy(CallContext& c, GuestZStream* dest, GuestZStream* src) {
  if (!dest || !src || !src->state) return Z_STREAM_ERROR;
  auto& original = *static_cast<Stream*>(src->state);
  input(original, *src, c.thread());
  auto s = std::make_unique<Stream>();
  s->thread = &c.thread();
  s->alloc = original.alloc;
  s->free = original.free;
  s->opaque = original.opaque;
  int result = Fn(&s->host, &original.host);
  if (result != Z_OK) return result;
  s->host.opaque = s.get();
  s->header = original.header;
  *dest = *src;
  dest->state = s.release();
  return result;
}
template <auto Fn>
int set_header(CallContext& c, GuestZStream* g, GuestGzHeader* header) {
  if (!g || !g->state || !header) return Z_STREAM_ERROR;
  auto& s = *static_cast<Stream*>(g->state);
  input(s, *g, c.thread());
  auto bridge = std::make_shared<Header>();
  bridge->guest = header;
  bridge->host = {header->text,      static_cast<uLong>(header->time),
                  header->xflags,    header->os,
                  header->extra,     header->extra_len,
                  header->extra_max, header->name,
                  header->name_max,  header->comment,
                  header->comm_max,  header->hcrc,
                  header->done};
  int result = Fn(&s.host, &bridge->host);
  if (result == Z_OK) s.header = std::move(bridge);
  output(s, *g);
  return result;
}
std::uint64_t bound(std::uint64_t n) {
  if (n > UINT64_MAX - (n >> 12) - (n >> 14) - (n >> 25) - 13) return UINT64_MAX;
  return n + (n >> 12) + (n >> 14) + (n >> 25) + 13;
}
std::uint64_t deflate_bound(CallContext& c, GuestZStream* g, std::uint64_t n) {
  if (n > ULONG_MAX) return n > UINT64_MAX / 2 ? UINT64_MAX : n + (n >> 3) + (n >> 6) + 128;
  if (!g || !g->state) return deflateBound(nullptr, static_cast<uLong>(n));
  auto& s = *static_cast<Stream*>(g->state);
  input(s, *g, c.thread());
  return deflateBound(&s.host, static_cast<uLong>(n));
}
template <auto Fn>
std::uint64_t checksum(std::uint64_t initial, const unsigned char* p, std::uint64_t size) {
  uLong value = static_cast<uLong>(initial);
  if (!p) return Fn(value, nullptr, 0);
  while (size) {
    auto n = static_cast<uInt>(std::min<std::uint64_t>(size, UINT_MAX));
    value = Fn(value, p, n);
    p += n;
    size -= n;
  }
  return value;
}
template <auto Fn>
std::uint64_t checksum32(std::uint64_t initial, const unsigned char* p, std::uint32_t size) {
  return checksum<Fn>(initial, p, size);
}
template <auto Fn>
std::uint64_t combine(std::uint64_t a, std::uint64_t b, std::int64_t len) {
  return Fn(static_cast<uLong>(a), static_cast<uLong>(b), static_cast<z_off64_t>(len));
}
std::uint64_t crc_gen(std::int64_t n) { return crc32_combine_gen64(n); }
std::uint64_t crc_op(std::uint64_t a, std::uint64_t b, std::uint64_t op) {
  return crc32_combine_op(static_cast<uLong>(a), static_cast<uLong>(b), static_cast<uLong>(op));
}
const char* version() { return guest_string(ZLIB_VERSION); }
const char* error(int code) { return guest_string(zError(code)); }
const std::uint32_t* crc_table() { return get_crc_table(); }
std::uint64_t compile_flags() { return (zlibCompileFlags() & ~255ul) | (process_config().isa() == GuestIsa::a32 ? 85u : 169u); }
int one_shot(bool compressing, unsigned char* dst, std::uint64_t* dst_len, const unsigned char* src,
             std::uint64_t* src_len, int level) {
  z_stream stream{};
  int result = compressing ? deflateInit(&stream, level) : inflateInit(&stream);
  if (result != Z_OK) return result;
  const auto input_size = *src_len, output_size = *dst_len;
  std::uint64_t in = 0, out = 0;
  unsigned char dummy;  // uncompress permits a zero-length result buffer.
  for (;;) {
    stream.next_in = const_cast<Bytef*>(src + in);
    stream.avail_in = static_cast<uInt>(std::min<std::uint64_t>(input_size - in, UINT_MAX));
    stream.next_out = output_size ? dst + out : &dummy;
    stream.avail_out = static_cast<uInt>(std::min<std::uint64_t>(output_size - out, UINT_MAX));
    if (!output_size && !compressing) stream.avail_out = 1;
    auto before_in = stream.avail_in, before_out = stream.avail_out;
    result = compressing ? deflate(&stream, in + stream.avail_in == input_size ? Z_FINISH : Z_NO_FLUSH)
                         : inflate(&stream, Z_NO_FLUSH);
    in += before_in - stream.avail_in;
    out += before_out - stream.avail_out;
    if (result == Z_STREAM_END) {
      result = out > output_size ? Z_BUF_ERROR : Z_OK;
      break;
    }
    if (result != Z_OK && result != Z_BUF_ERROR) break;
    if (out >= output_size) {
      result = Z_BUF_ERROR;
      break;
    }
    if (before_in == stream.avail_in && before_out == stream.avail_out) {
      result = compressing ? Z_BUF_ERROR : Z_DATA_ERROR;
      break;
    }
    if (!compressing && in == input_size && stream.avail_out) {
      result = Z_DATA_ERROR;
      break;
    }
  }
  *dst_len = std::min(out, output_size);
  *src_len = in;
  if (compressing)
    deflateEnd(&stream);
  else
    inflateEnd(&stream);
  return result;
}
int compress2_impl(unsigned char* d, std::uint64_t* dn, const unsigned char* s, std::uint64_t sn, int level) {
  return one_shot(true, d, dn, s, &sn, level);
}
int compress_impl(unsigned char* d, std::uint64_t* dn, const unsigned char* s, std::uint64_t sn) {
  return compress2_impl(d, dn, s, sn, Z_DEFAULT_COMPRESSION);
}
int uncompress_impl(unsigned char* d, std::uint64_t* dn, const unsigned char* s, std::uint64_t sn) {
  return one_shot(false, d, dn, s, &sn, 0);
}
int uncompress2_impl(unsigned char* d, std::uint64_t* dn, const unsigned char* s, std::uint64_t* sn) {
  return one_shot(false, d, dn, s, sn, 0);
}
std::int64_t inflate_mark(CallContext& c, GuestZStream* g) {
  if (!g || !g->state) return -65536;
  auto& s = *static_cast<Stream*>(g->state);
  input(s, *g, c.thread());
  return inflateMark(&s.host);
}
std::uint64_t codes_used(CallContext& c, GuestZStream* g) {
  if (!g || !g->state) return UINT64_MAX;
  auto& s = *static_cast<Stream*>(g->state);
  input(s, *g, c.thread());
  auto n = inflateCodesUsed(&s.host);
  return n == static_cast<unsigned long>(-1) ? UINT64_MAX : n;
}
struct BackCallbacks {
  GuestThread& thread;
  GuestAddr input, output, in_desc, out_desc;
};
unsigned back_input(void* opaque, unsigned char** data) {
  auto& cb = *static_cast<BackCallbacks*>(opaque);
  return static_cast<unsigned>(cb.thread.call(cb.input, {cb.in_desc, h2g(data)}));
}
int back_output(void* opaque, unsigned char* data, unsigned n) {
  auto& cb = *static_cast<BackCallbacks*>(opaque);
  return static_cast<int>(cb.thread.call(cb.output, {cb.out_desc, h2g(data), n}));
}
int inflate_back(CallContext& c, GuestZStream* g, GuestAddr in, GuestAddr in_desc, GuestAddr out,
                 GuestAddr out_desc) {
  BackCallbacks cb{c.thread(), in, out, in_desc, out_desc};
  return operation<&inflateBack>(c, g, back_input, static_cast<void*>(&cb), back_output,
                                 static_cast<void*>(&cb));
}
}  // namespace
void register_libz(HleLibrary& lib) {
  zlib32::register_adapters(lib);
  default_alloc_thunk = make_thunk(lib.soname(), "<default zalloc>", wrap<&default_alloc>());
  default_free_thunk = make_thunk(lib.soname(), "<default zfree>", wrap<&default_free>());
#define B(n, f) bind<&f>(lib, n)
  B("zlibVersion", version);
  B("zError", error);
  B("zlibCompileFlags", compile_flags);
  B("get_crc_table", crc_table);
  B("deflateInit_", deflate_init);
  B("deflateInit2_", deflate_init2);
  B("inflateInit_", inflate_init);
  B("inflateInit2_", inflate_init2);
  B("inflateBackInit_", inflate_back_init);
#define OP(n, ...) bind<&operation<&::n, false __VA_OPT__(, ) __VA_ARGS__>>(lib, #n)
#define RESET(n, ...) bind<&operation<&::n, true __VA_OPT__(, ) __VA_ARGS__>>(lib, #n)
  OP(deflate, int);
  OP(inflate, int);
  RESET(deflateReset);
  RESET(deflateResetKeep);
  RESET(inflateReset);
  RESET(inflateResetKeep);
  RESET(inflateReset2, int);
  OP(deflateSetDictionary, const unsigned char*, unsigned);
  OP(inflateSetDictionary, const unsigned char*, unsigned);
  OP(deflateGetDictionary, unsigned char*, unsigned*);
  OP(inflateGetDictionary, unsigned char*, unsigned*);
  OP(deflateParams, int, int);
  OP(deflateTune, int, int, int, int);
  OP(deflatePending, unsigned*, int*);
  OP(deflatePrime, int, int);
  OP(inflatePrime, int, int);
  OP(inflateSync);
  OP(inflateSyncPoint);
  OP(inflateUndermine, int);
  OP(inflateValidate, int);
#undef OP
#undef RESET
  bind<&end<&deflateEnd>>(lib, "deflateEnd");
  bind<&end<&inflateEnd>>(lib, "inflateEnd");
  bind<&end<&inflateBackEnd>>(lib, "inflateBackEnd");
  bind<&copy<&deflateCopy>>(lib, "deflateCopy");
  bind<&copy<&inflateCopy>>(lib, "inflateCopy");
  bind<&set_header<&deflateSetHeader>>(lib, "deflateSetHeader");
  bind<&set_header<&inflateGetHeader>>(lib, "inflateGetHeader");
  B("deflateBound", deflate_bound);
  B("compressBound", bound);
  B("compress", compress_impl);
  B("compress2", compress2_impl);
  B("uncompress", uncompress_impl);
  B("uncompress2", uncompress2_impl);
  bind<&checksum32<&::adler32>>(lib, "adler32");
  bind<&checksum32<&::crc32>>(lib, "crc32");
  bind<&checksum<&::adler32>>(lib, "adler32_z");
  bind<&checksum<&::crc32>>(lib, "crc32_z");
  bind<&combine<&::adler32_combine64>>(lib, "adler32_combine");
  bind<&combine<&::adler32_combine64>>(lib, "adler32_combine64");
  bind<&combine<&::crc32_combine64>>(lib, "crc32_combine");
  bind<&combine<&::crc32_combine64>>(lib, "crc32_combine64");
  B("crc32_combine_gen", crc_gen);
  B("crc32_combine_gen64", crc_gen);
  B("crc32_combine_op", crc_op);
  B("inflateMark", inflate_mark);
  B("inflateCodesUsed", codes_used);
  B("inflateBack", inflate_back);
#undef B
  extern void register_gzip(HleLibrary&);
  register_gzip(lib);
}
}  // namespace venture::hle::libc
