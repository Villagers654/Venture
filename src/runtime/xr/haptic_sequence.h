#pragma once
#include "xr/api.h"
#include <span>
#include <vector>
namespace venture::xr {
constexpr float haptic_emulation_rate = 250;
struct HapticSample { float amplitude; XrDuration duration; };
// Rectified RMS downsampling retains energy from signed PCM, including
// alternating-sign carrier samples. Core vibration cannot reproduce a carrier.
std::vector<HapticSample> pcm_envelope(std::span<const float>, float rate,
                                      std::size_t capacity, std::uint32_t &consumed);
XrResult queue_haptic(XrSession, const XrHapticActionInfo &, std::span<const float>,
                      float rate, bool append, std::uint32_t &consumed);
XrResult queue_envelope(XrSession, const XrHapticActionInfo &, std::span<const float>, XrDuration);
void cancel_sequence(XrSession, const XrHapticActionInfo * = nullptr);
}
