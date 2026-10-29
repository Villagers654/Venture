#include "common/environment.h"
#include "core/linux_errno.h"
namespace {
constexpr int interrupted_wait = -venture::linux_errno::EINTR;
}
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <optional>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "audio/audio.h"
#include "platform/audio_backend.h"
#include "platform/audio_endpoints.h"
#include "platform/change_cache.h"
#include "av_telemetry.h"
#include <format>
#include <iterator>
#include "platform/audio_timer.h"
#include "platform/thread_priority.h"
#include "common/log.h"
#include "core/thread.h"
#include "core/signals.h"
#include "core/thread_runtime.h"
#include "core/wait.h"
#include "hle/aaudio/aaudio.h"
#include "hle/opensles/opensles.h"

namespace venture::audio {
// Opt-in capture of the final 48 kHz stereo mix passed to the host device.
// The device callback only counts/copies into preallocated memory; the monitor
// writes the WAV after publication, never doing file I/O on the audio thread.
class DebugCapture {
 public:
  explicit DebugCapture(const char* path) : path_(std::filesystem::u8path(path)) {
    double seconds = 15;
    if (const auto* value = venture::environment::getenv("VENTURE_AUDIO_CAPTURE_SECONDS")) seconds = std::stod(value);
    if (!std::isfinite(seconds) || seconds <= 0 || seconds > 120)
      throw std::runtime_error("VENTURE_AUDIO_CAPTURE_SECONDS must be in (0, 120]");
    if (const auto* value = venture::environment::getenv("VENTURE_AUDIO_CAPTURE_START_NS")) {
      start_ns_ = std::stoll(value);
      if (start_ns_ < 0) throw std::runtime_error("capture start must be nonnegative");
    }
    samples_.resize(static_cast<std::size_t>(seconds * 48000) * 2);
    if(const auto* value=venture::environment::getenv("VENTURE_AUDIO_CAPTURE_AFTER_API")) {
      after_api_=value;armed_=false;
      if(const auto* hash=venture::environment::getenv("VENTURE_AUDIO_CAPTURE_SOURCE_SHA256"))source_sha256_=hash;
      double delay=5;
      if(const auto* text=venture::environment::getenv("VENTURE_AUDIO_CAPTURE_API_DELAY"))delay=std::stod(text);
      if(!std::isfinite(delay) || delay<0 || delay>30)throw std::runtime_error("capture API delay must be in [0,30]");
      api_delay_ns_=static_cast<std::int64_t>(delay*1000000000);
    }
    // The harness publishes the window start when it reaches a route time (gated
    // routes make that host time unknown at launch): stay unarmed until the file exists.
    if (const auto* value = venture::environment::getenv("VENTURE_AUDIO_CAPTURE_START_FILE"); value && *value) {
      start_file_ = std::filesystem::u8path(value);
      armed_ = false;
    }
    if (start_ns_ >= 0) {
      const auto duration = std::int64_t(samples_.size() / 2) * 1'000'000'000 / 48000;
      if (start_ns_ > std::numeric_limits<std::int64_t>::max() - duration)
        throw std::runtime_error("capture deadline exceeds host clock range");
      end_ns_ = start_ns_ + duration;
    }
  }
  void source_started(const Stream& stream) {
    if(armed_.load(std::memory_order_acquire) || after_api_!=stream.config.api ||
       (!source_sha256_.empty() && source_sha256_!=stream.media_source_sha256))return;
    start_ns_=av::now_ns()+api_delay_ns_;
    end_ns_=start_ns_+std::int64_t(samples_.size()/2)*1000000000/48000;
    armed_.store(true,std::memory_order_release);
    av::emit("audio_capture_window",std::format("\"start_ns\":{},\"source_id\":{},\"api\":\"{}\"",
      start_ns_,stream.telemetry_id,stream.config.api));
  }
  void process(const float* samples, std::size_t count) {
    readers_.fetch_add(1);
    struct Leave { std::atomic<unsigned>& value; ~Leave() { value.fetch_sub(1); } } leave{readers_};
    if (!accepting_.load()) return;
    std::uint64_t audible = 0;
    float peak = peak_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < count; ++i) {
      const auto magnitude = std::abs(samples[i]);
      audible += magnitude > 1e-7f;
      peak = std::max(peak, magnitude);
    }
    total_.fetch_add(count, std::memory_order_relaxed);
    non_silent_.fetch_add(audible, std::memory_order_relaxed);
    peak_.store(peak, std::memory_order_relaxed);
    if (ready_.load(std::memory_order_relaxed)) return;
    if(!armed_.load(std::memory_order_acquire))return;
    std::size_t skip = 0;
    bool deadline_reached = false;
    if (start_ns_ >= 0) {
      const auto end = av::now_ns();
      const auto begin = end - std::int64_t(count / 2) * 1'000'000'000 / 48000;
      if (end <= start_ns_) return;
      if (begin >= end_ns_) {
        ready_.store(true, std::memory_order_release);
        return;
      }
      if (end >= end_ns_) {
        count = std::min(count, std::size_t((end_ns_ - begin) * 48000 / 1'000'000'000) * 2);
        deadline_reached = true;
      }
      if (begin < start_ns_) skip = std::min(count, std::size_t((start_ns_ - begin) * 48000 / 1'000'000'000) * 2);
    } else if (!used_ && !audible) return;
    samples += skip; count -= skip;
    const auto copy = std::min(count, samples_.size() - used_);
    std::copy_n(samples, copy, samples_.data() + used_);
    used_ += copy;
    if (deadline_reached || used_ == samples_.size()) ready_.store(true, std::memory_order_release);
  }
  void poll() {
    std::lock_guard lock(writer_mutex_);
    if (!start_file_.empty() && !armed_.load(std::memory_order_acquire)) arm_from_file();
    if (ready_.load(std::memory_order_acquire)) write();
    const auto now = std::chrono::steady_clock::now();
    if (now - last_report_ >= std::chrono::seconds(5)) { report(); last_report_ = now; }
  }
  // Stop capture admission, then finish the in-flight copy. The OS device and
  // guest producers keep running; no guest callback or device join is required.
  void finish() {
    accepting_.store(false);
    while (readers_.load()) std::this_thread::yield();
    std::lock_guard lock(writer_mutex_);
    write(); report();
  }
 private:
  std::filesystem::path path_;
  std::vector<float> samples_;
  std::size_t used_ = 0;
  bool written_ = false;
  std::atomic<bool> accepting_{true};
  std::atomic<unsigned> readers_{0};
  std::mutex writer_mutex_;
  std::int64_t start_ns_ = -1;
  std::int64_t end_ns_ = -1;
  std::string after_api_;
  std::string source_sha256_;
  std::int64_t api_delay_ns_=0;
  std::filesystem::path start_file_;
  std::chrono::steady_clock::time_point next_start_check_{};
  void arm_from_file() {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_start_check_) return;
    next_start_check_ = now + std::chrono::milliseconds(50);
    std::error_code missing;
    if (!std::filesystem::exists(start_file_, missing)) return;
    std::ifstream in(start_file_);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto key = text.find("\"start_ns\":");
    if (key == text.npos) return;
    start_ns_ = std::stoll(text.substr(key + 11));
    end_ns_ = start_ns_ + std::int64_t(samples_.size() / 2) * 1'000'000'000 / 48000;
    armed_.store(true, std::memory_order_release);
    av::emit("audio_capture_window", std::format("\"start_ns\":{},\"source\":\"route\"", start_ns_));
  }
  std::atomic<bool> armed_{true};
  std::atomic<bool> ready_{false};
  std::atomic<std::uint64_t> total_{0}, non_silent_{0};
  std::atomic<float> peak_{0};
  std::chrono::steady_clock::time_point last_report_ = std::chrono::steady_clock::now();
  void report() {
    log::info("audio", "Host mixer: {} samples, {} non-silent, peak {}", total_.load(), non_silent_.load(), peak_.load());
  }
  void write() {
    if (written_) return;
    written_ = true;
    try {
      if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path());
      std::ofstream out(path_, std::ios::binary);
      out.exceptions(std::ios::badbit | std::ios::failbit);
      auto u16 = [&](std::uint16_t n) { out.put(char(n)); out.put(char(n >> 8)); };
      auto u32 = [&](std::uint32_t n) { u16(std::uint16_t(n)); u16(std::uint16_t(n >> 16)); };
      out.write("RIFF", 4); u32(std::uint32_t(36 + used_ * 2)); out.write("WAVEfmt ", 8);
      u32(16); u16(1); u16(2); u32(48000); u32(192000); u16(4); u16(16);
      out.write("data", 4); u32(std::uint32_t(used_ * 2));
      for (std::size_t i = 0; i < used_; ++i)
        u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::clamp(samples_[i], -1.0f, 1.0f) * 32767)));
      out.close();
      log::info("audio", "Host mixer WAV: {} ({} stereo frames)", path_.string(), used_ / 2);
    } catch (const std::exception& error) { log::error("audio", "Mixer capture failed: {}", error.what()); }
  }
};
Backend::Backend() {
  // Construct the real writer before this backend registers its destructor.
  // Its final telemetry must drain before the writer joins and frees its queue;
  // constructing only the Telemetry facade does not establish that ordering.
  timing_enabled_ = av::enabled();
}
static_assert(std::atomic<Stream*>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<double>::is_always_lock_free);
namespace {
std::int64_t deadline(std::int64_t duration) {
  auto now = monotonic_ns();
  return duration > std::numeric_limits<std::int64_t>::max() - now
             ? std::numeric_limits<std::int64_t>::max()
             : now + duration;
}
}  // namespace
std::uint32_t sample_bytes(Format format) {
  switch (format) {
    case Format::i16:
      return 2;
    case Format::i24:
      return 3;
    case Format::f32:
    case Format::i32:
      return 4;
    case Format::u8:
      return 1;
  }
  return 0;
}
float decode_sample(const void* data, Format format) {
  switch (format) {
    case Format::u8:
      return (float(*static_cast<const std::uint8_t*>(data)) - 128) / 128;
    case Format::i16: {
      std::int16_t v;
      std::memcpy(&v, data, 2);
      return float(v) / 32768;
    }
    case Format::i32: {
      std::int32_t v;
      std::memcpy(&v, data, 4);
      return float(double(v) / 2147483648.0);
    }
    case Format::i24: {
      auto p = static_cast<const std::uint8_t*>(data);
      std::uint32_t v =
          p[0] | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16);
      if (v & 0x800000) v |= 0xff000000;
      return float(std::bit_cast<std::int32_t>(v)) / 8388608;
    }
    case Format::f32: {
      float v;
      std::memcpy(&v, data, 4);
      return std::isfinite(v) ? v : 0;
    }
  }
  return 0;
}
void encode_sample(void* data, Format format, float sample) {
  double v = std::isfinite(sample) ? std::clamp(double(sample), -1.0, 1.0) : 0;
  switch (format) {
    case Format::u8: {
      auto s = std::uint8_t(std::clamp(std::lrint(v * 128 + 128), 0l, 255l));
      std::memcpy(data, &s, 1);
      break;
    }
    case Format::i16: {
      auto s = std::int16_t(std::clamp(std::lrint(v * 32768), -32768l, 32767l));
      std::memcpy(data, &s, 2);
      break;
    }
    case Format::i32: {
      auto s = std::int32_t(
          std::clamp(v * 2147483648.0, -2147483648.0, 2147483647.0));
      std::memcpy(data, &s, 4);
      break;
    }
    case Format::i24: {
      auto s = std::uint32_t(
          std::int32_t(std::clamp(v * 8388608.0, -8388608.0, 8388607.0)));
      auto p = static_cast<std::uint8_t*>(data);
      p[0] = std::uint8_t(s);
      p[1] = std::uint8_t(s >> 8);
      p[2] = std::uint8_t(s >> 16);
      break;
    }
    case Format::f32: {
      auto s = float(v);
      std::memcpy(data, &s, 4);
      break;
    }
  }
}

