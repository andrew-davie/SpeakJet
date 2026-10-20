# text2speakjet

Converts English text into SpeakJet (AtariVox) MSA byte codes. By default the
output is DASM `.byte` lines; it can also produce a C array, decimal text or
raw binary.

It needs Python 3.8 or later and nothing else. Keep these files together in
one folder:

| File | What it is |
|---|---|
| `text2speakjet.py` | the tool |
| `cmudict.dict.gz` | CMU Pronouncing Dictionary (~134,000 words) |
| `LICENSE.cmudict` | the dictionary's licence (keep it with the dictionary) |
| `exceptions.txt` | your own pronunciations, checked before the dictionary |
| `phrasealator.txt` | Magnevation's SpeakJet spellings of 1,423 words and 25 sound effect phrases (optional; not MIT licensed, see below) |
| `convert_phrasealator.py` | rebuilds `phrasealator.txt` from `docs/reference/phrasealator/PhraseALator.Dic` |
| `test_text2speakjet.py` | its tests (`python3 test_text2speakjet.py`; `make test` runs them too) |

## Quick start

```
python3 text2speakjet.py "Welcome to the AtariVox"
```

```
; "Welcome to the AtariVox" -- 28 bytes + terminator, ~1.2s
welcome_to_the_atarivox
                    .byte 147, 14, 159, 195, 7, 133, 140, 4                      ; WW STRESS EHLL KO FAST AX MM, pause 30ms     Welcome
                    .byte 192, 7, 139, 4                                         ; TU FAST UW, pause 30ms                       to
                    .byte 169, 7, 133, 4                                         ; DH FAST AX, pause 30ms                       the
                    .byte 14, 132, 192, 7, 135, 148, 7, 128, 166, 135, 197, 187  ; STRESS AY TU FAST OH RR FAST IY VV OH OK SE  AtariVox
                    .byte $FF
```

Each word gets its own `.byte` line, with a comment naming its codes. The
phrase ends with a `$FF` terminator byte (change or remove it with
`--terminator`).

To run it without typing `python3`, make it executable once with
`chmod +x text2speakjet.py`.

## Output formats

| Option | Output |
|---|---|
| (default) | DASM `.byte` lines under a label |
| `--format c` | C array |
| `--format dec` | decimal bytes on one line, e.g. `192, 14, 131, 141, 255` |
| `--format bin` | raw bytes (use `-o FILE` or redirect; never written to a terminal) |

```
python3 text2speakjet.py -n phraseWellDone "Well done"
python3 text2speakjet.py --format c "Well done"
python3 text2speakjet.py --format dec "Ten, nine, eight"
python3 text2speakjet.py --format bin -o hello.bin "Hello"
```

The same phrase as a C array:

```c
// "Well done" -- 8 bytes + terminator, ~0.4s
const unsigned char well_done[] = {
    147, 14, 159, 4,    // WW STRESS EHLL, pause 30ms  Well
    175, 14, 134, 141,  // DO STRESS UX NE             done
    0xFF
};
```

Output options:

- `-o FILE` writes to a file instead of the terminal.
- `-n NAME` sets the label or array name. Without it, the name is made from
  the first few words of the text.
- `--terminator N` sets a different end byte; `--terminator none` leaves it off.
- `--separator ' '` sets the separator for `--format dec` (default `, `).
- `--c-type TYPE` sets the C element type (default `const unsigned char`).

## Many phrases at once

Put one phrase per line in a text file, as `name: text`:

```
# lines starting with # are ignored
phraseGreeting: Hello there!
phraseGameOver: Game over, man. Game over.
That line has no name, so it gets one made from its text
```

```
python3 text2speakjet.py -f phrases.txt -o phrases.asm
```

You get one label (or array) per line. Without `-f` or any text, it reads the
same format from stdin.

## How words are turned into speech

Each word is looked up in this order:

1. `exceptions.txt`
2. the short spoken forms of "a", "the", "to", "for", "of", "and", "an" and
   "from"
3. `phrasealator.txt`, the PhraseALator dictionary
4. the CMU dictionary
5. two dictionary words joined together (`moonbase` = `moon` + `base`)
6. a guess from the spelling (the NRL letter-to-sound rules)

Steps 5 and 6 are guesses, so the tool prints a warning for each one:

