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

The same ROMs on a second unit (an original AtariVox), and fitting to both
chips (see docs/design.md): same firmware, a 0.3% slower clock, and no
extra sound.

- SE, SO and SH: the chip's quiet low onset and full-level hiss (the onset
  was the loudest part here). Hiss sounds 50% to 57%, 4.4 to 2.9 dB, over
  both chips.
- The vowels' and liquids' formant volumes are the chip's, measured from
  single voicing pulses and corrected for the output filter; the
  diphthongs' vowel ends take them too.
- Allophones over both chips: 73.1% to 74.7% spectrogram match, 4.30 to
  3.97 dB.

A third VoxProbe recording, of single voicing pulses on both units:

- The voicing fade is the chip's measured curve (`voice_fade`), not a
  straight 8 ms ramp: fitted to 18 sounds' single pulses on two AtariVoxes,
  each of which it fits as well. All allophones over both chips: 74.7% to
  75.5% spectrogram match, 3.97 to 3.90 dB; voiced sounds 85.7% to 87.0%.
- Six stops have a quiet part silenced, which the earlier fit had placed
  where the recording held only its background noise: the tails of KE, EK and
  OK, a faint 3.6 kHz tone before TU's hiss, and a blip before CH's and TS's.
  Neither chip plays anything there above its background. Timing is
  unchanged, and so are the scores (the comparison already ignored what lies
  under the background).

Rebuilt from the recordings, on both units, keeping what was measured (pitch,
frequency grid, timing) and replacing what was fitted:

- Voicing measured from the pulses' waveforms: the fade (`voice_fade`) holds
  2 ms and is gone by 7.75 ms; the output filter (`output_fir`) is the one the
  pulses give; the 18 measured vowels', nasals' and liquids' formants and
  volumes are the mean of both units. The library's pulses match the chips'
  waveforms to -16.4 dB, from -8.6.
- The second and third formants restart at each sound's own measured phase
  against the first (`formant_phases`), and the fade keeps its measured
  onset: voiced sounds 2.09 to 1.90 dB.
- 26 voiced sounds (the diphthongs, voiced fricatives ZZ, ZH and DH, voiced
  stops BO, DO, ED, OD, GE, GO, EG and OG, and IH, NO and LO) are played from
  the chip's own formant frames, pulse by pulse, at their measured length,
  and so is the effect A2.
- The fricatives, voiceless stops, affricates and the other voiced stops (all
  but TT) are refitted through the new filter to both units at once, keeping
  their lengths: KE, KO, EK, OK, DE, JH and CH 10 to 40 points closer.
- The sound effects' traces lose the recording's background at their start
  and end, and the tone effects are traced again, every tone on its own
  oscillator, starting and stopping in place (`fx2_points`). A sound can now
  have 256 parts.
- Over both units: allophones 75.4% to 82.2% spectrogram match and 3.89 to
  2.53 dB; effects 70.5% to 80.5% and 4.21 to 3.45 dB; connected speech
  unchanged.

The sounds furthest from both units, beyond how far the units are from each
other, looked at for what they have in common:

- Vowels, nasals, liquids and diphthongs bring their voicing up from silence
  over 40 ms (`ONSET_MS`). On both units their first three voicing periods
  stand at -17, -6.3 and -1.5 dB, which were -8, -2.7 and 0 here: the mixer's
  limit had made the 20 to 50 ms volume glide finish within two pulses.
  Voiced sounds 92.1% to 93.2%.
- TS is hiss that starts at its loudest at 3.5 kHz and dies away over 120 ms,
  as on both units, instead of a long broad burst into a short hiss: 13% to
  37% (the units match each other 35%).
- A4 sweeps on to 3.9 kHz and dies away, as on both units, instead of
  stopping at 1315 Hz: 40% to 68%, 9.0 to 2.7 dB.
- M1 holds longer and stops at 300 ms instead of 250, as on both units: 4.3
  to 3.9 dB.
- The nasals come up over 25 ms (`NASAL_ONSET_MS`): both units have them
  within 3 dB of full level by the second period.
- NGE and NGO, never measured before, are fitted to both units' harmonics
  (which agree within 0.5 dB): NGE 87% to 97% and 3.9 to 1.1 dB, NGO 93% to
  96% and 3.0 to 1.8 dB.
