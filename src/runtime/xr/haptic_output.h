#pragma once
#include "xr/api.h"
#include <nlohmann/json.hpp>
#include <string_view>
namespace venture::xr {
bool haptic_bound(XrSession, const XrHapticActionInfo &);
// Venture suggested a */output/haptic binding for this action on the info's hand (any hand without a subaction).
bool haptic_suggested(const XrHapticActionInfo &);
void clear_haptic_bindings(XrSession);
// The player's vibration for the info's hand (controls::Tuning: off, or a strength),
// 0 to 1, from the controller the host reports there; without a hand, the
// stronger of the two. 1 where the host reports no controller yet.
float vibration_scale(XrSession, const XrHapticActionInfo &);
// Records the */output/haptic bindings Venture suggested to the host (all host suggestions pass through here).
void note_haptic_suggestions(XrInstance, const XrInteractionProfileSuggestedBinding &);
XrResult apply_haptic(std::string_view api, XrSession, const XrHapticActionInfo &,
                      const XrHapticVibration &, std::string_view stage = "host_pulse");
XrResult stop_haptic(std::string_view api, XrSession, const XrHapticActionInfo &,
                     std::string_view stage = "host_stop", nlohmann::json fields = nullptr);
void haptic_event(std::string_view api, XrSession, const XrHapticActionInfo &,
                  float amplitude, XrDuration duration, float frequency,
                  std::string_view host_call, XrResult, nlohmann::json fields = nullptr);
}
