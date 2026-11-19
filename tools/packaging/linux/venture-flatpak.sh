#!/bin/sh
# Flatpak entry point: point the bundled OpenXR loader at the host runtime.
# Flatpak moves XDG_CONFIG_HOME into the sandbox, so the loader would not find
# ~/.config/openxr/1/active_runtime.json (exposed by --filesystem=xdg-config/openxr:ro)
# or the host's /etc/xdg one (host-etc). A runtime library given by an absolute
# host path that the sandbox lacks is redirected to the host-os mount /run/host.
#
# SteamVR's manifest is a link into the Steam folder, often through Steam's own
# links in ~/.steam (steam, root), which the sandbox does not have: such a
# target is looked up in the Steam folder itself (~/.local/share/Steam,
# exposed read-only for SteamVR).
link_target() {  # LINK: its target inside the sandbox, or nothing
  target=$(readlink "$1") || return 1
  case "$target" in /*) ;; *) target="$(dirname "$1")/$target" ;; esac
  [ -e "$target" ] && { readlink -f "$target"; return 0; }
  # A host manifest's absolute link names the host's /usr or /etc.
  case "$1" in /run/host/*) [ -e "/run/host$target" ] && { readlink -f "/run/host$target"; return 0; } ;; esac
  for link in "$HOME/.steam/steam/" "$HOME/.steam/root/"; do
    case "$target" in
      "$link"*) mapped="$HOME/.local/share/Steam/${target#"$link"}"
                [ -e "$mapped" ] && { readlink -f "$mapped"; return 0; } ;;
    esac
  done
  return 1
}
if [ -z "$XR_RUNTIME_JSON" ]; then
  for json in "$HOME/.config/openxr/1/active_runtime.json" \
              /run/host/etc/xdg/openxr/1/active_runtime.json \
              /run/host/usr/share/openxr/1/active_runtime.json; do
    if [ -L "$json" ]; then
      json=$(link_target "$json") || continue
    fi
    [ -e "$json" ] || continue
    lib=$(sed -n 's/.*"library_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$json" | head -n 1)
    case "$lib" in
      /*) if [ ! -e "$lib" ] && [ -e "/run/host$lib" ]; then
            out="${XDG_RUNTIME_DIR:-/tmp}/app/${FLATPAK_ID:-org.venture_emu.Venture}"
            mkdir -p "$out"
            sed "s#\"library_path\"[[:space:]]*:[[:space:]]*\"/#\"library_path\": \"/run/host/#" "$json" > "$out/active_runtime.json"
            json="$out/active_runtime.json"
          fi ;;
    esac
    # Relative library paths resolve beside the real manifest (json is no link here).
    export XR_RUNTIME_JSON="$json"
    break
  done
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
