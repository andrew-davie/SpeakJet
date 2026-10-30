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

**The hardware.** The SpeakJet is a Microchip PIC18F1320 with its own
firmware. With no crystal it runs on the PIC's internal 8 MHz oscillator,
calibrated to ±2 %: 2 million instructions a second, so a nominal 8000 Hz
rate is 250 instruction cycles a sample. The two units' rates above are 0.5
and 0.2 % fast, each its own oscillator's error, and every length, pitch and
tone scales with it together. The steady 15.7 kHz line in every recording is
the Atari 2600's scanline rate (3,579,545 Hz / 228), identical on both units,
not the chip.

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
them: the envelope steps to full at once and falls linearly over about 7 ms
(`ENV_RISE` 0.5, `ENV_FALL` 58 ticks). There is no rise: with the formants,
levels and phases held from the body of the pulse, the envelope's value at each
of the first eight ticks was fitted free, and on both units it is zero for a few
ticks (the gate's position in the period) and then 0.8–1.0 at once; a linear
8-tick rise had been the best of the shapes tried before that measurement, and
it left the valleys between the formants 10–16 dB too shallow. Every formant
restarts each period (the phase step from pulse to pulse is zero at every
pitch), and all three restart at one phase: a fit with one shared phase per
sound beats a fit with three free ones on both units, and the value is the
same for every sound (so the library restarts at zero and stores none).

**The offset.** Beside the sines, every voiced sound's pulse carries an
envelope-shaped term: the same step and fall, no oscillation, a level of its
own per sound (up to 1.3 times the loudest formant on UH, LE, RR; a tenth of
it on AX, AY, EH), the same on both units, and the sign varies. It is not the
recording chain (no high-pass corner reproduces it), not a fixed mid-level
(its size differs five-fold between sounds), and a free fourth oscillator
fitted to replace it runs to zero frequency. The library carries it as a
fourth oscillator at 0 Hz: one signed byte per sound (`dc`). With the
envelope, the shared phase and the offset, the library's pulse for the
cleanly measured vowels is within 1 dB of the chip's in every sixth-octave
band from 150 Hz to 1.3 kHz, valleys included, and the residual is 0.09–0.15
of the pulse's rms (0.015 is the chip's own pulse-to-pulse scatter).

**Output stage.** Two real poles, near 2 kHz and 2.9 kHz (`LPF_FC` 2396,
`LPF_Q` 0.491). The first comes from the forward model of the voicing pulses
with free poles (1983 and 1972 Hz on the two units); the pulses say almost
nothing above 4 kHz, so the second comes from the image levels of the tones
from 6.5 to 23.5 kHz, after removing the hold's sinc, with the first pole held:
2.9 kHz fits them to 2.3–2.6 dB rms on both units, 12 kHz to 9–10 dB. (An
earlier version used 720 Hz/10 kHz from the images alone, which they fit
equally well, with the formant levels fitted under it compensating for it.)
Under every tone the chip's output carries a flat noise floor (`FLOOR`,
measured 1.8 %).

**Noise.** The chip's hiss is random noise, Gaussian to the sample on both
units (kurtosis 2.8–3.2, 4–5 % of samples beyond two sigma), not a tone
whose frequency is thrown about (that has a constant envelope: kurtosis
1.9, nothing beyond two sigma). So the noise is white noise through a
two-pole resonance at the tick rate, centre nf and bandwidth nw, with its
gain holding the output power constant whatever the band. The bands are
read off the chip's 1/6-octave spectra: FF, TH and HE are flat from 150 Hz
to the stage's rolloff; SH and CH peak near 2.8 kHz; WH at 860 Hz; SE, SO,
TS and ZZ at the top of the band (a resonance at Nyquist); PE and PO are
low-passed. The voiced fricatives' bands are the floor between the voice's
harmonics (VV 1.5 kHz, DH 2 kHz, ZH 2.4 kHz, JH 2.5 kHz). Levels are
matched to the chip's rms. CH is unvoiced (no periodicity at the pitch
period on either unit). Each noise sound is a centre, a bandwidth and a
level.

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

**Effects' levels.** Each effect part carries its level the same way,
measured against the voice on both units; after it every recorded effect
is within a decibel of the chip. An effect's gate dips 8 dB in its off
half, not to silence (C8, C9, both units).

**Polarity.** The voice's pulse starts downward on both units (every vowel
at Pitch 20).

**Levels.** Each sound carries its level as a half-decibel byte (an
effect, on every part alike), set so that its level against the mean of the
twelve steady vowels is the chip's: both VoxRecord units, 150 Hz high-pass on
both sides, the recording's background power taken off; the two units agree
within half a decibel on nearly every sound, and every measured sound is
within half a decibel of them (glides follow their two sounds). The scorers
take each recording at its own level, so fitting can move a sound's loudness
freely; it is set back after every refit. The formant and noise amplitudes
keep their measured balance; the byte scales them.

**No output quantiser.** Both chips put out their quiet consonants and the
stops' tails whole, far below a 48th of full scale; a quantiser there
silenced them.

**Volume.** 16 is 15 dB under 32; from 32 up the level barely moves (-1.3,
-1.1, 0, +2.2 dB at 32, 64, 96, 127), measured on both units at Pitch 20.

