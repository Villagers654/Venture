#pragma once
#include "xr/api.h"
#include <chrono>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
namespace venture::xr {
constexpr float haptic_emulation_rate = 250;
struct HapticSample { float amplitude; XrDuration duration; };
// Rectified RMS downsampling retains energy from signed PCM, including
// alternating-sign carrier samples. Core vibration cannot reproduce a carrier.
std::vector<HapticSample> pcm_envelope(std::span<const float>, float rate,
                                      std::size_t capacity, std::uint32_t &consumed);

// Haptic output. Every host haptic call (xrApplyHapticFeedback, xrStopHapticFeedback) runs on
// one worker thread, never on the caller's: these functions only queue work and return, so a
// runtime whose haptic calls are slow cannot hold up the game's OpenXR calls, which share the
// guest API lock with its haptic calls. Work runs in the order it was queued. A newer request
// for an action and hand replaces one of the same kind still waiting there, and a stop
// withdraws everything still waiting for what it stops.

// PCM played as core pulses at haptic_emulation_rate (hosts without XR_FB_haptic_pcm, or whose
// device answers a PCM rate of 0); `consumed` is how much of `input` was accepted: all of a
// buffer when nothing waits, and otherwise up to native_pcm_backlog waiting, as for native PCM.
XrResult queue_haptic(XrSession, const XrHapticActionInfo &, std::span<const float>,
                      float rate, bool append, std::uint32_t &consumed);
// An amplitude envelope played as core pulses (hosts without XR_FB_haptic_amplitude_envelope).
XrResult queue_envelope(XrSession, const XrHapticActionInfo &, std::span<const float>, XrDuration);
// A core vibration (apply_haptic on the worker), replacing a sequence playing there.
void queue_vibration(std::string_view api, XrSession, const XrHapticActionInfo &, const XrHapticVibration &,
                     std::string_view stage = "guest_request");
// Stops output on the target (stop_haptic on the worker) and withdraws work waiting for it.
void queue_stop(std::string_view api, XrSession, const XrHapticActionInfo &, std::string_view stage = "guest_request");
// PCM for a host with XR_FB_haptic_pcm, scaled by the player's vibration strength on the
// worker. Returns the samples accepted: all of a buffer when nothing waits for the host, and
// otherwise up to native_pcm_backlog of samples waiting there (the host paces the game).
std::uint32_t queue_native_pcm(XrSession, const XrHapticActionInfo &, std::span<const float>, float rate,
                               bool append);
// An envelope for a host with XR_FB_haptic_amplitude_envelope, scaled on the worker.
void queue_native_envelope(XrSession, const XrHapticActionInfo &, std::span<const float>, XrDuration);
constexpr XrDuration native_pcm_backlog = 100'000'000;
// Withdraws the session's (or one action's) waiting work and stops its sequences. With wait,
// returns once no host call for it is waiting or running: before its action or session goes.
void cancel_sequence(XrSession, const XrHapticActionInfo * = nullptr, bool wait = true);
// True once no queued work is waiting or running (sequences may still have pulses to play).
bool haptics_settled(std::chrono::milliseconds timeout);
}
