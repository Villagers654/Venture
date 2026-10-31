#include "launcher/ui/style.h"
#include "launcher/system_fonts.h"
#include "common/paths.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <imgui_freetype.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <unordered_map>
#include <vector>

namespace venture::launcher::ui {
namespace {
float g_scale = 1;
Fonts g_fonts;
struct AnimState {
  float value;
  int frame;
};
std::unordered_map<ImGuiID, AnimState> g_anim;

std::string encode(char32_t cp) {
  std::string out;
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
  return out;
}

std::string first_existing(std::initializer_list<const char*> candidates) {
  for (const char* path : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return path;
  }
  return {};
}

#ifdef _WIN32
constexpr const char* kFontDir = "C:/Windows/Fonts/";
#endif

std::vector<std::string> g_fallbacks;  // merged after each base font: symbols, emoji

ImFont* add_font(const std::string& path, const std::string& icon_path) {
  auto& io = ImGui::GetIO();
  if (path.empty()) return nullptr;
  ImFontConfig base;
  base.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;
  ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), kBody, &base);
  if (!font) return nullptr;
  if (!icon_path.empty()) {
    ImFontConfig merge;
    merge.MergeMode = true;
    merge.PixelSnapH = true;
    merge.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;
    io.Fonts->AddFontFromFileTTF(icon_path.c_str(), kBody, &merge);
  }
  for (const std::string& fallback : g_fallbacks) {
    ImFontConfig merge;
    merge.MergeMode = true;
    merge.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting | ImGuiFreeTypeLoaderFlags_LoadColor;
    io.Fonts->AddFontFromFileTTF(fallback.c_str(), kBody, &merge);
  }
  return font;
}
}  // namespace

