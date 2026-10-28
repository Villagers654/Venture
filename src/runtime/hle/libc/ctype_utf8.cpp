#include <algorithm>
#include <cstring>

#include "hle/libc/libc_a.h"
#include "abi32.h"
namespace venture::hle::libc {
int encode_utf8(char* out, std::uint32_t c) {
  if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) {
    set_error(err::kEILSEQ);
    return -1;
  }
  if (c < 0x80) {
    out[0] = static_cast<char>(c);
    return 1;
  }
  int n = c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
  for (int i = n - 1; i > 0; --i) {
    out[i] = static_cast<char>(0x80 | (c & 63));
    c >>= 6;
  }
  out[0] = static_cast<char>((n == 2 ? 0xc0 : n == 3 ? 0xe0 : 0xf0) | c);
  return n;
}
namespace {
// bionic bits/mbstate_t.h: eight bytes on LP64, first four hold pending UTF-8.
struct MbState {
  unsigned char seq[4]{};
  unsigned char reserved[4]{};
};
static_assert(sizeof(MbState) == 8);
thread_local MbState implicit_state;
std::uint64_t invalid(MbState& state) {
  state = {};
  set_error(err::kEILSEQ);
  return UINT64_MAX;
}
std::uint64_t mbrtowc_impl(std::uint32_t* out, const char* input, std::uint64_t n, MbState* p) {
  auto& state = p ? *p : implicit_state;
  if (!input) {
    input = "";
    n = 1;
    out = nullptr;
  }
  if (!n) return UINT64_MAX - 1;
  unsigned have = 0;
  while (have < 4 && state.seq[have]) ++have;
  std::uint64_t used = 0;
  if (!have) {
    state.seq[0] = static_cast<unsigned char>(input[used++]);
    have = 1;
  }
  auto first = state.seq[0];
  unsigned want = first < 0x80                     ? 1
                  : first >= 0xc2 && first <= 0xdf ? 2
                  : first >= 0xe0 && first <= 0xef ? 3
                  : first >= 0xf0 && first <= 0xf4 ? 4
                                                   : 0;
  if (!want) return invalid(state);
  while (have < want && used < n) {
    auto b = static_cast<unsigned char>(input[used++]);
    if ((b & 0xc0) != 0x80) return invalid(state);
    if (have == 1 && ((first == 0xe0 && b < 0xa0) || (first == 0xed && b > 0x9f) ||
                      (first == 0xf0 && b < 0x90) || (first == 0xf4 && b > 0x8f)))
      return invalid(state);
    state.seq[have++] = b;
  }
  if (have < want) return UINT64_MAX - 1;
  std::uint32_t cp = first & (want == 1 ? 127 : want == 2 ? 31 : want == 3 ? 15 : 7);
  for (unsigned i = 1; i < want; ++i) cp = (cp << 6) | (state.seq[i] & 63);
  if ((want == 2 && cp < 0x80) || (want == 3 && cp < 0x800) || (want == 4 && cp < 0x10000) || cp > 0x10ffff ||
      (cp >= 0xd800 && cp <= 0xdfff))
    return invalid(state);
  state = {};
  if (out) *out = cp;
  return cp ? used : 0;
}
std::uint64_t mbrlen_impl(const char* s, std::uint64_t n, MbState* state) {
  return mbrtowc_impl(nullptr, s, n, state);
}
int mbsinit_impl(const MbState* state) { return !state || !state->seq[0]; }
std::uint64_t wcrtomb_impl(char* out, std::uint32_t cp, MbState* p) {
  auto& state = p ? *p : implicit_state;
  if (state.seq[0]) return invalid(state);
  if (!out) {
    state = {};
    return 1;
  }
  auto n = encode_utf8(out, cp);
  return n < 0 ? UINT64_MAX : static_cast<std::uint64_t>(n);
}
std::uint64_t mbsnrtowcs_impl(std::uint32_t* out, const char** source, std::uint64_t bytes,
                              std::uint64_t limit, MbState* p) {
  auto& state = p ? *p : implicit_state;
  auto s = *source;
  std::uint64_t used = 0, count = 0;
  while (used < bytes && (!out || count < limit)) {
    std::uint32_t cp;
    auto n = mbrtowc_impl(&cp, s + used, bytes - used, &state);
    // bionic/wchar.cpp treats a byte-limited incomplete sequence as EILSEQ
    // and resets mbstate; this intentionally differs from glibc.
    if (n >= UINT64_MAX - 1) {
      if (out) *source = s + (n == UINT64_MAX - 1 ? bytes : used);
      return invalid(state);
    }
    if (out) out[count] = cp;
    if (!cp) {
      if (out) *source = nullptr;
      return count;
    }
    used += n;
    ++count;
  }
  if (out) *source = s + used;
  return count;
}
std::uint64_t mbsrtowcs_impl(std::uint32_t* out, const char** source, std::uint64_t limit, MbState* p) {
  return mbsnrtowcs_impl(out, source, UINT64_MAX, limit, p);
}
std::uint64_t wcsnrtombs_impl(char* out, const std::uint32_t** source, std::uint64_t chars,
                              std::uint64_t limit, MbState* p) {
  auto& state = p ? *p : implicit_state;
  if (state.seq[0]) return invalid(state);
  auto s = *source;
  std::uint64_t used = 0, count = 0;
  while (count < chars && (!out || used < limit)) {
    char b[4];
    int n = encode_utf8(b, s[count]);
    if (n < 0) {
      if (out) *source = s + count;
      return UINT64_MAX;
    }
    if (out && static_cast<std::uint64_t>(n) > limit - used) break;
    if (out) std::memcpy(out + used, b, n);
    if (!s[count]) {
      if (out) *source = nullptr;
      return used;
    }
    used += n;
    ++count;
  }
  if (out) *source = s + count;
  return used;
}
std::uint64_t wcsrtombs_impl(char* out, const std::uint32_t** source, std::uint64_t limit, MbState* p) {
  return wcsnrtombs_impl(out, source, UINT64_MAX, limit, p);
}
std::uint64_t mbstowcs_impl(std::uint32_t* out, const char* s, std::uint64_t n) {
  MbState state{};
  return mbsrtowcs_impl(out, &s, n, &state);
}
std::uint64_t wcstombs_impl(char* out, const std::uint32_t* s, std::uint64_t n) {
  MbState state{};
  return wcsrtombs_impl(out, &s, n, &state);
}
int mbtowc_impl(std::uint32_t* out, const char* s, std::uint64_t n) {
  if (!s) {
    implicit_state = {};
    return 0;
  }
  auto r = mbrtowc_impl(out, s, n, nullptr);
  if (r >= UINT64_MAX - 1) {
    set_error(err::kEILSEQ);
    return -1;
  }
  return static_cast<int>(r);
}
int mblen_impl(const char* s, std::uint64_t n) { return mbtowc_impl(nullptr, s, n); }
int wctomb_impl(char* out, std::uint32_t cp) {
  if (!out) {
    implicit_state = {};
    return 0;
  }
  return static_cast<int>(wcrtomb_impl(out, cp, nullptr));
}
std::uint32_t btowc_impl(int c) { return c >= 0 && c < 128 ? c : UINT32_MAX; }
int wctob_impl(std::uint32_t cp) { return cp < 128 ? cp : -1; }
// bionic bits/mbstate_t.h: ILP32 owns four bytes, all pending UTF-8.
struct State32 {
  MbState value{}; unsigned char* wire;
  explicit State32(unsigned char* p) : wire(p) { if (p) std::memcpy(value.seq, p, 4); }
  ~State32() { if (wire) std::memcpy(wire, value.seq, 4); }
  MbState* get() { return wire ? &value : nullptr; }
};
std::uint32_t mbrtowc32(std::uint32_t* out, const char* s, std::uint32_t n, unsigned char* p) {
  State32 state(p); return static_cast<std::uint32_t>(mbrtowc_impl(out, s, n, state.get()));
}
std::uint32_t mbrlen32(const char* s, std::uint32_t n, unsigned char* p) { return mbrtowc32(nullptr,s,n,p); }
int mbsinit32(const unsigned char* p) { return !p || !p[0]; }
std::uint32_t wcrtomb32(char* out, std::uint32_t cp, unsigned char* p) { State32 state(p); return static_cast<std::uint32_t>(wcrtomb_impl(out,cp,state.get())); }
std::uint32_t mbsnrtowcs32(std::uint32_t* out, const char** src, std::uint32_t bytes, std::uint32_t n, unsigned char* p) {
  State32 state(p); return static_cast<std::uint32_t>(mbsnrtowcs_impl(out,src,bytes,n,state.get()));
}
std::uint32_t mbsrtowcs32(std::uint32_t* out, const char** src, std::uint32_t n, unsigned char* p) { return mbsnrtowcs32(out,src,UINT32_MAX,n,p); }
std::uint32_t wcsnrtombs32(char* out, const std::uint32_t** src, std::uint32_t chars, std::uint32_t n, unsigned char* p) {
  State32 state(p); return static_cast<std::uint32_t>(wcsnrtombs_impl(out,src,chars,n,state.get()));
}
std::uint32_t wcsrtombs32(char* out, const std::uint32_t** src, std::uint32_t n, unsigned char* p) { return wcsnrtombs32(out,src,UINT32_MAX,n,p); }
std::uint32_t mbstowcs32(std::uint32_t* out, const char* s, std::uint32_t n) { return static_cast<std::uint32_t>(mbstowcs_impl(out,s,n)); }
std::uint32_t wcstombs32(char* out, const std::uint32_t* s, std::uint32_t n) { return static_cast<std::uint32_t>(wcstombs_impl(out,s,n)); }
std::uint32_t mbsrtowcs_l32(std::uint32_t* out, const char** src, std::uint32_t n, unsigned char* p, std::uint32_t) { return mbsrtowcs32(out,src,n,p); }
std::uint32_t wcsrtombs_l32(char* out, const std::uint32_t** src, std::uint32_t n, unsigned char* p, std::uint32_t) { return wcsrtombs32(out,src,n,p); }
}  // namespace
void register_multibyte(HleLibrary& lib) {
  if (process_config().isa() == GuestIsa::a32) {
#define A(n, f) abi32::adapt<&f>(n)
    A("mbrtowc", mbrtowc32); A("mbrtoc32", mbrtowc32); A("mbrlen", mbrlen32); A("mbsinit", mbsinit32);
    A("wcrtomb", wcrtomb32); A("c32rtomb", wcrtomb32);
    A("mbsnrtowcs", mbsnrtowcs32); A("mbsrtowcs", mbsrtowcs32); A("wcsnrtombs", wcsnrtombs32); A("wcsrtombs", wcsrtombs32);
    A("mbstowcs", mbstowcs32); A("wcstombs", wcstombs32);
    A("mbsrtowcs_l", mbsrtowcs_l32); A("wcsrtombs_l", wcsrtombs_l32);
#undef A
  }
#define B(n) bind<&n##_impl>(lib, #n)
  B(mbrtowc);
  B(mbrlen);
  B(mbsinit);
  B(wcrtomb);
  B(mbsnrtowcs);
  B(mbsrtowcs);
  B(wcsnrtombs);
  B(wcsrtombs);
  B(mbstowcs);
  B(wcstombs);
  B(mbtowc);
  B(mblen);
  B(wctomb);
  B(btowc);
  B(wctob);
#undef B
  bind<&mbrtowc_impl>(lib, "mbrtoc32");
  bind<&wcrtomb_impl>(lib, "c32rtomb");
  bind<&mbsrtowcs_impl>(lib, "mbsrtowcs_l");
  bind<&wcsrtombs_impl>(lib, "wcsrtombs_l");
}
}  // namespace venture::hle::libc
