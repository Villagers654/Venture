"""The translation tools: PO reading/writing, extraction from C++ sources,
placeholder and plural checks, and the generated catalog source."""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import i18n_compile  # noqa: E402
import i18n_extract  # noqa: E402
import i18n_po as po  # noqa: E402

SOURCE = r'''
#include "i18n.h"
// A comment that mentions tr("Not extracted") stays a comment.
/* Nor tr("this") */
void f(int count, std::string name) {
  const char* s = "tr(\"not a call\")";
  // Ordinary note.
  // i18n: Sidebar page.
  // Keep it short.
  draw(tr("Library"));
  draw(tr("Library"));
  draw(trc("verb", "Open"));
  draw(trf("Copy {0} to {1}", a, b));
  draw(trn("{0} game", "{0} games", count));
  draw(trnc("store", "{0} item", "{0} items", count));
  draw(i18n::tr("Split " "literal\n"));
  draw(tr(R"x(Raw "text")x"));
  static const char* kItems[] = {mark("Automatic"), markc("rate", "Off")};
  obj.tr("member, ignored");
  other::tr("other namespace, ignored");
  int x = 1'000;
  char c = '"';
  draw(tr("Caf\303\251 é"));
}
'''

POT_HEADER = 'msgid ""\nmsgstr ""\n"Content-Type: text/plain; charset=UTF-8\\n"\n'


def project(files, po_files=None, min_completion=0.25):
    tmp = tempfile.TemporaryDirectory()
    root = Path(tmp.name)
    (root / "i18n").mkdir()
    (root / "src").mkdir()
    (root / "i18n" / "i18n.json").write_text(json.dumps({"domain": "test", "project": "Test", "sources": ["src"], "exclude": ["src/i18n"], "min_completion": min_completion}))
    for name, text in files.items():
        (root / "src" / name).write_text(text, encoding="utf-8")
    for name, text in (po_files or {}).items():
        (root / "i18n" / name).write_text(text, encoding="utf-8")
    return tmp, root


class ExtractTest(unittest.TestCase):
    def setUp(self):
        self.tmp, self.root = project({"a.cpp": SOURCE})
        self.cfg = po.load_config(self.root)

    def tearDown(self):
        self.tmp.cleanup()

    def test_markers_and_comments(self):
        catalog, errors = i18n_extract.build(self.cfg)
        self.assertEqual(errors, [])
        by = catalog.by_key()
        self.assertEqual(
            sorted(k[1] for k in by),
            sorted(["Library", "Open", "Copy {0} to {1}", "{0} game", "{0} item", "Split literal\n", 'Raw "text"', "Automatic", "Off", "Café é"]),
        )
        library = by[(None, "Library")]
        self.assertEqual(library.comments, ["Sidebar page. Keep it short."])
        self.assertEqual(library.references, ["src/a.cpp:10", "src/a.cpp:11"])
        self.assertEqual(by[("verb", "Open")].flags, [])
        self.assertEqual(by[(None, "Copy {0} to {1}")].flags, [po.FORMAT_FLAG])
        game = by[(None, "{0} game")]
        self.assertEqual((game.msgid_plural, game.msgstr), ("{0} games", ["", ""]))
        self.assertEqual(by[("store", "{0} item")].msgid_plural, "{0} items")
        self.assertIn(("rate", "Off"), by)

    def test_round_trip_and_check_ignores_line_numbers(self):
        text, _ = i18n_extract.render(self.cfg)
        self.assertEqual(po.write(po.parse(text), i18n_extract.HEADER_ORDER), text)
        self.cfg.pot.write_text(text, encoding="utf-8")
        self.assertEqual(i18n_extract.main(["--root", str(self.root), "--check"]), 0)
        moved = (self.root / "src" / "a.cpp").read_text().replace("void f(", "\n\nvoid f(")
        (self.root / "src" / "a.cpp").write_text(moved)
        self.assertEqual(i18n_extract.main(["--root", str(self.root), "--check"]), 0)
        (self.root / "src" / "a.cpp").write_text(moved.replace('tr("Library")', 'tr("Libraries")'))
        self.assertEqual(i18n_extract.main(["--root", str(self.root), "--check"]), 1)

    def test_errors(self):
        (self.root / "src" / "b.cpp").write_text('void g(const char* v) { tr(v); trf("{} of {}", 1, 2); trf("{0", 1); tr(""); }\n')
        _, errors = i18n_extract.build(self.cfg)
        self.assertEqual(len(errors), 4, errors)
        self.assertIn("plain string literals", errors[0])
        self.assertIn("number the arguments", errors[1])
        self.assertIn("not a valid std::format pattern", errors[2])
        self.assertIn("empty string", errors[3])


