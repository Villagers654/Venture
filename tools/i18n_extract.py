#!/usr/bin/env python3
"""Writes the source catalog i18n/<domain>.pot from the strings marked in the
code. Developers: wrap user-facing literals in tr()/trf()/trn()... from
src/launcher/i18n/i18n.h (number arguments {0} {1}; a `// i18n:` comment above
becomes the translator note), then run this script and commit the POT.

  python tools/i18n_extract.py           rewrite the POT
  python tools/i18n_extract.py --check   fail when the POT is out of date

The check ignores the line numbers in references, so moving code around does
not make the committed POT stale; adding, changing or removing a marked string
or its translator comment does. Settings come from i18n/i18n.json.
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import i18n_po as po  # noqa: E402

# Marker -> (number of leading literal arguments, their roles, formats).
MARKERS = {
    "tr": (("msgid",), False),
    "trc": (("msgctxt", "msgid"), False),
    "trf": (("msgid",), True),
    "trcf": (("msgctxt", "msgid"), True),
    "trn": (("msgid", "msgid_plural"), True),
    "trnc": (("msgctxt", "msgid", "msgid_plural"), True),
    "mark": (("msgid",), False),
    "markc": (("msgctxt", "msgid"), False),
}
COMMENT_TAG = "i18n:"


@dataclass
class Token:
    kind: str  # "id", "str", "punct", "comment", "other"
    text: str  # identifier, punctuation, comment body, or the decoded string
    line: int
    plain: bool = True  # a string literal without an encoding prefix
    first_on_line: bool = False


_SIMPLE_ESCAPES = {"n": 10, "t": 9, "r": 13, "a": 7, "b": 8, "f": 12, "v": 11, "\\": 92, '"': 34, "'": 39, "?": 63}


def _decode(body: bytes, where: str) -> str | None:
    """The literal's text, or None when its bytes are not UTF-8 (binary data)."""
    out = bytearray()
    i = 0
    while i < len(body):
        c = body[i]
        if c != 0x5C:
            out.append(c)
            i += 1
            continue
        e = chr(body[i + 1])
        if e in _SIMPLE_ESCAPES:
            out.append(_SIMPLE_ESCAPES[e])
            i += 2
        elif e in "01234567":
            m = re.match(rb"[0-7]{1,3}", body[i + 1 :])
            out.append(int(m.group(0), 8) & 0xFF)
            i += 1 + len(m.group(0))
        elif e == "x":
            m = re.match(rb"[0-9a-fA-F]+", body[i + 2 :])
            out.append(int(m.group(0), 16) & 0xFF)
            i += 2 + len(m.group(0))
        elif e in "uU":
            width = 4 if e == "u" else 8
            out += chr(int(body[i + 2 : i + 2 + width], 16)).encode("utf-8")
            i += 2 + width
        else:
            raise SyntaxError(f"{where}: unknown escape \\{e}")
    try:
        return out.decode("utf-8")
    except UnicodeDecodeError:
        return None


_IDENT = re.compile(rb"[A-Za-z_]\w*")
_NUMBER = re.compile(rb"\.?\d(?:[eEpP][+-]|[\w.'])*")
_PREFIX = re.compile(rb"(u8|u|U|L)?(R)?\"")


