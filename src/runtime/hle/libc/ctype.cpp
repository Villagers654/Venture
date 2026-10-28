#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

#include "hle/libc/ctype_unicode.h"
#include "hle/libc/libc_a.h"
namespace venture::hle::libc {
namespace {
bool in(int c, int lo, int hi) { return c >= lo && c <= hi; }
int alpha(int c) { return in(c, 'a', 'z') || in(c, 'A', 'Z'); }
int digit(int c) { return in(c, '0', '9'); }
int alnum(int c) { return alpha(c) || digit(c); }
int blank(int c) { return c == ' ' || c == '\t'; }
int cntrl(int c) { return in(c, 0, 31) || c == 127; }
int graph(int c) { return in(c, 33, 126); }
int lower(int c) { return in(c, 'a', 'z'); }
int print(int c) { return in(c, 32, 126); }
int punct(int c) { return graph(c) && !alnum(c); }
int space(int c) { return c == ' ' || in(c, 9, 13); }
int upper(int c) { return in(c, 'A', 'Z'); }
int xdigit(int c) { return digit(c) || in(c, 'a', 'f') || in(c, 'A', 'F'); }
int tolower_impl(int c) { return upper(c) ? c + 32 : c; }
int toupper_impl(int c) { return lower(c) ? c - 32 : c; }
int tolower_unchecked(int c) { return c | 0x20; }
int toupper_unchecked(int c) { return c & ~0x20; }
int ascii(int c) { return in(c, 0, 127); }
int toascii_impl(int c) { return c & 127; }
constexpr std::array<const char*, 12> classes = {"alnum", "alpha", "blank", "cntrl", "digit", "graph",
                                                 "lower", "print", "punct", "space", "upper", "xdigit"};
constexpr std::array<int (*)(int), 12> narrow = {alnum, alpha, blank, cntrl, digit, graph,
                                                 lower, print, punct, space, upper, xdigit};
std::uint64_t wctype_impl(const char* name) {
  for (unsigned i = 0; i < classes.size(); ++i)
    if (!std::strcmp(name, classes[i])) return i + 1;
  return 0;
}
int iswctype_impl(std::uint32_t cp, std::uint64_t kind) {
  if (!kind || kind > 12) return 0;
  if (cp < 128) return narrow[kind - 1](cp);
  auto cat = utf8proc_category(static_cast<utf8proc_int32_t>(cp));
  // bionic/wctype.cpp selects ICU binary properties, including Other_*
  // characters that have no case mapping (utf8proc_isupper/islower differ).
  bool a = (cat >= UTF8PROC_CATEGORY_LU && cat <= UTF8PROC_CATEGORY_LO) || cat == UTF8PROC_CATEGORY_NL ||
           unicode::contains(unicode::other_alphabetic, cp);
  bool d = cat == UTF8PROC_CATEGORY_ND;
  bool s =
      cp == 0x85 || cat == UTF8PROC_CATEGORY_ZS || cat == UTF8PROC_CATEGORY_ZL || cat == UTF8PROC_CATEGORY_ZP;
  bool graphical =
      cat != UTF8PROC_CATEGORY_CN && cat != UTF8PROC_CATEGORY_CC && cat != UTF8PROC_CATEGORY_CS && !s;
  switch (kind) {
    case 1:
      return a || d;
    case 2:
      return a;
    case 3:
      return cat == UTF8PROC_CATEGORY_ZS;
    case 4:
      return cat == UTF8PROC_CATEGORY_CC;
    case 5:
      return d;
    case 6:
      return graphical;
    case 7:
      return cat == UTF8PROC_CATEGORY_LL || unicode::contains(unicode::other_lowercase, cp);
    case 8:
      return graphical || cat == UTF8PROC_CATEGORY_ZS;
    case 9:
      return cat >= UTF8PROC_CATEGORY_PC && cat <= UTF8PROC_CATEGORY_PO;
    case 10:
      return s;
    case 11:
      return cat == UTF8PROC_CATEGORY_LU || unicode::contains(unicode::other_uppercase, cp);
    case 12:
      return d || in(cp, 0xff21, 0xff26) || in(cp, 0xff41, 0xff46);
    default:
      return 0;
  }
}
template <unsigned Kind>
int wide(std::uint32_t cp) {
  return iswctype_impl(cp, Kind);
}
std::uint32_t towlower_impl(std::uint32_t cp) { return static_cast<std::uint32_t>(utf8proc_tolower(cp)); }
std::uint32_t towupper_impl(std::uint32_t cp) { return static_cast<std::uint32_t>(utf8proc_toupper(cp)); }
std::uint64_t wctrans_impl(const char* name) {
  if (!std::strcmp(name, "tolower")) return 1;
  if (!std::strcmp(name, "toupper")) return 2;
  return 0;
}
std::uint32_t towctrans_impl(std::uint32_t cp, std::uint64_t op) {
  if (op == 1) return towlower_impl(cp);
  if (op == 2) return towupper_impl(cp);
  set_error(err::kEINVAL);
  return 0;
}
// bionic upstream-openbsd ctype_.c and tolower_.c: exported symbols are
// pointers to 257-entry tables. EOF is entry zero; table[c+1] indexes a byte.
std::array<std::uint8_t, 257> ctype_table{};
std::array<std::int16_t, 257> lower_table{}, upper_table{};
const std::uint8_t* ctype_ptr = ctype_table.data();
const std::int16_t* lower_ptr = lower_table.data();
const std::int16_t* upper_ptr = upper_table.data();
struct Locale {
  std::uint64_t mb_max;
};
std::atomic<bool> global_utf8{true};
thread_local GuestAddr local_locale = UINT64_MAX;
bool valid_locale(const char* n) {
  return n && (!*n || !std::strcmp(n, "C") || !std::strcmp(n, "POSIX") || !std::strcmp(n, "C.UTF-8") ||
               !std::strcmp(n, "en_US.UTF-8"));
}
std::uint64_t mb_max(GuestAddr locale) {
  return locale == UINT64_MAX ? (global_utf8.load() ? 4 : 1) : g2h<Locale>(locale)->mb_max;
}
std::uint64_t mb_cur_max() { return mb_max(local_locale); }
char* setlocale_impl(int category, const char* name) {
  if (category < 0 || category > 12) {
    set_error(err::kEINVAL);
    return nullptr;
  }
  if (name) {
    if (!valid_locale(name)) {
      set_error(err::kENOENT);
      return nullptr;
    }
    global_utf8.store(!*name || std::strstr(name, "UTF-8"));
  }
  return const_cast<char*>(global_utf8.load() ? "C.UTF-8" : "C");
}
char* setlocale32(int category, const char* name) {
  auto* text = setlocale_impl(category, name);
  return text ? const_cast<char*>(low_string(text)) : nullptr;
}
GuestAddr newlocale_impl(int mask, const char* name, GuestAddr) {
  if ((mask & ~0x1fbf) || !name) {
    set_error(err::kEINVAL);
    return 0;
  }
  if (!valid_locale(name)) {
    set_error(err::kENOENT);
    return 0;
  }
  auto l = static_cast<Locale*>(allocate(sizeof(Locale)));
  if (l) l->mb_max = !*name || std::strstr(name, "UTF-8") ? 4 : 1;
  return h2g(l);
}
GuestAddr uselocale_impl(GuestAddr locale) {
  auto old = local_locale;
  if (locale) local_locale = locale;
  return old;
}
GuestAddr duplocale_impl(GuestAddr locale) {
  auto p = static_cast<Locale*>(allocate(sizeof(Locale)));
  if (p) p->mb_max = mb_max(locale);
  return h2g(p);
}
// bionic/libc/include/locale.h: 10 pointers followed by 14 char values.
struct LConv {
  const char* strings[10];
  std::uint8_t fields[14];
};
// AArch64 Android's plain char is unsigned: guest CHAR_MAX is 255, not 127.
LConv* localeconv_impl() {
  static LConv value = [] {
    LConv l{};
    std::fill_n(l.strings, 10, "");
    l.strings[0] = ".";
    std::fill_n(l.fields, 14, 255);
    return l;
  }();
  return &value;
}
LConv* localeconv32() {
  // bionic locale.h ILP32: ten 4-byte pointers and fourteen unsigned chars.
  static const auto p = [] {
    struct Wire {
      std::uint32_t strings[10];
      std::uint8_t fields[14];
    } wire{};
    std::fill_n(wire.strings, 10, h2g32(low_string("")));
    wire.strings[0] = h2g32(low_string("."));
    std::fill_n(wire.fields, 14, 255);
    return low_data(std::as_bytes(std::span(&wire, 1)));
  }();
  return g2h<LConv>(p);  // already the guest wire layout; never dereferenced as host LConv
}
const char* langinfo_l(int item, GuestAddr locale) {
  static const char* values[] = {"",          "",         "%a %b %e %H:%M:%S %Y",
                                 "%m/%d/%y",  "%H:%M:%S", "%I:%M:%S %p",
                                 "AM",        "PM",       "Sunday",
                                 "Monday",    "Tuesday",  "Wednesday",
                                 "Thursday",  "Friday",   "Saturday",
                                 "Sun",       "Mon",      "Tue",
                                 "Wed",       "Thu",      "Fri",
                                 "Sat",       "January",  "February",
                                 "March",     "April",    "May",
                                 "June",      "July",     "August",
                                 "September", "October",  "November",
                                 "December",  "Jan",      "Feb",
                                 "Mar",       "Apr",      "May",
                                 "Jun",       "Jul",      "Aug",
                                 "Sep",       "Oct",      "Nov",
                                 "Dec"};
  if (item == 1) return mb_max(locale) == 4 ? "UTF-8" : "ANSI_X3.4-1968";
  if (item > 1 && item < 46) return values[item];
  if (item == 51) return ".";
  if (item == 53) return "^[yY]";
  if (item == 54) return "^[nN]";
  return "";
}
const char* langinfo(int item) { return langinfo_l(item, local_locale); }
const char* langinfo32(int item, GuestAddr locale) { return low_string(langinfo_l(item, locale)); }
const char* langinfo_current32(int item) { return langinfo32(item, local_locale); }
}  // namespace
void register_ctype_locale(HleLibrary& lib) {
  for (int c = -1; c < 256; ++c) {
    auto i = c + 1;
    lower_table[i] = static_cast<std::int16_t>(tolower_impl(c));
    upper_table[i] = static_cast<std::int16_t>(toupper_impl(c));
    ctype_table[i] = upper(c) * 1 | lower(c) * 2 | digit(c) * 4 | space(c) * 8 | punct(c) * 16 |
                     cntrl(c) * 32 | ((in(c, 'a', 'f') || in(c, 'A', 'F')) ? 64 : 0) | (c == ' ' ? 128 : 0);
  }
  if (process_config().isa() == GuestIsa::a32) {
    auto export_table = [&](const char* name, auto& table) {
      auto p = h2g32(g2h(low_data(std::as_bytes(std::span(table)))));
      lib.add_data(name, low_data(std::as_bytes(std::span(&p, 1))));
    };
    export_table("_ctype_", ctype_table);
    export_table("_tolower_tab_", lower_table);
    export_table("_toupper_tab_", upper_table);
  } else {
    lib.add_data("_ctype_", h2g(&ctype_ptr));
    lib.add_data("_tolower_tab_", h2g(&lower_ptr));
    lib.add_data("_toupper_tab_", h2g(&upper_ptr));
  }
#define CLASS(n, i)              \
  bind<&n>(lib, "is" #n);        \
  bind<&n>(lib, "is" #n "_l");   \
  bind<&wide<i>>(lib, "isw" #n); \
  bind<&wide<i>>(lib, "isw" #n "_l")
  CLASS(alnum, 1);
  CLASS(alpha, 2);
  CLASS(blank, 3);
  CLASS(cntrl, 4);
  CLASS(digit, 5);
  CLASS(graph, 6);
  CLASS(lower, 7);
  CLASS(print, 8);
  CLASS(punct, 9);
  CLASS(space, 10);
  CLASS(upper, 11);
  CLASS(xdigit, 12);
#undef CLASS
#define B(n, f) bind<&f>(lib, n)
  B("isascii", ascii);
  B("toascii", toascii_impl);
  B("tolower", tolower_impl);
  B("tolower_l", tolower_impl);
  B("toupper", toupper_impl);
  B("toupper_l", toupper_impl);
  B("_tolower", tolower_unchecked);
  B("_toupper", toupper_unchecked);
  B("wctype", wctype_impl);
  B("wctype_l", wctype_impl);
  B("iswctype", iswctype_impl);
  B("iswctype_l", iswctype_impl);
  B("wctrans", wctrans_impl);
  B("wctrans_l", wctrans_impl);
  B("towctrans", towctrans_impl);
  B("towctrans_l", towctrans_impl);
  B("towlower", towlower_impl);
  B("towlower_l", towlower_impl);
  B("towupper", towupper_impl);
  B("towupper_l", towupper_impl);
  B("newlocale", newlocale_impl);
  B("uselocale", uselocale_impl);
  B("freelocale", deallocate);
  B("duplocale", duplocale_impl);
  if (process_config().isa() == GuestIsa::a32) {
    B("setlocale", setlocale32);
    B("localeconv", localeconv32);
    B("nl_langinfo", langinfo_current32);
    B("nl_langinfo_l", langinfo32);
  } else {
    B("setlocale", setlocale_impl);
    B("localeconv", localeconv_impl);
    B("nl_langinfo", langinfo);
    B("nl_langinfo_l", langinfo_l);
  }
  B("__ctype_get_mb_cur_max", mb_cur_max);
#undef B
  extern void register_multibyte(HleLibrary&);
  register_multibyte(lib);
}
}  // namespace venture::hle::libc
