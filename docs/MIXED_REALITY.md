# Passthrough (mixed reality)

Enable **Settings → Runtime → Passthrough (mixed reality)**. The preference
defaults to off, gates both backends, and applies to the next game launch,
including Steam launches. Existing `steamFramePassthrough` settings migrate to
`passthrough`; an explicit new value takes precedence.

The launcher passes `VENTURE_PASSTHROUGH=1` or `0` on every launch, overriding
inherited values. The runtime captures it once in `configure_game` before XR
initialization and does not read the launcher's settings file. Direct runtime
launches default off; only the exact value `1` opts in.

## Backends

- **Native OpenXR:** A64 `XR_FB_passthrough` calls and layers are forwarded when
  the setting is on and the host actually exposes the extension. The host instance
  requests `XR_FB_passthrough` and `XR_FB_triangle_mesh` only when the launch
  environment contains exactly `VENTURE_PASSTHROUGH=1`. This refers to
  a host-provided API, not color replacement. Meta documents
  [Passthrough over Link](https://developers.meta.com/horizon/documentation/native/android/mobile-passthrough-over-link/)
  as a developer feature with separate runtime toggles and a developer account.
  Its presence does not establish that every Quest Link setup supports it.
  Virtual Desktop's public VDXR extension table currently does not list
  `XR_FB_passthrough`; its color-key feature alone does not provide this API.
  Native passthrough was tested on a Meta Quest 3 over Air Link, with the visual
  issue described under hardware status below. Wired USB Link and native Virtual
  Desktop passthrough were not tested.
- **Steam Frame over Steam Link:** Reconstruction passthrough on Windows x64,
  with NVIDIA, HEVC and SteamVR, requires separate PC/Frame companions from
  [Steam Link Native Passthrough](https://github.com/McFredward/steam-link-native-passthrough).
  This backend supports **Steam Frame only**. The Frame supplies its camera
  image locally; camera video never enters Venture.

The setting's Info button opens a dialog with separate Quest Link / Air Link and
Steam Frame setup cards. Each card explains its requirements and links to the
corresponding setup guide or companion project.

The upstream `XR_FB_passthrough` compatibility declaration remains available
when the setting is off or no backend is usable: both property structures report
unsupported and creation fails, allowing extension-requiring games to run in VR.
Native support is never inferred just from a runtime's product name. The local
Steam Link backend retains its SteamVR runtime-name check.

## Rendering and lifecycle

The local reconstruction route creates independent SDR RGBA8 images per eye,
preserves poses/FOV, array slices and submitted image rectangles, and handles
UNORM/sRGB, straight/premultiplied alpha, fractional coverage, reconstruction
opacity and additive emission. Host maximum dimensions and rect mapping use
the same helpers as normal VR. Requests within the limit remain 1:1; larger
images are scaled with their aspect ratio preserved. A zero host limit remains
unbounded, as in ordinary VR.

A bounded SBN1 pipe session declares source format to a verified local SteamVR
companion. PID, process-image path, nonce, version and record size are checked;
no shared handles are imported. The companion separately proves exact video/mask
association and controls camera recovery. No guessed timestamp joins are added.
A missing/withdrawn grant makes MR output opaque. Missing or incompatible
companions cannot grant camera use.

All discovery and pipe I/O runs on a worker. Withdrawal revokes the local lease
immediately; retirement is published only after the worker's pipe handles are
closed. During MR-to-VR withdrawal, normal presentation temporarily forces color
alpha opaque through an MR-only shader variant. It retains depth chains, arrays,
MSAA, ordinary formats and asynchronous host ownership. After retirement the
additional alpha conversion stops. No worker wait/join is added to the frame path.
The unused worker sleeps until the next request. Paused guest passthrough layers
may remain in the submitted list: they are validated and omitted locally while
other layers use ordinary VR presentation. Resuming restores the MR route.

STOPPING, EXITING and LOSS_PENDING latch suspension for that session run. A final
already-begun frame cannot request cameras again. Only a successful new guest
`xrBeginSession` clears suspension. Ordinary recoverable submit errors withdraw
the source without pretending the session stopped. Session teardown drains the
worker and releases owned images/handles.

Ordinary VR uses its original single/multisample color shader. The alpha variant
is separately compiled and selected only for MR or an outstanding withdrawal.
Unknown and unenabled next-chain structures are ignored consistently. Recognized,
enabled camera effects that the local backend cannot honor are refused.

Supported local composition is a reconstruction underlay with at most one stereo
projection. Room/scene understanding, spatial anchors, environment depth,
projected geometry, camera textures, HDR/MSAA MR images and additional camera
color effects are outside this profile. These restrictions do not apply to
ordinary VR after MR withdrawal.

## Software validation

`tests/passthrough` builds real Vulkan pixel tests and a separate executable that
links production `openxr.cpp`, `graphics.cpp` and `xr/session.cpp`. Its external
OpenXR host, transport grant and unrelated platform services are controlled test
ports; guest frame/session functions and async ownership are not replaced.
Cases include STOPPING → final EndFrame → EndSession → restart, MR exit,
retirement, feature/layer pause and resume, normal float/MSAA images, depth
chains and async host submission both after MR exit and with paused layers.
Property tests exercise the production `xrGetSystemProperties` and creation entry
points against a host that reports passthrough support, with the option off/on.
Host-initialization tests capture the actual `xrCreateInstance` extension list
with the option unset, disabled, invalid or enabled, and with different host
extension availability. The separate `passthrough_configuration` test exercises
production `configure_game`, strict environment parsing, contradictory launcher
settings and the stable per-launch snapshot independently of ELF32 loading.
Shader tests compare ordinary VR SPIR-V with the retained upstream shader using
the same compiler. Pixel tests cover stereo slices, alpha, emission, opacity,
host limits, cropping (including one-pixel edge crops after downscaling), and
invalid/unenabled chains. Registry regeneration is
checked against the pinned SDK.

On Windows, `tests/steam_link_transport` links the actual production named-pipe
client. An owned server fixture is copied into a private runtime directory and
registered through an isolated LOCALAPPDATA. Production process/PID/path checks
remain enabled. Tests cover complete/fragmented/malformed replies, wrong nonce
and PID, pending-I/O cancellation/destruction, late replies, server death,
reconnect and fresh lease generations. No installed SteamVR registry is modified.

The feature workflow runs on PRs touching the listed passthrough implementation
and test files, or by manual dispatch. It has no push trigger and does not match
the root CMake file or entire launcher, XR or build-system directories. It tests
actual launcher settings and real `play` launches
against a child probe, including contradictory inherited environment values, and
runs the unchanged
shipping scanner on the built Windows/Linux launchers with `McFredward` as a
build identity. The public project link is formatted from plain product
identifiers; the feature changes no global scanner exceptions.

## Hardware status and scope

The minimal test branch containing only passthrough plus the independent build
scripts passed the user's Steam Frame hardware test before this rebase to Venture
current upstream main (`ae3ed1f`) and the follow-up fixes. That Frame test did not
show displaced black silhouettes. The latest rebase and follow-up fixes have not
had a fresh Frame hardware run.

The native path was subsequently tested on a Meta Quest 3 over Air Link with
Oculus runtime 1.208.0. Passthrough initialized and displayed successfully; the
logs report no `xrEndFrame` errors through the last reported count of 4,200
frames, with submission settling at approximately 72 FPS. A visual defect remains:
displaced black silhouettes appear beside game elements and Air Link's own menu
elements displayed over passthrough. The displacement persists while stationary,
increases with viewing distance, and was not resolved by disabling Sliced Encoding.

These observations suggest a shared Meta/Air Link composition or mask-projection
issue, but the cause is unconfirmed and an interaction with Venture's submitted
layers has not been ruled out. Successful API calls do not establish correct
visual composition. Wired USB Link and native Virtual Desktop passthrough were
not tested.

Launcher pacing, Unity compatibility fixes, preview migration, FFmpeg discovery
and scanner policy are separate PR branches. They are not required source changes
in this feature branch. The optional test branch adds only the independent
Windows build/install scripts and deployment-name configuration to this feature.
The scripts are maintained separately in `feat/windows-build-install`.
