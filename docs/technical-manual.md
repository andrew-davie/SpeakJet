# The SpeakJet: a technical manual for implementers

**Version 1.0, 2026-11-02.** The version rises with every change to what is known about the chip (new
measurements, corrected findings, newly modelled mechanisms); the revision history is at the end. Regenerated
parameter tables alone do not change it.

Copyright © 2026 Andrew Davie. All rights reserved. This document is not covered by the MIT licence
that applies to the SpeakJet library's source code; that licence grants no rights in this document.

This document describes what the SpeakJet speech chip does, in enough detail to
write an emulator of it from scratch. It covers the chip's interface, its
synthesizer, how each of its 127 sounds is built and timed, what every command
changes, the analogue stage after it, and the parameters of every sound.

Everything here comes from two sources only:

- the SpeakJet User's Manual (Magnevation, revision 1.1, 2004), for the
  interface and the intended design;
- recordings of two real AtariVoxes playing test ROMs: an AtariVox+ ("unit A")
  and an original AtariVox ("unit B").

Statements are marked by how firm they are:

- **measured**: read directly off the recordings, on both units unless it says
  otherwise;
- **fitted**: a value chosen because it makes the model match the recordings
  best, where no single direct reading exists;
- **open**: not known; the model's choice is stated.

The C library this document ships with (`speakjet.c`) is a working
implementation of everything described here, and `docs/design.md` follows the
model measurement by measurement. Section 13 lists where the model and the chip
still differ.

Contents:

1. The hardware
2. The host interface
3. The synthesizer, tick by tick
4. The voice
5. Noise
6. Allophones
7. Sound effects
8. Commands
9. Reading ahead
10. The output stage
11. Implementation outline
12. How the measurements were made
13. Known gaps and open questions
- Appendix A: allophone parameters
- Appendix B: effect parameters
- Appendix C: the library's table encodings
- Appendix D: where the chip departs from its manual
- Revision history

---

## 1. The hardware

**The chip.** The SpeakJet is a Microchip PIC18F1320 running Magnevation's
firmware. It has no crystal. It runs on the PIC's internal 8 MHz RC oscillator,
factory-calibrated to about ±2 %, so two chips differ in clock by a fraction of
a percent.

**The tick.** The firmware makes one output sample per tick, at a nominal 8000
ticks a second. The manual's 8192 Hz is not the real rate. The real rate is
whatever the RC oscillator gives. Two independent readings agree to 0.1 %:

```
                         unit A (AtariVox+)   unit B (original)
images of steady tones        8039 Hz             8013 Hz
SCP 1000 Hz tone (VP9)        8046.8 Hz           8014.3 Hz
```

The library runs at 8026 Hz, between the two units. An emulator should pick
one rate and use it everywhere.

**Arithmetic in 8000ths (measured).** The firmware computes everything for a
nominal 8000 Hz tick. A tone programmed as 697 Hz comes out at 697 × Fs / 8000,
and a period of N ticks lasts N / Fs seconds. So pitch, tone frequency and
duration all scale together with the chip's real clock. In an emulator:

- advance an oscillator's phase by f / 8000 cycles per tick;
- compute pulse periods in ticks from 8000 (section 4);
- run the tick loop at the real rate Fs.

Durations in this document are given in milliseconds at the nominal rate. The
library converts them to ticks at its own rate, `round(ms × Fs / 1000)`, which
is within the two units' spread.

**Two units, one firmware (measured).** The two units behave identically apart
from their clocks: the same lengths, the same pulse shapes to the sample, the
same levels to about half a decibel.

**Not the chip.** A steady line at 15.7 kHz in every recording is the Atari
2600's horizontal scan rate (3,579,545 / 228 Hz). It is identical on both
units and is not part of the SpeakJet's output.

---

## 2. The host interface

**Bytes and the buffer.** The host sends bytes over a serial line into a
64-byte FIFO. A command is one byte, or two for the codes that take a data
byte. A command stays counted in the buffer until it has finished executing: a
sound occupies its byte until its last sample.

**Lines.**

- **Buffer Half Full** is raised while more than 32 bytes are in use. The
  AtariVox's READY pin is its inverse.
- **Speaking** is raised while a sound plays or bytes are waiting.

**Command codes.**

```
code     bytes  name          what it does
0-6        1    P0-P6         pause (section 8)
7          1    Fast          the next sound plays faster
8          1    Slow          the next sound plays slower
14         1    Stress        no measurable effect
15         1    Relax         no measurable effect
16         1    Wait          no measurable effect
20         2    Volume v      0-127
21         2    Speed v       0-127
22         2    Pitch v       0-255
23         2    Bend v        0-15
24         2    PortCtr       I/O port; nothing audible
25         2    Port          I/O port; nothing audible
26         2    Repeat n      the next sound plays n times
28         2    CallPhrase    EEPROM phrases; not emulated
29         2    GotoPhrase    EEPROM phrases; not emulated
30         2    Delay n       silence for n × 10 ms
31         1    Reset         the controls back to their defaults
128-199    1    allophones    72 speech sounds (Appendix A)
200-254    1    effects       55 sound effects (Appendix B)
255        1    end of phrase nothing audible
```

All other codes below 128 take one byte and do nothing.

**Defaults** at power-on and after Reset:

```
Volume 96   Speed 114   Pitch 87 (manual: 88; see section 4)   Bend 5   Repeat 0
```

Fast, Slow, Stress and Relax are cleared at the end of every sound. Repeat is
cleared when the sound it applies to starts.

**Execution.** The chip takes the command at the head of the buffer. A control
(Volume, Speed, Pitch, Bend, Repeat, Fast, Slow) is carried out at once and its
bytes leave the buffer. A sound or pause is built and played, and its byte
leaves the buffer only when it ends. The one exception is reading ahead
(section 9).

---

## 3. The synthesizer, tick by tick

The manual describes the synthesizer as five oscillators, an envelope
oscillator, a noise source on oscillators 4 and 5, and a mixer, all set through
registers. The behaviour of every sound can be described in simpler terms, and
this is the model the rest of the document uses.

**Frames.** At any moment the chip is producing one *frame*:

- three tones, each a frequency and a level 0-31;
- a sound level, as a factor;
- one noise band: a centre frequency, a bandwidth and a level.

**Segments.** Every sound is a short list of *segments*. Each segment lasts a
whole number of ticks and moves from one frame to another. Within a segment the
position is `t = tick / (ticks − 1)`, running 0 to 1, and every field of the
frame moves linearly from start to end unless a flag says otherwise. A segment
is either *voiced*, with its tones shaped by the pulse envelope (section 4), or
*plain*, with its tones as steady sines.

**One tick.** Each tick:

1. If nothing is playing, take the next command from the buffer.
2. Interpolate the current frame.
3. Add the tones: voiced (section 4) or plain (section 7).
4. Add the noise band (section 5).
5. Apply the segment's gates and level shapes (section 7).
6. Apply the sound's attack and release ramps (section 6).
7. Multiply by the Volume gain (section 8).
8. Add the effects' noise bed (section 7).
9. Advance to the next segment, or end the sound.

The result is one sample, held until the next tick, then filtered by the
AtariVox's analogue stage (section 10).

**Mixing levels.** The library's scale has a full-level vowel peak near 0.5.
Relative to the voiced formant sum, plain tones sit at 0.6 and the noise at
0.86 of the same level number. Only the ratios matter.

---

## 4. The voice

Vowels, nasals, liquids, glides, voiced consonants and the voiced parts of
effects share one voice.

### 4.1 The period

The voice's period is a whole number of ticks. Two rules were measured:

```
speech (allophones)           N = floor(8000 / rate) + 1
effects' own pulse            N = ceil(8000 / rate)
```

The two agree except where 8000 / rate is a whole number. There the effect's
period is one tick shorter. VoxProbe 9 measured this on both units:

```
Pitch               100   120   150   160   200   250
235 C5 (effect)      80    67    54    50    40    32
136 AW (vowel)            67          41
floor(8000/p) + 1    81    67    54    51    41    33
ceil(8000/p)         80    67    54    50    40    32
```

