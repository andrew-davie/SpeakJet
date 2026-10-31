# The SpeakJet: a technical manual

What the SpeakJet speech chip actually does, as measured on two real AtariVoxes
(an AtariVox+, "unit A", and an original AtariVox, "unit B"). The SpeakJet
User's Manual (Magnevation, revision 1.1, 2004) describes the chip's interface
and its intended design. This document describes its behaviour: how each sound
is formed, how long it lasts, what every command changes, and where the chip
departs from the manual. Every statement here was measured on both units unless
it says otherwise; open questions are marked as such.

The measurements come from test ROMs played on the two units and recorded from
the AtariVox's audio output at 48 kHz: VoxRecord (every allophone and effect,
connected groups), VoxProbe 1 to 7 (pitch, pulse shape, lengths, repeats,
effects at several Pitches and Bends, the synthesizer driven directly) and the
phrases of Man Goes Down. The library in this repository (`docs/design.md`) is
the model built from them.

## 1. The hardware

**The chip.** The SpeakJet is a Microchip PIC18F1320 microcontroller running
Magnevation's firmware. It has no crystal: it runs on the PIC's internal 8 MHz
RC oscillator, factory-calibrated to about ±2 %, giving 2 million instructions a
second.

**The sample rate.** The firmware produces one output sample per "tick", at a
nominal 8000 ticks a second (250 instruction cycles each). The real rate is the
RC oscillator's: unit A runs at 8039 Hz and unit B at 8013 Hz (0.5 % and 0.2 %
fast), measured from the images of steady tones. Everything the chip does,
lengths, pitches and tone frequencies alike, scales with that one clock, so
two chips differ slightly in pitch and tempo together. The manual's 8192 Hz is
not the real rate.

**Arithmetic in 8000ths.** The firmware computes its oscillator steps for a
nominal 8000 Hz tick: a tone programmed as 697 Hz comes out at 697 × Fs/8000.
So frequencies are exact relative to the chip's own clock and off by that
clock's error in absolute terms.

