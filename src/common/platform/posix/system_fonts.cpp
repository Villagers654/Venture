#include "common/system_fonts.h"

#include <dlfcn.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <type_traits>

namespace venture::system_fonts {
namespace {
// fontconfig, loaded at run time; declarations follow <fontconfig/fontconfig.h>.
struct Fontconfig {
  void* (*init)();  // FcInitLoadConfigAndFonts
  void* (*name_parse)(const unsigned char*);
  int (*substitute)(void*, void*, int);
  void (*default_substitute)(void*);
  void* (*match)(void*, void*, int*);
  int (*get_string)(const void*, const char*, int, unsigned char**);
  int (*get_integer)(const void*, const char*, int, int*);
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
    bind(get_integer, "FcPatternGetInteger");
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
std::optional<Face> fontconfig_face(const char* pattern, bool family_only) {
  static const auto* const fc = new Fontconfig;  // kept until exit
  if (!fc->config || !pattern) return std::nullopt;
  void* request = fc->name_parse(reinterpret_cast<const unsigned char*>(pattern));
  if (!request) return std::nullopt;
  fc->substitute(fc->config, request, match_pattern);
  fc->default_substitute(request);
  int result = 0;
  void* found = fc->match(fc->config, request, &result);
  fc->destroy(request);
  if (!found) return std::nullopt;
  Face face;
  unsigned char* value = nullptr;
  if (fc->get_string(found, "file", 0, &value) == result_match) face.file = reinterpret_cast<const char*>(value);
  if (int index = 0; fc->get_integer(found, "index", 0, &index) == result_match) face.index = index;
  if (family_only) {
    // The pattern's families precede ':' and are separated by ','.
    const std::string families = "," + lower(std::string(pattern, std::strcspn(pattern, ":"))) + ",";
    unsigned char* family = nullptr;
    if (fc->get_string(found, "family", 0, &family) != result_match ||
        families.find("," + lower(reinterpret_cast<const char*>(family)) + ",") == std::string::npos)
      face.file.clear();
  }
  fc->destroy(found);
  std::error_code ec;
  if (face.file.empty() || !std::filesystem::is_regular_file(face.file, ec)) return std::nullopt;
  return face;
}
}  // namespace

std::string fontconfig_file(const char* pattern, bool family_only) {
  auto face = fontconfig_face(pattern, family_only);
  return face ? face->file.string() : std::string{};
}
std::optional<Face> find(const Query& query) {
  auto face = fontconfig_face(query.fontconfig, query.family_only);
  if (face && query.sample && !covers(*face, query.sample)) return std::nullopt;
  return face;
}
}  // namespace venture::system_fonts
