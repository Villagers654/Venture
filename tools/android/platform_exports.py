#!/usr/bin/env python3
"""Write the exports of Venture's Android libovrplatformloader.so.

The library must export every function Meta's loader exports, or a game that
imports one fails to load (Android binds every import at load). The set is
the export union of Meta's loaders (tests/runtime/ovrplatform_coverage.txt)
plus every function the Platform HLE implements (its VENTURE_EXPORT entries
and generated tables). Each export is a native entry image slot
(src/runtime/cpu/native_image_slot.inc) whose id is its position in the
sorted list, from 1; the names table maps ids back to names.

The 32-bit library (--isa arm, for ARM32 games on CPUs that run AArch32) has
ARM slots instead: each loads its id into r12 and branches to the A32 entry
(android_game/platform/a32_entry.S), which converts the AAPCS32 call to the
A64 handler's registers by the export's signature (--signatures, from the
pinned Platform SDK descriptors in src/runtime/core/generated/a32_vendor.json;
see android_game/platform/a32_call.h for the letters).

  platform_exports.py --repo DIR --asm OUT.S --names OUT.inc [--isa arm --signatures OUT.inc]
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

INC_PATTERNS = {
    "requests.inc": r'VENTURE_REQUEST\("([^"]+)"',
    "options.inc": r'VENTURE_OPTION\("([^"]+)"',
    "model_getters.inc": r'VENTURE_MODEL_GETTER\("([^"]+)"',
    "message_getters.inc": r'VENTURE_GETTER\("([^"]+)"',
}
NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")

# Descriptor kinds -> signature letters (android_game/platform/a32_call.h).
WORD_SIGNED = {"I8", "I16", "I32", "SSize", "Long", "Off", "Time"}
WORD_UNSIGNED = {"U8", "U16", "U32", "Ptr", "Size", "ULong", "Handle", "Callback"}
# Pointers to records whose A64 layout the handlers read, by descriptor tag.
LAYOUTS = {
    "JNIEnv *": "u", "int16_t *": "u", "uint32_t *": "u",  # same on both
    "const char *const *": "P", "char **": "P",              # pointer arrays, count next
    "ovrKeyValuePair *": "K",                                 # pair arrays, count next
    "ovrMatchmakingCustomQueryData *": "Q",
}
# Entry points the descriptors lack: the WithOptions initializers
# (appId, activity, JNIEnv, ovrKeyValuePair[], count).
EXTRA_SIGNATURES = {
    "ovr_PlatformInitializeAndroidWithOptions": "wuuuKu",
    "ovr_PlatformInitializeAndroidAsynchronousWithOptions": "IuuuKu",
}


def argument_letter(arg: dict) -> str | None:
    kind, tag = arg["kind"], arg.get("tag", "")
    if kind in WORD_SIGNED:
        return "i"
    if kind in WORD_UNSIGNED:
        return "u"
    if kind in ("I64", "U64"):
        return "I"
    if kind == "F32":
        return "f"
    if kind == "F64":
        return "d"
    if kind in ("Layout", "OutPtr"):
        return LAYOUTS.get(tag)
    return None


def result_letter(result: dict) -> str | None:
    kind = result["kind"]
    if kind == "Void":
        return "v"
    if kind == "U8":
        return "B"
    if kind in WORD_SIGNED or kind in WORD_UNSIGNED:
        return "w"
    if kind in ("I64", "U64"):
        return "I"
    if kind == "F32":
        return "f"
    if kind == "F64":
        return "d"
    if kind == "Composite" and result.get("tag") == "ovrKeyValuePair":
        return "C"
    return None


def signatures(repo: Path) -> dict[str, str]:
    """Export name -> signature, for each export the 32-bit library can call."""
    rows = json.loads((repo / "src/runtime/core/generated/a32_vendor.json").read_text(encoding="utf-8"))
    out = dict(EXTRA_SIGNATURES)
    for row in rows:
        if row["library"] != "libovrplatformloader.so" or row.get("variadic"):
            continue
        letters = [result_letter(row["result"])] + [argument_letter(a) for a in row["arguments"]]
        if None in letters:
            continue
        # An array's count is the argument after it.
        if any(c in "PK" and (i + 1 >= len(letters) or letters[i + 1] not in "iu")
               for i, c in enumerate(letters[1:], 1)):
            continue
        out[row["symbol"]] = "".join(letters)
    return out


def export_names(repo: Path) -> list[str]:
    hle = repo / "src/runtime/hle/ovrplatform"
    names: set[str] = set()
    for line in (repo / "tests/runtime/ovrplatform_coverage.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("#"):
            continue
        name = line.split("\t", 1)[0]
        if name and name != "name":
            names.add(name)
    for path in sorted(hle.glob("*.cpp")):
        names.update(re.findall(r'VENTURE_EXPORT\(\s*"([^"]+)"', path.read_text(encoding="utf-8")))
    for inc, pattern in INC_PATTERNS.items():
        names.update(re.findall(pattern, (hle / inc).read_text(encoding="utf-8")))
    for enum in re.findall(r'VENTURE_ENUM_TYPE\("([^"]+)"\)', (hle / "enum_strings.inc").read_text(encoding="utf-8")):
        names.update({enum + "_ToString", enum + "_FromString"})
    bad = sorted(n for n in names if not NAME.match(n))
    if bad:
        raise SystemExit(f"platform_exports: not symbol names: {bad[:5]}")
    return sorted(names)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--asm", type=Path, required=True)
    parser.add_argument("--names", type=Path, required=True)
    parser.add_argument("--isa", choices=("arm64", "arm"), default="arm64")
    parser.add_argument("--signatures", type=Path)
    args = parser.parse_args()
    names = export_names(args.repo)
    banner = "// Generated by tools/android/platform_exports.py. Do not edit.\n"
    outputs = [(args.names, banner + '"",\n' + "".join(f'"{n}",\n' for n in names))]
    if args.isa == "arm64":
        asm = [banner, '#include "cpu/native_image_slot.inc"\n', "  .text\n"]
        asm += [f"VENTURE_IMAGE_SLOT {name}, {i}, venture_native_hle_call\n" for i, name in enumerate(names, 1)]
    else:
        if not args.signatures:
            parser.error("--isa arm needs --signatures")
        asm = [banner, '#include "android_game/platform/a32_slot.inc"\n', "  .text\n"]
        asm += [f"VENTURE_A32_SLOT {name}, {i}\n" for i, name in enumerate(names, 1)]
        table = signatures(args.repo)
        outputs.append((args.signatures, banner + '"",\n' + "".join(f'"{table.get(n, "")}",\n' for n in names)))
        print(f"platform_exports: {sum(1 for n in names if n in table)} with a 32-bit signature")
    asm.append('#if defined(__linux__)\n  .section .note.GNU-stack, "", %progbits\n#endif\n')
    outputs.append((args.asm, "".join(asm)))
    for path, text in outputs:
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists() or path.read_text(encoding="utf-8") != text:
            path.write_text(text, encoding="utf-8", newline="\n")
    print(f"platform_exports: {len(names)} exports")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