**The output.** Each sample is held until the next (so a tone at f also has
images at k·Fs ± f, audible above 4 kHz on full-range speakers at about -30 dB),
then passes an analogue low-pass stage that behaves as two poles at about
1609 Hz with Q 0.462. That figure fits tones the chip sets directly (the twelve
DTMF pairs' level ratios and a single oscillator at 125, 1000 and 3000 Hz) to
within 0.45 dB on both units. There is no audible output quantisation: the
quietest consonants and the tails of stops come out whole.

**Not the chip.** A steady line at 15.7 kHz in every recording is the Atari
2600's horizontal scan rate (3,579,545 / 228 Hz), identical on both units.

**Two units, one firmware.** The two units behave identically apart from their
clock rates: the same lengths, the same pulse shapes to the sample, the same
levels to about half a decibel.

## 2. The synthesizer

The manual describes the chip's synthesizer as five oscillators, an envelope
oscillator, a noise source on oscillators 4 and 5, and a mixer, all controlled
by registers that the Serial Control Protocol ("\0", "nJ", "vN", "X") can write
directly:

```
register   function (manual)                        measured
0          envelope frequency                       the envelope runs at exactly f
1-5        oscillator frequencies                   in 8000ths (above)
6          distortion (noise) on oscillators 4, 5
7          master volume                            linear: 127, 64, 32, 16, 8 give
                                                    +2.4, -3.5, -9.5, -15.5, -21.6 dB
                                                    re 96, to 0.1 dB
8          envelope control: bits 0-1 waveform      saw, sine, triangle, square
           (saw, sine, triangle, square); bit 6
           envelopes oscillators 1-3; bit 7
           half-envelopes 4 and 5
11-15      oscillator volumes 0-31                  linear: 16, 8, 4, 2 give -5.7,
                                                    -11.8, -17.9, -24.2 dB re 31
```

- Oscillators 1, 2 and 3 are equally loud; oscillators 4 and 5 sit 6 dB under
  them at the same volume.
- **The envelope waveforms.** The saw is the voice's own pulse (below). The
  triangle is a rising ramp followed by an instant drop. The saw's period is
  floor(8000 / f) + 1 ticks. An envelope set through the registers runs on
  without restarting the tones under it.
- **The bed under a steady tone.** A single oscillator set directly carries a
  faint noise bed about 75 dB per 6 Hz bin under the tone, much the same at
  125, 1000 and 3000 Hz.
- The manual's mixer limit (oscillators 1-3 totalling 63 or less) was probed
  but is not yet characterised.

## 3. The voice

Vowels, nasals, liquids, glides and the voiced consonants share one voice.

**Pitch.** The voice's period is a whole number of ticks:
N = floor(Fs / Pitch) + 1. So f0 steps rather than glides: Pitch 157 to 160 all
give 157.6 Hz. The period holds to one 48 kHz sample from pulse to pulse: the
chip's voice is exactly periodic, with no jitter, and the gaps between its
harmonics sit 30-60 dB under them.

**The pulse.** Each period, three formant sines restart at the same phase and
sound under an envelope that steps to full at once and then falls linearly to
nothing over about 7 ms (58 ticks). There is no rise: measured tick by tick on
isolated pulses (Pitch 40), the envelope is zero for a few ticks and then
0.8-1.0 at once. The pulse starts downward on both units. All three formants
restart at one phase, the same for every sound, and a zero phase fits best.
The averaged pulses of the two units are identical to the sample.

**Formants.** Each voiced sound is three formant frequencies with a level each
(0 to 31), which is why every vowel's spectrum is a comb of harmonics shaped by
three peaks and the skirts of the envelope. There is no separate fundamental
or DC component: the chip's raw spectra carry no extra energy at the voice
fundamental.

**Noise.** The hiss is random noise, Gaussian to the sample (kurtosis 2.8-3.2,
4-5 % of samples beyond two sigma), not a tone whose frequency is thrown
about (that has a constant envelope). It behaves as white noise through a
two-pole resonance at the tick rate, each sound with its own centre and
bandwidth: FF is flat to the output stage's roll-off; SE, TS and ZZ resonate
at the top of the band (about 3.8-4 kHz), SO at 3.6 kHz, SH at 2.9, CH 3.1, JH
2.8, TH 2.3, ZH 2.2, DH 2.0, HE 1.7, VV 1.6 kHz, WH at 800 Hz; PE and PO are
low-passed. The voiced fricatives are the voice and a noise band together; CH
is unvoiced.

## 4. Allophones

**Lengths.** A sound's length does not depend on what comes before or after it,
on whether it is streamed or sent alone, or on Pitch (VoxProbe 5, to 5 ms). It
is not the manual's length: vowels play about twice it, diphthongs 1.3 times,
the K stops 0.75 times. Speed is linear: length = L114 · (162 - Speed) / 48,
where L114 is the length at the default Speed 114 (AW: 95, 197 and 287 ms at
Speed 127, 90 and 60).

**Fast and Slow.** Fast plays the next sound at about 0.77 of its length and
Slow at about 1.22, and neither changes a stop. That figure comes from single
sounds; phrases suggest Slow stretches vowels, nasals and diphthongs more
(about 1.3-1.5) and hisses less. *Open: it needs measuring sound by sound.*

**Moving between sounds.** The voice moves to each new sound's formants over
about 25 ms. After silence it starts on the new sound's own formants (no sweep
up from 0 Hz).

**Joins.** Inside a run of sounds the level never fades between them (the chip's
level drops no more than 14-30 dB between sounds in a phrase): the voice runs
on. A sound followed at once by another is cut short, by about 15 ms, or 30 ms
when both are voiced. Only the edges of a run ramp: about 35 ms in and 30 ms
out. Pauses and effects end a run. The "followed at once" test steps over
modifier and setting bytes (Fast, Slow, Stress, Relax, Pitch, Speed, Volume,
Bend, Repeat) placed between sounds.

**Glides.** The sixteen diphthong-like allophones (IYRR, EYIY, AYWW...) run from
one allophone's frame to another's, levels included: IYRR starts on IY and ends
on RR. The first frame holds for a fifth of the sound, the move takes the next
seven tenths, and the second frame holds the last tenth.

