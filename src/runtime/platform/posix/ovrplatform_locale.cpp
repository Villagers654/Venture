#include <algorithm>
#include <cstdlib>

#include "hle/ovrplatform/provider.h"

namespace venture::ovrplatform {
std::string host_locale() {
  const char* value = nullptr;
  for (auto key : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
    value = std::getenv(key);
    if (value && *value) break;
  }
  std::string name = value ? value : "";
  name.resize(name.find_first_of(".@") == name.npos ? name.size() : name.find_first_of(".@"));
  if (name == "C" || name == "POSIX") return {};
  std::replace(name.begin(), name.end(), '_', '-');
  return name;
}
}  // namespace venture::ovrplatform
