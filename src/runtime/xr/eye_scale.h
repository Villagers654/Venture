// Eye size compensation: many Quest games multiply the eye size they are told
// (Unity's eyeTextureResolutionScale, Unreal's vr.PixelDensity), because a
// Quest's recommendation is deliberately modest. A PC host's recommendation
// already includes its supersampling, so the multiplier only adds pixels the
// compositor scales away again. Venture learns each game's multiplier (its eye
// over the size it was told) and tells it a size divided by that multiplier,
// so it renders at the target: the host's recommendation or the Display
// setting's size. The records live in common/library/eye_scale_db.h.
//
//   VENTURE_EYE_SCALE = auto | off
//     auto (the default; the launcher's "Match game resolution") compensates a
//     learned or bundled multiplier; off tells the game the target itself.
//     Both keep learning.
#pragma once
#include "common/library/eye_scale_db.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <optional>
#include <string_view>

namespace venture::xr::eye_scale {
using library::eye_scale::bundled;
using library::eye_scale::compensation_for;
using library::eye_scale::parse_state;
using library::eye_scale::Record;
using library::eye_scale::State;
using library::eye_scale::state_name;

inline library::eye_scale::Extent extent(VkExtent2D e) { return {e.width, e.height}; }
inline Record learn(const std::optional<Record>& previous, double applied, VkExtent2D told, VkExtent2D target,
                    VkExtent2D rendered) {
  return library::eye_scale::learn(previous, applied, extent(told), extent(target), extent(rendered));
}

enum class Mode { automatic, off };
Mode parse_mode(std::string_view);

// Selects this process's game: reads VENTURE_EYE_SCALE and the game's record,
// and fixes the compensation for the process. Before any eye size is reported.
void begin(std::string_view package);
// The compensation in force (1: none, also before begin).
double compensation();
// A projection eye the game rendered, with the told and target sizes then in
// force. Updates the user's record when it changes.
void observe(VkExtent2D told, VkExtent2D target, VkExtent2D rendered);
// For tests: forget begin() and observations.
void reset_for_tests();
} // namespace venture::xr::eye_scale
