#pragma once
// OpenXR OBS Mirror (github.com/elliotttate/OpenXR-Layer-OBSMirror): an
// implicit OpenXR API layer that copies the game's eyes to an OBS source. It
// loads into Venture's runtime like into any OpenXR app; the launcher only
// reports whether it is installed and enabled (Settings > Display).
#include "common/paths.h"
#include <string>
#include <vector>

namespace venture::launcher {
inline constexpr const char* kObsMirrorUrl = "https://github.com/elliotttate/OpenXR-Layer-OBSMirror";

struct ObsMirror {
  enum class State { missing, disabled, ready } state = State::missing;
  fs::path manifest;    // the layer's JSON manifest
  std::string version;  // its implementation_version, when given
};

// One registered implicit layer: its manifest and whether the registry
// disables it (a non-zero DWORD).
struct ImplicitLayer {
  fs::path manifest;
  bool disabled = false;
};
// The OBS Mirror layer among `layers` (its api_layer name or its manifest's
// file name): ready when one copy is enabled and its library exists.
ObsMirror find_obs_mirror(const std::vector<ImplicitLayer>& layers);
// The implicit OpenXR layers the loader sees (Windows registry, machine then
// user); empty elsewhere.
std::vector<ImplicitLayer> implicit_openxr_layers();
}  // namespace venture::launcher
