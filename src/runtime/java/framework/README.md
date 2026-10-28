# Venture Java framework

This jar supplies the Android classes used by converted application code on the
host JVM. The initial implementation targets the startup paths measured in the
r2-java report, including UnityPlayerActivity, Unity's AGDK/AppCompat activity,
and UE4 GameActivity. It does not provide the entire Android API.

`AppMain` prepares the Android main Looper, attaches the manifest Application,
creates its ContentProviders in manifest initialization order, calls
Application.onCreate, and attaches and launches the manifest activity. Activity
creation, start and resume precede surface creation/change and window focus.
The main thread then blocks in Looper.loop. Production Java MessageQueues share
their owning NDK Looper: Java enqueues wake native waits, and native fd/frame
callbacks execute on the owning Android thread. Java-only tests use JVM waits. Explicit in-process activities use
one activity stack with pause/stop, finish and previous-activity resume.
External applications, Android services and Binder IPC are unavailable.

## Build and integration

```powershell
python tools/bootstrap.py --jdk
.deps/cmake/bin/cmake --preset windows-release
.deps/cmake/bin/cmake --build --preset windows-release
```

The build produces `bin/venture-framework.jar` and the static
`venture_java_framework` target. `tools/bootstrap_framework.py` pins and verifies
Temurin 21 and the Android 32 resource archive, caching downloads under
`%LOCALAPPDATA%/Venture/dev-cache`. Only compiled resource data and its notices
are extracted from the SDK; its Java stub classes are never included.
The C++ and Java code use portable APIs. Linux requires a JDK 21 at `.deps/jdk`
(or `VENTURE_JAVA_HOME`); Linux compilation has not been verified here.

The integrated `--game` path performs this sequence:

1. Call `java::register_framework_natives()` from
   `../natives/framework_natives.h` before starting the JVM. It installs the
   `NativeHost` registrar through the existing `java::add_framework_natives`.
2. Put `venture-framework.jar` on the JVM classpath, before the bridge helper
   jar if that jar also contains `com.venture.runtime.NativeLoader`.
   This implementation preserves the bridge's `load0`, `bindClass`, reflection
   rewrite and descriptor helper interfaces. Ordinary loads route through
   `java::load_guest_library` with the actual caller's classloader.
3. Populate ProcessConfig, start the JVM, and call `java::run_app_main` with the
   converted jar on the runtime main thread. It adopts that thread as a
   GuestThread, attaches to the JVM, mounts the APK through PackageAssets,
   invokes AppMain, and blocks until the Looper exits. Exceptions are reported.
4. Install the libandroid callbacks in `../natives/native_activity_bridge.h`.
   libandroid owns ANativeActivity, global JNI references and guest callbacks;
   the framework owns the Java lifecycle. Absent callbacks throw and log.
5. Use `java::acquire_surface_window` from `../natives/surface_bridge.h` for
   ANativeWindow_fromSurface. Its input is a **host jobject**, not a guest ART
   address. The returned NativeWindow has one acquired reference. Null/released
   surfaces return null; release with `android::release`. Surface release and
   acquisition use the Java object monitor to preserve reference ownership.
6. Apply the path aliases below at the guest VFS filename boundary and supply
   display/vsync updates through `../natives/display_bridge.h`.

These are additive interfaces. No existing core, JVM, window or asset contract
signature has changed. The runtime installs the production bridges before starting HotSpot. See
`docs/COMPATIBILITY.md` for the measured game stages and remaining blockers.

## Filesystem paths

Context File objects and ApplicationInfo.sourceDir, dataDir and nativeLibraryDir
contain absolute **host paths** from ProcessConfig. This lets unmodified JDK
File/ZipFile/FileInputStream code access application data and the APK. Returning
Android-looking strings from File would send JDK I/O to the wrong filesystem.
Framework private-file helpers reject path components that escape their root.

When these strings cross JNI into guest code, the VFS must recognize only these
configured aliases using `java::framework_path_to_guest` from
`../natives/path_bridge.h`, before its ordinary Android mount resolution:

| Host root | Android alias |
| --- | --- |
| config.apk | `/data/app/<package>/base.apk` |
| config.native_lib_dir | `/data/app/<package>/lib/arm64` |
| config.data_dir | `/data/user/0/<package>` |
| config.guest_cache_dir | `/data/user/0/<package>/cache` |
| config.obb_dir | `/storage/emulated/0/Android/obb/<package>` |
| config.data_dir/volumes/data | `/data` |
| config.data_dir/volumes/system | `/system` |
| config.data_dir/external | `/storage/emulated/0` |
| config.data_dir/external-data/files | `/storage/emulated/0/Android/data/<package>/files` |