// --- color --------------------------------------------------------------------
namespace color {
ImVec4 v(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
ImU32 alpha(ImU32 c, float a) {
  ImVec4 f = v(c);
  f.w *= std::clamp(a, 0.f, 1.f);
  return ImGui::ColorConvertFloat4ToU32(f);
}
ImU32 mix(ImU32 a, ImU32 b, float t) {
  ImVec4 x = v(a), y = v(b);
  t = std::clamp(t, 0.f, 1.f);
  return ImGui::ColorConvertFloat4ToU32(
      {x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t});
}
}  // namespace color

// --- fonts ----------------------------------------------------------------------
Fonts& fonts() { return g_fonts; }

void load_fonts() {
  auto& io = ImGui::GetIO();
#ifdef _WIN32
  std::string dir = kFontDir;
  std::string icons = first_existing({"C:/Windows/Fonts/SegoeIcons.ttf", "C:/Windows/Fonts/segmdl2.ttf"});
  std::string regular = first_existing({"C:/Windows/Fonts/segoeui.ttf"});
  std::string semilight = first_existing({"C:/Windows/Fonts/segoeuisl.ttf", "C:/Windows/Fonts/segoeui.ttf"});
  std::string semibold = first_existing({"C:/Windows/Fonts/seguisb.ttf", "C:/Windows/Fonts/segoeuib.ttf"});
  std::string bold = first_existing({"C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/seguisb.ttf"});
  std::string italic = first_existing({"C:/Windows/Fonts/segoeuii.ttf"});
  for (const char* fallback : {"C:/Windows/Fonts/seguisym.ttf", "C:/Windows/Fonts/seguiemj.ttf"})
    if (std::string path = first_existing({fallback}); !path.empty()) g_fallbacks.push_back(path);
#else
  // fontconfig's choice: Inter, then Noto Sans, then the desktop's sans-serif.
  // Symbols and color emoji only when those families are installed.
  for (const char* fallback : {"Noto Sans Symbols 2", "Noto Color Emoji"})
    if (std::string path = system_font(fallback, true); !path.empty()) g_fallbacks.push_back(path);
  std::string icons;  // no system icon font on Linux yet; controls fall back to text only
  std::string regular = system_font("Inter,Noto Sans,sans-serif:style=Regular");
  std::string semilight = regular;
  std::string semibold = system_font("Inter,Noto Sans,sans-serif:weight=semibold");
  std::string bold = semibold;
  std::string italic = system_font("Inter,Noto Sans,sans-serif:slant=italic");
  // No fontconfig or fonts: the release's bundled DejaVu Sans (the JRE's fonts).
  if (regular.empty()) {
    std::error_code ec;
    auto fonts = paths::executable().parent_path() / "java/jdk/lib/fonts";
    if (fs::exists(fonts / "DejaVuSans.ttf", ec)) {
      regular = semilight = italic = (fonts / "DejaVuSans.ttf").string();
      semibold = bold = fs::exists(fonts / "DejaVuSans-Bold.ttf", ec)
          ? (fonts / "DejaVuSans-Bold.ttf").string() : regular;
    }
  }
#endif
  g_fonts.regular = add_font(regular, icons);
  if (!g_fonts.regular) {
    g_fonts.regular = io.Fonts->AddFontDefault();
    icons.clear();
  }
  g_fonts.semilight = add_font(semilight, icons);
  g_fonts.semibold = add_font(semibold, icons);
  g_fonts.bold = add_font(bold, icons);
  g_fonts.italic = add_font(italic, icons);
  if (!g_fonts.semilight) g_fonts.semilight = g_fonts.regular;
  if (!g_fonts.semibold) g_fonts.semibold = g_fonts.regular;
  if (!g_fonts.bold) g_fonts.bold = g_fonts.semibold;
  if (!g_fonts.italic) g_fonts.italic = g_fonts.regular;
  g_fonts.icons = !icons.empty();
  io.FontDefault = g_fonts.regular;
}

const char* icon(char32_t code_point) {
  static std::unordered_map<char32_t, std::string> cache;
  if (!g_fonts.icons || !code_point) return "";
  auto [it, inserted] = cache.try_emplace(code_point);
  if (inserted) it->second = encode(code_point);
  return it->second.c_str();
}

// --- scale & motion ----------------------------------------------------------
void set_scale(float value) { g_scale = value; }
float scale() { return g_scale; }

float anim(ImGuiID id, float target, float speed) {
  int frame = ImGui::GetFrameCount();
  auto [it, inserted] = g_anim.try_emplace(id, AnimState{target, frame});
  if (!inserted && it->second.frame != frame) {
    float dt = ImGui::GetIO().DeltaTime;
    float k = 1 - std::exp(-speed * dt);
    it->second.value += (target - it->second.value) * k;
    if (std::abs(target - it->second.value) < 0.001f) it->second.value = target;
    it->second.frame = frame;
  }
  return it->second.value;
}
float anim(const char* key, float target, float speed) { return anim(ImGui::GetID(key), target, speed); }

void apply_style() {
  ImGuiStyle& st = ImGui::GetStyle();
  st = ImGuiStyle{};
  ImGui::StyleColorsDark(&st);
  st.WindowPadding = {28, 24};
  st.FramePadding = {14, 9};
  st.ItemSpacing = {10, 10};
  st.ItemInnerSpacing = {8, 6};
  st.CellPadding = {0, 0};
  st.IndentSpacing = 18;
  st.ScrollbarSize = 6;
  st.GrabMinSize = 24;
  st.WindowRounding = 0;
  st.ChildRounding = 12;
  st.FrameRounding = 10;
  st.PopupRounding = 12;
  st.ScrollbarRounding = 6;
  st.GrabRounding = 6;
  st.TabRounding = 8;
  st.WindowBorderSize = 0;
  st.ChildBorderSize = 0;
  st.PopupBorderSize = 0;
  st.FrameBorderSize = 0;
  st.SeparatorTextBorderSize = 1;
  st.DisabledAlpha = 0.4f;
  st.SelectableTextAlign = {0, 0.5f};
  ImVec4* c = st.Colors;
  using color::v;
  c[ImGuiCol_Text] = v(color::text);
  c[ImGuiCol_TextDisabled] = v(color::text3);
  c[ImGuiCol_WindowBg] = v(color::bg);
  c[ImGuiCol_ChildBg] = v(IM_COL32(0, 0, 0, 0));
  c[ImGuiCol_PopupBg] = v(color::surface_hi);
  c[ImGuiCol_Border] = v(color::stroke);
  c[ImGuiCol_BorderShadow] = v(IM_COL32(0, 0, 0, 0));
  c[ImGuiCol_FrameBg] = v(color::surface);
  c[ImGuiCol_FrameBgHovered] = v(color::surface_hi);
  c[ImGuiCol_FrameBgActive] = v(color::surface_hi);
  c[ImGuiCol_TitleBg] = v(color::bg);
  c[ImGuiCol_TitleBgActive] = v(color::bg);
  c[ImGuiCol_ScrollbarBg] = v(IM_COL32(0, 0, 0, 0));
  c[ImGuiCol_ScrollbarGrab] = v(IM_COL32(255, 255, 255, 30));
  c[ImGuiCol_ScrollbarGrabHovered] = v(IM_COL32(255, 255, 255, 52));
  c[ImGuiCol_ScrollbarGrabActive] = v(IM_COL32(255, 255, 255, 70));
  c[ImGuiCol_CheckMark] = v(color::accent_text);
  c[ImGuiCol_SliderGrab] = v(color::accent);
  c[ImGuiCol_SliderGrabActive] = v(color::accent_hi);
  c[ImGuiCol_Button] = v(color::surface_hi);
  c[ImGuiCol_ButtonHovered] = v(color::surface_top);
  c[ImGuiCol_ButtonActive] = v(color::surface_top);
  c[ImGuiCol_Header] = v(color::surface_top);
  c[ImGuiCol_HeaderHovered] = v(color::surface_top);
  c[ImGuiCol_HeaderActive] = v(color::surface_top);
  c[ImGuiCol_Separator] = v(color::stroke);
  c[ImGuiCol_SeparatorHovered] = v(color::stroke_hi);
  c[ImGuiCol_SeparatorActive] = v(color::accent);
  c[ImGuiCol_ResizeGrip] = v(IM_COL32(0, 0, 0, 0));
  c[ImGuiCol_TextSelectedBg] = v(color::alpha(color::accent, 0.45f));
  c[ImGuiCol_NavCursor] = v(color::selected);
  c[ImGuiCol_ModalWindowDimBg] = v(IM_COL32(12, 13, 15, 180));
  c[ImGuiCol_PlotHistogram] = v(color::accent);
  // Lines are drawn as geometry: the baked line textures need linear
  // sampling, and the atlas is sampled nearest (see begin_frame).
  st.AntiAliasedLinesUseTex = false;
  st.ScaleAllSizes(g_scale);
  st.FontScaleDpi = g_scale;
}

// --- drawing ---------------------------------------------------------------------
// The glyph atlas is sampled nearest: glyphs keep the font's fractional
// advances, and each lands on whole pixels instead of being smeared across
// two by linear filtering. Artwork switches back to linear around its draw.
void begin_frame(bool crisp_glyphs) {
  auto& io = ImGui::GetPlatformIO();
  if (crisp_glyphs && io.DrawCallback_SetSamplerNearest)
    ImGui::GetBackgroundDrawList()->AddCallback(io.DrawCallback_SetSamplerNearest, nullptr);
}

void draw_image(ImDrawList* dl, ImTextureID texture, ImVec2 a, ImVec2 b, ImVec2 uv0, ImVec2 uv1, ImU32 col,
                float rounding, ImDrawFlags corners) {
  auto& io = ImGui::GetPlatformIO();
  bool switch_sampler = io.DrawCallback_SetSamplerLinear && io.DrawCallback_SetSamplerNearest;
  if (switch_sampler) dl->AddCallback(io.DrawCallback_SetSamplerLinear, nullptr);
  if (rounding > 0)
    dl->AddImageRounded(texture, a, b, uv0, uv1, col, rounding, corners);
  else
    dl->AddImage(texture, a, b, uv0, uv1, col);
  if (switch_sampler) dl->AddCallback(io.DrawCallback_SetSamplerNearest, nullptr);
}

void draw_text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, std::string_view text,
               float wrap_width) {
  dl->AddText(font, s(size), pos, col, text.data(), text.data() + text.size(), wrap_width);
}