For speech, `rate` is the Pitch. For an effect it is the Pitch plus the part's
own offset (section 7.4). The rate is held at 10 or more. The period is
recomputed at the start of each pulse, so a Pitch change or a gliding offset
takes effect at the next pulse.

The result: f0 steps rather than glides. Pitch 157 to 160 all give 157.6 Hz.

**The default pitch (open).** At the reset default every voiced sound and
effect runs 92 ticks. Pitch 88, the manual's default, gives 91 by either rule.
Only Pitch 87 gives 92 (ceil(8000 / 87) = 92). The library's default is 87. No
recording yet plays the same sound at the default and after an explicit Pitch
88, so it is not known whether the default is really 87 or the default period
is computed some other way.

**Exactness (measured).** The period holds to one 48 kHz sample from pulse to
pulse. The gaps between harmonics sit 30-60 dB under them. The chip's voice has
no jitter and no noise floor of its own.

### 4.2 The pulse

At the start of each period the formant oscillators restart at phase zero, and
an envelope multiplies their sum:

```
age = ticks since the period started (0 .. N-1)
env = age / 0.5                        for age < 0.5   (that is, 0 at age 0)
env = 1 − (age − 0.5) / 58             after, down to 0
```

So the envelope is zero on the period's first tick, then at full level at
once, then falls linearly to nothing over 58 ticks (about 7 ms). It stays at
zero for the rest of a longer period. Measured tick by tick on isolated pulses
at Pitch 40, the envelope is zero for a few ticks and then 0.8-1.0 at once:
there is no rise.

The voice sample is:

```
sum   = A1·sin(2π·φ1) + A2·sin(2π·φ2) + A3·sin(2π·φ3)
voice = − env · sum / 128 · level
```

where A1-A3 are the formant levels (0-31), φk the formant phases in cycles, and
`level` the sound's level factor. The minus sign is measured: the pulse starts
downward on both units, on every vowel at Pitch 20. All three formants restart
at the same phase, the same for every sound, and zero fits best.

Each formant's phase advances by Fk / 8000 cycles per tick.

The averaged pulses of the two units are identical to the sample.

**Formants (measured).** Each voiced sound is three formant frequencies with a
level each, so every vowel's spectrum is a comb of harmonics shaped by three
peaks and the skirts of the pulse envelope. There is no separate fundamental
and no DC component.

### 4.3 The envelope oscillator's other shapes

The manual's register 8 gives the envelope oscillator four waveforms. The
voice's pulse above is the saw. Three effects' parts use the others. With
`x = age / N`:

```
saw (the voice)   as in 4.2
sine              env = 0.5 + 0.5·cos(2π·x)
triangle          env = x                     (measured, VoxProbe 7: a rising
                                               ramp, then straight back to 0)
square            env = 1 for x < 0.5, else 0
```

### 4.4 Which tones the pulse shapes

In a voiced effect part, not every tone need follow the pulse:

- **Default voiced part (flag V):** the first two tones pulse, and their
  phases restart each pulse. The third tone is a plain sine beside them at 0.6.
- **V with A ("all"):** all three tones pulse.
- **V with o ("one"):** only the first tone pulses. The other two are plain
  sines (measured on 210 A0: a voice at Pitch under two smooth glides).
- **P ("pulsed") without V:** all three tones take the envelope, but their
  phases run on through each pulse instead of restarting, at 0.6.

Speech always pulses all three formants with the phases restarted.

A plain tone's phase is never reset by the pulse.

---

## 5. Noise

**The source (measured).** The chip's hiss is random noise, Gaussian to the
sample: kurtosis 2.8-3.2, and 4-5 % of samples beyond two sigma. It is not a
tone whose frequency is thrown about, which would have a constant envelope.

**The model.** White noise through a two-pole resonance at the tick rate, each
sound with its own centre `fc`, bandwidth `bw` and level `na`:

```
r  = exp(−π · max(bw, 10) / Fs)
c  = cos(2π · fc / Fs)
g  = sqrt((1 − r²) · ((1 + r²)² − 4r²c²) / (1 + r²))
y  = g · u + 2rc · y[−1] − r² · y[−2]          u: uniform white noise in −0.5..0.5
noise = 0.86 · na / 128 · level · y
```

The gain g holds the output's power equal to the input's at any centre and
width. The noise level byte is not limited to 31: 238 C8 uses 233.

**Measured centres.** FF is flat to the output stage's roll-off. SE, TS and ZZ
resonate at the top of the band (about 3.8-4 kHz), SO at 3.6 kHz, SH at 2.9,
CH 3.1, JH 2.8, TH 2.3, ZH 2.2, DH 2.0, HE 1.7, VV 1.6 kHz, WH at 800 Hz. PE
and PO are low-passed. The voiced fricatives are the voice and a noise band
together. CH is unvoiced.

**The random source.** The library uses one 32-bit linear congruential
generator for every random number:

```
seed = seed · 1664525 + 1013904223   (mod 2³²)
u    = (seed >> 8) / 2²⁴ − 0.5
```

Any good white source will do. The chip's own generator was not identified.

**Noise in a pulsed effect.** In a part flagged A, the noise also takes the
pulse envelope. 230 C0 and 254 M2 are silent between their pulses at Pitch 20.

---

## 6. Allophones

The 72 allophones, codes 128-199, fall into eight kinds. Each has a length, a
frame (three formants and levels, a noise band, a level), a Repeat copy length
and, for some, a second frame. Appendix A lists them.

```
kind     sounds                                  built as
vowel    IY .. UW                                transition, hold
nasal    MM NE NO NGE NGO, LE LO WW RR           transition, hold
vfric    VV ZZ ZH DH                             transition, hold (voice + noise)
glide    IYRR .. OWWW (16)                       one allophone's frame to another's
fric     CH HE HO WH FF SE SO SH TH              noise held
stop     TT TU TS KE KO EK OK PE PO              closure, release (noise)
vstop    BE BO EB OB DE DO ED OD GE GO EG OG     closure with voice bar, release
affr     JH                                      silent closure, then the fricative
```

The liquids and WW sit with the nasals because they are built the same way.

### 6.1 Length

**A sound's length is its own (measured).** It does not depend on what comes
before or after it, on whether it is streamed or sent alone, or on Pitch
(VoxProbe 5, to 5 ms). It is not the manual's length. Vowels play about twice
the manual's figure, diphthongs about 1.3 times, the K stops 0.75 times.
Appendix A gives each sound's length at Speed 114.

**Speed (measured).** Speech length is linear in Speed:

```
length = L114 · (162 − Speed) / 48
```

where L114 is the length at the default Speed 114. 136 AW plays 95, 197 and
287 ms at Speed 127, 90 and 60.

**Fast and Slow.** Fast multiplies the next sound's length by 0.77 and Slow by
1.22, with a floor of 10 ms before the Speed law. Neither changes a stop, a
voiced stop or JH (TT is 45-50 ms plain, fast or slow). These factors come
from single sounds. Recordings of sounds alone show Slow adding a roughly fixed
amount instead (about +30 ms, 163 AYWW about +90), and phrases suggest vowels,
nasals and glides stretch more than hisses. *Open: needs measuring sound by
sound between other sounds.*

### 6.2 Moving between sounds

**Transition (measured).** The voice moves to each new sound's frame over
25 ms, linearly, from wherever the last sound left it. The transition is part
of the sound's length, not added to it, and does not scale with Speed.

**After silence.** At the start of a phrase or after a pause, the transition
starts on the new sound's own formants at zero level. The formants do not
sweep up from 0 Hz.

**Vowels, nasals and voiced fricatives** are two segments: the transition from
the last frame to the target, then the target held for the rest of the length.

**Fricatives** are one plain segment: the noise band held, no transition.

### 6.3 Glides

The sixteen diphthong-like allophones run from one allophone's frame to
another's, levels included (measured on all 16). 149 IYRR starts on IY's frame
and ends on RR's. Their own table rows hold only the pair. With L the played
length in ticks:

