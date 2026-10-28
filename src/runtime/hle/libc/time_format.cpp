#include "hle/libc/time_format.h"
#include "abi32.h"

#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "hle/libc/libc_a.h"
#include "common/log.h"
#include "platform/libc_host.h"
namespace venture::hle::libc {
namespace {
std::int64_t floor_div(std::int64_t a, std::int64_t b) {
  auto q = a / b;
  return q - ((a % b) < 0);
}
std::int64_t mod(std::int64_t a, std::int64_t b) { return a - floor_div(a, b) * b; }
// Proleptic Gregorian civil/day conversion, era decomposition as described by
// Howard Hinnant, https://howardhinnant.github.io/date_algorithms.html.
std::int64_t days(std::int64_t y, int m, int d) {
  y -= m <= 2;
  auto era = floor_div(y, 400), yoe = y - era * 400;
  auto doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
bool utc(std::int64_t t, GuestTm& out) {
  auto day = floor_div(t, 86400), second = mod(t, 86400), z = day + 719468;
  auto era = floor_div(z, 146097), doe = z - era * 146097,
       yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  auto y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  auto d = doy - (153 * mp + 2) / 5 + 1, m = mp + (mp < 10 ? 3 : -9);
  y += m <= 2;
  if (y - 1900 < INT32_MIN || y - 1900 > INT32_MAX) {
    set_error(err::kEOVERFLOW);
    return false;
  }
  out = {static_cast<int>(second % 60),
         static_cast<int>(second / 60 % 60),
         static_cast<int>(second / 3600),
         static_cast<int>(d),
         static_cast<int>(m - 1),
         static_cast<int>(y - 1900),
         static_cast<int>(mod(day + 4, 7)),
         static_cast<int>(day - days(y, 1, 1)),
         0,
         0,
         "UTC"};
  return true;
}
std::mutex zone_mutex;
std::int64_t seconds_west = 0;
std::int32_t daylight_value = 0;
const char* zone_names[2] = {"UTC", "UTC"};
struct Zone32 {
  std::uint32_t names[2];
  std::int32_t west, daylight;
};
GuestStorage zone32_storage;
void publish_zone32() {
  auto* out = g2h<Zone32>(zone32_storage.resize(sizeof(Zone32)));
  *out = {{h2g32(low_string(zone_names[0])), h2g32(low_string(zone_names[1]))},
          static_cast<std::int32_t>(seconds_west),
          daylight_value};
}
std::vector<std::unique_ptr<char[]>> saved_names;
bool fixed_zone = false;
// POSIX TZ's offset is seconds west of UTC (the opposite sign to tm_gmtoff).
// Parse the fixed-offset form without modifying the host process environment.
bool fixed_time_zone(std::string_view text, platform::TimeZone& zone) {
  if (text.empty()) { zone = {0, false, "UTC", "UTC"}; return true; }
  std::size_t end = 0;
  std::string name;
  if (text[0] == '<') {
    end = text.find('>');
    if (end == std::string_view::npos) return false;
    name = text.substr(1, end - 1); ++end;
  } else {
    while (end < text.size() && ((text[end] >= 'A' && text[end] <= 'Z') ||
           (text[end] >= 'a' && text[end] <= 'z'))) ++end;
    name = text.substr(0, end);
  }
  if (name.size() < 3) return false;
  if (end == text.size() && name != "UTC" && name != "GMT") return false;
  int sign = 1, offset = 0;
  if (end < text.size() && (text[end] == '+' || text[end] == '-')) sign = text[end++] == '-' ? -1 : 1;
  for (int field = 0; end < text.size() && field < 3; ++field) {
    int value = 0;
    auto [tail, error] = std::from_chars(text.data() + end, text.data() + text.size(), value);
    if (error != std::errc{} || value < 0 || value > (field ? 59 : 24)) return false;
    offset += value * (field == 0 ? 3600 : field == 1 ? 60 : 1);
    end = static_cast<std::size_t>(tail - text.data());
    if (end == text.size()) break;
    if (text[end++] != ':' || end == text.size()) return false;
  }
  if (end != text.size()) return false;
  zone = {sign * offset, false, name, name};
  return true;
}
template <bool Low = false>
void tzset_impl() {
  auto z = platform::time_zone();
  const auto* tz = Low ? getenv32("TZ") : guest_getenv("TZ");
  const bool fixed = tz != nullptr;
  if (tz && !fixed_time_zone(tz, z)) {
    log::warn("libc", "tzset: unsupported guest timezone rule; using UTC");
    set_error(err::kENOTSUP);
    z = {0, false, "UTC", "UTC"};
  }
  std::lock_guard lock(zone_mutex);
  fixed_zone = fixed;
  seconds_west = z.seconds_west;
  daylight_value = z.has_dst;
  for (int i = 0; i < 2; ++i) {
    const auto& s = i ? z.daylight : z.standard;
    auto p = std::make_unique<char[]>(s.size() + 1);
    std::memcpy(p.get(), s.c_str(), s.size() + 1);
    zone_names[i] = p.get();
    saved_names.push_back(std::move(p));
  }
  if constexpr (Low) publish_zone32();
}
void ensure_zone() {
  static std::once_flag once;
  std::call_once(once, tzset_impl<>);
}
GuestTm* gmtime_r_impl(const std::int64_t* t, GuestTm* out) { return utc(*t, *out) ? out : nullptr; }
GuestTm* localtime_r_impl(const std::int64_t* t, GuestTm* out) {
  ensure_zone();
  std::lock_guard lock(zone_mutex);
  std::tm host{};
  if (!fixed_zone && platform::local_time(*t, host)) {
    auto local = days(static_cast<std::int64_t>(host.tm_year) + 1900, host.tm_mon + 1, host.tm_mday) * 86400 +
                 host.tm_hour * 3600 + host.tm_min * 60 + host.tm_sec;
    *out = {host.tm_sec,
            host.tm_min,
            host.tm_hour,
            host.tm_mday,
            host.tm_mon,
            host.tm_year,
            host.tm_wday,
            host.tm_yday,
            host.tm_isdst,
            local - *t,
            zone_names[host.tm_isdst > 0]};
    return out;
  }
  // UCRT only accepts 1970..3000. Outside that range use the current standard
  // offset. Historical/future DST transitions outside the CRT range are approximate.
  if ((*t < 0 && seconds_west > 0 && *t < INT64_MIN + seconds_west) ||
      (*t > 0 && seconds_west < 0 && *t > INT64_MAX + seconds_west)) {
    set_error(err::kEOVERFLOW);
    return nullptr;
  }
  if (!utc(*t - seconds_west, *out)) return nullptr;
  out->gmtoff = -seconds_west;
  out->zone = zone_names[0];
  return out;
}
thread_local GuestTm static_tm{};
GuestTm* gmtime_impl(const std::int64_t* t) { return gmtime_r_impl(t, &static_tm); }
GuestTm* localtime_impl(const std::int64_t* t) { return localtime_r_impl(t, &static_tm); }
std::int64_t raw_time(const GuestTm& t) {
  auto y = std::int64_t(t.year) + 1900 + floor_div(t.mon, 12);
  auto m = static_cast<int>(mod(t.mon, 12)) + 1;
  return days(y, m, 1) * 86400 + (std::int64_t(t.mday) - 1) * 86400 + std::int64_t(t.hour) * 3600 +
         std::int64_t(t.min) * 60 + t.sec;
}
std::int64_t timegm_impl(GuestTm* t) {
  auto sec = raw_time(*t);
  GuestTm normalized;
  if (!utc(sec, normalized)) return -1;
  *t = normalized;
  return sec;
}
std::int64_t mktime_impl(GuestTm* t) {
  ensure_zone();
  std::tm host{};
  host.tm_sec = t->sec;
  host.tm_min = t->min;
  host.tm_hour = t->hour;
  host.tm_mday = t->mday;
  host.tm_mon = t->mon;
  host.tm_year = t->year;
  host.tm_isdst = t->isdst;
  std::int64_t result;
  {
    std::lock_guard lock(zone_mutex);
    if (fixed_zone || !platform::make_local_time(host, result))
      result = raw_time(*t) + seconds_west - (!fixed_zone && t->isdst > 0 ? 3600 : 0);
  }
  return localtime_r_impl(&result, t) ? result : -1;
}
constexpr const char* weekdays[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                    "Thursday", "Friday", "Saturday"};
constexpr const char* months[] = {"January", "February", "March",     "April",   "May",      "June",
                                  "July",    "August",   "September", "October", "November", "December"};
std::string dec(std::int64_t n, int width = 0, char fill = '0') {
  auto s = std::to_string(n);
  if (static_cast<int>(s.size()) < width) {
    if (n < 0 && fill == '0')
      s.insert(1, width - s.size(), '0');
    else
      s.insert(0, width - s.size(), fill);
  }
  return s;
}
std::string time_format(const char* fmt, const GuestTm& t, int depth = 0) {
  if (depth > 8) return {};
  std::string out;
  auto y = std::int64_t(t.year) + 1900;
  auto iso_day = days(y, t.mon + 1, t.mday);
  auto thursday = iso_day + 3 - mod(iso_day + 3, 7);
  GuestTm thu{};
  utc(thursday * 86400, thu);
  auto iso_year = std::int64_t(thu.year) + 1900;
  auto iso_week = (thursday - days(iso_year, 1, 1)) / 7 + 1;
  for (auto p = fmt; *p; ++p) {
    if (*p != '%') {
      out += *p;
      continue;
    }
    ++p;
    if (!*p) break;
    char fill = '0';
    bool no_pad = false, upper = false;
    int width = 0;
    while (*p == '_' || *p == '-' || *p == '0' || *p == '^' || *p == '#') {
      if (*p == '_') fill = ' ';
      if (*p == '-') no_pad = true;
      if (*p == '^') upper = true;
      ++p;
    }
    while (*p >= '0' && *p <= '9') {
      width = std::min(10000, width * 10 + *p - '0');
      ++p;
    }
    if (*p == 'E' || *p == 'O') ++p;
    if (!*p) break;
    std::string s;
    auto num = [&](auto n, int w) { return dec(n, no_pad ? 0 : width ? width : w, fill); };
    auto nested = [&](const char* f) { return time_format(f, t, depth + 1); };
    switch (*p) {
      case '%':
        s = "%";
        break;
      case 'n':
        s = "\n";
        break;
      case 't':
        s = "\t";
        break;
      case 'a':
      case 'A':
        if (t.wday >= 0 && t.wday < 7) s = weekdays[t.wday];
        if (*p == 'a') s.resize(std::min<std::size_t>(3, s.size()));
        break;
      case 'b':
      case 'h':
      case 'B':
        if (t.mon >= 0 && t.mon < 12) s = months[t.mon];
        if (*p != 'B') s.resize(std::min<std::size_t>(3, s.size()));
        break;
      case 'C':
        s = num(floor_div(y, 100), 2);
        break;
      case 'd':
        s = num(t.mday, 2);
        break;
      case 'e':
        s = dec(t.mday, 2, ' ');
        break;
      case 'H':
        s = num(t.hour, 2);
        break;
      case 'k':
        s = dec(t.hour, 2, ' ');
        break;
      case 'I':
        s = num(t.hour % 12 ? t.hour % 12 : 12, 2);
        break;
      case 'l':
        s = dec(t.hour % 12 ? t.hour % 12 : 12, 2, ' ');
        break;
      case 'j':
        s = num(t.yday + 1, 3);
        break;
      case 'm':
        s = num(t.mon + 1, 2);
        break;
      case 'M':
        s = num(t.min, 2);
        break;
      case 'S':
        s = num(t.sec, 2);
        break;
      case 'p':
        s = t.hour < 12 ? "AM" : "PM";
        break;
      case 'P':
        s = t.hour < 12 ? "am" : "pm";
        break;
      case 'y':
        s = num(mod(y, 100), 2);
        break;
      case 'Y':
        s = num(y, 4);
        break;
      case 'w':
        s = num(t.wday, 1);
        break;
      case 'u':
        s = num(t.wday ? t.wday : 7, 1);
        break;
      case 'U':
        s = num((t.yday + 7 - t.wday) / 7, 2);
        break;
      case 'W':
        s = num((t.yday + 7 - (t.wday + 6) % 7) / 7, 2);
        break;
      case 'V':
        s = num(iso_week, 2);
        break;
      case 'G':
        s = num(iso_year, 4);
        break;
      case 'g':
        s = num(mod(iso_year, 100), 2);
        break;
      case 'z':
        if (t.isdst >= 0) {
          auto offset = t.gmtoff;
          s = offset < 0 ? "-" : "+";
          offset = offset < 0 ? -offset : offset;
          s += dec(offset / 3600, 2) + dec(offset % 3600 / 60, 2);
        }
        break;
      case 'Z':
        if (t.zone) s = t.zone;
        break;
      case 's': {
        auto copy = t;
        s = std::to_string(mktime_impl(&copy));
        break;
      }
      case 'c':
        s = nested("%a %b %e %H:%M:%S %Y");
        break;
      case 'x':
      case 'D':
        s = nested("%m/%d/%y");
        break;
      case 'X':
      case 'T':
        s = nested("%H:%M:%S");
        break;
      case 'F':
        s = nested("%Y-%m-%d");
        break;
      case 'R':
        s = nested("%H:%M");
        break;
      case 'r':
        s = nested("%I:%M:%S %p");
        break;
      case '+':
        s = nested("%a %b %e %H:%M:%S %Z %Y");
        break;
      default:
        s = *p;
        break;
    }
    if (upper)
      for (auto& c : s)
        if (c >= 'a' && c <= 'z') c -= 32;
    out += s;
  }
  return out;
}
std::uint64_t strftime_impl(char* out, std::uint64_t n, const char* fmt, const GuestTm* t) {
  auto s = time_format(fmt, *t);
  if (s.size() >= n) return 0;
  std::memcpy(out, s.c_str(), s.size() + 1);
  return s.size();
}
std::uint64_t wcsftime_impl(std::uint32_t* out, std::uint64_t n, const std::uint32_t* fmt, const GuestTm* t) {
  std::string f;
  while (*fmt) {
    char b[4];
    auto count = encode_utf8(b, *fmt++);
    if (count < 0) return 0;
    f.append(b, count);
  }
  auto s = time_format(f.c_str(), *t);
  std::vector<std::uint32_t> wide;
  for (std::size_t i = 0; i < s.size();) {
    utf8proc_int32_t cp;
    auto count = utf8proc_iterate(reinterpret_cast<const unsigned char*>(s.data() + i), s.size() - i, &cp);
    if (count < 1) {
      set_error(err::kEILSEQ);
      return 0;
    }
    wide.push_back(cp);
    i += count;
  }
  if (wide.size() >= n) return 0;
  std::copy(wide.begin(), wide.end(), out);
  out[wide.size()] = 0;
  return wide.size();
}
char* asctime_r_impl(const GuestTm* t, char* out) {
  if (t->year < -1900 || t->year > 8099 || t->wday < 0 || t->wday > 6 || t->mon < 0 || t->mon > 11) {
    set_error(err::kEOVERFLOW);
    return nullptr;
  }
  auto s = time_format("%a %b %e %H:%M:%S %Y\n", *t);
  if (s.size() != 25) {
    set_error(err::kEOVERFLOW);
    return nullptr;
  }
  std::memcpy(out, s.c_str(), 26);
  return out;
}
thread_local char time_buffer[26];
char* asctime_impl(const GuestTm* t) { return asctime_r_impl(t, time_buffer); }
char* ctime_r_impl(const std::int64_t* t, char* out) {
  GuestTm value;
  return localtime_r_impl(t, &value) ? asctime_r_impl(&value, out) : nullptr;
}
char* ctime_impl(const std::int64_t* t) { return ctime_r_impl(t, time_buffer); }
double difftime_impl(std::int64_t a, std::int64_t b) {
  return static_cast<double>(static_cast<long double>(a) - static_cast<long double>(b));
}
bool same(char a, char b) { return (a | 32) == (b | 32); }
struct ParseState {
  bool year = false, month = false, day = false, yday = false, hour12 = false;
  int century = -1, short_year = -1, ampm = -1, week = -1, week_start = 0, iso_year = -1;
};
const char* parse_time(const char* p, const char* format, GuestTm& t, ParseState& state, int depth = 0) {
  if (depth > 8) return nullptr;
  auto whitespace = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
  auto number = [&](int lo, int hi, int count, int& out) {
    while (*p == ' ') ++p;
    auto start = p;
    int n = 0;
    while (count-- && *p >= '0' && *p <= '9') n = n * 10 + *p++ - '0';
    if (p == start || n < lo || n > hi) return false;
    out = n;
    return true;
  };
  auto names = [&](const char* const* table, int count, int& out) {
    for (int i = 0; i < count; ++i) {
      auto q = p;
      auto n = table[i];
      while (*n && *q && same(*q, *n)) {
        ++q;
        ++n;
      }
      if (!*n) {
        p = q;
        out = i;
        return true;
      }
      if (q - p >= 3) {
        p += 3;
        out = i;
        return true;
      }
    }
    return false;
  };
  for (auto f = format; *f; ++f) {
    if (whitespace(*f)) {
      while (whitespace(*p)) ++p;
      continue;
    }
    if (*f != '%') {
      if (*p++ != *f) return nullptr;
      continue;
    }
    ++f;
    if (*f == 'E' || *f == 'O') ++f;
    if (!*f) return nullptr;
    int n = 0;
    const char* sub = nullptr;
    switch (*f) {
      case '%':
        if (*p++ != '%') return nullptr;
        break;
      case 'n':
      case 't':
        while (whitespace(*p)) ++p;
        break;
      case 'a':
      case 'A':
        if (!names(weekdays, 7, t.wday)) return nullptr;
        break;
      case 'b':
      case 'B':
      case 'h':
        if (!names(months, 12, t.mon)) return nullptr;
        state.month = true;
        break;
      case 'd':
      case 'e':
        if (!number(1, 31, 2, t.mday)) return nullptr;
        state.day = true;
        break;
      case 'm':
        if (!number(1, 12, 2, n)) return nullptr;
        t.mon = n - 1;
        state.month = true;
        break;
      case 'H':
      case 'k':
        if (!number(0, 23, 2, t.hour)) return nullptr;
        break;
      case 'I':
      case 'l':
        if (!number(1, 12, 2, t.hour)) return nullptr;
        state.hour12 = true;
        break;
      case 'M':
        if (!number(0, 59, 2, t.min)) return nullptr;
        break;
      case 'S':
        if (!number(0, 60, 2, t.sec)) return nullptr;
        break;
      case 'j':
        if (!number(1, 366, 3, n)) return nullptr;
        t.yday = n - 1;
        state.yday = true;
        break;
      case 'Y':
        if (!number(0, 9999, 4, n)) return nullptr;
        t.year = n - 1900;
        state.year = true;
        break;
      case 'y':
        if (!number(0, 99, 2, state.short_year)) return nullptr;
        state.year = true;
        break;
      case 'C':
        if (!number(0, 99, 2, state.century)) return nullptr;
        state.year = true;
        break;
      case 'U':
      case 'W':
        if (!number(0, 53, 2, state.week)) return nullptr;
        state.week_start = *f == 'W';
        break;
      case 'V':
        if (!number(1, 53, 2, state.week)) return nullptr;
        state.week_start = 2;
        break;
      case 'G':
        if (!number(0, 9999, 4, state.iso_year)) return nullptr;
        break;
      case 'g':
        if (!number(0, 99, 2, n)) return nullptr;
        state.iso_year = n + (n <= 68 ? 2000 : 1900);
        break;
      case 'w':
        if (!number(0, 6, 1, t.wday)) return nullptr;
        break;
      case 'u':
        if (!number(1, 7, 1, n)) return nullptr;
        t.wday = n % 7;
        break;
      case 'p':
        if (same(p[0], 'a') && same(p[1], 'm'))
          state.ampm = 0;
        else if (same(p[0], 'p') && same(p[1], 'm'))
          state.ampm = 1;
        else
          return nullptr;
        p += 2;
        break;
      case 'z': {
        if (*p == 'Z') {
          ++p;
          t.gmtoff = 0;
          t.zone = "UTC";
          break;
        }
        int sign = *p == '-' ? -1 : 1;
        if (*p != '-' && *p != '+') return nullptr;
        ++p;
        int hh, mm;
        if (!number(0, 23, 2, hh)) return nullptr;
        if (*p == ':') ++p;
        if (!number(0, 59, 2, mm)) return nullptr;
        t.gmtoff = sign * (hh * 3600 + mm * 60);
        t.isdst = 0;
        break;
      }
      case 'Z': {
        ensure_zone();
        std::lock_guard lock(zone_mutex);
        bool found = false;
        for (auto zone : {"UTC", "GMT", zone_names[0], zone_names[1]}) {
          auto len = std::strlen(zone);
          if (len && !std::strncmp(p, zone, len)) {
            p += len;
            t.zone = zone;
            t.gmtoff = zone == zone_names[0]   ? -seconds_west
                       : zone == zone_names[1] ? -seconds_west + 3600
                                               : 0;
            t.isdst = zone == zone_names[1];
            found = true;
            break;
          }
        }
        if (!found) return nullptr;
        break;
      }
      case 's': {
        char* end;
        auto seconds = parse_signed(p, &end, 10);
        if (end == p || !localtime_r_impl(&seconds, &t)) return nullptr;
        p = end;
        break;
      }
      case 'F':
        sub = "%Y-%m-%d";
        break;
      case 'D':
      case 'x':
        sub = "%m/%d/%y";
        break;
      case 'T':
      case 'X':
        sub = "%H:%M:%S";
        break;
      case 'R':
        sub = "%H:%M";
        break;
      case 'r':
        sub = "%I:%M:%S %p";
        break;
      case 'c':
        sub = "%a %b %e %H:%M:%S %Y";
        break;
      default:
        return nullptr;
    }
    if (sub) {
      p = parse_time(p, sub, t, state, depth + 1);
      if (!p) return nullptr;
    }
  }
  return p;
}
char* strptime_impl(const char* input, const char* format, GuestTm* t) {
  ParseState state;
  auto p = parse_time(input, format, *t, state);
  if (!p) return nullptr;
  if (state.short_year >= 0)
    t->year = (state.century >= 0       ? state.century * 100
               : state.short_year <= 68 ? 2000
                                        : 1900) +
              state.short_year - 1900;
  else if (state.century >= 0)
    t->year = state.century * 100 - 1900;
  if (state.hour12) t->hour = t->hour % 12 + (state.ampm == 1 ? 12 : 0);
  if (state.week >= 0) {
    auto year = state.iso_year >= 0 ? state.iso_year : std::int64_t(t->year) + 1900;
    std::int64_t day;
    if (state.week_start == 2) {
      auto jan4 = days(year, 1, 4);
      day = jan4 - mod(jan4 + 3, 7) + (state.week - 1) * 7 + mod(t->wday + 6, 7);
    } else {
      auto jan1 = days(year, 1, 1);
      auto jan1weekday = mod(jan1 + 4 - state.week_start, 7);
      day = jan1 + mod(7 - jan1weekday, 7) + (state.week - 1) * 7 + mod(t->wday - state.week_start, 7);
    }
    GuestTm normalized;
    utc(day * 86400, normalized);
    t->year = normalized.year;
    t->mon = normalized.mon;
    t->mday = normalized.mday;
    t->wday = normalized.wday;
    t->yday = normalized.yday;
  } else if (state.year && state.yday) {
    GuestTm normalized;
    utc((days(std::int64_t(t->year) + 1900, 1, 1) + t->yday) * 86400, normalized);
    t->mon = normalized.mon;
    t->mday = normalized.mday;
    t->wday = normalized.wday;
  } else if (state.year && state.month && state.day) {
    auto d = days(std::int64_t(t->year) + 1900, t->mon + 1, t->mday);
    t->wday = static_cast<int>(mod(d + 4, 7));
    t->yday = static_cast<int>(d - days(std::int64_t(t->year) + 1900, 1, 1));
  }
  return const_cast<char*>(p);
}
}  // namespace
namespace {
// bionic time.h: nine ints, ARM long tm_gmtoff, pointer tm_zone; 44 bytes.
struct Tm32 { std::int32_t sec, min, hour, mday, mon, year, wday, yday, isdst, gmtoff; std::uint32_t zone; };
static_assert(sizeof(Tm32) == 44);
GuestTm widen(const Tm32& t) { return {t.sec,t.min,t.hour,t.mday,t.mon,t.year,t.wday,t.yday,t.isdst,t.gmtoff,g2h<const char>(t.zone)}; }
Tm32 narrow(const GuestTm& t) { return {t.sec,t.min,t.hour,t.mday,t.mon,t.year,t.wday,t.yday,t.isdst,std::int32_t(t.gmtoff), t.zone ? h2g32(low_string(t.zone)) : 0}; }
template<bool Local> Tm32* time_r32(const std::int32_t* time, Tm32* out) {
  std::int64_t value = *time; GuestTm t;
  if (!(Local ? localtime_r_impl(&value, &t) : gmtime_r_impl(&value, &t))) return nullptr;
  *out = narrow(t); return out;
}
template<bool Local> Tm32* time32(const std::int32_t* time) {
  thread_local GuestStorage storage;
  return time_r32<Local>(time, g2h<Tm32>(storage.resize(sizeof(Tm32))));
}
template<bool Local> std::int32_t mktime32(Tm32* out) {
  auto t = widen(*out); auto result = Local ? mktime_impl(&t) : timegm_impl(&t);
  if (result < INT32_MIN || result > INT32_MAX) { set_error(err::kEOVERFLOW); return -1; }
  *out = narrow(t); return static_cast<std::int32_t>(result);
}
std::uint64_t strftime32(char* out, std::uint64_t n, const char* fmt, const Tm32* t) {
  auto wide = widen(*t); return strftime_impl(out, n, fmt, &wide);
}
std::uint64_t wcsftime32(std::uint32_t* out, std::uint64_t n, const std::uint32_t* fmt, const Tm32* t) {
  auto wide = widen(*t); return wcsftime_impl(out, n, fmt, &wide);
}
char* strptime32(const char* input, const char* fmt, Tm32* t) {
  auto wide = widen(*t); auto* result = strptime_impl(input, fmt, &wide); if (result) *t = narrow(wide); return result;
}
char* asctime_r32(const Tm32* t, char* out) { auto wide = widen(*t); return asctime_r_impl(&wide, out); }
char* asctime32(const Tm32* t) { thread_local GuestStorage storage; return asctime_r32(t, g2h<char>(storage.resize(26))); }
char* ctime_r32(const std::int32_t* t, char* out) { std::int64_t wide = *t; return ctime_r_impl(&wide, out); }
char* ctime32(const std::int32_t* t) { thread_local GuestStorage storage; return ctime_r32(t, g2h<char>(storage.resize(26))); }
// Locale_t is an opaque token, ignored by the existing fixed C locale formatter.
std::uint64_t strftime_l32(char* out, std::uint64_t n, const char* fmt, const Tm32* t, std::uint32_t) { return strftime32(out,n,fmt,t); }
char* strptime_l32(const char* in, const char* fmt, Tm32* t, std::uint32_t) { return strptime32(in,fmt,t); }
}
void register_time_format(HleLibrary& lib) {
  ensure_zone();
  if (process_config().isa() == GuestIsa::a32) {
#define A(n, f) abi32::adapt<&f>(n)
    A("gmtime_r", time_r32<false>); A("localtime_r", time_r32<true>); A("gmtime", time32<false>); A("localtime", time32<true>);
    A("mktime", mktime32<true>); A("timelocal", mktime32<true>); A("timegm", mktime32<false>);
    A("strftime", strftime32); A("wcsftime", wcsftime32); A("strptime", strptime32);
    A("asctime_r", asctime_r32); A("asctime", asctime32); A("ctime_r", ctime_r32); A("ctime", ctime32);
    A("strftime_l", strftime_l32); A("strptime_l", strptime_l32);
#undef A
    tzset_impl<true>();
    bind<&tzset_impl<true>>(lib, "tzset");
    auto p = zone32_storage.address();
    lib.add_data("tzname", p);
    lib.add_data("timezone", p + offsetof(Zone32, west));
    lib.add_data("daylight", p + offsetof(Zone32, daylight));
  } else {
    bind<&tzset_impl<>>(lib, "tzset");
    lib.add_data("tzname", h2g(zone_names));
    lib.add_data("timezone", h2g(&seconds_west));
    lib.add_data("daylight", h2g(&daylight_value));
  }
#define B(n) bind<&n##_impl>(lib, #n)
  B(gmtime_r);
  B(gmtime);
  B(localtime_r);
  B(localtime);
  B(timegm);
  B(mktime);
  B(asctime_r);
  B(asctime);
  B(ctime_r);
  B(ctime);
  B(strftime);
  B(wcsftime);
  B(strptime);
  B(difftime);
#undef B
  bind<&mktime_impl>(lib, "timelocal");
  bind<&strftime_impl>(lib, "strftime_l");
  bind<&strptime_impl>(lib, "strptime_l");
}
}  // namespace venture::hle::libc
