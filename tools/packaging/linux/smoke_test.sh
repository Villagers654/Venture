#!/usr/bin/env bash
# Install and start each Linux package in a bare container of its target
# distribution (no GPU, no game): `venture --version`, `venture-runtime --help`,
# and the bundled JRE drawing text headlessly. Also lists, for every ELF in the
# layout, each library the system has to provide.
#
#   tools/packaging/linux/smoke_test.sh [--layout DIR] [--packages DIR] [TEST...]
#   TEST: ldd deb rpm aur appimage flatpak (default: all)
set -uo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/../../.." && pwd)
layout=$root/dist/Venture-linux64
packages=$root/dist/packages
tests=()
while [ $# -gt 0 ]; do
  case "$1" in
    --layout) layout=$(realpath "$2"); shift 2 ;;
    --packages) packages=$(realpath "$2"); shift 2 ;;
    *) tests+=("$1"); shift ;;
  esac
done
[ ${#tests[@]} -gt 0 ] || tests=(ldd deb rpm aur appimage flatpak)
pkg() { ls "$packages"/Venture-*-linux-x86_64."$1" 2>/dev/null | head -n 1; }
probe_dir=$(mktemp -d); trap 'rm -rf "$probe_dir"' EXIT
failures=0
result() { echo "== $1: $2"; [ "$2" = PASS ] || failures=$((failures + 1)); }
box() {  # IMAGE SCRIPT: run SCRIPT in a bare container with the inputs mounted read-only
  podman run --rm --network=host --security-opt label=disable -v "$layout:/layout:ro" \
    -v "$packages:/packages:ro" -v "$probe_dir:/probe:ro" -v "$here:/packaging:ro" "$1" bash -c "$2"
}

# Text drawn by the bundled JRE (java.desktop, headless) with its own fonts.
cat > "$probe_dir/FontProbe.java" <<'EOF'
import java.awt.*; import java.awt.image.BufferedImage;
public class FontProbe { public static void main(String[] a) {
  BufferedImage image = new BufferedImage(64, 16, BufferedImage.TYPE_INT_ARGB);
  Graphics2D g = image.createGraphics(); g.setFont(new Font(Font.SANS_SERIF, Font.PLAIN, 12));
  g.drawString("Venture", 0, 12); int ink = 0;
  for (int y = 0; y < 16; y++) for (int x = 0; x < 64; x++) if ((image.getRGB(x, y) >>> 24) != 0) ink++;
  System.out.println("font " + g.getFont().getFontName() + " ink " + ink); System.exit(ink > 0 ? 0 : 1); } }
EOF
"$root/.deps/jdk/bin/javac" --release 21 -d "$probe_dir" "$probe_dir/FontProbe.java"

# $1 = installed layout directory inside the container.
checks='
set -e; d=$1
"$d/venture" --version
"$d/venture-runtime" --help | head -n 1
"$d/java/jdk/bin/java" -Djava.awt.headless=true -cp /probe FontProbe
'

ldd_check='
rm -f /tmp/missing
for f in $(find /layout -type f); do
  head -c 4 "$f" 2>/dev/null | grep -q ELF || continue
  out=$(ldd "$f" 2>&1) || true
  # Sonames the layout itself ships are loaded by the application first (dlopen
  # by path: FFmpeg, the JVM); only libraries found nowhere count as missing.
  for lib in $(echo "$out" | sed -n "s/^[[:space:]]*\([^ ]*\) => not found.*/\1/p"); do
    [ -n "$(find /layout -name "$lib" -print -quit)" ] || { echo "MISSING $lib (needed by ${f#/layout/})"; touch /tmp/missing; }
  done
  echo "$out" | sed -n "s/^[[:space:]]*\([^ ]*\) => \(\/[^ ]*\).*/\1 \2/p" | grep -v " /layout/" | cut -d" " -f1
done | sort | uniq -c | sort -rn > /tmp/system-libs
echo "system libraries used (count of ELF files):"; cat /tmp/system-libs
[ ! -e /tmp/missing ]'

for t in "${tests[@]}"; do case $t in
  ldd)
    for image in docker.io/library/ubuntu:22.04 docker.io/library/ubuntu:24.04 docker.io/library/debian:12 \
                 registry.fedoraproject.org/fedora:41 docker.io/library/archlinux:latest docker.io/opensuse/tumbleweed:latest; do
      echo "--- ldd + start, bare $image"
      if box "$image" "command -v find >/dev/null || zypper -nq in findutils >/dev/null 2>&1
          command -v ldd >/dev/null || zypper -nq in glibc-devel >/dev/null 2>&1
          ( $ldd_check ) && bash -c '$checks' _ /layout"
      then result "layout on $image" PASS; else result "layout on $image" FAIL; fi
    done ;;
  deb)
    for image in docker.io/library/ubuntu:22.04 docker.io/library/ubuntu:24.04 docker.io/library/debian:12; do
      echo "--- deb on $image (no recommends)"
      if box "$image" "set -e; export DEBIAN_FRONTEND=noninteractive; apt-get -qq update >/dev/null
          cp /packages/$(basename "$(pkg deb)") /tmp/v.deb; apt-get -qq install -y --no-install-recommends /tmp/v.deb >/dev/null
          dpkg -s venture | grep -E '^(Version|Depends):'; command -v venture; bash -c '$checks' _ /opt/venture"
      then result "deb on $image" PASS; else result "deb on $image" FAIL; fi
    done ;;
  rpm)
    for image in registry.fedoraproject.org/fedora:41 registry.fedoraproject.org/fedora:44 docker.io/opensuse/tumbleweed:latest; do
      echo "--- rpm on $image (no weak deps)"
      if box "$image" "set -e; f=/packages/$(basename "$(pkg rpm)")
          if command -v dnf >/dev/null; then dnf -y -q install --setopt=install_weak_deps=False \$f >/dev/null
          else zypper -nq --no-gpg-checks in --no-recommends --allow-unsigned-rpm \$f >/dev/null; fi
          rpm -q venture; command -v venture; bash -c '$checks' _ /opt/venture"
      then result "rpm on $image" PASS; else result "rpm on $image" FAIL; fi
    done ;;
  aur)
    echo "--- AUR venture-bin with makepkg -si on archlinux"
    if box docker.io/library/archlinux:latest "set -e; pacman -Sqyu --noconfirm --needed base-devel >/dev/null 2>&1
        useradd -m build; echo 'build ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/build
        cp -r /packages/aur /home/build/venture-bin; cp /packages/$(basename "$(pkg tar.gz)") /home/build/venture-bin/
        chown -R build /home/build/venture-bin
        su build -c 'cd ~/venture-bin && makepkg -si --noconfirm >/dev/null 2>&1'
        pacman -Qi venture-bin | grep -E '^(Version|Depends On)'; command -v venture; bash -c '$checks' _ /opt/venture"
    then result "AUR on archlinux" PASS; else result "AUR on archlinux" FAIL; fi ;;
  appimage)
    echo "--- AppImage on bare ubuntu:22.04 (no FUSE: --appimage-extract-and-run)"
    if box docker.io/library/ubuntu:22.04 "set -e; cp /packages/$(basename "$(pkg AppImage)") /tmp/v.AppImage
        /tmp/v.AppImage --appimage-extract-and-run --version"
    then result "AppImage on ubuntu:22.04" PASS; else result "AppImage on ubuntu:22.04" FAIL; fi ;;
  flatpak)
    echo "--- Flatpak bundle, user installation (a throwaway FLATPAK_USER_DIR; runtime from the system)"
    id=org.venture_emu.Venture
    export FLATPAK_USER_DIR=$probe_dir/flatpak-user
    mkdir -p "$FLATPAK_USER_DIR"
    if command -v ostree >/dev/null; then  # OSTree's 3%-free-disk guard, for this throwaway repo only
      ostree init --repo="$FLATPAK_USER_DIR/repo" --mode=bare-user-only
      ostree config --repo="$FLATPAK_USER_DIR/repo" set core.min-free-space-size 256MB
    fi
    if flatpak --user install -y --noninteractive --bundle "$(pkg flatpak)" >/dev/null && \
       flatpak run --command=/app/venture/venture $id --version && \
       flatpak run --command=/app/venture/venture-runtime $id --help | head -n 1 && \
       flatpak run --filesystem="$probe_dir:ro" --command=/app/venture/java/jdk/bin/java $id \
         -Djava.awt.headless=true -cp "$probe_dir" FontProbe
    then result "Flatpak (user install)" PASS; else result "Flatpak (user install)" FAIL; fi
    flatpak --user info $id | grep -E "Runtime|Version|Installed" || true
    flatpak --user uninstall -y --noninteractive $id >/dev/null; unset FLATPAK_USER_DIR ;;
esac; done
echo "failures: $failures"; exit $((failures > 0))