```
hold  = floor(L / 5)          the first frame (the 25 ms transition into it inside)
move  = floor(7·L / 10)       linear from the first frame to the second
rest  = L − hold − move       the second frame held
```

### 6.4 Stops

A stop is two segments.

**Closure.** The sound before fades away through the closure; it is not cut
to silence (both units fall 30 dB over 50-60 ms at Speed 114, twice that at
Speed 60). The closure lasts 30 ms × the Speed factor. It runs from the last
frame to a silent frame, linearly.

**Voiced stops** hum through the closure on their first formant alone: the
closure ends not on silence but on the stop's own F1 at its own level A1 and
sound level. BE, BO, DE, DO, GE and GO open on this voice bar 2-9 dB under the
release (measured).

**Burst and release.** The release is the stop's measured length (Appendix A).
It runs from the stop's frame to its second frame. Where no second frame is
listed it holds. Unvoiced stops' releases are noise only. Voiced stops' are
voiced plus their noise. Some voiced stops release toward formants of their
own, with F1 falling into a labial or alveolar closure: 171 BO, 172 EB,
173 OB, 176 ED, 177 OD.

**JH** is a silent closure of 30 ms (not Speed-scaled), then the voiced
fricative held.

### 6.5 Joins

**Cutting short (measured).** A sound followed at once by another sound is cut
short: its last segment loses 15 ms, or 30 ms when the last segment is voiced
and the next sound is a voiced allophone (not a fricative or unvoiced stop).
The cut is skipped if it would leave the segment with a tick or less.

"Followed at once" looks past any modifier and setting bytes between the two
sounds: Fast, Slow, Stress, Relax, Wait, Volume, Speed, Pitch, Bend and
Repeat. A pause, a Delay, a Reset, any other command, or the end of the buffer
means the sound is not followed at once. An effect following counts as a
sound.

**Ramps (measured).** A sound alone ramps in over 35 ms and out over 30 ms,
linearly in amplitude, measured from the sound's first and last ticks. Inside
a run of sounds the level never fades between them: the chip's level drops no
more than 14-30 dB between sounds in a phrase, where ramping each sound out and
in would drop it 40-65 dB. So:

- a sound followed at once by another has no release ramp;
- a sound that follows one joined to it has no attack ramp.

Only the edges of a run ramp. Pauses and effects end a run. An allophone
followed at once by an effect is still cut and loses its release, but the
effect starts with its own attack.

### 6.6 Levels

Each allophone has its own level relative to the vowels, in half-decibel
steps, the same on both units within half a decibel (Appendix A). It applies to
the whole frame: formants and noise alike.

### 6.7 Repeat

Repeat n plays the next sound n times. Repeat 0 and 1 both play it once. Each
further copy lasts the sound's own *copy length*, measured onset to onset
(Appendix A). It is not the first play's length and not the manual's. Copy
lengths scale with Speed like the sound.

- **Vowels and others.** A copy is the target frame held for the copy length.
  The copies run on seamlessly.
- **Stops.** Each copy is the stop again: a closure of 20 ms × the Speed
  factor, then the release for the rest of the copy length. 174 DE, 175 DO,
  170 BE and 171 BO keep their voice bar through the copy's closure (about
  22 dB dips). GE, GO and the final stops close silently (27-35 dB dips).
- **Glides.** Each copy glides back from where the last ended to the first
  frame and on again, in the same fifths and tenths as the first play (149 IYRR
  × 5 rises 1383 to 2083 Hz over its first 60 ms each time).
- **JH and ZH** have copies of 80 and 70 ms.

The attack and release ramps apply once, over the first play and all its
copies together.

---

## 7. Sound effects

The 55 effects, codes 200-254, are built from the same parts as speech. Each
effect is one to eleven *parts* played in sequence, each part one segment.
Appendix B lists them all.

### 7.1 A part

Each part has:

- a length in milliseconds;
- up to three tones, each with a start and end frequency and a start and end
  level (0-31);
- optionally a noise band (centre, bandwidth, level);
- a level in half-decibels, and optionally a level ramp across the part;
- optionally a gate period;
- optionally pulse-rate offsets from Pitch, at the part's start and end;
- flags.

Frequencies and levels move across the part as in section 3. A part's level
ramp of R half-decibels multiplies the level by 10^(R·t/40) at position t, so
the ramp is linear in decibels.

### 7.2 Flags

```
V  voiced: the tones pulse at the part's pulse rate (section 4.4)
P  pulsed: all tones take the pulse envelope, phases running on
A  all: every tone pulses, and the noise too
o  one: only the first tone pulses
i  the envelope is a sine (section 4.3)
t  the envelope is a triangle
q  the envelope is a square
X  sweep geometrically: f = fa · (fb / fa)^t, when both are above 0
E  settle toward the end frequency: f = fb + (fa − fb) · e^(−4t)
D  decay: level × 10^(−35·t/20), 35 dB down by the part's end
F  fade: level × (1 − t), straight to nothing
B  bump: level × sin(π·t), a half sine
S  noise sweep, with the gate period (section 7.6)
W  wander: the tone's pitch and level drift (section 7.7)
```

A part with neither V nor P is plain: its tones are steady sines at 0.6 and
its phases run on from part to part.

### 7.3 Tones and their ending

