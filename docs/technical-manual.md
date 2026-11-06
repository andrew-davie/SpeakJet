# The SpeakJet: a technical manual for implementers

**Version 1.8, 2026-11-06.** The version rises with every change to what is known about the chip (new
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
Volume 96   Speed 114   Pitch 88 (92 ticks: see section 4)   Bend 5   Repeat 0
```

Fast, Slow, Stress and Relax are cleared at the end of every sound. Repeat is
cleared when the sound it applies to starts.

**Execution.** The chip takes the command at the head of the buffer. A control
(Volume, Speed, Pitch, Bend, Repeat, Fast, Slow) is carried out at once and its
bytes leave the buffer. A sound or pause is built and played, and its byte
leaves the buffer only when it ends. The one exception is reading ahead
(section 9).

**The serial control protocol (SCP).** Besides the buffer, a second module
watches every byte. `\` followed by `0` (every chip's node, and the AtariVox's)
enters control mode; `\` followed by any other non-digit leaves it, as does `X`.
Outside control mode, a `\` and the character after it that isn't a digit go on
to the buffer as usual; a `\` sent as a command's data byte (Pitch 92) is data.
In control mode nothing reaches the buffer and every command acts at once:

```
char     what it does
0-9 A-F  into the 16-character input buffer (decimal digits make the value)
H        Memory Type = value (0 registers, 32 EEPROM: not emulated)
J        Memory Address = value
N        write value to the addressed register; the input buffer clears
R        clear the input buffer and the 64-byte buffer (a sound playing plays on)
S / T    hold / release the buffer (the MSA ignores it while held)
V        say "Ready" (not emulated: the chip's phrase is unmeasured)
W        hard reset
X        leave control mode
```

The registers: 0 the envelope's rate (Hz), 1-5 the oscillators' frequencies
(0-3999 Hz), 6 distortion on oscillators 4 and 5 (0-255), 7 the master volume
(0-127, the same as the Volume command), 8 envelope control (bits 0-1 the shape
as section 4.3; bit 6 envelopes oscillators 1-3; bit 7 half envelopes 4 and 5),
11-15 the oscillators' volumes (0-31).

Between the MSA's sounds the synthesizer plays the registers: five sines, the
envelope at register 0's rate (ceil(8000 / rate) ticks with the twelve one-tick-
long rates, the effects' rule; enveloped tones restart with each pulse), the
plain tones at the effects' tone scale plus 5.9 dB, enveloped ones at the voice's.
The level is measured: on both units VoxProbe 9's 1000 Hz tone at volume 31 stands
3.1-3.6 dB over 235 C5 and 241 D1, which put it 4.9 and 6.9 dB above the effects'
scale; the mean is used. A register tone has no bed (section 7.5). Any MSA sound
takes the synthesizer over and leaves the oscillator volumes at 0, as the
manual's auto-silence does. Unmeasured: the distortion's law (modelled as a random
frequency error of up to distortion / 255 each tick), the envelope at 0 Hz
(modelled silent), when a write lands inside a tick.

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
speech (allophones)           N = floor(65536 / step) + 1,   step = floor(rate · 65536 / 8000)
effects' own pulse            N = ceil(8000 / rate)
```

The voice counts a 16-bit phase up by `step` each tick and starts a pulse when it
wraps (VoxProbe 2's sweep of every Pitch 20-255, both units: this matches every
Pitch to measuring precision, where the simpler floor(8000 / rate) + 1 missed 27 of
them). The two rules agree except where 8000 / rate is a whole number, where the
effect's period is one tick shorter. VoxProbe 9 measured this on both units:

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

**Twelve rates one tick long (measured).** At Pitch 44, 51, 53, 56, 63, 77,
87, 88, 92, 104, 127 and 151 the period is one tick longer than floor(8000 / rate) + 1
gives. For speech the phase step produces these by itself; an effect's pulse adds
the extra tick at those twelve rates explicitly:
VoxProbe 2's sweep of every Pitch found them, and VoxProbe 10's sweep of the
envelope oscillator's own register (87-89, 99-101, 119-121, 149-151, 159-161,
199-201, 249-250) shows the same extra tick at 87, 88 and 151 on both units, so
the speech engine and the register share one period routine. The reset default
is the manual's Pitch 88: it is one of the twelve, which is why the default runs
92 ticks.

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

**High pass (measured, 1.8).** An allophone's noise is high-passed twice, two
first-order stages at 400 Hz: every unvoiced fricative on both units had 6-21 dB
too much under 400 Hz without it. A sound may set its own corner (`hp`, 16 Hz
steps; PE and PO, whose noise reaches lower). An effect's noise is not high-passed,
except the swept noise of 238 C8 and 239 C9 (flag S), at 150 Hz.

**White floor (measured, 1.8).** WH has a flat noise floor about 30 dB under its
700 Hz band out to 12 kHz, which a resonance alone cannot make: `wf` adds that
much white noise, in 256ths of the band's level.

**The sibilants' starting burst (measured, 1.8).** SE, SO and SH start on a burst
of low noise 12-18 dB over their body's 100-700 Hz, every take on both units: a
one-pole low-pass (`tc`, 32 Hz steps; 800 Hz when 0) of the same white source at
`th`/256 of the band's level, fading linearly to nothing over 16 ms.

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

**Fast and Slow.** Fast multiplies the next sound's length by 2/3 and Slow by
4/3, with a floor of 10 ms before the Speed law (VoxProbe 12, both units).
Neither changes an unvoiced stop or JH (TT is 45-50 ms plain, fast or slow);
Slow also lengthens the voiced stops by 4/3, Fast does not shorten them.
*Open: under Slow, 155 AY runs about 20 ms longer than 4/3 gives; VoxProbe 13
section 5 measures what part of a vowel Slow scales.*

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

**F1 on its own schedule (measured, 1.8; 161 AXUW).** On both units AXUW's F2 falls
steadily through the move while F1 opens from about 520 to 640 Hz and closes only in
the last 60 ms. A glide may carry two fields of its own: `ba`, its first frame's F1
in percent (F1 rising to it over the hold, from the first allophone's own), and
`cl`, the point (percent of L) until which F1 holds while F2 and F3 move; F1 then
moves with the rest to the end of the move. AXUW has ba 120, cl 75. The other
fifteen glides keep the shared schedule (each was tested: holding F1 made them
worse); 189 OWWW comes close.

### 6.4 Stops

A stop is two segments.

**Closure.** The sound before fades away through the closure; it is not cut
to silence (both units fall 30 dB over 50-60 ms at Speed 114, twice that at
Speed 60). The closure lasts 30 ms × the Speed factor, or the stop's own `cl`
(measured, 1.8: lined up on the release, BE BO DE DO's bar lasts 50-55 ms, the
final stops' 5-16). It runs from the last frame to a silent frame, linearly.

**Voiced stops** hum through the closure on their first formant alone: the
closure ends not on silence but on the stop's own F1 at its own level A1 and
sound level. BE, BO, DE, DO, GE and GO open on this voice bar 2-9 dB under the
release (measured). The bar also carries the upper formants, weak: at 0.12 of the
release's (both units, the bar alone: 6-10 dB more than F1's tail at 1.8-5 kHz).
From silence the bar is there at once at its level, not ramped in; a stop may give
its bar a fade-in of its own (`ba`, ms: 175 DO's bar is quiet for its first ~30 ms)
and a level of its own against the release (`bl`, half-decibels). After the
release's first pulse a stop may hold its own level (`rl`, half-decibels against
the bar: DE's and DO's voicing after the burst).

**Burst and release.** The release is the stop's measured length (Appendix A).
It runs from the stop's frame to its second frame. Where no second frame is
listed it holds. Unvoiced stops' releases are noise only. Voiced stops' are
voiced plus their noise. Some voiced stops release toward formants of their
own, with F1 falling into a labial or alveolar closure: 171 BO, 172 EB,
173 OB, 176 ED, 177 OD.

**Onsets (measured, 1.8).** The voiced stops' and fricatives' first 1-8 ms play
3-19 dB over the rest (VoxRecord solo takes, both units). A sound may carry an
onset: its first `ob` ms play `og`/2 dB louder; in a stop or affricate the onset
starts at the release, not the closure. A sound with an onset, from silence, starts
on its own frame at full level, without the attack ramp.

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

The ramps differ by kind (measured, 1.8, both units, the background allowed for):
an unvoiced fricative (HE HO WH FF SE SO SH TH) switches on and off in 3 ms; a
nasal rises in 25 ms (full in 15 ms at −20 dB, not 25); a voiced stop from silence
starts its bar at once (3 ms). A sound may set its own attack and release (`aa`,
`rr`, ms): 185 WH starts later than its length suggests, SE and SO fade over most
of their length.

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

**The same sound twice.** A sound sent straight after itself, with no pause
or control between, plays as a Repeat copy of the first: no new onset and no
overlap cut between the two (VoxProbe 12, both units; matches within 1-13 ms).

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
m  smooth: tones and the voice switch on and off over 3 ms instead of stepping
u  banded column: the noise is replaced by clicks under a comb (section 7.10)
```

**Smooth (m, fitted per sound).** A tone switching on from silence, or off to
silence, at a part boundary ramps over 24 ticks (3 ms) instead of stepping; a
voice starting after unvoiced sound comes in over the same 24 ticks. A step is a
broadband click: 202 R2 and 218 A8 show none on the chip, so they carry m. It is
per sound, not universal: 233 C3's tones do switch sharply on the chip, and one
ramp for every effect made the library worse (P 4.721 to 4.757).

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

**Ramps.** An effect ramps in over 1 ms and out over 25 ms (1.8; was 15). Effects
never join: they have their full ramps whatever follows. An effect may set its own
attack and release (the `fxs` table, Appendix C).

**The starting click (measured, 1.8).** Many effects open on a click the
library had lacked: about 5 ms of flat broadband energy, −28 to −49 dB under the
sound's peak from 150 Hz to beyond 1.5 kHz, then nothing (220 B0, 219 A9, 223 B3
on every take, both units). The library adds the same white source low-passed at
`fxs` corner × 32 Hz (800 Hz when 0), at level/256, fading linearly over 5 ms.
Effects with one: 203 R3, 204 R4, 216 A6, 219 A9, 220 B0, 221 B1, 223 B3, 228 B8,
236 C6. A longer burst (16 ms) and a click at every part change were both worse.

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

**Whose bed it is (measured, 2026-11-03).** The bed is the chip's own, not the
recording path's: it is the same on unit A, unit B and the Stella take of
VoxProbe 10 (201 R1 −33.2 / −33.4 / −33.1 dB under the tone, 235 C5 −36.7 /
−36.5 / −36.8), though Stella's chain is about 8 dB quieter in silence. It is
specific to effects: under VoxProbe 10's plain 1000 Hz register tone it is about
57 dB down on all three takes, against 34-40 dB under an effect's tone. What in
the effect path makes it is open (section 13). The model treats the bed as a
fixed post-process, not a fitted value.

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
noise band's centre climbs from 0.47 to 2.1 times its table value in the first
40 % and falls back over the rest, 5 dB quieter at the top. With
`p = (tick mod G) / G`:

```
s      = p / 0.4          for p < 0.4,  else 1 − (p − 0.4) / 0.6
centre = fc · e^(0.75·(2s − 1))
level  = 10^(−5·s / 20)
```

On both units the band spans about 650 Hz to 2.4 kHz every 20 ms, the level
peaking at the bottom (1.8: the range was 0.6, too narrow). The chip's own level
step is nearer 15 dB, but 15 scored worse on spectrum, E and P than 5. The swept
noise is high-passed at 150 Hz (section 5). A swept part is not also gated.

### 7.7 Wander (252 M0)

252 M0 is a tone whose pitch wanders about 1.5 % rms over tens of
milliseconds, with a matching level wobble. It is continuous, not pulsed. The
model: white noise through a one-pole low-pass (coefficient 0.055), scaled so
its rms is 1.5 %, multiplying the tone's frequency. The level wobble of 7.5
runs at its full 10 % on this part.

### 7.8 The families

- **Robots (200-209, R0-R9).** A carrier at about 800 + 300·n Hz beside a
  pulse that follows Pitch at a fixed offset, heard mostly as its rate and
  the octave (measured, both units, Pitch 40-200): 200 R0 at Pitch + 175 Hz,
  201 R1 and 203 R3 at Pitch + 278, 204-207 R4-R7 at about Pitch + 175-300
  (their parts' own offsets). The low "tone and its octave" are this pulse,
  so they move with Pitch while the carrier does not. 202 R2's moving line is
  not yet modelled (open). 205 R5's line near 2.65 kHz is always the pulse
  harmonic nearest that frequency.
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

### 7.10 Banded columns (u)

**Measured.** Some effects hold short columns of even broadband energy crossed
by narrow, regular dark stripes: 232 C2 at about 205-225 ms (stripes every
~485 Hz, clearest on unit B), 227 B7 at about 63-81 ms (~489 Hz) and 85-101 ms
(~549 Hz, less clear). The column is not noise: its colour is even, with no
grain. The stripe spacing is not the voice pulse rate (about 88 Hz at Pitch 88)
and is not yet explained.

**The model (fitted).** On a part with u, the noise source's white noise is
replaced by a click every voice period (floor(8000 / Pitch) + 1 ticks, height
√(period / 12) so the power matches the noise), passed through a comb that
subtracts 0.6 of itself one pulse-rate period ago (ceil(8000 / rate) ticks, the
rate as in section 7.4). That cuts dim notches every rate hertz. The result is
scaled by 1 / √(1 + 0.6²) and by 0.25 so that the low levels the chip needs
have fine steps, then goes through the part's noise band as usual. The column
is kept out of the bed's input, so it raises no bed of its own.

### 7.11 Peaking filters (library)

The library can give an effect up to three peaking filters (RBJ biquads at
the chip's rate, applied before the Volume gain), a fixed shaping of its
spectrum. They are the library's tool for closing a measured spectrum gap,
not a chip mechanism. No sound uses them in this version: tried on drawn
effects, they lifted tones where the chip's gap was broad heat, and the lines
then dominated the wrong frames.

### 7.12 Measured effect structures (2026-11-04)

From the VoxRecord takes, both units agreeing:

- **254 M2:** three bright bands moving together, rising from about 520 to
  960 Hz to the middle and falling back. Their spacing holds at about 1220 Hz,
  each band about 7 dB under the one below, and the third sits exactly at
  2 × second − main (an intermodulation product). Every band carries side
  lines about 262 Hz either side. A model with a separate 262 Hz pulse
  (Pitch + 174) made a comb of flat lines the chip doesn't have; the library's
  voiced M2 is the closer model.
- **231 C1:** a buzz whose harmonic spacing (its pulse rate) glides from
  about 475 Hz near 40 ms through 449, 400, 348, 292 and 259 to about 200 Hz at
  210 ms and about 165 Hz at 230 ms, with two or three bright bands like
  formants. A table with that measured glide scored worse than the library's
  own glide.
- **234 C4, 213 A3:** their heat is from pulsing (all tones pulsed), not from
  separate noise.
- **219 A9 (1.8, tracked on both units):** both tones hold 47 ms at 2605 and
  1300 Hz before sweeping; they then fall linearly to 240 ms (upper to 1325 Hz,
  6.6 Hz/ms; lower to 100 Hz, 6.1 Hz/ms); the lower tone then holds at 100 Hz at
  full level and fades to nothing by about 295 ms. The library had swept at once
  and down to 64 Hz.
- **223 B3 (1.8):** where the tone changes from 3.1 to 1.4 kHz the chip glides
  over about 5 ms (the band energy stepping down through 2.5 and 2.1 kHz), about
  4 ms later than the library's part boundary had it. A glide at every part change
  of every effect was tried and was worse everywhere else: B3's is its own.
- **228 B8 (1.8):** its upper tone (about 4 kHz) starts about 40 ms after the
  lower one.
- **229 B9:** three blocks of crisp, thin lines (10-80, 115-165,
  205-305 ms) over a broad band of heat under its low V line (about
  300-1000 Hz, 80-200 ms). Plain tones get the lines and miss the band; voiced
  tones get the band and smear the lines.

## 8. Commands

**Pitch** sets the voice's period (section 4.1) and the base of the effects'
pulse rates. A Pitch command reaches the sound after it, never the one
playing (section 9).

**Speed** scales speech by (162 − Speed) / 48 and effects by
(147 − Speed) / 33.

**Bend (measured)** adds the same number of hertz to every tone and formant:

```
shift = 44 · (Bend − 5) Hz,  plus 22 Hz when Bend < 5
```

So Bend 0 gives −198 Hz, Bend 4 −22 Hz and Bend 15 +440 Hz. 222 B2's 1412 Hz tone measures
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
−30 dB. A tone near the half rate (about 4 kHz) and its own image fall
together and read as one lump in a coarse spectrum (214 A4). The AtariVox has
no reconstruction filter to remove them; the library keeps them, so that it
sounds like the hardware.

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
- The recording path carries steady lines that are not the chip, at the same
  level in silence as under a sound, on both units: each chip's clock leaking
  at 8.00-8.05 kHz (unit B about 140 Hz wide, unit A about 35 Hz; unit B's
  floor there is about 15 dB over unit A's, so the line dominates the band),
  its double at 15.8-15.9 kHz, a 1 kHz comb to 13 kHz (about 10 Hz wide) with
  sidebands 120 Hz either side, and the 60 Hz hum family. The narrow lines are
  subtracted from each recording as sine waves, their amplitude and phase
  measured in its silences and carried through the sounds (a notch would also
  cut the chip's own tones there). The broad bands, 7.5-8.6 kHz, 15.4-16.4 kHz
  and the clock images meeting the tones at 3.8-4.27 kHz, are left out of
  every comparison; a band that loses bins is scaled back to its full width.
- Fit to one recording, check on all of them. A structure fitted to the solo
  takes can get worse at other Pitch, Bend or Speed settings (235 C5 was worse
  on all 14 of its recordings).
- Before calling an odd spectral feature chip output, compare its level in the
  sound against the same recording's silence.

**Scoring.** The library is judged against the recordings by three numbers:
the whole-sound spectrum error, a combined spectrum, time-frequency and level
error, and a perceptual score over every recording of each sound. Each table
value was set to minimise those, within limits that keep every sound's
measured structure. The whole-sound spectrum anchors each side on its total
energy, not its loudest band, and counts every band above the recording's
background, valleys between lines included.

Effects rebuilt from hand-drawn structures are judged on three separate
questions instead: whether the lines are where the chip's are (a line-match
score in semitones, both ways), whether the loudness shape over time matches
(the level-over-time part of the combined error), and the whole-sound
spectrum as a guard.

---

## 13. Known gaps and open questions

Where the library and the chip still differ, or the chip is not understood:

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
- **What makes the effects' bed.** It is the chip's own and absent under a plain
  register tone (section 7.5), but whether it comes from the effect path's
  sequencing, envelopes or oscillator updates is open. The model's rule (bed
  from the change per tick) is a stand-in.
- **The banded columns' stripe spacing** (section 7.10): about 485-549 Hz,
  neither the Pitch nor any known offset.
- **Low Pitch on rebuilt voiced effects.** 233 C3 and 236 C6 match better at
  Pitch 88 and 150 since their rebuild but worse at Pitch 60 (E +1.6 to +3.2),
  so how their pulse tracks a low Pitch is not modelled right.
- **254 M2's sidebands** (section 7.12): the ±262 Hz lines on every band are
  measured but not explained.
- **237 C7** is a cluster of short banded columns in an arch shape, not yet
  modelled; **227 B7** waits on the bed rule (section 7.5).

---

## Appendix A: allophone parameters

The library's table, as it plays. Lengths are at Speed 114 in milliseconds,
frequencies in hertz (stored in 16 Hz steps), levels 0-31, the sound level in
decibels relative to the vowels. "copy" is the Repeat copy length. A glide
lists the pair it runs between. A voiced stop may list the frame its release
moves to. This appendix and Appendix B are generated by SpeakJetLab's
`tools/analysis/manual_tables.py --write` from the library's parameter files
(params.json, fx.json, fxstruct.json), quantised as the tables store them.

<!-- ALLOPHONES-BEGIN -->
```
code name  kind    ms  copy   level   F1   F2   F3   A1 A2 A3   noise Hz  bw Hz  lvl  2nd frame / pair
 128 IY    vowel   125    75    -2.5    272 2208 2544   28 29 18           -      -    -  
 129 IH    vowel   140    70    -0.5    352 1760 2448   28 30 16           -      -    -  
 130 EY    vowel   140    70    -0.5    544 1760 2368   29 25 14           -      -    -  
 131 EH    vowel   145    75    +2.0    608 1664 2384   22 17 11           -      -    -  
 132 AY    vowel   155    70    +6.0    704 1664 2192   16 10  6           -      -    -  
 133 AX    vowel   135    75    -0.5    528 1504 2384   29 26 15           -      -    -  
 134 UX    vowel   140    70    -0.5    624 1328 2352   30 26 16           -      -    -  
 135 OH    vowel   135    70    -0.5    704 1152 2368   28 23 15           -      -    -  
 136 AW    vowel   135    75    -2.5    704  960 2448   30 30 13           -      -    -  
 137 OW    vowel   125    70    -2.0    528  864 2336   22 29  7           -      -    -  
 138 UH    vowel   125    70    -0.5    448 1056 2256   19 24  7           -      -    -  
 139 UW    vowel   120    75    -4.0    368 1056 2096   27 30  6           -      -    -  
 140 MM    nasal   100    70    -4.0    272  768 1424   18  1  9           -      -    -  
 141 NE    nasal   100    75    -8.5    272 1536 2512   28  9 15           -      -    -  
 142 NO    nasal   105    70    -8.0    272 1136 2512   26  7 15           -      -    -  
 143 NGE   nasal   100    70    -8.5    352  480 2240   30  1 19           -      -    -  
 144 NGO   nasal    95    70    -6.5    368 1472 1904   27  8 14           -      -    -  
 145 LE    nasal   115    70    -6.0    448  960 2640   24 29  6           -      -    -  
 146 LO    nasal   110    70    -6.5    448  864 2448   26 30  6           -      -    -  
 147 WW    nasal   110    70    -3.0    448  784 2096   16 25  2           -      -    -  
 148 RR    nasal   125    75    +0.5    432 1312 1664   20 20  5           -      -    -  
 149 IYRR  glide   250   240    +0.0                                       -      -    -  IY -> RR
 150 EYRR  glide   255   230    +0.0                                       -      -    -  EY -> RR
 151 AXRR  glide   255   230    +0.0                                       -      -    -  AX -> RR
 152 AWRR  glide   255   235    +0.0                                       -      -    -  AW -> RR
 153 OWRR  glide   225   215    +0.0                                       -      -    -  OW -> RR
 154 EYIY  glide   225   185    +0.0                                       -      -    -  EY -> IY
 155 OHIY  glide   265   250    +0.0                                       -      -    -  OH -> IY
 156 OWIY  glide   270   290    +0.0                                       -      -    -  OW -> IY
 157 OHIH  glide   240   210    +0.0                                       -      -    -  OH -> IH
 158 IYEH  glide   225   190    +0.0                                       -      -    -  IY -> EH
 159 EHLL  glide   185   170    +0.0                                       -      -    -  EH -> LE
 160 IYUW  glide   235   240    +0.0                                       -      -    -  IY -> UW
 161 AXUW  glide   215   185    +0.0                                       -      -    -  AX -> UW
 162 IHWW  glide   265   255    +0.0                                       -      -    -  IH -> WW
 163 AYWW  glide   260   240    +0.0                                       -      -    -  AY -> WW
 164 OWWW  glide   220   175    +0.0                                       -      -    -  OW -> WW
 165 JH    affr     55    80   -14.0   1968 3264 3904   28  5 24        2672    784   52  
 166 VV    vfric   105    70   -10.0    528 1520 2288   11 30 13        1536    336   30  
 167 ZZ    vfric   105    70    -7.0    272 1472 2336   31 21 23        4000    784   19  
 168 ZH    vfric    95    70   -10.5    432 1664 2944   30 29  3        2592    496   35  
 169 DH    vfric    95    70   -11.5    288 1584 2336   28 30 10        2304    960   16  
 170 BE    vstop    50    75    -2.0    320 1760 2224   30 29 10        2192   1040    7  
 171 BO    vstop    50    70    -4.0    320 1392 2384   28 24 11        2288   2976    3  -> 832 1392 2384
 172 EB    vstop    35    45   -13.0    352 1760 2272   30 27  8        1696    960   18  -> 272 1760 2272
 173 OB    vstop    25    60    -1.5    592 1392 2352   18 17  7        2416    752    5  -> 272 1392 2352
 174 DE    vstop    40    60    -4.5    272 1920 2640   30 23 13        1872   1568   11  
 175 DO    vstop    40    55    -7.0    400 1488 2448   30 26 15        2832   1056    1  
 176 ED    vstop    40    45   -18.0    288 1936 2624   26 19  8        1424   1632   19  -> 256 1936 2624
 177 OD    vstop    35    45   -12.0    272 1520 2448   19 18  7        1296   1472   13  -> 272 1520 2448
 178 GE    vstop    35    65   -10.0    272 1984 2544   30  1  5        2144   1296   16  
 179 GO    vstop    50    65   -10.0    272 1920 3568   30  6  1        1664    928   13  
 180 EG    vstop    30    55    -3.5    272 2096 2864   31 26  1        2128   2336   20  
 181 OG    vstop    25    55    -2.5    272 1200 1312   28  1 30        1504    496    6  
 182 CH    affr     55    60   +22.0    672 1904 2784    0  0  0        2864    464    2  
 183 HE    fric     70    65   -15.5      0    0    0    0  0  0        1824    816    9  
 184 HO    fric     70    90   -18.0      0    0    0    0  0  0        1408    560   16  
 185 WH    fric     70    80   -14.5      0    0    0    0  0  0         752    240   12  
 186 FF    fric     70    65    -6.0      0    0    0    0  0  0        3008   1376    9  
 187 SE    fric     90    45   -13.5      0    0    0    0  0  0        3456    416   92  
 188 SO    fric     95    35    +6.0      0    0    0    0  0  0        3392    384   11  
 189 SH    fric     65    55    -8.0      0    0    0    0  0  0        2784    368   46  
 190 TH    fric     45    45    +6.0      0    0    0    0  0  0        2304    768    3  
 191 TT    stop     30    45   -12.0      0    0    0    0  0  0        3184    368   83  
 192 TU    stop     80    65    +3.0      0    0    0    0  0  0        3184    416   14  
 193 TS    stop     95   135    +4.0      0    0    0    0  0  0        3824    720    9  
 194 KE    stop     25    55    -7.5      0    0    0    0  0  0        2016    416   57  
 195 KO    stop     25    55    +1.0      0    0    0    0  0  0        1616    304   20  
 196 EK    stop     15    55   +20.0      0    0    0    0  0  0        2320    288    2  
 197 OK    stop     15    45    +1.0      0    0    0    0  0  0        1824    560   46  
 198 PE    stop     55    70   -26.0      0    0    0    0  0  0          64   2384   31  
 199 PO    stop     50   110   -26.5      0    0    0    0  0  0          64   2496   28  

Their own structure (1.8; fields left out are 0, "as the kind"; section 6 and Appendix C):
 128 IY    rr 20 ms  aa 25 ms
 129 IH    aa 45 ms
 132 AY    aa 60 ms
 133 AX    aa 5 ms
 134 UX    aa 10 ms
 137 OW    aa 5 ms
 139 UW    aa 30 ms
 140 MM    rr 20 ms  aa 10 ms
 141 NE    rr 10 ms
 144 NGO   rr 10 ms  aa 5 ms
 146 LO    rr 10 ms
 147 WW    rr 10 ms  aa 10 ms
 161 AXUW  cl 75 %  ba 120 %
 165 JH    cl 35 ms  rr 10 ms  hp 32 Hz
 166 VV    wf 16/256
 167 ZZ    ob 5 ms  og +6.0 dB  aa 15 ms
 168 ZH    ob 3 ms  wf 16/256  rr 10 ms  aa 30 ms
 169 DH    ob 3 ms  og +2.0 dB
 170 BE    ob 5 ms  og +2.0 dB  cl 34 ms  rl -2.0 dB  rr 30 ms  ba 8 ms  bl -8.0 dB  hp 528 Hz
 171 BO    ob 5 ms  og +3.0 dB  cl 29 ms  rr 30 ms  bl -10.0 dB
 172 EB    ob 6 ms  og +4.0 dB  cl 8 ms  bl -2.0 dB  hp 656 Hz
 173 OB    cl 4 ms
 174 DE    ob 5 ms  og +1.0 dB  cl 28 ms  rl -5.0 dB  rr 10 ms  bl -4.0 dB  aa 5 ms  hp 528 Hz
 175 DO    cl 44 ms  rr 20 ms  bl -4.0 dB  aa 10 ms
 176 ED    ob 5 ms  og +9.0 dB  cl 10 ms  ba 24 ms  hp 720 Hz
 177 OD    ob 5 ms  og +6.0 dB  cl 8 ms  ba 16 ms  bl -2.0 dB
 178 GE    ob 5 ms  og +3.0 dB  cl 13 ms  rr 10 ms  bl +2.0 dB  hp 800 Hz
 179 GO    ob 5 ms  og +6.0 dB  cl 8 ms  rr 10 ms  ba 24 ms  bl -4.0 dB  hp 720 Hz
 180 EG    ob 5 ms  og +4.0 dB  cl 11 ms  ba 32 ms  bl -4.0 dB  hp 784 Hz
 181 OG    ob 5 ms  og +1.0 dB  cl 4 ms
 182 CH    ob 5 ms  og +3.0 dB  wf 16/256  rr 10 ms  aa 5 ms  hp 784 Hz
 183 HE    wf 48/256  th 24/256  hp 32 Hz
 184 HO    ob 5 ms  og +9.0 dB  wf 16/256  aa 5 ms  hp 464 Hz
 185 WH    ob 5 ms  og +3.0 dB  wf 96/256  hp 208 Hz
 186 FF    ob 5 ms  og +6.0 dB  wf 48/256  rr 10 ms  aa 5 ms  hp 208 Hz
 187 SE    wf 16/256  rr 10 ms  tc 544 Hz  th 224/256  aa 10 ms  hp 528 Hz
 188 SO    tc 544 Hz  th 224/256  aa 25 ms  hp 336 Hz
 189 SH    ob 3 ms  og +3.5 dB  rr 10 ms  tc 672 Hz  th 160/256  aa 10 ms  hp 528 Hz
 190 TH    rr 10 ms  hp 80 Hz
 191 TT    ob 4 ms  og +1.0 dB  cl 35 ms  wf 16/256  rr 10 ms  hp 464 Hz
 192 TU    ob 5 ms  og +3.0 dB  cl 22 ms  wf 16/256  aa 5 ms
 193 TS    ob 10 ms  og +0.5 dB  cl 26 ms  wf 16/256  rr 10 ms  hp 464 Hz
 194 KE    ob 5 ms  og +3.0 dB  cl 22 ms  hp 80 Hz
 195 KO    cl 30 ms  aa 5 ms  hp 144 Hz
 196 EK    ob 5 ms  og +6.0 dB  cl 25 ms  wf 48/256
 197 OK    ob 5 ms  og +3.0 dB
 198 PE    cl 35 ms  rr 10 ms  hp 32 Hz
 199 PO    cl 5 ms  wf 48/256  rr 10 ms  aa 5 ms  hp 32 Hz
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
  1     70 ms  Vo       +8.0 dB  416->640/3  784->800/17  128->416/1  pulse Pitch+167->+166 Hz
  2     20 ms  F        -6.5 dB  640->1088/15  352->464/29
  own: attack 5 ms
201 R1
  1     65 ms  Vo       +3.0 dB  704->768/2  1120->1072/29  3024->3504/6  pulse Pitch+284->+270 Hz
  2     10 ms  F       -12.0 dB  1008->1312/30  624->720/30
  own: release 10 ms
202 R2
  1     80 ms  V        +3.5 dB  688->368/2->4  1536->1040/11->8  1296->1312/30->19  ramp -2.0 dB
203 R3
  1     65 ms  Vo       +3.0 dB  624/4  1632->1584/27  384->320/1  pulse Pitch+278->+280 Hz
  2     20 ms  F       -10.0 dB  1680->1408/24  512->960/16  272->192/15
  own: release 10 ms, click 24/256
204 R4
  1     80 ms  Vq       +2.0 dB  240->672/4  1424->1520/1  1920->1856/30  pulse Pitch+88->+368 Hz
  own: click 24/256
205 R5
  1     80 ms  Vq       +8.0 dB  512->448/2  4000->1872/1  2192->2240/15  pulse Pitch+293->+317 Hz
206 R6
  1     80 ms  Vq       +0.5 dB  800->256/4  1376->1872/2  2528->2464/29  pulse Pitch+333->+52 Hz
  own: release 10 ms
207 R7
  1     80 ms  Vi       +1.0 dB  176->256/1  768->480/6  2832->2800/24  pulse Pitch+109->+263 Hz
  own: release 10 ms
208 R8
  1     65 ms  V        +1.5 dB  736->640/4  2624->1744/4  3056->3136/29  pulse Pitch+261->+216 Hz
  2     20 ms  V        +1.5 dB  640->576/5  pulse Pitch+216->+216 Hz
  own: release 10 ms
209 R9
  1     85 ms  V        -3.0 dB  672->608/5  1728->1328/2  3344->3520/29  pulse Pitch+262->+224 Hz
210 A0
  1    160 ms  Vo       -1.0 dB  240->256/23  1520->2496/10  3200->2752/11  pulse Pitch-26->+54 Hz  ramp +2.0 dB
  2    150 ms  Vo       -1.0 dB  288->400/30  2304->3072/1  2752/3  pulse Pitch+50->+111 Hz  ramp +1.0 dB
  3     40 ms  VF       -1.0 dB  416/17  pulse Pitch+77->+101 Hz
  own: attack 5 ms
211 A1
  1    450 ms  VA       +3.0 dB  176/15  240->1248/14  704->1248/17
  2    510 ms  VA       +3.0 dB  176/15  1232->1696/14  1408->3008/17
212 A2
  1    445 ms  VA       -2.5 dB  176->208/26  1696->1280/27  2928->1504/24
  2    275 ms  VA       -2.5 dB  192/24  1264->656/30  1328->912/28
  3    230 ms  VA       -4.5 dB  240->192/25  496->176/27  960->672/30
213 A3
  1    525 ms  P        -1.0 dB  432->1520/29  720->2576/27  1360->3536/29
214 A4
  1     15 ms  -        +0.5 dB  176->240/26  544->704/12  noise 1872 bw 3120 /23
  2     75 ms  -        +4.5 dB  240->496/26  736->1584/14
  3     30 ms  -        +7.0 dB  560->896/21  1616->2768/11  192/2
  4     10 ms  -        +3.5 dB  944->1024/24  3296->3728/3->10  208/6
  5      5 ms  -       +10.0 dB  1088/20  192/1
  6     50 ms  X        +3.0 dB  1168->3856/30  192/5
  7     75 ms  F        +7.0 dB  3920->3824/12
  own: attack 10 ms
215 A5
  1     10 ms  -       -64.0 dB  no tones
  2     15 ms  m        +4.0 dB  624->608/23->17  176/1
  3     60 ms  m        +4.0 dB  608->544/17->18  176->160/1
  4     10 ms  m        +4.0 dB  544->528/18->14  160->144/1
  5     20 ms  m        +4.0 dB  528->512/14->20  144/1->7
  6     20 ms  m        +4.0 dB  512->480/20->23  144->176/7->19
  7     20 ms  m        +4.0 dB  480->464/23->22  176->208/19->8
  8     20 ms  m        +4.0 dB  464->432/22->18  208->224/8->15
  9     20 ms  m        +4.0 dB  432->416/18->28  224->256/15->10
  10     20 ms  m        +4.0 dB  416->384/28->18  256/10->17
  11     20 ms  m        +4.0 dB  384->336/18->26  256->288/17
  12     20 ms  m        +4.0 dB  336->320/26->29  288->320/17->18
  13     40 ms  m        +4.0 dB  320->272/29->18  320->384/18->15
  14     20 ms  m        +4.0 dB  272->256/18->14  384->400/15->21
  15     20 ms  m        +4.0 dB  256->224/14->12  400->432/21->20
  16     20 ms  m        +4.0 dB  224->208/12->21  432->464/20->18
  17     10 ms  m        +4.0 dB  208->192/21->15  464/18->16
  18     10 ms  m        +4.0 dB  192->176/15->14  464->480/16->24
  19     20 ms  m        +4.0 dB  176->160/14->8  480->496/24->19
  20     10 ms  m        +4.0 dB  160/8->10  496->512/19
  21     10 ms  m        +4.0 dB  160->144/10->14  512->528/19
  22     20 ms  m        +4.0 dB  144->128/14->4  528->544/19->26
  23     20 ms  m        +4.0 dB  128/4  544->576/26->22
  24     20 ms  m        +4.0 dB  128->160/4  576->608/22->25
  25     20 ms  m        +4.0 dB  160->176/4  608->624/25->20
  26     20 ms  m        +4.0 dB  176->160/4  624/20->14
  27     25 ms  m        +4.0 dB  160->96/4  624/14->18
  28     10 ms  m        +4.0 dB  96->64/4->1  624/18->3
  29     10 ms  -       -64.0 dB  no tones
216 A6
  1      5 ms  -       -64.0 dB  no tones
  2     15 ms  m        +2.5 dB  288->320/29->17  368->800/9->7  368->864/21->27
  3     40 ms  m        +2.5 dB  320->784/17->30  800->1840/7->9  864->2000/27->16
  4      5 ms  m        +2.5 dB  784->800/30->27  1840->2000/9->4  2000/16->12
  5     15 ms  m        +2.5 dB  800/27->23  2000/0  2000/12->9
  6     40 ms  m        +2.5 dB  800/23  2000/0  2000/9->2
  7      5 ms  m        +2.5 dB  800/23->24
  8     50 ms  m        +2.5 dB  800/24->10
  9     10 ms  -       -59.5 dB  no tones
  own: click 24/256
217 A7
  1    115 ms  -        +0.5 dB  352->416/0->31  720->896/0->28  1056->1376/27->29  ramp -9.5 dB
  2    185 ms  -        +1.0 dB  416->560/24->30  1008->1344/26->28  1664/21->18  ramp -1.0 dB
  3    255 ms  -        +2.0 dB  608->976/31->24  1392->1856/26->30  2272->1600/2->1  ramp -3.0 dB
  4     35 ms  F       -12.5 dB  848->1152/31->25  2128->1872/28  ramp -1.5 dB
218 A8
  1      5 ms  -       -60.5 dB  no tones
  2     20 ms  m        +1.5 dB  192->320/7->16  208->512/2->10
  3      5 ms  m        +1.5 dB  320->336/16->19  512->560/10->13
  4     30 ms  m        +1.5 dB  336->496/19->27  560->1008/13->30
  5     30 ms  m        +1.5 dB  496/27->12  1008/30->6
  6      5 ms  m        +1.5 dB  496/12->19  1008/6->21
  7      5 ms  m        +1.5 dB  496/19->8  1008/21->2
  8     25 ms  m        +1.5 dB  496->208/8->6  1008->240/2->3
  9     20 ms  -       -53.5 dB  no tones
  10     20 ms  m        +1.5 dB  192->288/4->1  176->832/3->9
  11     30 ms  m        +1.5 dB  288->432/1->18  832->1776/9->22
  12     10 ms  m        +1.5 dB  432->496/18->24  1776->2000/22->30
  13     10 ms  m        +1.5 dB  496/24->18  2000/30
  14     15 ms  m        +1.5 dB  496/18->11  2000/30->15
  15     35 ms  m        +1.5 dB  496/11->3  2000/15->6
  16     10 ms  -       -64.0 dB  no tones
219 A9
  1     45 ms  -        +1.0 dB  2608/24  1296/26
  2    195 ms  -        +1.0 dB  2608->1328/24  1296->96/26
  3     70 ms  -        -2.0 dB  96/30  ramp -10.0 dB
  own: click 12/256 to 8000 Hz
220 B0
  1    120 ms  -        +4.0 dB  2592->2576/22
  2     55 ms  F        +4.0 dB  2576/22
  own: attack 5 ms, release 5 ms, click 12/256 to 8000 Hz
221 B1
  1    145 ms  -       +11.0 dB  1424->3840/21  ramp -9.0 dB
  2    110 ms  -        -2.5 dB  3904->1520/31  ramp +7.5 dB
  3    135 ms  F       +13.5 dB  1408->1392/14  ramp +1.0 dB
  own: click 48/256 to 800 Hz
222 B2
  1     10 ms  -       -64.0 dB  no tones
  2    105 ms  m        +7.5 dB  1392/28->30
  3     10 ms  m        +7.5 dB  1392/30->28  192/2->1
  4     10 ms  m        +7.5 dB  1392->1968/28->30  192/1->4
  5     10 ms  m        +7.5 dB  1968->2576/30->26  192/4->8
  6     15 ms  m        +7.5 dB  2576->2480/26->16  192/8->5
  7      0 ms  um       +7.5 dB  2480/16->15  192/5->7  1696/16->15  noise 2128 bw 2080 /37  pulse Pitch+396->+402 Hz
  8     10 ms  um       +7.5 dB  2480/15  192/7->2  1696/15->20  noise 2032 bw 1744 /34  pulse Pitch+395->+402 Hz
  9      5 ms  um       +7.5 dB  2480/15->9  192/2->3  1696/20->22  noise 1936 bw 2336 /36  pulse Pitch+403->+392 Hz
  10      5 ms  m        +7.5 dB  2512/0  192/3->4  1696/22->28
  11     35 ms  m        +7.5 dB  2512/0  192/4->3  1696/28->30
  12     50 ms  m        +7.5 dB  2512/0  192->608/3->4  1696/30
  13     15 ms  m        +7.5 dB  2512/0  608/4  1696/30->12
  14     15 ms  m        +7.5 dB  2512/0  608/4->3  1696/12->23
  15     55 ms  m        +7.5 dB  2512/0  608/3->2  1696/23->30
  16     10 ms  m        +7.5 dB  2512/0  608/2->1  1696/30->9
  17     30 ms  m        +7.5 dB  2512/0  608/1  1696/9->6
  18      5 ms  m        +7.5 dB  2512/0  608/0  1696/6->2
  19     10 ms  -       -64.0 dB  no tones
223 B3
  1    105 ms  -        -0.5 dB  3120->3056/22  608->592/7
  2      5 ms  -        -0.5 dB  3056->1392/22  592/7
  3    140 ms  -        +1.0 dB  1392/30  592/6
  4     55 ms  F        -1.0 dB  1408/28  608->592/6
  own: click 12/256 to 8000 Hz
224 B4
  1    145 ms  -        +0.0 dB  1824/31  1984->304/26
  2    145 ms  -        +0.0 dB  1824/31  304->2000/26
  3     80 ms  -        -3.0 dB  1824/31  2000/16
  own: release 10 ms
225 B5
  1    110 ms  -        +5.0 dB  3520->3488/18  960->0/19
  2     50 ms  -        -5.0 dB  3408->3552/31
226 B6
  1     20 ms  -        +0.0 dB  3392->3472/30
  2      5 ms  -        -1.5 dB  3280->1696/21
  3     20 ms  -        -1.0 dB  1920->1632/30
  4      5 ms  -        +1.0 dB  2112->3552/26
  5     10 ms  -        +5.5 dB  3552->3440/20
  6      5 ms  -        -3.0 dB  3616->1456/29
  7     15 ms  -        -0.5 dB  1856->1792/28
  8      5 ms  -        -4.0 dB  2064->3280/27
  9      5 ms  -        -1.5 dB  3584->3872/29
  10      5 ms  -        +2.5 dB  3664->1792/29
  11     65 ms  F        +2.5 dB  1792/29
  own: release 10 ms
227 B7
  1     40 ms  -        +5.0 dB  2816->2912/24
  2     10 ms  F        +5.0 dB  2912/24
  3      5 ms  -        -5.0 dB  no tones
  4     55 ms  -        +0.5 dB  3984->3888/27
  5     55 ms  F        +0.5 dB  3888/27
228 B8
  1     40 ms  E        +2.5 dB  2192->2032/23
  2    270 ms  E        +2.5 dB  2032->1280/23  3776->2560/21
  own: click 12/256 to 8000 Hz
229 B9
  1     60 ms  -        -9.5 dB  1824->1808/29  912->896/14  2736->2720/27
  2     30 ms  VA      -11.5 dB  848->672/26
  3     55 ms  VA       -9.0 dB  512->544/29  2512/30  3808->3824/30
  4     30 ms  VA       -8.0 dB  528->576/29  448->416/15  592->624/24
  5     95 ms  -        -7.0 dB  1776->1792/27  896/13  3552/30
  6     15 ms  F        -7.5 dB  1776->1840/29
230 C0
  1     30 ms  -       -64.0 dB  no tones
  2     65 ms  VAm      +7.0 dB  2080->800/16->8  2320/16->12  pulse Pitch-2->-1 Hz
  3      5 ms  VAm      +7.0 dB  800->688/8->13  2320/12->8  pulse Pitch-7->-5 Hz
  4     20 ms  VAm      +7.0 dB  688->448/13  2320/8->6  pulse Pitch-6->-11 Hz
  5     15 ms  VAm      +7.0 dB  448->272/13->23  2320/6->5  pulse Pitch-15->-4 Hz
  6     10 ms  VAm      +7.0 dB  272->224/23->9  2320/5->1  pulse Pitch-14->-12 Hz
  7     10 ms  Vm       +7.0 dB  224->176/9->21  2320/1  pulse Pitch-19->-17 Hz
  8      5 ms  Vm       +7.0 dB  176->160/21->3  pulse Pitch-8->-7 Hz
  9     15 ms  Vm       +7.0 dB  160/3->2  pulse Pitch-18->-2 Hz
  10     35 ms  Vm       +7.0 dB  160/2  pulse Pitch-13->-17 Hz
  11     10 ms  Vm       +7.0 dB  160/2->1  pulse Pitch-16->-1 Hz
  12     40 ms  -       -64.0 dB  no tones
231 C1
  1     35 ms  VA       +6.0 dB  704->1888/12  pulse Pitch+161->+424 Hz
  2     30 ms  VA       +1.0 dB  528->944/21  1696->2512/26  1776->2128/24  pulse Pitch+198->+397 Hz
  3     75 ms  VA       +1.0 dB  1024->1792/21  2336->1584/23  pulse Pitch+394->+215 Hz
  4    150 ms  VAF      -0.5 dB  1584->480/9  2096->1152/10  1744->3040/30  pulse Pitch+224->-8 Hz
  own: release 10 ms
232 C2
  1     80 ms  VA       -2.5 dB  528->592/29  880->1200/24  2336->1632/28  pulse Pitch-21->-24 Hz
  2     20 ms  VA       -8.0 dB  544/28  1280->1216/7  1824->1552/11  pulse Pitch-18->-24 Hz
  3     20 ms  VA      -20.5 dB  464->720/12  1072->1568/9  1312->1024/28  pulse Pitch-17->-23 Hz
  4     35 ms  -        -6.5 dB  3824->3008/10  noise 2496 bw 1120 /9
  5     55 ms  VAF      +2.0 dB  3376->2880/28  1376->1168/1  1536->1936/1  pulse Pitch-18->-21 Hz
233 C3
  1     30 ms  -       -64.0 dB  no tones
  2     25 ms  Vm       +6.5 dB  3184->3232/11->8  pulse Pitch-20->-31 Hz
  3     25 ms  Vm       +6.5 dB  3232->3152/8->17  pulse Pitch-20->-29 Hz
  4     10 ms  Vm       +6.5 dB  3152->3280/17->24  pulse Pitch-11->-43 Hz
  5    100 ms  Vm       +6.5 dB  3280->3840/24->14  pulse Pitch-26->-46 Hz
  6     30 ms  Vm       +6.5 dB  3840->3632/14->9  pulse Pitch-50->-28 Hz
  7     25 ms  Vm       +6.5 dB  3632->3520/9->4  pulse Pitch-42->-25 Hz
  8     15 ms  m        +6.5 dB  4000/2
  9     40 ms  -       -61.0 dB  no tones
234 C4
  1     75 ms  V        +2.5 dB  960->944/26  2688->3264/4  1808->2544/3
  2     55 ms  V        +1.5 dB  1968->1920/30  2768->2176/3
  3     85 ms  V        -2.5 dB  3008->2992/30
  own: attack 5 ms
235 C5
  1     50 ms  -        -7.5 dB  2912->2576/19  672->736/4
  2     20 ms  -        +4.5 dB  832->736/6  752->944/4  2496->1040/4->2
  3    105 ms  -        -5.0 dB  288->304/16  656->448/17  960->576/15
  4    190 ms  VD       +8.0 dB  320->192/19
236 C6
  1     25 ms  -       -64.0 dB  no tones
  2     25 ms  VAm      +7.5 dB  2016/6->7  pulse Pitch-29->-16 Hz
  3     30 ms  VAm      +7.5 dB  2016->1904/7->6  pulse Pitch-15->+41 Hz
  4     10 ms  VAm      +7.5 dB  1904->1664/6->13  2784/2->8  pulse Pitch-15->+35 Hz
  5     20 ms  VAm      +7.5 dB  1664->960/13->7  2784->2960/8->12  pulse Pitch+46->+53 Hz
  6     30 ms  VAm      +7.5 dB  960->912/7->14  2960->2192/12->15  pulse Pitch+41->+43 Hz
  7     10 ms  VAm      +7.5 dB  912/14->5  2192->1920/15->24  pulse Pitch+40->+42 Hz
  8     20 ms  VAm      +7.5 dB  912/5->6  1920->2528/24->23  pulse Pitch+38->+3 Hz
  9      5 ms  VAm      +7.5 dB  912/6  2528->2592/23->7  pulse Pitch+39->+1 Hz
  10     15 ms  VAm      +7.5 dB  912/6->14  2592->2944/7->9  pulse Pitch+45->-3 Hz
  11     25 ms  VAm      +7.5 dB  912->1600/14->20  2944/9->12  pulse Pitch+42->-3 Hz
  12     15 ms  VAm      +7.5 dB  1600->2048/20->16  pulse Pitch-19->-30 Hz
  13     25 ms  VAm      +7.5 dB  2048/16->12  pulse Pitch-24->-31 Hz
  14     30 ms  VAm      +7.5 dB  2048/12->6  pulse Pitch-17->-33 Hz
  15     50 ms  -       -64.0 dB  no tones
  own: click 24/256
237 C7
  1     45 ms  V        +2.5 dB  544/19  pulse Pitch-13->+4 Hz
  2     50 ms  V        +3.5 dB  656->2960/22  pulse Pitch+5->-6 Hz
  3     40 ms  V        +4.0 dB  480->2624/30  pulse Pitch+6->+38 Hz
  4     50 ms  V        +7.0 dB  720->2128/24  pulse Pitch+45->+93 Hz
  5     45 ms  V        +2.5 dB  592->1536/29  pulse Pitch+90->+126 Hz
  6     40 ms  V        +4.5 dB  384->1104/22  pulse Pitch+120->+148 Hz
  7     15 ms  V        +8.0 dB  1184->528/21  pulse Pitch+173->+150 Hz
  8     85 ms  V        -2.0 dB  464->416/23  pulse Pitch+166->-6 Hz
  own: attack 20 ms
238 C8
  1     90 ms  S        -5.5 dB  no tones  noise 1952 bw 288 /233  gate 20 ms  ramp -0.5 dB
  own: attack 5 ms
239 C9
  1     75 ms  S        -4.5 dB  no tones  noise 1712 bw 240 /194  gate 20 ms
  own: attack 5 ms
240 D0
  1    100 ms  -        +1.0 dB  DTMF pair/31+31
  2     55 ms  F        +1.0 dB  DTMF pair/31+31
241 D1
  1    100 ms  -        +0.5 dB  DTMF pair/31+31
  2     55 ms  F        +0.5 dB  DTMF pair/31+31
  own: release 10 ms
242 D2
  1    100 ms  -        +0.5 dB  DTMF pair/31+31
  2     55 ms  F        +0.5 dB  DTMF pair/31+31
  own: release 10 ms
243 D3
  1    100 ms  -        +0.5 dB  DTMF pair/31+31
  2     55 ms  F        +0.5 dB  DTMF pair/31+31
244 D4
  1    100 ms  -        +0.5 dB  DTMF pair/31+31
  2     55 ms  F        +0.5 dB  DTMF pair/31+31
  own: release 10 ms
245 D5
  1    100 ms  -        +0.5 dB  DTMF pair/31+31
  2     55 ms  F        +0.5 dB  DTMF pair/31+31
246 D6
  1    100 ms  -        +0.5 dB  DTMF pair/31+31
  2     55 ms  F        +0.5 dB  DTMF pair/31+31
247 D7
  1    100 ms  -        +0.5 dB  DTMF pair/31+31
  2     55 ms  F        +0.5 dB  DTMF pair/31+31
248 D8
  1    100 ms  -        +1.0 dB  DTMF pair/31+31
  2     55 ms  F        +1.0 dB  DTMF pair/31+31
  own: release 10 ms
249 D9
  1    100 ms  -        +1.0 dB  DTMF pair/31+31
  2     55 ms  F        +1.0 dB  DTMF pair/31+31
250 D10
  1    100 ms  -        +0.5 dB  DTMF pair/31+31
  2     55 ms  F        +0.5 dB  DTMF pair/31+31
  own: release 10 ms
251 D11
  1    100 ms  -        +1.0 dB  DTMF pair/31+31
  2     55 ms  F        +1.0 dB  DTMF pair/31+31
  own: release 10 ms
252 M0
  1   1190 ms  DW      +11.5 dB  1840->1856/17  noise 1632 bw 208 /5
  own: attack 5 ms
253 M1
  1    305 ms  D       +14.5 dB  no tones  noise 2016 bw 1424 /47
254 M2
  1    255 ms  VA       -4.0 dB  496->992/27  1728->2192/25  2928->3456/30
  2    260 ms  VA       -5.0 dB  976->448/30  2224->1664/30  3440->2880/27
  own: attack 5 ms, release 5 ms
```
<!-- EFFECTS-END -->

## Appendix C: the library's table encodings

**Peaking filters (fxeq).** A side table of four bytes a filter: the effect
(code − 200), the centre (16 Hz steps), the gain (half-decibels, signed) and
Q (tenths). At most three filters an effect; empty in this version.


These are the library's choices, not the chip's, given for anyone reading the
tables in `speakjet_tables.c`.

**Units.** Frequencies in 16 Hz steps, lengths in 5 ms steps, levels in
half-decibels as signed bytes. Every field is a byte.

**An allophone row**, 26 bytes:

```
kind  ms  f1 f2 f3  a1 a2 a3  nf nw na  cp  lv  cb  ob og cl rl wf rr tc th ba bl aa hp
```

`cb` is 1 where a stop's Repeat copies keep the voice bar. A glide's `f1` and
`f2` hold the indices of the pair. A side table `allog` lists the voiced stops
with a second frame: index, then three formants. The fields after `cb` (1.8) are
0 for "as the class": `ob` onset ms and `og` its gain (half-decibels, signed);
`cl` a stop's closure ms; `rl` a stop's release level (half-decibels, signed);
`wf` a white floor (/256); `rr` and `aa` the sound's own release and attack ms;
`tc` and `th` the starting burst's corner (32 Hz steps) and level (/256); `ba` and
`bl` a voiced stop's bar fade-in (ms) and level (half-decibels, signed); `hp` the
noise high pass corner (16 Hz steps). On a glide, `cl` and `ba` are its F1 schedule
(section 6.3).

**An effect's structure (fxs)**, 4 bytes an effect, 55 rows: its own attack (ms),
release (ms), starting click level (/256) and the click's low-pass corner (32 Hz
steps); 0 for the defaults.

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
                         square 3), o 16, u 32, m 64
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
1.8   2026-11-06   measured on the VoxRecord solo takes, both units (newest sessions), by band x time
                   difference against the chip's own take-to-take spread: the voice period from a 16-bit phase
                   step (floor(65536 / floor(rate x 65536 / 8000)) + 1); unvoiced fricatives switch on and off in
                   3 ms; nasals rise in 25 ms; a voiced stop's bar is there at once from silence, its upper
                   formants weak (x0.12), each stop with its own closure, bar level and release level; louder
                   onsets on stops and fricatives; the sibilants' starting burst of low noise (SE SO SH); a
                   white floor under WH's band; the fricatives' noise high-passed twice (400 Hz, own corners for
                   PE PO); per-sound attack and release lengths. Effects: a 5 ms broadband click at the start
                   (220 B0, 219 A9, 223 B3, 221 B1 and the others with one); the effect release 25 ms; C8/C9's
                   swept band 650 Hz - 2.4 kHz with its noise high-passed at 150 Hz; 219 A9 holds its tones 47 ms
                   before sweeping and ends on a held 100 Hz tone; 223 B3 glides between its tones over ~5 ms;
                   228 B8's upper tone starts 40 ms late. Glides: per glide, F1 may hold and close late (161
                   AXUW: F1 opens to ~640 Hz and closes in the last 60 ms while F2 falls throughout)
1.7   2026-11-05   Fast x2/3 and Slow x4/3, Slow on the voiced stops; the same sound twice plays
                   as a Repeat copy (VoxProbe 12, both units)
1.6   2026-11-04   the serial control protocol (section 2): commands, registers, the register synthesizer;
                   a register tone's level measured against 235 C5 and 241 D1 (VoxProbe 9, both units)
1.5   2026-11-04   the hold's images at the half rate; output kept faithful to the AtariVox; effect peaking
                   filters (7.11, unused); measured structures of 254 M2, 231 C1, 229 B9 (7.12); path lines
                   subtracted and the clock bands left out of scoring; the spectrum anchored on total energy;
                   the drawn-structure scoring; new open items (low Pitch on rebuilt voiced effects, M2's
                   sidebands, C7)
1.4   2026-11-03   flags m (3 ms on/off ramps) and u (banded columns, section 7.10); the bed is the chip's own
                   (Stella = A = B) and effect-specific (57 dB down under a register tone); the recording path's
                   clock lines at 8 and 16 kHz, now left out of scoring
1.3   2026-11-03   the period's twelve one-tick-long rates (VoxProbe 2 and 10); the default is the manual's Pitch
                   88 (it is one of them), no longer 87
1.2   2026-11-03   the robots' pitch pulse: R0-R3 carry a pulse at Pitch + a fixed offset (R0 175, R1 and R3
                   278 Hz), their "low tone and octave"; modelled for R0, R1, R3
1.1   2026-11-03   Bend below 5: the half step is +22 Hz (Bend 0 is -198 Hz, not -242), as the library and B2's
                   measured 1213 Hz at Bend 0 have it
1.0   2026-11-02   rewritten as an implementer's reference: host interface, tick loop, both period
                   rules (VoxProbe 9), the pulse and its shapes, noise, allophone and effect
                   construction, joins, Repeat, reading ahead, the exact output stage, clocks from
                   VoxProbe 9, known gaps, generated parameter appendices, table encodings
```
