// The application icon: the logo (assets/venture.svg) on a rounded dark tile, so it reads
// on light and dark taskbars. Shared by the window icon and the build tool that writes venture.exe's icon.
#pragma once
#include <string>
#include <string_view>

namespace venture::launcher::ui {
// An SVG document for the icon; `logo` is the source of assets/venture.svg (its viewBox is 28 29 200 200).
inline std::string app_icon_svg(std::string_view logo) {
  auto open = logo.find('>', logo.find("<svg"));
  auto close = logo.rfind("</svg>");
  std::string_view paths = open == std::string_view::npos || close == std::string_view::npos || close < open
                               ? std::string_view{}
                               : logo.substr(open + 1, close - open - 1);
  // The logo's 200-unit box maps to 176 units centered on the 256-unit tile.
  return std::string(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">)svg"
                     R"svg(<rect x="4" y="4" width="248" height="248" rx="56" fill="#1A1B1E"/>)svg"
                     R"svg(<g transform="translate(15.36 13.48) scale(0.88)">)svg") +
         std::string(paths) + "</g></svg>";
}
}  // namespace venture::launcher::ui
