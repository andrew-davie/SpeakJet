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

Every allophone and sound effect, alone and in connected speech, is compared
with recordings of two real AtariVoxes (an original AtariVox and a later
unit): the library is given each recording's own background noise, both
sides are cut out by the same rule, and clipped sounds are left out. The
measure, M, has ten parts: length, level over time, the time-frequency
picture after the two are lined up (smoothed over 15 ms and 1.5 ERB, so the
noise speckle that two real chips differ in doesn't count), its worst 25 ms,
how much the level flickers, the timing inside the sound, sideways spectral
shift, the whole-sound spectrum, the positions of its peaks, and pitch. Each
part is divided by how far the two real chips are from each other on that
part, and the parts furthest off count most. So 0 means as close as a second
chip, 1 about twice the chips' own difference, and lower is better.

This branch is the second, from-scratch implementation (see
[How it works](#how-it-works)). Scored the same way over the 752 recordings
(both units: VoxRecord, its connected groups, the fourth VoxProbe, Man Goes
Down): 1.58 after two days, with the allophones alone at 1.08, the sound
effects alone at 1.53, the connected groups at 1.62 and Man Goes Down's
phrases at 2.34. The two real chips are about 0.5 from each other. The
first implementation's figures are in its own branch's README, for whoever
wants to compare.

What is still known to be off:

- The pulsed biological effects (C2, C3, C7) and M2 are the worst effects;
  their gating and the noise under it are only roughly right.
- Connected timing: the phrases run long against the chip even with the
  overlap rule, and the copies of a repeated glide are not timed as the
  chip's.
- The valley between the first two formants of the vowels is a few dB too
  full.
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

This branch is a second implementation, written from scratch from the
SpeakJet User's Manual and recordings of two AtariVoxes only, with none of
the first implementation's findings or tables, to see whether the same
method lands in the same place. The brief was the smallest footprint: a rule
wherever a rule could be found, a few bytes of parameters per sound
otherwise. `speakjet.c` is about 700 lines; the tables are 72 allophones of
14 bytes and about 100 effect parts of 15 bytes (frequencies in 16 Hz steps,
lengths in 5 ms), about 2.5 KB; the object is about 12.5 KB.

What the recordings gave:

- **The chip's output.** Samples at 8026 Hz (the two units: 8039 and 8013),
  with the chip's own arithmetic in 8000ths, each held until the next, then
  two real poles near 2 kHz and 2.9 kHz, and a noise floor under every sound.
- **The voice.** A period of floor(Fs / Pitch) + 1 ticks; at each period
  three formant sines restart at one phase under an envelope that steps to
  full at once and falls linearly over about 7 ms, with a per-sound offset
  under the same envelope. The pulse is the same on both units to the
  sample.
- **Noise.** An oscillator whose frequency is thrown about by white noise
  every sample (the manual's distortion): a flat band with sharp edges, and
  when thrown past the chip's Nyquist it folds back into the hiss sounds'
  peak at 4 kHz. Each noise sound is a centre, a throw and a level.
- **Timing and controls.** A sound's length is the same streamed, alone or
  before a pause, and the same at every Pitch; it is linear in Speed; Fast
  is 0.8 and Slow 1.25; every sound ramps in over 35 ms and out over 30; a
  Repeat's copies run on seamlessly at the manual's length; a sound followed
  at once by another is cut 10 ms short; Bend moves every oscillator by 44
  Hz a step; Volume 16 to 127 spans 6 dB; Stress, Relax and code 18 do
  nothing measurable.
- **Sound effects.** Each is one to six parts of up to three sweeping tones,
  a noise band, a gate and a few flags, written from the spectrograms and
  then fitted. The robots are a tone at 800 + 300·n Hz over a voiced glide.

The parameters were first read off the recordings, then refined with the
same score as above, sound by sound.

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

The emulation is written from the SpeakJet User's Manual and recordings of
the real chip, as described in [`docs/design.md`](docs/design.md). No code
or tables were taken from other SpeakJet emulations, nor, on this branch,
from the first implementation. Its behaviour is checked by the tests in
`tests/` and the reference samples, and its accuracy against the recordings
as described above.

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
