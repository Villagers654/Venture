#pragma once

#include <miniaudio.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <utility>

#include "audio/output_mirror.h"
#include "core/guest.h"

namespace venture {
class GuestThread;
}
namespace venture::audio {
class DebugCapture;

// Android AAudio.h values (API 32). I24 is packed little endian, not I32.
enum class Format : std::int32_t { i16 = 1, f32 = 2, i24 = 3, i32 = 4, u8 = 5 };
enum class Direction : std::int32_t { output = 0, input = 1 };
// The mixer requests five-millisecond refills. Keep enough guest frames for
// that duration at fractional rates (44.1 kHz needs 221, not 220 frames).
inline constexpr std::int32_t refill_quantum(std::int32_t rate) {
  return rate > 0 ? 1 + (rate - 1) / 200 : 1;
}
// Stable IDs for the two default routes supported by the mixer, not physical
// endpoint IDs. Enumeration never opens a capture stream.
inline constexpr std::int32_t default_output_id = 1, default_input_id = 2;
struct DeviceInfo { Direction direction; std::string name; };
std::vector<DeviceInfo> default_devices();
// Whether a WASAPI endpoint ID ("{0.0.0.00000000}.{guid}") is the endpoint an
// XR runtime named: the same ID, or the endpoint GUID alone, case-insensitive.
bool wasapi_endpoint_matches(std::string_view endpoint_id, std::string_view named);
// AudioDeviceInfo.getId() for a default endpoint.
inline std::int32_t device_id(const DeviceInfo& device) {
  return device.direction == Direction::input ? default_input_id : default_output_id;
}
enum class State : std::int32_t {
  uninitialized = 0,
  unknown,
  open,
  starting,
  started,
  pausing,
  paused,
  flushing,
  flushed,
  stopping,
  stopped,
  closing,
  closed,
  disconnected
};
inline constexpr std::int32_t disconnected = -899, illegal_argument = -898,
                              internal_error = -896, invalid_state = -895,
                              invalid_handle = -892, unimplemented = -890,
                              unavailable = -889, no_memory = -887,
                              null_pointer = -886, timeout = -885,
                              invalid_format = -883, out_of_range = -882,
                              invalid_rate = -880;

std::uint32_t sample_bytes(Format format);
float decode_sample(const void* data, Format format);
void encode_sample(void* data, Format format, float sample);

// SPSC ring in guest-rate float frames. Public HLE read/write calls serialize
// their side separately. Acquire/release publishes samples before positions.
class Ring {
 public:
  Ring(std::uint32_t capacity, std::uint32_t channels);
  std::uint32_t readable() const;
  std::uint32_t writable() const;
  float* write_pointer(std::uint32_t* contiguous);
  const float* read_pointer(std::uint32_t* contiguous) const;
  void produced(std::uint32_t frames);
  void consumed(std::uint32_t frames);
  void reset();  // only while producer and consumer are quiescent
  const std::uint32_t capacity, channels;

