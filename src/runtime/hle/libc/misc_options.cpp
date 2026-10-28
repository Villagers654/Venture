#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "common/log.h"
#include "hle/libc/libc_a.h"
namespace venture::hle::libc {
namespace {
char* option_arg = nullptr;
int option_index = 1, option_error = 1, option_bad = '?', option_reset = 0;
struct Options32 {
  std::uint32_t arg;
  std::int32_t index = 1, error = 1, bad = '?', reset = 0;
};
GuestStorage options32_storage;
struct Options32Sync {
  Options32* wire = nullptr;
  Options32Sync() {
    wire = g2h<Options32>(options32_storage.address());
    option_arg = g2h<char>(wire->arg);
    option_index = wire->index;
    option_error = wire->error;
    option_bad = wire->bad;
    option_reset = wire->reset;
  }
  ~Options32Sync() {
    if (wire) *wire = {h2g32(option_arg), option_index, option_error, option_bad, option_reset};
  }
};
std::mutex options_mutex;
int first_nonoption = 1, last_nonoption = 1;
const char* next_option = nullptr;
bool finished = false;
// bionic getopt.h: name pointer, int has_arg, int* flag, int val, LP64 padding.
struct LongOption {
  const char* name;
  int has_arg;
  int* flag;
  int value;
};
static_assert(sizeof(LongOption) == 32);
int option_failure(bool missing, const char* options) {
  if (option_error && *options != ':')
    log::warn("libc", "getopt: {}",
              missing ? "option requires an argument" : "unrecognized or ambiguous option");
  return missing && *options == ':' ? ':' : '?';
}
template <bool Low>
int getopt_impl_common(int argc, char** argv, const char* options, const LongOption* longs, int* long_index,
                       bool long_only) {
  std::lock_guard lock(options_mutex);
  struct NoSync {};
  [[maybe_unused]] std::conditional_t<Low, Options32Sync, NoSync> sync;
  option_arg = nullptr;
  if (option_index <= 0 || option_reset) {
    option_index = 1;
    option_reset = 0;
    next_option = nullptr;
    first_nonoption = last_nonoption = 1;
    finished = false;
  }
  if (option_index == 1 && !next_option) {
    first_nonoption = last_nonoption = 1;
    finished = false;
  }
  bool ordered = *options == '+', return_nonoption = *options == '-';
  if (ordered || return_nonoption) ++options;
  if (finished) return -1;
  if (!next_option || !*next_option) {
    next_option = nullptr;
    if (last_nonoption > option_index) last_nonoption = option_index;
    if (first_nonoption > option_index) first_nonoption = option_index;
    if (!ordered && !return_nonoption) {
      if (first_nonoption != last_nonoption && last_nonoption != option_index) {
        std::rotate(argv + first_nonoption, argv + last_nonoption, argv + option_index);
        first_nonoption += option_index - last_nonoption;
        last_nonoption = option_index;
      } else if (last_nonoption != option_index)
        first_nonoption = option_index;
      while (option_index < argc && (argv[option_index][0] != '-' || !argv[option_index][1])) ++option_index;
      last_nonoption = option_index;
    }
    if (option_index >= argc) {
      if (first_nonoption != last_nonoption) option_index = first_nonoption;
      finished = true;
      return -1;
    }
    auto arg = argv[option_index];
    if (!std::strcmp(arg, "--")) {
      ++option_index;
      if (first_nonoption != last_nonoption && last_nonoption != option_index) {
        std::rotate(argv + first_nonoption, argv + last_nonoption, argv + option_index);
        first_nonoption += option_index - last_nonoption;
      } else if (first_nonoption == last_nonoption)
        first_nonoption = option_index;
      option_index = first_nonoption;
      finished = true;
      return -1;
    }
    if (arg[0] != '-' || !arg[1]) {
      if (ordered) {
        finished = true;
        return -1;
      }
      option_arg = argv[option_index++];
      return 1;
    }
    bool is_long = longs && (arg[1] == '-' || (long_only && (arg[2] || !std::strchr(options, arg[1]))));
    if (is_long) {
      const char* name = arg + (arg[1] == '-' ? 2 : 1);
      auto equal = std::strchr(name, '=');
      auto length = equal ? static_cast<std::size_t>(equal - name) : std::strlen(name);
      int match = -1;
      bool ambiguous = false;
      for (int i = 0; longs[i].name; ++i)
        if (!std::strncmp(name, longs[i].name, length)) {
          if (std::strlen(longs[i].name) == length) {
            match = i;
            ambiguous = false;
            break;
          }
          if (match >= 0)
            ambiguous = true;
          else
            match = i;
        }
      if (match < 0 && long_only && arg[1] != '-' && std::strchr(options, arg[1]))
        is_long = false;
      else {
        ++option_index;
        if (match < 0 || ambiguous) {
          option_bad = 0;
          return option_failure(false, options);
        }
        auto& opt = longs[match];
        if (equal) {
          if (!opt.has_arg) {
            option_bad = opt.value;
            return option_failure(false, options);
          }
          option_arg = const_cast<char*>(equal + 1);
        } else if (opt.has_arg == 1) {
          if (option_index >= argc) {
            option_bad = opt.value;
            return option_failure(true, options);
          }
          option_arg = argv[option_index++];
        }
        if (long_index) *long_index = match;
        if (opt.flag) {
          *opt.flag = opt.value;
          return 0;
        }
        return opt.value;
      }
    }
    next_option = arg + 1;
    ++option_index;
  }
  unsigned char ch = *next_option++;
  auto found = std::strchr(options, ch);
  if (!found || ch == ':') {
    option_bad = ch;
    return option_failure(false, options);
  }
  if (found[1] == ':') {
    if (*next_option) {
      option_arg = const_cast<char*>(next_option);
      next_option = nullptr;
    } else if (found[2] != ':') {
      next_option = nullptr;
      if (option_index >= argc) {
        option_bad = ch;
        return option_failure(true, options);
      }
      option_arg = argv[option_index++];
    }
  }
  return ch;
}
template <bool Low = false>
int getopt_impl(int argc, char** argv, const char* options) {
  return getopt_impl_common<Low>(argc, argv, options, nullptr, nullptr, false);
}
template <bool Low = false>
int getopt_long_impl(int argc, char** argv, const char* options, const LongOption* longs, int* index) {
  return getopt_impl_common<Low>(argc, argv, options, longs, index, false);
}
template <bool Low = false>
int getopt_long_only_impl(int argc, char** argv, const char* options, const LongOption* longs, int* index) {
  return getopt_impl_common<Low>(argc, argv, options, longs, index, true);
}
int fold(int c, int flags) { return (flags & 16) && c >= 'A' && c <= 'Z' ? c + 32 : c; }
bool char_class(std::string_view name, unsigned c) {
  bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'), digit = c >= '0' && c <= '9',
       space = c == ' ' || (c >= 9 && c <= 13);
  if (name == "alnum") return alpha || digit;
  if (name == "alpha") return alpha;
  if (name == "digit") return digit;
  if (name == "space") return space;
  if (name == "blank") return c == ' ' || c == '\t';
  if (name == "lower") return c >= 'a' && c <= 'z';
  if (name == "upper") return c >= 'A' && c <= 'Z';
  if (name == "xdigit") return digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  if (name == "cntrl") return c < 32 || c == 127;
  if (name == "print") return c >= 32 && c < 127;
  if (name == "graph") return c > 32 && c < 127;
  if (name == "punct") return c > 32 && c < 127 && !alpha && !digit;
  return false;
}
int fnmatch_impl(const char* pattern, const char* text, int flags) {
  std::string_view pat(pattern), str(text);
  std::unordered_map<std::uint64_t, bool> memo;
  auto match = [&](auto&& self, std::size_t p, std::size_t s) -> bool {
    auto key = (std::uint64_t(p) << 32) | s;
    if (auto it = memo.find(key); it != memo.end()) return it->second;
    auto run = [&]() -> bool {
      for (;;) {
        if (p == pat.size()) return s == str.size() || ((flags & 8) && str[s] == '/');
        char c = pat[p++];
        bool leading = s < str.size() && str[s] == '.' && (s == 0 || ((flags & 1) && str[s - 1] == '/'));
        if (c == '*') {
          if ((flags & 4) && leading) return false;
          while (p < pat.size() && pat[p] == '*') ++p;
          for (auto i = s;; ++i) {
            if (self(self, p, i)) return true;
            if (i == str.size() || ((flags & 1) && str[i] == '/')) break;
          }
          return false;
        }
        if (s == str.size()) return false;
        if (c == '?') {
          if (((flags & 1) && str[s] == '/') || ((flags & 4) && leading)) return false;
          ++s;
          continue;
        }
        if (c == '[') {
          auto q = p;
          bool inverse = q < pat.size() && (pat[q] == '!' || pat[q] == '^');
          if (inverse) ++q;
          bool matched = false, any = false;
          while (q < pat.size() && (pat[q] != ']' || !any)) {
            if (pat[q] == '[' && q + 1 < pat.size() && pat[q + 1] == ':') {
              auto end = pat.find(":]", q + 2);
              if (end != std::string_view::npos) {
                matched |= char_class(pat.substr(q + 2, end - q - 2), static_cast<unsigned char>(str[s]));
                q = end + 2;
                any = true;
                continue;
              }
            }
            unsigned a = static_cast<unsigned char>(pat[q++]);
            if (a == '\\' && !(flags & 2) && q < pat.size()) a = static_cast<unsigned char>(pat[q++]);
            unsigned b = a;
            if (q + 1 < pat.size() && pat[q] == '-' && pat[q + 1] != ']') {
              ++q;
              b = static_cast<unsigned char>(pat[q++]);
              if (b == '\\' && !(flags & 2) && q < pat.size()) b = static_cast<unsigned char>(pat[q++]);
            }
            auto ch = fold(static_cast<unsigned char>(str[s]), flags);
            matched |= ch >= fold(a, flags) && ch <= fold(b, flags);
            any = true;
          }
          if (q < pat.size()) {
            if (((flags & 1) && str[s] == '/') || ((flags & 4) && leading) || (matched == inverse))
              return false;
            p = q + 1;
            ++s;
            continue;
          }
        }
        if (c == '\\' && !(flags & 2)) {
          if (p == pat.size()) return false;
          c = pat[p++];
        }
        if (fold(static_cast<unsigned char>(c), flags) != fold(static_cast<unsigned char>(str[s++]), flags))
          return false;
      }
    };
    return memo[key] = run();
  };
  return match(match, 0, 0) ? 0 : 1;
}
}  // namespace
void register_options(HleLibrary& lib) {
  bind<&fnmatch_impl>(lib, "fnmatch");
  if (process_config().isa() == GuestIsa::a32) {
    bind<&getopt_impl<true>>(lib, "getopt");
    bind<&getopt_long_impl<true>>(lib, "getopt_long");
    bind<&getopt_long_only_impl<true>>(lib, "getopt_long_only");
    auto p = options32_storage.resize(sizeof(Options32));
    *g2h<Options32>(p) = {};
    lib.add_data("optarg", p + offsetof(Options32, arg));
    lib.add_data("optind", p + offsetof(Options32, index));
    lib.add_data("opterr", p + offsetof(Options32, error));
    lib.add_data("optopt", p + offsetof(Options32, bad));
    lib.add_data("optreset", p + offsetof(Options32, reset));
  } else {
    bind<&getopt_impl<>>(lib, "getopt");
    bind<&getopt_long_impl<>>(lib, "getopt_long");
    bind<&getopt_long_only_impl<>>(lib, "getopt_long_only");
    lib.add_data("optarg", h2g(&option_arg));
    lib.add_data("optind", h2g(&option_index));
    lib.add_data("opterr", h2g(&option_error));
    lib.add_data("optopt", h2g(&option_bad));
    lib.add_data("optreset", h2g(&option_reset));
  }
}
}  // namespace venture::hle::libc