ImVec2 measure(ImFont* font, float size, std::string_view text, float wrap_width) {
  return font->CalcTextSizeA(s(size), FLT_MAX, wrap_width, text.data(), text.data() + text.size());
}

std::string ellipsize(ImFont* font, float size, std::string_view text, float max_width) {
  if (measure(font, size, text).x <= max_width) return std::string(text);
  const std::string_view dots = "\xE2\x80\xA6";
  float dots_w = measure(font, size, dots).x;
  size_t lo = 0, hi = text.size();
  while (lo < hi) {
    size_t mid = (lo + hi + 1) / 2;
    while (mid < text.size() && (static_cast<unsigned char>(text[mid]) & 0xC0) == 0x80) ++mid;
    if (measure(font, size, text.substr(0, mid)).x + dots_w <= max_width) {
      lo = mid;
    } else {
      hi = mid - 1;
      while (hi > 0 && hi < text.size() && (static_cast<unsigned char>(text[hi]) & 0xC0) == 0x80) --hi;
    }
  }
  std::string out(text.substr(0, lo));
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out + std::string(dots);
}

void soft_shadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float spread, float strength) {
  constexpr int kLayers = 8;
  for (int i = kLayers; i >= 1; --i) {
    float t = float(i) / kLayers;
    float grow = spread * t;
    int alpha = int(255 * strength * (1 - t) * (1 - t) * 0.5f);
    if (alpha <= 0) continue;
    dl->AddRectFilled({a.x - grow, a.y - grow * 0.5f}, {b.x + grow, b.y + grow * 1.5f}, IM_COL32(0, 0, 0, alpha),
                      rounding + grow);
  }
}

void vertical_fade(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom) {
  dl->AddRectFilledMultiColor(a, b, top, top, bottom, bottom);
}

namespace {
bool g_on_card = false;
ImU32 field_bg(float t) {
  return g_on_card ? color::mix(color::surface_hi, color::surface_top, t) : color::mix(color::surface, color::surface_hi, t);
}
}  // namespace
void set_on_card(bool on_card) { g_on_card = on_card; }

void hline(ImDrawList* dl, float x0, float x1, float y, ImU32 col) {
  y = std::round(y);
  dl->AddRectFilled({x0, y}, {x1, y + std::max(1.f, std::round(s(1)))}, col);
}

