# PhraseALator

> **Deprecated: for reference only.** PhraseALator is Magnevation's 2004
> Windows program for driving a real SpeakJet over a serial port. It is kept
> here only as the record of how Magnevation's own software encoded SpeakJet
> phrases. It is not part of this project, isn't maintained, and isn't needed
> to use anything here; nothing builds or runs it. Its code table also
> disagrees with the SpeakJet manual (see below), so don't treat it as an
> authority on the chip.

| File | What it is |
|---|---|
| [`PhraseALator.zip`](PhraseALator.zip) | The original download, `setup.exe`, an Inno Setup installer for PhraseALator 1.4 (October 2004). It contains the program, its Visual Basic source (`SpeakJet.bas` and the `.frm` forms), its dictionary and its licence (`EULA.txt`) |
| [`PhraseALator.Dic`](PhraseALator.Dic) | Its word dictionary: 1,452 entries of hand-made SpeakJet spellings. Identical to the copy in the installer |

Both are unmodified, as downloaded from Magnevation's site (via the Internet
Archive's copy of `www.magnevation.com/software/`).

## Licence

**Copyright Magnevation LLC.** Neither file is covered by this repository's MIT
licence. PhraseALator's licence (`EULA.txt`, inside the installer) says:

- it is **freeware**, which may be passed to third parties "only if it is in
  the original form from Magnevation LLC", which is why both files are
  unmodified (§3)
- "You may reverse-engineer, decompile or disassemble the program", and may
  "use component parts for the development of your own products as long as
  the following clause is prominently displayed during the use of the newly
  developed product" (§4):

  > Portions of this program are provided by and copyrighted by Magnevation LLC.

text2speakjet uses one component part, the dictionary, and so prints that
clause whenever it uses a word from it.

If you hold the rights to these files and want them removed, please open an
issue.

## How text2speakjet uses it

text2speakjet doesn't read these files. `tools/text2speakjet/convert_phrasealator.py`
converts the dictionary into `tools/text2speakjet/phrasealator.txt`, in
text2speakjet's own format: 1,448 entries, including 25 of the 27 `fx...`
demo phrases. (`fxstatus1`, `fxstatus2`, `x-ray` and `yo-yo` are left out,
because text2speakjet splits words at digits and hyphens and could never look
them up.) The converted file carries the same copyright and licence as the
dictionary.

The converter reproduces what PhraseALator does with an entry, as read from
its source:

- words in an entry are looked up in the dictionary, recursively (the `fx...`
  demos are built this way)
- each comma adds P2 (200 ms); two spaces add P6 (90 ms), three or more
  P4 P1 (130 ms)
- characters other than letters, digits, `\`, space and comma are dropped,
  so the `#` of a sharp note is lost and `\NTA#2` plays A2, just as in
  PhraseALator
- `\NTC1` to `\NTB3` are notes: Pitch (22) followed by the note's frequency
  in Hz (C1 = 33 up to B3 = 247; equal tempered, except F#2, which
  PhraseALator has as 93)
- `\120` is the byte 120; an unknown code is dropped

PhraseALator's own slips are kept as the bytes it would have sent. In
`fxwomwom`, `\BEND \ 1` loses its `1` (PhraseALator takes it for a word and
finds no such word), so the Bend takes the next code as its value; the chip
clamps Bend to 15.

## Its code table and the manual

PhraseALator's table (`InitPhoneneNames` in `SpeakJet.bas`) agrees with the
manual for every code except:

| Code | Manual | PhraseALator |
|---|---|---|
| 18 | not listed | `\SOFT`, "Play the Next Sound Soft" |
| 24, 25, 29 | PortCtr, Port, Goto Phrase | `\PCTRL`, `\POUT`, `\PLAY` (other names, same codes) |
| 135 | OH ("Hot, Clock, Fox") | `\AW` |
| 136 | AW ("Father, Fall") | `\OH` |
| 159 | EHLL | `\EHLE` (another name, same code) |

The dictionary's spellings follow the manual's names (`\OH` in "hot",
"clock", "fox"; `\AW` in "father" and "saw"), so PhraseALator sent OH and
AW swapped from what the manual says. This project follows the manual: the
conversion maps each name by the manual's code, so the converted "hot" plays
OH (135). Only a real SpeakJet can settle which is right: send 183 135 191
and 183 136 191 (HE, the vowel, TT) and listen for which says "hot".

The source's list of `cv...` constants (as opposed to its table) also skips
UX and so is one out from AW onwards, but the table doesn't use those
constants for allophones, so it doesn't affect what PhraseALator sends.
