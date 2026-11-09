#include "launcher/app.h"
#include "launcher/assets/controller_art.h"
#include "launcher/store_link.h"
#include "launcher/i18n/i18n.h"
#include "launcher/ui/rich_text.h"
#include "launcher/ui/style.h"
#include "launcher/window_style.h"
#include "common/controls.h"
#include "common/environment.h"
#include "common/http/client.h"
#include "common/json.h"
#include "common/log.h"
#include "common/saves_migration.h"
#include "common/search.h"

#include <algorithm>
#include <functional>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <charconv>
#include <format>
#include <memory>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <type_traits>
#include <unordered_map>

namespace venture::launcher {
using namespace ui;
using namespace i18n::markers;
namespace {

std::string bytes(uint64_t n) {
  if (n >= 1024ull * 1024 * 1024) return std::format("{:.1f} GB", double(n) / (1024.0 * 1024 * 1024));
  if (n >= 1024 * 1024) return std::format("{:.0f} MB", double(n) / (1024.0 * 1024));
  return std::format("{} KB", n / 1024);
}

// Remaining time the way the Quest store words it: "12 min left".
std::string time_left(uint64_t seconds) {
  if (seconds < 60) return "Less than a minute left";
  uint64_t minutes = (seconds + 30) / 60;
  if (minutes < 60) return std::format("{} min left", minutes);
  return minutes % 60 ? std::format("{} hr {} min left", minutes / 60, minutes % 60)
                      : std::format("{} hr left", minutes / 60);
}

std::string lowercase(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

std::string initials(std::string_view name) {
  std::string out;
  bool start = true;
  for (char ch : name) {
    if (ch == ' ' || ch == '_' || ch == '.') {
      start = true;
    } else if (start && std::isalnum(static_cast<unsigned char>(ch))) {
      out += char(std::toupper(static_cast<unsigned char>(ch)));
      start = false;
      if (out.size() == 2) break;
    }
  }
  return out;
}

// The name Meta shows for the account: its display name, else its alias.
std::string account_name(const AccountView& a) {
  if (!a.display_name.empty()) return a.display_name;
  return a.alias.empty() ? "Meta account" : a.alias;
}

// Each account keeps one muted hue so several accounts stay distinguishable.
ImU32 account_tint(std::string_view key) {
  static constexpr ImU32 palette[] = {IM_COL32(56, 98, 166, 255),  IM_COL32(112, 84, 160, 255),
                                      IM_COL32(40, 128, 120, 255), IM_COL32(168, 92, 70, 255),
                                      IM_COL32(150, 70, 110, 255), IM_COL32(88, 124, 60, 255)};
  return palette[std::hash<std::string_view>{}(key) % std::size(palette)];
}

// Round initials avatar; `ring` marks the active account.
void avatar(ImDrawList* dl, ImVec2 c, float r, const AccountView* a, bool ring, float font_size) {
  std::string ini = a ? initials(account_name(*a)) : std::string();
  if (a && account_name(*a) == "Meta account") ini.clear();
  dl->AddCircleFilled(c, r, a ? account_tint(a->key) : color::surface_top, 48);
  if (ring) dl->AddCircle(c, r + s(3), color::selected, 48, s(2));
  if (!ini.empty()) {
    ImVec2 is = measure(fonts().semibold, font_size, ini);
    draw_text(dl, fonts().semibold, font_size, {std::round(c.x - is.x * 0.5f), text_y(fonts().semibold, font_size, c.y)},
              IM_COL32_WHITE, ini);
  } else {
    draw_glyph(dl, icons::account, font_size, c, color::text);
  }
}

// Every page opens with a header row of this height; the title and any
// controls beside it share its center line.
constexpr float kHeaderHeight = 40, kAfterHeader = 22, kChipHeight = 34;  // filter_chip's height
// How far controls beside a page title stay from the content's right edge:
// clear of the window buttons the launcher draws itself (46 px each, three).
float header_right_inset() { return client_side_decorations() ? 3 * 46 - 36 + 12 : 0; }
// List pages (downloads, settings, accounts) share one column width.
constexpr float kPageWidth = 860;
// Buttons and dropdowns in rows share one size: as wide as a settings
// dropdown (wider only for a longer label), and one control height.
constexpr float kControlW = 180, kControlH = 38;
float row_button_w(std::string_view label, char32_t glyph = 0) {
  return std::max(s(kControlW), button_width(label, glyph));
}
void page_title(std::string_view title) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  draw_text(ImGui::GetWindowDrawList(), fonts().semibold, kDisplay,
            {std::round(p.x), text_y(fonts().semibold, kDisplay, p.y, s(kHeaderHeight))}, color::text, title);
  ImGui::Dummy({measure(fonts().semibold, kDisplay, title).x, s(kHeaderHeight)});
}

// A rounded surface with its own padding that grows with its content.
void begin_card(const char* id, float width, ImVec2 padding = {24, 22}, ImU32 bg = color::surface) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {s(padding.x), s(padding.y)});
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, s(12));
  ImGui::BeginChild(id, {width, 0}, ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  set_on_card(true);
}
void end_card() {
  set_on_card(false);
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
}

// A rule between rows; it takes no layout space of its own.
void divider() {
  ImVec2 p = ImGui::GetCursorScreenPos();
  hline(ImGui::GetWindowDrawList(), p.x, p.x + ImGui::GetContentRegionAvail().x, p.y);
}

// Meta gives new accounts a generated "user_<digits>" alias; it is noise next
// to a real name.
bool generated_alias(std::string_view alias) {
  if (!alias.starts_with("user_") || alias.size() == 5) return false;
  return std::ranges::all_of(alias.substr(5), [](char ch) { return ch >= '0' && ch <= '9'; });
}

// A text link at the cursor, flush with the text beside it.
bool link(const char* id_label, ImU32 col = color::text) {
  std::string_view label = id_label;
  label = label.substr(0, label.find("##"));
  ImFont* f = fonts().semibold;
  ImVec2 size = measure(f, kSmall, label);
  ImVec2 p = ImGui::GetCursorScreenPos();
  bool clicked = ImGui::InvisibleButton(id_label, size);
  bool hov = ImGui::IsItemHovered();
  if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  draw_text(dl, f, kSmall, p, hov ? color::text : col, label);
  if (hov) hline(dl, p.x, p.x + size.x, p.y + size.y - s(1), color::text);
  return clicked;
}

// Centered empty state: glyph, headline, and a line of guidance when there is one.
void empty_state(char32_t glyph, std::string_view headline, std::string_view sub) {
  float w = ImGui::GetContentRegionAvail().x;
  ImVec2 p = ImGui::GetCursorScreenPos();
  float r = s(36);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddCircleFilled({p.x + w * 0.5f, p.y + r}, r, color::surface, 48);
  draw_glyph(dl, glyph, 26, {p.x + w * 0.5f, p.y + r}, color::text2);
  ImGui::Dummy({w, r * 2});
  gap(18);
  if (!headline.empty()) {
    ImVec2 hs = measure(fonts().semibold, kTitle, headline);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - hs.x) * 0.5f);
    text(fonts().semibold, kTitle, color::text, headline, false);
    gap(2);
  }
  if (sub.empty()) return;
  ImVec2 ss = measure(fonts().regular, kBody, sub);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (w - ss.x) * 0.5f));
  text(fonts().regular, kBody, color::text2, sub, false);
}

float g_row_end = -1;  // window y just below the last setting_row()
constexpr float kHelpIcon = 24;  // a setting's help icon button

// Settings-style row inside a card: a one-line title left, centred on the
// control right-aligned. `after_title` (a small icon button) sits right after
// the title. The description documents the row at its call site; rows are one
// line, so it is not drawn.
template <class Control, class AfterTitle = std::nullptr_t>
void setting_row(std::string_view title_text, std::string_view /*description*/, float control_w, float control_h,
                 Control control, AfterTitle after_title = nullptr) {
  float avail = ImGui::GetContentRegionAvail().x;
  float left = std::max(s(120), avail - control_w - s(24));
  float pad = s(12), spacing = ImGui::GetStyle().ItemSpacing.y;
  ImVec2 start = ImGui::GetCursorPos(), screen = ImGui::GetCursorScreenPos();
  const auto title = ellipsize(fonts().regular, kBody, title_text, left);
  ImVec2 ts = measure(fonts().regular, kBody, title);
  float row_h = std::max({ts.y, control_h, s(38)});  // every row the same height
  draw_text(ImGui::GetWindowDrawList(), fonts().regular, kBody,
            {std::round(screen.x), text_y(fonts().regular, kBody, screen.y + pad, row_h)}, color::text, title);
  if constexpr (!std::is_same_v<AfterTitle, std::nullptr_t>) {
    ImGui::SetCursorPos({start.x + ts.x + s(6), start.y + pad + (row_h - s(kHelpIcon)) * 0.5f});
    after_title();
  }
  ImGui::SetCursorPos({start.x + avail - control_w, start.y + pad + (row_h - control_h) * 0.5f});
  ImGui::BeginGroup();
  control();
  ImGui::EndGroup();
  ImGui::SetCursorPos({start.x, start.y + pad + row_h + pad - spacing});
  ImGui::Dummy({0, 0});
  g_row_end = ImGui::GetCursorPosY();
}

// Store descriptions, parsed once per text.
void description(std::string_view raw, float width) {
  static std::unordered_map<std::string, std::vector<Block>> parsed;
  auto it = parsed.find(std::string(raw));
  if (it == parsed.end()) it = parsed.emplace(std::string(raw), parse_description(raw)).first;
  draw_description(it->second, width);
}

// The community server, for help and news.
constexpr const char* kDiscord = "https://discord.gg/Ey9hHszvwa";

// A modal dialog in the house style; returns true while open. Every dialog is
// a title, a short message, optional settings-style rows, and dialog_footer().
float g_dialog_w = 0;  // content width of the open dialog
bool begin_dialog(const char* id, float width) {
  ImGui::SetNextWindowSize({s(width), 0}, ImGuiCond_Appearing);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {s(28), s(24)});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, s(16));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, color::surface);
  bool open = ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
  if (!open) {
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
  }
  g_dialog_w = s(width) - s(56);
  return open;
}
void dialog_text(std::string_view title, std::string_view message) {
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + g_dialog_w);
  text(fonts().semibold, kTitle, color::text, title);
  if (!message.empty()) {
    gap(8);
    text(fonts().regular, kBody, color::text2, message);
  }
  ImGui::PopTextWrapPos();
}
struct DialogButton {
  std::string_view label;
  Tone tone = Tone::secondary;
  bool enabled = true;
  char32_t glyph = 0;
};
// A rule across the dialog, then `extra` (a secondary action) at the left and
// the buttons at the right, the main one last. Returns the button clicked, or -1.
int dialog_footer(std::initializer_list<DialogButton> buttons, const std::function<void(float)>& extra = {}) {
  const float h = s(38);
  if (ImGui::GetCursorPosY() != g_row_end) gap(22);  // a setting row brings its own padding
  const float y = ImGui::GetCursorScreenPos().y, x = ImGui::GetWindowPos().x;
  hline(ImGui::GetWindowDrawList(), x, x + ImGui::GetWindowWidth(), y);
  ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, y + s(20)});
  const ImVec2 row = ImGui::GetCursorPos();
  if (extra) extra(h);
  float total = -s(10);
  for (const auto& b : buttons) total += std::max(s(96), button_width(b.label, b.glyph)) + s(10);
  ImGui::SetCursorPos({row.x + g_dialog_w - total, row.y});
  int clicked = -1, i = 0;
  for (const auto& b : buttons) {
    if (i) ImGui::SameLine(0, s(10));
    const float bw = std::max(s(96), button_width(b.label, b.glyph));
    if (button(std::format("{}##dialog{}", b.label, i).c_str(), b.glyph, b.tone, {bw, h}, b.enabled)) clicked = i;
    ++i;
  }
  ImGui::SetCursorPos({row.x, row.y + h});
  ImGui::Dummy({g_dialog_w, 0});
  return clicked;
}
void end_dialog() {
  ImGui::EndPopup();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

}  // namespace

// ---------------------------------------------------------------------------
void App::theme() {
  set_scale(scale_);
  apply_style();
}

void App::art(std::string_view id, ImVec2 size, float rounding, float zoom, ImDrawFlags corners) {
  id = meta::base_app(id);  // a pinned version shows its game's artwork
  ImVec2 p = ImGui::GetCursorScreenPos();
  p = {std::round(p.x), std::round(p.y)};
  ImVec2 q{std::round(p.x + size.x), std::round(p.y + size.y)};
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float want = (q.x - p.x) * (1 + zoom);
  if (SDL_Texture* texture = textures_->get(id, want)) {
    float w = 0, h = 0;
    SDL_GetTextureSize(texture, &w, &h);
    float aspect = size.x / size.y, source = w / h;
    // An app icon (an import without a banner) sits whole on a blur of itself rather than cropped to the tile.
    if (source < 1.25f && aspect > 1.5f) {
      dl->AddRectFilled(p, q, color::surface_hi, rounding, corners);
      if (SDL_Texture* ambient = textures_->ambient(id))
        draw_image(dl, ImTextureID(reinterpret_cast<uintptr_t>(ambient)), p, q, {0.25f, 0.3f}, {0.75f, 0.7f},
                   color::alpha(IM_COL32_WHITE, 0.55f), rounding, corners);
      float side = std::round((q.y - p.y) * 0.72f);
      ImVec2 a{std::round((p.x + q.x - side) * 0.5f), std::round((p.y + q.y - side) * 0.5f)};
      draw_image(dl, ImTextureID(reinterpret_cast<uintptr_t>(textures_->get(id, side))), a, {a.x + side, a.y + side},
                 {0, 0}, {1, 1}, IM_COL32_WHITE, side * 0.18f);
      ImGui::Dummy(size);
      return;
    }
    // A wide banner cropped to the tile shows only part of its width; pick the level by the visible part.
    if (source > aspect) {
      texture = textures_->get(id, want * source / aspect);
      SDL_GetTextureSize(texture, &w, &h);
    }
    ImVec2 uv0{0, 0}, uv1{1, 1};
    if (source > aspect) {
      float crop = aspect / source;
      uv0.x = (1 - crop) / 2;
      uv1.x = 1 - uv0.x;
    } else {
      float crop = source / aspect;
      uv0.y = (1 - crop) / 2;
      uv1.y = 1 - uv0.y;
    }
    if (zoom > 0) {  // crop towards the center
      float cx = (uv0.x + uv1.x) / 2, cy = (uv0.y + uv1.y) / 2;
      float hw = (uv1.x - uv0.x) / 2 / (1 + zoom), hh = (uv1.y - uv0.y) / 2 / (1 + zoom);
      uv0 = {cx - hw, cy - hh};
      uv1 = {cx + hw, cy + hh};
    }
    draw_image(dl, ImTextureID(reinterpret_cast<uintptr_t>(texture)), p, q, uv0, uv1, IM_COL32_WHITE, rounding, corners);
  } else {
    // Placeholder until the cover arrives: a quiet tile with a game glyph.
    dl->AddRectFilled(p, q, color::surface_hi, rounding, corners);
    draw_glyph(dl, icons::game, std::clamp(size.y / scale_ * 0.22f, 14.f, 40.f), {(p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f},
               color::text3);
  }
  ImGui::Dummy(size);
}

void App::draw_backdrop(std::string_view id, ImVec2 a, ImVec2 b, float strength) {
  id = meta::base_app(id);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (SDL_Texture* ambient = textures_->ambient(id)) {
    ImU32 tint = color::alpha(IM_COL32_WHITE, strength);
    draw_image(dl, ImTextureID(reinterpret_cast<uintptr_t>(ambient)), a, b, {0.05f, 0.05f}, {0.95f, 0.95f}, tint);
  }
}

// ---------------------------------------------------------------------------
void App::draw() {
  // SDL's software renderer samples nearest half a texel off.
  begin_frame(std::string_view(SDL_GetRendererName(renderer_)) != SDL_SOFTWARE_RENDERER);
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
  ImGui::Begin("Venture shell", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();

  draw_sidebar();
  ImGui::SameLine(0, 0);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {s(36), s(24)});
  ImGui::BeginChild("Content", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding,
                    (page_ == 0 && selected_.empty()) ? ImGuiWindowFlags_NoScrollWithMouse
                                                      : ImGuiWindowFlags_None);
  ImGui::PopStyleVar();

  if (steam_pending_ && settings_.add_games_to_steam && page_ != 2) {
    begin_card("steam-pending", ImGui::GetContentRegionAvail().x, {20, 14});
    float y = ImGui::GetCursorPosY();
    float right = ImGui::GetWindowContentRegionMax().x;
    auto pending = std::format("{} games will appear in Steam after it restarts.", steam_games_);
    ImVec2 bp = ImGui::GetCursorScreenPos();
    draw_text(ImGui::GetWindowDrawList(), fonts().regular, kBody, {std::round(bp.x), text_y(fonts().regular, kBody, bp.y, s(38))},
              color::text, pending);
    const char* label = steam_restart_ ? "Waiting for Steam to close##pending" : "Restart Steam##pending";
    float bw = row_button_w(steam_restart_ ? "Waiting for Steam to close" : "Restart Steam");
    ImGui::SetCursorPos({right - bw, y});
    if (button(label, 0, Tone::secondary, {bw, s(38)}, !steam_restart_)) restart_steam();
    end_card();
    gap(20);
  }

  if (page_ == 0 && !selected_.empty())
    draw_game_page();
  else
    switch (page_) {
      case 0: draw_library(); break;
      case 1: draw_downloads(); break;
      case 2: draw_settings(); break;
      case 3: draw_account(); break;
      case 4: draw_import(); break;
      case 5: draw_controls(); break;
    }
  draw_uninstall();
  draw_texture_warning();
  draw_save_import();
  draw_store_link();
  draw_cancel_download();
  draw_report();
  draw_update();
  draw_remove_account();
  draw_install_ask();
  draw_import_ask();
  ImGui::EndChild();
  ImGui::End();
  draw_toast();
  draw_chrome();
}

// Minimize, maximize/restore and close at the top right, and the title strip
// and borders the hit test turns into moves and resizes.
void App::draw_chrome() {
  if (!client_side_decorations()) return;
  ImGuiViewport* vp = ImGui::GetMainViewport();
  const bool maximized = SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED;
  const ImVec2 button{s(46), s(32)};
  const ImVec2 origin{vp->WorkPos.x + vp->WorkSize.x - 3 * button.x, vp->WorkPos.y};
  ImGui::SetNextWindowPos(origin);
  ImGui::SetNextWindowSize({3 * button.x, button.y});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
  ImGui::Begin("##chrome", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  ImGui::PopStyleVar();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float t = std::max(1.f, std::round(s(1)));
  const float g = std::round(s(5));  // glyph half-size
  auto control = [&](const char* id, ImU32 hover) {
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, button);
    if (ImGui::IsItemHovered()) {
      dl->AddRectFilled(a, {a.x + button.x, a.y + button.y}, hover);
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImGui::SameLine(0, 0);
    return std::pair{pressed, ImVec2{std::round(a.x + button.x * 0.5f), std::round(a.y + button.y * 0.5f)}};
  };
  {
    auto [pressed, c] = control("##minimize", color::surface_hi);
    dl->AddLine({c.x - g, c.y}, {c.x + g, c.y}, color::text, t);
    if (pressed) SDL_MinimizeWindow(window_);
  }
  {
    auto [pressed, c] = control("##maximize", color::surface_hi);
    if (maximized) {
      dl->AddRect({c.x - g + s(2), c.y - g}, {c.x + g, c.y + g - s(2)}, color::text, 0, 0, t);
      dl->AddRectFilled({c.x - g, c.y - g + s(2)}, {c.x + g - s(2), c.y + g}, color::bg);
      dl->AddRect({c.x - g, c.y - g + s(2)}, {c.x + g - s(2), c.y + g}, color::text, 0, 0, t);
    } else {
      dl->AddRect({c.x - g, c.y - g}, {c.x + g, c.y + g}, color::text, 0, 0, t);
    }
    if (pressed) maximized ? SDL_RestoreWindow(window_) : SDL_MaximizeWindow(window_);
  }
  {
    auto [pressed, c] = control("##close", IM_COL32(196, 43, 28, 255));
    dl->AddLine({c.x - g, c.y - g}, {c.x + g, c.y + g}, color::text, t);
    dl->AddLine({c.x - g, c.y + g}, {c.x + g, c.y - g}, color::text, t);
    if (pressed) {
      SDL_Event close{};
      close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
      close.window.windowID = SDL_GetWindowID(window_);
      SDL_PushEvent(&close);
    }
  }
  ImGui::End();
  const ImVec2 size = ImGui::GetIO().DisplaySize;  // pixels, like the hit test's points
  chrome_ = {int(size.x), int(size.y), maximized, s(40), s(6),
             {origin.x - vp->Pos.x, origin.y - vp->Pos.y, 3 * button.x, button.y}, ImGui::IsAnyItemHovered()};
}

// Notices appear as a toast at the bottom of the window and fade out after a
// few seconds unless the pointer rests on them.
void App::draw_toast() {
  double now = ImGui::GetTime();
  if (notice_ != toast_) {
    toast_ = notice_;
    toast_since_ = now;
  }
  ImGuiViewport* vp = ImGui::GetMainViewport();
  float shown = anim("toast-visible", toast_.empty() ? 0.f : 1.f, 14);
  if (shown < 0.01f && toast_.empty()) return;
  static std::string last;  // keeps the text while it fades out
  if (!toast_.empty()) last = toast_;
  float max_w = std::min(s(620), vp->WorkSize.x - s(80));
  ImVec2 ts = measure(fonts().regular, kBody, last, max_w - s(84));
  ImVec2 size{std::min(max_w, ts.x + s(84)), ts.y + s(30)};
  float sidebar = s(248);
  ImVec2 pos{vp->WorkPos.x + sidebar + (vp->WorkSize.x - sidebar - size.x) * 0.5f,
             vp->WorkPos.y + vp->WorkSize.y - size.y - s(28) + (1 - shown) * s(16)};
  // The window also holds the shadow around the toast: drawn past a window
  // the size of the toast, the shadow was cut off at its edges and only showed
  // as dark square corners behind the rounded ones.
  const ImVec2 lo{s(16), s(8)}, hi{s(16), s(24)};
  ImGui::SetNextWindowPos({pos.x - lo.x, pos.y - lo.y});
  ImGui::SetNextWindowSize({size.x + lo.x + hi.x, size.y + lo.y + hi.y});
  ImGui::SetNextWindowBgAlpha(0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::Begin("##toast", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  ImGui::PopStyleVar(2);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p{ImGui::GetWindowPos().x + lo.x, ImGui::GetWindowPos().y + lo.y};
  ImVec2 q{p.x + size.x, p.y + size.y};
  soft_shadow(dl, p, q, s(12), s(16), 0.6f * shown);
  dl->AddRectFilled(p, q, color::alpha(color::surface_top, shown), s(12));
  draw_text(dl, fonts().regular, kBody, {std::round(p.x + s(20)), std::round(p.y + s(15))}, color::alpha(color::text, shown),
            last, max_w - s(84));
  ImGui::SetCursorScreenPos({q.x - s(46), p.y + (size.y - s(32)) * 0.5f});
  if (icon_button("dismiss-toast", icons::cancel, nullptr, 32)) notice_.clear();
  bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(p, q);
  ImGui::End();
  if (hovered) toast_since_ = now;
  if (!toast_.empty() && now - toast_since_ > 7.0) notice_.clear();
}

void App::draw_sidebar() {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, color::bg);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {s(16), s(24)});
  ImGui::BeginChild("Sidebar", {s(248), 0}, ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float win_h = ImGui::GetWindowHeight();

  // Brand: the Venture logo and wordmark, on the page header's center line.
  {
    ImVec2 row = ImGui::GetCursorScreenPos();
    float m = std::round(s(52));
    ImVec2 p{std::round(row.x + s(10)), std::round(row.y + (s(kHeaderHeight) - m) * 0.5f)};
    if (SDL_Texture* mark = logo(int(m)))
      draw_image(dl, ImTextureID(reinterpret_cast<uintptr_t>(mark)), p, {p.x + m, p.y + m});
    draw_text(dl, fonts().semibold, 22, {std::round(p.x + m + s(14)), text_y(fonts().semibold, 22, row.y, s(kHeaderHeight))},
              color::text, "Venture");
    ImGui::Dummy({0, s(kHeaderHeight)});
  }
  gap(kAfterHeader + 4);


  auto jobs = downloads_->snapshot();
  auto active = std::ranges::count_if(jobs, [](auto& j) {
    return j.state == download::State::downloading || j.state == download::State::queued ||
           j.state == download::State::checking;
  });

  struct Nav {
    const char* label;
    char32_t glyph;
    int page;
  };
  // i18n: Sidebar pages.
  const Nav nav[] = {{tr("Library"), icons::library, 0},
                     {tr("Downloads"), icons::download, 1},
                     {tr("Import"), icons::package, 4},
                     {tr("Controls"), icons::game, 5},
                     {tr("Settings"), icons::settings, 2}};
  for (const auto& item : nav) {
    ImGui::PushID(item.page);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = s(40);
    if (ImGui::InvisibleButton("##nav", {w, h})) {
      page_ = item.page;
      if (item.page == 0) selected_.clear();
    }
    bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    bool current = page_ == item.page;
    float t = anim(ImGui::GetID("hover"), hovered ? 1.f : 0.f, 20);
    float sel = anim(ImGui::GetID("selected"), current ? 1.f : 0.f, 18);
    ImU32 rest = color::alpha(color::surface, t);
    dl->AddRectFilled(p, {p.x + w, p.y + h}, color::mix(rest, color::selected, sel), h * 0.5f);
    ImU32 fg = color::mix(color::mix(color::text2, color::text, t), color::on_selected, sel);
    draw_glyph(dl, item.glyph, kBody + 1, {p.x + s(26), p.y + h * 0.5f}, fg);
    draw_text(dl, fonts().semibold, kBody, {std::round(p.x + s(fonts().icons ? 50 : 18)), text_y(fonts().semibold, kBody, p.y, h)},
              fg, item.label);
    if (item.page == 1 && active) {
      std::string count = std::to_string(active);
      ImVec2 cs = measure(fonts().semibold, kLabel, count);
      float bw = std::max(s(22), cs.x + s(12)), bh = s(22);
      ImVec2 b0{std::round(p.x + w - bw - s(12)), std::round(p.y + (h - bh) * 0.5f)};
      dl->AddRectFilled(b0, {b0.x + bw, b0.y + bh}, color::accent, bh * 0.5f);
      draw_text(dl, fonts().semibold, kLabel, {std::round(b0.x + (bw - cs.x) * 0.5f), text_y(fonts().semibold, kLabel, b0.y, bh)},
                IM_COL32_WHITE, count);
    }
    gap(4);
    ImGui::PopID();
  }

  // Account pinned to the bottom; opens the accounts page.
  float block_h = s(60);
  ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY() + s(20), win_h - block_h - s(18)));
  {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = block_h;
    if (ImGui::InvisibleButton("##account", {w, h})) page_ = 3;
    bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    float t = anim(ImGui::GetID("account-hover"), hovered || page_ == 3 ? 1.f : 0.f, 20);
    dl->AddRectFilled(p, {p.x + w, p.y + h}, color::alpha(color::surface, t), s(14));
    ImVec2 c{p.x + s(28), p.y + h * 0.5f};
    const AccountView* current = session_ ? account(account_key_) : nullptr;
    AccountView fallback{account_key_, session_ ? session_->alias : "", session_ ? session_->display_name : ""};
    const AccountView* shown = current ? current : session_ ? &fallback : nullptr;
    avatar(dl, c, s(17), shown, false, kSmall);
    float tx = p.x + s(56);
    std::string name = shown ? account_name(*shown) : tr("Sign in");
    draw_text(dl, fonts().semibold, kSmall + 1, {std::round(tx), text_y(fonts().semibold, kSmall + 1, c.y - s(9))},
              color::text, ellipsize(fonts().semibold, kSmall + 1, name, w - s(64)));
    // i18n: Under the account name at the bottom of the sidebar.
    std::string state = !session_          ? tr("Connect your Meta account")
                        : options_.offline ? tr("Offline")
                        : accounts_.size() > 1 ? trn("{0} account", "{0} accounts", accounts_.size())
                                               : tr("Meta account");
    draw_text(dl, fonts().regular, kSmall - 0.5f, {std::round(tx), text_y(fonts().regular, kSmall - 0.5f, c.y + s(9))},
              color::text2, ellipsize(fonts().regular, kSmall - 0.5f, state, w - s(64)));
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
void App::draw_library() {
  auto all = visible_games();
  auto jobs = downloads_->snapshot();
  // Hidden entries are left out unless the player shows them (the chip below).
  const auto hidden_count = std::ranges::count_if(all, [&](auto& g) { return hidden(g.id); });
  std::erase_if(all, [&](auto& g) { return !settings_.listed(g.id); });

  // Header: title left; search, sort and refresh at the end of the filter row.
  float top = ImGui::GetCursorPosY();
  page_title("Library");
  if (!warning_.empty()) {
    ImGui::SameLine(0, s(10));
    ImGui::SetCursorPosY(top + s(kHeaderHeight - 20) * 0.5f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy({s(20), s(20)});
    draw_glyph(ImGui::GetWindowDrawList(), icons::info, kBody, {p.x + s(10), p.y + s(10)}, color::text3);
    if (ImGui::IsItemHovered())
      tooltip("Meta didn't confirm your full library, so games confirmed earlier are kept. " + warning_);
  }
  float after_title = top + s(kHeaderHeight);
  const float sort_w = measure(fonts().semibold, kSmall + 1, "Sort by: Recently played").x + s(52);
  if (!all.empty() || !search_.empty() || hidden_count) {
    // At the end of the filter chips' row, centred on them and clear of the
    // window buttons; on its own row under the title when the window is narrow.
    const char* sorts[] = {"Name", "Recently played"};
    float content_w = ImGui::GetContentRegionAvail().x;
    float title_w = measure(fonts().semibold, kDisplay, "Library").x + s(warning_.empty() ? 24 : 54);
    const bool can_refresh = refreshing_ || (session_ && !options_.offline);
    float fixed = s(10) + sort_w + (can_refresh ? s(10) + s(38) : 0);
    const bool chips = !all.empty() || hidden_count;
    const float chips_w = chips ? s(hidden_count ? 380 : 240) : title_w;
    bool beside = content_w - chips_w - fixed >= s(200);
    float search_w = std::min(s(260), (beside ? content_w - chips_w : content_w) - fixed);
    float toolbar_w = search_w + fixed;
    if (beside && !chips) search_w = std::max(s(120), search_w - s(header_right_inset())), toolbar_w = search_w + fixed;
    float x = ImGui::GetWindowContentRegionMax().x - toolbar_w - (beside && !chips ? s(header_right_inset()) : 0);
    float y = !beside ? after_title + s(10)
              : chips ? after_title + s(kAfterHeader - 6) + (s(kChipHeight) - s(38)) * 0.5f
                      : top + s(kHeaderHeight - 38) * 0.5f;
    if (!beside) after_title = y + s(38);
    ImGui::SetCursorPos({beside ? x : ImGui::GetWindowContentRegionMin().x, y});
    search_field("search", "Search", &search_, search_w);
    ImGui::SameLine(0, s(10));
    dropdown("sort", "Sort by: ", sorts, 2, &sort_, sort_w);
    if (can_refresh) {
      ImGui::SameLine(0, s(10));
      if (refreshing_) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy({s(38), s(38)});
        ImGui::SetCursorScreenPos({p.x + s(10), p.y + s(10)});
        spinner(9, color::text2);
      } else if (icon_button("refresh", icons::refresh, nullptr, 38, true, true)) {
        refresh();
      }
    }
  }
  ImGui::SetCursorPosY(after_title + s(kAfterHeader) - s(6));

  // Filter chips; "Hidden" shows or hides the entries the player hid.
  if (!all.empty() || hidden_count) {
    auto installed_count = std::ranges::count_if(all, [&](auto& g) { return installed(g.id) != nullptr; });
    std::string all_label = std::format("All  {}##all", all.size());
    std::string installed_label = std::format("Installed  {}##installed", installed_count);
    if (filter_chip(all_label.c_str(), !installed_only_)) installed_only_ = false;
    ImGui::SameLine(0, s(8));
    if (filter_chip(installed_label.c_str(), installed_only_)) installed_only_ = true;
    if (hidden_count) {
      ImGui::SameLine(0, s(8));
      // i18n: Library filter chip that lists the games the player hid; {0} is how many.
      std::string hidden_label = trf("Show hidden  {0}", hidden_count) + "##hidden";
      if (filter_chip(hidden_label.c_str(), settings_.show_hidden))
        action([&] { set_show_hidden(!settings_.show_hidden); });
    }
    gap(18);
  }

  std::erase_if(all, [&](auto& g) {
    return !search::matches(search_, g.name) || (installed_only_ && !installed(g.id));
  });
  std::sort(all.begin(), all.end(), [&](auto& a, auto& b) {
    if (sort_ && a.recent != b.recent) return a.recent > b.recent;
    // Installed games first when nothing was played yet, like the headset.
    if (sort_ && (installed(a.id) != nullptr) != (installed(b.id) != nullptr)) return installed(a.id) != nullptr;
    return lowercase(a.name) < lowercase(b.name);
  });

  // The grid's padding only makes room for the hover ring and lift, so the
  // child starts that much further out and the tiles line up with the title.
  float ring = s(6);
  ImGui::SetCursorPos({ImGui::GetCursorPosX() - ring, ImGui::GetCursorPosY() - ring});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {ring, ring});
  ImGui::BeginChild("Game grid", {ImGui::GetContentRegionAvail().x + ring * 2, 0}, ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  // Smooth wheel scrolling.
  static float scroll_target = 0;
  scroll_target = std::clamp(scroll_target, 0.f, ImGui::GetScrollMaxY());
  if (ImGui::IsWindowHovered())
    scroll_target = std::clamp(scroll_target - ImGui::GetIO().MouseWheel * s(140), 0.f, ImGui::GetScrollMaxY());
  if (std::abs(scroll_target - ImGui::GetScrollY()) > 0.5f)
    ImGui::SetScrollY(ImGui::GetScrollY() +
                      (scroll_target - ImGui::GetScrollY()) * (1 - std::exp(-16 * ImGui::GetIO().DeltaTime)));

  if (all.empty()) {
    gap(80);
    if (refreshing_)
      empty_state(icons::library, "Finding your games\xE2\x80\xA6", "");
    else if (!search_.empty())
      empty_state(icons::search, "No results", "");
    else if (installed_only_)
      empty_state(icons::download, "Nothing installed yet", "");
    else if (hidden_count)
      // i18n: Library empty state when the player hid every game; "Show hidden" is the filter chip.
      empty_state(icons::hide, tr("All your games are hidden"), tr("Select Show hidden to see them."));
    else if (!session_)
      empty_state(icons::library, "No games yet", "Sign in, or import a game.");
    else
      empty_state(icons::library, "No games yet", refresh_error_);
    if (session_ && !refreshing_ && search_.empty() && !installed_only_ && !hidden_count && !options_.offline) {
      gap(22);
      float bw = s(200);
      ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - bw) * 0.5f + ImGui::GetCursorPosX());
      if (button("Refresh library", 0, Tone::primary, {bw, s(44)})) refresh();
    }
    if (!session_ && !refreshing_ && search_.empty()) {
      gap(22);
      float bw = s(200);
      ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - bw) * 0.5f + ImGui::GetCursorPosX());
      if (button("Sign in with Meta", 0, Tone::primary, {bw, s(44)})) {
        page_ = 3;
        sign_in();
      }
    }
  }

  float grid_w = ImGui::GetContentRegionAvail().x;
  float gap_x = std::round(s(20)), gap_y = std::round(s(22));
  int columns = std::max(1, int((grid_w + gap_x) / (s(236) + gap_x)));
  float card_w = std::floor((grid_w - gap_x * (columns - 1)) / columns);
  float art_h = std::round(card_w * 9 / 16);
  float card_h = art_h + std::round(s(58));
  ImVec2 start = ImGui::GetCursorScreenPos();
  // Room above the first row for a hovered card's lift and focus ring (3 + 3 px
  // plus the 2 px stroke), which the scroll region would otherwise clip.
  start = {std::round(start.x), std::round(start.y + s(8))};
  ImDrawList* gdl = ImGui::GetWindowDrawList();
  for (size_t i = 0; i < all.size(); ++i) {
    auto& g = all[i];
    int col = int(i % columns), row = int(i / columns);
    ImVec2 p{std::round(start.x + col * (card_w + gap_x)), std::round(start.y + row * (card_h + gap_y))};
    ImGui::SetCursorScreenPos(p);
    ImGui::PushID(g.id.c_str());
    bool clicked = ImGui::InvisibleButton("card", {card_w, card_h});
    bool hovered = ImGui::IsItemHovered();
    // Right-click: hide the entry from the library, or bring a hidden one back.
    // --smoke-view library-menu opens it on the first card for screenshots,
    // library-menu-hidden on the first hidden one.
    const bool is_hidden = hidden(g.id);
    // --smoke-view library-hover shows the first card hovered (its hide button),
    // library-hover-hidden the first hidden one (its Unhide button).
    static bool smoke_hover = options_.smoke_view.starts_with("library-hover");
    if (smoke_hover && (is_hidden || options_.smoke_view != "library-hover-hidden") &&
        std::exchange(smoke_hover, false))
      smoke_hovered_ = g.id;
    hovered |= smoke_hovered_ == g.id;
    static bool smoke_card_menu = options_.smoke_view.starts_with("library-menu");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
        (smoke_card_menu && (is_hidden || options_.smoke_view != "library-menu-hidden") &&
         std::exchange(smoke_card_menu, false))) {
      ImGui::OpenPopup("card-menu");
      // At the pointer; a smoke run's has no position, so the card's middle.
      card_menu_at_ = ImGui::IsMousePosValid() && ImGui::IsItemClicked(ImGuiMouseButton_Right)
                          ? ImGui::GetIO().MousePos
                          : ImVec2{p.x + card_w * 0.5f, p.y + card_h * 0.4f};
    }
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    float t = anim(ImGui::GetID("hover"), hovered ? 1.f : 0.f, 16);
    const auto* local = installed(g.id);
    bool running = running_.contains(g.id) && running_[g.id].process.active();
    auto job = download::base_job(jobs, g.id);
    bool downloading = job && (job->state == download::State::downloading ||
                               job->state == download::State::queued || job->state == download::State::checking);

    // Art tile: lifts on hover with the white focus ring Horizon uses.
    float lift = std::round(s(3) * t);
    ImVec2 a0{p.x, p.y - lift}, a1{p.x + card_w, p.y + art_h - lift};
    float r = s(12);
    if (t > 0.01f) soft_shadow(gdl, a0, a1, r, s(18), 0.7f * t);
    ImGui::SetCursorScreenPos(a0);
    art(g.id, {card_w, art_h}, r, 0.03f * t);
    if (!local && !downloading) gdl->AddRectFilled(a0, a1, color::alpha(color::bg, 0.42f * (1 - t * 0.6f)), r);
    if (is_hidden)  // shown with "Show hidden": dimmed, with the hidden mark (below)
      gdl->AddRectFilled(a0, a1, color::alpha(color::bg, 0.5f * (1 - t * 0.6f)), r);
    // Top-left, across from the cloud badge: a hidden entry's mark, and on hover
    // the button that hides the entry, or on a hidden one brings it back.
    if (is_hidden || t > 0.01f) {
      ImVec2 c{a0.x + s(22), a0.y + s(22)};
      float cr = s(14);
      ImVec2 m = ImGui::GetIO().MousePos;
      bool over = t > 0.01f && (m.x - c.x) * (m.x - c.x) + (m.y - c.y) * (m.y - c.y) <= (cr + s(2)) * (cr + s(2));
      float shown = is_hidden ? 1.f : t;
      gdl->AddCircleFilled(c, cr + (over ? s(1.5f) : 0),
                           over ? IM_COL32_WHITE : color::alpha(IM_COL32(20, 21, 24, 190), shown), 32);
      draw_glyph(gdl, is_hidden && t > 0.5f ? icons::view : icons::hide, kSmall, c,
                 over ? color::on_selected : color::alpha(color::text, shown));
      if (over) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        tooltip(is_hidden ? tr("Unhide") : tr("Hide from library"));
      }
      if (clicked && over) {
        clicked = false;
        action([&] { set_hidden(g.id, !is_hidden); });
      }
    }
    if (downloading) {
      vertical_fade(gdl, {a0.x, a1.y - s(44)}, a1, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 150));
      float frac = job->total ? float(double(job->done) / double(job->total)) : 0.f;
      ImGui::SetCursorScreenPos({a0.x + s(12), a1.y - s(14)});
      progress_bar(frac, {card_w - s(24), s(4)}, !job->total);
    }
    if (!local && !downloading) {  // cloud badge: available to install
      ImVec2 c{a1.x - s(22), a0.y + s(22)};
      gdl->AddCircleFilled(c, s(14), IM_COL32(20, 21, 24, 190), 32);
      draw_glyph(gdl, icons::cloud, kSmall, c, color::text);
    }
    if (t > 0.01f) {
      gdl->AddRect({a0.x - s(3), a0.y - s(3)}, {a1.x + s(3), a1.y + s(3)}, color::alpha(color::selected, t), r + s(3), 0,
                   s(2));
      // Quick action: play an installed game, or install one.
      bool can_play = local && !running;
      bool can_install = !local && !downloading && session_ && !options_.offline;
      if (can_play || can_install) {
        ImVec2 c{a1.x - s(30), a1.y - s(30)};
        float cr = s(20);
        ImVec2 m = ImGui::GetIO().MousePos;
        bool over = (m.x - c.x) * (m.x - c.x) + (m.y - c.y) * (m.y - c.y) <= cr * cr;
        gdl->AddCircleFilled(c, cr + (over ? s(1.5f) : 0), color::alpha(over ? IM_COL32_WHITE : color::selected, t), 40);
        draw_glyph(gdl, can_play ? icons::play : icons::download, kBody, {c.x + (can_play ? s(1) : 0), c.y},
                   color::alpha(color::on_selected, t));
        if (clicked && over) {
          clicked = false;
          if (can_play)
            action([&] { play(g); });
          else
            action([&] { install_for_owner(g); });
        }
      }
    }

    // Caption: name, then one quiet status line.
    float ty = p.y + art_h + std::round(s(12));
    draw_text(gdl, fonts().semibold, kBody, {p.x, ty}, color::text, ellipsize(fonts().semibold, kBody, g.name, card_w));
    float sy = ty + std::round(s(22));
    std::string status;
    ImU32 status_col = color::text2;
    if (running) {
      draw_text(gdl, fonts().regular, kSmall, {p.x, sy}, color::text2, "Playing now");
    } else {
      if (downloading)
        status = job->state == download::State::downloading && job->total
                     ? std::format("Downloading  {:.0f}%", 100.0 * double(job->done) / double(job->total))
                     : std::string(download::label(job->state));
      else if (job && job->state == download::State::paused && !local)
        status = "Paused";
      else if (job && job->state == download::State::failed)
        status = "Needs attention", status_col = color::warning;
      else if (!local)
        status = installed(g.id, false) ? "Partly downloaded" : "Not installed";
      else
        status = g.publisher;
      if (is_hidden) {
        // i18n: Status under a library entry the player hid, while hidden entries are shown.
        status = (status.empty() ? std::string() : status + "  \xC2\xB7  ") + tr("Hidden");
        status_col = color::text3;
      }
      draw_text(gdl, fonts().regular, kSmall, {p.x, sy}, status_col, ellipsize(fonts().regular, kSmall, status, card_w));
    }
    if (clicked) select(g.id);
    ImGui::SetNextWindowPos(card_menu_at_, ImGuiCond_Appearing);
    if (begin_menu("card-menu")) {
      if (is_hidden ? menu_item(tr("Unhide"), icons::view) : menu_item(tr("Hide from library"), icons::hide))
        action([&] { set_hidden(g.id, !is_hidden); });
      end_menu();
    }
    ImGui::PopID();
  }
  if (!all.empty()) {
    int rows = int((all.size() + columns - 1) / columns);
    ImGui::SetCursorScreenPos({start.x, start.y + rows * (card_h + gap_y)});
    ImGui::Dummy({grid_w, s(10)});
  }
  ImGui::EndChild();
}

