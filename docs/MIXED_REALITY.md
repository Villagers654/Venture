# Experimental mixed reality over Steam Link

This branch connects Quest OpenXR passthrough to
[Steam Link Native Passthrough](https://github.com/McFredward/steam-link-native-passthrough).
It targets **Quest games emulated on a Windows PC and displayed on Steam Frame**.
The Frame supplies its own camera image; camera video never enters Venture.

**The maintainer confirmed working Spirit Sling Tabletop MR display on Steam
Frame over Steam Link on October 29, 2026.** This is the first hardware result
for this integration. An October30 follow-up using companion release0.3.15
reported improved clarity/smoothness and lower mask decode/queue times.
Occasional rain/compression spikes remain; see the versioned evidence below. The route remains
experimental and does not establish full support for every Quest MR feature or
game, or validate all transitions and failure recovery on hardware.

## Upstream baseline

The branch is rebased onto upstream Venture **0.1.1.1**, commit `cfdad78`
(October 30, 2026), including the Space Pirate Trainer DX startup fixes for
fragment threading and VoIP exports. Shared fragment changes are merged with
the Steam Frame integration: deferred commits remain callable from Unity's
render thread, lifecycle execution remains on the UI Looper, and headless
fragments accept named containers. Invalid `View.NO_ID` values remain rejected.

The headset observations below precede this upstream update. The latest Windows
build and installation were confirmed before this rebase; they are not a new
headset validation of the rebased build. Automated checks cover both the
upstream framework behavior and Spirit Sling's permission callbacks. Repeat
Spirit Sling on hardware after installing the rebased build.

## Trying the integration

On your Windows PC, select the fork branch once:

```powershell
git fetch origin
git switch feat/steam-link-passthrough
```

For each test update, pull and run the installer from the checkout:

```powershell
git pull --ff-only
.\install.cmd
```

It prepares the project's pinned build dependencies, compiles the current
checkout, runs the OpenXR metadata/Unity permission checks, packages Venture
and installs a verified portable build directly in
`%LOCALAPPDATA%\Venture-Passthrough`. Updates overwrite that same installation;
previous builds are not retained. The next installation also removes the older
installer-created builds from its `versions` subdirectory before copying files,
freeing their disk space for the update.
The `Venture Passthrough` desktop shortcut opens the normal launcher; it uses
the normal Venture game library and saved settings. Enable the feature under
**Settings → Runtime → Steam Frame passthrough**. Close Venture and its game
before running the installer.

Use an ordinary Windows terminal. Git for Windows is required; Python 3.13+
x64 is reused or installed for the current user through WinGet when available.
FFmpeg verification discovers Git's bundled `gpg`/`gpgv` even when only Git's
`cmd` directory is on PATH, and also recognizes native GnuPG installations.
The first build downloads and compiles portable toolchain, JDK, FFmpeg and
ANGLE dependencies and can take substantially longer than later cached builds.
The installer keeps its build log under `build/passthrough-install`.
Build subprocess stdout and stderr are explicitly forwarded into that
transcript, including Python tracebacks on Windows PowerShell 5.1.
Before packaging, the installer removes its generated `dist/Venture-win64`
layout so an incomplete package without a release manifest cannot block a retry.
It also removes that layout on completion or failure to avoid retaining another
copy of the installation. Compiler output and dependency caches are retained;
later builds compile only affected sources. Redirected paths and directory
junctions are refused before cleanup.

PowerShell options, when needed:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1 -Jobs 4 -NoLaunch
```

`-InstallDir` selects a separate preview root. Files are checked and replaced
individually, using at most one temporary file instead of staging a second full
build. Unchanged files are reused and obsolete packaged files are removed.
If an update is interrupted, rerun the installer to repair it. The normal
Venture installation, shared game library and build caches are retained.

1. Build this branch using the normal [Windows build instructions](../README.md#build-windows).
2. Install matching PC and Frame companions using the linked project's normal
   installer. Its supported host route requires Windows x64, NVIDIA, HEVC and
   Steam Frame over Steam Link. This patch does not install or modify companions.
3. Select SteamVR as the active OpenXR runtime and start the Steam Link session.
4. Open Venture normally and enable **Settings → Runtime → Steam Frame passthrough**.
   The adjacent **Info** button identifies Steam Frame as the only target,
   explains the companion requirement and opens its GitHub project.
   The preference is saved in Venture's normal `settings.json` and is read once
   before the next game's XR initialization. It applies to UI and Steam launches;
   no special startup command or environment setting is required.

5. Launch the Quest build of Spirit Sling Tabletop and enable its MR mode.
   The first submitted pair remains opaque until the companion grants the
   source format. The runtime log reports waiting, acceptance and withdrawal.
   A format grant alone does not prove a correct headset picture.

Game logs use the shared `%LOCALAPPDATA%\Venture\logs` directory, even when the
preview executable is installed under `Venture-Passthrough`. For Spirit
Sling, collect `runtime-com.meta.spiritsling.log`, its `.console.log` counterpart
and `launcher.log` after the test. A correctly enabled Steam Link backend logs
`Create passthrough: backend=Steam Link` when the game creates its feature.

Turn off **Steam Frame passthrough** and restart the game to disable this
Steam Link backend. Running games retain their startup preference. The old
startup variable no longer selects the backend, including through old shortcuts.
Hosts offering native `XR_FB_passthrough` use their own
implementation without this setting. A32 guests do not expose these APIs yet.

## Implemented boundary

| Area | Behavior |
| --- | --- |
| Guest API | A64 `XR_FB_passthrough`, properties, handles, create/destroy, start/pause, layer resume/pause and reconstruction opacity |
| Native host | Passthrough calls and layers are forwarded; native triangle meshes and geometry instances are available when the host offers them |
| Steam Link source | SBN1 v2 with bandwidth accounting; fresh v1 connection/nonce fallback for an older endpoint |
| Identity | Canonical local runtime from `openvrpaths.vrpath`, bounded process discovery, nonredirected executable paths, held process lifetime, kernel pipe server PID and actual Venture process PID |
| Wire validation | Version, size, flags, profile, PIDs, echoed nonce, nonzero binding/generation and zero reserved bytes |
| Connection | One worker and one live pipe; bounded initial/partial-record waits; healthy idle leases have no periodic expiry; EOF, malformed replies, process death and opt-out revoke permission |
| Color/coverage | One reconstruction underlay followed by one stereo projection, or a camera-only reconstruction layer |
| Stereo | Actual released image and array slice copied to independent 2D host textures; original poses, FOV, rectangles, resolution and source pixels are preserved |
| Alpha | Fractional coverage and premultiplied additive emission preserved; straight alpha premultiplied once; opaque layers and unavailable grants produce A=1 |
| Opacity | Camera opacity P with a projection uses A'=1-P*(1-A), retaining scene RGB contribution E |
| Lifecycle | Pause, layer removal, session stopping/loss/end and destruction withdraw source intent; current single-projection VR output remains opaque on MR exit |

The Steam Link path currently accepts single-sample 8-bit RGBA/BGRA UNORM or
sRGB color swapchains, including arrays. It uses Venture's existing host format
fallback and value conversion where SteamVR requires another supported format.
The guest image-layout vertical flip is applied to pixels and submitted rects.
The companion owns native reprojection, foveation, exact encoded-video/mask
association, camera permission and recovery. Venture supplies no guessed frame
numbers, separate mask stream or color-key extraction.

Unsupported Steam Link arrangements return errors: additional composition
layers, projected passthrough geometry, camera color maps/edge effects,
camera-only partial opacity, multisampled or HDR exchange images, and enabled
composition color transforms. Depth and motion chains are not forwarded through
the ordinary-RGBA route. This patch does not implement room/scene understanding,
spatial anchors, environment-depth occlusion, camera texture access or private
VrApi passthrough interfaces. Games requiring these need additional work.

## Validation

### October30 companion follow-up

The maintainer reports a perceived improvement with Steam Link Native Passthrough
**0.3.15 / PC0.3.11 / Frame0.3.5**. Read-only Frame hashes confirm that exact
receiver/decoder pair. All15,876 masks decode/upload and actual within-picture
slice jobs are present. Mean mask worker time falls12.646→11.259ms and queue
mean8.032→4.924ms; focused-game RGB QP>=40 share falls8.73%→1.24% compared with
the preceding0.3.14 run. These different source scenes/durations support the
perception but do not isolate a causal quality gain. Coded extent stays1152x4608;
peak decoder time and occasional compression/transport events remain.

The companion now defaults to negotiated lossy temporal HEVC masks; the
maintainer authorized that separate quality tradeoff after the original lossless
run below. Venture continues to provide genuine same-eye alpha. It does not
encode the mask itself, lower eye resolution, remove effects or bypass exact
image/camera/Ready checks. Explicit lossless preferences remain available in the
companion. See its [current release evidence](https://github.com/McFredward/steam-link-native-passthrough/blob/main/docs/development/RELEASE-0.3.15.md).

### Initial October29 lossless observation

The successful user-run Spirit Sling log reports an accepted SBN1 v2 source,
mostly 90 Hz submissions and zero `xrEndFrame` errors. The accompanying sender
log also demonstrates a separate bandwidth problem: 11 of 40 logged budget
samples reduced RGB video to 1 Mbit/s while reserving lossless coverage traffic.
One later 80-second interval carried approximately 22.66 Mbit/s of original
video and 123.69 Mbit/s of compressed coverage, calculated from byte-counter
deltas. These are this test's measurements, not network-capacity guarantees.
Almost all its masks used RLE/Zstd; encoding overhead was already small compared
with the compressed masks themselves. Reducing alpha precision, removing effects
or disabling metadata accounting was not accepted as a fix at that checkpoint.
The later explicit authorization for lossy HEVC masks is described above.

The focused fixture builds production lifecycle/chain/stereo/copy/shader code
against a scripted OpenXR host and source connection, with real Vulkan GPU
images and pixel readback. Its transport is scripted: it does not execute
Windows named-pipe discovery, the SteamVR Vulkan-to-native copy, encoding,
streaming or Frame composition.

On a Linux development host with CMake, a C++23 compiler, Vulkan development
headers/loader, a Vulkan driver and `glslangValidator`:

```sh
cmake -S tests/passthrough -B build/passthrough-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/passthrough-tests --parallel 2
ctest --test-dir build/passthrough-tests --output-on-failure
```

The fixture checks SBN1 positive/negative/malformed records, native forwarding,
capability filtering, null passthrough spaces, lifecycle, actual stereo slice
separation, UNORM/sRGB pixel preservation, straight/fractional/additive alpha,
opacity, unavailable/revoked grants, MR-to-VR transitions and unsupported layouts.
Registry regeneration is checked too. Saved preferences are tested for default-off,
strict boolean parsing and repeated on/off round trips. Native Windows tests call
the actual runtime selector with a scripted SteamVR host: a saved off remains
off with the old startup variable present, and saved on works with it absent
or set to 0. A32 and other runtimes still refuse the Steam Link backend.
The real settings command is also exercised across fresh launcher processes.
Shipping scanner checks use the actual stripped Windows/Linux launcher with
`McFredward` as the build account. The public companion project's GitHub
namespace is intentional product content, even when its owner matches the
Windows account. Only that account in its known HTTPS GitHub namespace is
exempt from identity matching. This also covers GCC's overlapping constant
chunks when constructing a URL string. Source/home paths, private logins,
hostnames and other URL origins remain checked, including in UTF-16 strings
and compressed JAR entries.
The dedicated workflow runs these tests
on Mesa's software Vulkan driver on forks as well as upstream PRs.
Its Windows job also compiles the affected production XR sources with the
project's pinned LLVM-MinGW/libc++ toolchain and `VK_NO_PROTOTYPES`, without
requiring a headset or rebuilding ANGLE. String-literal pointer comparisons
are compile errors in that check.

The supplied Spirit Sling 1.2.0/version-code 24 baseline report shows both a
missing `XR_FB_passthrough` extension with Insight Passthrough initialization
failure and Unity permission-helper calls rejected by the framework's fragment
thread checks. The Java fix allows background lookups and queued commits as in
AOSP 12L, while lifecycle execution and `commitNow` still require the UI Looper.
`framework_fragment_permissions` tests that actual callback path. These are
identified and corrected baseline defects. A later fix also supplies Unity's
three-argument headless fragment add and ignores disabled projection extension
nodes, matching the ordinary composition path.

For the Spirit Sling hardware result, record the exact game version, Venture
revision, PC/Frame companion versions, SteamVR versions, GPU/driver and bitrate.
Retain matching PC/Frame logs and observe:

- First launch, intro/loading, table placement, gameplay and in-game MR toggle.
- Both eyes independently, solid objects, soft edges, particles, UI and fades.
- Steam overlay and interactive desktop, then return to the game.
- Companion absence, connection loss/reconnect, standby/resume and clean exit.
- Whether the game requests any currently unsupported layer or spatial API.

Compare with ordinary VR at the same render settings. Only an observed paired
headset result establishes that SteamVR and the companions preserve Venture's
final coverage for this game's actual render layout.

For a black picture with working audio/controllers, collect a runtime debug
report first. Check the `Guest requests XR_FB_passthrough` and
`Create passthrough` messages, then `Steam Link frame refused`/`Eye unavailable`
and source waiting/acceptance. `VENTURE_TRACE_XR_FRAMES=1` adds bounded early
frame layer-type/flag logs. Audio or controller models alone do not establish
that the game submitted a valid color projection or requested passthrough.

The current application-source contract is documented in the companion's
[SBN1 API](https://github.com/McFredward/steam-link-native-passthrough/blob/main/docs/protocols/application-source-v1.md)
and [integration guide](https://github.com/McFredward/steam-link-native-passthrough/blob/main/docs/development/GAME-INTEGRATION.md).