 private:
  std::vector<float> data_;
  alignas(64) std::atomic<std::uint64_t> read_{0};
  alignas(64) std::atomic<std::uint64_t> write_{0};
};

struct Config {
  Direction direction = Direction::output;
  Format format = Format::i16;
  std::int32_t rate = 48000, channels = 2, capacity = 960,
               callback_frames = 240;
  std::int32_t burst_frames = 0;  // negotiated device quantum in guest-rate frames
  const char* api = "native";
  // Android AudioTrack/OpenSL tracks belong to the logical default mixer.
  bool follow_default_output = false;
  // Legacy AudioRecord and OpenSL recorders follow the default input across a
  // lost microphone, as Android restores their routes; AAudio input reports
  // DISCONNECTED instead.
  bool follow_default_input = false;
  // Streaming AudioTrack drains only frames written before stop; later writes
  // prefill the next play and writes while inactive never wait for consumption.
  bool stop_at_written_position = false;
  // Callback producers may fill their first block while STARTING. Rendering
  // begins only after that preparation, rather than counting startup silence
  // as missed playback. Blocking-write streams retain explicit prefill.
  bool prefill_output = false;
};

class Stream : public std::enable_shared_from_this<Stream> {
 public:
  static std::shared_ptr<Stream> create(Config config, std::int32_t* result);
  ~Stream();
  // AudioTrack can resume while an asynchronous stop is still playing out.
  std::int32_t start();
  std::int32_t start_resuming_drain();
  std::int32_t pause();
  std::int32_t stop();
  // AAudio output stop drains queued PCM and the current callback first.
  std::int32_t request_stop();
  std::int32_t flush();
  std::int32_t flush_cancelling_drain();
  std::int32_t close(GuestThread* caller = nullptr);
  std::int32_t transfer(GuestThread* caller, void* data, std::int32_t frames,
                        std::int64_t timeout_ns, bool writing);
  // Nonblocking operations for the guest-callback worker / OpenSL queue.
  std::uint32_t write(const void* data, std::uint32_t frames);
  std::uint32_t read(void* data, std::uint32_t frames);
  std::int32_t wait_state(GuestThread& caller, State input, State* next,
                          std::int64_t timeout_ns);
  std::int32_t timestamp(std::int64_t* position, std::int64_t* time) const;
  std::int32_t set_buffer_size(std::int32_t frames);
  void set_state(State value);
  void notify();  // worker/control threads only, never the host device callback
  // The worker invokes this without holding stream locks. Returning false
  // requests STOP. No guest code executes on the miniaudio device thread.
  void launch_worker(std::function<void(Stream&)> work);
  std::int32_t prepare_guest_worker();
  bool on_worker() const;
  const Config config;
  Ring ring;
  std::atomic<State> state{State::open};
  std::atomic<std::int32_t> buffer_size;
  std::atomic<std::int64_t> frames_written{0}, frames_read{0};
  // Decoded producer samples, before gain/resampling; distinguishes source
  // silence from a muted or starved host output in VENTURE_AUDIO_STATS reports.
  std::atomic<std::uint64_t> non_silent_written{0};
  std::atomic<std::int32_t> xruns{0};
  // Android counts underrun edges; acceptance retains every short host pull.
  std::atomic<std::uint64_t> lost_callbacks{0};
  std::atomic<std::uint64_t> missing_frames{0};
  // Producer-side dropouts: digital silence the guest itself wrote between
  // sounding samples (an abrupt cut to exact zero, 4800 frames or shorter).
  // A guest mixer that misses its deadline pads with zeros; the host device
  // never underruns then, but the output crackles all the same.
  std::atomic<std::uint64_t> guest_gaps{0}, guest_gap_frames{0};
  std::uint64_t telemetry_id = 0;
  // Immutable identity for an opt-in source-specific media measurement.
  std::string media_source_sha256;
  std::atomic<std::uint64_t> worker_work_ns_max{0}, worker_gap_ns_max{0};
  std::atomic<std::uint64_t> callback_count{0}, callback_wall_ns{0},
      callback_cpu_ns{0}, callback_cpu_samples{0}, callback_wall_ns_max{0},
      callback_adopt_ns_max{0};
  std::atomic<std::uint64_t> callback_cpu_cycles{0}, callback_cpu_cycles_samples{0};
  std::atomic<std::int32_t> callback_guest_tid{0};
  // Opt-in correlation only. The device copies numeric snapshots into a
  // bounded SPSC queue; monitor formatting and file writes stay off realtime.
  const bool timing_enabled;
  std::atomic<std::uint64_t> worker_begin_ns{0}, worker_end_ns{0},
      worker_due_ns{0}, worker_wake_late_ns{0}, callback_adopt_begin_ns{0},
      callback_begin_ns{0}, callback_end_ns{0}, callback_last_wall_ns{0};
  void record_callback(std::uint64_t adopt_ns, std::uint64_t wall_ns,
                       std::int64_t cpu_ns, std::int64_t cpu_cycles = -1,
                       std::uint64_t begin_ns = 0, std::int64_t new_jit_blocks = -1);
  std::atomic<float> gain_left{1}, gain_right{1};
  std::atomic<bool> quit{false};
  // A finite decoded source has deliberately reached EOS. Short host reads
  // after its last queued sample are normal silence, not producer starvation.
  std::atomic<bool> source_eos{false};
  // The guest has queued nothing and no callback of its is running (an OpenSL
  // player left PLAYING with an empty buffer queue): the silence that follows
  // is the game's choice, not a producer missing its deadline.
  std::atomic<bool> guest_idle{false};
  std::atomic<std::uint64_t> buffer_generation{0};
  std::mutex io_lock;