// ---------------------------------------------------------------------------
void App::draw_details() { draw_game_page(); }

// The game page's download block: percentage and time left over the bar,
// transferred size and speed under it. Paused progress is drawn muted.
void App::download_progress(const download::Job& job, float width) {
  using download::State;
  bool active = job.state == State::queued || job.state == State::checking || job.state == State::downloading;
  bool measured = job.total && job.state != State::queued && job.state != State::checking;
  float frac = job.total ? float(double(job.done) / double(job.total)) : 0.f;
  float shown = anim(ImGui::GetID(("progress-" + job.app_id).c_str()), frac, 6.f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  p = {std::round(p.x), std::round(p.y)};
  float right = p.x + width, row = s(30), y = p.y;
  // Percentage (or the queue state) with the time left on its line.
  ImFont* lead_font = fonts().semibold;
  float lead_size = measured ? kHeading + 4 : kBody;
  std::string lead = measured ? std::format("{:.0f}%", frac * 100)
                     : job.importing ? std::string("Importing")
                                     : std::string(download::label(job.state));
  float lead_top = text_y(lead_font, lead_size, y, row);
  draw_text(dl, lead_font, lead_size, {p.x, lead_top}, measured ? color::text : color::text2,
            ellipsize(lead_font, lead_size, lead, width));
  std::string note;
  ImU32 note_col = color::text2;
  // A paused download says so in the badge above.
  if (job.state == State::failed)
    note = "Needs attention", note_col = color::warning;
  else if (job.state == State::downloading && job.bytes_per_second > 0 && job.total > job.done)
    note = time_left(uint64_t(double(job.total - job.done) / job.bytes_per_second));
  if (measured && !note.empty()) {
    // Centered on the percentage's cap height so both read as one line.
    float mid = cap_center(lead_font, lead_size, lead_top);
    float nw = measure(fonts().regular, kSmall, note).x;
    draw_text(dl, fonts().regular, kSmall,
              {std::round(right - nw), std::round(mid - cap_center(fonts().regular, kSmall, 0))}, note_col, note);
  }
  y += row + s(8);
  ImGui::SetCursorScreenPos({p.x, y});
  progress_bar(shown, {width, s(6)}, active && !measured, active ? 0 : color::text3);
  y += s(6);
  // Transferred size and speed under the bar.
  if (measured) {
    y += s(8);
    float h = s(18);
    float top = text_y(fonts().regular, kSmall, y, h);
    draw_text(dl, fonts().regular, kSmall, {p.x, top}, color::text3, bytes(job.done) + " of " + bytes(job.total));
    if (job.state == State::downloading && job.bytes_per_second > 0) {
      std::string speed = bytes(uint64_t(job.bytes_per_second)) + "/s";
      draw_text(dl, fonts().regular, kSmall, {std::round(right - measure(fonts().regular, kSmall, speed).x), top},
                color::text3, speed);
    }
    y += h;
  }
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy({width, y - p.y});
  if (job.state == State::failed && !job.error.empty()) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    text(fonts().regular, kSmall, color::warning, job.error);
    ImGui::PopTextWrapPos();
  }
}

void App::draw_game_page() {
  auto list = visible_games();
  auto it = std::ranges::find_if(list, [&](auto& g) { return g.id == selected_; });
  if (it == list.end()) {
    selected_.clear();
    return;
  }
  auto g = *it;
  const auto* local = installed(g.id);
  const auto* partial = installed(g.id, false);
  auto detail = details_.find(g.id);
  bool update = update_available(g.id);
  // --smoke-view running shows a running game's page for screenshots.
  bool running = (running_.contains(g.id) && running_[g.id].process.active()) || options_.smoke_view == "running";
  auto job = download::base_job(downloads_->snapshot(), g.id);
  bool downloading = job && (job->state == download::State::downloading || job->state == download::State::queued ||
                             job->state == download::State::checking);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 origin = ImGui::GetWindowPos();
  float win_w = ImGui::GetWindowWidth();
  float scroll = ImGui::GetScrollY();
  float appear = anim(ImGui::GetID(("page-" + g.id).c_str()), 1.f, 9.f);
  // The cover's colors wash softly into the top of the page.
  ImVec2 b0{origin.x, origin.y - scroll}, b1{origin.x + win_w, origin.y + s(520) - scroll};
  draw_backdrop(g.id, b0, b1, 0.30f * appear);
  vertical_fade(dl, b0, b1, color::alpha(color::bg, 0.35f), color::bg);

  {
    ImVec2 header = ImGui::GetCursorPos();
    ImGui::SetCursorPos({header.x - s(12), header.y + s(kHeaderHeight - 34) * 0.5f});
    float bw = button_width("Library", icons::chevron_left) - s(16);
    if (button("Library##back", icons::chevron_left, Tone::ghost, {bw, s(34)})) {
      selected_.clear();
      return;
    }
    ImGui::SetCursorPos({header.x, header.y + s(kHeaderHeight)});
    gap(kAfterHeader - ImGui::GetStyle().ItemSpacing.y);
  }

  // Media left, info card right (stacked on narrow windows). Wide windows center the page at a store-like width
  // instead of growing the art without limit.
  float full = ImGui::GetContentRegionAvail().x, avail = std::min(full, s(1360));
  float pad = std::floor((full - avail) * 0.5f);
  if (pad > 0) ImGui::Indent(pad);
  bool stacked = avail < s(900);
  float card_w = stacked ? avail : std::clamp(avail * 0.38f, s(340), s(440));
  float media_w = stacked ? avail : avail - card_w - s(24);
  float media_h = std::round(media_w * 9 / 16);
  float slide = (1 - appear) * s(14);
  ImVec2 mp = ImGui::GetCursorScreenPos();
  mp.y += slide;
  soft_shadow(dl, mp, {mp.x + media_w, mp.y + media_h}, s(12), s(24), 0.7f);
  ImGui::SetCursorScreenPos(mp);
  art(g.id, {media_w, media_h}, s(12));
  float after_media = ImGui::GetCursorPosY();
  if (!stacked) {
    ImGui::SameLine(0, s(24));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + slide);
  } else {
    gap(18);
  }

  const float card_top = ImGui::GetCursorScreenPos().y;
  begin_card("info", card_w, {24, 24});
  {
    float inner = ImGui::GetContentRegionAvail().x;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
    text(fonts().semibold, kTitle + 2, color::text, g.name);
    ImGui::PopTextWrapPos();
    if (!g.publisher.empty()) text(fonts().regular, kBody, color::text2, g.publisher, false);
    gap(10);
    // State badges and the installed build.
    if (running)
      badge("Playing now", color::positive, color::alpha(color::positive, 0.16f));
    else if (local)
      badge("Installed", color::text, color::surface_top, icons::check);
    else if (downloading)
      badge("Downloading", color::accent_text, color::accent_wash, icons::download);
    else if (job && job->state == download::State::failed)
      badge("Needs attention", color::warning, color::alpha(color::warning, 0.14f));
    else if (partial || (job && job->state == download::State::paused && job->done))
      badge("Paused", color::text2, color::surface_hi);
    else
      badge("Not installed", color::text2, color::surface_hi, icons::cloud);
    if (update) {
      ImGui::SameLine(0, s(6));
      badge("Update available", color::accent_text, color::accent_wash, icons::update);
    }
    if (hidden(g.id)) {
      ImGui::SameLine(0, s(6));
      badge(tr("Hidden"), color::text2, color::surface_hi, icons::hide);
    }
    if (local) {
      ImGui::SameLine(0, s(10));
      ImVec2 b = ImGui::GetCursorScreenPos();
      std::string build = (local->plan.version.empty() ? std::string() : "Version " + local->plan.version + "  \xC2\xB7  ") +
                          bytes(local->bytes);
      std::string shown = ellipsize(fonts().regular, kSmall, build, ImGui::GetContentRegionAvail().x);
      draw_text(ImGui::GetWindowDrawList(), fonts().regular, kSmall, {std::round(b.x), text_y(fonts().regular, kSmall, b.y, s(22))},
                color::text3, shown);
      ImGui::Dummy({measure(fonts().regular, kSmall, shown).x, s(22)});
    }
    if (local && !local->plan.assets_complete) {
      gap(10);
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
      text(fonts().regular, kSmall, color::warning,
           "Meta's asset catalog was unavailable when this was installed; some downloadable content may be missing.");
      ImGui::PopTextWrapPos();
    }
    gap(14);

    // Primary action, full width like the store's price button. Play and
    // Install act for the owning account (see draw_owner).
    auto owners = game_owners(g.id);
    std::string owner = owner_for(g.id);
    bool active_owns = std::ranges::find(owners, account_key_) != owners.end();
    // Files and the overflow menu sit beside the primary button (not mid-download);
    // the menu is there for every game, as it can hide the game from the library.
    bool tools = partial && !downloading;
    // A version pinned beside the game's own install keeps its build.
    const bool pinned = !meta::version_binary(g.id).empty();
    // Any version of a store game can be installed beside the others.
    const bool versions = digits(g.id, 5) && !owners.empty() && !downloading;
    ImVec2 cta{inner - (tools ? s(52) * 2 : s(52)), s(46)}, more{};
    auto draw_tools = [&] {
      if (tools) {
        ImGui::SameLine(0, s(6));
        if (icon_button("folder", icons::folder, nullptr, 46, true, true))
          action([&] { open_folder(partial->directory); });
      }
      ImGui::SameLine(0, s(6));
      more = ImGui::GetCursorScreenPos();
      // --smoke-view menu opens it once for screenshots.
      static bool smoke_menu = options_.smoke_view == "menu";
      if (icon_button("more", icons::more, nullptr, 46, true, true) || std::exchange(smoke_menu, false))
        ImGui::OpenPopup("game-menu");
    };
    // A running game always offers Quit, even when its install no longer counts as
    // complete (files changed or a catalog refresh listed new ones).
    if (local || running) {
      if (launch_choice(g.id, play_key(g.id)).status == library::LaunchStatus::reconnect) {
        if (button("Reconnect to play##play", 0, Tone::primary, cta)) page_ = 3;
      } else if (running) {
        // Quits as the headset's menu would: the game gets its Android lifecycle and saves first.
        auto& r = running_[g.id];
        const bool armed = std::chrono::steady_clock::now() - r.started >= std::chrono::seconds(3);
        if (button(r.quitting ? "Quitting...##play" : "Quit game##play", r.quitting ? 0 : icons::cancel,
                   Tone::secondary, cta, !r.quitting && armed))
          r.quitting = r.process.request_stop();
      } else if (button("Play##play", icons::play, Tone::primary, cta)) {
        action([&] { play(g); });
      }
      draw_tools();
      if (update) {
        gap(8);
        if (button("Update", icons::update, Tone::secondary, {inner, s(42)}, session_ && !options_.offline && !downloading))
          action([&] { install(g); });
      }
    } else if (downloading) {
      download_progress(*job, inner);
      gap(4);
      if (button("Pause download##download", icons::pause, Tone::secondary, {inner - s(52), s(46)}))
        downloads_->cancel(g.id);
      ImGui::SameLine(0, s(6));
      if (icon_button("show-downloads", icons::download, nullptr, 46, true, true)) page_ = 1;
    } else {
      // Downloads belong to the active account, so installing a game that only
      // another account owns switches to that account first.
      const AccountView* other = !owner.empty() && owner != account_key_ ? account(owner) : nullptr;
      bool usable = other ? other->signed_in && !other->needs_reauth : session_.has_value();
      std::string size_note;
      if (detail != details_.end() && detail->second.size) size_note = "  \xC2\xB7  " + bytes(detail->second.size);
      // Progress of a paused download: the live job, else what an earlier
      // session left on disk against its install plan.
      std::optional<download::Job> paused;
      if (job && (job->state == download::State::paused || job->state == download::State::failed) && job->done &&
          job->total)
        paused = job;
      else if (partial && partial->bytes && partial->plan.size() > partial->bytes)
        paused = download::Job{.app_id = g.id, .done = partial->bytes, .total = partial->plan.size()};
      bool resumable = paused.has_value();
      if (paused) {
        download_progress(*paused, inner);
        gap(4);
        size_note.clear();
      }
      std::string label = (resumable || partial ? "Resume download" : "Install") + size_note + "##install";
      if (button(label.c_str(), icons::download, Tone::primary, cta, usable && !options_.offline))
        action([&] { request_install(g); });
      draw_tools();
      if (!session_ && owners.empty()) {
        gap(8);
        text(fonts().regular, kSmall, color::text3, "Sign in to Meta to install this game.", false);
      }
    }
    // The owning account (when it is not simply the active one) and the mods
    // folder, at the bottom of a card as tall as the cover beside it.
    bool owner_row = owners.size() > 1 || (!owners.empty() && !active_owns);
    const float bottom = card_top + media_h - s(24);
    if (owner_row || tools) {
      auto* storage = ImGui::GetStateStorage();
      const ImGuiID rows_id = ImGui::GetID("info-rows");
      const float rows_h = storage->GetFloat(rows_id, 0);
      ImVec2 at = ImGui::GetCursorScreenPos();
      at.y += s(6);
      if (!stacked && rows_h > 0) at.y = std::max(at.y, std::round(bottom - rows_h));
      ImGui::SetCursorScreenPos(at);
      // The account picker and Open folder share one width and height.
      float control_w = button_width("Open folder", icons::folder);
      if (const AccountView* a = owner_row ? account(owner_for(g.id)) : nullptr)
        control_w = std::max(control_w, std::min(s(220), s(70) + measure(fonts().semibold, kSmall, account_name(*a)).x));
      ImGui::BeginGroup();
      if (owner_row) {
        divider();
        draw_owner(g, local != nullptr, control_w);
      }
      if (tools) {
        divider();
        draw_mods(partial->directory, partial->plan.package, control_w);
      }
      ImGui::EndGroup();
      // The rows round their own positions, so their height can differ by a pixel
      // with where they sit; following that pixel moved them back and forth every
      // frame. Only a real change of height moves them.
      const float height = ImGui::GetItemRectSize().y;
      if (std::abs(height - rows_h) >= 2) storage->SetFloat(rows_id, height);
    }
    if (!stacked && ImGui::GetCursorScreenPos().y < bottom)
      ImGui::Dummy({0, bottom - ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y});
    {
      ImGui::SetNextWindowPos({more.x + s(46), more.y + s(52)}, ImGuiCond_Appearing, {1, 0});
      if (begin_menu("game-menu")) {
        if (tools && !pinned && menu_item("Check for updates", icons::refresh, false, session_ && !options_.offline)) {
          details_.erase(g.id);
          select(g.id);
        }
        if (versions && menu_item(partial ? "Install another version\xE2\x80\xA6" : "Install a version\xE2\x80\xA6",
                                  icons::download, false, session_ && !options_.offline))
          action([&] { request_versions(g); });
        if (tools && steam_sync_allowed_ && settings_.add_games_to_steam) {
          bool in = in_steam(g.id);
          if (menu_item(in ? "Remove from Steam" : "Add to Steam", 0, false, true,
                        ImTextureID(reinterpret_cast<uintptr_t>(steam_logo(int(std::round(s(16))))))))
            action([&] { set_in_steam(g.id, !in); });
        }
        // The game's data for the account it plays as: its private storage, where
        // saves live, and its shared storage (Android/data/<package>, in
        // external-data), where homebrew ports look for the game data the player
        // provides.
        auto open_data = [&](bool shared) {
          action([&, shared] {
            const auto key = launch_choice(g.id, play_key(g.id)).account_key;
            auto folder = account_data_dir(data_, key.empty() ? "local" : key, partial->plan.package,
                                           partial->plan.pinned ? partial->plan.folder() : std::string());
            if (shared) folder /= "external-data";
            fs::create_directories(folder);
            open_folder(folder);
          });
        };
        if (tools && menu_item("Show save data", icons::folder, false, true)) open_data(false);
        if (tools && menu_item("Show game data", icons::folder, false, true)) open_data(true);
        // Save imports go to the game's own install.
        if (tools && !pinned && menu_item("Import save data", icons::save, false, !running)) save_import_target_ = g.id;
        // An import no signed-in account owns, keyed by its package: the store
        // listing it is names it and brings its art (found by package, or here).
        if (tools && !pinned && partial->plan.local && !digits(partial->plan.app_id, 5)) {
          if (partial->plan.store_app_id.empty()) {
            if (menu_item(tr("Link to store page\xE2\x80\xA6"), icons::globe, false, !options_.offline)) {
              store_link_target_ = g.id;
              store_link_text_.clear();
              store_link_check_ = {};
            }
          } else if (menu_item(tr("Remove store link"), 0, false, true)) {
            action([&] { remove_store_link(g.id); });
          }
        }
        // A view filter only: the game stays installed, playable and in Steam.
        if (hidden(g.id) ? menu_item(tr("Unhide"), icons::view) : menu_item(tr("Hide from library"), icons::hide))
          action([&] { set_hidden(g.id, !hidden(g.id)); });
        if (tools && menu_item("Uninstall", icons::trash, true, !running && !downloads_->busy()))
          uninstall_target_ = *partial;
        end_menu();
      }
    }
  }
  end_card();
  ImGui::SetCursorPosY(std::max(after_media + slide, ImGui::GetCursorPosY()) + s(26));
  ImGui::Dummy({0, 0});

  // A runtime that stopped with an error explains itself; the log stays one
  // click away instead of always on screen.
  // A game that closed itself for a known reason (exit code 0) explains it too.
  // --smoke-view stopped shows it for screenshots.
  if (options_.smoke_view == "stopped") {
    auto& r = running_[g.id];
    r.exit = 1;
    r.failure = "Example: the game stopped with an error.";
  }
  if (auto r = running_.find(g.id);
      r != running_.end() && r->second.exit && (*r->second.exit != 0 || !r->second.failure.empty())) {
    begin_card("stopped", ImGui::GetContentRegionAvail().x, {22, 18}, color::alpha(color::danger, 0.12f));
    float right = ImGui::GetWindowContentRegionMax().x;
    float y = ImGui::GetCursorPosY();
    text(fonts().semibold, kBody, color::text, g.name + (*r->second.exit != 0 ? " stopped unexpectedly" : " closed"),
         false);
    const std::string failure =
        r->second.failure.empty() ? std::format("Exit code {}", *r->second.exit) : r->second.failure;
    ImGui::PushTextWrapPos(right - s(400));
    text(fonts().regular, kSmall, color::text2, failure);
    ImGui::PopTextWrapPos();
    const float block = ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y - y;
    const float row_h = std::max(block, s(kControlH));
    // The three buttons share one width.
    const float bw = std::max({button_width("Report issue"), button_width("Copy log"), button_width("Hide log")});
    const float by = y + (row_h - s(kControlH)) * 0.5f;
    bool open = log_open_.contains(g.id);
    const char* toggle_label = open ? "Hide log##log" : "Show log##log";
    float x = right - bw;
    ImGui::SetCursorPos({x, by});
    if (button(toggle_label, 0, Tone::secondary, {bw, s(kControlH)})) {
      if (open) log_open_.erase(g.id);
      else log_open_.insert(g.id);
    }
    x -= s(8) + bw;
    ImGui::SetCursorPos({x, by});
    if (button("Copy log##copy", 0, Tone::secondary, {bw, s(kControlH)})) ImGui::SetClipboardText(r->second.tail.c_str());
    x -= s(8) + bw;
    ImGui::SetCursorPos({x, by});
    if (button("Report issue##issue", 0, Tone::secondary, {bw, s(kControlH)})) share_report(g.id);
    ImGui::SetCursorPosY(y + row_h);
    ImGui::Dummy({0, 0});
    if (open) {
      gap(10);
      ImGui::PushStyleColor(ImGuiCol_ChildBg, color::alpha(color::bg, 0.85f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {s(14), s(10)});
      ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, s(8));
      ImGui::BeginChild("Log", {0, s(220)}, ImGuiChildFlags_AlwaysUseWindowPadding);
      text(fonts().regular, kSmall, color::text2, r->second.tail);
      if (ImGui::IsWindowAppearing()) ImGui::SetScrollHereY(1.0f);
      ImGui::EndChild();
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor();
    }
    end_card();
    gap(22);
  }

  // Tabs: About, and Add-ons when the game has any.
  bool checking_addons = addons_pending_.contains(g.id) || details_pending_.contains(g.id);
  // Add-ons belong to the game's own install, not a pinned version.
  auto addon_list = meta::version_binary(g.id).empty() ? addons(g.id) : std::vector<meta::Addon>{};
  bool has_addons = !addon_list.empty() || checking_addons;
  std::string addon_tab = addon_list.empty() ? std::string("Add-ons") : std::format("Add-ons  {}", addon_list.size());
  // About, Add-ons when the game has any, and once it is installed Controls and
  // its own Settings, which override Settings for it.
  const bool has_settings = partial != nullptr;
  std::vector<const char*> tab_items{"About"};
  if (has_addons) tab_items.push_back(addon_tab.c_str());
  if (has_settings) {
    tab_items.push_back("Controls");
    tab_items.push_back("Settings");
  }
  // --smoke-view game-controls* and game-settings* keep those tabs open for
  // screenshots (by name: the Add-ons tab can come and go while it is checked).
  const char* smoke_tab = options_.smoke_view.starts_with("game-controls")   ? "Controls"
                          : options_.smoke_view.starts_with("game-settings") ? "Settings"
                                                                             : nullptr;
  if (smoke_tab && has_settings) {
    static bool scrolled = false;
    if (!std::exchange(scrolled, true)) ImGui::SetScrollHereY(0.0f);
    for (int i = 0; i < int(tab_items.size()); ++i)
      if (std::string_view(tab_items[size_t(i)]) == smoke_tab) game_tab_ = i;
  }
  if (game_tab_ >= int(tab_items.size())) game_tab_ = 0;
  if (tab_items.size() > 1) {
    tabs("game-tabs", tab_items.data(), int(tab_items.size()), &game_tab_, avail);
    gap(20);
  } else {
    text(fonts().semibold, kTitle, color::text, "About", false);
    gap(8);
  }
  float body_w = std::min(ImGui::GetContentRegionAvail().x, avail);
  const std::string_view tab = tab_items[game_tab_];
  if (tab == "Controls")
    draw_game_controls(g, std::min(body_w, s(1180)));
  else if (tab == "Settings")
    draw_game_settings(g, std::min(body_w, s(kPageWidth)));
  else if (game_tab_ == 1 && has_addons)
    draw_addons(g, std::min(body_w, s(980)));
  else
    draw_about(g, body_w);
  gap(30);
  if (pad > 0) ImGui::Unindent(pad);
}

