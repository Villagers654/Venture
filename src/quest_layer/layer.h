#pragma once
// The Quest compatibility layer (docs/design/native-headsets.md, section 5.4).
// An OpenXR API layer between a Quest game and the headset's own runtime:
//  - it answers the Quest extensions in emulated_extensions() that the runtime
//    lacks, and passes through the ones the runtime has;
//  - it runs an OpenXR 1.1 game on a 1.0 runtime: the instance is created at
//    1.0 when the runtime refuses 1.1, and the 1.1 core the game may use is
//    answered here (xrLocateSpaces, the LOCAL_FLOOR reference space);
//  - where the runtime has XR_FB_display_refresh_rate but refuses a rate it
//    lists, the request succeeds and the runtime keeps its rate, as a Quest
//    game expects of a listed rate.
// Everything follows from what the runtime reports; nothing names a headset.
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <cstdint>
#include <span>
#include <string>

namespace venture::quest_layer {
struct Extension {
  const char* name;
  std::uint32_t version;
};
// The extensions this layer can answer (VENTURE_QUEST_LAYER_EXTENSIONS).
std::span<const Extension> emulated_extensions();

XrResult negotiate(const XrNegotiateLoaderInfo* loader, const char* layer_name, XrNegotiateApiLayerRequest* request);

// What the layer did for one instance, for the headset check and the tests.
struct Report {
  bool lowered = false;           // created at 1.0 for a 1.1 application
  std::string emulated;           // comma-separated extensions answered here
  std::uint32_t refresh_fallbacks = 0;
};
// The newest instance's report (empty without one).
Report report();
}  // namespace venture::quest_layer
