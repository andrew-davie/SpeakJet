#!/usr/bin/env python3
# This file is part of the SpeakJet C-Library package.
# see https://github.com/andrew-davie/SpeakJet
#
# Copyright (c) 2026 Andrew Davie
# SPDX-License-Identifier: MIT
"""text2speakjet.py -- English text to SpeakJet (AtariVox) MSA codes.

Stand-alone (Python 3.8+, standard library only). Needs cmudict.dict.gz next to
this script; exceptions.txt and phrasealator.txt there are loaded too, if
present.

    text2speakjet.py "Welcome to the AtariVox"           DASM .byte lines on stdout
    text2speakjet.py --format c "Well done"              C array
    text2speakjet.py --format dec "Ten, nine, eight"     decimal bytes
    text2speakjet.py --format bin -o out.bin "Hello"     raw bytes
    text2speakjet.py -f phrases.txt                      one label per line

Pipeline: each word is looked up in the exceptions file, then the converted
PhraseALator dictionary (Magnevation's own SpeakJet spellings, if
phrasealator.txt is present; --no-phrasealator skips it), then the CMU
Pronouncing Dictionary (ARPAbet with stress), then tried as two dictionary
words joined ("moonbase" = moon + base), and last guessed with the NRL
letter-to-sound rules (Elovitz et al., 1976). Anything not from the dictionary
or exceptions is reported on stderr -- add it to exceptions.txt to fix it.

ARPAbet is then mapped to SpeakJet allophones by context: front/back variants
of K G B D P S N L H NG from the neighbouring vowel, initial/final stop
variants (BE/EB, DO/OD, KE/EK...), TU vs TT, TS, the R-coloured vowels
(IYRR EYRR AWRR OWRR AXRR), EHLL, IYUW/IYEH/IHWW, OHIH vs OHIY. STRESS goes
before primary-stressed vowels, FAST before unstressed ones (--no-fast to
turn off). Function words ("a", "to", "the", "at"...) are left unstressed.

Inline markup, mixed freely with the text:
    [pitch 96] [speed 86] [volume 100] [bend 5] [fast] [slow] [stress]
    [relax] [reset] [wait] [pause 0-6] [p0-p6]     control codes
    {OHIY TT} {M2} {200}                           raw allophones/SFX/bytes
    (HH AH1 S K)                                   ARPAbet for one word
Persistent controls (pitch, speed, volume, bend...) get a Reset (31) added
at the end unless --no-reset.

Phrase files (-f, or stdin): one phrase per line, "name: text"; blank lines
and lines starting with # are skipped. A line without "name:" gets a name
made from its text.

exceptions.txt: one word per line, then either ARPAbet (stress digits
optional) or SpeakJet codes in braces, used exactly as written:
    atarivox    AE1 T AA0 R IY0 V AA2 K S
    husk        {HO STRESS UX SE KO}
"""

import argparse
import gzip
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CMUDICT_PATH = os.path.join(HERE, "cmudict.dict.gz")
EXCEPTIONS_PATH = os.path.join(HERE, "exceptions.txt")
PHRASEALATOR_PATH = os.path.join(HERE, "phrasealator.txt")


# ---------------------------------------------------------------------------
# MSA codes (SpeakJet user manual). Durations in ms at the default speed.

ALLOPHONES = [
    ("IY", 70), ("IH", 70), ("EY", 70), ("EH", 70), ("AY", 70), ("AX", 70),
    ("UX", 70), ("OH", 70), ("AW", 70), ("OW", 70), ("UH", 70), ("UW", 70),
    ("MM", 70), ("NE", 70), ("NO", 70), ("NGE", 70), ("NGO", 70), ("LE", 70),
    ("LO", 70), ("WW", 70), ("RR", 70), ("IYRR", 200), ("EYRR", 200),
    ("AXRR", 190), ("AWRR", 200), ("OWRR", 185), ("EYIY", 165), ("OHIY", 200),
    ("OWIY", 225), ("OHIH", 185), ("IYEH", 170), ("EHLL", 140), ("IYUW", 180),
    ("AXUW", 170), ("IHWW", 170), ("AYWW", 200), ("OWWW", 131), ("JH", 70),
    ("VV", 70), ("ZZ", 70), ("ZH", 70), ("DH", 70), ("BE", 45), ("BO", 45),
    ("EB", 10), ("OB", 10), ("DE", 45), ("DO", 45), ("ED", 10), ("OD", 10),
    ("GE", 55), ("GO", 55), ("EG", 55), ("OG", 55), ("CH", 70), ("HE", 70),
    ("HO", 70), ("WH", 70), ("FF", 70), ("SE", 40), ("SO", 40), ("SH", 50),
    ("TH", 40), ("TT", 50), ("TU", 70), ("TS", 170), ("KE", 55), ("KO", 55),
    ("EK", 55), ("OK", 45), ("PE", 99), ("PO", 99),
]
assert len(ALLOPHONES) == 72

SOUND_EFFECTS = (
    [("R%d" % i, 80) for i in range(10)]
    + list(zip(["A%d" % i for i in range(10)], [300, 101, 102, 540, 530, 500, 135, 600, 300, 250]))
    + list(zip(["B%d" % i for i in range(10)], [200, 270, 280, 260, 300, 100, 104, 100, 270, 262]))
    + list(zip(["C%d" % i for i in range(10)], [160, 300, 182, 120, 175, 350, 160, 260, 95, 75]))
    + [("D%d" % i, 95) for i in range(12)]
    + [("M0", 125), ("M1", 250), ("M2", 530)]
)
assert len(SOUND_EFFECTS) == 55

CODE = {}
DURATION = {}
for _i, (_n, _ms) in enumerate(ALLOPHONES):
    CODE[_n] = 128 + _i
    DURATION[128 + _i] = _ms
for _i, (_n, _ms) in enumerate(SOUND_EFFECTS):
    CODE[_n] = 200 + _i
    DURATION[200 + _i] = _ms
NAME = {c: n for n, c in CODE.items()}

PAUSE_MS = [0, 100, 200, 700, 30, 60, 90]
SINGLE_CONTROLS = {"FAST": 7, "SLOW": 8, "STRESS": 14, "RELAX": 15, "WAIT": 16, "RESET": 31}
for _i in range(7):
    SINGLE_CONTROLS["P%d" % _i] = _i
# name: (code, max argument). These persist until changed or Reset.
ARG_CONTROLS = {
    "VOLUME": (20, 127), "SPEED": (21, 127), "PITCH": (22, 255), "BEND": (23, 15),
    "PORTCTR": (24, 255), "PORT": (25, 255), "REPEAT": (26, 255), "CALL": (28, 255),
    "GOTO": (29, 255), "DELAY": (30, 255),
}
PERSISTENT = {20, 21, 22, 23, 24, 25}
CONTROL_NAME = {v: k for k, v in SINGLE_CONTROLS.items()}
ARG_NAME = {v[0]: k for k, v in ARG_CONTROLS.items()}

