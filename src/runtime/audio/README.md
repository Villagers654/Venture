# Guest audio

The runtime CMake target links `venture_audio`. Call
`venture::audio::register_libraries()` after the core HLE registry/Linker is ready,
before loading guest ELF files. The device and microphone stay lazy. Call
`shutdown_libraries()` after guest threads exit to drain callback workers.
The backend also drains these workers on process teardown. The integrated runtime registers these libraries before loading guest modules.

One miniaudio 0.11.23 output device mixes up to 64 total output/capture streams;
capture shares a separate default input device, released when its last stream
stops/closes. Both use 48 kHz stereo float host buffers. Guest PCM (mono/stereo,
8–192 kHz) is converted with preallocated miniaudio converters. What the guest
sees of the hardware is always Quest's: 48 kHz output (AudioManager properties,
AudioDeviceInfo rates, AAudio hardware getters), whatever rate the host endpoint
runs at; the mixer resamples to it. Guest callback
workers use `GuestThread::adopt_current()` and guest-owned callback buffers.
The device thread never enters the JIT, allocates, locks a mutex, wakes a futex,
or destroys a stream. It uses bounded slots, SPSC rings and quiescent removal.

Host periods request 5 ms × 2. By default the guest output worker queues two
5 ms bursts, aiming at roughly 10–20 ms total latency, subject to the actual
host endpoint and JIT scheduling. Larger requested callback blocks/buffers
increase latency. A 1 ms worker poll performs futex wakeups away from the host
device thread. Blocking guest I/O and state waits use the core futex primitive.

Callback workers and guest threads asking THREAD_PRIORITY_AUDIO/URGENT_AUDIO
or SCHED_FIFO 2/3 get host audio scheduling (MMCSS Pro Audio on Windows,
best-effort SCHED_FIFO or raised nice on POSIX). Engines also ask other
real-time classes for their own audio renderers (Wwise's threads, Meta XR
Audio's EPS renderer, UE4): Android refuses an app and so does Venture (the
guest sees EPERM and SCHED_OTHER), but the host still runs those threads with
audio scheduling until they ask for SCHED_OTHER. A headset reserves cores for
the game; a PC under load does not, and a renderer that misses its deadline
pads its output with zeros. Such dropouts never reach the device as underruns,
so each output stream also counts them: digital silence of 4800 frames or
less that the guest wrote between sounding samples, cut abruptly (a fade to
zero or a longer pause does not count). Every log reports them every 10 s
("Game audio dropouts"), beside host "Output underruns"; AV telemetry carries
both per stream.

When the host XR runtime names the headset's audio endpoints, output and
capture open on them, as native PC VR apps do (Meta's VRC.PC.Audio.1): on
Windows the runtime's XR_OCULUS_audio_device_guid answer (a WASAPI endpoint ID
or its GUID) is matched against the active endpoints. The answer is rechecked
every 2 s and on endpoint notifications; a changed or newly active endpoint
reopens the device with guest streams still attached. A runtime without the
extension (SteamVR, which switches the Windows default itself; VDXR; Linux
runtimes, which expose no standard sink name), an answer naming no active
endpoint, or one that fails to open leaves the system default. The choice and
its reason are logged as a `facts` line.

With VENTURE_PC_AUDIO=on (the launcher's "Audio on PC too" setting, off by
default, globally or per game), the headset mix also plays on the system
default output while the game plays on the headset endpoint and the default is
another endpoint (Windows: compared by WASAPI endpoint ID; SDL: by the device
the default resolves to). When the headset endpoint is the default, or the game
plays on the default anyway (SteamVR, Linux runtimes today), it plays once.
The headset device callback copies each finished block into a lock-free SPSC
queue (`output_mirror.h`) and returns; a second device on the system default
pulls it on its own clock through a 4-point resampler whose ratio (within
+-0.5 %) a fill-level controller sets to hold the queue at
2 x (both periods) + 10 ms, absorbing clock drift. A starved queue plays
silence and refills; one that overflowed restarts from fresh samples. The PC
device failing to open, start or keep running is logged once and dropped for
the process; the headset path never waits for it. The microphone is never
mirrored. Decisions are `facts` lines.

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
