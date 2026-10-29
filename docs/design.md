# Design

This is the second, from-scratch implementation of the SpeakJet emulation
(branch `rebuild`). It was written from two sources only: the SpeakJet User's
Manual (revision 1.1, 2004) and recordings of two AtariVoxes (an AtariVox+,
"unit A", and an original AtariVox, "unit B") playing the VoxRecord and
VoxProbe 1 to 5 test ROMs. None of the first implementation's findings,
tables or code were used. The aim was the smallest model that matches the
recordings: rules wherever a rule could be found, and a few bytes of
parameters per sound where it could not.

The whole library is `speakjet.c` (about 700 lines) and a generated table,
`speakjet_tables.c`: 72 allophones of 14 bytes and about 100 effect parts of
15 bytes, frequencies in 16 Hz steps and lengths in 5 ms. Compiled with
`-O2` the object is about 12.5 KB, of which the tables are about 2.5 KB.

## What the recordings gave

Everything below was measured from the recordings in this order; each
finding is a rule in the code with its constant in `struct tune`.

**Sample rate.** A tone made at rate Fs and held between samples has images
at k·Fs ± f. From the images of the DTMF tones: unit A 8039 Hz, unit B 8013
Hz. The library uses their mean, `RATE` 8026. The manual's 8192 is nominal.

**The chip counts in 8000ths.** Tones programmed as 697 and 1209 Hz come out
at exactly Fs/8000 times that on both units, so the oscillators' phase
increments are computed for a nominal 8000 Hz tick (`NOMINAL`), while the real
tick is 1/8026 s.

**Voicing period.** From VoxProbe 2 (AW at every Pitch 0–254, both units):
the period is an integer number of ticks, N = floor(Fs / Pitch) + 1, so f0
plateaus (Pitch 157–160 all give 157.6 Hz).

**The voicing pulse.** VoxProbe 3 holds 18 vowels, nasals and liquids at
Pitch 40, where each pulse stands alone. Averaged pulses are identical on both
units to the sample. Fitting a forward model (formant sines restarting at the
period, under a shared envelope, through the hold and the output filter) to
them: the envelope is a triangle, about 1 ms rise and 6 ms fall
(`ENV_RISE` 8, `ENV_FALL` 50 ticks), and every formant restarts each period
(the phase step from pulse to pulse is zero at every pitch). The restart
phases are not all zero; the library uses zero, since the score is spectral.

**Output stage.** Image levels from 6.5 to 23.5 kHz, after removing the
hold's sinc, fall off like two real poles near 2.7 kHz (`LPF_FC`, `LPF_Q`
0.25). Under every tone the chip's output carries a flat noise floor; a
multiplicative floor of 8 % (`FLOOR`) scored best, for the vowels too.

**Noise.** The unvoiced sounds are flat bands with sharp edges (SE, SO, TT
and TS peak at 4 kHz, the chip's Nyquist). An oscillator whose frequency is
thrown about uniformly by ±nw Hz every sample (the manual's register 6,
"disordered with white noise") makes exactly that: a flat band of width 2·nw
centred on nf, and when thrown past Nyquist it folds back into a peak there.
Each noise sound is a centre, a throw and a level.

**Length.** A sound's length does not depend on whether it is streamed, sent
alone, or followed by pauses (VoxProbe 5 sections 2–4 agree to 5 ms), nor on
Pitch. It is not the manual's length: vowels play about twice it, diphthongs
1.3×, the K stops 0.75×. So each allophone carries its measured length at
Speed 114. Speed is linear: length = L114 · (162 − Speed) / 48 (AW: 95, 197,
287 ms at Speed 127, 90, 60). Fast is ×0.8 and Slow ×1.25 (the manual says ½
and 1½). Each further copy of a Repeat adds about the manual's length. A sound
followed at once by another sound is cut short (`OVERLAP`): SE before IY is
80 ms where SE alone is 100.

**Level over time.** Every sound ramps in over about 35 ms and out over 30
(`ATTACK`, `RELEASE`; the effects 10 and 15).

**Bend** moves every oscillator by the same +44 Hz per step (D1's 697 and
1209 both step by 44, as do AW's three formants); below 5 there is a 22 Hz
jump. **Volume** 16/32/64/96/127 gives −3.3/−1.3/−0.6/0/+2.5 dB. **Stress**,
**Relax** and code 18 change nothing measurable in length, pitch or level.

**Stops.** A closure of silence (`CLOSURE` 30 ms; a voiced stop hums through
it on its first formant, quietly), then the burst and release, which is the
measured length.

**Effects.** Each is one to six parts: up to three tones sweeping from a start
to an end frequency (linearly, geometrically, or settling), a noise band, a
gate period for the pulsed ones, and flags for decay (exponential, 35 dB),
a half-sine bump, and a per-gate noise sweep (C8, C9). The robots are a plain
tone at 800 + 300·n Hz over a voiced glide; the DTMF tones are the standard
pairs. The recipes were written from the recordings' spectrograms and then
fitted.

## Fitting

The allophone and effect parameters were first read off the recordings
(formant peaks, band edges, lengths) and then refined by Nelder-Mead on the M
score (the shared scorer, unchanged): each sound against its
VoxRecord items on both units, and the global constants against the
connected-speech groups, the Man Goes Down phrases and the VoxProbe pairs.
The score compares the library to the chip the way two chips compare to each
other, so 0 is "as close as a second chip".

## What is not modelled

Restart phases of the formants (zero here; the chip's are fixed but not
zero); the Stress and Relax commands; Wait, the port commands and the EEPROM
phrases; the demo mode; the Serial Control Protocol.