DEFAULT_SPEED = 114


class MarkupError(Exception):
    pass


def parse_codes(text):
    """'{...}' / '[...]' contents -> list of byte values."""
    out = []
    toks = text.replace(",", " ").split()
    i = 0
    while i < len(toks):
        t = toks[i].upper()
        i += 1
        if t.isdigit():
            v = int(t)
            if v > 254:
                raise MarkupError("byte %d out of range (255 is the terminator)" % v)
            out.append(v)
        elif t in CODE:
            out.append(CODE[t])
        elif t in SINGLE_CONTROLS:
            out.append(SINGLE_CONTROLS[t])
        elif t == "PAUSE":
            if i >= len(toks) or not toks[i].isdigit() or int(toks[i]) > 6:
                raise MarkupError("'pause' needs 0-6")
            out.append(int(toks[i]))
            i += 1
        elif t in ARG_CONTROLS:
            code, top = ARG_CONTROLS[t]
            if i >= len(toks) or not toks[i].isdigit() or int(toks[i]) > top:
                raise MarkupError("'%s' needs a value 0-%d" % (t.lower(), top))
            out += [code, int(toks[i])]
            i += 1
        else:
            raise MarkupError("unknown code '%s'" % toks[i - 1])
    return out


def describe(codes):
    """Byte values -> readable names, for the comments beside the output."""
    groups = []             # runs of allophones (space-joined); other items stand alone
    run = []

    def item(text):
        if run:
            groups.append(" ".join(run))
            run.clear()
        groups.append(text)

    i = 0
    while i < len(codes):
        c = codes[i]
        if c in ARG_NAME and i + 1 < len(codes):
            item("%s %d" % (ARG_NAME[c].lower(), codes[i + 1]))
            i += 2
            continue
        if c <= 6:
            item("pause %dms" % PAUSE_MS[c])
        elif c in (7, 8, 14, 15):
            run.append(CONTROL_NAME[c])         # FAST/SLOW/STRESS/RELAX read with the allophones
        elif c in CONTROL_NAME:
            item(CONTROL_NAME[c].lower())
        elif c in NAME:
            run.append(NAME[c])
        else:
            run.append(str(c))
        i += 1
    if run:
        groups.append(" ".join(run))
    return ", ".join(groups)


def estimate_ms(codes):
    speed = DEFAULT_SPEED
    total = 0.0
    scale = 1.0
    i = 0
    while i < len(codes):
        c = codes[i]
        if c in ARG_NAME:
            if c == 21 and i + 1 < len(codes):
                speed = max(codes[i + 1], 1)
            i += 2
            continue
        if c <= 6:
            total += PAUSE_MS[c]
        elif c == 7:
            scale = 0.5
        elif c == 8:
            scale = 1.5
        elif c == 31:
            speed = DEFAULT_SPEED
        elif c in DURATION:
            total += DURATION[c] * scale * (DEFAULT_SPEED / speed if c < 200 else 1.0)
            scale = 1.0
        i += 1
    return total


# ---------------------------------------------------------------------------
# Dictionary

ARPA_VOWELS = {"AA", "AE", "AH", "AO", "AW", "AY", "EH", "ER", "EY", "IH", "IY", "OW", "OY", "UH", "UW"}
ARPA_CONSONANTS = {"B", "CH", "D", "DH", "F", "G", "HH", "JH", "K", "L", "M", "N", "NG", "P", "R",
                   "S", "SH", "T", "TH", "V", "W", "Y", "Z", "ZH"}

# Weak forms used instead of the dictionary's citation forms
WEAK_FORMS = {
    "a": "AH0", "the": "DH AH0", "to": "T UW0", "for": "F ER0", "of": "AH0 V",
    "and": "AH0 N D", "from": "F R AH0 M", "an": "AH0 N",
}
# Spoken without STRESS (and without FAST unless the weak form is reduced)
FUNCTION_WORDS = set(WEAK_FORMS) | {
    "i", "me", "my", "we", "us", "our", "you", "your", "he", "him", "his", "she", "her", "it",
    "its", "they", "them", "their", "am", "is", "are", "was", "were", "be", "been", "at", "in",
    "on", "by", "with", "as", "or", "but", "if", "so", "than", "that", "this", "these", "those",
    "do", "does", "did", "has", "have", "had", "will", "can", "would", "should", "could",
}


def parse_arpabet(text):
    """'HH AH1 S K' -> [(sym, stress)], stress None for consonants. Raises on bad symbols."""
    phs = []
    for tok in text.upper().split():
        m = re.fullmatch(r"([A-Z]+)([012]?)", tok)
        if not m or (m.group(1) not in ARPA_VOWELS and m.group(1) not in ARPA_CONSONANTS):
            raise MarkupError("bad ARPAbet symbol '%s'" % tok)
        sym, digit = m.groups()
        if sym in ARPA_VOWELS:
            phs.append((sym, int(digit) if digit else None))
        else:
            if digit:
                raise MarkupError("stress digit on consonant '%s'" % tok)
            phs.append((sym, None))
    if any(s in ARPA_VOWELS and st is None for s, st in phs):
        phs = guess_stress([s for s, _ in phs], "")
    return phs


