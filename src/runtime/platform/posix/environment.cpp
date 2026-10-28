#include "platform/environment.h"
#include <cstdlib>
namespace venture::platform {
std::filesystem::path application_data_home() {
  if (const char* p = std::getenv("XDG_DATA_HOME")) return p;
  if (const char* p = std::getenv("HOME")) return std::filesystem::path(p) / ".local/share";
  return std::filesystem::current_path();
}
}
