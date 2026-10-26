# Design

speakjet is an emulation of the Magnevation SpeakJet. It was written from the
*SpeakJet User's Manual* (Magnevation, revisions 1.0 and 1.1, 2004, whose
tables are identical) and from general phonetics. Copies of both are in
[`reference/`](reference/).

Where two independent sets of measurements of the real chip agree, they
replace guesses: the [openFPGA-7800](https://github.com/Miasmark/openFPGA-7800)
AtariVox engine's, read from recordings of games speaking (MIT licence), and
those behind Stella's SpeakJet engine (its `feature/speakjet` branch). They
set some formants of nine allophones (and, until measured directly, the
output filter and the Speed curve); see
[Measurements of the real chip](#measurements-of-the-real-chip).
No code or tables were taken from either.

A recording of a real SpeakJet playing a known sequence of codes, compared
sound by sound with this library, sets how long the chip plays each kind of
sound, Fast, how loud its noise is against its vowels, and the A5 and M1
sound effects; see [A recording of the chip](#a-recording-of-the-chip). A
second recording, of every allophone and effect on its own and of sweeps of
each control, sets the Speed curve, how pauses follow Speed, three pause
lengths, each allophone's length, the shape of the fricatives' hiss, and the
sound effects; see [The VoxRecord recording](#the-voxrecord-recording). Its
third, clean take sets the output filter, five vowels' formants, each
allophone's level, Bend, Stress and Relax, the pitch, and the effects'
timbre; see [The third take](#the-third-take).

The manual documents the chip's synthesizer in some detail, but not the MSA
"sound component database" that drives it: what oscillator settings make each
allophone, and how one sound joins the next. So the library has three parts,
and this document says, for each, what comes from the manual and what is a
choice made here.

```
serial bytes ──> input buffer ──> MSA sequencer ──> synthesizer ──> samples
 sj_write_byte()  (64 bytes)       (segments)        (5 osc + env)   sj_render()
```

All of it is in `speakjet.c`, in three sections: the synthesizer, the sound
tables with the MSA sequencer, and the chip (input buffer and API).

## Input buffer

**From the manual:**

- The buffer is 64 bytes.
- The Buffer Half Full line is set when the buffer holds more than 32 bytes.
  It is the chip's flow-control line.
- "Each command is removed from the input buffer after the command is
  executed": a command counts towards the buffer until it has finished
  playing, not just until it starts.
- Codes 20 to 26 and 28 to 30 take a data byte; the rest are one byte.

**The AtariVox's READY pin.** The *AtariVox Programmer's Guide* (Alex
Herbert, 2004) says READY, on joystick pin 2, reads 0 when the SpeakJet's
input buffer is full and data should not be sent. That is the flow control
the manual gives Buffer Half Full, so an emulator should drive READY as
`!sj_buffer_half_full()`. Note that the SpeakJet also has a pin named
*Ready* (D0); that one only shows the chip has passed its self-test and is
on, and isn't the AtariVox's READY.

**Choices:**

- A command that doesn't fit is dropped whole. `sj_write()` returns false.
- `sj_write_byte()` holds the first byte of a two-byte command until its data
  byte arrives, and only then checks that the command fits.
- Reset (`sj_reset()`, the reset line) empties the buffer and restores the
  default controls, but doesn't cut short the sound already playing.

## MSA sequencer

The sequencer takes commands from the buffer one at a time and turns each
sound into a list of *segments*. A segment glides the synthesizer's registers
from wherever they are to a target over some time, then holds the target. The
synthesizer is never set abruptly except where the manual says so, which is
what joins one sound smoothly to the next.

**From the manual:**

| | |
|---|---|
| Allophones | Codes 128 to 199, and their durations ("Msec." column) |
| Sound effects | Codes 200 to 254, and their durations |
| Controls | Defaults: Volume 96, Speed 114, Pitch 88, Bend 5. Ranges: Volume and Speed 0 to 127, Pitch 0 to 255, Bend 0 to 15 |
| Pauses | Codes 0 to 6: 0, 100, 200, 700, 30, 60 and 90 ms |
| Pause behaviour | Pauses 1 to 3 "ramp the volume while the formant frequencies are being changed"; pauses 4 to 6 "wait for silence before changing the formant frequencies" |
| Delay | Code 30 waits "the number of 10ms intervals" in its data byte |
| Modifiers | Fast (7), Slow (8), Stress (14) and Relax (15) apply to the next sound only: Fast plays it "at 1/2 the time it normally would play", Slow "at 1 and 1/2 the time". Repeat (26) plays the next sound n times. Reset (31) restores the defaults |
| Pitch | In Hz, and only for voiced sounds; 0 "will not actually vocalize" |

**Choices** (the manual gives no figures for these):

- **Formants.** Each allophone's oscillator frequencies and volumes come from
  published formant measurements of adult male English speech, except 23
  formants of nine allophones that two sets of measurements of the chip agree
  on (see [Measurements of the real chip](#measurements-of-the-real-chip)).
  Diphthongs hold their first target, slide to the second, and briefly hold
  it. Stops
  are a closure (silence, or a low voice bar for voiced stops), a noise burst
  and then aspiration or voicing.
- **Sound effects.** Apart from DTMF (standard tone pairs) and M1, the
  effects are traced from a recording of the chip: each glides between
  measured points of frequency and level, as a tone, noise or a buzz (see
  [The VoxRecord recording](#the-voxrecord-recording)). Their lengths are
  the chip's, not the manual's; DTMF tones last 153 ms, not 95.
- **Lengths.** The chip plays speech sounds longer than the manual's
  lengths. Each allophone has its own measured length, from 0.84 (VV) to 2.6
  (SO) times the manual's, fitted to sounds played alone and then, by class,
  to sounds strung together (see [The third take](#the-third-take)).
  **Fast** plays a sound at 0.6 of its length rather than the manual's half
  (see
  [A recording of the chip](#a-recording-of-the-chip)).
- **Levels.** Each allophone's level against AW is the chip's, measured from
  the VoxRecord recording's third take (see
  [The third take](#the-third-take)). The voiceless fricatives' hiss is
  shaped to the chip's spectra.
- **Speed.** A sound's length falls in a straight line with Speed:
  (146 − Speed) / 32 times its length at the default, 4.6 times at Speed 0
  and 0.59 at 127. Pauses follow Speed too.
- **Stress** raises F2 and F3 by 80 Hz and leaves the pitch alone; **Relax**
  does nothing, as measured on the chip (see [The third take](#the-third-take)).
  The manual says only that "with STRESS, IH sounds more like (but not quite)
  the IY sound".
- **Bend** adds to the frequencies of oscillators 1 to 3, in speech and
  effects alike: 44 Hz a step from 5 up, and 44 × Bend − 198 Hz below 5. The
  manual says only that it runs from a "deep-hollow" to a "High-metallic"
  sounding voice.
- **Pitch** sets the voicing period one sample longer than the setting asks
  for, as the chip does: Pitch 250 sounds at 242.6 Hz.
- **Transitions.** Speech sounds glide into their target over 35% of their
  length, kept between 20 and 50 ms.
- **Ramps.** A pause ramps the volume down over 10 ms. When the buffer runs
  empty, the chip ramps to silence over 15 ms.
- **Pause lengths** are the manual's, except pause 3 (650 ms, not 700),
  pause 5 (35, not 60) and pause 6 (70, not 90), and all are lengthened or
  shortened by Speed as sounds are.
- **Pauses 1 to 3** glide the formants towards the first target of the sound
  that follows. **Pauses 4 to 6** make the next sound start at its own
  frequencies instead of gliding into them.
- **Commands with no effect.** Wait (16), port control and output (24, 25)
  and the EEPROM phrase commands (28, 29) are accepted but do nothing. There
  is no serial control protocol to restart a wait, no output pins, and the
  contents of the AtariVox's SpeakJet EEPROM aren't known. Soft (18), which
  the manual doesn't list (see below), is ignored too, as are unknown codes,
  including 255.

## Allophone names

The library and text2speakjet use the manual's names for the codes:
`sj_sound_name()` returns them, and text2speakjet writes them in its output.

One name needs a note. Allophone 159, the "EL" of "saddle" and "angle", is
**EHLL** in the manual: in its code table (`159 EHLL Saddle, Angle, Spell`)
and its phonetic guide (`\EHLL`), in both revision 1.0 and revision 1.1.
Magnevation's PhraseALator software spells it **EHLE**, and that spelling has
spread: it is in the PhraseALator dictionary, the *AtariVox Programmer's
Guide*'s example phrase, and Mike McCauley's Arduino SpeakJet library
(`Phoneme_EHLE = 159`). The two names mean the same byte, so the choice
doesn't affect the sound. text2speakjet accepts EHLE as an alias on input
and writes EHLL.

## Magnevation's PhraseALator

Magnevation's PhraseALator software (in
[`reference/phrasealator/`](reference/phrasealator/), deprecated and kept for
reference only) shipped with its Visual Basic source, and its code table
settles two things the manual doesn't:

- **Soft (18).** The manual's command list goes from Wait (16) to Volume
  (20). PhraseALator has a button for code 18, "Play the Next Sound Soft",
  and its dictionary uses it, mostly just before BE or BO. So it is a real
  command the manual leaves out. Nothing says what it does to the sound, so
  the library ignores it; text2speakjet accepts `SOFT` and `[soft]`, and
  keeps it where the dictionary has it, so a real chip still gets it.
- **Notes.** Its `\NTC1` to `\NTB3` buttons send Pitch with the note's
  frequency in Hz, which agrees with the manual saying Pitch is in Hz.

It also disagrees with the manual about two codes: it calls 135 AW and 136
OH, where both editions of the manual have 135 OH ("Hot, Clock, Fox") and 136
AW ("Father, Fall"). Its dictionary's spellings use the manual's names, so
PhraseALator sent the two vowels swapped. The library and text2speakjet
follow the manual, and a recording of a real SpeakJet confirms its order:
135 is the more open vowel of "hot" (see [The third take](#the-third-take)).

## Synthesizer

**From the manual:**

- Output is 8192 samples per second.
- Oscillators 1 to 3 run from 0 to 3999 Hz with a volume of 0 to 31. They are
  summed in a mixer limited to 63.
- Oscillators 4 and 5 share a distortion control (0 to 255) that spreads
  white noise around their frequency.
- A sixth oscillator envelopes the others. Its wave is saw, sine, triangle or
  square. Oscillators 1 to 3 can be fully enveloped, and 4 and 5 half
  enveloped.
- The master volume runs from 0 to 127.
- A two-pole low pass filter follows the PWM output.

**Choices:**

- Oscillators 1 to 3 are sines whose phase restarts at each envelope period,
  so they ring like formants excited by a glottal pulse. For speech, the
  envelope frequency is the voicing pitch.
- Distortion is applied as random frequency modulation of oscillators 4 and
  5, from a xorshift noise source.
- The output filter, after a DC blocker, is an 11-tap FIR with the chip's
  measured response: flat to 1.5 kHz, −5.7 dB at 2.4 kHz, −15.8 dB at
  3.6 kHz (see [The third take](#the-third-take)).

## Measurements of the real chip

### Sources

- **openFPGA-7800's AtariVox engine** (MIT licence;
  [`docs/ATARIVOX.md`](https://github.com/Miasmark/openFPGA-7800/blob/Mister-Pocket-Port/docs/ATARIVOX.md)).
  It took the exact codes games send from their ROMs (Juno First, Stratovox,
  Gorf, rubyQ), measured clean recordings of those games and of Magnevation's
  alphabet demo, and fitted a synthesizer much like this one to them: sine
  formants restarted each pitch period, a two-pole output filter, 8192 Hz.
- **Stella's SpeakJet engine** (its `feature/speakjet` branch), whose
  parameters were fitted to recordings of the chip.

Neither is definitive. Each was fitted with its own model, and each has gaps.

### Method

1. Only what **both** sources measured is a candidate. A value from one source
   alone is not used, however plausible.
2. The tolerance was set before comparing: two measurements **agree** when
   they are within **10% or 88 Hz** of each other, whichever is wider. 88 Hz is
   the spacing of the voice's harmonics at the default pitch (88). A formant
   read from a recording at that pitch can only be placed to within about one
   harmonic, so a tighter tolerance would reject real agreement as noise.
3. A value the two agree on is set to their **mean**, so neither source is
   preferred over the other.
4. Anything not confirmed **stays as it was**, including values where both
   sources disagree with this library in the same direction but not with each
   other.

### Formants

openFPGA measured nine allophones, and Stella has values for all of them. In
Hz, as F1 / F2 / F3; "–" is a formant openFPGA didn't measure, and openFPGA's
ranges are taken at their midpoints:

| | openFPGA | Stella | Agree | Was | Now |
|---|---|---|---|---|---|
| AX | 520 / 1515 / 2400 | 523 / 1505 / 2390 | all | 520 / 1500 / 2450 | 521 / 1510 / 2395 |
| RR | 438 / 1318 / 1763 | 440 / 1318 / 1676 | all | 350 / 1100 / 1450 | 439 / 1318 / 1720 |
| IH | 300 / 1775 / 2520 | 344 / 1773 / 2460 | all | 390 / 1990 / 2550 | 322 / 1774 / 2490 |
| EY | 480 / 1870 / 2460 | 535 / 1771 / 2390 | all | 470 / 2000 / 2600 | 508 / 1820 / 2425 |
| OW | 495 / 904 / – | 529 / 879 / – | F1 F2 | 480 / 900 / 2350 | 512 / 891 / 2350 |
| UX | 605 / 1335 / – | 622 / 1321 / – | F1 F2 | 640 / 1190 / 2390 | 613 / 1328 / 2390 |
| EH | 590 / 1650 / – | 610 / 1681 / – | F1 F2 | 530 / 1840 / 2480 | 600 / 1666 / 2480 |
| MM | 280 / 1150 / 2200 | 256 / 1420 / 2174 | F1 F3 | 250 / 1000 / 2200 | 268 / 1000 / 2187 |
| AW | 610 / 1030 / – | 712 / 965 / – | F2 | 590 / 880 / 2540 | 590 / 998 / 2540 |

The diphthongs built from these vowels change with them. Oscillator volumes
are unchanged: only one source measured them.

### Speed

Duration was exp(−0.02 (Speed − 114)), openFPGA's fit to games speaking at
Speeds 80 to 127. The measurements behind Stella's engine agree with it to
within 8% from 60 to 114:

| Speed | openFPGA | Stella | Was (114 / Speed) |
|---|---|---|---|
| 60 | 2.94 | 2.76 | 1.90 |
| 80 | 1.97 | 2.12 | 1.43 |
| 100 | 1.32 | 1.43 | 1.14 |
| 114 | 1.00 | 1.00 | 1.00 |

Above 114 and below 60 the two disagree with each other, so the curve was
extrapolated there. The VoxRecord recording has since measured the whole range
and replaced it (see [The VoxRecord recording](#the-voxrecord-recording)).

### Output filter

openFPGA fitted a 2-pole low pass at 2200 Hz to the long-term spectrum of the
alphabet recording; Stella's measurement, made independently, is 1700 Hz.
That was used until the VoxRecord recording's third take measured the
response directly (see [The third take](#the-third-take)).

### A recording of the chip

A recording of a real SpeakJet playing Alex Herbert's AtariVox test ROM
(`vox_test`, 2004) was compared with this library playing the same codes. The
ROM's five phrases are: M1; "testing, testing, one two three"; A5 then
"danger", three times (Pitch 110, Bend 1); a phrase at Speed 116, Bend 4,
Pitch 55; and BE IY DE IY five times at Speed 127. Each sound's start and end
were read from the recording's band energies, and levels compared against the
vowel beside each noise sound.

What agreed already: Pitch (88 and 110 Hz exactly), the 200 ms pause, and the
formants of the steady vowels EH and IH (within 5%).

What changed:

| | Recording | Was | Now |
|---|---|---|---|
| Steady sounds, against the manual's length | EH 2.07, IH 1.6, NGE 1.43; slow SE 1.67, TH 1.42, NE 1.14 | 1.0 | 1.5 |
| Diphthongs | EYIY 1.0, IHWW 1.5; fast OHIY 1.2, AXRR 1.3 | 1.0 | 1.2 |
| Voiceless stops | TT 1.0 | 1.0 | 1.0 |
| Fast | 0.53 to 0.67 (NE, JH, AXRR) | 0.5 | 0.6 |
| SE under the vowel beside it | 23 dB | 0 dB | 23 dB |
| CH, TH, FF; stop bursts; aspiration | 16, 20, 21; 16 to 23; 19 dB | 0 to 7 dB | measured |
| A5 | two tones 12 dB apart, gliding together 486 → 190 → 478 Hz and 2525 → 2232 → 2522 Hz | a 160 Hz buzz | the recording |
| M1 | 30 ms of noise mostly under 2 kHz, then noise around 1 to 2 kHz dying away by 30 dB | noise around 600 and 1800 Hz fading linearly | the recording |

With the new lengths and the existing Speed curve, the four speech phrases
come within 9% of the recording's length (they were 7 to 25% short), and at
Speed 127 a BE IY syllable is 115 ms against the recording's 115.

The ROM sets Bend 1 before A5, so A5's frequencies were those of the
recording divided by Bend 1's factor. The VoxRecord recording has since shown
that Bend doesn't scale the effects that way, and A5 is now traced from it at
the default Bend. Fricatives the recording doesn't have (SO, SH, HE, HO, WH, and the
noise of the voiced fricatives) take the mean of those it has. M1 is 3 dB
quieter against the vowels than the recording, as its noise oscillators are
already at full volume.

The recording also suggested, on too few examples to change, that pause 6
is about 55 ms rather than 90, and that Bend moves F1 and F2 by about 3% a
step and F3 barely at all. The VoxRecord recording confirms the first.

### The VoxRecord recording

`VoxRecord` is a 4K ROM, built on Alex Herbert's `vox_test` and AtariVox
driver, that plays a fixed sequence: every allophone and every sound effect
on its own, each followed by 400 ms of silence; sweeps of Pitch, Speed, Bend
and Volume; the pauses and delays between two AWs; Fast, Slow, Stress and
Relax on six sounds; Repeat; Soft (18); and some consonant-vowel-consonant
syllables. A real AtariVox playing it was recorded at 48 kHz, twice, and
compared with this library playing the same bytes.

The two takes agree closely: allophone lengths to 2.5 ms on average (12 ms at
most, SE), the fricatives' spectra to 2 to 4 dB, the Speed sweep to 15 ms, the
effects' traces to within a semitone (except the noisy C8). The lengths and
fricatives are fitted to the mean of the two.

The recording is overdriven: the vowels are clipped (about 15% of their
samples), so their levels, their formants and the Volume control can't be
read from it. Timing, pitch, and the fricatives (which aren't clipped) can.
Its clock runs 0.48% slow (the television's 15734 Hz line whistle reads as
15809 Hz, and every DTMF tone is high by the same amount), which is corrected
for.

What changed:

| | Recording | Was |
|---|---|---|
| Speed | length falls in a straight line: 2115 ms at Speed 0, 1825 at 20, 1560 at 40, 1225 at 60, 955 at 80, for TT AW EYIY SE; (146 − Speed) / 32 of the default length | exp(−0.02 (Speed − 114)), 9.8 times at Speed 0 |
| Pauses and Speed | two pause 2s after each group last 1845 ms at Speed 0, 1540 at 20, 815 at 80: the same factor as the sounds | not scaled |
| Pauses 3, 5, 6 | shorter than the manual's 700, 60 and 90 ms: about 650, 45 and 70 (measured against the two delays, whose length is exact; the takes differ by up to 35 ms on a pause) | the manual's |
| Allophone lengths | audible lengths from 18 ms (OK) to 268 ms (OWIY): vowels IH to OH about 140 ms, back vowels 120, nasals and liquids 90 to 110, HE HO WH FF about 75, SE 86, SO 100 | 1.5 times for every steady sound and 1.2 for diphthongs, which made the nasals, liquids, voiced fricatives and HE to FF 30 to 40 ms too long and the front vowels 20 ms too short |
| Fricative hiss | in third octaves from 250 Hz to 4 kHz: SE and SO peak at 3.2 to 4 kHz, SH at 2 to 3 kHz, HE and HO at 1 to 1.5 kHz, WH at 0.6 to 0.9 kHz | broad noise, mostly under 2.2 kHz |
| DTMF | 152 to 154 ms | 95 ms |
| Other sound effects | swept, stepped and pulsed tones and buzzes, many of different lengths from the manual's (A1 and A2 about 975 ms, M0 1.2 s) | guesses from the names |
| Bend on sound effects | adds to the frequencies rather than multiplying them: D1's tones move together by −198, −154, 0, +220 and +439 Hz at Bends 0, 1, 5, 10 and 15, keeping their 512 Hz spacing, and A5 the same at 0 and 1 | multiplied, like speech |

The lengths were fitted by playing each allophone on its own here, measuring
its audible length the same way as on the recording (from where it rises out
of silence to where it falls back, at the same depth below its peak), and
adjusting its factor until the two matched; all are within 2 ms but EYRR (6
ms). The stops and affricates aren't fitted, since where a stop starts and
ends on a recording says nothing about its silent closure. The fricatives
were fitted by matching their third-octave spectra through this library's
output filter, keeping each one's overall level. The effects were traced
every 5 ms (strongest frequency, level, and whether the sound is a tone,
noise, or a buzz with a lower pitch found by autocorrelation) and reduced to
the fewest points that follow the trace to within 5% and 4 dB; A1, A2, A3 and
M2 turn out to buzz at the default speech pitch.

What agreed already: Pitch (30 to 120 Hz exactly; 160, 200 and 250 come out
1.6 to 2.8% flat on the chip, which isn't modelled), pauses 1, 2 and 4 and
both delays (within the recording's 10 ms), Fast (0.65 to 0.75 of the plain
length, as audible lengths, which include the fade), Slow (1.3 to 1.5),
Repeat, and that Soft (18) changes nothing audible in BE and BO.

Bend on the effects is 44 Hz a step from 5 up and from 1 down, with 22 Hz
missing somewhere between 1 and 5; here the shortfall is taken at 5. It is
applied to oscillators 1 to 3, the tones.

What it shows but doesn't settle:

- **An extra sound** between the Speed 80 and Speed 100 groups, in all three
  takes alike: an 85 ms burst, then three 180 ms tones at 746, 501 and
  746 Hz. No code in the sequence accounts for it, the driver sends the bytes
  plainly, and it matches none of the 55 effects. The bytes there are
  `2 2 21 100 191 136 154 187`.

### The third take

A third take, recorded at a lower level, is barely clipped (0.25% of samples;
the vowels peak at full scale and clip at most 1.7% of theirs), with the
noise floor 18 dB further down. It repeats the first two (Speed, pauses,
pitch, the extra sound, the fricatives' spectra to 1 to 2 dB) and measures
what they couldn't. Everything below was refitted to it, in this order, each
measured the same way on the recording and on this library:

| | The chip | Was |
|---|---|---|
| Output filter | the effects that sweep one pure tone lose level with frequency alone: solving for a response common to ten of them gives flat to 1.5 kHz, −2.2 dB at 1.9 kHz, −5.7 at 2.4, −10.8 at 2.9, −15.8 at 3.6, −17.2 at 3.9 (the shape of a third-order Butterworth at 2 kHz); an 11-tap FIR matches it to 0.9 dB | 2-pole Butterworth at 2200 Hz |
| Vowel formants (linear prediction) | AX, UX, EH, EY, IH, OW, UH, LO and RR within about 5% (several of these were set from the two independent measurements, so they are confirmed); AW F1 695, UW 380 / 1065, AY 710 / 1660, OH 690 / 1150, WW 495 / 765 Hz | AW F1 590, UW 300 / 870, AY 660 / 1720, OH 730 / 1090, WW 300 / 650 |
| 135 and 136 | the same F1 (672 Hz), but 135's F2 is 1178 Hz and 136's 1035, alone and in HE _ TT: 135 is the more open "hot", 136 the rounder "fall", as the manual has them | unknown |
| Bend on speech | adds, like the effects: AW's F1, F2 and F3 all move by about 40 Hz a step (F3 2480 to 2912 Hz at Bend 15) | a semitone a step |
| Stress | F2 and F3 up 65 to 100 Hz (AW, EYIY, NE), pitch and F1 unchanged | pitch × 1.12, F1 × 0.94, F2 × 1.06 |
| Relax | no measurable change | pitch × 0.9, F1 × 1.06, F2 × 0.94 |
| Pitch | each period one sample long: 87.1 Hz for 88, 156.9 for 160, 195.2 for 200, 242.6 for 250 | exact |
| Levels against AW | vowels within 1.5 dB; nasals 4 dB and liquids 2.5 dB quieter than here; the hiss 4 to 12 dB quieter (FF 12, HE and HO 7, SE 6), JH 13 and VV 8; BE and BO 7 dB louder, PE and PO 9 | the vox_test recording's levels, itself probably clipped |
| Allophone lengths | vowels and diphthongs 10 to 14 ms shorter than the first two takes had them (their clipping made loud sounds read long) | the mean of the first two |
| Volume | linear in amplitude, to 1.5 dB | linear |
| Sound effects | the tones are pure sines (harmonics 45 dB and more down; the stacks in the first two takes were clipping); R0 to R9 are three tones each; A1, A2, A3 and M2 buzz at 87 Hz with three formants (M2, "WOW", is a vowel glide) | the strongest frequency only |

How each was fitted:

- **Output filter.** Frames of single steady tones from the ten effects that
  sweep one, solved by least squares for one response in 250 Hz bands plus a
  level for each effect. A recursive filter at 8192 Hz can't hold the
  response so close to 4096 Hz, hence the FIR.
- **Vowels.** With the measured filter the vowels' third-octave spectra are
  4.0 dB RMS from the chip's (5.0 before), with their original formant
  volumes; the 3.2 to 4 kHz band, 10 dB short before, is 3 dB short.
- **Fricatives** were refitted through the new filter to the mean of the
  three takes' spectra, then their levels set with the others.
- **Levels** are a gain for each allophone (`allophone_gain_db`), fitted so
  that each one's level against AW matches the chip's to 0.1 dB.
- **Lengths** were refitted after that, in two steps. Played alone, each
  allophone's audible length was matched to the chip's (0.2 ms on average).
  But strung together the chip gives sounds longer slots than their audible
  length alone suggests: held-out syllables such as MM IY MM came out 12%
  short (320 ms on the chip, 235 here). So one multiplier for each class was
  then fitted to 30 connected groups (AW MM at eight pitches, AW SE at eight
  Bends, TT AW EYIY SE at Speeds 0 to 80, and the nine syllables of sections
  12 and 13): vowels 0.92, nasals 1.32, diphthongs 1.10, voiceless
  fricatives 1.14, stops and affricates 1.40. The groups go from 6% short
  (38 ms) to within 1% (16 ms; 22 ms with each group left out of its own
  fit), and the syllables from 12% to 2% short, at the cost of sounds played
  alone running 13 ms long on average. The liquids and voiced fricatives,
  which no group contains, keep their first-step lengths.
- **Effects** were retraced: tones as up to three sines, noise, or for an
  effect that buzzes for a quarter or more of its length, a buzz throughout,
  its three formants followed from frame to frame. The traced levels have
  the output filter's loss added back, and each effect's overall level was
  fitted against AW (0.2 dB on average). DTMF is 2.4 dB quieter than before.
  A1 and A2 are the roughest: the chip has a steady low formant under two
  rising ones, which the trace only partly keeps.

### Not used

Bend, how fast formants move from one sound to the next, oscillator volumes,
the other allophones and the sound effects are measured by at most one of
the sources, so they remain the choices described above.

### Comparing every sound

Every allophone and effect was then compared with the third take on its own:
the RMS difference of its third-octave spectrum (250 Hz to 4 kHz) for the
allophones, and how much of the hardware's spectrogram it reproduces after
time alignment for the effects. That found four more things, now fixed:

| | Found | Fix |
|---|---|---|
| AWRR, OWRR, OWIY | 1 to 3 dB further from the chip than before the third take | they start from the earlier OH and AW, which the corrections to OH and AW had moved |
| Fricative onsets | the noise oscillators glided up from the previous sound, so SE and SO swept up into their hiss | noise that starts now starts at its own frequencies |
| Nasals | the chip's nasals have strong resonances near 1.4 to 1.6 kHz and 2.2 to 2.6 kHz | each nasal's resonances and volumes fitted to its spectrum (NGE by a search on the whole sound) |
| Stops | 6 to 8 dB too much at 3.2 to 4 kHz | each stop's burst and aspiration frequencies scaled together (0.5 to 1.15); most of the chip's bursts are lower |

The levels were refitted after each change. Results, against the third take:

| | Original library | Before these fixes | After |
|---|---|---|---|
| Allophone spectra (mean RMS difference) | 6.0 dB | 5.2 dB | 4.6 dB (better than the original on 61 of 72, and none more than 1 dB worse) |
| Nasals | 8.8 dB | 8.6 dB | 4.4 dB (spectrogram 56% to 85%) |
| Syllables of sections 12 and 13 (spectrogram match) | 70% | 77% | 82% |

### Fitting whole sounds

Scoring every sound the same way (how much of the chip's spectrogram each
reproduces, both cropped to the same depth below their peak) left 14 sounds
under 30%, all of them hiss, stops or effects. Two further passes fitted
those as whole sounds.

Effects. Seven effects (C0, C2, C3, C4, C6, C8, C9) are tones the chip
switches on and off at a voicing rate: each now has a gate (`fx_gate`, a
rate and an envelope wave) found by search. R0 to R9 are steady tones on the
chip, not the traced glides, and are rebuilt from measured tones keeping the
traced loudness. The effects' levels were refitted.

Allophones. A test program builds one allophone from a set of values with
the library's own constructors and plays it through the library exactly as
sjwav does (bit for bit the same output). A search (differential evolution)
moved the values to bring the spectrogram closest to the chip's, with the
third-octave spectrum and the length as further terms. What it found:

| | Found | Fix |
|---|---|---|
| Hiss is narrow on the chip | TT and TU hiss in a band near 2.8 to 3.6 kHz; the bursts and aspiration here were broad noise (distortion 255 and 230) centred at 1 to 2 kHz | each stop now has its own burst and aspiration noise widths and aspiration volumes (`STOPN()`) |
| SE, SO and SH start with a thump | about 10 ms of a 400 to 600 Hz tone before the hiss, in both unclipped takes | `thumped()` |
| CH and TS had a thump the chip hasn't | a stop with no aspiration drove the noise oscillators at 0 Hz, which hold the phase the burst left them at: a step the dc blocker turns into a low thump | that part is now silent |
| Voiced stops | their loci and voicing differ from the published values; EB, OB, ED and OD last 25 to 35 ms on the chip, not 10 | fitted loci, voice bar and onset voicing |
| JH, VV | JH is a buzz with a resonance near 2 kHz, VV a buzz with resonances near 500 and 1500 Hz | fitted frames |

Three things kept the fit honest:

- The chip doesn't match itself on hiss. The first and third takes match
  each other only 28% (TT), 34% (SE), 46% (FF), 49% (TS), 53% (TH) and 70%
  (SO, SH). Fitting the third take alone fitted that take's noise (TT 69%,
  above what the chip manages against itself), so each sound is fitted to
  both takes and two noise seeds together.
- A silent closure can't be measured from a sound played alone (it is
  cropped with the leading silence), so the search left it anywhere (ED and
  OD began with 120 ms of silence). The stops whose closure is silent keep
  the closure length they had; TT's comes from the connected groups.
- A sound played alone and the same sound in connected speech don't take the
  same time on the chip. TT and SE, which the 30 connected groups contain,
  were fitted with the groups' lengths as another term.

Each sound's parts still add up to the manual's length, the difference being
in allophone_length. Results, against the third take:

| | Original library | Before | After |
|---|---|---|---|
| Allophones (spectrogram, mean) | 57% | 63% | 73% |
| Allophones under 30% | 13 | 10 | 1 (TS) |
| Voiceless stops | 32% | 32% | 65% |
| Fricatives | 37% | 44% | 65% |
| Effects (spectrogram, mean) | 29% | 64% | 70% |
| Effects under 30% | 40 | 4 | 1 (C8) |
| Connected groups (mean length error) | | 17 ms | 12 ms |

A hiss sound's score against one take moves by up to 10 points with the
noise, so scores on single hiss sounds are only good to about that.

### The VoxProbe recording

The VoxProbe ROM (see the VoxProbe repository) plays a sequence built to
test the engine rather than individual sounds: low pitches where each voicing
pulse stands alone, Repeat, pauses and delays three times each, effects at
other Pitches and Speeds, a Bend sweep on a DTMF tone, a Volume sweep and a
Pitch sweep. One recording, of the same AtariVox, changed the engine:

| | On the chip | Before | Now |
|---|---|---|---|
| Voicing | each pulse restarts the formant sines, which fall in a straight line to nothing in a fixed time (7.4 ms for AW at pitch 40, a fit with R² 0.96); the rest of a long period is silent, and above 125 Hz the next pulse cuts the ramp short | a sawtooth over the whole period | `ENV_VOICE`, 8 ms (the best fit to both recordings; 7.4 to 8.5 ms fitted to AW's waveform, 5 to 8.7 ms for other single sounds) |
| Effects and Speed | stretched by the speech's Speed curve (A1 2.70 times at speed 60, the curve 2.69) | not stretched | stretched |
| Voiced effects and Pitch | A1 and M2 follow the Pitch control (60.3, 87.9, 149.5 Hz at 60, 88, 150) | the traced pitch | the traced pitch scaled by the Pitch control |
| Repeat | each copy after the first adds just the manual's length (AW 70 ms; IY, UW, MM six times within 6 ms) | each copy the whole stretched sound | copies at the manual's length |
| Lengths of vowels, nasals, liquids and diphthongs | (from Repeat) the manual's length and a fixed time | a multiplier fitted to each of 37 sounds | the manual's length and 28 ms (`TRANSITION_MS`) |
| Fast, Slow | 0.63 to 0.66, 1.31 to 1.42 | 0.6, 1.5 | 0.65, 1.35 |
| Pauses | AW, pause, AW: 21, 109, 212, 661, 26, 47, 73 ms between the AWs; pause 0 still leaves a gap | 0, 100, 200, 650, 30, 45, 70 ms | lengths that give the chip's gaps within about 5 ms |
| Delay | 30, 60, 90 ms leave 44, 75, 116 ms; unchanged by Speed | exact | 12 ms more |

The spectra here are compared with each floored 30 dB under its own peak:
compared whole, the hardware's noise floor against the library's silence
counts as a difference (a pure DTMF tone, 98% spectrogram match, read 19 dB
off), and that chose a 6 ms ramp that was worse than the old sawtooth. With
the floor, the fixed ramp at 8 ms takes the voiced allophones' spectra from
3.94 to 3.82 dB of the third take, and the VoxProbe recording's sustained
sounds from 5.2 to 4.5 dB; 6 ms is worse than the sawtooth on the third take
at a floor of 25, 30 or 35 dB, and 8 ms better on both recordings at all
three. The voiced stops, VV and JH were refitted
with it, TT's and SE's lengths refitted to the connected groups, and the
levels refitted. Against the third take, allophones go from 72.6% to 73.6%
spectrogram match and from 5.25 to 4.28 dB since the last release (floored
spectra); the connected groups from 13 to 14 ms (one constant now does what 37
lengths did).

Confirmed as the library had them: Bend adds 44 Hz a step and only 22 Hz
between 4 and 5 (the two DTMF tones stay 512.2 Hz apart at every Bend), and
moves C8's noise but not SE's hiss; Stress raises F2 and F3 by about 85 Hz
and Relax changes nothing; Volume is linear in amplitude (within 0.5 dB from
8 to 127); Pause scales with Speed.

Not changed:

- Pitch. The chip's voicing frequency departs from the setting by +0.5% to
  -1.7%, and not smoothly (229.7 Hz at 230, 167.5 at 170). The library's
  formula is up to 2.7% off at high pitch, which makes most of the remaining
  error there, but twelve settings can't recover the chip's mapping, and the
  recorder's clock is only known to about 0.7% (its 2600 line-rate whine reads
  15816.9 Hz against 15699.8; the third take's correction used the broadcast
  15734 Hz and may be 0.2% short).
- DTMF and tones sit about 5 Hz above nominal at every Bend (an offset, not a
  ratio), and Bend's steps alternate 43 and 45 Hz above Bend 7: the chip's
  frequency arithmetic is quantised somehow, but neither a divider nor an
  accumulator step fits.
- The extra sound in the VoxRecord recording is the chip's: at the same point
  of the long Speed sweep, after TT, the rest of the speed 100 group is lost,
  and after 0.8 s of silence it plays DTMF 0, 1, 0 at Bend 0 and carries on
  at 136 Hz and 5.5 dB louder. It looks like a reset and isn't emulated.

### The second VoxProbe recording

VoxProbe 2 plays every Pitch from 0 to 254, the voicing fade at pitch 40 on
18 sounds and at other Volumes and Bends, the robot effects at other
Pitches, Bends and Speed, all twelve DTMF tones, and variants of the long
Speed sweep. The recorder runs 0.72% slow (its 2600 line-rate whine reads
15814 Hz against 15699.8), and the frequencies here are corrected for it.

| | On the chip | Before | Now |
|---|---|---|---|
| Voicing frequency | each period is a whole number of ticks of a 7986 Hz clock: 8000 / Pitch (whole numbers) and one more; above pitch 200 the frequency holds flat across runs of settings and steps exactly where the count changes; 12 settings (among them the default, 88) take one tick more, the same on both halves of each test | Pitch / (1 + Pitch / 8192), up to 2.6% off | the count, with the 12 listed; within 0.12% RMS (0.27% at worst) of the chip from pitch 40 to 254 |
| Frequencies | all seven DTMF frequencies are whole multiples of 4.3845 Hz (697.17 Hz is 159, 1468.75 Hz 335, each within 0.12 Hz); a step of Bend is 10 of them, Bend 4 to 5 is 5, Stress about 19 | the standard DTMF frequencies, Bend in 44 Hz steps, Stress 80 Hz | oscillators on that grid (`FREQ_STEP_HZ`), the chip's DTMF steps, Bend and Stress in steps |
| Formants | at pitch 40 each pulse is a few milliseconds of the three formant sines alone, fitted directly | published values and spectrum fits | measured for 16 vowels, liquids and MM (NE and NO were worse with them on both recordings, and keep theirs) |
| The voicing fade | the same at Volume 32 to 127 and Bend 0 to 15; over 4.5 to 6.9 ms across 18 sounds | | unchanged (8 ms) |

The measured formants took the first VoxProbe recording's sustained sounds,
which weren't used to measure them, from 4.5 to 3.7 dB of the chip's
spectra, and LE, long the worst allophone, by 7 dB. Against the third take,
the vowels, nasals, liquids and diphthongs go from 3.81 to 3.58 dB and from
81.6% to 84.1% spectrogram match; no sound is more than 1 dB worse.

Tried and not kept:

- A voicing pulse at the start of each sound. At very low pitch the chip
  starts each sound, and each copy of a repeated one, with a pulse (AW said
  twice at pitch 0 to 6 is two pulses), so pitch 0 isn't silent; but at
  pitch 33 and 38 the pulses run on evenly across a repeat. A pulse at every
  start made the sustained sounds 0.4 dB worse, so pitch 0 stays silent here.
- The robots' lower part. R7, R8 and R9 have a component (about 640 Hz in R8
  and R9) that follows Pitch but not Bend, unlike their main tones, which
  follow Bend but not Pitch. Putting it on a noise oscillator enveloped by
  the voicing brought their spectra closer on this recording at every Pitch
  and Bend, but cost the third take 6 to 16 points of spectrogram match.
- The formants' relative strengths from the same fits, which made both
  recordings worse.

The extra sound: the chip's DTMF 0, 1, 0 and change of state happened in
three of five variants of the long sweep (the original; without its speed 0
group, one group later; the slow groups then one AW, two groups earlier) and
not in two (one speed 0 group then speed 100; the same bytes all at speed
60), nor anywhere else in the recording. It is neither a fixed position in
the stream nor a fixed time; it needs a long run of slow sounds.

### A second AtariVox

The same three ROMs (VoxRecord and both VoxProbes) recorded on a different
unit, an original AtariVox:

- Same firmware. Every Pitch from 40 to 254 gives the same voicing count,
  the same 12 exceptions included; the DTMF steps and the formants are the
  same. Everything runs 0.3% slower (tones 0.9971 times, timing 0.34%
  longer): the chip's own clock.
- No extra sound: every variant of the long Speed sweep plays correctly on
  it. That reset belongs to the first unit.
- The two chips agree far more closely than the library does with either:
  on the VoxRecord sounds, 97 to 98% spectrogram match on vowels, nasals,
  liquids and diphthongs (0.8 to 0.9 dB), 95% on effects, 65 to 75% on hiss
  and stops; the library was at 72 to 74% overall against both. The low
  agreement between takes of the first unit on hiss came from the clipped
  first take, not from the chip. FF and TS do differ between the two chips.
- The background whine isn't a clock reference after all: on this setup it
  sits 0.8% higher and drifts 0.18% within a recording.

Fitting to both chips together:

| | Found | Fix |
|---|---|---|
| SE, SO, SH | the chip's low onset is quiet (20 to 27 dB under the hiss), then the hiss at full level; here the onset was the loudest part and the hiss 20 dB under it, which the second chip's quieter recording exposed | refitted to both chips with their level envelopes as a further term; SE's length still fitted to the connected groups |
| Vowel and liquid volumes | the single-pulse fits give each formant's amplitude, which needs dividing by the output filter's gain at its frequency to be a volume (the first try, without, was worse) | the chip's volumes for 15 vowels and liquids, and for the vowel ends of the diphthongs |

Against both chips, the allophones go from 73.1% to 74.7% spectrogram match
and from 4.30 to 3.97 dB, the hiss sounds from 50% to 57% and 4.4 to 2.9 dB.

Tried and not kept: a voicing fade for each sound (the chips' fades differ
by sound in a pattern both repeat, but one ramp already reproduces most of
it: 0.02 dB); a curved fade; and starting the formant sines at any phase but
0 at each pulse, or not restarting them (all 7 to 12 points worse, which
confirms the restart). What still separates the library's vowels from the
chip's is mostly the valleys between formants, 8 to 17 dB deeper here.

## What isn't known, and what isn't emulated

### Unknown

The manual doesn't say, so these are guesses (described above):

| | What the library does |
|---|---|
| The oscillator settings behind each allophone | Built from published formant measurements of English speech; only the formants that two sets of measurements of the chip agree on are from the chip, and no volumes are |
| The sound effects' waveforms | Traced from a recording: up to three sines, noise, or a buzz through three formants; seven are gated at a fitted voicing rate |
| How one sound joins the next | Glides over 35% of a sound, held to 20 to 50 ms; a 10 ms ramp into a pause; a 15 ms ramp to silence when the buffer empties |
| The synthesizer's details | Distortion as random frequency modulation; the arithmetic behind the chip's voicing count at the 12 settings that differ from 8000 / Pitch + 1; whether a pulse starts each sound |
| What Soft (18) does | Ignored; it is a real command (see PhraseALator above), but nothing describes its effect |

Apart from the measured values above, none of this has been compared with a
real SpeakJet. Two recordings have been (see
[A recording of the chip](#a-recording-of-the-chip) and
[The VoxRecord recording](#the-voxrecord-recording)), the second in three
takes, the last of them clean.

### Not emulated

| Chip feature | Here |
|---|---|
| Wait (16), and the Start that ends it | Accepted and ignored |
| The Serial Control Protocol, entered by the escape character `\` (byte 92) and a node digit (`0` to `7` as text), then commands: EEPROM programming, direct access to the synthesizer registers, buffer clearing, baud rate and detune | Not modelled; the library takes the MSA byte stream only, and ignores 92 like any other reserved code, where a real chip would switch to control mode |
| EEPROM phrases: Call Phrase (28), Goto Phrase (29) and the 16 phrase cells | Ignored; the contents of the AtariVox's SpeakJet EEPROM aren't known |
| The power-up and reset announcement (the chip says "ready" unless configured not to) | Not made; a new or reset chip is silent |
| The eight Event Inputs, including RC servo mode | Not modelled; the AtariVox doesn't use them |
| Port control and output (24, 25) and the status pins in user mode | Ignored |
| The Auto Silence setting | Always on, the chip's factory default |
| The D0 Ready line (self-test passed) | Not provided; the library is always ready. Buffer Half Full and Speaking are |
| Serial timing (baud rate) | Not modelled; the library takes whole bytes |

## The C API's additions

These are for emulators, and go beyond the chip itself:

- `sj_write_byte()` frames the serial byte stream as described above.
- `sj_get_controls()` / `sj_set_controls()` read and restore the controls and
  pending modifiers, for save states. Sound that is playing, and the buffer,
  are not part of that state.
- `sj_power_on()` returns the whole chip, sound included, to its power-on
  state.

A chip isn't safe to use from more than one thread at a time. Separate chips
are independent: the library has no global state.

## Testing

- `tests/speakjet_test.c` checks every sound's length against the manual, a
  phrase's total length, the buffer and Buffer Half Full, the output range of
  every sound, the byte-stream framing, the controls round trip and the sound
  names.
- `tests/reference_test.c` renders each phrase in `samples/codes.txt` and
  compares it with the WAV beside it, to within 2 LSB. That allows for
  floating point differing in its last bits between compilers and maths
  libraries. A real change to the sound is far larger: moving one vowel's
  second formant by 10 Hz changes those samples by thousands.

When a change to the sound is intended, run `make samples` to regenerate the
references, listen to the result, and commit the new WAVs with the change.