namespace {
// Ink bounds of a glyph relative to its text origin, in pixels.
bool ink(ImFont* font, float size, ImWchar cp, ImVec4* out) {
  ImFontBaked* baked = font->GetFontBaked(s(size));
  const ImFontGlyph* g = baked ? baked->FindGlyphNoFallback(cp) : nullptr;
  if (!g || !g->Visible) return false;
  float k = s(size) / baked->Size;
  *out = {g->X0 * k, g->Y0 * k, g->X1 * k, g->Y1 * k};
  return true;
}
}  // namespace

float text_y(ImFont* font, float size, float top, float height) {
  // Center the cap height (the "H"), not the line box: Segoe UI's box has
  // more room above the caps than below the baseline, so box centering sits
  // text visibly low next to icons and inside pills.
  ImVec4 h;
  if (!ink(font, size, 'H', &h)) return std::round(top + (height - s(size)) * 0.5f);
  return std::round(top + (height - (h.w - h.y)) * 0.5f - h.y);
}

float cap_center(ImFont* font, float size, float top) {
  ImVec4 h;
  if (!ink(font, size, 'H', &h)) return std::round(top) + s(size) * 0.5f;
  return std::round(top) + (h.y + h.w) * 0.5f;
}

void draw_glyph(ImDrawList* dl, char32_t glyph, float size, ImVec2 center, ImU32 col) {
  const char* g = icon(glyph);
  if (!*g) return;
  // Icons are centered on their ink, which the icon font does not center in
  // its line box.
  ImVec4 k;
  ImVec2 pos;
  if (ink(g_fonts.regular, size, ImWchar(glyph), &k)) {
    pos = {std::round(center.x - (k.x + k.z) * 0.5f), std::round(center.y - (k.y + k.w) * 0.5f)};
  } else {
    ImVec2 gs = measure(g_fonts.regular, size, g);
    pos = {std::round(center.x - gs.x * 0.5f), std::round(center.y - gs.y * 0.5f)};
  }
  draw_text(dl, g_fonts.regular, size, pos, col, g);
}

// --- controls ----------------------------------------------------------------------
namespace {
// Shared hover/press bookkeeping for custom-drawn widgets.
struct Interaction {
  bool clicked, hovered, held;
  float hover;
};
Interaction interact(const char* id, ImVec2 size, bool enabled = true) {
  ImGui::BeginDisabled(!enabled);
  bool clicked = ImGui::InvisibleButton(id, size);
  ImGui::EndDisabled();
  bool hovered = enabled && ImGui::IsItemHovered();
  bool held = enabled && ImGui::IsItemActive();
  if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  float t = anim(ImGui::GetItemID(), hovered ? 1.f : 0.f, 20);
  return {clicked && enabled, hovered, held, t};
}
std::string_view visible_label(const char* id_label) {
  std::string_view full(id_label);
  return full.substr(0, full.find("##"));
}
}  // namespace

