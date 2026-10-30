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
  Unsupported Meta features are hidden. A64 passthrough is available through a
  real host extension or the experimental Steam Link reconstruction backend
  selected in **Settings → Runtime → Steam Frame passthrough**. `configure_game`
  snapshots that saved default-off preference before XR initialization; no file
  I/O or environment lookup occurs in the frame-time selector. See
  [mixed reality](../../../docs/MIXED_REALITY.md) for its supported
  layouts, test evidence and hardware validation status. There are no fabricated
  sensor-valid flags or successful stubs for unavailable camera effects.
- POSIX platform implementations are present, but no Linux build was run on
  this Windows machine (WSL is not installed).

The optional `VENTURE_MENU_CHORD=stick-clicks` fallback holds both controller stick
(or mapped trackpad) clicks for 500 ms and presses the left-hand menu until a
click releases, so games that need the menu held (height calibration) work.
Release both clicks to rearm. Focus loss or inactive inputs cancel the hold.
It applies to VrApi and OpenXR and preserves ordinary game clicks; overlapping
game bindings produce a warning. Its host action set has priority zero, so a
game's higher-priority binding can make the fallback unavailable.
