# The SpeakJet: a technical manual for implementers

**Version 1.25, 2026-11-18.** The version rises with every change to what is known about the chip (new
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
- **open**: not known; the model's choice is stated;
- **assumed**: a rule fitted on one phrase or a handful of items, kept because it makes those match while a probe
  that tests it is pending. The library's source marks each one ASSUMPTION; section 6.8 lists them. An assumed rule
  may be wrong in contexts nobody has recorded.

Only recordings of the two reference units count. A recording of Alex Herbert's vox_test ROM on an unidentified
AtariVox (probes/VoxTest) is not used for anything in this document: its pauses run 25-55 ms under both units'.

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

**The long period (library 0.5.4, 1.23).** After silence a plain vowel's second period is 12 ticks (1.5 ms) long, not
its first: VoxProbe 15's solo takes (both units) give pulses at -23, -7, -1 dB with gaps 10.7, 12.3, 11.4 ms on AW and
11.5, 12.9, 11.8 on EH; the faint first pulse is why the earlier 4 / 16 / 28 / 40 ms reading took the long period for a
delayed start. All 12 plain vowels, IH and OH included: X 0.906 -> 0.883, every one better.

**The floor under the voice (library 0.5.4, 1.23).** The chip's vowel spectra meet ours at the harmonic peaks but sit
5-19 dB higher in the valleys between them (h8; AX UX AW IY, both units), without growing with the harmonic: a noise
floor, not jitter. The library adds the effects' signal-held floor (white noise times the signal's magnitude held 20 ms,
0.024) under voiced speech: 72 allophones X 0.867 -> 0.848, phrases 313.0 -> 311.4.

**The fall at the end (library 0.5.4, 1.23).** Fading into silence, the chip's vowel slows: OH's last periods on both units
run 11.48, 11.96, 13.25 ms (and 11.56, 12.56). Held steady, ours kept every harmonic sharp, and the whole-sound spectrum
sank 7-12 dB under the chip's between harmonics (OH AX UX, from 400 Hz up). The library lengthens each new period in a
vowel's or glide's last 25 ms by up to 14 ticks, in proportion to how far into those 25 ms it starts, when nothing joins
on: 72 allophones X 0.848 -> 0.840, OH 0.799 -> 0.746, phrases 311.4 -> 309.8. AW is left out: its fade has its own
rules, and with it every phrase ending on AW was worse.

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

**The source (measured).** The chip's hiss is Gaussian-distributed to the
sample: kurtosis 2.8-3.2, and 4-5 % of samples beyond two sigma. It is not a
tone whose frequency is thrown about, which would have a constant envelope.

**Checked: random to measurement (1.20).** Takes of the same sound first appeared to share one noise waveform
(take-to-take correlation 0.45-0.71 on FF HE TH). It was the recordings' mains hum: two different sounds match as
well (FF against HE 0.74), and above 500 Hz the match falls to 0.11-0.37, what random noise gives by chance over the
lags searched. The hum before each sound (about 112-138 Hz and its harmonics) also made an 8 ms "repeat". No
deterministic structure in the noise is shown.

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
`th`/64 of the band's level, fading linearly to nothing over 16 ms (1.10: the
scale was /256; SE's burst at the byte's top was half the chip's, and twice it
is better on every figure).

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

**Speed (measured).** Speech length follows two straight lines that meet at the default Speed 114:

```
length = L114 · (147 − Speed) / 33      Speed below 114 (the effects' law)
length = L114 · (162 − Speed) / 48      Speed 114 and above
```

where L114 is the length at the default Speed 114. Below 114 (measured, 1.11, VoxProbe 12, both units): 155 AY and
161 IYUW play x1.21 at Speed 108, where the single straight line gave x1.125 (Man Goes Down's phrase 1 runs at
Speed 108). Above 114 the older line holds at Speed 118 (Man Goes Down's phrase 2 keeps its length on it); Speed 127
has no recording of speech on either unit (VoxProbe 13 section 3).

**Fast and Slow.** Fast multiplies the next sound's length by 2/3 and Slow by
4/3, with a floor of 10 ms before the Speed law (VoxProbe 12, both units).
Neither changes an unvoiced stop or JH (TT is 45-50 ms plain, fast or slow);
Slow also lengthens the voiced stops by 4/3, Fast does not shorten them.
*Open: under Slow, 155 AY runs about 20 ms longer than 4/3 gives; VoxProbe 13
section 5 measures what part of a vowel Slow scales.*

**Two sounds whose length depends on a neighbour (library 0.5.4, 1.23).** Against the 88 phrase items of VoxProbes 4, 7
and 8 (both units), two sounds play a length in connected speech that their solo takes contradict, by one 5 ms step of
the tables. 128 IY straight after another sound (not after a pause) runs 5 ms shorter than from silence: its solo takes
want 130 ms (regions 5.4 against 14.7 at 125), the phrase items 125 (mean cost 321.5 -> 311.3, 7 items better, none
worse; 10 ms overshoots). 145 LE before another sound runs 5 ms longer than alone: solo 110 ms, the phrase items 115
(312.0 -> 309.4, none worse; after a sound instead: 311.5). A general rule (every sound, or every vowel, 4-7 ms shorter
before a sound) was mixed and is not used, nor 132 AY shorter before a sound (the two units disagree).

### 6.2 Moving between sounds

**The formants never jump (library 0.5.5, 1.24).** Each sound starts from where the last voiced sound left the formants:
they are held through a hiss and through a pause under 70 ms (both units, NE P4 AW: the AW opens on NE's F2), and SE and
TT set them to the alveolar locus, F1 400 Hz and F2 1600 (SE AW: the vowel's first ~12 ms on a low F1 under a 1.5 kHz
line). A pause of 70 ms or more clears them (held across P1, SE P1 AW and TT P1 AW read 57-140 % worse). **A vowel moves
at a rate**: 5 ms per ERB of F1/F2 distance, never under the common transition, stretched by Speed and Slow. These two
replaced four of the earlier per-pair rules and improved joins no rule had reached (SE P4 AW, TT P4 AW 16-40 % closer).

**Transition (measured).** The voice moves to each new sound's frame over
25 ms, linearly, from wherever the last sound left it. The transition is part
of the sound's length, not added to it, and does not scale with Speed.

**After silence.** At the start of a phrase or after a pause, the transition
starts on the new sound's own formants at zero level. The formants do not
sweep up from 0 Hz.

**Vowels, nasals and voiced fricatives** are two segments: the transition from
the last frame to the target, then the target held for the rest of the length.

**Crossing joins (1.14, ASSUMPTION in its constants; the shape measured on both units).** Between two sounds of the
vowel family (vowels, glides, and the liquids LE LO WW RR) the formants do not wait for the boundary. Each moves on
one straight line that starts inside the first sound and ends about 31 ms into the second; the line's length grows
with the distance: 5 + 5.5·d ms, at least 20, d the F1+F2 ERB distance between the first sound's last frame and the
second's first. Levels keep the 25 ms transition. Measured with LPC formant tracks on VoxProbe 13 section 5 (units A
and B agree within ~5 ms) against each join's boundary:

```
join      d (ERB)   chip move (F1 and F2 together)    25 ms rule
AW IY     11        ~100-175 ms from onset            150-175
IY AW     11        ~100-175                          141-166
OW IY     10.7      ~90-175                           136-161
AW EH     4.3       ~100-145                          109-134
UW AW     3.4       ~90-115 (VoxProbe 4)              short already
AW WW     ~3        ~100-115                          close already
```

Small moves therefore look like the old rule; large ones (front against back vowels, IY into WW) start up to ~50 ms
before the boundary. An earlier reading that F1 and F2 move at different times was a tracker fault: LPC at 8 kHz
swaps IY's F2 (~2180 Hz) for the next pole down. The constants were fitted on VoxProbe 13 section 5 together with
VoxProbe 4 section 5 and VoxProbe 8 section 10; held out, VoxProbe 4's AW IY P 0.949 -> 0.013, IY AW 0.805 -> 0.029.
Nasals keep the 25 ms rule from silence (MM IY, IY MM, NE IY: the chip moves in ~10 ms). **After a sound a true nasal
(MM NE NO NGE NGO) moves in 5 ms** (0.5.4; AW MM, IY MM: the vowel's F1 gone from the nasal at once on the chip; LE LO
WW RR keep 25). **AW** starts with a 20 ms fade-in, not its own 45, before an unvoiced sound or LE (AW TT, AW LE: 7-18 dB louder
in its first 20-40 ms on the chip), and fades out over 35 ms, or 25 after a pause, a slowed sound, LE or AYWW (AY AW, IY AW, NE AW 27-55 %
closer; Slow NE AW, AYWW AW 19-30 %). **TT before a vowel**, pauses between allowed, is 3 dB louder than alone
(TT P4 AW, Slow TT AW: the burst at -24..-29 dB against the vowel, ours -30..-34).

**Nasals carry a noise part (measured, 0.5.3).** On all four takes of MM and NE the chip's consecutive pitch
pulses match only 0.73-0.83 above 700 Hz (vowels 0.95-1.00; ours 0.94-0.97). Letting the formant sines run free
across pulses scored worse on every nasal, so the difference is noise, not phase. NE NO NGE already had a noise
band; 140 MM, 144 NGO, 145 LE and 146 LO now get one (2400 Hz, 1200 wide, level 3): MM 1.21 -> 1.01, NGO 1.09 ->
1.02, LE 1.16 -> 1.09, LO 0.87 -> 0.86, phrases 513.9 -> 513.0.

**Fricatives** are one plain segment: the noise band held, no transition.

**Joins measured against a real unit (library 0.5.4, 1.23).** The phrase items' cost has a yardstick now: each unit's
recording scored against the other unit's, mean 143.9 over the 88 items (the library 309.6 before these rules, 264.0 after).
Into TT a vowel's F2 climbs to the alveolar locus (1600 Hz) over its last 20 ms (AW TT: 856-1525 Hz emptied 12-21 dB and
1525-1922 filled in AW's last 25 ms); out of SE or TT, with no pause between, a sound's F2 starts there (SE AW opens on a
~1.5 kHz line for 15 ms). An unslowed TT before a vowel releases 10 ms longer. Across a pause under 70 ms the next sound
starts from the last one's formants (NE P4 AW opens on NE's F2, its F1 band 15-20 ms late; across P1 it doesn't). An unslowed
nasal hands over to a vowel 5 ms sooner (MM AW); a vowel with F2 under 1500 Hz ends 15 ms sooner into a nasal (AW MM; IY
MM wants no cut). AW before a short pause fades over 10 ms (AW P4 AW: the first AW ends sharply); a slowed nasal comes in
almost at once (Slow NE AW). Rejected on the same items: a vowel run-on before TT, a shorter TT aspiration, voicing under
TT's aspiration, longer pauses after a stop (the units disagree), the vowel's end fall before an unvoiced sound or after one
for AW, an AW F2 rise at every end, a shorter glide hold under Slow, Bend's effect changed.

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
worse); 189 OWWW comes close. Since 0.5.3 F3 holds with F1 until `cl` (both units: AXUW's F3 stays
near 2350 Hz until the late fall, where it had slid from AX's 2381 to UW's 2114 through the move;
1.21 -> 1.18). Holding F2 too, or keeping F3 fixed throughout, scored worse.

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

**The bar is partly noise (measured, 0.5.3).** On both units, above 700 Hz the part of the bar that changes from
one pulse to the next carries nearly all of the bar's energy (within 0-3 dB of the whole, BE BO DE DO); below 700 Hz
the bar is periodic. Consecutive pulses match one another only 0.40-0.59 above 700 Hz, where ours, all formants,
matched 0.76-0.81. So 170 BE and 171 BO's bar drops F2 and F3 for a noise band (`LBAR_NF` 1800 Hz, `LBAR_NW`
1400 Hz, `LBAR_NA` 6; BE 1.21 -> 1.14, BO 1.24 -> 1.20), and the alveolars DE DO ED OD keep their weak formants
with a noise band beside them (`BAR_NF` 1000 Hz, `BAR_NW` 800, `BAR_NA` 4; DE 1.29 -> 1.24, DO 1.14 -> 1.09,
OD 0.84 -> 0.79). The velars' bar is periodic on the chip (0.70-0.87) and stays as it was. Not done: the
alveolars' formants replaced as the labials' are (Man Goes Down P1, whose DO follows a pause, 1.2-1.5 % further),
and noise in EB OB (EB further). In BE IY the units disagree: unit A 3.0 % closer, unit B 3.2 % further.
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
173 OB, 176 ED, 177 OD. 193 TS's release decays (measured, 1.10: −13 dB over
its 95 ms on both units, every take, its low band — the white floor — holding
while the band falls): the band ends the release at `nd`/256 of its level, the
amplitude falling linearly (TS 128).

**After a sound (measured, 1.9; timing revised 1.11).** The closures above were measured from silence, where a voiced stop's closure is its
voice bar. After another sound the chip closes for at least 45 ms × the Speed factor: Man Goes Down's "goes" (NE then
GO), both units, sits 30-40 ms at −27 to −37 dB before GO's burst. With GO's own 8 ms the library ran straight on and
"goes" lost its /g/. Only stops played straight after a sound change (MGD P1, EH DE, AW TT, MGD P2 among the scored
recordings); repeats and stops after silence or a pause are as before.

**After a nasal (fitted, 1.11, on Man Goes Down's phrase 1, both units, band by band).** When a voiced stop follows
140-144 MM NE NO NGE NGO straight away, the chip's closure goes nearly silent in every band, 20-32 ms under the
row's peak −45 dB, closing at 473-485 ms into "man goes down". The library: the closure lasts 38 ms (× Slow when the
stop is slowed, × the Speed factor); the nasal fades into the bar over the first 65 % of it; the bar is the stop's
own, 16 dB down, with F1 at 0.3 and F2 and F3 at 2.5 times the usual bar's; the nasal's F2 and F3 fall into it (to
0.92 and 0.82 of theirs: NE GO, F2 1640 → 1510 Hz, F3 2615 → 2140) instead of rising to the stop's; the release
then rises from 10 dB down over 10 ms, with no onset boost. 145-148 LE LO WW RR, which the library files with the
nasals, do not count: "Hello, world"'s LO ED has no recording. Vowel into voiced stop (VoxProbe 4's EH DE) shows no
such gap on either unit.

