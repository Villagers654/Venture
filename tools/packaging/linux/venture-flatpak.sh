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
# Vulkan: the runtime's /usr/share/vulkan/icd.d is a symlink to the GL extensions' merged
# /usr/lib/<triplet>/GL/vulkan/icd.d, which the loader also searches itself, so every driver
# loaded twice and each GPU enumerated twice with the same deviceUUID (GPU selection then
# fails). Pin the loader to the GL extensions' directory once, unless the user chose drivers.
if [ -z "$VK_DRIVER_FILES" ] && [ -z "$VK_ICD_FILENAMES" ]; then
  icd=/usr/lib/$(uname -m)-linux-gnu/GL/vulkan/icd.d
  [ -d "$icd" ] && export VK_DRIVER_FILES="$icd"
fi
exec /app/venture/venture "$@"
