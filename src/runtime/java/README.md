# HotSpot / ARM64 JNI bridge

`venture_java` is a static library built by the runtime CMake target. It dynamically
loads the pinned JVM; no host JNI import library or host compiler ABI beyond the
C JNI boundary is required. The runtime executable still needs the core agent's
implementations. All replacements used by standalone bridge tests live under
`tests/runtime/java_*`, never in runtime source.

Startup order:

1. Run `convert_apk(ConversionOptions, ConvertedApk*, error)` at install/first
   launch. Reject failure; its artifact is diagnostic when verification fails.
2. Register framework native installers with `add_framework_natives`. They run
   once after the bridge starts; later registrations execute immediately.
3. Call `start_jvm` with `java_home` and the bridge JAR
   (`VENTURE_JAVA_BRIDGE_JAR` CMake variable), framework JAR and verified app JAR on the
   class path. Defaults are G1 and a 4 MiB Java stack; debug builds add
   `-Xcheck:jni`. `extra_options` can adjust these for integration tests.
4. Create the framework agent's app classloader and call `set_app_class_loader`
   for native-created-thread FindClass fallback. Explicit native-transition and
   JNI_OnLoad loader contexts take precedence, including the bootstrap loader.
5. The app loader must call `NativeLoader.bindClass(Class<?>)` after defining a
   class and before initializing it. Library loading also scans exported JNI
   classes with `Class.forName(name, false, loader)`. This hook covers definitions
   that become available later without rewriting native declarations. The bridge
   does not own `dalvik.system.*` or framework classloader implementations.
6. Stop guest work/framework queues first, then call `stop_jvm` at process
   shutdown. HotSpot is started once and cannot restart after destruction.

`host_env` attaches the calling platform thread if necessary; owned attachments
detach on thread exit. The guest invocation table implements Attach, daemon
Attach, Detach, GetEnv and Destroy with JNI status codes. Guest GetEnv never
implicitly attaches. Guest JNIEnv wrappers are thread-affine; their 233-word
tables contain four reserved nulls and all 229 Android JNI 1.6 thunks. Compile-time
offset assertions cross-check the pinned Android order against the host JDK.

Guest N/V/A calls decode descriptors into host `jvalue[]`. Method IDs from normal
lookup and reflection share the descriptor registry. JNI references remain opaque
64-bit handles; array/string/direct-buffer memory is accessible only for its JNI
lifetime. Exception and reference operations are forwarded to HotSpot. Local
frames surround whole native transitions and JNI_OnLoad, not individual HLE calls.

RegisterNatives installs descriptor-specific records and 27-byte executable host
stubs. Stubs are allocated RW, then changed to RX and instruction-cache flushed.
The assembly entry saves native ABI registers and stack arguments, then a
noexcept C dispatcher adopts the host thread and calls the guest. Records/stubs,
loaded guest libraries and retained loader references survive through process
shutdown so in-flight callbacks cannot enter freed code. No JVM-global mutex is
held across Java execution or guest calls.

Library scanning reads ELF dynamic symbol/hash/string tables via the existing
Library mapping contract, including both SysV and GNU hashes. No new linker
virtual method is needed. Short JNI names take precedence over overload names;
explicit registration takes precedence over automatic export binding. Library
ownership is classloader-specific, JNI_OnLoad is optional and version-checked,
and failed loads remain failed rather than rerunning partial initialization.

## Additive core contract

`GuestCallArgs::stack` is the **only modification to a core contract**. When it
is nonempty, integer and double spans contain at most eight register values each,
and the core must place these already ordered spill slots at the outgoing,
16-byte-aligned guest SP. This is necessary for mixed GP/FP overflow. The double
span contains D-register bits; fixed float arguments occupy their low S-register
bits. Existing calls with an empty `stack` retain their original semantics.
`GuestThread::call` implements these ordered slots and validates the register spans. `stop_jvm` and `set_app_class_loader` are additive Java
API entries; no existing signature was changed.

## Validation and remaining integration work

```
python tools/bootstrap.py --jdk
.deps/cmake/bin/cmake --preset windows-release
.deps/cmake/bin/cmake --build --preset windows-release
python tests/runtime/java_extract_owned.py
build/windows-release/bin/venture_tests.exe
build/windows-release/bin/venture_java_tests.exe
python tests/runtime/java_conversion_test.py
```

The separate JVM process uses `-Xcheck:jni -Xverify:all`. It tests every
virtual/static/nonvirtual and constructor N/V/A family, independent register
exhaustion and mixed spills, all result widths, reflected IDs, reverse assembly
calls (including the measured AGDK signature), recursion, exceptions, local
references, strings, arrays, direct buffers, attachment, and exported binding
without initializing classes. Reverse callbacks and export binding execute real ARM64 thunks through the core,
including mixed spills and nested Dynarmic calls. Register-only marshaling tests
retain FakeCpu for isolated ABI checks.

The owned Bait/Gorilla/Epic libgame tests compare this scanner with an independent
ELF section-table census and confirm optional JNI_OnLoad. Conversion tests pass
a real synthetic DEX through the pinned converter, execute the result, check
annotations/default interface methods/exceptions/synchronization/arrays, exercise
the C++ subprocess entry, reject missing dependencies, and invalidate corrupted
caches. No game binary or credentials are checked in.

Win64 embedding/trampolines are executed in tests. POSIX dlopen, process spawning,
RX allocation and SysV assembly are implemented; the SysV assembly is assembled
for x86_64 Linux. A full Linux build/run remains unverified on this Windows host.
First Contact's Java can be converted, but its ARM32 native code requires a
separate A32 handle/argument bridge and is not supported by this ARM64 bridge.

Other remaining integration boundaries: the framework classloader must install
the bindClass hook; arbitrary externally supplied bytecode must pass through the
same rewriter (raw DEX is not valid input to JNI DefineClass); HotSpot monitor and
Java blocking calls currently use HotSpot's waits, so guest signal interruption
while blocked there needs coordination with the core. All-nine HotSpot gates
remain failed without the Android framework; unresolved hierarchy and verifier
diagnostics must be rerun after it is merged. This is not a game-boot claim.
