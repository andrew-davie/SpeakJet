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
words joined ("moonbase" = moon + base), and last guessed with letter-to-sound
decision trees (Festival's, trained on the CMU dictionary; lts_cmu.json.gz).
"the" is "thee" before a vowel. Anything not from the dictionary
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
    [digits]                                       numbers digit by digit, to the next word
    [contour 100 90]                               the next word's pitch, per syllable
    [gap 4] [gap none]                             the pause between words from here on
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
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CMUDICT_PATH = os.path.join(HERE, "cmudict.dict.gz")
EXCEPTIONS_PATH = os.path.join(HERE, "exceptions.txt")
PHRASEALATOR_PATH = os.path.join(HERE, "phrasealator.txt")

# The PhraseALator EULA lets its parts be used in other programs "as long as
# the following clause is prominently displayed during the use"
MAGNEVATION_NOTICE = "Portions of this program are provided by and copyrighted by Magnevation LLC."


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

# Other names for codes, accepted on input. Output always uses the manual's
# names. EHLE is PhraseALator's spelling of EHLL (159), and is used by its
# dictionary, the AtariVox Programmer's Guide and other SpeakJet code; the
# manual calls it EHLL in both editions.
ALIASES = {"EHLE": "EHLL"}

PAUSE_MS = [0, 100, 200, 700, 30, 60, 90]
# SOFT (18) isn't in the SpeakJet manual. It comes from Magnevation's
# PhraseALator, which sends it before a sound ("Play the Next Sound Soft"); its
# dictionary uses it, mostly before BE/BO. The speakjet library ignores it.
SINGLE_CONTROLS = {"FAST": 7, "SLOW": 8, "STRESS": 14, "RELAX": 15, "WAIT": 16, "SOFT": 18, "RESET": 31}
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
DEFAULT_PITCH = 88


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
        elif t in ALIASES:
            out.append(CODE[ALIASES[t]])
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
        elif c in (7, 8, 14, 15, 18):
            run.append(CONTROL_NAME[c])         # FAST/SLOW/STRESS/RELAX/SOFT read with the allophones
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


# How long the chip plays each sound, as the library has it (speakjet.c,
# measured from recordings of the real chip): CHIP_LENGTH is how much longer
# than the manual's length each allophone lasts at the default speed
# (allophone_length, or the class's length where that is 0), CHIP_EFFECT_MS
# the length of each sound effect (codes 200 to 254) and CHIP_PAUSE_MS of
# pauses 0 to 6 (pause_ms). Keep these in step with speakjet.c.
CHIP_LENGTH = [
    1.29, 1.46, 1.76, 1.77, 1.83, 1.62, 1.78, 1.75, 1.61, 1.40, 1.37, 1.35,
    0.91, 0.87, 0.91, 0.87, 0.87, 1.04, 1.14, 1.26, 1.51, 1.05, 1.16, 1.23,
    1.11, 1.14, 1.10, 1.07, 1.05, 1.10, 1.19, 1.12, 1.06, 1.14, 1.27, 1.11,
    1.34, 1.00, 0.84, 0.99, 0.88, 0.88, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00,
    1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 1.03, 1.01, 0.97, 0.96, 2.26,
    2.28, 1.12, 0.96, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00,
]
CHIP_EFFECT_MS = [
    138, 133, 138, 133, 133, 133, 128, 128, 133, 133, 398, 1008, 1003, 573,
    298, 588, 233, 628, 313, 353, 213, 433, 438, 348, 418, 208, 208, 203, 358,
    333, 248, 333, 253, 253, 268, 383, 308, 423, 148, 128, 153, 153, 153, 153,
    153, 153, 153, 153, 153, 153, 153, 153, 1233, 250, 558,
]
CHIP_PAUSE_MS = [0, 100, 200, 650, 30, 45, 70]


def chip_length(code):
    """How much longer the chip plays an allophone than the manual's length."""
    return CHIP_LENGTH[code - 128]


def speed_factor(speed):
    """The library's Speed curve: lengths fall in a straight line with speed,
    reaching zero at 146 (msa_speed_factor() in speakjet.c)."""
    return (146.0 - speed) / (146.0 - DEFAULT_SPEED)


def estimate_ms(codes):
    speed = DEFAULT_SPEED
    total = 0.0
    scale = 1.0
    i = 0
    while i < len(codes):
        c = codes[i]
        if c in ARG_NAME:
            if c == 21 and i + 1 < len(codes):
                speed = codes[i + 1]
            i += 2
            continue
        if c <= 6:
            total += CHIP_PAUSE_MS[c] * speed_factor(speed)
        elif c == 7:
            scale = 0.6
        elif c == 8:
            scale = 1.5
        elif c == 31:
            speed = DEFAULT_SPEED
        elif c in DURATION:
            if c < 200:
                total += DURATION[c] * scale * chip_length(c) * speed_factor(speed)
            else:
                total += CHIP_EFFECT_MS[c - 200]
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
        self.used_phrasealator = False      # the EULA's notice is then shown
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
            self.used_phrasealator = True
            return kind, data, "phrasealator"
        if w in self.cmu:
            return "arpa", parse_arpabet(self.cmu[w]), "cmudict"
        # in no dictionary: an acronym or a word with no vowel is spelled out
        if (word.isupper() and 2 <= len(word) <= 4) or not re.search("[aeiouy]", w):
            return "arpa", parse_arpabet(spell(w)), "spelled"
        # a possessive: the word, then S, Z or IH Z after the sound it ends with
        m = re.fullmatch(r"(.+)'s", w)
        if m:
            kind, data, source = self.lookup(m.group(1))
            if kind == "raw":
                return "raw", list(data) + [CODE["ZZ"]], source + " + 's"
            last = data[-1][0] if data else ""
            ending = ("IH0 Z" if last in ("S", "Z", "SH", "ZH", "CH", "JH")
                      else "S" if last in ("P", "T", "K", "F", "TH") else "Z")
            return "arpa", data + parse_arpabet(ending), source + " + 's"
        split = self.compound(w)
        if split:
            a, b = split
            second = [(s, 2 if st == 1 else st) for s, st in parse_arpabet(self.cmu[b])]
            return "arpa", parse_arpabet(self.cmu[a]) + second, "GUESS: %s + %s" % (a, b)
        return "arpa", letter_to_sound(w), "GUESS: letter-to-sound"

    def compound(self, w):
        best = None
        for k in range(3, len(w) - 2):
            a, b = w[:k], w[k:]
            if a in self.cmu and b in self.cmu:
                if best is None or min(len(a), len(b)) > min(len(best[0]), len(best[1])):
                    best = (a, b)
        return best


# ---------------------------------------------------------------------------
# Letter to sound, for words in no dictionary: Festival's decision trees for
# the CMU lexicon (lts_cmu.json.gz, made by convert_festival_lts.py; see
# LICENSE.festival). One tree per letter, asking about the letters up to three
# places either side; each leaf is the phones that letter makes.

LTS_PATH = os.path.join(HERE, "lts_cmu.json.gz")
_lts_trees = None


def letter_to_sound(word):
    """Spelling -> [(sym, stress)], from the trees. Tidied a little: a
    consonant isn't doubled, and only the first primary stress is kept."""
    global _lts_trees
    if _lts_trees is None:
        with gzip.open(LTS_PATH, "rt", encoding="utf-8") as f:
            _lts_trees = json.load(f)["trees"]

    letters = ["#"] + list(re.sub(r"[^a-z]", "", word.lower())) + ["#"]

    def at(i):
        return letters[i] if 0 <= i < len(letters) else "0"   # '0': past the boundary

    phones = []
    for i in range(1, len(letters) - 1):
        node = _lts_trees.get(letters[i])
        while isinstance(node, list):
            offset, letter, yes, no = node
            node = yes if at(i + offset) == letter else no
        for ph in (node or "").split():
            if not (phones and ph == phones[-1] and ph in ARPA_CONSONANTS):
                phones.append(ph)

    syms = []
    primary = False
    for ph in phones:
        sym, digit = ph.rstrip("012"), ph[len(ph.rstrip("012")):]
        if sym in ARPA_VOWELS:
            stress = int(digit) if digit else 0
            if stress == 1:
                stress = 2 if primary else 1
                primary = True
            syms.append((sym, stress))
        else:
            syms.append((sym, None))
    if not primary:
        return guess_stress([s for s, _ in syms], word.upper())
    return syms


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


# the names of the letters, for spelling a word out
LETTER_NAMES = {
    "a": "EY1", "b": "B IY1", "c": "S IY1", "d": "D IY1", "e": "IY1", "f": "EH1 F",
    "g": "JH IY1", "h": "EY1 CH", "i": "AY1", "j": "JH EY1", "k": "K EY1", "l": "EH1 L",
    "m": "EH1 M", "n": "EH1 N", "o": "OW1", "p": "P IY1", "q": "K Y UW1", "r": "AA1 R",
    "s": "EH1 S", "t": "T IY1", "u": "Y UW1", "v": "V IY1", "w": "D AH1 B AH0 L Y UW0",
    "x": "EH1 K S", "y": "W AY1", "z": "Z IY1",
}


def spell(word):
    """'bbc' -> ARPAbet for "B B C", each letter stressed."""
    return " ".join(LETTER_NAMES[c] for c in word.lower() if c in LETTER_NAMES)


_ORDINAL = {"one": "first", "two": "second", "three": "third", "five": "fifth",
            "eight": "eighth", "nine": "ninth", "twelve": "twelfth"}


def ordinal_words(n):
    """21 -> twenty first."""
    words = number_words(n)
    last = words[-1]
    if last in _ORDINAL:
        last = _ORDINAL[last]
    elif last.endswith("y"):
        last = last[:-1] + "ieth"
    else:
        last += "th"
    return words[:-1] + [last]


def year_words(n):
    """1984 -> nineteen eighty four; 1905 -> nineteen oh five; 1900 -> nineteen hundred."""
    century, rest = divmod(n, 100)
    if rest == 0:
        return number_words(century) + ["hundred"]
    return number_words(century) + (["oh"] if rest < 10 else []) + number_words(rest)


def money_words(tok):
    """$5.50 -> five dollars and fifty cents."""
    dollars, _, cents = tok[1:].replace(",", "").partition(".")
    dollars, cents = int(dollars), int(cents or 0)
    words = []
    if dollars or not cents:
        words += number_words(dollars) + ["dollar" if dollars == 1 else "dollars"]
    if cents:
        words += (["and"] if words else []) + number_words(cents) + ["cent" if cents == 1 else "cents"]
    return words


def time_words(tok):
    """3:00 -> three o'clock; 3:05 -> three oh five; 3:30 -> three thirty."""
    hours, minutes = (int(x) for x in tok.split(":"))
    if minutes == 0:
        return number_words(hours) + ["o'clock"]
    return number_words(hours) + (["oh"] if minutes < 10 else []) + number_words(minutes)


def digit_words(digits):
    return [_ONES[int(d)] for d in digits]


def expand_number(tok, digits=False):
    """'42' -> forty two. Read digit by digit when asked (digits=True), when
    there are two or more dots (an IP address or version number: 'one nine
    two dot one six eight...') or when it starts with a zero ('007': 'zero
    zero seven'). Commas grouping thousands are ignored."""
    if tok.count(".") >= 2:
        words = []
        for i, part in enumerate(tok.split(".")):
            words += (["dot"] if i else []) + digit_words(part)
        return words
    whole, _, frac = tok.replace(",", "").partition(".")
    if digits:
        words = digit_words(whole)
    elif len(whole) > 1 and whole[0] == "0":
        words = digit_words(whole)
    elif len(whole) == 4 and "," not in tok and not frac and 1000 <= int(whole) <= 1999 \
            and int(whole) % 100:
        words = year_words(int(whole))      # four figures before 2000, not a round hundred: a year
    else:
        words = number_words(int(whole))
    if frac:
        words += ["point"] + digit_words(frac)
    return words


# ---------------------------------------------------------------------------
# Phrase builder

# numbers: dotted (192.168.1.1, two or more dots), grouped in thousands
# (1,000,000: exactly three digits after each comma, so "1, 2, 3" stays a
# list), or plain, with an optional decimal part
NUMBER_RE = r"\d+(?:\.\d+){2,}|\d{1,3}(?:,\d{3})+(?:\.\d+)?|\d+(?:\.\d+)?"
MONEY_RE = r"\$(?:\d{1,3}(?:,\d{3})+|\d+)(?:\.\d\d)?"
TIME_RE = r"\d{1,2}:\d\d"
ORDINAL_RE = r"\d+(?:st|nd|rd|th)\b"
ABBREV_RE = r"(?:[A-Za-z]\.){2,}"          # U.S.A.: spelled out
TOKEN_RE = re.compile(
    r"\[[^\]]*\]|\{[^}]*\}|\([^)]*\)|" + "|".join((MONEY_RE, TIME_RE, ORDINAL_RE, NUMBER_RE, ABBREV_RE))
    + r"|[A-Za-z]+(?:'[A-Za-z]+)*|\.\.\.|…|--|—|[.,;:!?\-]|\S")
SYMBOL_WORDS = {"%": ["percent"], "&": ["and"], "+": ["plus"], "=": ["equals"], "@": ["at"]}
PUNCTUATION_PAUSE = {
    ",": [2], ";": [2], ":": [2], ".": [3], "!": [3], "?": [3],
    "...": [2, 1], "…": [2, 1], "--": [2, 1], "—": [2, 1], "-": [],
}


class Segment:
    def __init__(self, label):
        self.label = label
        self.codes = []
        self.word = False           # a word (or raw sounds), as opposed to controls
        self.content = False        # a content word, which can carry an accent
        self.explicit = False       # its pitch was written ([contour], [pitch])
        self.phrase_end = None      # the punctuation ending a phrase after it


# The vowel allophones: the nucleus of each syllable, for [contour]
NUCLEI = set(range(128, 140)) | set(range(149, 165))
MODIFIERS = {7, 8, 14, 15, 18}      # FAST SLOW STRESS RELAX SOFT, which lead a sound


def apply_contour(codes, pitches, current=None):
    """Put a Pitch command before each syllable's vowel, ahead of the
    modifiers leading it: one pitch per syllable, the last one held. None is
    put where the pitch (current: the one in force before the word) wouldn't
    change."""
    out = []
    lead = None             # where the modifiers before the next sound start
    syllable = 0
    i = 0
    while i < len(codes):
        c = codes[i]
        if c in ARG_NAME and i + 1 < len(codes):
            out += codes[i:i + 2]       # a two byte command, kept whole
            lead = None
            i += 2
            continue
        if c in MODIFIERS:
            if lead is None:
                lead = len(out)
        elif c in NUCLEI:
            want = pitches[min(syllable, len(pitches) - 1)]
            if want is not None and want != current:
                at = len(out) if lead is None else lead
                out[at:at] = [22, want]
                current = want
            syllable += 1
            lead = None
        else:
            lead = None
        out.append(c)
        i += 1
    return out


class Converter:
    def __init__(self, lexicon, args):
        self.lex = lexicon
        self.args = args
        self.warnings = []

    def word_codes(self, word, arpa=None, source="inline"):
        if arpa is not None:
            kind, data = "arpa", arpa
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
        digits = False          # [digits]: numbers read digit by digit, up to the next word
        for tok in TOKEN_RE.findall(text):
            if re.fullmatch(r"\[\s*digits\s*\]", tok, re.I):
                digits = True
                continue
            m = re.fullmatch(r"\[\s*gap\s+(none|\d)\s*\]", tok, re.I)
            if m:
                g = m.group(1).lower()
                if g != "none" and int(g) > 6:
                    raise MarkupError("'gap' needs a pause code 0-6 or 'none'")
                words.append(("gap", None if g == "none" else int(g), tok))
                continue
            m = re.fullmatch(r"\[\s*contour((?:\s+\d+)+)\s*\]", tok, re.I)
            if m:
                pitches = [int(v) for v in m.group(1).split()]
                if any(v > 255 for v in pitches):
                    raise MarkupError("'contour' pitches must be 0-255")
                words.append(("contour", pitches, tok))
                continue
            if tok[0] in "[{":
                words.append(("codes", tok[1:-1], tok))
            elif tok[0] == "(":
                words.append(("arpa", tok[1:-1], tok))
            elif tok[0] == "$" and len(tok) > 1:
                words += [("word", w, w) for w in money_words(tok)]
            elif re.fullmatch(TIME_RE, tok):
                words += [("word", w, w) for w in time_words(tok)]
            elif re.fullmatch(ORDINAL_RE, tok):
                words += [("word", w, w) for w in ordinal_words(int(tok[:-2]))]
            elif re.fullmatch(ABBREV_RE, tok):
                words.append(("arpa", spell(tok.replace(".", "")), tok))
            elif tok[0].isdigit():
                for w in expand_number(tok, digits):
                    words.append(("word", w, w))
            elif tok in SYMBOL_WORDS:
                words += [("word", w, w) for w in SYMBOL_WORDS[tok]]
            elif tok[0].isalpha():
                words.append(("word", tok, tok))
                digits = False
            elif tok in PUNCTUATION_PAUSE:
                words.append(("punct", tok, tok))
            elif tok not in "\"'`":
                self.warnings.append("ignored '%s'" % tok)

        pitch_now = args.pitch
        contour = None          # [contour]: pitches for the next word's syllables
        word_gap = args.word_gap    # [gap]: changes it for the rest of the phrase
        gap_explicit = False    # the gap owed was written out, so it's kept at the end

        for k, (kind, payload, label) in enumerate(words):
            if kind == "contour":
                contour = payload
                continue
            if kind == "gap":
                word_gap = payload
                if gap is not None and not gap_explicit:
                    gap = None if payload is None else [payload]
                continue
            if kind == "punct":
                if segs:
                    gap = PUNCTUATION_PAUSE[payload] if payload != "-" else []
                    gap_explicit = False
                    if payload != "-":
                        segs[-1].phrase_end = payload
                continue
            if kind == "codes":
                codes = parse_codes(payload)
                if codes and all(c <= 6 for c in codes):
                    gap = codes                 # an explicit pause replaces the word gap
                    gap_explicit = True
                    continue
                self._flush_gap(segs, gap)
                gap = None
                pitch_now = self._pitch_after(codes, pitch_now)
                if any(c >= 128 for c in codes):
                    explicit = bool(contour) or 22 in prefix
                    if contour:
                        codes = apply_contour(codes, contour, pitch_now)
                        pitch_now = contour[-1]
                        contour = None
                    seg = Segment(label)
                    seg.codes = prefix + codes
                    seg.word = seg.content = True
                    seg.explicit = explicit
                    prefix = []
                    segs.append(seg)
                else:
                    prefix += codes
                continue

            self._flush_gap(segs, gap)
            gap = None
            if kind == "arpa":
                codes, stressed = self.word_codes(label, parse_arpabet(payload))
            elif payload.lower() == "the" and self._vowel_follows(words, k):
                # "thee" before a vowel, as Festival's post-lexical rule has it
                codes, stressed = self.word_codes(payload, parse_arpabet("DH IY0"),
                                                  "the before a vowel")
            else:
                codes, stressed = self.word_codes(payload)
            explicit = bool(contour) or 22 in prefix
            if contour:
                codes = apply_contour(codes, contour, pitch_now)
                pitch_now = contour[-1]
                contour = None
            seg = Segment(label)
            seg.codes = prefix + codes
            seg.word = True
            seg.content = kind == "arpa" or payload.lower() not in FUNCTION_WORDS
            seg.explicit = explicit
            prefix = []
            segs.append(seg)
            gap = None if word_gap is None else [word_gap]
            gap_explicit = False

        # a pause written at the very end is kept; a word gap or punctuation
        # pause there isn't
        if gap_explicit:
            self._flush_gap(segs, gap)

        if args.intonation is not None:
            self._intonate(segs, args.intonation, args.pitch)

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
    def _intonate(segs, mean, pitch):
        """Festival's 'simple' intonation, per phrase: a baseline falling from
        above the mean through the phrase, a peak on each content word's
        accented syllable (the STRESSed one, or its only one), and a fall at
        the end (a rise for a question). Pitch is set only where it moves --
        the phrase start, each accent and the syllable after it, the end -- and
        the library glides between. Also Klatt's phrase-final lengthening: the
        last syllable before a pause gets SLOW instead of FAST. Words given a
        pitch by hand are left alone."""
        std = max(1, round(mean * 0.2))
        phrases, current = [], []
        for seg in segs:
            if seg.word:
                current.append(seg)
            if seg.phrase_end and current:
                phrases.append((current, seg.phrase_end))
                current = []
        if current:
            phrases.append((current, "."))

        for words, end in phrases:
            # the syllables, as (seg, nucleus position in seg.codes)
            syllables = []
            accents = set()
            for seg in words:
                positions = Converter._nuclei(seg.codes)
                stressed = [n for n in positions if Converter._stressed(seg.codes, n)]
                if seg.content and positions:
                    accents.add((id(seg), (stressed or positions)[0]))
                syllables += [(seg, n) for n in positions]
            if not syllables:
                continue

            # phrase-final lengthening: SLOW, not FAST, on the last syllable
            last_seg, last_n = syllables[-1]
            Converter._lengthen(last_seg, last_n)

            count = len(syllables)
            targets = {}
            for i, (seg, n) in enumerate(syllables):
                base = mean + 0.6 * std - std * (i / (count - 1) if count > 1 else 0)
                if (id(seg), n) in accents:
                    targets[i] = round(base + std)
                    if i + 1 < count:
                        targets.setdefault(i + 1, round(mean + 0.6 * std - std * ((i + 1) / (count - 1))))
                elif i == 0:
                    targets[i] = round(base)
            targets[count - 1] = mean + std if end == "?" else mean - std

            for seg in words:
                if seg.explicit:
                    continue
                order = [i for i, (sg, _) in enumerate(syllables) if sg is seg]
                pitches = [targets.get(i) for i in order]
                if any(p is not None for p in pitches):
                    seg.codes = apply_contour(seg.codes, pitches, pitch)
                pitch = Converter._pitch_after(seg.codes, pitch)

    @staticmethod
    def _nuclei(codes):
        """Positions of the syllable nuclei (vowel allophones) in codes."""
        out, i = [], 0
        while i < len(codes):
            if codes[i] in ARG_NAME:
                i += 2
                continue
            if codes[i] in NUCLEI:
                out.append(i)
            i += 1
        return out

    @staticmethod
    def _stressed(codes, n):
        """Whether STRESS is among the modifiers just before the nucleus at n."""
        i = n - 1
        while i >= 0 and codes[i] in MODIFIERS:
            if codes[i] == 14:
                return True
            i -= 1
        return False

    @staticmethod
    def _lengthen(seg, n):
        """SLOW, and no FAST, on the nucleus at n (seg.codes changes length)."""
        start = n
        while start > 0 and seg.codes[start - 1] in MODIFIERS:
            start -= 1
        lead = [c for c in seg.codes[start:n] if c != 7]
        if 8 not in lead:
            lead.append(8)
        seg.codes[start:n] = lead

    def _vowel_follows(self, words, k):
        """Whether the next word (markup between is skipped) starts with a vowel sound."""
        for kind, payload, _ in words[k + 1:]:
            if kind == "word":
                kind_, data, _ = self.lex.lookup(payload)
                if kind_ == "raw":
                    sounds = [c for c in data if c >= 128]
                    return bool(sounds) and sounds[0] in NUCLEI
                return bool(data) and data[0][0] in ARPA_VOWELS
            if kind == "arpa":
                syms = parse_arpabet(payload)
                return bool(syms) and syms[0][0] in ARPA_VOWELS
            if kind == "punct":
                return False
        return False

    @staticmethod
    def _pitch_after(codes, pitch):
        """The pitch in force after codes: Pitch (22) sets it, Reset (31)
        puts it back to the default."""
        i = 0
        while i < len(codes):
            if codes[i] in ARG_NAME:
                if codes[i] == 22 and i + 1 < len(codes):
                    pitch = codes[i + 1]
                i += 2
                continue
            if codes[i] == 31:
                pitch = DEFAULT_PITCH
            i += 1
        return pitch

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
               "{RAW ALLOPHONES or numbers}; (ARPAbet for one word). See the script header for details. "
               + MAGNEVATION_NOTICE + " (the PhraseALator dictionary, phrasealator.txt)")
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
    if lexicon.used_phrasealator:
        # required by the PhraseALator EULA for programs using its parts
        print(MAGNEVATION_NOTICE, file=sys.stderr)

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
