// Installed fonts by fontconfig pattern (POSIX); Windows uses its Fonts folder.
#pragma once
#include <string>
namespace venture::launcher {
// The file fontconfig matches for `pattern` (such as "Inter,Noto Sans:weight=semibold"),
// or empty without fontconfig. With `family_only`, empty unless the match is
// one of the pattern's families rather than a substitute.
std::string system_font(const char* pattern, bool family_only = false);
}  // namespace venture::launcher
