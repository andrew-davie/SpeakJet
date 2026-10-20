#!/usr/bin/env python3
# This file is part of the SpeakJet C-Library package.
# see https://github.com/andrew-davie/SpeakJet
#
# Copyright (c) 2026 Andrew Davie
# SPDX-License-Identifier: MIT
"""Convert Magnevation's PhraseALator.Dic into text2speakjet's format.

    python3 tools/text2speakjet/convert_phrasealator.py [PhraseALator.Dic] [phrasealator.txt]

Defaults: docs/reference/PhraseALator.Dic in, phrasealator.txt beside this
script out. Each word becomes a line 'word {CODES}', which text2speakjet uses
exactly as written.

What changes on the way:
  - codes are written in upper case (the .Dic file mixes \\Fast and \\FAST)
  - \\EHLE, PhraseALator's name for allophone 159, is written EHLL
  - \\SOFT is dropped: neither edition of the SpeakJet manual defines it
  - the fx... entries are left out: they are demo phrases built from other
    entries and from PhraseALator's musical-note pitch macros (\\NTC2...),
    whose byte values aren't documented
  - words text2speakjet can't look up (with a hyphen or a digit) are left out
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import text2speakjet as t2s  # noqa: E402

# written with the manual's names, though text2speakjet would accept these too
RENAME = t2s.ALIASES
DROP = {"SOFT"}

HEADER = """\
# PhraseALator dictionary, converted for text2speakjet.
#
# Generated from docs/reference/PhraseALator.Dic by convert_phrasealator.py --
# do not edit; put your own pronunciations in exceptions.txt, which is checked
# first.
#
# The pronunciations are Magnevation LLC's, from the dictionary supplied with
# its PhraseALator software (2004). They are NOT covered by this repository's
# MIT licence; see docs/reference/README.md. text2speakjet works without this
# file: delete it, or run with --no-phrasealator, and the CMU dictionary is
# used instead.
#
# %d words. Left out: %s.
"""


def convert(lines):
    words, skipped = [], []
    for line in lines:
        line = line.strip()
        if not line or line.startswith("[") or "=" not in line:
            continue
        word, definition = (part.strip() for part in line.split("=", 1))
        if word.lower().startswith("fx"):
            skipped.append(word)
            continue
        if not re.fullmatch(r"[A-Za-z]+(?:'[A-Za-z]+)*", word):
            skipped.append(word)
            continue

        codes = []
        for tok in definition.split():
            if not tok.startswith("\\"):
                raise SystemExit("%s: unexpected '%s'" % (word, tok))
            name = tok[1:].upper()
            if name in DROP:
                continue
            codes.append(RENAME.get(name, name))

        # every code must be one text2speakjet knows
        try:
            t2s.parse_codes(" ".join(codes))
        except t2s.MarkupError as e:
            raise SystemExit("%s: %s" % (word, e))
        words.append((word.lower(), " ".join(codes)))
    return words, skipped


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs", "reference", "PhraseALator.Dic")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "phrasealator.txt")

    with open(src, encoding="latin-1") as f:
        words, skipped = convert(f.read().splitlines())

    width = max(len(w) for w, _ in words)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER % (len(words), ", ".join(skipped)))
        f.write("\n")
        for word, codes in words:
            f.write("%-*s  {%s}\n" % (width, word, codes))
    print("%s: %d words, %d left out" % (out, len(words), len(skipped)))


if __name__ == "__main__":
    main()
