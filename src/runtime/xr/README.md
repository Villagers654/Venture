# XR integration

Link `venture_xr` and register `hle::openxr::register_library()` and
`hle::vrapi::register_libraries()` with the runtime linker before loading guest
libraries. The runtime core and launcher implementations are separate wave-2
work. This branch does not add a substitute CPU or linker to production code.

`host_xr()` creates one process-lifetime Khronos instance on the loader's
default (system active) runtime; no registration or runtime settings are
written. There is no fallback runtime: a game launch fails with
`HostXr::error` (for example "No OpenXR runtime is available..." or "...
reports no headset..."), after polling xrGetSystem for up to 30 s while the
runtime reports `XR_ERROR_FORM_FACTOR_UNAVAILABLE`. `available == false`
permits flat mode for probes and tests. `venture_xr_probe` prints the real
runtime identity, HMD result and extension inventory without creating a session.

OpenXR and VrApi share one session with exclusive ownership. The guest drives
frame scheduling. OpenXR permits a pending wait while the previous frame is
being rendered. VrApi caches one prediction per application frame index and
supports explicit Wait/Begin and implicit Submit scheduling. GPU queue locking
covers frame begin/end and swapchain acquisition/release. Buffered VrApi
haptics use a separate bounded worker; it never schedules frames.

When `gfx/xr_images.h` is merged, CMake builds the ANGLE adapter against
`venture_gfx`. Registration installs it without creating a context. The caller
must have the texture's GLES share group current for ring creation, copying
and destruction. Persistent texture slots are independent of acquired XR
indices. The adapter waits VrApi EGL fences and uses GL/Vulkan GPU copies;
`release_images()` drops destination bridges before their VkImages disappear.
The standalone branch uses injected implementations of `xr/xr_images.h` in
tests only. The adapter was syntax-checked against the GLES worktree's header;
it still needs an ANGLE execution test after integration.

Registry output is checked in. Regenerate with:

```
python tools/codegen/xr_generate.py --registry .deps/fetch/openxr_sdk-src/specification/registry/xr.xml
```

`--check` detects stale output. Metadata records structure sizes, alignments,
member offsets, pointer/count relationships and permitted chain parents for
future A32 conversion. Current marshaling is A64. The guest API version is
1.0, with `XR_KHR_locate_spaces` emulated; the 1.1 loader symbol is exported.

`venture_tests` selects the mock manifest in its own process. The mock owns real
Vulkan images and scripted poses/actions; its tests require a Vulkan driver
but no headset. CTest also checks missing-runtime/no-HMD flat mode and codegen.
Test core/GPU fakes remain under `tests/` and need to stay isolated from the
integrator's real core test target.

## Remaining work

For headless integration, `VENTURE_XR_RUNTIME=mock` selects the bundled manifest;
it is never selected otherwise.
`VENTURE_MOCK_DUMP_DIR` writes submitted projection eye images and `frames.jsonl`.
`VENTURE_MOCK_DUMP_EVERY=N` captures frame 1 and every Nth submitted frame (default 90,
zero disables images). Readback uses the last released Vulkan image, its array
layer and submitted rectangle. `VENTURE_MOCK_REALTIME=1` paces frames at the scripted
display period; it is off for unit tests. Capture failures return an XR error.
`VENTURE_MOCK_SPACE_WARP=1` makes the mock a host with `XR_FB_space_warp`; Venture
then offers the extension to A64 guests and forwards their space-warp swapchains and
per-view `XrCompositionLayerSpaceWarpInfoFB` chains.
Space warp is the host's: without it, Unity titles still call the Oculus SDK's
`OVRManager.SetSpaceWarp(true)`, which records the flag whatever the runtime did and
paces the title to half rate. `core/engine_policy.cpp` makes that SDK entry follow the
session's real space warp state, so titles render every frame at the host rate.

- The measured VrApi import union has 71 symbols; 55 are registered. The 16
  remaining imports are deliberately unresolved: Android Surface create (all
  three versions) and get, Vulkan system creation and buffer/foveation-buffer
  get, sampler-state get/set, boundary-visibility request/get, system UI,
  keyboard overlay, remote emulation, property-float and update-foveation.
  These need real producers, compositor features or a verified private ABI.
- The ANGLE adapter currently follows the graphics bridge's single-sample,
  one-mip color 2D/array restrictions. It does not support depth, cubemaps,
  multisample resolve or externally substituted texture names. The test
  provider supports cube images, so analytic VrApi cube translation is tested
  independently of this graphics limitation.
- VrApi analytic layers require the corresponding host composition extension.
  SteamVR's probed inventory lacks cylinder/cube/equirect support; a local
  compositor fallback remains necessary there. Sheared transforms, cube offset
  warping and built-in texture sentinels return unsupported. Hand geometry and
  aim are authored approximations; haptic byte streams approximate amplitude
  through core pulses, not a PCM carrier.
- Guest Vulkan creation rejects allocation callbacks and pNext feature chains
  until the Vulkan agent supplies typed callback/feature translation. A32,
  merged ANGLE execution and full game startup have not been validated.
- Color emulation follows the report's Rec.709 baseline; wider gamuts require
  host support or actual conversion. Refresh emulation uses measured periods;
  before a host without XR_FB_display_refresh_rate shows its first frame, the
  period it reported on the previous launch (data dir `host-displays.json`, per
  runtime and headset) stands in, so games do not start at Android's 90 Hz.
  Unsupported Meta features are hidden except for the upstream
  `XR_FB_passthrough` compatibility declaration: without a usable backend its
  properties report unsupported and creation fails. A64 passthrough is on by
  default and uses the first backend the host offers that starts: host
  `XR_FB_passthrough` (forwarded), host `XR_HTC_passthrough` (planar layers), an
  `ALPHA_BLEND` or `ADDITIVE` environment blend mode, an OpenVR headset camera
  (`tracked_camera.h`, Windows), or, temporarily, the Steam Link native
  passthrough companion over its SBN1 pipe (`passthrough_source.h`,
  `passthrough_source_sbn1.h`, Windows); an opt-in color key replaces them all
  (`hle/openxr/passthrough.cpp`, probed once per host instance). `configure_game`
  captures `VENTURE_PASSTHROUGH` before XR initialization (`0` turns every
  backend off); no file I/O or environment lookup occurs in the frame-time selector. See
  [Mixed Reality](https://github.com/Villagers654/Venture/wiki/Mixed-Reality) on the wiki for supported
  setups. There are no fabricated
  sensor-valid flags or successful stubs for unavailable camera effects.
- POSIX platform implementations are present, but no Linux build was run on
  this Windows machine (WSL is not installed).

Controller bindings follow the player's controls (the launcher's Controls page). The launcher passes them in
`VENTURE_CONTROLS` (JSON, only what differs from the defaults in
`src/common/controls.h`); `xr/bindings.cpp` translates every Touch path a game
suggests onto each host controller profile the runtime defines (the profile's
extension enabled), with trackpad halves through `XR_EXT_dpad_binding` when the
host has it. A host that refuses a path gets the suggestion again without touch
sensors, then with only the controller's listed inputs.

The default (Automatic) menu works on every runtime: the controller's left menu
input is bound as the game's own menu (Touch, Touch Plus, Pro, Pico 4, Focus 3,
Cosmos, G2, WMR and Vive menu buttons, Index trackpad press, PSVR2 Create, Steam
Frame View), so runtimes that leave that button to games (Meta, Virtual Desktop's
VDXR, Pico, Monado) open the menu with it, and both stick clicks pressed together
(the Stick click rows) open it as well, for runtimes that keep the button for
themselves (SteamVR's dashboard). The generic controller has no menu button and
uses the stick clicks alone. A player's own choice replaces both. The game's
menu is bound where the player put it when that is a left-hand button; any
other choice (a right-hand button, a held button, two buttons held together) and
the default's stick clicks are synthesized by `xr/menu_chord.cpp`: a
priority-zero Venture action set reads the chosen inputs and presses the game's
menu once they are held for the chosen time, until one releases, so menu holds
(height calibration) work. Release both to rearm. Focus loss or inactive inputs
cancel the hold. The buttons still reach the game, and a game's higher-priority
binding on them can hide them from the menu. Menu actions are forgotten only
when that action or action set is destroyed: OpenXR handles are unique only
within their type (VDXR numbers each type from 1). For a host profile Venture
does not remap, Automatic adds the old stick-click fallback where the host
leaves a game's menu unbound; `VENTURE_MENU_CHORD=off` disables the stick
clicks beside a menu button and that fallback, and `=stick-clicks` forces the
fallback (developer overrides; the launcher no longer sets them). The runtime
reports the controllers it sees to `VENTURE_CONTROLLER_REPORT` (the launcher's
Connected and Last used tags).
