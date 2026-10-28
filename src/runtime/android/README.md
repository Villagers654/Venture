# Android native integration

Link `venture_android` to the runtime after merging core and libc. Call
`hle::register_libandroid()`, `register_libmediandk()` and
`register_libcamera2ndk()` before loading guest libraries. This branch builds
the static component; the unified runtime registers it before guest loading.
All implementations use standard C++ and zlib; no OS API escapes platform code.
The host ABI is x64 and the implemented guest ABI is AArch64, not ARM32.

## Package and fd integration

Call `PackageAssets::get().open_apk(config.apk)` once the process is configured.
It indexes the APK and sorted sibling main/patch OBB ZIPs. For another OBB
directory, call `open_obb()` for each expansion. APK entries win ordinary-path
collisions; every expansion also has an explicit `<filename>.obb/<entry>`
namespace. Failed replacement leaves the previous index available. Existing
streams retain their backing archive after replacement.

Stored streams read the original file at its indexed offset. Deflate streams
use a 64 KiB input window, validate length/CRC at EOF and restart inflation for
backward seeks. `AAsset_getBuffer` allocates a contiguous guest buffer on demand;
it is the explicit whole-entry allocation path, including for stored entries.
`AAssetManager_open` addresses `assets/`; `PackageAssets` also exposes `res/`,
libraries and OBB paths for Java/VFS users. `AAssetDir` lists files only.

**libc part B must implement the two declarations in `libc_bridge.h`:**

* `venture::android::libc_open_host_file_readonly(path, offset, length)` returns a
  guest fd for the whole archive, not a sliced stream or a CRT fd. The range
  arguments permit validation; the NDK separately returns the asset range.
* `venture::android::libc_poll_guest_fds(span<GuestPollFd>)` is a nonblocking poll on
  the libc fd table with Linux POLL flags. Return negative Linux errno on failure.

These remain real link dependencies, not runtime success/failure fakes.
Loopers wait through core `futex_wait` and check fd readiness every 2 ms while
fds are registered. A future libc readiness subscription can remove this
bounded polling. Callbacks run on the polling guest thread; callback removal,
re-registration, wakeups and pollAll deadlines are supported.

## Java/framework and graphics integration

`framework_bridge.h` exposes `asset_manager_from_java(_jobject*)` and the
process-global package asset manager. A null jobject returns null. The framework
owns class/type validation; the minimal native hook shares the one package.

Install `SurfaceBridge` using `set_surface_bridge()`. The incoming env is a
guest JNIEnv token: obtain a host JNIEnv through `java::host_env()`. Resolve
Surface objects with JNI, not raw jobject equality. `from_surface` must acquire
the returned window; `to_surface` creates a local JNI reference on the caller.
Keep Surface global references alive while any window uses them.

`create_native_window()` returns one reference. The registry checks membership
before accessing guest-supplied pointers. Host operations can pin storage with
`pin_native_window()`; the contract's raw accessor assumes a held reference.
Serialize direct width/height/format updates with `NativeWindow::lock`.
`set_cpu_presenter()` optionally presents CPU frames to a mirror. Without a
presenter the CPU path discards frames as an offscreen target.

The framework creates `NativeActivity` after resolving `android.app.lib_name`,
passing `java::guest_vm()`, its UI thread's `java::guest_env()` and a live Activity
global reference. Pass the manifest's `android.app.func_name` when present.
All lifecycle calls and destruction must run on that same UI thread. Creation
adopts the thread and invokes the guest entrypoint with the actual LP64 NDK
layout. Drive `event`, `window`, `input_queue`, `focus`, `content_rect`, resize
and redraw from Java callbacks. `save_state(guest_free)` copies the callback's
malloc-owned result and frees it through the guest libc thunk.

Create an input queue, inject normalized `InputEvent` samples with
`enqueue_input()`, and destroy it after the activity's queue-destroyed callback.
Events remain valid through `finishEvent`; completion receives the handled bit.
Historical batches and IME pre-dispatch are not synthesized.

Call `display_vsync(monotonic_time, period_ns)` from the host display/XR loop.
Choreographer posts work to the owner's looper using this phase/period and
dispatches refresh-rate changes there. The unattached flat mode uses a 60 Hz
clock. Timeline predictions expose one candidate, not Android SurfaceFlinger
timelines. `setFrameRate` stores an advisory content-rate hint.

## Capabilities and validation

Configuration defaults select English/US, API 32 (overridden by ProcessConfig),
landscape, density 320, VR headset, no touch/keyboard/navigation. These are the
runtime's resource-selection policy, not measured host-display characteristics.
Sensor and camera lists are empty. No host thermal service is advertised.
Performance sessions record advisory target/actual durations without changing
host scheduling or power settings. Tracing is disabled.

AHardwareBuffer supports CPU-only RGBA8888/RGBX8888/RGB565/BLOB storage. GPU usage,
protected allocations, native handles and sync fence import are unavailable.
OBB mounts report the package's guest OBB directory; the VFS must map that
directory. Unmount removes the mount record, not the package's persistent ZIP
index or open streams. No font APIs were present in the measured import union.
See `../hle/libmediandk/README.md` for per-game media imports and backend plans.

`venture_tests` uses the real core, guest callbacks, signal delivery and VFS.
`tests/runtime/android_test_support.cpp` provides only callback adapters and
an explicitly injected ready-fd fixture for looper isolation;
asset descriptors use the real VFS and callbacks execute through Dynarmic.
The fixture generator uses Python zipfile as an independent oracle, optionally
reads the owned downloads, and creates a sparse ZIP64 archive beyond 4 GiB.
Real binaries are read only; no game execution is implied by these tests.
