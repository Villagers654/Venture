#include "xr/host_xr.h"
#include <iostream>
#include <openxr/openxr.h>
int main() {
  auto &host = venture::xr::host_xr();
  std::cout << "Runtime: " << host.runtime_name << " " << XR_VERSION_MAJOR(host.runtime_version)
            << '.' << XR_VERSION_MINOR(host.runtime_version) << '.'
            << XR_VERSION_PATCH(host.runtime_version) << '\n';
  std::cout << "HMD available: " << host.available << " (xrGetSystem result " << host.system_result
            << ")\n";
  if (!host.available) std::cout << "Error: " << host.error << '\n';
  for (const auto &[name, version] : host.available_extensions)
    std::cout << name << " v" << version
              << (host.enabled_extensions.contains(name) ? " enabled" : "") << '\n';
  return 0;
}
