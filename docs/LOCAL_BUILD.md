# Build and install the checkout on Windows

Requires Windows x64 and Git for Windows. From a cloned checkout run:

```powershell
.\install.cmd
```

The script uses Venture's pinned bootstrap and `windows-release` preset, builds
both production executables, and invokes the ordinary release packager. It is
independent of passthrough, preview-packaging helpers and game-specific Java
fixtures. It installs into `%LOCALAPPDATA%\Venture-Local`, creates a desktop
shortcut and launches Venture. Repeated runs update that same directory.

Missing Python 3.13+ is installed per user with winget. A fresh FFmpeg build needs
native GnuPG; when missing, winget installs `GnuPG.GnuPG`, which can request a
Windows administrator prompt. An already verified FFmpeg cache needs no GnuPG
installation. Signature verification remains mandatory. The script does not
change Git's MSYS tools or FFmpeg's build recipe.

The initial build downloads and compiles dependencies. Later runs reuse pinned
toolchains, ANGLE and compiler caches and rebuild changed source. Generated
package copies are removed on success or failure; the caches needed for
incremental builds remain. Full native stdout/stderr is recorded in
`build/local-install/*-build.log`, including Python tracebacks.

Optional arguments:

```powershell
.\install.cmd -Jobs 4 -NoLaunch
.\install.cmd -InstallDir D:\Apps\Venture-Local
.\install.cmd -RunTests
```

`-RunTests` builds and runs the configured test suite before installation.
Without it the script builds the application only. Close Venture and its game
before updating. Source package hashes are checked before installation, copied
files are verified, and only files from the previous release manifest are
removed. Other files are retained. Each file is staged and verified before atomic
replacement, including the release manifest and installation receipt, so a failed
write cannot truncate the existing file. At most one
file is duplicated temporarily. An interrupted update is marked `updating` and
can be repaired by rerunning the script; no successful installation or launch
is reported until verification completes. This is an in-place updater and does
not retain rollback versions.

An optional checkout-local `.venture-install.json` can select different default
installation and shortcut names without changing the build or runtime:

```json
{"installDirectoryName":"Venture-Local","shortcutName":"Venture Local"}
```

The script accepts only simple directory/shortcut names in this profile.
`-InstallDir` still overrides the destination. The profile is optional and is
not required for an ordinary checkout.
