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

    def test_thousands_separators(self):
        self.assertEqual(t2s.expand_number("1,000,000"), ["one", "million"])
        self.assertEqual(t2s.expand_number("2,500.75"),
                         ["two", "thousand", "five", "hundred", "point", "seven", "five"])
        self.assertEqual(dec("1,000,000"), dec("one million"))
        # a list stays a list: only groups of exactly three digits join
        self.assertEqual(dec("1, 2, 3"), dec("one, two, three"))

    def test_dotted_numbers(self):
        self.assertEqual(t2s.expand_number("192.168.1.1"),
                         "one nine two dot one six eight dot one dot one".split())
        self.assertEqual(dec("version 1.2.3"), dec("version one dot two dot three"))
        # one dot is still a decimal point
        self.assertEqual(t2s.expand_number("3.5"), ["three", "point", "five"])

    def test_leading_zeros(self):
        self.assertEqual(t2s.expand_number("007"), ["zero", "zero", "seven"])
        self.assertEqual(t2s.expand_number("0"), ["zero"])
        self.assertEqual(t2s.expand_number("0.5"), ["zero", "point", "five"])

    def test_digits_markup(self):
        self.assertEqual(dec("[digits] 5551234"), dec("five five five one two three four"))
        # every number up to the next word, so a phone number reads whole
        self.assertEqual(dec("call [digits] 555-1234 now 42"),
                         dec("call five five five - one two three four now forty two"))
        self.assertNotEqual(dec("5551234"), dec("[digits] 5551234"))


class Markup(unittest.TestCase):
    def test_raw_codes(self):
        self.assertEqual(t2s.parse_codes("OHIY TT"), [155, 191])
        self.assertEqual(t2s.parse_codes("M2 200"), [254, 200])
        self.assertEqual(t2s.parse_codes("pitch 100"), [22, 100])

    def test_ehle_alias(self):
        # PhraseALator's name for EHLL; the manual's name is used on output
        self.assertEqual(t2s.parse_codes("EHLE"), [159])
        self.assertEqual(dec("{WW EHLE}"), [147, 159])
        self.assertIn("EHLL", run("{EHLE}").stdout)

    def test_contour(self):
        # a pitch before each syllable's vowel, ahead of its STRESS/FAST
        self.assertEqual(t2s.apply_contour([147, 14, 159, 195, 7, 133, 140], [100, 90]),
                         [147, 22, 100, 14, 159, 195, 22, 90, 7, 133, 140])
        # fewer pitches than syllables: the last is held
        self.assertEqual(t2s.apply_contour([128, 129, 130], [90]),
                         [22, 90, 128, 22, 90, 129, 22, 90, 130])
        # a two byte command in the word is kept whole
        self.assertEqual(t2s.apply_contour([21, 128, 131], [95]), [21, 128, 22, 95, 131])
        codes = dec("[contour 100 90] welcome", CMU)
        self.assertEqual(codes[:3], [147, 22, 100])
        self.assertEqual(codes[-1], 31)                 # pitch changed, so Reset added
        self.assertEqual(run("[contour 300] hi").returncode, 1)

    def test_explicit_pause_at_end_kept(self):
        self.assertEqual(dec("level [p5]", CMU)[-1], 5)
        self.assertEqual(dec("level {5}", CMU)[-1], 5)
        self.assertNotIn(dec("level,", CMU)[-1], range(7))     # punctuation: dropped
        self.assertNotIn(dec("level", CMU)[-1], range(7))      # word gap: dropped

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

    def convert(self, *lines):
        return dict(cpa.convert(["[words]"] + list(lines))[0])

    def test_conversion(self):
        words = self.convert(
            "well = \\WW \\Fast \\EHLE ",
            "bird=\\FAST \\SOFT \\BE \\AXRR \\ED",
            # words looked up; each comma is P2, two spaces P6; a note is
            # Pitch and its frequency; \\120 is a byte
            "fxtest=\\Reset well,, bird  \\NTA2 \\SPEED \\120",
            "x-ray= \\EH \\EK \\SE")
        self.assertEqual(words["well"], "WW FAST EHLL")
        self.assertEqual(words["bird"], "FAST SOFT BE AXRR ED")
        self.assertEqual(words["fxtest"],
                         "RESET WW FAST EHLL P2 P2 FAST SOFT BE AXRR ED P6 PITCH 110 SPEED 120")
        self.assertNotIn("x-ray", words)

    def test_conversion_quirks(self):
        words = self.convert(
            "sharp=\\NTA#2",          # the '#' is dropped, as in PhraseALator
            "spaces=\\IY   \\IY",     # three or more spaces: P4 P1
            "saw=\\SE \\AW",         # the manual's AW (136), not PhraseALator's 135
            "slip=\\BEND \\C0",      # a value past the range stays as raw bytes
            "unknown=\\IY \\XYZZY")  # unknown codes are dropped
        self.assertEqual(words["sharp"], "PITCH 110")
        self.assertEqual(words["spaces"], "IY P4 P1 IY")
        self.assertEqual(t2s.parse_codes(words["saw"]), [187, 136])
        self.assertEqual(words["slip"], "23 230")
        self.assertEqual(words["unknown"], "IY")

    def test_notes_match_phrasealator(self):
        self.assertEqual(cpa.NOTES["NTC1"], 33)
        self.assertEqual(cpa.NOTES["NTA2"], 110)
        self.assertEqual(cpa.NOTES["NTF#2"], 93)
        self.assertEqual(cpa.NOTES["NTB3"], 247)

    def test_soft(self):
        self.assertEqual(t2s.parse_codes("SOFT BE"), [18, 170])
        self.assertEqual(dec("[soft] {BE}"), [18, 170])
        self.assertIn("SOFT BE", run("{SOFT BE}").stdout)

    def test_magnevation_notice(self):
        notice = "Portions of this program are provided by and copyrighted by Magnevation LLC."
        self.assertIn(notice, run("hello").stderr)
        self.assertNotIn(notice, run("hello", CMU).stderr)

    def test_converted_file_is_up_to_date(self):
        src = cpa.DEFAULT_SRC
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
