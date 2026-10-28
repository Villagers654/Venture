# Oculus Platform HLE

`make_runtime_session()` and `register_library()` replace the guest
`libovrplatformloader.so` before any original loader code runs. The aliases cover
bare Unity P/Invoke names, Android sonames, and `libovrplatform.so`. Keep the
session alive until all guest threads stop. The core/launcher integrator calls:

```cpp
auto identity = venture::ovrplatform::read_app_identity(
    plan_path, process_config().package, process_config().version_code);
auto platform = venture::ovrplatform::make_runtime_session(identity);
venture::ovrplatform::register_library(*platform);
// Then load the guest. On logout or process teardown: platform->shutdown().
```

The runtime core implementations are absent from this branch. The library and
its registration code compile against their existing headers; no core contracts
were changed. `main.cpp` belongs to the integrator and is not replaced here.

## App identity and ownership

None of the nine surveyed APK manifests has `com.oculus.vr.APP_ID`. Read the
launcher's `plan.json` `appId`, `binaryId`, `package`, and `versionCode`; check the
package/version against the independently parsed APK. Require the game's native
initialization argument to match that app ID. Do not infer it from package names,
public store titles, installed files, or a delivery-app ID. Download/signing
validation remains the launcher's responsibility. A plan is not ownership proof.

The provider uses the same live active-entitlement query as `tools/meta_cli.py`
(`doc_id=4850747515044496`). Each entitlement request reads the current protected
session, makes a fresh request, and checks that the session did not change while
the response was in flight. Only an exact active Quest-app match grants success.
An incomplete/missing page is unknown; a complete page without the app is
not-entitled. Errors, wrong accounts/platforms/packages, revoked grants, and
network failures never grant ownership. There is no offline success cache.

The native runtime reads DPAPI CurrentUser JSON v1 with entropy
`QuestPort.MetaSession.v1` from the user's known LocalAppData Venture directory.
The fallback reader lives under `runtime/platform/win32`, not in the portable
provider. It is read-only and never logs credentials. `SecretStore` is injectable
so the launcher's common implementation can replace this fallback at integration.
The POSIX implementation compiles but returns unavailable until a libsecret
adapter is supplied; it never falls back to plaintext. No user/global XR or
Oculus settings are touched.

## Providers and observable failure

`Provider` separates ownership, app-scoped user identity, access token, and proof.
Only ownership has a production provider. The scoped-user seam and its accessors
are implemented and tested, but the account-wide ID/alias is deliberately not
returned as an app-scoped user. The three scoped-auth calls complete with
unsupported errors; synchronous `ovr_GetLoggedInUserID` is zero until a provider
establishes a legitimate scoped identity. Initialization establishes the local
facade and loads the session; the separate entitlement request verifies ownership.
`ovr_SetDeveloperAccessToken` cannot replace the signed-in account or verify an
arbitrary token's scope: it reports failure (native void callers also get a log).

237 generated request operations cover achievements, leaderboards, IAP, rooms,
matchmaking, cloud storage, VoIP, NetSync, challenges, group presence, moderation,
and integrity APIs. Unsupported requests return unique nonzero 64-bit IDs and
complete with the exact pinned message type and an error. The custom-type HTTP
request also completes with an error; it never sends account credentials to a
guest-selected URL. Packet sends fail; receives and voice PCM polls report no
data. Normal launch details contain no invitations or invented room/session IDs.
Unsupported synchronous services and private/unknown signatures remain absent
from the symbol table so the core missing-symbol diagnostic stays visible.

Error_GetCode returns the explicit Venture service error range defined in
`provider.h` (-20000 through -20009), not host errno or invented Meta error codes.
Platform APIs report errors in messages and do not use libc errno. Raw server
error text, tokens, account IDs/aliases, and nonce contents are never logged.
Request diagnostics include function, ID, numeric message type and error code.
Caller-module/offset and timing tracing remain integration work.

## Queue and ABI

Messages are enqueued in completion order. Polling is nonblocking. A host worker
handles network requests; it never enters guest code. Initialization queues
subsequent requests until it finishes. Shutdown cancels pending work exactly once
and rejects late completions. Request IDs are never reused within a session.

