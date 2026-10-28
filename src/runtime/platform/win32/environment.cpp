#include "platform/environment.h"
#include <windows.h>
#include <vector>
namespace venture::platform {
std::filesystem::path application_data_home() {
  auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  if (!size) return std::filesystem::current_path();
  std::vector<wchar_t> value(size);
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), size)) return std::filesystem::current_path();
  return value.data();
}
}