const App::ModsSummary& App::mods_summary(const std::string& package, const fs::path& install) {
  auto& m = mods_[package];
  const double now = ImGui::GetTime();
  if (m.checked >= 0 && now - m.checked < 2) return m;
  m = {.checked = now};
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(mods_dir(data_, package), ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const auto name = entry.path().filename();
    if (fs::is_regular_file(install / name, ec)) {
      ++m.active;
      m.bytes += entry.file_size(ec);
      m.name = paths::utf8(name);
    } else {
      ++m.unused;
    }
  }
  return m;
}

// The mods folder on the game page: its same-named files replace the installed
// expansion files at launch. A status shows what is in use or ignored.
void App::draw_mods(const fs::path& install, const std::string& package, float bw) {
  const auto& m = mods_summary(package, install);
  std::string status;
  if (m.active) status = std::format("{} in use", m.active);
  if (m.unused) status += std::format("{}{} unused", status.empty() ? "" : "  \xC2\xB7  ", m.unused);
  const ImU32 tone = m.unused ? color::warning : color::positive;
  const auto tip = (m.active == 1 ? "In use: " + m.name + "\n\n" : std::string()) +
                   "Files here replace the game's OBB files of the same name, for example main.<version>." + package +
                   ".obb. Files under any other name are unused. Delete a file to go back to the original.";
  float sw = status.empty() ? 0 : std::round(measure(fonts().regular, kSmall, status).x + s(14));
  setting_row("Mods", tip, sw + bw, s(38), [&] {
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (sw)
      draw_text(ImGui::GetWindowDrawList(), fonts().regular, kSmall,
                {p.x, text_y(fonts().regular, kSmall, p.y, s(38))}, tone, status);
    ImGui::SetCursorScreenPos({p.x + sw, p.y});
    if (button("Open folder##mods", icons::folder, Tone::secondary, {bw, s(38)}))
      action([&] {
        auto folder = mods_dir(data_, package);
        fs::create_directories(folder);
        open_folder(folder);
        mods_.erase(package);
      });
  });
}

// Description plus the store's "Additional details" list.
void App::draw_about(const meta::Game& g, float width) {
  const auto* local = installed(g.id);
  auto detail = details_.find(g.id);
  bool two = width >= s(960);
  float side_w = two ? std::min(s(380), width * 0.36f) : width;
  float main_w = two ? width - side_w - s(40) : width;
  ImGui::BeginGroup();
  description(g.description.empty() ? "A Quest game from your Meta library." : g.description, std::min(main_w, s(760)));
  ImGui::EndGroup();
  if (two) ImGui::SameLine(0, s(40));
  else gap(26);
  ImGui::BeginGroup();
  heading("Additional details");
  gap(10);
  std::vector<std::pair<std::string, std::string>> rows;
  if (!g.publisher.empty()) rows.emplace_back("Publisher", g.publisher);
  if (local) {
    rows.emplace_back("Installed version", local->plan.version.empty() ? "Unknown" : local->plan.version);
    rows.emplace_back("Size on disk", bytes(local->bytes));
  }
  if (detail != details_.end()) {
    if (!detail->second.version.empty()) rows.emplace_back("Latest version", detail->second.version);
    if (detail->second.size && !local) rows.emplace_back("Download size", bytes(detail->second.size));
  }
  auto owners = game_owners(g.id);
  if (!owners.empty()) {
    std::string names;
    for (auto& k : owners)
      if (auto* a = account(k)) names += (names.empty() ? "" : ", ") + account_name(*a);
    rows.emplace_back(owners.size() > 1 ? "Owned by" : "Owner", names);
  }
  rows.emplace_back("Platform", "Meta Quest");
  float key_w = std::min(s(150), side_w * 0.42f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float y = p.y;
  for (size_t i = 0; i < rows.size(); ++i) {
    float wrap = side_w - key_w;
    ImVec2 vs = measure(fonts().regular, kBody, rows[i].second, wrap);
    float h = std::round(vs.y + s(24));
    draw_text(dl, fonts().regular, kBody, {std::round(p.x), std::round(y + s(12))}, color::text2, rows[i].first);
    draw_text(dl, fonts().regular, kBody, {std::round(p.x + key_w), std::round(y + s(12))}, color::text, rows[i].second,
              wrap);
    y += h;
    if (i + 1 < rows.size()) hline(dl, p.x, p.x + side_w, y);
  }
  ImGui::Dummy({side_w, y - p.y});
  if (details_pending_.contains(g.id)) {
    gap(10);
    ImGui::BeginGroup();
    spinner(7, color::text3);
    ImGui::SameLine(0, s(8));
    text(fonts().regular, kSmall, color::text3, "Checking the latest release\xE2\x80\xA6", false);
    ImGui::EndGroup();
  } else if (auto e = detail_errors_.find(g.id); e != detail_errors_.end()) {
    gap(10);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + side_w);
    text(fonts().regular, kSmall, color::warning, e->second);
    ImGui::PopTextWrapPos();
    gap(6);
    if (button("Try again##details", icons::refresh, Tone::secondary, {0, s(34)})) select(g.id);
  }
  ImGui::EndGroup();
}

// "Playing as" / "Owned by" pill. With several owning accounts it opens a
// picker; the choice applies to this game only.
void App::draw_owner(const meta::Game& g, bool installed, float w) {
  auto owners = game_owners(g.id);
  std::string key = owner_for(g.id);
  const AccountView* a = account(key);
  if (!a) return;
  bool pick = owners.size() > 1;
  std::string lead = installed ? "Playing as" : pick ? "Installing with" : "Owned by";
  std::string name = account_name(*a);
  ImFont* rf = fonts().regular;
  ImFont* sf = fonts().semibold;
  float r = s(12), h = s(38);
  bool chev = pick && fonts().icons;
  ImVec2 p{};
  bool clicked = false;
  ImGui::PushID("owner");
  setting_row(lead, !installed && key != account_key_ ? "Installing switches to this account." : "", w, h, [&] {
    p = ImGui::GetCursorScreenPos();
    bool hovered = false;
    if (pick) {
      clicked = ImGui::InvisibleButton("##owner", {w, h});
      hovered = ImGui::IsItemHovered();
      if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    } else {
      ImGui::Dummy({w, h});
    }
    float t = anim(ImGui::GetID("hover"), hovered ? 1.f : 0.f, 18);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, {p.x + w, p.y + h}, color::mix(color::surface_hi, color::surface_top, t), h * 0.5f);
    // Avatar and name centred together, like a button's glyph and label.
    const float chev_w = chev ? s(18) : 0;
    const auto shown = ellipsize(sf, kSmall, name, w - r * 2 - s(8) - chev_w - s(24));
    const float content = r * 2 + s(8) + measure(sf, kSmall, shown).x + chev_w;
    float x = std::round(p.x + (w - content) * 0.5f);
    avatar(dl, {x + r, p.y + h * 0.5f}, r, a, false, kLabel);
    x += r * 2 + s(8);
    draw_text(dl, sf, kSmall, {x, text_y(sf, kSmall, p.y, h)}, color::text, shown);
    if (chev)
      draw_glyph(dl, icons::chevron_down, kLabel, {p.x + (w + content) * 0.5f - s(6), p.y + h * 0.5f}, color::text2);
  });
  if (clicked) ImGui::OpenPopup("owner-picker");
  place_menu(p, h, int(owners.size()) * s(52) + s(12));
  if (begin_menu("owner-picker")) {
    for (const auto& k : owners) {
      const AccountView* o = account(k);
      if (!o) continue;
      ImGui::PushID(k.c_str());
      ImVec2 q = ImGui::GetCursorScreenPos();
      float rw = s(300), rh = s(50);
      bool usable = o->signed_in && !o->needs_reauth;
      bool chosen = ImGui::InvisibleButton("##pick", {rw, rh});
      bool hov = ImGui::IsItemHovered();
      if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      ImDrawList* pdl = ImGui::GetWindowDrawList();
      if (hov) pdl->AddRectFilled(q, {q.x + rw, q.y + rh}, color::surface_top, s(8));
      avatar(pdl, {q.x + s(24), q.y + rh * 0.5f}, s(15), o, false, kLabel);
      float tx = q.x + s(50);
      draw_text(pdl, sf, kSmall + 1, {std::round(tx), text_y(sf, kSmall + 1, q.y + rh * 0.5f - s(9))}, color::text,
                ellipsize(sf, kSmall + 1, account_name(*o), rw - s(50) - s(34)));
      std::string state = !usable ? "Reconnect to play" : k == account_key_ ? "Active account" : "Signed in";
      draw_text(pdl, rf, kLabel + 0.5f, {std::round(tx), text_y(rf, kLabel + 0.5f, q.y + rh * 0.5f + s(9))},
                usable ? color::text3 : color::warning, state);
      if (k == key) draw_glyph(pdl, icons::check, kSmall, {q.x + rw - s(20), q.y + rh * 0.5f}, color::accent_text);
      if (chosen) {
        owner_pick_[g.id] = k;
        ImGui::CloseCurrentPopup();
      }
      ImGui::PopID();
    }
    end_menu();
  }
  ImGui::PopID();
}

// Add-ons, judged by the account that owns the game. Owned ones come first;
// the unowned catalog is collapsed because some games list dozens of SKUs.
void App::draw_addons(const meta::Game& g, float width) {
  auto list = addons(g.id);
  bool checking = addons_pending_.contains(g.id) || details_pending_.contains(g.id);
  const auto* local = installed(g.id);
  bool running = running_.contains(g.id) && running_[g.id].process.active();
  auto owners = game_owners(g.id);
  bool active_owns = std::ranges::find(owners, account_key_) != owners.end();
  std::ranges::sort(list, [](const meta::Addon& a, const meta::Addon& b) {
    if (a.owned != b.owned) return a.owned;
    return lowercase(a.name) < lowercase(b.name);
  });
  size_t owned = std::ranges::count(list, true, &meta::Addon::owned);
  size_t to_install = std::ranges::count_if(list, [](auto& a) { return a.owned && !a.installed && !a.downloading; });
  ImFont* rf = fonts().regular;
  ImFont* sf = fonts().semibold;

  // Summary line with refresh and bulk install on the right, all on the
  // 36 px control line.
  float top = ImGui::GetCursorPosY();
  float left_x = ImGui::GetCursorPosX();
  std::string summary = list.empty() ? "Checking what's available\xE2\x80\xA6"
                        : owned      ? std::format("{} owned  \xC2\xB7  {} available", owned, list.size())
                                     : std::format("{} available  \xC2\xB7  none owned by this account", list.size());
  {
    ImVec2 sp = ImGui::GetCursorScreenPos();
    draw_text(ImGui::GetWindowDrawList(), rf, kBody, {std::round(sp.x), text_y(rf, kBody, sp.y, s(36))}, color::text2,
              summary);
  }
  float right = left_x + width;
  if (checking) {
    ImGui::SetCursorPos({right - s(26), top + s(10)});
    spinner(8, color::text3);
  } else {
    ImGui::SetCursorPos({right - s(36), top});
    if (icon_button("refresh-addons", icons::refresh, nullptr, 36, session_ && !options_.offline, true)) {
      if (!details_.contains(g.id)) {
        select(g.id);
      } else {
        addons_.erase(g.id);
        refresh_addons(g.id);
      }
    }
    if (to_install > 1 && local && active_owns) {
      std::string bulk = std::format("Install all owned ({})", to_install);
      float bw = button_width(bulk, icons::download);
      ImGui::SetCursorPos({right - s(36) - s(10) - bw, top});
      if (button((bulk + "##addons").c_str(), icons::download, Tone::secondary, {bw, s(36)},
                 session_ && !options_.offline))
        action([&] { install_all_owned_addons(g.id); });
    }
  }
  ImGui::SetCursorPos({left_x, top + s(48)});

  if (!active_owns && !owners.empty()) {
    begin_card("addons-owner", width);
    text(rf, kBody, color::text2, "Add-ons are checked against the account that owns this game.");
    if (const AccountView* o = account(owner_for(g.id))) {
      gap(12);
      std::string label_text = "Switch to " + account_name(*o) + "##addons-switch";
      if (button(label_text.c_str(), icons::swap, Tone::secondary, {0, s(38)}, o->signed_in && !o->needs_reauth))
        action([&, key = o->key] { switch_account(key); });
    }
    end_card();
    return;
  }
  if (list.empty()) return;
  std::string app_error;
  if (auto e = addon_errors_.find(g.id); e != addon_errors_.end()) {
    app_error = e->second;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    text(rf, kSmall, color::warning, app_error);
    ImGui::PopTextWrapPos();
    gap(10);
  }

  begin_card("addons", width, {20, 6});
  auto row = [&](const meta::Addon& a, bool last) {
    ImGui::PushID(a.sku.c_str());
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    bool own_error = !a.error.empty() && a.error != app_error;
    float h = s(own_error ? 82 : 64);
    float ctrl_w = s(200);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##row", {std::max(s(40), w - ctrl_w), h});
    float cy = std::round(p.y + s(32));
    draw_text(dl, sf, kBody, {std::round(p.x), text_y(sf, kBody, cy - s(10))}, a.owned ? color::text : color::text2,
              ellipsize(sf, kBody, a.name, w - ctrl_w - s(16)));
    std::string sub;
    if (a.downloading)
      sub = a.total ? std::format("Downloading  \xC2\xB7  {} of {}", bytes(a.done), bytes(a.total)) : "Queued";
    else if (a.installed)
      sub = a.size ? "Installed  \xC2\xB7  " + bytes(a.size) : "Installed";
    else if (a.owned)
      sub = a.size ? "Owned  \xC2\xB7  " + bytes(a.size) : "Owned";
    else
      sub = a.price_text.empty() ? "Available in the Meta store" : a.price_text + "  \xC2\xB7  Meta store";
    draw_text(dl, rf, kSmall, {std::round(p.x), text_y(rf, kSmall, cy + s(10))}, color::text3, sub);
    if (own_error)
      draw_text(dl, rf, kSmall, {std::round(p.x), text_y(rf, kSmall, cy + s(30))}, color::warning,
                ellipsize(rf, kSmall, a.error, w - ctrl_w - s(16)));

    float cx = p.x + w;
    if (a.installed) {
      ImGui::SetCursorScreenPos({cx - s(36), cy - s(18)});
      if (icon_button("remove", icons::trash, running ? "Close the game to remove add-ons" : nullptr, 36,
                      !running && !downloads_->busy()))
        action([&] { uninstall_addon(g.id, a.sku); });
      float bw = badge_width("Installed", icons::check);
      ImGui::SetCursorScreenPos({cx - s(36) - s(10) - bw, cy - s(11)});
      badge("Installed", color::text, color::surface_top, icons::check);
    } else if (a.downloading) {
      float bw = s(150);
      ImGui::SetCursorScreenPos({cx - bw, cy - s(2)});
      progress_bar(a.total ? float(double(a.done) / double(a.total)) : 0.f, {bw, s(4)}, !a.total);
    } else if (a.owned) {
      float bw = button_width("Install", icons::download);
      ImGui::SetCursorScreenPos({cx - bw, cy - s(18)});
      bool can = local && session_ && !options_.offline;
      if (button("Install##addon", icons::download, Tone::secondary, {bw, s(36)}, can))
        action([&] { install_addon(g.id, a.sku); });
      if (!local && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) tooltip("Install the game first");
    } else {
      float bw = badge_width("Not owned");
      ImGui::SetCursorScreenPos({cx - bw, cy - s(11)});
      badge("Not owned", color::text3, color::surface_hi);
    }
    if (!last) hline(dl, p.x, p.x + w, p.y + h);
    ImGui::SetCursorScreenPos({p.x, p.y + h + s(1)});
    ImGui::Dummy({w, 0});
    ImGui::PopID();
  };

  size_t unowned = list.size() - owned;
  size_t preview = owned ? 0 : 3;
  bool expanded = addons_expanded_.contains(g.id);
  size_t shown = owned + (expanded ? unowned : std::min(unowned, preview));
  bool toggle_row = unowned > preview;
  for (size_t i = 0; i < shown; ++i) row(list[i], i + 1 == shown && !toggle_row);
  if (toggle_row) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = s(48);
    if (ImGui::InvisibleButton("##more-addons", {w, h})) {
      if (expanded) addons_expanded_.erase(g.id);
      else addons_expanded_.insert(g.id);
    }
    bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    size_t hidden = unowned - std::min(unowned, preview);
    std::string more = expanded ? "Show fewer"
                                : std::format("Show {} more add-on{} you don't own", hidden, hidden == 1 ? "" : "s");
    ImVec2 ms = measure(sf, kSmall + 1, more);
    ImU32 col = hov ? color::text : color::accent_text;
    draw_text(dl, sf, kSmall + 1, {std::round(p.x), text_y(sf, kSmall + 1, p.y, h)}, col, more);
    draw_glyph(dl, expanded ? icons::chevron_up : icons::chevron_down, kLabel, {p.x + ms.x + s(14), p.y + h * 0.5f}, col);
  }
  end_card();
}

// ---------------------------------------------------------------------------
void App::draw_downloads() {
  page_title("Downloads");
  gap(kAfterHeader);
  auto jobs = downloads_->snapshot();
  if (jobs.empty()) {
    // Centred in the page below the title; the block's height comes from the last frame.
    auto* storage = ImGui::GetStateStorage();
    const ImGuiID id = ImGui::GetID("empty-downloads");
    const float block = storage->GetFloat(id, s(260)), top = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(top + std::max(0.f, (ImGui::GetContentRegionAvail().y - block) * 0.5f - s(kHeaderHeight)));
    const float start = ImGui::GetCursorPosY();
    empty_state(icons::download, "No downloads", "");
    gap(22);
    float bw = row_button_w("Go to library");
    ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - bw) * 0.5f + ImGui::GetCursorPosX());
    if (button("Go to library", 0, Tone::secondary, {bw, s(kControlH)})) page_ = 0;
    storage->SetFloat(id, ImGui::GetCursorPosY() - start);
    return;
  }
  float w = std::min(ImGui::GetContentRegionAvail().x, s(kPageWidth));
  begin_card("downloads", w, {20, 6});
  for (size_t index = 0; index < jobs.size(); ++index) {
    auto& j = jobs[index];
    ImGui::PushID(int(index));
    ImVec2 p = ImGui::GetCursorScreenPos();
    float row_w = ImGui::GetContentRegionAvail().x;
    float h = s(j.error.empty() ? 96 : 116);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float thumb_h = std::round(s(63)), thumb_w = std::round(s(112));
    ImGui::SetCursorScreenPos({p.x, std::round(p.y + (h - thumb_h) * 0.5f)});
    art(j.app_id, {thumb_w, thumb_h}, s(8));
    float x = std::round(p.x + thumb_w + s(18));
    // Title, state and progress stack centered on the thumbnail.
    float mid = std::round(p.y + h * 0.5f) - (j.error.empty() ? 0 : s(10));
    float right = p.x + row_w;
    std::string title = j.asset_name.empty() ? j.name : j.name + "  \xC2\xB7  " + j.asset_name;
    draw_text(dl, fonts().semibold, kBody, {x, text_y(fonts().semibold, kBody, mid - s(18))}, color::text,
              ellipsize(fonts().semibold, kBody, title, right - x - s(170)));
    bool active = j.state == download::State::queued || j.state == download::State::checking ||
                  j.state == download::State::downloading;
    float fraction = j.total ? float(double(j.done) / double(j.total)) : 0.f;
    std::string state = j.importing && j.state == download::State::downloading ? "Importing" : download::label(j.state);
    if (j.state == download::State::downloading && j.total) state += std::format("  \xC2\xB7  {:.0f}%", fraction * 100);
    std::string detail = bytes(j.done) + " of " + (j.total ? bytes(j.total) : "\xE2\x80\xA6");
    if (j.bytes_per_second > 0 && j.total > j.done && active) {
      auto seconds = uint64_t(double(j.total - j.done) / j.bytes_per_second);
      detail += std::format("  \xC2\xB7  {}/s  \xC2\xB7  {}", bytes(uint64_t(j.bytes_per_second)), time_left(seconds));
    }
    ImU32 state_col = j.state == download::State::complete ? color::text2
                      : !j.error.empty()                   ? color::warning
                                                           : color::text2;
    draw_text(dl, fonts().regular, kSmall, {x, text_y(fonts().regular, kSmall, mid + s(2))}, state_col,
              j.state == download::State::complete ? state : state + "  \xC2\xB7  " + detail);
    if (j.state != download::State::complete) {
      ImGui::SetCursorScreenPos({x, mid + s(18)});
      progress_bar(fraction, {right - x - s(170), s(4)}, active && !j.total);
    }
    if (!j.error.empty())
      draw_text(dl, fonts().regular, kSmall, {x, text_y(fonts().regular, kSmall, mid + s(36))}, color::warning,
                ellipsize(fonts().regular, kSmall, j.error, right - x - s(170)));
    // Queue order, pause or resume, and cancel, right to left.
    float by = p.y + (h - s(36)) * 0.5f;
    ImGui::SetCursorScreenPos({right - s(36), by});
    if (j.state == download::State::complete) {
      if (icon_button("clear", icons::cancel, nullptr, 36))
        action([&] { downloads_->remove(j, false); });
      draw_glyph(dl, icons::check, kBody + 2, {right - s(58), p.y + h * 0.5f}, color::positive);
    } else {
      if (icon_button("cancel", icons::cancel, nullptr, 36)) cancel_target_ = j;
      ImGui::SetCursorScreenPos({right - s(76), by});
      if (active) {
        if (icon_button("pause", icons::pause, nullptr, 36, true, true))
          downloads_->cancel(j.app_id);
      } else if (icon_button("resume", icons::resume, nullptr, 36, session_ && !options_.offline, true)) {
        action([&] { downloads_->resume(j, session_->access_token); });
      }
      ImGui::SetCursorScreenPos({right - s(116), by});
      if (icon_button("later", icons::chevron_down, nullptr, 36, index + 1 < jobs.size()))
        downloads_->move(j, 1);
      ImGui::SetCursorScreenPos({right - s(156), by});
      if (icon_button("sooner", icons::chevron_up, nullptr, 36, index > 0)) downloads_->move(j, -1);
    }
    if (index + 1 < jobs.size()) hline(dl, p.x, right, p.y + h);
    ImGui::SetCursorScreenPos({p.x, p.y + h + s(1)});
    ImGui::Dummy({row_w, 0});
    ImGui::PopID();
  }
  end_card();
}

