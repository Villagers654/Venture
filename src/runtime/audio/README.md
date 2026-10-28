# Guest audio

The runtime CMake target links `venture_audio`. Call
`venture::audio::register_libraries()` after the core HLE registry/Linker is ready,
before loading guest ELF files. The device and microphone stay lazy. Call
`shutdown_libraries()` after guest threads exit to drain callback workers.
The backend also drains these workers on process teardown. The integrated runtime registers these libraries before loading guest modules.

One miniaudio 0.11.23 output device mixes up to 64 total output/capture streams;
capture shares a separate default input device, released when its last stream
stops/closes. Both use 48 kHz stereo float host buffers. Guest PCM (mono/stereo,
8–192 kHz) is converted with preallocated miniaudio converters. Guest callback
workers use `GuestThread::adopt_current()` and guest-owned callback buffers.
The device thread never enters the JIT, allocates, locks a mutex, wakes a futex,
or destroys a stream. It uses bounded slots, SPSC rings and quiescent removal.

Host periods request 5 ms × 2. By default the guest output worker queues two
5 ms bursts, aiming at roughly 10–20 ms total latency, subject to the actual
host endpoint and JIT scheduling. Larger requested callback blocks/buffers
increase latency. A 1 ms worker poll performs futex wakeups away from the host
device thread. Blocking guest I/O and state waits use the core futex primitive.

WASAPI automatically reroutes streams following the system default, using
miniaudio's default endpoint routing. PulseAudio (including PipeWire's Pulse
server) handles stream moves. Unexpected device stop notifications disconnect
the affected guest streams and deliver AAudio error callbacks; reopening
reinitializes the endpoint. Direct ALSA default-device rerouting and real
headset hotplug have not been tested. No system settings are changed. Null
devices are explicit test configurations, never a production fallback.

OpenSL UUID symbols are pointer cells to 16-byte UUIDs. Every interface is a
guest pointer cell to a guest table of HLE thunk addresses. The tables contain
the full OpenSL ES 1.0.1/Android slot layouts, including unsupported methods
that log and return `SL_RESULT_FEATURE_UNSUPPORTED`. Audio players/recorders
support PCM buffer queues; URI/file sources, effects/device capabilities,
async realization, event/marker callbacks, and Java routing proxies fail
explicitly. Stream type/performance hints are accepted before realization;
recording presets that would promise voice DSP are rejected. No echo
cancellation/noise suppression is advertised. AAudio shares the output mixer
and reports SHARED mode; Android device IDs/audio sessions are unsupported.
Timestamps describe software consumption/capture, not a hardware presentation
clock. Only CLOCK_MONOTONIC is provided. Guest ABI is ARM64/LP64; First Contact
needs ARM32 core and audio layouts later.

`venture_audio_tests` uses the real runtime core and a running null miniaudio device
for callback cadence, completion, waits and device-loss/reopen tests.
`venture_audio_render_tests` uses an initialized, manually driven null device to
inspect actual mixed samples and rate/format conversion. The null backend is selected only by the test executables. `tests/runtime/audio_game_surface_test.py` reads the
owned APKs in place and compares audio ELF imports and lookup strings against
the test executable's real registered symbol surface. Coverage of symbols is
not a claim that the games have booted or their audio has run under the JIT.