 private:
  explicit Stream(Config config);
  std::int32_t start_impl(bool resume_stopping);
  std::int32_t flush_impl(bool cancel_stopping);
  friend class Backend;
  friend struct StreamTestAccess;
  std::recursive_mutex control_lock_;
  std::mutex join_lock_;
  std::thread worker_;
  std::atomic<bool> worker_finished_{true};
  std::atomic<bool> guest_prepare_requested_{false}, guest_prepared_{false};
  std::mutex prepare_lock_;
  std::condition_variable prepare_changed_;
  std::atomic<unsigned> event_waiters_{0};
  void refill_worker(const std::function<void(Stream&)>& work);
  alignas(4) std::uint32_t event_ =
      0;  // std::atomic_ref: core futex is a plain guest word
  std::atomic<std::uint64_t> stamp_seq_{0};
  std::atomic<std::int64_t> stamp_position_{0}, stamp_time_{0};
  ma_data_converter converter_{};
  bool converter_ready_ = false;
  std::atomic<std::int64_t> stop_position_{-1};
  std::atomic<std::uint64_t> write_interrupt_{0};
  void complete_stop();
  int slot_ = -1;
  bool underrunning_ = false;  // device-owned; reset after detach drains readers
  // Producer-owned (io_lock) dropout detector state.
  std::uint32_t zero_run_ = 0;
  float before_zero_ = 0;
  bool sounded_ = false;
  void render(float* output, const float* input, std::uint32_t frames);
  void track_gaps(const float* frames, std::uint32_t count);  // producer side, io_lock
};

class Backend {
 public:
  static Backend& get();
  // Publish a partial debug WAV without stopping/joining the audio device.
  void finish_capture();
  void shutdown(); // host-only; idempotent, after guest callbacks have stopped
  // Guest execution has stopped for good: sources play out what they queued
  // and nothing follows, so later short pulls are not underruns.
  void guest_sources_ended();
  ~Backend();
  // Before first stream: explicit null device for tests, never a fallback when
  // a real endpoint is unavailable. No change to system audio settings.
  bool use_null_device(bool start_device = true);
  struct Hardware {
    std::int32_t rate = 0, channels = 0, format = -1;
  };
  Hardware hardware(Direction direction);
  bool output_active();
  std::pair<std::int32_t, std::int32_t> output_properties();
  std::int32_t frames_per_burst(Direction direction, std::int32_t guest_rate);
  std::int32_t attach(Stream& stream);
  void playback_started(const Stream& stream);
  void detach(Stream& stream);
  // Same routine as the real device callback; useful for sample-flow checks.
  void process(Direction direction, float* output, const float* input,
               std::uint32_t frames);
  // Samples counted after summing/clipping, at the host device callback boundary.
  struct MixerStats {
    std::uint64_t output_frames = 0, non_silent_samples = 0;
    float peak = 0;
  };
  MixerStats mixer_stats() const;
  // The host endpoints the XR runtime names for the headset, as native PC VR
  // apps play and record on them (Meta: VRC.PC.Audio.1). source(direction)
  // answers the runtime's current endpoint (Windows: a WASAPI endpoint ID or
  // its GUID), empty for none; `runtime` names it in the log, and with no
  // source says why there is none. Devices open on the named endpoint while it
  // is active, otherwise on the system default (never changed). Open devices
  // follow the runtime's answer: rechecked every 2 s and on endpoint changes.
  using HeadsetSource = std::function<std::string(Direction)>;
  void set_headset_endpoints(std::string runtime, HeadsetSource source);
  void request_route_check();  // recheck now (endpoint notifications, tests)
  struct Route {
    bool open = false, headset = false;  // headset: on the endpoint the runtime names
    std::string named, device;           // runtime's answer; endpoint opened
  };
  Route route(Direction direction);
  // The PC output (VENTURE_PC_AUDIO=on): while the game plays on the headset
  // endpoint and the system default output is another endpoint, the same mix
  // also plays there, on its own clock (OutputMirror). The headset path never
  // waits for it; a PC output that fails is logged once and dropped for the
  // process. Never the microphone. Read from the environment at startup;
  // tests set it.
  void set_pc_output(bool enabled);
  struct PcOutput {
    PcOutputPlan plan = PcOutputPlan::off;
    bool open = false;
  };
  PcOutput pc_output();
  OutputMirror& output_mirror() { return mirror_; }