void text(ImFont* font, float size, ImU32 col, std::string_view value, bool wrap) {
  ImGui::PushFont(font, size);
  ImGui::PushStyleColor(ImGuiCol_Text, col);
  if (wrap) {
    bool outer = ImGui::GetCurrentWindow()->DC.TextWrapPos >= 0;
    if (!outer) ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(value.data(), value.data() + value.size());
    if (!outer) ImGui::PopTextWrapPos();
  } else {
    ImGui::TextUnformatted(value.data(), value.data() + value.size());
  }
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

void heading(std::string_view value) { text(g_fonts.semibold, kHeading, color::text, value, false); }

void gap(float px) { ImGui::Dummy({0, s(px)}); }

float button_width(std::string_view label, char32_t glyph) {
  const char* g = icon(glyph);
  float w = measure(g_fonts.semibold, kBody, label).x;
  if (*g) w += measure(g_fonts.regular, kBody, g).x + (label.empty() ? 0 : s(8));
  return w + s(label.empty() ? 22 : 40);
}

bool button(const char* id_label, char32_t glyph, Tone tone, ImVec2 size, bool enabled, ImTextureID image) {
  std::string_view shown = visible_label(id_label);
  ImFont* font = g_fonts.semibold;
  const char* glyph_text = image ? "" : icon(glyph);
  ImVec2 text_size = measure(font, kBody, shown);
  ImVec2 glyph_size = image ? ImVec2{s(16), s(16)} : *glyph_text ? measure(g_fonts.regular, kBody, glyph_text) : ImVec2{0, 0};
  float spacing = ((image || *glyph_text) && !shown.empty()) ? s(8) : 0;
  float content = text_size.x + glyph_size.x + spacing;
  if (size.x <= 0) size.x = button_width(shown, glyph);
  if (size.y <= 0) size.y = s(40);
  ImGui::PushID(id_label);
  ImVec2 p = ImGui::GetCursorScreenPos();
  auto in = interact("##button", size, enabled);
  float t = in.hover;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 q{p.x + size.x, p.y + size.y};
  float r = size.y * 0.5f;
  ImU32 bg = 0, fg = color::text;
  switch (tone) {
    case Tone::primary:
      bg = in.held ? color::accent_lo : color::mix(color::accent, color::accent_hi, t);
      fg = color::on_accent;
      break;
    case Tone::secondary:
      bg = in.held ? color::surface_top : color::mix(color::surface_hi, color::surface_top, t);
      break;
    case Tone::ghost:
      bg = color::alpha(color::surface_hi, in.held ? 1.f : t);
      fg = color::mix(color::text2, color::text, t);
      break;
    case Tone::danger:
      bg = in.held ? color::mix(color::danger, IM_COL32(0, 0, 0, 255), 0.15f)
                   : color::mix(color::danger, IM_COL32(255, 255, 255, 255), 0.08f * t);
      fg = IM_COL32_WHITE;
      break;
    case Tone::light:
      bg = in.held ? IM_COL32(200, 203, 209, 255) : color::mix(color::selected, IM_COL32_WHITE, t);
      fg = color::on_selected;
      break;
  }
  if (!enabled) {
    bg = color::alpha(bg, 0.4f);
    fg = color::alpha(fg, 0.45f);
  }
  dl->AddRectFilled(p, q, bg, r);
  float x = std::round(p.x + (size.x - content) * 0.5f);
  if (image) {
    float h = std::round(glyph_size.y * 0.5f), cy = std::round(p.y + size.y * 0.5f);
    draw_image(dl, image, {x, cy - h}, {x + glyph_size.x, cy + h}, {0, 0}, {1, 1}, fg);
    x += glyph_size.x + spacing;
  } else if (*glyph_text) {
    draw_glyph(dl, glyph, kBody, {x + glyph_size.x * 0.5f, p.y + size.y * 0.5f}, fg);
    x += glyph_size.x + spacing;
  }
  draw_text(dl, font, kBody, {x, text_y(font, kBody, p.y, size.y)}, fg, shown);
  ImGui::PopID();
  return in.clicked;
}

bool icon_button(const char* id, char32_t glyph, const char* tip, float size, bool enabled, bool filled) {
  ImGui::PushID(id);
  ImVec2 dim{s(size), s(size)};
  ImVec2 p = ImGui::GetCursorScreenPos();
  auto in = interact("##icon", dim, enabled);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 c{p.x + dim.x * 0.5f, p.y + dim.y * 0.5f};
  float rest = filled ? 1.f : 0.f;
  ImU32 bg = color::alpha(filled ? color::mix(color::surface_hi, color::surface_top, in.hover) : color::surface_hi,
                          std::max(rest, in.held ? 1.f : in.hover));
  dl->AddCircleFilled(c, dim.x * 0.5f, bg, 40);
  ImU32 fg = enabled ? color::mix(color::text2, color::text, std::max(in.hover, rest * 0.6f))
                     : color::alpha(color::text3, 0.6f);
  if (*icon(glyph))
    draw_glyph(dl, glyph, size >= 40 ? kBody + 1 : kBody, c, fg);
  else
    draw_text(dl, g_fonts.semibold, kBody, {std::round(c.x - s(6)), text_y(g_fonts.semibold, kBody, p.y, dim.y)}, fg,
              "...");
  if (in.hovered && tip && *tip) tooltip(tip);
  ImGui::PopID();
  return in.clicked;
}

bool filter_chip(const char* id_label, bool selected) {
  std::string_view shown = visible_label(id_label);
  ImFont* font = g_fonts.semibold;
  ImVec2 ts = measure(font, kSmall + 1, shown);
  ImVec2 size{ts.x + s(28), s(34)};
  ImGui::PushID(id_label);
  ImVec2 p = ImGui::GetCursorScreenPos();
  auto in = interact("##chip", size);
  float sel = anim(ImGui::GetID("sel"), selected ? 1.f : 0.f, 22);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 rest = color::mix(color::surface, color::surface_hi, in.hover);
  dl->AddRectFilled(p, {p.x + size.x, p.y + size.y}, color::mix(rest, color::selected, sel), size.y * 0.5f);
  ImU32 fg = color::mix(color::text, color::on_selected, sel);
  draw_text(dl, font, kSmall + 1, {std::round(p.x + s(14)), text_y(font, kSmall + 1, p.y, size.y)}, fg, shown);
  ImGui::PopID();
  return in.clicked;
}

float badge_width(std::string_view value, char32_t glyph) {
  const char* g = icon(glyph);
  float w = measure(g_fonts.semibold, kLabel, value).x + s(18);
  if (*g) w += measure(g_fonts.regular, kLabel, g).x + s(5);
  return w;
}

void badge(std::string_view value, ImU32 fg, ImU32 bg, char32_t glyph) {
  const char* g = icon(glyph);
  ImVec2 size{badge_width(value, glyph), s(22)};
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, {p.x + size.x, p.y + size.y}, bg, s(6));
  float x = p.x + s(9);
  if (*g) {
    ImVec2 gs = measure(g_fonts.regular, kLabel, g);
    draw_glyph(dl, glyph, kLabel, {x + gs.x * 0.5f, p.y + size.y * 0.5f}, fg);
    x += gs.x + s(5);
  }
  draw_text(dl, g_fonts.semibold, kLabel, {std::round(x), text_y(g_fonts.semibold, kLabel, p.y, size.y)}, fg, value);
  ImGui::Dummy(size);
}

