#include "xr/haptic_sequence.h"
#include "xr/haptic_output.h"
#include "common/log.h"
#include "core/wait.h"
#include "core/av_telemetry.h"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <format>
#include <map>
#include <limits>
#include <mutex>
#include <string>
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
Key key_of(XrSession session, const XrHapticActionInfo &info) { return {session, info.action, info.subactionPath}; }
bool matches(const Key &key, XrSession session, const XrHapticActionInfo *info) {
  return std::get<0>(key) == session && (!info || (std::get<1>(key) == info->action &&
      (!info->subactionPath || !std::get<2>(key) || std::get<2>(key) == info->subactionPath)));
}
// Emulated pulses this far behind their schedule (the host's calls took longer than the
// pulses they play) are dropped rather than played late, so output never lags further.
constexpr std::int64_t kLateNs = 50'000'000;
// A partly consumed native PCM buffer is offered again after this long.
constexpr std::int64_t kPcmRetryNs = 4'000'000;
// Waiting one-shot requests (vibrations, stops, envelopes) beyond this drop the oldest.
constexpr std::size_t kMaxOps = 64;
constexpr std::int64_t kSlowCallNs = 50'000'000;

struct Op {
  enum class Kind { vibrate, stop, envelope } kind;
  Key key;
  XrHapticActionInfo action;
  std::string api, stage;
  XrHapticVibration pulse{XR_TYPE_HAPTIC_VIBRATION};
  std::vector<float> amplitudes;
  XrDuration duration = 0;
};
struct Stream {
  XrHapticActionInfo action;
  std::uint64_t id;
  bool native = false;
  // Emulated (core pulses): samples still to play and when the next one starts.
  std::deque<HapticSample> samples;
  std::int64_t due = 0;
  bool pcm = true;
  // Native PCM: samples the host has not taken yet.
  std::deque<float> pending;
  float rate = 0;
  bool append_next = false;
  bool sending = false;
};
struct Pulse { Key key; std::uint64_t id; XrHapticActionInfo action; bool pcm; XrHapticVibration pulse; };
struct Send { Key key; std::uint64_t id; XrHapticActionInfo action; std::vector<float> samples; float rate; bool append; };

class Worker {
 public:
  std::mutex mutex;
  std::condition_variable changed, settled;
  std::deque<Op> ops;
  std::map<Key, Stream> streams;
  std::vector<Key> active;  // targets with a host call running now
  std::uint64_t next_id = 1;
  bool stopping = false;
  std::uint64_t dropped = 0, slow_calls = 0, late_pulses = 0;
  std::int64_t worst_call_ns = 0, last_report_ns = 0;
  std::thread thread;

