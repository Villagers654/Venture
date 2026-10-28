#include "common/environment.h"
#include <cstdio>
#include <string_view>
int main() {
  for (int i=0;i<3;++i) {
    const auto* value=venture::environment::getenv("VENTURE_TRACE_LOOPER");
    std::puts(!value ? "unset" : !*value ? "empty" : std::string_view(value)=="current-value" ? "current-value" : "unexpected");
    venture::environment::refresh();
  }
}
