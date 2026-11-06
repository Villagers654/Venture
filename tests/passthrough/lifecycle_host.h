#pragma once
#include "xr/api.h"
#include <array>
#include <vector>
namespace venture::test::passthrough::host {
struct Frame { std::uint32_t layers{}; std::array<XrCompositionLayerProjectionView,2> views{}; bool depth = false;
  XrEnvironmentBlendMode blend_mode{}; std::vector<XrStructureType> types; std::vector<XrCompositionLayerQuad> quads;
  XrCompositionLayerFlags projection_flags = 0; };
void reset();
void event(XrSessionState);
Frame last_frame();
extern XrResult begin_result;
}
