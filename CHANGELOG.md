# Changelog

## Unreleased

Measurements of the real chip, where two independent sources agree, replace
some guesses. The sound changes, so the reference samples are regenerated.

- Formants: 23 formant frequencies in 9 allophones (OW, AX, RR, UX, IH, MM,
  EY, EH, AW) are the mean of the two measurements, where they are within 10%
  or 88 Hz of each other. The biggest corrections are RR (350/1100/1450 to
  439/1318/1720 Hz) and IH's F2 (1990 to 1774 Hz). Volumes are unchanged.
- Speed: duration is exp(-0.02 (Speed - 114)) instead of 114 / Speed.
- Output filter: a Butterworth low pass at 2200 Hz instead of 3400 Hz.

`docs/design.md` describes the sources and the method.

text2speakjet:

- Words in no dictionary are guessed with Festival's letter-to-sound decision
  trees (trained on the CMU dictionary) instead of the 1976 NRL rules: 43%
  of unseen words exactly right instead of 24%.
- Acronyms and words with no vowel are spelled out; possessives ('s) of
  known words are handled; "the" is "thee" before a vowel.
- Years, ordinals, money, times and %, &, +, =, @ are read as words; numbers
  with thousands separators, dotted numbers and leading zeros are read
  correctly; `[digits]` reads numbers digit by digit.
- `--intonation` follows Festival's "simple" model (declining baseline,
  accent peaks, a fall at the end or a rise for a question) with Klatt's
  phrase-final lengthening.
- `[contour]` sets a word's pitch per syllable; `[gap]` the pause between
  words; an explicit pause at the end of a phrase is kept.
- Magnevation's PhraseALator dictionary is used before the CMU dictionary
  (converted from its own rules; see docs/reference/phrasealator/), with the
  notice its licence asks for; `SOFT` (18) and the alias `EHLE` are accepted.
- The intro sample says "thee AtariVox" (rendered with the measurements above).

## 1.0.0

First release.

- The speakjet library: the SpeakJet's input buffer, MSA sequencer and
  synthesizer, with the serial byte-stream framing, status lines and save
  state support an emulator needs.
- sjwav, which renders SpeakJet codes to a WAV file.
- text2speakjet, which turns English text into SpeakJet codes.
- Sample phrases, which are also the reference for the tests.
