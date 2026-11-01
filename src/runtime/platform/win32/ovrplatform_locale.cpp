#include <windows.h>

#include "common/environment.h"
#include "hle/ovrplatform/provider.h"

namespace venture::ovrplatform {
std::string host_locale() {
  if (const char* tag = environment::getenv("VENTURE_LOCALE"); tag && *tag) return tag;  // Settings > Game language
  wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
  if (!GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH)) return {};
  std::string result;
  // Windows locale names are ASCII BCP-47 language tags.
  for (auto* p = name; *p; ++p) result += static_cast<char>(*p);
  return result;
}
}  // namespace venture::ovrplatform