// ---------------------------------------------------------------------------
// Changes apply as they are made; text fields save when editing ends.
void App::draw_settings() {
  page_title(tr("Settings"));
  gap(kAfterHeader);
  float w = std::min(ImGui::GetContentRegionAvail().x, s(kPageWidth));
  auto input = [&](const char* id, std::string* value, const char* hint) {
    return [=, this] {
      if (text_field(id, hint, value, std::min(s(380), w * 0.48f)).committed) action([&] { save_settings(); });
    };
  };
  float field_w = std::min(s(380), w * 0.48f);

  heading(tr("Appearance"));
  begin_card("appearance", w, {22, 2});
  {
    static const char* sizes[] = {"100%", "115%", "130%", "150%"};
    static constexpr int percents[] = {100, 115, 130, 150};
    int choice = int(std::ranges::find(percents, settings_.interface_size) - std::begin(percents));
    if (choice == 4) choice = 0;
    float dw = s(180);
    setting_row(tr("Interface size"), "Makes text and everything else in Venture larger.", dw, s(38), [&] {
      if (dropdown("interface-size", "", sizes, 4, &choice, dw))
        action([this, size = percents[choice]] {
          settings_.interface_size = size;
          save_settings();
          fit_window();
          theme();
        });
    });
  }
  divider();
  draw_language_row();
  ImGui::Dummy({0, 0});  // the last row keeps its bottom padding
  end_card();
  gap(20);

  heading(tr("Library"));
  begin_card("library", w, {22, 2});
  setting_row("Games folder", "New downloads go here. Games already installed stay where they are.", field_w,
              s(38), input("##games", &settings_games_, ""));
  divider();
  setting_row("Android platform-tools (adb)",
              "Used to import games from a headset. Leave empty to find adb automatically.", field_w, s(38),
              input("##adb", &settings_adb_, "Found automatically"));
  divider();
  setting_row("Add games to Steam", "Installed games appear in Steam and SteamVR after Steam restarts.", s(44), s(24),
              [&] {
                if (toggle("steam", &settings_.add_games_to_steam)) action([&] { save_settings(); });
              });
  if (settings_.add_games_to_steam) {
    divider();
    setting_row("Add new games to Steam", "On by default. Each game's menu can add or remove it later.", s(44), s(24),
                [&] {
                  if (toggle("steam-new", &settings_.steam_add_new)) action([&] { save_settings(); });
                });
  }
  divider();
  {
    const char* counts[] = {"One at a time", "2", "3", "4"};
    int choice = std::clamp(settings_.parallel_downloads, 1, 4) - 1;
    float dw = s(180);
    setting_row("Downloads at once",
                "More games download side by side on a fast connection. A game's own files and add-ons still "
                "download one after another.",
                dw, s(38), [&] {
      if (dropdown("parallel-downloads", "", counts, 4, &choice, dw))
        action([this, jobs = choice + 1] {
          settings_.parallel_downloads = jobs;
          save_settings();
        });
    });
  }
  divider();
  setting_row("Ask when installing", "Installing a new game asks which drive it goes to and whether it goes to Steam.",
              s(44), s(24), [&] {
                if (toggle("install-ask", &settings_.install_ask)) action([&] { save_settings(); });
              });
  divider();
  // The info button's tooltip is the row's one-line description.
  setting_row(tr("Look up imported games online"),
              "Finds the store page of an imported game no account owns on OculusDB. Only its package name is sent.",
              s(44), s(24), [&] {
                if (toggle("lookup-imports", &settings_.lookup_imports_online))
                  action([&] {
                    // Turned on: imports that found nothing are looked up again.
                    if (settings_.lookup_imports_online) {
                      store_link_tried_.clear();
                      std::error_code ec;
                      fs::remove(data_ / "cache" / "store-links.json", ec);
                    }
                    save_settings();
                  });
              }, [&] {
                icon_button("lookup-imports-help", icons::info,
                            tr("Finds an imported game's store page on OculusDB. Only its package name is sent."),
                            kHelpIcon);
              });
  if (steam_pending_ && settings_.add_games_to_steam) {
    divider();
    float bw = row_button_w(steam_restart_ ? "Waiting for Steam to close" : "Restart Steam");
    setting_row(std::format("{} games are waiting for Steam", steam_games_), "Steam adds them the next time it starts.", bw,
                s(38), [&] {
                  if (button(steam_restart_ ? "Waiting for Steam to close" : "Restart Steam", 0, Tone::secondary,
                             {bw, s(38)}, !steam_restart_))
                    restart_steam();
                });
  }
  ImGui::Dummy({0, 0});  // the last row keeps its bottom padding
  end_card();
  gap(20);

  // --smoke-view display* scrolls the Display card into view for screenshots.
  if (options_.smoke_view.starts_with("display")) ImGui::SetScrollHereY(0.0f);
  heading(tr("Display"));
  begin_card("display", w, {22, 2});
  draw_display_rows(settings_.display, false, "global");
  ImGui::Dummy({0, 0});  // the last row keeps its bottom padding
  end_card();
  gap(20);

  heading(tr("Runtime"));
  // --smoke-view settings-runtime(-texture-warning) shows the Runtime card at the top for screenshots.
  if (options_.smoke_view.starts_with("settings-runtime")) ImGui::SetScrollHereY(0.0f);
  begin_card("runtime", w, {22, 2});
  setting_row("Runtime", "Leave empty to use the runtime that came with Venture.", field_w, s(38),
              input("##runtime", &settings_runtime_, "Included runtime"));
  divider();
  {
    // Automatic picks what the VR runtime or headset offers; Color key is the
    // user's choice for a streaming tool that keys a color (never automatic).
    const char* modes[] = {"Automatic", "Color key", "Off"};
    int mode = !settings_.passthrough ? 2 : settings_.color_key ? 1 : 0;
    float dw = s(180);
    setting_row("Passthrough (mixed reality)", "", dw, s(38), [&] {
      if (dropdown("passthrough", "", modes, 3, &mode, dw))
        action([this, mode] {
          settings_.passthrough = mode != 2;
          settings_.color_key = mode == 1;
          save_settings();
        });
    }, [&] {
      if (icon_button("passthrough-help", icons::info, "Mixed reality setups (opens the Venture wiki)", kHelpIcon))
        action([] { open_url("https://github.com/Villagers654/Venture/wiki/Mixed-Reality"); });
    });
    if (settings_.passthrough && settings_.color_key) {
      divider();
      // The common defaults of keying tools, magenta first (rare in games).
      static const char* names[] = {"Magenta", "Green", "Blue", "Black", "Custom"};
      static const char* hexes[] = {"FF00FF", "00FF00", "0000FF", "000000"};
      const auto upper = [](std::string text) {
        for (auto& c : text) c = char(std::toupper(static_cast<unsigned char>(c)));
        return text;
      };
      int preset = 4;
      for (int i = 0; i < 4; ++i)
        if (upper(settings_.key_color) == hexes[i]) preset = i;
      if (key_custom_) preset = 4;
      const bool custom = preset == 4;
      const float sw = s(38), hw = s(120), gapw = s(10);
      const float cw = sw + gapw + (custom ? hw + gapw : 0) + dw;
      setting_row("Key color", "", cw, s(38), [&] {
        const auto rgb = environment::parse_color_key(settings_.key_color).value_or(std::array<std::uint8_t, 3>{255, 0, 255});
        const ImVec2 at = ImGui::GetCursorScreenPos();
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(at, {at.x + sw, at.y + sw}, IM_COL32(rgb[0], rgb[1], rgb[2], 255), s(10));
        draw->AddRect(at, {at.x + sw, at.y + sw}, color::stroke_hi, s(10));
        ImGui::Dummy({sw, sw});
        ImGui::SameLine(0, gapw);
        if (custom) {
          if (text_field("##key-hex", "RRGGBB", &settings_key_hex_, hw, ImGuiInputTextFlags_CharsHexadecimal)
                  .committed) {
            if (environment::parse_color_key(settings_key_hex_))
              action([this, hex = upper(settings_key_hex_)] { settings_.key_color = hex; save_settings(); });
            else
              settings_key_hex_ = settings_.key_color;
          }
          ImGui::SameLine(0, gapw);
        }
        int choice = preset;
        if (dropdown("key-color", "", names, 5, &choice, dw))
          action([this, choice] {
            key_custom_ = choice == 4;
            if (choice < 4) settings_.key_color = hexes[choice];
            settings_key_hex_ = settings_.key_color;
            save_settings();
          });
      });
    }
  }
  divider();
  setting_row("Show runtime console", "Opens a console window with live output when a game starts.", s(44), s(24), [&] {
    if (toggle("console", &settings_.console)) action([&] { save_settings(); });
  });
  divider();
  {
    static const char* names[] = {"System language", "English (US)", "English (UK)", "German", "French",
                                  "Spanish", "Italian", "Portuguese (Brazil)", "Dutch", "Polish", "Russian",
                                  "Swedish", "Turkish", "Japanese", "Korean", "Chinese (Simplified)",
                                  "Chinese (Traditional)"};
    static const char* tags[] = {"", "en-US", "en-GB", "de-DE", "fr-FR", "es-ES", "it-IT", "pt-BR", "nl-NL",
                                 "pl-PL", "ru-RU", "sv-SE", "tr-TR", "ja-JP", "ko-KR", "zh-Hans-CN", "zh-Hant-TW"};
    constexpr int count = int(std::size(tags));
    int choice = int(std::ranges::find(tags, settings_.game_language) - std::begin(tags));
    if (choice == count) choice = 0;
    float dw = s(180);
    setting_row("Game language",
                "The language games see as the system language. Most games also have their own language "
                "option; this one is for games that only follow the system.",
                dw, s(38), [&] {
      if (dropdown("language", "", names, count, &choice, dw)) {
        settings_.game_language = tags[choice];
        action([&] { save_settings(); });
      }
    });
  }
  divider();
  {
    // i18n: Choices for "Compress textures".
    const char* modes[] = {tr("Always"), tr("Automatic"), tr("Off")};
    float dw = s(180);
    // i18n: Settings row: store game textures compressed, in less GPU memory.
    setting_row(tr("Compress textures"),
                "Stores game textures at a quarter of the GPU memory, with a small loss of detail. Automatic: "
                "only once the GPU memory in use passes a threshold.",
                dw, s(38), [&] {
      int mode = settings_.texture_compression;
      if (dropdown("textures", "", modes, 3, &mode, dw)) {
        auto apply = [this, mode] {
          settings_.texture_compression = mode;
          save_settings();
        };
        // Down from Always asks first.
        if (settings_.texture_compression_warns(library::texture_compression_modes[settings_.texture_compression],
                                                library::texture_compression_modes[mode]))
          texture_warning_ = apply;
        else action(apply);
      }
    });
  }
  if (settings_.texture_compression == 1) {
    divider();
    static const char* shares[] = {"25%", "50%", "75%", "90%"};
    static constexpr int percents[] = {25, 50, 75, 90};
    int choice = int(std::ranges::find(percents, settings_.texture_compression_threshold) - std::begin(percents));
    if (choice == 4) choice = 1;
    float dw = s(180);
    // i18n: Settings row: GPU memory in use before new textures are compressed.
    setting_row(tr("Compression threshold"),
                "GPU memory in use before new textures are compressed. Lower leaves room for larger games.", dw,
                s(38), [&] {
      if (dropdown("texture-threshold", "", shares, 4, &choice, dw))
        action([this, percent = percents[choice]] {
          settings_.texture_compression_threshold = percent;
          save_settings();
        });
    });
  }
  divider();
  setting_row("Keep translated code",
              "Saves the game code Venture translates, so later launches start sooner and load new areas without "
              "stutter. Uses up to about 1 GB of disk per game.",
              s(44), s(24), [&] {
    if (toggle("translated-code", &settings_.keep_translated_code)) action([&] { save_settings(); });
  });
  divider();
  setting_row("Check for updates", "Venture tells you when a new version is out.", s(44), s(24), [&] {
    if (toggle("updates", &settings_.check_updates)) action([&] { save_settings(); });
  });
  ImGui::Dummy({0, 0});  // the last row keeps its bottom padding
  end_card();
  gap(20);

  heading(tr("Troubleshooting"));
  begin_card("diagnostics", w, {22, 2});
  {
    const char* levels[] = {"Trace", "Debug", "Info", "Warnings", "Errors"};
    float dw = s(180);
    setting_row("Log detail", "More detail helps when reporting a problem.", dw, s(38), [&] {
      if (dropdown("log", "", levels, 5, &settings_.log_level, dw)) action([&] { save_settings(); });
    });
  }
  divider();
  {
    float bw = row_button_w("Open logs", icons::folder);
    setting_row("Logs", "Launcher and game logs are kept on this PC.", bw, s(38), [&] {
      if (button("Open logs", icons::folder, Tone::secondary, {bw, s(38)}))
        action([&] { open_folder(data_ / "logs"); });
    });
  }
  divider();
  {
    float bw = row_button_w("Upload debug report");
    setting_row("Debug report",
                "Uploads the last game launch's logs and this PC's setup and gives you a link to put in a bug "
                "report. Your name, folders and account details are left out.",
                bw, s(38), [&] {
                  if (button("Upload debug report", 0, Tone::secondary, {bw, s(38)})) share_report({});
                });
  }
  ImGui::Dummy({0, 0});  // the last row keeps its bottom padding
  end_card();
  gap(20);

  heading(tr("About"));
  begin_card("about", w, {22, 18});
  {
    ImVec2 p = ImGui::GetCursorScreenPos();
    p = {std::round(p.x), std::round(p.y)};
    float m = std::round(s(44));
    ImDrawList* adl = ImGui::GetWindowDrawList();
    if (SDL_Texture* mark = logo(int(m)))
      draw_image(adl, ImTextureID(reinterpret_cast<uintptr_t>(mark)), p, {p.x + m, p.y + m});
    float tx = std::round(p.x + m + s(16)), cy = p.y + m * 0.5f;
    draw_text(adl, fonts().semibold, kBody, {tx, text_y(fonts().semibold, kBody, cy - s(10))}, color::text, "Venture");
    draw_text(adl, fonts().regular, kSmall, {tx, text_y(fonts().regular, kSmall, cy + s(10))}, color::text2,
              "Version " VENTURE_VERSION);
    float bw = row_button_w("Join our Discord", icons::people);
    ImGui::Dummy({m, m});
    ImGui::SetCursorScreenPos({p.x + ImGui::GetContentRegionAvail().x - bw, cy - s(19)});
    if (button("Join our Discord##discord", icons::people, Tone::secondary, {bw, s(38)}, true,
               ImTextureID(reinterpret_cast<uintptr_t>(discord_logo(int(std::round(s(16))))))))
      action([] { open_url(kDiscord); });
    ImGui::SetCursorScreenPos({p.x, p.y + m});
    ImGui::Dummy({0, 0});
  }
  end_card();
  gap(30);
  // --smoke-view settings-end shows the bottom for screenshots.
  if (options_.smoke_view == "settings-end") ImGui::SetScrollHereY(1.0f);
}

// Settings > Appearance > Language: Automatic (the system's language), English
// and each compiled-in translation by its own name. Applies at once.
void App::draw_language_row() {
  std::vector<std::string_view> codes{"", "en"};
  std::vector<const char*> names{tr("Automatic"), "English"};
  for (const auto& language : i18n::languages()) {
    codes.push_back(language.code);
    names.push_back(language.name.data());
  }
  int choice = int(std::ranges::find(codes, settings_.interface_language) - codes.begin());
  if (choice == int(codes.size())) choice = 0;
  float dw = s(180);
  setting_row(tr("Language"), {}, dw, s(38), [&] {
    if (dropdown("interface-language", "", names.data(), int(names.size()), &choice, dw))
      action([this, code = std::string(codes[choice])] {
        settings_.interface_language = code;
        i18n::set_language(code);
        save_settings();
      });
  });
}

// ---------------------------------------------------------------------------
// Display overrides: resolution and frame rate, in Settings for every game and
// in each game's Settings tab, where "Use global setting" follows Settings.
namespace {
// Display settings (library/display.h): the dropdowns list the choices in this
// order, a game's after "Use global setting".
std::vector<std::string> resolution_ids() {
  std::vector<std::string> ids{"runtime"};
  for (const auto& p : library::headset_presets) ids.emplace_back(p.id);
  ids.emplace_back("percent");
  ids.emplace_back("custom");
  return ids;
}
std::vector<std::string> resolution_names() {
  std::vector<std::string> names{"Runtime default"};
  for (const auto& p : library::headset_presets) names.emplace_back(p.name);
  names.emplace_back("Percent of runtime");
  names.emplace_back("Custom size");
  return names;
}
std::vector<std::string> rate_ids() {
  std::vector<std::string> ids{"runtime"};
  for (int hz : library::frame_rates) ids.push_back(std::to_string(hz));
  ids.emplace_back("custom");
  return ids;
}
std::vector<std::string> rate_names() {
  std::vector<std::string> names{"Runtime default"};
  for (int hz : library::frame_rates) names.push_back(std::format("{} Hz", hz));
  names.emplace_back("Custom");
  return names;
}
// A whole-number field drawn as a pill like the dropdowns, its value centered
// in both directions; `suffix` (such as "Hz") follows the value inside the
// pill. Returns true when an edit within [low, high] is committed to `value`.
bool number_field(const char* id, int* value, int low, int high, float width, std::string_view suffix = {}) {
  static std::unordered_map<ImGuiID, std::string> buffers;
  const ImGuiID key = ImGui::GetID(id);
  auto& text = buffers[key];
  if (ImGui::GetActiveID() != key) text = *value ? std::to_string(*value) : std::string();
  ImFont* font = fonts().semibold;
  constexpr float size = kSmall + 1;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = s(38);
  const bool hot = ImGui::IsMouseHoveringRect(p, {p.x + width, p.y + h}) || ImGui::GetActiveID() == key;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, {p.x + width, p.y + h}, color::mix(color::surface_hi, color::surface_top, hot ? 1.f : 0.f),
                    h * 0.5f);
  const float tw = measure(font, size, text).x;
  const float sw = suffix.empty() ? 0 : measure(fonts().regular, size, suffix).x + s(5);
  const float left = std::max(s(8), (width - tw - sw) * 0.5f);
  if (!suffix.empty())
    draw_text(dl, fonts().regular, size, {std::round(p.x + left + tw + s(5)), p.y + text_y(fonts().regular, size, 0, h)},
              color::text2, suffix);
  ImGui::PushFont(font, size);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {left, text_y(font, size, 0, h)});
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
  ImGui::SetNextItemWidth(width - sw);
  ImGui::InputText(id, &text, ImGuiInputTextFlags_CharsDecimal);
  const bool committed = ImGui::IsItemDeactivatedAfterEdit();
  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar();
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy({width, h});
  if (!committed) return false;
  int parsed = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (ec != std::errc{} || end != text.data() + text.size() || parsed < low || parsed > high) return false;
  *value = parsed;
  return true;
}
}  // namespace

const library::HostDisplay* App::host_display() {
  // The runtime rewrites the file while a game starts; a glance every 2 s is plenty.
  const double now = ImGui::GetTime();
  if (now - host_display_checked_ >= 2) {
    host_display_checked_ = now;
    std::error_code ec;
    const auto file = data_ / "host-display.json";
    const auto time = fs::last_write_time(file, ec);
    if (ec) host_display_.reset();
    else if (time != host_display_time_ || !host_display_) host_display_ = library::load_host_display(data_);
    host_display_time_ = ec ? fs::file_time_type{} : time;
  }
  return host_display_ ? &*host_display_ : nullptr;
}

void App::draw_display_rows(library::DisplaySettings& d, bool per_game, const char* id) {
  ImGui::PushID(id);
  const float dw = s(kControlW);
  const auto* host = host_display();
  // A game's dropdowns lead with "Use global setting" (an empty choice).
  auto res_ids = resolution_ids(), res_names = resolution_names(), hz_ids = rate_ids(), hz_names = rate_names();
  if (per_game) {
    res_ids.insert(res_ids.begin(), "");
    res_names.insert(res_names.begin(), "Use global setting");
    hz_ids.insert(hz_ids.begin(), "");
    hz_names.insert(hz_names.begin(), "Use global setting");
  }
  auto pointers = [](const std::vector<std::string>& v) {
    std::vector<const char*> out;
    for (const auto& item : v) out.push_back(item.c_str());
    return out;
  };
  const auto res_items = pointers(res_names), hz_items = pointers(hz_names);
  auto index = [](const std::vector<std::string>& ids, const std::string& value) {
    return int(std::ranges::find(ids, value) - ids.begin()) % int(ids.size());
  };
  // A host that lists its refresh rates switches only to those; the others are dimmed.
  std::unique_ptr<bool[]> dim(new bool[hz_ids.size()]{});
  if (host && !host->rates.empty())
    for (std::size_t i = 0; i < hz_ids.size(); ++i) {
      int hz = 0;
      if (std::from_chars(hz_ids[i].data(), hz_ids[i].data() + hz_ids[i].size(), hz).ec == std::errc{} && hz)
        dim[i] = std::ranges::none_of(host->rates, [&](double r) { return std::abs(r - hz) < 0.5; });
    }
  bool changed = false;
  // --smoke-view display-menu / game-settings-menu / display-rates opens a list once.
  static bool smoke_menu = options_.smoke_view == "display-menu" || options_.smoke_view == "game-settings-menu";
  static bool smoke_rates = options_.smoke_view == "display-rates";
  setting_row("Resolution per eye", "", dw, s(38), [&] {
    if (std::exchange(smoke_menu, false)) {
      ImGui::PushID("resolution");
      ImGui::OpenPopup("##menu");
      ImGui::PopID();
    }
    int choice = index(res_ids, d.resolution);
    if (dropdown("resolution", "", res_items.data(), int(res_items.size()), &choice, dw)) {
      d.resolution = res_ids[choice];
      if (std::ranges::find(library::resolution_percents, d.percent) == std::end(library::resolution_percents))
        d.percent = library::DisplaySettings{}.percent;
      // A new custom size starts at the runtime's size, or Quest 3's.
      if (d.resolution == "custom" && !d.width) {
        d.width = host ? host->width : library::headset_presets[0].width;
        d.height = host ? host->height : library::headset_presets[0].height;
      }
      changed = true;
    }
  });
  if (d.resolution == "percent") {
    divider();
    std::vector<std::string> names;
    for (int p : library::resolution_percents) names.push_back(std::format("{}%", p));
    const auto items = pointers(names);
    int choice = int(std::ranges::find(library::resolution_percents, d.percent) - std::begin(library::resolution_percents));
    if (choice >= int(items.size())) choice = 0;
    // The size this gives on the runtime of the last game launch, in gray left of the percent.
    std::string result;
    if (host) {
      const auto [w, h] = library::percent_size(*host, library::resolution_percents[choice]);
      result = std::format("{} x {}", w, h);
    }
    const float rw = result.empty() ? 0 : measure(fonts().regular, kSmall + 1, result).x + s(16);
    setting_row("Percent of runtime size", "", dw + rw, s(38), [&] {
      const ImVec2 p = ImGui::GetCursorScreenPos();
      if (!result.empty())
        draw_text(ImGui::GetWindowDrawList(), fonts().regular, kSmall + 1,
                  {std::round(p.x), p.y + text_y(fonts().regular, kSmall + 1, 0, s(38))}, color::text3, result);
      ImGui::SetCursorScreenPos({p.x + rw, p.y});
      if (dropdown("percent", "", items.data(), int(items.size()), &choice, dw)) {
        d.percent = library::resolution_percents[choice];
        changed = true;
      }
    });
  }
  if (d.resolution == "custom") {
    divider();
    const float fw = (dw - s(28)) * 0.5f;
    setting_row("Size per eye", "", dw, s(38), [&] {
      changed |= number_field("##width", &d.width, library::min_custom_side, library::max_custom_side, fw);
      ImGui::SameLine(0, 0);
      ImVec2 p = ImGui::GetCursorScreenPos();
      const ImVec2 xs = measure(fonts().regular, kBody, "x");
      draw_text(ImGui::GetWindowDrawList(), fonts().regular, kBody,
                {std::round(p.x + (s(28) - xs.x) * 0.5f), text_y(fonts().regular, kBody, p.y, s(38))},
                color::text2, "x");
      ImGui::Dummy({s(28), s(38)});
      ImGui::SameLine(0, 0);
      changed |= number_field("##height", &d.height, library::min_custom_side, library::max_custom_side, fw);
    });
  }
  divider();
  setting_row("Frame rate", "", dw, s(38), [&] {
    if (std::exchange(smoke_rates, false)) {
      ImGui::PushID("rate");
      ImGui::OpenPopup("##menu");
      ImGui::PopID();
    }
    int choice = index(hz_ids, d.rate);
    if (dropdown("rate", "", hz_items.data(), int(hz_items.size()), &choice, dw, dim.get())) {
      d.rate = hz_ids[choice];
      changed = true;
    }
  });
  if (d.rate == "custom") {
    divider();
    setting_row("Custom frame rate", "", dw, s(38), [&] {
      changed |= number_field("##custom-rate", &d.custom_rate, library::min_custom_rate, library::max_custom_rate, dw, "Hz");
    });
  }
  divider();
  // --smoke-view display-mirror opens the mirror list once.
  static bool smoke_mirror = options_.smoke_view == "display-mirror";
  setting_row("Desktop mirror", "", dw, s(38), [&] {
    if (std::exchange(smoke_mirror, false)) {
      ImGui::PushID("mirror");
      ImGui::OpenPopup("##menu");
      ImGui::PopID();
    }
    std::vector<std::string> ids{"cropped", "full", "off"}, names{"Cropped", "Whole eye", "Off"};
    if (per_game) {
      ids.insert(ids.begin(), "");
      names.insert(names.begin(), "Use global setting");
    }
    const auto items = pointers(names);
    int choice = index(ids, d.mirror);
    if (dropdown("mirror", "", items.data(), int(items.size()), &choice, dw)) {
      d.mirror = ids[choice];
      changed = true;
    }
  });
  // The eye row, while the mirror (this game's, or the global one it follows) is on.
  // --smoke-view display-mirror-eye opens its list once.
  static bool smoke_eye = options_.smoke_view == "display-mirror-eye";
  if ((per_game && d.mirror.empty() ? settings_.display.mirror : d.mirror) != "off") {
    divider();
    setting_row("Mirror eye", "", dw, s(38), [&] {
      if (std::exchange(smoke_eye, false)) {
        ImGui::PushID("mirror-eye");
        ImGui::OpenPopup("##menu");
        ImGui::PopID();
      }
      std::vector<std::string> ids{"left", "right"}, names{"Left", "Right"};
      if (per_game) {
        ids.insert(ids.begin(), "");
        names.insert(names.begin(), "Use global setting");
      }
      const auto items = pointers(names);
      int choice = index(ids, d.mirror_eye);
      if (dropdown("mirror-eye", "", items.data(), int(items.size()), &choice, dw)) {
        d.mirror_eye = ids[choice];
        changed = true;
      }
    });
  }
  divider();
  // The game's sound also on the PC's own output, beside the headset (off by
  // default). --smoke-view display-pc-audio opens its list once.
  static bool smoke_pc_audio = options_.smoke_view == "display-pc-audio";
  // i18n: Settings row: also play the game's sound on the PC's speakers or headphones.
  setting_row(tr("Audio on PC too"), "", dw, s(38), [&] {
    if (std::exchange(smoke_pc_audio, false)) {
      ImGui::PushID("pc-audio");
      ImGui::OpenPopup("##menu");
      ImGui::PopID();
    }
    std::vector<std::string> ids{"off", "on"};
    // i18n: Choices for "Audio on PC too".
    std::vector<std::string> names{tr("Off"), tr("On")};
    if (per_game) {
      ids.insert(ids.begin(), "");
      names.insert(names.begin(), tr("Use global setting"));
    }
    const auto items = pointers(names);
    int choice = index(ids, d.pc_audio);
    if (dropdown("pc-audio", "", items.data(), int(items.size()), &choice, dw)) {
      d.pc_audio = ids[choice];
      changed = true;
    }
  });
  ImGui::PopID();
  if (changed) action([this] { save_settings(); });
}

void App::draw_game_settings(const meta::Game& g, float width) {
  // A game without its own entry shows "Use global setting"; an entry back at
  // that is dropped when settings are saved.
  auto it = settings_.game_display.try_emplace(g.id, library::game_display_defaults()).first;
  heading(tr("Display"));
  begin_card("game-display", width, {22, 2});
  draw_display_rows(it->second, true, "game");
  ImGui::Dummy({0, 0});  // the last row keeps its bottom padding
  end_card();
  heading(tr("Runtime"));
  begin_card("game-runtime", width, {22, 2});
  draw_game_runtime_rows(g.id);
  ImGui::Dummy({0, 0});
  end_card();
  if (options_.smoke_view == "game-settings-override-end") ImGui::SetScrollHereY(1.0f);
}

void App::draw_game_runtime_rows(const std::string& id) {
  ImGui::PushID("game-runtime");
  // Looked up by id: saving drops an entry back at "Use global setting".
  auto entry = [this, id]() -> library::DisplaySettings& {
    return settings_.game_display.try_emplace(id, library::game_display_defaults()).first->second;
  };
  auto& d = entry();
  const float dw = s(kControlW);
  bool changed = false;
  // The global row's choices after "Use global setting" (an empty choice).
  std::vector<std::string> ids{""};
  ids.insert(ids.end(), std::begin(library::texture_compression_modes), std::end(library::texture_compression_modes));
  const char* modes[] = {tr("Use global setting"), tr("Always"), tr("Automatic"), tr("Off")};
  setting_row(tr("Compress textures"), "", dw, s(38), [&] {
    int choice = int(std::ranges::find(ids, d.texture_compression) - ids.begin()) % int(ids.size());
    if (dropdown("textures", "", modes, int(std::size(modes)), &choice, dw)) {
      auto apply = [this, entry, next = ids[choice]] {
        auto& game = entry();
        game.texture_compression = next;
        // A threshold belongs to the game's own Automatic only.
        if (next != "auto") game.texture_compression_threshold = 0;
        save_settings();
      };
      // A change that takes the game's effective setting down from Always asks first.
      auto effective = [&](const std::string& mode) {
        return mode.empty() ? library::texture_compression_environment(settings_.texture_compression, 0, nullptr).first
                            : mode;
      };
      if (settings_.texture_compression_warns(effective(d.texture_compression), effective(ids[choice])))
        texture_warning_ = apply;
      else action(apply);
    }
  });
  // The game's own Automatic takes its own threshold or follows the global one.
  if (d.texture_compression == "auto") {
    divider();
    static constexpr int percents[] = {0, 25, 50, 75, 90};
    const char* shares[] = {tr("Use global setting"), "25%", "50%", "75%", "90%"};
    int choice = int(std::ranges::find(percents, d.texture_compression_threshold) - std::begin(percents));
    if (choice == int(std::size(percents))) choice = 0;
    setting_row(tr("Compression threshold"), "", dw, s(38), [&] {
      if (dropdown("texture-threshold", "", shares, int(std::size(shares)), &choice, dw)) {
        d.texture_compression_threshold = percents[choice];
        changed = true;
      }
    });
  }
  ImGui::PopID();
  if (changed) action([this] { save_settings(); });
}

