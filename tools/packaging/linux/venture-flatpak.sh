#!/bin/sh
# Flatpak entry point: point the bundled OpenXR loader at the host runtime.
# Flatpak moves XDG_CONFIG_HOME into the sandbox, so the loader would not find
# ~/.config/openxr/1/active_runtime.json (exposed by --filesystem=xdg-config/openxr:ro)
# or the host's /etc/xdg one (host-etc). A runtime library given by an absolute
# host path that the sandbox lacks is redirected to the host-os mount /run/host.
#
# A manifest is often a link: SteamVR's into its own folder (the Steam
# library's SteamVR, or a system copy such as /opt/steamvr, both exposed
# read-only), often through Steam's own links in ~/.steam (steam, root), which
# the sandbox does not have; a distribution runtime's into /usr/share. Each
# link is followed inside the sandbox: a target it lacks is looked up under
# /run/host (the host's /usr and /etc), and a target through ~/.steam/steam or
# ~/.steam/root in the Steam folder itself (~/.local/share/Steam).
host=/run/host
resolve() {  # PATH: the file it names inside the sandbox, links followed; fails when it is not visible
  path=$1 hops=0
  while [ -L "$path" ]; do
    hops=$((hops + 1)); [ "$hops" -le 16 ] || return 1
    target=$(readlink "$path") || return 1
    case "$target" in /*) ;; *) target="$(dirname "$path")/$target" ;; esac
    if [ ! -e "$target" ] && [ ! -L "$target" ]; then
      found=
      case "$target" in
        /*) if [ -e "$host$target" ] || [ -L "$host$target" ]; then found=$host$target; fi ;;
      esac
      for link in "$HOME/.steam/steam/" "$HOME/.steam/root/"; do
        case "$target" in
          "$link"*) mapped="$HOME/.local/share/Steam/${target#"$link"}"
                    if [ -e "$mapped" ] || [ -L "$mapped" ]; then found=$mapped; fi ;;
        esac
      done
      [ -n "$found" ] || return 1
      target=$found
    fi
    path=$target
  done
  [ -e "$path" ] || return 1
  readlink -f "$path"
}
if [ -z "$XR_RUNTIME_JSON" ]; then
  for json in "$HOME/.config/openxr/1/active_runtime.json" \
              "$host/etc/xdg/openxr/1/active_runtime.json" \
              "$host/usr/share/openxr/1/active_runtime.json"; do
    json=$(resolve "$json") || continue
    lib=$(sed -n 's/.*"library_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$json" | head -n 1)
    case "$lib" in
      /*) if [ ! -e "$lib" ] && [ -e "$host$lib" ]; then
            out="${XDG_RUNTIME_DIR:-/tmp}/app/${FLATPAK_ID:-org.venture_emu.Venture}"
            mkdir -p "$out"
            sed "s#\"library_path\"[[:space:]]*:[[:space:]]*\"/#\"library_path\": \"$host/#" "$json" > "$out/active_runtime.json"
            json="$out/active_runtime.json"
          fi ;;
    esac
    # Relative library paths resolve beside the real manifest (json is no link here).
    export XR_RUNTIME_JSON="$json"
    break
  done
fi
# SteamVR's client library reads OpenVR's path registry (where SteamVR, its
# config and its logs are) from $XDG_CONFIG_HOME/openvr, which Flatpak moves
# into the app's folder. Name the user's own (xdg-config/openvr:ro) unless one
# is set.
if [ -z "$VR_PATHREG_OVERRIDE" ] && [ -f "$HOME/.config/openvr/openvrpaths.vrpath" ]; then
  export VR_PATHREG_OVERRIDE="$HOME/.config/openvr/openvrpaths.vrpath"
fi
# The client also fails to start when it cannot write its log ("Failed to
# initialize logging"). It logs to the registry's "log" folder (usually Steam's
# logs, which the manifest grants); when the sandbox cannot write that one
# (another Steam layout), VR_LOG_PATH names ~/.config/openvr/logs, also granted.
if [ -z "$VR_LOG_PATH" ] && [ -n "$VR_PATHREG_OVERRIDE" ] && [ -f "$VR_PATHREG_OVERRIDE" ]; then
  log=$(tr -d '\n\r' < "$VR_PATHREG_OVERRIDE" | sed -n 's/.*"log"[[:space:]]*:[[:space:]]*\[[[:space:]]*"\([^"]*\)".*/\1/p')
  if [ -z "$log" ] || [ ! -d "$log" ] || [ ! -w "$log" ]; then
    mkdir -p "$HOME/.config/openvr/logs" 2>/dev/null
    [ -w "$HOME/.config/openvr/logs" ] && export VR_LOG_PATH="$HOME/.config/openvr/logs"
  fi
fi
# Vulkan: the host's own drivers, the ones its native games run on, wherever they load in
# the sandbox; the runtime's GL extension (Flathub's Mesa) only for GPUs the host has none
# for. SteamOS ships a Turnip months newer than the extension's (27.0-devel against 26.2.5
# on the Steam Frame). With the extension's Turnip, the Frame's Adreno 750 hung during
# H3VR2's loads: after its submissions the LRZ unit stayed busy, so the firmware's wait for
# idle at the next preemption to SteamVR's compositor never ended, and the kernel blamed
# whichever app submitted next. Native SteamVR games, on the host's Turnip, did not hang.
# A host driver is used when the runtime's dynamic loader resolves its library completely
# (every symbol version present, and libraries only the host has linked in from it, below);
# a driver for another architecture or a newer C library than the runtime's is left out.
# The runtime's /usr/share/vulkan/icd.d is a symlink to the GL extensions' merged
# /usr/lib/<triplet>/GL/vulkan/icd.d, which the loader also searches itself, so every driver
# loaded twice and each GPU enumerated twice with the same deviceUUID (GPU selection then
# fails): the loader is given one list of manifests, unless the user chose drivers.
ldso=$(ls /lib/ld-linux-*.so.* /lib64/ld-linux-*.so.* 2>/dev/null | head -n 1)
library() {  # MANIFEST: its library_path
  tr -d '\n\r' < "$1" | sed -n 's/.*"library_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p'
}
hostlib() {  # NAME: the host's copy of a library, under $host
  for dir in /usr/lib/$(uname -m)-linux-gnu /usr/lib64 /usr/lib; do
    [ -f "$host$dir/$1" ] && { echo "$host$dir/$1"; return 0; }
  done
  return 1
}
# LIBRARY: true when the runtime's dynamic loader resolves all of it. A library it needs
# that the runtime lacks (SteamOS's Mesa links libSPIRV-Tools.so, which no Flatpak runtime
# has) is linked from the host into $deps.next, so only names the runtime does not provide
# come from the host; a symbol version the runtime's libraries lack rejects the driver. Not
# linked, so the driver stays out: a name the app bundles (its own copies load through
# $ORIGIN, which LD_LIBRARY_PATH would override), and a library versioned to its exact
# build (soname x.y.z: a driver release's private part, which loads companions of the same
# release by dlopen that no trace shows, as NVIDIA's do).
loads() {
  [ -n "$ldso" ] && [ -f "$1" ] || return 1
  rm -rf "$deps.next" && mkdir -p "$deps.next" || return 1
  [ -d "$deps" ] && cp -P "$deps"/* "$deps.next"/ 2>/dev/null
  tries=0
  while [ "$tries" -lt 16 ]; do
    tries=$((tries + 1))
    status=0
    trace=$(LD_LIBRARY_PATH="$deps.next${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$ldso" --list "$1" 2>&1) || status=$?
    case "$trace" in *"not found (required by"*) return 1 ;; esac
    missing=$(printf '%s\n' "$trace" | sed -n \
      -e 's/^[[:space:]]*\([^[:space:]]*\) => not found.*/\1/p' \
      -e 's/.*error while loading shared libraries: \([^:]*\): cannot open.*/\1/p' | head -n 1)
    if [ -z "$missing" ]; then
      [ "$status" -eq 0 ]
      return
    fi
    case "$missing" in */*|*.so.*.*.*) return 1 ;; esac
    [ -e "$deps.next/$missing" ] && return 1
    [ -n "$(find /app -name "$missing" -print 2>/dev/null | head -n 1)" ] && return 1
    found=$(hostlib "$missing") || return 1
    ln -s "$found" "$deps.next/$missing" || return 1
  done
  return 1
}
if [ -z "$VK_DRIVER_FILES" ] && [ -z "$VK_ICD_FILENAMES" ]; then
  icd=/usr/lib/$(uname -m)-linux-gnu/GL/vulkan/icd.d
  out="${XDG_RUNTIME_DIR:-/tmp}/app/${FLATPAK_ID:-org.venture_emu.Venture}/vulkan"
  deps="$out/lib"
  rm -rf "$deps" "$deps.next"
  files= taken=" "
  for json in "$host"/usr/share/vulkan/icd.d/*.json "$host"/etc/vulkan/icd.d/*.json; do
    [ -f "$json" ] || continue
    lib=$(library "$json")
    case "$lib" in
      /*) ;;
      */*|"") continue ;;
      *)  # A bare name (Debian's manifests): the host's library directories.
          lib=$(hostlib "$lib") || continue
          lib=${lib#"$host"} ;;
    esac
    name=$(basename "$lib")
    case "$taken" in *" $name "*) continue ;; esac
    mkdir -p "$out" || break
    loads "$host$lib" || continue
    rm -rf "$deps" && mv "$deps.next" "$deps" || continue
    copy="$out/host-$(basename "$json")"
    sed "s#\"library_path\"[[:space:]]*:[[:space:]]*\"[^\"]*\"#\"library_path\": \"$host$lib\"#" "$json" > "$copy" || continue
    files="${files:+$files:}$copy" taken="$taken$name "
  done
  rm -rf "$deps.next"
  if [ -n "$files" ]; then
    for json in "$icd"/*.json; do
      [ -f "$json" ] || continue
      case "$taken" in *" $(basename "$(library "$json")") "*) continue ;; esac
      files="$files:$json"
    done
    export VK_DRIVER_FILES="$files"
    if [ -n "$(ls -A "$deps" 2>/dev/null)" ]; then
      export LD_LIBRARY_PATH="$deps${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    fi
  else
    rm -rf "$deps"
    [ -d "$icd" ] && export VK_DRIVER_FILES="$icd"
  fi
fi
exec /app/venture/venture "$@"