Message/error/user/proof/initialize/options/launch handles are opaque in the SDK,
not public C structs. Small handle cells and all borrowed strings are allocated
through AddressSpace in production; C++ strings/maps are never exposed to guest
code. A registry validates handle kind. FreeMessage releases its entire graph;
options have Create/Destroy lifetime and launch details survive until shutdown.
The public by-reference POD layouts have fixed-width fields and static_asserts
in `abi.h`. No Windows `long` crosses the guest ABI. AAPCS32 is not implemented.

Numeric messages and signatures come from Meta's native 1.1.71 declarations
(pinned mirror commit in `sdk_sources.json`), official Unity Platform packages
53.0.0-preview and 69.0.0, with overlapping message values checked against
207.0.0. Each overlapping enum value must agree during generation. These are
declaration versions, not claims that all surveyed loaders have that build.
The measured native loader builds are 1.1.32, .45, .72, .79, .101, and .117;
Epic's build is unknown. Public handle-based APIs avoid depending on their
private Horizon object layouts. Private or differing signatures are not guessed.

Android WithOptions register positions and Unity internal-wrapper aliases were
also checked against the V-Speedway/Bait loader's A64 code: WithOptions at
0x537f0 preserves X0..X4, and Wrapper at 0x53570 tail-calls Internal with the same
app ID and JNI context. Nonempty init options receive an explicit failure.
The nine measured export lists contain no `ovr_Platform*Version` function; version
macros/build strings are not callable exports. No fictitious version API or SDK
capability version is advertised.

`tests/runtime/ovrplatform_coverage.txt` lists all 1,136 names, exported-by games,
and import/retained-name references. 502 corpus names resolve; 634 remain missing.
Retained Unity wrapper names do not establish that gameplay invokes them.

Regenerate declaration tables and the coverage report with:

```text
python tools/bootstrap.py --platform-sdk
python src/runtime/hle/ovrplatform/generate.py --evidence <r5-platform/evidence>
```

SDK packages are SHA-256 verified and cached in LocalAppData/Venture/dev-cache;
only local declaration copies go into `.deps`. Generated tables and hashes, not
Meta implementations or owned game binaries, are checked in. Normal builds need
neither the SDK downloads nor the corpus.

## Auth investigation and validation (2026-10-08)

The runtime-side `venture-runtime-platform-check` tool reads credentials only
inside its native process. It accepts a plan, APK package/version and optional
`--probe-auth`. Only status codes/booleans reach stdout. It never persists tokens
or gives candidates to guest code.

Using the real V-Speedway plan (Quest app 3927222113966481, build 745):

* Fresh account entitlement succeeded.
* POST `/authenticate_application` with that game's own app ID returned HTTP 200
  and a token different from the profile token.
* GET `/me?fields=id,alias` with the candidate returned HTTP 200 and a numeric
  identity different from the stored profile identity. Neither value was logged.
* This establishes that the candidate exchange works for this app/account, not
  that the result is the SDK's app-scoped identity/token. No service-reported
  app-scope confirmation, comparison with the same app on Quest, or unmodified
  game-backend acceptance test was available. No proof-minting endpoint was
  established. Thus gameplay access-token, identity and proof remain unsupported.

Meta's [account-linking reference](https://developers.meta.com/vr/documentation/android-apps/ps-account-linking/)
also uses `/me` for org-scoped identities; that endpoint's success cannot by itself
prove SDK app scope. The [user-verification reference](https://developers.meta.com/vr/documentation/native/ps-ownership/)
describes nonce validation, not a public nonce-minting method. There is no integrity
or anti-cheat bypass, forged nonce, or alternate publisher login in this code.
This follows the provider split and truthful fallback policy in the supplied
`r5-platform/report.md` (2026-10-08).

Verified with the requested llvm-mingw Windows release preset: 20 doctest cases,
including real loopback HTTP, wrong account/app, revocation, partial pages,
session replacement during a response, redaction, queue ordering, cancellation,
concurrent consumers, AAPCS64 wrappers, and all registered request completions.
The corpus validator re-extracted all nine real APK loaders and matched all
1,136 export names. PE imports show only Windows/UCRT DLLs (static C++ and curl).
The POSIX secret-store translation unit passed a syntax check; a native Linux
build was not run because this host has no WSL installation.

Remaining gaps: 634 unregistered names (mostly models/private synchronous APIs),
nonempty initialization options, service callbacks/notifications, real scoped
auth/proof, remote social/commerce/game services, POSIX secret storage, A32, and
execution through the merged loader/JIT. Completed unsupported errors are not
claims that those online services or the games are playable.