**Levels.** Each sound has a level of its own relative to the vowels, the same
on both units within half a decibel.

## 5. Stops

**Closure.** Before a stop, the sound before fades away through the closure
(both units fall 30 dB over 50-60 ms at Speed 114, twice that at Speed 60); it
is not cut to silence.

**Voiced stops** (BE, BO, DE, DO, GE, GO, and the final ones) hum through the
closure on their first formant: BE, BO, DE, DO, GE and GO open on a voice bar
2-9 dB under the release. The first formant falls into the closure. Some
release toward formants of their own (BO, EB, OB, ED, OD).

**Burst and release.** Then the burst and release, which is the stop's
measured length.

## 6. Repeat

Repeat N plays a sound N times. Each further copy lasts the sound's own copy
length, measured onset to onset; it is not the first play's length and not the
manual's.

- **Stops.** Each copy is the stop again: a closure of about 20 ms, then the
  release. Copy lengths: BE 75, BO 70, DE 60, DO 55, GE and GO 65, EG and OG 55,
  OB 60, EB, ED and OD 45 ms. GE, GO and the final stops close silently between
  copies (27-35 dB dips); BE, BO, DE and DO keep their voice bar through it
  (about 22 dB dips).
- **Glides.** Each copy glides back from where the last ended to the first
  frame and on again (IYRR × 5 rises 1383 → 2083 Hz over its first 60 ms each
  time). Their copies run 185-290 ms.
- **Others.** JH's copies are 80 ms, ZH's 70; the vowels' copies run on
  seamlessly.

## 7. Sound effects

The 55 effects (codes 200-254) are built from the same parts as speech, played
as one to eight "parts" in sequence. Each part plays up to three tones and a
noise band for its length.