- C8 and C9 are noise from 400 Hz to 2.8 kHz gated at 52 Hz 15 dB deep, as on
  both units, instead of traced tones: 34% to 58% and 8.4 to 2.9 dB. A
  frame's envelope can now gate oscillators 4 and 5 to a set depth
  (`noise_depth`).
- C0, C1, C2, C4, C6 and C7 have the broad noise under their tones that both
  units have (`fx_noise`): C1 42% to 67% and 5.7 to 1.7 dB, C7 6.3 to 1.9 dB,
  the others 4 to 8 points closer.
- The traced tone effects' volumes are no longer cut off at the oscillators'
  limit, which had flattened the loudest part of 30 of them (A0's swell, B6's
  fall, the DTMF tones' body); each is scaled so its loudest point is at the
  limit: B2 4.9 to 0.6 dB, R0 3.1 to 1.0 dB.
- The tone effects have the noise skirt both units put under a pure tone, 15
  to 30 dB down: M0 67% to 97%, A0 66% to 86%, B6 6.1 to 0.7 dB. B7 is left
  as it was (its skirt, which on the chip comes only with its 3.9 kHz tone,
  made its spectrogram worse).
- Over both units: allophones 82.2% to 83.3% and 2.53 to 2.49 dB; voiced
  sounds 92.1% to 93.6% and 1.90 to 1.81 dB; effects 80.5% to 85.3% and 3.45
  to 1.90 dB; connected speech unchanged.

The recordings themselves checked (see docs/design.md, "Checking the
recordings themselves"): the background measured and subtracted, clipped
sounds left out (unit A's third take clips in 101 sounds), and the result
checked against recordings nothing was fitted to. Every stage still improves
on the one before. Where the rebuild had made sounds worse:

- IY, EY, EH, AY, AX, UX, OH, AW and MM restart their formants together; their
  measured phases, from pitch 40, don't hold at speaking pitch (IY 3.4 to
  2.2 dB on the held-out VoxProbe recording).
- OB and PO go back to their values before the refit (PO 48% to 64% on
  unit B).

The output above 4 kHz (see docs/design.md, "The output above 4 kHz"):

- The output is now at 48 kHz (`SJ_SAMPLE_RATE`), not 8192 Hz. The chip makes
  its samples at about 8028 Hz (measured from the mirror images of its tones,
  8040 and 8015 Hz on the two units, not the manual's 8192), holds each until
  the next, and the AtariVox's low pass shapes the images that puts up to
  23 kHz; all three are modelled (`CHIP_RATE`, `output_sos`). Above 4 kHz the
  library matches unit B within 2.0 dB where it had nothing (15.5 dB off).
- The noise sounds are fitted again at the chip's rate, their level held
  (`noise_rate_fit`): stops and fricatives 70.5% to 73.3% on unit B, the
  second take 57% to 60%.
- The comparison runs to 16 kHz.

The fourth VoxProbe recording, on both units, held out (see docs/design.md):

- Every stage holds on it: below 4 kHz the original library matches 27%,
  14181cc 58%, 708ed16 64%, now 63.5%; above 4 kHz 19 dB off before, 2 dB
  now.
- Bend moves the noise of effects too (C8 and C9 move with it on the chip).
- A3 buzzes at the voicing frequency and follows Pitch.
- Found and not yet modelled: the gates of C0 to C6 follow Pitch (the voicing
  frequency plus an offset), M2's part near 2.1 kHz, and the effects at
  bend 15.

The same recording, looked at again for those three (see docs/design.md, "The
gates, M2 and bend 15"). Fitted on the default pitch, pitch 60 and bend 0;
pitch 150, the speeds and bend 15 left out to check. Effects only; speech is
unchanged to the bit. The reference samples are regenerated.

- C2, C4 and C6 pulse as the voicing does: at the voicing frequency plus 20.5
  Hz under, 0 and 42.7 Hz over, each pulse fading in a fixed time (the voicing
  fade, 1.6 times as long for C6), with their noise refitted. On the items
  left out C2 73 to 77%, C4 65 to 71%, C6 65 to 67% (2.5 to 2.2, 4.0 to 3.6,
  3.0 to 2.1 dB). C0 and C3 keep their gates (worse or no better that way).
- Bend moves an effect's noise as a block, added after the distortion spread,
  rather than moving the oscillators and widening it: C8 and C9 at bend 15
  10 to 8.8 and 7.5 dB; the first VoxProbe recording, left out, agrees.
- C8 and C9 hold each random value of their noise 0.75 ms, which gives the
  sharp lower edge both units have at bend 15, with the frequencies and spread
  fitted again. Scored over six noise sequences rather than one: all their
  items 46 to 57% and 3.8 to 2.8 dB; bend 15 37 to 62% and 8.3 to 4.4 dB.
- M2 doesn't follow Bend (the same within half a hertz at bend 0 and 15). Its
  "part near 2.1 kHz" is its own F2, which on this recording starts 260 Hz
  higher than on the VoxRecord recording, with no F1; what decides M2's
  starting formants isn't known.
- A traced effect's tone doesn't glide while silent (C3 at bend 15 had swept
  in from 227 Hz).
- All effect items: bend 15 46.0 to 48.3% and 6.21 to 6.08 dB (with C8 and C9
  to 3.5 to 4.5 dB), bend 0 4.58 to 4.35 dB, the pitches 3.78 to 3.71 dB; the
  VoxRecord recording unchanged.

The open items from that, worked through (see docs/design.md, "A1, R8, R9,
C0 and M2"). Effects only; speech is unchanged to the bit. The reference
samples are regenerated.

- A1 is traced again as three formants: steady at 207 Hz, 720 to 1645 Hz, and
  a faster one from about 120 to 2870 Hz that crosses it near 300 ms. Its old
  trace had followed the strongest peak from part to part. Fitted at the
  default only: everywhere else 30 to 93% and 7.4 to 1.3 dB, on the first
  VoxProbe recording 36 to 91% and 7.3 to 1.1 dB. Bend 0, where the chip
  mirrors the fast formant, isn't modelled.
- R8 and R9 pulse: their low tone restarts at half its own frequency, moved
  with Pitch, so its lines follow Pitch and stay put under Bend (the "126 Hz
  lower" low tone). Their steady high tone moves to oscillator 4 and R9 loses
  its noise. On the items left out (pitch 150, bend 15, the speeds) R8 61 to
  77% and 4.9 to 2.8 dB, R9 34 to 67% and 7.1 to 3.2 dB.
- All effect items, floor-matched: default 66.0 to 70.8% and 2.77 to 2.27 dB,
  the pitches 56.0 to 62.5% and 3.86 to 3.31, bend 0 53.7 to 61.8% and 4.40 to
  3.62, bend 15 51.8 to 64.0% and 5.50 to 3.99, the speeds 57.8 to 67.2% and
  3.85 to 2.89; VoxRecord unit B 84.9 to 86.2%.
- C0 and M2 are measured but unchanged: C0's sweep holds at about 205 Hz
  (420 Hz at bend 15), and M2 starts from the formants an earlier sound left
  (A3's end on this recording, A1's on the first VoxProbe recording).

Two AtariVoxes recorded playing Man Goes Down, compared with the game run in
Gopher2600 on this library, scored on every recording at full band (see
docs/design.md, "Man Goes Down"). The reference samples are regenerated.

- One scorer for all recordings, 150 Hz to 16 kHz: the frame-by-frame
  comparisons had cut everything above 4 kHz.
- C1 is voiced at its own pitch, which sweeps a voicing tick at a time from
  333 to 570 to 200 Hz whatever the Pitch control, through three formants,
  with oscillator 3 running free of the voicing (new `frame.free_osc`). In the
  game 0.60 to 0.85 and 4.3 dB too loud to level; the default unchanged; left
  out, bend 0 0.73 to 0.78 and 5.4 to 3.0 dB, pitch 150 0.74 to 0.77, pitch 60
  0.81 to 0.79.
- Slow doesn't lengthen the voiceless consonants (codes 182 and up).
- A pause before a stop shortens its closure by the pause, keeping 20 ms.
- Sounds played from the chip's frames keep their level between two sounds
  instead of dying away and rising again.
- ZZ and ZH have hiss at 3.5 to 4.5 kHz.
- The game's four phrases: 0.82 to 0.83, 0.75 to 0.875, 0.60 to 0.85, 0.63 to
  0.875. Everything else unchanged or better, but C1 at pitch 60.

Every item of the three recordings scored at full band and worked from the
worst up (see docs/design.md, "Every sound, worst first"). The reference
samples are regenerated.

- C3 pulses with the voicing at a distance growing along its trace (new
  `fx_pulse.drift`): its pulses slow from 15 to 22 ms apart as it plays. All
  18 C3 items 0.668 to 0.772, 4.06 to 2.65 dB; pitch 60 0.50 to 0.72.
- A tone Bend takes under nothing wraps round, about 5.7 kHz up, folded at the
  chip's rate, and stays there: A1 at bend 0 0.59 to 0.82. A1's first formant
  is 175 Hz (was 207). All A1 items 0.910 to 0.944.
- C0 pulses with the voicing, 4 Hz under it, its noise 6 dB quieter: 0.762 to
  0.797, pitch 60 0.73 to 0.80.
- C8's and C9's gate keeps time with Speed (new `sound.speed_gate`): C8 at
  speed 90 0.62 to 0.68.
- A noise floor 24 dB under the output's level, as the chip has under its
  tones: the VoxRecord effects 5.99 to 4.36 dB, the DTMF tones 6.4 to 10.2 dB
  to 2.7 to 7.5 dB. The speech is unchanged.

The consonants are shaped after the oscillators (see docs/design.md, "The
consonants, shaped"). The reference samples are regenerated.

- Peaking filters half an octave apart from 180 Hz to 3.6 kHz shape 19
  consonants' spectra to the chip's, with broadband noise under TU, KO and EK
  and each kept at its own level (`consonant_eq`, `synth_eq()`). Fitted to the
  VoxRecord recording; on the fourth VoxProbe recording, left out, GO 0.839
  to 0.869 and 4.78 to 2.61 dB, ED 4.30 to 1.87 dB, BO 2.28 to 1.12 dB, ZZ
  1.95 to 1.11 dB, KO 0.850 to 0.870. Man Goes Down within 0.002.

Lengths, levels and shaping refitted on every recording of both units at once,
VoxRecord's connected groups included: the library given each recording's own
background, both measured by the same rule (length, level over time,
time-frequency picture after alignment, timing within the sound, sideways
spectral shifts, whole-sound spectrum, pitch), each against how closely the
two units agree. A fit was kept only where no kind of recording came out
worse. 752 items, against the two units' own agreement: 1.09 to 0.93 overall,
the allophones alone 0.81 to 0.49, the effects alone 1.05 to 0.94, the
connected groups 1.08 to 0.97, Man Goes Down 1.34 to 1.27. The reference
samples are regenerated.

- A sound that is the last before a pause or the end, and isn't repeated, has
  a length of its own (`final_length`): on the chip a vowel played last is
  longer than inside a phrase (IY and AW 1.25 times, EH 1.18) and TT and FF
  shorter (0.64, 0.79), which no single length fitted both ways.
- 46 allophones' lengths (`refit_length`): the vowels played alone were about
  20% short (EY and AX 1.4 times, AY 1.49, OH 1.24) and the voiceless
  fricatives long (TS and SO 0.83, SE 0.88, TH 0.73). The framed sounds too
  (OG 0.81, ED 0.84, ZZ 0.94, ZH 1.18; EG 1.21 times when last).
- 8 allophones' levels where they sit beside others (`refit_gain_db`): HE +5.8
  dB, NE -6.4, ZZ +4.3, EK -4.9.
- The peaking filters shape 98 sounds (`sound_eq`, was `consonant_eq`), 45 of
  them effects: R2 0.99 to 0.58, A3 0.96 to 0.58, A8 1.02 to 0.72, R5 1.20 to
  0.92, D6 1.08 to 0.77; WH 0.58 to 0.17, EYRR 1.51 to 1.18, SH 1.16 to 0.96.

text2speakjet:

- Its length estimate applies `refit_length` and `final_length`.

- Its length estimate follows the changes above: Slow skips the voiceless
  consonants, a pause shortens a following stop's closure, C1 is 302 ms and
  A1 978 ms.
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
