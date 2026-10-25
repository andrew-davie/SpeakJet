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

A recording of a real SpeakJet playing Alex Herbert's vox_test ROM, compared
sound by sound with the library, corrects timing, noise levels and two
effects. The reference samples are regenerated again.

- Lengths: the chip plays steady sounds (vowels, nasals, liquids,
  fricatives) 1.5 times the manual's length, diphthongs 1.2 times, and stops
  as given. The tables keep the manual's lengths; the factor is applied as a
  sound plays. The test phrases went from 7 to 25% short of the recording to
  within 9%.
- Fast plays a sound at 0.6 of its length instead of 0.5.
- Noise: fricatives, stop bursts and aspiration are 12 to 23 dB quieter, so
  they sit under the vowels as on the chip (SE 23 dB under the vowel beside
  it, as recorded, where it was level with it).
- A5 is two tones 12 dB apart gliding together 486 > 190 > 478 Hz and
  2525 > 2232 > 2522 Hz, as recorded (at Bend 1), instead of a 160 Hz buzz.
- M1 is a short low burst then 1 to 2 kHz noise dying away by 30 dB.

A second recording, of a real AtariVox playing the VoxRecord ROM (every
allophone and effect on its own, and sweeps of each control), corrects
timing, the fricatives and the sound effects. The reference samples are
regenerated again.

- Speed: a sound's length falls in a straight line with Speed,
  (146 - Speed) / 32 of its length at the default, instead of
  exp(-0.02 (Speed - 114)): 4.6 times at Speed 0 instead of 9.8, 0.59 at 127
  instead of 0.77.
- Pauses are lengthened and shortened by Speed as sounds are; they weren't.
- Pause 3 is 650 ms (was 700), pause 5 35 ms (was 60), pause 6 70 ms (was
  90), which give the silences the recording has.
- Lengths: each allophone other than the stops and affricates has its own
  length, fitted to its length on the chip, instead of 1.5 times the manual's
  for every steady sound and 1.2 for diphthongs. Nasals, liquids, voiced
  fricatives and HE, HO, WH, FF were 30 to 40 ms too long; the front vowels
  20 ms too short.
- Fricatives: the voiceless fricatives' hiss is shaped to the chip's spectra
  (SE and SO peak at 3.2 to 4 kHz, SH at 2 to 3 kHz, HE and HO around 1 to
  1.5 kHz) at the same overall levels.
- DTMF tones last 153 ms instead of the manual's 95.
- Sound effects: all but DTMF and M1 are traced from the recording, gliding
  between measured points as a tone, noise or a buzz, with the chip's
  lengths (A1 and A2 nearly a second, M0 1.2 s) instead of guesses with the
  manual's lengths. A5 is traced at the default Bend and no longer assumes
  that Bend scales it.

text2speakjet:

- Its length estimate uses the library's Speed curve, lengths, pause lengths
  and effect lengths, and lengthens pauses with Speed.

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
