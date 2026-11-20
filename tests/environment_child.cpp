#include "common/environment.h"
#include <cstdio>
#include <cstdlib>
#include <string_view>
int main(int argc, char** argv) {
  // With names: each one's value as this process got it ("NAME=value", or "NAME unset").
  if (argc > 1) {
    for (int i = 1; i < argc; ++i) {
      const char* value = std::getenv(argv[i]);
      if (value) std::printf("%s=%s\n", argv[i], value);
      else std::printf("%s unset\n", argv[i]);
    }
    return 0;
  }
  for (int i=0;i<3;++i) {
    const auto* value=venture::environment::getenv("VENTURE_TRACE_LOOPER");
    std::puts(!value ? "unset" : !*value ? "empty" : std::string_view(value)=="current-value" ? "current-value" : "unexpected");
    venture::environment::refresh();
  }
}
