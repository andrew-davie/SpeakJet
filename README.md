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
- Output: mono float samples at 8192 Hz.

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

/* pull audio: mono floats in -1..1, at SJ_SAMPLE_RATE (8192 Hz) */
float buf[256];
while (sj_render(sj, buf, 256) || sj_buffered(sj) > 0) {
  /* play or resample buf */
}

sj_free(sj);
```

The AtariVox's READY pin goes low when the SpeakJet's buffer is filling, so
drive READY as `!sj_buffer_half_full(sj)` (see
[`docs/design.md`](docs/design.md); the chip's own pin named *Ready* is a
different signal). The input buffer drains as you render, so
render in step with the emulated time to keep READY's timing right.

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

## text2speakjet

`tools/text2speakjet/text2speakjet.py` converts English into SpeakJet codes,
as DASM `.byte` lines, a C array, decimal or raw bytes. It uses
Magnevation's own SpeakJet spellings of over 1,400 common words and sound
effect phrases (from its PhraseALator dictionary) and the CMU Pronouncing
Dictionary (about 134,000 words) for the rest, and picks the right allophone
variant for each sound from its neighbours. It adds stress, pauses for
punctuation and spoken numbers, and takes markup for pitch, speed and raw
codes. It needs Python 3.8 or later and nothing else.

```
python3 tools/text2speakjet/text2speakjet.py "Welcome to the AtariVox"
```

See [its README](tools/text2speakjet/README.md) for the options and markup.

## Samples

[`samples/`](samples/) holds a WAV for each phrase in
[`samples/phrases.txt`](samples/phrases.txt): speech, pitch, speed, stress,
Bend, sound effects, DTMF and Repeat. `samples/codes.txt` has each phrase's
codes. The WAVs double as the test reference, so `make test` fails if a
change alters the sound. When a change is meant to alter it, `make samples`
regenerates them.

## How it works

[`docs/design.md`](docs/design.md) describes the input buffer, the MSA
sequencer and the synthesizer. It says what each takes from the SpeakJet
User's Manual and what is a choice made here: the manual documents the
synthesizer and the timings, but not the oscillator settings behind each
allophone, so those are derived from published formant measurements of
English speech. Where two independent sets of measurements of the real chip
agree, they replace guesses: some formants of nine allophones, the Speed
curve and the output filter's corner. `docs/design.md` explains how agreement
was decided and lists every measured value.

## Layout

| Path | |
|---|---|
| `speakjet.c`, `speakjet.h` | The library |
| `tests/` | Unit tests and the reference sample test |
| `tools/sjwav.c`, `tools/render.c` | The WAV renderer |
| `tools/text2speakjet/` | Text to SpeakJet codes, with its dictionaries and tests |
| `samples/` | Sample phrases, their codes and WAVs |
| `docs/design.md` | How the emulation works, and what is guessed |
| `docs/reference/` | The SpeakJet User's Manual, NRL Report 7948, and (deprecated) Magnevation's PhraseALator |

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

## How it was made

The code, tests and documentation in this repository were written by Claude,
Anthropic's AI model, working under Andrew Davie's direction. He decided what
to build and how it should behave, reviewed and tested the results, including
running the library in an Atari 2600 emulator's AtariVox, and edited them.

The emulation is written from the SpeakJet User's Manual, as described in
[`docs/design.md`](docs/design.md), and corrected where two independent sets
of measurements of the real chip agree. No code or tables were taken from
other SpeakJet emulations. Its behaviour is checked by the tests in `tests/`
against the manual's figures and the reference samples.

## Credits

Code by Claude, directed by Andrew Davie.

The letter-to-sound rules text2speakjet uses for words that aren't in the
dictionary are from Elovitz, Johnson, McHugh and Shore, *Automatic
Translation of English Text to Phonetics by Means of Letter-to-Sound Rules*,
NRL Report 7948 (1976).

The measured formants, Speed curve and output filter corner rest on two
independent sets of measurements of the real SpeakJet: those of the AtariVox
engine in [openFPGA-7800](https://github.com/Miasmark/openFPGA-7800) (MIT
licence), taken from recordings of games speaking, and those behind Stella's
SpeakJet engine (its `feature/speakjet` branch). Only values both agree on
are used.
