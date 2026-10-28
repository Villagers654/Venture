# Venture Android guest integration fixtures

These small NDK-built libraries exercise the ABI and platform behavior used by
Unity/IL2CPP, Unreal and Quest native plugins. They are independent of the host
CMake project and of the runtime implementation. They contain real ARM guest
code, including bundled C++/unwind/compiler support code, rather than host mocks.

From the repository root in PowerShell, with Python 3.11 or newer:

```powershell
python tools/bootstrap.py --ndk
python tests/guest/build.py
```

Bootstrap's existing dependencies and default behavior remain unchanged. The
optional NDK is pinned to **r30 LTS (30.0.16248370)**, the latest stable release on
the [official download page](https://developer.android.com/ndk/downloads) when
verified on 2026-10-08. The Windows archive is
`https://dl.google.com/android/repository/android-ndk-r30-windows.zip`,
727,980,788 bytes, with published SHA-1
`9bf167a1985fa7d4a036186b78f702eab9179408`.
The downloaded archive passed that verification. Its additionally measured
SHA-256 is `b830098aaf18b67a42eb831c404e15e5f2990a474f054ac145b0bc957ac6d729`.

The zip is streamed once into `%LOCALAPPDATA%\Venture\dev-cache`, then extracted
into this worktree's `.deps/android-ndk`. A file lock serializes cache downloads
between worktrees. Cached archives are verified before extraction; existing
installations are checked for the pinned revision. No SDK manager, admin rights,
system PATH changes or emulator installation is required.

`build.py` uses `.deps/cmake/bin/cmake.exe`, `.deps/ninja/ninja.exe` and the NDK's
`build/cmake/android.toolchain.cmake`. It builds these output directories:

| Variant below `build/guest/` | ABI | Minimum API | C++ runtime | TLS / relocations |
| --- | --- | --- | --- | --- |
| `arm64-v8a-android32` | AArch64 | 32 | static | ELF TLS / TLSDESC, APS2 + standard DT_RELR |
| `arm64-v8a-android23` | AArch64 | 23 | static | emutls, APS2 packed RELA |
| `armeabi-v7a-android23` | ARMv7 | 23 | static | emutls, APS2 packed REL |
| `arm64-v8a-android32-shared` | AArch64 | 32 | shared | C++ cross-library fixture only |

All guest sources use `-O2 -fexceptions -frtti` and warnings as errors. The NDK's
stack protection and FORTIFY defaults are retained. `-fno-builtin` prevents most
libc/libm calls from disappearing through constant folding. AArch64 uses
`-moutline-atomics`; ARMv7 uses `-mfpu=neon-fp16` for the Quest hardware's NEON and
half storage/conversion instructions. Dot-product code is isolated in an
instruction-enabled function and invoked only when HWCAP_ASIMDDP is advertised.

Build one variant, or limit parallel jobs:

```powershell
python tests/guest/build.py --variant arm64-v8a-android23 --jobs 4
python tests/guest/build.py --variant arm64-v8a-android32 --variant armeabi-v7a-android23
```

Every output directory has `manifest.json` and `imports.txt`. The manifest lists
entry points, expected stdout, arguments, supporting libraries, dependencies,
undefined/defined dynamic symbols, ABI/API, TLS model and relocation features.
The shared C++ variant includes the NDK's unmodified `libc++_shared.so` beside the
fixtures. The build driver audits ELF machine/type, exported entries, both TLS
modules, relocation packing and the PIE runner; a failed build removes any old
manifest so it cannot advertise stale binaries. Build outputs are git-ignored.

## Test contract and runtime harness

Each runnable fixture is `libvtest_<name>.so` with the exported entry:

```cpp
extern "C" int vtest_main(int argc, char** argv);
```

Pass a null-terminated argv array, with the library name in `argv[0]`.
`files` additionally receives a writable **guest-visible** directory in `argv[1]`.
It creates `vtest-files` beneath that directory and requires that child not to exist.
It removes its files and directory on success. Other entries ignore arguments.

Tests print deterministic checkpoints to stdout, then exactly one final line:

```text
VTEST RESULT <name> PASS
VTEST RESULT <name> FAIL <reason>
```

Success returns 0; failure returns nonzero. `expected/<name>.txt` is the complete
successful stdout, with LF newlines enforced by `.gitattributes`. Numerical
tolerances, clocks, alignment, IDs, paths, capabilities and thread scheduling
are checked internally and never printed. Library iteration emits just the two
relevant test basenames in a fixed order. Hash-container iteration is not printed.
Logcat output belongs in the runtime log and must not be mixed with guest stdout.
Use the C locale, and preserve guest stdout bytes as LF even on Windows.

The intended runtime command (to be implemented by the runtime team) is:

```text
venture-runtime --run-guest-test build/guest/<variant>/libvtest_<name>.so --test-dir <tmp>
```

For example:

```powershell
venture-runtime --run-guest-test build/guest/arm64-v8a-android32/libvtest_files.so --test-dir <tmp>
```

For each manifest entry the harness should:

1. Start a fresh process, set the guest search path to that variant's directory,
   and map a fresh writable guest directory to `<tmp>`.
2. Load the entry library and its DT_NEEDED dependencies, apply relocations,
   initialize TLS, and run constructors before calling `vtest_main`.
3. Make listed support libraries available for the test's own `dlopen`. **Do not
   preload** the TLS or lifecycle peers: late load and actual unload are tested.
4. Capture stdout separately from diagnostics; enforce the manifest timeout and
   compare stdout bytes exactly to the repository-relative expected-output file.
   Both the returned status and the final PASS line are required.
5. Drain every guest thread, including detached ones, before unloading test code.
   Then perform TLS/process finalization and library cleanup, and discard `<tmp>`.

Thread-local destructors in workers must run before `pthread_join` returns.
Fresh processes isolate random state, loader scope, constructors, pthread_once
and environment state; repeated calls in one process are not the contract.
Timeouts turn a broken synchronization primitive into a test failure. The host
harness should make timeout failures distinguishable from assertion failures.

Each full variant also builds **`vtest_runner`**, a tiny ET_DYN PIE executable:

```text
vtest_runner ./libvtest_files.so /guest/writable-directory
```

It uses `dlopen(RTLD_NOW)`, looks up `vtest_main`, forwards arguments, calls it and
closes the library. This is a device/guest convenience, not a Windows executable.
Its status 2 means launcher/loader failure. The shared-only variant uses the
full API 32 variant's runner or the runtime entry mechanism.

## Coverage

The 20 main fixtures are built for each of the three full variants; the additional
`cpp_shared` entry repeats `cpp` with a single shared C++ runtime across two DSOs.
Helper DSOs are support artifacts and intentionally have no `vtest_main`.

| Test | What it checks and why games need it |
| --- | --- |
| `strings` | Disjoint memcpy, forward/backward overlapping memmove, memset/memcmp/memchr, string lengths/copies/concatenation/searches, case comparison, strtok_r, strdup, wide memory/strings and ASCII multibyte conversion; asset parsers and generated IL2CPP code. Overlapping memcpy is undefined and is deliberately avoided. |
| `printf` | Signed/unsigned, 64-bit and size_t formatting, hex/octal, padding/precision, positional arguments, null-pointer formatting, f/e/E/g/G/a/A, signed zero and direct mixed-varargs printf; snprintf sizing/truncation and copied va_list forwarding through a noinline helper, including arguments spilling both integer and FP registers. |
| `parse` | Mixed sscanf fields, scansets, widths and %n; strtol/strtoull bases, end pointers, invalid input, limits and ERANGE; strtod/strtof hex, exponents, NaN, overflow and underflow; engine configuration and numeric parsing. |
| `malloc` | malloc/calloc/realloc/free, usable size, posix_memalign/memalign/aligned_alloc, invalid alignment, a 256 MiB arena touched every 4 KiB and 4096 small allocations; engine arenas and aligned native objects. |
| `libm` | Double and float sin/cos/tan/exp/log/pow/sqrt, fma, fmod/remainder, frexp/ldexp/modf, round/lrint, rounding modes, signed zero and domain NaN; fixed reference constants within four ULPs, with exact fused/decomposition checks. |
| `cpp`, `cpp_shared` | Custom exceptions across both DSO directions, RAII destruction while unwinding, exception_ptr/rethrow, cross-library RTTI/dynamic_cast/typeid, string/vector/map/unordered_map, thread/mutex/condition_variable/future, nontrivial thread_local cleanup and chrono. |
| `lifecycle` | Two static constructors in declaration order, an explicit __cxa_atexit registration, and reverse callback/destructor ordering observed after dlclose; plugin startup and unloading. |
| `pthreads` | create/join/detach, return values, normal/recursive/errorcheck mutexes, trylock/timedlock, predicate-based cond wait/timedwait/broadcast, rwlocks, once, keys/destructors, reusable barriers, sem_t wait/trywait/timedwait/post/value/destroy, self/equal, stack attributes, names, sched_yield, gettid/getpid; job queues and GC rendezvous. |
| `tls` | __thread and nontrivial thread_local in the main DSO and a late-dlopen peer; initial values, existing/new thread initialization, main/worker isolation and reverse destructor order across DSOs; native ELF TLS and emutls builds. |
| `atomics` | 8/16/32/64-bit std::atomic, bool/pointers, AArch64 128-bit std::atomic and an explicit __atomic_compare_exchange_16 call, successful/failed CAS, eight-thread CAS contention (80000 increments), acquire/release payload handoff and outline atomic helpers. |
| `jumps` | Guest setjmp/longjmp and sigsetjmp/siglongjmp, stack/register state and saved signal masks; coroutine and fault-recovery paths. |
| `signals` | sigaction/raise with SA_SIGINFO, siginfo fields, pending blocked signals, sigprocmask/pthread_sigmask, pthread_kill to a worker and a reply, sigaltstack and a deliberate null-load SIGSEGV recovered with siglongjmp; IL2CPP/Boehm GC and crash handling, without timers. |
| `time` | MONOTONIC/REALTIME/BOOTTIME/THREAD_CPUTIME clocks and resolutions, gettimeofday/time, nanosleep/usleep, monotonic sleep duration check, UTC-only localtime_r/gmtime_r/strftime/mktime roundtrips; frame and wall clocks. |
| `files` | fopen/fread/fwrite/fseek/ftell/fflush/EOF, open/read/write/lseek/pread/pwrite/close, stat/fstat/lstat, mkdir/rmdir/unlink/rename, directory iteration, access/realpath/getcwd, private file mmap (changes do not persist), anonymous zeroed mmap, mprotect/munmap/msync; bundles and save data. |
| `dl` | NOW/LAZY/GLOBAL/LOCAL loading, function/data lookup, default-scope visibility, reference counts, missing libraries/symbols and dlerror reset, dladdr, guest dl_iterate_phdr callback, auxv HWCAP/page size and SDK system property; native plugin discovery. |
| `misc` | Environment mutation, qsort/bsearch guest callbacks, seeded rand_r/random replay and nonconstant output, errno isolation, sysconf/uname/isatty, IDs and raw gettid syscall, actual blocked futex wait/wake, prctl names, pipe/poll/eventfd/epoll and bidirectional socketpair; engine platform glue. |
| `log` | __android_log_print, __android_log_write and __android_log_vprint with mixed guest varargs; returns/filtering are runtime policy, successful execution must not crash. |
| `neon` | Vector float add/multiply against scalar arithmetic, vtbl out-of-range zero behavior, signed/unsigned saturating ops, float16 storage conversions, HWCAP-guarded signed dot product; math, codecs and DSP. |
| `jni` | JNI_OnLoad/GetEnv/FindClass/RegisterNatives, registered addition, exported Java multiplication/greeting/runner; future bidirectional Java bridge. |
| `ndk_api` | Linked AAssetManager/AAsset calls with a future real-manager probe, dlsym of ANativeWindow APIs and API-guarded AChoreographer symbols; Android platform glue without a device or Surface. |

## NDK/API quirks and runtime obligations

The API 23 builds intentionally omit symbols absent from that Android level:
pthread barriers and AChoreographer (introduced in API 24), pthread_getname_np
(API 26; the older build reads its name using prctl), aligned_alloc (API 28).
Their other checks and fixed checkpoint output are identical. ARM32 Clang has no
`__int128`, so that build checks through 64 bits. Optional AArch64 dot product is
skipped when not advertised, with identical stdout. These conditions are also
recorded in the manifests; a PASS is for the variant's supported API surface.

The API 32 build explicitly selects native TLS and **standard** DT_RELR tags
because the NDK toolchain otherwise selects Android-specific RELR tags. Both old
and new fixtures also carry Android APS2 relocation streams. Prebuilt NDK C++
support may still contain emutls even in the native-TLS variant: the runtime must
handle native TLS and bundled emutls code together. ELF symbol versions (LIBC,
LIBM, LIBDL, etc.) and default symbol interposition must work. For ARM32, support
ELF32 REL relocations, its TLS/va_list/pthread layouts and AAPCS32 float argument
rules when execution support arrives.

`imports.txt` is the exact per-library inventory produced with:

```text
llvm-readelf --dyn-syms --wide <library.so>
```

It includes weak undefined symbols and version suffixes. The manifest's
`artifacts` also lists DT_NEEDED and definitions. Symbols supplied by the peer
DSOs or bundled `libc++_shared.so` are guest imports, not HLE requirements. Static
C++ support introduces more libc imports than each source explicitly names;
provide all remaining imports from the listed HLE system libraries. FORTIFY
wrappers such as `__memcpy_chk`, `__strlen_chk` and `__vsnprintf_chk`, data such as
`__sF`, `__errno`, stack protection, `__cxa_atexit`/`__cxa_finalize`, and
`__cxa_thread_atexit_impl` are part of the real NDK ABI. Guest unwind/C++ code
needs correct dl_iterate_phdr metadata and execution/callback trampolines.
Compiler-rt/libatomic helpers, including outline and 128-bit atomics, are linked
into guest code; they are not necessarily undefined imports requiring HLE.

The runtime needs coherent exclusive monitors and acquire/release ordering,
bionic-compatible structures and errno TLS, guest callbacks from native HLE,
signal frames/alternate stacks/mask restoration, a Linux-like VFS and descriptor
model (including futex/eventfd/epoll), working UTC time conversion and clocks,
correct dlopen scope/unload semantics, and auxv/properties consistent with the
chosen variant. `ro.build.version.sdk` must be numeric and at least the fixture's
minimum API. `uname` describes the guest as Linux; IDs and online CPU count must
be valid without exposing any particular host values.

`jni` is marked `requires_java_bridge` and should be reported as pending until
the Java bridge exists. Load it through that bridge so JNI_OnLoad runs with a
real guest JavaVM/JNIEnv before calling vtest_main. The matching
`java/VtestJni.java` checks native registration, symbol binding, return values and
NewStringUTF; no Java compiler or Android SDK is required for today's native
build. Without JNI_OnLoad, vtest_main deliberately fails.

For the future asset stage, package `assets/vtest-asset.txt` at the asset root and
call exported `vtest_ndk_asset_probe(AAssetManager*)`; 0 means pass, negative values
identify failure. Today's ndk_api entry checks symbol existence and the probe's
null guard; it does not claim to have tested a real asset manager, Surface or
Choreographer callback delivery. liblog messages should appear in the runtime
log, separately from the stdout golden files.

Builds and ELF audits were verified for all four output variants on Windows x64
using NDK r30. **No ARM code was executed.** The golden outputs and synchronization
paths were reviewed for deterministic behavior; actual runtime execution remains
the integration validation step once the runtime implements the harness.

Run the integrated runtime fixtures after building the host and guest targets:

```powershell
python tools/run_guest_tests.py
```

The harness runs each test in a fresh process, enforces its manifest timeout,
and compares exact stdout bytes and the exit code with `expected/`. It covers
both complete ARM64 variants plus the shared-libc++ fixture. Results and diffs
are written below `build/guest/results/`; use `--variant` and `--test` to narrow
an investigation. The host build compiles VtestJni.java; the runtime loads its
class and calls JNI_OnLoad through the real bridge before vtest_main.

Native engine diagnostics use `venture-runtime --game <install> --boot-native`.
This converts APK Java classes when no explicit `--class-path` is supplied.
Conversion can report missing framework dependencies: this diagnostic mode still
attempts individual class loads under HotSpot verification, without claiming the
whole APK is verified or starting an Android Activity. See `docs/COMPATIBILITY.md`
for the actual game stages reached.
