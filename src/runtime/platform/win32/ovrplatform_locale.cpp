#include <windows.h>

#include "hle/ovrplatform/provider.h"

namespace venture::ovrplatform {
std::string host_locale() {
  wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
  if (!GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH)) return {};
  std::string result;
  // Windows locale names are ASCII BCP-47 language tags.
  for (auto* p = name; *p; ++p) result += static_cast<char>(*p);
  return result;
}
}  // namespace venture::ovrplatform
