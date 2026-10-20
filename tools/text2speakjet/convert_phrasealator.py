#!/usr/bin/env python3
# This file is part of the SpeakJet C-Library package.
# see https://github.com/andrew-davie/SpeakJet
#
# Copyright (c) 2026 Andrew Davie
# SPDX-License-Identifier: MIT
"""Convert Magnevation's PhraseALator.Dic into text2speakjet's format.

    python3 tools/text2speakjet/convert_phrasealator.py [PhraseALator.Dic] [phrasealator.txt]

Defaults: docs/reference/phrasealator/PhraseALator.Dic in, phrasealator.txt
beside this script out. Each entry becomes a line 'word {CODES}', which
text2speakjet uses exactly as written.

An entry is turned into codes the way PhraseALator itself does it, as read
from its source (SpeakJet.bas, in docs/reference/phrasealator/PhraseALator.zip):

  - words in an entry are looked up in the dictionary, recursively
  - each comma adds P2; two spaces add P6, three or more P4 P1
  - characters other than letters, digits, '\\', space and comma are dropped
    (so the '#' of a sharp note is lost, as in PhraseALator)
  - \\NTC1 to \\NTB3 are notes: Pitch (22) followed by the note's frequency
    in Hz, equal tempered
  - \\<number> is that byte; unknown codes are dropped

Codes are mapped by the SpeakJet manual's names, which PhraseALator's table
matches except for three things: it calls 159 EHLE (the manual's EHLL), it has
OH and AW the other way round (the manual's 135 OH and 136 AW are followed
here; see docs/design.md), and it adds SOFT (18), which the manual doesn't
list. PCTRL, POUT and PLAY are its names for PORTCTR, PORT and GOTO.

Entries text2speakjet can't look up (with a hyphen or a digit) are left out.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import text2speakjet as t2s  # noqa: E402

DEFAULT_SRC = os.path.join(ROOT, "docs", "reference", "phrasealator", "PhraseALator.Dic")

# PhraseALator's names, by the manual's code for each
CODES = dict(t2s.CODE)
CODES.update({n: c for n, c in t2s.SINGLE_CONTROLS.items()})
CODES.update({n: c for n, (c, _) in t2s.ARG_CONTROLS.items()})
CODES.update({"EHLE": 159, "PCTRL": 24, "POUT": 25, "PLAY": 29})

# \NTC1 to \NTB3, as PhraseALator's GetNoteFreq gives them: the notes in Hz,
# equal tempered and rounded, except F#2, which it has as 93 (92.5 Hz). Sharps
# can't actually be reached: the '#' is dropped before the lookup
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
NOTE_HZ = [33, 35, 37, 39, 41, 44, 46, 49, 52, 55, 58, 62,
           65, 69, 73, 78, 82, 87, 93, 98, 104, 110, 117, 123,
           131, 139, 147, 156, 165, 175, 185, 196, 208, 220, 233, 247]
NOTES = {"NT%s%d" % (name, octave): NOTE_HZ[12 * (octave - 1) + i]
         for octave in (1, 2, 3) for i, name in enumerate(NOTE_NAMES)}

MAX_DEPTH = 20

HEADER = """\
# PhraseALator dictionary, converted for text2speakjet.
#
# Generated from docs/reference/phrasealator/PhraseALator.Dic by
# convert_phrasealator.py -- do not edit; put your own pronunciations in
# exceptions.txt, which is checked first.
#
# Portions of this program are provided by and copyrighted by Magnevation LLC.
#
# The pronunciations are Magnevation LLC's, from the dictionary supplied with
# its PhraseALator software (2004), used under the PhraseALator licence's
# terms for component parts (see docs/reference/phrasealator/README.md). They
# are NOT covered by this repository's MIT licence. text2speakjet works
# without this file: delete it, or run with --no-phrasealator, and the CMU
# dictionary is used instead.
#
# %d entries. Left out: %s.
"""


def read_dictionary(lines):
    """The [words] entries, keyed in lower case, as PhraseALator reads them."""
    entries = {}
    for line in lines:
        line = line.strip()
        if not line or line.startswith("[") or "=" not in line:
            continue
        word, definition = (part.strip() for part in line.split("=", 1))
        entries.setdefault(word.lower(), definition)
    return entries


def extract(text, entries, depth=0):
    """PhraseALator's ExtractAllophoneListFromSayData: words looked up,
    commas and runs of spaces turned into pauses."""
    out = []
    word = ""
    commas = spaces = 0
    kind = old = None
    for ch in text + " ":
        if ch.isascii() and (ch.isalnum() or ch == "\\"):
            kind = "text"
            word += ch
        elif ch == " ":
            kind = "spaces"
            spaces += 1
        elif ch == ",":
            kind = "commas"
            commas += 1
        if kind != old:
            if old == "text":
                if word.startswith("\\"):
                    out.append(word)
                elif depth < MAX_DEPTH:
                    out.append(extract(entries.get(word.lower(), ""), entries, depth + 1))
                word = ""
            elif old == "commas":
                out.append(" \\P2" * commas)
                commas = 0
            elif old == "spaces":
                out.append({1: "", 2: " \\P6"}.get(spaces, " \\P4 \\P1"))
                spaces = 0
            old = kind
    return "".join(out)


def tokens(allophones):
    """PhraseALator's FormatAllophoneList: split into '\\CODE' tokens."""
    text = re.sub(r"[^A-Z0-9#]", "\\\\", allophones.upper())
    return [t for t in text.split("\\") if t]