**Steadiness (measured).** A plain tone holds its pitch to 0.03-0.13 % rms
(the DTMF tones and the robots' carriers) and its level to 0.01-0.09 dB rms in
its middle. A tone starts within 1-3 ms.

**The fade (measured).** At the end of a tone effect the level holds, then
falls straight to nothing, linear in amplitude: −6 dB half way, −20 dB at nine
tenths. That is the F flag on the last part. On the DTMF digits it takes about
55 ms.

**Ramps.** An effect ramps in over 1 ms and out over 15 ms. Effects never
join: they have their full ramps whatever follows.

### 7.4 Pulse rates

A voiced effect part pulses at its own rate, the Pitch plus an offset in hertz
that may glide across the part:

```
rate = Pitch + pa + (pb − pa) · tick / (ticks − 1)
```

The offset is the same number of hertz at every Pitch (measured at Pitch 40,
60, 88, 150 and 200, both units) and Bend does not move it. Examples: 232 C2
runs at Pitch − 22. 233 C3 runs at Pitch − 22 falling to − 42 as its tone
rises. 231 C1 runs near Pitch + 386, falling. The period comes from the rate
by the effects' rule in section 4.1.

### 7.5 The bed

**Measured.** Effects carry a noise bed between their spectral lines, 35-45 dB
more than a steady tone set directly through the registers. Rewriting that
tone's volume register many times adds none. The bed grows with the tone's
frequency up to about 1.5 kHz and levels off above. The DTMF digits'
beds (697-1633 Hz) sit where a bed proportional to the signal's change per tick
puts them; 220 B0's, 221 B1's and 228 B8's sit 6-9 dB under it.

**The model.** Timing error: the change in the output per tick, smoothed by
one pole, times white noise:

```
bed = bed + 0.69096 · (v − v_prev − bed)
v   = v + 0.078 · 2u · bed            u: uniform white noise in −0.5..0.5
```

`v_prev` is the previous tick's final sample. The bed is added after the
Volume gain, on every segment except a voiced speech segment: effects,
fricatives, unvoiced stops and pauses (where it dies away). Voiced speech
gets nothing (section 4.1).

**Level wobble.** Plain effect tones also carry a slow wobble in level:
white noise through a one-pole low-pass (coefficient 0.05, spread to about
64 Hz), scaled so its rms is 0.6 % on a steady tone. On a wandering part it is
10 %.

### 7.6 Gates and the noise sweep

**Gates (measured).** A gated part alternates on and off at its gate period.
The off half dips about 8 dB (× 0.4), not to silence. The gate period is in
milliseconds and does not scale with Speed. It counts from the part's start:
the off half is `tick mod G ≥ G/2`.

**Noise sweep (S, measured on 238 C8 and 239 C9).** Over each gate period the
noise band's centre climbs from 0.55 to 1.8 times its table value in the first
40 % and falls back over the rest, 5 dB quieter at the top. With
`p = (tick mod G) / G`:

```
s      = p / 0.4          for p < 0.4,  else 1 − (p − 0.4) / 0.6
centre = fc · e^(0.6·(2s − 1))
level  = 10^(−5·s / 20)
```

On both units that is about 650 Hz to 3 kHz and back every 20 ms. A swept part
is not also gated.

### 7.7 Wander (252 M0)

252 M0 is a tone whose pitch wanders about 1.5 % rms over tens of
milliseconds, with a matching level wobble. It is continuous, not pulsed. The
model: white noise through a one-pole low-pass (coefficient 0.055), scaled so
its rms is 1.5 %, multiplying the tone's frequency. The level wobble of 7.5
runs at its full 10 % on this part.

### 7.8 The families

- **Robots (200-209, R0-R9).** A carrier at about 800 + 300·n Hz beside a low
  tone and its octave. Several pulse on a sine or square envelope at a rate
  well above Pitch. 205 R5's line near 2.65 kHz is always the pulse harmonic
  nearest that frequency.
- **A series (210-219).** 210 A0 is a voice at the current Pitch, its rate
  gliding from Pitch − 18 to + 100 Hz, under two plain tones crossing in an X.
  211 A1 and 212 A2 are slow three-tone voiced glides. The rest are plain
  multi-tone sweeps.
- **B series (220-229).** Plain tones and chirps. 220 B0 is 2600 Hz. 227 B7 is
  2906 Hz for 40 ms, a 10 ms fade and a 5 ms gap, then 3908 Hz. 228 B8's upper
  line is exactly the octave of its lower one throughout its fall to 1301 Hz.
- **C series (230-239).** Pulsed chirps and buzzes at their own pulse rates.
  238 C8 and 239 C9 are swept noise (7.6).
- **DTMF (240-251, D0-D11).** The standard telephone pairs, 697, 770, 852 and
  941 by 1209, 1336 and 1477 Hz, at equal level, within 3 Hz of the standard
  once each unit's clock is divided out. Keypad order: D1 to D9 are 1 to 9,
  D0 is 0, D10 is * and D11 is #, the last three on the bottom row. Each holds
  100 ms, then fades over 55 ms. The library computes the pair from the code
  and keeps only the lengths and level in its table.
- **M series (252-254).** 252 M0 wanders (7.7). 253 M1 is a decaying noise
  burst. 254 M2 is a pulsed three-tone sweep up and back.

### 7.9 Effects and Speed

Effects scale with Speed more steeply than speech, on every part including the
closing fade:

```
length = L114 · (147 − Speed) / 33
```

This is the best single line over every effect recording at a Speed other
than 114. Played alone at Speed 20, 60 and 127, 241 D1 and 230 C0 read about
(145.3 − Speed) / 31.3, and the chip's law is steeper at low Speeds than near
114. 230 C0's ratios run about 4 % over 241 D1's (VoxProbe 9).

---

## 8. Commands

**Pitch** sets the voice's period (section 4.1) and the base of the effects'
pulse rates. A Pitch command reaches the sound after it, never the one
playing (section 9).

**Speed** scales speech by (162 − Speed) / 48 and effects by
(147 − Speed) / 33.

**Bend (measured)** adds the same number of hertz to every tone and formant:

```
shift = 44 · (Bend − 5) Hz,  and a further −22 Hz when Bend < 5
```

So Bend 0 gives −242 Hz and Bend 15 +440 Hz. 222 B2's 1412 Hz tone measures
1213, 1257, 1304, 1392, 1547 and 1854 Hz at Bend 0, 1, 2, 4, 8 and 15. 241
D1's 697 and 1209 Hz move together, and so do 136 AW's three formants. A zero
frequency stays zero. Bend does not move noise centres or pulse rates.

**Bend below zero (measured, not modelled).** A tone bent below somewhere
between 46 and 90 Hz does not go silent. A line appears near 2.4 kHz instead,
moving against the bent tone at a slope of about −0.9, not the −1 of a plain
mirror. 210 A0, 215 A5, 218 A8 and 235 C5 all show it at Bend 0-2. The
library has no such rule: a tone bent below zero runs at a negative frequency,
which sounds as the positive one. See section 13.

**Volume (measured)** is linear in amplitude on speech, effects and tones set
directly alike: IY rises 12 dB from 16 to 64 and 5.5 dB from 64 to 127 (VoxProbe
8). The library still uses an older measured curve, flatter above 32:

```
Volume    0     8    16     32     64    96    127
dB      off  −20.3  −15.5  −1.3  −1.1    0   +2.2
```

(−0.6 dB per step below 16, linear between the points). The difference is
listed in section 13.

**Fast, Slow** as in 6.1. **Repeat** as in 6.7.

**Stress, Relax and Wait** change nothing measurable in length, pitch or
level.

**Pauses.** P1 and P4 leave shorter gaps than the manual's 100 and 30 ms
(VoxProbe 8: 136 AW P1 136 AW about 95-100 ms of silence, 141 NE P1 136 AW
about 100-105, P4 about 15-20). Pauses do not scale with Speed.

```
pause   P0   P1   P2    P3   P4   P5   P6
ms       0   90   200   700  15   60   90
```

The library's P1 and P4 include the sound's own release, so the silence
measured matches the chip's.

**Delay n** is n × 10 ms of silence.

**Reset** returns the controls to their defaults. In the library it does not
clear the buffer, so the commands after it still play. Unit B shows no reset
event at all; how the chip treats the bytes behind a Reset was not measured.

---

## 9. Reading ahead

**Measured.** While a sound plays, the chip takes the one command after it
early, at the sound's release:

- for an effect whose last part fades (F), at the start of that part;
- otherwise, in the last 30 ms of the sound (25-30 ms measured).

A Speed or Volume read this way is carried out at once, and the rest of the
sound plays at the new setting:

- "Speed v, 241 D1, Speed 114" fades at Speed 114's length whatever v;
- "Volume 16, 128 IY, Volume 96" jumps to Volume 96 in IY's last 25-30 ms.

A Pitch read ahead does not move the sound (VoxProbe 4 and 7). Only the one
command right after is read: the Pitch after "Repeat 1" is not.

**The library.** It reads ahead only while a sound (not a pause) plays and
after its Repeat copies are done. A Speed read ahead rescales the remaining
ticks of the current segment and every later one by the ratio of the new Speed
factor to the old, and the sound's total length with them. The command's bytes
leave the buffer with the sound.

*Open: whether pauses read ahead, and whether Bend does.*

---

## 10. The output stage

**Hold.** Each chip sample is held until the next. So a tone at f also has
images at k·Fs ± f, audible above 4 kHz on full-range speakers at about
−30 dB.

**The filter (measured and fitted).** The AtariVox then low-passes the held
signal: two real poles and one real zero.

```
H(s) = (1 + s/ωz) / ((1 + s/ω1)(1 + s/ω2))

from fc = 1609 Hz, Q = 0.462:
  h  = 1 / (2Q),  r = sqrt(h² − 1)
  ω1 = 2π·fc·(h − r)   →  about 1075 Hz
  ω2 = 2π·fc·(h + r)   →  about 2407 Hz
zero ωz = 2π · 14000 Hz
```

The poles fit tones the chip sets directly (the twelve DTMF pairs' level ratios
and single oscillators at 125, 1000 and 3000 Hz) to within 0.45 dB on both
units. The zero is the lift that the images above 8 kHz need, up to 15 kHz,
across every effect on both units; a resistor in series with the stage's
capacitor would make one.

**Run it exactly.** A digital filter made by the bilinear transform and fed the
held samples averaged over each output sample puts the images 1 dB low at 5
kHz and 8 dB low at 14 kHz. The library instead solves the analogue stage
exactly on the held input and samples it at the output rate. The stage is two
first-order sections in cascade, z0 following x at ω1 and z1 following z0 at
ω2. For x held over a time d:

```
e1 = exp(−ω1·d),  e2 = exp(−ω2·d)
u  = z0 − x
z0 = x + u·e1
z1 = x + (z1 − x)·e2 + u · ω2/(ω2 − ω1) · (e1 − e2)
y  = z1 + (ω2/ωz) · (z0 − z1)            the zero: z1 + z1'/ωz
```

With the poles in radians per chip tick, d is in ticks. Each output sample
advances by Fs / 48000 ticks. When a chip tick falls inside an output sample,
settle on the old held value up to it, run the tick, and settle on the new
value for the rest.

**No quantisation (measured).** The quietest consonants and the tails of stops
come out whole. There is no audible output step size and no nonlinearity
worth modelling.

**The click (open).** Some sounds end with a click a few milliseconds after
they stop, mostly above 9 kHz. It is not modelled.

---

## 11. Implementation outline

**State.**

- The 64-byte FIFO, its count, and how many bytes the executing command holds.
- The controls: Volume, Speed, Pitch, Bend, Repeat, and the one-shot Fast and
  Slow.
- The current sound: its segments (32 is enough; 237 C7 needs 8 parts, 226 B6
  11), the current segment and tick, the tick within the sound and its total,
  the attack and release lengths in ticks, Repeat copies to come.
- Whether the last sound joined this one, and the last frame played.
- Synthesizer: the period and the tick within it, three phases, the noise
  resonator's two delays, the bed and its previous sample, the wobble and
  wander filters, the random seed.
- Output: the held sample, its position within the output sample, the
  filter's two states.

**Starting a command** (when nothing plays):

1. Look at the byte at the head of the buffer.
2. A control: apply it and drop its bytes. Repeat.
3. A pause or Delay: one silent segment.
4. An allophone: build its segments (section 6) at the current Speed, Bend and
   Fast/Slow. Then look past the modifiers for a following sound and cut the
   last segment and drop the release (6.5). Drop the attack if the previous
   sound joined this one. Clear Repeat.
5. An effect: build one segment per part (section 7) at the current Speed and
   Bend. Clear Repeat.

**Each tick:** as in section 3. The pulse period is recomputed when the last
period ends. The segment advances when its ticks run out. At the end of the
last segment, play the next Repeat copy, or end the sound: drop its bytes (and
any read ahead), clear Fast, Slow, Stress and Relax.

**Determinism.** The library computes its per-sample sines, exponentials and
logarithms with its own series so that every platform gives the same output to
the bit. An emulator need not.

**Size.** The library's tables, the 72 allophones and 55 effects, take about
2.7 KB, and the whole library compiles to about 22 KB.

---

## 12. How the measurements were made

**Recordings.** Every recording was made from the AtariVox's audio output at
48 kHz, 16-bit, with the 2600 driving the AtariVox from a test ROM. The two
units were recorded separately.

```
VoxRecord      every allophone and effect alone, connected groups, at default settings
VoxProbe 1-3   pitch, pulse shape, single pulses
VoxProbe 4     every sound at Pitch 60 and 150 and Bend 0 and 15, held-out checks
VoxProbe 5     lengths in context, chunked and isolated sending, Repeat
VoxProbe 6     Volume, slowed stops
VoxProbe 7     the synthesizer driven directly through the registers, effects at Pitch 20
VoxProbe 8     reading ahead, the effects' Speed law, pauses, Slow by class, the bed
VoxProbe 9     clocks, the period rules, Bend sweeps, tones near 0 Hz
Man Goes Down  the phrases of a released game, for whole-phrase checks
```

**Lessons.**

- Never put a control directly after the sound under test. The chip reads it
  ahead (section 9). Separate them with a pause.
- Divide each unit's clock out before comparing frequencies or lengths.
- Measure the recording's background first: at about −42 dBFS it hides the
  quiet tail of every pulse and hiss.

**Scoring.** The library is judged against the recordings by three numbers:
the whole-sound spectrum error, a combined spectrum, time-frequency and level
error, and a perceptual score over every recording of each sound. Each table
value was set to minimise those, within limits that keep every sound's
measured structure.

---

## 13. Known gaps and open questions

Where the library and the chip still differ, or the chip is not understood:

- **The default pitch.** 87 in the library, 88 in the manual (4.1).
- **Bend below zero.** The chip's wrapped line near 2.4 kHz (section 8) is not
  modelled.
- **215 A5** has a tone the library lacks, about 40 Hz unbent. At Bend 15 it is
  at 480 Hz and as loud as the main tone; bent below zero it appears near
  2.5 kHz at −12 dB.
- **Volume.** The chip is linear; the library uses the older curve (section 8).
- **Pulse timbre in some effects.** 235 C5's opening and the robots 200-203 R0
  to R3 have lines that move with Pitch: pulse trains where the library has
  plain tones. The chip's pulse there carries mostly f and 2f, where the voice
  envelope gives the full series. The effects' pulse source is not yet
  understood.
- **The death sound in Man Goes Down** ("Reset, Speed 62, Pitch 212, 231 C1").
  The chip plays it in 731 ms against the library's 707. Its pulse rate climbs
  from 228 to 575 Hz over the first 180 ms, where the library's starts at
  411 Hz. 231 C1's offsets were measured at Pitch 88. How they behave at high
  Pitch and low Speed is not known.
- **Structures not yet modelled.** 205 R5 (its harmonics look made by the
  pulse's own shape), 208 R8, 209 R9, 238 C8, 239 C9, 220 B0, 204 R4 (energy at
  600-1200 Hz from an unknown source), 228 B8's locked octave.
- **Fast and Slow** by class of sound, and between sounds (6.1).
- **The nasals' pulse at low Pitch.** 143 NGE's first formant dies away more
  slowly than a single formant would, with a ripple every 300 Hz in its
  spectrum. 144 NGO's falls sooner and then beats. Two close low components are
  likely.
- **Pauses between words** in long phrases run 30-45 ms long in places in the
  model.
- **The effects' bed:** its dependence on level, and 220 B0's low end (its bed
  falls away below 300 Hz).
- **The mixer's limit** on three oscillators totalling over 63 (manual) was
  probed but not characterised.
- **The click** at the end of some sounds.
- **Reading ahead** of pauses and Bend.

---

## Appendix A: allophone parameters

The library's table, as it plays. Lengths are at Speed 114 in milliseconds,
frequencies in hertz (stored in 16 Hz steps), levels 0-31, the sound level in
decibels relative to the vowels. "copy" is the Repeat copy length. A glide
lists the pair it runs between. A voiced stop may list the frame its release
moves to. This appendix is generated by `tools/analysis/manual_tables.py
--write` from the library's parameter files.

<!-- ALLOPHONES-BEGIN -->
```
code name  kind    ms  copy   level   F1   F2   F3   A1 A2 A3   noise Hz  bw Hz  lvl  2nd frame / pair
 128 IY    vowel   135    75    -2.5   272 2208 2544   29 30 20          -      -    -  
 129 IH    vowel   135    70    -0.5   352 1760 2448   28 30 16          -      -    -  
 130 EY    vowel   140    70    -0.5   544 1760 2368   29 25 14          -      -    -  
 131 EH    vowel   140    75    +2.0   608 1664 2384   22 17 11          -      -    -  
 132 AY    vowel   160    70    +6.0   704 1664 2192   16 10  6          -      -    -  
 133 AX    vowel   145    75    -0.5   528 1504 2384   29 26 15          -      -    -  
 134 UX    vowel   135    70    -0.5   624 1328 2352   30 26 16          -      -    -  
 135 OH    vowel   135    70    -0.5   704 1152 2368   27 23 15          -      -    -  
 136 AW    vowel   130    75    -2.5   704  960 2448   30 30 13          -      -    -  
 137 OW    vowel   130    70    -1.5   528  864 2336   19 26  6          -      -    -  
 138 UH    vowel   135    70    -2.0   448 1072 2272   21 26  7          -      -    -  
 139 UW    vowel   120    75    -4.0   368 1056 2112   29 30  7          -      -    -  
 140 MM    nasal   105    70    -4.0   272  768 1424   18  1  9          -      -    -  
 141 NE    nasal   105    75    -8.5   272 1536 2560   28 10 15          -      -    -  
 142 NO    nasal   105    70    -8.0   272 1152 2512   29  8 17          -      -    -  
 143 NGE   nasal   105    70    -5.0   352  528 2240   21  1 14          -      -    -  
 144 NGO   nasal   105    70    -6.5   368 1472 1904   27  8 14          -      -    -  
 145 LE    nasal   110    70    -6.0   448  960 2640   24 29  6          -      -    -  
 146 LO    nasal   115    70    -6.5   448  880 2448   25 30  6          -      -    -  
 147 WW    nasal   120    70    -3.0   448  784 2096   16 25  2          -      -    -  
 148 RR    nasal   130    75    +0.5   432 1312 1664   20 20  5          -      -    -  
 149 IYRR  glide   250   240    +0.0                                     -      -    -  IY -> RR
 150 EYRR  glide   255   230    +0.0                                     -      -    -  EY -> RR
 151 AXRR  glide   255   230    +0.0                                     -      -    -  AX -> RR
 152 AWRR  glide   255   235    +0.0                                     -      -    -  AW -> RR
 153 OWRR  glide   225   215    +0.0                                     -      -    -  OW -> RR
 154 EYIY  glide   225   185    +0.0                                     -      -    -  EY -> IY
 155 OHIY  glide   265   250    +0.0                                     -      -    -  OH -> IY
 156 OWIY  glide   270   290    +0.0                                     -      -    -  OW -> IY
 157 OHIH  glide   240   210    +0.0                                     -      -    -  OH -> IH
 158 IYEH  glide   225   190    +0.0                                     -      -    -  IY -> EH
 159 EHLL  glide   185   170    +0.0                                     -      -    -  EH -> LE
 160 IYUW  glide   235   240    +0.0                                     -      -    -  IY -> UW
 161 AXUW  glide   215   185    +0.0                                     -      -    -  AX -> UW
 162 IHWW  glide   265   255    +0.0                                     -      -    -  IH -> WW
 163 AYWW  glide   260   240    +0.0                                     -      -    -  AY -> WW
 164 OWWW  glide   220   175    +0.0                                     -      -    -  OW -> WW
 165 JH    affr     60    80   -11.5  1936 3760 3984   29  7 13       2736    672   49  
 166 VV    vfric   100    70   -11.0   528 1504 2272   11 30 14       1552    288   25  
 167 ZZ    vfric   110    70    -7.0   272 1472 2336   31 21 23       4000    784   19  
 168 ZH    vfric   105    70   -10.5   432 1632 2704   30 30  3       2608    624   28  
 169 DH    vfric   105    70   -10.5   288 1568 2240   28 29 10       2368    976   12  
 170 BE    vstop    55    75    -3.0   320 1760 2240   29 29 11       2224    976    5  copies keep voice bar
 171 BO    vstop    65    70    -4.0   336 1408 2368   29 23 10       2272   2560    2  release to 768 1408 2368, copies keep voice bar
 172 EB    vstop    30    45    -5.0   368 1792 2192   22 22  7       1664    880    8  release to 240 1792 2192
 173 OB    vstop    20    60    +3.5   592 1408 2352   10 12  5       2176    896    3  release to 240 1408 2352
 174 DE    vstop    45    60    -8.0   272 1920 2624   30 23 12       2224   1792    8  copies keep voice bar
 175 DO    vstop    55    55    -7.5   416 1488 2432   29 28 14       2512   1744    4  copies keep voice bar
 176 ED    vstop    25    45   -11.0   288 1936 2640   30 25 12       1440   1520   10  release to 224 1936 2640
 177 OD    vstop    25    45    -9.0   288 1520 2432   22 31 11       1344   1392    2  release to 256 1520 2432
 178 GE    vstop    40    65   -10.0   272 1984 2544   30  1  5       2144   1296    5  
 179 GO    vstop    40    65   -10.0   272 1920 3568   30  6  1       1664    928    4  
 180 EG    vstop    20    55    -3.5   272 2096 2864   31 26  1       2128   2336    6  
 181 OG    vstop    20    55    -2.5   272 1152 1312   27  1 30       1440    480    2  
 182 CH    affr     65    60   +14.0   672 1904 2784    0  0  0       3136    464    4  
 183 HE    fric    100    65   -13.0     0    0    0    0  0  0       1728    928    7  
 184 HO    fric    105    90   -18.0     0    0    0    0  0  0       1552    592   14  
 185 WH    fric     90    80    -2.5     0    0    0    0  0  0        816    240    3  
 186 FF    fric    100    65   -10.5     0    0    0    0  0  0       3680   2000   19  
 187 SE    fric    110    45    -5.5     0    0    0    0  0  0       3568    784   30  
 188 SO    fric    110    35    -3.0     0    0    0    0  0  0       3376    592   24  
 189 SH    fric     80    55    -4.5     0    0    0    0  0  0       2768    480   27  
 190 TH    fric     65    45    +6.0     0    0    0    0  0  0       2304    768    3  
 191 TT    stop     45    45    -4.5     0    0    0    0  0  0       3120    528   25  
 192 TU    stop     85    65    -4.5     0    0    0    0  0  0       3360    512   28  
 193 TS    stop    120   135    +7.0     0    0    0    0  0  0       4000   1104    5  
 194 KE    stop     25    55    -3.5     0    0    0    0  0  0       2032    320   29  
 195 KO    stop     25    55    -2.0     0    0    0    0  0  0       1552    272   28  
 196 EK    stop     10    55   +22.5     0    0    0    0  0  0       2224    240    5  
 197 OK    stop     15    45    +4.5     0    0    0    0  0  0       1856    784   22  
 198 PE    stop     65    70   -26.0     0    0    0    0  0  0         64   2384   31  
 199 PO    stop     75   110   -26.5     0    0    0    0  0  0         64   2496   28  
```
<!-- ALLOPHONES-END -->

## Appendix B: effect parameters

Each effect's parts in order: length at Speed 114, flags (section 7.2; "-"
none), level in decibels, then each tone as "start->end frequency / level"
(a level "a->b" moves across the part), then the noise band "noise centre
bw width /level", the gate, the pulse offsets from Pitch and the level ramp
where used. The DTMF pairs come from the code (7.8). Generated with Appendix A.