// Signing in from any browser: Meta's link to open there. Its Continue button
// opens an oculus:// address, which comes back by itself while Venture opens
// those links, and is pasted back otherwise.
void App::manual_sign_in() {
  ImFont* rf = fonts().regular;
  ImFont* sf = fonts().semibold;
  gap(6);
  if (!auth_manual_) {
    gap(4);
    if (link("Sign in manually##manual", color::text2)) auth_manual_ = true;
    return;
  }
  const float w = ImGui::GetContentRegionAvail().x;
  const bool paste = link_handler_ != 1 || auth_paste_open_;
  text(sf, kSmall, color::text, paste ? "1. Open this link in any browser and sign in to Meta."
                                      : "Open this link in any browser and sign in to Meta. When it asks, let it open Venture.");
  gap(8);
  const float copy_w = button_width("Copy link"), open_w = button_width("Open");
  ImVec2 p = ImGui::GetCursorScreenPos();
  const float field_w = w - copy_w - open_w - s(20), h = s(34);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, {p.x + field_w, p.y + h}, color::surface_hi, h * 0.5f);
  draw_text(dl, rf, kSmall, {p.x + s(16), text_y(rf, kSmall, p.y, h)}, color::text2,
            ellipsize(rf, kSmall, auth_url_, field_w - s(32)));
  ImGui::Dummy({field_w, h});
  ImGui::SameLine(0, s(10));
  ImGui::SetCursorScreenPos({p.x + field_w + s(10), p.y - (s(40) - h) * 0.5f});
  if (button("Open##manual-open", 0, Tone::secondary, {open_w, s(40)})) {
    try {
      open_url(auth_url_);
    } catch (const std::exception &) {
      auth_paste_error_ = "No browser opened. Copy the link instead.";
    }
  }
  ImGui::SetCursorScreenPos({p.x + field_w + open_w + s(20), p.y - (s(40) - h) * 0.5f});
  if (button("Copy link##manual-copy", 0, Tone::secondary, {copy_w, s(40)})) {
    ImGui::SetClipboardText(auth_url_.c_str());
    auth_paste_error_ = "Link copied.";
  }
  ImGui::SetCursorScreenPos({p.x, p.y + h});
  if (!paste) {
    gap(12);
    if (link("Paste the address instead##manual-paste-open", color::text2)) auth_paste_open_ = true;
  } else {
    gap(14);
    text(sf, kSmall, color::text, "2. Paste the oculus:// address that Meta's Continue button tries to open.");
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - s(5));  // the hint belongs to step 2
    text(rf, kSmall, color::text2, "In Firefox, press Ctrl+Shift+K before Continue, then copy the message it shows.");
    gap(2);
    if (submit_field("manual-paste", "oculus://\xE2\x80\xA6", &auth_paste_, w, icons::lock)) {
      paste_sign_in(auth_paste_);
      if (verifying_) auth_paste_.clear();
    }
    gap(8);
  }
  if (!auth_paste_error_.empty()) {
    text(rf, kSmall, auth_paste_error_ == "Link copied." ? color::text2 : color::warning, auth_paste_error_);
    gap(4);
  }
}

// ---------------------------------------------------------------------------
void App::draw_account() {
  page_title("Accounts");
  gap(kAfterHeader);
  float w = std::min(ImGui::GetContentRegionAvail().x, s(kPageWidth));
  ImFont* rf = fonts().regular;
  ImFont* sf = fonts().semibold;
  if (accounts_.empty()) {
    begin_card("connect", w, {28, 28});
    text(sf, kTitle, color::text, "Connect your Meta account", false);
    gap(8);
    if (button(signing_in_ ? "Waiting for Meta\xE2\x80\xA6##signin" : "Sign in with Meta##signin", 0, Tone::primary,
               {s(220), s(46)}, !signing_in_))
      sign_in();
  } else {
    begin_card("accounts", w, {20, 8});
    // Active account first, then the others in the order they were added.
    std::vector<const AccountView*> rows;
    for (auto& a : accounts_)
      if (a.key == account_key_) rows.insert(rows.begin(), &a);
      else rows.push_back(&a);
    for (size_t i = 0; i < rows.size(); ++i) {
      const AccountView& a = *rows[i];
      bool active = a.key == account_key_ && session_;
      bool usable = a.signed_in && !a.needs_reauth;
      ImGui::PushID(a.key.c_str());
      ImVec2 p = ImGui::GetCursorScreenPos();
      float row_w = ImGui::GetContentRegionAvail().x, row_h = s(76);
      ImDrawList* dl = ImGui::GetWindowDrawList();
      float r = s(22);
      avatar(dl, {p.x + r + s(3), p.y + row_h * 0.5f}, r, &a, active, kBody);
      float tx = p.x + r * 2 + s(22);

      // Controls from the right edge inwards: overflow menu, then the action.
      float x = p.x + row_w, cy = p.y + row_h * 0.5f;
      x -= s(36);
      ImGui::SetCursorScreenPos({x, cy - s(18)});
      if (icon_button("more", icons::more, nullptr, 36)) ImGui::OpenPopup("account-menu");
      if (begin_menu("account-menu")) {
        if (active && menu_item("Refresh library", icons::refresh, false, !refreshing_ && !options_.offline)) refresh();
        if (usable && !a.primary && accounts_.size() > 1 && menu_item("Open at startup", icons::star))
          action([&, key = a.key] { mark_primary(key); });
        if (a.signed_in && menu_item("Sign out", icons::signout)) action([&, key = a.key] { sign_out_account(key); });
        if (menu_item("Remove from Venture", icons::trash, true)) {
          remove_account_ = a.key;
          remove_saves_ = false;
        }
        end_menu();
      }
      x -= s(10);
      const char* cta = nullptr;
      if (!usable) cta = a.needs_reauth ? "Reconnect" : "Sign in";
      else if (!active) cta = "Switch";
      if (cta) {
        float bw = button_width(cta);
        x -= bw;
        ImGui::SetCursorScreenPos({x, cy - s(18)});
        bool enabled = !signing_in_ && !options_.offline;
        if (button((std::string(cta) + "##cta").c_str(), 0, usable ? Tone::secondary : Tone::primary, {bw, s(36)},
                   enabled)) {
          if (usable) action([&, key = a.key] { switch_account(key); });
          else sign_in();  // Meta returns the same account id, so the vault reuses this slot
        }
        x -= s(10);
      } else if (active && refreshing_) {
        x -= s(20);
        ImGui::SetCursorScreenPos({x, cy - s(9)});
        spinner(9, color::text2);
      }

      float text_w = x - tx - s(8);
      draw_text(dl, sf, kBody + 1, {std::round(tx), text_y(sf, kBody + 1, cy - s(10))}, color::text,
                ellipsize(sf, kBody + 1, account_name(a), text_w));
      std::string state = active          ? (options_.offline ? "Active  \xC2\xB7  offline" : "Active")
                          : a.needs_reauth ? "Sign-in expired"
                          : a.signed_in    ? "Signed in"
                                           : "Signed out  \xC2\xB7  games and saves are kept";
      if (a.primary && accounts_.size() > 1) state += "  \xC2\xB7  Opens at startup";
      if (!a.alias.empty() && !a.display_name.empty() && a.alias != a.display_name && !generated_alias(a.alias))
        state = "@" + a.alias + "  \xC2\xB7  " + state;
      draw_text(dl, rf, kSmall, {std::round(tx), text_y(rf, kSmall, cy + s(11))}, a.needs_reauth ? color::warning : color::text2,
                ellipsize(rf, kSmall, state, text_w));
      hline(dl, p.x, p.x + row_w, p.y + row_h);
      ImGui::SetCursorScreenPos({p.x, p.y + row_h + s(1)});
      ImGui::Dummy({row_w, 0});
      ImGui::PopID();
    }
    // Add account row.
    {
      size_t signed_in = std::ranges::count(accounts_, true, &AccountView::signed_in);
      bool room = signed_in < 4;
      bool enabled = room && !signing_in_ && !options_.offline;
      ImVec2 p = ImGui::GetCursorScreenPos();
      float row_w = ImGui::GetContentRegionAvail().x, row_h = s(68);
      ImGui::BeginDisabled(!enabled);
      bool clicked = ImGui::InvisibleButton("##add-account", {row_w, row_h});
      ImGui::EndDisabled();
      bool hov = enabled && ImGui::IsItemHovered();
      if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      ImDrawList* dl = ImGui::GetWindowDrawList();
      float r = s(22);
      ImVec2 c{p.x + r + s(3), p.y + row_h * 0.5f};
      dl->AddCircleFilled(c, r, hov ? color::surface_top : color::surface_hi, 48);
      draw_glyph(dl, icons::add, kBody, c, enabled ? color::text : color::text3);
      std::string label = signing_in_ ? "Waiting for Meta\xE2\x80\xA6" : "Add account";
      draw_text(dl, sf, kBody, {std::round(p.x + r * 2 + s(22)), text_y(sf, kBody, p.y, row_h)},
                enabled ? color::text : color::text3, label);
      if (!room && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        tooltip("Up to four accounts can be signed in. Sign one out to add another.");
      if (clicked) action([&] { add_account(); });
    }
  }
  if (signing_in_ || !auth_status_.empty()) {
    gap(4);
    // Spinner, status and Cancel share one line, centred on the text.
    ImVec2 row = ImGui::GetCursorScreenPos();
    const float line = measure(rf, kSmall, "Ag").y;
    float x = row.x;
    if (signing_in_) {
      ImGui::SetCursorScreenPos({x, row.y + (line - s(14)) * 0.5f});
      spinner(7, color::text2);
      x += s(14) + s(10);
    }
    ImGui::SetCursorScreenPos({x, row.y});
    text(rf, kSmall, color::text2, auth_status_, false);
    if (signing_in_) {
      ImGui::SetCursorScreenPos({x + measure(rf, kSmall, auth_status_).x + s(14), row.y});
      if (link("Cancel##signin-cancel", color::text2)) cancel_sign_in();
    }
    ImGui::SetCursorScreenPos({row.x, row.y + line});
    ImGui::Dummy({0, 0});
    if (signing_in_ && !verifying_ && !auth_url_.empty()) manual_sign_in();
  }
  end_card();
}

void App::draw_remove_account() {
  if (remove_account_ && !ImGui::IsPopupOpen("Remove account?")) ImGui::OpenPopup("Remove account?");
  if (begin_dialog("Remove account?", 460)) {
    const AccountView* a = remove_account_ ? account(*remove_account_) : nullptr;
    dialog_text("Remove " + (a ? account_name(*a) : std::string("this account")) + "?",
                "Venture forgets its sign-in. Installed games stay, and you can add the account again any time.");
    gap(10);
    divider();
    setting_row("Also delete its saves", "Saves can't be recovered once deleted.", s(44), s(24),
                [&] { toggle("remove-saves", &remove_saves_); });
    const int clicked = dialog_footer({{"Cancel"}, {"Remove", Tone::danger}});
    if (clicked == 1 && remove_account_)
      action([&, key = *remove_account_, saves = remove_saves_] { remove_account(key, saves); });
    if (clicked >= 0) {
      remove_account_.reset();
      ImGui::CloseCurrentPopup();
    }
    end_dialog();
  }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Import: games installed on a headset, or saved on this computer, become
// library installs of their own files.
void App::draw_import_items(ImportScan& scan, bool device, float width) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  size_t ready = 0;
  uint64_t ready_bytes = 0;
  // A headset can hold many games: a search narrows its list, and long folder lists. Hidden picks stay picked.
  if ((device && !scan.items.empty()) || scan.items.size() > 6 || !scan.search.empty()) {
    gap(6);
    search_field(device ? "import-search##device" : "import-search##local", "Search games", &scan.search,
                 std::min(s(320), width));
    gap(10);
  }
  for (size_t index = 0; index < scan.items.size(); ++index) {
    auto& item = scan.items[index];
    auto match = import_match(item);
    if (!match.importable) item.selected = false;
    ready += item.selected;
    if (item.selected && !installed_package(item.package)) ready_bytes += item.bytes;
    if (!search::matches(scan.search, match.game ? match.game->name : item.label) &&
        !search::matches(scan.search, item.package))
      continue;
    ImGui::PushID(int(index));
    divider();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = s(72), right = p.x + width;
    // Selection box.
    float box = s(22);
    ImVec2 b0{std::round(p.x), std::round(p.y + (h - box) * 0.5f)};
    ImGui::SetCursorScreenPos(b0);
    if (ImGui::InvisibleButton("##pick", {box, box}) && match.importable) item.selected = !item.selected;
    bool hovered = ImGui::IsItemHovered() && match.importable;
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (item.selected) {
      dl->AddRectFilled(b0, {b0.x + box, b0.y + box}, color::accent, s(6));
      draw_glyph(dl, icons::check, kSmall, {b0.x + box * 0.5f, b0.y + box * 0.5f}, color::on_accent);
    } else {
      dl->AddRect(b0, {b0.x + box, b0.y + box},
                  match.importable ? (hovered ? color::text2 : color::text3) : color::stroke, s(6), 0, s(1.5f));
    }
    // Cover of the matched game, else a placeholder.
    float thumb_h = std::round(s(45)), thumb_w = std::round(s(80));
    float tx = std::round(b0.x + box + s(16));
    ImGui::SetCursorScreenPos({tx, std::round(p.y + (h - thumb_h) * 0.5f)});
    if (match.game) {
      art(match.game->id, {thumb_w, thumb_h}, s(6));
    } else {
      ImVec2 t0 = ImGui::GetCursorScreenPos();
      dl->AddRectFilled(t0, {t0.x + thumb_w, t0.y + thumb_h}, color::surface_hi, s(6));
      draw_glyph(dl, icons::package, kHeading, {t0.x + thumb_w * 0.5f, t0.y + thumb_h * 0.5f}, color::text3);
    }
    // Name, then package, version and size.
    float x = std::round(tx + thumb_w + s(16)), mid = std::round(p.y + h * 0.5f);
    ImU32 badge_fg = match.importable ? color::positive : match.game ? color::text2 : color::warning;
    float bw = badge_width(match.status);
    float text_right = right - bw - s(16);
    std::string title = match.game ? match.game->name : item.label.empty() ? item.package : item.label;
    draw_text(dl, fonts().semibold, kBody, {x, text_y(fonts().semibold, kBody, mid - s(10))}, color::text,
              ellipsize(fonts().semibold, kBody, title, text_right - x));
    std::string version = item.version_name.empty() ? std::format("Version {}", item.version_code)
                                                    : std::format("Version {} ({})", item.version_name, item.version_code);
    std::string detail = std::format("{}  \xC2\xB7  {} in {} file{}", version, bytes(item.bytes), item.files,
                                     item.files == 1 ? "" : "s");
    if (match.game || !item.label.empty()) detail = item.package +"  \xC2\xB7  " + detail;
    if (item.saves) detail += std::format("  \xC2\xB7  saves {}", bytes(item.save_bytes));
    if (item.skipped) detail += std::format("  \xC2\xB7  {} skipped", item.skipped);
    draw_text(dl, fonts().regular, kSmall, {x, text_y(fonts().regular, kSmall, mid + s(11))}, color::text2,
              ellipsize(fonts().regular, kSmall, detail, text_right - x));
    ImGui::SetCursorScreenPos({right - bw, mid - s(12)});
    badge(match.status, badge_fg, color::alpha(badge_fg, 0.14f));
    ImGui::SetCursorScreenPos({p.x, p.y + h});
    ImGui::Dummy({width, 0});
    ImGui::PopID();
  }
  if (scan.items.empty()) return;
  divider();
  gap(14);
  // What is picked, and the Import button; where the games go is asked next.
  float bw = s(190);
  ImVec2 row = ImGui::GetCursorScreenPos();
  if (ready)
    draw_text(dl, fonts().regular, kSmall, {row.x, text_y(fonts().regular, kSmall, row.y, s(40))}, color::text2,
              std::format("{} selected{}", ready == 1 ? std::string("1 game") : std::format("{} games", ready),
                          ready_bytes ? "  \xC2\xB7  " + bytes(ready_bytes) : std::string()));
  ImGui::SetCursorScreenPos({row.x + width - bw, row.y});
  std::string label = ready == 0   ? std::string("Import")
                      : ready == 1 ? std::string("Import 1 game")
                                   : std::format("Import {} games", ready);
  label += device ? "##device" : "##local";
  if (button(label.c_str(), icons::download, Tone::primary, {bw, s(40)}, ready > 0))
    action([&, device] { request_import(device); });
  ImGui::SetCursorScreenPos({row.x, row.y + s(40)});
  ImGui::Dummy({width, 0});
  gap(4);
}

void App::draw_import() {
  // --smoke-view import-ask: the dialog the Import button opens, for the scanned games.
  static bool smoke_ask = options_.smoke_view == "import-ask";
  if (smoke_ask && !import_local_.busy && !import_local_.items.empty() && !refreshing_ && !scanning_) {
    smoke_ask = false;
    request_import(false);
  }
  page_title("Import");
  gap(kAfterHeader);
  float w = std::min(ImGui::GetContentRegionAvail().x, s(kPageWidth));

  heading("From a headset");
  begin_card("import-headset", w, {22, 18});
  {
    float inner = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = s(40);
    std::string headline = "No headset connected";
    auto device = std::ranges::find(import_devices_, import_serial_, &android::adb::Device::serial);
    if (device != import_devices_.end() && device->ready())
      headline = std::format("{}  \xC2\xB7  {} games", device->model.empty() ? "Headset" : device->model,
                             import_device_.items.size());
    else if (import_device_.busy)
      headline = "Looking for a headset";
    dl->AddCircleFilled({p.x + h * 0.5f, p.y + h * 0.5f}, h * 0.5f, color::surface_hi, 32);
    draw_glyph(dl, icons::game, kHeading, {p.x + h * 0.5f, p.y + h * 0.5f}, color::text2);
    float tx = std::round(p.x + h + s(14));
    draw_text(dl, fonts().semibold, kBody, {tx, text_y(fonts().semibold, kBody, p.y, h)}, color::text,
              ellipsize(fonts().semibold, kBody, headline, inner - h - s(200)));
    if (ImGui::IsMouseHoveringRect({tx, p.y}, {p.x + inner - s(200), p.y + h}))
      tooltip("Connect a Quest with USB and allow debugging on the headset, then scan.");
    float bw = row_button_w("Scan headset", icons::refresh);
    ImGui::SetCursorScreenPos({p.x + inner - bw, p.y + (h - s(kControlH)) * 0.5f});
    if (import_device_.busy) {
      ImVec2 sp = ImGui::GetCursorScreenPos();
      ImGui::Dummy({bw, h});
      ImGui::SetCursorScreenPos({sp.x + bw - s(28), sp.y + s(11)});
      spinner(9, color::text2);
    } else if (button("Scan headset", icons::refresh, Tone::secondary, {bw, s(kControlH)})) {
      scan_headset();
    }
    ImGui::SetCursorScreenPos({p.x, p.y + h + s(14)});
    ImGui::Dummy({inner, 0});
    // Several headsets (or one over USB and Wi-Fi): pick the one to list.
    if (import_devices_.size() > 1) {
      for (size_t i = 0; i < import_devices_.size(); ++i) {
        const auto& d = import_devices_[i];
        auto label = (d.model.empty() ? d.serial : d.model + "  " + d.serial) + (d.ready() ? "" : "  (" + d.state + ")");
        label += "##headset" + std::to_string(i);
        if (i) ImGui::SameLine(0, s(8));
        if (filter_chip(label.c_str(), d.serial == import_serial_) && !import_device_.busy && d.serial != import_serial_) {
          import_serial_ = d.serial;
          scan_headset();
        }
      }
      gap(12);
    }
    if (!import_device_.error.empty()) {
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
      text(fonts().regular, kSmall, color::warning, import_device_.error);
      ImGui::PopTextWrapPos();
      gap(6);
    }
    draw_import_items(import_device_, true, inner);
    // Wi-Fi debugging: adb connect to the headset's address.
    {
      float cw = row_button_w("Connect", icons::refresh), fw = std::min(s(240), inner * 0.4f);
      ImVec2 row = ImGui::GetCursorPos();
      const ImVec2 at = ImGui::GetCursorScreenPos();
      draw_text(ImGui::GetWindowDrawList(), fonts().regular, kBody,
                {std::round(at.x), text_y(fonts().regular, kBody, at.y, s(38))}, color::text, "Over Wi-Fi");
      ImGui::SetCursorPos({row.x + inner - cw - fw - s(10), row.y});
      bool enter = text_field("##address", "192.168.1.20:5555", &import_address_, fw,
                              ImGuiInputTextFlags_EnterReturnsTrue).entered;
      ImGui::SameLine(0, s(10));
      if ((button("Connect##wifi", icons::refresh, Tone::secondary, {cw, s(kControlH)},
                  !import_device_.busy && !import_address_.empty()) ||
           enter) &&
          !import_address_.empty())
        connect_headset();
    }
  }
  end_card();
  gap(20);

  heading("From this computer");
  begin_card("import-local", w, {22, 18});
  {
    float inner = ImGui::GetContentRegionAvail().x;
    float fw = row_button_w("Choose folder", icons::folder), aw = row_button_w("Choose APK or ZIP", icons::package);
    ImVec2 start = ImGui::GetCursorPos();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    constexpr auto label = "A game's APK with its data";
    draw_text(ImGui::GetWindowDrawList(), fonts().regular, kBody,
              {std::round(at.x), text_y(fonts().regular, kBody, at.y, s(kControlH))}, color::text, label);
    float after = start.y;
    ImGui::SetCursorPos({start.x + inner - fw - aw - s(10), start.y});
    if (button("Choose folder", icons::folder, Tone::secondary, {fw, s(kControlH)})) choose_import(true);
    ImGui::SameLine(0, s(10));
    if (button("Choose APK or ZIP", icons::package, Tone::secondary, {aw, s(kControlH)})) choose_import(false);
    const bool listing = import_local_.busy || !import_local_.error.empty() || !import_local_.items.empty();
    ImGui::SetCursorPosY(std::max(after, start.y + s(kControlH)) + (listing ? s(12) : 0));
    ImGui::Dummy({inner, 0});
    if (import_local_.busy) {
      ImVec2 sp = ImGui::GetCursorScreenPos();
      ImGui::SetCursorScreenPos({sp.x + s(9), sp.y + s(9)});
      spinner(9, color::text2);
      draw_text(ImGui::GetWindowDrawList(), fonts().regular, kSmall,
                {sp.x + s(30), text_y(fonts().regular, kSmall, sp.y, s(18))}, color::text2, "Reading the game files...");
      ImGui::SetCursorScreenPos({sp.x, sp.y + s(26)});
      ImGui::Dummy({inner, 0});
    }
    if (!import_local_.error.empty()) {
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
      text(fonts().regular, kSmall, color::warning, import_local_.error);
      ImGui::PopTextWrapPos();
      gap(6);
    }
    draw_import_items(import_local_, false, inner);
  }
  end_card();
  gap(20);

  // A game the account owns that its library did not list, checked on the
  // game's own store listing. --smoke-view import-link shows it offline.
  if ((session_ && !options_.offline) || options_.smoke_view == "import-link") {
    heading("From your Meta account");
    begin_card("import-owned", w, {22, 18});
    {
      float inner = ImGui::GetContentRegionAvail().x;
      const float fw = std::min(s(420), inner * 0.6f);
      ImVec2 start = ImGui::GetCursorPos();
      const ImVec2 at = ImGui::GetCursorScreenPos();
      draw_text(ImGui::GetWindowDrawList(), fonts().regular, kBody,
                {std::round(at.x), text_y(fonts().regular, kBody, at.y, s(kControlH))}, color::text,
                ellipsize(fonts().regular, kBody, "A game you own, by its store link", inner - fw - s(16)));
      ImGui::SetCursorPos({start.x + inner - fw, start.y + (s(kControlH) - s(34)) * 0.5f});  // submit_field is 34 tall
      if (submit_field("owned-link", "meta.com/experiences/\xE2\x80\xA6", &owned_link_, fw, icons::add, !adding_))
        add_owned();
      ImGui::SetCursorPos({start.x, start.y + s(kControlH)});
      ImGui::Dummy({inner, 0});
    }
    end_card();
    gap(20);
  }

  // Saves for an installed game, from a headset or a copy of its Android/data folder.
  heading("Save data");
  begin_card("import-saves", w, {22, 18});
  {
    float inner = ImGui::GetContentRegionAvail().x;
    std::vector<meta::Game> games;
    for (const auto& g : visible_games())
      if (installed(g.id)) games.push_back(g);
    std::ranges::sort(games, {}, [](const meta::Game& g) { return lowercase(g.name); });
    if (games.empty()) {
      text(fonts().regular, kSmall, color::text3, "Install or import a game first.", false);
    } else {
      auto current = std::ranges::find(games, save_game_, &meta::Game::id);
      int index = current == games.end() ? 0 : int(current - games.begin());
      std::vector<const char*> names;
      for (const auto& g : games) names.push_back(g.name.c_str());
      float fw = row_button_w("From a folder", icons::folder), hw = row_button_w("From headset", icons::download);
      ImVec2 row = ImGui::GetCursorPos();
      ImGui::SetCursorPos(row);
      dropdown("save-game", "", names.data(), int(names.size()), &index, std::min(s(360), inner - fw - hw - s(20)));
      save_game_ = games[index].id;
      const bool running = running_.contains(save_game_) && running_[save_game_].process.active();
      ImGui::SetCursorPos({row.x + inner - fw - hw - s(10), row.y});
      constexpr auto replaces = "Replaces the game's saves in Venture; the current ones are kept in a backup.";
      if (button("From a folder##saves-page", icons::folder, Tone::secondary, {fw, s(kControlH)}, !running))
        choose_save_folder(save_game_);
      if (ImGui::IsItemHovered())
        tooltip(replaces);
      ImGui::SameLine(0, s(10));
      if (button("From headset##saves-page", icons::download, Tone::primary, {hw, s(kControlH)}, !running))
        action([id = save_game_, this] { import_saves_later(id, {}); });
      if (ImGui::IsItemHovered())
        tooltip(replaces);
      ImGui::SetCursorPos({row.x, row.y + s(kControlH)});
      ImGui::Dummy({inner, 0});
    }
  }
  end_card();
  gap(30);
  if (options_.smoke_view == "import-end") ImGui::SetScrollHereY(1.0f);
}

void App::request_import(bool device) {
  const bool steam = settings_.add_games_to_steam && (steam_sync_allowed_ || options_.smoke_view == "import-ask");
  import_choices_.clear();  // free space changes between imports
  for (auto& location : launcher::install_locations(settings_, launcher::local_volumes()))
    import_choices_.emplace_back(location, launcher::free_space(location.folder));
  if (!settings_.install_ask || (!steam && import_choices_.size() < 2)) return start_import(device);
  import_choice_ = 0;
  import_steam_ = settings_.steam_add_new;
  import_remember_ = false;
  import_ask_ = device;
}

void App::draw_import_ask() {
  if (import_ask_ && !ImGui::IsPopupOpen("Import games")) ImGui::OpenPopup("Import games");
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {s(10), 0});
  if (begin_dialog("Import games", 480)) {
    const bool device = import_ask_.value_or(false);
    auto& scan = device ? import_device_ : import_local_;
    const float w = s(424);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // The picked games: new ones need space, installed ones update in place.
    std::vector<std::pair<const ImportItem*, ImportMatch>> picked;
    uint64_t new_bytes = 0, total = 0;
    size_t updates = 0;
    for (const auto& item : scan.items) {
      if (!item.selected) continue;
      auto match = import_match(item);
      if (!match.importable) continue;
      if (installed_package(item.package)) ++updates;
      else new_bytes += item.bytes;
      total += item.bytes;
      picked.emplace_back(&item, match);
    }

    // One game: its cover and name, as when installing. Several: how many.
    {
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const float tw = std::round(s(96)), th = std::round(s(54)), x = at.x + tw + s(14);
      const auto* game = picked.size() == 1 ? picked.front().second.game : nullptr;
      if (game) {
        art(game->id, {tw, th}, s(8));
      } else {
        dl->AddRectFilled(at, {at.x + tw, at.y + th}, color::surface_hi, s(8));
        draw_glyph(dl, picked.size() == 1 ? icons::package : icons::game, kHeading,
                   {at.x + tw * 0.5f, at.y + th * 0.5f}, color::text3);
      }
      std::string name = std::format("{} games", picked.size());
      if (picked.size() == 1) {
        const auto& item = *picked.front().first;
        name = game ? game->name : item.label.empty() ? item.package : item.label;
      }
      draw_text(dl, fonts().semibold, kHeading, {x, text_y(fonts().semibold, kHeading, at.y + th * 0.5f - s(20), s(22))},
                color::text, ellipsize(fonts().semibold, kHeading, name, at.x + w - x));
      draw_text(dl, fonts().regular, kSmall, {x, text_y(fonts().regular, kSmall, at.y + th * 0.5f + s(2), s(18))},
                color::text2, bytes(total));
      ImGui::SetCursorScreenPos({at.x, at.y + th});
      ImGui::Dummy({w, 0});
    }
    gap(14);

    // Where new games go (each library folder, or a new one on another drive) and Steam.
    bool fits = true;
    const bool any_new = updates < picked.size();
    if (import_choices_.size() > 1 && any_new) {
      std::vector<std::string> labels;
      for (auto& [location, free] : import_choices_) {
        // Two folders on one drive are told apart by their names.
        const bool shared = std::ranges::count_if(import_choices_, [&](auto& other) {
                              return other.first.drive == location.drive;
                            }) > 1;
        const auto drive = shared ? location.drive + "  \xC2\xB7  " + paths::utf8(location.folder.filename())
                                  : location.drive;
        labels.push_back(free ? std::format("{}  \xC2\xB7  {} free", drive, bytes(free)) : drive);
      }
      std::vector<const char*> items;
      for (auto& label : labels) items.push_back(label.c_str());
      import_choice_ = std::clamp(import_choice_, 0, int(items.size()) - 1);
      const auto& [location, free] = import_choices_[import_choice_];
      fits = !free || free > new_bytes;
      const float dw = w - s(130);
      divider();
      setting_row("Install to", "", dw, s(38), [&] {
        dialog_dropdown("import-to", items.data(), int(items.size()), &import_choice_, dw);
      });
      if (!fits)
        text(fonts().regular, kSmall, color::danger,
             std::format("Not enough space: the new games need {}.", bytes(new_bytes)), false);
    }
    const bool steam = settings_.add_games_to_steam && (steam_sync_allowed_ || options_.smoke_view == "import-ask");
    if (steam && any_new) {
      divider();
      setting_row("Add to Steam", "Appears in your Steam library and launches in SteamVR.", s(44), s(24),
                  [&] { toggle("import-steam", &import_steam_); });
    }
    if (updates) {
      gap(10);
      text(fonts().regular, kSmall, color::text3,
           updates == 1 && picked.size() == 1 ? std::string("The installed game updates where it is.")
           : updates == picked.size()          ? std::string("The installed games update where they are.")
           : updates == 1                      ? std::string("1 installed game updates where it is.")
                                               : std::format("{} installed games update where they are.", updates),
           false);
    }

    const int clicked = dialog_footer({{"Cancel"}, {"Import", Tone::primary, fits && !picked.empty(), icons::download}},
                                      [&](float h) {
      const ImVec2 row = ImGui::GetCursorScreenPos();
      ImGui::SetCursorScreenPos({row.x, row.y + (h - s(16)) * 0.5f});
      checkbox("import-remember", "Don't ask again", &import_remember_, {}, true);
    });
    if (clicked == 1) {
      const auto folder = import_choices_.size() > 1 ? import_choices_[import_choice_].first.folder : fs::path{};
      action([&, device, folder, add = import_steam_, remember = import_remember_, steam] {
        if (remember) {
          // The chosen drive becomes the games folder; the old one stays a library folder.
          auto next = settings_;
          next.install_ask = false;
          next.steam_add_new = add;
          if (!folder.empty()) next.games = folder;
          apply_settings(std::move(next), false);
          settings_games_ = paths::utf8(settings_.games);
        }
        start_import(device, folder, steam ? std::optional<bool>(add) : std::nullopt);
      });
    }
    if (clicked >= 0) {
      import_ask_.reset();
      ImGui::CloseCurrentPopup();
    }
    end_dialog();
  }
  ImGui::PopStyleVar();
}

