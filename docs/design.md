# Design

speakjet is a clean room emulation of the Magnevation SpeakJet. It was
written from the *SpeakJet User's Manual* (Magnevation, revisions 1.0 and
1.1, 2004, whose tables are identical) and from general phonetics. Copies of
both are in [`reference/`](reference/). No other SpeakJet implementation was consulted.

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
| Modifiers | Fast (7), Slow (8), Stress (14) and Relax (15) apply to the next sound only. Repeat (26) plays the next sound n times. Reset (31) restores the defaults |

**Choices** (the manual gives no figures for these):

- **Formants.** Each allophone's oscillator frequencies and volumes come from
  published formant measurements of adult male English speech. Diphthongs
  hold their first target, slide to the second, and briefly hold it. Stops
  are a closure (silence, or a low voice bar for voiced stops), a noise burst
  and then aspiration or voicing.
- **Sound effects.** Apart from DTMF (standard tone pairs), the manual gives
  only names and lengths, so the effects are approximations built from the
  same oscillators. Their lengths match the manual.
- **Speed.** Duration is taken to be inversely proportional to Speed, with
  Speed held to 8 or more.
- **Fast and Slow** scale the next sound's duration by 0.5 and 1.5.
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
  contents of the AtariVox's SpeakJet EEPROM aren't known. Unknown codes,
  including 255, are ignored.

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
- The output filter is a Butterworth low pass at 3400 Hz, after a DC blocker.

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
