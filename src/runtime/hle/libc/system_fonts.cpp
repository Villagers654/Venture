#include "system_fonts.h"
#include "common/log.h"
#include "common/paths.h"
#include <algorithm>
#include <array>
#include <format>
#include <mutex>

namespace venture::hle::libc::system_fonts {
namespace {
using venture::system_fonts::Face;
using venture::system_fonts::Query;
namespace scripts = venture::system_fonts::scripts;

// The Android file names (with an alternate when a collection's faces come from
// several host files) and what each is made of on the host.
struct Slot {
  std::string_view name;
  Query query;
  std::string_view alternate = {};
};
constexpr std::string_view sans[] = {"segoeui.ttf", "arial.ttf"};
constexpr std::string_view sans_bold[] = {"segoeuib.ttf", "arialbd.ttf", "segoeui.ttf"};
constexpr std::string_view sans_italic[] = {"segoeuii.ttf", "ariali.ttf", "segoeui.ttf"};
constexpr std::string_view sans_bold_italic[] = {"segoeuiz.ttf", "arialbi.ttf", "segoeuib.ttf"};
constexpr std::string_view serif[] = {"times.ttf", "georgia.ttf"};
constexpr std::string_view serif_bold[] = {"timesbd.ttf", "georgiab.ttf", "times.ttf"};
constexpr std::string_view serif_italic[] = {"timesi.ttf", "georgiai.ttf", "times.ttf"};
constexpr std::string_view serif_bold_italic[] = {"timesbi.ttf", "georgiaz.ttf", "timesbd.ttf"};
constexpr std::string_view mono[] = {"consola.ttf", "cour.ttf"};
constexpr std::string_view arabic[] = {"segoeui.ttf", "arial.ttf", "tahoma.ttf"};
constexpr std::string_view ethiopic[] = {"ebrima.ttf", "nyala.ttf"};
constexpr std::string_view hebrew[] = {"segoeui.ttf", "arial.ttf"};
constexpr std::string_view thai[] = {"leelawui.ttf", "tahoma.ttf"};
constexpr std::string_view armenian[] = {"segoeui.ttf", "arial.ttf"};
constexpr std::string_view georgian[] = {"segoeui.ttf", "sylfaen.ttf"};
constexpr std::string_view devanagari[] = {"Nirmala.ttc", "Nirmala.ttf", "mangal.ttf"};
constexpr std::string_view bengali[] = {"Nirmala.ttc", "Nirmala.ttf", "vrinda.ttf"};
constexpr std::string_view tamil[] = {"Nirmala.ttc", "Nirmala.ttf", "latha.ttf"};
enum : int {
  roboto, roboto_bold, roboto_italic, roboto_bold_italic, noto_serif, noto_serif_bold, noto_serif_italic,
  noto_serif_bold_italic, droid_sans_mono, droid_sans, droid_sans_bold, naskh_arabic, sans_ethiopic, sans_hebrew,
  sans_thai, sans_armenian, sans_georgian, sans_devanagari, sans_bengali, sans_tamil, symbols, cjk_sc, cjk_tc, cjk_jp,
  cjk_kr, color_emoji, slot_count
};
constexpr std::array<Slot, slot_count> slots{{
    {"Roboto-Regular.ttf", {sans, "Roboto,Noto Sans,sans-serif:style=Regular", U'A'}},
    {"Roboto-Bold.ttf", {sans_bold, "Roboto,Noto Sans,sans-serif:weight=bold", U'A'}},
    {"Roboto-Italic.ttf", {sans_italic, "Roboto,Noto Sans,sans-serif:slant=italic", U'A'}},
    {"Roboto-BoldItalic.ttf", {sans_bold_italic, "Roboto,Noto Sans,sans-serif:weight=bold:slant=italic", U'A'}},
    {"NotoSerif-Regular.ttf", {serif, "Noto Serif,serif:style=Regular", U'A'}},
    {"NotoSerif-Bold.ttf", {serif_bold, "Noto Serif,serif:weight=bold", U'A'}},
    {"NotoSerif-Italic.ttf", {serif_italic, "Noto Serif,serif:slant=italic", U'A'}},
    {"NotoSerif-BoldItalic.ttf", {serif_bold_italic, "Noto Serif,serif:weight=bold:slant=italic", U'A'}},
    {"DroidSansMono.ttf", {mono, "Droid Sans Mono,Noto Sans Mono,DejaVu Sans Mono,monospace", U'A'}},
    // Android keeps these names as links to Roboto.
    {"DroidSans.ttf", {sans, "Roboto,Noto Sans,sans-serif:style=Regular", U'A'}},
    {"DroidSans-Bold.ttf", {sans_bold, "Roboto,Noto Sans,sans-serif:weight=bold", U'A'}},
    {"NotoNaskhArabic-Regular.ttf", {arabic, "Noto Naskh Arabic,Noto Sans Arabic,sans-serif:lang=ar", char32_t{0x0627}}},
    {"NotoSansEthiopic-Regular.ttf", {ethiopic, "Noto Sans Ethiopic,sans-serif:lang=am", char32_t{0x1200}}},
    {"NotoSansHebrew-Regular.ttf", {hebrew, "Noto Sans Hebrew,sans-serif:lang=he", char32_t{0x05D0}}},
    {"NotoSansThai-Regular.ttf", {thai, "Noto Sans Thai,sans-serif:lang=th", char32_t{0x0E01}}},
    {"NotoSansArmenian-Regular.ttf", {armenian, "Noto Sans Armenian,sans-serif:lang=hy", char32_t{0x0531}}},
    {"NotoSansGeorgian-Regular.ttf", {georgian, "Noto Sans Georgian,sans-serif:lang=ka", char32_t{0x10D0}}},
    {"NotoSansDevanagari-Regular.ttf", {devanagari, "Noto Sans Devanagari,sans-serif:lang=hi", char32_t{0x0915}}},
    {"NotoSansBengali-Regular.ttf", {bengali, "Noto Sans Bengali,sans-serif:lang=bn", char32_t{0x0995}}},
    {"NotoSansTamil-Regular.ttf", {tamil, "Noto Sans Tamil,sans-serif:lang=ta", char32_t{0x0B95}}},
    {"NotoSansSymbols-Regular-Subsetted.ttf", scripts::symbols},
    // One collection on a Quest (faces JP, KR, SC, TC); a host's CJK fonts are
    // often separate files, which then keep their own names.
    {"NotoSansCJK-Regular.ttc", scripts::simplified_chinese, "NotoSansSC-Regular.otf"},
    {"NotoSansCJK-Regular.ttc", scripts::traditional_chinese, "NotoSansTC-Regular.otf"},
    {"NotoSansCJK-Regular.ttc", scripts::japanese, "NotoSansJP-Regular.otf"},
    {"NotoSansCJK-Regular.ttc", scripts::korean, "NotoSansKR-Regular.otf"},
    {"NotoColorEmoji.ttf", scripts::emoji},
}};
struct Font {
  int weight;
  bool italic;
  int slot;
};
struct Family {
  std::string_view name, lang;
  std::vector<Font> fonts;
};
// Android 12's fonts.xml order: the named families (the first is the default),
// then the fallback chain, searched in order for a glyph.
const std::vector<Family>& families() {
  static const std::vector<Family> value{
      {"sans-serif", {}, {{400, false, roboto}, {400, true, roboto_italic}, {700, false, roboto_bold},
                          {700, true, roboto_bold_italic}}},
      {"serif", {}, {{400, false, noto_serif}, {400, true, noto_serif_italic}, {700, false, noto_serif_bold},
                     {700, true, noto_serif_bold_italic}}},
      {"monospace", {}, {{400, false, droid_sans_mono}}},
      {{}, "und-Arab", {{400, false, naskh_arabic}}},
      {{}, "und-Ethi", {{400, false, sans_ethiopic}}},
      {{}, "und-Hebr", {{400, false, sans_hebrew}}},
      {{}, "und-Thai", {{400, false, sans_thai}}},
      {{}, "und-Armn", {{400, false, sans_armenian}}},
      {{}, "und-Geor,und-Geok", {{400, false, sans_georgian}}},
      {{}, "und-Deva", {{400, false, sans_devanagari}}},
      {{}, "und-Beng", {{400, false, sans_bengali}}},
      {{}, "und-Taml", {{400, false, sans_tamil}}},
      {{}, "und-Zsym", {{400, false, symbols}}},
      {{}, "zh-Hans", {{400, false, cjk_sc}}},
      {{}, "zh-Hant,zh-Bopo", {{400, false, cjk_tc}}},
      {{}, "ja", {{400, false, cjk_jp}}},
      {{}, "ko", {{400, false, cjk_kr}}},
      {{}, "und-Zsye", {{400, false, color_emoji}}},
  };
  return value;
}
struct Alias {
  std::string_view name, to;
  int weight = 0;
};
constexpr Alias aliases[] = {
    {"sans-serif-thin", "sans-serif", 100}, {"sans-serif-light", "sans-serif", 300},
    {"sans-serif-medium", "sans-serif", 500}, {"sans-serif-black", "sans-serif", 900},
    {"arial", "sans-serif"}, {"helvetica", "sans-serif"}, {"tahoma", "sans-serif"}, {"verdana", "sans-serif"},
    {"times", "serif"}, {"times new roman", "serif"}, {"palatino", "serif"}, {"georgia", "serif"},
    {"baskerville", "serif"}, {"goudy", "serif"}, {"fantasy", "serif"}, {"ITC Stone Serif", "serif"},
    {"sans-serif-monospace", "monospace"}, {"monaco", "monospace"}, {"courier", "monospace"},
    {"courier new", "monospace"},
};
std::optional<Face> host_face(const Query& query) {
  auto face = venture::system_fonts::find(query);
  if (!face) return std::nullopt;
  // The VFS opens the file itself, never through a link.
  std::error_code ec;
  auto real = std::filesystem::canonical(face->file, ec);
  if (ec) return std::nullopt;
  face->file = std::move(real);
  return face;
}
std::mutex catalog_mutex;
std::shared_ptr<const Catalog> current;
}  // namespace

const File* Catalog::find(std::string_view name) const {
  for (const auto& file : files)
    if (file.name == name) return &file;
  return nullptr;
}
Catalog build(const Finder& find) {
  Catalog out;
  // Each slot's entry in out.files and the face index fonts.xml names.
  struct Pick {
    std::size_t file;
    int index;
  };
  std::array<std::optional<Pick>, slot_count> picks;
  for (int i = 0; i < slot_count; ++i) {
    const auto& slot = slots[i];
    auto face = find(slot.query);
    if (!face) continue;
    // Another face of a collection already listed under this slot's names shares it.
    auto shared = std::ranges::find_if(out.files, [&](const File& f) {
      return f.host.file == face->file && (f.name == slot.name || f.name == slot.alternate);
    });
    if (shared == out.files.end()) {
      std::string name(slot.name);
      if (out.find(name)) name = slot.alternate;
      if (name.empty() || out.find(name)) continue;
      out.files.push_back({std::move(name), *face});
      shared = out.files.end() - 1;
    }
    picks[i] = Pick{std::size_t(shared - out.files.begin()), face->index};
  }
  std::string& xml = out.fonts_xml;
  xml =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<!--\n"
      "    The system fonts of a Quest (Android 12), each one of this computer's\n"
      "    installed fonts under its Android name. The first family is the default;\n"
      "    families without a name are the fallback chain, searched in order.\n"
      "-->\n"
      "<familyset version=\"23\">\n";
  std::vector<std::string_view> named;
  bool aliased = false;
  // Aliases follow the named families, as in Android's file.
  const auto write_aliases = [&] {
    aliased = true;
    for (const auto& alias : aliases)
      if (std::ranges::find(named, alias.to) != named.end())
        xml += std::format("    <alias name=\"{}\" to=\"{}\"{} />\n", alias.name, alias.to,
                           alias.weight ? std::format(" weight=\"{}\"", alias.weight) : std::string());
  };
  for (const auto& family : families()) {
    if (family.name.empty() && !aliased) write_aliases();
    std::string fonts;
    for (const auto& font : family.fonts) {
      if (!picks[font.slot]) continue;
      const auto& pick = *picks[font.slot];
      fonts += std::format("        <font weight=\"{}\" style=\"{}\"{}>{}</font>\n", font.weight,
                           font.italic ? "italic" : "normal",
                           pick.index ? std::format(" index=\"{}\"", pick.index) : std::string(),
                           out.files[pick.file].name);
    }
    if (fonts.empty()) continue;
    if (!family.name.empty()) {
      xml += std::format("    <family name=\"{}\">\n", family.name);
      named.push_back(family.name);
    } else {
      xml += std::format("    <family lang=\"{}\">\n", family.lang);
    }
    xml += fonts + "    </family>\n";
  }
  if (!aliased) write_aliases();
  xml += "</familyset>\n";
  return out;
}
Catalog host_catalog() { return build(host_face); }
std::shared_ptr<const Catalog> catalog() {
  std::lock_guard lock(catalog_mutex);
  if (!current) {
    auto built = std::make_shared<Catalog>(host_catalog());
    std::string names;
    for (const auto& file : built->files)
      names += std::format("{}{}={}#{}", names.empty() ? "" : ", ", file.name, paths::utf8(file.host.file.filename()),
                           file.host.index);
    log::info("fonts", "/system/fonts from the host's fonts: {}", names.empty() ? "none found" : names);
    current = std::move(built);
  }
  return current;
}
void set_catalog(Catalog value) {
  std::lock_guard lock(catalog_mutex);
  current = std::make_shared<const Catalog>(std::move(value));
}
std::filesystem::path host_file(std::string_view guest_path) {
  if (!guest_path.starts_with(directory) || guest_path.size() <= directory.size() + 1 ||
      guest_path[directory.size()] != '/')
    return {};
  const auto name = guest_path.substr(directory.size() + 1);
  if (name.find('/') != name.npos) return {};
  const auto files = catalog();
  const auto* file = files->find(name);
  return file ? file->host.file : std::filesystem::path{};
}
bool is_virtual(std::string_view p) {
  const auto below = [&](std::string_view root) {
    return p == root || (p.starts_with(root) && p.size() > root.size() && p[root.size()] == '/');
  };
  return p == etc_directory || p == fonts_xml_path || below(directory);
}
}  // namespace venture::hle::libc::system_fonts

