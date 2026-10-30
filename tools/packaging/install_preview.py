#!/usr/bin/env python3
"""Update a verified portable build in one owned preview directory."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import sys
import tempfile
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent / "windows"))
from build_installer import load_layout

OWNER = "Venture Steam Link Passthrough preview"
MARKER = "venture-preview-install.json"


def reject_links(path: Path, tree: bool = False) -> None:
    for candidate in (path, *path.parents):
        if candidate.is_symlink() or (hasattr(candidate, "is_junction") and candidate.is_junction()):
            raise RuntimeError("Preview paths must not contain links: " + str(candidate))
    if tree and path.exists():
        for directory, directories, files in os.walk(path, followlinks=False):
            for name in directories + files:
                candidate = Path(directory) / name
                if candidate.is_symlink() or (hasattr(candidate, "is_junction") and candidate.is_junction()):
                    raise RuntimeError("Preview layout must not contain links: " + str(candidate))


def write_json(path: Path, value: dict) -> None:
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def managed_files(manifest: Path) -> dict:
    files = json.loads(manifest.read_text(encoding="utf-8"))["files"]
    # An old manifest supplies only a removal list: its files may have been
    # changed or partly updated. Never let it name installer metadata or escape.
    for name in files:
        relative = PurePosixPath(name)
        if (not relative.parts or relative.is_absolute() or ".." in relative.parts or
                ":" in name or "\\" in name or
                relative.parts[0].lower() in {MARKER, "current.json", "release-files.json", "versions"}):
            raise RuntimeError("Unsafe preview manifest path: " + name)
    return files


def matches(path: Path, entry: dict) -> bool:
    if not path.is_file() or path.stat().st_size != entry["bytes"]:
        return False
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest() == entry["sha256"]


def copy_verified(source: Path, target: Path, entry: dict) -> None:
    if matches(target, entry):
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    # At most one file is duplicated temporarily; no complete staged build or
    # backup is retained. A failed copy cannot truncate the installed file.
    with tempfile.NamedTemporaryFile(prefix=target.name + ".", suffix=".tmp", dir=target.parent, delete=False) as stream:
        temporary = Path(stream.name)
    try:
        shutil.copy2(source, temporary)
        if not matches(temporary, entry):
            raise RuntimeError("Package changed while installing: " + str(source))
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)


def remove_legacy_versions(root: Path) -> None:
    versions = root / "versions"
    if not versions.exists():
        return
    # Only former installer outputs are removed. Unknown files/directories are
    # left alone, even inside this owned preview root.
    for candidate in sorted(versions.iterdir()):
        if not candidate.is_dir() or not re.fullmatch(r"[0-9a-f]{24}", candidate.name):
            continue
        try:
            names = load_layout(candidate)
        except (RuntimeError, ValueError, KeyError, OSError):
            continue
        if ("venture-passthrough.cmd" in names and
                hashlib.sha256((candidate / "release-files.json").read_bytes()).hexdigest()[:24] == candidate.name):
            shutil.rmtree(candidate)
    if not any(versions.iterdir()):
        versions.rmdir()


def install(layout: Path, root: Path, revision: str) -> dict:
    # Validate before creating or modifying an installation. Paths are absolute
    # without resolving links away, so redirected destinations are detected.
    layout, root = Path(os.path.abspath(layout)), Path(os.path.abspath(root))
    reject_links(layout, tree=True)
    reject_links(root, tree=True)
    if root == root.parent or root.is_relative_to(layout) or layout.is_relative_to(root):
        raise RuntimeError("Choose a separate preview installation directory")
    names = load_layout(layout)
    if "venture-passthrough.cmd" not in names:
        raise RuntimeError("The package lacks its passthrough starter")
    files = managed_files(layout / "release-files.json")
    manifest_bytes = (layout / "release-files.json").read_bytes()
    manifest_hash = hashlib.sha256(manifest_bytes).hexdigest()
    marker = root / MARKER
    if root.exists():
        if not marker.is_file() or json.loads(marker.read_text()).get("owner") != OWNER:
            raise RuntimeError("Destination is not an owned Venture preview; choose a new directory")
    else:
        root.mkdir(parents=True)
        write_json(marker, {"owner": OWNER, "schema": 2})
    current = root / "current.json"
    if current.exists() and json.loads(current.read_text()).get("owner") != OWNER:
        raise RuntimeError("The current preview receipt has a different owner")
    previous_manifest = root / "release-files.json"
    previous_files = managed_files(previous_manifest) if previous_manifest.exists() else {}
    # A partial in-place update must not advertise a successfully installed
    # revision. Rerunning the installer repairs/rechecks every managed file.
    current.unlink(missing_ok=True)
    # Free former builds before copying, so migration also works on a disk
    # without room for another complete installation. The source is verified.
    remove_legacy_versions(root)
    for name, entry in files.items():
        copy_verified(layout / name, root / name, entry)
    for name in previous_files.keys() - files.keys():
        obsolete = root / name
        obsolete.unlink(missing_ok=True)
        parent = obsolete.parent
        while parent != root and parent.is_dir() and not any(parent.iterdir()):
            parent.rmdir()
            parent = parent.parent
    copy_verified(layout / "release-files.json", previous_manifest,
                  {"bytes": len(manifest_bytes), "sha256": manifest_hash})
    receipt = {"owner": OWNER, "schema": 2, "revision": revision,
               "directory": str(root), "manifest_sha256": manifest_hash}
    write_json(current, receipt)
    return receipt


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--layout", type=Path, required=True)
    parser.add_argument("--install-root", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    args = parser.parse_args()
    print(json.dumps(install(args.layout, args.install_root, args.revision)))


if __name__ == "__main__":
    main()
