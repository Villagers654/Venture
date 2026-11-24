#pragma once
#include "xr/api.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
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

// The next host pulse of an emulated stream, taken from the front of `samples` (4 ms each).
// A host call per sample is 250 a second per hand, more than any native game sends; Index
// controllers' lighthouse driver then stalls both controllers' input. So host calls are at
// least host_pulse_spacing apart, and within that the pulse follows the stream's shape:
// - the first host_pulse_spacing of samples always go into one pulse, at their RMS level but
//   never below host_pulse_peak_kept of their peak, so a short hit keeps its punch;
// - silence at the end of that window ends the pulse early (the motor stops between hits);
// - past the window the pulse continues only while samples stay at the pulse's level (within
//   host_pulse_step, or that fraction of the level), so ramps, fades and a hit's tail arrive
//   as pulses of their own;
// - silence (at most host_pulse_silent) sends no pulse: amplitude 0, `span` long.
// `duration` is how long the pulse plays and `span` how much of the stream it covers.
constexpr XrDuration host_pulse_spacing = 20'000'000;
constexpr float host_pulse_peak_kept = 0.7f, host_pulse_step = 0.05f, host_pulse_step_fraction = 0.12f,
                host_pulse_silent = 0.02f;
struct HostPulse { float amplitude; XrDuration duration; XrDuration span; };
HostPulse next_host_pulse(std::deque<HapticSample> &samples);

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
// A game that wants a buzz while something lasts asks for a long vibration (OVRPlugin: about
// 2 s) and renews it every frame, then stops it. When its frame thread stalls (a load, a hitch)
// neither the renewal nor the stop comes, and the last request buzzes for its full length.
// Renewed every frame, a few renewal intervals feel the same: once a hand's vibrations come
// kRenewStreak times in a row, each within kRenewNs of the last, each is cut to
// kCapIntervals intervals, at least kCapMinNs. A one-off rumble keeps its length.
struct RenewalCap {
  static constexpr std::int64_t kRenewNs = 250'000'000, kCapMinNs = 100'000'000;
  static constexpr int kRenewStreak = 3, kCapIntervals = 4;
  std::int64_t last = 0, interval = 0;
  int streak = 0;
  XrDuration duration(XrDuration requested, std::int64_t now) {
    const auto gap = last ? now - last : -1;
    if (gap >= 0 && gap <= kRenewNs) {
      interval = streak ? (interval * 3 + gap) / 4 : gap;
      ++streak;
    } else {
      streak = 0;
      interval = 0;
    }
    last = now ? now : 1;
    const XrDuration cap = std::max<XrDuration>(kCapMinNs, kCapIntervals * interval);
    return streak >= kRenewStreak && requested > cap ? cap : requested;
  }
};
// A core vibration (apply_haptic on the worker), replacing a sequence playing there. Amplitude
// 0 is a stop (queue_stop): Quest SDKs stop a vibration that way, and a runtime that ignores
// a silent request (SteamVR) kept the last one buzzing for its full length. A vibration renewed
// every frame is shortened (RenewalCap).
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
