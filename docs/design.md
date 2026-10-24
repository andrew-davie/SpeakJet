# Design

speakjet is an emulation of the Magnevation SpeakJet. It was written from the
*SpeakJet User's Manual* (Magnevation, revisions 1.0 and 1.1, 2004, whose
tables are identical) and from general phonetics. Copies of both are in
[`reference/`](reference/).

Where two independent sets of measurements of the real chip agree, they
replace guesses: the [openFPGA-7800](https://github.com/Miasmark/openFPGA-7800)
AtariVox engine's, read from recordings of games speaking (MIT licence), and
those behind Stella's SpeakJet engine (its `feature/speakjet` branch). They
set the output filter's corner, the Speed curve and some formants of nine
allophones; see [Measurements of the real chip](#measurements-of-the-real-chip).
No code or tables were taken from either.

A recording of a real SpeakJet playing a known sequence of codes, compared
sound by sound with this library, sets how long the chip plays each kind of
sound, Fast, how loud its noise is against its vowels, and the A5 and M1
sound effects; see [A recording of the chip](#a-recording-of-the-chip).

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
- **Sound effects.** Apart from DTMF (standard tone pairs), the manual gives
  only names and lengths, so the effects are approximations built from the
  same oscillators, except A5 and M1, which are measured (see
  [A recording of the chip](#a-recording-of-the-chip)). Their lengths match
  the manual.
- **Lengths.** The chip plays speech sounds longer than the manual's
  lengths: steady sounds (vowels, nasals, liquids, fricatives) 1.5 times,
  diphthongs 1.2 times, stops and affricates as given. **Fast** plays a sound
  at 0.6 of its length rather than the manual's half. Both are measured (see
  [A recording of the chip](#a-recording-of-the-chip)).
- **Noise.** Fricatives, stop bursts and aspiration sit 14 to 23 dB under
  the vowels, measured likewise.
- **Speed.** Each step of Speed changes a sound's length by about 2%:
  exp(−0.02 (Speed − 114)), measured between Speeds 60 and 114 and
  extrapolated outside them (see
  [Measurements of the real chip](#measurements-of-the-real-chip)).
- **Stress** raises the pitch by 12% and tenses the vowel (F1 × 0.94,
  F2 × 1.06). **Relax** does the opposite (pitch × 0.9, F1 × 1.06,
  F2 × 0.94). The manual says only that "with STRESS, IH sounds more like (but
  not quite) the IY sound".
- **Bend** is a semitone per step either side of the default of 5. The manual
  says only that it runs from a "deep-hollow" to a "High-metallic" sounding
  voice.
- **Transitions.** Speech sounds glide into their target over 35% of their
  length, kept between 20 and 50 ms.
- **Ramps.** A pause ramps the volume down over 10 ms. When the buffer runs
  empty, the chip ramps to silence over 15 ms.
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
follow the manual. A real SpeakJet would settle it: send 183 135 191 and
183 136 191 and listen for which says "hot".

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
- The output filter is a Butterworth low pass at 2200 Hz, after a DC blocker.
  The corner is measured (see
  [Measurements of the real chip](#measurements-of-the-real-chip)).

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

Duration is exp(−0.02 (Speed − 114)), openFPGA's fit to games speaking at
Speeds 80 to 127. The measurements behind Stella's engine agree with it to
within 8% from 60 to 114:

| Speed | openFPGA | Stella | Was (114 / Speed) |
|---|---|---|---|
| 60 | 2.94 | 2.76 | 1.90 |
| 80 | 1.97 | 2.12 | 1.43 |
| 100 | 1.32 | 1.43 | 1.14 |
| 114 | 1.00 | 1.00 | 1.00 |

Above 114 and below 60 the two disagree with each other, so the curve is
extrapolated there. Both contradict the old inverse law in the same direction
(shorter at 127, longer at 40), so the curve is the better fit throughout.

### Output filter

A Butterworth low pass at 2200 Hz, after a DC blocker. openFPGA fitted 2200 Hz
to the long-term spectrum of the alphabet recording, using a synthesizer like
this one; Stella's measurement, made independently, is 1700 Hz. Both put the
real voice's top end far below what the earlier 3400 Hz let through. Of the
two, openFPGA's value is used because its model matches this library's.

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

The ROM sets Bend 1 before A5, so A5's frequencies are those of the
recording divided by Bend 1's factor; whether the chip bends its effects isn't
known. Fricatives the recording doesn't have (SO, SH, HE, HO, WH, and the
noise of the voiced fricatives) take the mean of those it has. M1 is 3 dB
quieter against the vowels than the recording, as its noise oscillators are
already at full volume.

The recording also suggests, but on too few examples to change, that pause 6
is about 55 ms rather than 90, and that Bend moves F1 and F2 by about 3% a
step and F3 barely at all.

### Not used

Bend, how fast formants move from one sound to the next, oscillator volumes,
the other allophones and the sound effects are measured by at most one of
the sources, so they remain the choices described above.

## What isn't known, and what isn't emulated

### Unknown

The manual doesn't say, so these are guesses (described above):

| | What the library does |
|---|---|
| The oscillator settings behind each allophone | Built from published formant measurements of English speech; only the formants that two sets of measurements of the chip agree on are from the chip, and no volumes are |
| The sound effects, apart from DTMF, A5 and M1 | Approximations, with the manual's lengths |
| How one sound joins the next | Glides over 35% of a sound, held to 20 to 50 ms; a 10 ms ramp into a pause; a 15 ms ramp to silence when the buffer empties |
| The Speed curve below 60 and above 114 | The curve measured between them, extrapolated |
| How much Stress and Relax change a sound | Pitch × 1.12 or × 0.9, with the first two formants shifted (the manual says only "a small amount") |
| The Bend curve | A semitone per step either side of 5 |
| The synthesizer's details | Formant oscillators restarting each voicing period, distortion as random frequency modulation |
| What Soft (18) does | Ignored; it is a real command (see PhraseALator above), but nothing describes its effect |
| Whether 135 is OH and 136 AW | The manual's order is followed; PhraseALator has them swapped (see above) |

Apart from the measured values above, none of this has been compared with a
real SpeakJet. One recording has been (see
[A recording of the chip](#a-recording-of-the-chip)); recordings of every
allophone and effect on their own, and of Speed and Bend sweeps, would settle
most of the rest.

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
