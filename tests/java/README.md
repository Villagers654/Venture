# Framework tests without the JIT

```powershell
python tools/bootstrap.py --jdk
python tests/java/run_games.py
# Optional exact linkage census against the supplied research output:
python tests/java/run_games.py --api-union <path-to-external_api_union.tsv>
```

The harness uses the owned APKs under `%LOCALAPPDATA%/Venture/games`, selecting
the highest installed version code. `--games-root` overrides the location;
individual game names select a subset. It downloads hash-verified dex2jar 2.4
into `.deps`, with the shared Venture development cache. No APK, DEX, native
library or converted game class is committed.

FrameworkTest exercises blocking monotonic message queues, cancellation and
safe shutdown, Bundle/Parcel values, XML preferences and persistence, JSON,
XMLPull and malformed binary XML. ApkMetadataTest reads manifests, compiled
resources and signer X.509 certificates from all installed packages (all nine
owned games pass, including ARM32 First Contact's metadata). LifecycleTest
checks Application/base-context attachment, activity and surface ordering,
surface ownership, text composition, host-vsync delivery, and in-process
activity/back transitions. The C++ `venture_framework_native_tests` target separately
checks JNI surface references, UTF-8, chunked asset reads, the loader classloader,
missing NativeActivity hooks, and configured path aliases using contract fakes.

## Real startup adapter

PrepareGame processes the actual primary classes.dex converted by dex2jar.
It recomputes JVM stack map frames from class metadata without initialization,
normalizes illegal abstract/final combinations on classes **and InnerClasses**
(seen in Bait's R8 output), redirects System.load/loadLibrary, and replaces
native method bodies with recording calls. All lifecycle, provider and
AppCompat bytecode remains intact, including attachBaseContext.

The recording native boundary allows Unity's NativeLoader.load bootstrap to
return true, then terminates at the first game native initializer. NativeActivity
creation is recorded through the libandroid hook. No ELF executes, and no test
claim extends past this boundary. These transformed `test-only.jar` files must
never be used for production. The production converter needs the same frame
and access-flag normalization, plus its own load/reflection/native binding
rewrites; this harness does not replace that pipeline.

Each `build/framework/<game>/startup.log` records the input version code and
DEX SHA-256, then the chronological sequence of direct application-to-Android
or dalvik method invocations with descriptors and thread names (`FRAMEWORK_CALL`).
It also records selected framework-internal lifecycle operations (`FRAMEWORK`),
library requests (`LOAD`) and native entry (`GUEST_NATIVE`/`REACHED`). Calls reached
indirectly through reflection are represented by the lifecycle trace or the
direct calls made inside the reflected body; this is not a JVM-wide profiler.
The script fails on conversion errors, missing classes/methods, other startup
exceptions, timeout, or failure to reach a native initializer.

## Verified startup boundaries

| Game (version code) | Direct framework calls in recorded run | Boundary reached without missing framework members on this path |
| --- | ---: | --- |
| Elixir (2400) | 16 | `UnityPlayer.initJni(Context)` |
| V-Speedway (745) | 53 | `UnityPlayer.initJni(Context)` |
| Blaston (203302372) | 90 | `UnityPlayer.initJni(Context,int)` |
| Bait! (119014291) | 263 | `UnityPlayer.initJni(Context,int)`, through AGDK/AppCompat Java construction |
| Vader Immortal I (387770) | 3 | `NativeActivity` requests `UE4!ANativeActivity_onCreate` |

Bait's trace includes ContentProvider initialization, activity base-context and
theme configuration, Android/app style resolution, layout inflation and input
view construction before native initialization. The test does not establish
AGDK initializeNativeCode or native rendering after Unity's initJni call.
Vader's trace stops in NativeActivity.onCreate before UE4's native body runs.
Full resume/focus/surface ordering is independently checked by LifecycleTest.

The descriptor census against the measured union currently resolves **1429 of
5989 members**, with **177 of 676 referenced declaring classes** present in the
android.*, dalvik.*, org.json and org.xmlpull namespaces. This counts inheritance
and exact field/method descriptors; it does not measure behavioral completeness
or method accessibility. Missing rows are emitted to
`build/framework/missing-api.tsv`. Classes referenced only through reflection
or native FindClass are outside this census.

The normal CMake build includes the APK-independent Java harness. Run all its
registered tests with:

```powershell
.deps/cmake/bin/ctest --test-dir build/windows-release --output-on-failure
build/windows-release/bin/venture_tests.exe
```

Windows llvm-mingw release builds and these tests have been run. No end-to-end
JIT, JNI guest thunk, graphics, XR, NativeActivity backend or Linux execution is
claimed by this framework-only test suite.