The mapper performs lexical normalization and root-component matching; it
does not permit arbitrary host paths. The guest VFS remains responsible for
symlink and mount policy. BaseDexClassLoader.findLibrary returns the guest
native-library path, after checking the host file. NativeLoader.load maps known
host paths back to Android paths; loadLibrary maps names to ELF filenames.
The guest VFS recognizes these configured host aliases. JNI additionally maps
path-valued File/Context methods and ApplicationInfo fields to Android strings;
file and ZIP constructors called from native code map Android paths back to
host paths. Java File objects retain host paths throughout JDK I/O.

## Resources and device profile

PackageModel reads binary AndroidManifest.xml, including application/provider
classes, metadata, aliases, versions, target SDK and MAIN launcher filters.
MAIN/INFO is accepted for Elixir and Vader as observed in r2-java. ARSC parsing
supports string pools, sparse/offset16 tables, references, primitive values,
arrays and style bags. Resources/Theme/TypedArray resolve application and
Android package-1 resources. Compiled XML is exposed through XmlPullParser;
LayoutInflater supports view construction, factories, includes and merges.
Configuration selection covers locale/country, density, orientation, night,
SDK and screen dp qualifiers. It is not AOSP's complete qualifier algorithm.

Build reports Oculus / Quest 3 / eureka / oculus, with SDK from ProcessConfig.
The logical desktop mirror defaults to **1920x1080, 320 dpi, 90 Hz** (960x540 dp).
These values describe the Android window, not Quest eye textures. XR/graphics
can call `configure_framework_display` once AppMain's Looper is ready. With
host-vsync enabled, Choreographer waits for `dispatch_framework_vsync` and
delivers the supplied monotonic nanosecond timestamp on the owning Looper.
Flat/debug mode uses a monotonic timer at the configured refresh rate.

Surfaces own reference-counted NativeWindows and report their actual RGBA8888
or RGB565 format. Surface resize/format changes recreate the window and issue
destroy/create/change callbacks. Display metrics updates do not yet relayout
existing windows automatically; the window/input integrator must drive resize.
Views maintain hierarchy, listener, layout and text/input state. A general Java
Canvas/widget renderer is not implemented.

Services expose local audio-focus/volume policy, wake-lock state and clipboard;
the clipboard is not the user's host clipboard. Battery/headset/volume sticky
intents describe a plugged-in desktop with no reported battery or wired headset.
Telephony, sensors, vibration, IME and Android network service capabilities are
reported unavailable. The XR profile advertises VR/headtracking; optional
handtracking and sensors are not advertised without a service implementation.

## Boundaries and remaining work

SharedPreferences persists Android-compatible XML using an atomic replacement
and supports typed values, sets and listeners. `apply` currently performs a
synchronous commit. Bundle and Parcel support in-process objects; Parcel is
**not** the Android byte/Binder wire format. APK v2/v3 and v1 certificate
extraction supplies signer identity; it does not verify APK authenticity or
implement signing-key rotation history.

Java AssetManager currently materializes assets into byte arrays, limiting
large assets to the Java array size and available memory. Native PackageAssets
streaming remains available to the NDK. Classloaders consume converted jars;
they do not interpret DEX or provide a complete DexPathList implementation.
Adapters retain/filter their data and publish results on the creating Looper.
Text watchers observe both direct Editable mutations and TextView replacement.
Dialogs retain content/button callbacks but presentation remains unsupported.
Audio device callbacks poll the native default-route inventory and deliver actual
add/remove changes on the requested Looper; they do not invent physical devices.
Insets retain typed values and visibility as immutable snapshots.

Optional Google task/asset-pack callback signatures are declaration-only contracts,
checked against play-services-tasks 18.1.0 and asset-delivery 2.3.0 and the public
[Tasks API](https://developers.google.com/android/reference/com/google/android/gms/tasks/Task)
and [asset-pack listener API](https://developer.android.com/reference/com/google/android/play/core/assetpacks/AssetPackStateUpdateListener).
The APK's own classes take precedence for these namespaces. No Google task producer,
asset-pack manager, or Google service is supplied. Service declarations can be
queried from the APK manifest; Context still rejects unsupported service startup.

Full menus, widgets/drawables, accessibility, media codecs, WebView, Android
network services and input producers remain gaps. Current real-game lifecycle
and rendering results are in [COMPATIBILITY.md](../../../../docs/COMPATIBILITY.md).
Unimplemented operations either lack a member or fail explicitly; lifecycle
hooks and listener callbacks with no default action are intentionally empty.

See [the Java harness](../../../../tests/java/README.md) for real-game startup
results and the exact API descriptor census. Third-party provenance and
licenses are in [NOTICE](NOTICE).
