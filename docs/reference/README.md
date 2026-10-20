# Reference documents

The documents the library and text2speakjet were written from, kept here
because their original sources are no longer reliably available.

**These documents are not covered by this repository's MIT licence.** Each
keeps its own copyright status, given below. They are included unmodified,
for reference only.

## SpeakJet User's Manual

*SpeakJet Natural Speech & Complex Sound Synthesizer User's Manual*,
Magnevation LLC. Two editions:

| File | Edition |
|---|---|
| [`speakjet-user-manual_v1_1.pdf`](speakjet-user-manual_v1_1.pdf) | Revision 1.1, September 17, 2004 (the later edition) |
| [`speakjet-user-manual_v1_0.pdf`](speakjet-user-manual_v1_0.pdf) | Revision 1.0, July 27, 2004 |

**Copyright 2004 Magnevation LLC. All rights reserved.** They are included
for reference because Magnevation no longer exists and the manual's original
home (www.speakjet.com) has gone. If you hold the rights to them and want them
removed, please open an issue.

The emulation is written from this manual. It gives the synthesizer's design,
the allophone and sound effect codes with their durations, the control codes,
and the input buffer and status lines. [`../design.md`](../design.md) says
which parts of the library come from it and which are choices made where it
is silent. The duration of every sound in the library matches the manual's
tables, and `tests/speakjet_test.c` checks this.

The two editions describe the same chip. Their code tables, durations,
control codes and synthesizer registers are identical. Revision 1.1 rewords
the general description, notes that the serial input is TTL level, lets an
Event Input trigger on both edges, drops the claim that each Serial Control
Protocol command returns an acknowledgement character, explains that the
volumes of oscillators 1 to 3 must total 63 or less, and lays out the
phonetic guide table more clearly. None of that changes the library.

## PhraseALator dictionary

[`PhraseALator.Dic`](PhraseALator.Dic)

The word dictionary supplied with Magnevation's PhraseALator software (2004):
1,452 entries of hand-made SpeakJet spellings, 1,425 of them words and 27
`fx...` demo phrases.

**Copyright Magnevation LLC.** It is included unmodified, for reference, on
the same basis as the manual. If you hold the rights to it and want it
removed, please open an issue.

text2speakjet doesn't read this file directly. `tools/text2speakjet/convert_phrasealator.py`
turns it into `tools/text2speakjet/phrasealator.txt`, in text2speakjet's own
format, which the tool uses before the CMU dictionary. On the way, codes are
written in upper case, `\EHLE` (PhraseALator's name for allophone 159; see
"Allophone names" in [`../design.md`](../design.md)) is written with the
manual's name, EHLL, and `\SOFT`, which neither edition of the manual defines, is
dropped. The `fx...` demos are left out: they are built from other entries
and from PhraseALator's musical-note pitch macros (`\NTC2`...), whose byte
values aren't documented. So are "x-ray" and "yo-yo", which text2speakjet
can't look up because it splits words at hyphens. That leaves 1,423 words.

`phrasealator.txt` carries the same copyright as this file and isn't covered
by the MIT licence either. text2speakjet works without it.

## NRL Report 7948

[`nrl-report-7948.pdf`](nrl-report-7948.pdf)

Honey Sue Elovitz, Rodney W. Johnson, Astrid McHugh and John E. Shore,
*Automatic Translation of English Text to Phonetics by Means of
Letter-to-Sound Rules*, NRL Report 7948, Naval Research Laboratory,
Washington, D.C., January 21, 1976. DTIC accession number AD-A021 929.

**Approved for public release; distribution unlimited.** It is a US
Government work, and so in the public domain in the United States. This copy
is the National Technical Information Service reproduction, from the
Internet Archive (item `DTIC_ADA021929`).

text2speakjet uses letter-to-sound rules based on this report to guess the
pronunciation of words that aren't in its dictionary. The report has 329
rules producing IPA; text2speakjet's version (`NRL_RULES` in
`tools/text2speakjet/text2speakjet.py`) has 306, producing ARPAbet.

## CMU Pronouncing Dictionary

Not in this folder: the dictionary itself is
[`tools/text2speakjet/cmudict.dict.gz`](../../tools/text2speakjet/cmudict.dict.gz),
under Carnegie Mellon University's BSD-style licence in
[`LICENSE.cmudict`](../../tools/text2speakjet/LICENSE.cmudict). Its source is
<https://github.com/cmusphinx/cmudict>.
