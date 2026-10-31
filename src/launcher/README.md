# Venture native launcher

The launcher's accounts, library, installs, add-ons, downloads, imports and game
launches live in `launcher::Core` (`core.h`). The window (`venture`, `app.h`)
and the command line (`venture-cli`, `cli.cpp`) are two front ends over it, so
both behave the same on Windows and Linux.

## Command line

`venture-cli --help` lists every command. Examples:

```powershell
venture-cli status
venture-cli library --installed
venture-cli info "beat saber"
venture-cli install v-speedway        # downloads as the owning account
venture-cli play v-speedway           # waits for the game; returns its exit code
venture-cli addons install moss --all
venture-cli import D:\apks\game.apk --yes
venture-cli accounts use 2
venture-cli settings set games-folder D:\Venture\games
```

`--json` prints machine-readable results and `--offline` uses only the saved
library. Games are named by name (or a unique part of it), app id or package.
Progress lines go to a terminal; redirected output gets one line per 10% (or
minute). The core's own log goes to `logs/cli-<pid>.log`, not the terminal.

The download queue belongs to one process (`locks/download-queue.lock`): while
the window is open, commands that download, import or cancel refuse and the
window manages them. `play` is a direct launch, like a Steam shortcut, and runs
beside the window. Ctrl+C during `play` asks the game to quit through its
Android lifecycle (exit 130); a second Ctrl+C leaves at once. Commands that ask
for confirmation take `--yes` when stdin is not a console. Exit codes: 0
success, 1 failure, 2 usage, 130 interrupted.

## Import

The **Import** page brings in games installed elsewhere:

- **From a headset**: a Quest connected by USB, or over Wi-Fi debugging
  (type its address and **Connect**), with developer mode on. **Scan
  headset** runs Android platform-tools `adb` (Settings → *Android
  platform-tools (adb)*, else `PATH`, `ANDROID_HOME`, `ANDROID_SDK_ROOT` and
  the SDK's default folders) and lists the installed third-party apps with
  their APK, everything in `/sdcard/Android/obb/<package>/`, and their saves
  in `/sdcard/Android/data/<package>/` (less its cache). With several
  headsets connected, pick one above the list. Nothing on the headset is
  changed.
- **From this computer**: a folder holding an APK and its `<package>/` data
  folder (or its expansion files beside it), the APK itself, or a ZIP archive
  laid out the same way. Choose it with the desktop's file dialog (the
  FileChooser portal on Linux) or drop it on the window.

Venture is a runtime, not a store: any app imports, with no account or
ownership check. An import installs the source's files as they are: its APK
(a modified build included), its expansion files, and its other top-level
data files as required local assets. When a signed-in account (an owner of
the game, else the active one) owns Meta's build with the same version code,
that build only supplies the files the source lacks, downloaded from Meta;
otherwise the install records no Meta build (`"source": "import"` in
`plan.json`). Each file copied from a headset is checked against the SHA-256
the headset computes for it, and every imported file's size and SHA-256 are
recorded in `plan.json`. Headset saves go to the importing account's storage
for the game (`local` without one) when it has none on this computer yet
(existing saves are never replaced). Imports run in the Downloads queue with
its progress, pause, resume and free-space check; an interrupted import
resumes from the same source, continuing a partly copied file where it
stopped. Split APKs are listed as skipped.

