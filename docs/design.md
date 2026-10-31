# Design

This SpeakJet emulation (branch `rebuild`) was written from scratch from two
sources only: the SpeakJet User's Manual (revision 1.1, 2004) and recordings
of two AtariVoxes (an AtariVox+, "unit A", and an original AtariVox, "unit
B") playing the VoxRecord and VoxProbe 1 to 7 test ROMs and Man Goes Down.
The aim is the smallest model that matches the recordings: a rule wherever a
rule could be found, and a few bytes of parameters per sound where it could
not.

The library is `speakjet.c` (about 1000 lines) and a generated table,
`speakjet_tables.c`, about 2.6 KB compiled: 72 allophones of 14 bytes, a
second frame for the five voiced stops that have their own, and the effects
as a byte stream of 146 parts, each its length, a byte saying which fields
follow, its flags and level, then only the tones and fields it uses (most
parts have no noise, gate, pulse offset or ramp). Frequencies are in 16 Hz
steps and lengths in 5 ms. Compiled with `-O2` the object is about 18 KB. The
measured constants are the fields of `struct tune` in `speakjet.c`, named
below in `code`.

## The chip

**Sample rate.** A tone made at rate Fs and held between samples has images
at k·Fs ± f. From the images of the DTMF tones: unit A 8039 Hz, unit B 8013
Hz. The library uses their mean, `RATE` 8026. The manual's 8192 is nominal.

**The hardware.** The SpeakJet is a Microchip PIC18F1320 with its own
firmware. With no crystal it runs on the PIC's internal 8 MHz oscillator,
calibrated to ±2 %: 2 million instructions a second, so a nominal 8000 Hz
rate is 250 instruction cycles a sample. The two units' rates are 0.5 and
0.2 % fast, each its own oscillator's error, and every length, pitch and tone
scales with it together. The steady 15.7 kHz line in every recording is the
Atari 2600's scanline rate (3,579,545 Hz / 228), identical on both units, not
the chip.

**The chip counts in 8000ths.** Tones programmed as 697 and 1209 Hz come out
at exactly Fs/8000 times that on both units, so the oscillators' phase
increments are computed for a nominal 8000 Hz tick (`NOMINAL`), while the
real tick is 1/8026 s.

**Output stage.** The samples are held between ticks, then pass two poles at
1609 Hz, Q 0.462 (`lpf_fc`, `lpf_q`). Fitted to tones whose level the chip
sets directly: the 12 DTMF pairs' level ratios and VoxProbe 7's single
oscillator at 125, 1000 and 3000 Hz (Serial Control Protocol, volume 31), all
14 within 0.45 dB on both units. The same direct tones show that the
oscillator volumes and the master volume (register 7) are exactly linear
(both units, to 0.1–0.4 dB), and that oscillators 4 and 5 sit 6 dB under 1
to 3. There is no output quantiser (`quant` 0): both chips put out their
quiet consonants and the stops' tails whole, far below a 48th of full scale.

## The voice

**Voicing period.** From VoxProbe 2 (AW at every Pitch 0–254, both units):
the period is an integer number of ticks, N = floor(Fs / Pitch) + 1, so f0
plateaus (Pitch 157–160 all give 157.6 Hz).

**The voicing pulse.** VoxProbe 3 holds 18 vowels, nasals and liquids at
Pitch 40, where each pulse stands alone. Averaged pulses are identical on
both units to the sample. Fitting a forward model (formant sines restarting
at the period, under a shared envelope, through the hold and the output
stage) to them: the envelope steps to full at once and falls linearly over
about 7 ms (`env_rise` 0.5, `env_fall` 58 ticks). With the formants, levels
and phases held from the body of the pulse, the envelope's value at each of
the first eight ticks was fitted free; on both units it is zero for a few
ticks and then 0.8–1.0 at once. Every formant restarts each period (the phase
step from pulse to pulse is zero at every pitch), all three at one phase, the
same for every sound. Swept across every steady vowel and nasal on both units
(the harmonics' mean error against the chip), zero is best: 2.01 dB, rising
to 2.16–2.19 at ±0.025 cycle and 4.5 at 0.1; half a cycle only flips the
sign. The pulse starts downward on both units (every vowel at Pitch 20).

**No offset.** The library's frame still has room for an envelope-shaped
term beside the sines at 0 Hz, but every sound's value is zero: in the raw
spectra the chip has no fundamental energy from it (UH's first harmonic is
−10 dB on the chip).

**The voice is exactly periodic.** At Pitch 88 every vowel and nasal's period
holds to one 48 kHz sample on both units, and the gaps between harmonics sit
30–60 dB under them. There is no noise floor under the voice and no jitter in
the formants (`floor` and `jitter` 0; at 0.05 and 0.03 they filled those gaps
to 15–30 dB).

**Allophones.** Each is a kind (vowel, nasal, glide, voiced fricative,
fricative, stop, voiced stop, affricate), three formants (frequency and level
0–31), a noise band (centre, half width, level), its length, its Repeat copy
length and a level byte. A glide carries the two allophones it runs between:
the first frame holds for a fifth of the sound, the move takes the next seven
tenths, the second frame holds the last tenth. The voice moves to a new
sound's frame over 25 ms (`transition`); after silence it starts at the
sound's own formants with no level, instead of sweeping up from 0 Hz.

**Noise.** The chip's hiss is random noise, Gaussian to the sample on both
units (kurtosis 2.8–3.2, 4–5 % of samples beyond two sigma), not a tone whose
frequency is thrown about (that has a constant envelope: kurtosis 1.9,
nothing beyond two sigma). So the noise is white noise through a two-pole
resonance at the tick rate, centre and bandwidth per sound, with its gain
holding the output power constant whatever the band. Read off the chip's
spectra: FF is flat to the stage's rolloff; SE, TS and ZZ resonate at the top
of the band (about 3.8–4 kHz), SO at 3.6 kHz; SH at 2.9 kHz, CH 3.1, JH 2.8,
TH 2.3, ZH 2.2, DH 2.0, HE 1.7, VV 1.6 kHz; WH at 800 Hz; PE and PO are
low-passed. CH is unvoiced (no periodicity at the pitch period on either
unit).

**Stops.** A closure (`closure` 30 ms, scaled with Speed) in which the sound
before fades away (both units fall 30 dB over 50–60 ms at Speed 114 and twice
that at Speed 60, with no cut to silence); a voiced stop hums through it on
its first formant: both units open BE, BO, DE, DO, GE and GO on a voice bar
2–9 dB under the release. Then the burst and release, which is the measured
length.

## Time

**Length.** A sound's length does not depend on whether it is streamed, sent
alone, or followed by pauses (VoxProbe 5 sections 2–4 agree to 5 ms), nor on
Pitch. It is not the manual's length: vowels play about twice it, diphthongs
1.3×, the K stops 0.75×. So each allophone carries its measured length at
Speed 114. Speed is linear: length = L114 · (162 − Speed) / 48 (`speed_a`,
`speed_b`; AW: 95, 197, 287 ms at Speed 127, 90, 60); the effects shorten
faster, (147 − Speed) / 33 (`fx_speed_a`, `fx_speed_b`), fitted on every
effect recording at another Speed (technical manual, section 7). Fast is ×0.77 and
Slow ×1.22 of the played length, stops unchanged (`fast`, `slow`; VoxRecord,
both units; the manual says ½ and 1½).

**Joins.** A sound followed at once by another keeps the voice running: no
release on it and no attack on the next; it is cut short by 15 ms, 30 when
both are voiced (`overlap`, `overlap_v`). Pauses, effects and silence keep
the ramps. The look-ahead steps over Fast, Slow, Stress, Relax and the
two-byte settings, so a pitch byte between words doesn't break a join.

**Repeat.** Each copy lasts the sound's own copy length (a byte per
allophone), measured onset to onset where VoxProbe 4 and 7 repeat it, and
the manual's length elsewhere. A stop's copy is the stop again: a closure
(`copy_closure` 20 ms) then the release. Measured on both units (VoxProbe 4
and 7, the 12 voiced stops at 2 to 5 copies): BE 75, BO 70, DE 60, DO 55, GE
and GO 65, EG and OG 55, OB 60, EB, ED and OD 45 ms. GE, GO and the final
stops close silently between copies (27–35 dB dips, 33–50 dB under 600 Hz);
BE, BO, DE and DO keep their voice bar through it (about 22 dB dips). A
glide's copy glides back from the last copy's end to its first frame and on
(IYRR ×5 rises 1383 → 2083 Hz over its first 60 ms each time on both units);
the glides' copies run 185–290 ms. JH's copy is 80 ms, ZH's 70.

**Level over time.** A sound after silence ramps in over 35 ms and out over
30 (`attack`, `release`). An effect's tone starts within 1–3 ms on both units
(`fx_attack` 1 ms, `fx_release` 15).

## Levels

**Each sound's level.** Each sound carries its level as a half-decibel byte
(an effect, a byte per part), set so that its level against the mean of the
twelve steady vowels is the chip's: both VoxRecord units, 150 Hz high-pass on
both sides, the recording's background power taken off. The two units agree
within half a decibel on nearly every sound. The scorers take each recording
at its own level, so fitting can move a sound's loudness freely; it is set
back to the chip's before every refit is judged.

**Volume.** The Volume command: 16 is −15.5 dB, 32 −1.3, 64 −1.1, 96 0, 127
+2.2 (speech at Pitch 20, both units). That isn't linear, though the master
volume register is; whether Volume is that register, with a level-dependent
stage after it, is open.

## Effects

**Parts.** Each effect is one to eight parts. A part plays up to three tones,
each sweeping from a start to an end frequency (linearly; `X` geometrically;
`E` settling toward the end), and a noise band, for its length. Flags:

- `V`: the first two tones pulse with the voice and the third runs plain
  beside them; `o`: only the first pulses; `A`: every tone and the noise
  pulse (C0 and M2 are silent between pulses at Pitch 20, both units); `P`:
  the plain tones pulse with the voice's envelope but run on through it
- `i`, `t`, `q`: the pulse's envelope is a sine, triangle or square instead
  of the voice's saw
- `D`: the level decays 35 dB over the part (`decay_db`); `B`: a half-sine
  bump; `F`: a fade, below
- a gate period for the gated ones: its off half dips 8 dB, not to silence
  (`gate_depth` 0.4; C8, C9, both units); `S`: the noise band sweeps with the
  gate (C8 and C9: 650 Hz to 3 kHz and back every 20 ms, both units)
- `W`: the tone wanders in pitch, about 1.5 % rms over tens of ms (M0, both
  units)

A part may also carry a level ramp across it (`lr`, half-decibels, even in
dB) and an end level per tone (`ve`), each used only where the chip's part
measurably changes level (R2, A0, A7, B1, C8).

**An effect's own pulse rate.** Measured pulse by pulse on VoxProbe 4 (Pitch
60, 88 and 150, both units) and VoxProbe 7 (Pitch 40 to 200): an effect's
pulses run at Pitch plus an offset of its own, the same number of hertz at
every Pitch and untouched by Bend, gliding within a part. Each part carries
the offset at its start and end (Hz), read at every pulse; the period is
computed from integers and one division so every compiler gets the same
tick. Each tone under the pulse restarts with it, so the lines are that
rate's harmonics.

