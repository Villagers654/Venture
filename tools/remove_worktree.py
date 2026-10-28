#!/usr/bin/env python3
"""Remove a git worktree (or a leftover worktree directory) WITHOUT following
directory junctions or symlinks.

Worktrees link shared toolchains into .deps/ with junctions. Recursive deletion
tools (robocopy /MIR, PowerShell 5.1 Remove-Item -Recurse, git worktree remove
on Windows) can follow those junctions and delete the shared toolchain itself.
This script unlinks every junction/symlink first, then deletes real files.

  python tools/remove_worktree.py C:/Users/me/Venture-wt/old-branch [...]
"""
from __future__ import annotations

import os
import stat
import subprocess
import sys
from pathlib import Path


def is_link(entry: os.DirEntry) -> bool:
    if entry.is_symlink():
        return True
    try:
        return Path(entry.path).is_junction()
    except OSError:
        return False


def unlink_link(path: str) -> None:
    if os.name == "nt":
        # rmdir removes a directory junction/symlink itself, never its target.
        result = subprocess.run(["cmd", "/c", "rmdir", path], capture_output=True)
        if result.returncode == 0:
            return
    os.unlink(path)


def remove_tree(root: str) -> None:
    with os.scandir(root) as entries:
        for entry in entries:
            if is_link(entry):
                unlink_link(entry.path)
            elif entry.is_dir(follow_symlinks=False):
                remove_tree(entry.path)
            else:
                try:
                    os.unlink(entry.path)
                except PermissionError:
                    os.chmod(entry.path, stat.S_IWRITE)
                    os.unlink(entry.path)
    os.rmdir(root)


def main() -> None:
    for arg in sys.argv[1:]:
        path = Path(arg).resolve(strict=False)
        if not path.exists():
            continue
        long_path = "\\\\?\\" + str(path) if os.name == "nt" else str(path)
        remove_tree(long_path)
        subprocess.run(["git", "worktree", "prune"], cwd=Path(__file__).resolve().parent.parent, check=False)
        print(f"removed {path}")


if __name__ == "__main__":
    main()