Ring::Ring(std::uint32_t cap, std::uint32_t ch)
    : capacity(cap), channels(ch), data_(std::size_t(cap) * ch) {}
std::uint32_t Ring::readable() const {
  // Read the consumer first. The owner of either end gets a valid SPSC
  // snapshot; a third-party observer gets a conservative, bounded count.
  auto read = read_.load(std::memory_order_acquire);
  auto write = write_.load(std::memory_order_acquire);
  return std::uint32_t(std::min(std::uint64_t(capacity), write - read));
}
std::uint32_t Ring::writable() const { return capacity - readable(); }
float* Ring::write_pointer(std::uint32_t* count) {
  auto at = std::uint32_t(write_.load(std::memory_order_relaxed) % capacity);
  *count = std::min(writable(), capacity - at);
  return data_.data() + std::size_t(at) * channels;
}
const float* Ring::read_pointer(std::uint32_t* count) const {
  auto at = std::uint32_t(read_.load(std::memory_order_relaxed) % capacity);
  *count = std::min(readable(), capacity - at);
  return data_.data() + std::size_t(at) * channels;
}
void Ring::produced(std::uint32_t frames) {
  write_.fetch_add(frames, std::memory_order_release);
}
void Ring::consumed(std::uint32_t frames) {
  read_.fetch_add(frames, std::memory_order_release);
}
void Ring::reset() {
  read_.store(0);
  write_.store(0);
}

Stream::Stream(Config c)
    : config(c),
      ring(std::uint32_t(c.capacity), std::uint32_t(c.channels)),
      buffer_size(c.direction == Direction::input ? c.capacity : std::min(c.capacity, c.burst_frames * 2)),
      timing_enabled(av::enabled()) {}