<!-- EFFECTS-BEGIN -->
```
200 R0
  1    65 ms  -       +2.0 dB  784->816/31  368->624/8  144->336/2
  2    15 ms  F       -2.5 dB  944->1200/10  400->352/14
201 R1
  1    65 ms  -       +5.0 dB  1072->1120/23  608/1  2960->3760/6
  2    10 ms  F       -0.5 dB  1152->1072/26  592->832/26
202 R2
  1    80 ms  V       +2.0 dB  656->352/2->4  1744->912/11->4  1264->1360/31->24  ramp +1.0 dB
203 R3
  1    65 ms  -       +2.0 dB  1600->1584/31  720->736/4  384->304/2
  2    15 ms  F       -8.0 dB  1808->1584/23  528->800/23  320->208/19
204 R4
  1    75 ms  Vq      +2.0 dB  288->560/4  1408->1536/1  1888->1904/30  pulse Pitch+113->+287 Hz
205 R5
  1    80 ms  Vq      +8.0 dB  512->448/2  4000->1872/1  2192->2240/15  pulse Pitch+293->+317 Hz
206 R6
  1    85 ms  Vq      +1.0 dB  688->256/4  1440->1808/2  2464->2528/28  pulse Pitch+280->+134 Hz
207 R7
  1    85 ms  Vi      +3.5 dB  176->256/2  1056->576/4  2832->2784/20  pulse Pitch+103->+203 Hz
208 R8
  1    65 ms  V       +1.5 dB  736->640/4  2624->1744/4  3056->3136/29  pulse Pitch+261->+216 Hz
  2    20 ms  V       +1.5 dB  640->576/5  pulse Pitch+216->+216 Hz
209 R9
  1    85 ms  V       -3.0 dB  672->608/5  1728->1328/2  3344->3520/29  pulse Pitch+262->+224 Hz
210 A0
  1   160 ms  Vo      -5.0 dB  192->272/27  1648->2704/20->10  3184->2864/18->14  pulse Pitch-18->+46 Hz  ramp +5.0 dB
  2   140 ms  V       -2.5 dB  272->368/30  pulse Pitch+46->+100 Hz  ramp +4.0 dB
  3    40 ms  VF      -0.5 dB  368->416/25  pulse Pitch+100->+100 Hz
211 A1
  1   450 ms  VA      +3.0 dB  176/15  240->1248/14  720->1456/17
  2   510 ms  VA      +3.0 dB  176/15  1248->1664/14  1456->2896/17
212 A2
  1   445 ms  VA      -2.5 dB  176->208/26  1760->1312/27  2848->1520/24
  2   275 ms  VA      -2.5 dB  192/24  1264->656/30  1456->1008/28
  3   230 ms  VA      -4.5 dB  240->192/25  496->176/27  1088->608/30
213 A3
  1   525 ms  P       -1.0 dB  432->1520/29  720->2576/27  1360->3536/29
214 A4
  1    15 ms  -      +12.0 dB  160->240/10  528->752/10
  2    75 ms  -       +6.5 dB  240->512/21  768->1552/10
  3    40 ms  -       +8.0 dB  608->1072/15  752->3600/1
  4    50 ms  X       +4.0 dB  1104->3728/23
  5    70 ms  F      +11.5 dB  3936->3840/10
215 A5
  1    90 ms  -       -1.5 dB  624->544/30
  2   295 ms  -       +0.0 dB  528->160/28  144->496/30
  3    85 ms  -       +1.0 dB  496->672/30
  4    30 ms  -       -0.5 dB  624->640/30
  5    55 ms  F       -0.5 dB  640/30
216 A6
  1    60 ms  -       +2.0 dB  176->800/27  464->1824/16
  2    75 ms  -       +1.0 dB  800/28  2032->1968/8
  3    60 ms  D       +3.5 dB  848->768/28
217 A7
  1   115 ms  -       +0.5 dB  352->416/0->31  720->896/0->28  1056->1376/27->29  ramp -9.5 dB
  2   185 ms  -       +1.0 dB  416->560/24->30  1008->1344/26->28  1664/21->18  ramp -1.0 dB
  3   255 ms  -       +2.0 dB  608->960/31->24  1392->1856/26->30  2272->1600/2->1  ramp -3.0 dB
  4    35 ms  F      -12.5 dB  848->1152/31->25  2128->1872/28  ramp -1.5 dB
218 A8
  1    90 ms  B       +0.5 dB  288->592/25  256->1328/28
  2    25 ms  B       -1.5 dB  576->144/28  1232->624/10
  3    35 ms  P       -4.5 dB  no tones
  4    55 ms  B       -0.5 dB  96->576/25  688->1712/23
  5    60 ms  B       +5.0 dB  544->416/16  1904->2048/25
219 A9
  1   250 ms  -       +2.0 dB  2672->1312/22  1552->80/24
220 B0
  1   120 ms  -       +4.0 dB  2592->2576/22
  2    55 ms  F       +4.0 dB  2576/22
221 B1
  1   145 ms  -      +11.0 dB  1424->3840/21  ramp -9.0 dB
  2   110 ms  -       -2.5 dB  3904->1520/31  ramp +7.5 dB
  3   135 ms  F      +13.5 dB  1408->1392/14  ramp +1.0 dB
222 B2
  1   110 ms  -       +7.0 dB  1392->1408/31
  2    20 ms  -       +6.5 dB  1408->2512/29  192->208/10
  3    20 ms  -       +7.0 dB  2400->2560/29  192->208/10
  4    50 ms  -       +7.5 dB  1648->1680/21  192/4
  5    60 ms  -       +7.0 dB  1712->1680/30  192->592/4
  6    65 ms  -       +7.0 dB  1696->1728/24  608->592/3
  7    75 ms  F       +7.5 dB  1712->1808/16  608->576/3
223 B3
  1   105 ms  -       -0.5 dB  3120->3056/22  608->592/7
  2   145 ms  -       +1.0 dB  1424/30  576->624/6
  3    55 ms  F       -1.0 dB  1360->1392/28  624->608/9
224 B4
  1   145 ms  -       +0.0 dB  1824/31  1984->304/26
  2   145 ms  -       +0.0 dB  1824/31  304->2000/26
  3    80 ms  -       -3.0 dB  1824/31  2000/16
225 B5
  1   110 ms  -       +5.0 dB  3520->3488/18  960->0/19
  2    50 ms  -       -5.0 dB  3408->3552/31
226 B6
  1    20 ms  -       +0.0 dB  3392->3472/30
  2     5 ms  -       -1.5 dB  3280->1696/21
  3    20 ms  -       -1.0 dB  1920->1632/30
  4     5 ms  -       +1.0 dB  2112->3552/26
  5    10 ms  -       +5.5 dB  3552->3440/20
  6     5 ms  -       -3.0 dB  3616->1456/29
  7    15 ms  -       -0.5 dB  1856->1792/28
  8     5 ms  -       -4.0 dB  2064->3280/27
  9     5 ms  -       -1.5 dB  3584->3872/29
  10     5 ms  -       +2.5 dB  3664->1792/29
  11    65 ms  F       +2.5 dB  1792/29
227 B7
  1    40 ms  -       +5.0 dB  2816->2912/24
  2    10 ms  F       +5.0 dB  2912/24
  3     5 ms  -       -5.0 dB  no tones
  4    55 ms  -       +0.5 dB  3984->3888/27
  5    55 ms  F       +0.5 dB  3888/27
228 B8
  1   310 ms  E       +2.5 dB  2192->1280/23  4000->2560/21
229 B9
  1    60 ms  -       -9.5 dB  1824->1808/29  912->880/14  2736->2720/27
  2    30 ms  VA     -11.5 dB  848->672/26
  3    55 ms  VA      -9.0 dB  512->544/29  2576/30  3904->3712/30
  4    30 ms  VA      -8.0 dB  528->576/29  448->416/15  592->624/24
  5    95 ms  -       -7.0 dB  1776->1792/27  896->928/13  3552/30
  6    15 ms  F       -7.5 dB  1776->1840/29
230 C0
  1    30 ms  VA     -17.0 dB  3872->2384/15  noise 2176 bw 992 /20  pulse Pitch-5->+5 Hz
  2    65 ms  VA      +2.5 dB  2496->1136/30  noise 1904 bw 896 /26  pulse Pitch-2->-4 Hz
  3    55 ms  VA      +3.5 dB  960->288/26  noise 2480 bw 768 /13  pulse Pitch-3->-8 Hz
  4    75 ms  VD     +12.5 dB  208->160/7  pulse Pitch-10->-11 Hz
231 C1
  1    35 ms  VA      +6.0 dB  704->1888/12  pulse Pitch+161->+424 Hz
  2    30 ms  VA      +1.0 dB  528->944/21  1696->2512/26  1776->2128/24  pulse Pitch+198->+397 Hz
  3    75 ms  VA      +1.0 dB  1024->1792/21  2336->1584/23  pulse Pitch+394->+215 Hz
  4   150 ms  VFA     -0.5 dB  1584->480/9  2096->1152/10  1744->3040/30  pulse Pitch+224->-8 Hz
232 C2
  1    80 ms  VA      -2.5 dB  528->592/29  880->1200/24  2336->1632/28  pulse Pitch-21->-24 Hz
  2    20 ms  VA      -8.0 dB  544/28  1280->1216/7  1824->1552/11  pulse Pitch-18->-24 Hz
  3    20 ms  VA     -20.5 dB  464->720/12  1072->1568/9  1312->1024/28  pulse Pitch-17->-23 Hz
  4    35 ms  -       -6.5 dB  3824->3008/10  noise 2496 bw 1120 /9
  5    55 ms  VFA     +2.0 dB  3376->2880/28  1376->1168/1  1536->1936/1  pulse Pitch-18->-21 Hz
233 C3
  1    75 ms  V       -7.5 dB  3344->3232/30  noise 2608 bw 816 /2  pulse Pitch-32->-23 Hz
  2   130 ms  V       +6.5 dB  3136->3776/17  noise 3024 bw 912 /1  pulse Pitch-20->-45 Hz
  3    25 ms  V       +5.5 dB  3392->2768/25  noise 2992 bw 592 /1  pulse Pitch-43->-37 Hz
234 C4
  1    75 ms  V       +2.5 dB  960->944/26  2688->3264/4  1808->2544/3
  2    55 ms  V       +1.5 dB  2000->1872/30  2768->2176/3
  3    85 ms  V       -2.5 dB  3072->2880/30
235 C5
  1    55 ms  -       -3.5 dB  2704/24  704->768/2
  2    25 ms  -       +9.5 dB  768->624/6  720->960/4  2496->1200/4->1
  3   110 ms  -       +1.0 dB  336->288/13  592->480/14  960->544/12
  4   180 ms  VD     +11.0 dB  336->192/19
236 C6
  1    45 ms  VA      -5.5 dB  1952->1536/21  pulse Pitch-33->-16 Hz
  2    50 ms  VA      +6.0 dB  2048->880/20  2480->2928/10  pulse Pitch-6->+36 Hz
  3    30 ms  VA      +1.5 dB  912/20  2592->1984/27  pulse Pitch+42->+44 Hz
  4    35 ms  VA      +1.5 dB  912/20  1984->2624/27  pulse Pitch+44->+47 Hz
  5    50 ms  VA      +3.5 dB  944->2496/20  2912->2752/20  pulse Pitch+50->-10 Hz
  6    65 ms  VFA     -4.5 dB  1904->2656/29  pulse Pitch-19->-27 Hz
237 C7
  1    45 ms  V       +2.5 dB  544/19  pulse Pitch-13->+4 Hz
  2    50 ms  V       +3.5 dB  656->2960/22  pulse Pitch+5->-6 Hz
  3    40 ms  V       +4.0 dB  480->2624/30  pulse Pitch+6->+38 Hz
  4    50 ms  V       +7.0 dB  720->2128/24  pulse Pitch+45->+93 Hz
  5    45 ms  V       +2.5 dB  592->1536/29  pulse Pitch+90->+126 Hz
  6    40 ms  V       +4.5 dB  384->1104/22  pulse Pitch+120->+148 Hz
  7    15 ms  V       +8.0 dB  1184->528/21  pulse Pitch+173->+150 Hz
  8    85 ms  V       -2.0 dB  464->416/23  pulse Pitch+166->-6 Hz
238 C8
  1    90 ms  S       -5.5 dB  no tones  noise 1952 bw 288 /233  gate 20 ms  ramp -0.5 dB
239 C9
  1    75 ms  S       -4.5 dB  no tones  noise 1712 bw 240 /194  gate 20 ms
240 D0
  1   100 ms  -       +1.0 dB  DTMF pair/31+31
  2    55 ms  F       +1.0 dB  DTMF pair/31+31
241 D1
  1   100 ms  -       +0.5 dB  DTMF pair/31+31
  2    55 ms  F       +0.5 dB  DTMF pair/31+31
242 D2
  1   100 ms  -       +0.5 dB  DTMF pair/31+31
  2    55 ms  F       +0.5 dB  DTMF pair/31+31
243 D3
  1   100 ms  -       +0.5 dB  DTMF pair/31+31
  2    55 ms  F       +0.5 dB  DTMF pair/31+31
244 D4
  1   100 ms  -       +0.5 dB  DTMF pair/31+31
  2    55 ms  F       +0.5 dB  DTMF pair/31+31
245 D5
  1   100 ms  -       +0.5 dB  DTMF pair/31+31
  2    55 ms  F       +0.5 dB  DTMF pair/31+31
246 D6
  1   100 ms  -       +0.5 dB  DTMF pair/31+31
  2    55 ms  F       +0.5 dB  DTMF pair/31+31
247 D7
  1   100 ms  -       +0.5 dB  DTMF pair/31+31
  2    55 ms  F       +0.5 dB  DTMF pair/31+31
248 D8
  1   100 ms  -       +1.0 dB  DTMF pair/31+31
  2    55 ms  F       +1.0 dB  DTMF pair/31+31
249 D9
  1   100 ms  -       +1.0 dB  DTMF pair/31+31
  2    55 ms  F       +1.0 dB  DTMF pair/31+31
250 D10
  1   100 ms  -       +0.5 dB  DTMF pair/31+31
  2    55 ms  F       +0.5 dB  DTMF pair/31+31
251 D11
  1   100 ms  -       +1.0 dB  DTMF pair/31+31
  2    55 ms  F       +1.0 dB  DTMF pair/31+31
252 M0
  1  1190 ms  DW     +11.5 dB  1840->1856/17  noise 1632 bw 208 /5
253 M1
  1   310 ms  D      +17.5 dB  no tones  noise 2000 bw 1584 /33
254 M2
  1   235 ms  VA      -5.0 dB  496->976/30  1728->2176/30  2912->3456/31
  2   280 ms  VA      -5.0 dB  960->496/31  2192->1744/30  3408->2896/30
```
<!-- EFFECTS-END -->

