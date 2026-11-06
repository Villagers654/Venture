"""gettext PO/POT reading and writing, std::format placeholder checks and
plural-expression parsing for the i18n tools (i18n_extract.py, i18n_compile.py).

Standard library only; no project paths. Projects describe themselves in
i18n/i18n.json (see load_config).
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path

FORMAT_FLAG = "python-brace-format"  # std::format's {0} syntax; gettext tools know it by this name


# --- Config ---------------------------------------------------------------------
@dataclass
class Config:
    root: Path
    domain: str  # the POT is i18n/<domain>.pot
    project: str  # Project-Id-Version in the POT header
    sources: list[str]  # directories (or files) scanned for marked strings, relative to root
    exclude: list[str]  # paths under sources that are skipped
    extensions: list[str]
    bugs: str = ""  # Report-Msgid-Bugs-To

    @property
    def directory(self) -> Path:
        return self.root / "i18n"

    @property
    def pot(self) -> Path:
        return self.directory / f"{self.domain}.pot"

    def po_files(self) -> list[Path]:
        return sorted(self.directory.glob("*.po"))


def load_config(root: Path) -> Config:
    data = json.loads((root / "i18n" / "i18n.json").read_text(encoding="utf-8"))
    return Config(
        root=root,
        domain=data["domain"],
        project=data.get("project", data["domain"]),
        sources=list(data["sources"]),
        exclude=list(data.get("exclude", [])),
        extensions=list(data.get("extensions", [".cpp", ".h", ".hpp", ".cc"])),
        bugs=data.get("bugs", ""),
    )


# --- PO model ----------------------------------------------------------------------
@dataclass
class Entry:
    msgid: str
    msgctxt: str | None = None
    msgid_plural: str | None = None
    msgstr: list[str] = field(default_factory=lambda: [""])
    comments: list[str] = field(default_factory=list)  # "#." extracted (translator) comments
    references: list[str] = field(default_factory=list)  # "#:" file:line
    flags: list[str] = field(default_factory=list)  # "#,"
    line: int = 0  # where the entry starts in its file
    prelude: list[str] = field(default_factory=list)  # every comment line as written

    @property
    def key(self) -> tuple[str | None, str]:
        return (self.msgctxt, self.msgid)

    @property
    def is_format(self) -> bool:
        return FORMAT_FLAG in self.flags

    @property
    def fuzzy(self) -> bool:
        return "fuzzy" in self.flags

    @property
    def translated(self) -> bool:
        return not self.fuzzy and bool(self.msgstr) and all(self.msgstr)


@dataclass
class Catalog:
    header: dict[str, str]
    entries: list[Entry]
    header_comments: list[str] = field(default_factory=list)

    def by_key(self) -> dict[tuple[str | None, str], Entry]:
        return {e.key: e for e in self.entries}


class PoError(ValueError):
    pass


_ESCAPES = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "a": "\a", "b": "\b", "f": "\f", "v": "\v"}


def _unquote(text: str, where: str) -> str:
    text = text.strip()
    if len(text) < 2 or text[0] != '"' or text[-1] != '"':
        raise PoError(f"{where}: expected a quoted string")
    body, out, i = text[1:-1], [], 0
    while i < len(body):
        c = body[i]
        if c == '"':
            raise PoError(f"{where}: unescaped quote")
        if c != "\\":
            out.append(c)
            i += 1
            continue
        if i + 1 >= len(body):
            raise PoError(f"{where}: dangling backslash")
        e = body[i + 1]
        if e in _ESCAPES:
            out.append(_ESCAPES[e])
            i += 2
        elif e in "01234567":
            m = re.match(r"[0-7]{1,3}", body[i + 1 :])
            out.append(chr(int(m.group(0), 8)))
            i += 1 + len(m.group(0))
        elif e == "x":
            m = re.match(r"[0-9a-fA-F]+", body[i + 2 :])
            if not m:
                raise PoError(f"{where}: bad \\x escape")
            out.append(chr(int(m.group(0), 16)))
            i += 2 + len(m.group(0))
        else:
            raise PoError(f"{where}: unknown escape \\{e}")
    return "".join(out)


def parse(text: str, name: str = "<po>") -> Catalog:
    """Reads a PO or POT file. Obsolete (#~) entries are dropped."""
    if text.startswith("\ufeff"):
        text = text[1:]
    entries: list[Entry] = []
    cur: Entry | None = None
    pending: dict = {}
    last: tuple | None = None  # (field, index) continuation target
    start = 0

    def finish():
        nonlocal cur, pending, last
        if pending.get("msgid") is not None:
            msgstr = pending.get("msgstr", {})
            if pending.get("msgid_plural") is None:
                if set(msgstr) != {0}:
                    raise PoError(f"{name}:{start}: msgid without a single msgstr")
                strings = [msgstr[0]]
            else:
                if not msgstr or sorted(msgstr) != list(range(len(msgstr))):
                    raise PoError(f"{name}:{start}: plural msgstr[] indices are not 0..n-1")
                strings = [msgstr[i] for i in range(len(msgstr))]
            entries.append(
                Entry(
                    msgid=pending["msgid"],
                    msgctxt=pending.get("msgctxt"),
                    msgid_plural=pending.get("msgid_plural"),
                    msgstr=strings,
                    comments=pending.get("comments", []),
                    references=pending.get("references", []),
                    flags=pending.get("flags", []),
                    line=start,
                    prelude=pending.get("prelude", []),
                )
            )
        elif any(k in pending for k in ("msgctxt", "msgid_plural", "msgstr")):
            raise PoError(f"{name}:{start}: entry without msgid")
        pending = {}
        last = None

    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        where = f"{name}:{number}"
        if not line:
            if pending:
                finish()
            continue
        if line.startswith("#~"):
            continue  # obsolete
        if line.startswith("#"):
            if pending.get("msgid") is not None:
                finish()
            if not pending:
                start = number
            pending.setdefault("prelude", []).append(line)
            kind, rest = line[:2], line[2:].strip()
            if kind == "#.":
                pending.setdefault("comments", []).append(rest)
            elif kind == "#:":
                pending.setdefault("references", []).extend(rest.split())
            elif kind == "#,":
                pending.setdefault("flags", []).extend(f.strip() for f in rest.split(",") if f.strip())
            continue
        if line.startswith('"'):
            if last is None:
                raise PoError(f"{where}: continuation line without a keyword")
            value = _unquote(line, where)
            if last[0] == "msgstr":
                pending["msgstr"][last[1]] += value
            else:
                pending[last[0]] += value
            continue
        m = re.match(r"(msgctxt|msgid_plural|msgid|msgstr(?:\[(\d+)\])?)\s+(.*)$", line)
        if not m:
            raise PoError(f"{where}: cannot read {line[:40]!r}")
        keyword, index, value = m.group(1), m.group(2), _unquote(m.group(3), where)
        if keyword in ("msgctxt", "msgid") and pending.get("msgid") is not None and "msgstr" in pending:
            finish()
        if not pending:
            start = number
        if keyword.startswith("msgstr"):
            i = int(index) if index is not None else 0
            if (index is None) == (pending.get("msgid_plural") is not None):
                raise PoError(f"{where}: {keyword} does not match the entry's plural form")
            if i in pending.setdefault("msgstr", {}):
                raise PoError(f"{where}: duplicate {keyword}")
            pending["msgstr"][i] = value
            last = ("msgstr", i)
        else:
            if keyword in pending:
                raise PoError(f"{where}: duplicate {keyword}")
            if keyword != "msgctxt" and "msgid" not in pending and keyword == "msgid_plural":
                raise PoError(f"{where}: msgid_plural before msgid")
            pending[keyword] = value
            last = (keyword, None)
    if pending:
        finish()

    header: dict[str, str] = {}
    header_comments: list[str] = []
    body: list[Entry] = []
    seen: set = set()
    for e in entries:
        if e.msgid == "" and e.msgctxt is None:
            header_comments = e.prelude
            for row in e.msgstr[0].split("\n"):
                if ":" in row:
                    k, v = row.split(":", 1)
                    header[k.strip()] = v.strip()
            continue
        if e.key in seen:
            raise PoError(f"{name}:{e.line}: duplicate entry {e.msgid[:40]!r}")
        seen.add(e.key)
        body.append(e)
    return Catalog(header=header, entries=body, header_comments=header_comments)


def read(path: Path) -> Catalog:
    return parse(path.read_text(encoding="utf-8"), str(path))


def _quote(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t").replace("\r", "\\r").replace("\n", "\\n") + '"'


def _field(keyword: str, value: str) -> list[str]:
    # Text with line breaks is written one line per row, as xgettext does.
    if "\n" in value.rstrip("\n"):
        parts = re.findall(r"[^\n]*\n|[^\n]+$", value)
        return [f'{keyword} ""'] + [_quote(p) for p in parts]
    return [f"{keyword} {_quote(value)}"]


def write(catalog: Catalog, header_order: list[str] | None = None) -> str:
    out: list[str] = list(catalog.header_comments)
    keys = header_order or list(catalog.header)
    header_text = "".join(f"{k}: {catalog.header[k]}\n" for k in keys if k in catalog.header)
    out += ['msgid ""', 'msgstr ""'] + [_quote(row) for row in re.findall(r"[^\n]*\n", header_text)]
    for e in catalog.entries:
        out.append("")
        out += [f"#. {c}" if c else "#." for c in e.comments]
        out += [f"#: {r}" for r in e.references]
        if e.flags:
            out.append("#, " + ", ".join(e.flags))
        if e.msgctxt is not None:
            out += _field("msgctxt", e.msgctxt)
        out += _field("msgid", e.msgid)
        if e.msgid_plural is not None:
            out += _field("msgid_plural", e.msgid_plural)
            for i, s in enumerate(e.msgstr):
                out += _field(f"msgstr[{i}]", s)
        else:
            out += _field("msgstr", e.msgstr[0])
    return "\n".join(out) + "\n"


# --- std::format placeholders ---------------------------------------------------------
def placeholders(pattern: str) -> list[int] | None:
    """Argument indices of a std::format pattern in order ("{}" counts from 0),
    or None when it is malformed or mixes automatic and manual indexing."""
    out: list[int] = []
    auto = manual = False
    nxt, i = 0, 0
    while i < len(pattern):
        c = pattern[i]
        if c == "}":
            if pattern[i + 1 : i + 2] == "}":
                i += 2
                continue
            return None
        if c != "{":
            i += 1
            continue
        if pattern[i + 1 : i + 2] == "{":
            i += 2
            continue
        close = pattern.find("}", i)
        if close < 0:
            return None
        body = pattern[i + 1 : close]
        if "{" in body:
            return None
        index = body.split(":", 1)[0]
        if index == "":
            auto = True
            out.append(nxt)
            nxt += 1
        elif index.isdigit() and len(index) <= 2:
            manual = True
            out.append(int(index))
        else:
            return None
        i = close + 1
    return None if auto and manual else out


def placeholder_problem(source: str, translation: str, plural_form: bool = False) -> str | None:
    """Why `translation` cannot stand in for `source`, or None."""
    want, got = placeholders(source), placeholders(translation)
    if want is None:
        return "the source is not a valid format pattern"
    if got is None:
        return "unbalanced braces or mixed {} and {0} (write {{ and }} for literal braces)"
    want_set, got_set = set(want), set(got)
    if plural_form and 0 in want_set and 0 not in got_set:
        want_set.discard(0)  # "one game": a form may spell out the count
    if want_set != got_set:
        missing = sorted(want_set - got_set)
        extra = sorted(got_set - want_set)
        parts = []
        if missing:
            parts.append("missing " + " ".join(f"{{{i}}}" for i in missing))
        if extra:
            parts.append("unknown " + " ".join(f"{{{i}}}" for i in extra))
        return ", ".join(parts)
    return None


# --- Plural-Forms ---------------------------------------------------------------------
class PluralError(ValueError):
    pass


_TOKEN = re.compile(r"\s*(?:(\d+)|(n)|(\|\||&&|==|!=|<=|>=|[-+*/%<>!?:()]))")


def _tokens(expr: str) -> list[str]:
    out, pos = [], 0
    expr = expr.strip()
    while pos < len(expr):
        m = _TOKEN.match(expr, pos)
        if not m or m.end() == pos:
            raise PluralError(f"unexpected {expr[pos:pos + 10]!r}")
        out.append(m.group(1) or m.group(2) or m.group(3))
        pos = m.end()
        while pos < len(expr) and expr[pos].isspace():
            pos += 1
    return out


class _Parser:
    # C precedence: ?: < || < && < == != < relational < + - < * / % < !
    def __init__(self, tokens: list[str]):
        self.t, self.i = tokens, 0

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else None

    def take(self, want=None):
        tok = self.peek()
        if tok is None or (want is not None and tok != want):
            raise PluralError(f"expected {want or 'more'} at token {self.i}")
        self.i += 1
        return tok

    def cond(self):
        c = self.binary(0)
        if self.peek() == "?":
            self.take("?")
            a = self.cond()
            self.take(":")
            b = self.cond()
            return ("?", c, a, b)
        return c

    LEVELS = [("||",), ("&&",), ("==", "!="), ("<", "<=", ">", ">="), ("+", "-"), ("*", "/", "%")]

    def binary(self, level):
        if level == len(self.LEVELS):
            return self.unary()
        left = self.binary(level + 1)
        while self.peek() in self.LEVELS[level]:
            op = self.take()
            right = self.binary(level + 1)
            if op in ("/", "%") and not (right[0] == "int" and right[1] != 0):
                raise PluralError("division only by a non-zero number")
            left = (op, left, right)
        return left

    def unary(self):
        if self.peek() == "!":
            self.take()
            return ("!", self.unary())
        tok = self.take()
        if tok == "(":
            inner = self.cond()
            self.take(")")
            return inner
        if tok == "n":
            return ("n",)
        if tok.isdigit():
            return ("int", int(tok))
        raise PluralError(f"unexpected {tok!r}")


_MASK = (1 << 64) - 1


def _eval(node, n: int) -> int:
    op = node[0]
    if op == "n":
        return n
    if op == "int":
        return node[1]
    if op == "!":
        return int(not _eval(node[1], n))
    if op == "?":
        return _eval(node[2], n) if _eval(node[1], n) else _eval(node[3], n)
    a = _eval(node[1], n)
    if op == "||":
        return int(bool(a) or bool(_eval(node[2], n)))
    if op == "&&":
        return int(bool(a) and bool(_eval(node[2], n)))
    b = _eval(node[2], n)
    return {
        "==": lambda: int(a == b),
        "!=": lambda: int(a != b),
        "<": lambda: int(a < b),
        "<=": lambda: int(a <= b),
        ">": lambda: int(a > b),
        ">=": lambda: int(a >= b),
        "+": lambda: (a + b) & _MASK,
        "-": lambda: (a - b) & _MASK,
        "*": lambda: (a * b) & _MASK,
        "/": lambda: a // b,
        "%": lambda: a % b,
    }[op]()


def _cpp(node) -> str:
    op = node[0]
    if op == "n":
        return "n"
    if op == "int":
        return f"{node[1]}ULL"
    if op == "!":
        return f"(!{_cpp(node[1])})"
    if op == "?":
        return f"({_cpp(node[1])} ? {_cpp(node[2])} : {_cpp(node[3])})"
    # Comparisons and logic are bool in C++; arithmetic on them promotes to int.
    return f"({_cpp(node[1])} {op} {_cpp(node[2])})"


@dataclass
class PluralForms:
    count: int
    tree: tuple

    def form(self, n: int) -> int:
        return _eval(self.tree, n)

    def cpp(self) -> str:
        """A C++ expression of `unsigned long long n`."""
        return _cpp(self.tree)


def parse_plural_forms(header_value: str | None) -> PluralForms:
    """Parses "nplurals=N; plural=EXPR;" and checks every n picks a form < N."""
    if not header_value:
        raise PluralError("no Plural-Forms header")
    m = re.fullmatch(r"\s*nplurals\s*=\s*(\d+)\s*;\s*plural\s*=\s*(.+?)\s*;?\s*", header_value)
    if not m:
        raise PluralError(f"cannot read {header_value!r}")
    count = int(m.group(1))
    if not 1 <= count <= 6:
        raise PluralError(f"nplurals={count} is out of range")
    parser = _Parser(_tokens(m.group(2)))
    tree = parser.cond()
    if parser.peek() is not None:
        raise PluralError(f"unexpected {parser.peek()!r} after the expression")
    forms = PluralForms(count, tree)
    for n in list(range(0, 1201)) + [10**k + d for k in range(4, 19) for d in (0, 1, 2, 5, 11, 21)]:
        f = forms.form(n)
        if not 0 <= f < count:
            raise PluralError(f"n={n} selects form {f} of {count}")
    return forms


# --- Checking a translation catalog against the source ----------------------------------
def check_catalog(po: Catalog, pot: Catalog, name: str) -> tuple[list[str], list[str], dict]:
    """Errors and warnings for a translation, and what compile() needs:
    {"forms": PluralForms|None, "valid": [Entry...], "translated": int, "total": int}."""
    errors: list[str] = []
    warnings: list[str] = []
    try:
        forms = parse_plural_forms(po.header.get("Plural-Forms"))
    except PluralError as e:
        forms = None
        errors.append(f"{name}: Plural-Forms: {e}")
    source = pot.by_key()
    valid: list[Entry] = []
    obsolete = 0
    for e in po.entries:
        src = source.get(e.key)
        if src is None:
            obsolete += 1
            continue
        if not e.translated:
            continue
        where = f"{name}:{e.line}"
        if (src.msgid_plural is None) != (e.msgid_plural is None):
            errors.append(f"{where}: {e.msgid[:50]!r} is {'plural' if src.msgid_plural else 'singular'} in the source")
            continue
        problem = None
        if src.msgid_plural is not None:
            if forms and len(e.msgstr) != forms.count:
                problem = f"has {len(e.msgstr)} plural forms, Plural-Forms says {forms.count}"
            else:
                for i, s in enumerate(e.msgstr):
                    p = placeholder_problem(src.msgid_plural, s, plural_form=True)
                    if p:
                        problem = f"form {i}: {p}"
                        break
        elif src.is_format:
            problem = placeholder_problem(src.msgid, e.msgstr[0])
        if problem:
            errors.append(f"{where}: {e.msgid[:50]!r}: {problem}")
            continue
        if src.msgid_plural is not None and forms is None:
            continue
        valid.append(e)
    if obsolete:
        warnings.append(f"{name}: {obsolete} entries are not in the source catalog (obsolete; ignored)")
    return errors, warnings, {"forms": forms, "valid": valid, "translated": len(valid), "total": len(pot.entries)}