void progress_bar(float fraction, ImVec2 size, bool indeterminate, ImU32 fill_col) {
  if (!fill_col) fill_col = color::accent_text;
  ImVec2 p = ImGui::GetCursorScreenPos();
  if (size.x <= 0) size.x = ImGui::GetContentRegionAvail().x;
  ImVec2 q{p.x + size.x, p.y + size.y};
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float r = size.y * 0.5f;
  dl->AddRectFilled(p, q, color::surface_top, r);
  if (indeterminate) {
    float t = std::fmod(float(ImGui::GetTime()) * 0.7f, 1.f);
    float w = size.x * 0.28f;
    float x0 = p.x - w + (size.x + w) * t;
    dl->PushClipRect(p, q, true);
    dl->AddRectFilled({x0, p.y}, {x0 + w, q.y}, fill_col, r);
    dl->PopClipRect();
  } else {
    fraction = std::clamp(fraction, 0.f, 1.f);
    float fill = std::max(size.y, size.x * fraction);
    if (fraction > 0) dl->AddRectFilled(p, {p.x + fill, q.y}, fill_col, r);
  }
  ImGui::Dummy(size);
}

bool search_field(const char* id, const char* hint, std::string* value, float width) {
  ImGui::PushID(id);
  float height = s(38);
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  bool focused = ImGui::GetID("##input") == ImGui::GetActiveID();
  bool hovered = ImGui::IsMouseHoveringRect(p, {p.x + width, p.y + height});
  float t = anim(ImGui::GetID("focus"), focused ? 1.f : 0.f, 16);
  float h = anim(ImGui::GetID("hover"), hovered ? 1.f : 0.f, 16);
  dl->AddRectFilled(p, {p.x + width, p.y + height}, field_bg(std::max(t, h * 0.6f)), height * 0.5f);
  if (t > 0.01f)
    dl->AddRect(p, {p.x + width, p.y + height}, color::alpha(color::selected, 0.55f * t), height * 0.5f, 0, s(1.5f));
  float left = s(16);
  if (const char* g = icon(icons::search); *g) {
    draw_glyph(dl, icons::search, kSmall + 1, {p.x + s(24), p.y + height * 0.5f}, color::text2);
    left = s(42);
  }
  bool clear = !value->empty();
  float right = clear ? s(36) : s(16);
  ImGui::SetCursorScreenPos({p.x + left, p.y});
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, color::text3);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, text_y(ImGui::GetFont(), kBody, 0, height)});
  ImGui::SetNextItemWidth(width - left - right);
  bool changed = ImGui::InputTextWithHint("##input", hint, value);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(4);
  if (clear) {
    ImGui::SetCursorScreenPos({p.x + width - s(34), p.y + (height - s(26)) * 0.5f});
    if (icon_button("clear", icons::cancel, nullptr, 26)) {
      value->clear();
      changed = true;
    }
  }
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy({width, height});
  ImGui::PopID();
  return changed;
}

bool submit_field(const char* id, const char* hint, std::string* value, float width, char32_t glyph,
                  bool enabled) {
  ImGui::PushID(id);
  const float height = s(34);
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  bool focused = ImGui::GetID("##input") == ImGui::GetActiveID();
  bool hovered = ImGui::IsMouseHoveringRect(p, {p.x + width, p.y + height});
  float t = anim(ImGui::GetID("focus"), focused ? 1.f : 0.f, 16);
  float h = anim(ImGui::GetID("hover"), hovered ? 1.f : 0.f, 16);
  dl->AddRectFilled(p, {p.x + width, p.y + height}, field_bg(std::max(t, h * 0.6f)), height * 0.5f);
  if (t > 0.01f)
    dl->AddRect(p, {p.x + width, p.y + height}, color::alpha(color::selected, 0.55f * t), height * 0.5f, 0, s(1.5f));
  draw_glyph(dl, glyph, kSmall, {p.x + s(20), p.y + height * 0.5f}, color::text2);
  const float left = s(36), right = s(36);
  ImGui::SetCursorScreenPos({p.x + left, p.y});
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, color::text3);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, text_y(ImGui::GetFont(), kSmall, 0, height)});
  ImGui::PushFont(g_fonts.regular, kSmall);
  ImGui::SetNextItemWidth(width - left - right);
  bool submit = ImGui::InputTextWithHint("##input", hint, value, ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::PopFont();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(4);
  if (!value->empty()) {
    ImGui::SetCursorScreenPos({p.x + width - s(31), p.y + (height - s(26)) * 0.5f});
    submit |= icon_button("submit", icons::check, nullptr, 26, enabled);
  }
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy({width, height});
  ImGui::PopID();
  return submit && enabled && !value->empty();
}