**The envelope oscillator.** The manual's synthesizer has a sixth oscillator
that only envelopes the other five, in one of four waveforms (register 8):
saw, sine, triangle, square. VoxProbe 7 drove it directly on both units: the
register-set envelopes run at exactly their frequency and don't restart the
tones, the triangle is a rising ramp and then an instant drop, and the saw's
period is floor(8000 / f) + 1 ticks. An effect's own envelope does restart
its tones; R5's lines are its envelope's harmonics. The robots (R0 to R9) are
a carrier at about 800 + 300·n Hz beside a low tone and its octave: a tone
at f under a sine or square envelope at f, the carrier running on beside it.

**Steady tones.** A plain tone holds its level: 0.01–0.09 dB rms in its
middle on the DTMF tones, B0, B3, R7 and R9 (`AM_STEADY`); only a wandering
part (M0) also wobbles in level.

**Tones end in a fade.** Every tone effect holds its level and then falls
straight to nothing over about 55 ms, linear in amplitude (−6 dB at half
way, −20 dB at nine tenths), on both units: a part flag (`F`) does that, and
the tables carry the measured hold followed by a 55 ms fading part. B0 is
2600 Hz for 120 ms then the fade; B7 is 2906 Hz for 40 ms, a 10 ms fade and a
5 ms gap, then 3908 Hz.

