#include "launcher/app.h"
#include "launcher/ui/rich_text.h"
#include "launcher/ui/style.h"
#include "launcher/window_style.h"
#include "common/environment.h"
#include "common/http/client.h"
#include "common/json.h"
#include "common/log.h"
#include "common/saves_migration.h"

#include <algorithm>
#include <functional>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <format>
#include <imgui_stdlib.h>
#include <unordered_map>

namespace venture::launcher {
using namespace ui;
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
constexpr float kHeaderHeight = 40, kAfterHeader = 22;
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

// Centered empty state: glyph, headline, one line of guidance.
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
  ImVec2 ss = measure(fonts().regular, kBody, sub);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (w - ss.x) * 0.5f));
  text(fonts().regular, kBody, color::text2, sub, false);
}

// Settings-style row inside a card: a one-line title left, centred on the
// control right-aligned. The description shows when the title is hovered.
template <class Control>
void setting_row(std::string_view title_text, std::string_view description, float control_w, float control_h,
                 Control control) {
  float avail = ImGui::GetContentRegionAvail().x;
  float left = std::max(s(120), avail - control_w - s(24));
  float pad = s(12), spacing = ImGui::GetStyle().ItemSpacing.y;
  ImVec2 start = ImGui::GetCursorPos(), screen = ImGui::GetCursorScreenPos();
  const auto title = ellipsize(fonts().regular, kBody, title_text, left);
  ImVec2 ts = measure(fonts().regular, kBody, title);
  float row_h = std::max({ts.y, control_h, s(38)});  // every row the same height
  draw_text(ImGui::GetWindowDrawList(), fonts().regular, kBody,
            {std::round(screen.x), text_y(fonts().regular, kBody, screen.y + pad, row_h)}, color::text, title);
  if (!description.empty() && ImGui::IsWindowHovered() &&
      ImGui::IsMouseHoveringRect({screen.x, screen.y}, {screen.x + left, screen.y + pad * 2 + row_h}))
    tooltip(description);
  ImGui::SetCursorPos({start.x + avail - control_w, start.y + pad + (row_h - control_h) * 0.5f});
  ImGui::BeginGroup();
  control();
  ImGui::EndGroup();
  ImGui::SetCursorPos({start.x, start.y + pad + row_h + pad - spacing});
  ImGui::Dummy({0, 0});
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
  gap(22);
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
    }
  draw_uninstall();
  draw_save_import();
  draw_cancel_download();
  draw_report();
  draw_update();
  draw_remove_account();
  draw_install_ask();
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
    if (ImGui::IsItemHovered()) dl->AddRectFilled(a, {a.x + button.x, a.y + button.y}, hover);
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
  int width = 0, height = 0;
  SDL_GetWindowSize(window_, &width, &height);
  chrome_ = {width, height, maximized, s(40), s(6),
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
  ImGui::SetNextWindowPos(pos);
  ImGui::SetNextWindowSize(size);
  ImGui::SetNextWindowBgAlpha(0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
  ImGui::Begin("##toast", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  ImGui::PopStyleVar();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetWindowPos();
  ImVec2 q{p.x + size.x, p.y + size.y};
  soft_shadow(dl, p, q, s(12), s(16), 0.6f * shown);
  dl->AddRectFilled(p, q, color::alpha(color::surface_top, shown), s(12));
  draw_text(dl, fonts().regular, kBody, {std::round(p.x + s(20)), std::round(p.y + s(15))}, color::alpha(color::text, shown),
            last, max_w - s(84));
  ImGui::SetCursorScreenPos({q.x - s(46), p.y + (size.y - s(32)) * 0.5f});
  if (icon_button("dismiss-toast", icons::cancel, nullptr, 32)) notice_.clear();
  bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
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
  const Nav nav[] = {{"Library", icons::library, 0},
                     {"Downloads", icons::download, 1},
                     {"Import", icons::package, 4},
                     {"Settings", icons::settings, 2}};
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
    std::string name = shown ? account_name(*shown) : "Sign in";
    draw_text(dl, fonts().semibold, kSmall + 1, {std::round(tx), text_y(fonts().semibold, kSmall + 1, c.y - s(9))},
              color::text, ellipsize(fonts().semibold, kSmall + 1, name, w - s(64)));
    std::string state = !session_          ? "Connect your Meta account"
                        : options_.offline ? "Offline"
                        : accounts_.size() > 1 ? std::format("{} accounts", accounts_.size())
                                               : "Meta account";
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

  // Header: title left; search, sort and refresh right.
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
  if (!all.empty() || !search_.empty()) {
    // Right of the title when it fits, else on its own row beneath it.
    const char* sorts[] = {"Name", "Recently played"};
    float content_w = ImGui::GetContentRegionAvail().x;
    float title_w = measure(fonts().semibold, kDisplay, "Library").x + s(warning_.empty() ? 24 : 54);
    float fixed = s(10) + sort_w + s(10) + s(38);
    bool beside = content_w - title_w - fixed >= s(200);
    float search_w = std::min(s(260), (beside ? content_w - title_w : content_w) - fixed);
    float toolbar_w = search_w + fixed;
    float x = ImGui::GetWindowContentRegionMax().x - toolbar_w;
    float y = beside ? top + s(kHeaderHeight - 38) * 0.5f : after_title + s(10);
    if (!beside) after_title = y + s(38);
    ImGui::SetCursorPos({beside ? x : ImGui::GetWindowContentRegionMin().x, y});
    search_field("search", "Search", &search_, search_w);
    ImGui::SameLine(0, s(10));
    dropdown("sort", "Sort by: ", sorts, 2, &sort_, sort_w);
    ImGui::SameLine(0, s(10));
    if (refreshing_) {
      ImVec2 p = ImGui::GetCursorScreenPos();
      ImGui::Dummy({s(38), s(38)});
      ImGui::SetCursorScreenPos({p.x + s(10), p.y + s(10)});
      spinner(9, color::text2);
    } else if (icon_button("refresh", icons::refresh, "Refresh library", 38, session_ && !options_.offline, true)) {
      refresh();
    }
  }
  ImGui::SetCursorPosY(after_title + s(kAfterHeader) - s(6));

  // Meta could not list, or did not confirm, this account's games: one it owns
  // can still be added by its store link, checked on that game's own listing.
  // --smoke-view link shows it offline for screenshots.
  const bool add_owned_shown = (session_ && (!refresh_error_.empty() || library_partial_) && !refreshing_ &&
                                !options_.offline && search_.empty() && !installed_only_) ||
                               options_.smoke_view == "link";
  auto add_owned_field = [&](float width) {
    if (submit_field("owned-link", "Add a game by store link", &owned_link_, width, icons::add, !adding_))
      add_owned();
    if (ImGui::IsItemHovered()) tooltip("Meta could not list all your games. Paste the store link of one you own.");
  };

  // Filter chips, and the store-link field at the row's end.
  if (!all.empty()) {
    auto installed_count = std::ranges::count_if(all, [&](auto& g) { return installed(g.id) != nullptr; });
    std::string all_label = std::format("All  {}##all", all.size());
    std::string installed_label = std::format("Installed  {}##installed", installed_count);
    const float row_x = ImGui::GetCursorPosX(), row_w = ImGui::GetContentRegionAvail().x;
    if (filter_chip(all_label.c_str(), !installed_only_)) installed_only_ = false;
    ImGui::SameLine(0, s(8));
    if (filter_chip(installed_label.c_str(), installed_only_)) installed_only_ = true;
    if (add_owned_shown) {
      // Under the sort and refresh controls, as wide as both.
      const float fw = std::min(sort_w + s(10) + s(38), row_w * 0.5f);
      ImGui::SameLine(row_x + row_w - fw);
      add_owned_field(fw);
    }
    gap(18);
  }

  auto query = lowercase(search_);
  std::erase_if(all, [&](auto& g) {
    return (!query.empty() && lowercase(g.name).find(query) == std::string::npos) ||
           (installed_only_ && !installed(g.id));
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
      empty_state(icons::library, "Finding your games\xE2\x80\xA6", "Reading your Meta library.");
    else if (!search_.empty())
      empty_state(icons::search, "No results", "Try a different name, or clear the search.");
    else if (installed_only_)
      empty_state(icons::download, "Nothing installed yet", "Install a game from your library to play it here.");
    else if (!session_)
      empty_state(icons::library, "", "Sign in with the Meta account that owns your Quest games.");
    else
      empty_state(icons::library, "No games yet",
                  refresh_error_.empty() ? "Games you own on Meta Quest will appear here."
                                         : refresh_error_);
    if (session_ && !refreshing_ && search_.empty() && !installed_only_ && !options_.offline) {
      gap(22);
      float bw = s(200);
      ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - bw) * 0.5f + ImGui::GetCursorPosX());
      if (button("Refresh library", 0, Tone::primary, {bw, s(44)})) refresh();
      if (add_owned_shown) {
        gap(16);
        const float fw = s(260);
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - fw) * 0.5f + ImGui::GetCursorPosX());
        add_owned_field(fw);
      }
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
  start = {std::round(start.x), std::round(start.y)};
  ImDrawList* gdl = ImGui::GetWindowDrawList();
  for (size_t i = 0; i < all.size(); ++i) {
    auto& g = all[i];
    int col = int(i % columns), row = int(i / columns);
    ImVec2 p{std::round(start.x + col * (card_w + gap_x)), std::round(start.y + row * (card_h + gap_y))};
    ImGui::SetCursorScreenPos(p);
    ImGui::PushID(g.id.c_str());
    bool clicked = ImGui::InvisibleButton("card", {card_w, card_h});
    bool hovered = ImGui::IsItemHovered();
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
        if (over) tooltip(can_play ? "Play" : "Install");
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
      draw_text(gdl, fonts().regular, kSmall, {p.x, sy}, status_col, ellipsize(fonts().regular, kSmall, status, card_w));
    }
    if (clicked) select(g.id);
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
    // Files and the overflow menu sit beside the primary button (not mid-download).
    bool tools = partial && !downloading;
    ImVec2 cta{inner - (tools ? s(52) * 2 : 0), s(46)}, more{};
    auto draw_tools = [&] {
      if (!tools) return;
      ImGui::SameLine(0, s(6));
      if (icon_button("folder", icons::folder, "Show files", 46, true, true))
        action([&] { open_folder(partial->directory); });
      ImGui::SameLine(0, s(6));
      more = ImGui::GetCursorScreenPos();
      // --smoke-view menu opens it once for screenshots.
      static bool smoke_menu = options_.smoke_view == "menu";
      if (icon_button("more", icons::more, "More", 46, true, true) || std::exchange(smoke_menu, false))
        ImGui::OpenPopup("game-menu");
    };
    if (local) {
      if (launch_choice(g.id, play_key(g.id)).status == library::LaunchStatus::reconnect) {
        if (button("Reconnect to play##play", 0, Tone::primary, cta)) page_ = 3;
      } else if (running) {
        // Quits as the headset's menu would: the game gets its Android lifecycle and saves first.
        auto& r = running_[g.id];
        if (button(r.quitting ? "Quitting...##play" : "Quit game##play", r.quitting ? 0 : icons::cancel,
                   Tone::secondary, cta, !r.quitting))
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
      if (icon_button("show-downloads", icons::download, "Show in Downloads", 46, true, true)) page_ = 1;
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
      if (!stacked && rows_h > 0) at.y = std::max(at.y, bottom - rows_h);
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
      storage->SetFloat(rows_id, ImGui::GetItemRectSize().y);
    }
    if (!stacked && ImGui::GetCursorScreenPos().y < bottom)
      ImGui::Dummy({0, bottom - ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y});
    if (tools) {
      ImGui::SetNextWindowPos({more.x + s(46), more.y + s(52)}, ImGuiCond_Appearing, {1, 0});
      if (begin_menu("game-menu")) {
        if (menu_item("Check for updates", icons::refresh, false, session_ && !options_.offline)) {
          details_.erase(g.id);
          select(g.id);
        }
        if (steam_sync_allowed_ && settings_.add_games_to_steam) {
          bool in = in_steam(g.id);
          if (menu_item(in ? "Remove from Steam" : "Add to Steam", 0, false, true))
            action([&] { set_in_steam(g.id, !in); });
        }
        // The game's files on its shared storage (Android/data/<package>), where
        // homebrew ports look for the game data the player provides.
        if (menu_item("Show game data", icons::folder, false, true))
          action([&] {
            const auto key = launch_choice(g.id, play_key(g.id)).account_key;
            auto folder = account_data_dir(data_, key.empty() ? "local" : key, partial->plan.package) / "external-data";
            fs::create_directories(folder);
            open_folder(folder);
          });
        if (menu_item("Import save data", icons::save, false, !running)) save_import_target_ = g.id;
        if (menu_item("Uninstall", icons::trash, true, !running && !downloads_->busy())) uninstall_target_ = *partial;
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
  auto addon_list = addons(g.id);
  bool has_addons = !addon_list.empty() || checking_addons;
  std::string addon_tab = addon_list.empty() ? std::string("Add-ons") : std::format("Add-ons  {}", addon_list.size());
  const char* tab_items[] = {"About", addon_tab.c_str()};
  if (!has_addons) game_tab_ = 0;
  if (has_addons) {
    tabs("game-tabs", tab_items, 2, &game_tab_, avail);
    gap(20);
  } else {
    text(fonts().semibold, kTitle, color::text, "About", false);
    gap(8);
  }
  float body_w = std::min(ImGui::GetContentRegionAvail().x, avail);
  if (game_tab_ == 1 && has_addons)
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
  ImGui::SetNextWindowPos({p.x + w, p.y + h + s(6)}, ImGuiCond_Always, {1, 0});  ImGui::SetNextWindowPos({p.x, p.y + h + s(6)});
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
    if (icon_button("refresh-addons", icons::refresh, "Check add-ons again", 36, session_ && !options_.offline, true)) {
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
    if (ImGui::IsItemHovered() && !a.description.empty()) tooltip(a.description);
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
      if (icon_button("remove", icons::trash, running ? "Close the game to remove add-ons" : "Remove add-on files", 36,
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
    empty_state(icons::download, "No downloads", "Games you install will show up here.");
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
      if (icon_button("clear", icons::cancel, "Clear from list", 36))
        action([&] { downloads_->remove(j, false); });
      draw_glyph(dl, icons::check, kBody + 2, {right - s(58), p.y + h * 0.5f}, color::positive);
    } else {
      if (icon_button("cancel", icons::cancel, "Cancel download", 36)) cancel_target_ = j;
      ImGui::SetCursorScreenPos({right - s(76), by});
      if (active) {
        if (icon_button("pause", icons::pause, "Pause  \xC2\xB7  keeps downloaded files", 36, true, true))
          downloads_->cancel(j.app_id);
      } else if (icon_button("resume", icons::resume, "Resume", 36, session_ && !options_.offline, true)) {
        action([&] { downloads_->resume(j, session_->access_token); });
      }
      ImGui::SetCursorScreenPos({right - s(116), by});
      if (icon_button("later", icons::chevron_down, "Move down", 36, index + 1 < jobs.size()))
        downloads_->move(j, 1);
      ImGui::SetCursorScreenPos({right - s(156), by});
      if (icon_button("sooner", icons::chevron_up, "Move up", 36, index > 0)) downloads_->move(j, -1);
    }
    if (index + 1 < jobs.size()) hline(dl, p.x, right, p.y + h);
    ImGui::SetCursorScreenPos({p.x, p.y + h + s(1)});
    ImGui::Dummy({row_w, 0});
    ImGui::PopID();
  }
  end_card();
  gap(12);
  text(fonts().regular, kSmall, color::text3,
       "Queued downloads start in list order. Paused and interrupted downloads resume where they left off.", false);
}

// ---------------------------------------------------------------------------
// Changes apply as they are made; text fields save when editing ends.
void App::draw_settings() {
  page_title("Settings");
  gap(kAfterHeader);
  float w = std::min(ImGui::GetContentRegionAvail().x, s(kPageWidth));
  auto input = [&](const char* id, std::string* value, const char* hint) {
    return [=, this] {
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {s(14), text_y(ImGui::GetFont(), kBody, 0, s(38))});
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, s(10));
      ImGui::PushStyleColor(ImGuiCol_FrameBg, color::surface_hi);
      ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, color::surface_top);
      ImGui::SetNextItemWidth(std::min(s(380), w * 0.48f));
      ImGui::InputTextWithHint(id, hint, value);
      if (ImGui::IsItemDeactivatedAfterEdit()) action([&] { save_settings(); });
      ImGui::PopStyleColor(2);
      ImGui::PopStyleVar(2);
    };
  };
  float field_w = std::min(s(380), w * 0.48f);

  heading("Appearance");
  begin_card("appearance", w, {22, 2});
  {
    static const char* sizes[] = {"100%", "115%", "130%", "150%"};
    static constexpr int percents[] = {100, 115, 130, 150};
    int choice = int(std::ranges::find(percents, settings_.interface_size) - std::begin(percents));
    if (choice == 4) choice = 0;
    float dw = s(180);
    setting_row("Interface size", "Makes text and everything else in Venture larger.", dw, s(38), [&] {
      if (dropdown("interface-size", "", sizes, 4, &choice, dw))
        action([this, size = percents[choice]] {
          settings_.interface_size = size;
          save_settings();
          fit_window();
          theme();
        });
    });
  }
  ImGui::Dummy({0, 0});  // the last row keeps its bottom padding
  end_card();
  gap(20);

  heading("Library");
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

  heading("Runtime");
  begin_card("runtime", w, {22, 2});
  setting_row("Runtime", "Leave empty to use the runtime that came with Venture.", field_w, s(38),
              input("##runtime", &settings_runtime_, "Included runtime"));
  divider();
  setting_row("Show runtime console", "Opens a console window with live output when a game starts.", s(44), s(24), [&] {
    if (toggle("console", &settings_.console)) action([&] { save_settings(); });
  });
  divider();
  {
    const char* modes[] = {"Automatic", "Always", "Off"};
    float dw = s(180);
    setting_row("Menu from both stick clicks",
                "Hold both sticks in for half a second to open the game's menu. Automatic: only for a controller whose VR runtime gives the game no menu button.",
                dw, s(38), [&] {
      if (dropdown("menu", "", modes, 3, &settings_.menu_button, dw)) action([&] { save_settings(); });
    });
  }
  divider();
  {
    const char* modes[] = {"Always", "Automatic", "Off"};
    float dw = s(180);
    setting_row("Compress textures",
                "Stores game textures at a quarter of the GPU memory, with a small loss of detail. Automatic: "
                "only once the GPU memory in use passes a threshold.",
                dw, s(38), [&] {
      if (dropdown("textures", "", modes, 3, &settings_.texture_compression, dw)) action([&] { save_settings(); });
    });
  }
  if (settings_.texture_compression == 1) {
    divider();
    static const char* shares[] = {"25%", "50%", "75%", "90%"};
    static constexpr int percents[] = {25, 50, 75, 90};
    int choice = int(std::ranges::find(percents, settings_.texture_compression_threshold) - std::begin(percents));
    if (choice == 4) choice = 1;
    float dw = s(180);
    setting_row("Compression threshold",
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
  setting_row("Check for updates", "Venture tells you when a new version is out.", s(44), s(24), [&] {
    if (toggle("updates", &settings_.check_updates)) action([&] { save_settings(); });
  });
  ImGui::Dummy({0, 0});  // the last row keeps its bottom padding
  end_card();
  gap(20);

  heading("Troubleshooting");
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

  heading("About");
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

// ---------------------------------------------------------------------------
void App::draw_account() {
  page_title("Accounts");
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + s(2));
  text(fonts().regular, kBody, color::text2, "Each Meta account keeps its own library, downloads and saves on this PC.",
       false);
  gap(kAfterHeader);
  float w = std::min(ImGui::GetContentRegionAvail().x, s(kPageWidth));
  ImFont* rf = fonts().regular;
  ImFont* sf = fonts().semibold;
  if (accounts_.empty()) {
    begin_card("connect", w, {28, 28});
    if (SDL_Texture* mark = logo(int(std::ceil(s(48))))) {
      ImVec2 p = ImGui::GetCursorScreenPos();
      draw_image(ImGui::GetWindowDrawList(), ImTextureID(reinterpret_cast<uintptr_t>(mark)), p,
                                           {p.x + s(48), p.y + s(48)});
      ImGui::Dummy({s(48), s(48)});
      gap(14);
    }
    text(sf, kTitle, color::text, "Connect your Meta account", false);
    gap(4);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - s(56));
    text(rf, kBody, color::text2,
         "Sign in with the account that owns your Quest games. Venture shows your library and downloads the games "
         "you own \xE2\x80\x94 nothing else.");
    ImGui::PopTextWrapPos();
    gap(22);
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
      if (icon_button("more", icons::more, "More", 36)) ImGui::OpenPopup("account-menu");
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
    gap(10);
    ImGui::BeginGroup();
    if (signing_in_) {
      spinner(7, color::text2);
      ImGui::SameLine(0, s(10));
    }
    text(rf, kSmall, color::text2, auth_status_, false);
    if (signing_in_) {
      ImGui::SameLine(0, s(14));
      if (button("Cancel##signin-cancel", 0, Tone::ghost, {0, s(30)})) {
        ++epoch_;
        browser_.close();
        auth_.reset();
        signing_in_ = verifying_ = false;
        auth_status_ = "Sign-in cancelled.";
      }
    }
    ImGui::EndGroup();
  }
  end_card();
  gap(18);
  ImGui::BeginGroup();
  ImVec2 p = ImGui::GetCursorScreenPos();
  draw_glyph(ImGui::GetWindowDrawList(), icons::lock, kSmall, {p.x + s(8), cap_center(fonts().regular, kSmall, p.y)},
             color::text3);
  ImGui::SetCursorScreenPos({p.x + s(26), p.y});
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - s(26));
  text(rf, kSmall, color::text3,
       std::string("Sign-ins are ") + SecretStore::protection() +
           " and only used to talk to Meta.");
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();
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
  // A headset can hold many games: a search narrows the list. Hidden picks stay picked.
  if (scan.items.size() > 6 || !scan.search.empty()) {
    gap(6);
    search_field(device ? "import-search##device" : "import-search##local", "Search games", &scan.search,
                 std::min(s(320), width));
    gap(10);
  }
  const auto query = lowercase(scan.search);
  for (size_t index = 0; index < scan.items.size(); ++index) {
    auto& item = scan.items[index];
    auto match = import_match(item);
    if (!match.importable) item.selected = false;
    ready += item.selected;
    if (item.selected && !installed_package(item.package)) ready_bytes += item.bytes;
    if (!query.empty()) {
      const auto name = match.game ? match.game->name : item.label;
      if (lowercase(name).find(query) == std::string::npos && lowercase(item.package).find(query) == std::string::npos)
        continue;
    }
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
    if (!device && ImGui::IsMouseHoveringRect({x, p.y}, {text_right, p.y + h})) tooltip(item.location);
    ImGui::SetCursorScreenPos({right - bw, mid - s(12)});
    badge(match.status, badge_fg, color::alpha(badge_fg, 0.14f));
    ImGui::SetCursorScreenPos({p.x, p.y + h});
    ImGui::Dummy({width, 0});
    ImGui::PopID();
  }
  if (scan.items.empty()) return;
  divider();
  gap(14);
  float bw = s(190);
  ImVec2 row = ImGui::GetCursorPos();
  float after = ImGui::GetCursorPosY();
  // Where new games go, as when installing: each library folder, or a new one
  // on another drive. Games already installed stay where they are.
  if (import_choices_.empty())
    for (auto& location : launcher::install_locations(settings_, launcher::local_volumes()))
      import_choices_.emplace_back(location, launcher::free_space(location.folder));
  bool fits = true;
  if (import_choices_.size() > 1) {
    std::vector<std::string> labels;
    for (auto& [location, free] : import_choices_) {
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
    const float dw = std::min(s(320), width - bw - s(12));
    ImGui::SetCursorPos({row.x, row.y + (s(40) - ImGui::GetFrameHeight()) * 0.5f});
    dropdown(device ? "import-to##device" : "import-to##local", "", items.data(), int(items.size()), &import_choice_,
             dw);
    const auto free = import_choices_[import_choice_].second;
    fits = !free || free > ready_bytes;
    if (ImGui::IsItemHovered()) tooltip("Install to " + paths::utf8(import_choices_[import_choice_].first.folder));
    after = std::max(after, ImGui::GetCursorPosY());
  }
  ImGui::SetCursorPos({row.x + width - bw, row.y});
  std::string label = ready == 0   ? std::string("Import")
                      : ready == 1 ? std::string("Import 1 game")
                                   : std::format("Import {} games", ready);
  label += device ? "##device" : "##local";
  const auto folder = import_choices_.size() > 1 ? import_choices_[import_choice_].first.folder : fs::path{};
  if (button(label.c_str(), icons::download, Tone::primary, {bw, s(40)}, ready > 0 && fits))
    action([&, folder] { start_import(device, folder); });
  ImGui::SetCursorPosY(std::max(after, row.y + s(40)));
  if (!fits) {
    gap(6);
    text(fonts().regular, kSmall, color::danger,
         std::format("Not enough space on that drive: the selected games need {}.", bytes(ready_bytes)));
  }
  gap(4);
}

void App::draw_import() {
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
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {s(14), text_y(ImGui::GetFont(), kBody, 0, s(38))});
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, s(10));
      ImGui::PushStyleColor(ImGuiCol_FrameBg, color::surface_hi);
      ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, color::surface_top);
      ImGui::SetNextItemWidth(fw);
      bool enter = ImGui::InputTextWithHint("##address", "192.168.1.20:5555", &import_address_,
                                            ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::PopStyleColor(2);
      ImGui::PopStyleVar(2);
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
    if (ImGui::IsMouseHoveringRect(at, {at.x + measure(fonts().regular, kBody, label).x, at.y + s(kControlH)}))
      tooltip("A folder that holds the APK and its <package> data folder, the APK itself, or a ZIP archive of them. "
              "You can also drop them on this window.");
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
        tooltip(std::string("A copy of the game's Android/data folder on this PC. ") + replaces);
      ImGui::SameLine(0, s(10));
      if (button("From headset##saves-page", icons::download, Tone::primary, {hw, s(kControlH)}, !running))
        action([id = save_game_, this] { import_saves_later(id, {}); });
      if (ImGui::IsItemHovered())
        tooltip(std::string("A Quest connected over USB with developer mode on. ") + replaces);
      ImGui::SetCursorPos({row.x, row.y + s(kControlH)});
      ImGui::Dummy({inner, 0});
    }
  }
  end_card();
  gap(30);
  if (options_.smoke_view == "import-end") ImGui::SetScrollHereY(1.0f);
}