**Tones.** A tone holds its pitch (0.03-0.13 % rms on the DTMF tones and the
robots' carriers) and its level (0.01-0.09 dB rms in its middle). A tone starts
within 1-3 ms. Tones may sweep, linearly, geometrically, or settling toward an
end frequency. At the end of a tone effect the level holds, then falls
straight to nothing over about 55 ms, linear in amplitude (-6 dB halfway, -20
dB at nine-tenths).

**Pulsed effects.** Many effects pulse: their tones restart at a pulse rate,
like the voice, so their spectra are combs of that rate's harmonics. The pulse
rate is the Pitch plus an offset of the effect's own, the same number of hertz
at every Pitch (measured at Pitch 40, 60, 88, 150 and 200) and untouched by Bend,
gliding within a part: C2 runs at Pitch - 22; C3 at Pitch - 22 falling to - 42
as its tone rises; C1 at Pitch + 386, falling 2 Hz per ms; R5 at Pitch + about
295. Some effects pulse every tone and their noise (C0 and M2 are silent
between pulses at Pitch 20); others pulse one or two tones beside a plain
carrier that runs on.

**Envelopes inside effects.** An effect's own envelope restarts its tones (its
lines are the envelope's harmonics), unlike an envelope set through the
registers.

**Families.**
- **Robots (R0-R9).** A carrier at about 800 + 300·n Hz beside a low tone and
  its octave (a tone at f under a sine or square envelope at f); some pick out
  one harmonic of their pulse (R5's line near 2.65 kHz is always the pulse
  harmonic nearest that frequency).
- **DTMF (D0-D11).** The standard telephone pairs (697, 770, 852, 941 by 1209,
  1336, 1477 Hz, keypad order, with 0, * and # on the bottom row) at equal
  level, within 3 Hz of the standard once each unit's clock is divided out;
  each holds about 100 ms at one level, then fades over about 55 ms.
- **B series.** Plain tones: B0 is 2600 Hz; B7 is 2906 Hz for 40 ms, a 10 ms
  fade and a 5 ms gap, then 3908 Hz; B8's upper line is exactly the octave of
  its lower one throughout its fall to 1301 Hz.
- **C series.** Pulsed chirps and buzzes at their own pulse rates; C8 and C9
  carry a noise band that sweeps from about 650 Hz to 3 kHz and back every 20
  ms, 5 dB quieter at the top.
- **M0** is a tone whose pitch wanders about 1.5 % rms over tens of
  milliseconds, with a matching level wobble; it is continuous, not pulsed.
- **A0** is a voice at the current Pitch under two plain tones.

**Gates.** A gated effect's off half dips about 8 dB, not to silence.

**Speed.** Effects scale with Speed by about (145.3 - Speed) / 31.3 on every part,
the closing fade included (D1 and C0 played alone at Speed 20, 60 and 127, both
units); over every effect recording at another Speed the best single line is
(147 - Speed) / 33, which the library uses: the chip's law is steeper at low
Speeds than near 114. Earlier readings that seemed to show a curved law, different for tonal
and pulsed effects, were of items followed by a Speed command, which the chip
reads ahead (section 8).

**The bed.** Effects carry a noise bed between their lines, 35-45 dB more
than a steady register-set tone (rewriting that tone's volume register adds
none; VoxProbe 8). The bed grows with the tone's frequency up to about
1.5 kHz and levels off above: the DTMF digits' beds (697-1633 Hz) sit where a
bed proportional to the signal's change per tick puts them, 220 B0's, 221 B1's
and 228 B8's 6-9 dB under it. *Open: its dependence on level, and 220 B0's low
end (its bed falls away below 300 Hz).*

## 8. Commands

- **Pitch** sets the voice's period as above, and offsets the effects' pulse
  rates.
- **Speed** scales speech lengths linearly (above); effects follow a steeper
  law (section 7).
- **Bend** moves every oscillator by the same 44 Hz per step from 5 (D1's 697 and
  1209 Hz both move 44 Hz; so do AW's three formants); below 5 there is a
  further 22 Hz.
- **Volume** is linear in amplitude on speech, effects and tones set directly
  alike (VoxProbe 8, both units: IY +12 dB from 16 to 64 and +5.5 from 64 to
  127, R0 and a register-set tone the same). Earlier readings that looked
  compressed on speech had the restoring Volume read ahead (below).
- **Fast, Slow** as in section 4.
- **Repeat** as in section 6.
- **Stress, Relax** and code 18 change nothing measurable in length, pitch or
  level.
- **Pauses**: P1 and P4 leave shorter gaps than the manual's 100 and 30 ms (VoxProbe
  8, both units: AW P1 AW ~95-100 ms of silence, NE P1 AW ~100-105, AW or NE P4 AW
  ~15-20); the library plays them as 90 and 15 ms, which with the sound's own fade
  gives those gaps. The others are the manual's lengths.
- **Reading ahead.** While a sound plays, the chip takes the one command after it
  early, at the sound's release: an effect's closing fade, or the last 25-30 ms
  of an allophone or of an effect without a fade. A Speed or Volume read so
  applies to the rest of the sound: "Speed v, D1, Speed 114" fades at Speed 114's
  length whatever v, and "Volume 16, IY, Volume 96" jumps to Volume 96 in IY's
  last 25-30 ms (VoxProbe 8, both units). A Pitch read so doesn't move the sound
  (VoxProbe 4 and 7), and a command two places on isn't read (VoxProbe 8: the
  Pitch after "Repeat 1"). *Open: pauses, and Bend.*

## 9. Still unknown

- How Slow (and Fast) stretch each class of sound; whether a modifier before a
  stop, which ignores it, carries to the next sound.
- The pauses between words in long phrases, which run 30-45 ms long in some
  places in the model.
- The effects' noise bed: its dependence on level, and 220 B0's low end.
- The nasals' pulse at low Pitch: NGE's first formant dies away more slowly
  than a single formant would and its spectrum ripples every 300 Hz; NGO's
  falls sooner and then beats; LO's matches the single-formant model to a
  decibel. Two close low components are likely.
- A few effects whose structure doesn't fit the model yet: R5 (its harmonics
  look made by the pulse's own shape), R8, B1, B8 (an octave locked to its
  fundamental), R4 (energy at 600-1200 Hz from an unknown source).
- The mixer's limit on three oscillators totalling over 63.
- The click at the end of some sounds (mostly above 9 kHz, a few milliseconds
  after the sound).
