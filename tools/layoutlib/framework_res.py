"""Builds the framework-res.apk Venture's layoutlib framework reads at run time.

layoutlib's framework classes (View, TextView, PhoneWindow, ...) were compiled
against com.android.internal.R; javac inlined those resource IDs into their code.
The resources come from the Android SDK platform's framework sources (data/res)
and are compiled with aapt2, with the IDs pinned to layoutlib's (--stable-ids and
staged groups from FrameworkResourceIds.java). A resource layoutlib names but the
platform sources lack (layoutlib's framework is one API level newer) gets a
neutral placeholder value at its ID, so a lookup finds a value instead of
failing; file resources (layouts, menus, ...) without a source are reported.

Usage: framework_res.py --aapt2 A --java J --layoutlib JAR --res DIR --out APK --work DIR
"""
import argparse
import hashlib
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path

HERE = Path(__file__).resolve().parent
VALUE_TAGS = {"string": "string", "color": "color", "dimen": "dimen", "bool": "bool", "integer": "integer",
              "style": "style", "attr": "attr", "array": "array", "string-array": "array",
              "integer-array": "array", "plurals": "plurals", "drawable": "drawable", "fraction": "fraction"}
PLACEHOLDERS = {
    "string": '<string name="{0}" translatable="false"></string>',
    "color": '<color name="{0}">#00000000</color>',
    "dimen": '<dimen name="{0}">0dp</dimen>',
    "bool": '<bool name="{0}">false</bool>',
    "integer": '<integer name="{0}">0</integer>',
    "fraction": '<fraction name="{0}">0%</fraction>',
    "array": '<array name="{0}" />',
    "plurals": '<plurals name="{0}"><item quantity="other"></item></plurals>',
    "drawable": '<drawable name="{0}">#00000000</drawable>',
    "style": '<style name="{0}" />',
    "attr": '<attr name="{0}" format="reference|color|boolean|integer|float|dimension|string|enum|flags|fraction" />',
    "id": '<item type="id" name="{0}" />',
}
# Placeholders whose neutral value breaks the code that reads it. Neither is in
# the public AOSP sources yet, so the value is the one the framework code implies.
# - With a zero default cursor blink interval,
#   ViewConfiguration.getTextCursorBlinkIntervalMillis is 0 and Editor's Blink
#   re-posts itself with no delay: a focused text field redraws its window on
#   every pass of the main looper. 500 ms is Editor's own initial interval.
# - AdaptiveIconDrawable parses the banner mask in its constructor and refuses an
#   empty path, so no adaptive icon inflates (the default activity icon a window
#   decor loads included). A full square crops nothing.
AOSP_VALUES = {
    "integer/def_accessibility_text_cursor_blink_interval_ms": '<integer name="{0}">500</integer>',
    "string/config_banner_mask": '<string name="{0}" translatable="false">"M0,0L100,0L100,100L0,100Z"</string>',
}
# The platform's staged public IDs are its own API level's; layoutlib's replace them.
SKIPPED_SOURCES = {"public-staging.xml"}


def defined_names(res):
    names = set()
    for directory in sorted(res.iterdir()):
        kind = directory.name.split("-")[0]
        for path in sorted(directory.iterdir()):
            if kind != "values":
                names.add(f"{kind}/{path.name.split('.')[0]}")
                continue
            if path.name in SKIPPED_SOURCES or path.suffix != ".xml":
                continue
            for element in ElementTree.parse(path).getroot():
                name = element.get("name")
                if not name:
                    continue
                if element.tag == "item":
                    names.add(f"{element.get('type')}/{name}")
                elif element.tag == "declare-styleable":
                    for child in element:
                        if child.tag == "attr" and child.get("name") and ":" not in child.get("name"):
                            names.add(f"attr/{child.get('name')}")
                elif element.tag in VALUE_TAGS:
                    names.add(f"{VALUE_TAGS[element.tag]}/{name}")
    return names


