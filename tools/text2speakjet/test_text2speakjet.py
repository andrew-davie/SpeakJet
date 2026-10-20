#!/usr/bin/env python3
# This file is part of the SpeakJet C-Library package.
# see https://github.com/andrew-davie/SpeakJet
#
# Copyright (c) 2026 Andrew Davie
# SPDX-License-Identifier: MIT
"""Tests for text2speakjet.py. Run: python3 tools/text2speakjet/test_text2speakjet.py"""

import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "text2speakjet.py")
sys.path.insert(0, HERE)
import text2speakjet as t2s  # noqa: E402
import convert_phrasealator as cpa  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
CMU = "--no-phrasealator"


def run(*args, stdin=None):
    return subprocess.run([sys.executable, TOOL] + list(args), input=stdin,
                          capture_output=True, text=stdin is None or isinstance(stdin, str))


def dec(text, *extra):
    r = run("--format", "dec", "--terminator", "none", *extra, text)
    assert r.returncode == 0, r.stderr
    return [int(c) for c in r.stdout.replace(",", " ").split()]


class CodeTable(unittest.TestCase):
    def test_codes_match_the_manual(self):
        self.assertEqual(t2s.CODE["IY"], 128)
        self.assertEqual(t2s.CODE["PO"], 199)
        self.assertEqual(t2s.CODE["R0"], 200)
        self.assertEqual(t2s.CODE["D11"], 251)
        self.assertEqual(t2s.CODE["M2"], 254)


class Words(unittest.TestCase):
    def test_dictionary_word(self):
        # HO FAST AX LO STRESS OWWW
        self.assertEqual(dec("hello", CMU), [184, 7, 133, 146, 14, 164])

    def test_word_gap_and_punctuation(self):
        codes = dec("hello, hello")
        self.assertIn(2, codes)                 # comma: 200ms pause
        self.assertNotEqual(codes[-1], 2)       # no pause at the very end

    def test_no_fast(self):
        self.assertNotIn(7, dec("hello", "--no-fast", CMU))

    def test_exceptions_file(self):
        r = run("atarivox")
        self.assertEqual(r.returncode, 0)
        self.assertNotIn("not in the dictionary", r.stderr)

    def test_unknown_word_warns(self):
        r = run("fnorbix")
        self.assertEqual(r.returncode, 0)
        self.assertIn("'fnorbix' not in the dictionary", r.stderr)

    def test_compound(self):
        r = run("moonbase")
        self.assertIn("moon + base", r.stderr)


class Numbers(unittest.TestCase):
    def test_expand(self):
        self.assertEqual(t2s.expand_number("42"), ["forty", "two"])
        self.assertEqual(t2s.expand_number("3.5"), ["three", "point", "five"])
        self.assertEqual(t2s.expand_number("1066"), ["one", "thousand", "sixty", "six"])

    def test_spoken(self):
        self.assertEqual(dec("42"), dec("forty two"))


class Markup(unittest.TestCase):
    def test_raw_codes(self):
        self.assertEqual(t2s.parse_codes("OHIY TT"), [155, 191])
        self.assertEqual(t2s.parse_codes("M2 200"), [254, 200])
        self.assertEqual(t2s.parse_codes("pitch 100"), [22, 100])

    def test_bad_markup(self):
        for bad in ("{BOGUS}", "[pitch 300]", "{255}", "(XX1)"):
            r = run(bad)
            self.assertEqual(r.returncode, 1, bad)

    def test_reset_after_persistent_controls(self):
        codes = dec("[pitch 100] hello")
        self.assertEqual(codes[:2], [22, 100])
        self.assertEqual(codes[-1], 31)
        self.assertNotEqual(dec("[pitch 100] hello", "--no-reset")[-1], 31)

    def test_inline_arpabet(self):
        self.assertEqual(dec("(HH AH0 L OW1)"), dec("hello", CMU))


class PhraseALator(unittest.TestCase):
    def test_used_by_default(self):
        # HE FAST EHLL LO OWWW, Magnevation's spelling
        self.assertEqual(dec("hello"), [183, 7, 159, 146, 164])
        self.assertIn("[phrasealator]", run("-v", "hello").stderr)

    def test_weak_forms_come_first(self):
        # PhraseALator's "a" is the letter name; in a sentence it's the weak form
        self.assertIn("[weak form]", run("-v", "a").stderr)

    def test_exceptions_come_first(self):
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write("hello {MM}\n")
        try:
            self.assertEqual(dec("hello", "--dict", f.name), [140])
        finally:
            os.unlink(f.name)

    def test_works_without_it(self):
        lex = t2s.Lexicon([], None)
        self.assertEqual(lex.lookup("hello")[2], "cmudict")

    def test_conversion(self):
        words, skipped = cpa.convert([
            "[words]",
            "well = \\WW \\Fast \\EHLE ",
            "bird=\\FAST \\SOFT \\BE \\AXRR \\ED",
            "fxtest=\\Reset testing,, one",
            "x-ray= \\EH \\EK \\SE",
        ])
        self.assertEqual(words, [("well", "WW FAST EHLL"), ("bird", "FAST BE AXRR ED")])
        self.assertEqual(skipped, ["fxtest", "x-ray"])

    def test_converted_file_is_up_to_date(self):
        src = os.path.join(ROOT, "docs", "reference", "PhraseALator.Dic")
        committed = os.path.join(HERE, "phrasealator.txt")
        if not (os.path.exists(src) and os.path.exists(committed)):
            self.skipTest("PhraseALator files not present")
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "phrasealator.txt")
            subprocess.run([sys.executable, os.path.join(HERE, "convert_phrasealator.py"), src, out],
                           check=True, capture_output=True)
            with open(out, encoding="utf-8") as a, open(committed, encoding="utf-8") as b:
                self.assertEqual(a.read(), b.read(), "rerun convert_phrasealator.py")


class Formats(unittest.TestCase):
    def test_formats_agree(self):
        codes = dec("hello world")
        r = run("--format", "bin", "--terminator", "none", "hello world", stdin=b"")
        self.assertEqual(list(r.stdout), codes)
        c = run("--format", "c", "-n", "hi", "hello world").stdout
        self.assertIn("const unsigned char hi[] = {", c)
        asm = run("-n", "hi", "hello world").stdout
        self.assertIn("hi\n", asm)
        self.assertIn(".byte $FF", asm)

    def test_terminator(self):
        r = run("--format", "dec", "hello")
        self.assertTrue(r.stdout.strip().endswith("255"))

    def test_phrase_file_on_stdin(self):
        r = run("--format", "c", stdin="first: hello\n# comment\nsecond: world\n")
        self.assertIn("first[]", r.stdout)
        self.assertIn("second[]", r.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=1)