def tokenize(data: bytes, name: str) -> list[Token]:
    """C++ tokens with comments kept: enough to find marker calls and their
    literal arguments, never matching inside strings or comments."""
    out: list[Token] = []
    i, line, n = 0, 1, len(data)
    line_has_token = False

    def add(tok: Token):
        nonlocal line_has_token
        tok.first_on_line = not line_has_token
        line_has_token = True
        out.append(tok)

    while i < n:
        c = data[i]
        if c == 0x0A:
            line += 1
            line_has_token = False
            i += 1
            continue
        if c in b" \t\r\f\v":
            i += 1
            continue
        if data.startswith(b"//", i):
            end = data.find(b"\n", i)
            end = n if end < 0 else end
            add(Token("comment", data[i + 2 : end].decode("utf-8", "replace").strip(), line))
            i = end
            continue
        if data.startswith(b"/*", i):
            end = data.find(b"*/", i + 2)
            end = n if end < 0 else end + 2
            add(Token("comment", "", line))
            line += data.count(b"\n", i, end)
            i = end
            continue
        m = _PREFIX.match(data, i)
        if m:
            start_line = line
            prefix, raw = m.group(1), m.group(2)
            j = m.end()
            if raw:
                paren = data.find(b"(", j)
                delim = data[j:paren]
                close = data.find(b")" + delim + b'"', paren)
                if paren < 0 or close < 0:
                    raise SyntaxError(f"{name}:{line}: unterminated raw string")
                text = data[paren + 1 : close].decode("utf-8", "replace")
                line += data.count(b"\n", i, close)
                i = close + len(delim) + 2
            else:
                k = j
                while k < n and data[k] != 0x22:
                    if data[k] == 0x5C:
                        k += 1
                    elif data[k] == 0x0A:
                        raise SyntaxError(f"{name}:{line}: unterminated string")
                    k += 1
                text = _decode(data[j:k], f"{name}:{line}")
                i = k + 1
            add(Token("str", text or "", start_line, plain=prefix is None and text is not None))
            continue
        if c == 0x27:  # character literal
            k = i + 1
            while k < n and data[k] != 0x27:
                k += 2 if data[k] == 0x5C else 1
            add(Token("other", "'", line))
            i = k + 1
            continue
        m = _NUMBER.match(data, i)
        if m and m.end() > i:
            add(Token("other", m.group(0).decode(), line))
            i = m.end()
            continue
        m = _IDENT.match(data, i)
        if m:
            add(Token("id", m.group(0).decode(), line))
            i = m.end()
            continue
        if data.startswith(b"::", i) or data.startswith(b"->", i):
            add(Token("punct", data[i : i + 2].decode(), line))
            i += 2
            continue
        add(Token("punct", chr(c), line))
        i += 1
    return out


@dataclass
class Found:
    msgctxt: str | None
    msgid: str
    msgid_plural: str | None
    is_format: bool
    reference: str
    comment: str | None


def _comment_above(tokens: list[Token], index: int, line: int) -> str | None:
    """The `// i18n:` comment block on the lines directly above `line`."""
    by_line: dict[int, str] = {}
    j = index - 1
    while j >= 0 and tokens[j].line >= line - 64:
        t = tokens[j]
        if t.kind == "comment" and t.first_on_line:
            by_line[t.line] = t.text
        j -= 1
    block: list[str] = []
    k = line - 1
    while k in by_line:
        block.insert(0, by_line[k])
        k -= 1
    for start, text in enumerate(block):
        if text.startswith(COMMENT_TAG):
            parts = [text[len(COMMENT_TAG) :].strip()] + [b.strip() for b in block[start + 1 :]]
            return " ".join(p for p in parts if p)
    return None


def extract_file(path: Path, rel: str, errors: list[str]) -> list[Found]:
    tokens = tokenize(path.read_bytes(), rel)
    found: list[Found] = []
    code = [t for t in tokens if t.kind != "comment"]
    positions = {id(t): i for i, t in enumerate(tokens)}
    for ci, t in enumerate(code):
        if t.kind != "id" or t.text not in MARKERS:
            continue
        if ci + 1 >= len(code) or code[ci + 1].text != "(":
            continue
        prev = code[ci - 1] if ci else None
        if prev is not None and prev.text in (".", "->"):
            continue
        if prev is not None and prev.text == "::" and not (ci >= 2 and code[ci - 2].text in ("i18n", "markers")):
            continue
        roles, is_format = MARKERS[t.text]
        values: dict[str, str] = {}
        k = ci + 2
        ok = True
        for role in roles:
            parts = []
            while k < len(code) and code[k].kind == "str":
                if not code[k].plain:
                    ok = False
                parts.append(code[k].text)
                k += 1
            if not parts or not ok:
                ok = False
                break
            values[role] = "".join(parts)
            if k >= len(code) or code[k].text not in (",", ")"):
                ok = False
                break
            k += 1
        if not ok:
            # Declarations (template <...> tr(const char (&)[N])) take no literal.
            if code[ci + 2].kind != "id" or code[ci + 2].text not in ("const", "char"):
                errors.append(f"{rel}:{t.line}: {t.text}() needs plain string literals")
            continue
        msgid = values["msgid"]
        if msgid == "":
            errors.append(f"{rel}:{t.line}: {t.text}() with an empty string")
            continue
        if is_format:
            for text in (msgid, values.get("msgid_plural")):
                if text is None:
                    continue
                indices = po.placeholders(text)
                if indices is None:
                    errors.append(f"{rel}:{t.line}: {text[:50]!r} is not a valid std::format pattern")
                elif len(indices) > 1 and re.search(r"\{(:[^}]*)?\}", text):
                    errors.append(f"{rel}:{t.line}: {text[:50]!r}: number the arguments ({{0}} {{1}}) so translations can reorder them")
        found.append(
            Found(
                msgctxt=values.get("msgctxt"),
                msgid=msgid,
                msgid_plural=values.get("msgid_plural"),
                is_format=is_format,
                reference=f"{rel}:{t.line}",
                comment=_comment_above(tokens, positions[id(t)], t.line),
            )
        )
    return found


