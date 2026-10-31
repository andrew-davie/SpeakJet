# Reference documents

The documents the library and text2speakjet were written from, kept here
because their original sources are no longer reliably available.

**These documents are not covered by this repository's MIT licence.** Each
keeps its own copyright status, given below. They are included unmodified,
for reference only.

## SpeakJet User's Manual

*SpeakJet Natural Speech & Complex Sound Synthesizer User's Manual*,
Magnevation LLC, revision 1.1, September 17, 2004:
[`speakjet-user-manual_v1_1.pdf`](speakjet-user-manual_v1_1.pdf).

**Copyright 2004 Magnevation LLC. All rights reserved.** It is included
for reference because Magnevation no longer exists and the manual's original
home (www.speakjet.com) has gone. If you hold the rights to them and want them
removed, please open an issue.

The emulation is written from this manual. It gives the synthesizer's design,
the allophone and sound effect codes with their durations, the control codes,
and the input buffer and status lines. [`../design.md`](../design.md) says
which parts of the library come from it and which are choices made where it
is silent. The duration of every sound in the library matches the manual's
tables, and `tests/speakjet_test.c` checks this.

## PhraseALator dictionary

[`phrasealator/`](phrasealator/) holds Magnevation's 2004 PhraseALator word
dictionary, unmodified: the source of `tools/text2speakjet/phrasealator.txt`,
which text2speakjet looks words up in first. Beside it is the original
download, kept because it carries the licence the dictionary is used under.
**Copyright Magnevation LLC**, used under the terms of PhraseALator's
licence; see [`phrasealator/README.md`](phrasealator/README.md), which also
lists where its code table disagrees with the manual.

## CMU Pronouncing Dictionary

Not in this folder: the dictionary itself is
[`tools/text2speakjet/cmudict.dict.gz`](../../tools/text2speakjet/cmudict.dict.gz),
under Carnegie Mellon University's BSD-style licence in
[`LICENSE.cmudict`](../../tools/text2speakjet/LICENSE.cmudict). Its source is
<https://github.com/cmusphinx/cmudict>.