 private:
  friend class Stream;
  friend struct BackendTestAccess;  // test-only access to the real null
                                    // device's stop notification
  Backend();
  struct Slot {
    std::atomic<Stream*> stream{nullptr};
    std::atomic<unsigned> readers{0};
  };
  std::array<Slot, 64> slots_{};
  std::mutex device_lock_;
  ma_context context_{};
  // A host without a usable microphone records silence, as a muted Quest
  // microphone: Quest titles never meet a failing recorder, and FMOD's
  // AudioRecord thread (Unity Microphone) spins on one without sleeping.
  ma_context silent_context_{};
  bool silent_context_ready_ = false;
  ma_device playback_{}, capture_{};
  bool context_ready_ = false, playback_ready_ = false, capture_ready_ = false,
       null_ = false, start_null_ = true;
  std::array<std::atomic<bool>, 2> suppressed_stops_{};
  std::atomic<bool> shutdown_{false}, lost_output_{false}, lost_input_{false};
  std::atomic<std::uint64_t> output_frames_{0}, non_silent_samples_{0};
  std::atomic<float> output_peak_{0};
  std::thread monitor_;
  struct SourceStats { std::uint64_t id, underruns, missing, written, consumed; int queue, watermark, rate, state; std::string api; std::uint64_t work_ns, gap_ns, callbacks, callback_wall, callback_cpu, callback_cpu_samples, callback_wall_max, callback_adopt_max; int callback_frames, capacity; std::uint64_t callback_cycles, callback_cycles_samples; std::int32_t callback_guest_tid; std::uint64_t non_silent; float gain_left,gain_right; std::int32_t xrun_episodes; std::uint64_t guest_gaps, guest_gap_frames; };
  std::vector<SourceStats> source_history_;
  std::uint64_t next_source_id_ = 1;
  std::uint64_t opened_output_streams_ = 0, closed_output_streams_ = 0;
  void stream_closed(Stream& stream);
  void remember_finished_worker(Stream& stream);
  std::atomic<double> mix_energy_{0};
  std::atomic<std::uint64_t> device_errors_{0}, reroutes_{0}, recoveries_{0};
  std::atomic<bool> endpoint_changed_{false};
  std::string endpoint_hash_ = "unavailable";
  void remember(Stream& stream);
  void telemetry();
  std::unique_ptr<DebugCapture> debug_capture_;
  struct UnderrunEvent {
    std::uint64_t time, source, previous_pull, worker_begin, worker_end,
        worker_due, wake_late, adopt_begin, callback_begin, callback_end,
        callback_wall;
    std::uint32_t requested, delivered, queue_before;
    std::int32_t rate, size, capacity, callback_frames, guest_tid, xruns;
  };
  std::array<UnderrunEvent, 256> underrun_events_{};
  std::atomic<std::uint64_t> underrun_write_{0}, underrun_read_{0},
      underrun_dropped_{0};
  std::uint64_t previous_output_pull_ns_ = 0, output_pull_ns_ = 0;
  bool timing_enabled_ = false;  // set before the device starts
  void record_underrun(const Stream&, std::uint32_t requested,
                       std::uint32_t delivered, std::uint32_t queue_before);
  void drain_underruns();
  // device_lock_ held through lock, which is released while the backend opens,
  // starts or closes the endpoint: WASAPI can block in those calls for seconds
  // on a newly connected endpoint (an H3VR2 player switched to a wireless
  // headset and FMOD's reopen waited on device_lock_ until the game froze).
  // False while another thread has the endpoint open in progress.
  bool ensure_device(Direction direction, std::unique_lock<std::mutex>& lock);
  // device_lock_ held: whether a guest call opens the endpoint itself. Not
  // while the monitor recovers a lost or rerouted endpoint: the guest asks it
  // to reopen (reopen_requested_), and the stream attaches and plays when the
  // endpoint is back.
  bool guest_opens(Direction direction) const;
  // Releases lock around fn, marking direction's endpoint busy meanwhile.
  template <class F> void unlocked(Direction direction, std::unique_lock<std::mutex>& lock, F&& fn);
  std::array<bool, 2> device_busy_{}, recovering_{};  // device_lock_
  // The opened endpoint's rate and period (internal frames), kept for guest
  // queries while it is closed or reopening; device_lock_.
  std::array<std::pair<std::uint32_t, std::uint32_t>, 2> last_period_{};
  std::pair<std::int32_t, std::int32_t> last_output_properties_{};
  std::array<std::atomic<bool>, 2> reopen_requested_{};
  // device_lock_ held: the runtime's endpoint for direction, matched against
  // the backend's active endpoints into id; false for the system default.
  // answered: named already holds the runtime's answer.
  bool headset_endpoint(Direction direction, std::string& named, std::string& reason, ma_device_id& id,
                        std::string& name, bool answered = false);
  std::string headset_answer(Direction direction, std::string& runtime, bool& sourced);
  // endpoints_changed: match the answer again even when it is unchanged.
  void check_routes(bool endpoints_changed);
  std::mutex headset_lock_;
  std::string headset_runtime_;
  HeadsetSource headset_source_;
  bool watching_endpoints_ = false;
  std::atomic<bool> route_check_{false};
  struct OpenRoute {
    Route route;
    ma_device_id id{};
    std::string logged;  // last decision logged
    bool failed = false;  // the runtime's endpoint did not open
    std::string failed_named;
    ma_device_id failed_id{};
    // The system default endpoint the device opened on (WASAPI), which
    // check_routes compares with the current default.
    bool default_known = false;
    ma_device_id default_id{};
  };
  // device_lock_ held: the backend's current default endpoint for direction.
  bool default_endpoint(Direction direction, ma_device_id& id);
  std::array<OpenRoute, 2> routes_{};  // device_lock_
  // The PC output, device_lock_ held.
  ma_device pc_{};
  bool pc_ready_ = false, pc_enabled_ = false, pc_dropped_ = false;
  PcOutputPlan pc_plan_ = PcOutputPlan::off;
  std::string pc_logged_;
  std::atomic<bool> pc_lost_{false}, pc_suppressed_{false};
  OutputMirror mirror_;
  void update_pc_output();
  void open_pc_output();
  void close_pc_output();
  void drop_pc_output(std::string_view why);
  int default_output_is_headset();  // 1, 0, or -1 unknown
  static void pc_callback(ma_device*, void*, const void*, ma_uint32);
  static void data_callback(ma_device*, void*, const void*, ma_uint32);
  static void notification_callback(const ma_device_notification*);
};

// Integration hook: call before loading game ELF libraries. Backend devices
// stay lazy, so registration never opens the microphone.
void register_libraries();
void shutdown_libraries();  // call after guest threads exit; also done at
                            // backend teardown

}  // namespace venture::audio
