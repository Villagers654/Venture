#include "launcher/ui/rich_text.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace venture::launcher::ui {
namespace {

bool starts(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool space(char c) { return c == ' ' || c == '\t'; }
bool alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) || (static_cast<unsigned char>(c) & 0x80); }

void append_utf8(std::string& out, uint32_t cp) {
  if (cp == 0 || cp > 0x10FFFF) return;
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
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// HTML a title pasted into its description becomes the equivalent Markdown.
std::string from_html(std::string_view in) {
  std::string out;
  std::vector<std::string> links;
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '<' && i + 1 < in.size() && (std::isalpha(static_cast<unsigned char>(in[i + 1])) || in[i + 1] == '/')) {
      size_t end = in.find('>', i);
      if (end == std::string_view::npos || in.find('<', i + 1) < end) {
        out += in[i];
        continue;
      }
      std::string tag = lower(in.substr(i + 1, end - i - 1));
      bool closing = !tag.empty() && tag[0] == '/';
      if (closing) tag.erase(0, 1);
      std::string name = tag.substr(0, tag.find_first_of(" \t\r\n/"));
      if (name == "br") {
        out += '\n';
      } else if (name == "p" || name == "div") {
        out += "\n\n";
      } else if (name == "li") {
        if (!closing) out += "\n- ";
      } else if (name == "ul" || name == "ol") {
        out += '\n';
      } else if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
        out += closing ? "\n\n" : "\n\n" + std::string(size_t(name[1] - '0'), '#') + ' ';
      } else if (name == "b" || name == "strong") {
        out += "**";
      } else if (name == "i" || name == "em") {
        out += '*';
      } else if (name == "hr") {
        out += "\n\n---\n\n";
      } else if (name == "a") {
        if (!closing) {
          // href="..." from the original (case-preserved) text.
          std::string_view raw = in.substr(i, end - i);
          std::string href;
          if (auto h = lower(raw).find("href="); h != std::string::npos && h + 5 < raw.size()) {
            char q = raw[h + 5];
            size_t from = h + 6, to = (q == '"' || q == '\'') ? raw.find(q, from) : raw.find_first_of(" >", h + 5);
            if (q != '"' && q != '\'') from = h + 5;
            href = std::string(raw.substr(from, to == std::string_view::npos ? raw.size() - from : to - from));
          }
          links.push_back(href);
          if (!href.empty()) out += '[';
        } else if (!links.empty()) {
          if (!links.back().empty()) out += "](" + links.back() + ")";
          links.pop_back();
        }
      }
      i = end;
      continue;
    }
    out += in[i];
  }
  return out;
}

std::string decode_entities(std::string_view in) {
  static const std::pair<std::string_view, std::string_view> named[] = {
      {"amp", "&"},   {"lt", "<"},   {"gt", ">"},    {"quot", "\""}, {"apos", "'"},
      {"nbsp", " "},  {"ndash", "\xE2\x80\x93"}, {"mdash", "\xE2\x80\x94"}, {"hellip", "\xE2\x80\xA6"},
      {"rsquo", "\xE2\x80\x99"}, {"lsquo", "\xE2\x80\x98"}, {"rdquo", "\xE2\x80\x9D"}, {"ldquo", "\xE2\x80\x9C"},
      {"bull", "\xE2\x80\xA2"}, {"copy", "\xC2\xA9"}, {"reg", "\xC2\xAE"}, {"trade", "\xE2\x84\xA2"}};
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '&') {
      size_t semi = in.find(';', i);
      if (semi != std::string_view::npos && semi - i <= 10) {
        std::string_view ent = in.substr(i + 1, semi - i - 1);
        if (!ent.empty() && ent[0] == '#') {
          bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
          uint32_t cp = 0;
          bool ok = ent.size() > (hex ? 2u : 1u);
          for (char c : ent.substr(hex ? 2 : 1)) {
            int d = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                    : hex && std::isxdigit(static_cast<unsigned char>(c)) ? std::tolower(c) - 'a' + 10
                                                                          : -1;
            if (d < 0) ok = false;
            cp = cp * (hex ? 16 : 10) + uint32_t(std::max(d, 0));
          }
          if (ok) {
            append_utf8(out, cp == 0xA0 ? ' ' : cp);
            i = semi;
            continue;
          }
        } else {
          for (auto& [name, value] : named)
            if (ent == name) {
              out += value;
              i = semi;
              goto next;
            }
        }
      }
    }
    out += in[i];
  next:;
  }
  return out;
}

