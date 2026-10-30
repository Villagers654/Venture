// Venture launcher design system: color tokens, type scale, icons, motion
// and the small set of custom-drawn controls the views are built from. The
// look follows Meta Horizon's dark theme: neutral greys (never pure black or
// white), one blue for primary actions, pill-shaped controls and light
// selected states.
#pragma once
#include <imgui.h>
#include <string>
#include <string_view>

namespace venture::launcher::ui {

// --- Color tokens (sRGB) ----------------------------------------------------
namespace color {
inline constexpr ImU32 bg = IM_COL32(26, 27, 30, 255);           // app background
inline constexpr ImU32 surface = IM_COL32(37, 38, 42, 255);      // cards, fields
inline constexpr ImU32 surface_hi = IM_COL32(48, 50, 54, 255);   // hover / raised
inline constexpr ImU32 surface_top = IM_COL32(60, 62, 67, 255);  // pressed, menus
inline constexpr ImU32 stroke = IM_COL32(255, 255, 255, 22);
inline constexpr ImU32 stroke_hi = IM_COL32(255, 255, 255, 40);
inline constexpr ImU32 text = IM_COL32(228, 230, 235, 255);
inline constexpr ImU32 text2 = IM_COL32(168, 171, 178, 255);     // secondary
inline constexpr ImU32 text3 = IM_COL32(122, 126, 134, 255);     // hints, captions
inline constexpr ImU32 selected = IM_COL32(228, 230, 235, 255);  // selected nav item / chip
inline constexpr ImU32 on_selected = IM_COL32(24, 25, 28, 255);
inline constexpr ImU32 accent = IM_COL32(0, 100, 224, 255);      // primary actions
inline constexpr ImU32 accent_hi = IM_COL32(26, 119, 242, 255);
inline constexpr ImU32 accent_lo = IM_COL32(0, 84, 190, 255);
inline constexpr ImU32 accent_text = IM_COL32(77, 155, 255, 255);  // links, highlights on dark
inline constexpr ImU32 accent_wash = IM_COL32(77, 155, 255, 38);
inline constexpr ImU32 on_accent = IM_COL32(255, 255, 255, 255);
inline constexpr ImU32 positive = IM_COL32(69, 189, 98, 255);
inline constexpr ImU32 warning = IM_COL32(247, 185, 40, 255);
inline constexpr ImU32 danger = IM_COL32(243, 66, 95, 255);
inline constexpr ImU32 danger_wash = IM_COL32(243, 66, 95, 38);
ImVec4 v(ImU32 c);
ImU32 alpha(ImU32 c, float a);  // multiply alpha
ImU32 mix(ImU32 a, ImU32 b, float t);
}  // namespace color

// --- Type -------------------------------------------------------------------
struct Fonts {
  ImFont* regular = nullptr;
  ImFont* semilight = nullptr;
  ImFont* semibold = nullptr;
  ImFont* bold = nullptr;
  ImFont* italic = nullptr;
  bool icons = false;
};
Fonts& fonts();
void load_fonts();
// Type scale (unscaled px).
inline constexpr float kDisplay = 28, kTitle = 22, kHeading = 17, kBody = 15, kSmall = 14, kLabel = 13;

// --- Icons (Segoe Fluent Icons / MDL2 code points) ---------------------------
// Returns "" when no icon font is available, so callers can always prefix.
const char* icon(char32_t code_point);
namespace icons {
inline constexpr char32_t library = 0xE8F1, download = 0xE896, settings = 0xE713, account = 0xE77B,
                          play = 0xF5B0, search = 0xE721, refresh = 0xE72C, back = 0xE72B, folder = 0xE8B7,
                          trash = 0xE74D, more = 0xE712, cancel = 0xE711, pause = 0xE769, resume = 0xE768,
                          info = 0xE946, check = 0xE73E, update = 0xE895, warning = 0xE7BA, signout = 0xF3B1,
                          globe = 0xE774, chevron_right = 0xE76C, chevron_left = 0xE76B, sort = 0xE8CB,
                          lock = 0xE72E, chevron_down = 0xE70D, chevron_up = 0xE70E, add = 0xE710, star = 0xE734,
                          star_fill = 0xE735, swap = 0xE8AB, package = 0xE7B8, cloud = 0xE753, puzzle = 0xEA86,
                          error = 0xE783, game = 0xE7FC, people = 0xE716, save = 0xE74E;
}

// --- Scale & motion ---------------------------------------------------------
void set_scale(float scale);
float scale();
inline float s(float v) { return v * scale(); }
// Smoothly approaches `target` (critically damped-ish), keyed by ImGui id.
float anim(ImGuiID id, float target, float speed = 16.f);
float anim(const char* key, float target, float speed = 16.f);

void apply_style();

// --- Drawing primitives -----------------------------------------------------
// Call once per frame before drawing; `crisp_glyphs` samples the glyph atlas
// nearest (off for renderers whose nearest sampling drops texel columns).
void begin_frame(bool crisp_glyphs);
// An image sampled linearly (artwork, logos).
void draw_image(ImDrawList* dl, ImTextureID texture, ImVec2 a, ImVec2 b, ImVec2 uv0 = {0, 0}, ImVec2 uv1 = {1, 1},
                ImU32 col = IM_COL32_WHITE, float rounding = 0, ImDrawFlags corners = 0);
// Text drawn directly into a draw list; `size` is unscaled.
void draw_text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, std::string_view text,
               float wrap_width = 0);
