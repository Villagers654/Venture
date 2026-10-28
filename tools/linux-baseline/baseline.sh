#!/usr/bin/env bash
# Build the Linux release on the glibc 2.35 baseline (docs/LINUX_RELEASE_BUILD.md).
#
#   tools/linux-baseline/baseline.sh image        # build the container image
#   tools/linux-baseline/baseline.sh deps         # ANGLE + FFmpeg for the baseline
#   tools/linux-baseline/baseline.sh release      # configure, build, package-release
#   tools/linux-baseline/baseline.sh run CMD...   # any command in the container
#
# Rootless podman; the checkout and $HOME are mounted at their own paths, so the
# .deps links and caches resolve as on the host. Baseline-built ANGLE/FFmpeg
# live in a separate data root (VENTURE_DATA_DIR, default .deps/baseline-data)
# so they never mix with artifacts built against a newer host glibc.
# Environment: VENTURE_BASELINE_JOBS (default 12), VENTURE_BASELINE_IMAGE,
# VENTURE_BASELINE_NAME (container name, e.g. for podman pause/unpause),
# VENTURE_BASELINE_CMAKE_ARGS (extra configure arguments, e.g.
# -DFETCHCONTENT_SOURCE_DIR_<NAME>=... to reuse already fetched sources).
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/../.." && pwd)
image=${VENTURE_BASELINE_IMAGE:-localhost/venture-linux-baseline:22.04}
data=${VENTURE_BASELINE_DATA:-$root/.deps/baseline-data}
jobs=${VENTURE_BASELINE_JOBS:-12}

run() {
  mkdir -p "$data"
  podman run --rm ${VENTURE_BASELINE_NAME:+--name "$VENTURE_BASELINE_NAME"} --userns=keep-id --network=host --security-opt label=disable \
    -v "$HOME:$HOME" -w "$root" -e HOME="$HOME" -e VENTURE_DATA_DIR="$data" \
    -e PATH="$root/.deps/cmake/bin:$root/.deps/ninja:$root/.deps/jdk/bin:/opt/venture-baseline/bin:/usr/lib/llvm-22/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
    "$image" "$@"
}

case "${1:-}" in
  image) podman build -t "$image" -f "$here/Containerfile" "$here" ;;
  deps)
    run python3 tools/angle/build_angle.py --jobs "$jobs"
    run python3 tools/ffmpeg/build_ffmpeg.py ;;
  release)
    run bash -c "cmake --preset linux-baseline ${VENTURE_BASELINE_CMAKE_ARGS:-} && cmake --build build/linux-baseline -j $jobs --target package-release" ;;
  run) shift; run "$@" ;;
  *) sed -n '2,13p' "$0"; exit 2 ;;
esac