  Worker() {
    // Ensure diagnostics outlive any producer during static shutdown.
    (void)av::enabled();
    thread = std::thread([this] { run(); });
  }
  ~Worker() {
    { std::lock_guard lock(mutex); stopping = true; }
    changed.notify_one();
    thread.join();
  }
  // --- under mutex ---------------------------------------------------------------------------
  void push(Op op) {
    op.action.next = nullptr;  // the caller's chain does not outlive its call
    if (op.kind == Op::Kind::stop) {
      std::erase_if(ops, [&](const Op &old) {
        return old.kind == Op::Kind::stop ? old.key == op.key : matches(old.key, std::get<0>(op.key), &op.action);
      });
    } else {
      std::erase_if(ops, [&](const Op &old) { return old.kind != Op::Kind::stop && old.key == op.key; });
    }
    if (ops.size() >= kMaxOps) {
      auto oldest = std::find_if(ops.begin(), ops.end(), [](const Op &o) { return o.kind != Op::Kind::stop; });
      ops.erase(oldest == ops.end() ? ops.begin() : oldest);
      ++dropped;
    }
    ops.push_back(std::move(op));
    changed.notify_one();
  }
  void stop(std::string_view api, std::string_view stage, XrSession session, const XrHapticActionInfo &action) {
    push(Op{Op::Kind::stop, key_of(session, action), action, std::string(api), std::string(stage)});
  }
  // Ends the sequences `keep` does not name that match, stopping their output.
  template <class Keep> void withdraw(XrSession session, const XrHapticActionInfo *info, Keep keep) {
    for (auto it = streams.begin(); it != streams.end();) {
      if (matches(it->first, session, info) && !keep(it->first, it->second)) {
        const auto action = it->second.action;
        const char *api = it->second.native ? "xr_pcm" : it->second.pcm ? "xr_pcm" : "xr_envelope";
        it = streams.erase(it);
        stop(api, "host_stop", session, action);
      } else ++it;
    }
  }
  void withdraw(XrSession session, const XrHapticActionInfo *info) {
    withdraw(session, info, [](const Key &, const Stream &) { return false; });
  }
  bool busy(XrSession session, const XrHapticActionInfo *info) const {
    for (const auto &op : ops)
      if (matches(op.key, session, info)) return true;
    for (const auto &key : active)
      if (matches(key, session, info)) return true;
    return false;
  }
  // --- worker --------------------------------------------------------------------------------
  void run() {
    std::unique_lock lock(mutex);
    while (!stopping) {
      auto batch = std::move(ops);
      ops.clear();
      const auto now = monotonic_ns();
      std::int64_t delay = 1'000'000'000;
      std::vector<Pulse> pulses;
      std::vector<Send> sends;
      for (auto it = streams.begin(); it != streams.end();) {
        auto &s = it->second;
        if (s.native) {
          if (s.sending) { ++it; continue; }
          if (s.pending.empty()) { it = streams.erase(it); continue; }
          if (s.due > now) { delay = std::min(delay, s.due - now); ++it; continue; }
          const auto count = std::min<std::size_t>(s.pending.size(), XR_MAX_HAPTIC_PCM_BUFFER_SIZE_FB);
          sends.push_back({it->first, s.id, s.action, std::vector<float>(s.pending.begin(), s.pending.begin() + count),
                           s.rate, s.append_next});
          s.sending = true;
          ++it;
          continue;
        }
        while (!s.samples.empty() && s.due < now && now - s.due - kLateNs >= s.samples.front().duration) {
          s.due += s.samples.front().duration;
          s.samples.pop_front();
          ++late_pulses;
        }
        if (s.due <= now && s.samples.empty()) { it = streams.erase(it); continue; }
        if (s.due <= now) {
          auto sample = s.samples.front();
          s.samples.pop_front();
          // Coalesce identical samples, preserving cancellation via the lock.
          while (!s.samples.empty() && s.samples.front().amplitude == sample.amplitude) {
            sample.duration += s.samples.front().duration;
            s.samples.pop_front();
          }
          pulses.push_back({it->first, s.id, s.action, s.pcm,
                            {XR_TYPE_HAPTIC_VIBRATION, nullptr, sample.duration, XR_FREQUENCY_UNSPECIFIED, sample.amplitude}});
          // Each pulse starts where the one before it ends.
          s.due = sample.duration > std::numeric_limits<std::int64_t>::max() - s.due
              ? std::numeric_limits<std::int64_t>::max() : s.due + sample.duration;
        }
        delay = std::min(delay, std::max<std::int64_t>(1, s.due - now));
        ++it;
      }
      if (batch.empty() && pulses.empty() && sends.empty()) {
        report(now);
        changed.wait_for(lock, std::chrono::nanoseconds(delay));
        continue;
      }
      active.clear();
      for (const auto &op : batch) active.push_back(op.key);
      for (const auto &p : pulses) active.push_back(p.key);
      for (const auto &s : sends) active.push_back(s.key);
      lock.unlock();
      // Host calls: none of the worker's state is locked while they run.
      for (const auto &op : batch) timed([&] { execute(op); });
      std::vector<std::pair<Key, std::uint64_t>> failed;
      for (const auto &p : pulses) {
        XrResult r = XR_SUCCESS;
        timed([&] { r = apply_haptic(p.pcm ? "xr_pcm" : "xr_envelope", std::get<0>(p.key), p.action, p.pulse); });
        if (r != XR_SUCCESS) failed.emplace_back(p.key, p.id);
      }
      std::vector<std::tuple<Key, std::uint64_t, XrResult, std::uint32_t>> sent;
      for (const auto &s : sends) {
        std::uint32_t consumed = 0;
        XrResult r = XR_SUCCESS;
        timed([&] { r = send(s, consumed); });
        sent.emplace_back(s.key, s.id, r, consumed);
      }
      lock.lock();
      for (const auto &[key, id] : failed)
        if (auto it = streams.find(key); it != streams.end() && it->second.id == id) streams.erase(it);
      const auto after = monotonic_ns();
      for (const auto &[key, id, r, consumed] : sent) {
        auto it = streams.find(key);
        if (it == streams.end() || it->second.id != id) continue;
        auto &s = it->second;
        s.sending = false;
        if (XR_FAILED(r)) { streams.erase(it); continue; }
        const auto taken = std::min<std::size_t>(consumed, s.pending.size());
        s.pending.erase(s.pending.begin(), s.pending.begin() + std::ptrdiff_t(taken));
        if (taken) s.append_next = true;
        // The host's buffer is full: offer the rest again shortly.
        s.due = taken ? after : after + kPcmRetryNs;
      }
      active.clear();
      settled.notify_all();
    }
  }
  template <class F> void timed(F &&call) {
    const auto start = monotonic_ns();
    call();
    const auto spent = monotonic_ns() - start;
    if (spent < kSlowCallNs) return;
    std::lock_guard lock(mutex);
    ++slow_calls;
    worst_call_ns = std::max(worst_call_ns, spent);
  }
  // At most every ten seconds, under mutex.
  void report(std::int64_t now) {
    if (!slow_calls && !dropped && !late_pulses) return;
    if (now - last_report_ns < 10'000'000'000) return;
    last_report_ns = now;
    std::string parts;
    auto add = [&](std::string part) { parts += (parts.empty() ? "" : "; ") + part; };
    if (slow_calls)
      add(std::format("{} host call(s) took {} ms or more (worst {} ms)", slow_calls, kSlowCallNs / 1'000'000,
                      worst_call_ns / 1'000'000));
    if (late_pulses) add(std::format("{} emulated pulse(s) skipped as too late", late_pulses));
    if (dropped) add(std::format("{} waiting request(s) dropped for newer ones", dropped));
    log::warn("xr", "Haptics behind a slow host since the last report: {}", parts);
    slow_calls = dropped = late_pulses = 0;
    worst_call_ns = 0;
  }
  static void execute(const Op &op) {
    const auto session = std::get<0>(op.key);
    switch (op.kind) {
      case Op::Kind::vibrate: apply_haptic(op.api, session, op.action, op.pulse, op.stage); break;
      case Op::Kind::stop: stop_haptic(op.api, session, op.action, op.stage); break;
      case Op::Kind::envelope: {
        const float scale = vibration_scale(session, op.action);
        if (scale <= 0) break;
        std::vector<float> scaled(op.amplitudes);
        for (auto &a : scaled) a *= std::min(scale, 1.f);
        XrHapticAmplitudeEnvelopeVibrationFB envelope{XR_TYPE_HAPTIC_AMPLITUDE_ENVELOPE_VIBRATION_FB, nullptr,
            op.duration, std::uint32_t(scaled.size()), scaled.data()};
        const auto r = ::xrApplyHapticFeedback(session, &op.action, reinterpret_cast<const XrHapticBaseHeader *>(&envelope));
        if (av::enabled()) {
          float peak = 0;
          for (auto a : op.amplitudes) peak = std::max(peak, a);
          haptic_event("xr_envelope", session, op.action, peak, op.duration, 0, "xrApplyHapticFeedback_native", r,
              {{"payload_type", int(XR_TYPE_HAPTIC_AMPLITUDE_ENVELOPE_VIBRATION_FB)}, {"stage", "guest_request"}});
        }
        break;
      }
    }
  }
  static XrResult send(const Send &s, std::uint32_t &consumed) {
    const auto session = std::get<0>(s.key);
    const float scale = vibration_scale(session, s.action);
    // Vibration off: taken and dropped, as for an unbound output.
    if (scale <= 0) { consumed = std::uint32_t(s.samples.size()); return XR_SUCCESS; }
    // Mixed PCM clips can overshoot their nominal [-1,1] range; the host gets them bounded.
    std::vector<float> samples(s.samples.size());
    float peak = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
      samples[i] = std::clamp(s.samples[i], -1.f, 1.f) * std::min(scale, 1.f);
      peak = std::max(peak, std::abs(s.samples[i]));
    }
    XrHapticPcmVibrationFB pcm{XR_TYPE_HAPTIC_PCM_VIBRATION_FB, nullptr, std::uint32_t(samples.size()), samples.data(),
                               s.rate, XrBool32(s.append ? XR_TRUE : XR_FALSE), &consumed};
    const auto r = ::xrApplyHapticFeedback(session, &s.action, reinterpret_cast<const XrHapticBaseHeader *>(&pcm));
    if (av::enabled())
      haptic_event("xr_pcm", session, s.action, peak, XrDuration(double(samples.size()) * 1e9 / s.rate), 0,
          "xrApplyHapticFeedback_native", r,
          {{"payload_type", int(XR_TYPE_HAPTIC_PCM_VIBRATION_FB)}, {"stage", "guest_request"},
           {"buffer_size", samples.size()}, {"sample_rate", s.rate}, {"append", s.append},
           {"samples_consumed", consumed}});
    return r;
  }
};
Worker &worker() { static Worker w; return w; }
}  // namespace