ImVec2 measure(ImFont* font, float size, std::string_view text, float wrap_width = 0);
std::string ellipsize(ImFont* font, float size, std::string_view text, float max_width);
void soft_shadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float spread, float strength);
void vertical_fade(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom);
// Top of a line of text whose cap height is centered in [top, top+height];
// with height 0 the caps center on `top`.
float text_y(ImFont* font, float size, float top, float height = 0);
// The cap-height center of text whose line box starts at `top`.
float cap_center(ImFont* font, float size, float top);
// A glyph whose ink is centered on `center`.
void draw_glyph(ImDrawList* dl, char32_t glyph, float size, ImVec2 center, ImU32 col);
// A one-pixel horizontal rule (filled, so every renderer draws it evenly).
void hline(ImDrawList* dl, float x0, float x1, float y, ImU32 col = color::stroke);
// Controls drawn on a card use one step lighter fills than on the page.
void set_on_card(bool on_card);

// --- Controls ---------------------------------------------------------------
// Text laid out with the cursor (wraps to the available width).
void text(ImFont* font, float size, ImU32 col, std::string_view text, bool wrap = true);
void heading(std::string_view text);  // section heading
void gap(float px);
enum class Tone { primary, secondary, ghost, danger, light };
// Pill button; a zero size fits the label at the standard 40 px height. A white `image` (16 px) replaces the glyph.
bool button(const char* id_label, char32_t glyph, Tone tone, ImVec2 size = {0, 0}, bool enabled = true,
            ImTextureID image = 0);
float button_width(std::string_view label, char32_t glyph = 0);
// Round icon button; `filled` draws its resting background.
bool icon_button(const char* id, char32_t glyph, const char* tooltip, float size = 36, bool enabled = true,
                 bool filled = false);
// Filter chip: light fill when selected. Returns true when clicked.
bool filter_chip(const char* id_label, bool selected);
// Small static pill for states such as "Update available".
void badge(std::string_view text, ImU32 fg, ImU32 bg, char32_t glyph = 0);
float badge_width(std::string_view text, char32_t glyph = 0);
// A zero `fill` uses the accent; paused progress passes a muted color.
void progress_bar(float fraction, ImVec2 size, bool indeterminate = false, ImU32 fill = 0);
bool search_field(const char* id, const char* hint, std::string* value, float width);
// A chip-high pill field that submits: true on Enter or its check button.
bool submit_field(const char* id, const char* hint, std::string* value, float width, char32_t glyph,
                  bool enabled = true);
// Pill showing "<prefix><current item>" that opens a menu of `items`.
bool dropdown(const char* id, const char* prefix, const char* const* items, int count, int* current,
              float width = 0);
// Underlined tabs; returns true when the selection changed.
bool tabs(const char* id, const char* const* items, int count, int* current, float width = 0);
// Menu popups and their rows.
bool begin_menu(const char* id);
void end_menu();
// `image` (a white logo, tinted like the glyphs) replaces the glyph when set.
bool menu_item(const char* label, char32_t glyph = 0, bool danger = false, bool enabled = true,
               ImTextureID image = {});
void spinner(float radius, ImU32 col);
bool toggle(const char* id, bool* value);
// A checkbox with its label (and an optional line under it); the whole row clicks.
// `small` is the compact form for secondary choices; `image` (a white logo)
// sits before the label.
bool checkbox(const char* id, std::string_view label, bool* value, std::string_view hint = {}, bool small = false,
              ImTextureID image = {});
void tooltip(std::string_view text);

}  // namespace venture::launcher::ui
