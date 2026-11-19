#!/usr/bin/env bash
# Collect WebKit2GTK 4.1 (GTK 3) and the libraries it needs into DEST, for the
# AppImage's Meta sign-in window where the system has no WebKitGTK that can
# open: the Steam Frame and SteamOS ship none, and GTK 4's WebKitGTK 6.0
# cannot open on a gamescope older than 3.16.26. The sign-in helper
# (src/launcher/platform/posix/auth_browser.cpp) uses DEST only then.
#
#   bundle_webkit.sh DEST       (in Ubuntu 22.04, the glibc 2.35 baseline,
#                                with libwebkit2gtk-4.1-0, glib-networking
#                                and patchelf installed)
#
# Ubuntu's libwebkit2gtk-4.1-0 is the security-maintained WebKitGTK of
# 22.04 (jammy-updates); each release takes its current version. Like other
# AppImages (the AppImage project's excludelist) the bundle leaves out the
# C runtime and what must match the host's drivers, display server and
# session: glibc, libstdc++, GL/EGL/GBM/DRM, Wayland, X11/xcb, fontconfig,
# freetype, harfbuzz, D-Bus, systemd. Every bundled library and program
# finds its neighbours through RUNPATH ($ORIGIN), so nothing changes the
# launcher's own library search.
#
# WebKit starts its web and network processes from the folder compiled into
# it (PKGLIBEXECDIR, /usr/lib/<triplet>/webkit2gtk-4.1; WEBKIT_EXEC_PATH counts
# only in developer builds). That path is rewritten, at the same length, to
# ./lib/<triplet>/webkit2gtk-4.1 (as "././/lib/..."), relative to the working directory, which
# the sign-in helper sets to DEST (as Tauri's AppImages do).
#
# Layout:  DEST/lib/*.so*                                libraries
#          DEST/lib/<triplet>/webkit2gtk-4.1/            WebKit's helper programs and injected bundle
#          DEST/lib/gio/modules/libgiognutls.so          TLS for libsoup (glib-networking)
#          DEST/VERSION                                  the package version
set -euo pipefail
dest=$1
case $(dpkg --print-architecture) in
  amd64) triplet=x86_64-linux-gnu ;;
  arm64) triplet=aarch64-linux-gnu ;;
  *) echo "Unsupported architecture" >&2; exit 1 ;;
esac
lib=/usr/lib/$triplet
webkit=$lib/libwebkit2gtk-4.1.so.0
helpers=$lib/webkit2gtk-4.1
[ -f "$webkit" ] && [ -d "$helpers" ] && [ -f "$lib/gio/modules/libgiognutls.so" ] ||
  { echo "Install libwebkit2gtk-4.1-0 and glib-networking first" >&2; exit 1; }
command -v patchelf >/dev/null || { echo "Install patchelf first" >&2; exit 1; }

host_provided='^(ld-linux.*|libc|libm|libdl|libpthread|librt|libresolv|libutil|libnsl|libanl|libmvec|libgcc_s|libstdc\+\+|libGL|libGLX|libGLdispatch|libOpenGL|libEGL|libGLESv1_CM|libGLESv2|libgbm|libdrm|libdrm_.*|libwayland-client|libwayland-server|libwayland-egl|libwayland-cursor|libX11|libX11-xcb|libxcb.*|libfontconfig|libfreetype|libharfbuzz|libexpat|libz|libdbus-1|libsystemd|libudev|libasound|libgpg-error|libgcrypt|libselinux|libcap)\.so'

rm -rf "$dest"
mkdir -p "$dest/lib/gio/modules" "$dest/lib/$triplet"
cp -a "$helpers" "$dest/lib/$triplet/webkit2gtk-4.1"
cp -L "$webkit" "$dest/lib/"
cp -L "$lib/gio/modules/libgiognutls.so" "$dest/lib/gio/modules/"

# The dependency closure of everything bundled, without what the host provides.
copy_deps() {
  ldd "$1" | awk '/=> \//{print $3}' | while read -r dep; do
    name=$(basename "$dep")
    [[ $name =~ $host_provided ]] && continue
    [ -e "$dest/lib/$name" ] && continue
    cp -L "$dep" "$dest/lib/$name"
    copy_deps "$dest/lib/$name"
  done
}
while IFS= read -r -d '' file; do
  if file -b "$file" | grep -q '^ELF'; then copy_deps "$file"; fi
done < <(find "$dest/lib" -type f -print0)

# RUNPATH: libraries beside each other; helpers and modules one or two levels down.
for file in "$dest"/lib/*.so*; do patchelf --set-rpath '$ORIGIN' "$file"; done
while IFS= read -r -d '' file; do
  file -b "$file" | grep -q '^ELF' || continue
  depth=$(realpath --relative-to="$dest/lib" "$(dirname "$file")" | awk -F/ '{print NF}')
  up=$(printf '/..%.0s' $(seq "$depth"))
  patchelf --set-rpath "\$ORIGIN$up" "$file"
done < <(find "$dest/lib/$triplet" "$dest/lib/gio" -type f -print0)

# The compiled-in helper folder, relative to the working directory.
python3 - "$dest" "$helpers" <<'EOF'
import pathlib, sys
dest, old = pathlib.Path(sys.argv[1]), sys.argv[2].encode()
new = b"././/" + old[len(b"/usr/"):]  # same length: ./lib/...
assert old.startswith(b"/usr/") and len(new) == len(old)
patched = 0
for f in dest.rglob("*"):
    if f.is_file() and not f.is_symlink():
        data = f.read_bytes()
        if old in data:
            f.write_bytes(data.replace(old, new))
            patched += 1
assert patched, "WebKit's helper folder was not found in the bundle"
print(f"rewrote the helper folder in {patched} files")
EOF

dpkg-query -W -f='${Version}\n' libwebkit2gtk-4.1-0 > "$dest/VERSION"
du -sh "$dest"