std::shared_ptr<Stream> Stream::create(Config c, std::int32_t* result) {
  *result = illegal_argument;
  if (c.channels < 1 || c.channels > 2 || c.capacity < 1 ||
      c.capacity > 65536 || c.callback_frames < 1 ||
      c.callback_frames > c.capacity)
    return {};
  if (c.rate < 8000 || c.rate > 192000) {
    *result = invalid_rate;
    return {};
  }
  if (!sample_bytes(c.format)) {
    *result = invalid_format;
    return {};
  }
  if (!c.burst_frames) c.burst_frames = Backend::get().frames_per_burst(c.direction, c.rate);
  if (c.burst_frames < 1) { *result = unavailable; return {}; }
  if (c.burst_frames > c.capacity) { *result = out_of_range; return {}; }
  try {
    auto s = std::shared_ptr<Stream>(new Stream(c));
    bool output = c.direction == Direction::output;
    auto cv = ma_data_converter_config_init(
        ma_format_f32, ma_format_f32, output ? std::uint32_t(c.channels) : 2,
        output ? 2 : std::uint32_t(c.channels),
        output ? std::uint32_t(c.rate) : 48000,
        output ? 48000 : std::uint32_t(c.rate));
    // The linear resampler's low-pass filter handles downsampling. All storage
    // is allocated here; conversion on the device thread allocates nothing.
    if (ma_data_converter_init(&cv, nullptr, &s->converter_) != MA_SUCCESS) {
      *result = no_memory;
      return {};
    }
    s->converter_ready_ = true;
    *result = Backend::get().attach(*s);
    if (*result) return {};
    return s;
  } catch (const std::bad_alloc&) {
    *result = no_memory;
    return {};
  }
}
Stream::~Stream() {
  close();
  if (worker_.joinable()) {
    if (on_worker())
      worker_.detach();
    else
      worker_.join();
  }
  if (converter_ready_) ma_data_converter_uninit(&converter_, nullptr);
}
void Stream::set_state(State value) {
  state.store(value);
  notify();
}
void Stream::notify() {
  // Counts are established before the waiter observes its token/state. Keep
  // both operations sequentially consistent so a skipped wake cannot race
  // with enqueueing a waiter that still sees the old notification token.
  std::atomic_ref(event_).fetch_add(1);
  if (event_waiters_.load()) futex_wake(h2g(&event_), std::numeric_limits<int>::max(), sync_census::WakeSource::audio);
}
std::int32_t Stream::start() { return start_impl(false); }
std::int32_t Stream::start_resuming_drain() { return start_impl(true); }
std::int32_t Stream::start_impl(bool resume_stopping) {
  std::lock_guard lock(control_lock_);
  auto st = state.load();
  if (st == State::disconnected) return disconnected;
  if (st == State::closed || st == State::closing) return invalid_state;
  if (st == State::stopping && !resume_stopping) return invalid_state;
  if (st == State::started || st == State::starting) return 0;
  auto result = Backend::get().attach(*this);
  if (result) return result;
  const bool prefill = config.direction == Direction::output && config.prefill_output && !ring.readable();
  stop_position_.store(-1);
  set_state(prefill ? State::starting : State::started);
  Backend::get().playback_started(*this);
  return 0;
}
std::int32_t Stream::pause() {
  std::lock_guard lock(control_lock_);
  if (config.direction == Direction::input) return unimplemented;
  if (state == State::disconnected) return disconnected;
  if (state != State::started && state != State::starting && state != State::paused) return invalid_state;
  if (config.stop_at_written_position) write_interrupt_.fetch_add(1);
  set_state(State::paused);
  Backend::get().detach(*this);
  return 0;
}
std::int32_t Stream::stop() {
  std::lock_guard lock(control_lock_);
  if (state == State::disconnected) return disconnected;
  if (state == State::closed || state == State::closing) return invalid_state;
  if (config.stop_at_written_position) write_interrupt_.fetch_add(1);
  set_state(State::stopped);
  Backend::get().detach(*this);
  stop_position_.store(-1);
  // Input stop discards unread capture frames, as AAudio does.
  if (config.direction == Direction::input) {
    std::lock_guard io(io_lock);
    ring.reset();
    ma_data_converter_reset(&converter_);
    buffer_generation.fetch_add(1);
  }
  return 0;
}
std::int32_t Stream::request_stop() {
  if (config.direction == Direction::input) return stop();
  std::lock_guard lock(control_lock_);
  const auto st = state.load();
  if (st == State::disconnected) return disconnected;
  if (st == State::closed || st == State::closing) return invalid_state;
  if (st == State::stopping || st == State::stopped) return 0;
  if (config.stop_at_written_position) {
    // Finish an already executing mixer pull before publishing the boundary.
    // This waits for a callback, never for queued audio or endpoint progress.
    Backend::get().detach(*this);
    std::lock_guard io(io_lock);
    stop_position_.store(frames_written.load());
    write_interrupt_.fetch_add(1);
    set_state(State::stopping);
  }
  const auto result = Backend::get().attach(*this);
  if (result) return result;
  // Keep the output consumer attached. Even an empty queue must wait for an
  // in-flight guest callback to return before publishing STOPPED.
  set_state(State::stopping);
  return 0;
}
void Stream::complete_stop() {
  std::lock_guard control(control_lock_);
  if (state != State::stopping) return;
  std::lock_guard io(io_lock);
  if (config.stop_at_written_position ? frames_read.load() < stop_position_.load() : ring.readable() != 0) return;
  Backend::get().detach(*this);
  if (!config.stop_at_written_position) ring.reset();
  ma_data_converter_reset(&converter_);
  buffer_generation.fetch_add(1);
  set_state(State::stopped);
}
std::int32_t Stream::flush() { return flush_impl(false); }
std::int32_t Stream::flush_cancelling_drain() { return flush_impl(true); }
std::int32_t Stream::flush_impl(bool cancel_stopping) {
  std::lock_guard lock(control_lock_);
  if (config.direction == Direction::input) return unimplemented;
  if (state != State::paused && state != State::stopped &&
      state != State::open && state != State::flushed &&
      !(cancel_stopping && state == State::stopping))
    return invalid_state;
  Backend::get().detach(*this);
  std::lock_guard io(io_lock);
  // Retain monotonic frame counters: flushed output is treated as consumed.
  frames_read.fetch_add(ring.readable());
  ring.reset();
  if (config.stop_at_written_position) write_interrupt_.fetch_add(1);
  stop_position_.store(-1);
  ma_data_converter_reset(&converter_);
  buffer_generation.fetch_add(1);
  set_state(State::flushed);
  return 0;
}
namespace {
thread_local Stream* worker_stream = nullptr;
struct EventWaiter {
  std::atomic<unsigned>& count;
  explicit EventWaiter(std::atomic<unsigned>& value) : count(value) { count.fetch_add(1); }
  ~EventWaiter() { count.fetch_sub(1); }
};
}
bool Stream::on_worker() const { return worker_stream == this; }
void Stream::record_callback(std::uint64_t adopt_ns, std::uint64_t wall_ns,
                             std::int64_t cpu_ns, std::int64_t cpu_cycles,
                             std::uint64_t begin_ns, std::int64_t new_jit_blocks) {
  callback_count.fetch_add(1, std::memory_order_relaxed);
  callback_wall_ns.fetch_add(wall_ns, std::memory_order_relaxed);
  if (cpu_ns >= 0) {
    callback_cpu_ns.fetch_add(cpu_ns, std::memory_order_relaxed);
    callback_cpu_samples.fetch_add(1, std::memory_order_relaxed);
  }
  if (cpu_cycles >= 0) {
    callback_cpu_cycles.fetch_add(cpu_cycles, std::memory_order_relaxed);
    callback_cpu_cycles_samples.fetch_add(1, std::memory_order_relaxed);
  }
  // One callback worker owns these maxima; monitor reads are atomic.
  const auto previous_max = callback_wall_ns_max.load();
  callback_wall_ns_max.store(std::max(previous_max, wall_ns));
  callback_adopt_ns_max.store(std::max(callback_adopt_ns_max.load(), adopt_ns));
  callback_last_wall_ns.store(wall_ns, std::memory_order_relaxed);
  // Sparse worker-side events delimit a new long callback exactly. The
  // one-second monitor interval also includes normal polling waits, which
  // cannot be assigned to this callback from samples alone. No device-thread
  // formatting or file writes; the shared bounded writer owns delivery.
  if (begin_ns && wall_ns >= 10'000'000 && wall_ns > previous_max &&
      av::Telemetry::get().enabled()) {
    av::emit("audio_callback_peak", std::format(
        "\"stream_id\":{},\"guest_tid\":{},\"begin_ns\":{},\"end_ns\":{},"
        "\"wall_ns\":{},\"adopt_ns\":{},\"cpu_ns\":{},\"cpu_cycles\":{},\"new_jit_blocks\":{}",
        telemetry_id, callback_guest_tid.load(), begin_ns, begin_ns + wall_ns,
        wall_ns, adopt_ns, cpu_ns, cpu_cycles, new_jit_blocks));
  }
}
std::int32_t Stream::close(GuestThread* caller) {
  {
    std::lock_guard lock(control_lock_);
    if (state != State::closed) {
      quit.store(true);
      set_state(State::closing);
      Backend::get().detach(*this);
      set_state(State::closed);
      Backend::get().stream_closed(*this);
    }
  }
  // Never hold a control lock while joining: a callback can requestStop.
  if (!on_worker()) {
    if (caller) {
      EventWaiter waiting{event_waiters_};
      while (!worker_finished_.load(std::memory_order_acquire)) {
        auto token = std::atomic_ref(event_).load();
        if (worker_finished_.load(std::memory_order_acquire)) break;
        // Closing is committed; deliver signals through the core and finish
        // draining before freeing any callback data, even after EINTR.
        futex_wait(*caller, h2g(&event_), token, -1);
      }
    }
    std::lock_guard join(join_lock_);
    if (worker_.joinable()) worker_.join();
  }
  return 0;
}
void Stream::launch_worker(std::function<void(Stream&)> work) {
  auto self = shared_from_this();
  worker_finished_.store(false);
  try {
    worker_ = std::thread([self, work = std::move(work)] {
      worker_stream = self.get();
      try {
        std::unique_ptr<platform::AudioThreadPriority> priority;
        platform::AudioTimer timer;
        auto previous = monotonic_ns();
        while (!self->quit.load()) {
          if (self->guest_prepare_requested_.exchange(false, std::memory_order_acq_rel)) {
            const auto begin = monotonic_ns();
            // Retain the same scoped-audio binding used by normal callbacks,
            // but pay CPU/TLS/stack setup before guest-visible playback starts.
            {
              platform::AudioThreadPriority initialization_priority;
              auto& guest = GuestThread::adopt_current();
              self->callback_guest_tid.store(guest.tid(), std::memory_order_relaxed);
            }
            if (self->timing_enabled) av::emit("audio_worker_prepare", std::format(
                "\"source_id\":{},\"guest_tid\":{},\"duration_ns\":{}",
                self->telemetry_id, self->callback_guest_tid.load(), monotonic_ns() - begin));
            {
              std::lock_guard ready(self->prepare_lock_);
              self->guest_prepared_.store(true, std::memory_order_release);
            }
            self->prepare_changed_.notify_all();
          }
          // An adopted callback thread remains a live guest pthread between
          // callbacks, including after requestStop. Android can deliver GC or
          // teardown signals then; waiting for the next data callback loses
          // that acknowledgement when the stream is stopped. Never run guest
          // signal handlers under audio control, I/O or backend locks.
          if (auto* guest = GuestThread::current()) {
            check_guest_exit(*guest);
            if (const auto pending = guest->pending_signals.load() & ~guest->signal_mask_snapshot.load()) {
              if (shutdown_trace::enabled)
                log::info("shutdown", "host-callback signal delivery tid={} pending={:#x}", guest->tid(), pending);
              signals::deliver_pending(*guest);
            }
          }
          const auto st = self->state.load();
          const auto active = st == State::started || st == State::starting || st == State::stopping;
          sync_census::add(active ? sync_census::audio_active_wakes : sync_census::audio_idle_wakes);
          if (active && !priority)
            priority = std::make_unique<platform::AudioThreadPriority>();
          else if (!active)
            priority.reset();
          const auto start = monotonic_ns();
          if (self->timing_enabled) {
            const auto due = self->worker_due_ns.load(std::memory_order_relaxed);
            self->worker_wake_late_ns.store(due && std::uint64_t(start) > due ? start - due : 0);
            self->worker_begin_ns.store(start, std::memory_order_release);
          }
          const auto gap = std::uint64_t(start - previous);
          auto maximum = self->worker_gap_ns_max.load(std::memory_order_relaxed);
          while (gap > maximum && !self->worker_gap_ns_max.compare_exchange_weak(maximum, gap, std::memory_order_relaxed)) {}
          previous = start;
          self->refill_worker(work);
          self->complete_stop();
          const auto finish = monotonic_ns();
          const auto duration = std::uint64_t(finish - start);
          if (self->timing_enabled) self->worker_end_ns.store(finish, std::memory_order_release);
          maximum = self->worker_work_ns_max.load(std::memory_order_relaxed);
          while (duration > maximum && !self->worker_work_ns_max.compare_exchange_weak(maximum, duration, std::memory_order_relaxed)) {}
          // Callback streams have no blocking transfer waiter. Avoid entering
          // a shared guest futex bucket on every idle/refill tick unnecessarily.
          if (self->event_waiters_.load()) self->notify();
          if (self->timing_enabled) self->worker_due_ns.store(monotonic_ns() + 1'000'000);
          timer.wait(std::chrono::milliseconds(1));
        }
      } catch (const ThreadExit&) {
        // The producer is gone: the queue plays out and nothing follows, so
        // later short pulls are the source's end, not underruns.
        self->source_eos = true;
        self->quit = true;
      } catch (const ProcessExit&) {
        self->source_eos = true;
        self->quit = true;
      } catch (const std::exception& e) {
        log::error("audio", "Guest audio worker failed: {}", e.what());
        self->set_state(State::disconnected);
      }
      Backend::get().remember_finished_worker(*self);
      {
        std::lock_guard ready(self->prepare_lock_);
        self->worker_finished_.store(true, std::memory_order_release);
      }
      self->prepare_changed_.notify_all();
      self->notify();
    });
  } catch (...) {
    worker_finished_.store(true, std::memory_order_release);
    throw;
  }
}
std::int32_t Stream::prepare_guest_worker() {
  if (on_worker()) return invalid_state;
  std::unique_lock lock(prepare_lock_);
  if (worker_finished_.load(std::memory_order_acquire) || quit.load()) return invalid_state;
  if (guest_prepared_.load(std::memory_order_acquire)) return 0;
  guest_prepare_requested_.store(true, std::memory_order_release);
  prepare_changed_.wait(lock, [&] {
    return guest_prepared_.load(std::memory_order_acquire) || worker_finished_.load(std::memory_order_acquire);
  });
  return guest_prepared_.load(std::memory_order_acquire) && !quit.load() ? 0 : unavailable;
}
void Stream::refill_worker(const std::function<void(Stream&)>& work) {
  // Refill a depleted queue in one wake, without sleeping between callbacks.
  // Four batches bound guest execution; no staging beyond the declared size.
  for (unsigned batch = 0; batch < 4; ++batch) {
    const auto before = frames_written.load(std::memory_order_relaxed);
    work(*this);
    const auto st = state.load();
    if (quit.load() || (st != State::started && st != State::starting) || config.direction != Direction::output ||
        frames_written.load(std::memory_order_relaxed) == before ||
        ring.readable() >= std::uint32_t(buffer_size.load())) break;
  }
  auto expected = State::starting;
  if (config.prefill_output && ring.readable() &&
      state.compare_exchange_strong(expected, State::started)) notify();
}
std::uint32_t Stream::write(const void* data, std::uint32_t frames) {
  std::lock_guard lock(io_lock);
  if (state == State::stopping && !config.stop_at_written_position) return 0;
  auto bytes = sample_bytes(config.format);
  auto src = static_cast<const std::uint8_t*>(data);
  auto limit = std::max(0, buffer_size.load() - std::int32_t(ring.readable()));
  frames = std::min(frames, std::uint32_t(limit));
  std::uint32_t total = 0;
  std::uint64_t non_silent = 0;
  while (total < frames) {
    std::uint32_t n;
    auto p = ring.write_pointer(&n);
    n = std::min(n, frames - total);
    if (!n) break;
    for (std::uint32_t i = 0; i < n * config.channels; ++i) {
      p[i] = decode_sample(
          src + (std::size_t(total) * config.channels + i) * bytes,
          config.format);
      non_silent += p[i] != 0;
    }
    ring.produced(n);
    total += n;
  }
  frames_written.fetch_add(total);
  non_silent_written.fetch_add(non_silent, std::memory_order_relaxed);
  return total;
}
std::uint32_t Stream::read(void* data, std::uint32_t frames) {
  std::lock_guard lock(io_lock);
  auto bytes = sample_bytes(config.format);
  auto dest = static_cast<std::uint8_t*>(data);
  std::uint32_t total = 0;
  while (total < frames) {
    std::uint32_t n;
    auto p = ring.read_pointer(&n);
    n = std::min(n, frames - total);
    if (!n) break;
    for (std::uint32_t i = 0; i < n * config.channels; ++i)
      encode_sample(dest + (std::size_t(total) * config.channels + i) * bytes,
                    config.format, p[i]);
    ring.consumed(n);
    total += n;
  }
  frames_read.fetch_add(total);
  return total;
}
std::int32_t Stream::transfer(GuestThread* caller, void* data,
                              std::int32_t frames, std::int64_t duration,
                              bool writing) {
  if (frames < 0 || duration < 0) return illegal_argument;
  if (!data && frames) return null_pointer;
  if (writing != (config.direction == Direction::output)) return invalid_state;
  std::int32_t total = 0;
  auto end = deadline(duration);
  std::optional<EventWaiter> waiting;
  if (caller && duration) waiting.emplace(event_waiters_);
  auto frame_bytes = sample_bytes(config.format) * config.channels;
  const auto write_interrupt = config.stop_at_written_position ? write_interrupt_.load() : 0;
  while (total < frames) {
    auto token = std::atomic_ref(event_).load();
    auto st = state.load();
    if (st == State::disconnected) return total ? total : disconnected;
    if (st == State::closed || st == State::closing)
      return total ? total : invalid_state;
    if (writing && st == State::stopping && !config.stop_at_written_position) return total ? total : invalid_state;
    if (writing && config.stop_at_written_position && write_interrupt_.load() != write_interrupt) return total;
    if (!writing && st != State::started) return total ? total : invalid_state;
    auto at =
        static_cast<std::uint8_t*>(data) + std::size_t(total) * frame_bytes;
    total += std::int32_t(writing ? write(at, frames - total)
                                  : read(at, frames - total));
    if (writing && config.stop_at_written_position && st != State::started) break;
    if (total == frames || !duration || monotonic_ns() >= end) break;
    if (!caller) break;
    // 1 ms worker wakeups keep the host device callback free of OS futex locks.
    auto result = futex_wait(*caller, h2g(&event_), token, end);
    if (result == interrupted_wait) {
      caller->set_errno(-interrupted_wait);
      return total ? total : internal_error;
    }  // AAudio has no EINTR result.
  }
  return total;
}
std::int32_t Stream::wait_state(GuestThread& caller, State input, State* next,
                                std::int64_t duration) {
  if (!next) return null_pointer;
  if (duration < 0) return illegal_argument;
  auto end = deadline(duration);
  std::optional<EventWaiter> waiting;
  if (duration) waiting.emplace(event_waiters_);
  for (;;) {
    auto token = std::atomic_ref(event_).load();
    *next = state.load();
    if (*next != input) return 0;
    if (!duration || monotonic_ns() >= end) return timeout;
    if (futex_wait(caller, h2g(&event_), token, end) == interrupted_wait) {
      caller.set_errno(-interrupted_wait);
      return internal_error;
    }
  }
}
std::int32_t Stream::set_buffer_size(std::int32_t frames) {
  if (frames < 0) return out_of_range;
  // Legacy AudioRecord has no writable playback threshold to resize.
  auto actual = config.direction == Direction::input ? config.capacity :
      std::clamp(frames, config.burst_frames, config.capacity);
  buffer_size.store(actual);
  if (timing_enabled) av::emit("audio_buffer_size", std::format(
      "\"source_id\":{},\"requested_frames\":{},\"actual_frames\":{},\"capacity_frames\":{},\"callback_frames\":{}",
      telemetry_id, frames, actual, config.capacity, config.callback_frames));
  notify();
  return actual;
}
std::int32_t Stream::timestamp(std::int64_t* position,
                               std::int64_t* time) const {
  if (!position || !time) return null_pointer;
  if (state != State::started) return invalid_state;
  for (;;) {
    auto seq = stamp_seq_.load(std::memory_order_acquire);
    if (seq & 1) continue;
    auto p = stamp_position_.load();
    auto t = stamp_time_.load();
    if (seq == stamp_seq_.load(std::memory_order_acquire)) {
      if (!t) return unavailable;
      *position = p;
      *time = t;
      return 0;
    }
  }
}
void Stream::render(float* output, const float* input, std::uint32_t frames) {
  const auto st = state.load();
  const bool draining = st == State::stopping && config.direction == Direction::output;
  if ((st != State::started && !draining) || !frames) return;
  bool out = config.direction == Direction::output;
  const auto queue_before = timing_enabled && out ? ring.readable() : 0;
  std::uint32_t offset = 0;
  float scratch[1024 * 2];
  while (offset < frames) {
    std::uint32_t contiguous;
    ma_uint64 source, dest;
    ma_result result;
    if (out) {
      auto p = ring.read_pointer(&contiguous);
      if (draining && config.stop_at_written_position) {
        const auto stop_position = stop_position_.load();
        if (stop_position >= 0)
          contiguous = std::min(contiguous, static_cast<std::uint32_t>(
              std::max<std::int64_t>(0, stop_position - frames_read.load())));
      }
      if (!contiguous) break;
      source = contiguous;
      dest = std::min(1024u, frames - offset);
      result = ma_data_converter_process_pcm_frames(&converter_, p, &source,
                                                    scratch, &dest);
      ring.consumed(std::uint32_t(source));
      frames_read.fetch_add(std::int64_t(source));
      float l = gain_left.load(std::memory_order_relaxed),
            r = gain_right.load(std::memory_order_relaxed);
      for (std::uint32_t i = 0; i < dest; ++i) {
        output[2 * (offset + i)] += scratch[2 * i] * l;
        output[2 * (offset + i) + 1] += scratch[2 * i + 1] * r;
      }
      offset += std::uint32_t(dest);
    } else {
      auto p = ring.write_pointer(&contiguous);
      if (!contiguous) break;
      source = frames - offset;
      dest = contiguous;
      result = ma_data_converter_process_pcm_frames(
          &converter_, input + 2 * offset, &source, p, &dest);
      ring.produced(std::uint32_t(dest));
      frames_written.fetch_add(std::int64_t(dest));
      offset += std::uint32_t(source);
    }
    if (result != MA_SUCCESS || (!source && !dest)) break;
  }
  const bool short_pull = offset < frames && !draining && !source_eos.load(std::memory_order_acquire);
  if (short_pull) {
    lost_callbacks.fetch_add(1, std::memory_order_relaxed);
    if (!underrunning_) xruns.fetch_add(1);
    if (out) missing_frames.fetch_add(frames - offset, std::memory_order_relaxed);
    if (out && timing_enabled) Backend::get().record_underrun(*this, frames, offset, queue_before);
  }
  underrunning_ = short_pull;
  stamp_seq_.fetch_add(1);
  stamp_position_.store(out ? frames_read.load() : frames_written.load());
  stamp_time_.store(monotonic_ns());
  stamp_seq_.fetch_add(1, std::memory_order_release);
}

