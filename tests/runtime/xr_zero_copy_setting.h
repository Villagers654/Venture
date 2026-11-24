#pragma once
#include "common/environment.h"
#include <cstdlib>

namespace venture::test {
// VENTURE_XR_ZERO_COPY for one scope ("0" keeps every chain private and
// copied, as tests of the copy path need; zero-copy is on by default).
struct ZeroCopySetting {
  explicit ZeroCopySetting(const char* value) { set(value); }
  ~ZeroCopySetting() { set(""); }
  static void set(const char* value) {
#ifdef _WIN32
    _putenv_s("VENTURE_XR_ZERO_COPY", value);
#else
    if (*value) setenv("VENTURE_XR_ZERO_COPY", value, 1); else unsetenv("VENTURE_XR_ZERO_COPY");
#endif
    venture::environment::refresh();
  }
};
}  // namespace venture::test