// Drops direction marks, zero-width characters and carriage returns, and
// turns no-break spaces into spaces.
std::string clean(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    std::string_view rest = in.substr(i);
    if (starts(rest, "\xC2\xA0")) {
      out += ' ';
      ++i;
      continue;
    }
    // U+200B..U+200F, U+2060, U+FEFF
    if (rest.size() >= 3 && static_cast<unsigned char>(rest[0]) == 0xE2 && static_cast<unsigned char>(rest[1]) == 0x80 &&
        static_cast<unsigned char>(rest[2]) >= 0x8B && static_cast<unsigned char>(rest[2]) <= 0x8F) {
      i += 2;
      continue;
    }
    if (starts(rest, "\xE2\x81\xA0") || starts(rest, "\xEF\xBB\xBF")) {
      i += 2;
      continue;
    }
    if (in[i] != '\r') out += in[i];
  }
  return out;
}

bool web_link(std::string_view url) { return starts(url, "https://") || starts(url, "http://"); }

// Inline Markdown: **bold**, __bold__, *italic*, _italic_, [text](url),
// bare URLs and backslash escapes. Inline media is dropped.
std::vector<Span> inline_spans(std::string_view s) {
  std::vector<Span> out;
  bool bold = false, italic = false;
  std::string cur;
  auto flush = [&](std::string link = {}) {
    if (cur.empty()) return;
    if (!out.empty() && out.back().bold == bold && out.back().italic == italic && out.back().link == link &&
        link.empty())
      out.back().text += cur;
    else
      out.push_back({cur, bold, italic, std::move(link)});
    cur.clear();
  };
  // A closing marker must exist later on the line, so a stray "*" stays text.
  auto closes = [&](size_t from, std::string_view marker) { return s.find(marker, from) != std::string_view::npos; };
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    char prev = i ? s[i - 1] : ' ';
    char next = i + 1 < s.size() ? s[i + 1] : ' ';
    if (c == '\\' && i + 1 < s.size() && std::ispunct(static_cast<unsigned char>(next))) {
      cur += next;
      ++i;
      continue;
    }
    if (starts(s.substr(i), "![") ) {
      // ![alt](url): embedded image or video.
      size_t mid = s.find("](", i);
      size_t end = mid == std::string_view::npos ? mid : s.find(')', mid);
      if (end != std::string_view::npos) {
        i = end;
        continue;
      }
    }
    if ((starts(s.substr(i), "**") || starts(s.substr(i), "__")) &&
        (bold || (!space(i + 2 < s.size() ? s[i + 2] : ' ') && closes(i + 2, s.substr(i, 2))))) {
      if (c == '_' && !bold && alnum(prev)) {
        cur += c;
        continue;
      }
      flush();
      bold = !bold;
      ++i;
      continue;
    }
    if ((c == '*' || c == '_') && !(c == '_' && (italic ? alnum(next) : alnum(prev)))) {
      bool can_open = !italic && !space(next) && closes(i + 1, std::string_view(&s[i], 1));
      bool can_close = italic && !space(prev);
      if (can_open || can_close) {
        flush();
        italic = !italic;
        continue;
      }
    }
    if (c == '[') {
      size_t mid = s.find("](", i);
      size_t end = mid == std::string_view::npos ? mid : s.find(')', mid + 2);
      if (end != std::string_view::npos && s.substr(i + 1, mid - i - 1).find('[') == std::string_view::npos) {
        std::string url(s.substr(mid + 2, end - mid - 2));
        flush();
        cur = std::string(s.substr(i + 1, mid - i - 1));
        flush(web_link(url) ? url : std::string());
        i = end;
        continue;
      }
    }
    if ((c == 'h' || c == 'H') && !alnum(prev) && web_link(lower(s.substr(i, 8)))) {
      size_t end = i;
      while (end < s.size() && !space(s[end])) ++end;
      while (end > i && std::string_view(".,;:!?)\"'").find(s[end - 1]) != std::string_view::npos) --end;
      flush();
      cur = std::string(s.substr(i, end - i));
      flush(cur);
      i = end - 1;
      continue;
    }
    cur += c;
  }
  flush();
  return out;
}

}  // namespace

