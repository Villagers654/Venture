#pragma once
#include <cstdint>
#include <memory>

namespace venture::xr {
// One application-format lease per process. All discovery and pipe I/O runs
// on its worker. The render seam reads only the current atomic generation.
class SteamLinkSource {
 public:
  SteamLinkSource();
  ~SteamLinkSource();
  void request(bool active);
  std::uint64_t permission() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// Explicit opt-in on the supported Windows SteamVR backend, A64 guests only.
bool steam_link_passthrough_configured();
} // namespace venture::xr
