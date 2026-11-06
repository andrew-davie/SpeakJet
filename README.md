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
./sjwav -o hello.wav 183, 7, 159, 146, 164, 2, 147, 151, 145, 176, 255
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
AtariVoxes (an original AtariVox and an AtariVox+). The library is given each
recording's own background noise, and both sides are cut out by the same rule.
Three scores, lower is better:

- **spectrum**: the whole-sound spectrum error in rms dB (a voice line by line
  over its harmonics), on the VoxRecord takes of each sound alone.
- **E**: the spectrum, the time-frequency picture and the level over time
  together, in rms dB, on the same takes.
- **P**: a perceptual score (length, level over time, time-frequency picture,
  spectrum, peaks and pitch), averaged over every recording of the sound:
  VoxRecord and VoxProbes 4, 7 and 8, 1191 recordings in all.

At 0.3.0, by sound (each sound's mean, then the mean over sounds):

```
              sounds   spectrum dB   E dB    P
allophones        72          2.62   4.31   0.61
effects           55          2.62   4.40   0.92
all sounds       127          2.62   4.35   0.74
every recording, item by item               0.89
```

What is still known to be off (`docs/technical-manual.md` section 13 has the
detail):

- The worst sounds by P are effects: 254 M2, 218 A8, 210 A0, 235 C5 and
  219 A9 (1.47 to 1.66). The best allophones score under 0.15.
- Man Goes Down's phrases score worse than the sounds alone. Its death sound
  (231 C1 at Pitch 212 and Speed 62) runs 24 ms short, and its pulse rate
  starts near 411 Hz where the chip's climbs from 228.
- Bend below zero: the chip folds a tone bent under about 50-90 Hz to near
  2.4 kHz; the library doesn't. 215 A5 also has a 40 Hz tone the library lacks.
- Volume: the chip is linear in amplitude; the library still uses an older,
  flatter curve above Volume 32.
- The reset default Pitch is 87 (the period the chip plays at its default),
  not the manual's 88: provisional until a recording settles it.
- 235 C5's opening and the robots 200 R0 to 203 R3 pulse with a timbre the
  library's pulse doesn't make. 205 R5, 208 R8, 209 R9, 238 C8, 239 C9 and
  220 B0 have structure not yet modelled.
- Stress and Relax do nothing, because nothing measurable was found.

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

[`docs/design.md`](docs/design.md) describes the model and every
measurement of the real chip behind it.

This branch was written from scratch from the SpeakJet User's Manual and
recordings of two AtariVoxes only. The brief was the smallest footprint: a rule
wherever a rule could be found, a few bytes of parameters per sound
otherwise. `speakjet.c` is about 1550 lines; the tables are about 5 KB
compiled: 72 allophones of 26 bytes, the effect parts as a byte stream, each
holding only the fields it uses, and a 4-byte structure row per effect; the
object is about 27 KB.

What the recordings gave:

- **The chip's output.** Samples at 8026 Hz (the two units: about 8047 and
  8014), with the chip's own arithmetic in 8000ths, each held until the next,
  then the AtariVox's analogue stage: two real poles from 1609 Hz (Q 0.462)
  and a real zero at 14 kHz, solved exactly on the held samples. The voice is
  exactly periodic, with no noise between its harmonics.
- **The voice.** A period of floor(65536 / step) + 1 ticks, step =
  floor(Pitch · 65536 / 8000): a 16-bit phase counted up each tick (an effect's
  pulse runs ceil(8000 / rate): one tick shorter wherever 8000 / rate is whole,
  VoxProbe 9); at each period
  three formant sines restart at one phase under an envelope that steps to
  full at once and falls linearly over about 7 ms. The pulse is the same on
  both units to the sample.
- **Noise.** White noise through a two-pole resonance at the chip's rate:
  the hiss is Gaussian noise on both units, not a tone. Each band is read
  off the chip's spectrum (FF flat, SH near 2.9 kHz, WH at 800 Hz, the
  sibilants peaking near Nyquist, PE and PO low-passed). Each noise sound is
  a centre, a bandwidth and a level; the allophones' noise is high-passed
  twice at 400 Hz, WH has a white floor, and SE, SO, SH start on a burst of
  low noise.