**DTMF.** D0 to D11 are a rule: the standard DTMF pairs (697, 770, 852, 941
Hz by 1209, 1336, 1477 Hz, in keypad order with 0, * and # on the bottom row)
at equal amplitude, held about 155 ms with the fade. Measured on both units
within 3 Hz of the standard once each unit's rate is divided out, so the rule
carries the standard values and the rate scaling does what the chip's does.

**The effects' bed.** An effect's tones sit over a noise bed. The library makes
it from the signal's change per tick (`SLEW`), so it grows with the tone's
frequency. That rule is not what the chip does: a steady tone set directly
through the Serial Control Protocol has a bed about −75 dB per bin under it
at 125, 1000 and 3000 Hz alike on both units, 15–27 dB under the library's,
and the effects' excess over the chip doesn't follow their frequency either.
But a bed proportional to the signal, set to the steady tones, scores far
worse on the effects (all of it spectrum and line width): the effects carry
much more bed than a steady tone, which SLEW's level roughly gives. Not yet
understood.

## Commands

**Bend** moves every oscillator by the same 44 Hz per step from 5 (D1's 697
and 1209 both step by 44, as do AW's three formants; `bend_hz`); below 5
there is a further 22 Hz. **Stress**, **Relax** and code 18 change nothing
measurable in length, pitch or level. The pauses are the manual's but P1 and P4,
played as 90 and 15 ms (measured, VoxProbe 8).

**Reading ahead.** At a sound's release (an effect's closing fade, else its last
`READ_AHEAD_MS`, 30 ms) the library takes the command after it early, if it is a
Speed or a Volume (`read_ahead`): Volume applies at once, and Speed rescales the
rest of the sound's segments. The command is then retired with the sound. Pitch
and the modifiers are left to run in their turn, and a pause doesn't read ahead.

## Fitting

The allophone and effect numbers were first read off the recordings (formant
peaks, band edges, lengths, levels) and refined against the recordings with
two scores, each comparing the library with the chip the way the two chips
compare with each other, so 0 is "as close as a second chip": E, on the
spectrum and the harmonics' levels, and P, on length, the level over time,
the time-frequency picture, spectrum, lines and pitch. Both add the
recording's own background to the library's render before comparing.

Each sound's numbers are annealed inside its measured structure (kind,
lengths, parts, tones and flags fixed; formants within ±10 % and kept in
order, levels within a factor of 2, the level byte within ±6 dB), on its own
recordings, its probe recordings and the connected recordings containing it.
A result is judged with its level set back to the chip's, and ships only if
it gains on one score without losing beyond a small margin on the other: an
allophone must gain 0.1 on E with P (alone and in connected speech) no more
than 0.05 and 0.02 worse; an effect must gain 0.1 on E with P no more than
0.02 worse, or 0.1 on P with E no more than 0.05 worse, and be no more than
0.2 worse on the plain spectrum. Every
ship rebuilds the C library and the Go port and checks that they render the
same samples.

## What is not modelled

The Serial Control Protocol; Wait, the port commands and the EEPROM phrases;
the demo mode; the Stress and Relax commands; the mixer's limit of 63 on the
three oscillators' volumes (`clip` 0); the output's second harmonic, about
40 dB under a tone on both units (B0; `dist2`, `dist3` 0). Known to be off:
the effects' bed (above), and the nasals' pulse: at Pitch 40 NGE's first
formant dies away more slowly than the library's and its skirt ripples every
300 Hz, NGO's falls sooner and then beats, while LO matches the chip to a
decibel.
