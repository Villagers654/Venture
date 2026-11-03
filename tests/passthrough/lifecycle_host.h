#pragma once
#include "xr/api.h"
#include <array>
namespace venture::test::passthrough::host {
struct Frame { std::uint32_t layers{}; std::array<XrCompositionLayerProjectionView,2> views{}; bool depth = false; };
void reset();
void event(XrSessionState);
Frame last_frame();
extern XrResult begin_result;
}