```
warning: 'fnorbix' not in the dictionary -- spelling rules: F N AO1 R B IH0 K S
```

If the guess sounds wrong, add the word to `exceptions.txt`.

The pronunciation is then turned into SpeakJet allophones. The tool picks the
right variant for each sound from its neighbours: front or back (KE/KO,
LE/LO, NE/NO), start or end of a word (BE/EB, DO/OD), TU/TT, and the combined
sounds (AWRR, EHLL, IYUW...). It also adds:

- **STRESS** before each stressed vowel.
- **FAST** before each unstressed vowel. Turn this off with `--no-fast`.
- **No stress on small words** like "a", "the", "to", "at" and "you". "a",
  "to", "for", "of", "and" and "from" use their short spoken forms.
- **Pauses between words:** 30ms by default. Change it with `--word-gap 0-6`
  (pause code) or turn it off with `--word-gap none`.
- **Pauses for punctuation:** `,` `;` `:` 200ms, `...` and `--` 300ms, `.` `!`
  `?` 700ms. A pause at the very end is dropped.
- **Numbers as words:** `42` is spoken "forty two" and `3.5` "three point
  five". Commas grouping thousands are understood (`1,000,000` is "one
  million"; `1, 2, 3` is still a list). Two or more dots are read digit by
  digit (`192.168.1.1` is "one nine two dot one six eight dot one dot one"),
  and so is a number starting with zero (`007` is "zero zero seven"). Put
  `[digits]` before a number to read it digit by digit too, such as a phone
  number: `[digits] 555-1234`. It lasts until the next word.

## Fine-tuning inside the text

Mix these into the text wherever you need them.

**Control codes**, in square brackets. They apply to what follows:

