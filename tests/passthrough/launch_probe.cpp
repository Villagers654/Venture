// Child of the actual launcher's play command. No game runtime or headset.
#include "common/environment.h"
#include "common/paths.h"
#include <string_view>

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string_view(argv[i]) != "--game") continue;
    const auto* passthrough = venture::environment::getenv("VENTURE_PASSTHROUGH");
    venture::paths::atomic_write(venture::paths::from_utf8(argv[i + 1]) / "launch-options.json",
        venture::Json{{"passthrough", passthrough ? passthrough : "missing"}}.dump());
    return 0;
  }
  return 2;
}
