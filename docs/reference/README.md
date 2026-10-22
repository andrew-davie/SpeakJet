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

## PhraseALator (deprecated, for reference only)

[`phrasealator/`](phrasealator/) holds Magnevation's 2004 PhraseALator
software (the original installer, with its Visual Basic source) and its word
dictionary, both unmodified. It's kept as the record of how Magnevation's own
software encoded SpeakJet phrases, and as the source of
`tools/text2speakjet/phrasealator.txt`. It isn't part of this project or
needed by anything in it. **Copyright Magnevation LLC**, used under the terms
of PhraseALator's licence; see [`phrasealator/README.md`](phrasealator/README.md),
which also lists where its code table disagrees with the manual.

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

Earlier versions of text2speakjet guessed the pronunciation of words in no
dictionary with rules based on this report (306 of them, adapted to produce
ARPAbet). They have been replaced by Festival's letter-to-sound decision
trees, which do markedly better on unseen words (see
`tools/text2speakjet/README.md`); the report stays here for reference.

## CMU Pronouncing Dictionary

Not in this folder: the dictionary itself is
[`tools/text2speakjet/cmudict.dict.gz`](../../tools/text2speakjet/cmudict.dict.gz),
under Carnegie Mellon University's BSD-style licence in
[`LICENSE.cmudict`](../../tools/text2speakjet/LICENSE.cmudict). Its source is
<https://github.com/cmusphinx/cmudict>.
