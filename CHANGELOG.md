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

A second take of the VoxRecord recording agrees with the first (allophone
lengths to 2.5 ms on average, spectra to 2 to 4 dB, the Speed sweep to 15
ms). The reference samples are regenerated again.

- Lengths and fricatives are refitted to the mean of the two takes.
- Pause 5 is 45 ms (was 35), measured against the delays in both takes.
- Bend on a sound effect adds to its tones' frequencies (44 Hz a step)
  instead of multiplying them, as both takes show for D1 and A5; speech is
  bent as before.

A third take of the VoxRecord recording, at a lower level, is barely
clipped, and measures what the first two couldn't. The reference samples are
regenerated again.

- Output filter: an 11-tap FIR with the chip's response, solved from the
  effects that sweep one pure tone (flat to 1.5 kHz, -5.7 dB at 2.4 kHz,
  -15.8 dB at 3.6 kHz), instead of a 2-pole Butterworth at 2200 Hz.
- Formants: AW F1 590 to 695 Hz, UW 300/870 to 380/1065, AY 660/1720 to
  710/1660, OH 730/1090 to 690/1150, WW 300/650 to 495/765. AX, UX, EH, EY,
  IH, OW, UH, LO and RR are confirmed to within about 5%.
- 135 is OH ("hot") and 136 AW, as the manual has them, confirmed.
- Bend adds to the frequencies of oscillators 1 to 3 for speech too (44 Hz a
  step), instead of a semitone a step; the synthesizer no longer multiplies.
- Stress raises F2 and F3 by 80 Hz and leaves the pitch alone, instead of
  pitch x 1.12 and formants +-6%; Relax does nothing, instead of pitch x 0.9.
- Pitch: each voicing period is one sample longer than the setting asks for
  (Pitch 250 sounds at 242.6 Hz), as the chip does.
- Levels: a gain for each allophone (allophone_gain_db) matches its level
  against AW to the chip's: nasals 4 dB, liquids 2.5 dB and the hiss 4 to
  12 dB quieter, JH 13 dB and VV 8 dB quieter, BE/BO 7 dB and PE/PO 9 dB
  louder.
- Fricative spectra refitted through the new filter to the mean of the three
  takes; lengths refitted to the third take (vowels and diphthongs 10 to 14 ms
  shorter: the clipping of the first two made them read long).
- Sound effects retraced: tones as up to three sines (R0-R9 are three tones),
  noise, or a buzz through three formants (A1, A2, A3 and M2 buzz at 87 Hz;
  M2 is a vowel glide), with the filter's loss added back and each effect's
  level fitted. DTMF is 2.4 dB quieter. MAX_PARTS is 64.

Lengths in connected speech. Fitting each allophone's length to its
audible length when played alone left syllables 12% short of the chip's
(MM IY MM 235 ms against 320). One multiplier per class (vowels 0.92, nasals
1.32, diphthongs 1.10, voiceless fricatives 1.14, stops and affricates 1.40)
is now fitted to 30 connected groups from the third take: they go from 6%
short to within 1% (22 ms average error with each group left out of its own
fit, 38 before). Sounds played alone now run 13 ms long on average. Stops and
affricates now have their own entries in allophone_length.

Comparing every allophone and effect with the third take found four more
things, now fixed. Allophone spectra go from 5.2 to 4.6 dB RMS of the chip's
(6.0 for the original library); syllables from 77% to 82% spectrogram match.

- AWRR, OWRR and OWIY start from the earlier OH and AW (fOHr, fAWr): the
  third take's corrections to OH and AW had put them 1 to 3 dB further off.
- Noise that starts now starts at its own frequencies (msa_noise_onset())
  instead of gliding up from the previous sound's: SE and SO no longer sweep
  up into their hiss.
- Nasals: resonances and volumes fitted to the chip's spectra (strong
  resonances near 1.4-1.6 and 2.2-2.6 kHz the published values lacked);
  nasal_frame() is gone. 8.6 to 4.4 dB.
- Stops: each stop's burst and aspiration frequencies scaled together (0.5
  to 1.15) to its spectrum; they were 6-8 dB too strong at 3.2-4 kHz.

Fitting whole sounds to the third take (see docs/design.md). Allophones go
from 63% to 73% spectrogram match, and from 10 under 30% to 1 (TS);
effects from 64% to 70%, and from 4 under 30% to 1 (C8). Connected groups
go from 17 ms to 12 ms mean length error.

- Effects: C0, C2, C3, C4, C6, C8 and C9 are gated at a fitted voicing rate
  (`fx_gate`); R0 to R9 are steady measured tones; levels refitted.
- Stops: each has its own burst and aspiration noise widths and aspiration
  volumes (`STOPN()`); BE, EB, OB, ED, OD, TT, TU, KE, EK and OK are fitted
  as whole sounds, the voiced ones with their own loci and voicing. TT 7% to
  47%, TU 0% to 65%, KE 19% to 65%, EK 15% to 88%.
- SE, SO and SH start with the chip's short low thump (`thumped()`); SE,
  SO, SH, FF and TH are fitted as whole sounds. SE 12% to 67%, SO 15% to
  60%.
- CH, JH, TS and VV refitted (JH 3% to 60%, VV 16% to 86%). A stop with no
  aspiration is now silent after its burst, instead of holding its noise
  oscillators at 0 Hz (a thump).
- Each refitted sound's parts still add up to the manual's length; the
  difference is in allophone_length.

A recording of the VoxProbe ROM, built to test the engine, changes it
(see docs/design.md):

- Voicing: each pulse restarts the formant oscillators, which fall in a
  straight line to nothing in a fixed 8 ms (`ENV_VOICE`), not a sawtooth over
  the whole period. Voiced spectra 3.94 to 3.81 dB of the third take, and the
  VoxProbe's sustained sounds 5.2 to 4.5 dB (spectra floored 30 dB under their
  peak).
- Effects are stretched by Speed like speech, and voiced effects (A1, M2)
  follow the Pitch control.
- Repeat: copies after the first take the manual's length.
- Vowels, nasals, liquids and diphthongs are the manual's length and 28 ms
  (`TRANSITION_MS`), replacing 37 fitted multipliers.
- Fast 0.65 and Slow 1.35; pause lengths give the chip's gaps (pause 0 is a
  short gap, not nothing); Delay is 12 ms longer than its intervals.
- Voiced stops, VV and JH refitted with the new voicing; TT's and SE's
  lengths refitted to the connected groups; levels refitted.

A second VoxProbe recording, of every Pitch and of single voicing pulses
(see docs/design.md):

- Pitch: each voicing period is a whole number of ticks of a 7986 Hz clock,
  8000 / Pitch + 1 (12 settings, among them the default, one more), instead
  of Pitch / (1 + Pitch / 8192). Within 0.12% of the chip from pitch 40 to
  254, where it was up to 2.6% off.
- The oscillators run on the chip's 4.3845 Hz frequency grid; DTMF tones are
  the chip's (a few hertz off the standard ones), Bend is 10 steps a step.
- Formants of 16 vowels, liquids and MM measured from single voicing pulses.
  The first VoxProbe recording's sustained sounds go from 4.5 to 3.7 dB; LE
  improves by 7 dB.

text2speakjet:

- Its length estimate uses the library's Speed curve, lengths, pause lengths
  and effect lengths, lengthens pauses and effects with Speed, and counts
  Delay.

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
