// iconv_open/iconv/iconv_close, ported from bionic libc/bionic/iconv.cpp.
//
// Bionic offers ASCII, UTF-8, UTF-16LE/BE, UTF-32LE/BE and WCHAR_T (UTF-32LE),
// names matched by Unicode TR22 alias rules, //IGNORE and //TRANSLIT on the
// destination, and no stateful encodings (so "UTF-16"/"UTF-32" without an
// endianness are EINVAL, as on a Quest). The descriptor lives in guest heap so
// its guest address is the iconv_t on both ISAs.
//
// Deliberate differences from bionic, each where bionic would hang or read
// out of bounds: a NUL in UTF-8 input converts as one byte (bionic's
// mbrtoc32 reports 0 bytes used and the loop never advances); an illegal
// UTF-8 sequence skipped by //IGNORE or replaced by //TRANSLIT consumes one
// byte (bionic adds (size_t)-1 to the source pointer); //IGNORE skipping the
// last input character ends the call instead of decoding past the buffer.
/*
 * Copyright (C) 2017 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <cstring>

#include "hle/libc/libc_a.h"
#include "abi32.h"

namespace venture::hle::libc {
namespace {
enum Encoding : std::uint32_t { kAscii, kUtf8, kUtf16Le, kUtf16Be, kUtf32Le, kUtf32Be, kWcharT };
enum Mode : std::uint32_t { kError, kIgnore, kTranslit };

// The iconv_t a guest holds: a guest-heap address on both ISAs.
struct Descriptor {
  Encoding src;
  Encoding dst;
  Mode mode;
};

bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
bool is_alnum(unsigned char c) { return is_digit(c) || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z'); }
unsigned char lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? c | 0x20 : c; }

// This matching is strange but true.
// See http://www.unicode.org/reports/tr22/#Charset_Alias_Matching.
bool match_encoding(const char* lhs, const char* rhs) {
  while (*lhs && *rhs) {
    // Skip non-alnum in lhs; "UTF-8", "UTF_8", "UTF8", "UTF 8" are all equivalent.
    // Also implement the "delete each 0 that is not preceded by a digit" rule.
    for (; *lhs; ++lhs) {
      auto c = static_cast<unsigned char>(*lhs);
      if (is_alnum(c) && (c != '0' || !is_digit(static_cast<unsigned char>(lhs[1])))) break;
    }
    // Case doesn't matter either.
    if (lower(static_cast<unsigned char>(*lhs)) != lower(static_cast<unsigned char>(*rhs))) break;
    ++lhs;
    ++rhs;
  }
  // As a special case we treat the GNU "//" extensions as end of string.
  return (*lhs == '\0' || std::strstr(lhs, "//") == lhs) && *rhs == '\0';
}

bool parse_encoding(const char* s, Encoding* encoding, Mode* mode) {
  if (const char* suffix = std::strstr(s, "//")) {
    if (!mode) return false;
    if (std::strcmp(suffix, "//IGNORE") == 0) *mode = kIgnore;
    else if (std::strcmp(suffix, "//TRANSLIT") == 0) *mode = kTranslit;
    else return false;
  }
  if (match_encoding(s, "utf8")) *encoding = kUtf8;
  else if (match_encoding(s, "ascii") || match_encoding(s, "usascii")) *encoding = kAscii;
  else if (match_encoding(s, "utf16le")) *encoding = kUtf16Le;
  else if (match_encoding(s, "utf16be")) *encoding = kUtf16Be;
  else if (match_encoding(s, "utf32le")) *encoding = kUtf32Le;
  else if (match_encoding(s, "utf32be")) *encoding = kUtf32Be;
  else if (match_encoding(s, "wchart")) *encoding = kWcharT;
  else return false;
  return true;
}

// One iconv() call; the source and destination cursors are the caller's.
class Converter {
 public:
  Converter(const Descriptor& d, char** src, std::uint64_t* src_left, char** dst, std::uint64_t* dst_left)
      : d_(d), src_(src), src_left_(src_left), dst_(dst), dst_left_(dst_left) {}

  std::uint64_t run() {
    bool converted = false;
    while (*src_left_ > 0) {
      converted = true;
      auto next = get_next();
      if (next == Next::kEnd) break;
      if (next == Next::kFail || !convert()) {
        set_error(error_);
        return UINT64_MAX;
      }
    }
    // bionic clears errno before each character, so a call that converted
    // anything and succeeded leaves errno 0.
    if (converted) set_error(0);
    if (d_.mode == kTranslit) return replacements_;
    if (ignored_) {
      set_error(err::kEILSEQ);
      return UINT64_MAX;
    }
    return 0;
  }

 private:
  enum class Next { kOk, kFail, kEnd };

  Next get_next() {
    for (;;) {
      error_ = 0;
      const char* s = *src_;
      switch (d_.src) {
        case kAscii:
          wc_ = static_cast<std::uint32_t>(static_cast<std::int32_t>(*s));  // bionic: char32_t from char
          src_used_ = 1;
          if (wc_ > 0x7f) error_ = err::kEILSEQ;
          break;
        case kUtf8: {
          auto n = decode_utf8(&wc_, s, *src_left_);
          if (n == UINT64_MAX - 1) {
            error_ = err::kEINVAL;
            return Next::kFail;
          }
          if (n == UINT64_MAX) {
            error_ = err::kEILSEQ;
            src_used_ = 1;
          } else {
            src_used_ = n ? n : 1;  // NUL is one byte
          }
          break;
        }
        case kUtf16Le:
        case kUtf16Be: {
          if (*src_left_ < 2) {
            error_ = err::kEINVAL;
            return Next::kFail;
          }
          bool swap = d_.src == kUtf16Be;
          wc_ = in16(s, swap);
          src_used_ = 2;
          // 0xd800-0xdbff: high surrogates; 0xdc00-0xdfff: low surrogates.
          if (wc_ >= 0xd800 && wc_ <= 0xdfff) {
            if (wc_ >= 0xdc00) {  // Low surrogate before high surrogate.
              error_ = err::kEILSEQ;
              return Next::kFail;
            }
            if (*src_left_ < 4) {
              error_ = err::kEINVAL;
              return Next::kFail;
            }
            std::uint32_t hi = wc_, lo = in16(s + 2, swap);
            wc_ = 0x10000 + ((hi - 0xd800) << 10) + (lo - 0xdc00);
            src_used_ = 4;
          }
          break;
        }
        case kUtf32Le:
        case kUtf32Be:
        case kWcharT:
          if (*src_left_ < 4) {
            error_ = err::kEINVAL;
            return Next::kFail;
          }
          wc_ = in32(s, d_.src == kUtf32Be);
          src_used_ = 4;
          break;
      }
      if (error_ != err::kEILSEQ) return Next::kOk;
      switch (d_.mode) {
        case kError:
          return Next::kFail;
        case kIgnore:
          *src_ += src_used_;
          *src_left_ -= src_used_;
          ignored_ = true;
          if (!*src_left_) return Next::kEnd;
          continue;
        case kTranslit:
          wc_ = '?';
          ++replacements_;
          return Next::kOk;
      }
      return Next::kFail;
    }
  }

  bool convert() {
    for (;;) {
      error_ = 0;
      switch (d_.dst) {
        case kAscii:
          buf_[0] = static_cast<char>(wc_);
          dst_used_ = 1;
          if (wc_ > 0x7f) error_ = err::kEILSEQ;
          break;
        case kUtf8: {
          int n = encode_utf8(buf_, wc_);
          if (n < 0) error_ = err::kEILSEQ;
          else dst_used_ = static_cast<std::uint64_t>(n);
          break;
        }
        case kUtf16Le:
        case kUtf16Be: {
          bool swap = d_.dst == kUtf16Be;
          if (wc_ < 0x10000) {  // BMP.
            out16(buf_, static_cast<std::uint16_t>(wc_), swap);
            dst_used_ = 2;
          } else {  // Supplementary plane; output surrogate pair.
            std::uint32_t v = wc_ - 0x10000;
            out16(buf_, static_cast<std::uint16_t>(0xd800 | (v >> 10)), swap);
            out16(buf_ + 2, static_cast<std::uint16_t>(0xdc00 | (v & 0x3ff)), swap);
            dst_used_ = 4;
          }
          break;
        }
        case kUtf32Le:
        case kUtf32Be:
        case kWcharT: {
          std::uint32_t v = d_.dst == kUtf32Be ? __builtin_bswap32(wc_) : wc_;
          std::memcpy(buf_, &v, 4);  // x86-64 host: little-endian
          dst_used_ = 4;
          break;
        }
      }
      if (error_ == err::kEILSEQ) {
        if (d_.mode == kIgnore) {
          *src_ += src_used_;
          *src_left_ -= src_used_;
          ignored_ = true;
          return true;
        }
        if (d_.mode == kTranslit) {
          wc_ = '?';
          ++replacements_;
          continue;
        }
        return false;
      }
      return emit();
    }
  }

  static std::uint32_t in16(const char* p, bool swap) {
    auto s = reinterpret_cast<const unsigned char*>(p);
    std::uint16_t v = static_cast<std::uint16_t>(s[0] | (s[1] << 8));
    return swap ? __builtin_bswap16(v) : v;
  }
  static std::uint32_t in32(const char* p, bool swap) {
    auto s = reinterpret_cast<const unsigned char*>(p);
    std::uint32_t v = s[0] | (s[1] << 8) | (s[2] << 16) | (std::uint32_t(s[3]) << 24);
    return swap ? __builtin_bswap32(v) : v;
  }
  static void out16(char* p, std::uint16_t v, bool swap) {
    if (swap) v = __builtin_bswap16(v);
    p[0] = static_cast<char>(v);
    p[1] = static_cast<char>(v >> 8);
  }

  bool emit() {
    if (dst_used_ > *dst_left_) {
      error_ = err::kE2BIG;
      return false;
    }
    std::memcpy(*dst_, buf_, dst_used_);
    *src_ += src_used_;
    *src_left_ -= src_used_;
    *dst_ += dst_used_;
    *dst_left_ -= dst_used_;
    return true;
  }

  const Descriptor& d_;
  char** src_;
  std::uint64_t* src_left_;
  char** dst_;
  std::uint64_t* dst_left_;
  std::uint32_t wc_ = 0;
  char buf_[16]{};
  std::uint64_t src_used_ = 0, dst_used_ = 0, replacements_ = 0;
  bool ignored_ = false;
  int error_ = 0;
};

Descriptor* open_descriptor(const char* dst, const char* src) {
  Descriptor value{kAscii, kAscii, kError};
  if (!dst || !src || !parse_encoding(src, &value.src, nullptr) || !parse_encoding(dst, &value.dst, &value.mode)) {
    set_error(err::kEINVAL);
    return nullptr;
  }
  auto* d = static_cast<Descriptor*>(allocate(sizeof(Descriptor)));
  if (!d) {
    set_error(err::kENOMEM);
    return nullptr;
  }
  *d = value;
  return d;
}

std::uint64_t convert(Descriptor* d, char** src, std::uint64_t* src_left, char** dst, std::uint64_t* dst_left) {
  if (!d) {
    set_error(err::kEBADF);
    return UINT64_MAX;
  }
  // Since none of these encodings are stateful, state flushing is a no-op.
  if (!src) return 0;
  return Converter(*d, src, src_left, dst, dst_left).run();
}

int close_descriptor(Descriptor* d) {
  if (!d) {
    set_error(err::kEBADF);
    return -1;
  }
  deallocate(d);
  return 0;
}

Descriptor* from_handle(std::uint64_t cd, std::uint64_t invalid) {
  return cd == invalid ? nullptr : reinterpret_cast<Descriptor*>(static_cast<std::uintptr_t>(cd));
}

std::uint64_t iconv_open_impl(const char* dst, const char* src) {
  auto* d = open_descriptor(dst, src);
  return d ? h2g(d) : UINT64_MAX;
}
std::uint64_t iconv_impl(std::uint64_t cd, char** src, std::uint64_t* src_left, char** dst, std::uint64_t* dst_left) {
  return convert(from_handle(cd, UINT64_MAX), src, src_left, dst, dst_left);
}
int iconv_close_impl(std::uint64_t cd) { return close_descriptor(from_handle(cd, UINT64_MAX)); }

// ILP32: iconv_t is a 32-bit guest address; char** and size_t* arrive as
// widened Out cells and are narrowed back after the call.
std::uint32_t iconv_open32(const char* dst, const char* src) {
  auto* d = open_descriptor(dst, src);
  return d ? h2g32(d) : UINT32_MAX;
}
std::uint32_t iconv32(std::uint32_t cd, char** src, std::uint64_t* src_left, char** dst, std::uint64_t* dst_left) {
  auto r = convert(from_handle(cd, UINT32_MAX), src, src_left, dst, dst_left);
  return r == UINT64_MAX ? UINT32_MAX : static_cast<std::uint32_t>(r);
}
int iconv_close32(std::uint32_t cd) { return close_descriptor(from_handle(cd, UINT32_MAX)); }
}  // namespace

void register_iconv(HleLibrary& lib) {
  if (process_config().isa() == GuestIsa::a32) {
    abi32::adapt<&iconv_open32>("iconv_open");
    abi32::adapt<&iconv32>("iconv");
    abi32::adapt<&iconv_close32>("iconv_close");
  }
  bind<&iconv_open_impl>(lib, "iconv_open");
  bind<&iconv_impl>(lib, "iconv");
  bind<&iconv_close_impl>(lib, "iconv_close");
}
}  // namespace venture::hle::libc
