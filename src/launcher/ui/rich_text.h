// Store descriptions: Meta's long descriptions are Markdown (headings, bold,
// lists, links and embedded media), and some titles paste HTML. They are
// parsed into blocks of styled spans and laid out with real line spacing.
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace venture::launcher::ui {

struct Span {
  std::string text;
  bool bold = false;
  bool italic = false;
  std::string link;  // http(s) target, when the span is a link
};

struct Block {
  enum class Kind { paragraph, heading, bullet, numbered, rule } kind = Kind::paragraph;
  int level = 0;             // heading level, 1 = largest
  std::string marker;        // "3." for numbered items
  bool after_blank = false;  // a blank line preceded it (a paragraph break)
  std::vector<Span> spans;
};

std::vector<Block> parse_description(std::string_view text);

// Lays out `blocks` at the cursor, `width` wide, and advances the cursor.
// Links open in the browser when clicked.
void draw_description(const std::vector<Block>& blocks, float width);

}  // namespace venture::launcher::ui