void App::request_install(const meta::Game& g) {
  // A pinned version's download resumes into its own folder.
  if (const auto binary = meta::version_binary(g.id); !binary.empty()) {
    const auto* i = installed(g.id, false);
    const auto* base = game(meta::base_app(g.id));
    if (!i || !base) throw std::runtime_error("This game is no longer in your library.");
    install_versions(*base, {meta::Build{std::string(binary), i->plan.version, i->plan.version_code}}, root_of(*i));
    return;
  }
  const bool steam = settings_.add_games_to_steam && (steam_sync_allowed_ || options_.smoke_view == "install");
  if (installed(g.id, false) || !settings_.install_ask) return install_for_owner(g);
  install_choices_.clear();
  for (auto& location : launcher::install_locations(settings_, launcher::local_volumes()))
    install_choices_.emplace_back(location, launcher::free_space(location.folder));
  if (!steam && install_choices_.size() < 2) return install_for_owner(g);
  install_choice_ = 0;
  install_steam_ = in_steam(g.id) && settings_.steam_add_new;
  install_remember_ = false;
  install_versions_ = false;
  install_ask_ = {g.id, g.name};
}
void App::request_versions(const meta::Game& g) {
  request_builds(g.id);
  install_choices_.clear();
  for (auto& location : launcher::install_locations(settings_, launcher::local_volumes()))
    install_choices_.emplace_back(location, launcher::free_space(location.folder));
  install_choice_ = 0;
  version_choice_ = -1;
  install_versions_ = true;
  install_ask_ = {g.id, g.name};
}

void App::draw_install_ask() {
  if (install_ask_ && !ImGui::IsPopupOpen("Install game")) ImGui::OpenPopup("Install game");
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {s(10), 0});
  if (begin_dialog("Install game", 480)) {
    const std::string id = install_ask_ ? install_ask_->first : std::string();
    const std::string name = install_ask_ ? install_ask_->second : std::string("This game");
    const float w = s(424);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto detail = details_.find(id);
    // Another version's size is unknown until it downloads.
    const uint64_t size = detail != details_.end() && !install_versions_ ? detail->second.size : 0;

    // The game: a small cover beside its name and download size.
    {
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const float tw = std::round(s(96)), th = std::round(s(54)), x = at.x + tw + s(14);
      art(id, {tw, th}, s(8));
      const float name_y = size ? at.y + th * 0.5f - s(20) : at.y + th * 0.5f - s(11);
      draw_text(dl, fonts().semibold, kHeading, {x, text_y(fonts().semibold, kHeading, name_y, s(22))}, color::text,
                ellipsize(fonts().semibold, kHeading, name, at.x + w - x));
      if (size)
        draw_text(dl, fonts().regular, kSmall, {x, text_y(fonts().regular, kSmall, at.y + th * 0.5f + s(2), s(18))},
                  color::text2, bytes(size));
      ImGui::SetCursorScreenPos({at.x, at.y + th});
      ImGui::Dummy({w, 0});
    }
    gap(14);

    // Which version (each one Meta published, newest first), or all of them.
    const auto versions = builds_.find(id);
    const bool versions_ready = !install_versions_ || versions != builds_.end();
    if (install_versions_) {
      const float dw = w - s(130);
      divider();
      setting_row("Version", "", dw, s(38), [&] {
        if (versions != builds_.end()) {
          const auto& list = versions->second;
          std::vector<std::string> labels;
          for (auto& b : list) {
            // Builds that share a version name are told apart by their version codes.
            std::string label = std::ranges::count(list, b.version, &meta::Build::version) > 1 ? b.label() : b.version;
            if (b.live) label += "  \xC2\xB7  latest";
            else if (!b.channels.empty() && b.channels.front() != "LIVE") label += "  \xC2\xB7  " + lowercase(b.channels.front());
            if (installed_build(id, b.binary_id)) label += "  \xC2\xB7  installed";
            labels.push_back(std::move(label));
          }
          labels.push_back(std::format("All versions ({})", list.size()));
          std::vector<const char*> items;
          for (auto& label : labels) items.push_back(label.c_str());
          // It starts on the latest build.
          if (version_choice_ < 0) {
            const auto live = std::ranges::find_if(list, &meta::Build::live);
            version_choice_ = live == list.end() ? 0 : int(live - list.begin());
          }
          version_choice_ = std::clamp(version_choice_, 0, int(items.size()) - 1);
          dialog_dropdown("version", items.data(), int(items.size()), &version_choice_, dw);
        } else if (auto e = builds_error_.find(id); e != builds_error_.end()) {
          text(fonts().regular, kSmall, color::danger, e->second, false);
        } else {
          const ImVec2 at = ImGui::GetCursorScreenPos();
          ImGui::SetCursorScreenPos({at.x + dw - s(18), at.y + s(12)});
          spinner(7, color::text2);
        }
      });
    }

    // Where it goes (each library folder, or a new one on another drive) and Steam.
    bool fits = true;
    if (install_choices_.size() > 1) {
      std::vector<std::string> labels;
      for (auto& [location, free] : install_choices_) {
        // Two folders on one drive are told apart by their names.
        const bool shared = std::ranges::count_if(install_choices_, [&](auto& other) {
                              return other.first.drive == location.drive;
                            }) > 1;
        const auto drive = shared ? location.drive + "  \xC2\xB7  " + paths::utf8(location.folder.filename())
                                  : location.drive;
        labels.push_back(free ? std::format("{}  \xC2\xB7  {} free", drive, bytes(free)) : drive);
      }
      std::vector<const char*> items;
      for (auto& label : labels) items.push_back(label.c_str());
      install_choice_ = std::clamp(install_choice_, 0, int(items.size()) - 1);
      const auto& [location, free] = install_choices_[install_choice_];
      fits = !size || !free || free > size;
      const float dw = w - s(130);
      divider();
      setting_row("Install to", "", dw,
                  s(38), [&] {
                    dialog_dropdown("install-to", items.data(), int(items.size()), &install_choice_, dw);
                  });
      if (!fits) text(fonts().regular, kSmall, color::danger, std::format("Not enough space: the game needs {}.", bytes(size)), false);
    }
    if (!install_versions_ && settings_.add_games_to_steam && (steam_sync_allowed_ || options_.smoke_view == "install")) {
      divider();
      setting_row("Add to Steam", "Appears in your Steam library and launches in SteamVR.", s(44), s(24),
                  [&] { toggle("steam-add", &install_steam_); });
    }

    // A quiet "don't ask again" at the left of the buttons.
    const int clicked = dialog_footer({{"Cancel"}, {"Install", Tone::primary, fits && versions_ready, icons::download}},
                                      [&](float h) {
      if (install_versions_) return;
      const ImVec2 row = ImGui::GetCursorScreenPos();
      ImGui::SetCursorScreenPos({row.x, row.y + (h - s(16)) * 0.5f});
      checkbox("install-remember", "Don't ask again", &install_remember_, {}, true);
    });
    if (clicked == 1 && install_versions_ && versions != builds_.end()) {
      const auto folder = install_choices_.empty() ? settings_.games : install_choices_[install_choice_].first.folder;
      const auto& list = versions->second;
      std::vector<meta::Build> chosen;
      if (version_choice_ >= int(list.size())) chosen = list;
      else chosen.push_back(list[size_t(version_choice_)]);
      action([&, id, folder, chosen] {
        const auto* g = game(id);
        if (!g) throw std::runtime_error("This game is no longer in your library.");
        const auto copy = *g;
        const auto queued = install_versions(copy, chosen, folder);
        if (queued > 1) notice_ = std::format("{} versions are downloading.", queued);
        else if (queued) notice_ = "The version is downloading.";
        else notice_ = chosen.size() == 1 ? "That version is installed already." : "Every version is installed already.";
      });
    } else if (clicked == 1) {
      const auto folder = install_choices_.empty() ? settings_.games : install_choices_[install_choice_].first.folder;
      action([&, id, folder, add = install_steam_, remember = install_remember_] {
        const auto* g = game(id);
        if (!g) throw std::runtime_error("This game is no longer in your library.");
        const auto copy = *g;
        if (remember) {
          // The chosen drive becomes the games folder; the old one stays a library folder.
          auto next = settings_;
          next.install_ask = false;
          next.steam_add_new = add;
          next.games = folder;
          apply_settings(std::move(next), false);
          settings_games_ = paths::utf8(settings_.games);
        }
        install_for_owner(copy, folder);
        if (settings_.add_games_to_steam && steam_sync_allowed_) set_in_steam(id, add);
      });
    }
    if (clicked >= 0) {
      install_ask_.reset();
      ImGui::CloseCurrentPopup();
    }
    end_dialog();
  }
  ImGui::PopStyleVar();
}

void App::draw_uninstall() {
  if (uninstall_target_ && !ImGui::IsPopupOpen("Uninstall game?")) ImGui::OpenPopup("Uninstall game?");
  if (begin_dialog("Uninstall game?", 460)) {
    const meta::Game* g = uninstall_target_ ? game(uninstall_target_->plan.app_id) : nullptr;
    const std::string version =
        g && uninstall_target_->plan.pinned ? " (" + uninstall_target_->plan.version + ")" : std::string();
    dialog_text(g ? trf("Uninstall {0}?", g->name + version) : std::string(tr("Uninstall this game?")),
                tr("Its files are removed from this PC. Saves are kept, and you can install it again from your library."));
    const int clicked = dialog_footer({{tr("Cancel")}, {tr("Uninstall"), Tone::danger}});
    if (clicked == 1 && uninstall_target_) {
      action([&] { uninstall(*uninstall_target_); });
      selected_.clear();
    }
    if (clicked >= 0) {
      uninstall_target_.reset();
      ImGui::CloseCurrentPopup();
    }
    end_dialog();
  }
}

void App::draw_texture_warning() {
  if (texture_warning_ && !ImGui::IsPopupOpen("Disable texture compression?"))
    ImGui::OpenPopup("Disable texture compression?");
  if (begin_dialog("Disable texture compression?", 460)) {
    // i18n: Warning when Compress textures changes from Always to Automatic or Off.
    dialog_text(tr("Disable texture compression?"),
                tr("This can increase video memory use by up to 4 times. It isn't recommended on GPUs with less than "
                   "20 GB of video memory."));
    // Cancel is the default (the main button, last), and Enter and Escape choose it.
    int clicked = dialog_footer({{tr("Change anyway")}, {tr("Cancel"), Tone::primary}});
    if (clicked < 0 && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                        ImGui::IsKeyPressed(ImGuiKey_Escape)))
      clicked = 1;
    // Change anyway: the change. The warning comes back with the next change down from Always.
    if (clicked == 0 && texture_warning_) action(texture_warning_);
    if (clicked >= 0) {
      texture_warning_ = nullptr;
      ImGui::CloseCurrentPopup();
    }
    end_dialog();
  }
}

void App::draw_save_import() {
  if (!save_import_target_.empty() && !ImGui::IsPopupOpen("Import save data")) ImGui::OpenPopup("Import save data");
  if (begin_dialog("Import save data", 460)) {
    const meta::Game* g = game(save_import_target_);
    dialog_text(g ? "Import saves for " + g->name : std::string("Import saves"),
                "From a Quest connected over USB (developer mode), or a copy of the game's Android/data folder. "
                "The saves they replace are kept in a backup.");
    const int clicked = dialog_footer({{"Cancel"}, {"From a folder", Tone::secondary, true, icons::folder},
                                       {"From headset", Tone::primary, true, icons::download}});
    if (clicked == 1) choose_save_folder(save_import_target_);
    if (clicked == 2) action([id = save_import_target_, this] { import_saves_later(id, {}); });
    if (clicked >= 0) {
      save_import_target_.clear();
      ImGui::CloseCurrentPopup();
    }
    end_dialog();
  }
}

void App::draw_store_link() {
  // --smoke-view store-link opens the dialog for the first linkable import, for screenshots.
  // store-link-mismatch shows it answering with a listing for another package.
  static bool smoke = options_.smoke_view == "store-link" || options_.smoke_view == "store-link-mismatch";
  if (smoke && store_link_target_.empty())
    for (const auto& i : installs_)
      if (i.plan.local && !digits(i.plan.app_id, 5)) {
        store_link_target_ = i.id();
        smoke = false;
        if (options_.smoke_view == "store-link-mismatch") {
          store_link_text_ = "https://www.meta.com/experiences/1234567890123456/";
          store_link_check_ = {.target = i.id(), .id = "1234567890123456", .name = "Example Game",
                               .listing_package = "com.example.other", .mismatch = true};
        }
        break;
      }
  if (!store_link_target_.empty() && !ImGui::IsPopupOpen("Link to store page")) ImGui::OpenPopup("Link to store page");
  if (begin_dialog("Link to store page", 500)) {
    auto& c = store_link_check_;
    const auto* install = installed(store_link_target_, false);
    const std::string package = install ? install->plan.package : std::string();
    dialog_text(tr("Link to store page"),
                // i18n: {0} is an Android package name such as com.studio.game.
                trf("Paste the game's Meta store link or its ID. Its name, description and art then come from the "
                    "store. The store page must be for {0}.",
                    package));
    gap(16);
    const bool checking = c.busy && c.target == store_link_target_;
    set_on_card(true);  // the dialog is a surface like a card: the field takes the card fill
    bool submit = submit_field("store-link", "meta.com/experiences/\xE2\x80\xA6", &store_link_text_, g_dialog_w,
                               icons::globe, !checking);
    set_on_card(false);
    if (c.target == store_link_target_ && (!c.error.empty() || c.mismatch || checking)) {
      gap(10);
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + g_dialog_w);
      if (checking)
        text(fonts().regular, kSmall, color::text3, tr("Checking the store page\xE2\x80\xA6"));
      else if (!c.error.empty())
        text(fonts().regular, kSmall, color::danger, c.error);
      else
        text(fonts().regular, kSmall, color::warning,
             // i18n: {0} is a game's store name, {1} and {2} Android package names.
             trf("This store page is for {0} ({1}), not {2}. Link it anyway only for a modified build or another "
                 "edition of the same game.",
                 c.name.empty() ? c.id : c.name, c.listing_package.empty() ? std::string("?") : c.listing_package,
                 package));
      ImGui::PopTextWrapPos();
    }
    // Link anyway links the page that was checked, until the field names another.
    const bool mismatch = c.mismatch && c.target == store_link_target_ && !checking &&
                          store_link::id_from_link(store_link_text_) == c.id;
    int clicked = mismatch ? dialog_footer({{tr("Cancel")}, {tr("Link anyway"), Tone::primary}})
                           : dialog_footer({{tr("Cancel")}, {tr("Link"), Tone::primary, !checking && !store_link_text_.empty()}});
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) clicked = 0;
    if (clicked == 1 && mismatch)
      link_store_anyway();
    else if ((clicked == 1 || submit) && !checking)
      check_store_link(store_link_target_, store_link_text_);
    if (clicked == 0 || !install || (c.done && c.target == store_link_target_)) {
      store_link_target_.clear();
      store_link_check_ = {};
      ImGui::CloseCurrentPopup();
    }
    end_dialog();
  }
}

void App::draw_cancel_download() {
  if (cancel_target_ && !ImGui::IsPopupOpen("Cancel download?")) ImGui::OpenPopup("Cancel download?");
  if (begin_dialog("Cancel download?", 460)) {
    const auto& j = cancel_target_;
    dialog_text(j ? "Cancel " + j->name + "?" : std::string("Cancel this download?"),
                "Its downloaded files are deleted. An installed game is kept as it is.");
    const int clicked = dialog_footer({{"Keep downloading"}, {"Cancel download", Tone::danger}});
    if (clicked == 1 && cancel_target_) action([&] { downloads_->remove(*cancel_target_, true); });
    if (clicked >= 0) {
      cancel_target_.reset();
      ImGui::CloseCurrentPopup();
    }
    end_dialog();
  }
}

namespace {
// True when release `tag` (v1.2.3.4) is a later version than `current` (1.2.3); missing parts count as 0.
bool newer_version(std::string_view tag, std::string_view current) {
  auto parts = [](std::string_view text) {
    if (text.starts_with('v')) text.remove_prefix(1);
    std::vector<int> out;
    for (std::size_t at = 0; at <= text.size();) {
      const auto end = std::min(text.find('.', at), text.size());
      int value = 0;
      for (char c : text.substr(at, end - at)) {
        if (c < '0' || c > '9') return std::vector<int>{};
        value = value * 10 + (c - '0');
      }
      out.push_back(value);
      at = end + 1;
    }
    return out;
  };
  auto a = parts(tag), b = parts(current);
  if (a.empty() || b.empty()) return false;
  a.resize(std::max(a.size(), b.size()));
  b.resize(a.size());
  return a > b;
}
}  // namespace

// Once per start: Flatpak updates itself, and offline or direct launches ask no one.
void App::check_update() {
  if (update_checked_ || !worker_) return;
  update_checked_ = true;
  if (!settings_.check_updates || options_.offline || options_.launch_requested ||
      venture::environment::getenv("FLATPAK_ID"))
    return;
  worker_->submit([this](std::stop_token stop) -> Worker::Completion {
    std::string tag, url;
    Json release;
    try {
      const auto r = http::get_latest_release(stop);
      if (r.status == 200) {
        release = Json::parse(r.body);
        tag = release.value("tag_name", "");
        url = release.value("html_url", "");
      }
    } catch (...) {
      log::info("launcher", "Update check failed: {}", exception_text());
    }
    return [this, tag, url, release] {
      const std::string version = tag.starts_with('v') ? tag.substr(1) : tag;
      if (!newer_version(tag, VENTURE_VERSION) || version == settings_.dismissed_update ||
          !url.starts_with("https://github.com/Villagers654/Venture/releases/"))
        return;
      log::info("launcher", "Venture {} is available", version);
      update_ = Update{version, url};
      update_->method = update::method();
      update_->file = update::pick(release, update_->method);
      update_->sums = update::sums(release);
      if (!update_->file || !update_->sums) update_->method = update::Method::page;
    };
  });
}

// Downloads and checks the update off the UI thread; then starts it and quits.
void App::start_update() {
  if (!update_ || update_->downloading || update_->method == update::Method::page) return;
  for (const auto &[id, r] : running_)
    if (!r.exit) {
      update_->error = "Close your game first, then update.";
      return;
    }
  if (!update_worker_) update_worker_ = std::make_unique<Worker>([this] { wake(); });
  update_->downloading = true;
  update_->error.clear();
  update_->done->store(0);
  update_->total->store(update_->file->size);
  update_worker_->submit([this, file = *update_->file, sums = *update_->sums, method = update_->method,
                          done = update_->done, total = update_->total,
                          folder = data_ / "updates"](std::stop_token stop) -> Worker::Completion {
    try {
      auto path = update::fetch(file, sums, folder, stop, [done, total](uint64_t d, uint64_t t) {
        done->store(d);
        if (t) total->store(t);
      });
      return [this, method, path] {
        try {
          update::apply(method, path);
          log::info("launcher", "Started the update from {}", paths::utf8(path));
          exit_reason_ = "update";
          quit_ = true;
        } catch (...) {
          if (update_) {
            update_->downloading = false;
            update_->error = exception_text();
          }
        }
      };
    } catch (...) {
      return [this, e = exception_text()] {
        log::warn("launcher", "Update download failed: {}", e);
        if (update_) {
          update_->downloading = false;
          update_->error = e;
        }
      };
    }
  });
}

void App::draw_update() {
  check_update();
  if (update_worker_) update_worker_->drain();
  // Waits for any other dialog, so it never stacks on one.
  if (update_ && !ImGui::IsPopupOpen("Update available") &&
      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    ImGui::OpenPopup("Update available");
  if (begin_dialog("Update available", 460)) {
    const std::string version = update_ ? update_->version : "";
    const bool in_place = update_ && update_->method != update::Method::page;
    const bool downloading = update_ && update_->downloading;
    std::string detail = std::format("You're on {}. ", VENTURE_VERSION);
    if (!in_place)
      detail += "Download it from the release page.";
    else if (downloading) {
      const auto done = update_->done->load(), total = update_->total->load();
      detail += total ? std::format("Downloading the update: {}%", done * 100 / total)
                      : std::string("Downloading the update\xE2\x80\xA6");
    } else if (update_->method == update::Method::installer)
      detail += "Venture closes to install it. Your games and saves stay.";
    else
      detail += "Venture replaces this AppImage and restarts. Your games and saves stay.";
    dialog_text(std::format("Venture {} is available", version), detail);
    if (update_ && !update_->error.empty()) {
      gap(8);
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + g_dialog_w);
      text(fonts().regular, kSmall, color::danger, update_->error);
      ImGui::PopTextWrapPos();
    }
    bool news = false;
    const int clicked = dialog_footer(
        {{"Not now", Tone::secondary, !downloading},
         {in_place ? (downloading ? "Updating\xE2\x80\xA6" : "Update now") : "Download", Tone::primary, !downloading}},
        [&](float h) {
          if (in_place) news = button("What's new##update", 0, Tone::ghost, {button_width("What's new"), h});
        });
    if (news) action([url = update_->url] { open_url(url); });
    if (clicked == 0) {
      settings_.dismissed_update = version;
      action([&] { save_settings(); });
      update_.reset();
      ImGui::CloseCurrentPopup();
    } else if (clicked == 1 && in_place) {
      start_update();
    } else if (clicked == 1) {
      if (update_) action([url = update_->url] { open_url(url); });
      update_.reset();
      ImGui::CloseCurrentPopup();
    }
    end_dialog();
  }
}

void App::share_report(std::string_view app_id) {
  action([&] {
    report_ = SharedReport{save_debug_report(app_id, VENTURE_VERSION)};
    upload_report();
  });
}

void App::upload_report() {
  report_->url.clear();
  report_->error.clear();
  share_debug_report(report_->file, [this, file = report_->file](std::string url, std::string error) {
    if (!report_ || report_->file != file) return;  // closed, or another report since
    report_->url = std::move(url);
    report_->error = std::move(error);
  });
}

void App::draw_report() {
  if (report_ && !ImGui::IsPopupOpen("Debug report")) ImGui::OpenPopup("Debug report");
  if (begin_dialog("Debug report", 480)) {
    const bool uploading = report_ && report_->url.empty() && report_->error.empty();
    const bool failed = report_ && !report_->error.empty();
    if (uploading)
      dialog_text("Uploading debug report\xE2\x80\xA6",
                  "Logs and system details from this PC, without your name, folders or account details.");
    else if (failed)
      dialog_text("Couldn't upload the debug report",
                  std::format("{} The report is saved as {}: attach it to a GitHub issue or post it in our Discord "
                              "instead.", report_->error, paths::utf8(report_->file.filename())));
    else
      dialog_text("Debug report uploaded",
                  "Put this link in a GitHub issue or our Discord with what happened. It stays readable for 90 days.");
    // The link, or the upload in progress, in a field-like box.
    if (!failed) {
      gap(16);
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float h = s(40);
      dl->AddRectFilled(p, {p.x + g_dialog_w, p.y + h}, color::surface_hi, s(10));
      if (uploading) {
        ImGui::SetCursorScreenPos({p.x + s(14), p.y + (h - s(18)) * 0.5f});
        spinner(9, color::text2);
      } else if (report_) {
        draw_text(dl, fonts().semibold, kBody, {p.x + s(14), text_y(fonts().semibold, kBody, p.y, h)},
                  color::accent_hi, ellipsize(fonts().semibold, kBody, report_->url, g_dialog_w - s(28)));
      }
      ImGui::SetCursorScreenPos({p.x, p.y + h});
      ImGui::Dummy({g_dialog_w, 0});
    }
    const char* main = failed ? "Try again" : report_ && report_->copied ? "Copied" : "Copy link";
    bool show = false;
    const int clicked = dialog_footer({{"Done"}, {main, Tone::primary, !uploading}}, [&](float h) {
      show = button("Show file##report", icons::folder, Tone::ghost, {button_width("Show file", icons::folder), h});
    });
    if (show && report_) action([file = report_->file] { reveal_file(file); });
    if (clicked == 0) {
      report_.reset();
      ImGui::CloseCurrentPopup();
    } else if (clicked == 1 && report_) {
      if (failed) {
        upload_report();
      } else {
        ImGui::SetClipboardText(report_->url.c_str());
        report_->copied = true;
      }
    }
    end_dialog();
  }
}