**179 GO before OW (assumed, 1.11, Man Goes Down's phrase 1).** Followed at once by a sound starting on 137 OW (OW,
OWRR, OWIY, OWWW), GO releases on F2 1430 and F3 1920 Hz, F3 the stronger by 4-5 dB, two level bands that then split
into OW's F2 and F3 (the user's drawn lines, matched to both units). Alone it keeps its table formants, which its solo
takes need (moving them there worsens its solo spectrum 2.28 → 4.12). No other context of GO is recorded (VoxProbe 13
section 7).

**A voiced stop into a vowel (assumed, 1.11, every recorded voiced-stop pair, both units).** With no Slow on the
vowel (none among the modifiers between them): the stop loses 30 − 10·d ms more off its end, d the ERB distance
between its release's F2 and the vowel's (BE EH 168 ms on the chip, 198 before; DO AYWW 284, 315 before; BE AW and
GO OWWW, far apart in F2, already matched); and a voiced stop from silence that isn't itself slowed closes for at
most 30 ms (175 DO's 44 ms closure alone made DO AYWW 31 ms long; DO alone needs the 44). With a Slow on the vowel
neither applies: Man Goes Down's "down" (DO, Slow AYWW) keeps DO's closure, as both units show.

**Onsets (measured, 1.8).** The voiced stops' and fricatives' first 1-8 ms play
3-19 dB over the rest (VoxRecord solo takes, both units). A sound may carry an
onset: its first `ob` ms play `og`/2 dB louder; in a stop or affricate the onset
starts at the release, not the closure. A sound with an onset, from silence, starts
on its own frame at full level, without the attack ramp. Since 0.5.0 `og` is signed: a negative one is a quiet
onset. 0.5.3 uses it on 156 NGO (15 ms 6 dB down, 1.15 -> 1.09) and 194 KE (5 ms 3 dB down, 0.84 -> 0.76); every
other voiced stop and nasal was scanned (5-15 ms, -3 to -12 dB) and kept its own. 198 PE and 199 PO share one
timing: a 15 ms closure with the low pulse at 25 ms (PE 1.11 -> 1.05, PO 1.09 -> 1.07).
187 SE carries the same low pulse (library 0.5.4, 1.23): under 100 Hz its four takes show one damped swing of the same
shape, sign and timing (+ at 3-11 ms, - at 15-19 ms after the onset), a deterministic column, not noise. Fitted to the
waveform (the takes' median, -5..60 ms): 40, 50 Hz, from 3 ms, 14 ms decay, phase 0 (waveform error 0.89 -> 0.51;
regions 26.9 -> 21.9). 189 SO's takes split two and two and 190 SH's disagree in sign, so neither carries one.

**JH** is a silent closure of 30 ms (not Speed-scaled), then the voiced
fricative held.

### 6.5 Joins

**Rebuilt from VoxProbe 16 (library 0.5.5, 1.24).** VoxProbe 16 recorded every kind of join on its own, on both units
(solo sounds in the same session; nasal, vowel, liquid, hiss and stop joins; Speed and Slow on joins; Man Goes Down's
phrases as sent and with each control stripped; its joins alone with their controls). The rules fitted to Man Goes Down's
phrases through a phrase score (1.11-1.23) were compensating for each other - a correct nasal overlap scored worse against
them - so they are gone, and the joins are judged on VoxProbe 16's items: the timing of each formant move against both
units' F1/F2 tracks, and the levels in sixteen bands with every take lined up on its own onset. On those items the
library stands at 4.07 dB mean band error (0.5.4: 4.57); unit A against unit B gives 2.77.

**Cutting short (measured).** A sound followed at once by another is cut short at its end: by 15 ms, or 30 ms when its last
segment is voiced and the next sound is a voiced allophone. Exceptions, each from recorded pairs:

- **vowel or glide into vowel or glide** (VoxProbe 4, 13 and 16, both units): 64 - 7.5 d ms plus the two sounds' own join
  terms, d the ERB distance of 0.6 x F1 plus F2 between the sound's last frame and the next sound's first; below zero the
  sound runs on by that much. F1 counts less than F2 (1.25): IY EH and EH IY overlap 18 ms, IY UW and UW IY run on 18 ms,
  all four 7.1 ERB apart on F1 + F2, the first pair mostly an F1 move, the second F2. On the 20 recorded vowel pairs the
  library's lengths are within 8.3 ms rms (12.3 with F1 and F2 equal); a least-squares fit puts F1's weight at 0.16, but
  below ~0.5 the handovers come early (move timing 12.2 -> 14.0 ms). WW and RR
  take this rule too; **LE and LO do not** (VoxProbe 16: LE AW and AW LE within 6 ms off it, 12-16 ms early on it).
- **a nasal into a vowel** overlaps by 13 ms, MM by 22 (VoxProbe 16, both units: MM hands over ~22 ms before its own end
  alone, NE ~13, whatever the vowel - a formant-distance rule put NE IY and NE UW 22-32 ms early), scaled by Speed and Slow.
  **A vowel into a nasal** overlaps by 15 ms. The 1.13 rule, no overlap unless either is slowed, was wrong both ways.
- **a vowel after NGE** (1.25) moves from NGE's frame 6 ms longer per ERB of F2 distance and is that much longer, as after
  GE at 8 (VoxProbe 16, both units: NGE AW 242 ms, NGE UW 243 where the plain join gave 195; NGE IY on time).
- **a stop on either side** takes the short 15 ms; after an unvoiced stop there is no cut at all (the TT AW pairs, VoxProbe
  4, 8 and 9, both units). A stop into a vowel: section 6.4.

**Toward a hiss (fitted, 1.11, Man Goes Down's phrase 4, both units; confirmed by the formant tracks of VoxProbe 4's AW SE,
1.24).** A voiced sound followed at once by 187 SE bends its F2 and F3 over its last 40 ms to those of 167 ZZ, SE's voiced
partner. Only SE is recorded this way.

**Speed on joined speech (VoxProbe 16, 1.24).** Below Speed 114 a vowel or glide takes its own law, (149.3 - Speed) / 35.3
(both units: AW IY and MM AW at Speed 90 x1.65-1.68 over their Speed 114 lengths; Man Goes Down's phrase 1 at Speed 108 is
consistent), and every other speech length the speech law (162 - Speed) / 48 - no longer the effects' law, which gave
Speed 90 MM AW x1.84. Above 114 the step law of 6.1 stands, and a nasal above Speed 118 takes it as the vowels do (Speed
127 MM AW hands over at 38-56 ms where the nasal law put it at 80-102). Speed 127 is exact on both pairs.

**After a pause (VoxProbe 16, 1.24).** An unvoiced stop after a pause code closes 15 ms longer (UX P1 EK 260 ms on both
units; 1.13's 30 read 10 ms long). The next sound starts from the formants the last voiced sound left (4.1); the 1.23
rules for a vowel's attack and AW's release around a short pause are gone.

**Removed in 1.24** (each had been fitted on the phrases, not on a recorded join): IY 5 ms shorter after a sound, LE 5 ms
longer before one, a vowel 10 ms shorter after MM, a nasal 5 ms shorter and a slowed nasal 10 ms longer before a vowel, a
low-F2 vowel 15 ms shorter into a nasal, a slowed vowel's F1 move into a nasal, TT 10 ms longer before a vowel, a slowed
glide's 25-50 ms run-on before a Slow (1.11's phrase 4 rule), a slowed nasal's quick attack, and phrase 2's "TT P1 BE"
closure exception. The rebuilt joins also fit VoxProbe 13's 87 recorded pairs better than 1.23 did (length error 18.1 ->
13.5 ms rms).

**Slow above Speed 115 (1.25, provisional).** A slowed vowel or glide takes the same Speed step as an unslowed one above
115, not the effects' law: Speed 118 FF RR Slow UW 210 ms against the units' 200-210 (was 225), Man Goes Down phrase 2 1675
against 1690/1685 (was 1725). Fitted on those two; the VoxProbe 16 band judge is unchanged (3.947 -> 3.949).

**Still open (section 13).** Slow on a vowel joined after a sound at Speed 108 runs ~35 ms shorter on the chip than
x1.29 x the Speed factor (MM Slow AY 285 against 320, Slow AY Slow NE 320 against 355). Letting it ignore Speed fits both
but takes phrase 1 to 1440 against the chip's 1490 and the band judge 3.947 -> 3.993, so it is not adopted; a
voiced stop after a pause (phrase 2's TT P1 BE: a 20 ms bar 25 ms in, then 95 ms of quiet); PO after a nasal (VoxProbe 13's
NE PO OW, its length now 300 against 285/295: the closure still 25 ms long, the release short).

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
- **187 SE** has a copy of 80 ms (measured, 1.11: Man Goes Down's phrase 4 plays Slow SE twice; the hiss lasts 192 ms
  on both units, 159 with the old 45 ms copy, 194 now).
- **Glides.** Each copy glides back from where the last ended to the first
  frame and on again, in the same fifths and tenths as the first play (149 IYRR
  × 5 rises 1383 to 2083 Hz over its first 60 ms each time).
- **JH and ZH** have copies of 80 and 70 ms.

The attack and release ramps apply once, over the first play and all its
copies together.

**The same sound twice.** A sound sent straight after itself, with no pause
or control between, plays as a Repeat copy of the first: no new onset and no
overlap cut between the two (VoxProbe 12, both units; matches within 1-13 ms).

### 6.8 What was fitted on Man Goes Down, and what changed (1.11; the join rules here were replaced in 1.24, see 6.5)

Rules fitted on the game's phrases are listed here so they can be told apart from what holds everywhere.

```
rule (section)                                   basis                                      status
speech Speed law below 114 (6.1)                 VoxProbe 12, single sounds                  measured
quiet closure after a nasal (6.4)                Man Goes Down phrase 1, band by band         fitted
GO's release before OW (6.4)                     phrase 1 and the user's drawn lines          assumed
a stop's cut: short after any stop (6.5)         the code's own rule; GO OWWW pairs           measured
no cut after an unvoiced stop (6.5)              VoxProbe 4, 8, 9 TT AW pairs                 measured
voiced stop into vowel: F2 trim, 30 ms cap (6.4) every recorded voiced-stop pair             assumed
vowel/glide joins by F1+F2 distance (6.5)        VoxProbe 4 vowel pairs                       assumed
bend toward ZZ before SE (6.5)                   phrase 4                                      fitted
SE's 80 ms copy (6.7)                            phrase 4                                      measured
slowed glide before a Slow +50 ms (6.5)          phrase 4 only                                 assumed
pause lengths P0 P3 P5 P6 (8)                    VoxProbe 1 section 5; phrases 1 and 2's gaps   measured
```