| Markup | Code |
|---|---|
| `[pitch 96]` | Pitch, 0-255 (SpeakJet default 88) |
| `[speed 86]` | Speed, 0-127 (default 114) |
| `[volume 100]` | Volume, 0-127 |
| `[bend 5]` | Bend, 0-15 |
| `[fast]` `[slow]` `[stress]` `[relax]` `[soft]` | apply to the next sound only (`[soft]`, code 18, comes from PhraseALator; the manual doesn't list it, and the speakjet library ignores it) |
| `[pause 0-6]` or `[p0]`-`[p6]` | 0, 100, 200, 700, 30, 60, 90ms |
| `[digits]` | read the numbers that follow digit by digit, up to the next word (not a SpeakJet code) |
| `[contour 100 90]` | the next word's pitch, one value per syllable (the last is held): a Pitch command goes before each syllable's vowel |
| `[reset]` `[wait]` | Reset, Wait |

A pause written in brackets replaces the normal pause between words at that
point, and is kept even at the very end of a phrase (the normal word gap and
punctuation pauses aren't).

`[contour]` shapes a word's pitch syllable by syllable, with the
pronunciation still coming from the dictionaries:

```
[contour 100 90] welcome to [contour 96] wen [contour 104] hop, [contour 104 78] pluto
```

**Raw codes**, in curly brackets. They're copied in as they are:

```
{OHIY TT}      allophones by name
{M2}           sound effect by name (R0-R9, A0-A9, B0-B9, C0-C9, D0-D11, M0-M2)
{200 14}       plain byte values (0-254)
```

Names are the SpeakJet manual's. `EHLE`, PhraseALator's spelling of `EHLL`
(code 159) that the AtariVox Programmer's Guide also uses, is accepted too;
the output always says `EHLL`. See "Allophone names" in `docs/design.md`.

**One word's pronunciation**, in round brackets, as ARPAbet with stress
digits (1 = stressed, 2 = secondary, 0 = unstressed):

```
You say (T AH0 M EY1 T OW2) tomato
```

Example combining all three:

```
python3 text2speakjet.py "[pitch 100] Ten, nine [slow] eight {M2} [reset]"
```

If the phrase changes pitch, speed, volume or bend, a Reset (31) is added at
the end automatically so later speech isn't affected. Turn that off with
`--no-reset`.

## Whole-phrase options

| Option | Effect |
|---|---|
| `--pitch N` | Pitch at the start of the phrase |
| `--speed N` | Speed at the start of the phrase |
| `--intonation [BASE]` | Pitch raised on stressed words and falling on the last word, around BASE (default 88). Adds 2 bytes per pitch change. |

## The exceptions file

`exceptions.txt` has one word per line, then its pronunciation, in one of two
forms:

```
# ARPAbet: the tool still picks variants and adds STRESS/FAST
atarivox    AE1 T AA0 R IY0 V AA2 K S

# SpeakJet codes in curly brackets: used exactly as written
husk        {HO STRESS UX SE KO}
```

ARPAbet without stress digits is accepted; the tool then guesses which vowel
is stressed. Lookups ignore upper/lower case. `--dict FILE` adds more
exception files (repeatable), and `--no-exceptions` skips `exceptions.txt`.

ARPAbet symbols:

| Vowels | Consonants |
|---|---|
| AA (h**o**t) · AE (h**a**t) · AH (b**u**t) · AO (f**a**ll) · AW (n**ow**) · AY (b**i**te) · EH (b**e**d) · ER (b**ir**d) · EY (g**a**te) · IH (s**i**t) · IY (s**ee**) · OW (g**o**) · OY (b**oy**) · UH (b**oo**k) · UW (f**oo**d) | B · CH · D · DH (**th**at) · F · G · HH · JH (**j**et) · K · L · M · N · NG (si**ng**) · P · R · S · SH · T · TH (**th**in) · V · W · Y · Z · ZH (a**z**ure) |

To see how the dictionary says a word, run with `-v`.

## Hearing it

`sjwav` (in `tools/`, built by `make`) plays codes through the speakjet
library and writes a WAV file:

```
python3 tools/text2speakjet/text2speakjet.py --format bin -o hello.bin "Hello"
./sjwav -b -o hello.wav < hello.bin
```

## Checking what it did

- `-v` prints each word's dictionary pronunciation, the allophones chosen and
  where the pronunciation came from (`cmudict`, `exceptions`, `GUESS`...).
- `--list-codes` prints every MSA code with its name and duration.
- The duration in each header comment is an estimate from the SpeakJet
  manual's sound lengths, not a measurement.

## The PhraseALator dictionary

`phrasealator.txt` holds the SpeakJet spellings Magnevation made by hand for
the dictionary supplied with its PhraseALator software, converted to this
tool's format. They were tuned on the real chip, so for the 1,423 words it
covers they're used ahead of the CMU dictionary. `-v` shows `[phrasealator]`
for those words.

It also has 25 of PhraseALator's sound effect phrases, which you use like
words: `fxalarm`, `fxalpha`, `fxburp`, `fxcountdown`, `fxcrickets`,
`fxdanger`, `fxfrogs`, `fxmonths`, `fxnumbers`, `fxping`, `fxsongscales`,
`fxufo`, `fxwomwom` and others (see the file for the full list).

The spellings are Magnevation's, used under the terms of PhraseALator's
licence and not covered by this repository's MIT licence (see
`docs/reference/phrasealator/README.md`). That licence asks for this notice
to be shown when its parts are used, so the tool prints it whenever it uses
a word from the file:

```
Portions of this program are provided by and copyrighted by Magnevation LLC.
```

The tool works without the file: run with `--no-phrasealator`, or delete
`phrasealator.txt`, and the CMU dictionary is used instead. Your own
`exceptions.txt` is always checked first.

`convert_phrasealator.py` rebuilds `phrasealator.txt` from the original
`docs/reference/phrasealator/PhraseALator.Dic`, converting each entry the way
PhraseALator itself did, but with the SpeakJet manual's names for the codes;
`make phrasealator` runs it.

## Limits

- Ordinary dictionary words come out clear and correctly stressed. Made-up
  words and names are guesses until you add them to `exceptions.txt`.
- It doesn't produce expressive pitch the way a hand-tuned phrase does. Use
  `[pitch]` markup (or `--intonation` as a starting point) for that.
- Where a word has more than one pronunciation in the dictionary, the first
  one is used. Use `(...)` or `exceptions.txt` to choose another.
- Words are split at hyphens and digits, so `x-ray` is "x" and "ray". Write
  `xray`, or add the word to `exceptions.txt`.
- The PhraseALator spellings were tuned by ear on a real SpeakJet, not on
  the speakjet library's emulation of it; `--no-phrasealator` compares.
