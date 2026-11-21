#include "common/standalone.h"
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>

namespace venture::standalone {
namespace {
// The connector's type: its name up to the last '-' ("HDMI-A-1" -> "HDMI-A").
std::string_view type_of(std::string_view name) {
  const auto dash = name.rfind('-');
  return dash == std::string_view::npos ? name : name.substr(0, dash);
}

#if defined(__linux__) && !defined(__ANDROID__)
std::vector<Connector> read_connectors() {
  namespace fs = std::filesystem;
  std::vector<Connector> out;
  std::error_code ec;
  for (fs::directory_iterator it("/sys/class/drm", ec), end; !ec && it != end; it.increment(ec)) {
    const auto name = it->path().filename().string();
    // card0-DSI-1: a connector; card0, renderD128, version: not.
    const auto dash = name.find('-');
    if (!name.starts_with("card") || dash == std::string::npos) continue;
    std::ifstream status(it->path() / "status");
    std::string state;
    if (!(status >> state)) continue;
    out.push_back({name.substr(dash + 1), state == "connected"});
  }
  return out;
}
#endif
}  // namespace

Decision classify(const std::vector<Connector>& connectors) {
  std::string panels, others;
  for (const auto& c : connectors) {
    if (!c.connected) continue;
    auto& list = type_of(c.name) == "DSI" ? panels : others;
    list += (list.empty() ? "" : ", ") + c.name;
  }
  if (!others.empty())
    return {false, std::format("no: a display other than a built-in DSI panel is connected ({})", others)};
  if (panels.empty()) return {false, "no: no connected display connector found"};
  return {true, std::format("yes: the only displays are built-in DSI panels ({})", panels)};
}

const Decision& detect() {
  static const Decision decision = [] {
    if (const char* forced = std::getenv("VENTURE_STANDALONE"); forced && (*forced == '0' || *forced == '1'))
      return Decision{*forced == '1', std::format("{}: VENTURE_STANDALONE={}", *forced == '1' ? "yes" : "no", forced)};
#if defined(__ANDROID__)
    return Decision{true, "yes: the Android build runs on the headset"};
#elif defined(__linux__)
    return classify(read_connectors());
#else
    return Decision{false, "no: Windows"};
#endif
  }();
  return decision;
}
}  // namespace venture::standalone
