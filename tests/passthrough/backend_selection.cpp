#include "core/process.h"
#include "xr/host_xr.h"
#include "xr/steam_link.h"
#include <stdexcept>

namespace venture {
const DeviceIdentity& native_device_identity() { static const DeviceIdentity identity{}; return identity; }
ProcessConfig& process_config() { static ProcessConfig config; return config; }
namespace xr { HostXr& host_xr() { static HostXr host; return host; } }
}
int main() {
  using namespace venture;
  const auto check = [](bool expected) {
    if (xr::steam_link_passthrough_configured() != expected)
      throw std::runtime_error("Unexpected Steam Link backend selection");
  };
  xr::host_xr().runtime_name = "SteamVR/OpenXR";
  check(false); // Default-off, even on the supported host.
  process_config().passthrough = true;
#ifdef _WIN32
  check(true);
#else
  check(false);
#endif
  process_config().abi = "armeabi-v7a";
  check(false);
  process_config().abi = "arm64-v8a";
  xr::host_xr().runtime_name = "Other OpenXR runtime";
  check(false);
  xr::host_xr().runtime_name.clear();
  check(false);
}
