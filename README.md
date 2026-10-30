<div align="center">

<img src="docs/media/logo.svg" width="72" alt="">

# Venture

**A Meta Quest emulator for PC VR.**

Play the Meta Quest games you own on your PC headset, at full PC speed and resolution.<br>
No virtual machine. Free and open source.

[**Download**](https://github.com/Villagers654/Venture/releases/latest) · [Website](https://venture-emu.org/) · [Report an issue](https://github.com/Villagers654/Venture/issues) · [Discord](https://discord.gg/Ey9hHszvwa)

<img src="docs/media/launcher.webp" width="760" alt="The Venture launcher: game library, a download in progress and Batman: Arkham Shadow ready to play">

</div>

## Why Venture

- **Fast.** Venture is by far the fastest way to play Quest games on PC, with steady frame times.
- **Compatible.** Runs all 25 games we tested; see the [compatibility wiki](docs/COMPATIBILITY.md). The next best runs 7.
- **Straight to your hardware.** Every game runs as an ordinary PC process that renders on your GPU and talks to your headset through OpenXR.
- **Simple.** Sign in with Meta, install, press Play. Your games can show up in your Steam library too.

<img src="docs/media/perf.jpg" alt="Average fps in seven games: Venture is faster in every one">

<img src="docs/media/lows.jpg" alt="1% low fps in seven games: Venture is faster in every one">

<img src="docs/media/compat.jpg" alt="Venture runs all 25 games tested; the next best runs 7">

### Install and play

1. Download `Venture-windows64-setup.exe` from [Releases](https://github.com/Villagers654/Venture/releases/latest) and run it.
   On Linux, install the package for your distribution (`.deb`, `.rpm`, AUR `venture-bin`, AppImage or Flatpak), or extract the `.tar.gz` anywhere.
2. Start Venture and choose Sign in with Meta and use the account that owns your Quest games.
3. Pick a game in your library and choose **Install**.
4. Start your headset's runtime, then choose **Play**.

### Games from somewhere else

The **Import** page adds games that aren't in your Meta library:

- **From a headset:** connect a Quest with developer mode on (USB, or Wi-Fi debugging) and choose **Scan headset**. Nothing on the headset is changed.
- **From this computer:** choose or drop an APK, a ZIP, or a folder with the APK and its data.

### Play from Steam

Turn on **Add games to Steam** in Settings. Installed games appear in Steam and SteamVR the next time Steam starts.

### Mods

Mods that replace a game's expansion file (OBB), such as RE4 VR Definitive Edition, go in the game's mods folder: open the game in your library and choose **Open folder** next to **Mods**, then put the mod's file there under the same name as the installed file (for example `main.203.com.Armature.VR4.obb`). It is used from the next launch; your installation is not changed, and deleting the file goes back to the original.

### Something wrong?

If a game crashes, click **Report issue** on the banner that appears. Otherwise, use **Upload debug report** in Settings. Either way, Venture collects the launcher and game logs from your last launch into one debug report, uploads it as an unlisted paste that expires after 90 days, and gives you a link to copy. If the upload fails, attach the report file instead (**Show file**).

[Open an issue](https://github.com/Villagers654/Venture/issues/new/choose) with the game, what happened, how to make it happen again and the debug report link. To watch a game's output live, turn on **Show runtime console** in Settings. For help or to chat, join the [Discord](https://discord.gg/Ey9hHszvwa).

### Build (Windows)

```sh
python tools/bootstrap.py --jdk --angle      # pinned toolchain, JDK and ANGLE into .deps/
.deps/cmake/bin/cmake --preset windows-release
.deps/cmake/bin/cmake --build --preset windows-release
```

### Build (Linux)

```sh
python3 tools/bootstrap.py --jdk             # pinned CMake, Ninja and JDK into .deps/
.deps/cmake/bin/cmake --preset linux-release
.deps/cmake/bin/cmake --build --preset linux-release
```

## License

Venture is licensed under GPL-3.0-or-later: see [LICENSE](LICENSE).

It's built on other people's work, and we're grateful for it. Code from these projects is used under their own licenses, kept in [LICENSES/](LICENSES):

| License | Whose code |
|---|---|
| [GPL-3.0-or-later](LICENSE) | [Eden](https://eden-emu.dev/)'s fork of Dynarmic, by the Eden Emulator Project |
| [0BSD](LICENSES/0BSD.txt) | [Dynarmic](https://github.com/merryhime/dynarmic), by merryhime (MerryMage) and its contributors |
| [MIT](LICENSES/MIT.txt) | [musl](https://musl.libc.org/), by Rich Felker et al. |
| [LGPL-2.1](LICENSES/LGPL-2.1.txt) | [FFmpeg](https://ffmpeg.org/), by the FFmpeg developers |

Thank you to the Eden Emulator Project, whose Dynarmic fork Venture's JIT is built on; to merryhime and everyone who has worked on Dynarmic, which made a fast, accurate ARM JIT possible; and to the emulator projects it grew up with and borrows from, yuzu, Citra and Dolphin (Gekko). Thank you also to AXRB for pioneering Quest emulation on PC VR.

Venture is not affiliated with Meta; see [DISCLAIMER.md](DISCLAIMER.md).

## Star history

<a href="https://www.star-history.com/?type=date&repos=Villagers654%2FVenture">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=Villagers654/Venture&type=date&theme=dark&legend=top-left" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=Villagers654/Venture&type=date&legend=top-left" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=Villagers654/Venture&type=date&legend=top-left" />
 </picture>
</a>
