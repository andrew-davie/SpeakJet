# speakjet

An emulation of the Magnevation **SpeakJet** speech and sound
synthesizer, the chip in the AtariVox peripheral for the Atari 2600, as a
small C library. It also includes **text2speakjet**, which turns English text
into SpeakJet codes, and **sjwav**, which renders codes to a WAV file.

- Plain C99, one `.c` and one `.h`, needs only libm.
- No global state; any number of chips can run side by side.
- Takes the raw serial byte stream, and gives you the Buffer Half Full and
  Speaking lines, so it drops straight into an emulator's AtariVox.
- Save state support.
- Measured from two real AtariVoxes: nearly every sound, timing and control
  is taken from recordings of the chip, and checked against recordings that
  nothing was fitted to (see [How close it is](#how-close-it-is)).
- Output: mono float samples at 48 kHz. The chip makes its own samples at
  about 8 kHz and holds each one until the next, which puts a faint edge
  above 4 kHz; the library does the same, through the chip's measured low
  pass.

Listen to [`samples/`](samples/) to hear what it sounds like.

## Quick start

```
make            # libspeakjet.a and sjwav
make test       # library tests, reference samples, text2speakjet tests

python3 tools/text2speakjet/text2speakjet.py --format dec "Hello, world."
./sjwav -o hello.wav 184, 7, 133, 146, 14, 164, 2, 147, 14, 151, 146, 176, 255
```

Or straight from text to sound:

```
python3 tools/text2speakjet/text2speakjet.py --format bin -o hello.bin "Hello, world."
./sjwav -b -o hello.wav < hello.bin
```

`make test` needs Python 3.8 or later for the text2speakjet tests. The library
itself needs only a C99 compiler.

## Using the library

Add `speakjet.c` and `speakjet.h` to your build and link with `-lm`.

```c
#include "speakjet.h"

sj_chip *sj = sj_new();

/* feed the serial byte stream, as the AtariVox receives it. two-byte
 * commands (e.g. 21 = Speed, then its value) are framed for you */
sj_write_byte(sj, 21);
sj_write_byte(sj, 100);
sj_write_byte(sj, 183);  /* HO */
sj_write_byte(sj, 135);  /* OH */

/* pull audio: mono floats in -1..1, at SJ_SAMPLE_RATE (48000 Hz) */
float buf[256];
while (sj_render(sj, buf, 256) || sj_buffered(sj) > 0) {
  /* play or resample buf */
}

sj_free(sj);
```

The AtariVox's READY pin goes low when the SpeakJet's buffer is filling, so
drive READY as `!sj_buffer_half_full(sj)` (see
[`docs/design.md`](docs/design.md); the chip's own pin named *Ready* is a
different signal). The input buffer drains as you render, so render in step
with the emulated time to keep READY's timing right: rendering one sample at
a time, whenever the emulated clock passes the next 1/48000 s, is fine.

The output is at 48 kHz because that is what carries the chip's sound above
4 kHz. If your audio runs at another rate, resample it as you would any
other source; a resampler that cuts at 4 kHz would throw that sound away.
The output peaks at about 0.5.

| Function | Description |
|---|---|
| `sj_new()` / `sj_free()` | Create or destroy a chip |
| `sj_write_byte()` | Feed one byte of the serial command stream |
| `sj_write()` | Write one command (code and data byte) directly |
| `sj_render()` | Render samples; returns true while the chip is producing sound |
| `sj_buffer_half_full()` | The Buffer Half Full line: more than 32 of the 64 bytes in use |
| `sj_speaking()` | The Speaking line |
| `sj_buffered()` | Bytes in the input buffer, including the command playing |
| `sj_reset()` | The reset line: buffer cleared, controls back to their defaults |
| `sj_power_on()` | Back to the power-on state, sound included |
| `sj_get_controls()` / `sj_set_controls()` | Volume, Speed, Pitch, Bend and pending modifiers, for save states |
| `sj_sound_name()` | An allophone's or sound effect's name, e.g. `"IY"` or `"M1"` |
| `sj_command_size()` | How many bytes a command takes |

A chip isn't thread safe: make your writes and renders from one thread, or
lock around them. Separate chips are independent. `speakjet.h` documents
every function.

## How close it is

Every allophone and sound effect is compared with recordings of two real
AtariVoxes (an original AtariVox and a later unit), with each recording's
background noise taken out and any clipped sound left out. Two numbers are
used: how much of the chip's spectrogram the library reproduces (after
lining the two up in time), and how far its overall spectrum is from the
chip's, in dB.

On the fourth VoxProbe recording, made after all the fitting and so never
fitted to (about 145 sounds on each unit, below 4 kHz):

| Library | Unit A | Unit B |
|---|---|---|
| The first version, from the manual and published formants | 27%, 10.6 dB | 27%, 10.6 dB |
| Fitted to the first recordings (14181cc) | 58%, 5.6 dB | 58%, 5.6 dB |
| Rebuilt from the recordings, checked against held-out data (708ed16) | 64%, 3.7 dB | 64%, 3.7 dB |
| Now, with the output above 4 kHz | 63.5%, 3.8 dB | 63.4%, 3.8 dB |

Above 4 kHz, where the chip has a quiet edge to its sound, the library had
nothing (19 dB off); it now matches the chip's level there within 2 dB.

On the VoxRecord recording of every sound, against the clean unit, it
matches 84.6% (1.8 dB). The two real chips match each other about 88%
(1.1 dB) on the sounds both recorded cleanly: that is about as close as it
can usefully get, and the gap left is real.

What is still known to be off, all described in
[`docs/design.md`](docs/design.md):

- The gated effects C0 to C6 change their gating with Pitch on the chip (at
  the voicing frequency plus an offset of their own); here they don't yet.
- M2 lacks a part near 2.1 kHz, and effects at high Bend settings are well
  off (C3 and R8 by about 12 dB at bend 15).
- Vowels played on their own are about 15 ms shorter than the chip's; in
  connected speech, which their lengths are fitted to, they are right.
- The hiss sounds have only been measured on the earlier recordings.

Numbers aren't everything: listening against the chip is the final check.

## text2speakjet

`tools/text2speakjet/text2speakjet.py` converts English into SpeakJet codes,
as DASM `.byte` lines, a C array, decimal or raw bytes. It uses
Magnevation's own SpeakJet spellings of over 1,400 common words and sound
effect phrases (from its PhraseALator dictionary) and the CMU Pronouncing
Dictionary (about 134,000 words) for the rest, guesses words in neither with
letter-to-sound decision trees from the Festival speech synthesis system, and
picks the right allophone variant for each sound from its neighbours. It adds
stress, pauses for punctuation, numbers, years, money and times as words,
optional Festival-style intonation (`--intonation`), and takes markup for
pitch, per-syllable melody, speed and raw codes. Its length estimates use
the library's measured lengths. It needs Python 3.8 or later and nothing
else.

```
python3 tools/text2speakjet/text2speakjet.py "Welcome to the AtariVox"
```

See [its README](tools/text2speakjet/README.md) for the options and markup.

## Samples

[`samples/`](samples/) holds a 48 kHz WAV for each phrase in
[`samples/phrases.txt`](samples/phrases.txt): speech, pitch, speed, stress,
Bend, sound effects, DTMF and Repeat. `samples/codes.txt` has each phrase's
codes. The WAVs double as the test reference, so `make test` fails if a
change alters the sound. When a change is meant to alter it, `make samples`
regenerates them.

## How it works

[`docs/design.md`](docs/design.md) describes the input buffer, the MSA
sequencer and the synthesizer, what each takes from the SpeakJet User's
Manual, and every measurement of the real chip behind them.

The manual documents the synthesizer's structure and the timings, but not the
oscillator settings behind each allophone or effect. The first version filled
those in from published formant measurements of English speech. Since then
nearly all of them have been measured from the chip itself, from recordings
of two AtariVoxes playing test ROMs made for the purpose: Alex Herbert's
vox_test, three takes of VoxRecord (every allophone and effect, and sweeps of
each control) and VoxProbe 1 to 4 (single voicing pulses, every Pitch,
sustained and repeated sounds, the move from one sound to the next, and the
effects at other Pitch and Bend settings). From them:

- **The chip's output.** It makes its samples at about 8028 Hz (not the
  manual's 8192), holds each until the next, and a measured low pass shapes
  what that puts above 4 kHz, up to 23 kHz.
- **The voice.** Each voicing pulse restarts the formant oscillators, which
  then fade along a measured curve, gone by 7.75 ms; the pitch is a whole
  number of ticks of a 7986 Hz clock, with the chip's 12 exceptions; the
  oscillators run on a 4.3845 Hz grid. Formants, volumes and phases come from
  single pulses, and 26 sounds (most diphthongs, most voiced stops, ZZ, ZH,
  DH, IH, NO and LO) are played from the chip's own formants, pulse by pulse.
  Voicing fades in over 40 ms from silence (25 ms for the nasals).
- **Timing and controls.** The lengths of sounds alone and in connected
  speech, the pauses, Repeat, Speed, Fast and Slow, Bend, Stress and Volume.
- **Noise.** The hiss of each fricative and stop, fitted to both units at the
  chip's own rate, with its level against the vowels kept.
- **Sound effects.** Traced from the recordings: tones on their own
  oscillators, buzzes, gated noise, and the noise the chip puts under its
  tones.

The method is described there too. Each recording's background is measured
and taken out, clipped sounds are left out, and every change is kept only if
it holds on both units and on recordings it wasn't fitted to.

## Other versions

The same emulation in Go, kept in step with this library and matching its
output to within float rounding, is in
[andrew-davie/SpeakJetGo](https://github.com/andrew-davie/SpeakJetGo), and is
merged upstream into [JetSetIlly/SpeakJet](https://github.com/JetSetIlly/SpeakJet),
which the Gopher2600 emulator uses for its AtariVox.

## Layout

| Path | |
|---|---|
| `speakjet.c`, `speakjet.h` | The library |
| `tests/` | Unit tests and the reference sample test |
| `tools/sjwav.c`, `tools/render.c` | The WAV renderer |
| `tools/text2speakjet/` | Text to SpeakJet codes, with its dictionaries and tests |
| `samples/` | Sample phrases, their codes and WAVs |
| `docs/design.md` | How the emulation works, and every measurement behind it |
| `docs/reference/` | The SpeakJet User's Manual, NRL Report 7948, and (deprecated) Magnevation's PhraseALator |
| `CHANGELOG.md` | What changed, with the measurements behind each change |

## License

MIT. See [`LICENSE`](LICENSE).

The CMU Pronouncing Dictionary (`tools/text2speakjet/cmudict.dict.gz`) is
copyright Carnegie Mellon University, under the BSD-style licence in
[`tools/text2speakjet/LICENSE.cmudict`](tools/text2speakjet/LICENSE.cmudict).

These are **not** covered by the MIT licence, and are included for reference;
see [`docs/reference/README.md`](docs/reference/README.md):

- the SpeakJet User's Manuals (`docs/reference/`), copyright Magnevation LLC
- Magnevation's PhraseALator software and dictionary
  (`docs/reference/phrasealator/`, deprecated and kept for reference only),
  and the dictionary's converted form `tools/text2speakjet/phrasealator.txt`,
  copyright Magnevation LLC and used under PhraseALator's licence, which asks
  that "Portions of this program are provided by and copyrighted by
  Magnevation LLC." be shown when its parts are used; text2speakjet prints it
  whenever it uses the dictionary. Everything works without these files;
  text2speakjet falls back to the CMU dictionary
- NRL Report 7948, a US Government work in the public domain

The letter-to-sound trees (`tools/text2speakjet/lts_cmu.json.gz`) are
converted from Festival's CMU lexicon rules, copyright the Centre for Speech
Technology Research, University of Edinburgh, under the permissive licence in
[`tools/text2speakjet/LICENSE.festival`](tools/text2speakjet/LICENSE.festival),
which asks that its notice be kept and modifications be marked (they are, in
the file's header and `convert_festival_lts.py`).

## How it was made

The code, tests, measurements and documentation in this repository were
written by Claude, Anthropic's AI model, working under Andrew Davie's
direction. He decided what to build and how it should behave, designed and
recorded the test ROMs on his own AtariVoxes, reviewed and tested the
results, including in an Atari 2600 emulator's AtariVox, and edited them.

The emulation is written from the SpeakJet User's Manual, as described in
[`docs/design.md`](docs/design.md), and measured from recordings of the real
chip. No code or tables were taken from other SpeakJet emulations. Its
behaviour is checked by the tests in `tests/` against the manual's figures
and the reference samples, and its accuracy against the recordings as
described above.

## Credits

Code by Claude, directed by Andrew Davie.

text2speakjet's letter-to-sound trees come from the Festival Speech
Synthesis System (Alan W Black, Paul Taylor, Richard Caley and others, CSTR,
University of Edinburgh), trained on the CMU Pronouncing Dictionary. Its
intonation follows Festival's "simple" model, its phrase-final lengthening
Klatt's duration rules, and its "the"/"thee" rule Festival's post-lexical
rules; these are reimplemented, not copied. Earlier versions used the
letter-to-sound rules of Elovitz, Johnson, McHugh and Shore, NRL Report 7948
(1976).

Before the chip was recorded here, the formants of nine allophones rested on
two independent sets of measurements of the real SpeakJet: those of the
AtariVox engine in [openFPGA-7800](https://github.com/Miasmark/openFPGA-7800)
(MIT licence), taken from recordings of games speaking, and those behind
Stella's SpeakJet engine (its `feature/speakjet` branch); only values both
agreed on were used. They have since been replaced by direct measurements of
the chip.