bool begin_menu(const char* id) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {s(6), s(6)});
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, s(2)});
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, s(12));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, color::surface_hi);
  bool open = ImGui::BeginPopup(id);
  if (!open) {
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
  }
  return open;
}

void end_menu() {
  ImGui::EndPopup();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
}

bool menu_item(const char* label, char32_t glyph, bool danger, bool enabled, ImTextureID image) {
  std::string_view shown = visible_label(label);
  ImGui::PushID(label);
  float w = std::max(s(220), measure(g_fonts.regular, kBody, shown).x + s(64));
  w = std::max(w, ImGui::GetContentRegionAvail().x);
  ImVec2 p = ImGui::GetCursorScreenPos();
  auto in = interact("##item", {w, s(38)}, enabled);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (in.hover > 0.01f) dl->AddRectFilled(p, {p.x + w, p.y + s(38)}, color::alpha(color::surface_top, in.hover), s(8));
  ImU32 fg = !enabled ? color::text3 : danger ? color::danger : color::text;
  float x = p.x + s(12);
  if (image) {
    float h = std::round(s(8));
    ImVec2 c{std::round(x + s(8)), std::round(p.y + s(19))};
    draw_image(dl, image, {c.x - h, c.y - h}, {c.x + h, c.y + h}, {0, 0}, {1, 1}, enabled ? color::text2 : color::text3);
    x += s(28);
  } else if (*icon(glyph)) {
    draw_glyph(dl, glyph, kBody, {x + s(8), p.y + s(19)}, danger && enabled ? color::danger : color::text2);
    x += s(28);
  }
  draw_text(dl, g_fonts.regular, kBody, {std::round(x), text_y(g_fonts.regular, kBody, p.y, s(38))}, fg, shown);
  ImGui::PopID();
  if (in.clicked) ImGui::CloseCurrentPopup();
  return in.clicked;
}

bool dropdown(const char* id, const char* prefix, const char* const* items, int count, int* current, float width) {
  ImGui::PushID(id);
  std::string shown = std::string(prefix) + items[*current];
  ImFont* font = g_fonts.semibold;
  ImVec2 ts = measure(font, kSmall + 1, shown);
  const char* chev = icon(icons::chevron_down);
  float chev_w = *chev ? measure(g_fonts.regular, kLabel, chev).x + s(8) : 0;
  ImVec2 size{width > 0 ? width : ts.x + chev_w + s(30), s(38)};
  ImVec2 p = ImGui::GetCursorScreenPos();
  auto in = interact("##dropdown", size);
  bool open = ImGui::IsPopupOpen("##menu");
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, {p.x + size.x, p.y + size.y}, field_bg(std::max(in.hover, open ? 1.f : 0.f)), size.y * 0.5f);
  // Centered in the field; a long label stops short of the chevron.
  const float text_x = std::max(p.x + s(15), std::min(p.x + (size.x - ts.x) * 0.5f, p.x + size.x - chev_w - s(15) - ts.x));
  draw_text(dl, font, kSmall + 1, {std::round(text_x), text_y(font, kSmall + 1, p.y, size.y)}, color::text, shown);
  if (*chev) draw_glyph(dl, icons::chevron_down, kLabel, {p.x + size.x - s(20), p.y + size.y * 0.5f}, color::text2);
  if (in.clicked) ImGui::OpenPopup("##menu");
  bool changed = false;
  ImGui::SetNextWindowPos({p.x, p.y + size.y + s(6)});
  if (begin_menu("##menu")) {
    for (int i = 0; i < count; ++i) {
      ImGui::PushID(i);
      ImVec2 row = ImGui::GetCursorScreenPos();
      if (menu_item(items[i])) {
        changed = *current != i;
        *current = i;
      }
      if (i == *current)
        draw_glyph(ImGui::GetWindowDrawList(), icons::check, kSmall,
                   {row.x + ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x - s(18),
                    row.y + s(19)},
                   color::accent_text);
      ImGui::PopID();
    }
    end_menu();
  }
  ImGui::PopID();
  return changed;
}

