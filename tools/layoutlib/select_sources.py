#!/usr/bin/env python3
"""Choose the sources of venture-framework-2d.jar (docs/design/layoutlib.md).

A 2D process runs layoutlib's Android framework classes. Venture's framework
classes join it in three ways:
  - a class layoutlib does not have (com.venture.runtime, Horizon OS APIs, ...)
    always comes from Venture;
  - a class both have comes from layoutlib, unless it matches a pattern in the
    keep file: those are Venture's host services (media, audio, databases,
    sensors, ...), which reach the host through Venture's natives;
  - a source under the 2D overlay directory replaces Venture's file of the same
    name, or adds a class;
  - a --fork-class from Venture's layoutlib fork (github.com/Villagers654/
    venture-layoutlib, bridge/src) does the same as an overlay source.
Writes the javac source list and the classes taken from Venture or the overlay
that layoutlib also has (ProcessLayoutlib leaves those out of layoutlib.jar).
"""
import argparse
import os
import re
import sys
from pathlib import Path


def java_classes(root: Path):
    for path in sorted(root.rglob('*.java')):
        yield path.relative_to(root).with_suffix('').as_posix(), path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--framework', required=True, type=Path)
    parser.add_argument('--extra', action='append', default=[], type=Path,
                        help='a source always compiled with the framework')
    parser.add_argument('--overlay', required=True, type=Path)
    parser.add_argument('--fork-root', type=Path, help="the layoutlib fork's bridge/src")
    parser.add_argument('--fork-class', action='append', default=[],
                        help='a class taken from the fork, as a/b/Name')
    parser.add_argument('--layoutlib', required=True, type=Path, help='extracted layoutlib classes')
    parser.add_argument('--keep', required=True, type=Path)
    parser.add_argument('--sources', required=True, type=Path)
    parser.add_argument('--replaced', required=True, type=Path)
    args = parser.parse_args()

    patterns, omits = [], []
    for line in args.keep.read_text(encoding='utf-8').splitlines():
        line = line.split('#', 1)[0].strip()
        if line.startswith('omit '):
            omits.append(re.compile(line[5:].strip()))
        elif line:
            patterns.append(re.compile(line))

    def in_layoutlib(name: str) -> bool:
        return (args.layoutlib / (name + '.class')).is_file()

    overlay = dict(java_classes(args.overlay))
    for name in args.fork_class:
        path = args.fork_root / (name + '.java')
        if name in overlay or not path.is_file():
            print(f'fork class {name}: missing, or also in the overlay', file=sys.stderr)
            return 1
        overlay[name] = path
    sources, replaced, unused = [], [], set(range(len(patterns)))
    for name, path in java_classes(args.framework):
        if name in overlay or any(omit.search(name) for omit in omits):
            continue
        if in_layoutlib(name):
            hits = [i for i, pattern in enumerate(patterns) if pattern.search(name)]
            if not hits:
                continue
            unused.difference_update(hits)
            replaced.append(name)
        sources.append(path)
    for name, path in overlay.items():
        if in_layoutlib(name):
            replaced.append(name)
        sources.append(path)
    sources.extend(args.extra)
    if unused:
        print('keep patterns matching nothing: ' + ', '.join(patterns[i].pattern for i in sorted(unused)),
              file=sys.stderr)
        return 1
    args.sources.parent.mkdir(parents=True, exist_ok=True)
    text = ''.join('"' + str(path.resolve()).replace('\\', '/') + '"\n' for path in sources)
    if not args.sources.is_file() or args.sources.read_text(encoding='utf-8') != text:
        args.sources.write_text(text, encoding='utf-8')
    text = ''.join(f'exclude {name.replace("/", ".")}\n' for name in sorted(replaced))
    if not args.replaced.is_file() or args.replaced.read_text(encoding='utf-8') != text:
        args.replaced.write_text(text, encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