def source_files(cfg: po.Config) -> list[tuple[Path, str]]:
    files: list[tuple[Path, str]] = []
    excluded = [(cfg.root / e).resolve() for e in cfg.exclude]
    for source in cfg.sources:
        base = cfg.root / source
        candidates = [base] if base.is_file() else sorted(p for p in base.rglob("*") if p.is_file())
        for p in candidates:
            if p.suffix not in cfg.extensions:
                continue
            r = p.resolve()
            if any(r == e or e in r.parents for e in excluded):
                continue
            files.append((p, p.relative_to(cfg.root).as_posix()))
    return sorted(set(files), key=lambda f: f[1])


HEADER_ORDER = ["Project-Id-Version", "Report-Msgid-Bugs-To", "Language", "MIME-Version", "Content-Type", "Content-Transfer-Encoding", "Plural-Forms"]


def build(cfg: po.Config) -> tuple[po.Catalog, list[str]]:
    errors: list[str] = []
    entries: dict[tuple, po.Entry] = {}
    for path, rel in source_files(cfg):
        try:
            found = extract_file(path, rel, errors)
        except (SyntaxError, UnicodeDecodeError) as e:
            errors.append(f"{rel}: {e}")
            continue
        for f in found:
            key = (f.msgctxt, f.msgid)
            e = entries.get(key)
            if e is None:
                e = entries[key] = po.Entry(msgid=f.msgid, msgctxt=f.msgctxt, msgid_plural=f.msgid_plural, msgstr=[""] if f.msgid_plural is None else ["", ""])
            elif e.msgid_plural != f.msgid_plural:
                errors.append(f"{f.reference}: {f.msgid[:50]!r} is used with different plural texts; add a context")
                continue
            if f.is_format and po.FORMAT_FLAG not in e.flags:
                e.flags.append(po.FORMAT_FLAG)
            if f.comment and f.comment not in e.comments:
                e.comments.append(f.comment)
            e.references.append(f.reference)
    header = {
        "Project-Id-Version": cfg.project,
        "Report-Msgid-Bugs-To": cfg.bugs,
        "Language": "",
        "MIME-Version": "1.0",
        "Content-Type": "text/plain; charset=UTF-8",
        "Content-Transfer-Encoding": "8bit",
        "Plural-Forms": "nplurals=INTEGER; plural=EXPRESSION;",
    }
    comments = [
        f"# {cfg.project} interface text. Generated by tools/i18n_extract.py; do not edit.",
        "# Translations are made on Crowdin; do not edit translations here.",
        "#",
        "#, fuzzy",
    ]
    return po.Catalog(header=header, entries=list(entries.values()), header_comments=comments), errors


def render(cfg: po.Config) -> tuple[str, list[str]]:
    catalog, errors = build(cfg)
    return po.write(catalog, HEADER_ORDER), errors


def without_lines(text: str) -> list[str]:
    return [re.sub(r":\d+$", "", row) if row.startswith("#: ") else row for row in text.splitlines()]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--check", action="store_true", help="compare with the committed POT instead of writing it")
    args = parser.parse_args(argv)
    cfg = po.load_config(args.root)
    text, errors = render(cfg)
    for e in errors:
        print(f"error: {e}", file=sys.stderr)
    if errors:
        return 1
    count = len(po.parse(text).entries)
    if args.check:
        current = cfg.pot.read_text(encoding="utf-8") if cfg.pot.exists() else ""
        if without_lines(current) != without_lines(text):
            diff = difflib.unified_diff(without_lines(current), without_lines(text), str(cfg.pot), "extracted", lineterm="", n=1)
            print("\n".join(list(diff)[:80]))
            print(f"{cfg.pot.relative_to(cfg.root).as_posix()} is out of date: run python tools/i18n_extract.py", file=sys.stderr)
            return 1
        print(f"{cfg.pot.relative_to(cfg.root).as_posix()} is up to date ({count} strings)")
        return 0
    cfg.pot.parent.mkdir(parents=True, exist_ok=True)
    with open(cfg.pot, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"wrote {cfg.pot.relative_to(cfg.root).as_posix()} ({count} strings)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