// ---------------------------------------------------------------------------
// Controls (MOCKUP). Which host controller input each Quest input reaches, for
// the controller the VR runtime reported; both controllers side by side.
namespace {
using controls::kProfiles;
using controls::kQuestCount;
using controls::MenuMode;
using controls::Quest;

std::string_view host_path(const controls::Profile& p, int choice) {
  return choice > 0 && choice <= int(p.inputs.size()) ? p.inputs[choice - 1].path : std::string_view{};
}
int host_index(const controls::Profile& p, std::string_view path) {
  for (size_t i = 0; i < p.inputs.size(); ++i)
    if (p.inputs[i].path == path) return int(i) + 1;
  return 0;
}
// A row's host inputs as component paths, in its order.
std::vector<std::string_view> host_paths(const controls::Profile& p, const std::vector<int>& row) {
  std::vector<std::string_view> out;
  for (const int i : row)
    if (const auto path = host_path(p, i); !path.empty()) out.push_back(path);
  return out;
}
void reset_controls(ControlsEdit& e, int profile) {
  const auto& p = kProfiles[profile];
  const int n = int(p.inputs.size());
  e.profile = profile;
  for (int hand = 0; hand < 2; ++hand)
    for (int q = 0; q < kQuestCount; ++q) {
      const int i = host_index(p, p.defaults[hand][q]);
      e.host[hand][q] = i ? std::vector<int>{i} : std::vector<int>{};
    }
  e.menu_mode = int(MenuMode::automatic);
  // One button and hold start on the automatic input; two buttons on both stick clicks.
  const int stick_click = std::max(1, host_index(p, "/input/thumbstick/click"));
  e.menu_input = p.menu_path.empty() ? stick_click : std::max(1, host_index(p, p.menu_path));
  e.menu_first = stick_click;
  e.menu_second = n + stick_click;
  e.menu_hold_ms = controls::kHoldMs;
  e.menu_chord_ms = controls::kChordHoldMs;
  e.initialized = true;
}

// A hand-prefixed choice (1..2n, left first) from a hand-qualified host path, or 0.
int choice_index(const controls::Profile& p, std::string_view path) {
  for (int hand = 0; hand < 2; ++hand) {
    const auto root = controls::hand_root(hand);
    if (path.starts_with(root)) {
      const int i = host_index(p, path.substr(root.size()));
      return i ? hand * int(p.inputs.size()) + i : 0;
    }
  }
  return 0;
}
std::string choice_path(const controls::Profile& p, int index) {
  const int n = int(p.inputs.size());
  const int i = std::clamp(index, 1, 2 * n) - 1;
  return std::string(controls::hand_root(i / n)) + std::string(p.inputs[size_t(i % n)].path);
}
// The editor on `config`'s choices for one controller (the defaults where it has none).
void open_controls(ControlsEdit& e, int profile, const controls::Config& config) {
  const bool custom = e.custom;
  reset_controls(e, profile);
  e.custom = custom;
  const auto& p = kProfiles[profile];
  const auto it = config.find(p.path);
  if (it == config.end()) return;
  const auto& c = it->second;
  for (int hand = 0; hand < 2; ++hand)
    for (int q = 0; q < kQuestCount; ++q)
      if (const auto& row = c.rows[size_t(hand)][size_t(q)]; row && q != int(Quest::menu)) {
        // Inputs this controller does not have are left out; none left keeps the default.
        std::vector<int> picked;
        for (const auto& path : *row)
          if (const int i = host_index(p, path); i && std::ranges::find(picked, i) == picked.end()) picked.push_back(i);
        if (row->empty() || !picked.empty()) e.host[size_t(hand)][size_t(q)] = std::move(picked);
      }
  const int first = choice_index(p, c.menu.first), second = choice_index(p, c.menu.second);
  if (first) e.menu_input = e.menu_first = first;
  if (second) e.menu_second = second;
  // Each mode keeps its own time; the other starts at its default.
  if (c.menu.mode == MenuMode::chord) e.menu_chord_ms = c.menu.hold_ms;
  else if (c.menu.mode == MenuMode::hold) e.menu_hold_ms = c.menu.hold_ms;
  const bool usable = c.menu.mode == MenuMode::chord ? first && second
                      : c.menu.mode == MenuMode::button || c.menu.mode == MenuMode::hold ? first != 0
                                                                                          : true;
  e.menu_mode = int(usable ? c.menu.mode : MenuMode::automatic);
}
// The editor's choices as saved: only what differs from the defaults.
controls::Choice edit_choice(const ControlsEdit& e) {
  const auto& p = kProfiles[e.profile];
  controls::Choice c;
  for (int hand = 0; hand < 2; ++hand)
    for (int q = 0; q < kQuestCount; ++q) {
      if (q == int(Quest::menu)) continue;
      controls::Inputs row;
      for (const auto path : host_paths(p, e.host[size_t(hand)][size_t(q)])) row.emplace_back(path);
      const auto fallback = p.defaults[size_t(hand)][size_t(q)];
      if (row != (fallback.empty() ? controls::Inputs{} : controls::Inputs{std::string(fallback)}))
        c.rows[size_t(hand)][size_t(q)] = std::move(row);
    }
  switch (MenuMode(e.menu_mode)) {
    case MenuMode::automatic: break;
    case MenuMode::off: c.menu.mode = MenuMode::off; break;
    case MenuMode::button: c.menu = {MenuMode::button, choice_path(p, e.menu_input), {}, controls::kHoldMs}; break;
    case MenuMode::hold: c.menu = {MenuMode::hold, choice_path(p, e.menu_input), {}, e.menu_hold_ms}; break;
    case MenuMode::chord:
      c.menu = {MenuMode::chord, choice_path(p, e.menu_first), choice_path(p, e.menu_second), e.menu_chord_ms};
      break;
  }
  return c;
}
// Writes the editor's choices into `config`; true when that changed it.
bool store_controls(const ControlsEdit& e, controls::Config& config) {
  const auto choice = edit_choice(e);
  const std::string key(kProfiles[e.profile].path);
  const auto it = config.find(key);
  if (choice == controls::Choice{}) {
    if (it == config.end()) return false;
    config.erase(it);
    return true;
  }
  if (it != config.end() && it->second == choice) return false;
  config[key] = choice;
  return true;
}
int profile_index(std::string_view path) {
  for (int i = 0; i < int(std::size(kProfiles)); ++i)
    if (kProfiles[i].path == path) return i;
  return -1;
}

// A hand-prefixed menu choice (1..2n): its hand, input and label.
struct Choice {
  int hand = 0;
  const controls::HostInput* input = nullptr;
};
Choice menu_choice(const controls::Profile& p, int index) {
  const int n = int(p.inputs.size());
  const int i = std::clamp(index, 1, 2 * n) - 1;
  return {i / n, &p.inputs[size_t(i % n)]};
}
// Inputs that work as a button: clicks, presses, touches and trackpad halves.
bool is_button(const controls::HostInput& in) {
  return !in.reserved && (in.path.ends_with("/click") || in.path.ends_with("/force") || in.path.ends_with("/touch") ||
                          in.path.find("/dpad_") != std::string_view::npos);
}
bool is_button(const Choice& c) { return is_button(*c.input) && controls::on_hand(*c.input, c.hand); }
std::string choice_label(const Choice& c) { return std::format("{} {}", c.hand ? "Right" : "Left", c.input->label); }
// "L stick", "R B": a choice short enough for two to share the menu pill.
std::string short_label(const Choice& c) {
  std::string l(c.input->label);
  if (l == "Thumbstick click") l = "stick";
  else if (l.starts_with("Thumbstick")) l = "stick" + l.substr(10);
  else if (l.starts_with("Trackpad ")) l = "pad " + l.substr(9);
  return std::format("{} {}", c.hand ? "R" : "L", l);
}
// The menu row's short line for a held button, and for two buttons; no hold reads as a plain press.

// The game's menu as the menu row resolves it: the pill's text and the host
// inputs it uses per hand.
struct MenuInput {
  std::string label;
  std::vector<std::pair<int, std::string_view>> at;  // hand, host input
};
MenuInput resolved_menu(const ControlsEdit& e) {
  const auto& p = kProfiles[e.profile];
  switch (MenuMode(e.menu_mode)) {
    case MenuMode::automatic: {
      // The controller's menu button where the runtime leaves it to the game, and
      // both stick clicks (the Stick click rows) on every runtime.
      MenuInput m{p.menu_path.empty() ? "Both stick clicks" : std::format("{} or both stick clicks", p.menu_label), {}};
      if (!p.menu_path.empty()) m.at.emplace_back(0, p.menu_path);
      const std::array clicks{host_paths(p, e.host[0][size_t(Quest::stick_click)]),
                              host_paths(p, e.host[1][size_t(Quest::stick_click)])};
      if (!clicks[0].empty() && !clicks[1].empty())
        for (int hand = 0; hand < 2; ++hand)
          for (const auto click : clicks[size_t(hand)]) m.at.emplace_back(hand, click);
      return m;
    }
    case MenuMode::button: {
      const auto c = menu_choice(p, e.menu_input);
      return {choice_label(c), {{c.hand, c.input->path}}};
    }
    case MenuMode::hold: {
      const auto c = menu_choice(p, e.menu_input);
      return {choice_label(c), {{c.hand, c.input->path}}};
    }
    case MenuMode::chord: {
      const auto a = menu_choice(p, e.menu_first), b = menu_choice(p, e.menu_second);
      std::string label;
      if (a.input == b.input && a.hand != b.hand) {
        std::string l(a.input->label);
        for (auto& ch : l) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (l.starts_with("thumbstick")) l.erase(0, 5);  // "stick click"
        label = std::format("Both {}{}", l, l.ends_with('s') ? "" : "s");
      } else {
        label = std::format("{} + {}", short_label(a), short_label(b));  // "L stick + R B"
      }
      return {label, {{a.hand, a.input->path}, {b.hand, b.input->path}}};
    }
    case MenuMode::off: return {"Off", {}};
  }
  return {};
}

// Screenshots of a row with two inputs: each Stick click also on the controller's
// first free button (a Steam Frame's bumper).
void add_second_stick_click(ControlsEdit& e) {
  const auto& p = kProfiles[e.profile];
  for (int hand = 0; hand < 2; ++hand) {
    auto& row = e.host[hand][size_t(Quest::stick_click)];
    if (const int bumper = host_index(p, "/input/shoulder/click")) {
      row.push_back(bumper);
      continue;
    }
    for (int i = 1; i <= int(p.inputs.size()); ++i) {
      const auto& in = p.inputs[size_t(i - 1)];
      bool taken = false;
      for (const auto& other : e.host[hand]) taken |= std::ranges::contains(other, i);
      if (!taken && !in.reserved && controls::on_hand(in, hand) && in.path.ends_with("/click") && !controls::is_axes(in.path)) {
        row.push_back(i);
        break;
      }
    }
  }
}

// Case-insensitive: every word of `query` is in the profile's name or aliases.
bool profile_matches(const controls::Profile& p, std::string_view query) {
  auto lower = [](std::string_view s) {
    std::string out(s);
    for (auto& ch : out) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return out;
  };
  const std::string hay = lower(p.name) + " " + lower(p.aliases);
  std::string q = lower(query);
  for (size_t at = 0; at < q.size();) {
    size_t end = q.find(' ', at);
    if (end == q.npos) end = q.size();
    if (end > at && hay.find(q.substr(at, end - at)) == std::string::npos) return false;
    at = end + 1;
  }
  return true;
}

// A small pill tag such as "Connected".
float tag_width(std::string_view t) { return measure(fonts().semibold, kLabel - 1, t).x + s(16); }
void draw_tag(ImDrawList* dl, ImVec2 at, float h, std::string_view t, ImU32 tone) {
  const float w = tag_width(t);
  dl->AddRectFilled(at, {at.x + w, at.y + h}, color::alpha(tone, 0.16f), h * 0.5f);
  draw_text(dl, fonts().semibold, kLabel - 1, {std::round(at.x + s(8)), text_y(fonts().semibold, kLabel - 1, at.y, h)},
            tone, t);
}

// One joined control of choices: the chosen one filled, like a selected chip.
bool segmented(const char* id, const char* const* items, int count, int* current, float width) {
  ImGui::PushID(id);
  const float h = s(32), pad = s(3);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, {p.x + width, p.y + h}, color::surface_hi, h * 0.5f);
  const float seg = (width - 2 * pad) / float(count);
  bool changed = false;
  for (int i = 0; i < count; ++i) {
    const ImVec2 a{p.x + pad + seg * float(i), p.y + pad}, b{a.x + seg, p.y + h - pad};
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(i);
    ImGui::InvisibleButton("##seg", {seg, b.y - a.y});
    ImGui::PopID();
    const bool on = *current == i, hot = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked() && !on) *current = i, changed = true;
    if (on) dl->AddRectFilled(a, b, color::selected, (b.y - a.y) * 0.5f);
    else if (hot) dl->AddRectFilled(a, b, color::surface_top, (b.y - a.y) * 0.5f);
    const ImVec2 ts = measure(fonts().semibold, kSmall, items[i]);
    draw_text(dl, fonts().semibold, kSmall, {std::round(a.x + (seg - ts.x) * 0.5f), text_y(fonts().semibold, kSmall, a.y, b.y - a.y)},
              on ? color::on_selected : color::text2, items[i]);
  }
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy({width, h});
  ImGui::PopID();
  return changed;
}

// A small caption between groups of menu rows.
void menu_section(std::string_view t) {
  gap(4);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  draw_text(ImGui::GetWindowDrawList(), fonts().semibold, kLabel - 1, {p.x + s(12), p.y}, color::text3, t);
  ImGui::Dummy({0, s(18)});
}
}  // namespace

// The controller shown: a pill that opens a searchable list of every supported
// controller, where Connected (the runtime reports it now) and Last used are tagged.
void App::draw_controller_picker(ControlsEdit& e, float width, const controls::Config& config) {
  ImGui::PushID("picker");
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 size{width, s(38)};
  ImGui::InvisibleButton("##pill", size);
  const bool hovered = ImGui::IsItemHovered(), open = ImGui::IsPopupOpen("##list");
  if (ImGui::IsItemClicked()) {
    controls_search_.clear();
    ImGui::OpenPopup("##list");
  }
  if (options_.smoke_view == "controls-search") {
    static int frames = 0;
    if (frames++ == 2) {
      controls_search_ = "vive";
      ImGui::OpenPopup("##list");
    }
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, {p.x + size.x, p.y + size.y}, hovered || open ? color::surface_top : color::surface_hi,
                    size.y * 0.5f);
  auto tag_of = [&](int i) -> std::pair<std::string_view, ImU32> {
    if (i == controls_connected_) return {"Connected", color::positive};
    if (i == controls_last_used_) return {"Last used", color::text2};
    return {{}, 0};
  };
  // The pill names the controller only; Connected and Last used show in the list.
  const auto name = ellipsize(fonts().semibold, kSmall + 1, kProfiles[e.profile].name, width - s(56));
  draw_text(dl, fonts().semibold, kSmall + 1, {std::round(p.x + s(16)), text_y(fonts().semibold, kSmall + 1, p.y, size.y)},
            color::text, name);
  draw_glyph(dl, icons::chevron_down, kLabel, {p.x + size.x - s(20), p.y + size.y * 0.5f}, color::text2);

  const float list_w = std::max(width, s(320));
  ImGui::SetNextWindowPos({p.x + width - list_w, p.y + size.y + s(6)});
  ImGui::SetNextWindowSize({list_w, 0});
  if (begin_menu("##list")) {
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(0);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
    search_field("search", "Search controllers", &controls_search_, list_w - s(12));
    ImGui::PopStyleColor();
    gap(4);
    int shown = 0, matches = 0;
    for (const auto& profile : kProfiles) matches += profile_matches(profile, controls_search_);
    ImGui::BeginChild("##rows", {0, std::min(s(40) * float(std::max(matches, 1)), s(360))}, 0,
                      ImGuiWindowFlags_NoBackground);
    for (int i = 0; i < int(std::size(kProfiles)); ++i) {
      if (!profile_matches(kProfiles[i], controls_search_)) continue;
      ++shown;
      ImGui::PushID(i);
      const ImVec2 row = ImGui::GetCursorScreenPos();
      const float row_w = ImGui::GetContentRegionAvail().x;
      if (menu_item(kProfiles[i].name.data()) && i != e.profile) open_controls(e, i, config);
      ImDrawList* ml = ImGui::GetWindowDrawList();
      float right = row.x + row_w - s(12);
      if (i == e.profile) {
        draw_glyph(ml, icons::check, kSmall, {right - s(6), row.y + s(19)}, color::accent_text);
        right -= s(24);
      }
      if (const auto [t, c] = tag_of(i); !t.empty()) draw_tag(ml, {right - tag_width(t), row.y + s(9)}, s(20), t, c);
      ImGui::PopID();
    }
    if (!shown) {
      gap(8);
      ImGui::Indent(s(12));
      text(fonts().regular, kSmall, color::text3, "No controller matches.", false);
      ImGui::Unindent(s(12));
      gap(8);
    }
    ImGui::EndChild();
    end_menu();
  }
  ImGui::PopID();
}

// The controller the runtime reported (the runtime writes <data>/controllers.json
// when a game's controller changes): Connected while that game runs, else Last used.
void App::read_controller_report() {
  const double now = ImGui::GetTime();
  if (now < controls_report_check_) return;
  controls_report_check_ = now + 1.0;
  if (options_.smoke_view.starts_with("controls") || options_.smoke_view.starts_with("game-controls")) {
    // Screenshots: as if an Index were connected and Touch Plus used before.
    controls_connected_ = options_.smoke_view == "controls-touch" ? -1 : 0;
    controls_last_used_ = 2;
    return;
  }
  std::error_code ec;
  const auto file = data_ / "controllers.json";
  const auto stamp = fs::last_write_time(file, ec);
  if (!ec && stamp != controls_report_stamp_) {
    controls_report_stamp_ = stamp;
    const Json report = Json::parse(paths::read(file, 1 << 14), nullptr, false);
    const auto left = report.is_object() ? str(report, "left") : std::string();
    const int index = profile_index(left.empty() && report.is_object() ? str(report, "right") : left);
    if (index >= 0) {
      controls_reported_ = index;
      controls_reported_at_ = report.is_object() ? std::int64_t(number(report, "time")) : 0;
    }
  }
  // Connected: a game that is running now reported it.
  bool live = false;
  const auto wall = std::chrono::system_clock::now();
  for (const auto& [id, run] : running_) {
    if (!run.process.active()) continue;
    const auto started = wall - (std::chrono::steady_clock::now() - run.started);
    const auto since = std::chrono::duration_cast<std::chrono::seconds>(started.time_since_epoch()).count();
    if (controls_reported_at_ + 2 >= since) live = true;
  }
  controls_connected_ = live ? controls_reported_ : -1;
  controls_last_used_ = live ? -1 : controls_reported_;
}

void App::draw_controls() {
  read_controller_report();
  if (!controls_.initialized) {
    // The page opens on the connected controller, else the last one used, else Touch.
    int profile = controls_connected_ >= 0 ? controls_connected_ : controls_last_used_ >= 0 ? controls_last_used_ : 1;
    for (int i = 0; i < int(std::size(kProfiles)); ++i) {
      const auto v = options_.smoke_view;
      if ((v == "controls" && i == 0) ||
          ((v == "controls-frame" || v.starts_with("controls-multi")) && kProfiles[i].name == "Steam Frame") ||
          (v == "controls-psvr2" && kProfiles[i].name == "PlayStation VR2 Sense") ||
          (v == "controls-vive" && kProfiles[i].name == "HTC Vive wand") ||
          (v == "controls-wmr" && kProfiles[i].name == "Windows Mixed Reality") ||
          (v.starts_with("controls-pico") && kProfiles[i].name == "Pico 4") ||
          (v == "controls-turned-cosmos" && kProfiles[i].name == "HTC Vive Cosmos") ||
          (v.starts_with("controls-turned-index") && kProfiles[i].name == "Valve Index") ||
          (v.starts_with("controls-turned-touch") && kProfiles[i].name == "Meta Quest Touch Plus") ||
          (v.starts_with("controls-turned-psvr2") && kProfiles[i].name == "PlayStation VR2 Sense") ||
          (v == "controls-generic" && kProfiles[i].name == "Generic controller"))
        profile = i;
    }
    open_controls(controls_, profile, settings_.controls);
    if (options_.smoke_view.starts_with("controls-multi")) add_second_stick_click(controls_);
    // The left Stick click's second list open: its Remove and Add another input.
    if (options_.smoke_view == "controls-multi-add") controls_open_line_ = std::array{0, int(Quest::stick_click), 1};
  }
  const float w = std::min(ImGui::GetContentRegionAvail().x, s(1180));
  const float top = ImGui::GetCursorPosY();
  page_title("Controls");
  const float bw = button_width("Reset to defaults");
  const float hw = std::min(w, ImGui::GetContentRegionAvail().x - s(header_right_inset()));
  const float pw = std::clamp(hw - bw - s(10) - measure(fonts().semibold, kDisplay, "Controls").x - s(24), s(180), s(300));
  ImGui::SetCursorPos({ImGui::GetCursorStartPos().x + hw - pw - bw - s(10), top + s(kHeaderHeight - 38) * 0.5f});
  draw_controller_picker(controls_, pw, settings_.controls);
  ImGui::SetCursorPos({ImGui::GetCursorStartPos().x + hw - bw, top + s(kHeaderHeight - 38) * 0.5f});
  if (button("Reset to defaults##controls", 0, Tone::secondary, {bw, s(38)})) reset_controls(controls_, controls_.profile);
  ImGui::SetCursorPosY(top + s(kHeaderHeight));
  gap(kAfterHeader);
  draw_controls_editor(controls_, nullptr, w, nullptr);
  // Saved as they change; the next launch passes them to the runtime.
  if (options_.smoke_view.empty() && store_controls(controls_, settings_.controls)) action([&] { settings_.save(data_); });
}

// A game's Controls tab: the player's controls, or the game's own.
void App::draw_game_controls(const meta::Game& g, float w) {
  read_controller_report();
  if (!controls_.initialized) {
    const int profile = controls_connected_ >= 0 ? controls_connected_ : controls_last_used_ >= 0 ? controls_last_used_ : 1;
    int shown = options_.smoke_view.starts_with("game-controls") ? 0 : profile;
    if (options_.smoke_view.starts_with("game-controls-multi")) shown = profile_index("/interaction_profiles/valve/frame_controller_valve");
    open_controls(controls_, shown, settings_.controls);
  }
  if (!game_controls_.initialized || controls_game_ != g.id) {
    controls_game_ = g.id;
    const auto own = settings_.game_controls.find(g.id);
    game_controls_.custom = own != settings_.game_controls.end();
    open_controls(game_controls_, controls_.profile, game_controls_.custom ? own->second : settings_.controls);
    if (options_.smoke_view.starts_with("game-controls")) {
      // Screenshots: this game's own choices, with two conflicts to show.
      game_controls_.custom = true;
      game_controls_.menu_mode = int(controls::MenuMode::chord);
      if (options_.smoke_view.starts_with("game-controls-multi")) {
        game_controls_.menu_mode = int(controls::MenuMode::automatic);
        add_second_stick_click(game_controls_);
      } else {
        game_controls_.host[0][int(controls::Quest::secondary)] = game_controls_.host[0][int(controls::Quest::primary)];
      }
    }
  }
  const bool was_custom = game_controls_.custom;
  draw_controls_editor(game_controls_.custom ? game_controls_ : controls_, &controls_, w, &g);
  if (!options_.smoke_view.empty()) return;
  bool changed = false;
  if (game_controls_.custom && !was_custom) {
    // The game's own controls start as a copy of the player's.
    settings_.game_controls[g.id] = settings_.controls;
    open_controls(game_controls_, controls_.profile, settings_.game_controls[g.id]);
    changed = true;
  } else if (!game_controls_.custom && was_custom) {
    changed = settings_.game_controls.erase(g.id) > 0;
  }
  if (game_controls_.custom) changed |= store_controls(game_controls_, settings_.game_controls[g.id]);
  else changed |= store_controls(controls_, settings_.controls);
  if (changed) action([&] { settings_.save(data_); });
}

// The picture of profile `p` for one hand: the user's installed render model,
// else Venture's bundled picture, else the generic controller.
const art::Image* App::controller_image(const controls::Profile& p, int hand) {
  const char* side = hand ? "right" : "left";
  bool pending = false;
  if (const art::Image* image =
          art::installed_controller(p.render_model, p.art, side, data_ / "cache/controllers", [this] { wake(); }, &pending))
    return image;
  if (const art::Image* image = p.art.empty() ? nullptr : art::controller(p.art, side)) return image;
  return art::controller("generic", side);
}

