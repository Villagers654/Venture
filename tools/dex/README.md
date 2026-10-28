# DEX conversion

Provision the pinned tools with `python tools/bootstrap.py --jdk`. This installs
Temurin **21.0.12.1+1**, dex2jar **2.4**, and ASM **9.8** into `.deps`. Downloads
are SHA-256 checked and shared through `%LOCALAPPDATA%/Venture/dev-cache`
(`$XDG_DATA_HOME/venture/dev-cache` or `~/.local/share/venture/dev-cache` on Linux).
Tools honor the shared data-root override and reuse the existing legacy root until migration.
Windows x64 uses the Temurin ZIP;
Linux x64 uses the pinned tar.gz selected by the same flag. No administrator
installation or machine environment changes are needed.

```
python tools/dex/convert_apk.py game.apk --package com.example.game --cache build/dex-cache --framework venture-framework.jar
python tools/dex/verify_owned.py --framework venture-framework.jar
python tools/dex/make_jre.py build/java-jre
```

`convert_apk.py` compiles the tools from this checkout and runs the Java driver
`com.venture.dex.ApkCache`. The runtime runs that same class from
`bin/java/dex/classes` on the bundled JDK (`java -cp <classes>
com.venture.dex.ApkCache ...`, same arguments), so launches need no Python. Its
DEX census (`DexSurface.java`) applies the checks of `dex_surface.py`, which
remains for the build-time census tools.

On Windows, runtime conversion uses `java/venture-java.exe`, a UTF-8-manifested
JNI Invocation API launcher. The driver receives a UTF-8 JSON request file;
its converter/linker/verifier JVMs receive UTF-8 JSON on stdin through the same
launcher. Installation, APK and cache paths never pass through `java.exe`'s
ANSI decoding. POSIX retains the ordinary Java launcher. Direct developer
invocations of `java.exe` still require paths representable in its ANSI code page.

Multiple `--framework` arguments supply additional actual framework/dependency
JARs. They must describe the runtime used at launch, not placeholder Android API
stubs. With no framework JAR the pipeline still emits useful diagnostics but
Android-dependent APKs cannot pass verification. No game is initialized or
launched by these commands. `verify_owned.py` visits all nine configured owned
packages, selects the latest installed version, and explicitly skips absent APKs.

The output is `<cache>/<package>/<dexsha>/app.jar` and `report.json`. For one DEX,
`dexsha` is its raw SHA-256; for multidex it hashes the sorted entry names and
individual content hashes. The report also keys validity on converter/rewriter
versions, tool sources, JDK build, and framework JAR hashes. A package file lock
serializes conversion across processes; a ready cache hit checks the JAR digest.
Failures exit 2 and are never accepted as ready. `--result-file` returns the
artifact/report paths to the C++ subprocess entry in `java/conversion.h`.

