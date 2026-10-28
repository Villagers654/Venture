#!/usr/bin/env bash
# Package a Linux release layout (dist/Venture-linux64 from the baseline build,
# docs/LINUX_RELEASE_BUILD.md) as tar.gz, .deb, .rpm, AUR PKGBUILD, AppImage
# and Flatpak bundle. Every artifact name contains "linux".
#
#   tools/packaging/linux/build_packages.sh [--layout DIR] [--out DIR] [--version X.Y.Z] [FORMAT...]
#   FORMAT: tar deb rpm aur appimage flatpak (default: all)
#
# Needs rootless podman (deb: baseline image, rpm: Fedora image), appimagetool
# (APPIMAGETOOL, default appimagetool on PATH) and flatpak with
# org.freedesktop.Platform//$FLATPAK_BRANCH (default 26.08) installed.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/../../.." && pwd)
layout=$root/dist/Venture-linux64
out=$root/dist/packages
version=$(sed -n 's/^project(Venture VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
formats=()
while [ $# -gt 0 ]; do
  case "$1" in
    --layout) layout=$(realpath "$2"); shift 2 ;;
    --out) out=$(realpath -m "$2"); shift 2 ;;
    --version) version=$2; shift 2 ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) formats+=("$1"); shift ;;
  esac
done
[ ${#formats[@]} -gt 0 ] || formats=(tar deb rpm aur appimage flatpak)
[ -x "$layout/venture" ] && [ -f "$layout/release-files.json" ] || { echo "Not a release layout: $layout" >&2; exit 1; }
id=org.venture_emu.Venture
base=Venture-$version-linux-x86_64
baseline_image=${VENTURE_BASELINE_IMAGE:-localhost/venture-linux-baseline:22.04}
fedora_image=${VENTURE_FEDORA_IMAGE:-registry.fedoraproject.org/fedora:44}
flatpak_branch=${FLATPAK_BRANCH:-26.08}
work=$out/.work
rm -rf "$work"; mkdir -p "$work" "$out"
in_container() {  # IMAGE CMD... with $root and $out mounted at their own paths
  local image=$1; shift
  podman run --rm --security-opt label=disable -v "$root:$root:ro" -v "$out:$out" \
    -v "$layout:$layout:ro" -w "$out" "$image" "$@"
}

# Desktop integration files shared by every format.
desktop_files() {  # DEST: share/{applications,icons,metainfo}
  install -Dm644 "$here/$id.desktop" "$1/applications/$id.desktop"
  install -Dm644 "$root/src/launcher/assets/venture.svg" "$1/icons/hicolor/scalable/apps/$id.svg"
  install -d "$1/metainfo"
  sed "s/@VERSION@/$version/" "$here/$id.metainfo.xml" > "$1/metainfo/$id.metainfo.xml"
}
# FHS tree for deb/rpm: /opt/venture + /usr/bin/venture + desktop files.
fhs_tree() {
  mkdir -p "$1/opt" "$1/usr/bin"
  cp -a --reflink=auto "$layout" "$1/opt/venture"
  ln -s ../../opt/venture/venture "$1/usr/bin/venture"
  desktop_files "$1/usr/share"
}

do_tar() {
  local stage=$work/tar/Venture-linux64
  mkdir -p "$stage"; cp -a --reflink=auto "$layout/." "$stage/"
  desktop_files "$stage/share"
  tar -C "$work/tar" --owner=0 --group=0 --sort=name -czf "$out/$base.tar.gz" Venture-linux64
  echo "tar: $out/$base.tar.gz"
}

do_deb() {
  local stage=$work/deb
  fhs_tree "$stage"
  install -Dm644 "$root/LICENSE" "$stage/usr/share/doc/venture/copyright"
  mkdir -p "$stage/DEBIAN"
  cat > "$stage/DEBIAN/control" <<EOF
Package: venture
Version: $version
Architecture: amd64
Maintainer: Venture developers <https://github.com/Villagers654/Venture/issues>
Installed-Size: $(du -sk "$stage" | cut -f1)
Depends: libc6 (>= 2.35), libgcc-s1
Recommends: libvulkan1, libwayland-client0, libwayland-egl1, libxkbcommon0, libpipewire-0.3-0 | libpulse0, libsecret-1-0, libfontconfig1
Section: games
Priority: optional
Homepage: https://venture-emu.org/
Description: Play Meta Quest games on PC VR
 Venture runs Meta Quest games on a PC VR headset through OpenXR. Every
 library it needs beyond glibc, the Vulkan loader and the desktop session is
 bundled in /opt/venture.
EOF
  in_container "$baseline_image" dpkg-deb --root-owner-group -Zxz -b "$stage" "$out/$base.deb" >/dev/null
  echo "deb: $out/$base.deb"
}

do_rpm() {
  local stage=$work/rpm
  fhs_tree "$stage/root"
  sed -e "s/@VERSION@/$version/" "$here/venture.spec" > "$stage/venture.spec"
  in_container "$fedora_image" bash -c "
    set -e; (command -v rpmbuild || dnf -y -q install rpm-build) >/dev/null 2>&1
    rpmbuild -bb --quiet --define '_topdir $stage/top' --define 'stage $stage/root' \
      --define '_build_id_links none' $stage/venture.spec"
  mv "$stage"/top/RPMS/x86_64/venture-*.rpm "$out/$base.rpm"
  echo "rpm: $out/$base.rpm"
}

do_aur() {
  [ -f "$out/$base.tar.gz" ] || do_tar
  local sum aur=$out/aur
  sum=$(sha256sum "$out/$base.tar.gz" | cut -d' ' -f1)
  mkdir -p "$aur"
  sed -e "s/@VERSION@/$version/g" -e "s/@SHA256@/$sum/g" "$here/PKGBUILD.in" > "$aur/PKGBUILD"
  cat > "$aur/.SRCINFO" <<EOF
pkgbase = venture-bin
	pkgdesc = Play Meta Quest games on PC VR (prebuilt Linux release)
	pkgver = $version
	pkgrel = 1
	url = https://venture-emu.org/
	arch = x86_64
	license = GPL-3.0-or-later
	depends = glibc>=2.35
	depends = gcc-libs
	optdepends = vulkan-icd-loader: Vulkan loader (required to run games)
	optdepends = libsecret: keep sign-ins in the desktop keyring
	optdepends = fontconfig: system fonts in the launcher
	provides = venture
	conflicts = venture
	options = !strip
	options = !debug
	source = $base.tar.gz::https://github.com/Villagers654/Venture/releases/download/v$version/$base.tar.gz
	sha256sums = $sum

pkgname = venture-bin
EOF
  echo "aur: $aur/PKGBUILD"
}

do_appimage() {
  local appdir=$work/Venture.AppDir tool=${APPIMAGETOOL:-appimagetool}
  mkdir -p "$appdir/usr"
  cp -a --reflink=auto "$layout" "$appdir/usr/venture"
  desktop_files "$appdir/usr/share"
  cp "$appdir/usr/share/applications/$id.desktop" "$appdir/$id.desktop"
  cp "$appdir/usr/share/icons/hicolor/scalable/apps/$id.svg" "$appdir/$id.svg"
  ln -s "$id.svg" "$appdir/.DirIcon"
  cat > "$appdir/AppRun" <<'EOF'
#!/bin/sh
here=$(dirname "$(readlink -f "$0")")
exec "$here/usr/venture/venture" "$@"
EOF
  chmod +x "$appdir/AppRun"
  ARCH=x86_64 VERSION=$version "$tool" --no-appstream "$appdir" "$out/$base.AppImage" >/dev/null
  echo "appimage: $out/$base.AppImage"
}

do_flatpak() {
  # Binary repackaging needs no SDK build step: the flatpak CLI alone (build-init,
  # build-finish, build-export) assembles the app from the layout, with the
  # finish-args of org.venture_emu.Venture.yml (which flatpak-builder can also use).
  local fp=$work/flatpak app args
  app=$fp/app
  flatpak build-init "$app" "$id" org.freedesktop.Platform org.freedesktop.Platform "$flatpak_branch" >/dev/null
  cp -a --reflink=auto "$layout" "$app/files/venture"
  install -Dm755 "$here/venture-flatpak.sh" "$app/files/bin/venture-flatpak.sh"
  desktop_files "$app/files/share"
  sed -i "s/^Exec=venture /Exec=venture-flatpak.sh /" "$app/files/share/applications/$id.desktop"
  mapfile -t args < <(sed -n 's/^  - \(--[a-z-]*=.*\|--[a-z-]*\)$/\1/p' "$here/$id.yml")
  flatpak build-finish "$app" --command=venture-flatpak.sh "${args[@]}" >/dev/null
  # OSTree refuses writes when less than 3% of the disk is free; a bundle's
  # throwaway repository only needs room for itself.
  if command -v ostree >/dev/null; then
    ostree init --repo="$fp/repo" --mode=archive-z2
    ostree config --repo="$fp/repo" set core.min-free-space-size 256MB
  fi
  flatpak build-export "$fp/repo" "$app" stable >/dev/null
  flatpak build-bundle "$fp/repo" "$out/$base.flatpak" "$id" stable
  echo "flatpak: $out/$base.flatpak"
}

for format in "${formats[@]}"; do "do_$format"; done
rm -rf "$work"
( cd "$out" && sha256sum Venture-* > SHA256SUMS-linux.txt 2>/dev/null || true )
ls -la "$out"