std::vector<Block> parse_description(std::string_view text) {
  std::string body = clean(text);
  if (body.find('<') != std::string::npos) body = from_html(body);
  body = decode_entities(body);
  std::vector<Block> out;
  bool blank = false;
  size_t start = 0;
  while (start <= body.size()) {
    size_t end = body.find('\n', start);
    if (end == std::string::npos) end = body.size();
    std::string_view line(body.data() + start, end - start);
    start = end + 1;
    while (!line.empty() && space(line.front())) line.remove_prefix(1);
    while (!line.empty() && space(line.back())) line.remove_suffix(1);
    if (line.empty()) {
      blank = true;
      continue;
    }
    Block b;
    b.after_blank = blank || out.empty();
    std::string_view content = line;
    if (starts(line, "![") || starts(line, "[![")) {
      // A line of embedded media separates paragraphs like a blank line.
      size_t close = line.rfind(')');
      if (close == line.size() - 1) {
        blank = true;
        continue;
      }
    }
    if (line.front() == '#') {
      size_t n = line.find_first_not_of('#');
      if (n != std::string_view::npos && n <= 6 && space(line[n])) {
        b.kind = Block::Kind::heading;
        b.level = int(n);
        content = line.substr(n + 1);
        while (!content.empty() && (content.back() == '#' || space(content.back()))) content.remove_suffix(1);
        while (!content.empty() && space(content.front())) content.remove_prefix(1);
      }
    }
    if (b.kind == Block::Kind::paragraph && (line.front() == '-' || line.front() == '*' || line.front() == '_') &&
        std::ranges::count(line, line.front()) >= 3 &&
        std::ranges::all_of(line, [&](char c) { return c == line.front() || space(c); })) {
      b.kind = Block::Kind::rule;
      content = {};
    }
    if (b.kind == Block::Kind::paragraph) {
      for (std::string_view bullet : {"- ", "* ", "+ ", "\xE2\x80\xA2", "\xE2\x97\x8F", "\xE2\x96\xAA", "\xE2\x97\xA6",
                                      "\xE2\x80\x93 ", "\xC2\xB7"}) {
        if (starts(line, bullet)) {
          b.kind = Block::Kind::bullet;
          content = line.substr(bullet.size());
          while (!content.empty() && space(content.front())) content.remove_prefix(1);
          break;
        }
      }
    }
    if (b.kind == Block::Kind::paragraph) {
      size_t d = 0;
      while (d < line.size() && d < 3 && std::isdigit(static_cast<unsigned char>(line[d]))) ++d;
      if (d && d + 1 < line.size() && (line[d] == '.' || line[d] == ')') && space(line[d + 1])) {
        b.kind = Block::Kind::numbered;
        b.marker = std::string(line.substr(0, d)) + ".";
        content = line.substr(d + 2);
        while (!content.empty() && space(content.front())) content.remove_prefix(1);
      }
    }
    if (b.kind != Block::Kind::rule) {
      b.spans = inline_spans(content);
      if (b.spans.empty()) {
        blank = blank || b.after_blank;
        continue;
      }
    }
    out.push_back(std::move(b));
    blank = false;
  }
  return out;
}

}  // namespace venture::launcher::ui