def to_codes(allophones):
    """PhraseALator's GetSpeakJetCodes, with the manual's names."""
    codes = []
    for tok in tokens(allophones):
        number = re.match(r"\d+", tok)
        if number:
            codes.append(int(number.group()) & 255)
        elif tok in CODES:
            codes.append(CODES[tok])
        elif tok in NOTES:
            codes += [22, NOTES[tok]]
        # anything else is dropped, as PhraseALator does
    return codes


def write_codes(codes):
    """Bytes -> text2speakjet's {...} names, which read back as the same bytes."""
    arg = {c: (n, top) for n, (c, top) in t2s.ARG_CONTROLS.items()}
    single = {c: n for n, c in t2s.SINGLE_CONTROLS.items()}
    out = []
    i = 0
    while i < len(codes):
        c = codes[i]
        if c in arg and i + 1 < len(codes):
            name, top = arg[c]
            # a value past the manual's range (a slip in the entry) stays as
            # the raw bytes PhraseALator sent
            out.append(("%s %d" if codes[i + 1] <= top else "%d %d")
                       % (name if codes[i + 1] <= top else c, codes[i + 1]))
            i += 2
            continue
        out.append(t2s.NAME.get(c) or single.get(c) or str(c))
        i += 1
    return " ".join(out)


def convert(lines):
    entries = read_dictionary(lines)
    words, skipped = [], []
    for word, definition in entries.items():
        if not re.fullmatch(r"[a-z]+(?:'[a-z]+)*", word):
            skipped.append(word)
            continue
        codes = to_codes(extract(definition, entries))
        if not codes:
            skipped.append(word)
            continue
        text = write_codes(codes)
        if t2s.parse_codes(text) != codes:
            raise SystemExit("%s: '%s' doesn't read back as %s" % (word, text, codes))
        words.append((word, text))
    return words, skipped


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SRC
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "phrasealator.txt")

    with open(src, encoding="latin-1") as f:
        words, skipped = convert(f.read().splitlines())

    width = max(len(w) for w, _ in words)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER % (len(words), ", ".join(skipped)))
        f.write("\n")
        for word, codes in words:
            f.write("%-*s  {%s}\n" % (width, word, codes))
    print("%s: %d entries, %d left out" % (out, len(words), len(skipped)))


if __name__ == "__main__":
    main()
