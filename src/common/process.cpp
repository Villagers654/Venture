#include "common/process.h"
#include <fstream>
namespace venture {
std::wstring quote_windows_arg(std::wstring_view arg) {
  std::wstring result = L"\"";
  size_t slashes = 0;
  for (auto c : arg) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    if (c == L'\"')
      result.append(slashes * 2 + 1, L'\\');
    else
      result.append(slashes, L'\\');
    slashes = 0;
    result += c;
  }
  result.append(slashes * 2, L'\\');
  result += L'\"';
  return result;
}
std::string tail_log(const fs::path &file, size_t limit) {
  std::ifstream in(file, std::ios::binary | std::ios::ate);
  if (!in)
    return {};
  auto end = in.tellg();
  auto n = std::min<uint64_t>(end < 0 ? 0 : uint64_t(end), limit);
  std::string text(n, '\0');
  in.seekg(std::streamoff(uint64_t(end) - n));
  in.read(text.data(), n);
  if (uint64_t(end) > n) {
    auto line = text.find('\n');
    if (line != text.npos)
      text.erase(0, line + 1);
  }
  return text;
}
} // namespace venture
