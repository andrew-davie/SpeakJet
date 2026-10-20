# Reference documents

The documents the library and text2speakjet were written from, kept here
because their original sources are no longer reliably available.

**These documents are not covered by this repository's MIT licence.** Each
keeps its own copyright status, given below. They are included unmodified,
for reference only.

## SpeakJet User's Manual

[`speakjet-usermanual.pdf`](speakjet-usermanual.pdf)

*SpeakJet Natural Speech & Complex Sound Synthesizer User's Manual*,
Magnevation LLC, revision 1.0, July 27, 2004.

**Copyright 2004 Magnevation LLC. All rights reserved.** It is included for
reference because Magnevation no longer exists and the manual's original home
(www.speakjet.com) has gone. If you hold the rights to it and want it removed,
please open an issue.

The emulation is written from this manual. It gives the synthesizer's design,
the allophone and sound effect codes with their durations, the control codes,
and the input buffer and status lines. [`../design.md`](../design.md) says
which parts of the library come from it and which are choices made where it
is silent. The duration of every sound in the library matches the manual's
tables, and `tests/speakjet_test.c` checks this.

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
