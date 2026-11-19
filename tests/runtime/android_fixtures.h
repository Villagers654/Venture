#pragma once
// Where the generated Android fixtures (android_make_fixtures.py) are: the
// VENTURE_ANDROID_FIXTURES environment variable, else android-fixtures in the
// build tree beside the test binary's bin directory (so a tree copied to another
// machine, such as the arm64 VM, still finds them), else the build-time path.
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include "common/paths.h"

namespace venture::test {
inline std::filesystem::path android_fixture_dir() {
  if (const char* dir = std::getenv("VENTURE_ANDROID_FIXTURES"); dir && *dir) return dir;
  std::error_code error;
  const auto executable = paths::executable();
  for (const auto& candidate : {executable.parent_path().parent_path() / "android-fixtures",
                                executable.parent_path() / "android-fixtures"})
    if (std::filesystem::is_directory(candidate, error)) return candidate;
#ifdef VENTURE_ANDROID_FIXTURES
  return VENTURE_ANDROID_FIXTURES;
#else
  return {};
#endif
}
inline std::string android_fixture(const char* name) { return (android_fixture_dir() / name).string(); }
}  // namespace venture::test