**Import save data** in an installed game's menu (`venture-cli import-saves
GAME --from FOLDER | --headset`) brings in saves alone, for the account the
game plays as: from a headset, its `/sdcard/Android/data/<package>/` (a
game's private storage cannot be read over adb); from this computer, a copy
of that folder (or of `/sdcard`, or of the folder's `files/`), or a copy of
the game's private storage (`shared_prefs/`, `databases/`...). Each top-level
folder the import brings replaces the current one, which first moves to
`data/save-backups/<account>/<package>/<time>/` in the data folder. The game
must be closed, and cannot start until the import finishes.

## Steam library

Settings → **Add games to Steam** is enabled by default (`addGamesToSteam`
in `settings.json`). Startup and installation changes sync complete installed
Quest games into the selected Steam account's non-Steam shortcuts, tagged
**Venture**, with OpenVR enabled for SteamVR. The MostRecent login is selected;
with no recent login only a single local userdata profile can be selected.
Other shortcuts, including RiftLift's, are preserved. Uninstalled Venture games
are removed on sync. Disabling the setting stops future syncs; it leaves existing
shortcuts in Steam. Missing Steam installations are harmless.

Steam must be closed before writing. While it runs, Settings shows “N games will
appear in Steam after it restarts” and an explicit **Restart Steam** button.
Venture never closes or starts Steam unless that button is pressed. Pending
changes apply when Steam is observed closed or at the next launcher start.
The button requests Steam's normal shutdown, waits for the client to close,
syncs, then reopens it. Existing malformed shortcuts are left untouched and the
reason is logged. Writes replace files atomically and retain one
`shortcuts.vdf.venture.bak`; cached artwork is converted to PNG under `config/grid`.
`VENTURE_STEAM_ROOT` overrides discovery for isolated tests; use a copied
`VENTURE_DATA_DIR` as well when testing the launcher.
Only interactive library sessions sync. Smoke, screenshot, headless and direct
launch modes skip sync and log that decision once. A redirected `VENTURE_DATA_DIR`
also disables discovery/sync unless `VENTURE_STEAM_ROOT` is explicitly provided.
Missing/unreadable games roots, failed scans and unreadable manifests never drive
shortcut removal; sync waits for an authoritative installation scan.

`venture.exe --launch <package>` runs through the same credential broker and
runtime supervisor as Play, with no visible library UI, and stays alive until
the runtime exits, returning its exit code. Like Play, any installed game
launches; the game's platform calls use a signed-in owner, else the active
account, else no account (Meta then answers them as unauthenticated). An
account picked with **Play as** that needs reconnecting is resolved in Venture
first. An already-open library stays open: the direct launch uses
a separate hidden supervisor. A shared per-package lock rejects a second launch
of a game that is already starting or running in either process.
The hidden supervisor leaves the UI's cached catalog and persistent download
queue alone; on-demand asset downloads use its own in-memory queue.

Steam injects `GameOverlayRenderer64.dll` into launched processes. Our runtime
worker is a separate native child created with `CreateProcessW`: Venture does
not load or explicitly forward the overlay DLL, but Steam's injection hooks can
follow child processes, so the worker may receive it. Its graphics hooks can
interact with Vulkan presentation, ANGLE's D3D/GLES translation and OpenXR's
swapchain/compositor integration. Overlay behavior has not been tested with real
Steam; disable the Steam overlay for a shortcut if it causes graphics problems.
Valve documents the supported graphics hooks in its
[Steam overlay documentation](https://partner.steamgames.com/doc/features/overlay).

On Linux the shortcut uses the absolute quoted Venture launcher binary (normally
`venture`), including quotes around paths with spaces. The same `--launch`
supervisor path uses POSIX process spawning and waits for the game's exit.
Discovery checks `~/.steam/steam`, `~/.local/share/Steam`, and the Flatpak roots
under `~/.var/app/com.valvesoftware.Steam`.

Linux Steam injects `gameoverlayrenderer.so` through `LD_PRELOAD`. Venture's
POSIX process launcher currently inherits that variable, so it also reaches the
runtime supervisor and worker. We leave it intact to preserve the user's Steam
integration; the worker should not blanket-clear `LD_PRELOAD`, which may contain
other user-selected libraries. Overlay-specific filtering would need Linux
compatibility evidence first. Its OpenGL/Vulkan hooks carry the same ANGLE and
OpenXR integration risk described above; no real-Steam overlay test was run.

Build and run from this worktree with the pinned llvm-mingw toolchain:

```powershell
.deps/cmake/bin/cmake --preset windows-release
.deps/cmake/bin/cmake --build --preset windows-release
build/windows-release/bin/venture_tests.exe
build/windows-release/bin/venture.exe
# Optional: --offline opens only the saved library, art, and local installations.
```

`venture.exe` is C++23 / Dear ImGui, using the SDL3 platform and SDL_Renderer3
backends. SDL chooses the graphics driver. The main thread owns the UI and GPU
textures; separate workers handle API requests, image decoding, and a sequential
download queue. SDL events wake the UI for completions. Rendering caps at 60 fps,
with event waits while idle or minimized. Display-scale changes resize the theme
and fonts. Segoe UI Variable/Segoe UI and common Linux fonts have an ImGui fallback.

`src/common` contains HTTP, Meta SSO/store parsing, library/cache reconciliation,
installation scanning, the download manager, paths, encrypted session storage, and
process launching. `platform/win32` implements DPAPI CurrentUser, paths, atomic
file replacement, and process handles. The launcher's Windows `AuthBrowser` hosts
WebView2 on its own STA thread and permits only Meta sign-in domains; callbacks
are challenge-bound, single-use, and expire after ten minutes. Cancellation and
account changes invalidate pending results before any credential write.

The verified NuGet package provides `WebView2.h` and the x64 loader DLL, copied
beside the executable. Small EventToken/UUID declarations bridge the Windows SDK
and MinGW. The Microsoft Edge WebView2 Runtime must be installed. All other
libraries are static. Curl is HTTP/HTTPS-only and uses Schannel on Windows and
OpenSSL on Linux. stb_image decodes JPEG/PNG/GIF; a static libwebp decoder handles
WebP covers now returned by Meta. The public artwork request uses an anonymous
storefront verifier, never the account access token. Dependency sources and their
licenses are under `.deps/fetch` (SDL: zlib; ImGui/JSON/doctest: MIT; curl: curl
license; stb: public domain/MIT; libwebp: BSD; WebView2: Microsoft SDK license).

Windows data lives in `%LOCALAPPDATA%\Venture`. The existing version-one
`meta-session.dpapi` format and `QuestPort.MetaSession.v1` entropy are preserved.
`library.json`, `settings.json`, `downloads.json`, `cache/art`, and `logs` live
alongside it. Neither cached manifests nor the queue store credentials or signed
URLs. On-disk `games/<package>/<versionCode>/plan.json` files written by
`tools/meta_cli.py` are recognized; every required file is size-checked before an
install is marked ready. Partial library responses never revoke confirmed titles.
Downloads recheck ownership and use `latest_supported_binary` with `EUREKA`, then
an application-scoped delivery token. Paused `.part` files survive restart; resume
validates HTTP ranges and final sizes before atomic replacement. A queue restored
after restart stays paused until the user resumes it. Uninstall removes the
confirmed version directory, not other versions or runtime save directories.

Play passes `--game`, `--package`, `--log logs/runtime-<package>.log`, `--account`
and `--parent-pid` to the runtime and serves its credential and asset requests over
inherited pipes. The runtime's stdout/stderr go to `logs/runtime-<package>.console.log`,
so its own log is not written twice; the previous run's log is kept as
`runtime-<package>.prev.log`. The launcher passes its log-level setting in the runtime's
environment as `VENTURE_LOG_LEVEL` (0 trace to 4 error). On a non-zero exit the launcher shows the runtime's
`[runtime]` error line, or the exit code. The runtime is the bundled one beside the
launcher; `settings.json` stores `runtimeExecutable` only when Settings overrides it.

Linux paths (`$XDG_DATA_HOME/venture` or `~/.local/share/venture`) and process
spawning are implemented. Linux sessions live in the Secret Service through
libsecret; plaintext tokens are never stored. The Linux `AuthBrowser` re-runs the
launcher as an `--auth-browser` helper process that loads WebKitGTK 6.0 (or
WebKit2GTK 4.1) at runtime, with an ephemeral cookie session and the same Meta
domain allow-list and callback interception as WebView2. The URL arrives on the
helper's stdin and events return on a private pipe; its stdout/stderr are
discarded. Without WebKitGTK the launcher still runs and sign-in reports the
missing package. A completed interactive Meta login on Linux still needs validation.

For repeatable live verification (no game downloads), run:

```powershell
build/windows-release/bin/venture.exe --smoke-seconds 30 --smoke-report out/smoke.json
# Optional: --smoke-details, --smoke-play, --smoke-auth,
#           --smoke-view settings|account|downloads
.deps/cmake/bin/cmake --build --preset windows-release --target venture_launcher_live_check
build/windows-release/bin/venture_launcher_live_check.exe --read-only-meta
```

The UI smoke mode writes a credential-free JSON report and BMP capture. Play uses
the same launch action as the button; the optional auth check opens Meta without
completing sign-in. The separate read-only check validates library, build listing,
live plan, public artwork, and delivery-token exchange without fetching APKs.

`venture.exe --smoke-install <app id> --smoke-report out/install.json` installs one
game headlessly through the Install action (owner selection, download job and
plan.json), waits for the download and the rescan, writes an `install` section in
the report and exits 0 when the install is complete. `--smoke-seconds` bounds it.

## Steam Frame passthrough setting

**Settings → Runtime → Steam Frame passthrough** is an explicit, default-off
preference (`steamFramePassthrough` in `settings.json`). Its adjacent Info button
states Steam Frame only and links [Steam Link Native Passthrough](https://github.com/McFredward/steam-link-native-passthrough).
The checkbox is disabled outside Windows; the project information remains accessible.

The runtime reads the saved value once in `configure_game`, before XR initialization,
so normal UI and Steam/direct launches use the same preference. A running game
does not change modes when the launcher setting changes. No environment variable
selects this backend. The compatibility CMD only opens the normal launcher.
The optional CLI key `steam-frame-passthrough on|off` edits that same saved preference.
See [mixed reality](../../docs/MIXED_REALITY.md) for required companions and limits.