- **Timing and controls.** A sound's length is the same streamed, alone or
  before a pause, and the same at every Pitch; it is linear in Speed; Fast
  is x2/3 and Slow x4/3 (Slow also on the voiced stops), and a sound straight
  after the same sound plays as a Repeat copy. Inside a run of sounds the
  voice runs on without fading, each sound cut 15 ms short (30 when both are
  voiced); only a run's edges ramp, 35 ms in and 30 out (unvoiced
  fricatives 3 ms, nasals in 25). Stops have their own closures, a voice bar
  with weak upper formants, and louder onsets. Each Repeat copy
  lasts the sound's measured copy length (a stop's copy is the stop again).
  Bend moves every oscillator by 44 Hz a step; Volume follows an older
  measured curve (16 is 15.5 dB down, 32 to 127 move little), where the chip
  was since found linear; Stress, Relax and code 18 do nothing measurable.
  The command after a sound is read at the sound's release, and a Speed or
  Volume there changes the rest of the sound.
- **Sound effects.** Each is one to eleven parts of up to three sweeping
  tones and a noise band, read off the chip's own tracks: pulsed effects at
  Pitch plus an offset of their own, the robots a carrier at 800 + 300·n Hz
  beside a low tone and its octave, plain tones that hold and end in a 55 ms
  fade, D0 to D11 the standard DTMF pairs. Many start on a 5 ms click.

The parameters were first read off the recordings, then refined inside each
sound's measured structure with the scores above, sound by sound. The whole
model, measurement by measurement, is in `docs/design.md`. The chip itself,
in enough detail to write an emulator from, is described in
`docs/technical-manual.md`.

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
| `docs/technical-manual.md` | The SpeakJet itself, as measured: hardware, synthesizer, voice, allophones, stops, Repeat, effects, commands, and what is still unknown |
| `docs/reference/` | The SpeakJet User's Manual, and the PhraseALator dictionary text2speakjet uses |
| `CHANGELOG.md` | What changed, with the measurements behind each change |

## License

MIT. See [`LICENSE`](LICENSE).

The CMU Pronouncing Dictionary (`tools/text2speakjet/cmudict.dict.gz`) is
copyright Carnegie Mellon University, under the BSD-style licence in
[`tools/text2speakjet/LICENSE.cmudict`](tools/text2speakjet/LICENSE.cmudict).

These are **not** covered by the MIT licence, and are included for reference;
see [`docs/reference/README.md`](docs/reference/README.md):

- the technical manual (`docs/technical-manual.md`), copyright Andrew Davie,
  all rights reserved
- the SpeakJet User's Manual (`docs/reference/`), copyright Magnevation LLC
- Magnevation's PhraseALator dictionary (`docs/reference/phrasealator/`,
  with the download that carries its licence) and the dictionary's converted form `tools/text2speakjet/phrasealator.txt`,
  copyright Magnevation LLC and used under PhraseALator's licence, which asks
  that "Portions of this program are provided by and copyrighted by
  Magnevation LLC." be shown when its parts are used; text2speakjet prints it
  whenever it uses the dictionary. Everything works without these files;
  text2speakjet falls back to the CMU dictionary

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

The emulation is written from the SpeakJet User's Manual and recordings of
the real chip only, as described in [`docs/design.md`](docs/design.md). Its behaviour is checked by the tests in
`tests/` and the reference samples, and its accuracy against the recordings
as described above.

## Credits

Code by Claude, directed by Andrew Davie.

text2speakjet's letter-to-sound trees come from the Festival Speech
Synthesis System (Alan W Black, Paul Taylor, Richard Caley and others, CSTR,
University of Edinburgh), trained on the CMU Pronouncing Dictionary. Its
intonation follows Festival's "simple" model, its phrase-final lengthening
Klatt's duration rules, and its "the"/"thee" rule Festival's post-lexical
rules; these are reimplemented, not copied.