Active length (ms, within 30 dB of the peak and 10 dB over each take's background), unit A / unit B / library before
these rules / library now:

```
phrase                                    unit A   unit B   before   now
phrase 1 "man goes down"                   1495     1502     1518     1502
phrase 2                                   1695     1700     1775     1700
phrase 4                                    942      945      858      925
VoxProbe 12 DO AYWW                         280      282      315      288
VoxProbe 4 AW IY                            261 (both units' median)  215      270
VoxProbe 4 BE EH                            168 (median)              198      168
```

Every single sound renders bit for bit as before these rules: each one acts only between two sounds. Man Goes Down's
phrases were checked against both units by eye on spectrograms as well as by length; the scorers' E and P were not
used for timing (one global lag over a whole phrase: moving 5 ms of silence in a gap cost E 1.7).

---

### 6.9 What VoxProbe 13 confirmed and corrected (1.13)

VoxProbe 13 build 5 was recorded on both units, 2600 and Stella, on 2026-11-08 (four takes). Each item's active
length was compared with the library's: within 30 dB of the peak and 10 dB over the take's background, the chips'
median against ours. Mean |error| per section, 0.4.2 to now: Speed near 114 12.3 to 11.6 ms, joins 14.1 to 10.2,
into a voiced stop 20.1 to 12.9, a stop's release 14.9 to 11.8, pauses 29.6 to 23.5, unvoiced stops and hisses
29.9 to 24.8. Man Goes Down: phrase 1 1502 ms, phrase 2 1712, phrase 4 940, against 1495/1502, 1695/1700 and
942/945 on the two units.

- **Pauses scale with Speed** on the effects' law, (147 - Speed) / 33. Pitch and Bend don't change them (AW P2 AW
  and AW P6 AW at Speed 100, 116 and 127, Pitch 55 and 110, Bend 1 and 4). At Speed 116 and 127 the chip's pauses
  are still 40-70 ms shorter than ours, so the true law is steeper there. Open.
- **The speech Speed law above 114** is too gentle for vowels and glides: IYUW, EYIY and OWWW alone are 18-30 ms
  long at Speed 118-127. The effects' law fits them, but it shortens Man Goes Down's phrase 2 (Speed 118, mostly
  consonants and Slows) to 1598-1665 ms against 1700. Not changed. Open.
- **A Slow on the second of two equal sounds** stops it playing as a Repeat copy: AW Slow SE Slow SE UX plays two
  whole SEs. SE's copy stays 80 ms: by picture the chips' AW SE SE runs to ~275 ms, as 80 gives (a 25 ms copy, fitted
  first, followed a length measure that loses quiet hiss under the takes' hum; see section 12).
- **The slowed-glide lengthening of 6.8 is confirmed in form** by picture on both units: Slow IYUW Slow SE's IYUW ends
  ~20 ms after ours without it, and Slow IYUW SE (no Slow after) already matches. It is 25 ms, not 50.
- **A stop into a vowel:** the extra trim of 6.8 now also applies with a Slow on the vowel (BE Slow EH, DO Slow AYWW
  were 40-49 ms long). *Assumed:* GE and GO take no extra trim (they were 15-50 ms short). GE before OW and UW is still
  40-50 ms short, for a reason not yet found.
- *Assumed:* after a sound, DE and DO close 25 ms longer and GE and GO 15 ms longer; after a nasal, DE DO GE GO close
  10 ms longer than BE and BO. Every stop after NGE is still 38-48 ms short.
- *Assumed:* a nasal (MM NE NO NGE NGO) and a vowel or glide join with no overlap (IY MM, MM IY, IY NE, NE IY were
  26-38 ms short), unless either is slowed. All three of phrase 1's nasal joins are slowed, and it keeps its length.
- **The search that followed (2026-11-08).** Each rule above was switched off in turn and scored two ways: VoxProbe 13's
  item lengths, and timing inside Man Goes Down's phrases (each phrase's spectrogram warped onto each unit's; the mean
  distance from the diagonal, ms). That showed the nasal and closure rules above moving phrase 2 by 50 ms. The rules
  now, all *assumed* unless stated:
  - above Speed 114 an unslowed vowel or glide, its Repeat copy, and a pause shrink by (155.9 - Speed) / 47.4, a step
    down from 114 (x0.80 at 118, x0.61 at 127, as VoxProbe 13 section 3 measures; the effects' law gives x0.88 at
    118), slowed ones keep the effects' law, and the other speech sounds don't shrink at all. This is what lets Man
    Goes Down P2 (Speed 118) and VoxProbe 13 agree: with the step on the vowels alone, P2's consonants ran 15-40 ms
    short each; before it, P2's vowels ran 15-30 ms long and hid that;
  - the nasal no-overlap rule is for vowels only, not glides (phrase 2's MM EYIY keeps its overlap);
  - the extra 10 ms after a nasal is not applied under Slow (phrase 1's Slow GO);
  - LE LO WW RR join vowels by the vowel join rule (phrase 2's IY WW IH);
  - SE SO SH take half of Slow's stretch;
  - an unvoiced stop straight after a pause code closes 30 ms longer; a slowed one after a sound, 20 ms longer;
  - a voiced stop after a pause keeps its whole closure (the 30 ms cap of 6.8 is not applied);
  - after NGE a voiced stop closes 40 ms longer;
  - no overlap into TH; TH between a voiced sound and another sound plays 25 ms longer (not before a pause);
  - the voiced-stop-into-vowel rule (30 - 10 x F2 distance) lengthens the release when it falls below zero (by
    picture: the burst lands with the chips' and the vowel after runs longer when F2 has further to go);
  - KE and KO after a nasal close 30 ms longer (NE KO OW by picture: KO bursts 30 ms after ours on both units);
  - an unvoiced stop's closure after a sound is silent after a 10 ms fade (measured: both units go quiet for 20-40 ms
    before every TT KO PO; ours faded across the whole closure and the runs of sound didn't match on 11 items);
  - no overlap from a vowel, glide or hiss into TT, nor from NE or NO into KE KO PE PO BE BO; PO into a voiced sound
    after a vowel overlaps 20 ms;
  - the sound before a slowed unvoiced stop runs on 40 ms (the Slow read ahead), unless it is a nasal or a pause follows
    the stop; the slowed stop then releases 15 ms longer before a voiced sound, and closes 20 ms longer only before a
    pause (Man Goes Down P2's UW Slow TT P1);
  - an unslowed NE or NO straight into GO or GE runs on 18 ms, 25 more when the GO is slowed, not with a Fast on it
    (NE GO OW's NE 90 -> 110 ms against 110-120; Man Goes Down P1's NE is slowed and keeps its fitted length);
  - the stop-into-vowel lengthening is not applied after BO; the GE glide is 8 ms per ERB (10 ended GE OW 10 ms late);
  - a vowel after GE glides from GE's F2 over 10 ms more per ERB of F2 distance, and is that much longer (by picture:
    GE OW's F2 falls from ~2000 Hz to OW's over ~60 ms on both units).

  Measured by picture on both units, not assumed:
  - a sound after a hiss starts as from silence: no sweep of formants or noise band from the hiss (SE TT AW, SE UX);
  - no starting burst on a sibilant straight after a hiss (the second SE of SE SE: this was phrase 4's dark blob).

  Every single sound at the default Speed is unchanged. Timing inside the phrases, 0.4.2 to now: phrase 1 14.7 to
  12.3 ms, phrase 2 16.7 to 8.5, phrase 4 9.9 to 6.8; lengths 1488, 1700 and 940 ms. VoxProbe 13's six sections
  summed (mean |error| per section, lengths13c): 55.0 ms. Boundaries inside items (each run of sound's start and
  end against the units', on the 38 items whose runs match on both units): 8.3 ms mean.
- **Tried and not taken:** a slowed TT's release stretched by Slow (the chips' aspiration runs ~75 ms after a vowel;
  no gain); a vowel running on before a Slow and a hiss (VoxProbe 13 AW Slow SE shows ~30 ms, but Man Goes Down P2's
  IH Slow TH gets worse); GO into a vowel by the GE rule (section 6 worse); the vowel join constants refitted.
- **Pauses, corrected placement.** Unit A's 2600 take had section 9 shifted by one item from Speed 100 AW P6 AW to
  Speed 127 AW P6 AW (an extra run at 223.65 s; the run count still matched). Placed by hand from the runs of sound,
  both units agree within 15 ms. Measured from each AW's start: the pause factor is x1.40 at Speed 100, x0.81 at 116
  and x0.61 at 127, which the laws above give; P2 is 188 ms, not 200 (the second AW started 10-20 ms late at every
  Speed and Pitch; section 9 12.0 -> 9.4 ms).
- **Still open:** Speed 115-117 (the step is fitted from 116 up);
  AW IH AW and AY IY 30-40 ms long; Man Goes Down P4's slowed IYUW ~25 ms early and its UX copies ~15 ms long (a
  fix of one moves the error to the other); NE before GO at Speed 90 and 127 (17 and 30 ms long: the second is the consonant-law conflict below); BO into AY EH 20 ms long; consonants above
  Speed 114 (P2 at 118 wants them unscaled, NE GO OW at 127 wants them scaled); IY after a voiced stop 15-30 ms short (a weak tail on both units); RR after a sound
  ~20 ms short; GO IY 30 ms short; a slowed TT's closure and aspiration; the death sound slowed ~20-50 ms short; the transitions'
  shape. VoxProbe 14's plan (in the lab's notes) carries a test for each.

### 6.10 What VoxProbe 14 measured (1.16)

VoxProbe 14 build 1 (sha1 f94b1488) was recorded on both units, 2600 and Stella, on 2026-11-09 (four takes). Item
lengths are taken band by band as in 6.9 (probes/VoxProbe14/analysis/lengths14c.py). The two units agree within
~10 ms on nearly every item, so each figure below is ours against the chips, for the library 0.4.3 (ours - chip).
Modelled so far: the Speed step, the join terms, KO after a nasal, the KE locus, Slow on KO and PO.

- **The Speed step is at 116, not 115.** At 115 the chip plays as at 114 (IYUW 225 ms at both). The library
  shortens from 115: IYUW -30, AW P2 AW -35 ms. At 113 ours is 15 ms long (AW, IYUW). *Modelled:* the step now
  starts at 116 (section 2 mean |error| 12.0 to 10.0 ms, worst -35 to +15); Speed 115 plays as 114.
- **Consonants shrink above 114** (the assumed "no change" is wrong). NE between AWs: +30, +35, +60 ms at Speed 114,
  118, 127; NE GO OW at 122 +22. DH between AWs is short: -32 and -50 ms at 114 and 118; FF at 118 -22.
- **Slow on an unvoiced stop.** The fitted +20 ms closure is right for TT, too much for KO and PO (+30, +25).
  Followed by P2, all three are 20-35 ms short: a pause after a slowed stop is longer on the chip. By picture and
  by parts: before a slowed TT the vowel ahead runs on (~155 ms) and TT's release grows; before a slowed KO or PO
  the vowel doesn't (~130 ms, as unslowed), the release is as unslowed, and the closure grows instead (KO 56 against
  44 ms, PO 101 against 85). *Modelled:* the run-on and the release's +15 ms are TT's only; a slowed KE KO PE PO
  after a sound closes 20 ms longer before a sound as well as before a pause. AW Slow KO AW +30 to -5, AW Slow PO AW
  +25 to -10 ms; section 4 15.5 to 11.8. Still open: after a slowed TT the chip's P2 is ~44 ms longer than ours; PO's
  release noise runs ~40 ms against the chip's ~15 (plain or slowed).
- **A hiss next to a vowel.** AW SE AW and AW TH AW are right, but AW Slow SE AW is -32 and IY SE AW -25 ms. A hiss
  into a vowel from a Reset is short: FF OW -25, SH IY -25 ms. The lone hisses (SE, TH, Slow SE, Slow TH) are right.
  By picture the hiss itself is what is short (SH before IY ~55 against ~80 ms, FF before OW ~65 against ~75, SE
  between vowels ~70 against 90-100), and the vowel after it 5-20 ms long; SH FF TH alone are within 5 ms. Taking the
  15 ms overlap away from every unvoiced hiss into a vowel fixed SH IY, FF OW and IY SE AW (to -10) but put SE AW, SE
  UX and the TH items 15-30 long, section 9 8.3 to 14.4 ms and Man Goes Down P4 955 against 942/945: not modelled.
  Open.
- **Velars:** KE before AW, OW, UW is 32-50 ms short; GO IY -30, KO IY -20. GE is right before every vowel. By
  picture on both units, the vowel after KE starts at a velar locus (F2 ~2000-2500 Hz, F1 low) and glides to its own
  over 60-80 ms; after KO, F2 starts ~1000 and rises into IY. Where the locus is the vowel's (KE IY, KO OW) there is no
  glide. *Modelled (KE locus):* after KE a vowel starts at F2 2100 Hz, F1 300, and glides 8 ms per ERB of F2
  distance, growing 4 ms per ERB: KE AW -30 to -5, KE OW -45 to -15, KE UW -30 to -5 ms, with the glide's shape on
  the pictures; section 7 11.7 to 8.0 ms. KO is not modelled: no single locus fits KO IY (-20) and KO EH (+5).
- **Joins are long.** AW IH +22, AW IH AW +43, IH AW IH +45, AY IY +35, AY IY AW +40 ms: about 20 ms a join.
  IY AW IY is right. *Modelled (join terms):* across every recorded vowel and liquid join (VoxProbe 4, 13, 14) the
  error doesn't follow the distance law; it follows the sounds. IH's joins are long whichever side it is on, RR's
  short. A few sounds carry a term of their own added to the join's cut (alloj): IH +15, AW +6, OW -10, RR -16 ms.
  The 26 joins without AY go from rms 16.8 to 4.1 ms (max 42 to 10); fitted on VoxProbe 4 and 13 alone, the terms
  took VoxProbe 14 from rms 30.1 to 10.6. Every sound alone and Man Goes Down's phrases are unchanged. AY IY stays
  ~32 ms long: on both units AY holds ~70 ms before F2 rises to IY (ours ~110), while AY EH is right. Open.
- **A nasal before BO or KO is long.** NE BO AW +45, NO BO AW +40, MM KO AW +37, NE KO AW and NO KO AW +30 ms. Before
  DO and GO it is right, and so is NGE before every stop. Taken apart by picture (2026-11-09) it is three faults:
  KO's closure after a nasal was 11-30 ms long (and 15 on VoxProbe 13's NE KO OW): *modelled*, 15 ms extra, not 30.
  BO and the vowel after it run ~30 ms long whatever comes before (see BO into a vowel). And KO's release with the
  vowel after it runs ~14 ms long, after AW as after a nasal. Taking the overlap rule for NE and NO into these stops
  away fits the item lengths but cuts NE ~20 ms short of both units: the rule stays. MM KO's gap is still ~10 ms long,
  and NGE before a stop is ~40 ms short in its own sound (~90 against ~130 ms), where VoxProbe 13's fit put the
  missing time into the closure.
- **BO into a vowel is long** (BO AW, AY, EH: +20 to +22 ms). BE into the same vowels is right. Over all 33 items
  of BE BO DE DO into a vowel (VoxProbe 4, 13, 14), the error follows neither the F2 distance the trim rule uses nor
  F1 with F2: BO before AY EH IH AW runs 15-27 ms long by picture all after the release, BO before OW UW IY within
  10; DO and DE before IY run 18-25 short, BE before IY 8-15 short while Man Goes Down P2's "be with" (BE IY) is
  right. A BO term and an IY term fitted VoxProbe 14 from VoxProbe 13 (rms 13.5 to 7.3 ms) but overshot BO OW, BO UW
  and BE IY and put Man Goes Down P2 18 ms long: not modelled. GE and GO are outside the trim rule (GO IY -30). Open.
- **A voiced stop into IY is short** (DO IY -20, GO IY -30). TH RR AW is -38.
- **The slowed glide:** Slow IYUW Slow AW +60, and Slow IYUW Slow SE AW +25 at Speed 100 and +80 at 127. Slow AYWW
  and Slow OWWW before Slow SE AW are -35 and -28. The 25 ms lengthening is wrong in size, and it changes with Speed
  and with the glide. By picture on both units: Slow's stretch of IYUW falls with Speed (x1.30 at 100, ~x1.24 at
  114, ~x1.0 at 127, where the slowed IYUW is ~130 ms, as plain; ours stays ~x1.35, 177 ms); in Slow IYUW Slow AW the
  AW is ~125 ms, as plain, where ours is ~165; after Slow AYWW and Slow OWWW the slowed SE is ~120 ms, ours ~80-90.
  Applying the step above 115 to slowed sounds as well put Man Goes Down P2 52 ms short (1648 against 1695/1700):
  not that. Open.
- **Slowed effects (Speed 0):** R8 R9 C9 within 15 ms; B0 +17, C8 -28, R5 -30.
- **Answered, no change:** copies of SE and SH, UX copies with Pitch codes between, M2 (with and without Pitch 150),
  RR and IY alone.
### 6.11 Rebuilt against both units (library 0.5.0, 1.18)

Every allophone and effect in 0.5.0 was judged by one measure: the render's spectrogram, at six resolutions from 1.3
to 43 ms, against the chip's four takes of the sound (both units), each take's background subtracted, the render
scored at the best of shifts of -12 to +12 ms round its onset and the distance divided by the takes' own
leave-one-out spread. 1.0 on that measure is "as far from the chip as another take of it". A rule went in only when
no sound got worse under it. Against 0.4.3 as shipped: allophones 1.07 -> 0.94 (56 closer, 4 further: IY IH OH UW),
effects 1.22 -> 0.98.

What the allophones gained, each measured on both units:

- **Attacks per vowel and glide**, 20-60 ms (AW AWRR AXRR AXUW AYWW IY IYRR IYUW OH OW OWIY 45; EYIY EYRR IHWW OHIH 60;
  AX EH EY OHIY OWWW UX 30; AY EHLL 20; IH UH OWRR IYEH the 35 ms default; UW 12). The chip's first pulse sits 4-12
  dB under its body and the second within 1-4 dB, in every context measured (solo, after TT P4, after P1, at Pitch
  150). A ramp that starts at once and runs 45 ms, with the first pulse immediate, reproduces that; a delayed first
  pulse, a per-pulse weighting and a ramp from a floor were each tried and scored worse on 11-45 allophones.
- **The attack is in pitch periods above Pitch 88**: aa ms at 88, shortened by 88/Pitch above it (VoxProbe 7's "Pitch
  150 then AW/IY/OW", both units, 5-17 % closer), never lengthened below it (Man Goes Down's IYUW at Pitch 76 is full
  within its first pulses).
- **The first voicing period of a plain vowel from silence is 12 ticks longer** (every plain vowel but IH and OH), and
  EY UX UH AW's pulse falls over 72 ticks, not 58 (section 4.2). Library 0.5.0 applied both to all twelve plain vowels;
  measured per vowel on the corrected measure (1.19), each suits some vowels and hurts others: OH with 0.4.3's entry and
  neither rule scores 0.88, with both 1.15. 0.5.1 applies them per vowel (11 allophones closer, none further).
  **Corrected in 0.5.4 (12.6):** that per-vowel choice, and the per-vowel attacks above, were made on a measure that could
  place a voiced sound a voice period off. On the corrected measure the 72-tick fall is worse for all four vowels and is
  gone (AW 1.07 -> 0.92, UH 1.04 -> 0.92, EY 1.04 -> 0.93, UX 1.03 -> 0.96); eleven attacks were re-fitted (OHIH 60 -> 0
  ms default, EYRR 60 -> default, IYRR 45 -> 10, AWRR 45 -> 60, EYIY 60 -> 20, IY 45 -> default, IYUW 45 -> default,
  IH -> 60, IYEH -> 60, OW 45 -> default, OWIY 45 -> default); BE TS LO AYWW's fade-outs too. The first period's 12
  ticks fall on the silent opening transition, before the first pulse that is heard, so they change nothing audible;
  moving them onto the first heard pulse scored worse, and they are left as they are.
- **PE and PO's low pulse swells in over 10 ms.**
- **Joins.** A vowel or glide after a voiced stop other than GE GO opens at its own level and amplitudes while its
  formants still glide from the stop's release (Man Goes Down's DO AYWW: the vowel at full level straight after the
  bar). A voiced fricative after a pause opens with 40 ms of its noise alone before its voicing (P2's EYIY P4 DH:
  the chip's frication 40 ms, 10 alone). A voiced stop after a short pause (P4-P6) opens its bar 12 ms into the
  pause (P1's ZZ P4 DO: the bar 11 ms after ZZ's end). The longer closure after a sound (6.9) belongs to DO (+25 ms)
  and GO (+15) only; DE and GE had it by analogy and ran 25 ms long after a vowel (VoxProbe 4's EH DE). A crossing
  cut (6.2) longer than a glide's last segment trims across its segments instead of being skipped (VoxProbe 8's
  AYWW AW, 38-42 ms long on both units, now within 4).
- **A vowel or glide above Pitch 88 runs longer** (assumed linear in Pitch: 16 % at 150; VoxProbe 7's "Pitch 150 then
  AW/IY/OW" on both units run 133-134 ms where the same sounds at 88 run 114-123 and the library gave 113-117; one Pitch
  measured, so the shape is an assumption).
- **Slow on a plain vowel is x1.29** (VoxProbe 8 section 2, both units: Slow AW and IY 149-150 ms where x4/3 gave
  154-157); glides and nasals keep x4/3 (6.1).
- **Levels.** SO, PO, OG and EK were 4-12 dB loud; HO's unreleased fit rendered 40 dB quiet; TS ended 18 ms early.

What was tried and rejected, each on both units' recordings: no attack ramp within 100 ms of the last sound, or on
a slowed vowel, or on a stressed one; the ramp only from silence; a release fade only into silence; an unvoiced stop
or hiss 5 dB louder after a vowel (the chip's TT and TH are, but the measure prefers the solo level); the pause
before a voiced stop shortened regardless of its length; a vowel from its own frame after GE GO (their F2 glide
carries the level).

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

**A tone switch (library 0.5.5, 1.24; 226 B6, all four takes).** At a part change a new tone takes the oscillator whose
tone just stopped, not an idle one, and an oscillator already sounding whose frequency leaps by more than 25 % plays the
midpoint of the old and new frequencies for 1.5 ms before the new one: on B6 every switch between its 1.8 and 3.5 kHz
tones leaves a ~1.7 ms burst at the arithmetic midpoint, the hump between 2.2 and 3.1 kHz on its whole-sound spectrum, low
at both ends and peaking in the middle (a constant-rate glide drew a slope instead). Not from silence, not into or out of
a voiced part, not on a small step. B6's tones above 4 kHz were the 3.5 kHz tone's image at the 8026 Hz rate (the two
lines sum to the rate on every take; the library's output images too): removed, with C2's and A4's. Tones above 4013 Hz
that pair with nothing (B9) are real.

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

**Per effect (1.15, 2026-11-08, both units).** Against the chip, the floor under each effect's tones (its middle,
bins clear of both sides' lines, the chip 8 dB over the take's background) is right on average with the model above:
-0.4 dB over 41 effects. Per sound it runs from -16 to +5 dB. No single change of the law fixes both ends: a lower
corner with a larger scale brings 220 B0 from +3.6 to +0.1 dB and 215 A5 from -7.0 to -0.7 but turns 227 B7 from +3.8
to -4.3, and over all 55 effects moves E 4.030 -> 4.022 and P 0.885 -> 0.890; a term proportional to the signal itself
does nothing for the high tones. So the library gives each effect its own scale of the bed (fxs `bs`, percent; 0 =
100), set where E and P both improve: 215 A5 151, 218 A8 160, 220 B0 81, 222 B2 77. Tried and left at 100 (worse):
R2, A6, A7, B4, B5, B7. 218 A8's floor sits 16 dB under the chip's and barely moves with the bed: something else
makes it on the chip (open).

**Under a voiced part (library 0.5.4, 1.23).** Under a voiced effect part that pulses only some of its tones (not
`A`, not smoothed `m`) the signal-held bed doubles (0.024 -> 0.048). The robots' chip spectra sit 10-15 dB over ours
in the valleys between harmonics (205 R5 at h1-h2), the gap not growing with the harmonic, so a floor and not jitter.
Over all 55 effects: X sum 51.25 -> 51.07, regions 2064 -> 2043; 203 R3, 204 R4, 205 R5, 207 R7, 208 R8, 209 R9 and
237 C7 better, 202 R2 +0.006. Under every voiced part it was worse (the `A` and `m` effects: 211 A1, 212 A2, 213 A3,
254 M2, 230 C0, 232 C2, 234 C4, 233 C3), and at four times the bed everything was worse.

**Level wobble.** Plain effect tones also carry a slow wobble in level:
white noise through a one-pole low-pass (coefficient 0.05, spread to about
64 Hz), scaled so its rms is 0.6 % on a steady tone. On a wandering part it is
10 %.

### 7.6 Gates and the noise sweep

**Gates (measured).** A gated part alternates on and off at its gate period.
The off half dips about 8 dB (× 0.4), not to silence. The gate period is in
milliseconds and does not scale with Speed. It counts from the part's start:
the off half is `tick mod G ≥ G/2`.

**Noise sweep (S, measured on 238 C8, both units; only C8 uses it).** Over each gate period the noise band's
centre moves in straight lines (linear in Hz) from 0.5 to 1.85 times its table value and back, half the period
each way, 5 dB quieter at the top. C8's period is 20 ms (its band runs about 800 to 2950 Hz, a full cycle every
19.5 ms on both units). With `p = (tick mod G) / G`:

```
s      = p / 0.5          for p < 0.5,  else 1 − (p − 0.5) / 0.5
centre = fc · (0.5 + 1.35·s)
level  = 10^(−5·s / 20)
```

Until 0.5.3 the centre moved geometrically (fc · e^(0.75·(2s − 1))), which curves on a linear frequency axis where
the chip's track is straight, and C8's period was 21 ms; the two changes take it from 1.75 to 1.63. An earlier
reading of an 8/12 ms rise and fall scores worse than half and half.

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
  burst: a block of low noise, 100-500 Hz, for its first ~50 ms under a hiss
  that has little under 500 Hz (measured, 1.10: both units, every take; the
  library's hiss is high-passed at 608 Hz and its burst runs 50 ms). 254 M2 is a pulsed three-tone sweep up and back.

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
- **231 C1 (1.9; Man Goes Down's death sound is C1 at Speed 62, Pitch 212):** its pulse rate starts low and rises
  smoothly to a peak about 70 ms in, Pitch + ~355 Hz at every Pitch recorded (60, 88, 150, 212: 415, 440, 495,
  575 Hz), then falls. The library's first two parts had ramped to Pitch + 424 and restarted at + 198, a sawtooth at
  every Pitch and, at Pitch 212 and Speed 62, a climb to 625 Hz and a jump back. Its offsets were re-anchored on the
  measured track, then every part refitted with the death sound weighted twice the rest of C1's recordings, and C1
  given a 20 ms attack (the chip rises over ~20 ms; ours had started at full level). Man Goes Down's death sound:
  P 1.477 -> 0.578, E 4.99 -> 4.49; C1's 20 recordings: spectrum 1.68 -> 1.36, E 3.70 -> 3.60, P 0.943 -> 0.922.
  Open: under Speed 62 the first part is 5-6 dB quiet and far brighter than the chip's (over 2 kHz -3 to -7 dB
  against -16 to -24) and steps up at the second part; part levels, ramps, extra tones, glides and tones running on
  through the pulses were each tried and were worse.
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

**Speed** scales speech by (147 − Speed) / 33 below Speed 114 and (162 − Speed) / 48 from 114 up (6.1), and effects
by (147 − Speed) / 33.

**Bend (measured)** adds the same number of hertz to every tone and formant:

```
shift = 44 · (Bend − 5) Hz,  plus 22 Hz when Bend < 5
```

So Bend 0 gives −198 Hz, Bend 4 −22 Hz and Bend 15 +440 Hz. 222 B2's 1412 Hz tone measures
1213, 1257, 1304, 1392, 1547 and 1854 Hz at Bend 0, 1, 2, 4, 8 and 15. 241
D1's 697 and 1209 Hz move together, and so do 136 AW's three formants. A zero
frequency stays zero. Bend does not move noise centres or pulse rates.

**Bend 0-2's second line (measured, 1.11, corrected; not modelled).** At Bend 0, 1 and 2, 215 A5 carries a second
line 11-12 dB under its tone that runs *parallel* to it, a fixed distance above: 2109-2124 Hz at Bend 0, 2024-2036 at
Bend 1, 1937-1948 at Bend 2, gone at Bend 4 (VoxProbe 9, both units). The distance falls 88 Hz a step, twice Bend's
44, so the line sits at the unbent tone + 1720 Hz + the size of the bend shift. Earlier versions described a line
moving against the tone (slope about −0.9 or −1, "K − f"); both units say parallel. 210 A0, 218 A8 and 235 C5 show a
line at Bend 0-2 too, not yet re-read this way. The library has no such line. See section 13.

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

**Pauses (measured, 1.11).** VoxProbe 1 section 5 plays AW Pn AW three times for each pause, recorded on both units,
on the 2600 and through Stella; every take agrees within about 10 ms. The gaps between the two AWs, and the pauses
that give them:

```
pause                P0    P1    P2    P3    P4    P5    P6
chip gap (ms)        44   125   230   680    45    62    91
pause (ms)           20    90   200   648    15    30    59
```

The library's AW Pn AW gaps come out a constant 17-22 ms under the chip's for every pause: that constant is AW's own
fade into and out of silence (section 13), not the pauses. Man Goes Down's phrases 1 and 2, whose pauses sit between
stops and hisses, match these values within a few ms (ZZ P4 DO 20 ms on both units, 19 in the library). P0 leaves
about 20 ms; P3, P5 and P6 run 52, 30 and 31 ms under the manual's figures. Pauses scale with Speed on the
effects' law, and Pitch and Bend don't change them (VoxProbe 13 section 9, both units; VoxProbe 8's reading that they
don't scale was wrong). See 6.9.

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

### 9.1 When a Pitch command takes effect (1.18, open)

A Pitch command (22 n) placed between sounds does not take effect at the next sound. Measured by the voicing period
in steady stretches of Man Goes Down, both units:

```
phrase  command                   what plays
P1      NE 22 100 8 GO OWWW       GO OWWW at ~100 at once
P1      ZZ 4 22 66 DO 8 AYWW NE   DO AYWW still at 100; the final NE at 66
P2      31 ... 22 64 MM EYIY      EYIY UX at 64 (after a reset)
P2      UX 4 22 80 FF RR 8 UW     FF RR UW still at 64; IY reads ~75; IH at 80
P2      IY 22 76 WW IH            IH still at 80; the slowed IYUW at 76
P2      TH 4 22 40 8 IYUW         IYUW at 76, never 40
```

The delay is one to five sounds and uneven, and "applies at the next pause" fits P2 but not P1's final NE. The
library applies every Pitch at the next sound, which is right from silence and wrong inside these phrases; this is
most of why phrase 2 renders 25 ms long. VoxProbe 15 carries the test (a vowel string with 22 n between sounds, with
and without a pause and Slow, and between two effects).

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
VoxProbe 10    the envelope oscillator by hand (register shapes, levels, noise), slowed effects, the voice
               against Pitch, Bend on voice and effects, the death sound at several Speeds and Pitches
VoxProbe 11    Man Goes Down's phrases connected and from a Reset, effects at three Speeds
VoxProbe 12    Slow and Fast, joins and codes between sounds, Man Goes Down taken apart (without its Slow,
               Pitch, Bend or Speed codes), the effects' Speed law, the default pitch, M2
VoxProbe 13    build 5, posted 2026-11-07, recorded 2026-11-08 on both units (4 takes): Bend 0-15, speech at Speed 100-127, joins
               (vowels, nasals, liquids, glides), stops into vowels with and without Slow, pauses under
               Speed, Pitch and Bend, unvoiced stops and hisses, Man Goes Down phrase 1
VoxProbe 14    build 1 (sha1 f94b1488), recorded 2026-11-09 on both units (4 takes): the Speed step, consonants
               above 114, Slow on unvoiced stops, hisses after vowels and alone, velars, joins, nasals and BO
               before stops, copies, the slowed glide, M2, every effect at Speed 0
Man Goes Down  the phrases of a released game, for whole-phrase checks
```

On 2026-11-05 every probe was recorded again on both units, each through a real 2600 and through Stella driving
the AtariVox, so most items have four takes. *This list is updated with each probe recorded.*

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
- **Use only the two reference units.** A recording of Alex Herbert's vox_test ROM on an unidentified AtariVox
  (probes/VoxTest) was briefly used on 2026-11-07 and dropped: its pauses run 25-55 ms under both reference units',
  which agree with each other within 10 ms. Before using any recording, name its unit.
- **Look in the earlier recordings before writing a new probe.** Several of 2026-11-07's questions were already
  answered by takes on disk: VoxProbe 1's AW Pn AW (every pause code, both units, recorded but never cut into
  items until then) gave the pause table; VoxProbe 4's sixteen pairs and VoxProbe 8, 9 and 12's stop pairs gave
  the join rules. An old probe recorded again holds data its first analysis never looked at.
- **Phrase timing is judged by eye and by length, not by the scores.** The combined error and the perceptual score
  align a whole phrase with one shift; moving 5 ms of silence inside a gap cost the combined error 1.7, more than
  any real change. Measure each word's and gap's length (from 30 dB under the peak and 10 dB over the take's own
  background) against both units, and read spectrograms with both units above the library.

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

- **Formant tracks (1.14).** LPC on the chip's 8 kHz signal swaps a high F2 (IY ~2180 Hz) for the next pole down
  from frame to frame; a 15/85 % crossing time read off such a track is wrong. Read the track itself, frame by
  frame, before trusting any timing taken from it.

### 12.5 Lessons from the 0.5.0 rebuild (1.18)

- **Score at the best of a few shifts, never at one onset.** An onset rule places a render by one feature; on a stop
  whose bar fades in it put the render 8 ms out and read 1.61 where 1.29 was right. The measure now scores at shifts
  of -12..+12 ms round the onset and keeps the best, as the gates always did; a shift must pay by 0.02 or the
  smallest is kept, since a pitch period later scores a voiced sound within 0.01-0.08.
- **Find the render's onset with the take's background added.** A clean render fires a 1 % rule on a stop's burst
  where the take's burst sits in room noise; the quiet fricatives (HE HO WH FF TH PE PO) need the first point the
  level holds for 10 ms.
- **Never re-align the takes onto each other by envelope correlation.** It put nasals and voiced stops a pitch pulse
  out (NO 0.72 -> 2.06, DH 0.67 -> 1.11) and the checker that used the same correlation reported them aligned; every
  rule measured under it had to be re-measured.
- **The picture places ours by the measure's own shift**, so what is looked at is what was scored.
- **Two judges can disagree on one sound.** The spectrogram measure wants AW's 45 ms attack on the solo takes; the
  phrase cost (a lined-up band-by-time difference) prefers 12 ms on every pair that starts with AW. The phrase cost
  aligns by correlation, which a slow onset pulls; it is now also run at the lag where its own cost is least.

### 12.6 The score corrected (library 0.5.4, 1.22)

Every release from 0.1.0 was scored the same way to find where the library had gone backwards. That found the score
itself at fault, twice:

- **X could place a voiced sound a voice period off.** Its shift search ran in 4 ms steps; two placements a period
  apart both look good to a periodic sound, and a 1 ms change of AW's fade-out moved its score from 0.90 to 1.14 with
  the sound unchanged (at its onset both scored 1.47). Scoring at the onset alone is no better: a quiet start moves the
  onset (NGO 1.15 -> 1.54). X now places the render by its whole level over time, 300 Hz up, smoothed over 30 ms so a
  period-long slip can't win, then finds the pulse phase within half a period in 0.25 ms steps. The fades 25, 29 and
  30 ms score 0.916, 0.900 and 0.898.
- **Phrases that open on a hiss were measured with the hiss outside their window.** The recordings' labels start at
  the vowel (the cutter misses a quiet hiss), so the render's hiss had nothing to match: VoxProbe 8's SE P1 AW cost
  15641 on unit A, 129 with the window started 250 ms earlier. Every version's phrase figure was inflated, the old
  ones most: 0.4.3's 1437 is 481 measured properly.

Lessons: score every release against every earlier one, not only the last; a choice made on a faulty measure is redone
when the measure is fixed, not kept for its history. A real take scored as ours is the check on what 1.0 means:
the original AtariVox's takes score about 1.0, the AtariVox+'s 1.2-2.4.

### 12.7 VoxProbe 16 (1.24)

VoxProbe 16 (build 1, sha1 83de1bce, recorded 2026-11-18 on both units, 2600 and Stella, six takes) records every kind of
join alone, ~3.3 minutes: the solo sounds the joins use; MM NE NGE into IY AW UW EYIY, plain and with Slow on either side;
IY AW UW into MM NE; every pair of IY EH AW UW, EYIY in and out, LE and RR beside vowels, SE and TT before and after vowels;
AW IY and MM AW at Speed 90 and 127 and with Slow; Man Goes Down's three phrases as sent and each without its Slows,
Pitches or Bends; and each of its joins alone with the controls in force. Its items are placed by the DTMF markers and the
silences (section 2's lone hisses by order). Two judges read it: each formant move's start and end against both units'
F1/F2 tracks (an LPC tracker run the same way on every take and on ours), and the levels in sixteen bands from 300 Hz to
5 kHz with every take lined up on its own onset, the chip's background added to ours. The two units agree within 5-10 ms on
every join; where their lengths differ (unit B's solo nasals read 35-45 ms long) it is the take's background, not the chip.

## 13. Known gaps and open questions

**From VoxProbe 16 (1.24, 1.25).** Slow on a vowel joined after a sound at Speed 108 runs ~35 ms shorter on the chip than
x1.29 x the Speed factor (Man Goes Down's MM Slow AY 285 ms against the library's 320), where isolated slowed pairs at
Speed 114 fit within 10 ms; at 118 it is fixed provisionally by the vowels' Speed step (section 6.5). A voiced stop after a pause
(phrase 2's TT P1 BE, both units): a 20 ms bar 25 ms after the hiss, 95 ms of quiet, then the vowel; the library plays
72 ms of silence then the bar and release as from silence. PO after a nasal (VoxProbe 13's NE PO OW): 300 ms against
285/295 since NE and NO overlap BO and PO (1.25; NE BO OW 280 -> 255, NO BO OW 290 -> 255); the chip's closure is still
~25 ms shorter and its release longer than ours. (AY IY and AW IH AW fixed in 1.25 by join terms: AY +12, IH +15 back in the table, WW and RR taking none from the sound after them). IY SE: the vowel ends ~15 ms early on ours (a smaller cut
helps it and hurts phrase 4). IYUW after P4 at Pitch 40 (phrase 2's end): the chip's voicing ~20 ms before ours, not in the
onset itself.

Where the library and the chip still differ, or the chip is not understood:

- **When a Pitch command takes effect inside a phrase** (section 9.1): one to five sounds late on Man Goes Down, not
  at the next sound; the library is wrong there and phrase 2 renders 25 ms long. For VoxProbe 15.
- **Unvoiced sounds decay slowly on the chip.** After every unvoiced stop and fricative the chip's level falls from
  -10 to -30 dB over 94-122 ms (TT TS TU KE PE PO EK SE SH SO TH HO WH FF, CH JH), ours over 2-14; GE GO ED over
  56-69 ms against ours 9-22. Not modelled; it costs little on the measure (the tail is quiet) but it is in every
  picture.
- **TH after a vowel** is 4-5 dB quieter than the chip's (Man Goes Down's IH TH), and TT's burst after AW looks lighter
  (VoxProbe 4), while the measure prefers their level alone.
- **AW's attack** (section 12.5): 45 ms by the spectrogram measure, 12 by the phrase cost.
- **Bend 0-2's second line** (section 8): measured parallel to the tone on 215 A5, not modelled; the other
  effects' lines not re-read.
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
- **Fast and Slow** by class of sound, and between sounds (6.1). Whether a Slow after a sound lengthens that sound
  (the chip reads ahead): VoxProbe 12's AW Slow IY AW says yes (+17 ms), Man Goes Down's phrase 1 AY before a Slow
  says no; phrase 4's IYUW is handled by an assumed rule (6.8).
- **The nasals' pulse at low Pitch.** 143 NGE's first formant dies away more
  slowly than a single formant would, with a ripple every 300 Hz in its
  spectrum. 144 NGO's falls sooner and then beats. Two close low components are
  likely.
- **Joins not yet right** (1.11; the vowel joins and stop-into-vowel joins are, section 6.5): the nasals' and liquids'
  joins (VoxProbe 4: IY MM 36 ms short, AW MM 15 short, AW LE and LE AW 16-18 long, AW RR 15 short); 191 TT into a
  vowel 6-16 ms short; Fast on a voiced stop is not modelled (Fast BE AW 19 ms long; the chip shortens BE ~20 ms and
  TT ~9-29 ms under Fast); every vowel's fade into and out of a pause leaves the gap ~19 ms under the chip's (the
  sound's edges, not the pause); Man Goes Down's phrase 4 still shows a low blob where its second SE starts and a
  column where the hiss meets UX, which neither unit has.
- **The assumed rules** of section 6.8 were tested by VoxProbe 13 build 5; section 6.9 gives what it confirmed and corrected.
- **A length can't see a quiet hiss after a vowel** (2026-11-08). The takes' hum floor sits ~30-35 dB under a vowel's
  peak; SE and TH after a vowel fall under it, so "active length" stops at the vowel (AW SE SE read 128 ms on unit A
  where the SE SE runs to ~275). Lengths are now taken band by band: a 5 ms frame is sound when any of 12 bands is
  12 dB over that band's floor in the take, so a hiss counts; the library's render is judged on the same take's floors
  at the take's peak level (lengths13c). The 2600 takes decide; a Stella take counts only where it agrees with them
  (the Stella takes carry extra sound events). Every reading is checked against the comparison pictures. Timing inside a phrase is measured by warping its spectrogram onto
  each unit's.
- **Check each placed item's shape against the other unit** (2026-11-08). A cutter can match the run count and still
  be shifted by an item: an extra run and a later merge cancel. Unit A's VoxProbe 13 section 9 was shifted by one item
  for five items, and the shifted windows read as "the units disagree by 55 ms" and as a long tail on unit A's AW.
  Every item's window length is now compared between the two 2600 takes (none differ by more than 50 ms), and each
  item's runs of sound (count and boundaries) against both units and ours: inside timing is scored on those boundaries,
  not only on the first onset to the last end.
- **A matching count is not a placement** (2026-11-09). VoxProbe 14's section 4 cut to its 11 items on three takes and
  was one item late on all three: AW Slow PO P2 AW cut as two runs, and the lone Slow TT and Slow KO (bursts under the
  cutter) made up the count. Sections whose items include a lone stop or hiss are placed from a 2 kHz high-pass
  envelope at offsets checked on all four takes (place14.py), and the cutter's count is never taken as proof.
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
- **238 C8** (1.60 on the corrected measure) and **239 C9** (1.42) are the furthest sounds. C8's sweep, period and band match
  the chip's track; its level ramp, closing tones, phase and band width were each checked and are not the gap,
  and its noise shows no pulse structure above the hum. What remains is not yet explained.
- **AW after a pause** starts ~15 ms late and its first pulses too soft (NE P4 AW, SE P4 AW, AW P1 AW, behind 0.4.3 by
  18-54 %); VoxProbe 15 section 5 records it. **AW before LE** opens on a first pulse ~4 dB weak where the chip's is full.
  **207 R7 and 233 C3** were closer in 0.1.0 (1.34, 0.75 against 1.43, 0.82).
- **227 B7** waits on the bed rule (section 7.5). 237 C7 was fixed in 0.5.2 (its pulse-rate track).

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
 131 EH    vowel   140    70    +1.5    624 1664 2368   22 17 11           -      -    -  
 132 AY    vowel   155    70    +6.0    704 1664 2192   16 10  6           -      -    -  
 133 AX    vowel   135    75    -0.5    528 1504 2384   29 26 15           -      -    -  
 134 UX    vowel   140    70    -1.0    608 1328 2352   30 23 15           -      -    -  
 135 OH    vowel   135    70    -0.5    704 1152 2368   28 23 15           -      -    -  
 136 AW    vowel   130    70    -2.5    688  960 2448   32 36 12           -      -    -  
 137 OW    vowel   125    70    -1.5    544  864 2368   20 27  7           -      -    -  
 138 UH    vowel   125    70    -0.5    448 1056 2256   19 24  7           -      -    -  
 139 UW    vowel   125    75    -4.0    368 1040 2112   29 30  6           -      -    -  
 140 MM    nasal   100    70    -4.0    272  768 1424   18  1  9        2400   1200    3  
 141 NE    nasal   100    75    -8.0    288 1552 2496   28  8 15         832    224    3  
 142 NO    nasal   105    70    -7.5    272 1136 2512   26  7 15         800    400    4  
 143 NGE   nasal   100    50   -10.0    336  416 2272   29  2 14        1376    496    4  
 144 NGO   nasal    95    70    -6.5    368 1472 1904   27  8 14        2400   1200    3  
 145 LE    nasal   115    70    -6.0    448  960 2640   24 29  6        2400   1200    3  
 146 LO    nasal   110    70    -6.5    448  864 2448   26 30  6        2400   1200    3  
 147 WW    nasal   110    70    -3.0    448  784 2112   16 25  2           -      -    -  
 148 RR    nasal   125    75    +0.5    432 1312 1664   20 20  5           -      -    -  
 149 IYRR  glide   250   265    +0.0                                       -      -    -  IY -> RR
 150 EYRR  glide   250   195    +0.0                                       -      -    -  EY -> RR
 151 AXRR  glide   255   230    +0.0                                       -      -    -  AX -> RR
 152 AWRR  glide   255   235    +0.0                                       -      -    -  AW -> RR
 153 OWRR  glide   230   190    +0.0                                       -      -    -  OW -> RR
 154 EYIY  glide   225   185    +0.0                                       -      -    -  EY -> IY
 155 OHIY  glide   255   235    +0.0                                       -      -    -  OH -> IY
 156 OWIY  glide   270   300    +0.0                                       -      -    -  OW -> IY
 157 OHIH  glide   240   260    +0.0                                       -      -    -  OH -> IH
 158 IYEH  glide   225   190    +0.0                                       -      -    -  IY -> EH
 159 EHLL  glide   185   180    +0.0                                       -      -    -  EH -> LE
 160 IYUW  glide   230   230    +0.0                                       -      -    -  IY -> UW
 161 AXUW  glide   235   285    +0.0                                       -      -    -  AX -> UW
 162 IHWW  glide   265   255    +0.0                                       -      -    -  IH -> WW
 163 AYWW  glide   255   255    +0.0                                       -      -    -  AY -> WW
 164 OWWW  glide   215   175    +0.0                                       -      -    -  OW -> WW
 165 JH    affr     55    85   -13.0   1968 3664 4080   31  5 13        3088   1136   42  
 166 VV    vfric   105    70   -10.0    528 1520 2288   11 30 13        1536    336   30  
 167 ZZ    vfric   105    65    -6.5    256 1472 2400   30 19 22        3936    800   30  
 168 ZH    vfric    90    85    -9.5    432 1680 3264   28 28  3        2624    480   36  
 169 DH    vfric    95    70   -11.5    288 1584 2336   28 30 10        2304    960   16  
 170 BE    vstop    40    80    -3.0    320 1744 2192   30 29 10        2368    960    6  
 171 BO    vstop    50    70    -4.0    320 1392 2384   28 24 11        2288   2976    3  -> 832 1392 2384
 172 EB    vstop    35    60   -12.5    272 1792 2288   27 23 15        1504    656   18  -> 512 1840 2064
 173 OB    vstop    25    60    -1.5    592 1392 2352   18 17  7        2416    752    5  -> 272 1392 2352
 174 DE    vstop    40    60    -4.5    272 1952 2624   30 25 15        1696    880   12  
 175 DO    vstop    30    55    -7.0    400 1488 2448   30 26 15        2832   1056    1  
 176 ED    vstop    40    45   -18.0    288 1936 2624   26 19  8        1424   1632   19  -> 256 1936 2624
 177 OD    vstop    35    45   -15.5    288 1456 2416   25 16  8        1056   1488   13  -> 240 1616 2624
 178 GE    vstop    45    65   -13.0    272 1856 2528   30  1  6        1936   1472   10  
 179 GO    vstop    50    65    -9.5    240 1952 3120   35  6  1        1376   1648    9  
 180 EG    vstop    30    55    -4.0    272 2080 4080   32 29  0         896   1424   16  
 181 OG    vstop    25    55    -5.5    272 1200 1312   28  1 30        1504    496    6  
 182 CH    affr     60    55   +22.0    640 1824 2480    0  0  0        2880    592    2  
 183 HE    fric     75    60   -17.5      0    0    0    0  0  0        1696    544   18  
 184 HO    fric     70    90   -18.0      0    0    0    0  0  0        1392    560   18  
 185 WH    fric     70    80   -14.5      0    0    0    0  0  0         752    240   12  
 186 FF    fric     75    65    -6.0      0    0    0    0  0  0        3008   1376    9  
 187 SE    fric     95    90   -22.5      0    0    0    0  0  0        3392    288  235  
 188 SO    fric     95    35    +1.5      0    0    0    0  0  0        3312    256   15  
 189 SH    fric     65    55    -7.0      0    0    0    0  0  0        2784    368   46  
 190 TH    fric     45    45    +7.5      0    0    0    0  0  0        2336    960    2  
 191 TT    stop     35    60   -12.5      0    0    0    0  0  0        3280    464   55  
 192 TU    stop     85    75    +3.5      0    0    0    0  0  0        3248    400   15  
 193 TS    stop    115   135    +2.5      0    0    0    0  0  0        3808    672   14  
 194 KE    stop     25    55    -7.5      0    0    0    0  0  0        2016    384   53  
 195 KO    stop     25    50    +1.0      0    0    0    0  0  0        1520    512   20  
 196 EK    stop     15    55   +17.0      0    0    0    0  0  0        1760    288    1  
 197 OK    stop     15    45    +1.0      0    0    0    0  0  0        1824    560   46  
 198 PE    stop     60    65   -30.0      0    0    0    0  0  0          64   2672   59  
 199 PO    stop     60   110   -28.5      0    0    0    0  0  0          80   1360   41  

Their own structure (1.8; fields left out are 0, "as the kind"; section 6 and Appendix C):
 128 IY    rr 20 ms
 129 IH    aa 60 ms
 130 EY    aa 30 ms
 131 EH    rr 30 ms  aa 30 ms
 132 AY    aa 60 ms
 133 AX    aa 30 ms
 134 UX    rr 35 ms  aa 30 ms
 136 AW    rr 25 ms  aa 45 ms
 137 OW    rr 32 ms
 139 UW    rr 39 ms  aa 12 ms
 140 MM    rr 20 ms  aa 10 ms
 141 NE    rr 10 ms  aa 18 ms
 143 NGE   rr 9 ms  aa 14 ms
 144 NGO   ob 15 ms  og -12.0 dB  rr 10 ms  aa 5 ms
 146 LO    rr 25 ms
 147 WW    rr 10 ms  aa 10 ms
 149 IYRR  rr 33 ms  aa 10 ms
 150 EYRR  rr 28 ms
 151 AXRR  aa 45 ms
 152 AWRR  aa 60 ms
 153 OWRR  rr 22 ms
 154 EYIY  aa 20 ms
 155 OHIY  rr 35 ms  aa 30 ms
 156 OWIY  rr 19 ms
 157 OHIH  rr 31 ms
 158 IYEH  aa 60 ms
 159 EHLL  rr 22 ms  aa 20 ms
 160 IYUW  rr 35 ms
 161 AXUW  cl 69 %  rr 34 ms  ba 121 %  aa 45 ms
 162 IHWW  aa 60 ms
 163 AYWW  rr 10 ms  aa 45 ms
 165 JH    rr 9 ms  hp 32 Hz
 166 VV    wf 16/256
 167 ZZ    ob 4 ms  og +7.0 dB  aa 15 ms
 168 ZH    ob 3 ms  wf 14/256  rr 19 ms  aa 3 ms
 169 DH    ob 3 ms  og +2.0 dB  aa 10 ms
 170 BE    ob 6 ms  og +2.0 dB  cl 40 ms  rl -1.0 dB  rr 10 ms  ba 9 ms  bl -9.0 dB  hp 528 Hz
 171 BO    ob 5 ms  og +3.0 dB  cl 24 ms  rr 30 ms  bl -10.0 dB
 172 EB    ob 7 ms  og +5.0 dB  cl 6 ms  bl -2.0 dB
 173 OB    cl 4 ms
 174 DE    ob 6 ms  og +1.0 dB  cl 29 ms  rl -4.5 dB  rr 22 ms  bl -5.0 dB  aa 3 ms
 175 DO    cl 44 ms  rr 20 ms  bl -4.0 dB  aa 10 ms
 176 ED    ob 5 ms  og +9.0 dB  cl 10 ms  ba 24 ms  hp 720 Hz
 177 OD    ob 6 ms  og +6.0 dB  cl 8 ms  ba 16 ms  bl -5.0 dB
 178 GE    ob 5 ms  og +3.0 dB  cl 5 ms  rr 10 ms  bl +2.0 dB  hp 800 Hz
 179 GO    ob 5 ms  og +5.0 dB  cl 9 ms  rr 9 ms  ba 26 ms  bl -6.0 dB  hp 720 Hz
 180 EG    ob 6 ms  og +4.0 dB  cl 10 ms  ba 42 ms  bl -15.0 dB  hp 784 Hz
 181 OG    ob 5 ms  og +1.0 dB  cl 4 ms
 182 CH    ob 4 ms  og +4.0 dB  wf 21/256  rr 9 ms  hp 784 Hz
 183 HE    wf 68/256  th 26/64  hp 304 Hz
 184 HO    ob 5 ms  og +9.0 dB  wf 16/256  aa 5 ms  hp 464 Hz
 185 WH    ob 5 ms  og +3.0 dB  wf 96/256  hp 208 Hz
 186 FF    ob 5 ms  og +6.0 dB  wf 48/256  rr 10 ms  aa 5 ms  hp 208 Hz
 187 SE    wf 19/256  tc 448 Hz  th 115/64  aa 11 ms  hp 528 Hz
 188 SO    tc 544 Hz  th 168/64  aa 25 ms  hp 336 Hz
 189 SH    ob 3 ms  og +3.5 dB  rr 10 ms  tc 672 Hz  th 80/64  aa 10 ms  hp 528 Hz
 190 TH    rr 10 ms  hp 80 Hz
 191 TT    ob 16 ms  og +2.5 dB  cl 35 ms  wf 20/256  rr 11 ms  hp 464 Hz
 192 TU    ob 5 ms  og +4.0 dB  cl 23 ms  wf 16/256  aa 5 ms
 193 TS    ob 10 ms  og +0.5 dB  cl 26 ms  wf 16/256  rr 45 ms  hp 464 Hz  nd 128/256
 194 KE    ob 5 ms  og -6.0 dB  cl 22 ms  hp 80 Hz
 195 KO    cl 30 ms  aa 6 ms  hp 304 Hz
 196 EK    ob 5 ms  og +6.0 dB  cl 26 ms  wf 85/256  rr 6 ms
 197 OK    ob 5 ms  og +3.0 dB
 198 PE    cl 15 ms  rr 13 ms  hp 32 Hz
 199 PO    cl 8 ms  wf 106/256  rr 9 ms  aa 5 ms  hp 32 Hz
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
  1     65 ms  -        +3.5 dB  800/29  432->544/7  208->288/1
  2     15 ms  F        +3.0 dB  784->800/14->0  544->528/11->0  272->288/3->0
201 R1
  1     10 ms  -        -8.0 dB  1136->1088/11  688/3  3008->3024/4  ramp +18.5 dB
  2     70 ms  -        +4.0 dB  1104/31  704->752/2  3088->3504/5
  3      5 ms  F        +5.5 dB  1168->1072/19->0  736->752/4->0  3312->3568/18->0
202 R2
  1     60 ms  V        +1.0 dB  624->368/2->3  1744->960/3->4  1312->1280/29->30  pulse Pitch+244->+107 Hz
  2     10 ms  V        +1.0 dB  384->400/2->3  912->1024/2->3  1344->1248/30  pulse Pitch+107->+107 Hz
  3     15 ms  V        -1.0 dB  384->368/2->3  1040->944/2  1296->1312/30  pulse Pitch+107->+107 Hz  ramp -36.5 dB
203 R3
  1      5 ms  Vo       -3.0 dB  656->688/4  1616->1664/30  pulse Pitch+272->+273 Hz
  2     60 ms  Vo       +3.0 dB  656->512/3  1584->1616/29  pulse Pitch+273->+283 Hz
  3     10 ms  VoD      +3.5 dB  704->480/3  1712->1552/31  pulse Pitch+272->+283 Hz
  4     15 ms  F        -8.5 dB  1968->1504/26  512->864/19  336->144/12  ramp -11.0 dB
  own: click 6/256
204 R4
  1     70 ms  VqE      +1.5 dB  240->688/3  1488->1536/2  1904->1888/31  pulse Pitch+89->+335 Hz
  2     10 ms  VF       +3.0 dB  736->656/19  1520->1488/2  1888->1872/16  pulse Pitch+335->+362 Hz
  own: click 3/256
205 R5
  1     60 ms  Vqo      +9.0 dB  464->448/2  2672->2688/1  2208->2192/12  pulse Pitch+293->+310 Hz
  2     25 ms  VqFo     +9.0 dB  480->448/2  2704->2688/0  2256->2128/13  pulse Pitch+310->+317 Hz
  own: attack 2 ms
206 R6
  1     65 ms  Vqm      -2.0 dB  784->512/5  1456->1680/3  2496->2512/31  pulse Pitch+330->+109 Hz
  2     25 ms  VqmF     -2.5 dB  528->384/4  1712->1744/4  2512->2544/29  pulse Pitch+109->+11 Hz
207 R7
  1     75 ms  Vi       +1.5 dB  176->256/1  768->512/4  2800->2784/24  pulse Pitch+113->+267 Hz
  2     20 ms  Vi       +2.0 dB  256->272/1  512->528/4  2768->2720/24  pulse Pitch+267->+267 Hz  ramp -23.5 dB
208 R8
  1     75 ms  V        +0.0 dB  704->624/6->7  2848->1504/3->2  3072->3104/26->27  pulse Pitch+254->+237 Hz
  2     15 ms  V        -1.0 dB  576->640/9->10  1632->1680/6  3136->3024/27  pulse Pitch+237->+237 Hz  ramp -27.5 dB
209 R9
  1     85 ms  V        -0.5 dB  704->624/5  1712->1392/3  3376->3424/29  pulse Pitch+258->+221 Hz
210 A0
  1    160 ms  Vo       -1.0 dB  128->272/31  1568->2464/10  3232->2752/11  pulse Pitch-28->+52 Hz  ramp +2.0 dB
  2    130 ms  Vo       -0.5 dB  288->384/31  2416->3264/4  2752->2992/5  pulse Pitch+48->+109 Hz  ramp +0.5 dB
  3     65 ms  VF       +2.5 dB  384->192/23  pulse Pitch+109->+60 Hz
  own: attack 5 ms
211 A1
  1    450 ms  VA       +3.0 dB  176/15  240->1248/14  704->1248/17
  2    510 ms  VA       +3.0 dB  176/15  1232->1696/14  1408->3008/17
  own: attack 2 ms
212 A2
  1    450 ms  VA       -2.5 dB  160->208/27  1664->1312/27  2912->1472/26
  2    295 ms  VA       -3.0 dB  192/26  1312->624/26  1360->880/27
  3    210 ms  VA       -4.0 dB  224->176/24->19  528->176/30->12  944->640/31
213 A3
  1    520 ms  VA       -5.0 dB  512->1504/27  896->2464/27  1392->3488/26
214 A4
  1     80 ms  -        +3.5 dB  208->480/31  624->1472/11
  2     50 ms  -        +3.5 dB  496->1136/31  1520->3424/10
  3     15 ms  -        +4.0 dB  1088->1728/27  3344->4016/6
  4     15 ms  -        +2.5 dB  1728->2144/31  4064->3888/15
  5     25 ms  -        +1.5 dB  2144->3920/31  4016/10
  6     50 ms  -        +1.5 dB  3904/30  4064->4080/18  ramp -9.0 dB
  7     10 ms  F        +0.5 dB  3952->3872/31->0  4080->3952/19->0
215 A5
  1    320 ms  -        +1.0 dB  640->224/31  48->432/12->31
  2    145 ms  -        +1.0 dB  432->608/31->26  208->64/30->23
  3     20 ms  -        +0.0 dB  592->624/20->31  64->48/16->24
  4     70 ms  E        -2.0 dB  608->624/31->5  48/27->2
216 A6
  1     60 ms  -        +2.5 dB  256->800/29  512->2064/19
  2     55 ms  -        +1.5 dB  800->816/31  2048->1968/8->3
  3     55 ms  -        +1.5 dB  800/31  1920->2048/3  ramp -11.0 dB
  4      5 ms  F       -10.0 dB  816->784/31->0  2096->2016/3->0
217 A7
  1      5 ms  m        +2.5 dB  416->400/17  752->704/16  1136/5
  2    185 ms  m        +2.0 dB  400->496/15  752->1120/12  1168->1552/15
  3     30 ms  m        +2.0 dB  480->528/22  1104->1200/24  1520->1632/3
  4     30 ms  m        +3.5 dB  496->544/31  1216->1264/24  1536->1632/5  1616->1744/1
  5    120 ms  m        +4.0 dB  560->720/19  1216->1520/21  1664->2240/2  1776->2192/1
  6     70 ms  m        +4.0 dB  720->832/20  1504->1680/22  2208->2480/1  2208->2512/1
  7     40 ms  m        +4.5 dB  784->912/15  1680->1792/16  2464->2624/1
  8      5 ms  m        +5.0 dB  880->896/31  1792->1760/17
  9     80 ms  m        +3.0 dB  880->976/13  1776->1856/12
  10     15 ms  mF       +2.5 dB  976/27->0  1872->1840/24->0
  own: release 40 ms
218 A8
  1     40 ms  -        -4.5 dB  256->512/21  624->944/7  ramp +11.5 dB
  2     30 ms  -        +2.0 dB  512->496/23  992/23
  3     40 ms  -        +2.0 dB  512->272/21  1024->992/19  ramp -12.5 dB
  4     30 ms  -       -64.0 dB  no tones
  5     55 ms  -        -5.5 dB  208->496/12  304->2048/14  ramp +13.5 dB
  6     35 ms  -        +2.0 dB  496->512/18  2000/16
  7     25 ms  -        +0.5 dB  496->512/27  2016/17  ramp -19.0 dB
  8      5 ms  F        -9.0 dB  512->496/21->0  1984->1968/18->0
219 A9
  1     45 ms  -        +1.5 dB  2592->2608/23  1344->1248/22
  2    195 ms  -        +1.5 dB  2608->1312/26  1312->96/25
  3     65 ms  -        -1.0 dB  96/31  noise 1216 bw 2432 /6  ramp -10.0 dB
  own: click 3/256 to 8000 Hz
220 B0
  1    115 ms  -        +4.0 dB  2608->2592/21
  2     55 ms  F        +3.5 dB  2592->2608/21
  own: attack 5 ms, release 5 ms, click 3/256 to 8000 Hz
221 B1
  1    145 ms  -       +10.0 dB  1392->3872/21  ramp -9.0 dB
  2    115 ms  -        -2.0 dB  3920->1424/30  ramp +8.5 dB
  3    125 ms  F       +13.5 dB  1408/13  ramp +1.0 dB
  own: click 12/256 to 800 Hz
222 B2
  1    100 ms  -        +7.5 dB  1408/31
  2     10 ms  -        +7.0 dB  1440->1408/31  208/2
  3     20 ms  -        +8.5 dB  1312->2512/27->31  192/3
  4     15 ms  -        +7.5 dB  2512->2544/30  208/3
  5      5 ms  -        +5.5 dB  2560->1664/31->21  208->192/2
  6     40 ms  -        +7.5 dB  1712/31  208/3
  7     45 ms  -        +8.0 dB  1712/31  208->544/4->2
  8     55 ms  -        +8.0 dB  1712/31  608/2
  9     90 ms  -        +8.0 dB  1712/18  608->592/2  ramp -8.5 dB
  10     10 ms  F        -0.5 dB  1712/15->0  608->592/2->0
223 B3
  1    105 ms  -        +3.0 dB  3088->3104/22  608->592/6
  2      5 ms  -        +3.0 dB  3248->1408/21  576->624/8
  3    140 ms  -        +3.0 dB  1392->1408/30  592/5
  4     55 ms  F        +3.0 dB  1408->1392/28  592->608/4
  own: click 3/256 to 8000 Hz
224 B4
  1    145 ms  -        +1.0 dB  1824->1808/31  2032->288/27
  2    145 ms  -        +0.5 dB  1824/30  304->2000/26
  3     80 ms  -        -2.0 dB  1888->1760/31  1952->2032/16
  own: release 40 ms
225 B5
  1      5 ms  -       -64.0 dB  no tones
  2     90 ms  -        +5.5 dB  3488/18  1008->144/19
  3     15 ms  -        +5.0 dB  3504->3520/18->10  160->0/19->9
  4     55 ms  -        -4.0 dB  3504->3472/30  64/24
226 B6
  1     20 ms  -        +0.0 dB  3536->3472/31  4080/9
  2     20 ms  -        +1.0 dB  1824->1792/31
  3     15 ms  -        -0.5 dB  3552->3424/28  4080/23
  4     15 ms  -        +1.0 dB  1824->1792/31
  5     10 ms  -        -0.5 dB  3424->3488/31  4080/20
  6     35 ms  -        +1.0 dB  1808/31
  7     50 ms  -        +1.5 dB  1808/31  ramp -30.5 dB
  8      5 ms  F       -12.0 dB  1760->1856/23->0
227 B7
  1      5 ms  -       -64.0 dB  no tones
  2     45 ms  -        +4.5 dB  2880->2928/25
  3      5 ms  F        +4.5 dB  2880->2864/23
  4     10 ms  -        -3.5 dB  no tones
  5     50 ms  -        +0.5 dB  3888/27
  6     60 ms  F        +0.5 dB  3904->3920/27
228 B8
  1     30 ms  -        +1.0 dB  2112->1952/31
  2     40 ms  -        +1.0 dB  1984->1680/28  3936->3328/31
  3     45 ms  -        +0.5 dB  1680->1520/27  3376->3008/31
  4     80 ms  -        +1.5 dB  1488->1328/27  3008->2688/28
  5    105 ms  -        +0.0 dB  1312->1280/26  2672->2592/24
  6     10 ms  F        +0.5 dB  1328->1264/31->0  2592/31->0
229 B9
  1     55 ms  -        -4.0 dB  912->896/15  1776->1840/31  2720/31
  2     10 ms  VA       -6.5 dB  912->928/21
  3     10 ms  VA       -6.5 dB  816->864/27
  4     10 ms  VA       -8.5 dB  784->768/21
  5     10 ms  VA       -6.0 dB  672->688/28
  6     10 ms  VA       -5.5 dB  608->560/31
  7     40 ms  VA       -3.5 dB  480->512/31  2512->2544/21  3600->3888/31  4080/31
  8     10 ms  VA       -5.0 dB  560->576/28
  9     10 ms  VA       -5.5 dB  608->624/31
  10     10 ms  VA       -4.5 dB  752/31
  11     95 ms  -        -3.5 dB  896->912/11  1792->1840/31  3440->3520/31  4080/15
  12     10 ms  F        -5.0 dB  1856->1824/22
230 C0
  1      5 ms  -       -64.0 dB  no tones
  2     30 ms  VAm      +4.5 dB  2368->1648/31->25  2288->2080/13  pulse Pitch-3->-3 Hz
  3     30 ms  VAm      +3.5 dB  1632->992/22  2240->2320/16  pulse Pitch-3->-2 Hz
  4     30 ms  VAm      +4.0 dB  928->592/15  2272->2336/14  pulse Pitch-2->-10 Hz
  5     25 ms  VAm      +2.5 dB  576->224/31  2288->2320/12  pulse Pitch-10->-17 Hz
  6      5 ms  Vm      -64.0 dB  no tones
  7     20 ms  Vm       +4.0 dB  176->96/23->3  2288->2336/4->1  pulse Pitch-23->-5 Hz
  8     10 ms  Vm       +4.0 dB  80->96/4
  9     20 ms  -        -5.5 dB  no tones  noise 496 bw 944 /4
  10    105 ms  -       -10.5 dB  no tones  noise 512 bw 928 /2
  11     25 ms  -       -64.0 dB  no tones
231 C1
  1     40 ms  VA      +10.5 dB  656->2000/9  pulse Pitch+40->+178 Hz
  2     30 ms  VA       +0.0 dB  480->848/26  2048->2176/19  2096->2224/19  pulse Pitch+178->+373 Hz
  3     70 ms  VA       +1.5 dB  1216->1776/20  2384->1632/25  pulse Pitch+374->+245 Hz
  4    155 ms  VAF      +0.0 dB  1584->624/8  2016->1312/6  1856->2992/25  pulse Pitch+226->-10 Hz
  own: attack 20 ms, release 10 ms
232 C2
  1     15 ms  VAm      -6.5 dB  496->528/8  928->1056/22  2352->1904/21  pulse Pitch-22->-22 Hz  ramp +6.0 dB
  2     25 ms  VAm      -2.0 dB  512->528/14  896->1088/30  2304->1968/26  pulse Pitch-22->-22 Hz
  3     60 ms  VAm      -1.5 dB  496->624/20  1152->1136/18  1856->1616/18  pulse Pitch-22->-22 Hz
  4     30 ms  VAm      -1.5 dB  592->576/20  1120->1152/16  1632->1664/7  pulse Pitch-22->-22 Hz  ramp -8.5 dB
  5     25 ms  -       -64.0 dB  no tones
  6     45 ms  VA       -2.0 dB  3328->3232/31  4080/3  pulse Pitch-22->-22 Hz
  7     10 ms  VAF      -3.0 dB  3248->3264/31->0  4080/19->0  pulse Pitch-22->-22 Hz
233 C3
  1     30 ms  -       -64.0 dB  no tones
  2     25 ms  Vm       +6.5 dB  3168->3200/14->3  pulse Pitch-17->-31 Hz
  3     25 ms  Vm       +6.5 dB  3200->3136/3->18  pulse Pitch-20->-31 Hz
  4     10 ms  Vm       +6.5 dB  3136->3280/18->21  pulse Pitch-9->-41 Hz
  5    100 ms  Vm       +6.5 dB  3280->3872/21->20  pulse Pitch-24->-44 Hz
  6     30 ms  Vm       +6.5 dB  3872->3648/20->11  pulse Pitch-50->-22 Hz
  7     25 ms  Vm       +6.5 dB  3648->3536/11->5  pulse Pitch-40->-19 Hz
  8     15 ms  m        +6.5 dB  4000/1
  9     40 ms  -       -61.0 dB  no tones
234 C4
  1     75 ms  VAm      +7.0 dB  912->992/2->19  pulse Pitch-2->-2 Hz
  2     40 ms  VAm      +6.0 dB  992->976/19->4  1904->1936/4->15  pulse Pitch-2->-2 Hz
  3     20 ms  VAm      +5.5 dB  1936->1888/13  pulse Pitch-2->-2 Hz
  4     30 ms  VAm      +5.0 dB  1888/12->3  2928->3024/11->18  pulse Pitch-2->-2 Hz
  5     50 ms  VAm      +5.5 dB  3040->3024/12  pulse Pitch-2->-2 Hz
  6      5 ms  VAmF     +6.0 dB  3040->2912/4->0  pulse Pitch-2->-2 Hz
  own: attack 5 ms
235 C5
  1     45 ms  -        +0.5 dB  2688->2720/17  752->784/3
  2     25 ms  -        +4.0 dB  2592->1008/13->0  768->720/11  384->368/6
  3    140 ms  -        +1.0 dB  704->288/24  896->672/12->2  352->288/26
  4    125 ms  Vm       +7.0 dB  304->176/12  ramp -7.0 dB
236 C6
  1     35 ms  VAm      +4.0 dB  1968->2064/9  pulse Pitch-21->-21 Hz  ramp +7.5 dB
  2     30 ms  VAm      +5.0 dB  2112->960/25  2880->2784/6  pulse Pitch-21->+42 Hz
  3     40 ms  VAm      +7.0 dB  928->960/8  2928->2016/14  pulse Pitch+42->+42 Hz
  4     35 ms  VAm      +6.0 dB  960->1008/6  2096->2048/20  pulse Pitch+42->+42 Hz
  5     30 ms  VAm      +6.5 dB  944->2000/23  2112->2848/8  pulse Pitch+42->-21 Hz
  6     55 ms  VAm      +5.5 dB  2064->2048/15  pulse Pitch-23->-23 Hz
  7     10 ms  VAmF     +4.0 dB  2064->2080/22->0  2880->2864/5->0  pulse Pitch-23->-23 Hz
  own: click 6/256
237 C7
  1     35 ms  V        +2.0 dB  528->512/17  pulse Pitch+0->-5 Hz
  2     50 ms  V        +2.5 dB  416->2704/23  pulse Pitch+2->-8 Hz
  3      5 ms  V        +1.5 dB  2752->1872/20  pulse Pitch+19->+15 Hz
  4     40 ms  V        +3.5 dB  464->2208/28  pulse Pitch+24->+71 Hz
  5     10 ms  V        +4.0 dB  2224->1472/23  pulse Pitch+76->+42 Hz
  6     45 ms  V        +7.5 dB  576->2144/17  pulse Pitch+59->+60 Hz
  7      5 ms  V        +8.0 dB  2048->1520/14  pulse Pitch+57->+86 Hz
  8     30 ms  V        +1.5 dB  624->1568/31  pulse Pitch+104->+108 Hz
  9     15 ms  V        +2.0 dB  1584->544/31  pulse Pitch+102->+102 Hz
  10     25 ms  V        +5.0 dB  416->1168/19  pulse Pitch+93->+99 Hz
  11     10 ms  V        +6.0 dB  1248->448/25  pulse Pitch+93->+90 Hz
  12     85 ms  V        -2.0 dB  464->480/31  pulse Pitch+104->+41 Hz
  13     10 ms  VF       -0.5 dB  448->480/20->0  pulse Pitch+48->+45 Hz
  own: attack 20 ms
238 C8
  1     90 ms  S        +8.0 dB  no tones  noise 1600 bw 48 /40  gate 20 ms  ramp +5.0 dB
  2      5 ms  m        -3.5 dB  3216/17->20  2480->2384/13->16
  own: attack 5 ms
239 C9
  1     10 ms  m        +7.0 dB  560->3296/14
  2     10 ms  m        +8.0 dB  3088->688/31
  3     10 ms  m        +6.0 dB  752->2800/20
  4     10 ms  m        +7.0 dB  2880->592/31
  5     10 ms  m        +9.0 dB  624->2160/18
  6     10 ms  m       +10.0 dB  2320->592/31
  7      5 ms  m        +5.5 dB  576->2064/20
  8      5 ms  m        +5.5 dB  2032->1984/6
  9     10 ms  mF       +5.5 dB  1968->2128/12->0
  own: release 30 ms
240 D0
  1     90 ms  -        +1.0 dB  DTMF pair/31+31
  2     65 ms  F        +0.5 dB  DTMF pair/31+31
  own: attack 2 ms
241 D1
  1    105 ms  -        +0.0 dB  DTMF pair/31+31
  2     50 ms  F        +0.0 dB  DTMF pair/31+31
  own: release 10 ms
242 D2
  1    100 ms  -        +0.5 dB  DTMF pair/31+31
  2     55 ms  F        +0.5 dB  DTMF pair/31+31  ramp -3.5 dB
  own: release 10 ms
243 D3
  1    100 ms  -        +0.5 dB  DTMF pair/31+31
  2     60 ms  F        +0.0 dB  DTMF pair/31+31
244 D4
  1     85 ms  -        +0.5 dB  DTMF pair/31+31
  2     70 ms  F        +1.0 dB  DTMF pair/31+31
  own: release 10 ms
245 D5
  1     95 ms  -        +1.0 dB  DTMF pair/31+31
  2     65 ms  F        +0.5 dB  DTMF pair/31+31
246 D6
  1    100 ms  -        +0.5 dB  DTMF pair/31+31
  2     55 ms  F        +0.5 dB  DTMF pair/31+31
247 D7
  1     90 ms  -        +1.0 dB  DTMF pair/31+31
  2     65 ms  F        +0.5 dB  DTMF pair/31+31
248 D8
  1    110 ms  -        +1.0 dB  DTMF pair/31+31
  2     45 ms  F        +0.5 dB  DTMF pair/31+31
  own: release 10 ms
249 D9
  1     95 ms  -        +0.5 dB  DTMF pair/31+31
  2     60 ms  F        +0.5 dB  0/2
250 D10
  1     50 ms  -        +1.0 dB  DTMF pair/31+31
  2     50 ms  -        +0.5 dB  DTMF pair/31+31
  3     55 ms  F        -0.5 dB  DTMF pair/31+31
  own: release 10 ms
251 D11
  1    100 ms  -        +0.5 dB  0/1
  2     55 ms  F        +0.0 dB  DTMF pair/31+31  ramp -2.5 dB
  own: release 10 ms
252 M0
  1     10 ms  W       +12.5 dB  1744->1776/18  noise 1792 bw 224 /6
  2    295 ms  W       +10.0 dB  1808->1856/16  noise 1648 bw 208 /6
  3   1055 ms  DW       +8.5 dB  1904->1696/16  noise 1584 bw 224 /6
253 M1
  1      0 ms  -       -64.0 dB  no tones
  2     40 ms  -       +19.5 dB  no tones  noise 1920 bw 1344 /30
  3    260 ms  -       +19.5 dB  no tones  noise 1680 bw 1168 /30  ramp -23.5 dB
  own: click 64/256 to 320 Hz
254 M2
  1      5 ms  -       -64.0 dB  no tones
  2    245 ms  VA       -3.5 dB  496->992/25  1696->2208/26  2880->3408/30
  3    270 ms  VA       -4.0 dB  1008->464/28  2224->1664/30  3408->2832/26
  own: attack 2 ms, release 15 ms
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

**An allophone row**, 27 bytes:

```
kind  ms  f1 f2 f3  a1 a2 a3  nf nw na  cp  lv  cb  ob og cl rl wf rr tc th ba bl aa hp nd
```

`cb` is 1 where a stop's Repeat copies keep the voice bar. A glide's `f1` and
`f2` hold the indices of the pair. A side table `allog` lists the voiced stops
with a second frame: index, then three formants. The fields after `cb` (1.8) are
0 for "as the class": `ob` onset ms and `og` its gain (half-decibels, signed);
`cl` a stop's closure ms; `rl` a stop's release level (half-decibels, signed);
`wf` a white floor (/256); `rr` and `aa` the sound's own release and attack ms;
`tc` and `th` the starting burst's corner (32 Hz steps) and level (/64 since 1.10,
/256 before); `ba` and `bl` a voiced stop's bar fade-in (ms) and level
(half-decibels, signed); `hp` the noise high pass corner (16 Hz steps); `nd`
(1.10) a decaying release: the noise band's level at the end of the sound's last
segment, /256 of its start (0: steady). On a glide, `cl` and `ba` are its F1
schedule (section 6.3).

**An effect's structure (fxs)**, 6 bytes an effect, 55 rows: its own attack (ms),
release (ms), starting burst level (/64 since 1.10, /256 before) and the burst's
low-pass corner (32 Hz steps), then (1.10) its own noise high pass corner (16 Hz
steps; 0 none) and the burst's length (ms; 0 the 5 ms click); 0 for the defaults.

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

- **1.25 (2026-11-18).** A vowel after NGE takes the velar move (6.5).

- **1.24 (2026-11-18, library 0.5.5).** Joins rebuilt from VoxProbe 16 (6.5): the Man Goes Down-fitted rules removed; a nasal
  overlaps the vowel after it by 13 ms, MM 22, a vowel the nasal after it by 15; LE and LO off the vowel rule; a vowel's own
  Speed law below 114 and the speech law for the rest; a nasal above Speed 118 on the step law; a stop after a pause closes 15
  ms longer, not 30; NGE's and MM's second formants are their 2249 and 1430 Hz lines. 12.7: VoxProbe 16.

```
1.23  2026-11-17   library 0.5.4: joins against a real unit (6.2); IY after a sound and LE before one take their phrase lengths (6.1); a plain vowel's long
                   voicing period is its second (4.1); 187 SE's
                   deterministic low pulse (6.4); the bed doubled under a voiced part pulsing only some tones (7.5)
1.22  2026-11-16   library 0.5.4: the score corrected (12.6); 0.5.1's 72-tick fall removed and attacks re-fitted
                   (6.11); AW TT nasal joins, TT before a vowel, AW's fade (6.2, 6.4)
1.21  2026-11-16   library 0.5.3: 238 C8's noise sweep linear in Hz and its period 20 ms (7.6; 1.75 -> 1.63); the
                   voiced stops' bar partly noise and nasals' noise part (6.2, 6.4); AXUW's F3 hold (6.3); NGO KE
                   quiet onsets, PE PO timing (6.4)
1.20  2026-11-16   library 0.5.2: 237 C7's pulse-rate track; the noise checked for a repeated sequence (none: an
                   apparent match across takes was the recordings' hum, section 5); C8's sweep period 20 ms
1.19  2026-11-16   library 0.5.1: the plain vowels' first-period and 72-tick-fall rules per vowel (6.11); IH AY OH back
                   on 0.4.3's entries; 233 C3 back on 0.4.3's description
1.18  2026-11-15   library 0.5.0's allophones and joins (6.11), merged with the VoxProbe 14 work of 1.16: per-vowel attacks, the attack in pitch periods above 88,
                   the plain vowels' first period and fall, PE PO's low pulse rise, the vowel after a voiced stop, the
                   voiced fricative after a pause, the voiced stop after a short pause, DO/GO-only closures, the glide
                   crossing cut; when a Pitch command takes effect (9.1, open); measuring lessons (12.5); known gaps
                   updated (Pitch timing, the chip's slow unvoiced decay, TT/TH level next to a vowel, AW's attack)
1.17  2026-11-15   library 0.5.0: the effects rebuilt against units A and B (section on effects): hard start, a flatter
                   bed with a 20 ms held flat term that ignores fades and ramps, a second-order output term, the gate's
                   off part the last 31 % of its period at -34 dB, swept noise climbing for half its cycle.
1.16  2026-11-09   VoxProbe 14 recorded: section 6.10 (the Speed step at 116, consonants shrink above 114, Slow on
                   unvoiced stops, SE after a vowel, velars, joins, nasals and BO before stops, the slowed glide, the
                   slowed effects); section 12: VoxProbe 14's line, a matching item count can still be one item late
1.15  2026-11-08   section 7.5: the bed per effect (fxs bs) after a floor survey of 41 effects on both units; 219 A9's
                   last part carries low noise; 229 B9's U-shaped pulsed line needs no fourth tone (the chip's high
                   tones stop before its rise)
1.14  2026-11-08   crossing joins (section 6.2): vowel-family formants move on one line across the join, length
                   growing with F1+F2 distance, ending ~31 ms into the next sound; section 12: formant tracks
                   need continuity checks (IY's F2 mislabelled at 8 kHz)
1.13  2026-11-08   VoxProbe 13 recorded (library 0.4.3): section 6.9 (the Speed step above 114, pauses, Slow and
                   copies, unvoiced-stop closures and read-ahead, voiced-stop closures, nasal and liquid joins,
                   after a hiss, GE, TH); section 12: band-wise lengths, run boundaries, checking placement against
                   the other unit; open items listed
1.12  2026-11-07   section 12: VoxProbe 10-13 and the 2026-11-05 recording set listed; lessons on using only the two
                   reference units, on reading earlier recordings before writing a probe, and on judging phrase
                   timing by length and spectrogram
1.11  2026-11-07   fitted on Man Goes Down's phrases and the recorded two-sound pairs, units A and B only (section 6.8;
                   vox_test, an unidentified unit, excluded): speech Speed below 114 on the effects' law (x1.21 at 108);
                   a voiced stop after a nasal closes nearly silent (38 ms, nasal formants falling, bar 16 dB down,
                   release ramp); GO's release before OW (assumed); a stop on either side takes the short cut, none
                   after an unvoiced stop; vowel and glide joins by F1+F2 distance (assumed, replacing the fixed 30 ms);
                   a voiced stop into a vowel trimmed by F2 distance and its closure capped at 30 ms without a Slow
                   (assumed); a bend toward ZZ before SE; SE's copy 80 ms; a slowed glide before a Slow +50 ms
                   (assumed); pauses P0 20, P3 648, P5 30, P6 59 (VoxProbe 1); Bend 0-2's second line corrected to
                   run parallel to the tone (VoxProbe 9); new mark "assumed"
1.10  2026-11-07   193 TS's release decays (-13 dB over its 95 ms, both units, every take; the white floor holds):
                   the band's end level as a field (nd); the sibilants' starting burst is twice what the
                   library gave (SE, SH, SO, both units): th rescaled /64; 183 HE's noise high pass 304 Hz;
                   186 FF 75 ms, 196 EK's band 2000 Hz, 195 KO's high pass 304 Hz; 253 M1's low block under a
                   high-passed hiss (fxs: a noise high pass and a burst length per effect; burst /64); the 2026-11-05
                   recordings' placement of unit A corrected (HE on WH's slot; HO WH FF TS KE KO EK unplaced)
1.9   2026-11-06   a stop's closure after a sound at least 45 ms (Man Goes Down's GO); 231 C1's pulse-rate track
                   measured at Pitch 60, 88, 150 and 212 (Man Goes Down's death sound)
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