void cancel_sequence(XrSession session, const XrHapticActionInfo *info, bool wait) {
  auto &w = worker();
  std::unique_lock lock(w.mutex);
  std::erase_if(w.ops, [&](const Op &op) { return op.kind != Op::Kind::stop && matches(op.key, session, info); });
  w.withdraw(session, info);
  w.changed.notify_one();
  if (wait) w.settled.wait(lock, [&] { return !w.busy(session, info); });
}
bool haptics_settled(std::chrono::milliseconds timeout) {
  auto &w = worker();
  std::unique_lock lock(w.mutex);
  return w.settled.wait_for(lock, timeout, [&] { return w.ops.empty() && w.active.empty(); });
}
void queue_vibration(std::string_view api, XrSession session, const XrHapticActionInfo &action,
                     const XrHapticVibration &pulse, std::string_view stage) {
  auto &w = worker();
  std::lock_guard lock(w.mutex);
  w.withdraw(session, &action);
  Op op{Op::Kind::vibrate, key_of(session, action), action, std::string(api), std::string(stage)};
  op.pulse = pulse;
  op.pulse.next = nullptr;
  w.push(std::move(op));
}
void queue_stop(std::string_view api, XrSession session, const XrHapticActionInfo &action, std::string_view stage) {
  auto &w = worker();
  std::lock_guard lock(w.mutex);
  w.withdraw(session, &action);
  w.stop(api, stage, session, action);
}
XrResult queue_haptic(XrSession session, const XrHapticActionInfo &action,
                      std::span<const float> input, float rate, bool append, std::uint32_t &consumed) {
  auto &w = worker();
  std::lock_guard lock(w.mutex);
  const Key key = key_of(session, action);
  // Append only preserves the exact preceding PCM stream. Wildcard-hand
  // streams and non-PCM effects that overlap this request must be replaced.
  w.withdraw(session, &action, [&](const Key &k, const Stream &s) { return append && k == key && s.pcm && !s.native; });
  const auto now = monotonic_ns();
  auto it = w.streams.find(key);
  if (it == w.streams.end()) {
    // A process has bounded active streams as well as bounded samples.
    if (w.streams.size() >= 64) { consumed = 0; return XR_ERROR_LIMIT_REACHED; }
    it = w.streams.emplace(key, Stream{action, w.next_id++}).first;
    it->second.due = now;
  } else if (it->second.samples.empty()) {
    // A drained stream continues from now, not from where it ran out.
    it->second.due = std::max(it->second.due, now);
  }
  // As a host paces PCM: a whole buffer when nothing waits, and otherwise only up to
  // native_pcm_backlog waiting, so a game that keeps appending (H3VR2's Haptics SDK)
  // re-offers the rest later instead of queueing seconds of buzz behind the player.
  // Waiting is time: the rest of the pulse playing now (identical samples play as one
  // long pulse) and the pulses still queued.
  std::int64_t waiting = std::max<std::int64_t>(0, it->second.due - now);
  for (const auto &sample : it->second.samples) waiting += sample.duration;
  constexpr auto pulse_ns = std::int64_t(1e9 / haptic_emulation_rate);
  const auto room = 4000 - it->second.samples.size();
  const std::size_t capacity = waiting == 0 ? room
      : waiting >= native_pcm_backlog ? 0
      : std::min<std::size_t>(room, std::size_t((native_pcm_backlog - waiting + pulse_ns - 1) / pulse_ns));
  auto samples = pcm_envelope(input, rate, capacity, consumed);
  for (auto sample : samples) it->second.samples.push_back(sample);
  w.changed.notify_one();
  return XR_SUCCESS;
}
XrResult queue_envelope(XrSession session, const XrHapticActionInfo &action,
                        std::span<const float> input, XrDuration duration) {
  auto &w = worker();
  std::lock_guard lock(w.mutex);
  std::erase_if(w.ops, [&](const Op &op) { return op.kind != Op::Kind::stop && matches(op.key, session, &action); });
  w.withdraw(session, &action);
  if (w.streams.size() >= 64) return XR_ERROR_LIMIT_REACHED;
  Stream stream{action, w.next_id++};
  stream.due = monotonic_ns();
  stream.pcm = false;
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
  w.streams[key_of(session, action)] = std::move(stream);
  w.changed.notify_one();
  return XR_SUCCESS;
}
std::uint32_t queue_native_pcm(XrSession session, const XrHapticActionInfo &action, std::span<const float> input,
                               float rate, bool append) {
  auto &w = worker();
  std::lock_guard lock(w.mutex);
  const Key key = key_of(session, action);
  // The hand's own native stream is continued or, without append, replaced below: no stop
  // between two clips, as a host replaces its own buffer.
  w.withdraw(session, &action, [&](const Key &k, const Stream &s) { return k == key && s.native; });
  auto it = w.streams.find(key);
  if (it == w.streams.end() || !append) {
    // A new clip replaces what waits (and, sent without append, what the host plays).
    if (it != w.streams.end()) w.streams.erase(it);
    Stream stream{action, w.next_id++};
    stream.native = true;
    stream.append_next = append;
    it = w.streams.emplace(key, std::move(stream)).first;
  }
  auto &s = it->second;
  s.rate = rate;
  const auto offered = std::min<std::size_t>(input.size(), XR_MAX_HAPTIC_PCM_BUFFER_SIZE_FB);
  const auto backlog = std::max<std::size_t>(1, std::size_t(std::ceil(double(rate) * native_pcm_backlog / 1e9)));
  // Pending includes a buffer the worker is offering the host now.
  const auto room = s.pending.empty() ? offered
      : s.pending.size() >= backlog ? 0 : std::min(offered, backlog - s.pending.size());
  s.pending.insert(s.pending.end(), input.begin(), input.begin() + std::ptrdiff_t(room));
  if (room) {
    s.due = std::min(s.due, monotonic_ns());
    w.changed.notify_one();
  }
  return std::uint32_t(room);
}
void queue_native_envelope(XrSession session, const XrHapticActionInfo &action, std::span<const float> input,
                           XrDuration duration) {
  auto &w = worker();
  std::lock_guard lock(w.mutex);
  w.withdraw(session, &action);
  Op op{Op::Kind::envelope, key_of(session, action), action, "xr_envelope", "guest_request"};
  op.amplitudes.assign(input.begin(), input.end());
  op.duration = duration;
  w.push(std::move(op));
}
}