void Backend::finish_capture() {
  std::lock_guard lock(device_lock_);
  if (debug_capture_) debug_capture_->finish();
}
Backend& Backend::get() {
  static Backend b;
  return b;
}
namespace {
struct EnumerationFailed {}; // logged; never cached
std::vector<DeviceInfo> enumerate_default_devices() {
  ma_context context{};
  auto config = ma_context_config_init();
  platform::configure_audio_context(config);
  auto result = ma_context_init(nullptr, 0, &config, &context);
  if (result != MA_SUCCESS) {
    log::warn("audio", "Audio device enumeration failed: {}", static_cast<int>(result));
    throw EnumerationFailed{};
  }
  struct Cleanup { ma_context* context; ~Cleanup() { ma_context_uninit(context); } } cleanup{&context};
  if (context.backend == ma_backend_null) {
    log::warn("audio", "No real audio backend available for device enumeration");
    throw EnumerationFailed{};
  }
  ma_device_info *outputs = nullptr, *inputs = nullptr;
  ma_uint32 output_count = 0, input_count = 0;
  result = ma_context_get_devices(&context, &outputs, &output_count, &inputs, &input_count);
  if (result != MA_SUCCESS) {
    log::warn("audio", "Audio endpoint enumeration failed: {}", static_cast<int>(result));
    throw EnumerationFailed{};
  }
  std::vector<DeviceInfo> devices;
  for (ma_uint32 i = 0; i < output_count; ++i)
    if (outputs[i].isDefault) { devices.push_back({Direction::output, outputs[i].name}); break; }
  for (ma_uint32 i = 0; i < input_count; ++i)
    if (inputs[i].isDefault) { devices.push_back({Direction::input, inputs[i].name}); break; }
  return devices;
}
} // namespace
// Android AudioManager.getDevices answers from audio service state. A fresh
// enumeration builds a whole backend context (milliseconds per call), and
// titles poll it from their main thread, so keep the answer until the host
// reports an endpoint change.
std::vector<DeviceInfo> default_devices() {
  // Notifications may arrive until exit.
  static auto* const devices = new platform::WatchedQuery<std::vector<DeviceInfo>>(
      [](auto changed, void* context) { return platform::watch_audio_endpoints(changed, context); },
      enumerate_default_devices);
  try {
    return devices->get();
  } catch (const EnumerationFailed&) {
    return {};
  }
}
Backend::~Backend() {
  shutdown();
}
void Backend::shutdown() {
  if (shutdown_.exchange(true)) return;
  if (monitor_.joinable()) monitor_.join();
  shutdown_libraries();
  if (capture_ready_) ma_device_uninit(&capture_);
  if (playback_ready_) ma_device_uninit(&playback_);
  capture_ready_ = playback_ready_ = false;
  telemetry();
  if (debug_capture_) debug_capture_->finish();
  if (context_ready_) ma_context_uninit(&context_);
  context_ready_ = false;
}
bool Backend::use_null_device(bool start_device) {
  std::lock_guard lock(device_lock_);
  if (context_ready_) return false;
  null_ = true;
  start_null_ = start_device;
  return true;
}
Backend::Hardware Backend::hardware(Direction direction) {
  std::lock_guard lock(device_lock_);
  auto& d = direction == Direction::output ? playback_ : capture_;
  if (!(direction == Direction::output ? playback_ready_ : capture_ready_))
    return {};
  auto rate = direction == Direction::output ? d.playback.internalSampleRate
                                             : d.capture.internalSampleRate;
  auto channels = direction == Direction::output ? d.playback.internalChannels
                                                 : d.capture.internalChannels;
  auto format = direction == Direction::output ? d.playback.internalFormat
                                               : d.capture.internalFormat;
  std::int32_t guest_format = -1;
  switch (format) {
    case ma_format_s16:
      guest_format = 1;
      break;
    case ma_format_f32:
      guest_format = 2;
      break;
    case ma_format_s24:
      guest_format = 3;
      break;
    case ma_format_s32:
      guest_format = 4;
      break;
    default:
      break;
  }
  return {std::int32_t(rate), std::int32_t(channels), guest_format};
}
bool Backend::ensure_device(Direction direction) {
  if (!context_ready_) {
    ma_backend backend = ma_backend_null;
    auto context_config = ma_context_config_init();
    platform::configure_audio_context(context_config);
    if (ma_context_init(null_ ? &backend : nullptr, null_ ? 1 : 0,
                        &context_config, &context_) != MA_SUCCESS) {
      device_errors_.fetch_add(1);
      return false;
    }
    context_ready_ = true;
    // Never silently play into a null backend when the real device fails.
    if (!null_ && context_.backend == ma_backend_null) return false;
    if (const auto* path = venture::environment::getenv("VENTURE_AUDIO_CAPTURE_WAV")) {
      try { debug_capture_ = std::make_unique<DebugCapture>(path); }
      catch (const std::exception& error) { log::error("audio", "Cannot enable mixer capture: {}", error.what()); }
    }
    monitor_ = std::thread([this] {
      const bool report = venture::environment::getenv("VENTURE_AUDIO_STATS") != nullptr;
      auto next_report = monotonic_ns() + 5'000'000'000;
      auto next_av = monotonic_ns() + 1'000'000'000;
      bool reconnect_output = false;
      std::int64_t next_reconnect = 0;
      while (!shutdown_.load()) {
        if (debug_capture_) debug_capture_->poll();
        if (monotonic_ns() >= next_av) {
          telemetry(); next_av = monotonic_ns() + 1'000'000'000;
        }
        if (report && monotonic_ns() >= next_report) {
          const auto stats = mixer_stats();
          log::info("audio", "Host mixer: output_frames={}, non_silent_samples={}, peak={}",
                    stats.output_frames, stats.non_silent_samples, stats.peak);
          std::lock_guard lock(device_lock_);
          for (std::size_t i = 0; i < slots_.size(); ++i) {
            auto* s = slots_[i].stream.load();
            if (!s || s->config.direction != Direction::output) continue;
            log::info("audio", "PCM source {}: state={}, written={}, consumed={}, non_silent_samples={}, gain=({},{}) xruns={}",
                      i, int(s->state.load()), s->frames_written.load(), s->frames_read.load(),
                      s->non_silent_written.load(), s->gain_left.load(), s->gain_right.load(), s->xruns.load());
          }
          next_report = monotonic_ns() + 5'000'000'000;
        }
        for (auto direction : {Direction::output, Direction::input}) {
          bool lost = direction == Direction::output
                          ? lost_output_.exchange(false)
                          : lost_input_.exchange(false);
          if (!lost) continue;
          device_errors_.fetch_add(1);
          endpoint_changed_.store(true);
          log::error("audio", "Default {} device stopped; notifying streams and recovering logical output tracks",
                     direction == Direction::output ? "output" : "capture");
          std::lock_guard lock(device_lock_);
          for (auto& slot : slots_)
            if (auto* s = slot.stream.load();
                s && s->config.direction == direction) {
              if (direction == Direction::output && s->config.follow_default_output)
                reconnect_output = true;
              else s->set_state(State::disconnected);
            }
          if (direction == Direction::output)
            av::emit("audio_endpoint", std::format("\"event\":\"lost\",\"ready\":false,\"backend\":\"{}\",\"endpoint_id_hash\":\"{}\",\"device_errors\":{},\"reroutes\":{},\"recoveries\":{}",
              ma_get_backend_name(context_.backend), endpoint_hash_, device_errors_.load(), reroutes_.load(), recoveries_.load()));
          auto& ready =
              direction == Direction::output ? playback_ready_ : capture_ready_;
          auto& device = direction == Direction::output ? playback_ : capture_;
          if (ready) {
            auto& suppressed = suppressed_stops_[std::size_t(direction)];
            suppressed.store(true);
            ma_device_uninit(&device);
            ready = false;
            suppressed.store(false);
          }
        }
        if (reconnect_output && monotonic_ns() >= next_reconnect) {
          std::lock_guard lock(device_lock_);
          const bool wanted = std::any_of(slots_.begin(), slots_.end(), [](const Slot& slot) {
            auto* s = slot.stream.load();
            return s && s->config.direction == Direction::output && s->config.follow_default_output;
          });
          if (!wanted) reconnect_output = false;
          else if (playback_ready_ || ensure_device(Direction::output)) {
            reconnect_output = false;
            recoveries_.fetch_add(1);
            endpoint_changed_.store(true);
            log::info("audio", "Recovered default output for logical audio tracks");
          }
          next_reconnect = monotonic_ns() + 100'000'000;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });
  }
  if (!null_ && context_.backend == ma_backend_null) return false;
  bool& ready =
      direction == Direction::output ? playback_ready_ : capture_ready_;
  if (ready) return true;
  auto& device = direction == Direction::output ? playback_ : capture_;
  auto cfg = ma_device_config_init(direction == Direction::output
                                       ? ma_device_type_playback
                                       : ma_device_type_capture);
  cfg.playback.format = cfg.capture.format = ma_format_f32;
  cfg.playback.channels = cfg.capture.channels = 2;
  cfg.sampleRate = 48000;
  cfg.periodSizeInMilliseconds = 5;
  cfg.periods = 2;
  cfg.performanceProfile = ma_performance_profile_low_latency;
  cfg.dataCallback = data_callback;
  cfg.notificationCallback = notification_callback;
  cfg.pUserData = this;
  // pDeviceID == nullptr follows the system default. miniaudio 0.11.23's
  // WASAPI autoStreamRouting reroutes on endpoint/default changes; SDL's
  // default devices move with the PipeWire/PulseAudio/ALSA default.
  if (ma_device_init(&context_, &cfg, &device) != MA_SUCCESS) { device_errors_.fetch_add(1); return false; }
  (direction == Direction::output ? lost_output_ : lost_input_).store(false);
  if ((!null_ || start_null_) && ma_device_start(&device) != MA_SUCCESS) {
    device_errors_.fetch_add(1);
    auto& suppressed = suppressed_stops_[std::size_t(direction)];
    suppressed.store(true);
    ma_device_uninit(&device);
    suppressed.store(false);
    return false;
  }
  ready = true;
  endpoint_changed_.store(true);
  // The endpoint's own format, which Venture converts its 48 kHz mix to; in debug reports.
  auto fact = [&](const char *kind, const auto &endpoint) {
    log::info("facts", "Audio {} device '{}' via {}: mix {} Hz {} ch; endpoint {} Hz {} ch {}, period {} frames",
              kind, endpoint.name, ma_get_backend_name(context_.backend), device.sampleRate, endpoint.channels,
              endpoint.internalSampleRate, endpoint.internalChannels, ma_get_format_name(endpoint.internalFormat),
              endpoint.internalPeriodSizeInFrames);
  };
  if (direction == Direction::output) fact("output", device.playback);
  else fact("capture", device.capture);
  return true;
}
std::int32_t Backend::attach(Stream& stream) {
  std::lock_guard lock(device_lock_);
  if (stream.slot_ >= 0) return 0;
  if (!ensure_device(stream.config.direction)) {
    log::error("audio", "Cannot open default audio device");
    return unavailable;
  }
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    if (!slots_[i].stream.load()) {
      if (!stream.telemetry_id) {
        stream.telemetry_id = next_source_id_++;
        if (stream.config.direction == Direction::output) ++opened_output_streams_;
      }
      stream.slot_ = int(i);
      slots_[i].stream.store(&stream);
      return 0;
    }
  }
  return unavailable;
}
void Backend::playback_started(const Stream& stream) {
  std::lock_guard lock(device_lock_);
  if(debug_capture_ && stream.config.direction==Direction::output)debug_capture_->source_started(stream);
}
void Backend::detach(Stream& stream) {
  std::lock_guard lock(device_lock_);
  if (stream.slot_ < 0) { remember(stream); return; }
  auto& slot = slots_[std::size_t(stream.slot_)];
  slot.stream.store(nullptr);
  // readers increments before loading the pointer. Seq-cst closes the race
  // between acquiring a stream and destruction; no shared_ptr refcount or
  // reclamation runs on the real-time thread. Only control threads wait here.
  while (slot.readers.load()) std::this_thread::yield();
  stream.underrunning_ = false;
  stream.slot_ = -1;
  remember(stream);
  if (stream.config.direction == Direction::input && capture_ready_ &&
      std::none_of(slots_.begin(), slots_.end(), [](const Slot& s) {
        auto p = s.stream.load();
        return p && p->config.direction == Direction::input;
      })) {
    // Release the microphone as soon as the last capturing stream stops or
    // closes. The output endpoint can remain warm for fast game restarts.
    suppressed_stops_[1].store(true);
    ma_device_uninit(&capture_);
    capture_ready_ = false;
    suppressed_stops_[1].store(false);
  }
}
std::pair<std::int32_t, std::int32_t> Backend::output_properties() {
  std::lock_guard lock(device_lock_);
  if (!ensure_device(Direction::output)) return {};
  // Report the actual opened endpoint's period in mixer-rate frames, accounting
  // for miniaudio's resampling between the device and the 48 kHz mixer.
  auto& output = playback_.playback;
  const auto frames = output.internalSampleRate ?
      (std::uint64_t(output.internalPeriodSizeInFrames) * playback_.sampleRate +
       output.internalSampleRate - 1) / output.internalSampleRate : 0;
  return {std::int32_t(playback_.sampleRate), std::int32_t(frames)};
}
std::int32_t Backend::frames_per_burst(Direction direction, std::int32_t guest_rate) {
  std::lock_guard lock(device_lock_);
  if (!ensure_device(direction)) return 0;
  const auto& device = direction == Direction::output ? playback_ : capture_;
  const auto rate = direction == Direction::output ? device.playback.internalSampleRate : device.capture.internalSampleRate;
  const auto period = direction == Direction::output ? device.playback.internalPeriodSizeInFrames : device.capture.internalPeriodSizeInFrames;
  if (!rate || !period) return refill_quantum(guest_rate);
  return std::int32_t((std::uint64_t(period) * guest_rate + rate - 1) / rate);
}
bool Backend::output_active() {
  // The same pin protocol as process() protects streams against detach.
  for (auto& slot : slots_) {
    slot.readers.fetch_add(1);
    auto* stream = slot.stream.load();
    const bool active = stream && stream->config.direction == Direction::output &&
                        (stream->state == State::started || stream->state == State::stopping);
    slot.readers.fetch_sub(1);
    if (active) return true;
  }
  return false;
}
void Backend::guest_sources_ended() {
  for (auto& slot : slots_) {
    slot.readers.fetch_add(1);
    if (auto* s = slot.stream.load()) s->source_eos = true;
    slot.readers.fetch_sub(1);
  }
  av::emit("guest_audio_sources_ended", "");
}
void Backend::process(Direction direction, float* output, const float* input,
                      std::uint32_t frames) {
  if (output && timing_enabled_) {
    previous_output_pull_ns_ = output_pull_ns_;
    output_pull_ns_ = monotonic_ns();
  }
  if (output) std::fill_n(output, std::size_t(frames) * 2, 0);
  for (auto& slot : slots_) {
    slot.readers.fetch_add(1);
    auto* s = slot.stream.load();
    if (s && s->config.direction == direction) s->render(output, input, frames);
    slot.readers.fetch_sub(1);
  }
  if (output) {
    std::uint64_t non_silent = 0;
    float peak = 0;
    double energy = 0;
    for (std::size_t i = 0; i < std::size_t(frames) * 2; ++i) {
      output[i] = std::clamp(output[i], -1.0f, 1.0f);
      energy += double(output[i]) * output[i];
      const float magnitude = std::abs(output[i]);
      if (magnitude > 0) ++non_silent;
      peak = std::max(peak, magnitude);
    }
    mix_energy_.fetch_add(energy, std::memory_order_relaxed);
    output_frames_.fetch_add(frames, std::memory_order_relaxed);
    non_silent_samples_.fetch_add(non_silent, std::memory_order_relaxed);
    auto previous = output_peak_.load(std::memory_order_relaxed);
    while (peak > previous && !output_peak_.compare_exchange_weak(
               previous, peak, std::memory_order_relaxed)) {}
  }
}
void Backend::record_underrun(const Stream& s, std::uint32_t requested,
                               std::uint32_t delivered, std::uint32_t queue_before) {
  const auto write = underrun_write_.load(std::memory_order_relaxed);
  if (write - underrun_read_.load(std::memory_order_acquire) >= underrun_events_.size()) {
    underrun_dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  underrun_events_[write % underrun_events_.size()] = {
      output_pull_ns_, s.telemetry_id, previous_output_pull_ns_,
      s.worker_begin_ns.load(), s.worker_end_ns.load(), s.worker_due_ns.load(),
      s.worker_wake_late_ns.load(), s.callback_adopt_begin_ns.load(),
      s.callback_begin_ns.load(), s.callback_end_ns.load(), s.callback_last_wall_ns.load(),
      requested, delivered, queue_before, s.config.rate, s.buffer_size.load(),
      s.config.capacity, s.config.callback_frames, s.callback_guest_tid.load(), s.xruns.load()};
  underrun_write_.store(write + 1, std::memory_order_release);
}
void Backend::drain_underruns() {
  auto read = underrun_read_.load(std::memory_order_relaxed);
  const auto write = underrun_write_.load(std::memory_order_acquire);
  while (read < write) {
    const auto event = underrun_events_[read % underrun_events_.size()];
    ++read;
    underrun_read_.store(read, std::memory_order_release);
    av::emit("audio_underrun", std::format(
        "\"event_ns\":{},\"source_id\":{},\"previous_pull_ns\":{},\"worker_begin_ns\":{},\"worker_end_ns\":{},\"worker_due_ns\":{},\"wake_late_ns\":{},\"adopt_begin_ns\":{},\"callback_begin_ns\":{},\"callback_end_ns\":{},\"callback_wall_ns\":{},\"requested_frames\":{},\"delivered_frames\":{},\"queue_before_frames\":{},\"rate\":{},\"buffer_size_frames\":{},\"capacity_frames\":{},\"callback_frames\":{},\"guest_tid\":{},\"xruns\":{},\"events_dropped\":{}",
        event.time, event.source, event.previous_pull, event.worker_begin, event.worker_end,
        event.worker_due, event.wake_late, event.adopt_begin, event.callback_begin, event.callback_end,
        event.callback_wall, event.requested, event.delivered, event.queue_before, event.rate,
        event.size, event.capacity, event.callback_frames, event.guest_tid, event.xruns, underrun_dropped_.load()));
  }
}
Backend::MixerStats Backend::mixer_stats() const {
  return {output_frames_.load(std::memory_order_relaxed),
          non_silent_samples_.load(std::memory_order_relaxed),
          output_peak_.load(std::memory_order_relaxed)};
}
void Backend::data_callback(ma_device* device, void* output, const void* input,
                            ma_uint32 frames) {
  auto* b = static_cast<Backend*>(device->pUserData);
  b->process(device->type == ma_device_type_capture ? Direction::input
                                                    : Direction::output,
             static_cast<float*>(output), static_cast<const float*>(input),
             frames);
  if (output && device->type != ma_device_type_capture && b->debug_capture_)
    b->debug_capture_->process(static_cast<const float*>(output), std::size_t(frames) * 2);
}
void Backend::notification_callback(
    const ma_device_notification* notification) {
  auto* b = static_cast<Backend*>(notification->pDevice->pUserData);
  auto capture = notification->pDevice->type == ma_device_type_capture;
  if (notification->type == ma_device_notification_type_rerouted && !capture) {
    b->reroutes_.fetch_add(1); b->endpoint_changed_.store(true);
  }
  if (notification->type == ma_device_notification_type_stopped &&
      !b->shutdown_.load() && !b->suppressed_stops_[capture ? 1 : 0].load()) {
    (notification->pDevice->type == ma_device_type_capture ? b->lost_input_
                                                           : b->lost_output_)
        .store(true);
  }
}
void Backend::stream_closed(Stream& s) {
  if (s.config.direction != Direction::output || !s.telemetry_id) return;
  std::lock_guard lock(device_lock_);
  ++closed_output_streams_;
  remember(s);
}
void Backend::remember_finished_worker(Stream& s) {
  if (s.state != State::closed || s.config.direction != Direction::output || !s.telemetry_id) return;
  // Close can detach while guest code is still running. Retain the final
  // callback/worker counters before publishing worker completion to close.
  std::lock_guard lock(device_lock_);
  remember(s);
}
void Backend::remember(Stream& s) {
  if (s.config.direction != Direction::output || !s.telemetry_id) return;
  SourceStats value{s.telemetry_id, s.lost_callbacks.load(), s.missing_frames.load(),
    std::uint64_t(s.frames_written.load()), std::uint64_t(s.frames_read.load()),
    int(s.ring.readable()), s.buffer_size.load(), s.config.rate, int(s.state == State::closing ? State::closed : s.state.load()), s.config.api, s.worker_work_ns_max.load(), s.worker_gap_ns_max.load(),
    s.callback_count.load(), s.callback_wall_ns.load(), s.callback_cpu_ns.load(),
    s.callback_cpu_samples.load(), s.callback_wall_ns_max.load(), s.callback_adopt_ns_max.load(), s.config.callback_frames, int(s.ring.capacity), s.callback_cpu_cycles.load(), s.callback_cpu_cycles_samples.load(), s.callback_guest_tid.load(),s.non_silent_written.load(),s.gain_left.load(),s.gain_right.load(),s.xruns.load()};
  auto found = std::find_if(source_history_.begin(), source_history_.end(), [&](const auto& v) { return v.id == value.id; });
  if (found == source_history_.end()) source_history_.push_back(std::move(value));
  else *found = std::move(value);
}
void Backend::telemetry() {
  if (!av::Telemetry::get().enabled()) return;
  std::lock_guard lock(device_lock_);
  drain_underruns();
  for (auto& slot : slots_) if (auto* s = slot.stream.load(); s && s->config.direction == Direction::output) remember(*s);
  if (endpoint_changed_.exchange(false)) {
    // Hash endpoint identity, never publish a raw name or ID. Resolve on monitor.
    std::uint64_t hash = 14695981039346656037ull;
    ma_device_info info{};
    if (playback_ready_ && ma_device_get_info(&playback_, ma_device_type_playback, &info) == MA_SUCCESS) {
      for (auto byte : std::string_view(reinterpret_cast<const char*>(&info.id), sizeof(info.id))) { hash ^= static_cast<unsigned char>(byte); hash *= 1099511628211ull; }
      endpoint_hash_ = std::format("{:016x}", hash);
    }
    av::emit("audio_endpoint", std::format("\"backend\":\"{}\",\"endpoint_id_hash\":\"{}\",\"event\":\"snapshot\",\"ready\":{},\"reroutes\":{},\"device_errors\":{},\"recoveries\":{}",
      context_ready_ ? ma_get_backend_name(context_.backend) : "unavailable", endpoint_hash_, playback_ready_, reroutes_.load(), device_errors_.load(), recoveries_.load()));
  }
  std::string sources;
  std::uint64_t underruns = 0, missing = 0;
  for (const auto& v : source_history_) {
    underruns += v.underruns; missing += v.missing;
    if (!sources.empty()) sources += ',';
    sources += std::format("{{\"id\":{},\"api\":\"{}\",\"state\":{},\"rate\":{},\"written\":{},\"consumed\":{},\"underruns\":{},\"missing_frames\":{},\"queue_frames\":{},\"watermark_frames\":{},\"worker_work_ns_max\":{},\"worker_gap_ns_max\":{},\"callback_count\":{},\"callback_wall_ns\":{},\"callback_cpu_ns\":{},\"callback_cpu_samples\":{},\"callback_wall_ns_max\":{},\"callback_adopt_ns_max\":{},\"callback_frames\":{},\"capacity_frames\":{},\"callback_cpu_cycles\":{},\"callback_cpu_cycles_samples\":{},\"callback_guest_tid\":{},\"non_silent_samples\":{},\"gain_left\":{},\"gain_right\":{},\"xrun_episodes\":{}}}",
      v.id, v.api, v.state, v.rate, v.written, v.consumed, v.underruns, v.missing, v.queue, v.watermark, v.work_ns, v.gap_ns, v.callbacks, v.callback_wall, v.callback_cpu, v.callback_cpu_samples, v.callback_wall_max, v.callback_adopt_max, v.callback_frames, v.capacity, v.callback_cycles, v.callback_cycles_samples, v.callback_guest_tid,v.non_silent,std::isfinite(v.gain_left)?v.gain_left:0.0f,std::isfinite(v.gain_right)?v.gain_right:0.0f,v.xrun_episodes);
  }
  double device_buffer_ms = 0;
  std::uint64_t device_period_frames = 0;
  if (playback_ready_ && playback_.playback.internalSampleRate) {
    device_period_frames = (std::uint64_t(playback_.playback.internalPeriodSizeInFrames) *
        playback_.sampleRate + playback_.playback.internalSampleRate - 1) /
        playback_.playback.internalSampleRate;
    auto buffer_frames = playback_.playback.internalPeriodSizeInFrames * playback_.playback.internalPeriods;
#ifdef _WIN32
    if (context_.backend == ma_backend_wasapi && playback_.wasapi.actualBufferSizeInFramesPlayback)
      buffer_frames = playback_.wasapi.actualBufferSizeInFramesPlayback;
#endif
    device_buffer_ms = double(buffer_frames) * 1000 / playback_.playback.internalSampleRate;
  }
  const auto frames = output_frames_.load();
  // Energy and frame counts are cumulative; differences give the last interval.
  const auto energy = mix_energy_.load();
  static std::uint64_t last_frames = 0;
  static double last_energy = 0;
  const auto rms = frames > last_frames ? std::sqrt(std::max(0.0, energy-last_energy) / ((frames-last_frames)*2)) : 0.0;
  const auto db = rms > 0 ? 20 * std::log10(rms) : -160.0;
  av::emit("audio_stats", std::format("\"opened_output_streams\":{},\"closed_output_streams\":{},\"device_buffer_ms\":{},\"device_period_frames\":{},\"output_frames\":{},\"rms_dbfs\":{},\"interval_frames\":{},\"underruns\":{},\"missing_frames\":{},\"device_errors\":{},\"reroutes\":{},\"recoveries\":{},\"backend\":\"{}\",\"endpoint_id_hash\":\"{}\",\"streams\":[{}]",
    opened_output_streams_, closed_output_streams_, device_buffer_ms, device_period_frames, frames, db, frames-last_frames, underruns, missing, device_errors_.load(), reroutes_.load(), recoveries_.load(), context_ready_ ? ma_get_backend_name(context_.backend) : "unavailable", endpoint_hash_, sources));
  last_frames = frames; last_energy = energy;
}
void register_libraries() {
  hle::aaudio::register_library();
  hle::opensles::register_library();
}
void shutdown_libraries() {
  hle::aaudio::shutdown_library();
  hle::opensles::shutdown_library();
}
}  // namespace venture::audio
