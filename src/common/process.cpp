#include "common/process.h"
#include <format>
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
std::string hotspot_fatal_summary(std::string_view report) {
  constexpr std::string_view banner = "# A fatal error has been detected by the Java Runtime Environment:";
  // A native allocation failure writes a report without the fatal banner.
  constexpr std::string_view memory = "# There is insufficient memory for the Java Runtime Environment";
  if (report.find(banner) == report.npos && report.find(memory) == report.npos) return {};
  std::string_view cause, code, frame, thread;
  bool frame_next = false, java_thread = false;
  while (!report.empty()) {
    const auto end = report.find('\n');
    auto line = report.substr(0, end);
    report = end == report.npos ? std::string_view{} : report.substr(end + 1);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    if (frame_next) {
      // "# C  [vulkan-1.dll+0x486d0]", on Linux "# C  [libX.so+0x1f]  symbol+0x4"
      frame_next = false;
      if (line.starts_with("# ") && line.size() > 4) frame = line.substr(4);
      while (!frame.empty() && frame.front() == ' ') frame.remove_prefix(1);
      continue;
    }
    if (line == "# Problematic frame:") { frame_next = true; continue; }
    // "#  EXCEPTION_ACCESS_VIOLATION (0xc0000005) at pc=0x..., pid=1, tid=2",
    // "#  SIGSEGV (0xb) at pc=...", "#  Internal Error (...)", "#  Out of Memory Error (...)"
    if (cause.empty() && line.starts_with("#  ") && line.size() > 3 && line[3] != ' ') {
      cause = line.substr(3);
      if (const auto at = cause.find(") at pc="); at != cause.npos)
        if (const auto open = cause.rfind(" (0x", at); open != cause.npos) code = cause.substr(open + 2, at - open - 2);
      continue;
    }
    if (cause.empty() && line.starts_with("# There is insufficient memory")) { cause = line.substr(2); continue; }
    if (thread.empty() && line.starts_with("Current thread (")) {
      if (const auto at = line.find("JavaThread \""); at != line.npos) {
        thread = line.substr(at + 12);
        thread = thread.substr(0, thread.find('"'));
        java_thread = true;
      } else if (const auto at = line.find("):  "); at != line.npos) {
        thread = line.substr(at + 4);
        thread = thread.substr(0, thread.find(" ["));
      }
    }
  }
  if (cause.empty()) return {};
  std::string where(frame);
  if (frame.starts_with('[')) {
    const auto close = frame.find(']');
    where = std::string(frame.substr(1, close == frame.npos ? frame.npos : close - 1));
    if (close != frame.npos) {
      auto symbol = frame.substr(close + 1);
      while (!symbol.empty() && symbol.front() == ' ') symbol.remove_prefix(1);
      if (!symbol.empty()) where += std::format(" ({})", symbol);
    }
  }
  const auto on = thread.empty() ? std::string{}
      : std::format(" ({}{})", java_thread ? "Java thread " : "", thread);
  if (!code.empty() && !where.empty()) return std::format("Host exception {} at {}{}", code, where, on);
  return std::format("Java runtime fatal error: {}{}{}", cause, where.empty() ? "" : " at " + where, on);
}
} // namespace venture