**Closure.** A stop's closure is the sound before fading away through it,
scaled with Speed: both units fall 30 dB over 50 to 60 ms at Speed 114 and
twice that at Speed 60, with no cut to silence.

**Pulsed effects.** At Pitch 20 each pulse stands alone: C0 and M2 are
silent between pulses, so every tone and the noise of such an effect pulse
with the voice (the `A` flag); M0 is continuous, plain tones.

**An effect's own pulse rate.** Measured pulse by pulse on VoxProbe 4 (Pitch
60, 88 and 150, both units), an effect's pulses run at Pitch plus an offset
of its own, the same number of hertz at every Pitch and untouched by Bend,
gliding within a part: C2 Pitch-22 throughout; C3 -22 falling to -42 as its
tone rises; C1 +386 at 80 ms, falling 2 Hz/ms; C6 -22 rising to +43 and
back; C7 0 rising to +155; C0 the voice's rate falling to -22. Each part
carries the offset at its start and end (`pa`, `pb`, Hz), read at every
pulse; the period is computed from integers and one division so every
compiler gets the same tick. Each tone under the pulse restarts with it; the
lines are that rate's harmonics.

**The envelope oscillator.** The manual's synthesizer has a sixth oscillator
that only envelopes the other five, in one of four waveforms (register 8):
saw, sine, triangle, square. The saw is the voice's measured pulse; a part
can pick the others (flags `i`, `t`, `q`). The robots' low lines are a tone
at f and its octave and nothing above, with the carrier one clean line: a
tone at f under a sine or square envelope at f, the carrier running on beside
it (a voiced part's plain third tone does not restart with the pulse).

**Tones end in a fade.** Every tone effect holds its level and then falls
straight to nothing over about 55 ms, linear in amplitude (-6 dB at half
way, -20 dB at nine tenths), on both units: a part flag (`F`) does that,
and the tones' tables carry the measured hold followed by a 55 ms fading
part. B0 is 2600 Hz; B3 is 598 + 3105 Hz at -8 dB for 100 ms then 1400 +
600 Hz; B7 is 2906 Hz for 40 ms, a 10 ms fade and a 5 ms gap, then 3908 Hz
at -6 dB. The chip holds these tones constant where the earlier fit had
them drifting or sweeping.

**DTMF.** D0 to D11 are the standard DTMF pairs (697, 770, 852, 941 Hz by
1209, 1336, 1477 Hz, in keypad order with 0, * and # on the bottom row) at
equal amplitude, held about 155 ms. Measured on both units within 3 Hz of
the standard once each unit's rate is divided out, so the rule carries the
standard values and the rate scaling does what the chip's does. B0 is 2600
Hz for 170 ms.

**Bend** moves every oscillator by the same +44 Hz per step (D1's 697 and
1209 both step by 44, as do AW's three formants); below 5 there is a 22 Hz
jump. **Volume** 16/32/64/96/127 gives −3.3/−1.3/−0.6/0/+2.5 dB. **Stress**,
**Relax** and code 18 change nothing measurable in length, pitch or level.

**Stops.** A closure (`CLOSURE` 30 ms) the sound before fades through, a
voiced stop humming through it on its first formant at full level: both
units open BE, BO, DE, DO, GE and GO on a voice bar 2-9 dB under the release.
Then the burst and release, which is the measured length. Each Repeat copy
of a stop is the stop again: a closure (`COPYCL` 20 ms) then the release, the
whole copy lasting the sound's own copy length, measured onset to onset on
both units (VoxProbe 4 and 7, the 12 voiced stops at 2 to 5 copies): BE 75,
BO 70, DE 60, DO 55, GE and GO 65, EG and OG 55, OB 60, EB, ED and OD 45 ms.
GE, GO and the final stops close silently between copies (27-35 dB dips,
33-50 dB under 600 Hz); BE, BO, DE and DO keep their voice bar through it
(about 22 dB dips).

**Effects.** Each is one to six parts: up to three tones sweeping from a start
to an end frequency (linearly, geometrically, or settling), a noise band, a
gate period for the pulsed ones, and flags for decay (exponential, 35 dB),
a half-sine bump, and a per-gate noise sweep (C8, C9). The robots are a plain
carrier at 800 + 300·n Hz beside a low tone and its octave (see the envelope
oscillator); the DTMF tones are the standard pairs. The recipes were written from the recordings' spectrograms and then
fitted.

## Fitting

The allophone and effect parameters were first read off the recordings
(formant peaks, band edges, lengths) and then refined by Nelder-Mead on the M
score (the shared scorer, unchanged): each sound against its
VoxRecord items on both units, and the global constants against the
connected-speech groups, the Man Goes Down phrases and the VoxProbe pairs.
The score compares the library to the chip the way two chips compare to each
other, so 0 is "as close as a second chip".

Since then each sound's numbers are annealed inside its measured structure
(parts, tones and flags fixed, every number within bounds about its measured
value), on the E (spectrum) and P (length, envelope, time-frequency, spectrum,
lines, pitch) scores together, and the sound's level then set back to the
chip's.

## What is not modelled

Restart phases of the formants (zero here; the chip's are fixed but not
zero); the Stress and Relax commands; Wait, the port commands and the EEPROM
phrases; the demo mode; the Serial Control Protocol. The output's second
harmonic, about 40 dB under a tone on both units (B0).