class PoTest(unittest.TestCase):
    def test_parse(self):
        text = POT_HEADER + '\n#. note\n#: a.cpp:1\n#, fuzzy, python-brace-format\nmsgctxt "c"\nmsgid ""\n"two "\n"lines"\nmsgstr "x\\ty"\n\n#~ msgid "old"\n#~ msgstr "alt"\n'
        catalog = po.parse(text)
        self.assertEqual(catalog.header["Content-Type"], "text/plain; charset=UTF-8")
        (e,) = catalog.entries
        self.assertEqual((e.msgctxt, e.msgid, e.msgstr, e.comments, e.flags), ("c", "two lines", ["x\ty"], ["note"], ["fuzzy", "python-brace-format"]))
        self.assertFalse(e.translated)  # fuzzy

    def test_malformed(self):
        for bad in ['msgid "a"\n', 'msgid "a"\nmsgstr[0] "b"\n', 'msgid "a"\nmsgid_plural "b"\nmsgstr "c"\n', 'msgid "a\nmsgstr ""\n',
                    'msgid "a"\nmsgstr "b"\nmsgid "a"\nmsgstr "c"\n']:
            with self.assertRaises(po.PoError, msg=bad):
                po.parse(bad)

    def test_placeholders(self):
        self.assertEqual(po.placeholders("{0} of {1:>3} {{x}}"), [0, 1])
        self.assertEqual(po.placeholders("{} {}"), [0, 1])
        self.assertIsNone(po.placeholders("{} {1}"))
        self.assertIsNone(po.placeholders("}"))
        self.assertIsNone(po.placeholders("{name}"))
        self.assertIsNone(po.placeholder_problem("{0} to {1}", "{1} vers {0}"))
        self.assertIn("missing {1}", po.placeholder_problem("{0} to {1}", "{0}"))
        self.assertIn("unknown {2}", po.placeholder_problem("{0}", "{0} {2}"))
        self.assertIsNone(po.placeholder_problem("{0} games", "un jeu", plural_form=True))
        self.assertIsNotNone(po.placeholder_problem("{0} in {1}", "un jeu", plural_form=True))

    def test_plural_forms(self):
        ru = po.parse_plural_forms("nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);")
        self.assertEqual([ru.form(n) for n in (1, 2, 5, 11, 21, 22, 25, 111)], [0, 1, 2, 2, 0, 1, 2, 2])
        self.assertEqual(po.parse_plural_forms("nplurals=1; plural=0;").form(7), 0)
        fr = po.parse_plural_forms("nplurals=2; plural=(n > 1);")
        self.assertEqual(fr.cpp(), "(n > 1ULL)")
        for bad in ["nplurals=2; plural=n;", "nplurals=2; plural=system(1);", "nplurals=2; plural=(n/0);", "nplurals=0; plural=0;", "", "nplurals=2; plural=n>1 ? 1 : 2;"]:
            with self.assertRaises(po.PluralError, msg=bad):
                po.parse_plural_forms(bad)


FR = """msgid ""
msgstr ""
"Language: fr\\n"
"Plural-Forms: nplurals=2; plural=(n > 1);\\n"

msgid "Library"
msgstr "Bibliothèque"

msgid "Copy {0} to {1}"
msgstr "Copier {0} vers {1}"

msgid "{0} game"
msgid_plural "{0} games"
msgstr[0] "{0} jeu"
msgstr[1] "{0} jeux"

msgid "Gone"
msgstr "Parti"
"""


class CompileTest(unittest.TestCase):
    def test_check_and_generate(self):
        tmp, root = project({"a.cpp": SOURCE}, {"fr.po": FR})
        with tmp:
            self.assertEqual(i18n_extract.main(["--root", str(root)]), 0)
            self.assertEqual(i18n_compile.main(["--root", str(root), "--check"]), 0)
            out = root / "out.cpp"
            self.assertEqual(i18n_compile.main(["--root", str(root), "--out", str(out)]), 0)
            text = out.read_text(encoding="utf-8")
            self.assertIn('{"Library"sv, {}, "Biblioth\\303\\250que"sv, false}', text)
            self.assertIn('"{0} jeu\\000{0} jeux"sv, true}', text)
            self.assertIn('"Fran\\303\\247ais"sv, 0.300f}', text)  # 3 of 10
            self.assertNotIn("Parti", text)  # not in the source catalog

    def test_bad_translations_fail_the_check_but_not_the_build(self):
        bad = FR.replace('"Copier {0} vers {1}"', '"Copier {0}"').replace('msgstr[1] "{0} jeux"\n', "")
        tmp, root = project({"a.cpp": SOURCE}, {"fr.po": bad})
        with tmp:
            i18n_extract.main(["--root", str(root)])
            self.assertEqual(i18n_compile.main(["--root", str(root), "--check"]), 1)
            out = root / "out.cpp"
            self.assertEqual(i18n_compile.main(["--root", str(root), "--out", str(out), "--min-completion", "0"]), 0)
            text = out.read_text(encoding="utf-8")
            self.assertIn("Biblioth", text)
            self.assertNotIn("Copier", text)

    def test_below_minimum_is_left_out(self):
        tmp, root = project({"a.cpp": SOURCE}, {"fr.po": FR}, min_completion=0.9)
        with tmp:
            i18n_extract.main(["--root", str(root)])
            out = root / "out.cpp"
            i18n_compile.main(["--root", str(root), "--out", str(out)])
            self.assertIn("catalogs() { return {}; }", out.read_text(encoding="utf-8"))


class RepositoryTest(unittest.TestCase):
    def test_this_repository(self):
        root = Path(__file__).resolve().parents[1]
        self.assertEqual(i18n_extract.main(["--root", str(root), "--check"]), 0)
        self.assertEqual(i18n_compile.main(["--root", str(root), "--check"]), 0)


if __name__ == "__main__":
    unittest.main()