def run(command):
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True)
    if result.returncode:
        sys.stderr.write(result.stdout + result.stderr)
        raise SystemExit(f"failed: {command[0]} ({result.returncode})")
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    for name in ("aapt2", "java", "layoutlib", "res", "out", "work"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    work = args.work
    work.mkdir(parents=True, exist_ok=True)
    names = defined_names(args.res)
    (work / "names.txt").write_text("\n".join(sorted(names)) + "\n", encoding="utf-8")
    generated = work / "generated" / "values"
    if generated.parent.exists():
        shutil.rmtree(generated.parent)
    generated.mkdir(parents=True)
    run([args.java, HERE / "FrameworkResourceIds.java", args.layoutlib, work / "stable-ids.txt",
         generated / "venture_staging.xml", work / "names.txt"])

    wanted = []
    for line in (work / "stable-ids.txt").read_text(encoding="utf-8").splitlines():
        wanted.append(line.split(" = ")[0].removeprefix("android:").replace("^attr-private", "attr"))
    staging = ElementTree.parse(generated / "venture_staging.xml").getroot()
    for group in staging.iter("staging-public-group"):
        wanted += [f"{group.get('type')}/{p.get('name')}" for p in group.iter("public")
                   if not p.get("name").startswith("venture_staged_gap_")]
    placeholders, unfilled = [], []
    for entry in wanted:
        if entry in names:
            continue
        kind, name = entry.split("/", 1)
        if entry in AOSP_VALUES:
            placeholders.append(AOSP_VALUES[entry].format(name))
        elif kind in PLACEHOLDERS:
            placeholders.append(PLACEHOLDERS[kind].format(name))
        else:
            unfilled.append(entry)
    (generated / "venture_placeholders.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<!-- Generated by tools/layoutlib/framework_res.py -->\n'
        "<resources>\n  " + "\n  ".join(placeholders) + "\n</resources>\n", encoding="utf-8")
    (work / "report.txt").write_text(
        f"{len(wanted)} layoutlib resources, {len(placeholders)} placeholders, {len(unfilled)} without a source:\n"
        + "\n".join(unfilled) + "\n", encoding="utf-8")

    # The platform sources compile once per source set (about a minute).
    digest = hashlib.sha256(str(args.res.resolve()).encode() + run([args.aapt2, "version"]).encode()).hexdigest()[:16]
    flat = work / f"flat-{digest}"
    if not (flat / ".complete").exists():
        if flat.exists():
            shutil.rmtree(flat)
        flat.mkdir()
        run([args.aapt2, "compile", "--feature-flags",
             "android.appwidget.flags.use_smaller_app_widget_system_radius=false",
             "--dir", args.res, "-o", flat / "res.zip"])
        (flat / ".complete").write_text("")
    inputs = work / "inputs"
    if inputs.exists():
        shutil.rmtree(inputs)
    inputs.mkdir()
    import zipfile
    with zipfile.ZipFile(flat / "res.zip") as archive:
        for member in archive.namelist():
            if member.removeprefix("values_").removesuffix(".arsc.flat") + ".xml" in SKIPPED_SOURCES:
                continue
            archive.extract(member, inputs)
    for source in generated.iterdir():
        run([args.aapt2, "compile", "-o", inputs, source])
    (work / "inputs.txt").write_text("\n".join(str(p) for p in sorted(inputs.iterdir())) + "\n", encoding="utf-8")
    manifest = work / "AndroidManifest.xml"
    manifest.write_text('<?xml version="1.0" encoding="utf-8"?>\n'
                        '<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="android"'
                        ' coreApp="true">\n  <application android:hasCode="false" />\n</manifest>\n', encoding="utf-8")
    temporary = args.out.with_suffix(".tmp")
    run([args.aapt2, "link", "-o", temporary, "--manifest", manifest, "-x", "--min-sdk-version", "36",
         "--stable-ids", work / "stable-ids.txt", "--no-auto-version", "--no-version-vectors",
         "@" + str(work / "inputs.txt")])
    temporary.replace(args.out)
    print(f"{args.out.name}: {len(placeholders)} placeholders, {len(unfilled)} resources without a source")


if __name__ == "__main__":
    main()
