#include "common/log.h"
#include "common/meta/native_host.h"
#include <cstdlib>
#include <vector>

namespace venture::meta {
namespace {
// Linux has no registry: each browser instead looks for an identical
// manifest file under its own per-user config directory.
std::vector<fs::path> manifest_dirs(const fs::path &home) {
  return {home / ".config/google-chrome/NativeMessagingHosts",
         home / ".config/microsoft-edge/NativeMessagingHosts",
         home / ".config/BraveSoftware/Brave-Browser/NativeMessagingHosts"};
}
} // namespace
void register_native_messaging_host() {
  auto home = std::getenv("HOME");
  if (!home || !*home)
    throw std::runtime_error(
        "Set HOME to register the Meta session native messaging host.");
  auto text = native_messaging_host_manifest().dump(2);
  auto name = std::string(kNativeMessagingHostName) + ".json";
  for (auto &dir : manifest_dirs(fs::path(home)))
    paths::atomic_write(dir / name, text);
  log::info("meta", "Registered the Meta session native messaging host at {}.",
           paths::utf8(native_messaging_host_executable()));
}
} // namespace venture::meta