class Lexicon:
    def __init__(self, exception_files, phrasealator_file=None):
        self.cmu = {}
        self.exceptions = {}
        self.phrasealator = {}
        if not os.path.exists(CMUDICT_PATH):
            sys.exit("text2speakjet: missing %s" % CMUDICT_PATH)
        with gzip.open(CMUDICT_PATH, "rt", encoding="utf-8") as f:
            for line in f:
                line = line.split("#", 1)[0].split()
                if len(line) < 2 or "(" in line[0]:
                    continue            # alternative pronunciations: keep the first
                self.cmu.setdefault(line[0], " ".join(line[1:]))
        for path in exception_files:
            self.load_exceptions(path)
        if phrasealator_file:
            self.load_exceptions(phrasealator_file, self.phrasealator)

    def load_exceptions(self, path, into=None):
        """Read a file of 'word pronunciation' lines into the exceptions (or into)."""
        table = self.exceptions if into is None else into
        with open(path, encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                line = line.split("#", 1)[0].strip()
                if not line:
                    continue
                parts = line.split(None, 1)
                if len(parts) != 2:
                    sys.exit("%s:%d: expected 'word pronunciation'" % (path, n))
                word, pron = parts[0].lower(), parts[1].strip()
                try:
                    if pron.startswith("{") and pron.endswith("}"):
                        table[word] = ("raw", parse_codes(pron[1:-1]))
                    else:
                        table[word] = ("arpa", parse_arpabet(pron))
                except MarkupError as e:
                    sys.exit("%s:%d: %s" % (path, n, e))

    def lookup(self, word):
        """-> (kind, data, source). kind 'arpa' (data = [(sym, stress)]) or 'raw' (bytes)."""
        w = word.lower()
        if w in self.exceptions:
            kind, data = self.exceptions[w]
            return kind, data, "exceptions"
        if w in WEAK_FORMS:
            return "arpa", parse_arpabet(WEAK_FORMS[w]), "weak form"
        # after the weak forms: PhraseALator's "a" is the letter name
        if w in self.phrasealator:
            kind, data = self.phrasealator[w]
            return kind, data, "phrasealator"
        if w in self.cmu:
            return "arpa", parse_arpabet(self.cmu[w]), "cmudict"
        split = self.compound(w)
        if split:
            a, b = split
            second = [(s, 2 if st == 1 else st) for s, st in parse_arpabet(self.cmu[b])]
            return "arpa", parse_arpabet(self.cmu[a]) + second, "GUESS: %s + %s" % (a, b)
        return "arpa", letter_to_sound(w), "GUESS: spelling rules"

    def compound(self, w):
        best = None
        for k in range(3, len(w) - 2):
            a, b = w[:k], w[k:]
            if a in self.cmu and b in self.cmu:
                if best is None or min(len(a), len(b)) > min(len(best[0]), len(best[1])):
                    best = (a, b)
        return best


# ---------------------------------------------------------------------------
# NRL letter-to-sound rules (Elovitz, Johnson, McHugh & Shore, NRL Report 7948,
# 1976). (left context, match, right context, ARPAbet). Context symbols:
#   #  one or more vowels          :  zero or more consonants
#   ^  one consonant               .  one voiced consonant (B D V G J L M N R W Z)
#   +  E, I or Y                   %  suffix: ER E ES ED ING ELY (right only)
#   &  sibilant (S C G Z X J CH SH)    @  T S R D L Z N J TH CH SH
#   space  word boundary

NRL_RULES = [
    (" ", "A", " ", "AH"), (" ", "ARE", " ", "AA R"), (" ", "AR", "O", "AH R"),
    ("", "AR", "#", "EH R"), ("^", "AS", "#", "EY S"), ("", "A", "WA", "AH"),
    ("", "AW", "", "AO"), (" :", "ANY", "", "EH N IY"), ("", "A", "^+#", "EY"),
    ("#:", "ALLY", "", "AH L IY"), (" ", "AL", "#", "AH L"), ("", "AGAIN", "", "AH G EH N"),
    ("#:", "AG", "E", "IH JH"), ("", "A", "^+:#", "AE"), (" :", "A", "^+ ", "EY"),
    ("", "A", "^%", "EY"), (" ", "ARR", "", "AH R"), ("", "ARR", "", "AE R"),
    (" :", "AR", " ", "AA R"), ("", "AR", " ", "ER"), ("", "AR", "", "AA R"),
    ("", "AIR", "", "EH R"), ("", "AI", "", "EY"), ("", "AY", "", "EY"), ("", "AU", "", "AO"),
    ("#:", "AL", " ", "AH L"), ("#:", "ALS", " ", "AH L Z"), ("", "ALK", "", "AO K"),
    ("", "AL", "^", "AO L"), (" :", "ABLE", "", "EY B AH L"), ("", "ABLE", "", "AH B AH L"),
    ("", "ANG", "+", "EY N JH"), ("", "A", "", "AE"),

    (" ", "BE", "^#", "B IH"), ("", "BEING", "", "B IY IH NG"), (" ", "BOTH", " ", "B OW TH"),
    (" ", "BUS", "#", "B IH Z"), ("", "BUIL", "", "B IH L"), ("", "B", "", "B"),

    (" ", "CH", "^", "K"), ("^E", "CH", "", "K"), ("", "CH", "", "CH"), (" S", "CI", "#", "S AY"),
    ("", "CI", "A", "SH"), ("", "CI", "O", "SH"), ("", "CI", "EN", "SH"), ("", "C", "+", "S"),
    ("", "CK", "", "K"), ("", "COM", "%", "K AH M"), ("", "C", "", "K"),

    ("#:", "DED", " ", "D IH D"), (".E", "D", " ", "D"), ("#:^E", "D", " ", "T"),
    (" ", "DE", "^#", "D IH"), (" ", "DO", " ", "D UW"), (" ", "DOES", "", "D AH Z"),
    (" ", "DOING", "", "D UW IH NG"), (" ", "DOW", "", "D AW"), ("", "DU", "A", "JH UW"),
    ("", "D", "", "D"),

    ("#:", "E", " ", ""), (" :", "E", " ", "IY"), ("#", "ED", " ", "D"), ("#:", "E", "D ", ""),
    ("", "EV", "ER", "EH V"), ("", "E", "^%", "IY"), ("", "ERI", "#", "IY R IY"),
    ("", "ERI", "", "EH R IH"), ("#:", "ER", "#", "ER"), ("", "ER", "#", "EH R"), ("", "ER", "", "ER"),
    (" ", "EVEN", "", "IY V EH N"), ("#:", "E", "W", ""), ("@", "EW", "", "UW"),
    ("", "EW", "", "Y UW"), ("", "E", "O", "IY"), ("#:&", "ES", " ", "IH Z"), ("#:", "E", "S ", ""),
    ("#:", "ELY", " ", "L IY"), ("#:", "EMENT", "", "M EH N T"), ("", "EFUL", "", "F UH L"),
    ("", "EE", "", "IY"), ("", "EARN", "", "ER N"), (" ", "EAR", "^", "ER"), ("", "EAD", "", "EH D"),
    ("#:", "EA", " ", "IY AH"), ("", "EA", "SU", "EH"), ("", "EA", "", "IY"), ("", "EIGH", "", "EY"),
    ("", "EI", "", "IY"), (" ", "EYE", "", "AY"), ("", "EY", "", "IY"), ("", "EU", "", "Y UW"),
    ("", "E", "", "EH"),

    ("", "FUL", "", "F UH L"), ("", "F", "", "F"),

    ("", "GIV", "", "G IH V"), (" ", "G", "I^", "G"), ("", "GE", "T", "G EH"),
    ("SU", "GGES", "", "G JH EH S"), ("", "GG", "", "G"), (" B#", "G", "", "G"), ("", "G", "+", "JH"),
    ("", "GREAT", "", "G R EY T"), ("#", "GH", "", ""), ("", "G", "", "G"),

    (" ", "HAV", "", "HH AE V"), (" ", "HERE", "", "HH IY R"), (" ", "HOUR", "", "AW ER"),
    ("", "HOW", "", "HH AW"), ("", "H", "#", "HH"), ("", "H", "", ""),

    (" ", "IN", "", "IH N"), (" ", "I", " ", "AY"), ("", "IN", "D", "AY N"), ("", "IER", "", "IY ER"),
    ("#:R", "IED", "", "IY D"), ("", "IED", " ", "AY D"), ("", "IEN", "", "IY EH N"),
    ("", "IE", "T", "AY EH"), (" :", "I", "%", "AY"), ("", "I", "%", "IY"), ("", "IE", "", "IY"),
    ("", "I", "^+:#", "IH"), ("", "IR", "#", "AY R"), ("", "IZ", "%", "AY Z"), ("", "IS", "%", "AY Z"),
    ("", "I", "D%", "AY"), ("+^", "I", "^+", "IH"), ("", "I", "T%", "AY"), ("#:^", "I", "^+", "IH"),
    ("", "I", "^+", "AY"), ("", "IR", "", "ER"), ("", "IGH", "", "AY"), ("", "ILD", "", "AY L D"),
    ("", "IGN", " ", "AY N"), ("", "IGN", "^", "AY N"), ("", "IGN", "%", "AY N"),
    ("", "IQUE", "", "IY K"), ("", "I", "", "IH"),

    ("", "J", "", "JH"),

    (" ", "K", "N", ""), ("", "K", "", "K"),

    ("", "LO", "C#", "L OW"), ("L", "L", "", ""), ("#:^", "L", "%", "AH L"), ("", "LEAD", "", "L IY D"),
    ("", "L", "", "L"),

    ("", "MOV", "", "M UW V"), ("", "M", "", "M"),

    ("E", "NG", "+", "N JH"), ("", "NG", "R", "NG G"), ("", "NG", "#", "NG G"),
    ("", "NGL", "%", "NG G AH L"), ("", "NG", "", "NG"), ("", "NK", "", "NG K"),
    (" ", "NOW", " ", "N AW"), ("", "N", "", "N"),

    ("", "OF", " ", "AH V"), ("", "OROUGH", "", "ER OW"), ("#:", "OR", " ", "ER"),
    ("#:", "ORS", " ", "ER Z"), ("", "OR", "", "AO R"), (" ", "ONE", "", "W AH N"), ("", "OW", "", "OW"),
    (" ", "OVER", "", "OW V ER"), ("", "OV", "", "AH V"), ("", "O", "^%", "OW"), ("", "O", "^EN", "OW"),
    ("", "O", "^I#", "OW"), ("", "OL", "D", "OW L"), ("", "OUGHT", "", "AO T"), ("", "OUGH", "", "AH F"),
    (" ", "OU", "", "AW"), ("H", "OU", "S#", "AW"), ("", "OUS", "", "AH S"), ("", "OUR", "", "AO R"),
    ("", "OULD", "", "UH D"), ("^", "OU", "^L", "AH"), ("", "OUP", "", "UW P"), ("", "OU", "", "AW"),
    ("", "OY", "", "OY"), ("", "OING", "", "OW IH NG"), ("", "OI", "", "OY"), ("", "OOR", "", "AO R"),
    ("", "OOK", "", "UH K"), ("", "OOD", "", "UH D"), ("", "OO", "", "UW"), ("", "O", "E", "OW"),
    ("", "O", " ", "OW"), ("", "OA", "", "OW"), (" ", "ONLY", "", "OW N L IY"),
    (" ", "ONCE", "", "W AH N S"), ("C", "O", "N", "AA"), ("", "O", "NG", "AO"),
    (" :^", "O", "N", "AH"), ("I", "ON", "", "AH N"), ("#:", "ON", " ", "AH N"),
    ("#^", "ON", "", "AH N"), ("", "O", "ST ", "OW"), ("", "OF", "^", "AO F"),
    ("", "OTHER", "", "AH DH ER"), ("", "OSS", " ", "AO S"), ("#:^", "OM", "", "AH M"),
    ("", "O", "", "AA"),

    ("", "PH", "", "F"), ("", "PEOP", "", "P IY P"), ("", "POW", "", "P AW"), ("", "PUT", " ", "P UH T"),
    ("", "P", "", "P"),

    ("", "QUAR", "", "K W AO R"), ("", "QU", "", "K W"), ("", "Q", "", "K"),

    (" ", "RE", "^#", "R IY"), ("", "R", "", "R"),

    ("", "SH", "", "SH"), ("#", "SION", "", "ZH AH N"), ("", "SOME", "", "S AH M"),
    ("#", "SUR", "#", "ZH ER"), ("", "SUR", "#", "SH ER"), ("#", "SU", "#", "ZH UW"),
    ("#", "SSU", "#", "SH UW"), ("#", "SED", " ", "Z D"), ("#", "S", "#", "Z"), ("", "SAID", "", "S EH D"),
    ("^", "SION", "", "SH AH N"), ("", "S", "S", ""), (".", "S", " ", "Z"), ("#:.E", "S", " ", "Z"),
    ("#:^##", "S", " ", "Z"), ("#:^#", "S", " ", "S"), ("U", "S", " ", "S"), (" :#", "S", " ", "Z"),
    (" ", "SCH", "", "S K"), ("", "S", "C+", ""), ("#", "SM", "", "Z M"), ("", "S", "", "S"),

    (" ", "THE", " ", "DH AH"), ("", "TO", " ", "T UW"), ("", "THAT", " ", "DH AE T"),
    (" ", "THIS", " ", "DH IH S"), (" ", "THEY", "", "DH EY"), (" ", "THERE", "", "DH EH R"),
    ("", "THER", "", "DH ER"), ("", "THEIR", "", "DH EH R"), (" ", "THAN", " ", "DH AE N"),
    (" ", "THEM", " ", "DH EH M"), ("", "THESE", " ", "DH IY Z"), (" ", "THEN", "", "DH EH N"),
    ("", "THROUGH", "", "TH R UW"), ("", "THOSE", "", "DH OW Z"), ("", "THOUGH", " ", "DH OW"),
    (" ", "THUS", "", "DH AH S"), ("", "TH", "", "TH"), ("#:", "TED", " ", "T IH D"),
    ("S", "TI", "#N", "CH"), ("", "TI", "O", "SH"), ("", "TI", "A", "SH"), ("", "TIEN", "", "SH AH N"),
    ("", "TUR", "#", "CH ER"), ("", "TU", "A", "CH UW"), (" ", "TWO", "", "T UW"), ("", "T", "", "T"),

    (" ", "UN", "I", "Y UW N"), (" ", "UN", "", "AH N"), (" ", "UPON", "", "AH P AO N"),
    ("@", "UR", "#", "UH R"), ("", "UR", "#", "Y UH R"), ("", "UR", "", "ER"), ("", "U", "^ ", "AH"),
    ("", "U", "^^", "AH"), ("", "UY", "", "AY"), (" G", "U", "#", ""), ("G", "U", "%", ""),
    ("G", "U", "#", "W"), ("#N", "U", "", "Y UW"), ("@", "U", "", "UW"), ("", "U", "", "Y UW"),

    ("", "VIEW", "", "V Y UW"), ("", "V", "", "V"),

    (" ", "WERE", "", "W ER"), ("", "WA", "S", "W AA"), ("", "WA", "T", "W AA"),
    ("", "WHERE", "", "W EH R"), ("", "WHAT", "", "W AA T"), ("", "WHOL", "", "HH OW L"),
    ("", "WHO", "", "HH UW"), ("", "WH", "", "W"), ("", "WAR", "", "W AO R"), ("", "WOR", "^", "W ER"),
    ("", "WR", "", "R"), ("", "W", "", "W"),

    (" ", "X", "", "Z"), ("", "X", "", "K S"),

    ("", "YOUNG", "", "Y AH NG"), (" ", "YOU", "", "Y UW"), (" ", "YES", "", "Y EH S"),
    (" ", "Y", "", "Y"), ("#:^", "Y", " ", "IY"), ("#:^", "Y", "I", "IY"), (" :", "Y", " ", "AY"),
    (" :", "Y", "#", "AY"), (" :", "Y", "^+:#", "IH"), (" :", "Y", "^#", "AY"), ("", "Y", "", "IH"),

    ("", "Z", "", "Z"),
]

NRL_BY_LETTER = {}
for _rule in NRL_RULES:
    NRL_BY_LETTER.setdefault(_rule[1][0], []).append(_rule)

_VOWEL_LETTERS = set("AEIOU")
_VOICED = set("BDVGJLMNRWZ")


def _is_cons(c):
    return c.isalpha() and c not in _VOWEL_LETTERS


def _match_context(word, pos, pattern, step):
    """Match a rule context from word[pos], walking left (step -1) or right (+1)."""
    seq = reversed(pattern) if step < 0 else pattern

    def at(p):
        return word[p] if 0 <= p < len(word) else " "

    def pair(p):
        return (at(p - 1) + at(p)) if step < 0 else (at(p) + at(p + 1))

    for ch in seq:
        c = at(pos)
        if ch == "#":
            if c not in _VOWEL_LETTERS:
                return False
            while at(pos) in _VOWEL_LETTERS:
                pos += step
        elif ch == ":":
            while _is_cons(at(pos)):
                pos += step
        elif ch == "^":
            if not _is_cons(c):
                return False
            pos += step
        elif ch == ".":
            if c not in _VOICED:
                return False
            pos += step
        elif ch == "+":
            if c not in "EIY":
                return False
            pos += step
        elif ch in "&@":
            if pair(pos) in (("CH", "SH") if ch == "&" else ("TH", "CH", "SH")):
                pos += 2 * step
            elif c in ("SCGZXJ" if ch == "&" else "TSRDLZNJ"):
                pos += step
            else:
                return False
        elif ch == "%":
            rest = word[pos:] if pos >= 0 else ""
            for suf in ("ING", "ELY", "ER", "ES", "ED", "E"):
                if rest.startswith(suf):
                    pos += len(suf)
                    break
            else:
                return False
        elif ch == " ":
            if c.isalpha():
                return False
            pos += step
        else:
            if c != ch:
                return False
            pos += step
    return True


def letter_to_sound(word):
    w = re.sub(r"[^A-Z]", "", word.upper())
    out = []
    i = 0
    while i < len(w):
        for left, match, right, phones in NRL_BY_LETTER.get(w[i], [("", w[i], "", "")]):
            if not w.startswith(match, i):
                continue
            if _match_context(w, i - 1, left, -1) and _match_context(w, i + len(match), right, 1):
                out += phones.split()
                i += len(match)
                break
        else:
            i += 1
    return guess_stress(out, w)


def guess_stress(syms, spelling):
    """Plain ARPAbet symbols -> [(sym, stress)]: one primary stress, by rough English rules."""
    vowels = [i for i, s in enumerate(syms) if s in ARPA_VOWELS]
    primary = None
    if vowels:
        primary = vowels[0]
        if len(vowels) >= 2 and re.search(r"(TION|SION|CIAN|IC|ICAL|ITY)$", spelling):
            primary = vowels[-2]
        elif len(vowels) >= 2 and re.match(r"(A|BE|DE|RE|UN)[^AEIOU]", spelling):
            primary = vowels[1]
    return [(s, (1 if i == primary else 0) if s in ARPA_VOWELS else None) for i, s in enumerate(syms)]


# ---------------------------------------------------------------------------
# ARPAbet -> SpeakJet allophones

VOWEL_MAP = {
    "IY": "IY", "IH": "IH", "EY": "EYIY", "EH": "EH", "AE": "AY", "AA": "OH", "AO": "AW",
    "OW": "OWWW", "UH": "UH", "UW": "UW", "AW": "AYWW", "OY": "OWIY", "ER": "AXRR",
}
R_COLOURED = {
    "IY": "IYRR", "IH": "IYRR", "EH": "EYRR", "EY": "EYRR", "AE": "EYRR", "AA": "AWRR",
    "AO": "OWRR", "OW": "OWRR", "UH": "OWRR", "UW": "OWRR", "AH": "AXRR",
}
SIMPLE_CONSONANTS = {
    "M": "MM", "R": "RR", "W": "WW", "F": "FF", "V": "VV", "Z": "ZZ", "ZH": "ZH", "SH": "SH",
    "TH": "TH", "DH": "DH", "CH": "CH", "JH": "JH",
}
VOICELESS = {"P", "T", "K", "F", "TH", "S", "SH", "CH", "HH"}
# Vowels (as SpeakJet allophones) that select a consonant's back variant. N, L and S
# take the back variant only for truly back vowels; stops, H and NG also for UX/AX
# (KO UX "come", DO UX "done", HO UX "husk", NGO "hung").
BACK = {"OH", "AW", "OW", "OWWW", "UH", "UW", "AWRR", "OWRR", "OWIY"}
BACK_LOOSE = BACK | {"UX", "AX"}
STOPS = {"B": ("BE", "BO", "EB", "OB"), "D": ("DE", "DO", "ED", "OD"),
         "G": ("GE", "GO", "EG", "OG"), "K": ("KE", "KO", "EK", "OK")}


def arpabet_to_speakjet(phs, fast_unstressed=True):
    """[(sym, stress)] for one word -> list of allophone/control names."""
    n = len(phs)
    sym = [s for s, _ in phs]
    is_vowel = [s in ARPA_VOWELS for s in sym]
    vmap = {}
    consumed = set()

    def after(j):
        return sym[j] if 0 <= j < n else None

    # vowels, with the merges that swallow a following R or L
    for j in range(n):
        if not is_vowel[j]:
            continue
        s, st = phs[j]
        nxt, nxt2 = after(j + 1), after(j + 2)
        coda_next = nxt2 is None or nxt2 not in ARPA_VOWELS
        # EHLL only where the L ends the word or its syllable: "well", "level", "wel-come",
        # but EH LO in "twelve", "help"
        l_closes = nxt2 is None or (after(j + 3) is not None and after(j + 3) in ARPA_VOWELS
                                    and nxt2 not in ARPA_VOWELS)
        if s == "ER":
            v = "AX" if nxt in ARPA_VOWELS else "AXRR"     # "arriving": AX RR OHIH...
        elif nxt == "R" and coda_next and s in R_COLOURED:
            v = R_COLOURED[s]
            consumed.add(j + 1)
        elif nxt == "L" and l_closes and (s == "EH" or (s == "AH" and st == 0)):
            v = "EHLL"
            consumed.add(j + 1)
        elif s == "AH":
            v = "AX" if st == 0 else "UX"
        elif s == "UW":
            v = "IHWW" if st == 1 and after(j - 1) in ("T", "D", "N", "S", "Z") else "UW"
        elif s == "AY":
            v = "OHIY" if nxt in VOICELESS else "OHIH"
        else:
            v = VOWEL_MAP[s]
        vmap[j] = v

    # Y glides into the following vowel
    y_alone = set()
    for i in range(n - 1):
        if sym[i] == "Y" and is_vowel[i + 1]:
            v = vmap[i + 1]
            if v.startswith("IY"):
                consumed.add(i)
            elif sym[i + 1] in ("UW", "UH"):
                vmap[i + 1] = "IYUW"
                consumed.add(i)
            elif sym[i + 1] in ("EH", "AH", "AE"):
                vmap[i + 1] = "IYEH"
                consumed.add(i)
    for i in range(n):
        if sym[i] == "Y" and i not in consumed:
            y_alone.add(i)

    def prev_vowel(i):
        for j in range(i - 1, -1, -1):
            if is_vowel[j]:
                return j
        return None

    def next_vowel(i):
        for j in range(i + 1, n):
            if is_vowel[j]:
                return j
        return None

    def onset(i):
        if i + 1 < n and is_vowel[i + 1]:
            return True
        if i + 2 < n and sym[i + 1] in ("R", "L", "W", "Y") and is_vowel[i + 2]:
            return True
        return prev_vowel(i) is None and next_vowel(i) is not None

    out = []
    i = 0
    while i < n:
        s, st = phs[i]
        if i in consumed:
            i += 1
            continue
        if is_vowel[i]:
            if st == 1:
                out.append("STRESS")
            elif st == 0 and fast_unstressed:
                out.append("FAST")
            out.append(vmap[i])
            if s == "ER" and vmap[i] == "AX":
                out.append("RR")
            i += 1
            continue

        ons = onset(i)
        ref = next_vowel(i) if ons else prev_vowel(i)
        if ref is None:
            ref = next_vowel(i) if not ons else prev_vowel(i)
        ctx = vmap.get(ref, "EH")
        nxt = after(i + 1)

        if s in SIMPLE_CONSONANTS:
            a = SIMPLE_CONSONANTS[s]
        elif i in y_alone:
            a = "IY"
        elif s in STOPS:
            front, back, front_end, back_end = STOPS[s]
            if ons:
                a = back if (ctx in BACK_LOOSE or (nxt == "W" and s in "KG")) else front
            else:
                a = back_end if ctx in BACK_LOOSE else front_end
        elif s == "P":
            a = "PO" if ctx in BACK_LOOSE else "PE"
        elif s == "T":
            if nxt == "S" and (after(i + 2) is None or not is_vowel[i + 2]):
                a = "TS"
                i += 1
            elif ons and ref is not None:
                prev = after(i - 1)
                if prev is None:
                    a = "TU"
                elif prev in ARPA_VOWELS:
                    # flapped before a reduced vowel ("thirty", "water"), not "pluto"
                    a = "TT" if phs[ref][1] == 0 and sym[ref] in ("AH", "IH", "IY", "ER") else "TU"
                else:
                    a = "TU" if phs[ref][1] == 1 and prev != "S" else "TT"
            else:
                a = "TT"
        elif s == "S":
            if nxt == "W":
                a = "SO"
            elif not ons or (nxt is not None and nxt not in ARPA_VOWELS):
                a = "SE"                                # coda, or before a consonant ("small")
            else:
                a = "SO" if ctx in BACK else "SE"
        elif s == "HH":
            a = "HO" if ctx in BACK_LOOSE else "HE"
        elif s == "N":
            a = "NO" if ctx in BACK else "NE"
        elif s == "NG":
            a = "NGO" if ctx in BACK_LOOSE else "NGE"
        elif s == "L":
            if ons:
                a = "LO" if ctx in BACK else "LE"
            else:
                a = "LE" if ctx in ("IY", "IH", "EYIY") else "LO"
        else:
            raise MarkupError("no SpeakJet mapping for '%s'" % s)
        out.append(a)
        i += 1
    return out


# ---------------------------------------------------------------------------
# Numbers

_ONES = ("zero one two three four five six seven eight nine ten eleven twelve thirteen "
         "fourteen fifteen sixteen seventeen eighteen nineteen").split()
_TENS = "_ _ twenty thirty forty fifty sixty seventy eighty ninety".split()


def number_words(n):
    if n < 20:
        return [_ONES[n]]
    if n < 100:
        return [_TENS[n // 10]] + ([_ONES[n % 10]] if n % 10 else [])
    for size, word in ((10 ** 9, "billion"), (10 ** 6, "million"), (1000, "thousand"), (100, "hundred")):
        if n >= size:
            rest = n % size
            return number_words(n // size) + [word] + (number_words(rest) if rest else [])
    return []


def expand_number(tok):
    whole, _, frac = tok.partition(".")
    words = number_words(int(whole))
    if frac:
        words += ["point"] + [_ONES[int(d)] for d in frac]
    return words


# ---------------------------------------------------------------------------
# Phrase builder

TOKEN_RE = re.compile(
    r"\[[^\]]*\]|\{[^}]*\}|\([^)]*\)|\d+(?:\.\d+)?|[A-Za-z]+(?:'[A-Za-z]+)*"
    r"|\.\.\.|…|--|—|[.,;:!?\-]|\S")
PUNCTUATION_PAUSE = {
    ",": [2], ";": [2], ":": [2], ".": [3], "!": [3], "?": [3],
    "...": [2, 1], "…": [2, 1], "--": [2, 1], "—": [2, 1], "-": [],
}


class Segment:
    def __init__(self, label):
        self.label = label
        self.codes = []


class Converter:
    def __init__(self, lexicon, args):
        self.lex = lexicon
        self.args = args
        self.warnings = []

    def word_codes(self, word, arpa=None):
        if arpa is not None:
            kind, data, source = "arpa", arpa, "inline"
        else:
            kind, data, source = self.lex.lookup(word)
        if kind == "raw":
            if self.args.verbose:
                print("  %-14s %-40s [%s]" % (word, describe(data), source), file=sys.stderr)
            return list(data), False
        if word.lower() in FUNCTION_WORDS:
            data = [(s, 2 if st == 1 else st) for s, st in data]
        arpa_text = " ".join(s + ("" if st is None or st == -1 else str(st)) for s, st in data)
        if source.startswith("GUESS"):
            self.warnings.append("'%s' not in the dictionary -- %s: %s" % (word, source[7:], arpa_text))
        names = arpabet_to_speakjet(data, not self.args.no_fast)
        if self.args.verbose:
            print("  %-14s %-28s %-40s [%s]" % (word, arpa_text, " ".join(names), source), file=sys.stderr)
        stressed = any(st == 1 for _, st in data)
        return [CODE.get(a, SINGLE_CONTROLS.get(a)) for a in names], stressed

    def convert(self, text):
        args = self.args
        segs = []
        prefix = []             # controls waiting to lead the next word
        gap = None              # pause codes owed after the previous word
        if args.pitch is not None:
            prefix += [22, args.pitch]
        if args.speed is not None:
            prefix += [21, args.speed]

        words = []              # (kind, payload, label)
        for tok in TOKEN_RE.findall(text):
            if tok[0] in "[{":
                words.append(("codes", tok[1:-1], tok))
            elif tok[0] == "(":
                words.append(("arpa", tok[1:-1], tok))
            elif tok[0].isdigit():
                for w in expand_number(tok):
                    words.append(("word", w, w))
            elif tok[0].isalpha():
                words.append(("word", tok, tok))
            elif tok in PUNCTUATION_PAUSE:
                words.append(("punct", tok, tok))
            elif tok not in "\"'`":
                self.warnings.append("ignored '%s'" % tok)

        content_idx = [k for k, w in enumerate(words) if w[0] in ("word", "arpa")]
        last_content = content_idx[-1] if content_idx else -1
        pitch_now = args.pitch

        for k, (kind, payload, label) in enumerate(words):
            if kind == "punct":
                if segs:
                    gap = PUNCTUATION_PAUSE[payload] if payload != "-" else []
                continue
            if kind == "codes":
                codes = parse_codes(payload)
                if codes and all(c <= 6 for c in codes):
                    gap = codes                 # an explicit pause replaces the word gap
                    continue
                self._flush_gap(segs, gap)
                gap = None
                if any(c >= 128 for c in codes):
                    seg = Segment(label)
                    seg.codes = prefix + codes
                    prefix = []
                    segs.append(seg)
                else:
                    prefix += codes
                continue

            self._flush_gap(segs, gap)
            gap = None
            if kind == "arpa":
                codes, stressed = self.word_codes(label, parse_arpabet(payload))
            else:
                codes, stressed = self.word_codes(payload)
            if args.intonation is not None:
                base = args.intonation
                if k == last_content:
                    want = base - 10
                elif stressed and payload.lower() not in FUNCTION_WORDS:
                    want = base + 8
                else:
                    want = base
                if want != pitch_now:
                    prefix += [22, want]
                    pitch_now = want
            seg = Segment(label)
            seg.codes = prefix + codes
            prefix = []
            segs.append(seg)
            gap = None if args.word_gap is None else [args.word_gap]

        if prefix:
            seg = Segment("")
            seg.codes = prefix
            segs.append(seg)

        allcodes = [c for s in segs for c in s.codes]
        if not args.no_reset and self._uses_persistent(allcodes) and allcodes[-1:] != [31]:
            if segs:
                segs[-1].codes.append(31)
        return segs

    @staticmethod
    def _flush_gap(segs, gap):
        if gap and segs:
            segs[-1].codes += gap

    @staticmethod
    def _uses_persistent(codes):
        i = 0
        while i < len(codes):
            if codes[i] in ARG_NAME:
                if codes[i] in PERSISTENT:
                    return True
                i += 2
            else:
                i += 1
        return False


# ---------------------------------------------------------------------------
# Output

def slug(text):
    words = re.findall(r"[A-Za-z0-9]+", re.sub(r"\[[^\]]*\]|\{[^}]*\}|\([^)]*\)", " ", text))
    s = "_".join(w.lower() for w in words[:4]) or "phrase"
    return ("speech_" + s) if s[0].isdigit() else s


def emit_c(phrases, args):
    lines = []
    for name, text, segs in phrases:
        codes = [c for s in segs for c in s.codes]
        lines.append('// "%s" -- %d bytes%s, ~%.1fs' % (
            text.replace("*/", "* /"), len(codes), "" if args.terminator is None else " + terminator",
            estimate_ms(codes) / 1000))
        lines.append("%s %s[] = {" % (args.c_type, name))
        rows = [(", ".join(str(c) for c in s.codes) + ",", describe(s.codes), s.label) for s in segs if s.codes]
        w1 = max([len(r[0]) for r in rows] + [0])
        w2 = max([len(r[1]) for r in rows] + [0])
        for a, b, c in rows:
            lines.append(("    %-*s  // %-*s  %s" % (w1, a, w2, b, c)).rstrip())
        if args.terminator is not None:
            lines.append("    0x%02X" % args.terminator)
        lines.append("};")
        lines.append("")
    return "\n".join(lines)


def emit_asm(phrases, args):
    lines = []
    for name, text, segs in phrases:
        codes = [c for s in segs for c in s.codes]
        lines.append('; "%s" -- %d bytes%s, ~%.1fs' % (
            text, len(codes), "" if args.terminator is None else " + terminator", estimate_ms(codes) / 1000))
        lines.append(name)
        rows = [(".byte " + ", ".join(str(c) for c in s.codes), describe(s.codes), s.label) for s in segs if s.codes]
        w1 = max([len(r[0]) for r in rows] + [0])
        w2 = max([len(r[1]) for r in rows] + [0])
        for a, b, c in rows:
            lines.append(("%20s%-*s  ; %-*s  %s" % ("", w1, a, w2, b, c)).rstrip())
        if args.terminator is not None:
            lines.append("%20s.byte $%02X" % ("", args.terminator))
        lines.append("")
    return "\n".join(lines)


def emit_dec(phrases, args):
    out = []
    for _, _, segs in phrases:
        codes = [c for s in segs for c in s.codes]
        if args.terminator is not None:
            codes.append(args.terminator)
        out.append(args.separator.join(str(c) for c in codes))
    return "\n".join(out) + "\n"


def emit_bin(phrases, args):
    data = bytearray()
    for _, _, segs in phrases:
        data += bytes(c for s in segs for c in s.codes)
        if args.terminator is not None:
            data.append(args.terminator)
    return bytes(data)


def read_phrase_file(text):
    phrases = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"([A-Za-z_]\w*)\s*:\s*(.*)$", line)
        if m:
            phrases.append((m.group(1), m.group(2)))
        else:
            phrases.append((None, line))
    return phrases


def list_codes():
    print("Allophones (128-199):")
    for name, ms in ALLOPHONES:
        print("  %3d  %-5s %4dms" % (CODE[name], name, ms))
    print("Sound effects (200-254):")
    for name, ms in SOUND_EFFECTS:
        print("  %3d  %-5s %4dms" % (CODE[name], name, ms))
    print("Controls:")
    for name, code in sorted(SINGLE_CONTROLS.items(), key=lambda kv: kv[1]):
        print("  %3d  %s%s" % (code, name, "  (%dms)" % PAUSE_MS[code] if code <= 6 else ""))
    for name, (code, top) in sorted(ARG_CONTROLS.items(), key=lambda kv: kv[1][0]):
        print("  %3d  %s <0-%d>" % (code, name, top))


def main():
    ap = argparse.ArgumentParser(
        description="Convert English text to SpeakJet (AtariVox) MSA codes.",
        epilog="Markup: [pitch 96] [speed 86] [fast] [slow] [stress] [pause 0-6] [reset]; "
               "{RAW ALLOPHONES or numbers}; (ARPAbet for one word). See the script header for details.")
    ap.add_argument("text", nargs="*", help="text to convert (omit to read a phrase file from -f or stdin)")
    ap.add_argument("-f", "--file", help="phrase file: one 'name: text' per line")
    ap.add_argument("-o", "--output", help="output file (default stdout)")
    ap.add_argument("--format", choices=("asm", "c", "dec", "bin"), default="asm", help="output format (default asm)")
    ap.add_argument("-n", "--name", help="array/label name for a single phrase")
    ap.add_argument("--c-type", default="const unsigned char", help="C element type (default 'const unsigned char')")
    ap.add_argument("--terminator", default="255",
                    help="end-of-phrase byte, or 'none' (default 255 = $FF)")
    ap.add_argument("--separator", default=", ", help="separator for --format dec (default ', ')")
    ap.add_argument("--word-gap", default="4",
                    help="pause code 0-6 between words, or 'none' (default 4 = 30ms)")
    ap.add_argument("--no-fast", action="store_true", help="don't put FAST before unstressed vowels")
    ap.add_argument("--no-reset", action="store_true", help="don't append Reset after persistent controls")
    ap.add_argument("--pitch", type=int, help="Pitch (22) at the start, 0-255 (SpeakJet default 88)")
    ap.add_argument("--speed", type=int, help="Speed (21) at the start, 0-127 (SpeakJet default 114)")
    ap.add_argument("--intonation", type=int, nargs="?", const=88, metavar="BASE",
                    help="simple pitch contour: raised on stressed words, falling on the last (default base 88)")
    ap.add_argument("--dict", action="append", default=[], help="extra exceptions file (repeatable)")
    ap.add_argument("--no-exceptions", action="store_true", help="don't load exceptions.txt")
    ap.add_argument("--no-phrasealator", action="store_true",
                    help="don't use the PhraseALator dictionary (phrasealator.txt); use the CMU dictionary instead")
    ap.add_argument("-v", "--verbose", action="store_true", help="show each word's ARPAbet and allophones on stderr")
    ap.add_argument("--list-codes", action="store_true", help="print the MSA code table and exit")
    args = ap.parse_args()

    if args.list_codes:
        list_codes()
        return

    args.terminator = None if args.terminator.lower() == "none" else int(args.terminator, 0)
    if args.terminator is not None and not 0 <= args.terminator <= 255:
        ap.error("--terminator must be 0-255 or 'none'")
    if args.word_gap.lower() == "none":
        args.word_gap = None
    else:
        args.word_gap = int(args.word_gap)
        if not 0 <= args.word_gap <= 6:
            ap.error("--word-gap must be 0-6 or 'none'")
    for opt, top in (("pitch", 255), ("speed", 127), ("intonation", 245)):
        v = getattr(args, opt)
        if v is not None and not 10 <= v <= top:
            ap.error("--%s out of range" % opt)

    if args.text:
        if args.file:
            ap.error("give text or -f, not both")
        phrases = [(args.name, " ".join(args.text))]
    else:
        if args.file:
            with open(args.file, encoding="utf-8") as f:
                phrases = read_phrase_file(f.read())
        elif sys.stdin.isatty():
            ap.error("no text given")
        else:
            phrases = read_phrase_file(sys.stdin.read())
        if args.name and len(phrases) == 1 and phrases[0][0] is None:
            phrases = [(args.name, phrases[0][1])]

    files = [] if args.no_exceptions or not os.path.exists(EXCEPTIONS_PATH) else [EXCEPTIONS_PATH]
    phrasealator = None if args.no_phrasealator or not os.path.exists(PHRASEALATOR_PATH) else PHRASEALATOR_PATH
    lexicon = Lexicon(files + args.dict, phrasealator)
    conv = Converter(lexicon, args)

    built = []
    used = set()
    for name, text in phrases:
        name = name or slug(text)
        base, k = name, 2
        while name in used:
            name = "%s_%d" % (base, k)
            k += 1
        used.add(name)
        if args.verbose:
            print("%s:" % name, file=sys.stderr)
        try:
            segs = conv.convert(text)
        except MarkupError as e:
            sys.exit("text2speakjet: %s: %s" % (name, e))
        built.append((name, text, segs))

    for w in dict.fromkeys(conv.warnings):
        print("warning: " + w, file=sys.stderr)

    if args.format == "bin":
        data = emit_bin(built, args)
        if args.output:
            with open(args.output, "wb") as f:
                f.write(data)
        elif sys.stdout.isatty():
            sys.exit("text2speakjet: refusing to write binary to a terminal; use -o FILE or redirect")
        else:
            sys.stdout.buffer.write(data)
        return

    text = {"c": emit_c, "asm": emit_asm, "dec": emit_dec}[args.format](built, args)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
