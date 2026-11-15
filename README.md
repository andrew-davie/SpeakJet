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

Every allophone and sound effect is scored against the chip's own recordings:
four takes of each sound alone, two from each of two real AtariVoxes (an
original AtariVox and an AtariVox+). The score, X, compares spectrograms at six
resolutions, from 1.3 ms to 43 ms windows, with each take's background noise
taken off, and finds the best alignment within 12 ms of the sound's start. It
is calibrated on the chip itself: the four takes are scored against each other,
and a sound's distance is divided by that spread. So **X = 1.0 means "as far
from the chip as another recording of the chip"**, and lower is closer.

0.5.2 against 0.4.3, both scored the same way on the same recordings:

```
                                      0.4.3   0.5.2
allophones (72), mean X                1.07    0.93   57 closer, none further
sound effects (55), mean X             1.22    0.97   53 closer, none further
joins (88 recorded pairs and phrases)  1437     514   mean cost; 56 closer, 15 further
lengths 15 ms or more off (88 items)     24      14
```

The joins are VoxProbe 4, 7 and 8's two-sound pairs and Man Goes Down's
phrases, on both units, scored by a lined-up time-frequency difference. Man
Goes Down's phrase lengths, against the two chips:

```
              unit A   unit B   0.4.3   0.5.2
phrase 1        1483     1490    1475    1491 ms
phrase 2        1676     1671    1683    1690 ms
phrase 4         782      787     779     771 ms
```

Biggest gains: 235 C5 -45 %, 253 M1 -43 %, 159 EHLL -36 %, 223 B3 -36 %,
213 A3 and 228 B8 -35 %. Every sound's score is in [`docs/scores.md`](docs/scores.md).

What is still known to be off (`docs/technical-manual.md` section 13 has the
detail):

- The furthest sounds are effects: 238 C8 (1.75), 207 R7 (1.44), 239 C9
  (1.43) and 206 R6 (1.34); then the voiced stops 174 DE (1.29) and 171 BO
  (1.24), whose closure bar is fuller and release darker on the chip.
- A Pitch command inside a phrase takes effect one to five sounds late on the
  chip; the library applies it at the next sound. Man Goes Down's phrase 2 runs
  long mostly because of this.
- The chip's noise is a fixed sequence that replays with each sound; the library's
  is random. This is the likely floor under every hiss, and why C8 and C9 are the
  furthest sounds.
- After an unvoiced stop or hiss the chip's level dies away over about 100 ms;
  the library's stops within a few.
- Bend below zero: the chip folds a tone bent under about 50-90 Hz to near
  2.4 kHz; the library doesn't. 215 A5 also has a 40 Hz tone the library lacks.
- Volume: the chip is linear in amplitude; the library still uses an older,
  flatter curve above Volume 32.
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

Every rule is taken from recordings of two AtariVoxes, and every claim is
checked against both. A rule went in only when no sound scored further from
the chip under it. `speakjet.c` is about 2450 lines; the tables are 7190 bytes
compiled: 72 allophones of 34 bytes, the effect parts as a byte stream holding
only the fields each uses, and a 9-byte structure row per effect; the object
is about 99 KB at -O2.

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
  the hiss is Gaussian-distributed noise on both units, not a tone (though not random:
  the chip replays the same sequence each time, which the library doesn't yet). Each band is read
  off the chip's spectrum (FF flat, SH near 2.9 kHz, WH at 800 Hz, the
  sibilants peaking near Nyquist, PE and PO low-passed). Each noise sound is
  a centre, a bandwidth and a level; the allophones' noise is high-passed
  twice at 400 Hz, WH has a white floor, and SE, SO, SH start on a burst of
  low noise.
- **Timing and controls.** A sound's length is the same streamed, alone or
  before a pause, and the same at every Pitch; it is linear in Speed; Fast
  is x2/3 and Slow x4/3 (x1.29 on a plain vowel; Slow also on the voiced stops), and a sound straight
  after the same sound plays as a Repeat copy. Inside a run of sounds the
  voice runs on without fading, each sound cut 15 ms short (30 when both are
  voiced); only a run's edges ramp, each vowel and glide in over its own 20-60
  ms (unvoiced fricatives 3 ms, nasals 25). Stops have their own closures, a voice bar
  with weak upper formants, and louder onsets. Each Repeat copy
  lasts the sound's measured copy length (a stop's copy is the stop again).
  Bend moves every oscillator by 44 Hz a step; Volume follows an older
  measured curve (16 is 15.5 dB down, 32 to 127 move little), where the chip
  was since found linear; Stress, Relax and code 18 do nothing measurable.
  The command after a sound is read at the sound's release, and a Speed or
  Volume there changes the rest of the sound.
- **Sound effects.** Each is one to thirteen parts of up to four sweeping
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
| `docs/scores.md` | Every sound's accuracy score, 0.4.3 against 0.5.2 |
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