## Appendix C: the library's table encodings

These are the library's choices, not the chip's, given for anyone reading the
tables in `speakjet_tables.c`.

**Units.** Frequencies in 16 Hz steps, lengths in 5 ms steps, levels in
half-decibels as signed bytes. Every field is a byte.

**An allophone row**, 14 bytes:

```
kind  ms  f1 f2 f3  a1 a2 a3  nf nw na  cp  lv  cb
```

`cb` is 1 where a stop's Repeat copies keep the voice bar. A glide's `f1` and
`f2` hold the indices of the pair. A side table `allog` lists the voiced stops
with a second frame: index, then three formants.

**An effect** is a byte stream of parts, located by an offset table. Each
part:

```
ms                       length, 5 ms steps
header                   bits 0-1  tones (0-3)
                         bit 2     noise band follows
                         bit 3     gate follows
                         bit 4     pulse offsets follow
                         bit 5     level ramp follows
                         bit 6     end levels follow
                         bit 7     a high flags byte follows
flags                    low byte (V 1, X 2, D 4, B 8, E 16, S 32, P 64, F 128)
[flags high]             A 1, W 2, envelope shape bits 2-3 (sine 1, triangle 2,
                         square 3), o 16
level                    half-decibels, signed
per tone                 start, end (16 Hz steps), level
[noise]                  centre, width (16 Hz steps), level
[gate]                   ms
[pulse offsets]          start, end: signed 16-bit little-endian, Hz
[level ramp]             half-decibels, signed
[end levels]             one per tone
```

Only the fields a part uses are stored.

## Appendix D: where the chip departs from its manual

```
manual                                  chip, measured
8192 Hz sample rate                     8013-8047 Hz; arithmetic in 8000ths
default Pitch 88                        the default period is Pitch 87's (open)
allophone lengths as listed             vowels ~2x, diphthongs ~1.3x, K stops ~0.75x
Fast 1/2, Slow 1 1/2 times              about 0.77x and 1.22x; stops unchanged
P1 100 ms, P4 30 ms                     about 90 and 15 ms
Volume                                  linear in amplitude
Speed                                   speech (162 - S)/48, effects (147 - S)/33
commands take effect in order           the one after a sound is read at its release
noise: an oscillator disordered by      Gaussian noise through a two-pole resonance
  white noise
```

## Revision history

```
1.0   2026-11-02   rewritten as an implementer's reference: host interface, tick loop, both period
                   rules (VoxProbe 9), the pulse and its shapes, noise, allophone and effect
                   construction, joins, Repeat, reading ahead, the exact output stage, clocks from
                   VoxProbe 9, known gaps, generated parameter appendices, table encodings
```
