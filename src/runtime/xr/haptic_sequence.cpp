#include "xr/haptic_sequence.h"
#include "xr/haptic_output.h"
#include "core/wait.h"
#include "core/av_telemetry.h"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <limits>
#include <mutex>
#include <tuple>
#include <thread>

namespace venture::xr {
std::vector<HapticSample> pcm_envelope(std::span<const float> input, float rate,
                                      std::size_t capacity, std::uint32_t &consumed) {
  consumed = 0;
  std::vector<HapticSample> out;
  if (!std::isfinite(rate) || rate <= 0 || input.empty()) return out;
  const double stride = std::max(1.0, double(rate) / haptic_emulation_rate);
  while (consumed < input.size() && out.size() < capacity) {
    auto end = std::min(input.size(), std::max(std::size_t(consumed) + 1,
        std::size_t(std::min(double(input.size()), std::ceil(double(out.size() + 1) * stride)))));
    double energy = 0;
    for (auto i = consumed; i < end; ++i) {
      const double sample = std::clamp(double(input[i]), -1.0, 1.0);
      energy += sample * sample;
    }
    auto duration = XrDuration(std::llround(double(end - consumed) * 1e9 / rate));
    out.push_back({float(std::sqrt(energy / (end - consumed))), std::max<XrDuration>(1, duration)});
    consumed = static_cast<std::uint32_t>(end);
  }
  return out;
}
namespace {
using Key = std::tuple<XrSession, XrAction, XrPath>;
struct Stream {
  XrHapticActionInfo action;
  std::deque<HapticSample> samples;
  std::int64_t due;
  bool pcm;
};
struct Scheduler {
  std::mutex mutex;
  std::condition_variable changed;
  std::map<Key, Stream> streams;
  bool stopping = false;
  std::thread worker;
  Scheduler() {
    // Ensure diagnostics outlive any producer during static shutdown.
    (void)av::enabled();
    worker = std::thread([this] { run(); });
  }
  ~Scheduler() {
    { std::lock_guard lock(mutex); stopping = true; }
    changed.notify_one();
    worker.join();
  }
  void run() {
    std::unique_lock lock(mutex);
    while (!stopping) {
      auto now = monotonic_ns();
      std::int64_t delay = 1'000'000'000;
      for (auto it = streams.begin(); it != streams.end();) {
        auto &s = it->second;
        if (s.due <= now && s.samples.empty()) { it = streams.erase(it); continue; }
        if (s.due <= now) {
          auto sample = s.samples.front();
          s.samples.pop_front();
          // Coalesce identical samples, preserving cancellation via the lock.
          while (!s.samples.empty() && s.samples.front().amplitude == sample.amplitude) {
            sample.duration += s.samples.front().duration;
            s.samples.pop_front();
          }
          XrHapticVibration pulse{XR_TYPE_HAPTIC_VIBRATION, nullptr, sample.duration,
                                  XR_FREQUENCY_UNSPECIFIED, sample.amplitude};
          auto r = apply_haptic(s.pcm ? "xr_pcm" : "xr_envelope", std::get<0>(it->first), s.action, pulse);
          if (r != XR_SUCCESS) { it = streams.erase(it); continue; }
          s.due = sample.duration > std::numeric_limits<std::int64_t>::max() - now
              ? std::numeric_limits<std::int64_t>::max() : now + sample.duration;
        }
        delay = std::min(delay, std::max<std::int64_t>(1, s.due - now));
        ++it;
      }
      changed.wait_for(lock, std::chrono::nanoseconds(delay));
    }
  }
};
Scheduler &scheduler() { static Scheduler s; return s; }
bool matches(const Key &key, XrSession session, const XrHapticActionInfo *info) {
  return std::get<0>(key) == session && (!info || (std::get<1>(key) == info->action &&
      (!info->subactionPath || !std::get<2>(key) || std::get<2>(key) == info->subactionPath)));
}
}
void cancel_sequence(XrSession session, const XrHapticActionInfo *info) {
  auto &s = scheduler();
  std::lock_guard lock(s.mutex);
  for (auto it = s.streams.begin(); it != s.streams.end();) {
    if (matches(it->first, session, info)) {
      stop_haptic(it->second.pcm ? "xr_pcm" : "xr_envelope", session, it->second.action);
      it = s.streams.erase(it);
    } else ++it;
  }
  s.changed.notify_one();
}
XrResult queue_haptic(XrSession session, const XrHapticActionInfo &action,
                      std::span<const float> input, float rate, bool append, std::uint32_t &consumed) {
  auto &s = scheduler();
  std::lock_guard lock(s.mutex);
  Key key{session, action.action, action.subactionPath};
  auto it = s.streams.find(key);
  // Append only preserves the exact preceding PCM stream. Wildcard-hand
  // streams and non-PCM effects that overlap this request must be replaced.
  for (auto old = s.streams.begin(); old != s.streams.end();) {
    bool preserve = append && old->first == key && old->second.pcm;
    if (matches(old->first, session, &action) && !preserve) {
      stop_haptic("xr_pcm", session, old->second.action);
      old = s.streams.erase(old);
    } else ++old;
  }
  it = s.streams.find(key);
  if (it == s.streams.end()) {
    // A process has bounded active streams as well as bounded samples.
    if (s.streams.size() >= 64) { consumed = 0; return XR_ERROR_LIMIT_REACHED; }
    it = s.streams.emplace(key, Stream{action, {}, monotonic_ns(), true}).first;
  }
  auto samples = pcm_envelope(input, rate, 4000 - it->second.samples.size(), consumed);
  for (auto sample : samples) it->second.samples.push_back(sample);
  s.changed.notify_one();
  return XR_SUCCESS;
}
XrResult queue_envelope(XrSession session, const XrHapticActionInfo &action,
                        std::span<const float> input, XrDuration duration) {
  cancel_sequence(session, &action);
  auto &s = scheduler();
  std::lock_guard lock(s.mutex);
  if (s.streams.size() >= 64) return XR_ERROR_LIMIT_REACHED;
  Stream stream{action, {}, monotonic_ns(), false};
  // Downsample dense envelopes to the emulation cadence, retaining energy.
  // Sparse/long envelopes keep their whole duration without unbounded storage.
  auto count = std::min(input.size(), std::size_t(std::max<XrDuration>(1,
      duration / 4'000'000 + (duration % 4'000'000 != 0))));
  for (std::size_t i = 0; i < count; ++i) {
    auto first = i * input.size() / count;
    auto last = (i + 1) * input.size() / count;
    double energy = 0;
    for (auto j = first; j < last; ++j) energy += double(input[j]) * input[j];
    auto begin = XrDuration((long double)duration * i / count);
    auto end = XrDuration((long double)duration * (i + 1) / count);
    stream.samples.push_back({float(std::sqrt(energy / (last - first))), end - begin});
  }
  s.streams[{session, action.action, action.subactionPath}] = std::move(stream);
  s.changed.notify_one();
  return XR_SUCCESS;
}
}