void App::request_install(const meta::Game& g) {
  const bool steam = settings_.add_games_to_steam && (steam_sync_allowed_ || options_.smoke_view == "install");
  if (installed(g.id, false) || !settings_.install_ask) return install_for_owner(g);
  install_choices_.clear();
  for (auto& location : launcher::install_locations(settings_, launcher::local_volumes()))
    install_choices_.emplace_back(location, launcher::free_space(location.folder));
  if (!steam && install_choices_.size() < 2) return install_for_owner(g);
  install_choice_ = 0;
  install_steam_ = in_steam(g.id) && settings_.steam_add_new;
  install_remember_ = false;
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
    const uint64_t size = detail != details_.end() ? detail->second.size : 0;

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
                    dropdown("install-to", "", items.data(), int(items.size()), &install_choice_, dw);
                    if (ImGui::IsItemHovered()) tooltip(paths::utf8(location.folder));
                  });
      if (!fits) text(fonts().regular, kSmall, color::danger, std::format("Not enough space: the game needs {}.", bytes(size)), false);
    }
    if (settings_.add_games_to_steam && (steam_sync_allowed_ || options_.smoke_view == "install")) {
      divider();
      setting_row("Add to Steam", "Appears in your Steam library and launches in SteamVR.", s(44), s(24),
                  [&] { toggle("steam-add", &install_steam_); });
    }

    // A quiet "don't ask again" at the left of the buttons.
    const int clicked = dialog_footer({{"Cancel"}, {"Install", Tone::primary, fits, icons::download}}, [&](float h) {
      const ImVec2 row = ImGui::GetCursorScreenPos();
      ImGui::SetCursorScreenPos({row.x, row.y + (h - s(16)) * 0.5f});
      checkbox("install-remember", "Don't ask again", &install_remember_, {}, true);
      if (ImGui::IsItemHovered()) tooltip("Later installs use these choices. Change them any time in Settings.");
    });
    if (clicked == 1) {
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
    dialog_text(g ? "Uninstall " + g->name + "?" : std::string("Uninstall this game?"),
                "Its files are removed from this PC. Saves are kept, and you can install it again from your library.");
    const int clicked = dialog_footer({{"Cancel"}, {"Uninstall", Tone::danger}});
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
                  report_->error + " Attach the saved file to a GitHub issue or post it in our Discord instead.");
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

}  // namespace venture::launcher