void App::draw_controls_editor(ControlsEdit& e, const ControlsEdit* base, float w, const meta::Game* game) {
  const auto& p = kProfiles[e.profile];
  const int n_inputs = int(p.inputs.size());
  const bool locked = base && !e.custom;  // a game without its own controls shows the defaults
  const int hover = std::exchange(controls_hover_, -1);  // the row under the pointer last frame
  const int active = std::exchange(controls_active_, -1);  // the row being changed last frame
  // Turned models stand back up when the page opens again or shows another controller.
  const int frame = ImGui::GetFrameCount();
  if (controller_turns_frame_ < frame - 1 || controller_turns_profile_ != e.profile) controller_turns_.clear();
  controller_turns_frame_ = frame;
  controller_turns_profile_ = e.profile;
  // A row being changed or pointed at lights its callouts; the others step back.
  const int lit_row = active >= 0 ? active : hover;
  const bool panel_open = std::exchange(controls_menu_panel_, false);  // the menu popover, last frame
  const auto menu = resolved_menu(e);
  const auto mode = MenuMode(e.menu_mode);
  auto quest_label = [](int hand, int q) {
    if (q == int(Quest::menu)) return std::string_view{"Menu"};
    return hand ? controls::kQuestInputs[q].right : controls::kQuestInputs[q].left;
  };
  // Rows that reach the same host input on one hand conflict. A held menu and a
  // two-button menu share on purpose: a press of one button still reaches the
  // game, only the hold or the pair opens the menu.
  auto reaches = [&](int hand, int q, std::string_view path) {
    if (path.empty()) return false;
    if (q != int(Quest::menu)) return std::ranges::contains(host_paths(p, e.host[hand][q]), path);
    return std::ranges::any_of(menu.at, [&](const auto& m) { return m.first == hand && m.second == path; });
  };
  std::array<std::array<int, kQuestCount>, 2> twin{};  // the other Quest row + 1
  for (int hand = 0; hand < 2; ++hand)
    for (int a = 0; a < kQuestCount; ++a)
      for (int b = 0; b < kQuestCount; ++b) {
        if (a == b || twin[hand][a]) continue;
        const bool menu_combo = mode == MenuMode::hold || menu.at.size() > 1;
        if (menu_combo && (a == int(Quest::menu) || b == int(Quest::menu))) continue;
        std::vector<std::string_view> paths;
        if (a == int(Quest::menu)) {
          for (const auto& [h, path] : menu.at)
            if (h == hand) paths.push_back(path);
        } else {
          paths = host_paths(p, e.host[hand][a]);
        }
        for (auto path : paths)
          if (reaches(hand, b, path)) twin[hand][a] = b + 1;
      }

  // Geometry: rows | picture | picture | rows, or one controller per card when
  // the window is too narrow for both.
  const float pad = s(22);
  const float inner = w - 2 * pad;
  // The label column also holds the hover + (and an extra input's x) beside the dropdown.
  const float row_h = s(50), head_h = s(28), mid = s(4), gutter = s(30), lw = s(100), mark_w = s(14);
  const float half_wide = (inner - mid) * 0.5f;
  const float dw = std::clamp(half_wide * 0.32f, s(146), s(190));
  const float rows_w = lw + dw + mark_w + s(8);
  const bool stacked = half_wide - rows_w - gutter < s(150);
  const float half = stacked ? inner : half_wide;
  // A row with several host inputs stacks one dropdown per input; both sides as
  // tall as the longer list.
  const float line_h = s(44);
  auto row_lines = [&](int hand, int q) { return q == int(Quest::menu) ? 1 : std::max<int>(1, int(e.host[hand][q].size())); };
  int extra_lines = 0;
  for (int hand = 0; hand < 2; ++hand) {
    int lines = 0;
    for (int q = 0; q < kQuestCount; ++q) lines += row_lines(hand, q) - 1;
    extra_lines = std::max(extra_lines, lines);
  }
  const float rows_h = head_h + row_h * 8 + line_h * float(extra_lines);
  const std::array<const art::Image*, 2> images{controller_image(p, 0), controller_image(p, 1)};
  // One scale for the pair, so the two controllers read as a matched pair.
  const float pic_room = std::min(half - rows_w - gutter, s(460));
  float pic_scale = 1e9f;
  for (const auto* image : images)
    if (image)
      pic_scale = std::min({pic_scale, (rows_h - head_h * 0.5f) / float(image->height), pic_room / float(image->width)});

  // The per-game line: own controls, the controller shown, and reset.
  auto toolbar = [&] {
    if (!game) return;
    const ImVec2 row = ImGui::GetCursorPos();
    ImGui::SetCursorPos({row.x, row.y + (s(38) - s(24)) * 0.5f});
    toggle("game-controls-custom", &game_controls_.custom);
    const ImVec2 t0 = ImGui::GetItemRectMin(), t1 = ImGui::GetItemRectMax();
    draw_text(ImGui::GetWindowDrawList(), fonts().regular, kBody,
              {t1.x + s(12), text_y(fonts().regular, kBody, t0.y, t1.y - t0.y)}, color::text,
              "Custom controls for this game");
    const float bw = button_width("Reset to my defaults");
    const float right = row.x + inner;
    // The picker and reset share the line when they fit beside the toggle, else take the next.
    const float label_end = t1.x - ImGui::GetWindowPos().x + ImGui::GetScrollX() + s(12) +
                            measure(fonts().regular, kBody, "Custom controls for this game").x + s(24);
    const bool wrap = right - bw - s(10) - s(220) < label_end;
    const float line = wrap ? row.y + s(38) + s(12) : row.y;
    const float pw = std::clamp(right - bw - s(10) - (wrap ? row.x : label_end), s(180), s(300));
    ImGui::SetCursorPos({right - pw - bw - s(10), line});
    draw_controller_picker(game_controls_.custom ? game_controls_ : controls_, pw,
                           game_controls_.custom && game ? settings_.game_controls[game->id] : settings_.controls);
    ImGui::SetCursorPos({right - bw, line});
    if (button("Reset to my defaults", 0, Tone::secondary, {bw, s(38)}, game_controls_.custom) && game) {
      // The game's own controls become the player's again (all controllers).
      settings_.game_controls[game->id] = settings_.controls;
      open_controls(game_controls_, game_controls_.profile, settings_.controls);
      if (options_.smoke_view.empty()) action([&] { settings_.save(data_); });
    }
    ImGui::SetCursorPos({row.x, line + s(38)});
    gap(14);
    divider();
    gap(18);
  };

  // Notes under each controller: a warning (a real conflict) or information.
  struct Note {
    std::string text;
    bool warning = true;
  };
  std::array<std::vector<Note>, 2> notes;
  // One controller: its caption, picture, rows and callouts. Returns its height.
  auto draw_hand = [&](int hand, ImVec2 at) {
    ImGui::PushID(hand);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const art::Image* image = images[hand];
    // The input component a host path names ("/input/b/click" -> "b").
    auto component = [](std::string_view path) {
      auto key = path.substr(7);
      return key.substr(0, key.find('/'));
    };
    // Trackpad halves point at that half of the trackpad.
    auto half_dy = [&](std::string_view path) {
      return (path.ends_with("/dpad_up") ? -0.03f : path.ends_with("/dpad_down") ? 0.03f : 0.f) * float(image->height);
    };
    // Where the unturned picture shows an input (rows keep this order while it turns).
    auto spot_of = [&](std::string_view path) -> std::optional<ImVec2> {
      if (!image || !path.starts_with("/input/")) return std::nullopt;
      const auto key = component(path);
      for (const auto& sp : image->spots)
        if (key == sp.input) return ImVec2{sp.x, sp.y + half_dy(path)};
      return std::nullopt;
    };
    struct Row {
      int q;
      std::vector<std::string_view> paths;  // the menu can take two buttons
      std::optional<ImVec2> spot;
    };
    std::vector<Row> rows;
    for (int q = 0; q < kQuestCount; ++q) {
      if (q == int(Quest::menu) ? hand != 0 : quest_label(hand, q).empty()) continue;
      std::vector<std::string_view> paths;
      if (q == int(Quest::menu)) {
        for (const auto& [h, mp] : menu.at)
          if (h == hand) paths.push_back(mp);
      } else {
        paths = host_paths(p, e.host[hand][q]);
      }
      std::optional<ImVec2> spot;
      for (auto path : paths)
        if (auto sp = spot_of(path); sp && (!spot || sp->y < spot->y)) spot = sp;
      rows.push_back({q, std::move(paths), spot});
    }
    // Top to bottom as their inputs sit on the picture, so no callouts cross;
    // rows without a spot follow.
    std::ranges::stable_sort(rows, {}, [](const Row& r) { return r.spot ? r.spot->y : 1e9f; });

    const float x0 = at.x + (stacked ? 0 : hand * (half + mid));
    const float rows_x = hand ? x0 + half - rows_w : x0;
    const float k = image ? pic_scale : 0;
    const float pic_w = image ? float(image->width) * k : 0, pic_h = image ? float(image->height) * k : 0;
    // The pictures meet in the middle; both hands share one center line.
    const float room_x = hand ? x0 : x0 + rows_w + gutter;
    const float pic_x = hand ? room_x + std::max(0.f, (pic_room - pic_w)) * (stacked ? 0.5f : 0.2f)
                             : room_x + pic_room - pic_w - std::max(0.f, (pic_room - pic_w)) * (stacked ? 0.5f : 0.2f);
    const float pic_y = at.y + head_h + (rows_h - head_h - pic_h) * 0.5f;
    // The player turns a controller by dragging it; a double click stands it back
    // up. The turn lasts while the page shows this controller, per hand.
    const bool can_turn = image && art::turnable(*image);
    const ImVec2 room0{room_x, at.y + head_h}, room1{room_x + pic_room, at.y + rows_h};
    art::Turn turn;
    std::shared_ptr<const art::Shot> shot;
    if (can_turn) {
      art::prepare_turn(*image, [this] { wake(); });
      ImGui::SetCursorScreenPos(room0);
      ImGui::SetNextItemAllowOverlap();
      ImGui::InvisibleButton("##turn", {room1.x - room0.x, room1.y - room0.y});
      auto& stored = controller_turns_[image];
      const bool dragging = ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.f);
      if (dragging) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        if (d.x != 0 || d.y != 0) stored.drag(d.x / s(1) * 0.5f, d.y / s(1) * 0.5f);
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
      } else if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      }
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) stored = {};
      // Screenshots: the left one turned toward the page, or ("-back") around to its far side.
      if (options_.smoke_view.starts_with("controls-turned") && hand == 0 && stored.identity())
        stored.drag(options_.smoke_view.ends_with("-back") ? 160.f : -70.f, 15.f);
      turn = stored;
      // The quick drawing while dragging, the smooth one when it rests; unturned,
      // the picture itself shows and the drawing only tells which inputs it hides.
      shot = art::turned(*image, turn, turn.identity() ? 0.5f : k, !dragging && !turn.identity(), [this] { wake(); });
    }
    const bool show_shot = shot && !turn.identity() && shot->width > 0;
    // Where an input is now, and whether the model hides it.
    // The drawing the page shows (a turned one, even a step behind the drag) or,
    // unturned, one of the still's own view: its depth decides what hides what.
    const bool shot_matches = shot && (show_shot || shot->turn == turn);
    struct Live {
      ImVec2 at;
      bool hidden = false;
      const art::Anchor* anchor = nullptr;  // with shot_matches: for covered_spans
    };
    auto live_of = [&](std::string_view path) -> std::optional<Live> {
      if (!image || !path.starts_with("/input/")) return std::nullopt;
      const auto key = component(path);
      const art::Anchor* anchor = nullptr;
      if (shot)
        for (const auto& a : shot->anchors)
          if (a.input == key) anchor = &a;
      const bool hidden = anchor && anchor->hidden && shot_matches;
      const art::Anchor* depth = shot_matches ? anchor : nullptr;
      if (show_shot) {
        if (!anchor) return std::nullopt;
        return Live{{anchor->x, anchor->y + half_dy(path)}, hidden, depth};
      }
      if (auto sp = spot_of(path)) return Live{*sp, hidden, depth};
      return std::nullopt;
    };
    if (image) {
      // A soft light behind the picture keeps black controllers apart from the card.
      const ImVec2 c{pic_x + pic_w * 0.5f, pic_y + pic_h * 0.5f};
      for (int ring = 16; ring >= 1; --ring)
        dl->AddCircleFilled(c, std::max(pic_w, pic_h) * 0.034f * float(ring), color::alpha(IM_COL32_WHITE, 0.0045f), 64);
    }
    if (SDL_Texture* texture = show_shot ? turned_texture(image, shot) : nullptr) {
      // A turned model may reach past the picture: it stays on its side of the card.
      const float out = stacked ? 0 : gutter * 0.5f;  // toward its rows, never the other controller
      dl->PushClipRect({room0.x - (hand ? 0 : out), room0.y - head_h}, {room1.x + (hand ? out : 0), room1.y}, true);
      draw_image(dl, ImTextureID(reinterpret_cast<uintptr_t>(texture)), {pic_x + shot->x0 * k, pic_y + shot->y0 * k},
                 {pic_x + shot->x1 * k, pic_y + shot->y1 * k});
      dl->PopClipRect();
    } else if (SDL_Texture* still = controller_texture(image)) {
      draw_image(dl, ImTextureID(reinterpret_cast<uintptr_t>(still)), {pic_x, pic_y}, {pic_x + pic_w, pic_y + pic_h});
    }
    draw_text(dl, fonts().semibold, kHeading,
              {std::round(hand ? rows_x + rows_w - measure(fonts().semibold, kHeading, "Right").x : rows_x),
               text_y(fonts().semibold, kHeading, at.y, s(24))},
              color::text, hand ? "Right" : "Left");

    // While the menu is being changed or pointed at, ring the buttons it uses
    // (on the right controller too, where its row has no callout).
    if (panel_open || lit_row == int(Quest::menu))
      for (const auto& [h, path] : menu.at)
        if (h == hand)
          if (auto live = live_of(path)) {
            const ImVec2 c{pic_x + live->at.x * k, pic_y + live->at.y * k};
            dl->AddCircleFilled(c, s(13), color::alpha(color::positive, live->hidden ? 0.12f : 0.25f), 32);
            dl->AddCircle(c, s(13), color::alpha(color::positive, live->hidden ? 0.5f : 1.f), 32, s(2.5f));
          }
    // Row columns, mirrored on the right: [label][input][mark] | [mark][input][label].
    const float label_x = hand ? rows_x + rows_w - lw : rows_x;
    const float drop_x = hand ? rows_x + mark_w + s(8) : rows_x + lw;
    const float mark_x = hand ? rows_x + mark_w * 0.5f : rows_x + rows_w - mark_w * 0.5f;
    const float edge = hand ? pic_x + pic_w + s(6) : pic_x - s(6);  // where callouts leave the picture
    const float dw_in = dw - s(8);
    float ry = at.y + head_h;
    for (size_t i = 0; i < rows.size(); ++i) {
      const int q = rows[i].q;
      ImGui::PushID(q);
      if (i) ry += row_h + line_h * float(row_lines(hand, rows[i - 1].q) - 1);
      const int lines = row_lines(hand, q);
      const float cy = ry + row_h * 0.5f;  // the first line; each further input one line_h lower
      if (i) hline(dl, rows_x, rows_x + rows_w, ry, color::alpha(color::stroke, 0.6f));
      ImGui::SetCursorScreenPos({rows_x, ry});
      ImGui::SetNextItemAllowOverlap();
      ImGui::InvisibleButton("##row", {rows_w, row_h + line_h * float(lines - 1)});
      // The row's dropdown sits on top of it: pointing at the dropdown lights the row too.
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem)) controls_hover_ = hand * kQuestCount + q;
      const bool lit_row_self = lit_row == hand * kQuestCount + q;
      const bool lit = lit_row_self;
      const bool dim = lit_row >= 0 && !lit;
      const ImU32 lit_tone = color::accent;
      const bool is_menu = q == int(Quest::menu);
      const bool lost = !is_menu && e.host[hand][q].empty();
      // A missing thumbrest only changes a hand pose; it is not worth a warning.
      const bool quiet = lost && !twin[hand][q] && q == int(Quest::thumbrest);
      const bool warn = twin[hand][q] || (lost && !quiet);
      const ImU32 tone = warn ? color::warning : is_menu ? color::positive : color::accent_text;
      // The callout: across from the input at its height to the picture's edge,
      // then to the row. Rows follow their spots' order, so lines never cross.
      // A changing row glows. Where the model stands in front of an input (by the
      // drawing's depth, live while it turns), its line goes on as faint dashes
      // from the silhouette in; an input the model hides gets an open dot.
      for (size_t pi = 0; pi < rows[i].paths.size(); ++pi) {
        const auto path = rows[i].paths[pi];
        // Each input's line ends at its own dropdown; the menu's all at its pill.
        const float cy = is_menu ? ry + row_h * 0.5f : ry + row_h * 0.5f + line_h * float(pi);
        const auto live = live_of(path);
        if (!live) continue;
        // Only the changing row's own lines glow. Another row's line to a button
        // the menu also uses stays as it is: it shows that row's binding, not the
        // menu's; the menu's buttons get their ring instead.
        const bool glow = lit_row_self;
        const bool faint = lit_row >= 0 && !glow;
        const ImVec2 a{pic_x + live->at.x * k, pic_y + live->at.y * k};
        const float rest = faint ? 0.28f : 0.6f;
        const ImU32 line = glow ? lit_tone : color::alpha(color::text2, live->hidden ? rest * 0.6f : rest);
        const float t = glow ? s(2.4f) : std::max(1.f, s(1.2f));
        const float end_x = mark_x + (hand ? -s(5) : s(5));
        const float bend = hand ? std::max(edge, a.x) : std::min(edge, a.x);
        const float foot = end_x + (hand ? -s(10) : s(10));
        const ImVec2 pts[4] = {a, {bend, a.y}, {foot, cy}, {end_x, cy}};
        const ImU32 behind = glow ? color::alpha(lit_tone, 0.45f) : color::alpha(color::text2, rest * 0.45f);
        auto lerp = [](ImVec2 p, ImVec2 q, float u) { return ImVec2{p.x + (q.x - p.x) * u, p.y + (q.y - p.y) * u}; };
        auto dashed = [&](ImVec2 from, ImVec2 to) {
          const float len = std::hypot(to.x - from.x, to.y - from.y), dash = s(3), space = s(4);
          for (float d0 = 0; d0 < len; d0 += dash + space)
            dl->AddLine(lerp(from, to, d0 / len), lerp(from, to, std::min(len, d0 + dash) / len), behind, t);
        };
        for (int seg = 0; seg < 3; ++seg) {
          const ImVec2 from = pts[seg], to = pts[seg + 1];
          // The spans the model covers, from the drawing's depth; without a drawing
          // of this view, a hidden input's line is covered as far as the picture.
          std::vector<std::pair<float, float>> covered;
          if (live->anchor)
            covered = art::covered_spans(*shot, *live->anchor, (from.x - pic_x) / k, (from.y - pic_y) / k,
                                         (to.x - pic_x) / k, (to.y - pic_y) / k);
          else if (live->hidden && seg < 2)
            covered = {{0.f, 1.f}};
          float u = 0;
          for (auto [c0, c1] : covered) {
            if (c0 > u) {
              if (glow) dl->AddLine(lerp(from, to, u), lerp(from, to, c0), color::alpha(lit_tone, 0.22f), t * 3.2f);
              dl->AddLine(lerp(from, to, u), lerp(from, to, c0), line, t);
            }
            dashed(lerp(from, to, c0), lerp(from, to, c1));
            u = c1;
          }
          if (u < 1) {
            if (glow) dl->AddLine(lerp(from, to, u), to, color::alpha(lit_tone, 0.22f), t * 3.2f);
            dl->AddLine(lerp(from, to, u), to, line, t);
          }
        }
        if (glow) dl->AddCircleFilled(a, s(10), color::alpha(lit_tone, live->hidden ? 0.14f : 0.28f), 24);
        dl->AddCircleFilled(a, s(5.5f), color::alpha(color::bg, live->hidden ? 0.6f : 0.9f), 20);
        if (live->hidden)
          dl->AddCircle(a, s(3.5f), glow ? lit_tone : color::alpha(IM_COL32_WHITE, faint ? 0.4f : 0.7f), 20, s(1.4f));
        else
          dl->AddCircleFilled(a, glow ? s(4.5f) : s(3.5f), glow ? lit_tone : color::alpha(IM_COL32_WHITE, faint ? 0.55f : 1.f), 20);
      }
      for (int line = 0; line < lines; ++line) {
        const float my = cy + line_h * float(line);
        if (lit) dl->AddCircleFilled({mark_x, my}, s(9), color::alpha(lit_tone, 0.25f), 20);
        dl->AddCircleFilled({mark_x, my}, s(5), lit ? lit_tone : warn || is_menu ? tone : color::alpha(color::text2, dim ? 0.45f : 0.8f), 16);
      }

      // The Quest input, and under it a warning only; the pill says the rest.
      std::string sub = is_menu         ? std::string()
                        : twin[hand][q] ? std::format("Shared with {}", quest_label(hand, twin[hand][q] - 1))
                        : lost && !quiet ? std::string("No button for it here")
                                         : std::string();
      // A one-line row that can take another input keeps the + spot beside its
      // dropdown free (shown on hover): a long label is ellipsized short of it.
      bool can_add = false;
      if (!is_menu && !locked && lines == 1)
        for (int i = 1; i <= n_inputs && !can_add; ++i) {
          const auto& in = p.inputs[size_t(i - 1)];
          can_add = !in.reserved && controls::on_hand(in, hand) && controls::is_axes(in.path) == (q == int(Quest::stick)) &&
                    !std::ranges::contains(e.host[hand][q], i);
        }
      const float label_room = lw - (can_add ? s(6) + s(26) + s(4) : 0.f);
      auto place = [&](ImFont* font, float size, std::string_view full, float y, ImU32 col) {
        const auto t = ellipsize(font, size, full, label_room);
        const float x = hand ? label_x + lw - measure(font, size, t).x : label_x;
        draw_text(dl, font, size, {std::round(x), y}, col, t);
      };
      const auto label = quest_label(hand, q);
      place(fonts().semibold, kBody, label, sub.empty() ? text_y(fonts().semibold, kBody, ry, row_h) : ry + s(7),
            color::text);
      if (!sub.empty()) place(fonts().regular, kLabel, sub, ry + s(28), color::warning);

      ImGui::SetCursorScreenPos({drop_x, ry + (row_h - s(38)) * 0.5f});
      if (is_menu) {
        // The menu row's own list holds every way to open the menu.
        const ImVec2 f = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##menu-pill", {dw_in, s(38)});
        const bool open = ImGui::IsPopupOpen("##menu-list");
        const bool pill_hover = ImGui::IsItemHovered();
        if (open || panel_open || ImGui::IsItemFocused()) controls_active_ = hand * kQuestCount + q;
        if (ImGui::IsItemClicked()) ImGui::OpenPopup("##menu-list");
        if (pill_hover && !open) tooltip(menu.label);
        if ((options_.smoke_view == "controls-menu" || options_.smoke_view == "controls-pico-menu") && !base) {
          static int frames = 0;
          if (frames++ == 3) ImGui::OpenPopup("##menu-list");
        }
        if (options_.smoke_view == "controls-hold" && !base) {
          static int frames = 0;
          if (frames++ == 3) {
            e.menu_mode = int(MenuMode::hold);
            e.menu_input = host_index(p, "/input/b/click");
            e.menu_hold_ms = 1000;
            controls_menu_detail_ = 1;
          }
        }
        // --smoke-view controls-shared: the menu on the left stick click alone,
        // which Stick click also uses, for the shared-button note.
        if (options_.smoke_view == "controls-shared" && !base) {
          static int frames = 0;
          if (frames++ == 3) {
            e.menu_mode = int(MenuMode::button);
            e.menu_input = host_index(p, "/input/thumbstick/click");
          }
        }
        if (options_.smoke_view.starts_with("controls-chord") && !base) {
          static int frames = 0;
          if (frames++ == 3) {
            e.menu_mode = int(MenuMode::chord);
            e.menu_first = host_index(p, "/input/thumbstick/click");
            e.menu_second = n_inputs + host_index(p, "/input/b/click");
            if (options_.smoke_view == "controls-chord") controls_menu_detail_ = 2;
          }
        }
        const bool automatic = mode == MenuMode::automatic;
        const ImU32 bg = automatic ? color::alpha(color::positive, pill_hover || open ? 0.22f : 0.14f)
                                   : (pill_hover || open ? color::surface_top : color::surface_hi);
        dl->AddRectFilled(f, {f.x + dw_in, f.y + s(38)}, bg, s(19));
        const auto shown = ellipsize(fonts().semibold, kSmall + 1, menu.label, dw_in - s(46));
        const ImVec2 ls = measure(fonts().semibold, kSmall + 1, shown);
        draw_text(dl, fonts().semibold, kSmall + 1,
                  {std::round(f.x + std::max(s(15), (dw_in - ls.x) * 0.5f - s(6))),
                   text_y(fonts().semibold, kSmall + 1, f.y, s(38))},
                  automatic ? color::positive : mode == MenuMode::off ? color::text3 : color::text, shown);
        draw_glyph(dl, icons::chevron_down, kLabel, {f.x + dw_in - s(20), f.y + s(19)},
                   automatic ? color::positive : color::text2);
        if (!locked) {
          // The list: Automatic, single buttons, hold, two buttons, off.
          ImGui::SetNextWindowPos({f.x, f.y + s(44)});
          ImGui::SetNextWindowSize({std::max(dw_in, s(300)), 0});
          if (begin_menu("##menu-list")) {
            auto item = [&](const std::string& t, bool checked) {
              const ImVec2 r = ImGui::GetCursorScreenPos();
              const bool hit = menu_item(t.c_str());
              if (checked)
                draw_glyph(ImGui::GetWindowDrawList(), icons::check, kSmall,
                           {r.x + ImGui::GetContentRegionAvail().x - s(18), r.y + s(19)}, color::accent_text);
              return hit;
            };
            ControlsEdit automatic_edit = e;
            automatic_edit.menu_mode = int(MenuMode::automatic);
            if (item(std::format("Automatic ({})##auto", resolved_menu(automatic_edit).label), automatic))
              e.menu_mode = int(MenuMode::automatic);
            menu_section("ONE BUTTON");
            ImGui::BeginChild("##buttons", {0, std::min(s(40) * float(2 * n_inputs), s(240))}, 0,
                              ImGuiWindowFlags_NoBackground);
            for (int c = 1; c <= 2 * n_inputs; ++c) {
              const auto choice = menu_choice(p, c);
              if (!is_button(choice)) continue;
              ImGui::PushID(c);
              if (item(choice_label(choice), mode == MenuMode::button && e.menu_input == c)) {
                e.menu_mode = int(MenuMode::button);
                e.menu_input = c;
              }
              ImGui::PopID();
            }
            ImGui::EndChild();
            menu_section("MORE");
            if (item("Hold a button...##hold", mode == MenuMode::hold)) {
              e.menu_mode = int(MenuMode::hold);
              controls_menu_detail_ = 1;
            }
            if (item("Press two buttons together...##chord", mode == MenuMode::chord)) {
              e.menu_mode = int(MenuMode::chord);
              controls_menu_detail_ = 2;
            }
            if (item("Off##off", mode == MenuMode::off)) e.menu_mode = int(MenuMode::off);
            end_menu();
          }
          // The compact picker for hold and two buttons.
          if (controls_menu_detail_) {
            ImGui::OpenPopup("##menu-detail");
            controls_menu_kind_ = controls_menu_detail_;
            controls_menu_detail_ = 0;
          }
          // A popover under the Menu pill: the button (or the pair) and the hold
          // time, applied as they change; a click outside closes it.
          // Under the pill, from its left edge into the gutter: the row labels stay visible.
          ImGui::SetNextWindowPos({f.x, f.y + s(44)});
          ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {s(14), s(12)});
          ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, s(12));
          ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, s(1));
          ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {s(8), 0});
          ImGui::PushStyleColor(ImGuiCol_PopupBg, color::surface);
          ImGui::PushStyleColor(ImGuiCol_Border, color::stroke_hi);
          if (ImGui::BeginPopup("##menu-detail")) {
            controls_menu_panel_ = true;
            const bool two = controls_menu_kind_ == 2;
            std::vector<std::string> names;
            std::vector<int> ids;
            for (int c = 1; c <= 2 * n_inputs; ++c)
              if (is_button(menu_choice(p, c))) names.push_back(choice_label(menu_choice(p, c))), ids.push_back(c);
            std::vector<const char*> items;
            for (auto& nm : names) items.push_back(nm.c_str());
            // As wide as the longest button name needs, within reason and the window;
            // a name still too long is ellipsized inside its pill.
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const float room = vp->WorkPos.x + vp->WorkSize.x - f.x - s(14) * 2 - s(16);
            const float fw = std::max(dw_in + s(12), std::min({dropdown_width("", items.data(), int(items.size())),
                                                               s(320), room}));
            text(fonts().semibold, kSmall, color::text, two ? "Press two buttons together" : "Hold a button", false);
            gap(8);
            auto pick = [&](const char* id, int* value) {
              int at_i = int(std::ranges::find(ids, *value) - ids.begin());
              if (at_i >= int(ids.size())) at_i = 0;
              if (dropdown(id, "", items.data(), int(items.size()), &at_i, fw)) *value = ids[size_t(at_i)];
            };
            if (two) {
              pick("first", &e.menu_first);
              // The pair reads as one unit: [first] + [second].
              const ImVec2 at_plus = ImGui::GetCursorScreenPos();
              const float ph = s(24);
              draw_glyph(ImGui::GetWindowDrawList(), icons::add, kBody, {at_plus.x + fw * 0.5f, at_plus.y + ph * 0.5f},
                         color::text);
              ImGui::Dummy({fw, ph});
              pick("second", &e.menu_second);
            } else {
              pick("button", &e.menu_input);
            }
            gap(10);
            text(fonts().regular, kLabel, color::text3, two ? "Hold both for" : "Hold for", false);
            gap(4);
            // Two buttons may open at once ("None"); a held button needs a hold (0 is just one button).
            static constexpr int kChordHolds[] = {0, 500, 1000, 2000};
            static const char* kChordHoldNames[] = {"None", "0.5 s", "1 s", "2 s"};
            const int* holds = two ? kChordHolds : kChordHolds + 1;
            const char** hold_names = two ? kChordHoldNames : kChordHoldNames + 1;
            const int count = two ? 4 : 3;
            int& ms = two ? e.menu_chord_ms : e.menu_hold_ms;
            int hold = int(std::find(holds, holds + count, ms) - holds);
            if (segmented("hold", hold_names, count, &hold, fw)) ms = holds[std::min(hold, count - 1)];
            ImGui::EndPopup();
          }
          ImGui::PopStyleColor(2);
          ImGui::PopStyleVar(4);
        }
        if (twin[hand][q])
          notes[hand].push_back({std::format("Menu and {} share a button; {} sees those presses too.",
                                             quest_label(hand, twin[hand][q] - 1),
                                             game ? std::string(game->name) : std::string("the game"))});
        if (mode == MenuMode::off) notes[hand].push_back({"Games get no menu button.", false});
      } else {
        // One dropdown per host input of the row, stacked. Each lists the inputs this
        // row can take on this hand (a stick only on a stick or pad) that the row's
        // other dropdowns do not hold; the last one can add another input, and with
        // several each can remove its own (as does the x beside the extra ones).
        auto& row = e.host[hand][q];
        auto eligible = [&](int i) {
          const auto& in = p.inputs[size_t(i - 1)];
          return controls::on_hand(in, hand) && controls::is_axes(in.path) == (q == int(Quest::stick));
        };
        auto unused = [&]() {
          for (int i = 1; i <= n_inputs; ++i)
            if (eligible(i) && !p.inputs[size_t(i - 1)].reserved && !std::ranges::contains(row, i)) return i;
          return 0;
        };
        constexpr int kAdd = -1;
        ImGui::BeginDisabled(locked);
        std::optional<int> remove;  // a line taken out after the loop
        for (int line = 0; line < lines; ++line) {
          ImGui::PushID(line);
          const float ly = ry + (row_h - s(38)) * 0.5f + line_h * float(line);
          const int current = line < int(row.size()) ? row[size_t(line)] : 0;
          std::vector<std::string> names{lines > 1 ? "Remove" : "Not assigned"};
          std::vector<int> ids{0};
          for (int i = 1; i <= n_inputs; ++i) {
            if (!eligible(i) || (i != current && std::ranges::contains(row, i))) continue;
            const auto& in = p.inputs[size_t(i - 1)];
            names.emplace_back(in.reserved ? "System (kept by the runtime)" : in.label);
            ids.push_back(i);
          }
          if (line == lines - 1 && current && unused()) {
            names.emplace_back("Add another input");
            ids.push_back(kAdd);
          }
          std::vector<const char*> items;
          for (auto& nm : names) items.push_back(nm.c_str());
          int pick = int(std::ranges::find(ids, current) - ids.begin());
          if (pick >= int(ids.size())) pick = 0;
          if (!locked && controls_open_line_ == std::array{hand, q, line}) {
            ImGui::PushID("host");
            ImGui::OpenPopup("##menu");
            ImGui::PopID();
            controls_open_line_.reset();
          }
          ImGui::SetCursorScreenPos({drop_x, ly});
          if (dropdown("host", "", items.data(), int(items.size()), &pick, dw_in)) {
            const int id = ids[size_t(pick)];
            if (id == kAdd) {
              // The next free input, its list opened for the player's pick.
              row.push_back(unused());
              controls_open_line_ = std::array{hand, q, int(row.size()) - 1};
            } else if (id == 0) {
              if (current) remove = line;
            } else if (!p.inputs[size_t(id - 1)].reserved) {
              if (current) row[size_t(line)] = id;
              else row.push_back(id);
            }
          }
          ImGui::PushID("host");
          const bool list_open = ImGui::IsPopupOpen("##menu");
          ImGui::PopID();
          if (list_open || ImGui::IsItemFocused()) controls_active_ = hand * kQuestCount + q;
          // Small buttons in the label column, outward from the dropdown: an extra
          // input's remove x, then on the row's last line the + while a free input is left.
          const float bs = 26;
          // The x and the + show only while the row is pointed at or being changed;
          // their spots stay reserved (the label never reaches them), so nothing moves on hover.
          int slot = 0;
          auto small_button = [&](const char* id, char32_t glyph, const char* tip, bool hover_shot, bool shown) {
            const float off = s(6) + float(slot++) * (s(bs) + s(4));
            const ImVec2 at_b{hand ? drop_x + dw_in + off : drop_x - off - s(bs), ly + (s(38) - s(bs)) * 0.5f};
            // Screenshots: the pointer on the left one.
            if (hover_shot && hand == 0) ImGui::GetIO().AddMousePosEvent(at_b.x + s(bs) * 0.5f, at_b.y + s(bs) * 0.5f);
            if (!shown) return false;
            ImGui::SetCursorScreenPos(at_b);
            const bool hit = icon_button(id, glyph, tip, bs);
            if (ImGui::IsItemHovered()) controls_hover_ = hand * kQuestCount + q;
            return hit;
          };
          // Both small buttons show only while the row is pointed at or being changed.
          const bool row_lit = hover == hand * kQuestCount + q || active == hand * kQuestCount + q;
          if (line > 0 && small_button("remove", icons::cancel, "Remove this input",
                                       options_.smoke_view.ends_with("-remove-hover"), row_lit))
            remove = line;
          if (line == lines - 1 && !locked && unused() &&
              small_button("add", icons::add, "Add another input",
                           (options_.smoke_view.ends_with("-plus-hover") && q == int(Quest::stick_click)) ||
                               (options_.smoke_view.ends_with("-plus-hover-thumbrest") && q == int(Quest::thumbrest)),
                           row_lit)) {
            row.push_back(unused());
            controls_open_line_ = std::array{hand, q, int(row.size()) - 1};
          }
          ImGui::PopID();
        }
        if (remove && *remove < int(row.size())) row.erase(row.begin() + *remove);
        if (options_.smoke_view == "controls-edit" && hand == 0 && q == int(Quest::secondary))
          controls_active_ = hand * kQuestCount + q;
        ImGui::EndDisabled();
      }
      ImGui::PopID();
    }
    ImGui::PopID();
    return std::max(rows_h, pic_y + pic_h - at.y);
  };
  auto draw_notes = [&](int hand, float x, float y) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < notes[hand].size(); ++i) {
      // Icon and text share one 20 px line, both centred on it.
      const float top = y + s(22) * float(i);
      const auto& note = notes[hand][i];
      draw_glyph(dl, note.warning ? icons::warning : icons::info, kSmall, {x + s(8), top + s(10)},
                 note.warning ? color::warning : color::text2);
      draw_text(dl, fonts().regular, kSmall, {x + s(24), text_y(fonts().regular, kSmall, top, s(20))}, color::text,
                note.text);
    }
    return notes[hand].empty() ? 0.f : s(12) + s(22) * float(notes[hand].size());
  };

  if (!stacked) {
    begin_card("controllers", w, {22, 20});
    toolbar();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float h = std::max(draw_hand(0, at), draw_hand(1, at));
    const float nh = std::max(draw_notes(0, at.x, at.y + h + s(8)), draw_notes(1, at.x + half + mid, at.y + h + s(8)));
    ImGui::SetCursorScreenPos(at);
    ImGui::Dummy({inner, h + nh});
    end_card();
  } else {
    for (int hand = 0; hand < 2; ++hand) {
      if (hand) gap(16);
      begin_card(hand ? "controller-right" : "controller-left", w, {22, 20});
      if (!hand) toolbar();
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const float h = draw_hand(hand, at);
      const float nh = draw_notes(hand, at.x, at.y + h + s(8));
      ImGui::SetCursorScreenPos(at);
      ImGui::Dummy({inner, h + nh});
      end_card();
    }
  }
}

}  // namespace venture::launcher
