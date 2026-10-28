#include "launcher/system_fonts.h"

#include <dlfcn.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <type_traits>

namespace venture::launcher {
namespace {
// fontconfig, loaded at run time; declarations follow <fontconfig/fontconfig.h>.
struct Fontconfig {
  void* (*init)();  // FcInitLoadConfigAndFonts
  void* (*name_parse)(const unsigned char*);
  int (*substitute)(void*, void*, int);
  void (*default_substitute)(void*);
  void* (*match)(void*, void*, int*);
  int (*get_string)(const void*, const char*, int, unsigned char**);
  void (*destroy)(void*);
  void* config = nullptr;
  Fontconfig() {
    void* library = dlopen("libfontconfig.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) return;
    bool ok = true;
    auto bind = [&](auto& function, const char* name) {
      function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(library, name));
      ok = ok && function;
    };
    bind(init, "FcInitLoadConfigAndFonts");
    bind(name_parse, "FcNameParse");
    bind(substitute, "FcConfigSubstitute");
    bind(default_substitute, "FcDefaultSubstitute");
    bind(match, "FcFontMatch");
    bind(get_string, "FcPatternGetString");
    bind(destroy, "FcPatternDestroy");
    if (ok) config = init();
  }
};
constexpr int match_pattern = 0;  // FcMatchPattern
constexpr int result_match = 0;   // FcResultMatch
std::string lower(std::string s) {
  std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}
}  // namespace

std::string system_font(const char* pattern, bool family_only) {
  static const auto* const fc = new Fontconfig;  // kept until exit
  if (!fc->config) return {};
  void* request = fc->name_parse(reinterpret_cast<const unsigned char*>(pattern));
  if (!request) return {};
  fc->substitute(fc->config, request, match_pattern);
  fc->default_substitute(request);
  int result = 0;
  void* found = fc->match(fc->config, request, &result);
  fc->destroy(request);
  if (!found) return {};
  std::string path;
  unsigned char* value = nullptr;
  if (fc->get_string(found, "file", 0, &value) == result_match) path = reinterpret_cast<const char*>(value);
  if (family_only) {
    // The pattern's families precede ':' and are separated by ','.
    const std::string families = "," + lower(std::string(pattern, std::strcspn(pattern, ":"))) + ",";
    unsigned char* family = nullptr;
    if (fc->get_string(found, "family", 0, &family) != result_match ||
        families.find("," + lower(reinterpret_cast<const char*>(family)) + ",") == std::string::npos)
      path.clear();
  }
  fc->destroy(found);
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec) ? path : std::string{};
}
}  // namespace venture::launcher
