#include "launcher/ui/rich_text.h"

#include "launcher/ui/style.h"
#include "common/process.h"

#include <algorithm>
#include <cmath>

namespace venture::launcher::ui {

void draw_description(const std::vector<Block>& blocks, float width) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 origin = ImGui::GetCursorScreenPos();
  origin = {std::round(origin.x), std::round(origin.y)};
  float y = origin.y;
  bool hover_window = ImGui::IsWindowHovered();
  ImVec2 mouse = ImGui::GetIO().MousePos;
  const Block* prev = nullptr;
  for (const Block& b : blocks) {
    using K = Block::Kind;
    bool list = b.kind == K::bullet || b.kind == K::numbered;
    bool prev_list = prev && (prev->kind == K::bullet || prev->kind == K::numbered);
    // Spacing before the block.
    if (prev) {
      float before = b.kind == K::heading                          ? 26
                     : b.after_blank                               ? 16
                     : list && prev_list                           ? 6
                     : list || prev_list || prev->kind == K::heading ? 8
                                                                   : 4;
      if (prev->kind == K::heading && !b.after_blank) before = 8;
      y += s(before);
    }
    if (b.kind == K::rule) {
      y += s(6);
      hline(dl, origin.x, origin.x + width, y);
      y += s(7);
      prev = &b;
      continue;
    }
    float size = b.kind == K::heading ? (b.level <= 1 ? kHeading + 2 : b.level == 2 ? kHeading : kBody + 1) : kBody;
    float line_h = std::round(s(size) * (b.kind == K::heading ? 1.35f : 1.55f));
    float indent = list ? s(24) : 0;
    ImU32 base = b.kind == K::heading ? color::text : color::text2;
    if (b.kind == K::bullet)
      dl->AddCircleFilled({origin.x + s(8), std::round(y + line_h * 0.5f)}, s(2.5f), color::text3, 12);
    else if (b.kind == K::numbered)
      draw_text(dl, fonts().regular, size,
                {std::round(origin.x + indent - s(8) - measure(fonts().regular, size, b.marker).x),
                 text_y(fonts().regular, size, y, line_h)},
                color::text3, b.marker);
    float left = origin.x + indent, right = origin.x + width;
    float x = left;
    bool line_start = true;
    float text_top = text_y(fonts().regular, size, y, line_h);
    auto new_line = [&] {
      y += line_h;
      text_top = text_y(fonts().regular, size, y, line_h);
      x = left;
      line_start = true;
    };
    for (const Span& sp : b.spans) {
      bool strong = sp.bold || b.kind == K::heading;
      ImFont* font = strong ? fonts().semibold : sp.italic ? fonts().italic : fonts().regular;
      ImU32 col = !sp.link.empty() ? color::accent_text : strong ? color::text : base;
      float space_w = measure(font, size, " ").x;
      std::string_view rest = sp.text;
      while (!rest.empty()) {
        // Next word with its leading spaces.
        size_t spaces = 0;
        while (spaces < rest.size() && rest[spaces] == ' ') ++spaces;
        size_t end = rest.find(' ', spaces);
        if (end == std::string_view::npos) end = rest.size();
        std::string_view word = rest.substr(spaces, end - spaces);
        rest.remove_prefix(end);
        float gap = line_start ? 0 : spaces * space_w;
        float ww = measure(font, size, word).x;
        if (!line_start && x + gap + ww > right) {
          new_line();
          gap = 0;
        }
        x += gap;
        // A word wider than the line (a long URL) breaks between characters.
        while (!word.empty()) {
          std::string_view piece = word;
          if (x + measure(font, size, piece).x > right) {
            size_t n = 0, fit = 0;
            while (n < word.size()) {
              size_t step = 1;
              while (n + step < word.size() && (static_cast<unsigned char>(word[n + step]) & 0xC0) == 0x80) ++step;
              if (x + measure(font, size, word.substr(0, n + step)).x > right) break;
              n += step;
              fit = n;
            }
            if (fit == 0 && !line_start) {
              new_line();
              continue;
            }
            piece = word.substr(0, std::max<size_t>(fit, 1));
          }
          float pw = measure(font, size, piece).x;
          ImVec2 a{std::round(x), text_top};
          bool hovered = !sp.link.empty() && hover_window && mouse.x >= a.x && mouse.x < a.x + pw &&
                         mouse.y >= y && mouse.y < y + line_h;
          draw_text(dl, font, size, a, hovered ? color::text : col, piece);
          if (!sp.link.empty()) {
            if (hovered) {
              ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
              tooltip(sp.link);
              if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                try { open_url(sp.link); } catch (const std::exception&) {}  // as before, a link may go nowhere
              }
              hline(dl, a.x, a.x + pw, std::round(text_y(font, size, y, line_h) + s(size) * 0.92f), color::text);
            }
          }
          x += pw;
          line_start = false;
          word.remove_prefix(piece.size());
          if (!word.empty()) new_line();
        }
      }
    }
    y += line_h;
    prev = &b;
  }
  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy({width, y - origin.y});
}

}  // namespace venture::launcher::ui