bool tabs(const char* id, const char* const* items, int count, int* current, float width) {
  ImGui::PushID(id);
  ImFont* font = g_fonts.semibold;
  float height = s(44);
  ImVec2 p = ImGui::GetCursorScreenPos();
  float full = width > 0 ? width : ImGui::GetContentRegionAvail().x;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  hline(dl, p.x, p.x + full, p.y + height - s(1));
  bool changed = false;
  float x = p.x, sel_x = 0, sel_w = 0;
  for (int i = 0; i < count; ++i) {
    ImVec2 ts = measure(font, kBody, items[i]);
    float w = ts.x + s(4);
    ImGui::SetCursorScreenPos({x, p.y});
    ImGui::PushID(i);
    auto in = interact("##tab", {w, height});
    ImGui::PopID();
    if (in.clicked && *current != i) {
      *current = i;
      changed = true;
    }
    bool on = *current == i;
    ImU32 fg = on ? color::text : color::mix(color::text2, color::text, in.hover);
    draw_text(dl, font, kBody, {std::round(x + s(2)), text_y(font, kBody, p.y, height - s(3))}, fg, items[i]);
    if (on) {
      sel_x = x - p.x;
      sel_w = w;
    }
    x += w + s(28);
  }
  float ax = anim(ImGui::GetID("x"), sel_x, 22), aw = anim(ImGui::GetID("w"), sel_w, 22);
  dl->AddRectFilled({p.x + ax, p.y + height - s(3)}, {p.x + ax + aw, p.y + height}, color::selected, s(1.5f));
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy({full, height});
  ImGui::PopID();
  return changed;
}

void spinner(float radius, ImU32 col) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  float r = s(radius);
  ImVec2 c{p.x + r, p.y + r};
  float t = float(ImGui::GetTime()) * 5.f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PathClear();
  dl->PathArcTo(c, r - s(1.5f), t, t + IM_PI * 1.4f, 32);
  dl->PathStroke(col, 0, s(2.25f));
  ImGui::Dummy({r * 2, r * 2});
}

bool toggle(const char* id, bool* value) {
  ImGui::PushID(id);
  ImVec2 size{s(44), s(24)};
  ImVec2 p = ImGui::GetCursorScreenPos();
  auto in = interact("##toggle", size);
  if (in.clicked) *value = !*value;
  float t = anim(ImGui::GetID("on"), *value ? 1.f : 0.f, 18);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 q{p.x + size.x, p.y + size.y};
  float r = size.y * 0.5f;
  ImU32 off = color::mix(color::surface_top, IM_COL32(74, 77, 83, 255), in.hover);
  dl->AddRectFilled(p, q, color::mix(off, color::mix(color::accent, color::accent_hi, in.hover), t), r);
  float knob = r - s(3);
  float x = p.x + r + (size.x - 2 * r) * t;
  dl->AddCircleFilled({x, p.y + r}, knob, IM_COL32_WHITE, 24);
  ImGui::PopID();
  return in.clicked;
}

bool checkbox(const char* id, std::string_view label, bool* value, std::string_view hint, bool small,
              ImTextureID image) {
  ImGui::PushID(id);
  const float box = s(small ? 16 : 22), pad = s(small ? 9 : 12), mark = image ? std::round(s(20)) : 0;
  const float size_text = small ? kSmall : kBody, lead = mark ? mark + s(9) : 0;
  ImVec2 lw = measure(g_fonts.regular, size_text, label),
         hw = hint.empty() ? ImVec2{} : measure(g_fonts.regular, kSmall, hint);
  ImVec2 size{box + pad + std::max(lead + lw.x, hw.x), hint.empty() ? box : box + s(20)};
  ImVec2 p = ImGui::GetCursorScreenPos();
  auto in = interact("##checkbox", size);
  if (in.clicked) *value = !*value;
  float t = anim(ImGui::GetID("on"), *value ? 1.f : 0.f, 18);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 q{p.x + box, p.y + box};
  ImU32 off = color::mix(color::surface_top, IM_COL32(74, 77, 83, 255), in.hover);
  const float round = s(small ? 4 : 6);
  dl->AddRectFilled(p, q, color::mix(off, color::mix(color::accent, color::accent_hi, in.hover), t), round);
  if (t < 1) dl->AddRect(p, q, color::alpha(IM_COL32(150, 154, 162, 255), 1 - t), round, 0, s(1.5f));
  if (t > 0.01f)
    draw_glyph(dl, icons::check, small ? kLabel - 2 : kSmall, {p.x + box * 0.5f, p.y + box * 0.5f},
               color::alpha(IM_COL32_WHITE, t));
  float x = p.x + box + pad;
  if (mark) {
    float y = std::round(p.y + (box - mark) * 0.5f);
    draw_image(dl, image, {std::round(x), y}, {std::round(x) + mark, y + mark});
  }
  draw_text(dl, g_fonts.regular, size_text, {std::round(x + lead), text_y(g_fonts.regular, size_text, p.y, box)},
            small ? color::text2 : color::text, label);
  if (!hint.empty())
    draw_text(dl, g_fonts.regular, kSmall, {x, text_y(g_fonts.regular, kSmall, p.y + box + s(10))}, color::text3, hint);
  ImGui::PopID();
  return in.clicked;
}

void tooltip(std::string_view value) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {s(10), s(7)});
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, s(8));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, color::surface_top);
  if (ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(s(360));
    text(g_fonts.regular, kSmall, color::text, value);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

}  // namespace venture::launcher::ui
