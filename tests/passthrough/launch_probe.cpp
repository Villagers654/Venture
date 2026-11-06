// Child of the actual launcher's play command. No game runtime or headset.
#include "common/environment.h"
#include "common/paths.h"
#include <string>
#include <string_view>

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string_view(argv[i]) != "--game") continue;
    const auto value = [](const char* name) {
      const auto* v = venture::environment::getenv(name);
      return std::string(v ? v : "missing");
    };
    venture::paths::atomic_write(venture::paths::from_utf8(argv[i + 1]) / "launch-options.json",
        venture::Json{{"passthrough", value("VENTURE_PASSTHROUGH")},
                      {"pcAudio", value("VENTURE_PC_AUDIO")},
                      {"textureCompression", value("VENTURE_TEXTURE_COMPRESSION")},
                      {"textureCompressionThreshold", value("VENTURE_TEXTURE_COMPRESSION_THRESHOLD")}}.dump());
    return 0;
  }
  return 2;
}