The gate compares exact class names and every native owner/name/descriptor/static
flag against the independently parsed DEX. It records converter method failures,
frame-generation failures per method, and HotSpot class-loading failures (including
the verifier's method/location detail). HotSpot runs with `-Xverify:all` and loads
and reflects on every emitted class without initializing it. Missing hierarchy
information is an error, not a guessed `Object` superclass. `report.json` retains
all diagnostics; `verification.jsonl`, `census.tsv`, and converter logs are nearby.
Class loading can report only the first HotSpot failure for a class; the separate
ASM pass diagnoses individual frame-generation failures in all its methods.

## Transformations

The conversion uses the pinned dex2jar API (`ExDex2Asm`), followed by ASM. It does
not replace native declarations with Java wrappers or synthesize Android classes.
These are the only adaptations:

1. Direct `System.loadLibrary/load` and `Runtime.loadLibrary/load` calls go to
   `com.venture.runtime.NativeLoader`. Runtime receivers are still evaluated
   and null-checked. Method-handle constants and bootstrap arguments use matching
   static adapters that retain the Runtime receiver parameter. StackWalker
   identifies the original caller's classloader.
2. Calls to `Method.invoke` go through `NativeLoader.invoke`, which intercepts
   only reflected System/Runtime loads and delegates other reflection unchanged.
   This closes the reflective native-loading route identified in r2-java. JNI
   method lookups and `FromReflectedMethod` also recognize native-load methods;
   calls from guest code use the current transition loader.
3. The stock dex2jar CLI's `LambadaNameSafeClassAdapter` is omitted. It renames
   `-` in DEX class names to `_`, although `-` is legal in JVM names (JVMS 4.2.2).
   Actual Cactus, V-Speedway, Blaston, Epic and First Contact declarations exposed
   this discrepancy. All class references keep their original names.
4. Class header and InnerClasses kind/final/abstract flags come from the real DEX
   class definitions. dex2jar's merging of stale `dalvik.annotation.InnerClass`
   flags otherwise produces illegal final+abstract combinations in Bait/Gorilla.
   Nesting visibility and staticness remain in InnerClasses; JVM-inapplicable
   header flags are removed as required by JVMS 4.1/4.7.6.
5. ASM computes StackMapTable frames from real app/framework/JDK class metadata.
   dex2jar 2.4 emits Java 8 class versions for DEX >= 037 without those frames.
   Class versions are not downgraded, and unresolved Android ancestry fails the
   gate. If framing a class fails, its unframed diagnostic output is retained
   with `ready: false`; it is not a runnable fallback.

Research basis: r2-java `report.md`, `outputs/external_type_union.json`,
`native_methods.json`, and `gameactivity_elf_evidence.json` supplied with this
worktree task. The all-nine runs confirm matching class/native declarations;
complete HotSpot verification still requires the Android framework and resolution
of any remaining verifier failures. A converted JAR alone is not compatibility.

## Runtime image and redistribution

`framework_census.py --output build/framework-census.json` reads every installed
APK's DEX and native string pools without executing game code. It resolves exact
class/member descriptors against bundled APK classes, the built framework/bridge
jars and the modules in `make_jre.py`. Missing classes and missing members are
counted separately, including members whose declaring class is missing.

Rank 0 follows manifest Application/launcher/provider lifecycle calls, class initializers,
virtual dispatch to allocated classes and their inherited callbacks, including
implementations of provided framework interfaces. Rank 1 includes other
Unity/UE4/Meta SDK methods and native/reflection class literals; rank 2 is the
remaining DEX surface. Ranks are conservative: SDK/feature guards and reflection
targets are not evaluated. Native name and descriptor pools are reported as
candidates, without inventing owner/member associations.
By default native names are filtered to known Java/DEX member names; use
`--native-pools` to retain every raw identifier candidate for JNI investigation
(unstripped Unity/UE binaries can produce hundreds of megabytes of identifiers).
API presence does not prove an operation is implemented or that a title reaches gameplay. RE4's stale
SHA-1 identifiers are accepted, as in ART; invalid Adler-32 checksums are
recorded by the census and rejected by the conversion reader.

`framework_census` is an optional CTest test, labelled `census`, that compares installed titles with
`framework_baseline.json`. Its per-gap value is `rank:remaining-work-category`.
A new member or higher reachability in a baselined title fails; removed gaps pass.
In `--check`, titles or versions absent from the baseline are reported as **not baselined**
and skipped before parsing. Installing or updating a game therefore cannot fail the
regression check. The test skips with exit 77 if games or baselined titles are absent.
It does not run in default CTest: `VENTURE_INSTALLED_GAME_TESTS` defaults to `OFF`.
The small APK-independent `framework_census_reader` remains in the default suite.
To enable and run the installed-games scan:

```
.deps/cmake/bin/cmake --preset windows-release -DVENTURE_INSTALLED_GAME_TESTS=ON
python tools/gpu_policy.py -- .deps/cmake/bin/ctest.exe --test-dir build/windows-release -L census --output-on-failure
```

After inspecting changes, use
`--write-baseline` to record the current remaining work. `--games`, `--build`,
`--jdk` and `--package` support other installations and focused analysis. The
baseline excludes Blaston, POPULATION: ONE and Gorilla Tag. Duplicate APKs with
identical surfaces are deduplicated; split APKs with differing surfaces fail
explicitly rather than being silently combined.

Coverage includes Android/Meta, `java`/`javax`/`sun`, JSON, XML, XMLPull/KXml and
Horizon framework packages. Legacy `org.apache.http` is included for target SDK
27 or lower, or an explicit `org.apache.http.legacy` uses-library declaration.
Google Play libraries are APK dependencies, not system APIs on a Quest.

Known limits: ranks over-approximate virtual dispatch (an allocated subclass also
makes base implementations reachable), and do not evaluate SDK/feature guards.
Reflection targets built at runtime and native member ownership are unresolved.
An absent APK ancestor suppresses inherited-member attribution, since that class
cannot link on ART either; dangling members with otherwise complete ancestry can
still be attributed to an external ancestor. Attribution prefers the superclass
chain over interfaces, and blames `java/lang/Object` or `java/io/Closeable` only
when no other platform ancestor exists. A bundled copy of a class Venture provides
under a platform prefix is never linked (the app class loader delegates parent-first),
so its members are checked against Venture's copy alone. The scan does not model
dynamic feature/split APK classpaths or native control flow. Manifest data resource
parsing accepts configuration-independent references; qualified resources whose
type-spec configuration masks are not modeled cause the whole filter to be skipped
with a warning, rather than matching a partially parsed filter or aborting startup.

`make_jre.py` includes `java.base,java.logging,java.xml,java.desktop,jdk.unsupported`.
The measured union uses logging and `sun.misc.Unsafe/Signal/SignalHandler`; the
framework needs XML resource parsers, and its `android.graphics` Bitmap, BitmapFactory
and Paint use `java.awt.image`, `javax.imageio` and `java.awt.Font` from `java.desktop`
(the JVM starts with `-Djava.awt.headless=true`, so no display is touched). Without it
the release layout fails with `NoClassDefFoundError` while dev runs on the full JDK pass.
`java.sql` and `java.net.http` are omitted.
Dynamic/reflection-only dependencies may require expanding this set later.

Temurin/OpenJDK is GPLv2 with the Classpath Exception on designated files, plus
third-party notices. Keep the supplied `legal/`, `NOTICE`, and applicable license
and source-offer materials when redistributing. jlink retains module `legal/`;
the script also copies distribution-level notices when present. Temurin's binary
provenance is its vendor's build, not a claim that HotSpot was built with
llvm-mingw. Venture itself uses the llvm-mingw C JNI boundary.

dex2jar is Apache-2.0; preserve its bundled `lib/open-source-license.txt` and
dependency notices. ASM is BSD-3-Clause; preserve its copyright/license text.
The vendored `android_jni.h` is AOSP Apache-2.0, with its original file notice;
`generate_jni.py` verifies its pinned Android 12L hash before generating slots.

Upstream sources: [Temurin build](https://github.com/adoptium/temurin21-binaries/releases/tag/jdk-21.0.12.1%2B1),
[dex2jar 2.4](https://github.com/pxb1988/dex2jar/tree/v2.4),
[ASM](https://asm.ow2.io/license.html),
[JVMS](https://docs.oracle.com/javase/specs/jvms/se21/html/jvms-4.html),
[Android JNI header](https://android.googlesource.com/platform/libnativehelper/+/refs/tags/android-12.1.0_r1/include_jni/jni.h).
