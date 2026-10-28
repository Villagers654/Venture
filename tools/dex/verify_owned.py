#!/usr/bin/env python3
"""Run conversion on each installed owned APK; missing installations are explicitly skipped."""

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.venture_paths import data_dir
import argparse
import json
import os
from convert_apk import build_tools, convert

PACKAGES = ["com.resolutiongames.baitsantacruz", "com.commutergames.vspeedwayalpha", "com.resolutiongames.ignis",
            "com.AnotherAxiom.GorillaTag", "games.b4t.epicrollercoasters.oculus", "com.CactusVRStudios.CactusCowboyPlantsAtWar",
            "com.magnopus.ElixirStore", "com.ILMxLAB.VaderImmortal.ep1", "com.Oculus.FirstContact"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--games", type=Path, default=data_dir() / 'games')
    parser.add_argument("--cache", type=Path, default=Path("build/dex-cache"))
    parser.add_argument("--framework", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path, default=Path("build/java-owned-results.json"))
    args = parser.parse_args()
    results = []
    tool_dir = build_tools()
    for package in PACKAGES:
        apks = sorted((args.games / package).glob("*/*.apk"), key=lambda p: int(p.parent.name) if p.parent.name.isdigit() else 0)
        if not apks:
            row = {"package": package, "skipped": "APK absent"}
        else:
            dest, report = convert(apks[-1], package, args.cache, [p.resolve() for p in args.framework], tool_dir=tool_dir)
            row = {k: report.get(k) for k in ("package", "dex_classes", "jar_classes", "dex_natives", "jar_natives", "failure_count", "seconds", "ready", "dex_sha256")}
            row["report"] = str(dest / "report.json")
        results.append(row)
        print(json.dumps(row), flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(results, indent=2), encoding="utf-8")
    return 2 if any(r.get("ready") is False for r in results) else 0


if __name__ == "__main__":
    raise SystemExit(main())
